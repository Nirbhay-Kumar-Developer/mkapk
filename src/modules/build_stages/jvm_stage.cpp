#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>
#include <fstream>
#include <set>
#include <algorithm>

namespace fs = std::filesystem;

static std::vector<fs::path> resolve_all_dependency_jars(const fs::path& build_dir) {
    std::vector<fs::path> jars;
    std::set<std::string> seen;

    fs::path deps_manifest = build_dir / "dependencies.txt";
    if (!fs::exists(deps_manifest)) {
        return jars;
    }

    std::ifstream in(deps_manifest);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();

        fs::path p(line);
        if (!fs::exists(p)) continue;

        // 1. Direct JAR dependencies
        if (p.extension() == ".jar") {
            std::string abs_path = fs::absolute(p).string();
            if (seen.insert(abs_path).second) {
                jars.push_back(p);
            }
        } 
        // 2. Extracted AAR artifacts
        else if (p.extension() == ".aar") {
            fs::path aar_dir = p.parent_path();

            // Primary bytecode container
            fs::path classes_jar = aar_dir / "classes.jar";
            if (fs::exists(classes_jar)) {
                std::string abs_classes = fs::absolute(classes_jar).string();
                if (seen.insert(abs_classes).second) {
                    jars.push_back(classes_jar);
                }
            }

            // Embedded secondary jars (e.g., emoji2/libs/repackaged.jar)
            fs::path inner_libs = aar_dir / "libs";
            if (fs::exists(inner_libs) && fs::is_directory(inner_libs)) {
                for (const auto& entry : fs::directory_iterator(inner_libs)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                        std::string abs_inner = fs::absolute(entry.path()).string();
                        if (seen.insert(abs_inner).second) {
                            jars.push_back(entry.path());
                        }
                    }
                }
            }
        }
    }

    return jars;
}

Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    fs::path java_out = ctx.build_dir / "classes" / "java_classes";
    fs::path dex_cache = ctx.build_dir / "dex_cache";
    fs::create_directories(dex_cache);

    // 1. Construct the complete classpath dynamically from dependencies.txt
    std::vector<fs::path> extra_jvm_classpaths = resolve_all_dependency_jars(ctx.build_dir);

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

    // 2. Only run javac / kotlinc if code modified, R.txt changed, or full rebuild requested
    if (ctx.diff.needs_jvm_compile || ctx.force_all) {
        UI::stage(UI::Msg::STAGE_SOURCE, UI::Msg::OP_COMPILING_JVM);

        auto logic_res = compile_source_logic(
            config, 
            ctx.tools, 
            ctx.active_plugins, 
            ctx.android_jar, 
            ctx.build_dir, 
            ctx.diff.changed_files, 
            ctx.diff.deleted_files, 
            (ctx.diff.res_changed || ctx.force_all), 
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
    if (!ctx.diff.needs_dex_rebuild && !ctx.force_all && fs::exists(target_classes_dex)) {
        return Result<void>::success();
    }

    // 4. Release Mode: R8 Whole-Program Optimization & Tree Shaking
    if (ctx.is_release) {
        UI::stage(UI::Msg::STAGE_MINIFY, UI::Msg::OP_R8_OPTIMIZE);
        auto r8_res = run_dex_r8(
            ctx.tools["r8"], 
            ctx.android_jar, 
            config, 
            ctx.build_dir, 
            ctx.run_func, 
            false, 
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

        // 5a. Incremental translation for modified application source classes
        if (!unified_dex_targets.empty()) {
            auto d8_inc_res = run_incremental_dex(
                ctx.tools["d8"], 
                ctx.android_jar, 
                ctx.src_dir, 
                java_out, 
                dex_cache, 
                unified_dex_targets,
                extra_jvm_classpaths,
                ctx.run_func
            );
            if (d8_inc_res.is_err()) return d8_inc_res;
        }

        // 5b. Batch-dex external dependency JARs into consolidated DEX slices
        fs::path deps_marker = dex_cache / ".deps_dexed";
        if (!fs::exists(deps_marker) || ctx.diff.needs_dex_rebuild || ctx.force_all) {
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

                std::vector<std::string> d8_batch_args = {
                    "d8",
                    "--min-api", "21",
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
        auto d8_merge_res = run_dex_d8(ctx.tools["d8"], ctx.android_jar, ctx.build_dir, dex_cache, ctx.run_func);
        if (d8_merge_res.is_err()) return d8_merge_res;
    }

    return Result<void>::success();
}
