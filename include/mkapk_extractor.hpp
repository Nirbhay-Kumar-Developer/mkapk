#ifndef MKAPK_EXTRACTOR_HPP
#define MKAPK_EXTRACTOR_HPP

#include <string>
#include <vector>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace MkapkExtractor {
    /**
     * Extracts an AAR file to the localized $PREFIX cache folder structure.
     * Maps AndroidManifest.xml, classes.jar, and the res/ directory tree cleanly.
     * 
     * @param aar_path Absolute path to the cached .aar file.
     * @return true if extraction succeeded, false otherwise.
     */
    bool extract_aar(const std::string& aar_path);

    /**
     * Iterates over a list of resolved artifact paths, identifies .aar files,
     * and triggers safe extraction.
     * 
     * @param resolved_paths Vector containing absolute paths of resolved .aar/.jar files.
     */
    void extract_all(const std::vector<std::string>& resolved_paths);
    
struct DependencyArtifacts {
    std::vector<fs::path> all_artifacts;       // Raw .aar and .jar paths from dependencies.txt
    std::vector<fs::path> jvm_classpath_jars;  // classes.jar, libs/*.jar, and root .jar files
    std::vector<fs::path> res_directories;     // Extracted res/ directories
    std::vector<fs::path> manifests;           // Extracted AndroidManifest.xml files
};
    
    DependencyArtifacts load_project_dependencies(const fs::path& build_dir);
    void save_project_dependencies(const fs::path& build_dir, const std::vector<std::string>& artifacts);
}

#endif // MKAPK_EXTRACTOR_HPP
