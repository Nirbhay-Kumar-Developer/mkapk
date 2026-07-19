#define _POSIX_C_SOURCE 200112L
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <zip.h>
#include <sys/stat.h>
#include <map>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include <cctype>
#include <spawn.h>
#include <sys/wait.h>
#include <stdexcept>

namespace fs = std::filesystem;

extern char** environ;

/**
 * Trims leading and trailing whitespaces, newlines (\n), and carriage returns (\r)
 * from captured string tokens to protect file system path routing.
 */
std::string trim_token(const std::string& str) {
    if (str.empty()) return str;
    
    size_t first = 0;
    size_t last = str.size() - 1;
    
    // Find first non-whitespace/non-newline character
    while (first < str.size() && (std::isspace(str[first]) || str[first] == '\n' || str[first] == '\r')) {
        first++;
    }
    
    // Find last non-whitespace/non-newline character
    while (last > first && (std::isspace(str[last]) || str[last] == '\n' || str[last] == '\r')) {
        last--;
    }
    
    return str.substr(first, (last - first + 1));
}

/**
 * ============================================================================
 * SECTION 1: SYSTEM SHARED LIBRARY AUTO-PLACEMENT ENGINE
 * ============================================================================
 */

/**
 * Parses out individual library tokens from the SYSTEM_SHARED_LIBS array inside config.json text.
 */
std::vector<std::string> parse_configured_libraries(const std::string& config_content) {
    std::vector<std::string> libs;
    size_t array_start = config_content.find("\"SYSTEM_SHARED_LIBS\":");
    if (array_start == std::string::npos) return libs;

    size_t start_bracket = config_content.find("[", array_start);
    size_t end_bracket = config_content.find("]", start_bracket);
    if (start_bracket == std::string::npos || end_bracket == std::string::npos) return libs;

    std::string array_body = config_content.substr(start_bracket + 1, end_bracket - start_bracket - 1);
    
    size_t pos = 0;
    while ((pos = array_body.find("\"", pos)) != std::string::npos) {
        size_t next_quote = array_body.find("\"", pos + 1);
        if (next_quote == std::string::npos) break;
        
        std::string lib_name = array_body.substr(pos + 1, next_quote - pos - 1);
        if (!lib_name.empty()) {
            libs.push_back(lib_name);
        }
        pos = next_quote + 1;
    }
    return libs;
}

/**
 * Automatically fetches configured .so libraries from Termux's localized 
 * ndk-multilib distribution tree, enabling seamless cross-compilation for all ABIs.
 */
