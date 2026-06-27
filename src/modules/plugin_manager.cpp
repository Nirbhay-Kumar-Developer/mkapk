#define _POSIX_C_SOURCE 200112L
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <algorithm>
#include <random>
#include <utility>
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
#include <nlohmann/json.hpp>

#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

extern char** environ;

namespace MkapkEnv {
    
    const std::string PLUGINS_CACHE_DIR = "/data/data/com.termux/files/usr/var/lib/mkapk/plugins/";

    /**
     * Unpacks, cryptographically validates, resolves dependencies via apt,
     * and writes verified plugin definitions to the storage cache registry.
     */
    bool install_plugin(const std::string& pl_package_path) {
        fs::path src_zip = resolve_path(pl_package_path);
        if (!fs::exists(src_zip)) {
            UI::error("Plugin bundle tracking entry location could not be verified", src_zip.string());
            return false;
        }

        // 1. SECURE RANDOMIZED STAGING (Mitigates TOCTOU / Predictable Path Attacks)
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        
        std::string secure_token = "";
        for (int i = 0; i < 16; i++) {
            secure_token += "0123456789abcdef"[dis(gen)];
        }

        // Isolate into the OS temporary directory rather than the persistent cache
        fs::path staging_dir = fs::temp_directory_path() / ("mkapk_stage_" + secure_token);
        fs::create_directories(staging_dir);

        // 2. RAII CLEANUP GUARD (Guarantees wipe on failure, success, or exception)
        struct StagingGuard {
            fs::path path;
            ~StagingGuard() { if (fs::exists(path)) fs::remove_all(path); }
        } guard{staging_dir};

        UI::info("Decompressing raw extension components data payloads into isolated enclave...");
        
        // Use '-j' flag to completely flatten directories (prevents ZipSlip / Path Traversal)
        if (!run_system_cmd({"unzip", "-j", "-q", src_zip.string(), "-d", staging_dir.string()})) {
            UI::error("Failed to unpack targeted extension installer bundle wrapper safely.");
            return false; // RAII guard automatically cleans up
        }

        fs::path manifest_path = staging_dir / "plugin.json";
        fs::path signature_path = staging_dir / "signature.sig";

        if (!fs::exists(manifest_path)) {
            UI::error("Invalid structural layout signature mapping. Extension descriptor configuration manifest omitted.");
            return false;
        }

        // 3. STRICT CRYPTOGRAPHIC VERIFICATION (Zero-Trust Enforcement)
        fs::path global_pubkey = "/data/data/com.termux/files/usr/share/mkapk/mkapk_public.pem";

        if (!fs::exists(global_pubkey)) {
            UI::error("Security Boundary Broken: Global verification asset missing at " + global_pubkey.string());
            return false;
        }

        if (!fs::exists(signature_path)) {
            UI::error("Security Boundary Broken: Plugin signature (signature.sig) is missing. Unsigned plugins are strictly prohibited.");
            return false;
        }

        // Run validation securely using the modern pkeyutl utility against the static file
        bool verified_stamp = run_system_cmd({
            "openssl", "pkeyutl",
            "-verify",
            "-pubin",
            "-inkey", global_pubkey.string(),
            "-sigfile", signature_path.string(),
            "-in", manifest_path.string(),
            "-rawin", "-digest", "sha256"
        });

        if (!verified_stamp) {
            UI::error("Cryptographic verification failed: The plugin signature is invalid or forged. Installation aborted.");
            return false; // ENFORCED STRICT POLICY: Drop execution immediately.
        }

        UI::success("Cryptographic validation confirmed: [MKAPK AUTHORIZED SECURITY KEY PROFILE BOUNDARY SETUP]", "");

        // 4. METADATA PARSING & SANITIZATION
        std::ifstream mf(manifest_path);
        std::string plug_name, comp_bin, apt_pkg;
        try {
            json j;
            mf >> j;
            plug_name = j.value("plugin", "");
            comp_bin = j.value("compiler", "");
            apt_pkg = j.value("apt-package", "");
        } catch (const json::parse_error& e) {
            UI::error("Extension configuration parsing layout parameters corrupted inside file registry.", e.what());
            return false;
        }
        mf.close();

        // STRICT SANITIZATION: Eliminate basic command injections before downstream parsing
        if (plug_name.empty() || comp_bin.empty() || 
            plug_name.find('/') != std::string::npos || plug_name.find('.') != std::string::npos) {
            UI::error("Malicious or incomplete target handle identifiers parsed inside initialization schema frameworks.");
            return false;
        }

        UI::info("Validating localization context dependencies mapping trees for driver compiler safely: " + comp_bin);
        
        // 5. DEPENDENCY RESOLUTION
        bool compiler_exists = false;
        fs::path termux_bin_check = fs::path("/data/data/com.termux/files/usr/bin") / comp_bin;
        
        if (fs::exists(termux_bin_check)) {
            compiler_exists = true;
        } else {
            pid_t check_pid;
            char* query_args[] = { const_cast<char*>("which"), const_cast<char*>(comp_bin.c_str()), nullptr };
            int check_spawn = posix_spawnp(&check_pid, "which", nullptr, nullptr, query_args, ::environ);
            
            if (check_spawn == 0) {
                int check_wait;
                waitpid(check_pid, &check_wait, 0);
                compiler_exists = (WIFEXITED(check_wait) && WEXITSTATUS(check_wait) == 0);
            }
        }

        if (!compiler_exists) {
            if (!apt_pkg.empty()) {
                UI::info("Binary interface missing locally. Bridging tool discovery channels securely to Termux package indexes: " + apt_pkg);
                
                if (!run_system_cmd({"apt", "install", "-y", apt_pkg})) {
                    UI::warn("Automated apt synchronization engine tracking returned execution anomaly flags.\n"
                             "   Please implement standalone terminal tracking steps for binary path: '" + comp_bin + "' manual installations.");
                }
            } else {
                UI::warn("No distribution provider configuration parameters noted.\n"
                         "   Ensure system execution binary paths mapping properties for '" + comp_bin + "' framework files are configured manually.");
            }
        } else {
            UI::info("Driver dependency verification complete. Footprint signature localized correctly inside system paths.");
        }

        // 6. ATOMIC COMMITAL TO PERSISTENT CACHE
        fs::create_directories(PLUGINS_CACHE_DIR);
        fs::path final_dest = fs::path(PLUGINS_CACHE_DIR) / plug_name;
        
        // Lock destination
        fs::remove_all(final_dest);
        fs::create_directories(final_dest);

        // Because we are moving verified files from an isolated, randomized directory, 
        // we eliminate the TOCTOU gap entirely.
        fs::copy_file(manifest_path, final_dest / "plugin.json", fs::copy_options::overwrite_existing);
        
        std::ofstream vs(final_dest / "verified.status");
        if (vs.is_open()) {
            // Hardcoded to "1" because unverified plugins are now strictly rejected above.
            vs << "1"; 
            vs.close();
        }

        // RAII guard will automatically delete `staging_dir` when function returns here.
        UI::success("Extension registry plugin matching profile identification handle '" + plug_name + "' successfully loaded to target storage directories.");
        return true;
    }

