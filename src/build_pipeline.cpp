#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <future>
#include <stdexcept>
#include <map>
#include <utility>

#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_config.hpp"
#include "mkapk_plugin_manager.hpp"
#include "pipeline_stage.hpp"
#include "mkapk_result.hpp"

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
        ctx.compile_architectures = { MkapkEnv::get_host_arch() };
    }

    auto [diff, new_state] = check_changes(ctx.build_dir, config, ctx.force_all, ctx.is_release);
    ctx.diff = diff;
    ctx.new_state = new_state;

    if (!ctx.diff.any_changes() && !ctx.force_all) {
        return "up-to-date";
    }

    ctx.resources_triggered = (ctx.diff.res_changed || ctx.diff.manifest_changed || ctx.diff.config_changed || ctx.force_all);

    // Deduplicated source scanner helper
    auto enqueue_all_sources = [&]() {
        if (!fs::exists(ctx.src_dir)) return;
        for (const auto& entry : fs::recursive_directory_iterator(ctx.src_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            if (ext == ".java") {
                auto& java_files = ctx.diff.changed_files["java"];
                if (std::find(java_files.begin(), java_files.end(), entry.path()) == java_files.end()) {
                    java_files.push_back(entry.path());
                }
            } else if (ext == ".kt") {
                auto& kt_files = ctx.diff.changed_files["kotlin"];
                if (std::find(kt_files.begin(), kt_files.end(), entry.path()) == kt_files.end()) {
                    kt_files.push_back(entry.path());
                }
            }
        }
    };

    if (ctx.diff.config_changed) {
        enqueue_all_sources();
    }

    // STEP 1: PARALLEL EXECUTION OF NATIVE STAGE & DEPENDENCY STAGE

    // Safely snapshot native changed files and flags on the main thread
    std::vector<fs::path> changed_native_files;
    auto nat_it = ctx.diff.changed_files.find("native");
    if (nat_it != ctx.diff.changed_files.end()) {
        changed_native_files = nat_it->second;
    }
    bool native_mode_switched = ctx.diff.mode_switched;
    bool native_config_changed = ctx.diff.config_changed;
    bool force_all = ctx.force_all;
    fs::path src_dir = ctx.src_dir;
    fs::path build_dir = ctx.build_dir;
    std::vector<std::string> compile_architectures = ctx.compile_architectures;
    RunFunc run_func = ctx.run_func;

    // Launch Native Compilation (Clang/NDK) immediately in the background.
    // Captured by value: the worker holds NO references to ctx or the mutable ctx.diff container.
    NativeStage native_stage;
    auto native_future = std::async(
        std::launch::async,
        [native_stage, &config, src_dir, build_dir, compile_architectures, run_func,
         changed_native_files = std::move(changed_native_files),
         mode_switched = native_mode_switched,
         config_changed = native_config_changed,
         force_all]() mutable -> Result<void> {
            return native_stage.execute(
                config,
                src_dir,
                build_dir,
                compile_architectures,
                run_func,
                std::move(changed_native_files),
                mode_switched,
                config_changed,
                force_all
            );
        }
    );

    // Run Dependency Stage (Aether Resolver, AAR Extraction, Manifest Merger)
    DependencyStage dep_stage;
    Result<void> res_dep = dep_stage.execute(config, ctx);
    if (res_dep.is_err()) {
        if (native_future.valid()) native_future.wait();
        throw std::runtime_error("Dependency resolution failure: " + res_dep.get_error());
    }

    // STEP 2: RESOURCE STAGE (Runs concurrently with remaining Native tasks)

    // Launch ResourceStage now that library AARs and manifests are resolved and extracted
    ResourceStage res_stage;
    auto resource_future = std::async(std::launch::async, [&]() -> Result<void> {
        return res_stage.execute(config, ctx);
    });

    // Await ResourceStage to verify AAPT2 symbol output
    Result<void> res_resource = resource_future.get();
    if (res_resource.is_err()) {
        if (native_future.valid()) native_future.wait();
        throw std::runtime_error("Resource pipeline failure: " + res_resource.get_error());
    }

    // STEP 3: SYMBOL SHIFT DETECTION & JVM RECOMPILATION DIRECTIVE
    fs::path r_txt_path = ctx.build_dir / "R.txt";
    std::string post_link_r_hash = fs::exists(r_txt_path) ? get_file_hash(r_txt_path) : "";
    std::string old_r_hash = ctx.new_state["meta|r_txt"];

    if (post_link_r_hash != old_r_hash) {
        ctx.diff.r_txt_changed = true;
        ctx.diff.needs_jvm_compile = true;
        ctx.diff.needs_dex_rebuild = true;
        ctx.diff.needs_repackage = true;
        ctx.new_state["meta|r_txt"] = post_link_r_hash;

        enqueue_all_sources();
    }

    // STEP 4: JVM STAGE (Runs concurrently with finishing Native Stage)
    JvmStage jvm_stage;
    auto jvm_future = std::async(std::launch::async, [&]() -> Result<void> {
        return jvm_stage.execute(config, ctx);
    });

    // Await both NativeStage and JvmStage completion
    Result<void> res_native = native_future.get();
    if (res_native.is_err()) {
        if (jvm_future.valid()) jvm_future.wait();
        throw std::runtime_error("Native compilation failure: " + res_native.get_error());
    }

    Result<void> res_jvm = jvm_future.get();
    if (res_jvm.is_err()) {
        throw std::runtime_error("JVM pipeline failure: " + res_jvm.get_error());
    }

    // STEP 5: PACKAGING, ZIPALIGN, AND APKSIGNER
    PackageStage pkg_stage;
    Result<void> res_pack = pkg_stage.execute(config, ctx);
    if (res_pack.is_err()) {
        throw std::runtime_error("Packaging failure: " + res_pack.get_error());
    }

    return ctx.final_output_msg;
}
