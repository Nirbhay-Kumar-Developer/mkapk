#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_result.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<Result<void>(const std::vector<std::string>&, const std::string&)>;

/**
 * Utility to collect all compiled .class files.
 */
std::vector<std::string> get_all_class_files(const fs::path& bin_dir) {
    fs::path resolved_bin = fs::absolute(bin_dir);
    fs::path class_dir = resolved_bin / "classes" / "java_classes";

    std::vector<std::string> all_files;
    if (fs::exists(class_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(class_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".class") {
                all_files.push_back(fs::absolute(entry.path()).string());
            }
        }
    }
    return all_files;
}

/**
 * Converts .class files to .dex incrementally using D8 with Java 8+ desugaring
 * and supports external dependency classpaths (AAR/JAR).
 */
Result<void> run_incremental_dex(
    const std::string& D8,
    const fs::path& android_jar,
    const fs::path& src_path,
    const fs::path& java_out,
    const fs::path& dex_cache,
    const std::vector<fs::path>& files_to_dex,
    const std::vector<fs::path>& extra_jvm_classpaths,
    RunFunc run) 
{
    if (files_to_dex.empty()) return Result<void>::success();

    for (const auto& src_file : files_to_dex) {
        fs::path rel_path = fs::relative(src_file, src_path);
        fs::path class_dir = java_out / rel_path.parent_path();
        std::string base_name = rel_path.stem().string();

        std::vector<std::string> family_classes;
        if (fs::exists(class_dir)) {
            for (const auto& entry : fs::directory_iterator(class_dir)) {
                std::string filename = entry.path().filename().string();
                // Exact matching for base class and its inner classes
                if (filename == base_name + ".class" || filename.rfind(base_name + "$", 0) == 0) {
                    family_classes.push_back(fs::absolute(entry.path()).string());
                }
            }
        }

        if (!family_classes.empty()) {
            fs::path target_dex_dir = dex_cache / rel_path.parent_path();
            fs::create_directories(target_dex_dir);

            std::vector<std::string> d8_args = {
                "d8", 
                "--min-api", "21", // Automates Java 8+ language desugaring
                "--lib", fs::absolute(android_jar).string(),
                "--classpath", fs::absolute(java_out).string(),
                "--output", fs::absolute(target_dex_dir).string()
            };

            // Inject extra dependency archives (from AAR/JAR) for interface desugaring
            for (const auto& jar : extra_jvm_classpaths) {
                d8_args.push_back("--classpath");
                d8_args.push_back(fs::absolute(jar).string());
            }

            for (const auto& cls : family_classes) {
                d8_args.push_back(cls);
            }

            auto res = run(d8_args, "Incremental D8 failed for: " + base_name);
            if (res.is_err()) return res;

            fs::path gen_dex = target_dex_dir / "classes.dex";
            fs::path final_dex = target_dex_dir / (base_name + ".dex");
            if (fs::exists(gen_dex)) {
                fs::rename(gen_dex, final_dex);
            }
        }
    }
    return Result<void>::success();
}

/**
 * Optimizes and shrinks bytecode for release using R8:
 * - Employs Whole-Program Tree Shaking & Inlining
 * - Automates keep-rules generation by merging AAPT2 and user rules
 * - Exports mapping.txt for de-obfuscation
 * - Uses Arg-Files to prevent E2BIG overflow on large codebases
 */
