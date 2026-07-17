#ifndef MKAPK_CONFIG_HPP
#define MKAPK_CONFIG_HPP

#include <string>
#include <vector>
#include <filesystem>

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
}

#endif