#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <future>
#include <memory>
#include <stdexcept>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"
#include "pipeline_stage.hpp"

namespace fs = std::filesystem;

std::string perform_build(const std::vector<std::string>& raw_args, const MkapkConfig& config) {
    PipelineContext ctx;
    ctx.raw_args = raw_args;
    ctx.is_release = std::find(raw_args.begin(), raw_args.end(), "-release") != raw_args.end();
    ctx.force_all = std::find(raw_args.begin(), raw_args.end(), "-all") != raw_args.end();
    ctx.ndk_all = std::find(raw_args.begin(), raw_args.end(), "-ndk-all") != raw_args.end();

    auto arch_it = std::find(raw_args.begin(), raw_args.end(), "-arch");
    if (arch_it != raw_args.end() && (arch_it + 1) != raw_args.end()) {
        ctx.arch_target = *(arch_it + 1);
    }

    std::string variant_dir = ctx.is_release ? "release" : "debug";
    ctx.bin_dir = fs::absolute("bin") / variant_dir;
    ctx.build_dir = fs::absolute("build") / variant_dir;
    ctx.src_dir = fs::absolute(MkapkEnv::resolve_path(config.src_dir));
    ctx.res_dir = fs::absolute(MkapkEnv::resolve_path(config.res_dir));
    ctx.manifest_path = fs::absolute(MkapkEnv::resolve_path(config.manifest));
    ctx.active_manifest_path = ctx.manifest_path;
    ctx.android_jar = fs::absolute(MkapkEnv::get_android_jar(config));
    fs::create_directories(ctx.bin_dir);
    fs::create_directories(ctx.build_dir);

    ctx.tools = MkapkEnv::get_tools_map(config);
    ctx.active_plugins = MkapkEnv::load_installed_plugins();

    ctx.run_func = [](const std::vector<std::string>& args, const std::string& err_msg) -> Result<void> {
        return smart_run(args, err_msg);
    };

    bool build_all_abis = (ctx.ndk_all || ctx.arch_target == "universal" || ctx.arch_target == "u");
    if (build_all_abis) {
        ctx.compile_architectures = {"armv7a-linux-androideabi", "aarch64-linux-android", "i686-linux-android", "x86_64-linux-android"};
    } else if (!ctx.arch_target.empty()) {
        ctx.compile_architectures = { ctx.arch_target };
    } else {
        std::string host_arch;
#if defined(__aarch64__)
        host_arch = "aarch64-linux-android";
#elif defined(__arm__)
        host_arch = "armv7a-linux-androideabi";
#elif defined(__x86_64__)
        host_arch = "x86_64-linux-android";
#elif defined(__i386__) || defined(__i686__)
        host_arch = "i686-linux-android";
#else
        host_arch = "aarch64-linux-android"; 
#endif
        ctx.compile_architectures = { host_arch };
    }

    auto [diff, new_state] = check_changes(ctx.build_dir, config, ctx.force_all, ctx.is_release);
    ctx.diff = diff;
    ctx.new_state = new_state;

    if (!ctx.diff.any_changes() && !ctx.force_all) {
        return "up-to-date";
    }

    ctx.resources_triggered = (ctx.diff.res_changed || ctx.diff.manifest_changed || ctx.force_all);
    
    ResourceStage res_stage;
    NativeStage native_stage;
    JvmStage jvm_stage;
    PackageStage pkg_stage;
    
    auto resource_worker = std::async(std::launch::async, [&]() -> Result<void> {
        return res_stage.execute(config, ctx);
    });

    auto native_worker = std::async(std::launch::async, [&]() -> Result<void> {
        return native_stage.execute(config, ctx);
    });

    Result<void> res_resource = resource_worker.get();
    if (res_resource.is_err()) throw std::runtime_error("Resource pipeline failure: " + res_resource.get_error());

    auto jvm_worker = std::async(std::launch::async, [&]() -> Result<void> {
        return jvm_stage.execute(config, ctx);
    });

    Result<void> res_native = native_worker.get();
    if (res_native.is_err()) throw std::runtime_error("Native compilation failure: " + res_native.get_error());

    Result<void> res_jvm = jvm_worker.get();
    if (res_jvm.is_err()) throw std::runtime_error("JVM pipeline failure: " + res_jvm.get_error());

    UI::info("All concurrent compilation tracks synchronization barriers cleared.");

    Result<void> res_pack = pkg_stage.execute(config, ctx);
    if (res_pack.is_err()) throw std::runtime_error("Packaging failure: " + res_pack.get_error());

    return ctx.final_output_msg;
}
