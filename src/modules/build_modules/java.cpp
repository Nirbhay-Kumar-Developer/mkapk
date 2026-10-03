#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_result.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<Result<void>(const std::vector<std::string>&, const std::string&)>;

Result<void> compile_incremental_java(
    const std::string& version,
    const std::vector<std::string>& flags,
    const fs::path& android_jar,
    const fs::path& out_dir,
    const std::vector<fs::path>& changed_files,
    RunFunc run_func,
    const std::vector<fs::path>& extra_dependency_jars)
{
    if (changed_files.empty()) return Result<void>::success();

    fs::create_directories(out_dir);

    std::vector<std::string> cp_components;
    cp_components.push_back(fs::absolute(android_jar).string());
    cp_components.push_back(fs::absolute(out_dir).string());

    for (const auto& jar : extra_dependency_jars) {
        if (fs::exists(jar)) {
            cp_components.push_back(fs::absolute(jar).string());
        }
    }

    fs::path libs_dir = "libs";
    if (fs::exists(libs_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(libs_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jar") {
                std::string abs_jar = fs::absolute(entry.path()).string();
                if (std::find(cp_components.begin(), cp_components.end(), abs_jar) == cp_components.end()) {
                    cp_components.push_back(abs_jar);
                }
            }
        }
    }

    std::string cp_str = "";
    for (size_t i = 0; i < cp_components.size(); ++i) {
        cp_str += cp_components[i] + (i == cp_components.size() - 1 ? "" : ":");
    }

    std::vector<std::string> args = {
        "javac",
        "--release", version,
        "-encoding", "UTF-8",
        "-classpath", cp_str,
        "-d", fs::absolute(out_dir).string(),
    };

    if (!flags.empty()) {
        args.insert(args.end(), flags.begin(), flags.end());
    }

    for (const auto& src_file : changed_files) {
        args.push_back(fs::absolute(src_file).string());
    }

    auto res = run_func(args, "Incremental Java compilation (javac) failed");
    if (res.is_err()) return res;
    
    return Result<void>::success();
}

Result<void> compile_java(
    const std::string& java_version,
    const std::vector<std::string>& javac_flags,
    const fs::path& android_jar,
    const fs::path& classes_dir,
    const fs::path& src_dir,
    RunFunc run_func) 
{
    UI::stage(UI::Msg::STAGE_JAVA, "Compiling all sources via JNI backend...");

    if (!fs::exists(src_dir)) {
        return Result<void>::error("Source directory missing: " + src_dir.string());
    }

    std::vector<fs::path> java_files;
    for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".java") {
            java_files.push_back(entry.path());
        }
    }

    if (java_files.empty()) {
        UI::info("No Java source files found.");
        return Result<void>::success();
    }

    return compile_incremental_java(
        java_version,
        javac_flags,
        android_jar,
        classes_dir,
        java_files,
        run_func,
        {});
}
