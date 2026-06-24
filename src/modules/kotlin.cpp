#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<void(const std::vector<std::string>&, const std::string&)>;

bool compile_incremental_kotlin(
    const std::string& KOTLINC,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func,
    const std::string& compose_plugin,
    const std::vector<std::string>& classpath_extra) 
{
    if (changed_files.empty()) return true;

    UI::stage(UI::Msg::KOTLIN_STAGE, "Compiling " + std::to_string(changed_files.size()) + " changed files");
    
    // Equivalent to classes_dir.mkdir(parents=True)
    fs::create_directories(classes_dir);

    // 1. Build the Classpath (Matches Python's absolute resolution)
    std::vector<std::string> cp_components;
    cp_components.push_back(fs::absolute(android_jar).string());
    cp_components.push_back(fs::absolute(classes_dir).string());

    // Add project-specific libs (*.jar) recursively
    fs::path libs_dir = "libs";
    if (fs::exists(libs_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(libs_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                cp_components.push_back(fs::absolute(entry.path()).string());
            }
        }
    }

    // Add extras from config
    for (const auto& extra : classpath_extra) {
        cp_components.push_back(fs::absolute(fs::path(extra)).string());
    }

    // Join classpath with colon
    std::string classpath = "";
    for (size_t i = 0; i < cp_components.size(); ++i) {
        classpath += cp_components[i] + (i == cp_components.size() - 1 ? "" : ":");
    }

    // 2. Construct base arguments
    std::vector<std::string> args = {
        KOTLINC,
        "-jvm-target", "1.8",
        "-no-jdk",
        "-classpath", classpath,
        "-d", fs::absolute(classes_dir).string()
    };

    // 3. Handle Compose Plugin (Ported from Python logic)
    if (!compose_plugin.empty()) {
        fs::path plugin_path = fs::path(compose_plugin);
        // Equivalent to expanduser() / exists()
        if (fs::exists(plugin_path)) {
            args.push_back("-Xplugin=" + fs::absolute(plugin_path).string());
            args.push_back("-P");
            args.push_back("plugin:androidx.compose.compiler.plugins.kotlin:suppressKotlinVersionCompatibilityCheck=true");
        }
    }

    // 4. Handle Large File Counts via Argument File (Essential Progression)
    fs::path sources_list_file = classes_dir / "kotlin_sources.txt";
    std::ofstream f(sources_list_file);
    if (f.is_open()) {
        for (const auto& p : changed_files) {
            f << fs::absolute(p).string() << "\n";
        }
        f.close();
        args.push_back("@" + sources_list_file.string());
    } else {
        UI::error("Could not write intermediate compilation argument routing maps.", sources_list_file.string());
        return false;
    }

    // 5. Execute via JVM/Backend
    run_func(args, "Kotlin compilation (kotlinc) failed");
    return true;
}

/**
 * Full build fallback: Collects all .kt files.
 * FIXED: Removed default values from implementation parameters.
 */
bool compile_kotlin(
    const std::string& KOTLINC,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const fs::path& src_dir,
    RunFunc run_func,
    const std::string& compose_plugin) 
{
    std::vector<fs::path> kt_files;
    if (!fs::exists(src_dir)) {
        UI::error("Kotlin workspace tracking directory missing from context layout", src_dir.string());
        return false;
    }

    for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".kt") {
            kt_files.push_back(entry.path());
        }
    }

    if (kt_files.empty()) return false;

    return compile_incremental_kotlin(KOTLINC, android_jar, classes_dir, kt_files, run_func, compose_plugin);
}