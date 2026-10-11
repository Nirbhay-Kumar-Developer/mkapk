#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <zip.h>
#include <fstream>

#include "mkapk_extractor.hpp"
#include "mkapk_ui.hpp"

namespace fs = std::filesystem;

namespace MkapkExtractor {

static bool extract_zip_entry(zip_t* archive, zip_uint64_t index, const fs::path& dest_path) {
    zip_file_t* file = zip_fopen_index(archive, index, 0);
    if (!file) return false;

    std::error_code ec;
    fs::create_directories(dest_path.parent_path(), ec);

    std::ofstream out(dest_path, std::ios::binary);
    if (!out.is_open()) {
        zip_fclose(file);
        return false;
    }

    char buffer[16384];
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
    fs::path marker_file = dest_dir / ".extracted";

    // Fast check: If already cleanly extracted and classes.jar exists, skip
    if (fs::exists(marker_file) && fs::exists(dest_dir / "classes.jar")) {
        return true;
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

        // Ignore directory entries themselves
        if (!entry_name.empty() && entry_name.back() == '/') {
            continue;
        }

        // 1. Android Manifest
        if (entry_name == "AndroidManifest.xml") {
            if (!extract_zip_entry(archive, i, dest_dir / "AndroidManifest.xml")) {
                extraction_failed = true;
            }
        } 
        // 2. Main bytecode archive
        else if (entry_name == "classes.jar") {
            if (!extract_zip_entry(archive, i, dest_dir / "classes.jar")) {
                extraction_failed = true;
            }
        } 
        // 3. ProGuard / R8 consumer keep rules
        else if (entry_name == "proguard.txt") {
            if (!extract_zip_entry(archive, i, dest_dir / "proguard.txt")) {
                extraction_failed = true;
            }
        }
        // 4. Resource symbol definitions
        else if (entry_name == "R.txt") {
            if (!extract_zip_entry(archive, i, dest_dir / "R.txt")) {
                extraction_failed = true;
            }
        }
        // 5. Android resources (res/*)
        else if (entry_name.rfind("res/", 0) == 0) {
            fs::path target_path = dest_dir / entry_name;
            if (!extract_zip_entry(archive, i, target_path)) {
                extraction_failed = true;
            }
        }
        // 6. Native prebuilt shared libraries (jni/*)
        else if (entry_name.rfind("jni/", 0) == 0) {
            fs::path target_path = dest_dir / entry_name;
            if (!extract_zip_entry(archive, i, target_path)) {
                extraction_failed = true;
            }
        }
        // 7. Embedded secondary JARs (libs/*)
        else if (entry_name.rfind("libs/", 0) == 0) {
            fs::path target_path = dest_dir / entry_name;
            if (!extract_zip_entry(archive, i, target_path)) {
                extraction_failed = true;
            }
        }
        // 8. Custom assets (assets/*)
        else if (entry_name.rfind("assets/", 0) == 0) {
            fs::path target_path = dest_dir / entry_name;
            if (!extract_zip_entry(archive, i, target_path)) {
                extraction_failed = true;
            }
        }
    }

    zip_close(archive);

    if (extraction_failed) {
        UI::error("Partial failure occurred during AAR extraction for " + lib_identifier);
        fs::remove(marker_file);
        return false;
    }

    // Touch completion marker
    std::ofstream marker(marker_file);
    marker << "1\n";
    marker.close();

    return true;
}

void extract_all(const std::vector<std::string>& resolved_paths) {
    for (const auto& path_str : resolved_paths) {
        fs::path p(path_str);
        if (p.extension() == ".aar") {
            extract_aar(p.string());
        }
    }
}
    
void save_project_dependencies(const fs::path& build_dir, const std::vector<std::string>& artifacts) {
    fs::path deps_file = build_dir / "dependencies.txt";
    std::ofstream out(deps_file);
    if (!out.is_open()) return;

    for (const auto& item : artifacts) {
        if (!item.empty()) {
            out << item << "\n";
        }
    }
}

DependencyArtifacts load_project_dependencies(const fs::path& build_dir) {
    DependencyArtifacts result;
    fs::path deps_file = build_dir / "dependencies.txt";
    if (!fs::exists(deps_file)) return result;

    std::ifstream in(deps_file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        fs::path p(line);
        if (!fs::exists(p)) continue;

        result.all_artifacts.push_back(p);

        if (p.extension() == ".jar") {
            result.jvm_classpath_jars.push_back(p);
        } else if (p.extension() == ".aar") {
            fs::path aar_dir = p.parent_path();

            // 1. Core bytecode jar
            fs::path classes_jar = aar_dir / "classes.jar";
            if (fs::exists(classes_jar)) {
                result.jvm_classpath_jars.push_back(classes_jar);
            }

            // 2. Secondary embedded jars (e.g. emoji2's libs/repackaged.jar)
            fs::path inner_libs = aar_dir / "libs";
            if (fs::exists(inner_libs) && fs::is_directory(inner_libs)) {
                for (const auto& entry : fs::directory_iterator(inner_libs)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                        result.jvm_classpath_jars.push_back(entry.path());
                    }
                }
            }

            // 3. Android resources
            fs::path res_dir = aar_dir / "res";
            if (fs::exists(res_dir) && !fs::is_empty(res_dir)) {
                result.res_directories.push_back(res_dir);
            }

            // 4. Android manifest
            fs::path manifest = aar_dir / "AndroidManifest.xml";
            if (fs::exists(manifest)) {
                result.manifests.push_back(manifest);
            }
        }
    }
    return result;
}

} // namespace MkapkExtractor
