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
    const std::vector<std::string>& resolved_paths,
    const std::string& min_sdk,
    const std::string& target_sdk) 
{
    UI::stage("Manifest Merger", "Merging library manifests with the primary AndroidManifest.xml");

    std::vector<std::string> target_manifests;
    
    fs::path primary_path(main_manifest);
    if (!fs::exists(primary_path)) {
        UI::error("Primary AndroidManifest.xml not found", main_manifest);
        return false;
    }
    target_manifests.push_back(fs::absolute(primary_path).string());

    fs::path final_output_path(output_manifest);
    fs::create_directories(final_output_path.parent_path());
    
    fs::path temp_output_path = fs::temp_directory_path() / "merged_AndroidManifest.xml";
    if (fs::exists(temp_output_path)) fs::remove(temp_output_path);

    target_manifests.push_back(fs::absolute(temp_output_path).string());

    for (const auto& path : resolved_paths) {
        fs::path file_path(path);
        if (file_path.extension() == ".aar") {
            std::string cached_manifest = resolve_cached_manifest_path(file_path);
            if (!cached_manifest.empty()) {
                target_manifests.push_back(cached_manifest);
                UI::info("[+] Enqueued for merging: " + fs::path(cached_manifest).parent_path().parent_path().filename().string());
            }
        }
    }

    // Build IPC argument vector passing SDK values as first tokens
    std::vector<std::string> daemon_args = {
        "manifestmerger",
        "--min-sdk", min_sdk,
        "--target-sdk", target_sdk
    };

    for (const auto& item : target_manifests) {
        daemon_args.push_back(item);
    }

    try {
        call_java_tool(daemon_args);
    } catch (const std::exception& e) {
        UI::error("Manifest merging execution failed during daemon processing", e.what());
        return false;
    }

    if (fs::exists(temp_output_path) && fs::file_size(temp_output_path) > 0) {
        std::error_code ec;
        fs::copy_file(temp_output_path, final_output_path, fs::copy_options::overwrite_existing, ec);
        fs::remove(temp_output_path, ec);
        
        UI::success("Manifest integration complete: " + final_output_path.filename().string());
        return true;
    } else {
        UI::error("Merged manifest output verification failed. File not found or empty at: " + output_manifest);
        return false;
    }
}
} // namespace MkapkManifestMerger
