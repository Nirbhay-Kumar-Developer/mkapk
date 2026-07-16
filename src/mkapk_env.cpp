#define _POSIX_C_SOURCE 200112L
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
#include "mkapk_config.hpp"
#include <spawn.h>
#include <chrono>
#include <random>

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
    c_args.reserve(args.size() + 1); // Minor optimization to prevent reallocation
    for (const auto& arg : args) {
        c_args.push_back(const_cast<char*>(arg.c_str()));
    }
    c_args.push_back(nullptr);

    pid_t pid;
    
    // We use posix_spawnp (with the 'p') to mirror execvp's PATH resolution behavior.
    // If you used posix_spawn, you would have to provide absolute paths.
    int spawn_status = posix_spawnp(&pid, c_args[0], nullptr, nullptr, c_args.data(), environ);

    // spawn_status is 0 on success
    if (spawn_status == 0) {
        int status;
        // Block and wait for the child process to finish
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
            return config; // is_valid remains false, triggering your CONFIG_MISSING error
        }

        // 2. Read the file
        std::ifstream file(config_path);
        if (!file.is_open()) {
            UI::error("Failed to open configuration file.", config_path.string());
            return config;
        }
        
        std::string config_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // 3. Parse without hardcoded fallback configurations
        try {
            json j = json::parse(config_content);

            // Empty strings match the exact behavior of original get_json_val()
            config.project_name   = j.value("NAME", "");
            config.bin_dir        = j.value("BIN_DIR", "");
            config.src_dir        = j.value("SRC_DIR", "");
            config.res_dir        = j.value("RES_DIR", "");
            config.assets_dir     = j.value("ASSETS_DIR", "");
            config.manifest       = j.value("MANIFEST", "");
            
            config.sdk_root       = j.value("SDK_ROOT", "");
            config.target_sdk     = j.value("TARGET_SDK", "");
            config.java_version   = j.value("JAVA_VERSION", "");
            config.compose_plugin = j.value("COMPOSE_PLUGIN", "");
            config.proguard_rules = j.value("PROGUARD_RULES", "");
            
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
            UI::error("Fatal parsing failure: Configuration JSON structure is malformed.", e.what());
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
        // Read directly out of struct configuration fields
        if (!config.sdk_root.empty() && !config.target_sdk.empty()) {
            fs::path sdk_root = resolve_path(config.sdk_root);
            fs::path jar_path = sdk_root / "platforms" / ("android-" + config.target_sdk) / "android.jar";
            if (fs::exists(jar_path)) return fs::absolute(jar_path);
        }

        // Log cleanly via UI wrapper and terminate build immediately
        UI::error("Architecture Build Error: android.jar dependencies not found.", 
                  "Verify that 'SDK_ROOT' and 'TARGET_SDK' are configured correctly inside your config.json.");
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
        if (config.sdk_root.empty()) {
             UI::warn("SDK_ROOT variable context not explicit in project layout configuration file.");
        }
        
        fs::path sdk_root = resolve_path(config.sdk_root);
        
        fs::path coord_jar = fs::path(TERMUX_SHARE) / "mkapk/mkapk-coordinator.jar";
        fs::path apksigner_jar = fs::path(TERMUX_SHARE) / "java/apksigner.jar";
        
        fs::path r8_jar = sdk_root / "cmdline-tools/latest/lib/r8.jar";
        fs::path d8_jar = sdk_root / "cmdline-tools/latest/lib/d8-classpath.jar";
        
        fs::path resguard_jar = resolve_path("~/AndResGuard/AndResGuard-cli-1.2.15.jar");
        
        fs::path kotlin_preloader = "/data/data/com.termux/files/usr/opt/kotlin/lib/kotlin-preloader.jar";

        std::vector<std::string> cp_entries;
        
        if (fs::exists(coord_jar)) cp_entries.push_back(coord_jar.string());
        else UI::error("Missing tool dependency footprint registry path", coord_jar.string());

        if (fs::exists(r8_jar)) cp_entries.push_back(r8_jar.string());
        else UI::error("Missing tool dependency footprint registry path", r8_jar.string());
        
        if (fs::exists(apksigner_jar)) cp_entries.push_back(apksigner_jar.string());
        else UI::error("Missing tool dependency footprint registry path", apksigner_jar.string());

        if (fs::exists(d8_jar)) cp_entries.push_back(d8_jar.string());

        if (fs::exists(resguard_jar)) cp_entries.push_back(resguard_jar.string());
        
        if (fs::exists(kotlin_preloader)) cp_entries.push_back(kotlin_preloader.string());
        else UI::error("Kotlin Compiler installation not found at standard path");

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
}