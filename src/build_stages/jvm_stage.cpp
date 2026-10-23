#include <vector>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <algorithm>
#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include "mkapk_extractor.hpp"
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    fs::path java_out = ctx.build_dir / "classes" / "java_classes";
    fs::path dex_cache = ctx.build_dir / "dex_cache";
    fs::create_directories(dex_cache);

    // 1. Construct the complete classpath dynamically from dependencies.txt
    auto dep_artifacts = MkapkExtractor::load_project_dependencies(ctx.build_dir);
    std::vector<fs::path> extra_jvm_classpaths = std::move(dep_artifacts.jvm_classpath_jars);

    // Ensure kotlin-stdlib is present on classpath if not explicitly resolved
    bool has_kotlin_stdlib = false;
    for (const auto& p : extra_jvm_classpaths) {
        if (p.filename().string().find("kotlin-stdlib") != std::string::npos) {
            has_kotlin_stdlib = true;
            break;
        }
    }

    if (!has_kotlin_stdlib) {
        const char* prefix_env = std::getenv("PREFIX");
        fs::path stdlib_jar = prefix_env ? fs::path(prefix_env) / "opt/kotlin/lib/kotlin-stdlib.jar"
                                         : "/data/data/com.termux/files/usr/opt/kotlin/lib/kotlin-stdlib.jar";
        if (fs::exists(stdlib_jar)) {
            extra_jvm_classpaths.push_back(stdlib_jar);
        }
    }

    // 2. Recompile source logic if JVM compilation is flagged, config changed, or forced
    if (ctx.diff.needs_jvm_compile || ctx.diff.config_changed || ctx.force_all) {
        UI::stage(UI::Msg::STAGE_SOURCE, UI::Msg::OP_COMPILING_JVM);

        auto logic_res = compile_source_logic(
            config,
            ctx.tools,
            ctx.active_plugins,
            ctx.android_jar,
            ctx.build_dir,
            ctx.diff.changed_files,
            ctx.diff.deleted_files,
            (ctx.diff.res_changed || ctx.diff.config_changed || ctx.force_all),
            ctx.run_func,
            extra_jvm_classpaths
        );

        if (logic_res.is_err()) {
            return Result<void>::error(logic_res.get_error());
        }

        java_out = logic_res.get_value().first;
        dex_cache = logic_res.get_value().second;
    }

    // 3. Early exit: If DEX artifacts exist and no rebuild triggers were flagged, skip D8/R8
    fs::path target_classes_dex = ctx.build_dir / "classes.dex";
    if (!ctx.diff.needs_dex_rebuild && !ctx.diff.config_changed && !ctx.force_all && fs::exists(target_classes_dex)) {
        return Result<void>::success();
    }

    // 4. Release Mode: R8 Whole-Program Optimization & Tree Shaking
    if (ctx.is_release) {
        UI::stage(UI::Msg::STAGE_MINIFY, UI::Msg::OP_R8_OPTIMIZE);

        // Dynamically wire -no-obs from raw invocation arguments
        bool no_obs = std::find(ctx.raw_args.begin(), ctx.raw_args.end(), "-no-obs") != ctx.raw_args.end();

        auto r8_res = run_dex_r8(
            ctx.tools["r8"],
            ctx.android_jar,
            config,
            ctx.build_dir,
            ctx.run_func,
            no_obs,
            extra_jvm_classpaths
        );
        if (r8_res.is_err()) return r8_res;
    }
    // 5. Debug Mode: Incremental D8 Translation & Unified Merge
    else {
        UI::stage(UI::Msg::STAGE_DEX);

        std::vector<fs::path> unified_dex_targets;
        for (const auto& [lang, files] : ctx.diff.changed_files) {
            auto plug_it = ctx.active_plugins.find("." + lang);
            if (lang == "java" || lang == "kotlin" || (plug_it != ctx.active_plugins.end() && plug_it->second.output_type == "jvm")) {
                for (const auto& f : files) unified_dex_targets.push_back(f);
            }
        }

        // If config changed, ensure all compiled classes are re-translated
        if (ctx.diff.config_changed && fs::exists(java_out)) {
            for (const auto& entry : fs::recursive_directory_iterator(java_out)) {
                if (entry.is_regular_file() && entry.path().extension() == ".class") {
                    unified_dex_targets.push_back(entry.path());
                }
            }
        }

        // 5a. Incremental translation for modified application source classes (using dynamic min_sdk)
        if (!unified_dex_targets.empty()) {
            auto d8_inc_res = run_incremental_dex(
                ctx.tools["d8"],
                ctx.android_jar,
                ctx.src_dir,
                java_out,
                dex_cache,
                unified_dex_targets,
                extra_jvm_classpaths,
                ctx.run_func,
                config.min_sdk
            );
            if (d8_inc_res.is_err()) return d8_inc_res;
        }

        // 5b. Batch-dex external dependency JARs into consolidated DEX slices
        fs::path deps_marker = dex_cache / ".deps_dexed";
        if (!fs::exists(deps_marker) || ctx.diff.needs_dex_rebuild || ctx.diff.config_changed || ctx.force_all) {
            // Remove previous dependency DEX slices from dex_cache
            for (const auto& entry : fs::directory_iterator(dex_cache)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                    std::string stem = entry.path().stem().string();
                    if (stem.rfind("dep_classes", 0) == 0 || stem.find('-') != std::string::npos) {
                        fs::remove(entry.path());
                    }
                }
            }

            if (!extra_jvm_classpaths.empty()) {
                fs::path temp_libs_dex_dir = dex_cache / "_tmp_libs";
                fs::create_directories(temp_libs_dex_dir);

                // Use response file to avoid CLI ARG_MAX limits on Android
                fs::path libs_list_file = dex_cache / "d8_libs_input.txt";
                std::ofstream libs_file(libs_list_file);
                for (const auto& jar : extra_jvm_classpaths) {
                    if (fs::exists(jar)) {
                        libs_file << fs::absolute(jar).string() << "\n";
                    }
                }
                libs_file.close();

                std::string min_api_val = config.min_sdk.empty() ? "21" : config.min_sdk;
                std::vector<std::string> d8_batch_args = {
                    "d8",
                    "--min-api", min_api_val,
                    "--lib", fs::absolute(ctx.android_jar).string(),
                    "--output", temp_libs_dex_dir.string(),
                    "@" + libs_list_file.string()
                };

                auto d8_batch_res = ctx.run_func(d8_batch_args, "Failed batch compilation of dependency JARs into DEX.");
                if (d8_batch_res.is_err()) {
                    fs::remove_all(temp_libs_dex_dir);
                    return d8_batch_res;
                }

                // Relocate generated DEX slices into dex_cache as dep_classes*.dex
                int dep_idx = 1;
                for (const auto& entry : fs::directory_iterator(temp_libs_dex_dir)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                        fs::path target_name = (dep_idx == 1)
                            ? (dex_cache / "dep_classes.dex")
                            : (dex_cache / ("dep_classes" + std::to_string(dep_idx) + ".dex"));
                        fs::rename(entry.path(), target_name);
                        dep_idx++;
                    }
                }

                fs::remove_all(temp_libs_dex_dir);
                fs::remove(libs_list_file);

                std::ofstream touch(deps_marker);
            }
        }

        // 5c. Merge all cached incremental app classes and dep_classes*.dex slices into final classes.dex
        auto d8_merge_res = run_dex_d8(
        ctx.tools["d8"], 
        ctx.android_jar, 
        ctx.build_dir, 
        dex_cache, 
        ctx.run_func,
        config.min_sdk);
        
        if (d8_merge_res.is_err()) 
        return d8_merge_res;
    }

    return Result<void>::success();
}
