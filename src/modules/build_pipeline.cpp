#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <future>  
#include <map>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

/**
 * Core asynchronous master orchestration logic.
 * Handles concurrent scheduling across Resource, Native NDK, and JVM layers.
 */
std::string perform_build(const std::vector<std::string>& raw_args, const std::string& config_content) {
    // 1. Setup Environment
    std::string proj_name = MkapkEnv::get_json_val("NAME", config_content);
    fs::path bin_dir = fs::absolute(MkapkEnv::resolve_path(MkapkEnv::get_json_val("BIN_DIR", config_content)));
    fs::path src_dir = fs::absolute(MkapkEnv::resolve_path(MkapkEnv::get_json_val("SRC_DIR", config_content)));
    fs::path res_dir = fs::absolute(MkapkEnv::resolve_path(MkapkEnv::get_json_val("RES_DIR", config_content)));
    fs::path manifest_path = fs::absolute(MkapkEnv::resolve_path(MkapkEnv::get_json_val("MANIFEST", config_content)));
    fs::path android_jar = fs::absolute(MkapkEnv::get_android_jar(config_content));

    fs::create_directories(bin_dir);

    bool is_release = std::find(raw_args.begin(), raw_args.end(), "-release") != raw_args.end();
    bool force_all = std::find(raw_args.begin(), raw_args.end(), "-all") != raw_args.end();
    bool ndk_all = std::find(raw_args.begin(), raw_args.end(), "-ndk-all") != raw_args.end();
    
    // --- INTERCEPT ARCHITECTURE CONTROLS PARAMETERS ---
    std::string arch_target = "";
    auto arch_it = std::find(raw_args.begin(), raw_args.end(), "-arch");
    if (arch_it != raw_args.end() && (arch_it + 1) != raw_args.end()) {
        arch_target = *(arch_it + 1);
    }

    auto tools = MkapkEnv::get_tools_map(config_content);
    std::map<std::string, LanguagePlugin> active_plugins = MkapkEnv::load_installed_plugins();

    RunFunc run_func = [](const std::vector<std::string>& args, const std::string& err_msg) {
        smart_run(args, err_msg);
    };

    // 2. UPDATED: Context-Aware Isolated Change Detection Pass
    auto [diff, new_state] = check_changes(bin_dir, config_content, force_all, is_release);
    if (!diff.any_changes() && !force_all) return "up-to-date";

    // --- SETUP ARCHITECTURE MATRIX CONFIGURATION ---
    std::vector<std::string> compile_architectures;
    bool build_all_abis = (ndk_all || arch_target == "universal" || arch_target == "u");

    if (build_all_abis) {
        compile_architectures = {"armv7a-linux-androideabi", "aarch64-linux-android", "i686-linux-android", "x86_64-linux-android"};
    } else if (!arch_target.empty()) {
        compile_architectures = { arch_target };
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
        compile_architectures = { host_arch };
    }

    // Record flag status for resource layer skipping checks
    bool resources_triggered = (diff.res_changed || diff.manifest_changed || force_all);

    // ============================================================================
    // PARALLEL TASK ORCHESTRATION LAYER
    // ============================================================================

    // WORKER THREAD A: Resource Compilation & Base Layout Processing
    auto resource_worker = std::async(std::launch::async, [&]() {
        if (resources_triggered) {
            std::cout << ">> [ASYNC-STAGE] Processing Resource Channels..." << std::endl;
            compile_resources(tools["aapt2"], res_dir, bin_dir, run_func, 
                             (diff.res_changed && !force_all) ? &diff.changed_resources : nullptr);

            link_manifest(tools["aapt2"], bin_dir / "unsigned.apk", android_jar, 
                         manifest_path, bin_dir, src_dir, run_func, !is_release);
        }
    });

    // WORKER THREAD B: Multi-Threaded Native ABI Compiler Pipelines
    auto native_worker = std::async(std::launch::async, [&]() {
        if (!diff.changed_files["native"].empty() || force_all) {
            std::cout << ">> [ASYNC-STAGE] Processing Native Compilation Matrix..." << std::endl;
            
            // 1. Parse the explicit targets array from config data
            // (Ensure MkapkEnv maps this directly to your inner JSON configuration object router parsing keys)
            std::vector<NativeTargetConfig> targets = MkapkEnv::parse_json_native_targets(config_content);

            // 2. FIXED: Forwarding 'targets' as the 8th parameter to fully satisfy native.cpp rules
            compile_native(MkapkEnv::get_json_val("NDK_BIN", config_content), 
                           src_dir, 
                           bin_dir, 
                           compile_architectures, 
                           MkapkEnv::get_json_val("TARGET_SDK", config_content), 
                           run_func, 
                           diff.changed_files["native"], 
                           targets);
            
            auto_place_system_libraries(config_content, bin_dir, compile_architectures);
        }
    });

    // WORKER THREAD C: Unified JVM Source Compilation & Cascaded Dexing
    auto jvm_worker = std::async(std::launch::async, [&]() {
        if (diff.src_changed || force_all) {
            std::cout << ">> [ASYNC-STAGE] Processing Source Pipelines..." << std::endl;
        }
        auto [java_out, dex_cache] = compile_source_logic(config_content, tools, active_plugins, android_jar, bin_dir, 
                                                        diff.changed_files, diff.deleted_files, 
                                                        (diff.res_changed || force_all), run_func);

        if (is_release) {
            std::cout << ">> [ASYNC-STAGE] Running R8 bytecode optimization..." << std::endl;
            run_dex_r8(tools["r8"], android_jar, config_content, bin_dir, run_func);
        } else {
            std::cout << ">> [ASYNC-STAGE] Running D8 incremental dexing..." << std::endl;
            std::vector<fs::path> unified_dex_targets;
            for (const auto& [lang, files] : diff.changed_files) {
                auto plug_it = active_plugins.find("." + lang);
                if (lang == "java" || lang == "kotlin" || (plug_it != active_plugins.end() && plug_it->second.output_type == "jvm")) {
                    for (const auto& f : files) unified_dex_targets.push_back(f);
                }
            }

            run_incremental_dex(tools["d8"], android_jar, src_dir, java_out, dex_cache, 
                               unified_dex_targets, run_func);
            run_dex_d8(tools["d8"], android_jar, bin_dir, dex_cache, run_func);
        }
    });

    // ============================================================================
    // SYNCHRONIZATION BARRIER BLOCK
    // ============================================================================
    resource_worker.get(); 
    native_worker.get();   
    jvm_worker.get();      

    // ============================================================================
    // MULTI-APK PACKAGING ITERATION LOOP (Runs on Main Thread)
    // ============================================================================
    std::cout << ">> [MAIN-THREAD] All compilation gates passed. Starting assembly matrix..." << std::endl;

    fs::path base_unsigned_apk = bin_dir / "unsigned.apk";
    
    if (!resources_triggered && !fs::exists(base_unsigned_apk)) {
        std::cout << ">> [MAIN-THREAD] Warning: Base unsigned container missing. Regenerating link tables..." << std::endl;
        link_manifest(tools["aapt2"], base_unsigned_apk, android_jar, manifest_path, bin_dir, src_dir, run_func, !is_release);
    }

    std::pair<std::string, std::string> ks_info = is_release ? 
        std::make_pair(MkapkEnv::resolve_path(MkapkEnv::get_json_val("KEYSTORE", config_content)).string(),
                       MkapkEnv::get_json_val("KEYSTORE_ALIAS", config_content)) : handle_debug_keystore();

    std::string profile_suffix = is_release ? ".release" : ".debug";

    struct PackTaskConfig { std::string filename_suffix; std::vector<std::string> target_abis; };
    std::vector<PackTaskConfig> package_matrix;

    if (ndk_all) {
        package_matrix.push_back({"-armv7" + profile_suffix, {"armv7a-linux-androideabi"}});
        package_matrix.push_back({"-arm64" + profile_suffix, {"aarch64-linux-android"}});
        package_matrix.push_back({"-x86" + profile_suffix, {"i686-linux-android"}});
        package_matrix.push_back({"-x86_64" + profile_suffix, {"x86_64-linux-android"}});
        package_matrix.push_back({"-universal" + profile_suffix, compile_architectures});
    } else if (arch_target == "universal" || arch_target == "u") {
        package_matrix.push_back({"-universal" + profile_suffix, compile_architectures});
    } else {
        package_matrix.push_back({profile_suffix, compile_architectures});
    }

    std::string dynamic_ret_path = "";

    for (const auto& task : package_matrix) {
        std::cout << ">> [PACK] Assembling variant " << task.filename_suffix << "..." << std::endl;
        
        fs::path loop_unsigned = bin_dir / ("unsigned" + task.filename_suffix + ".apk");
        fs::copy_file(base_unsigned_apk, loop_unsigned, fs::copy_options::overwrite_existing);

        inject_assets_and_dex(loop_unsigned, bin_dir, MkapkEnv::get_json_val("ASSETS_DIR", config_content), task.target_abis, is_release);

        fs::path aligned_apk = align_apk(tools["zipalign"], "4", loop_unsigned, bin_dir, run_func);
        fs::path final_apk = bin_dir / (proj_name + task.filename_suffix + ".apk");
        
        sign_apk(tools["apksigner"], final_apk, aligned_apk, ks_info.first, ks_info.second, run_func);
        
        if (fs::exists(loop_unsigned)) fs::remove(loop_unsigned);
        if (fs::exists(aligned_apk)) fs::remove(aligned_apk);

        dynamic_ret_path = final_apk.string();
    }

    save_state(bin_dir, new_state, is_release);
    return ndk_all ? "Split architecture compilation matrix generated inside: " + bin_dir.string() : dynamic_ret_path;
}