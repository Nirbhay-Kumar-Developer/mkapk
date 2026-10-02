#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <regex>
#include <sstream>
#include <iomanip>
#define XXH_INLINE_ALL 
#include <xxhash.h>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_config.hpp"

namespace fs = std::filesystem;

/**
 * SECTION 1: HASHING ENGINE (XXH3 64-bit streaming)
 */
std::string get_file_hash(const fs::path& file_path) {
    if (!fs::exists(file_path)) {
        return "";
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return "";

    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr) return "";
    
    if (XXH3_64bits_reset(state) == XXH_ERROR) {
        XXH3_freeState(state);
        return "";
    }

    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        if (XXH3_64bits_update(state, buffer, file.gcount()) == XXH_ERROR) {
            XXH3_freeState(state);
            return "";
        }
    }

    XXH64_hash_t hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);

    std::stringstream stream;
    stream << std::setfill('0') << std::setw(16) << std::hex << hash;
    return stream.str();
}

/**
 * INTERNAL HELPERS
 */
static fs::path get_profile_state_path(const fs::path& build_dir, bool is_release) {
    fs::path hash_dir = build_dir / ".hashes";
    fs::create_directories(hash_dir);
    return is_release ? (hash_dir / "release_file_hashes.txt") : (hash_dir / "debug_file_hashes.txt");
}

