#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <map>
#include <algorithm>
#include <functional>
#include <utility>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<Result<void>(const std::vector<std::string>&, const std::string&)>;

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
        details += " [Verified Driver]";
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
    RunFunc run) 
{

    fs::path java_out = fs::absolute(bin_dir / "classes" / "java_classes");
    fs::path dex_cache = fs::absolute(bin_dir / "dex_cache");
    fs::path gen_src = fs::absolute(bin_dir / "gen"); 

    fs::create_directories(java_out);
    fs::create_directories(dex_cache);

    // 1. Clean up stale class/dex files
    cleanup_stale_assets(deleted_files, java_out, dex_cache);

    // --- PHASE 0: EXTRACT KOTLIN STANDARD LIBRARY ---
    if (changed_files.find("kotlin") != changed_files.end() && !changed_files["kotlin"].empty()) {
        const char* prefix_env = std::getenv("PREFIX");
        fs::path kotlin_lib_root = prefix_env ? fs::path(prefix_env) / "opt/kotlin/lib/" : "/data/data/com.termux/files/usr/opt/kotlin/lib/";
        fs::path stdlib_jar = kotlin_lib_root / "kotlin-stdlib.jar";

        if (fs::exists(stdlib_jar)) {
            if (!fs::exists(java_out / "kotlin/Unit.class")) {
                UI::stage("Kotlin stdlib", "Unpacking runtime classes payload into destination layout");
                
                std::vector<std::string> unzip_args = {
                    "unzip", "-q", "-o", 
                    stdlib_jar.string(), 
                    "-d", java_out.string()
                };
                
                auto unzip_res = run(unzip_args, "Failed to extract Kotlin standard library elements.");
                if (unzip_res.is_ok()) {
                    fs::remove_all(java_out / "META-INF");
                } else {
                    UI::warn("Stdlib unpack notice: " + unzip_res.get_error());
                }
            }
        } else {
            UI::warn("Kotlin runtime validation failure: Standard library jar not located inside prefix location");
        }
    }

    // --- PHASE 1: PRE-COLLECT ALL JAVA REFERENCE STUBS ---
    std::vector<fs::path> unified_java_sources;
    if (changed_files.find("java") != changed_files.end()) {
        unified_java_sources = changed_files["java"];
    }

    if (fs::exists(gen_src)) {
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

        UI::stage(UI::Msg::KOTLIN_STAGE, "Joint analysis mapping active");
        
        // EXPLICITLY PASS THE CLASSPATH STRINGS HERE
        auto kot_res = compile_incremental_kotlin(
            tools["kotlinc"],
            fs::absolute(android_jar),
            java_out,
            joint_sources,
            run,
            compose_plug
        );
        if (kot_res.is_err()) {
            return Result<std::pair<fs::path, fs::path>>::error(kot_res.get_error());
        }
    }

    // --- PHASE 3: CORE JVM JAVA BYTECODE GENERATION ---
    if (changed_files.find("java") != changed_files.end() && !changed_files["java"].empty()) {
        std::string java_ver = config.java_version;
        if (java_ver.empty()) java_ver = "17";
        
        UI::stage(UI::Msg::JAVA_STAGE, std::to_string(changed_files["java"].size()) + " files total");
        auto java_res = compile_incremental_java(
            java_ver, 
            {}, 
            fs::absolute(android_jar), 
            fs::absolute(java_out), 
            changed_files["java"], 
            run
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
