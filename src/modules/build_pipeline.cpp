#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"
#include "mkapk_plugin_manager.hpp"
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
    ctx.active_plugins = MkapkPluginManager::load_installed_plugins();

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

    // Invalidate resource linking if resources, manifest, or config.json shifted
    ctx.resources_triggered = (ctx.diff.res_changed || ctx.diff.manifest_changed || ctx.diff.config_changed || ctx.force_all);

    // If config.json changed, populate source batches to ensure all code is recompiled
    if (ctx.diff.config_changed && fs::exists(ctx.src_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(ctx.src_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            if (ext == ".java") {
                if (std::find(ctx.diff.changed_files["java"].begin(), ctx.diff.changed_files["java"].end(), entry.path()) == ctx.diff.changed_files["java"].end()) {
                    ctx.diff.changed_files["java"].push_back(entry.path());
                }
            } else if (ext == ".kt") {
                if (std::find(ctx.diff.changed_files["kotlin"].begin(), ctx.diff.changed_files["kotlin"].end(), entry.path()) == ctx.diff.changed_files["kotlin"].end()) {
                    ctx.diff.changed_files["kotlin"].push_back(entry.path());
                }
            }
        }
    }

    // 1. Dependency Resolution & Cache Writing
    DependencyStage dep_stage;
    Result<void> res_dep = dep_stage.execute(config, ctx);
    if (res_dep.is_err()) {
        throw std::runtime_error("Dependency resolution failure: " + res_dep.get_error());
    }

    // 2. Resource Compilation and AAPT2 Manifest/Symbol Linking
    ResourceStage res_stage;
    Result<void> res_resource = res_stage.execute(config, ctx);
    if (res_resource.is_err()) {
        throw std::runtime_error("Resource pipeline failure: " + res_resource.get_error());
    }

    // 3. Detect AAPT2 R.txt ID changes and trigger JVM recompilation if necessary
    fs::path r_txt_path = ctx.build_dir / "R.txt";
    std::string post_link_r_hash = fs::exists(r_txt_path) ? get_file_hash(r_txt_path) : "";
    std::string old_r_hash = ctx.new_state["meta|r_txt"];

    if (post_link_r_hash != old_r_hash) {
        ctx.diff.r_txt_changed = true;
        ctx.diff.needs_jvm_compile = true;
        ctx.diff.needs_dex_rebuild = true;
        ctx.diff.needs_repackage = true;
        ctx.new_state["meta|r_txt"] = post_link_r_hash;

        // Populate both Java and Kotlin source lists so all dependent code rebinds IDs
        if (fs::exists(ctx.src_dir)) {
            for (const auto& entry : fs::recursive_directory_iterator(ctx.src_dir)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                if (ext == ".java") {
                    if (std::find(ctx.diff.changed_files["java"].begin(), ctx.diff.changed_files["java"].end(), entry.path()) == ctx.diff.changed_files["java"].end()) {
                        ctx.diff.changed_files["java"].push_back(entry.path());
                    }
                } else if (ext == ".kt") {
                    if (std::find(ctx.diff.changed_files["kotlin"].begin(), ctx.diff.changed_files["kotlin"].end(), entry.path()) == ctx.diff.changed_files["kotlin"].end()) {
                        ctx.diff.changed_files["kotlin"].push_back(entry.path());
                    }
                }
            }
        }
    }

    // 4. Native C/C++ Compilation (Internal parallel workers handle ABI matrices safely)
    NativeStage native_stage;
    Result<void> res_native = native_stage.execute(config, ctx);
    if (res_native.is_err()) {
        throw std::runtime_error("Native compilation failure: " + res_native.get_error());
    }

    // 5. Java/Kotlin Compilation, D8/R8 Dexing & Dependency JAR Batching
    JvmStage jvm_stage;
    Result<void> res_jvm = jvm_stage.execute(config, ctx);
    if (res_jvm.is_err()) {
        throw std::runtime_error("JVM pipeline failure: " + res_jvm.get_error());
    }

    // 6. Packaging, Zipalign, and Apksigner
    PackageStage pkg_stage;
    Result<void> res_pack = pkg_stage.execute(config, ctx);
    if (res_pack.is_err()) {
        throw std::runtime_error("Packaging failure: " + res_pack.get_error());
    }

    return ctx.final_output_msg;
}
