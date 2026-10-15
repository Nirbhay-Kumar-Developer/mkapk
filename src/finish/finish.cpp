#include <zip.h>
#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <cctype>
#include <stdexcept>
#include <set>
#include <cstdlib>
#include <utility>

#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"

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

// SECTION 1: SYSTEM SHARED LIBRARY AUTO-PLACEMENT

/**
 * Automatically fetches configured .so libraries from Termux's localized 
 * ndk-multilib distribution tree, enabling seamless cross-compilation for all ABIs.
 */

void auto_place_system_libraries(const MkapkConfig& config, const fs::path& bin_dir, const std::vector<std::string>& arch_list) {
    std::vector<std::string> targeted_libs = config.system_shared_libs;

    const char* prefix_env = std::getenv("PREFIX");
    fs::path termux_usr_dir = prefix_env ? fs::path(prefix_env) : fs::path("/data/data/com.termux/files/usr");
    fs::path termux_global_lib = termux_usr_dir / "lib";
    
    fs::path mkapk_cache_lib = MkapkEnv::get_dependency_cache_dir();

    std::string host_abi = MkapkEnv::triplet_to_abi(MkapkEnv::get_host_arch());

    for (const std::string& arch_raw : arch_list) {
        std::string arch = trim_token(arch_raw);
        if (arch.empty()) continue;
        
        std::string abi_name = MkapkEnv::triplet_to_abi(arch);
        std::string sysroot_folder = MkapkEnv::ARCH_TO_SYSROOT.count(arch) 
            ? MkapkEnv::ARCH_TO_SYSROOT.at(arch) 
            : "unknown";

        if (abi_name == "unknown" || sysroot_folder == "unknown") continue;
        
        fs::path target_abi_dir = bin_dir / "lib" / abi_name;
        fs::create_directories(target_abi_dir);

        bool is_host_match = (abi_name == host_abi);
        std::set<std::string> copied_libs;

        // PHASE 1: Auto-place native shared libraries (.so) bundled in AAR dependencies
         if (fs::exists(mkapk_cache_lib)) {
            for (const auto& entry : fs::recursive_directory_iterator(mkapk_cache_lib)) {
                if (entry.is_regular_file() && entry.path().extension() == ".so") {
                    // Check if file is inside a jni/<abi_name> folder of an extracted AAR
                    fs::path parent_abi = entry.path().parent_path().filename();
                    fs::path parent_jni = entry.path().parent_path().parent_path().filename();

                    if (parent_abi == abi_name && parent_jni == "jni") {
                        std::string so_name = entry.path().filename().string();
                        try {
                            fs::copy_file(entry.path(), target_abi_dir / so_name, fs::copy_options::overwrite_existing);
                            copied_libs.insert(so_name);
                            UI::info(UI::Msg::OP_LIB_AUTOPLACED + "[" + abi_name + "] " + so_name + " (from dependency)");
                        } catch (const fs::filesystem_error& e) {
                            UI::error("Failed to copy AAR native dependency: " + so_name + " to " + abi_name, e.what());
                        }
                    }
                }
            }
        }

        // PHASE 2: Resolve and place configured system_shared_libs
        for (const std::string& lib_base_raw : targeted_libs) {
            std::string lib_base = trim_token(lib_base_raw);
            if (lib_base.empty()) continue;

            std::string filename = (lib_base.rfind("lib", 0) == 0) ? (lib_base + ".so") : ("lib" + lib_base + ".so");

            // Skip if this library was already extracted from an AAR dependency
            if (copied_libs.count(filename) || fs::exists(target_abi_dir / filename)) {
                continue;
            }

            fs::path source_file;
            bool found = false;

            // STRATEGY 1: Dynamically scan Termux's multilib triple paths
            fs::path multilib_target = termux_usr_dir / sysroot_folder / "lib" / filename;
            if (fs::exists(multilib_target)) {
                source_file = multilib_target;
                found = true;
            }

            // STRATEGY 2: Fallback check against native host paths if compiling for active device ABI
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
                    // Resolve symlinks to physical binaries before copying
                    fs::path resolved_source = fs::is_symlink(source_file) ? fs::canonical(source_file) : source_file;
                    fs::copy_file(resolved_source, target_abi_dir / filename, fs::copy_options::overwrite_existing);
                    UI::info(UI::Msg::OP_LIB_AUTOPLACED + "[" + abi_name + "] " + filename);
                } catch (const fs::filesystem_error& e) {
                    UI::error("Failed to copy library dependency: " + filename + " to " + abi_name, e.what());
                }
            } else {
                throw std::runtime_error("Architecture Build Error: Cannot resolve dependency library runtime file link handle '" + filename + "' for targeted ABI [" + abi_name + "].");
            }
        }
    }
}

// SECTION 2: APK ASSEMBLY, ALIGNMENT, AND SIGNING STAGES

