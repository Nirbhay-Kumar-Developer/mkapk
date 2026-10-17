#include <zip.h>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <future>

#include "mkapk_extractor.hpp"
#include "mkapk_tools.hpp"
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

    fs::path dep_dir = aar.parent_path();
    fs::path dest_dir = dep_dir / "extracted_aar";
    fs::path hash_file = dep_dir / "extracted_aar.hash";
    std::string lib_identifier = aar.stem().string();

    // 1. Cache Validation: check hash and AndroidManifest.xml (classes.jar is optional in AARs)
    if (fs::exists(dest_dir) && fs::exists(hash_file)) {
        std::ifstream hf(hash_file);
        std::string expected_hash;
        hf >> expected_hash;
        hf.close();

        std::string actual_hash = get_dir_hash(dest_dir);
        if (!expected_hash.empty() && expected_hash == actual_hash && fs::exists(dest_dir / "AndroidManifest.xml")) {
            return true;
        }
    }

    // 2. Re-extraction on Mismatch or Missing Cache
    std::error_code ec;
    if (fs::exists(dest_dir)) {
        fs::remove_all(dest_dir, ec);
    }
    if (fs::exists(hash_file)) {
        fs::remove(hash_file, ec);
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

        if (!entry_name.empty() && entry_name.back() == '/') {
            continue;
        }

        if (entry_name == "AndroidManifest.xml" ||
            entry_name == "classes.jar" ||
            entry_name == "proguard.txt" ||
            entry_name == "R.txt" ||
            entry_name.rfind("res/", 0) == 0 ||
            entry_name.rfind("jni/", 0) == 0 ||
            entry_name.rfind("libs/", 0) == 0 ||
            entry_name.rfind("assets/", 0) == 0) {

            fs::path target_path = dest_dir / entry_name;
            if (!extract_zip_entry(archive, i, target_path)) {
                extraction_failed = true;
                break;
            }
        }
    }

    zip_close(archive);

    // Fail only if zip extraction errored or AndroidManifest.xml is missing
    if (extraction_failed || !fs::exists(dest_dir / "AndroidManifest.xml")) {
        UI::error("Failure in AAR extraction for " + lib_identifier);
        fs::remove_all(dest_dir, ec);
        fs::remove(hash_file, ec);
        return false;
    }

    // 3. Compute Hash of Extracted Directory and Write Marker
    std::string computed_hash = get_dir_hash(dest_dir);
    if (computed_hash.empty()) {
        UI::error("Failed to compute directory hash for " + lib_identifier);
        fs::remove_all(dest_dir, ec);
        fs::remove(hash_file, ec);
        return false;
    }

    std::ofstream out_hash(hash_file);
    if (!out_hash.is_open()) {
        UI::error("Failed to persist hash for " + lib_identifier);
        return false;
    }
    out_hash << computed_hash << "\n";
    out_hash.close();

    return true;
}

bool extract_all(const std::vector<std::string>& resolved_paths) {
    std::vector<std::future<bool>> tasks;
    for (const auto& path_str : resolved_paths) {
        fs::path p(path_str);
        if (p.extension() == ".aar") {
            tasks.push_back(std::async(std::launch::async, [path_str]() {
                return extract_aar(path_str);
            }));
        }
    }

    bool all_success = true;
    for (auto& task : tasks) {
        if (!task.get()) {
            all_success = false;
        }
    }
    return all_success;
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
            fs::path aar_dir = p.parent_path() / "extracted_aar";

            // 1. Core bytecode JAR
            fs::path classes_jar = aar_dir / "classes.jar";
            if (fs::exists(classes_jar)) {
                result.jvm_classpath_jars.push_back(classes_jar);
            }

            // 2. Secondary embedded JARs (e.g., libs/repackaged.jar)
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
