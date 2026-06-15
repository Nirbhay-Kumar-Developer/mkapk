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

namespace fs = std::filesystem;

using RunFunc = std::function<void(const std::vector<std::string>&, const std::string&)>;

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
 * Handles configuration mapping polymorphicly for JVM and Native systems.
 */
void execute_plugin_compiler(
    const LanguagePlugin& plugin,
    const std::vector<fs::path>& files,
    const fs::path& java_out,
    const fs::path& bin_dir,
    const fs::path& android_jar,
    RunFunc run) 
{
    if (files.empty()) return;

    std::cout << ">> [" << plugin.name << "] Compiling " << files.size() << " files via " << plugin.compiler << "...";
    if (plugin.is_verified) {
        std::cout << " [Verified Driver]";
    }
    std::cout << std::endl;

    std::vector<std::string> args = { plugin.compiler };

    // Polymorphic structural flag configurations mapping to core system types
    if (plugin.output_type == "jvm") {
        // Formulate standard Java Virtual Machine arguments setup
        args.push_back("-classpath");
        args.push_back(fs::absolute(android_jar).string() + ":" + fs::absolute(java_out).string());
        args.push_back("-d");
        args.push_back(fs::absolute(java_out).string());
    } else if (plugin.output_type == "native") {
        // Native linking targets matching intermediate build paths
        fs::path native_out_dir = bin_dir / "libs" / "obj";
        fs::create_directories(native_out_dir);
        
        args.push_back("-c"); // Standard compiler instruction
        args.push_back("-I" + fs::absolute(android_jar).parent_path().string());
    }

    // Append localized source files targets list
    for (const auto& path : files) {
        args.push_back(fs::absolute(path).string());
    }

    run(args, plugin.name + " pipeline execution error");
}

/**
 * Orchestrates joint source compilation tasks dynamically.
 * UPDATED: Resolves circular Kotlin -> Java dependencies through unified Joint-Compilation.
 */
std::pair<fs::path, fs::path> compile_source_logic(
    const std::string& config_content,
    std::map<std::string, std::string>& tools,
    const std::map<std::string, LanguagePlugin>& active_plugins,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    std::map<std::string, std::vector<fs::path>>& changed_files,
    std::map<std::string, std::vector<fs::path>>& deleted_files,
    bool do_res,
    RunFunc run) {

    // Unified destination directory for classes to let Java reference custom dependencies seamlessly
    fs::path java_out = fs::absolute(bin_dir / "classes" / "java_classes");
    fs::path dex_cache = fs::absolute(bin_dir / "dex_cache");
    fs::path gen_src = fs::absolute(bin_dir / "gen"); 

    fs::create_directories(java_out);
    fs::create_directories(dex_cache);

    // 1. Clean up stale class/dex files
    cleanup_stale_assets(deleted_files, java_out, dex_cache);

    // --- PHASE 1: PRE-COLLECT ALL JAVA REFERENCE STUBS ---
    // We aggregate regular Java source changes alongside generated layout references (R.java)
    // upfront so that Kotlin's AST analyzer can accurately reconcile cross-language calls.
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

    // Sync back the completely aggregated vector list into our shared mapping table
    changed_files["java"] = unified_java_sources;

    // --- PHASE 2: JOINT KOTLIN COMPILATION STEP ---
    // Feed BOTH Kotlin paths and Java file signatures directly into kotlinc.
    if (changed_files.find("kotlin") != changed_files.end() && !changed_files["kotlin"].empty()) {
        std::string compose_plug = MkapkEnv::get_json_val("COMPOSE_PLUGIN", config_content);
        
        std::vector<fs::path> joint_sources = changed_files["kotlin"];
        joint_sources.insert(joint_sources.end(), unified_java_sources.begin(), unified_java_sources.end());

        compile_incremental_kotlin(
            tools["kotlinc"],
            fs::absolute(android_jar),
            java_out,
            joint_sources, // Optimized joint-sources vector structure
            run,
            compose_plug
        );
    }

    // --- PHASE 3: CORE JVM JAVA BYTECODE GENERATION ---
    // Run default Java compilation securely via the daemon channels to finalize actual bytecode generation.
    if (changed_files.find("java") != changed_files.end() && !changed_files["java"].empty()) {
        std::string java_ver = MkapkEnv::get_json_val("JAVA_VERSION", config_content);
        if (java_ver.empty()) java_ver = "11";
        
        std::cout << ">> [JAVA] Compiling " << changed_files["java"].size() << " files (including generated references)..." << std::endl;
        compile_incremental_java(java_ver, {}, fs::absolute(android_jar), fs::absolute(java_out), changed_files["java"], run);
    }

    // --- PHASE 4: EXTENSIBLE DYNAMIC PLUGIN PIPELINE ---
    for (const auto& [ext, plugin] : active_plugins) {
        if (plugin.name == "java" || plugin.name == "kotlin" || plugin.name == "native") {
            continue;
        }

        auto change_entry = changed_files.find(plugin.name);
        if (change_entry != changed_files.end() && !change_entry->second.empty()) {
            execute_plugin_compiler(plugin, change_entry->second, java_out, bin_dir, android_jar, run);
        }
    }

    // --- PHASE 5: NATIVE ENGINES VERIFICATION PASS ---
    if (changed_files.find("native") != changed_files.end() && !changed_files["native"].empty()) {
        std::cout << ">> [NATIVE] Processing localized C/C++ core module updates..." << std::endl;
    }

    return {java_out, dex_cache};
}