#ifndef MKAPK_EXTRACTOR_HPP
#define MKAPK_EXTRACTOR_HPP

#include <string>
#include <vector>

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
}

#endif // MKAPK_EXTRACTOR_HPP