void auto_place_system_libraries(const MkapkConfig& config, const fs::path& bin_dir, const std::vector<std::string>& arch_list) {
    // Direct lookup from the pre-parsed configuration object array vector!
    std::vector<std::string> targeted_libs = config.system_shared_libs;

    UI::stage("NDK Syslibs", "Auto-resolving system shared dependencies from Termux ndk-multilib");

    fs::path termux_usr_dir = "/data/data/com.termux/files/usr";
    fs::path termux_global_lib = "/data/data/com.termux/files/usr/lib";
    
    std::map<std::string, std::string> arch_to_abi_map = {
        {"aarch64-linux-android", "arm64-v8a"},
        {"armv7a-linux-androideabi", "armeabi-v7a"},
        {"i686-linux-android", "x86"},
        {"x86_64-linux-android", "x86_64"}
    };

    std::map<std::string, std::string> arch_to_sysroot_folder = {
        {"aarch64-linux-android", "aarch64-linux-android"},
        {"armv7a-linux-androideabi", "arm-linux-androideabi"}, 
        {"i686-linux-android", "i686-linux-android"},
        {"x86_64-linux-android", "x86_64-linux-android"}
    };

    std::string host_abi;
#if defined(__aarch64__)
    host_abi = "arm64-v8a";
#elif defined(__arm__)
    host_abi = "armeabi-v7a";
#elif defined(__x86_64__)
    host_abi = "x86_64";
#else
    host_abi = "x86";
#endif

    for (const std::string& arch_raw : arch_list) {
        std::string arch = trim_token(arch_raw);
        if (arch.empty()) continue;

        std::string abi_name = arch_to_abi_map.count(arch) ? arch_to_abi_map[arch] : "unknown";
        std::string sysroot_folder = arch_to_sysroot_folder.count(arch) ? arch_to_sysroot_folder[arch] : "unknown";
        
        if (abi_name == "unknown" || sysroot_folder == "unknown") continue;

        fs::path target_abi_dir = bin_dir / "lib" / abi_name;
        fs::create_directories(target_abi_dir);

        bool is_host_match = (abi_name == host_abi);

        for (const std::string& lib_base_raw : targeted_libs) {
            std::string lib_base = trim_token(lib_base_raw);
            if (lib_base.empty()) continue;

            std::string filename = "lib" + lib_base + ".so";
            fs::path source_file;
            bool found = false;

            // STRATEGY 1: Dynamically scan Termux's multilib triple paths for ANY configured library
            fs::path multilib_target = termux_usr_dir / sysroot_folder / "lib" / filename;
            if (fs::exists(multilib_target)) {
                source_file = multilib_target;
                found = true;
            }

            // STRATEGY 2: Fallback check against native host paths if compiling for the active device ABI
            if (!found && is_host_match) {
                fs::path host_target = termux_global_lib / filename;
                if (fs::exists(host_target)) {
                    source_file = host_target;
                    found = true;
                }
            }

            // STRATEGY 3: Project-specific local workspace prebuilts override directory
            if (!found) {
                fs::path project_prebuilt = fs::current_path() / "prebuilts" / abi_name / filename;
                if (fs::exists(project_prebuilt)) {
                    source_file = project_prebuilt;
                    found = true;
                }
            }

            // Execution Phase
            if (found) {
                try {
                    fs::copy_file(source_file, target_abi_dir / filename, fs::copy_options::overwrite_existing);
                    UI::info("[+] Auto-placed [" + abi_name + "]: " + filename);
                } catch (const fs::filesystem_error& e) {
                    UI::error("Failed copying library dependency target context: " + filename + " to " + abi_name, e.what());
                }
            } else {
                throw std::runtime_error("Architecture Build Error: Cannot resolve dependency library runtime file link handle '" + filename + "' for targeted ABI [" + abi_name + "].");
            }
        }
    }
}

/**
 * ============================================================================
 * SECTION 2: APK ASSEMBLY, ALIGNMENT, AND SIGNING STAGES
 * ============================================================================
 */

