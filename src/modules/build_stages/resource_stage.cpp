#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

static std::vector<fs::path> resolve_all_dependency_res_dirs(const fs::path& build_dir) {
    std::vector<fs::path> res_dirs;
    std::set<std::string> seen;

    fs::path deps_manifest = build_dir / "dependencies.txt";
    if (!fs::exists(deps_manifest)) {
        return res_dirs;
    }

    std::ifstream in(deps_manifest);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();

        fs::path p(line);
        if (!fs::exists(p)) continue;

        if (p.extension() == ".aar") {
            fs::path ext_res = p.parent_path() / "res";
            if (fs::exists(ext_res) && !fs::is_empty(ext_res)) {
                std::string abs_path = fs::absolute(ext_res).string();
                if (seen.insert(abs_path).second) {
                    res_dirs.push_back(ext_res);
                }
            }
        }
    }

    return res_dirs;
}

Result<void> ResourceStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    fs::path flat_dir = ctx.build_dir / "flat_res";
    fs::create_directories(flat_dir);

    // 1. Gather all extracted AAR dependency resource folders dynamically from dependencies.txt
    std::vector<fs::path> lib_res_dirs = resolve_all_dependency_res_dirs(ctx.build_dir);
    bool missing_flata = false;

    for (const auto& extra_res : lib_res_dirs) {
        // Artifact naming structure: .../<artifact_id>/<version>/res
        std::string lib_name = extra_res.parent_path().parent_path().filename().string();
        std::string lib_version = extra_res.parent_path().filename().string();
        fs::path lib_out_arc = flat_dir / (lib_name + "_" + lib_version + ".flata");
        
        if (!fs::exists(lib_out_arc)) {
            missing_flata = true;
        }
    }

    bool needs_compile = ctx.diff.res_changed || ctx.force_all || missing_flata;
    bool needs_link = ctx.diff.needs_manifest_relink || ctx.force_all || !fs::exists(ctx.build_dir / "unsigned.apk");

    if (!needs_compile && !needs_link) {
        return Result<void>::success();
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
