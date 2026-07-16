#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <zip.h>

#include "mkapk_extractor.hpp"
#include "mkapk_ui.hpp[span_1](start_span)"

namespace fs = std::filesystem;

namespace MkapkExtractor {

/**
 * Extracts a specific file stream from a ZIP archive to a destination path.
 */
static bool extract_zip_entry(zip_t* archive, zip_uint64_t index, const fs::path& dest_path) {
    zip_file_t* file = zip_fopen_index(archive, index, 0);
    if (!file) return false;

    fs::create_directories(dest_path.parent_path());
    std::ofstream out(dest_path, std::ios::binary);
    if (!out.is_open()) {
        zip_fclose(file);
        return false;
    }

    char buffer[8192];
    zip_int64_t bytes_read;
    while ((bytes_read = zip_fread(file, buffer, sizeof(buffer))) > 0) {
        out.write(buffer, bytes_read);
    }

    zip_fclose(file);
    return true;
}

/**
 * Parses out the <library> (groupId.artifactId) and <version> from a cached file path.
 * Path format: .../mkapk/lib/<groupId>.<artifactId>/<version>/<artifactId>-<version>.aar
 */
static bool parse_metadata_from_path(const fs::path& aar_path, std::string& library_name, std::string& version) {
    try {
        auto parent = aar_path.parent_path();
        version = parent.filename().string();
        library_name = parent.parent_path().filename().string();
        return !library_name.empty() && !version.empty();
    } catch (...) {
        return false;
    }
}

bool extract_aar(const std::string& aar_path) {
    fs::path aar(aar_path);
    if (!fs::exists(aar)) {
        UI::error("AAR file does not exist", aar_path);[span_2](start_span)[span_2](end_span)
        return false;
    }

    std::string library_name;
    std::string version;
    if (!parse_metadata_from_path(aar, library_name, version)) {
        UI::error("Failed to parse library metadata from path", aar_path);[span_3](start_span)[span_3](end_span)
        return false;
    }

    // Target structural cache slot directory
    const char* prefix_env = std::getenv("PREFIX");
    fs::path prefix = prefix_env ? fs::path(prefix_env) : "/data/data/com.termux/files/usr";[span_4](start_span)[span_4](end_span)
    fs::path dest_dir = prefix / "var/lib/mkapk/lib" / library_name / version;

    // Fast return if already extracted
    if (fs::exists(dest_dir / "AndroidManifest.xml") && fs::exists(dest_dir / "classes.jar")) {
        return true; 
    }

    UI::stage("Extracting AAR", library_name + ":" + version);[span_5](start_span)[span_5](end_span)
    fs::create_directories(dest_dir);

    int err = 0;
    zip_t* archive = zip_open(aar.string().c_str(), 0, &err);
    if (!archive) {
        UI::error("Failed to open AAR archive: " + aar_path);[span_6](start_span)[span_6](end_span)
        return false;
    }

    zip_int64_t num_entries = zip_get_num_entries(archive, 0);
    bool extraction_failed = false;

    for (zip_int64_t i = 0; i < num_entries; ++i) {
        const char* name = zip_get_name(archive, i, 0);
        if (!name) continue;

        std::string entry_name(name);

        // Filter and map exact targets
        if (entry_name == "AndroidManifest.xml") {
            if (!extract_zip_entry(archive, i, dest_dir / "AndroidManifest.xml")) {
                extraction_failed = true;
            }
        } 
        else if (entry_name == "classes.jar") {
            if (!extract_zip_entry(archive, i, dest_dir / "classes.jar")) {
                extraction_failed = true;
            }
        } 
        else if (entry_name.rfind("res/", 0) == 0) {
            // Keep nested directory hierarchy for drawable, values, layout, etc.
            fs::path target_res_path = dest_dir / entry_name;
            if (entry_name.back() == '/') {
                fs::create_directories(target_res_path);
            } else {
                if (!extract_zip_entry(archive, i, target_res_path)) {
                    extraction_failed = true;
                }
            }
        }
    }

    zip_close(archive);

    if (extraction_failed) {
        UI::error("Partial failure occurred during AAR extraction mapping for " + library_name);[span_7](start_span)[span_7](end_span)
        fs::remove_all(dest_dir); // Prevent dirty cache state
        return false;
    }

    return true;
}

void extract_all(const std::vector<std::string>& resolved_paths) {
    for (const auto& path : resolved_paths) {
        if (path.rfind(".aar") != std::string::npos || path.size() >= 4 && path.substr(path.size() - 4) == ".aar") {
            extract_aar(path);
        }
    }
}

} // namespace MkapkExtractor