// Injects classes.dex, assets, and ONLY the requested native libraries for this package task.
void inject_assets_and_dex(
    const fs::path& unsigned_apk, 
    const fs::path& bin_dir, 
    const fs::path& assets_dir, 
    const std::vector<std::string>& allowed_abis,
    bool is_release) 
{
    UI::stage("Packager", "Injecting DEX bytecode components, raw runtime assets, and cross-compiled libraries mapping tables securely");

    fs::path dex_file = bin_dir / "classes.dex";
    fs::path native_libs_path = bin_dir / "lib";
    fs::path resolved_assets = assets_dir.empty() ? "" : fs::absolute(assets_dir);

    int err = 0;
    zip_t* apk = zip_open(unsigned_apk.string().c_str(), 0, &err);
    if (!apk) throw std::runtime_error("Compression error encounter boundary broken: Failed to open target unsigned container archive for assembly injection steps.");

    std::map<std::string, fs::path> injection_map;
    if (fs::exists(dex_file)) injection_map["classes.dex"] = dex_file;

    std::map<std::string, std::string> triple_to_abi = {
        {"aarch64-linux-android", "arm64-v8a"},
        {"armv7a-linux-androideabi", "armeabi-v7a"},
        {"i686-linux-android", "x86"},
        {"x86_64-linux-android", "x86_64"}
    };

    std::vector<std::string> target_abis;
    for (const auto& entry : allowed_abis) {
        if (triple_to_abi.count(entry)) {
            target_abis.push_back(triple_to_abi[entry]);
        } else {
            target_abis.push_back(entry);
        }
    }

    if (fs::exists(native_libs_path)) {
        for (const auto& entry : fs::recursive_directory_iterator(native_libs_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".so") {
                std::string rel = fs::relative(entry.path(), bin_dir).string();
                
                bool abi_allowed = false;
                for (const auto& abi : target_abis) {
                    std::string segment = "lib/" + abi + "/";
                    if (rel.rfind(segment, 0) == 0) {
                        abi_allowed = true;
                        break;
                    }
                }

                if (abi_allowed) {
                    injection_map[rel] = entry.path();
                }
            }
        }
    }

    if (!resolved_assets.empty() && fs::exists(resolved_assets)) {
        for (const auto& entry : fs::recursive_directory_iterator(resolved_assets)) {
            if (entry.is_regular_file()) {
                std::string rel = "assets/" + fs::relative(entry.path(), resolved_assets).string();
                injection_map[rel] = entry.path();
            }
        }
    }

    // --- STRIP AND INJECT LAYER WITH CONDITION COMPRESSION CONTROLS ---
    for (auto const& [arc_path, real_path] : injection_map) {
        zip_int64_t idx = zip_name_locate(apk, arc_path.c_str(), 0);
        if (idx >= 0) {
            zip_delete(apk, idx);
        }

        bool is_native_lib = (arc_path.rfind("lib/", 0) == 0 && real_path.extension() == ".so");
        fs::path file_to_inject = real_path;

        // OPTIMIZATION 1: If it's a release build and a native library, strip all debug symbols safely!
        if (is_release && is_native_lib) {
            UI::info("[STRIP] Evicting unneeded metadata symbols safely via posix_spawn: " + arc_path);
            
            std::string abs_path_str = fs::absolute(real_path).string();
            
            // Explicit positional structural mapping configuration for strip arguments
            char* strip_args[] = {
                const_cast<char*>("strip"),
                const_cast<char*>("--strip-unneeded"),
                const_cast<char*>(abs_path_str.c_str()),
                nullptr
            };

            pid_t pid;
            int spawn_status = posix_spawn(&pid, "/data/data/com.termux/files/usr/bin/strip", nullptr, nullptr, strip_args, environ);
            
            // Fallback search to path variable if explicit Termux prefix constraint fails
            if (spawn_status != 0) {
                spawn_status = posix_spawnp(&pid, "strip", nullptr, nullptr, strip_args, environ);
            }

            if (spawn_status == 0) {
                int wait_status;
                if (waitpid(pid, &wait_status, 0) != -1) {
                    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
                        UI::warn("Code stripping binary process returned non-zero execution flag on: " + arc_path);
                    }
                } else {
                    UI::warn("Failed to wait for active stripping subprocess tracking context.");
                }
            } else {
                UI::warn("Process invocation barrier dropped error flags. Skipping stripping pass optimization rules.");
            }
        }

        zip_source_t* src = zip_source_file(apk, file_to_inject.string().c_str(), 0, 0);
        if (src) {
            zip_int64_t new_idx = zip_file_add(apk, arc_path.c_str(), src, ZIP_FL_OVERWRITE);
            
            if (new_idx >= 0 && is_native_lib) {
                if (is_release) {
                    zip_set_file_compression(apk, new_idx, ZIP_CM_DEFLATE, 9);
                    UI::info("[RELEASE-COMPRESS] Deflated allocation table layout: " + arc_path + " (Max Size Savings)");
                } else {
                    zip_set_file_compression(apk, new_idx, ZIP_CM_STORE, 0);
                    UI::info("[DEBUG-STORE] Stored uncompressed asset chunk: " + arc_path + " (Zero-Extraction Layout Ready)");
                }
            }
        }
    }

    zip_close(apk);
}

 // Optimizes the APK for RAM efficiency.

fs::path align_apk(const std::string& ZIPALIGN, const std::string& alignment, 
                         const fs::path& in_apk, const fs::path& bin_dir, RunFunc run_func) {
    UI::stage("ZipAlign", "Aligning archive layout borders for RAM access page performance optimizations");
    fs::path aligned_apk = bin_dir / "aligned_temp.apk";

    if (fs::exists(aligned_apk)) fs::remove(aligned_apk);

    run_func({
        ZIPALIGN, "-f", "-p", alignment,
        in_apk.string(), aligned_apk.string()
    }, "Zipalign verification and translation run dropped tracking error boundaries.");

    return aligned_apk;
}

