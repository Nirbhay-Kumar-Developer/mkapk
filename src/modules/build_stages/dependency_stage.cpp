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
    fs::create_directories(dep_hash_file.parent_path());

    std::stringstream deps_ss;
    for (const auto& dep : config.dependencies) {
        deps_ss << dep << "\n";
    }
    
    // FIX: Hash the string configuration data natively
    std::hash<std::string> hasher;
    std::string current_deps_hash = std::to_string(hasher(deps_ss.str()));

    std::string cached_deps_hash = "";
    if (fs::exists(dep_hash_file)) {
        std::ifstream hf(dep_hash_file);
        hf >> cached_deps_hash;
    }

    bool deps_config_changed = (current_deps_hash != cached_deps_hash) || ctx.force_all;

    std::vector<std::string> cached_artifacts = MkapkResolver::resolve_dependencies(config.dependencies, config);
    bool missing_cached_files = false;
    for (const auto& art_path : cached_artifacts) {
        if (!fs::exists(art_path)) {
            missing_cached_files = true;
            break;
        }
    }

    if (deps_config_changed || cached_artifacts.empty() || missing_cached_files) {
        UI::stage("Resolver", "Resolving dependencies via Maven matrix...");
        ctx.all_resolved_artifacts = MkapkResolver::resolve_dependencies(config.dependencies, config);

        std::ofstream hf(dep_hash_file);
        hf << current_deps_hash;
    } else {
        UI::info("Dependencies configuration up-to-date. Using cached resolution graph.");
        ctx.all_resolved_artifacts = cached_artifacts;
    }

    std::sort(ctx.all_resolved_artifacts.begin(), ctx.all_resolved_artifacts.end());
    auto last = std::unique(ctx.all_resolved_artifacts.begin(), ctx.all_resolved_artifacts.end());
    ctx.all_resolved_artifacts.erase(last, ctx.all_resolved_artifacts.end());

    MkapkExtractor::extract_all(ctx.all_resolved_artifacts);

    fs::path merged_manifest_output = ctx.build_dir / "AndroidManifest.xml";

    bool manifest_src_changed = ctx.diff.manifest_changed;
    bool merged_manifest_missing = !fs::exists(merged_manifest_output);

    if (manifest_src_changed || deps_config_changed || merged_manifest_missing || ctx.force_all) {
        bool merge_success = MkapkManifestMerger::merge_manifests(
            ctx.manifest_path.string(), 
            merged_manifest_output.string(), 
            ctx.all_resolved_artifacts
        );

        if (merge_success) {
            ctx.active_manifest_path = merged_manifest_output;
        } else {
            UI::warn("Manifest integration anomaly caught. Falling back to primary configuration file layout.");
        }
    } else {
        ctx.active_manifest_path = merged_manifest_output;
    }

    return Result<void>::success();
}