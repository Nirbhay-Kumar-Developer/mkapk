#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<void(const std::vector<std::string>&, const std::string&)>;

/**
 * Utility to collect all .class files.
  */
std::vector<std::string> get_all_class_files(const fs::path& bin_dir) {
    fs::path resolved_bin = fs::absolute(bin_dir);
    
    // Scans both destination directories generated during joint compilation
    std::vector<fs::path> class_dirs = {
        resolved_bin / "classes" / "java_classes"
    };

    std::vector<std::string> all_files;
    for (const auto& c_dir : class_dirs) {
        if (fs::exists(c_dir)) {
            for (const auto& entry : fs::recursive_directory_iterator(c_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".class") {
                    all_files.push_back(fs::absolute(entry.path()).string());
                }
            }
        }
    }
    return all_files;
}

/**
 * Converts .class files to .dex incrementally.
 * Processed here inside dex.cpp to isolate D8 interactions.
 */
void run_incremental_dex(const std::string& D8,
                         const fs::path& android_jar,
                         const fs::path& src_path,
                         const fs::path& java_out,
                         const fs::path& dex_cache,
                         const std::vector<fs::path>& files_to_dex,
                         RunFunc run) {
    if (files_to_dex.empty()) return;

    for (const auto& src_file : files_to_dex) {
        fs::path rel_path = fs::relative(src_file, src_path);
        fs::path class_dir = java_out / rel_path.parent_path();
        std::string base_name = rel_path.stem().string();

        std::vector<std::string> family_classes;
        if (fs::exists(class_dir)) {
            for (const auto& entry : fs::directory_iterator(class_dir)) {
                std::string filename = entry.path().filename().string();
                if (filename == base_name + ".class" || filename.find(base_name + "$") == 0) {
                    family_classes.push_back(fs::absolute(entry.path()).string());
                }
            }
        }

        if (!family_classes.empty()) {
            fs::path target_dex_dir = dex_cache / rel_path.parent_path();
            fs::create_directories(target_dex_dir);

            std::vector<std::string> d8_args = {
                "d8", 
                "--lib", fs::absolute(android_jar).string(),
                "--classpath", fs::absolute(java_out).string(),
                "--output", fs::absolute(target_dex_dir).string()
            };
            for (const auto& cls : family_classes) d8_args.push_back(cls);

            run(d8_args, "Incremental D8 failed for: " + base_name);

            fs::path gen_dex = target_dex_dir / "classes.dex";
            fs::path final_dex = target_dex_dir / (base_name + ".dex");
            if (fs::exists(gen_dex)) {
                fs::rename(gen_dex, final_dex);
            }
        }
    }
}

/**
 * (Step iv) Converts .class files using R8.
 */
void run_dex_r8(
    const std::string& R8_TOOL,
    const fs::path& android_jar,
    const MkapkConfig& config,
    const fs::path& bin_dir,
    RunFunc run_func,
    bool no_obs)
{
    std::cout << ">> [R8] Optimizing for release (Obfuscation: " << (no_obs ? "false" : "true") << ")..." << std::endl;

    fs::path bin_dir_path = fs::absolute(bin_dir);
    std::vector<std::string> class_files = get_all_class_files(bin_dir_path);

    if (class_files.empty()) {
        std::cerr << "!! Error: No class files found for R8. Check compiler output." << std::endl;
        std::exit(1); 
    }

    // --- SOLUTION 3: RESOLVE JETBRAINS ANNOTATIONS PATH ---
    const char* prefix_env = std::getenv("PREFIX");
    fs::path kotlin_lib_root = prefix_env ? fs::path(prefix_env) / "opt/kotlin/lib/" : "/data/data/com.termux/files/usr/opt/kotlin/lib/";
    fs::path annotations_jar = kotlin_lib_root / "annotations-13.0.jar"; 

    std::vector<std::string> args = {
        R8_TOOL,
        "--release",
        "--lib", fs::absolute(android_jar).string()
    };

    // If the companion annotations archive payload exists, append it as a reference graph library
    if (fs::exists(annotations_jar)) {
        args.push_back("--lib");
        args.push_back(fs::absolute(annotations_jar).string());
    } else {
        std::cerr << "!! Warning: JetBrains annotations jar not found at: " << annotations_jar << std::endl;
    }

    // Continue with the remaining standard R8 args setup
    args.push_back("--output");
    args.push_back(bin_dir_path.string());

    if (!no_obs) {
        std::string pg_rules_raw = config.proguard_rules;
        if (!pg_rules_raw.empty()) {
            fs::path pg_rules = fs::absolute(MkapkEnv::resolve_path(pg_rules_raw));
            if (fs::exists(pg_rules)) {
                args.push_back("--pg-conf");
                args.push_back(pg_rules.string());
            } else {
                std::cout << "!! Warning: ProGuard rules not found at " << pg_rules << std::endl;
            }
        }
    }

    for (const auto& file : class_files) {
        args.push_back(file);
    }

    run_func(args, "R8 optimization failed");
}

/**
 * (Step iv Alternate) Merges incrementally dexed files using D8.
 */
void run_dex_d8(
    const std::string& D8_TOOL,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    const fs::path& dex_cache,
    RunFunc run_func) 
{
    std::cout << ">> [DEX] Merging with D8..." << std::endl;

    fs::path bin_dir_path = fs::absolute(bin_dir);
    std::vector<std::string> all_dex;

    if (fs::exists(dex_cache)) {
        for (const auto& entry : fs::recursive_directory_iterator(dex_cache)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                all_dex.push_back(fs::absolute(entry.path()).string());
            }
        }
    }

    if (all_dex.empty()) {
        std::cerr << "!! Error: No DEX files found in cache. Did incremental dexing fail?" << std::endl;
        std::exit(1);
    }

    std::vector<std::string> args = {
        D8_TOOL,
        "--lib", fs::absolute(android_jar).string(),
        "--output", bin_dir_path.string()
    };

    for (const auto& dex : all_dex) {
        args.push_back(dex);
    }

    run_func(args, "D8 merge failed");
}