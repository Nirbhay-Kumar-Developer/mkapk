#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <iomanip>
#define XXH_INLINE_ALL
#include <xxhash.h>

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

// Generate collision-free names for arbitrarily nested Maven paths
static std::string get_stable_flata_name(const fs::path& extra_res) {
    std::string abs_str = fs::absolute(extra_res).string();
    XXH64_hash_t hash = XXH3_64bits(abs_str.data(), abs_str.size());
    std::stringstream ss;
    ss << "res_" << std::setfill('0') << std::setw(16) << std::hex << hash << ".flata";
    return ss.str();
}

Result<void> ResourceStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    fs::path flat_dir = ctx.build_dir / "flat_res";
    fs::create_directories(flat_dir);

    // 1. Invalidate flat_res cache if config or build profile mode switched
    if (ctx.diff.mode_switched || ctx.diff.config_changed || ctx.force_all) {
        if (fs::exists(flat_dir)) {
            fs::remove_all(flat_dir);
            fs::create_directories(flat_dir);
        }
    }

    // 2. Gather extracted AAR dependency resource folders from dependencies.txt
    std::vector<fs::path> lib_res_dirs = resolve_all_dependency_res_dirs(ctx.build_dir);
    bool missing_flata = false;

    for (const auto& extra_res : lib_res_dirs) {
        fs::path lib_out_arc = flat_dir / get_stable_flata_name(extra_res);
        if (!fs::exists(lib_out_arc)) {
            missing_flata = true;
            break;
        }
    }

    bool needs_compile = ctx.diff.res_changed ||
                         ctx.diff.config_changed ||
                         ctx.diff.mode_switched ||
                         ctx.force_all ||
                         missing_flata;

    bool needs_link = ctx.diff.needs_manifest_relink ||
                      ctx.diff.config_changed ||
                      ctx.diff.mode_switched ||
                      ctx.force_all ||
                      !fs::exists(ctx.build_dir / "unsigned.apk");

    if (!needs_compile && !needs_link) {
        return Result<void>::success();
    }

    // 3. Compile modified or all resources into .flat / .flata files
    if (needs_compile) {
        // Force full pass (pass nullptr for changed_resources) if config or profile changed
        const std::vector<fs::path>* changed_ptr =
            (ctx.diff.res_changed && !ctx.diff.config_changed && !ctx.diff.mode_switched && !ctx.force_all)
            ? &ctx.diff.changed_resources
            : nullptr;

        auto comp_res = compile_resources(
            ctx.tools["aapt2"],
            ctx.res_dir,
            ctx.build_dir,
            ctx.run_func,
            changed_ptr,
            lib_res_dirs
        );
        if (comp_res.is_err()) return comp_res;
    }

    // 4. Resource linking
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
