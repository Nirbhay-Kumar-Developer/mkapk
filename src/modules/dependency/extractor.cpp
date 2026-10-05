#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <zip.h>

#include "mkapk_extractor.hpp"
#include "mkapk_ui.hpp"

namespace fs = std::filesystem;

namespace MkapkExtractor {

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

bool extract_aar(const std::string& aar_path) {
    fs::path aar(aar_path);
    if (!fs::exists(aar)) {
        UI::error("AAR file does not exist", aar_path);
        return false;
    }

    fs::path dest_dir = aar.parent_path();
    std::string lib_identifier = aar.stem().string();

    // Check if extracted components already exist on disk including res if present
    if (fs::exists(dest_dir / "AndroidManifest.xml") && 
        fs::exists(dest_dir / "classes.jar") &&
        (!fs::exists(dest_dir / "res") || fs::is_directory(dest_dir / "res"))) {
        // Only skip if the extraction marker exists
        if (fs::exists(dest_dir / ".extracted")) {
            return true;
        }
    }

    UI::stage("Extracting AAR", lib_identifier);

    int err = 0;
    zip_t* archive = zip_open(aar.string().c_str(), 0, &err);
    if (!archive) {
        UI::error("Failed to open AAR archive: " + aar_path);
        return false;
    }

    zip_int64_t num_entries = zip_get_num_entries(archive, 0);
    bool extraction_failed = false;

    for (zip_int64_t i = 0; i < num_entries; ++i) {
        const char* name = zip_get_name(archive, i, 0);
        if (!name) continue;

        std::string entry_name(name);

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
        UI::error("Partial failure occurred during AAR extraction mapping for " + lib_identifier);
        fs::remove(dest_dir / "AndroidManifest.xml");
        fs::remove(dest_dir / "classes.jar");
        fs::remove_all(dest_dir / "res");
        return false;
    }

    // Touch completion marker
    std::ofstream(dest_dir / ".extracted").close();
    return true;
}

void extract_all(const std::vector<std::string>& resolved_paths) {
    for (const auto& path : resolved_paths) {
        if (path.rfind(".aar") != std::string::npos || (path.size() >= 4 && path.substr(path.size() - 4) == ".aar")) {
            extract_aar(path);
        }
    }
}

} // namespace MkapkExtractor