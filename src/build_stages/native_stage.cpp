#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include <filesystem>

namespace fs = std::filesystem;

Result<void> NativeStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    if (config.native_targets.empty() && config.system_shared_libs.empty()) {
        return Result<void>::success();
    }

    // Purge stale compiled shared libraries across ABI target transitions or config modifications
    fs::path lib_dir = ctx.build_dir / "lib";
    if (ctx.diff.mode_switched || ctx.diff.config_changed || ctx.force_all) {
        if (fs::exists(lib_dir)) {
            fs::remove_all(lib_dir);
            fs::create_directories(lib_dir);
        }
    }

    // Rebuild native binaries if source files changed, config shifted, or a rebuild is forced
    bool needs_native_build = !ctx.diff.changed_files["native"].empty() ||
                              ctx.diff.config_changed ||
                              ctx.diff.mode_switched ||
                              ctx.force_all;

    if (!config.native_targets.empty() && needs_native_build) {
        UI::stage(UI::Msg::STAGE_NATIVE, UI::Msg::OP_COMPILING_NATIVE);
        bool native_ok = compile_native(config.ndk_bin, ctx.src_dir, ctx.build_dir,
                                        ctx.compile_architectures, config.target_sdk,
                                        ctx.run_func, ctx.diff.changed_files["native"], config);
        if (!native_ok) {
            return Result<void>::error("Native compilation stage failed.");
        }
    }

    // Auto-resolve system or prebuilt dependency libraries into target ABI directories
    if (!config.system_shared_libs.empty() || ctx.diff.config_changed || ctx.force_all) {
        UI::stage(UI::Msg::STAGE_NDK_LIBS, UI::Msg::OP_RESOLVING_LIBS);
        auto_place_system_libraries(config, ctx.build_dir, ctx.compile_architectures);
    }

    return Result<void>::success();
}
