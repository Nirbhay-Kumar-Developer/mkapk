#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <algorithm>
#include <utility>
#include <fstream>

#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"

namespace fs = std::filesystem;

/**
 * Removes compiled .class and .dex files when the source is deleted.
 */
void cleanup_stale_assets(const std::map<std::string, std::vector<fs::path>>& deleted_files,
                         const fs::path& java_out,
                         const fs::path& dex_cache) {
    for (auto const& [lang, files] : deleted_files) {
        for (const auto& rel_path : files) {
            std::string base_name = rel_path.stem().string();
            fs::path parent_dir = java_out / rel_path.parent_path();

            if (fs::exists(parent_dir)) {
                for (const auto& entry : fs::directory_iterator(parent_dir)) {
                    std::string filename = entry.path().filename().string();
                    if (filename == base_name + ".class" || filename.find(base_name + "$") == 0) {
                        fs::remove(entry.path());
                    }
                }
            }

            fs::path target_dex = (dex_cache / rel_path).replace_extension(".dex");
            fs::remove(target_dex);
        }
    }
}

/**
 * Executes a dynamic compilation process for a registered language plugin.
 */
Result<void> execute_plugin_compiler(
    const LanguagePlugin& plugin,
    const std::vector<fs::path>& files,
    const fs::path& java_out,
    const fs::path& bin_dir,
    const fs::path& android_jar,
    RunFunc run)
{
    if (files.empty()) return Result<void>::success();

    std::string details = "Compiling " + std::to_string(files.size()) + " files via " + plugin.compiler;
    if (plugin.is_verified) {
        details += " [Verified]";
    }
    UI::stage(plugin.name, details);

    std::vector<std::string> args = { plugin.compiler };

    if (plugin.output_type == "jvm") {
        args.push_back("-classpath");
        args.push_back(fs::absolute(android_jar).string() + ":" + fs::absolute(java_out).string());
        args.push_back("-d");
        args.push_back(fs::absolute(java_out).string());
    } else if (plugin.output_type == "native") {
        fs::path native_out_dir = bin_dir / "libs" / "obj";
        fs::create_directories(native_out_dir);

        args.push_back("-c");
        args.push_back("-I" + fs::absolute(android_jar).parent_path().string());
    }

    for (const auto& path : files) {
        args.push_back(fs::absolute(path).string());
    }

    return run(args, plugin.name + " pipeline execution error");
}

// Helper to purge stale .class files of a .java file before recompiling:
static void purge_stale_classes_for_sources(
    const std::vector<fs::path>& java_sources,
    const fs::path& src_dir,
    const fs::path& gen_dir,
    const fs::path& java_out)
{
    for (const auto& src_file : java_sources) {
        fs::path abs_src = fs::absolute(src_file);
        fs::path rel_path;
        if (abs_src.string().rfind(gen_dir.string(), 0) == 0) {
            rel_path = fs::relative(abs_src, gen_dir);
        } else if (abs_src.string().rfind(src_dir.string(), 0) == 0) {
            rel_path = fs::relative(abs_src, src_dir);
        } else {
            continue;
        }

        std::string base_name = rel_path.stem().string();
        fs::path parent_dir = java_out / rel_path.parent_path();
        if (fs::exists(parent_dir)) {
            for (const auto& entry : fs::directory_iterator(parent_dir)) {
                std::string fname = entry.path().filename().string();
                if (fname == base_name + ".class" || fname.rfind(base_name + "$", 0) == 0) {
                    fs::remove(entry.path());
                }
            }
        }
    }
}

/**
 * Orchestrates joint source compilation tasks dynamically.
 */
