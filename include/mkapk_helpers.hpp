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
#include "mkapk_result.hpp"
#include "mkapk_config.hpp"
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace MkapkEnv {
    fs::path resolve_path(std::string path_str);
    std::string get_tool_path(const std::string& name, const MkapkConfig& config);
    fs::path get_android_jar(const MkapkConfig& config);
    std::map<std::string, std::string> get_tools_map(const MkapkConfig& config);
    std::string get_jni_classpath(const MkapkConfig& config);
    bool init_project();
    bool run_system_cmd(const std::vector<std::string>& args);
}

void cleanup_stale_assets(
    const std::map<std::string, std::vector<fs::path>>& deleted_files, 
    const fs::path& java_out, 
    const fs::path& dex_cache
);

Result<std::pair<fs::path, fs::path>> compile_source_logic(
    const MkapkConfig& config,
    std::map<std::string, std::string>& tools,
    const std::map<std::string, LanguagePlugin>& active_plugins,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    std::map<std::string, std::vector<fs::path>>& changed_files,
    std::map<std::string, std::vector<fs::path>>& deleted_files,
    bool do_res,
    RunFunc run,
    const std::vector<fs::path>& extra_jvm_classpaths = {}
);

Result<void> smart_run(const std::vector<std::string>& args, const std::string& err_msg);

void auto_place_system_libraries(
    const MkapkConfig& config, 
    const fs::path& bin_dir, 
    const std::vector<std::string>& arch_list
);

std::string perform_build(const std::vector<std::string>& raw_args, const MkapkConfig& config);

#endif // MKAPK_HELPERS_HPP
