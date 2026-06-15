#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <openssl/md5.h>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

/**
 * SECTION 1: HASHING ENGINE
  */
std::string get_file_hash(const fs::path& file_path) {
    if (!fs::exists(file_path)) {
        return "";
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return "";

    MD5_CTX md5Context;
    MD5_Init(&md5Context);
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        MD5_Update(&md5Context, buffer, file.gcount());
    }

    unsigned char result[MD5_DIGEST_LENGTH];
    MD5_Final(result, &md5Context);

    char hexResult[2 * MD5_DIGEST_LENGTH + 1];
    for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) {
        sprintf(&hexResult[i * 2], "%02x", result[i]);
    }
    return std::string(hexResult);
}

/**
 * INTERNAL HELPER: Resolves the absolute path targeting the profile-specific state file.
 */
fs::path get_profile_state_path(const fs::path& bin_dir, bool is_release) {
    fs::path hash_dir = bin_dir / ".hashes";
    fs::create_directories(hash_dir); // Ensure hidden directory tree exists
    
    if (is_release) {
        return hash_dir / "release_file_hashes.txt";
    } else {
        return hash_dir / "debug_file_hashes.txt";
    }
}

/**
 * SECTION 2: STATE LOADING
 */
std::map<std::string, std::map<std::string, std::string>> load_state_map(const fs::path& hash_file) {
    std::map<std::string, std::map<std::string, std::string>> state;
    if (!fs::exists(hash_file)) return state;

    std::ifstream f(hash_file);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string type, path, hash;
        if (std::getline(ss, type, '|') && std::getline(ss, path, '|') && std::getline(ss, hash, '|')) {
            state[type][path] = hash;
        }
    }
    return state;
}

/**
 * SECTION 3: DYNAMIC PLUGIN-AWARE INCREMENTAL LOGIC
 * UPDATED: Context-aware profile signature routing added via `is_release`.
 */
std::pair<BuildResults, std::map<std::string, std::string>> check_changes(
    const fs::path& bin_dir, 
    const std::string& config_content, 
    bool force_all,
    bool is_release) // Added profile flag to govern differential resolution tracking
{
    // Resolve the targeting profile database path cleanly
    fs::path hash_file = get_profile_state_path(bin_dir, is_release);
    
    // Explicitly clamp variant profile strings to guarantee mode transitions match files
    std::string current_mode = is_release ? "release" : "debug"; 
    
    auto old_state = load_state_map(hash_file);
    
    fs::path src_path = MkapkEnv::resolve_path(MkapkEnv::get_json_val("SRC_DIR", config_content));
    fs::path res_path = MkapkEnv::resolve_path(MkapkEnv::get_json_val("RES_DIR", config_content));
    fs::path manifest_path = MkapkEnv::resolve_path(MkapkEnv::get_json_val("MANIFEST", config_content));

    BuildResults results;
    std::map<std::string, std::string> next_state; 

    // A: Check Mode Switch (Guarded locally for sanity, though profile file isolation handles this naturally now)
    results.mode_switched = (old_state["meta"]["mode"] != current_mode);

    // B: Dynamic Tool/Extension Registration Parsing
    std::map<std::string, LanguagePlugin> installed_plugins = MkapkEnv::load_installed_plugins();
    
    if (installed_plugins.find(".java") == installed_plugins.end()) {
        installed_plugins[".java"] = {"java", "javac", ".java", "jvm", "", true};
    }
    if (installed_plugins.find(".kt") == installed_plugins.end()) {
        installed_plugins[".kt"] = {"kotlin", "kotlinc", ".kt", "jvm", "", true};
    }
    
    std::vector<std::string> native_exts = {".cpp", ".h", ".c", ".s", ".cc", ".hpp"};
    for (const auto& ext : native_exts) {
        if (installed_plugins.find(ext) == installed_plugins.end()) {
            installed_plugins[ext] = {"native", "clang", ext, "native", "", true};
        }
    }

    results.changed_files["java"] = {};
    results.changed_files["kotlin"] = {};
    results.changed_files["native"] = {};

    // C: Scan Source Directory
    if (fs::exists(src_path)) {
        for (const auto& entry : fs::recursive_directory_iterator(src_path)) {
            if (!entry.is_regular_file()) continue;
            
            std::string rel_path = fs::relative(entry.path(), src_path).string();
            std::string f_hash = get_file_hash(entry.path());
            next_state["src|" + rel_path] = f_hash;

            if (force_all || results.mode_switched || old_state["src"][rel_path] != f_hash) {
                std::string ext = entry.path().extension().string();
                
                auto plugin_match = installed_plugins.find(ext);
                if (plugin_match != installed_plugins.end()) {
                    std::string lang_handle = plugin_match->second.name;
                    results.changed_files[lang_handle].push_back(entry.path());
                }
            }
        }
    }

    // D: Scan Resource Directory
    if (fs::exists(res_path)) {
        for (const auto& entry : fs::recursive_directory_iterator(res_path)) {
            if (!entry.is_regular_file()) continue;
            std::string rel_path = fs::relative(entry.path(), res_path).string();
            std::string f_hash = get_file_hash(entry.path());
            next_state["res|" + rel_path] = f_hash;

            if (force_all || results.mode_switched || old_state["res"][rel_path] != f_hash) {
                results.changed_resources.push_back(entry.path());
                results.res_changed = true;
            }
        }
    }

    // E: Detect Deletions
    for (auto const& [old_path, hash] : old_state["src"]) {
        if (next_state.find("src|" + old_path) == next_state.end()) {
            results.deleted_files["src"].push_back(fs::path(old_path));
        }
    }

    // F: Manifest Check
    std::string current_manifest_hash = get_file_hash(manifest_path);
    next_state["meta|manifest"] = current_manifest_hash;
    next_state["meta|mode"] = current_mode;

    results.manifest_changed = (current_manifest_hash != old_state["meta"]["manifest"]) || results.mode_switched;
    
    bool dynamic_src_changes = !results.deleted_files["src"].empty();
    for (const auto& [lang, files] : results.changed_files) {
        if (!files.empty()) {
            dynamic_src_changes = true;
            break;
        }
    }
    results.src_changed = dynamic_src_changes;

    return {results, next_state};
}

/**
 * SECTION 4: STATE SAVING
 * UPDATED: Routes state output targets dynamically based on `is_release`.
 */
void save_state(const fs::path& bin_dir, const std::map<std::string, std::string>& next_state, bool is_release) {
    fs::path state_file = get_profile_state_path(bin_dir, is_release);
    
    std::ofstream f(state_file);
    if (!f.is_open()) {
        std::cerr << "!! Warning: Failed to persist project state verification maps to: " << state_file.filename().string() << std::endl;
        return;
    }
    
    for (auto const& [key, hash] : next_state) {
        f << key << "|" << hash << "|\n";
    }
}