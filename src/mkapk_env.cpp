#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <map>
#include <algorithm>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace MkapkEnv {

    // Termux standard prefixes
    const std::string TERMUX_BIN = "/data/data/com.termux/files/usr/bin/";
    const std::string TERMUX_SHARE = "/data/data/com.termux/files/usr/share/";
    const std::string TERMUX_ETC = "/data/data/com.termux/files/usr/etc/";
    const std::string TERMUX_LIB = "/data/data/com.termux/files/usr/lib/";
    
    // Extensible Package Management Standard Location
    const std::string PLUGINS_CACHE_DIR = "/data/data/com.termux/files/usr/var/lib/mkapk/plugins/";
    
    // Baked-in Developer Public Key for verification.
    const std::string EMBEDDED_PUBLIC_KEY = R"(-----BEGIN PUBLIC KEY-----
    MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAmlAatG4czhWUB/IwDDxg
    ztdhcVLPLMJwBf6xqa26dpdDgxur6dP3cJ1FThBfzpoIfFAhleYyGSEPOUWTOSuc
    1PI+O9OYZks6vE9NpZYMeOGIb+0d0/boAjMocsw8+H/n4ds4g/1GUIfDkO2ER+v+
    ZWnGf2fRolmOsZj0+TB4vpcia/2rF2oJyuJXwVqXJkckxg9ZcIqihlY9l+FRlerz
    BPBXMj1gTOyDZrG7zHVn6RSY1njC6eo8ZeBguHEfpwANlXeEGlSGhDFeDK88rDgv
    4IG8C+4DX/b57RSNYHDmv5NMXp3KmTia6/1R5WyWMwWyp4JUuu5VFa0pLEDGWij/
    fQIDAQAB
    -----END PUBLIC KEY-----)";

    // Forward helper for standard execution tracking
    bool run_system_cmd(const std::vector<std::string>& args) {
        pid_t pid = fork();
        if (pid == 0) {
            std::vector<char*> c_args;
            for (const auto& arg : args) c_args.push_back(const_cast<char*>(arg.c_str()));
            c_args.push_back(nullptr);
            execvp(c_args[0], c_args.data());
            _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        return false;
    }
    
    std::string get_json_val(const std::string& key, const std::string& json_content) {
        try {
            json j = json::parse(json_content);

            std::function<std::string(const json&)> search = [&](const json& obj) -> std::string {
                if (obj.is_object()) {
                    if (obj.contains(key)) {
                        if (obj[key].is_string()) return obj[key].get<std::string>();
                        if (obj[key].is_number() || obj[key].is_boolean()) return obj[key].dump();
                        return ""; // Ignore nested objects/arrays for flat scalar requests
                    }
                    for (auto& el : obj.items()) {
                        std::string res = search(el.value());
                        if (!res.empty()) return res;
                    }
                }
                return "";
            };

            return search(j);
        } catch (const json::parse_error&) {
            return ""; // Fail gracefully on invalid JSON
        }
    }

    fs::path resolve_path(std::string path_str) {
        if (path_str.empty()) return "";
        if (path_str[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) {
                std::string remainder = (path_str.size() > 1 && path_str[1] == '/') ? path_str.substr(2) : path_str.substr(1);
                return fs::absolute(fs::path(home) / remainder);
            }
        }
        return fs::absolute(fs::path(path_str));
    }

    fs::path get_android_jar(const std::string& config_content) {
        std::string sdk_root_str = get_json_val("SDK_ROOT", config_content);
        std::string target_sdk = get_json_val("TARGET_SDK", config_content);

        if (!sdk_root_str.empty() && !target_sdk.empty()) {
            fs::path sdk_root = resolve_path(sdk_root_str);
            fs::path jar_path = sdk_root / "platforms" / ("android-" + target_sdk) / "android.jar";
            if (fs::exists(jar_path)) return fs::absolute(jar_path);
        }

        fs::path termux_jar = fs::path(TERMUX_LIB) / "android.jar";
        if (fs::exists(termux_jar)) return termux_jar;

        throw std::runtime_error("android.jar file dependencies not found. Please review the SDK_ROOT target parameter inside config.json.");
    }

    std::string read_config_file() {
        std::ifstream file("config.json");
        if (!file.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    std::string get_tool_path(const std::string& name, const std::string& config_content) {
        fs::path termux_tool = fs::path(TERMUX_BIN) / name;
        if (fs::exists(termux_tool)) return termux_tool.string();
        return name;
    }

    std::map<std::string, std::string> get_tools_map(const std::string& config_content) {
        std::map<std::string, std::string> tools;
        std::vector<std::string> names = {"aapt2", "zipalign", "apksigner", "d8", "r8", "resguard", "javac", "kotlinc"};
        for (const auto& name : names) {
            tools[name] = get_tool_path(name, config_content);
        }
        return tools;
    }

    std::string get_jni_classpath(const std::string& config_content) {
        std::string sdk_root_str = get_json_val("SDK_ROOT", config_content);
        if (sdk_root_str.empty()) {
             UI::warn("SDK_ROOT variable context not explicit in project layout configuration file.");
        }
        
        fs::path sdk_root = resolve_path(sdk_root_str);
        
        fs::path coord_jar = fs::path(TERMUX_SHARE) / "mkapk/mkapk-coordinator.jar";
        fs::path apksigner_jar = fs::path(TERMUX_SHARE) / "java/apksigner.jar";
        
        fs::path r8_jar = sdk_root / "cmdline-tools/latest/lib/r8.jar";
        fs::path d8_jar = sdk_root / "cmdline-tools/latest/lib/d8-classpath.jar";
        
        fs::path resguard_jar = resolve_path("~/AndResGuard/AndResGuard-cli-1.2.15.jar");

        std::vector<std::string> cp_entries;
        
        if (fs::exists(coord_jar)) cp_entries.push_back(coord_jar.string());
        else UI::error("Missing tool dependency footprint registry path", coord_jar.string());

        if (fs::exists(r8_jar)) cp_entries.push_back(r8_jar.string());
        else UI::error("Missing tool dependency footprint registry path", r8_jar.string());
        
        if (fs::exists(apksigner_jar)) cp_entries.push_back(apksigner_jar.string());
        else UI::error("Missing tool dependency footprint registry path", apksigner_jar.string());

        if (fs::exists(d8_jar)) cp_entries.push_back(d8_jar.string());

        if (fs::exists(resguard_jar)) cp_entries.push_back(resguard_jar.string());

        std::string full_cp = "";
        for (size_t i = 0; i < cp_entries.size(); ++i) {
            full_cp += cp_entries[i] + (i == cp_entries.size() - 1 ? "" : ":");
        }
        
        return full_cp;
    }
    
    std::vector<NativeTargetConfig> parse_json_native_targets(const std::string& config_content) {
        std::vector<NativeTargetConfig> targets;
        try {
            json j = json::parse(config_content);
            
            if (j.contains("NATIVE_TARGETS") && j["NATIVE_TARGETS"].is_array()) {
                for (const auto& item : j["NATIVE_TARGETS"]) {
                    NativeTargetConfig cfg;
                    
                    if (item.contains("NAME") && item["NAME"].is_string()) {
                        cfg.name = item["NAME"].get<std::string>();
                    }
                    
                    if (item.contains("SOURCES") && item["SOURCES"].is_array()) {
                        for (const auto& src : item["SOURCES"]) {
                            if (src.is_string()) cfg.sources.push_back(src.get<std::string>());
                        }
                    }
                    
                    if (item.contains("EXTRA_FLAGS") && item["EXTRA_FLAGS"].is_array()) {
                        for (const auto& flag : item["EXTRA_FLAGS"]) {
                            if (flag.is_string()) cfg.extra_flags.push_back(flag.get<std::string>());
                        }
                    }
                    
                    targets.push_back(cfg);
                }
            }
        } catch (const json::parse_error& e) {
            UI::error("JSON Structural compilation parsing check failed inside NATIVE_TARGETS definition blocks.", e.what());
        }
        return targets;
    }

    bool init_project() {
        fs::create_directories(PLUGINS_CACHE_DIR);

        const fs::path TEMPLATE_PATH = fs::path(TERMUX_ETC) / "setup/proj-templates/android";
        UI::stage("Initialization", "Seeding default template paths structure");

        if (!fs::exists(TEMPLATE_PATH)) {
            UI::error("Target directory mirroring path location not verified structural setup layout base", TEMPLATE_PATH.string());
            return false;
        }

        try {
            for (const auto& entry : fs::directory_iterator(TEMPLATE_PATH)) {
                const auto& src = entry.path();
                auto dest = fs::current_path() / src.filename();
                
                if (fs::is_directory(src)) {
                    fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                } else {
                    fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
                }
                UI::info("Created project template file mirror mapping layer: " + src.filename().string());
            }
            UI::success("Operational workspace parameters initialized cleanly.");
            return true;
        } catch (const fs::filesystem_error& e) {
            UI::error("Filesystem failure encountered during layout environment setup routine execution.", e.what());
            return false;
        }
    }
   
    bool install_plugin(const std::string& pl_package_path) {
        fs::path src_zip = resolve_path(pl_package_path);
        if (!fs::exists(src_zip)) {
            UI::error("Plugin bundle tracking entry location could not be verified", src_zip.string());
            return false;
        }

        fs::create_directories(PLUGINS_CACHE_DIR);
        fs::path staging_dir = fs::path(PLUGINS_CACHE_DIR) / ".staging";
        fs::remove_all(staging_dir);
        fs::create_directories(staging_dir);

        UI::info("Decompressing raw extension components data payloads...");
        if (!run_system_cmd({"unzip", "-q", src_zip.string(), "-d", staging_dir.string()})) {
            UI::error("Failed to unpack targeted extension installer bundle wrapper.");
            fs::remove_all(staging_dir);
            return false;
        }

        fs::path manifest_path = staging_dir / "plugin.json";
        fs::path signature_path = staging_dir / "signature.sig";

        if (!fs::exists(manifest_path)) {
            UI::error("Invalid structural layout signature mapping. Extension descriptor configuration manifest omitted.");
            fs::remove_all(staging_dir);
            return false;
        }

        bool verified_stamp = false;
        if (fs::exists(signature_path)) {
            fs::path pubkey_temp = staging_dir / "mkapk_pub.pem";
            std::ofstream kf(pubkey_temp);
            if (kf.is_open()) {
                kf << EMBEDDED_PUBLIC_KEY;
                kf.close();

                verified_stamp = run_system_cmd({"openssl", "dgst", "-sha256", "-verify", pubkey_temp.string(), 
                                                 "-signature", signature_path.string(), manifest_path.string()});
            }
        }

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
            fs::remove_all(staging_dir);
            return false;
        }
        mf.close();

        if (plug_name.empty() || comp_bin.empty()) {
            UI::error("Target data keys missing inside initialization schema framework structures.");
            fs::remove_all(staging_dir);
            return false;
        }

        if (verified_stamp) {
            UI::success("Cryptographic validation confirmed: [MKAPK AUTHORIZED SECURITY KEY PROFILE BOUNDARY SETUP]", "🛡️  ");
        } else {
            UI::warn("Extension verification signature structural block omitted or non-compliant: [UNSIGNED DEVELOPER ASSET]");
        }

        UI::info("Validating localization context dependencies mapping trees for driver compiler: " + comp_bin);
        if (!run_system_cmd({"command", "-v", comp_bin})) {
            if (!apt_pkg.empty()) {
                UI::info("Binary interface missing locally. Bridging tool discovery channels to Termux package indexes: " + apt_pkg);
                if (!run_system_cmd({"apt", "install", "-y", apt_pkg})) {
                    UI::warn("Automated apt synchronization engine tracking returned execution anomaly flags.\n"
                             "   Please implement standalone terminal tracking steps for binary path: '" + comp_bin + "' manual installations.");
                }
            } else {
                UI::warn("No distribution provider configuration parameters noted.\n"
                         "   Ensure system execution binary paths mapping properties for '" + comp_bin + "' framework files are configured manually.");
            }
        } else {
            UI::info("Driver dependency verification complete. Footprint signature localized correctly inside system shell.");
        }

        fs::path final_dest = fs::path(PLUGINS_CACHE_DIR) / plug_name;
        fs::remove_all(final_dest);
        fs::create_directories(final_dest);

        fs::copy_file(manifest_path, final_dest / "plugin.json", fs::copy_options::overwrite_existing);
        
        std::ofstream vs(final_dest / "verified.status");
        if (vs.is_open()) {
            vs << (verified_stamp ? "1" : "0");
            vs.close();
        }

        fs::remove_all(staging_dir);
        UI::success("Extension registry plugin matching profile identification handle '" + plug_name + "' successfully loaded to target storage directories.");
        return true;
    }

    bool uninstall_plugin(const std::string& plugin_name) {
        fs::path target_path = fs::path(PLUGINS_CACHE_DIR) / plugin_name;
        if (!fs::exists(target_path)) {
            UI::error("Extension structural context database verification target path location mapping omitted for key handle", plugin_name);
            return false;
        }

        try {
            fs::remove_all(target_path);
            UI::success("Plugin mapping metadata tracking nodes for identifier tag '" + plugin_name + "' cleanly deleted from operational layouts.");
            return true;
        } catch (const fs::filesystem_error& e) {
            UI::error("Filesystem failure tracking routine mapping deletion calls.", e.what());
            return false;
        }
    }

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

                        if (fs::exists(status_path)) {
                            std::ifstream sf(status_path);
                            char flag;
                            if (sf >> flag) {
                                plugin.is_verified = (flag == '1');
                            }
                            sf.close();
                        }

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