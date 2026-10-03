#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cctype>

#include "mkapk_resolver.hpp"
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"

namespace MkapkResolver {

/**
 * Clean token splitter to tokenize JVM arrays bounded by pipe delimiters.
 */
static std::vector<std::string> split_tokens(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

/**
 * Helper to trim whitespace from IPC string paths.
 */
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::vector<std::string> resolve_dependencies(const std::vector<std::string>& coordinates, const MkapkConfig& config) {
    std::vector<std::string> resolved_paths;
    if (coordinates.empty()) return resolved_paths;

    // 1. Package unified protocol arguments to send down the global daemon pipe
    std::vector<std::string> daemon_args = {"resolve"};
    daemon_args.insert(daemon_args.end(), coordinates.begin(), coordinates.end());

    // 2. Delegate execution securely to the background thread pool proxy handler
    auto res = call_java_tool(daemon_args);
    if (res.is_err()) {
        UI::warn(std::string("Resolver notice: ") + res.get_error());
    }

    // 3. Parse explicit IPC output payload returned by Java Daemon (MKAPK_RESOLVED|/path/a.aar|/path/b.jar...)
    const auto& outputs = get_last_daemon_output();
    for (const auto& line : outputs) {
        if (line.rfind("MKAPK_RESOLVED|", 0) == 0) {
            std::stringstream ss(line.substr(15));
            std::string path_token;
            while (std::getline(ss, path_token, '|')) {
                std::string clean_path = trim(path_token);
                if (!clean_path.empty() && std::filesystem::exists(clean_path)) {
                    resolved_paths.push_back(clean_path);
                }
            }
        }
    }

    // 4. Post-Resolution Fallback: On-disk cache discovery if daemon output was missing
    if (resolved_paths.empty()) {
        const char* prefix_env = std::getenv("PREFIX");
        std::filesystem::path local_cache = prefix_env 
            ? std::filesystem::path(prefix_env) / "var/lib/mkapk/lib" 
            : "/data/data/com.termux/files/usr/var/lib/mkapk/lib";

        for (const auto& coordinate : coordinates) {
            std::vector<std::string> coords = split_tokens(coordinate, ':');
            if (coords.size() >= 3) {
                std::string group_id = coords[0];
                std::string artifact_id = coords[1];
                std::string version = (coords.size() == 3) ? coords[2] : coords.back();

                // FIX: Convert group_id dots to folder slashes (e.g., androidx.core -> androidx/core)
                std::string group_path = group_id;
                std::replace(group_path.begin(), group_path.end(), '.', '/');

                std::filesystem::path target_version_dir = local_cache / group_path / artifact_id / version;
                
                if (std::filesystem::exists(target_version_dir) && std::filesystem::is_directory(target_version_dir)) {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(target_version_dir)) {
                        if (entry.is_regular_file()) {
                            std::string ext = entry.path().extension().string();
                            if (ext == ".jar" || ext == ".aar") {
                                resolved_paths.push_back(entry.path().string());
                            }
                        }
                    }
                }
            }
        }
    }

    // 5. De-duplicate layout items safely to resolve graph collisions
    std::sort(resolved_paths.begin(), resolved_paths.end());
    resolved_paths.erase(std::unique(resolved_paths.begin(), resolved_paths.end()), resolved_paths.end());

    return resolved_paths;
}

} // namespace MkapkResolver