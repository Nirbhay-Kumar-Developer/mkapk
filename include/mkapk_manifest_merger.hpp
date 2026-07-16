#ifndef MKAPK_MANIFEST_MERGER_HPP
#define MKAPK_MANIFEST_MERGER_HPP

#include <string>
#include <vector>
#include "mkapk_config.hpp"

namespace MkapkManifestMerger {
    /**
     * Gathers all dependency manifests, runs the programmatic Java Merger,
     * and writes the validated result directly to the build/ directory.
     * 
     * @param main_manifest Path to the primary AndroidManifest.xml (e.g., /app/src/main/AndroidManifest.xml)
     * @param output_manifest Path to place the merged build output (e.g., /app/build/AndroidManifest.xml)
     * @param resolved_paths List of resolved absolute paths for jar/aar dependencies returned by the resolver.
     * @return true if merging succeeded and output manifest exists on disk, false otherwise.
     */
    bool merge_manifests(
        const std::string& main_manifest,
        const std::string& output_manifest,
        const std::vector<std::string>& resolved_paths
    );
}

#endif // MKAPK_MANIFEST_MERGER_HPP