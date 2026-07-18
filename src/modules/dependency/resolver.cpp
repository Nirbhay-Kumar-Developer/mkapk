#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <filesystem>

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

std::vector<std::string> resolve_dependencies(const std::string& coordinate, const MkapkConfig& config) {
    std::vector<std::string> resolved_paths;

    // 1. Package unified protocol arguments to send down the global daemon pipe
    std::vector<std::string> daemon_args = {"resolve", coordinate};

    // 2. Delegate execution securely to the background thread pool proxy handler
    try {
        // call_java_tool implicitly locks console streams, acts as a polling sync block, 
        // and throws on target exception tokens (e.g. MKAPK_TASK_FAILED)
        call_java_tool(daemon_args);
    } 
    catch (const std::exception& e) {
        UI::error("Aether Resolver Engine task validation failed.", e.what());
        return resolved_paths;
    }

    // 3. Post-Resolution Cache Verification Pass
    // Since the daemon has successfully downloaded the artifact and its transitives into the global pool,
    // we query the target package directory directly to discover the exact artifact paths.
    const char* prefix_env = std::getenv("PREFIX");
    std::filesystem::path local_cache = prefix_env 
        ? std::filesystem::path(prefix_env) / "var/lib/mkapk/lib" 
        : "/data/data/com.termux/files/usr/var/lib/mkapk/lib";

    std::vector<std::string> coords = split_tokens(coordinate, ':');
    if (coords.size() >= 4) {
        std::string group_id = coords[0];
        std::string artifact_id = coords[1];
        std::string extension = coords[2];
        std::string version = coords[3];

        // Map path pattern structure matching: var/lib/mkapk/lib/<groupId>.<artifactId>/<version>/
        std::filesystem::path target_version_dir = local_cache / (group_id + "." + artifact_id) / version;
        
        if (std::filesystem::exists(target_version_dir) && std::filesystem::is_directory(target_version_dir)) {
            // Collect the explicit targeted module item first
            std::filesystem::path primary_artifact = target_version_dir / (artifact_id + "-" + version + "." + extension);
            if (std::filesystem::exists(primary_artifact)) {
                resolved_paths.push_back(primary_artifact.string());
            }

            // Fallback scan: Grab any other underlying transient dependencies (.jar / .aar files) 
            // fetched into this specific operational path scope during the aether dependency download block
            for (const auto& entry : std::filesystem::directory_iterator(target_version_dir)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    if ((ext == ".jar" || ext == ".aar") && entry.path() != primary_artifact) {
                        resolved_paths.push_back(entry.path().string());
                    }
                }
            }
        }
    }

    return resolved_paths;
}

} // namespace MkapkResolver