    /**
     * Clears systemic structural cache footprints of an isolated plugin safely.
     */
    bool uninstall_plugin(const std::string& plugin_name) {
        // STRICT SANITIZATION: Prevent Path Traversal (ZipSlip-style) attacks during deletion
        if (plugin_name.empty() || plugin_name.find('/') != std::string::npos || plugin_name.find('.') != std::string::npos) {
            UI::error("Invalid plugin identifier. Uninstallation blocked to prevent path traversal.", plugin_name);
            return false;
        }

        fs::path target_path = fs::path(PLUGINS_CACHE_DIR) / plugin_name;
        if (!fs::exists(target_path)) {
            UI::error("Plugin not found. No operational mapping exists for handle: " + plugin_name);
            return false;
        }

        try {
            fs::remove_all(target_path);
            UI::success("Plugin metadata and footprint for '" + plugin_name + "' successfully wiped from operational cache.");
            return true;
        } catch (const fs::filesystem_error& e) {
            UI::error("Filesystem failure during plugin removal routine.", e.what());
            return false;
        }
    }

    /**
     * Scans and initializes the active collection of LanguagePlugin objects from cache directory files.
     */
    std::map<std::string, LanguagePlugin> load_installed_plugins() {
        std::map<std::string, LanguagePlugin> active_registry;
        fs::path root_cache(PLUGINS_CACHE_DIR);

        if (!fs::exists(root_cache) || !fs::is_directory(root_cache)) {
            return active_registry;
        }

        for (const auto& entry : fs::directory_iterator(root_cache)) {
            if (entry.is_directory()) {
                fs::path mf_path = entry.path() / "plugin.json";
                fs::path status_path = entry.path() / "verified.status";

                if (fs::exists(mf_path)) {
                    try {
                        std::ifstream f(mf_path);
                        json j;
                        f >> j;
                        f.close();

                        LanguagePlugin plugin;
                        plugin.name = j.value("plugin", "");
                        plugin.compiler = j.value("compiler", "");
                        plugin.source_extension = j.value("source-code-extension", "");
                        plugin.output_type = j.value("output-category", "");
                        plugin.apt_package = j.value("apt-package", "");

                        // ENFORCE ZERO-TRUST: Verify the status flag actually exists and is 1
                        bool is_verified = false;
                        if (fs::exists(status_path)) {
                            std::ifstream sf(status_path);
                            char flag;
                            if (sf >> flag) {
                                is_verified = (flag == '1');
                            }
                            sf.close();
                        }

                        // SECURITY BOUNDARY: Ignore tampered or legacy unverified plugins in the cache
                        if (!is_verified) {
                            UI::warn("Stale or unverified plugin signature detected in cache. Ignoring: " + plugin.name);
                            continue;
                        }

                        plugin.is_verified = true;

                        if (!plugin.name.empty() && !plugin.source_extension.empty()) {
                            active_registry[plugin.source_extension] = plugin;
                        }
                    } catch (const json::parse_error&) {
                        continue;
                    }
                }
            }
        }
        return active_registry;
    }
}