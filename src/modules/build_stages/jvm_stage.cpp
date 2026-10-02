#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>

namespace fs = std::filesystem;

Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    fs::path java_out = ctx.build_dir / "classes" / "java_classes";
    fs::path dex_cache = ctx.build_dir / "dex_cache";

    // 1. Only run javac / kotlinc if code modified, R.txt changed, or full rebuild requested
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
            ctx.run_func
        );
        
        if (logic_res.is_err()) {
            return Result<void>::error(logic_res.get_error());
        }
        
        java_out = logic_res.get_value().first;
        dex_cache = logic_res.get_value().second;
    }

    // 2. Early exit: If DEX artifacts exist and no rebuild triggers were flagged, skip D8/R8
    fs::path target_classes_dex = ctx.build_dir / "classes.dex";
    if (!ctx.diff.needs_dex_rebuild && !ctx.force_all && fs::exists(target_classes_dex)) {
        return Result<void>::success();
    }

    // 3. Release Mode: R8 Whole-Program Optimization & Tree Shaking
    if (ctx.is_release) {
        UI::stage(UI::Msg::STAGE_MINIFY, UI::Msg::OP_R8_OPTIMIZE);
        auto r8_res = run_dex_r8(ctx.tools["r8"], ctx.android_jar, config, ctx.build_dir, ctx.run_func, false);
        if (r8_res.is_err()) return r8_res;
    } 
    // 4. Debug Mode: Incremental D8 Translation & Merge
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
                ctx.run_func
            );
            if (d8_inc_res.is_err()) return d8_inc_res;
        }

        // Merge cached incremental DEX files into final classes.dex
        auto d8_merge_res = run_dex_d8(ctx.tools["d8"], ctx.android_jar, ctx.build_dir, dex_cache, ctx.run_func);
        if (d8_merge_res.is_err()) return d8_merge_res;
    }

    return Result<void>::success();
}
