#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"

Result<void> PackageStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    std::filesystem::path base_unsigned_apk = ctx.build_dir / "unsigned.apk";
    
    if (!ctx.resources_triggered && !std::filesystem::exists(base_unsigned_apk)) {
        UI::warn("Base container missing. Forcing resource link pass resolution...");
        link_manifest(ctx.tools["aapt2"], base_unsigned_apk, ctx.android_jar, ctx.active_manifest_path, ctx.build_dir, ctx.src_dir, ctx.run_func, !ctx.is_release);
    }

    if (!std::filesystem::exists(base_unsigned_apk)) {
        return Result<void>::error("Fatal Build Error: Base unsigned APK container is missing. Halting packaging loop.");
    }

    std::pair<std::string, std::string> ks_info;
    if (ctx.is_release) {
        std::filesystem::path resolved_ks = MkapkEnv::resolve_path(config.keystore);
        if (std::filesystem::exists(std::filesystem::current_path() / "keystores" / resolved_ks.filename())) {
            ks_info = {(std::filesystem::current_path() / "keystores" / resolved_ks.filename()).string(), config.keystore_alias};
        } else {
            ks_info = {resolved_ks.string(), config.keystore_alias};
        }
    } else {
        auto ks_res = handle_debug_keystore();
        if (ks_res.is_err()) return Result<void>::error(ks_res.get_error());
        ks_info = ks_res.get_value();
    }

    std::string profile_suffix = ctx.is_release ? ".release" : ".debug";
    struct PackTaskConfig { std::string filename_suffix; std::vector<std::string> target_abis; };
    std::vector<PackTaskConfig> package_matrix;

    if (ctx.ndk_all) {
        package_matrix.push_back({"-armv7" + profile_suffix, {"armv7a-linux-androideabi"}});
        package_matrix.push_back({"-arm64" + profile_suffix, {"aarch64-linux-android"}});
        package_matrix.push_back({"-x86" + profile_suffix, {"i686-linux-android"}});
        package_matrix.push_back({"-x86_64" + profile_suffix, {"x86_64-linux-android"}});
        package_matrix.push_back({"-universal" + profile_suffix, ctx.compile_architectures});
    } else if (ctx.arch_target == "universal" || ctx.arch_target == "u") {
        package_matrix.push_back({"-universal" + profile_suffix, ctx.compile_architectures});
    } else {
        package_matrix.push_back({profile_suffix, ctx.compile_architectures});
    }

    std::string dynamic_ret_path = "";

    for (const auto& task : package_matrix) {
        UI::stage(UI::Msg::PACK_STAGE, "Variant target: " + task.filename_suffix);
        
        std::filesystem::path loop_unsigned = ctx.build_dir / ("unsigned" + task.filename_suffix + ".apk");
        std::filesystem::copy_file(base_unsigned_apk, loop_unsigned, std::filesystem::copy_options::overwrite_existing);

        auto inject_res = inject_assets_and_dex(loop_unsigned, ctx.build_dir, config.assets_dir, task.target_abis, ctx.is_release);
        if (inject_res.is_err()) return inject_res;

        std::filesystem::path target_processed_apk = loop_unsigned;
        if (ctx.is_release) {
            std::filesystem::path resguard_jar = MkapkEnv::resolve_path("~/AndResGuard/AndResGuard-cli-1.2.15.jar");
            std::filesystem::path config_xml = std::filesystem::current_path() / "andresguard.xml";
            
            if (std::filesystem::exists(resguard_jar) && std::filesystem::exists(config_xml)) {
                target_processed_apk = obfuscate_resources(ctx.tools["resguard"], loop_unsigned, ctx.build_dir, ctx.run_func);
            }
        }

        std::filesystem::path aligned_apk = align_apk(ctx.tools["zipalign"], "4", target_processed_apk, ctx.build_dir, ctx.run_func);
        std::filesystem::path final_apk = ctx.bin_dir / (config.project_name + task.filename_suffix + ".apk");
        
        UI::stage(UI::Msg::SIGN_STAGE, final_apk.filename().string());
        auto sign_res = sign_apk(ctx.tools["apksigner"], final_apk, aligned_apk, ks_info.first, ks_info.second, ctx.run_func);
        if (sign_res.is_err()) return sign_res;
        
        if (std::filesystem::exists(loop_unsigned)) std::filesystem::remove(loop_unsigned);

        dynamic_ret_path = final_apk.string();
    }

    save_state(ctx.build_dir, ctx.new_state, ctx.is_release);
    
    ctx.final_output_msg = ctx.ndk_all ? 
        "Split architecture packaging structural distribution layout written within: " + ctx.bin_dir.string() : 
        dynamic_ret_path;

    return Result<void>::success();
}