Result<void> run_dex_r8(
    const std::string& R8_TOOL,
    const fs::path& android_jar,
    const MkapkConfig& config,
    const fs::path& bin_dir,
    RunFunc run_func,
    bool no_obs,
    const std::vector<fs::path>& extra_dependency_jars)
{
    fs::path bin_dir_path = fs::absolute(bin_dir);
    std::vector<std::string> class_files = get_all_class_files(bin_dir_path);

    if (class_files.empty()) {
        return Result<void>::error(UI::Msg::FATAL_INTERNAL);
    }

    const char* prefix_env = std::getenv("PREFIX");
    fs::path kotlin_lib_root = prefix_env ? fs::path(prefix_env) / "opt/kotlin/lib/" : "/data/data/com.termux/files/usr/opt/kotlin/lib/";
    fs::path annotations_jar = kotlin_lib_root / "annotations-13.0.jar"; 

    std::vector<std::string> args = {
        R8_TOOL,
        "--release",
        "--min-api", "21",
        "--lib", fs::absolute(android_jar).string(),
        "--output", bin_dir_path.string()
    };

    if (fs::exists(annotations_jar)) {
        args.push_back("--lib");
        args.push_back(fs::absolute(annotations_jar).string());
    }

    if (!no_obs) {
        // 1. AAPT2 Keep Rules
        fs::path aapt_rules = bin_dir_path / "aapt_rules.pro";
        if (fs::exists(aapt_rules)) {
            args.push_back("--pg-conf");
            args.push_back(aapt_rules.string());
        }

        // 2. User ProGuard Rules
        if (!config.proguard_rules.empty()) {
            fs::path pg_rules = fs::absolute(MkapkEnv::resolve_path(config.proguard_rules));
            if (fs::exists(pg_rules)) {
                args.push_back("--pg-conf");
                args.push_back(pg_rules.string());
            } else {
                std::cout << "!! Warning: ProGuard rules not found at " << pg_rules << std::endl;
            }
        }

        // 3. Mapping output
        fs::path mapping_file = bin_dir_path / "mapping.txt";
        args.push_back("--pg-map-output");
        args.push_back(mapping_file.string());
    }

    // 4. Batch all app class files AND dependency JARs into r8_inputs.txt
    fs::path input_list_file = bin_dir_path / "r8_inputs.txt";
    std::ofstream input_file(input_list_file);
    if (input_file.is_open()) {
        for (const auto& file : class_files) {
            input_file << file << "\n";
        }
        std::set<std::string> seen_jars;
        for (const auto& dep_jar : extra_dependency_jars) {
            if (fs::exists(dep_jar)) {
                std::string abs_jar = fs::absolute(dep_jar).string();
                if (seen_jars.insert(abs_jar).second) {
                    input_file << abs_jar << "\n";
                }
            }
        }
        input_file.close();
        args.push_back("@" + input_list_file.string());
    } else {
        for (const auto& file : class_files) {
            args.push_back(file);
        }
        for (const auto& dep_jar : extra_dependency_jars) {
            if (fs::exists(dep_jar)) {
                args.push_back(fs::absolute(dep_jar).string());
            }
        }
    }

    auto res = run_func(args, "R8 optimization failed");
    if (res.is_err()) return res;

    return Result<void>::success();
}

/**
 * Merges cached incremental DEX files or raw class files using D8.
 */
Result<void> run_dex_d8(
    const std::string& D8_TOOL,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    const fs::path& dex_cache,
    RunFunc run_func) 
{
    fs::path bin_dir_path = fs::absolute(bin_dir);
    std::vector<std::string> inputs;

    // 1. Gather all cached incremental .dex files
    if (fs::exists(dex_cache)) {
        for (const auto& entry : fs::recursive_directory_iterator(dex_cache)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                inputs.push_back(fs::absolute(entry.path()).string());
            }
        }
    }

    // 2. Fallback: If cache is empty, gather all compiled .class files
    if (inputs.empty()) {
        inputs = get_all_class_files(bin_dir_path);
    }

    if (inputs.empty()) {
        return Result<void>::error("No .dex or .class files found for D8. Check compiler output.");
    }

    std::vector<std::string> args = {
        D8_TOOL,
        "--min-api", "21", // Automates Java 8+ language feature desugaring
        "--lib", fs::absolute(android_jar).string(),
        "--output", bin_dir_path.string()
    };

    // Prevent ARG_MAX overflow during intermediate DEX merge
    fs::path merge_list_file = bin_dir_path / "d8_inputs.txt";
    std::ofstream merge_file(merge_list_file);
    if (merge_file.is_open()) {
        for (const auto& input : inputs) {
            merge_file << input << "\n";
        }
        merge_file.close();
        args.push_back("@" + merge_list_file.string());
    } else {
        for (const auto& input : inputs) {
            args.push_back(input);
        }
    }

    auto res = run_func(args, "D8 merge failed");
    if (res.is_err()) return res;

    return Result<void>::success();
}
