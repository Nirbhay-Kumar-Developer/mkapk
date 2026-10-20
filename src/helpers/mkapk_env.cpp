#define _POSIX_C_SOURCE 200112L
#include <spawn.h>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <map>
#include <sys/wait.h>
#include <nlohmann/json.hpp>
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"

extern char** environ;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace MkapkEnv {
    // Termux standard prefixes
    const std::string TERMUX_BIN = "/data/data/com.termux/files/usr/bin/";
    const std::string TERMUX_SHARE = "/data/data/com.termux/files/usr/share/";
    const std::string TERMUX_ETC = "/data/data/com.termux/files/usr/etc/";
    const std::string TERMUX_LIB = "/data/data/com.termux/files/usr/lib/";

    // Forward helper for standard execution tracking
    bool run_system_cmd(const std::vector<std::string>& args) {
        if (args.empty()) {
            return false;
        }

        // Prepare the arguments array for the C API
        std::vector<char*> c_args;
        c_args.reserve(args.size() + 1);
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        pid_t pid;
        int spawn_status = posix_spawnp(&pid, c_args[0], nullptr, nullptr, c_args.data(), environ);

        if (spawn_status == 0) {
            int status;
            if (waitpid(pid, &status, 0) != -1) {
                return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
        }

        return false;
    }

    MkapkConfig load_config(const fs::path& config_path) {
        MkapkConfig config;

        // 1. Validate file exists in the targeted directory
        if (!fs::exists(config_path)) {
            return config;
        }

        // 2. Read the file
        std::ifstream file(config_path);
        if (!file.is_open()) {
            UI::error("Failed to open configuration file.", config_path.string());
            return config;
        }

        std::string config_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // 3. Parse JSON layout safely
        try {
            json j = json::parse(config_content);

            config.project_name   = j.value("NAME", "MyApp");
            config.bin_dir        = j.value("BIN_DIR", "bin");
            config.src_dir        = j.value("SRC_DIR", "src");
            config.res_dir        = j.value("RES_DIR", "res");
            config.assets_dir     = j.value("ASSETS_DIR", "assets");
            config.manifest       = j.value("MANIFEST", "AndroidManifest.xml");

            config.sdk_root       = j.value("SDK_ROOT", "/data/data/com.termux/files/home/android-sdk/Sdk");
            config.target_sdk     = j.value("TARGET_SDK", "36");
            config.min_sdk        = j.value("MIN_SDK","21");
            config.java_version   = j.value("JAVA_VERSION", "17");
            config.compose_plugin = j.value("COMPOSE_PLUGIN", "");
            config.proguard_rules = j.value("PROGUARD_RULES", "proguard-rules.pro");

            config.keystore       = j.value("KEYSTORE", "");
            config.keystore_alias = j.value("KEYSTORE_ALIAS", "");
            config.ndk_bin        = j.value("NDK_BIN", "");

            if (j.contains("SYSTEM_SHARED_LIBS") && j["SYSTEM_SHARED_LIBS"].is_array()) {
                for (const auto& lib : j["SYSTEM_SHARED_LIBS"]) {
                    if (lib.is_string()) config.system_shared_libs.push_back(lib.get<std::string>());
                }
            }

            if (j.contains("DEPENDENCIES") && j["DEPENDENCIES"].is_array()) {
                for (const auto& dep : j["DEPENDENCIES"]) {
                    if (dep.is_string()) config.dependencies.push_back(dep.get<std::string>());
                }
            }

            if (j.contains("NATIVE_TARGETS") && j["NATIVE_TARGETS"].is_array()) {
                for (const auto& item : j["NATIVE_TARGETS"]) {
                    NativeTargetConfig target_cfg;
                    target_cfg.name = item.value("NAME", "");

                    if (item.contains("SOURCES") && item["SOURCES"].is_array()) {
                        for (const auto& src : item["SOURCES"]) {
                            if (src.is_string()) target_cfg.sources.push_back(src.get<std::string>());
                        }
                    }
                    if (item.contains("EXTRA_FLAGS") && item["EXTRA_FLAGS"].is_array()) {
                        for (const auto& flag : item["EXTRA_FLAGS"]) {
                            if (flag.is_string()) target_cfg.extra_flags.push_back(flag.get<std::string>());
                        }
                    }
                    config.native_targets.push_back(target_cfg);
                }
            }

            config.is_valid = true;

        } catch (const json::parse_error& e) {
            UI::error("Parsing failure: Configuration JSON structure is malformed.", e.what());
            config.is_valid = false;
        }

        return config;
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

    fs::path get_android_jar(const MkapkConfig& config) {
        if (config.target_sdk.empty()) {
            UI::error("SDK Configuration Error",
            "'target_sdk' must be configured in 'config.json'.");
            std::exit(1);
        }
        
        fs::path sdk_root;
        if (config.sdk_root.empty()) {
            const char* home = std::getenv("HOME");
            sdk_root = fs::path(home) / "android-sdk" / "Sdk";
        } else {
            sdk_root = resolve_path(config.sdk_root);
        }
        
        fs::path target_platform_dir = sdk_root / "platforms" / ("android-" + config.target_sdk);
        fs::path jar_path = target_platform_dir / "android.jar";

        if (fs::exists(jar_path)) {
            return fs::absolute(jar_path);
        }

        UI::warn("android.jar not found for API " + config.target_sdk + ". Attempting automatic installation...");

        // Locate sdkmanager binary (check cmdline-tools locations, then fallback to PATH)
        std::string sdkmanager_bin = "sdkmanager";
        fs::path candidate_latest = sdk_root / "cmdline-tools" / "latest" / "bin" / "sdkmanager";

        if (fs::exists(candidate_latest)) {
            sdkmanager_bin = candidate_latest.string();
        } else {
        // Search generic cmdline-tools subdirectories if 'latest' is version-tagged (e.g., cmdline-tools/12.0/bin)
            fs::path cmdline_tools_dir = sdk_root / "cmdline-tools";
            if (fs::exists(cmdline_tools_dir)) {
                for (const auto& entry : fs::directory_iterator(cmdline_tools_dir)) {
                    fs::path possible_bin = entry.path() / "bin" / "sdkmanager";
                    if (fs::exists(possible_bin)) {
                        sdkmanager_bin = possible_bin.string();
                        break;
                    }
                }
            }
        }

        std::string platform_package = "platforms;android-" + config.target_sdk;
        UI::stage("SDK Manager", "Installing " + platform_package);

        std::vector<std::string> install_cmd = {
            sdkmanager_bin,
            platform_package
        };

        bool install_ok = run_system_cmd(install_cmd);

        if (install_ok && fs::exists(jar_path)) {
            UI::success("Successfully installed platform: android-" + config.target_sdk);
            return fs::absolute(jar_path);
        }

        UI::error("android.jar dependency could not be resolved.",
              "Failed to install '" + platform_package + "' via sdkmanager.\n"
              "Run manually: " + sdkmanager_bin + "\""+ platform_package + "\"");
        std::exit(1);
    }

    std::string get_tool_path(const std::string& name, const MkapkConfig& config) {
        if (name == "ndk-build" || name == "clang" || name == "strip") {
            if (!config.ndk_bin.empty()) {
                fs::path ndk_tool = resolve_path(config.ndk_bin) / name;
                if (fs::exists(ndk_tool)) return ndk_tool.string();
            }
        }

        fs::path termux_tool = fs::path(TERMUX_BIN) / name;
        if (fs::exists(termux_tool)) return termux_tool.string();
        return name;
    }

    std::map<std::string, std::string> get_tools_map(const MkapkConfig& config) {
        std::map<std::string, std::string> tools;
        std::vector<std::string> names = {"aapt2", "zipalign", "apksigner", "d8", "r8", "resguard", "javac", "kotlinc"};
        for (const auto& name : names) {
            tools[name] = get_tool_path(name, config);
        }
        return tools;
    }

    std::string get_jni_classpath(const MkapkConfig& config) {
        std::vector<std::string> cp_entries;
        std::set<std::string> seen_paths;

        auto add_cp = [&](const fs::path& p) {
            if (fs::exists(p)) {
                std::string abs_path = fs::absolute(p).string();
                if (seen_paths.insert(abs_path).second) {
                    cp_entries.push_back(abs_path);
                }
            }
        };

        const char* prefix_env = std::getenv("PREFIX");
        fs::path prefix_path = prefix_env ? fs::path(prefix_env) : fs::path("/data/data/com.termux/files/usr");
        fs::path termux_share = prefix_path / "share";

        // 1. MKAPK COORDINATOR & PRIVATE MAVEN DEPENDENCIES ($PREFIX/share/mkapk/)
        fs::path mkapk_dir = termux_share / "mkapk";
        if (fs::exists(mkapk_dir)) {
            for (auto it = fs::recursive_directory_iterator(mkapk_dir);
            it != fs::recursive_directory_iterator(); ++it) {
                
                // Skip descending into the tools directory
                if (it->is_directory() && it->path().filename() == "tools") {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file() && it->path().extension() == ".jar") {
                    add_cp(it->path());
                }
            }
        } else {
            UI::error("Missing mkapk share directory: " + mkapk_dir.string() + " Reinstall mkapk");
        }

        // 2. SYSTEM JAVA SHARE TOOLS ($PREFIX/share/java/ e.g., apksigner.jar)
        fs::path java_share = termux_share / "java";
        if (fs::exists(java_share)) {
            for (const auto& entry : fs::directory_iterator(java_share)) {
                if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                    add_cp(entry.path());
                }
            }
        }

        // 3. KOTLIN PRELOADER ONLY (Avoid loading kotlin-main-kts.jar which has legacy SLF4J 1.7 bindings)
        fs::path kotlin_preloader = prefix_path / "opt/kotlin/lib/kotlin-preloader.jar";
        if (fs::exists(kotlin_preloader)) {
            add_cp(kotlin_preloader);
        }

        // 5. ANDROID SDK CMDLINE-TOOLS (R8, D8, ManifestMerger & its nested runtime dependencies)
        if (std::string sdk_root_str = !config.sdk_root.empty() ? config.sdk_root : "~/android-sdk/Sdk"; !sdk_root_str.empty()) { 
            
            fs::path sdk_root = resolve_path(sdk_root_str);
            fs::path cmdline_lib = sdk_root / "cmdline-tools/latest/lib"; 
            
            if (fs::exists(cmdline_lib)) {
                for (const auto& entry : fs::recursive_directory_iterator(cmdline_lib)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                        std::string fname = entry.path().filename().string();
                        if (fname.find("kotlin") == std::string::npos) {
                            add_cp(entry.path());
                        }
                    }
                }
            } else {
                UI::warn("Android SDK cmdline-tools missing at: " + cmdline_lib.string());
            }
        } else {
            UI::warn("SDK_ROOT variable not set and default Android SDK is not installed.");
        }

        // 6. ASSEMBLE DELIMITED CLASSPATH STRING
        std::string full_cp = "";
        for (size_t i = 0; i < cp_entries.size(); ++i) {
            full_cp += cp_entries[i] + (i == cp_entries.size() - 1 ? "" : ":");
        }

        return full_cp;
    }

    bool init_project() {
        const fs::path TEMPLATE_PATH = fs::path(TERMUX_ETC) / "setup/proj-templates/android";
        UI::stage("Initialization", "Generating default project structure...");

        if (!fs::exists(TEMPLATE_PATH)) {
            UI::error("mkapk template directory not found: ", TEMPLATE_PATH.string());
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
            }
            UI::success("mkapk template project initialized successfully.");
            return true;
        } catch (const fs::filesystem_error& e) {
            UI::error("Filesystem error: ", e.what());
            return false;
        }
    }
}
