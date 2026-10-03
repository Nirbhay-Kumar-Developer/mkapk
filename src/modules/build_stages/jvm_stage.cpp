#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>

namespace fs = std::filesystem;

Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    fs::path java_out = ctx.build_dir / "classes" / "java_classes";
    fs::path dex_cache = ctx.build_dir / "dex_cache";

    // 1. Filter and construct the strict classpath for active dependencies ONLY
    std::vector<fs::path> extra_jvm_classpaths;
    for (const auto& artifact : ctx.all_resolved_artifacts) {
        fs::path file_path(artifact);
        if (file_path.extension() == ".aar") {
            fs::path classes_jar = file_path.parent_path() / "classes.jar";
            if (fs::exists(classes_jar)) {
                extra_jvm_classpaths.push_back(classes_jar);
            }
        } else if (file_path.extension() == ".jar") {
            if (fs::exists(file_path)) {
                extra_jvm_classpaths.push_back(file_path);
            }
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
        auto r8_res = run_dex_r8(ctx.tools["r8"], ctx.android_jar, config, ctx.build_dir, ctx.run_func, false);
        if (r8_res.is_err()) return r8_res;
    } 
    // 5. Debug Mode: Incremental D8 Translation & Merge
    else {
        UI::stage(UI::Msg::STAGE_DEX);
        
        std::vector<fs::path> unified_dex_targets;
        for (const auto& [lang, files] : ctx.diff.changed_files) {
            auto plug_it = ctx.active_plugins.find("." + lang);
            if (lang == "java" || lang == "kotlin" || (plug_it != ctx.active_plugins.end() && plug_it->second.output_type == "jvm")) {
                for (const auto& f : files) unified_dex_targets.push_back(f);
            }
        }

        // Only run incremental translation if there are modified classes
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

        // Pre-dex external dependency JARs if not yet cached in dex_cache
        std::vector<fs::path> jars_to_dex;
        for (const auto& jar : extra_jvm_classpaths) {
            fs::path target_cached_dex = dex_cache / jar.filename().replace_extension(".dex");
            if (!fs::exists(target_cached_dex) || ctx.force_all) {
                jars_to_dex.push_back(jar);
            }
        }

        if (!jars_to_dex.empty()) {
            std::vector<std::string> d8_library_args = {
                "d8",
                "--lib", fs::absolute(ctx.android_jar).string(),
                "--output", dex_cache.string()
            };
            for (const auto& j : jars_to_dex) d8_library_args.push_back(j.string());
            
            auto d8_lib_res = ctx.run_func(d8_library_args, "Failed compilation of external library classes into DEX cache slots.");
            if (d8_lib_res.is_err()) return d8_lib_res;
        }

        // Merge cached incremental DEX files into final classes.dex
        auto d8_merge_res = run_dex_d8(ctx.tools["d8"], ctx.android_jar, ctx.build_dir, dex_cache, ctx.run_func);
        if (d8_merge_res.is_err()) return d8_merge_res;
    }

    return Result<void>::success();
}
