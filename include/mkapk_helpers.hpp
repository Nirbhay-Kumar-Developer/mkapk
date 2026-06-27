#ifndef MKAPK_HELPERS_HPP
#define MKAPK_HELPERS_HPP

#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <functional>
#include <utility>
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

/**
 * ============================================================================
 * SECTION 1: COMMON TYPES & CALLBACKS
 * ============================================================================
 */

using RunFunc = std::function<void(const std::vector<std::string>&, const std::string&)>;

/**
 * ============================================================================
 * SECTION 2: MKAPK ENVIRONMENT & CONFIGURATION (MkapkEnv)
 * ============================================================================
 */
namespace MkapkEnv {
    fs::path resolve_path(std::string path_str);
    fs::path get_android_jar(MkapkConfig& config);
    std::string get_tool_path(const std::string& name, MkapkConfig& config);
    std::filesystem::path get_android_jar(const MkapkConfig& config);
    std::map<std::string, std::string> get_tools_map(const MkapkConfig& config);
    std::string get_jni_classpath(const MkapkConfig& config);
    bool init_project();

    // --- EXTENSIBLE PACKAGE MANAGEMENT PLUGIN FRAMEWORK SYSTEM ---
    
    /**
     * Unpacks, verifies cryptographic signature records, installs dependencies 
     * via Termux apt, and writes plugin definitions safely to persistent cache storage.
     */
    bool install_plugin(const std::string& pl_package_path);

    /**
     * Removes structural configuration footprints and clears the plugin from the storage cache registry.
     */
    bool uninstall_plugin(const std::string& plugin_name);

    /**
     * Scans and initializes the active collection of LanguagePlugin objects from cache directory files
     */
     bool run_system_cmd(const std::vector<std::string>& args);
    std::map<std::string, LanguagePlugin> load_installed_plugins();
}

/**
 * ============================================================================
 * SECTION 3: SOURCE COMPILATION & DEXING (MkapkCompiler)
 * ============================================================================
 */

void cleanup_stale_assets(
    const std::map<std::string, std::vector<fs::path>>& deleted_files, 
    const fs::path& java_out, 
    const fs::path& dex_cache
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

std::pair<fs::path, fs::path> compile_source_logic(
    const MkapkConfig& config,
    std::map<std::string, std::string>& tools,
    const std::map<std::string, LanguagePlugin>& active_plugins,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    std::map<std::string, std::vector<fs::path>>& changed_files,
    std::map<std::string, std::vector<fs::path>>& deleted_files,
    bool do_res,
    RunFunc run
);

/**
 * ============================================================================
 * SECTION 4: DAEMON SUBPROCESS LIFECYCLE & IPC
 * ============================================================================
 */

void start_daemon(const std::string& classpath);
void stop_daemon();
void call_java_tool(const std::vector<std::string>& args);
void smart_run(const std::vector<std::string>& args, const std::string& err_msg);

void auto_place_system_libraries(
    const MkapkConfig& config, 
    const fs::path& bin_dir, 
    const std::vector<std::string>& arch_list
);

std::string perform_build(const std::vector<std::string>& raw_args, const MkapkConfig& config);

/**
 * ============================================================================
 * SECTION 5: CHANGE CHECKER (Standalone)
 * ============================================================================
 */

std::pair<BuildResults, std::map<std::string, std::string>> check_changes(
    const fs::path& bin_dir, 
    const MkapkConfig& config, 
    bool force_all,
    bool is_release
);

void save_state(
    const fs::path& bin_dir, 
    const std::map<std::string, std::string>& next_state,
    bool is_release
);
#endif // MKAPK_HELPERS_HPP