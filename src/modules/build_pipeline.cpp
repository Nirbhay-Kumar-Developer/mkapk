#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <future>  
#include <map>
#include <functional>
#include <stdexcept>
#include <exception>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"

// Include the newly wired dependency resolver, extractor, and manifest merger modules
#include "mkapk_resolver.hpp"
#include "mkapk_extractor.hpp" 
#include "mkapk_manifest_merger.hpp"

namespace fs = std::filesystem;

/**
 * Core asynchronous master orchestration logic.
 * Handles concurrent scheduling across Resource, Native NDK, and JVM layers.
 */
std::string perform_build(const std::vector<std::string>& raw_args, const MkapkConfig& config) {
    // 1. Setup Environment & Sanitization
    std::string proj_name = config.project_name;
    
    bool is_release = std::find(raw_args.begin(), raw_args.end(), "-release") != raw_args.end();
    bool force_all = std::find(raw_args.begin(), raw_args.end(), "-all") != raw_args.end();
    bool ndk_all = std::find(raw_args.begin(), raw_args.end(), "-ndk-all") != raw_args.end();
    
    std::string variant_dir = is_release ? "release" : "debug";

    // Optimized Directories Matrix
    fs::path bin_dir = fs::absolute("bin") / variant_dir;
    fs::path build_dir = fs::absolute("build") / variant_dir;
    
    fs::path src_dir = fs::absolute(MkapkEnv::resolve_path(config.src_dir));
    fs::path res_dir = fs::absolute(MkapkEnv::resolve_path(config.res_dir));
    fs::path manifest_path = fs::absolute(MkapkEnv::resolve_path(config.manifest));
    fs::path android_jar = fs::absolute(MkapkEnv::get_android_jar(config));

    fs::create_directories(bin_dir);
    fs::create_directories(build_dir);

    // ============================================================================
    // STEP 1: ON-DEVICE DEPENDENCY RESOLUTION & CACHE VALIDATION PASSTHROUGH
    // ============================================================================
    std::vector<std::string> all_resolved_artifacts;
    fs::path active_manifest_path = manifest_path; 

    if (!config.dependencies.empty()) {
        UI::stage("Resolver", "Processing root dependency configuration matrix");

        for (const auto& coordinate : config.dependencies) {
            // MkapkResolver checks local storage internally before contacting remote Maven networks
            auto resolved = MkapkResolver::resolve_dependencies(coordinate, config);
            all_resolved_artifacts.insert(all_resolved_artifacts.end(), resolved.begin(), resolved.end());
        }

        // De-duplicate layout items safely to resolve graph collisions
        std::sort(all_resolved_artifacts.begin(), all_resolved_artifacts.end());
        auto last = std::unique(all_resolved_artifacts.begin(), all_resolved_artifacts.end());
        all_resolved_artifacts.erase(last, all_resolved_artifacts.end());

        // Unpack new elements automatically to $PREFIX/var/lib/mkapk/lib/ via libzip
        MkapkExtractor::extract_all(all_resolved_artifacts);

        // Merge library manifests down into the local build subdirectory workspace
        fs::path merged_manifest_output = build_dir / "AndroidManifest.xml";
        bool merge_success = MkapkManifestMerger::merge_manifests(
            manifest_path.string(), 
            merged_manifest_output.string(), 
            all_resolved_artifacts
        );

        if (merge_success) {
            active_manifest_path = merged_manifest_output;
        } else {
            UI::warn("Manifest integration anomaly caught. Falling back to primary configuration file layout.");
        }
    }

    // --- INTERCEPT ARCHITECTURE CONTROLS PARAMETERS ---
    std::string arch_target = "";
    auto arch_it = std::find(raw_args.begin(), raw_args.end(), "-arch");
    if (arch_it != raw_args.end() && (arch_it + 1) != raw_args.end()) {
        arch_target = *(arch_it + 1);
    }

    auto tools = MkapkEnv::get_tools_map(config);
    std::map<std::string, LanguagePlugin> active_plugins = MkapkEnv::load_installed_plugins();

    RunFunc run_func = [](const std::vector<std::string>& args, const std::string& err_msg) {
        smart_run(args, err_msg);
    };

    // 2. Context-Aware Isolated Change Detection Pass (Passing build_dir path framework)
    auto [diff, new_state] = check_changes(build_dir, config, force_all, is_release);
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

    // Record flag status for resource layer skipping checks (Uses the updated manifest tracking context)
    bool resources_triggered = (diff.res_changed || diff.manifest_changed || force_all || !all_resolved_artifacts.empty());

    // ============================================================================
    // PARALLEL TASK ORCHESTRATION LAYER (ANTI-DEADLOCK POOLING)
    // ============================================================================

    // WORKER THREAD A: Resource Compilation & Base Layout Processing
    auto resource_worker = std::async(std::launch::async, [&]() {
        if (resources_triggered) {
            UI::stage(UI::Msg::RES_STAGE, "Processing resource channels");
            
            // Step 1: Compile application raw resources via res.cpp
            compile_resources(tools["aapt2"], res_dir, build_dir, run_func, 
                             (diff.res_changed && !force_all) ? &diff.changed_resources : nullptr);

            // Step 2: Extract AAR dependency names to fetch extracted resource maps for AAPT2 link passes
            std::vector<std::string> aar_res_dirs;
            const char* prefix_env = std::getenv("PREFIX");
            fs::path prefix_path = prefix_env ? fs::path(prefix_env) : "/data/data/com.termux/files/usr";
            
            for (const auto& path : all_resolved_artifacts) {
                fs::path file_path(path);
                if (file_path.extension() == ".aar") {
                    auto parent = file_path.parent_path();
                    std::string version = parent.filename().string();
                    std::string library_name = parent.parent_path().filename().string();
                    
                    fs::path ext_res = prefix_path / "var/lib/mkapk/lib" / library_name / version / "res";
                    if (fs::exists(ext_res) && !fs::is_empty(ext_res)) {
                        // Compile library raw resource trees cleanly straight into your flat cache workspace
                        compile_resources(tools["aapt2"], ext_res, build_dir, run_func, nullptr);
                    }
                }
            }

            // Step 3: Package final linked layout containing the dynamically constructed manifest path configuration
            link_manifest(tools["aapt2"], build_dir / "unsigned.apk", android_jar, 
                         active_manifest_path, build_dir, src_dir, run_func, !is_release);
        }
    });

    // WORKER THREAD B: Multi-Threaded Native ABI Compiler Pipelines
    auto native_worker = std::async(std::launch::async, [&]() {
        if (!diff.changed_files["native"].empty() || force_all) {
            UI::stage(UI::Msg::NATIVE_STAGE, "Compiling multi-architecture variants");
            
            compile_native(config.ndk_bin, 
                           src_dir, 
                           build_dir, 
                           compile_architectures, 
                           config.target_sdk, 
                           run_func, 
                           diff.changed_files["native"], 
                           config.native_targets);
        }
        
        // System libraries are placed independently of native source compilation.
        if (!config.system_shared_libs.empty() || !diff.changed_files["native"].empty() || force_all) {
            auto_place_system_libraries(config, build_dir, compile_architectures);
        }
    });

    // ============================================================================
    // THREAD SYNCHRONIZATION & EXCEPTION ROUTING
    // ============================================================================
    std::exception_ptr pipeline_err = nullptr;
    std::string err_stage;

    // BARRIER 1: Wait for resources[span_1](start_span)[span_1](end_span). Move directly above JVM worker initialization to 
    // guarantee R.java code generation maps are completely flushed to disk[span_2](start_span)[span_2](end_span).
    try {
        resource_worker.get();
    } catch (...) {
        pipeline_err = std::current_exception();
        err_stage = "Resource pipeline failure";
    }

    // WORKER THREAD C: Unified JVM Source Compilation & Cascaded Dexing
    std::future<void> jvm_worker;
    if (!pipeline_err) {
        jvm_worker = std::async(std::launch::async, [&]() {
            if (diff.src_changed || force_all) {
                UI::stage("Source Pipeline", "Analyzing active code changes");
            }

            // Append extracted classes.jar files down to the compilation configuration list
            const char* prefix_env = std::getenv("PREFIX");
            fs::path prefix_path = prefix_env ? fs::path(prefix_env) : "/data/data/com.termux/files/usr";
            std::vector<fs::path> extra_jvm_classpaths;

            for (const auto& artifact : all_resolved_artifacts) {
                fs::path file_path(artifact);
                if (file_path.extension() == ".aar") {
                    auto parent = file_path.parent_path();
                    std::string version = parent.filename().string();
                    std::string library_name = parent.parent_path().filename().string();
                    
                    fs::path classes_jar = prefix_path / "var/lib/mkapk/lib" / library_name / version / "classes.jar";
                    if (fs::exists(classes_jar)) {
                        extra_jvm_classpaths.push_back(classes_jar);
                    }
                } else if (file_path.extension() == ".jar") {
                    extra_jvm_classpaths.push_back(file_path);
                }
            }

            // Intercept active tools mapping table and append the extra classpaths dynamically into libs/ structure
            fs::path local_libs_dir = fs::absolute("libs");
            fs::create_directories(local_libs_dir);
            
            for (const auto& jar_target : extra_jvm_classpaths) {
                fs::path local_link = local_libs_dir / jar_target.filename();
                if (!fs::exists(local_link)) {
                    try {
                        fs::create_symlink(jar_target, local_link);
                    } catch (...) {
                        fs::copy_file(jar_target, local_link, fs::copy_options::overwrite_existing);
                    }
                }
            }
            
            auto [java_out, dex_cache] = compile_source_logic(config, tools, active_plugins, android_jar, build_dir, 
                                                            diff.changed_files, diff.deleted_files, 
                                                            (diff.res_changed || force_all), run_func);

            if (is_release) {
                UI::stage("Minification", "Running R8 bytecode optimization");
                run_dex_r8(tools["r8"], android_jar, config, build_dir, run_func);
            } else {
                UI::stage("Dexing", "Running D8 incremental translation");
                std::vector<fs::path> unified_dex_targets;
                for (const auto& [lang, files] : diff.changed_files) {
                    auto plug_it = active_plugins.find("." + lang);
                    if (lang == "java" || lang == "kotlin" || (plug_it != active_plugins.end() && plug_it->second.output_type == "jvm")) {
                        for (const auto& f : files) unified_dex_targets.push_back(f);
                    }
                }

                // Compile changed files incrementally
                run_incremental_dex(tools["d8"], android_jar, src_dir, java_out, dex_cache, 
                                   unified_dex_targets, run_func);

                // Compile unresolved class dependencies inside extra classpaths directly into the D8 caching pool
                std::vector<fs::path> jars_to_dex;
                for (const auto& jar : extra_jvm_classpaths) {
                    fs::path target_cached_dex = dex_cache / jar.filename().replace_extension(".dex");
                    if (!fs::exists(target_cached_dex) || force_all) {
                        jars_to_dex.push_back(jar);
                    }
                }

                if (!jars_to_dex.empty()) {
                    std::vector<std::string> d8_library_args = {
                        "d8",
                        "--lib", fs::absolute(android_jar).string(),
                        "--output", dex_cache.string()
                    };
                    for (const auto& j : jars_to_dex) {
                        d8_library_args.push_back(j.string());
                    }
                    run_func(d8_library_args, "Failed compilation of external library classes into DEX cache slots.");
                }

                run_dex_d8(tools["d8"], android_jar, build_dir, dex_cache, run_func);
            }

            // Remove temporary class links to preserve clean state boundaries
            for (const auto& jar_target : extra_jvm_classpaths) {
                fs::remove(local_libs_dir / jar_target.filename());
            }
        });
    }

    // BARRIER 2: Ensure Native Compiler finishes securely
    try {
        native_worker.get();
    } catch (...) {
        if (!pipeline_err) {
            pipeline_err = std::current_exception();
            err_stage = "Native compilation failure";
        }
    }

    // BARRIER 3: Ensure JVM Compiler finishes securely
    if (jvm_worker.valid()) {
        try {
            jvm_worker.get();
        } catch (...) {
            if (!pipeline_err) {
                pipeline_err = std::current_exception();
                err_stage = "JVM pipeline failure";
            }
        }
    }

    // FINAL UNWIND: Handle exceptions securely
    if (pipeline_err) {
        try {
            std::rethrow_exception(pipeline_err);
        } catch (const std::exception& e) {
            throw std::runtime_error(err_stage + ": " + e.what());
        } catch (...) {
            throw std::runtime_error(err_stage + ": Unknown fatal synchronization error.");
        }
    }

    // ============================================================================
    // MULTI-APK PACKAGING ITERATION LOOP (Runs on Main Thread)
    // ============================================================================
    UI::info("All concurrent compilation tracks synchronization barriers cleared.");

    fs::path base_unsigned_apk = build_dir / "unsigned.apk";
    
    if (!resources_triggered && !fs::exists(base_unsigned_apk)) {
        UI::warn("Base container missing. Forcing resource link pass resolution...");
        link_manifest(tools["aapt2"], base_unsigned_apk, android_jar, active_manifest_path, build_dir, src_dir, run_func, !is_release);
    }

    if (!fs::exists(base_unsigned_apk)) {
        throw std::runtime_error("Fatal Build Error: Base unsigned APK container is missing. Halting packaging loop.");
    }

    // Keystores explicit subdirectory validation mapping layout routing
    std::pair<std::string, std::string> ks_info;
    if (is_release) {
        fs::path resolved_ks = MkapkEnv::resolve_path(config.keystore);
        if (fs::exists(fs::current_path() / "keystores" / resolved_ks.filename())) {
            ks_info = {(fs::current_path() / "keystores" / resolved_ks.filename()).string(), config.keystore_alias};
        } else {
            ks_info = {resolved_ks.string(), config.keystore_alias};
        }
    } else {
        ks_info = handle_debug_keystore();
    }

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
        UI::stage(UI::Msg::PACK_STAGE, "Variant target: " + task.filename_suffix);
        
        fs::path loop_unsigned = build_dir / ("unsigned" + task.filename_suffix + ".apk");
        
        fs::copy_file(base_unsigned_apk, loop_unsigned, fs::copy_options::overwrite_existing);

        inject_assets_and_dex(loop_unsigned, build_dir, config.assets_dir, task.target_abis, is_release);

        // --- BUG FIX: INTERCEPT ANDRESGUARD RESOURCE OBFUSCATION FOR PRODUCTION RELEASE BUILDS ---
        fs::path target_processed_apk = loop_unsigned;
        if (is_release) {
            fs::path resguard_jar = MkapkEnv::resolve_path("~/AndResGuard/AndResGuard-cli-1.2.15.jar");
            fs::path config_xml = fs::current_path() / "andresguard.xml";
            
            if (fs::exists(resguard_jar) && fs::exists(config_xml)) {
                target_processed_apk = obfuscate_resources(tools["resguard"], loop_unsigned, build_dir, run_func);
            }
        }

        fs::path aligned_apk = align_apk(tools["zipalign"], "4", target_processed_apk, build_dir, run_func);
        fs::path final_apk = bin_dir / (proj_name + task.filename_suffix + ".apk");
        
        UI::stage(UI::Msg::SIGN_STAGE, final_apk.filename().string());
        sign_apk(tools["apksigner"], final_apk, aligned_apk, ks_info.first, ks_info.second, run_func);
        
        if (fs::exists(loop_unsigned)) fs::remove(loop_unsigned);
        if (fs::exists(aligned_apk)) fs::remove(aligned_apk);

        dynamic_ret_path = final_apk.string();
    }

    save_state(build_dir, new_state, is_release);
    return ndk_all ? "Split architecture packaging structural distribution layout written within: " + bin_dir.string() : dynamic_ret_path;
}