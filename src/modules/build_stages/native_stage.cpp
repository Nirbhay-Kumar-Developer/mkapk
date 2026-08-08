#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"

Result<void> NativeStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    if (!ctx.diff.changed_files["native"].empty() || ctx.force_all) {
        UI::stage(UI::Msg::NATIVE_STAGE, "Compiling multi-architecture variants");
        
        compile_native(config.ndk_bin, 
                       ctx.src_dir, 
                       ctx.build_dir, 
                       ctx.compile_architectures, 
                       config.target_sdk, 
                       ctx.run_func, 
                       ctx.diff.changed_files["native"], 
                       config.native_targets);
    }
    
    if (!config.system_shared_libs.empty() || !ctx.diff.changed_files["native"].empty() || ctx.force_all) {
        auto_place_system_libraries(config, ctx.build_dir, ctx.compile_architectures);
    }

    return Result<void>::success();
}
