#ifndef MKAPK_TOOLS_HPP
#define MKAPK_TOOLS_HPP

#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <set>
#include <utility>
#include <functional>
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<Result<void>(const std::vector<std::string>&, const std::string&)>;

struct LanguagePlugin {
    std::string name;                  
    std::string compiler;              
    std::string source_extension;      
    std::string output_type;           
    std::string apt_package;           
    bool is_verified = false;          
};

// Structural and actionable build flags
struct BuildResults {
    // Stage Booleans
    bool mode_switched                = false;
    bool src_changed                  = false;
    bool res_changed                  = false;
    bool manifest_changed             = false;
    bool assets_changed               = false;
    bool proguard_changed             = false;
    bool andresguard_changed          = false;
    bool r_txt_changed                = false;

    // Action Directives
    bool needs_manifest_relink        = false; 
    bool needs_jvm_compile            = false;
    bool needs_repackage              = false; 
    bool needs_dex_rebuild            = false; 
    bool needs_resource_obfuscation   = false; 
    
    // File tracking
    std::map<std::string, std::vector<fs::path>> changed_files;
    std::map<std::string, std::vector<fs::path>> deleted_files;
    std::vector<fs::path> changed_resources;
    std::vector<fs::path> changed_assets;

    bool any_changes() const {
        return src_changed || res_changed || manifest_changed || 
               assets_changed || proguard_changed || andresguard_changed || 
               r_txt_changed || mode_switched || needs_manifest_relink || 
               needs_jvm_compile || needs_repackage || 
               needs_dex_rebuild || needs_resource_obfuscation;
    }
};

// Abstract change detection interface
class IChangeChecker {
public:
    virtual ~IChangeChecker() = default;
    virtual std::pair<BuildResults, std::map<std::string, std::string>> detect_changes(
        const fs::path& build_dir,
        const MkapkConfig& config,
        bool force_all,
        bool is_release
    ) = 0;
};

std::string get_file_hash(const fs::path& file_path);
std::pair<BuildResults, std::map<std::string, std::string>> check_changes(
    const fs::path& bin_dir, 
    const MkapkConfig& config, 
    bool force_all,
    bool is_release
);
void save_state(const fs::path& build_dir, const std::map<std::string, std::string>& next_state, bool is_release);

// JVM Stage
Result<void> compile_incremental_java(
    const std::string& version,
    const std::vector<std::string>& flags,
    const fs::path& android_jar,
    const fs::path& out_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func,
    const std::vector<fs::path>& extra_dependency_jars = {}
);

Result<void> compile_incremental_kotlin(
    const std::string& KOTLINC,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func,
    const std::string& compose_plugin = "",
    bool is_release = false,
    const std::vector<std::string>& classpath_extra = {}
);

Result<void> compile_kotlin(
    const std::string& KOTLINC,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const fs::path& src_dir,
    RunFunc run_func,
    const std::string& compose_plugin = ""
);

// Resources
Result<void> compile_resources(
    const std::string& AAPT2,
    const fs::path& res_dir,
    const fs::path& bin_dir,
    RunFunc run_func,
    const std::vector<fs::path>* changed_res_files = nullptr,
    const std::vector<fs::path>& extra_dependency_res_dirs = {}
);

Result<void> link_manifest(
    const std::string& AAPT2,
    const fs::path& unsigned_apk,
    const fs::path& android_jar,
    const fs::path& manifest,
    const fs::path& bin_dir,
    const fs::path& src_dir,
    RunFunc run_func,
    bool debug = false
);

fs::path obfuscate_resources(
    const std::string& RESGUARD_TOOL,
    const fs::path& in_apk,
    const fs::path& build_dir,
    RunFunc run_func
);

// Native Stage
bool compile_native(
    const std::string& NDK_BIN, 
    const fs::path& src_dir,
    const fs::path& bin_dir,
    const std::vector<std::string>& arch_list,
    const std::string& target_api,
    RunFunc run_func,
    const std::vector<fs::path>& changed_files,
    const MkapkConfig& config
);

// Dexing
Result<void> run_dex_r8(
    const std::string& R8_TOOL,
    const fs::path& android_jar,
    const MkapkConfig& config,
    const fs::path& bin_dir,
    RunFunc run_func,
    bool no_obs = false
);

Result<void> run_dex_d8(
    const std::string& D8_TOOL,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    const fs::path& dex_cache,
    RunFunc run_func
);

Result<void> run_incremental_dex(
    const std::string& D8,
    const fs::path& android_jar,
    const fs::path& src_path,
    const fs::path& java_out,
    const fs::path& dex_cache,
    const std::vector<fs::path>& files_to_dex,
    const std::vector<fs::path>& extra_jvm_classpaths,
    RunFunc run
);

// Packaging
Result<void> inject_assets_and_dex(
    const fs::path& unsigned_apk, 
    const fs::path& bin_dir, 
    const fs::path& assets_dir, 
    const std::vector<std::string>& allowed_abis,
    bool is_release 
);

fs::path align_apk(
    const std::string& ZIPALIGN, 
    const std::string& alignment, 
    const fs::path& in_apk, 
    const fs::path& bin_dir, 
    RunFunc run_func
);

Result<void> sign_apk(
    const std::string& APKSIGNER, 
    const fs::path& final_apk, 
    const fs::path& aligned_apk, 
    const std::string& keystore, 
    const std::string& alias, 
    RunFunc run_func
);

Result<std::pair<std::string, std::string>> handle_debug_keystore();

#endif // MKAPK_TOOLS_HPP
