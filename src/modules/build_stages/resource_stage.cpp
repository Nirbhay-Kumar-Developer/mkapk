#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>

Result<void> ResourceStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    if (!ctx.resources_triggered) {
        return Result<void>::success();
    }

    UI::stage(UI::Msg::RES_STAGE, "Processing resource channels");

    auto comp_res = compile_resources(
        ctx.tools["aapt2"], 
        ctx.res_dir, 
        ctx.build_dir, 
        ctx.run_func, 
        (ctx.diff.res_changed && !ctx.force_all) ? &ctx.diff.changed_resources : nullptr
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
