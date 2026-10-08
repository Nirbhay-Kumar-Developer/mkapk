#include "pipeline_stage.hpp"
#include "mkapk_resolver.hpp"
#include "mkapk_extractor.hpp"
#include "mkapk_manifest_merger.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_tools.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <functional>

namespace fs = std::filesystem;

Result<void> DependencyStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
    ctx.active_manifest_path = ctx.manifest_path;

    if (config.dependencies.empty()) {
        return Result<void>::success();
    }

    fs::path dep_hash_file = ctx.build_dir / ".hashes" / "deps.hash";
    fs::path deps_manifest_file = ctx.build_dir / "dependencies.txt";
    fs::create_directories(dep_hash_file.parent_path());

    // 1. Compute configuration hash of requested dependencies
    std::stringstream deps_ss;
    for (const auto& dep : config.dependencies) {
        deps_ss << dep << "\n";
    }
    
    std::hash<std::string> hasher;
    std::string current_deps_hash = std::to_string(hasher(deps_ss.str()));

    std::string cached_deps_hash = "";
    if (fs::exists(dep_hash_file)) {
        std::ifstream hf(dep_hash_file);
        hf >> cached_deps_hash;
    }

    bool deps_config_changed = (current_deps_hash != cached_deps_hash) || ctx.force_all;

    // 2. Read dependencies.txt if it already exists
    std::vector<std::string> cached_artifacts;
    bool missing_cached_files = false;

    if (fs::exists(deps_manifest_file)) {
        std::ifstream mf(deps_manifest_file);
        std::string line;
        while (std::getline(mf, line)) {
            if (line.empty()) continue;
            // Trim carriage return if present
            if (line.back() == '\r') line.pop_back();

            if (!fs::exists(line)) {
                missing_cached_files = true;
            }
            cached_artifacts.push_back(line);
        }
    } else {
        missing_cached_files = true;
    }

    // 3. Resolve dependencies if configuration changed or cached artifacts are missing on disk
    if (deps_config_changed || cached_artifacts.empty() || missing_cached_files) {
        UI::stage("Resolver", "Resolving dependencies via Maven matrix...");
        ctx.all_resolved_artifacts = MkapkResolver::resolve_dependencies(config.dependencies, config);

        if (ctx.all_resolved_artifacts.empty()) {
            return Result<void>::error("Resolver returned no artifacts for the specified dependencies.");
        }

        // De-duplicate paths
        std::sort(ctx.all_resolved_artifacts.begin(), ctx.all_resolved_artifacts.end());
        auto last = std::unique(ctx.all_resolved_artifacts.begin(), ctx.all_resolved_artifacts.end());
        ctx.all_resolved_artifacts.erase(last, ctx.all_resolved_artifacts.end());

        // Unpack archives (AARs)
        MkapkExtractor::extract_all(ctx.all_resolved_artifacts);

        // Save resolved list to the project build cache
        std::ofstream mf(deps_manifest_file);
        for (const auto& art_path : ctx.all_resolved_artifacts) {
            mf << art_path << "\n";
        }
        mf.close();

        // Update dependencies configuration hash
        std::ofstream hf(dep_hash_file);
        hf << current_deps_hash;
        hf.close();
    } else {
        UI::info("Dependencies configuration up-to-date. Using cached resolution graph.");
        ctx.all_resolved_artifacts = cached_artifacts;
        
        // Ensure all cached AARs have their classes.jar and res/ unpacked
        MkapkExtractor::extract_all(ctx.all_resolved_artifacts);
    }

    // 4. Merge AndroidManifest.xml
    fs::path merged_manifest_output = ctx.build_dir / "AndroidManifest.xml";

    bool manifest_src_changed = ctx.diff.manifest_changed;
    bool merged_manifest_missing = !fs::exists(merged_manifest_output);

    if (manifest_src_changed || deps_config_changed || merged_manifest_missing || ctx.force_all) {
        bool merge_success = MkapkManifestMerger::merge_manifests(
            ctx.manifest_path.string(), 
            merged_manifest_output.string(), 
            ctx.all_resolved_artifacts,
            config.min_sdk,
            config.target_sdk
        );

        if (merge_success) {
            ctx.active_manifest_path = merged_manifest_output;
        } else {
            return Result<void>::error("Manifest merger failed. Check library dependencies and AndroidManifest.xml formatting.");
        }
    } else {
        ctx.active_manifest_path = merged_manifest_output;
    }
    
    return Result<void>::success();
}
