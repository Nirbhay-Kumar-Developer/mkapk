#include <vector>
#include <string>
#include <filesystem>
#include <system_error>
#include <set>
#include <unistd.h>

#include "mkapk_manifest_merger.hpp"
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_ipc.hpp"

namespace fs = std::filesystem;

namespace MkapkManifestMerger {

static std::string resolve_cached_manifest_path(const fs::path& aar_path) {
    try {
        fs::path parent_dir = aar_path.parent_path();
        fs::path manifest_path = parent_dir / "extracted_aar" / "AndroidManifest.xml";
        if (fs::exists(manifest_path)) {
            return fs::absolute(manifest_path).string();
        }

        // Fallback if extracted directly into parent folder
        fs::path direct_manifest = parent_dir / "AndroidManifest.xml";
        if (fs::exists(direct_manifest)) {
            return fs::absolute(direct_manifest).string();
        }
    } catch (...) {
        // Fall through on any filesystem exceptions
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

    fs::path primary_path = MkapkEnv::resolve_path(main_manifest);
    if (!fs::exists(primary_path)) {
        UI::error("Primary AndroidManifest.xml not found", primary_path.string());
        return false;
    }

    fs::path final_output_path = fs::absolute(output_manifest);
    fs::create_directories(final_output_path.parent_path());

    // Secure, isolated temporary output in the build folder to allow atomic rename
    fs::path temp_output_path = final_output_path.parent_path() /
        ("AndroidManifest.xml.tmp." + std::to_string(getpid()));

    std::error_code ec;
    if (fs::exists(temp_output_path, ec)) {
        fs::remove(temp_output_path, ec);
    }

    // RAII guard to guarantee cleanup of temporary file on error or early return
    struct TempGuard {
        fs::path path;
        bool released = false;
        ~TempGuard() {
            if (!released) {
                std::error_code err;
                fs::remove(path, err);
            }
        }
    } guard{temp_output_path};

    std::vector<std::string> target_manifests;
    target_manifests.push_back(fs::absolute(primary_path).string());
    target_manifests.push_back(fs::absolute(temp_output_path).string());

    // Deduplicate and collect AAR library manifests
    std::set<std::string> seen_manifests;
    for (const auto& path : resolved_paths) {
        fs::path file_path = MkapkEnv::resolve_path(path);
        if (file_path.extension() == ".aar") {
            std::string cached_manifest = resolve_cached_manifest_path(file_path);
            if (!cached_manifest.empty() && seen_manifests.insert(cached_manifest).second) {
                target_manifests.push_back(cached_manifest);
                UI::info("[+] Enqueued for merging: " + file_path.filename().string());
            }
        }
    }

    std::string safe_min_sdk = min_sdk.empty() ? "21" : min_sdk;
    std::string safe_target_sdk = target_sdk.empty() ? "33" : target_sdk;

    // Build IPC argument vector
    std::vector<std::string> daemon_args = {
        "manifestmerger",
        "--min-sdk", safe_min_sdk,
        "--target-sdk", safe_target_sdk
    };

    // Deduplicate and forward all resolved .jar dependencies so indexActiveClasses() indexes them
    std::set<std::string> seen_jars;
    for (const auto& path : resolved_paths) {
        fs::path file_path = MkapkEnv::resolve_path(path);
        if (file_path.extension() == ".jar" && fs::exists(file_path)) {
            std::string abs_jar = fs::absolute(file_path).string();
            if (seen_jars.insert(abs_jar).second) {
                daemon_args.push_back("--jar");
                daemon_args.push_back(abs_jar);
            }
        }
    }

    for (const auto& item : target_manifests) {
        daemon_args.push_back(item);
    }

    auto ipc_res = IpcClient::call_java_tool(daemon_args);
    if (ipc_res.is_err()) {
        UI::error("Manifest merging failed", ipc_res.get_error());
        return false;
    }

    if (!fs::exists(temp_output_path, ec) || fs::file_size(temp_output_path, ec) == 0) {
        UI::error("Merged manifest was not created or is empty at: " + temp_output_path.string());
        return false;
    }

    // Atomic rename into final output path (falls back to copy if cross-filesystem)
    fs::rename(temp_output_path, final_output_path, ec);
    if (ec) {
        fs::copy_file(temp_output_path, final_output_path, fs::copy_options::overwrite_existing, ec);
        fs::remove(temp_output_path, ec);
        if (ec) {
            UI::error("Failed to commit merged manifest to: " + final_output_path.string(), ec.message());
            return false;
        }
    }

    guard.released = true;
    UI::info("Manifest merging completed.");
    return true;
}

} // namespace MkapkManifestMerger
