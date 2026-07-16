#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <sstream>

#include "mkapk_manifest_merger.hpp"
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"

namespace fs = std::filesystem;

namespace MkapkManifestMerger {

/**
 * Resolves the cached manifest file path inside $PREFIX for a given resolved AAR path.
 */
static std::string resolve_cached_manifest_path(const fs::path& aar_path) {
    // AAR Path structure: .../mkapk/lib/<groupId>.<artifactId>/<version>/<artifactId>-<version>.aar
    try {
        auto parent_dir = aar_path.parent_path();
        fs::path manifest_path = parent_dir / "AndroidManifest.xml";
        if (fs::exists(manifest_path)) {
            return fs::absolute(manifest_path).string();
        }
    } catch (...) {
        // Fall through to empty string
    }
    return "";
}

bool merge_manifests(
    const std::string& main_manifest,
    const std::string& output_manifest,
    const std::vector<std::string>& resolved_paths) 
{
    UI::stage("Manifest Merger", "Merging library manifests with the primary AndroidManifest.xml");[span_1](start_span)[span_1](end_span)

    // 1. Gather all target manifest file paths
    std::vector<std::string> target_manifests;
    
    // Add primary application manifest
    fs::path primary_path(main_manifest);
    if (!fs::exists(primary_path)) {
        UI::error("Primary AndroidManifest.xml not found", main_manifest);[span_2](start_span)[span_2](end_span)
        return false;
    }
    target_manifests.push_back(fs::absolute(primary_path).string());

    // Add target output manifest destination path
    fs::path output_path(output_manifest);
    fs::create_directories(output_path.parent_path());
    target_manifests.push_back(fs::absolute(output_path).string());

    // Collect extracted manifests from the dependency layout
    for (const auto& path : resolved_paths) {
        fs::path file_path(path);
        // Only target AAR dependencies (JAR files do not contain manifests)
        if (file_path.extension() == ".aar") {
            std::string cached_manifest = resolve_cached_manifest_path(file_path);
            if (!cached_manifest.empty()) {
                target_manifests.push_back(cached_manifest);
                UI::info("[+] Enqueued for merging: " + fs::path(cached_manifest).parent_path().parent_path().filename().string());[span_3](start_span)[span_3](end_span)
            }
        }
    }

    // 2. Construct the unified IPC command payload
    // Protocol: MANIFESTMERGER|mainManifest|outputManifest|libManifest1|libManifest2|...
    std::stringstream ss;
    ss << "manifestmerger"; // Matches the handleTask() command mapping key in our JVM Daemon
    for (const auto& item : target_manifests) {
        ss << "|" << item;
    }

    // Convert to a flat vector of strings to route to call_java_tool
    std::vector<std::string> daemon_args;
    std::string arg;
    while (std::getline(ss, arg, '|')) {
        daemon_args.push_back(arg);
    }

    // 3. Hand-off to JVM Daemon
    try {
        call_java_tool(daemon_args);[span_4](start_span)[span_4](end_span)
    } catch (const std::exception& e) {
        UI::error("Manifest merging execution failed during daemon processing", e.what());[span_5](start_span)[span_5](end_span)
        return false;
    }

    // 4. Final verification layer
    if (fs::exists(output_path) && fs::file_size(output_path) > 0) {
        UI::success("Manifest integration complete: " + output_path.filename().string());[span_6](start_span)[span_6](end_span)
        return true;
    } else {
        UI::error("Merged manifest output verification failed. File not found or empty at: " + output_manifest);[span_7](start_span)[span_7](end_span)
        return false;
    }
}

} // namespace MkapkManifestMerger