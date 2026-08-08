#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>
#include <set>
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_result.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<Result<void>(const std::vector<std::string>&, const std::string&)>;

Result<void> compile_incremental_kotlin(
    const std::string& KOTLINC,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func,
    const std::string& compose_plugin) 
{
    if (changed_files.empty()) return Result<void>::success();

    UI::stage(UI::Msg::KOTLIN_STAGE, "Compiling " + std::to_string(changed_files.size()) + " changed files");
    
    fs::create_directories(classes_dir);

    // Use an ordered set tracking vector to deduplicate classpaths while maintaining resolution order
    std::vector<std::string> cp_components;
    std::set<std::string> visited_paths;

    auto add_to_cp = [&](const fs::path& p) {
        if (fs::exists(p)) {
            std::string abs_str = fs::absolute(p).string();
            if (visited_paths.find(abs_str) == visited_paths.end()) {
                visited_paths.insert(abs_str);
                cp_components.push_back(abs_str);
            }
        }
    };

    // 1. Android SDK Bootclasspath
    add_to_cp(android_jar);

    // 2. Active Output Output Directory (for joint compilation Java class references)
    add_to_cp(classes_dir);

    // 3. Local Project Libs Directory
    fs::path libs_dir = "libs";
    if (fs::exists(libs_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(libs_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                add_to_cp(entry.path());
            }
        }
    }

    // Assemble the delimited classpath string
    std::string classpath = "";
    for (size_t i = 0; i < cp_components.size(); ++i) {
        classpath += cp_components[i] + (i == cp_components.size() - 1 ? "" : ":");
    }

    std::vector<std::string> args = {
        KOTLINC,
        "-language-version", "1.9",
        "-jvm-target", "1.8",
        "-no-jdk",
        "-classpath", classpath,
        "-d", fs::absolute(classes_dir).string()
    };

    if (!compose_plugin.empty()) {
        fs::path plugin_path = fs::path(compose_plugin);
        if (fs::exists(plugin_path)) {
            args.push_back("-Xplugin=" + fs::absolute(plugin_path).string());
            args.push_back("-P");
            args.push_back("plugin:androidx.compose.compiler.plugins.kotlin:suppressKotlinVersionCompatibilityCheck=true");
        }
    }

    // Write source file paths to an arg-file (@sources.txt) to avoid OS arg list length limits
    fs::path sources_list_file = classes_dir / "kotlin_sources.txt";
    std::ofstream f(sources_list_file);
    if (f.is_open()) {
        for (const auto& p : changed_files) {
            f << fs::absolute(p).string() << "\n";
        }
        f.close();
        args.push_back("@" + sources_list_file.string());
    } else {
        return Result<void>::error("Could not write intermediate compilation argument routing maps to: " + sources_list_file.string());
    }

    auto res = run_func(args, "Kotlin compilation (kotlinc) failed");
    if (res.is_err()) return res;
    
    return Result<void>::success();
}