// Signs the APK.

void sign_apk(const std::string& APKSIGNER, const fs::path& final_apk, 
                    const fs::path& aligned_apk, const std::string& keystore, 
                    const std::string& alias, RunFunc run_func) {
    
    UI::stage("ApkSigner", "Generating cryptographic profile signatures block into " + final_apk.filename().string());

    fs::path ks_path = fs::absolute(keystore);
    if (!fs::exists(ks_path)) {
        throw std::runtime_error("Security certificate footprint missing: Security profile store file not verified at destination context: " + ks_path.string());
    }

    // Base positional arguments matching target profile signatures
    std::vector<std::string> args = {
        APKSIGNER, "sign",
        "--ks", ks_path.string(),
        "--ks-key-alias", alias,
        "--out", final_apk.string()
    };

    // If handling the localized automated debug profile layout
    if (alias == "androiddebugkey") {
        args.push_back("--ks-pass");
        args.push_back("pass:android");
        args.push_back("--key-pass");
        args.push_back("pass:android");
    } 
                        
    args.push_back(aligned_apk.string());
    
    // Executes cleanly via native fork/exec (thanks to our smart_run patch),
    // inheriting the interactive console shell.
    run_func(args, "ApkSigner runtime verification tracking block dropped fatal signature errors.");

    if (fs::exists(aligned_apk)) fs::remove(aligned_apk);
}

std::pair<std::string, std::string> handle_debug_keystore() {
    const char* home_env = std::getenv("HOME");
    fs::path home;

    if (!home_env || std::string(home_env).empty()) {
        UI::warn("HOME environment variable is undefined or empty. Falling back to current working directory for debug keys.");
        home = fs::current_path();
    } else {
        home = fs::path(home_env);
    }
    
    fs::path debug_ks = home / ".android/debug.keystore";

    if (!fs::exists(debug_ks)) {
        UI::info("Development encryption profiles missing locally. Restoring default debug.keystore structural maps securely...");
        fs::create_directories(debug_ks.parent_path());
        
        std::string ks_str = debug_ks.string();

        // Isolate each argument cleanly to bypass any shell macro expansions
        char* keytool_args[] = {
            const_cast<char*>("keytool"),
            const_cast<char*>("-genkey"),
            const_cast<char*>("-v"),
            const_cast<char*>("-keystore"), const_cast<char*>(ks_str.c_str()),
            const_cast<char*>("-storepass"), const_cast<char*>("android"),
            const_cast<char*>("-alias"), const_cast<char*>("androiddebugkey"),
            const_cast<char*>("-keypass"), const_cast<char*>("android"),
            const_cast<char*>("-keyalg"), const_cast<char*>("RSA"),
            const_cast<char*>("-keysize"), const_cast<char*>("2048"),
            const_cast<char*>("-validity"), const_cast<char*>("10000"),
            const_cast<char*>("-dname"), const_cast<char*>("CN=Android Debug,O=Android,C=US"),
            nullptr
        };

        pid_t pid;
        // Attempt invocation directly targeting Termux's standard binary path layout first
        int spawn_status = posix_spawn(&pid, "/data/data/com.termux/files/usr/bin/keytool", nullptr, nullptr, keytool_args, environ);
        
        // Fall back to PATH-based resolution if the absolute Termux path structure fails
        if (spawn_status != 0) {
            spawn_status = posix_spawnp(&pid, "keytool", nullptr, nullptr, keytool_args, environ);
        }

        if (spawn_status == 0) {
            int wait_status;
            if (waitpid(pid, &wait_status, 0) != -1) {
                if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
                    UI::warn("Automated keytool profile certificate generation returned a non-zero execution status flag.");
                }
            } else {
                UI::warn("Failed to wait for the active keytool generation subprocess tracking thread.");
            }
        } else {
            throw std::runtime_error("Security certificate generation failure: posix_spawn failed to execute 'keytool'.");
        }
    }
    
    return {debug_ks.string(), "androiddebugkey"};
}