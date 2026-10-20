#include <vector>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <future>
#include <mutex>
#include <system_error>

#include "pipeline_stage.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_helpers.hpp"
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

static Result<fs::path> align_apk_isolated(const std::string& ZIPALIGN, const std::string& alignment,
                                           const fs::path& in_apk, const fs::path& dest_apk, RunFunc run_func) {
    if (fs::exists(dest_apk)) {
        fs::remove(dest_apk);
    }

    // Android zipalign accepts: zipalign -f -P <pagesize_kb> <align> <infile> <outfile>
    // Note: '-P' cannot be used in combination with '-p'.
    auto align_res = run_func({
        ZIPALIGN, "-f", "-P", "16", alignment,
        in_apk.string(), dest_apk.string()
    }, "Running zipalign on " + in_apk.filename().string());

    if (align_res.is_err() || !fs::exists(dest_apk)) {
        std::string err_msg = align_res.is_err() ? align_res.get_error() : "Aligned output file not created.";
        return Result<fs::path>::error("Zipalign failed for " + in_apk.filename().string() + ": " + err_msg);
    }

    return Result<fs::path>::success(dest_apk);
}

Result<void> PackageStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    if (!ctx.diff.needs_repackage && !ctx.diff.config_changed && !ctx.force_all) {
        return Result<void>::success();
    }

    fs::path base_unsigned_apk = ctx.build_dir / "unsigned.apk";

    if (!ctx.resources_triggered && !fs::exists(base_unsigned_apk)) {
        UI::warn("Base container missing. Forcing resource link pass resolution...");
        auto link_res = link_manifest(ctx.tools["aapt2"], base_unsigned_apk, ctx.android_jar, ctx.active_manifest_path, ctx.build_dir, ctx.src_dir, ctx.run_func, !ctx.is_release);
        if (link_res.is_err()) return link_res;
    }

    if (!fs::exists(base_unsigned_apk)) {
        return Result<void>::error(UI::Msg::FATAL_INTERNAL);
    }

    // --- SINGLE-PASS RESOURCE OBFUSCATION ---
    fs::path base_container = base_unsigned_apk;
    fs::path obfuscated_cached_apk = ctx.build_dir / "unsigned_obfuscated.apk";

    if (ctx.is_release) {
        fs::path resguard_jar = MkapkEnv::resolve_path("/data/data/com.termux/files/usr/share/mkapk/tools/AndResGuard-cli-1.2.15.jar");
        fs::path config_xml = fs::current_path() / "andresguard-config.xml";
        if (!fs::exists(config_xml)) config_xml = fs::current_path() / "andresguard.xml";

        if (fs::exists(resguard_jar) && fs::exists(config_xml)) {
            bool must_obfuscate = ctx.diff.needs_resource_obfuscation ||
                                  ctx.diff.config_changed ||
                                  ctx.force_all ||
                                  !fs::exists(obfuscated_cached_apk);

            if (must_obfuscate) {
                fs::path resguard_res = obfuscate_resources(
                    ctx.tools["resguard"],
                    base_unsigned_apk,
                    ctx.build_dir,
                    ctx.run_func
                );

                if (fs::exists(resguard_res) && resguard_res != base_unsigned_apk) {
                    fs::copy_file(resguard_res, obfuscated_cached_apk, fs::copy_options::overwrite_existing);
                    base_container = obfuscated_cached_apk;
                }
            } else if (fs::exists(obfuscated_cached_apk)) {
                base_container = obfuscated_cached_apk;
            }
        } else {
            UI::warn(UI::Msg::WARN_RESGUARD_MISS);
        }
    }

    // Keystore configuration
    std::pair<std::string, std::string> ks_info;
    if (ctx.is_release) {
        fs::path resolved_ks = MkapkEnv::resolve_path(config.keystore);
        if (fs::exists(fs::current_path() / "keystores" / resolved_ks.filename())) {
            ks_info = {(fs::current_path() / "keystores" / resolved_ks.filename()).string(), config.keystore_alias};
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

    std::mutex signing_ipc_mutex;
    std::vector<std::future<Result<std::string>>> workers;
    workers.reserve(package_matrix.size());

    for (const auto& task : package_matrix) {
        workers.push_back(std::async(std::launch::async, [&ctx, &config, &ks_info, &signing_ipc_mutex, task, base_container]() -> Result<std::string> {
            fs::path loop_unsigned = ctx.build_dir / ("unsigned" + task.filename_suffix + ".apk");

            std::error_code ec;
            fs::copy_file(base_container, loop_unsigned, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return Result<std::string>::error("Failed to copy base APK for " + task.filename_suffix + ": " + ec.message());
            }

            auto inject_res = inject_assets_and_dex(loop_unsigned, ctx.build_dir, config.assets_dir, task.target_abis, ctx.is_release);
            if (inject_res.is_err()) {
                if (fs::exists(loop_unsigned)) fs::remove(loop_unsigned);
                return Result<std::string>::error(inject_res.get_error());
            }

            fs::path isolated_aligned_apk = ctx.build_dir / ("aligned_temp" + task.filename_suffix + ".apk");
            auto align_res = align_apk_isolated(ctx.tools["zipalign"], "4", loop_unsigned, isolated_aligned_apk, ctx.run_func);
            if (align_res.is_err()) {
                if (fs::exists(loop_unsigned)) fs::remove(loop_unsigned);
                return Result<std::string>::error(align_res.get_error());
            }

            fs::path final_apk = ctx.bin_dir / (config.project_name + task.filename_suffix + ".apk");

            Result<void> sign_res = Result<void>::success();
            {
                std::lock_guard<std::mutex> lock(signing_ipc_mutex);
                UI::stage(UI::Msg::STAGE_SIGN, final_apk.filename().string());
                sign_res = sign_apk(ctx.tools["apksigner"], final_apk, isolated_aligned_apk, ks_info.first, ks_info.second, ctx.run_func);
            }

            if (fs::exists(loop_unsigned)) fs::remove(loop_unsigned);
            if (fs::exists(isolated_aligned_apk)) fs::remove(isolated_aligned_apk);

            if (sign_res.is_err()) {
                return Result<std::string>::error(sign_res.get_error());
            }

            return Result<std::string>::success(final_apk.string());
        }));
    }

    std::string dynamic_ret_path = "";
    for (auto& worker : workers) {
        auto res = worker.get();
        if (res.is_err()) {
            return Result<void>::error(res.get_error());
        }
        dynamic_ret_path = res.get_value();
    }

    if (fs::exists(ctx.build_dir / "resguard_out")) {
        fs::remove_all(ctx.build_dir / "resguard_out");
    }

    save_state(ctx.build_dir, ctx.new_state, ctx.is_release);

    ctx.final_output_msg = ctx.ndk_all ? ctx.bin_dir.string() : dynamic_ret_path;
    return Result<void>::success();
}