Result<std::pair<fs::path, fs::path>> compile_source_logic(
    const MkapkConfig& config,
    std::map<std::string, std::string>& tools,
    const std::map<std::string, LanguagePlugin>& active_plugins,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    std::map<std::string, std::vector<fs::path>>& changed_files,
    std::map<std::string, std::vector<fs::path>>& deleted_files,
    bool do_res,
    RunFunc run,
    const std::vector<fs::path>& extra_jvm_classpaths)
{
    fs::path java_out = fs::absolute(bin_dir / "classes" / "java_classes");
    fs::path dex_cache = fs::absolute(bin_dir / "dex_cache");
    fs::path gen_src = fs::absolute(bin_dir / "gen");

    fs::create_directories(java_out);
    fs::create_directories(dex_cache);

    // 1. Clean up stale class/dex files
    cleanup_stale_assets(deleted_files, java_out, dex_cache);

    // Convert extra_jvm_classpaths to string vector for Kotlinc
    std::vector<std::string> classpath_extra_strs;
    for (const auto& p : extra_jvm_classpaths) {
        classpath_extra_strs.push_back(fs::absolute(p).string());
    }

    // --- PHASE 1: PRE-COLLECT ALL JAVA REFERENCE STUBS ---
    std::vector<fs::path> unified_java_sources;
    if (changed_files.find("java") != changed_files.end()) {
        unified_java_sources = changed_files["java"];
    }

    bool r_classes_missing = true;
    if (fs::exists(java_out)) {
        for (const auto& entry : fs::recursive_directory_iterator(java_out)) {
            if (entry.is_regular_file() && entry.path().filename() == "R.class") {
                r_classes_missing = false;
                break;
            }
        }
    }

    if ((do_res || r_classes_missing) && fs::exists(gen_src)) {
        for (const auto& entry : fs::recursive_directory_iterator(gen_src)) {
            if (entry.is_regular_file() && entry.path().extension() == ".java") {
                fs::path absolute_gen_path = fs::absolute(entry.path());
                if (std::find(unified_java_sources.begin(), unified_java_sources.end(), absolute_gen_path) == unified_java_sources.end()) {
                    unified_java_sources.push_back(absolute_gen_path);
                }
            }
        }
    }

    changed_files["java"] = unified_java_sources;

    // --- PHASE 2: JOINT KOTLIN COMPILATION STEP ---
    if (changed_files.find("kotlin") != changed_files.end() && !changed_files["kotlin"].empty()) {
        std::string compose_plug = config.compose_plugin;
        std::vector<fs::path> joint_sources = changed_files["kotlin"];
        joint_sources.insert(joint_sources.end(), unified_java_sources.begin(), unified_java_sources.end());

        bool is_release = (bin_dir.string().find("release") != std::string::npos);

        auto kot_res = compile_incremental_kotlin(
            tools["kotlinc"],
            fs::absolute(android_jar),
            java_out,
            joint_sources,
            run,
            compose_plug,
            is_release,
            classpath_extra_strs
        );
        if (kot_res.is_err()) {
            return Result<std::pair<fs::path, fs::path>>::error(kot_res.get_error());
        }
    }

    // --- PHASE 3: ABI-AWARE INCREMENTAL JAVA BYTECODE GENERATION ---
    fs::path abs_src_dir = fs::absolute(MkapkEnv::resolve_path(config.src_dir));
    fs::path bytecode_state_file = fs::absolute(bin_dir / ".hashes" / "bytecode_graph.bin");
    fs::path impacted_list_file = fs::absolute(bin_dir / ".hashes" / "impacted_java.txt");
    fs::create_directories(bytecode_state_file.parent_path());

    bool has_direct_java = (changed_files.find("java") != changed_files.end() && !changed_files["java"].empty());
    bool has_deleted_src = (deleted_files.find("src") != deleted_files.end() && !deleted_files["src"].empty());
    bool has_kotlin_changes = (changed_files.find("kotlin") != changed_files.end() && !changed_files["kotlin"].empty());

    std::string java_ver = config.java_version.empty() ? "17" : config.java_version;

    // Compile directly modified Java sources
    if (has_direct_java) {
        purge_stale_classes_for_sources(changed_files["java"], abs_src_dir, gen_src, java_out);

        UI::stage(UI::Msg::STAGE_JAVA, std::to_string(changed_files["java"].size()) + " modified file(s)");
        auto java_res = compile_incremental_java(
            java_ver,
            {},
            fs::absolute(android_jar),
            java_out,
            changed_files["java"],
            run,
            extra_jvm_classpaths
        );
        if (java_res.is_err()) {
            return Result<std::pair<fs::path, fs::path>>::error(java_res.get_error());
        }
    }
    

    // --- PHASE 4: EXTENSIBLE DYNAMIC PLUGIN PIPELINE ---
    for (const auto& [ext, plugin] : active_plugins) {
        if (plugin.name == "java" || plugin.name == "kotlin" || plugin.name == "native") {
            continue;
        }

        auto change_entry = changed_files.find(plugin.name);
        if (change_entry != changed_files.end() && !change_entry->second.empty()) {
            auto plug_res = execute_plugin_compiler(plugin, change_entry->second, java_out, bin_dir, android_jar, run);
            if (plug_res.is_err()) {
                return Result<std::pair<fs::path, fs::path>>::error(plug_res.get_error());
            }
        }
    }

    return Result<std::pair<fs::path, fs::path>>::success({java_out, dex_cache});
}
