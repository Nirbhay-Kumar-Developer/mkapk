#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>

Result<void> ResourceStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    if (!ctx.resources_triggered) {
        return Result<void>::success();
    }

    UI::stage(UI::Msg::RES_STAGE, "Processing resource channels");
    
    std::vector<std::filesystem::path> lib_res_dirs;
    
    for (const auto& path : ctx.all_resolved_artifacts) {
        std::filesystem::path file_path(path);
        if (file_path.extension() == ".aar") {
            // Path-agnostic lookup: extracted res/ directory lives alongside the .aar artifact
            std::filesystem::path ext_res = file_path.parent_path() / "res";
            if (std::filesystem::exists(ext_res) && !std::filesystem::is_empty(ext_res)) {
                lib_res_dirs.push_back(ext_res);
            }
        }
    }

    auto comp_res = compile_resources(
        ctx.tools["aapt2"], 
        ctx.res_dir, 
        ctx.build_dir, 
        ctx.run_func, 
        (ctx.diff.res_changed && !ctx.force_all) ? &ctx.diff.changed_resources : nullptr,
        lib_res_dirs
    );
    if (comp_res.is_err()) return comp_res;

    auto link_res = link_manifest(
        ctx.tools["aapt2"], 
        ctx.build_dir / "unsigned.apk", 
        ctx.android_jar, 
        ctx.active_manifest_path, 
        ctx.build_dir, 
        ctx.src_dir, 
        ctx.run_func, 
        !ctx.is_release
    );
    if (link_res.is_err()) return link_res;

    return Result<void>::success();
}