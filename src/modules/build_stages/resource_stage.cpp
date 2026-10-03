#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>

namespace fs = std::filesystem;

Result<void> ResourceStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    bool needs_compile = ctx.diff.res_changed || ctx.force_all;
    bool needs_link = ctx.diff.needs_manifest_relink || ctx.force_all || !fs::exists(ctx.build_dir / "unsigned.apk");

    if (!needs_compile && !needs_link) {
        return Result<void>::success();
    }

    // 1. Gather all extracted AAR dependency resource folders
    std::vector<fs::path> lib_res_dirs;
    for (const auto& path : ctx.all_resolved_artifacts) {
        fs::path file_path(path);
        if (file_path.extension() == ".aar") {
            fs::path ext_res = file_path.parent_path() / "res";
            if (fs::exists(ext_res) && !fs::is_empty(ext_res)) {
                lib_res_dirs.push_back(ext_res);
            }
        }
    }

    // 2. Compile modified or all resources into .flat / .flata files
    if (needs_compile) {
        auto comp_res = compile_resources(
            ctx.tools["aapt2"], 
            ctx.res_dir, 
            ctx.build_dir, 
            ctx.run_func, 
            (ctx.diff.res_changed && !ctx.force_all) ? &ctx.diff.changed_resources : nullptr,
            lib_res_dirs
        );
        if (comp_res.is_err()) return comp_res;
    }

    // 3. Resource linking ONLY occurs here
    if (needs_link) {
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
    }

    return Result<void>::success();
}
