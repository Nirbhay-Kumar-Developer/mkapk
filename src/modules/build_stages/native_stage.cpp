#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"

Result<void> NativeStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    if (config.native_targets.empty() && config.system_shared_libs.empty()) {
        return Result<void>::success(); // Silent bypass when project has no C/C++ code
    }

    if (!config.native_targets.empty() && (!ctx.diff.changed_files["native"].empty() || ctx.force_all)) {
        UI::stage(UI::Msg::STAGE_NATIVE, UI::Msg::OP_COMPILING_NATIVE);
        compile_native(config.ndk_bin, ctx.src_dir, ctx.build_dir, 
                       ctx.compile_architectures, config.target_sdk, 
                       ctx.run_func, ctx.diff.changed_files["native"], config);
    }
    
    if (!config.system_shared_libs.empty()) {
        UI::stage(UI::Msg::STAGE_NDK_LIBS, UI::Msg::OP_RESOLVING_LIBS);
        auto_place_system_libraries(config, ctx.build_dir, ctx.compile_architectures);
    }

    return Result<void>::success();
}