static std::map<std::string, std::map<std::string, std::string>> load_state_map(const fs::path& hash_file) {
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
 * SECTION 2: JAVA DEPENDENCY RESOLUTION
 * If A.java changes, finds all other .java files that import or refer to class A.
 */
static std::vector<fs::path> expand_java_dependencies(
    const fs::path& src_path, 
    const std::vector<fs::path>& directly_changed_java) 
{
    if (directly_changed_java.empty() || !fs::exists(src_path)) {
        return directly_changed_java;
    }

    std::set<std::string> changed_class_symbols;
    for (const auto& file : directly_changed_java) {
        changed_class_symbols.insert(file.stem().string());
    }

    std::set<fs::path> resolved_batch(directly_changed_java.begin(), directly_changed_java.end());
    std::vector<std::pair<fs::path, std::string>> source_cache;

    // Cache content of all other Java sources in memory for quick scanning
    for (const auto& entry : fs::recursive_directory_iterator(src_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".java") {
            if (resolved_batch.find(entry.path()) == resolved_batch.end()) {
                std::ifstream f(entry.path());
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                source_cache.push_back({entry.path(), content});
            }
        }
    }

    // Fixed-point traversal: propagate dependencies transitively
    bool discovered_new = true;
    while (discovered_new) {
        discovered_new = false;
        for (auto it = source_cache.begin(); it != source_cache.end();) {
            bool depends = false;
            for (const auto& sym : changed_class_symbols) {
                std::regex sym_regex("\\b" + sym + "\\b");
                if (std::regex_search(it->second, sym_regex)) {
                    depends = true;
                    break;
                }
            }

            if (depends) {
                resolved_batch.insert(it->first);
                changed_class_symbols.insert(it->first.stem().string());
                it = source_cache.erase(it);
                discovered_new = true;
            } else {
                ++it;
            }
        }
    }

    return std::vector<fs::path>(resolved_batch.begin(), resolved_batch.end());
}

/**
 * SECTION 3: CONCRETE CHANGE CHECKER IMPLEMENTATION
 */
class IncrementalChangeChecker : public IChangeChecker {
public:
    std::pair<BuildResults, std::map<std::string, std::string>> detect_changes(
        const fs::path& build_dir, 
        const MkapkConfig& config, 
        bool force_all,
        bool is_release) override 
    {
        fs::path hash_file = get_profile_state_path(build_dir, is_release);
        std::string current_mode = is_release ? "release" : "debug";
        auto old_state = load_state_map(hash_file);

        BuildResults results;
        std::map<std::string, std::string> next_state;

        // 1. Mode Switch Detection
        results.mode_switched = (old_state["meta"]["mode"] != current_mode);
        next_state["meta|mode"] = current_mode;

        // 2. Discover Plugins
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

        // 3. Scan Source Files
        fs::path src_path = MkapkEnv::resolve_path(config.src_dir);
        std::vector<fs::path> directly_changed_java;

        if (fs::exists(src_path)) {
            for (const auto& entry : fs::recursive_directory_iterator(src_path)) {
                if (!entry.is_regular_file()) continue;
                
                std::string rel_path = fs::relative(entry.path(), src_path).string();
                std::string f_hash = get_file_hash(entry.path());
                next_state["src|" + rel_path] = f_hash;

                if (force_all || results.mode_switched || old_state["src"][rel_path] != f_hash) {
                    std::string ext = entry.path().extension().string();
                    if (ext == ".java") {
                        directly_changed_java.push_back(entry.path());
                    } else {
                        auto plugin_match = installed_plugins.find(ext);
                        if (plugin_match != installed_plugins.end()) {
                            results.changed_files[plugin_match->second.name].push_back(entry.path());
                        }
                    }
                }
            }
        }

        // Expand Java changes to all dependent classes
        results.changed_files["java"] = expand_java_dependencies(src_path, directly_changed_java);

        // 4. Scan Resources
        fs::path res_path = MkapkEnv::resolve_path(config.res_dir);
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

        // 5. Scan Assets
        fs::path assets_path = MkapkEnv::resolve_path(config.assets_dir);
        if (fs::exists(assets_path)) {
            for (const auto& entry : fs::recursive_directory_iterator(assets_path)) {
                if (!entry.is_regular_file()) continue;
                std::string rel_path = fs::relative(entry.path(), assets_path).string();
                std::string f_hash = get_file_hash(entry.path());
                next_state["asset|" + rel_path] = f_hash;

                if (force_all || results.mode_switched || old_state["asset"][rel_path] != f_hash) {
                    results.changed_assets.push_back(entry.path());
                    results.assets_changed = true;
                }
            }
        }

        // 6. Source Deletions Check
        for (auto const& [old_path, hash] : old_state["src"]) {
            if (next_state.find("src|" + old_path) == next_state.end()) {
                results.deleted_files["src"].push_back(fs::path(old_path));
            }
        }

        // 7. AndroidManifest.xml Check
        fs::path manifest_path = MkapkEnv::resolve_path(config.manifest);
        std::string manifest_hash = get_file_hash(manifest_path);
        next_state["meta|manifest"] = manifest_hash;
        results.manifest_changed = (manifest_hash != old_state["meta"]["manifest"]) || results.mode_switched;

        // 8. Proguard Rules Check
        fs::path pg_rules_path = MkapkEnv::resolve_path(config.proguard_rules);
        std::string pg_hash = fs::exists(pg_rules_path) ? get_file_hash(pg_rules_path) : "";
        next_state["meta|proguard"] = pg_hash;
        results.proguard_changed = (pg_hash != old_state["meta"]["proguard"]) || results.mode_switched;

        // 9. AndResGuard Configuration Check
        fs::path andres_config = fs::current_path() / "andresguard-config.xml";
        if (!fs::exists(andres_config)) {
            andres_config = fs::current_path() / "andresguard.xml";
        }
        std::string andres_hash = fs::exists(andres_config) ? get_file_hash(andres_config) : "";
        next_state["meta|andresguard"] = andres_hash;
        results.andresguard_changed = (andres_hash != old_state["meta"]["andresguard"]) || results.mode_switched;

        // 10. AAPT2 R.txt Symbol Mapping Check
        fs::path r_txt_path = build_dir / "R.txt";
        std::string r_txt_hash = fs::exists(r_txt_path) ? get_file_hash(r_txt_path) : "";
        next_state["meta|r_txt"] = r_txt_hash;
        results.r_txt_changed = (r_txt_hash != old_state["meta"]["r_txt"]) || results.mode_switched;

        // 11. General Source Modification Flag
        bool has_source_changes = !results.deleted_files["src"].empty();
        for (const auto& [lang, files] : results.changed_files) {
            if (!files.empty()) {
                has_source_changes = true;
                break;
            }
        }
        results.src_changed = has_source_changes;

        // -------------------------------------------------------------
        // 12. ACTIONABLE DECISION DIRECTIVES
        // -------------------------------------------------------------

        // Manifest or resource changes require an aapt2 link pass
        results.needs_manifest_relink = results.manifest_changed || results.res_changed || force_all;

        // JVM sources recompile if code changed OR public resource IDs shifted
        results.needs_jvm_compile = results.src_changed || results.r_txt_changed || force_all;

        // Assets or manifest modifications require repackaging
        results.needs_repackage = results.assets_changed || results.needs_manifest_relink || force_all;

        // Dexing and Obfuscation directives
        if (is_release) {
            results.needs_dex_rebuild = results.proguard_changed || results.needs_jvm_compile || force_all;
            results.needs_resource_obfuscation = results.andresguard_changed || results.res_changed || results.manifest_changed || force_all;
        } else {
            results.needs_dex_rebuild = results.needs_jvm_compile || force_all;
            results.needs_resource_obfuscation = false;
        }

        if (results.needs_dex_rebuild || results.needs_resource_obfuscation) {
            results.needs_repackage = true;
        }

        return {results, next_state};
    }
};

/**
 * SECTION 4: PUBLIC INTERFACE EXPORTS
 */
std::pair<BuildResults, std::map<std::string, std::string>> check_changes(
    const fs::path& build_dir, 
    const MkapkConfig& config, 
    bool force_all,
    bool is_release) 
{
    IncrementalChangeChecker checker;
    return checker.detect_changes(build_dir, config, force_all, is_release);
}

void save_state(const fs::path& build_dir, const std::map<std::string, std::string>& next_state, bool is_release) {
    fs::path state_file = get_profile_state_path(build_dir, is_release);
    
    std::ofstream f(state_file);
    if (!f.is_open()) {
        std::cerr << "!! Warning: Failed to persist project state verification maps to: " << state_file.filename().string() << std::endl;
        return;
    }
    
    for (auto const& [key, hash] : next_state) {
        f << key << "|" << hash << "|\n";
    }
}
