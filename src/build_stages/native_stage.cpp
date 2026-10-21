#include <vector>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <algorithm>

#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

// Thread-safe overload: receives native changed files by value and holds no reference to ctx.diff
Result<void> NativeStage::execute(
    const MkapkConfig& config,
    const fs::path& src_dir,
    const fs::path& build_dir,
    const std::vector<std::string>& compile_architectures,
    RunFunc run_func,
    std::vector<fs::path> changed_files,
    bool mode_switched,
    bool config_changed,
    bool force_all)
{
    if (config.native_targets.empty() && config.system_shared_libs.empty()) {
        return Result<void>::success();
    }

    // Purge stale compiled shared libraries across ABI target transitions or config modifications
    fs::path lib_dir = build_dir / "lib";
    if (mode_switched || config_changed || force_all) {
        if (fs::exists(lib_dir)) {
            std::error_code ec;
            fs::remove_all(lib_dir, ec);
            fs::create_directories(lib_dir, ec);
        }
    }

    // Fix for BUG-15: If any C/C++ header changed, recompile all native sources in target
    bool header_changed = false;
    for (const auto& file : changed_files) {
        std::string ext = file.extension().string();
        if (ext == ".h" || ext == ".hpp") {
            header_changed = true;
            break;
        }
    }

    // Rebuild native binaries if source files changed, headers shifted, config altered, or forced
    bool needs_native_build = !changed_files.empty() ||
                              header_changed ||
                              config_changed ||
                              mode_switched ||
                              force_all;

    if (!config.native_targets.empty() && needs_native_build) {
        // If a header modified, clear changed_files so compile_native() builds all target sources
        std::vector<fs::path> files_to_compile = header_changed ? std::vector<fs::path>{} : changed_files;

        UI::stage(UI::Msg::STAGE_NATIVE, UI::Msg::OP_COMPILING_NATIVE);
        bool native_ok = compile_native(config.ndk_bin, src_dir, build_dir,
                                        compile_architectures, config.target_sdk,
                                        run_func, files_to_compile, config);
        if (!native_ok) {
            return Result<void>::error("Native compilation stage failed.");
        }
    }

    return Result<void>::success();
}

// Concrete virtual override implementation for PipelineStage interface
Result<void> NativeStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    std::vector<fs::path> changed_native;
    auto it = ctx.diff.changed_files.find("native");
    if (it != ctx.diff.changed_files.end()) {
        changed_native = it->second;
    }

    return execute(
        config,
        ctx.src_dir,
        ctx.build_dir,
        ctx.compile_architectures,
        ctx.run_func,
        std::move(changed_native),
        ctx.diff.mode_switched,
        ctx.diff.config_changed,
        ctx.force_all
    );
}
