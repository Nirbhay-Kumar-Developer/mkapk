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

void compile_incremental_java(
    const std::string& version,
    const std::vector<std::string>& flags,
    const fs::path& android_jar,
    const fs::path& out_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func) 
{
    if (changed_files.empty()) return;

    // Ensure output directory exists
    fs::create_directories(out_dir);

    // 1. Build Classpath
    std::vector<std::string> cp_components;
    cp_components.push_back(fs::absolute(android_jar).string());
    cp_components.push_back(fs::absolute(out_dir).string());

    fs::path libs_dir = "libs";
    if (fs::exists(libs_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(libs_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                cp_components.push_back(fs::absolute(entry.path()).string());
            }
        }
    }

    // Join with colon (Linux/Android standard)
    std::string cp_str = "";
    for (size_t i = 0; i < cp_components.size(); ++i) {
        cp_str += cp_components[i] + (i == cp_components.size() - 1 ? "" : ":");
    }

    // 2. Construct Command
    std::vector<std::string> args = {
        "javac",
        "-source", version,
        "-target", version,
        "-encoding", "UTF-8",
        "-classpath", cp_str,
        "-d", fs::absolute(out_dir).string(),
        "-Xlint:none"
    };

    if (!flags.empty()) {
        args.insert(args.end(), flags.begin(), flags.end());
    }

    // 3. Prepare Source Files
    for (const auto& src_file : changed_files) {
        args.push_back(fs::absolute(src_file).string());
    }

    // 4. Execute via the RunFunc (routed to JNI in smart_run)
    run_func(args, "Incremental Java compilation (javac) failed");
}

/**
 * (Step iii Alternate) Full build method. 
 */
bool compile_java(
    const std::string& java_version,
    const std::vector<std::string>& javac_flags,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const fs::path& src_dir,
    RunFunc run_func) 
{
    std::cout << ">> [JAVA] Compiling all sources via JNI backend..." << std::endl;

    if (!fs::exists(src_dir)) {
        std::cerr << "!! Source directory missing: " << src_dir << std::endl;
        return false;
    }

    std::vector<fs::path> java_files;
    for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".java") {
            java_files.push_back(entry.path());
        }
    }

    if (java_files.empty()) {
        std::cout << "!! No Java source files found." << std::endl;
        return false;
    }

    // Corrected function call and return logic
    compile_incremental_java(
        java_version,
        javac_flags,
        android_jar,
        classes_dir,
        java_files,
        run_func);

    return true;
}