#ifndef MKAPK_TOOLS_HPP
#define MKAPK_TOOLS_HPP

#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <set>
#include <utility>
#include <functional>

namespace fs = std::filesystem;

/**
 * ============================================================================
 * SECTION 1: COMMON TYPES & CALLBACKS
 * ============================================================================
 */

using RunFunc = std::function<void(const std::vector<std::string>&, const std::string&)>;

struct LanguagePlugin {
    std::string name;                  
    std::string compiler;              
    std::string source_extension;      
    std::string output_type;           
    std::string apt_package;           
    bool is_verified = false;          
};

// Comprehensive result structure for dynamic plugin change detection logic
struct BuildResults {
    bool mode_switched = false;
    bool src_changed = false;
    bool res_changed = false;
    bool manifest_changed = false;
    
    // Extensible dynamic dictionary replacing static hardcoded vectors
    std::map<std::string, std::vector<fs::path>> changed_files;
    std::map<std::string, std::vector<fs::path>> deleted_files;
    std::vector<fs::path> changed_resources;

    // Helper to determine if any build action is required across core or plugins
    bool any_changes() const {
        if (src_changed || res_changed || manifest_changed || mode_switched) {
            return true;
        }
        for (const auto& [lang, files] : changed_files) {
            if (!files.empty()) return true;
        }
        return false;
    }
};

struct NativeTargetConfig {
    std::string name;
    std::vector<std::string> sources;
    std::vector<std::string> extra_flags;
};

/**
 * ============================================================================
 * SECTION 2: CHANGE DETECTION (Standalone)
 * ============================================================================
 */

std::string get_file_hash(const fs::path& file_path);
std::map<std::string, std::string> scan_directory(const fs::path& dir_path);

// Updated: Change detection engine can now evaluate against dynamic registered plugins
std::pair<BuildResults, std::map<std::string, std::string>> check_changes(
    const fs::path& bin_dir, 
    const std::string& config_content, 
    bool force_all
);

void save_state(const fs::path& bin_dir, const std::map<std::string, std::string>& state);

/**
 * ============================================================================
 * SECTION 3: JAVA & KOTLIN COMPILATION
 * ============================================================================
 */

// Java Module
void compile_incremental_java(
    const std::string& version,
    const std::vector<std::string>& flags,
    const fs::path& android_jar,
    const fs::path& out_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func
);

// Kotlin Module
bool compile_incremental_kotlin(
    const std::string& KOTLINC,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func,
    const std::string& compose_plugin = "",
    const std::vector<std::string>& classpath_extra = {}
);

bool compile_kotlin(
    const std::string& KOTLINC,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const fs::path& src_dir,
    RunFunc run_func,
    const std::string& compose_plugin = ""
);

/**
 * ============================================================================
 * SECTION 4: RESOURCE MANAGEMENT (AAPT2)
 * ============================================================================
 */

void compile_resources(
    const std::string& AAPT2,
    const fs::path& res_dir,
    const fs::path& bin_dir,
    RunFunc run_func,
    const std::vector<fs::path>* changed_res_files = nullptr
);

void link_manifest(
    const std::string& AAPT2,
    const fs::path& unsigned_apk,
    const fs::path& android_jar,
    const fs::path& manifest,
    const fs::path& bin_dir,
    const fs::path& src_dir,
    RunFunc run_func,
    bool debug = false
);

/**
 * ============================================================================
 * SECTION 5: NATIVE COMPILATION (NDK)
 * ============================================================================
 */

bool compile_native(
    const std::string& NDK_BIN, 
    const fs::path& src_dir,
    const fs::path& bin_dir,
    const std::vector<std::string>& arch_list,
    const std::string& target_api,
    RunFunc run_func,
    const std::vector<fs::path>& changed_files,
    const std::vector<NativeTargetConfig>& native_targets
);

/**
 * ============================================================================
 * SECTION 6: DEXING (D8 / R8)
 * ============================================================================
 */

void run_dex_r8(
    const std::string& R8_TOOL,
    const fs::path& android_jar,
    const std::string& config_content,
    const fs::path& bin_dir,
    RunFunc run_func,
    bool no_obs = false
);

void run_dex_d8(
    const std::string& D8_TOOL,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    const fs::path& dex_cache,
    RunFunc run_func
);

void run_incremental_dex(
    const std::string& D8,
    const fs::path& android_jar,
    const fs::path& src_path,
    const fs::path& java_out,
    const fs::path& dex_cache,
    const std::vector<fs::path>& files_to_dex,
    RunFunc run
);

/**
 * ============================================================================
 * SECTION 7: PACKAGING & SIGNING
 * ============================================================================
 */

void inject_assets_and_dex(
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

void sign_apk(
    const std::string& APKSIGNER, 
    const fs::path& final_apk, 
    const fs::path& aligned_apk, 
    const std::string& keystore, 
    const std::string& alias, 
    RunFunc run_func
);

void to_place_system_libraries(
    const std::string& config_content, 
    const fs::path& bin_dir, 
    const std::vector<std::string>& arch_list
);

std::pair<std::string, std::string> handle_debug_keystore();

#endif // MKAPK_TOOLS_HPP