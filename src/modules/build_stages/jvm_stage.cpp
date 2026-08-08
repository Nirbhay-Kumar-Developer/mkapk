#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>

Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    if (ctx.diff.src_changed || ctx.force_all) {
        UI::stage("Source Pipeline", "Analyzing active code changes");
    }

    // Execute compilation logic, directly passing classpaths to Kotlinc and Javac
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
    
    auto [java_out, dex_cache] = logic_res.get_value();

    if (ctx.is_release) {
        UI::stage("Minification", "Running R8 bytecode optimization");
        auto r8_res = run_dex_r8(ctx.tools["r8"], ctx.android_jar, config, ctx.build_dir, ctx.run_func, false);
        if (r8_res.is_err()) return r8_res;
    } else {
        UI::stage("Dexing", "Running D8 incremental translation");
        std::vector<std::filesystem::path> unified_dex_targets;
        for (const auto& [lang, files] : ctx.diff.changed_files) {
            auto plug_it = ctx.active_plugins.find("." + lang);
            if (lang == "java" || lang == "kotlin" || (plug_it != ctx.active_plugins.end() && plug_it->second.output_type == "jvm")) {
                for (const auto& f : files) unified_dex_targets.push_back(f);
            }
        }

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

        std::vector<std::filesystem::path> jars_to_dex;

        if (!jars_to_dex.empty()) {
            std::vector<std::string> d8_library_args = {
                "d8",
                "--lib", std::filesystem::absolute(ctx.android_jar).string(),
                "--output", dex_cache.string()
            };
            for (const auto& j : jars_to_dex) d8_library_args.push_back(j.string());
            
            auto d8_lib_res = ctx.run_func(d8_library_args, "Failed compilation of external library classes into DEX cache slots.");
            if (d8_lib_res.is_err()) return d8_lib_res;
        }

        auto d8_merge_res = run_dex_d8(ctx.tools["d8"], ctx.android_jar, ctx.build_dir, dex_cache, ctx.run_func);
        if (d8_merge_res.is_err()) return d8_merge_res;
    }

    return Result<void>::success();
}
