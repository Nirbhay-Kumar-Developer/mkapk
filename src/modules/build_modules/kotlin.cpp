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
    const std::string& compose_plugin,
    bool is_release) 
{
    if (changed_files.empty()) return Result<void>::success();

    UI::stage(UI::Msg::STAGE_KOTLIN, "Compiling " + std::to_string(changed_files.size()) + " files (Release: " + (is_release ? "true" : "false") + ")");
    
    fs::create_directories(classes_dir);

    // Track paths to deduplicate while maintaining resolution priority
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

    // 2. Active Output Directory (for joint compilation with generated R.java & Java class references)
    add_to_cp(classes_dir);

    // 3. Local Project Libs Directory (.jar dependencies)
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

    // 4. Base Compiler Flags (Targeting JVM 17 for modern runtime compatibility)
    std::vector<std::string> args = {
        KOTLINC,
        "-jvm-target", "17",
        "-no-jdk",
        "-classpath", classpath,
        "-d", fs::absolute(classes_dir).string()
    };

    // 5. Automated Release Bytecode Stripping
    // Drops runtime parameter null-checks (checkNotNullParameter), cutting code size
    if (is_release) {
        args.push_back("-Xno-param-assertions");
        args.push_back("-Xno-call-assertions");
        args.push_back("-nowarn");
    }

    // 6. Jetpack Compose Compiler Plugin Integration
    if (!compose_plugin.empty()) {
        fs::path plugin_path = fs::path(compose_plugin);
        if (fs::exists(plugin_path)) {
            args.push_back("-Xplugin=" + fs::absolute(plugin_path).string());
            args.push_back("-P");
            args.push_back("plugin:androidx.compose.compiler.plugins.kotlin:suppressKotlinVersionCompatibilityCheck=true");
        }
    }

    // 7. Auto-Detect and Enable Standard Kotlin Compiler Plugins (Parcelize & Serialization)
    const char* prefix_env = std::getenv("PREFIX");
    fs::path kotlin_lib_root = prefix_env ? fs::path(prefix_env) / "opt/kotlin/lib/" : "/data/data/com.termux/files/usr/opt/kotlin/lib/";

    fs::path parcelize_plugin = kotlin_lib_root / "kotlin-parcelize-compiler.jar";
    if (fs::exists(parcelize_plugin)) {
        args.push_back("-Xplugin=" + fs::absolute(parcelize_plugin).string());
    }

    fs::path serialization_plugin = kotlin_lib_root / "kotlinx-serialization-compiler.jar";
    if (fs::exists(serialization_plugin)) {
        args.push_back("-Xplugin=" + fs::absolute(serialization_plugin).string());
    }

    // 8. Write source file paths to an arg-file (@kotlin_sources.txt) to avoid OS ARG_MAX limits
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
