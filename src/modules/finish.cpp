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

namespace fs = std::filesystem;

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
void auto_place_system_libraries(const std::string& config_content, const fs::path& bin_dir, const std::vector<std::string>& arch_list) {
    std::vector<std::string> targeted_libs = parse_configured_libraries(config_content);
    
    if (std::find(targeted_libs.begin(), targeted_libs.end(), "c++_shared") == targeted_libs.end()) {
        targeted_libs.push_back("c++_shared");
    }

    UI::stage("NDK Syslibs", "Auto-resolving system shared dependencies from Termux ndk-multilib");

    fs::path termux_usr_dir = "/data/data/com.termux/files/usr";
    fs::path termux_global_lib = "/data/data/com.termux/files/usr/lib";
    
    // Canonical triplet-to-ABI name converter mapping dictionary
    std::map<std::string, std::string> arch_to_abi_map = {
        {"aarch64-linux-android", "arm64-v8a"},
        {"armv7a-linux-androideabi", "armeabi-v7a"},
        {"i686-linux-android", "x86"},
        {"x86_64-linux-android", "x86_64"}
    };

    // Maps compilation target triples back to Termux's explicit cross-compiler directory structures
    std::map<std::string, std::string> arch_to_sysroot_folder = {
        {"aarch64-linux-android", "aarch64-linux-android"},
        {"armv7a-linux-androideabi", "arm-linux-androideabi"}, // Note: Termux maps armv7a back to 'arm' triple base folder layout
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

            // STRATEGY 1: Resolve core dependencies directly via Termux's true ndk-multilib layout folders
            if (lib_base == "c++_shared") {
                // Generates paths matching: /usr/<target-triple-root>/lib/libc++_shared.so
                fs::path multilib_target = termux_usr_dir / sysroot_folder / "lib" / filename;
                if (fs::exists(multilib_target)) {
                    source_file = multilib_target;
                    found = true;
                }
            }

            // STRATEGY 2: Fallback check against native host path context if building for local phone architecture
            if (!found && is_host_match) {
                fs::path host_target = termux_global_lib / filename;
                if (fs::exists(host_target)) {
                    source_file = host_target;
                    found = true;
                }
            }

            // STRATEGY 3: Local workspace prebuilts override routing
            if (!found) {
                fs::path project_prebuilt = fs::current_path() / "prebuilts" / abi_name / filename;
                if (fs::exists(project_prebuilt)) {
                    source_file = project_prebuilt;
                    found = true;
                }
            }

            // File Placement Pass Execution
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
    bool is_release) // Added is_release control flag to govern the optimization pipeline
{
    UI::stage("Packager", "Injecting DEX bytecode components, raw runtime assets, and cross-compiled libraries mapping tables");

    fs::path dex_file = bin_dir / "classes.dex";
    fs::path native_libs_path = bin_dir / "lib";
    fs::path resolved_assets = assets_dir.empty() ? "" : fs::absolute(assets_dir);

    int err = 0;
    zip_t* apk = zip_open(unsigned_apk.string().c_str(), 0, &err);
    if (!apk) throw std::runtime_error("Compression error encounter boundary broken: Failed to open target unsigned container archive for assembly injection steps.");

    std::map<std::string, fs::path> injection_map;
    if (fs::exists(dex_file)) injection_map["classes.dex"] = dex_file;

    // Standard triplet-to-ABI converter maps
    std::map<std::string, std::string> triple_to_abi = {
        {"aarch64-linux-android", "arm64-v8a"},
        {"armv7a-linux-androideabi", "armeabi-v7a"},
        {"i686-linux-android", "x86"},
        {"x86_64-linux-android", "x86_64"}
    };

    // Convert requested allowed compilation triples into canonical Android ABI names
    std::vector<std::string> target_abis;
    for (const auto& entry : allowed_abis) {
        if (triple_to_abi.count(entry)) {
            target_abis.push_back(triple_to_abi[entry]);
        } else {
            target_abis.push_back(entry); // Fallback if already an ABI token
        }
    }

    // Handle Native Libs with strict ABI filtering constraints
    if (fs::exists(native_libs_path)) {
        for (const auto& entry : fs::recursive_directory_iterator(native_libs_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".so") {
                std::string rel = fs::relative(entry.path(), bin_dir).string(); // Output looks like: lib/arm64-v8a/libnative.so
                
                // Inspect which ABI path segment this file resides within
                bool abi_allowed = false;
                for (const auto& abi : target_abis) {
                    std::string segment = "lib/" + abi + "/";
                    if (rel.rfind(segment, 0) == 0) { // Verify path starts with our target ABI signature
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

    // Handle Assets
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

        // Determine file properties
        bool is_native_lib = (arc_path.rfind("lib/", 0) == 0 && real_path.extension() == ".so");
        fs::path file_to_inject = real_path;

        // OPTIMIZATION 1: If it's a release build and a native library, strip all debug symbols!
        if (is_release && is_native_lib) {
            UI::info("[STRIP] Evicting unneeded metadata symbols from runtime asset: " + arc_path);
            
            // Execute Termux's local host 'strip' binary on the file before archiving it
            std::string strip_cmd = "strip --strip-unneeded " + fs::absolute(real_path).string();
            int ret = std::system(strip_cmd.c_str());
            if (ret != 0) {
                UI::warn("Code stripping tool chain optimization layer execution pass dropped error signals on: " + arc_path);
            }
        }

        zip_source_t* src = zip_source_file(apk, file_to_inject.string().c_str(), 0, 0);
        if (src) {
            zip_int64_t new_idx = zip_file_add(apk, arc_path.c_str(), src, ZIP_FL_OVERWRITE);
            
            if (new_idx >= 0 && is_native_lib) {
                if (is_release) {
                    // OPTIMIZATION 2A: Use maximum DEFLATE compression for production release size minimization
                    zip_set_file_compression(apk, new_idx, ZIP_CM_DEFLATE, 9);
                    UI::info("[RELEASE-COMPRESS] Deflated allocation table layout: " + arc_path + " (Max Size Savings)");
                } else {
                    // OPTIMIZATION 2B: Keep completely uncompressed for debug zero-extraction memory maps
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

    std::vector<std::string> args = {
        APKSIGNER, "sign",
        "--ks", ks_path.string(),
        "--ks-key-alias", alias,
        "--out", final_apk.string()
    };

    if (alias == "androiddebugkey") {
        args.push_back("--ks-pass");
        args.push_back("pass:android");
        args.push_back("--key-pass");
        args.push_back("pass:android");
    }

    args.push_back(aligned_apk.string());
    run_func(args, "ApkSigner runtime verification tracking block dropped fatal signature errors.");

    if (fs::exists(aligned_apk)) fs::remove(aligned_apk);
}

std::pair<std::string, std::string> handle_debug_keystore() {
    const char* home_env = std::getenv("HOME");
    if (!home_env) throw std::runtime_error("System workspace parameter trace error: HOME environment context variable drops empty tracking properties.");
    
    fs::path home = home_env;
    fs::path debug_ks = home / ".android/debug.keystore";

    if (!fs::exists(debug_ks)) {
        UI::info("Development encryption profiles missing locally. Restoring default debug.keystore structural maps...");
        fs::create_directories(debug_ks.parent_path());
        
        std::string cmd = "keytool -genkey -v -keystore " + debug_ks.string() + 
                          " -storepass android -alias androiddebugkey -keypass android " +
                          "-keyalg RSA -keysize 2048 -validity 10000 " +
                          "-dname \"CN=Android Debug,O=Android,C=US\"";
        
        int ret = std::system(cmd.c_str());
        if (ret != 0) UI::warn("Stand-alone keytool profile certificate automated creation routines returned execution tracking warning anomalies.");
    }
    return {debug_ks.string(), "androiddebugkey"};
}