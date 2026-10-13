#ifndef MKAPK_CONFIG_HPP
#define MKAPK_CONFIG_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

// Defined here to eliminate circular include dependencies cleanly
struct NativeTargetConfig {
    std::string name;
    std::vector<std::string> sources;
    std::vector<std::string> extra_flags;
};

struct MkapkConfig {
    // Project Metadata
    std::string project_name;
    
    // Core Directories & Paths
    std::string bin_dir;
    std::string src_dir;
    std::string res_dir;
    std::string assets_dir;
    std::string manifest;
    
    // SDK & Compilation
    std::string sdk_root;
    std::string target_sdk;
    std::string min_sdk;
    std::string java_version;
    std::string compose_plugin;
    std::string proguard_rules;
    
    // NDK
    std::string ndk_bin;
    std::vector<std::string> system_shared_libs;
    std::vector<NativeTargetConfig> native_targets;
    
    // Security
    std::string keystore;
    std::string keystore_alias;
    
    // Dependencies
    std::vector<std::string> dependencies;

    // Validation Flag
    bool is_valid = false;
};

namespace MkapkEnv {
    MkapkConfig load_config(const std::filesystem::path& config_path = std::filesystem::current_path() / "config.json");
    
    inline const std::map<std::string, std::string> ARCH_TO_ABI = {
        {"aarch64-linux-android", "arm64-v8a"},
        {"armv7a-linux-androideabi", "armeabi-v7a"},
        {"i686-linux-android", "x86"},
        {"x86_64-linux-android", "x86_64"}
    };
    
    inline std::string triplet_to_abi(const std::string& triplet) {
        auto it = ARCH_TO_ABI.find(triplet);
        return (it != ARCH_TO_ABI.end()) ? it->second : "unknown";
    }
    
    inline std::string get_host_arch() {
#if defined(__arm__)
        return "armv7a-linux-androideabi";
#elif defined(__x86_64__)
        return "x86_64-linux-android";
#elif defined(__i386__) || defined(__i686__)
        return "i686-linux-android";
#else
        return "aarch64-linux-android";
#endif
    }
    
    inline const std::map<std::string, std::string> ARCH_TO_SYSROOT = {
        {"aarch64-linux-android", "aarch64-linux-android"},
        {"armv7a-linux-androideabi", "arm-linux-androideabi"},
        {"i686-linux-android", "i686-linux-android"},
        {"x86_64-linux-android", "x86_64-linux-android"}
    };
    
    inline fs::path get_dependency_cache_dir() {
        const char* home = std::getenv("HOME");
        return home ? (fs::path(home) / ".mkapk/lib")
                    : fs::path("/data/data/com.termux/files/home/.mkapk/lib");
    }
}

#endif