// Injects classes.dex, assets, and ONLY the requested native libraries for this package task.
Result<void> inject_assets_and_dex(
    const fs::path& unsigned_apk, 
    const fs::path& bin_dir, 
    const fs::path& assets_dir, 
    const std::vector<std::string>& allowed_abis,
    bool is_release) 
{
    fs::path dex_file = bin_dir / "classes.dex";
    fs::path native_libs_path = bin_dir / "lib";
    fs::path resolved_assets = assets_dir.empty() ? "" : fs::absolute(assets_dir);

    int err = 0;
    zip_t* apk = zip_open(unsigned_apk.string().c_str(), 0, &err);
    if (!apk) {
        return Result<void>::error("Compression error encounter boundary broken: Failed to open target unsigned container archive for assembly injection steps.");
    }

    std::map<std::string, fs::path> injection_map;
    if (fs::exists(bin_dir)) {
        for (const auto& entry : fs::directory_iterator(bin_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                std::string fname = entry.path().filename().string();
                if (fname.rfind("classes", 0) == 0) {
                    injection_map[fname] = entry.path();
                }
            }
        }
    }
    // Map architecture triplets (e.g. "aarch64-linux-android") or raw ABIs (e.g. "arm64-v8a")
    std::vector<std::string> target_abis;
    for (const auto& entry : allowed_abis) {
        auto it = MkapkEnv::ARCH_TO_ABI.find(entry);
        if (it != MkapkEnv::ARCH_TO_ABI.end()) {
            target_abis.push_back(it->second);
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

        // Optimization: Strip symbols from native libraries in release builds
        if (is_release && is_native_lib) {
            MkapkEnv::run_system_cmd({"strip", "--strip-unneeded", fs::absolute(real_path).string()});
        }

        zip_source_t* src = zip_source_file(apk, file_to_inject.string().c_str(), 0, 0);
        if (src) {
            zip_int64_t new_idx = zip_file_add(apk, arc_path.c_str(), src, ZIP_FL_OVERWRITE);
            
            // Keep .so files STORED (uncompressed) for direct mmap and 16KB/4KB page alignment
            if (new_idx >= 0 && is_native_lib) {
                zip_set_file_compression(apk, new_idx, ZIP_CM_STORE, 0);
            }
        }
    }

    zip_close(apk);
    return Result<void>::success();
}

// Optimizes the APK for RAM efficiency.
fs::path align_apk(const std::string& ZIPALIGN, const std::string& alignment, 
                   const fs::path& in_apk, const fs::path& bin_dir, RunFunc run_func) {
    UI::stage(UI::Msg::STAGE_ALIGN, UI::Msg::OP_ALIGNING);
    fs::path aligned_apk = bin_dir / "aligned_temp.apk";

    if (fs::exists(aligned_apk)) fs::remove(aligned_apk);

    // Pass -P 16 without -p (they are mutually exclusive)
    run_func({
        ZIPALIGN, "-f", "-P", "16", alignment,
        in_apk.string(), aligned_apk.string()
    }, "Zipalign verification and translation run dropped tracking error boundaries.");

    return aligned_apk;
}

// Signs the APK.
Result<void> sign_apk(const std::string& APKSIGNER, const fs::path& final_apk, 
                      const fs::path& aligned_apk, const std::string& keystore, 
                      const std::string& alias, RunFunc run_func) {
                          
    fs::path ks_path = fs::absolute(keystore);
    if (!fs::exists(ks_path)) {
        return Result<void>::error(UI::Msg::ERR_KEYSTORE_MISSING + ": " + ks_path.string());
    }

    // Base positional arguments matching target profile signatures
    std::vector<std::string> args = {
        APKSIGNER, "sign",
        "--ks", ks_path.string(),
        "--ks-key-alias", alias,
        "--out", final_apk.string()
    };

    // Automated debug profile credentials
    if (alias == "androiddebugkey") {
        args.push_back("--ks-pass");
        args.push_back("pass:android");
        args.push_back("--key-pass");
        args.push_back("pass:android");
    } 
                        
    args.push_back(aligned_apk.string());
    
    auto res = run_func(args, "ApkSigner runtime verification failed.");
    if (res.is_err()) return res;

    if (fs::exists(aligned_apk)) fs::remove(aligned_apk);
    return Result<void>::success();
}

Result<std::pair<std::string, std::string>> handle_debug_keystore() {
    const char* home_env = std::getenv("HOME");
    fs::path home;

    if (!home_env || std::string(home_env).empty()) {
        home = fs::current_path();
    } else {
        home = fs::path(home_env);
    }
    
    fs::path debug_ks = home / ".android/debug.keystore";

    if (!fs::exists(debug_ks)) {
        fs::create_directories(debug_ks.parent_path());
        
        std::string ks_str = debug_ks.string();
        
        MkapkEnv::run_system_cmd({
            "keytool", "-genkey", "-v",
            "-keystore", ks_str,
            "-storepass", "android",
            "-alias", "androiddebugkey",
            "-keypass", "android",
            "-keyalg", "RSA",
            "-keysize", "2048",
            "-validity", "10000",
            "-dname", "CN=Android Debug"
        });
    }
    return Result<std::pair<std::string, std::string>>::success({debug_ks.string(), "androiddebugkey"});
}
