#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include "mkapk_helpers.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<void(const std::vector<std::string>&, const std::string&)>;


void compile_resources(
    const std::string& AAPT2,
    const fs::path& res_dir,
    const fs::path& bin_dir,
    RunFunc run_func,
    const std::vector<fs::path>* changed_res_files) 
{
    fs::path flat_dir = bin_dir / "flat_res";
    fs::create_directories(flat_dir);

    if (!fs::exists(res_dir)) {
        std::cerr << "!! Resource directory not found: " << res_dir << std::endl;
        return;
    }

    // Case 1: First build or force-all rebuild (changed_res_files is nullptr)
    if (changed_res_files == nullptr) {
        std::cout << ">> [RES] Compiling all resources (aapt2 compile)..." << std::endl;
        
        std::vector<std::string> args = {
            AAPT2, "compile",
            "--dir", fs::absolute(res_dir).string(),
            "-o", fs::absolute(flat_dir).string()
        };
        run_func(args, "Full resource compilation failed");

    } 
    // Case 2: Incremental build (Batching specific files)
    else if (!changed_res_files->empty()) {
        std::cout << ">> [RES] Compiling " << changed_res_files->size() 
                  << " changed resources (BATCH)..." << std::endl;

        std::vector<std::string> args = {
            AAPT2, "compile",
            "-o", fs::absolute(flat_dir).string()
        };

        // Resolve absolute paths for the background JVM/Daemon safety
        for (const auto& f : *changed_res_files) {
            args.push_back(fs::absolute(f).string());
        }
        
        run_func(args, "Batch resource compilation failed");
    } 
    // Case 3: No changes detected by the hash checker
    else {
        std::cout << ">> [RES] No resource changes detected." << std::endl;
    }
}

/**
 * (Step ii) Links compiled resources and manifest to produce the base APK.
 * Ported: R.java generation in src_dir and absolute path linkage.
 */
void link_manifest(
    const std::string& AAPT2,
    const fs::path& unsigned_apk,
    const fs::path& android_jar,
    const fs::path& manifest,
    const fs::path& bin_dir,
    const fs::path& src_dir,
    RunFunc run_func,
    bool debug) 
{
    std::cout << ">> [RES] Linking Manifest & Resources " 
              << (debug ? "(DEBUG)" : "") << "..." << std::endl;

    fs::path flat_dir = bin_dir / "flat_res";
    if (!fs::exists(flat_dir) || fs::is_empty(flat_dir)) {
        std::cerr << "!! Error: No compiled flat resources found." << std::endl;
        std::exit(1); // Force exit on critical failure
    }

    // Base link command
    std::vector<std::string> args = {
        AAPT2, "link",
        "-o", fs::absolute(unsigned_apk).string(),
        "-I", fs::absolute(android_jar).string(),
        "--manifest", fs::absolute(manifest).string(),
        "--java", fs::absolute(src_dir).string(),
        "--auto-add-overlay"
    };

    // Gather all .flat files produced in Step i
    for (const auto& entry : fs::directory_iterator(flat_dir)) {
        if (entry.path().extension() == ".flat") {
            args.push_back(fs::absolute(entry.path()).string());
        }
    }

    if (debug) args.push_back("--debug-mode");

    run_func(args, "Manifest linking failed");
}


fs::path obfuscate_resources(
    const std::string& java_bin,
    const fs::path& resguard_jar,
    const fs::path& config_xml,
    const fs::path& in_apk,
    const fs::path& bin_dir,
    RunFunc run_func) 
{
    if (!fs::exists(resguard_jar)) {
        std::cout << ">> [RES] AndResGuard JAR missing. Skipping obfuscation." << std::endl;
        return in_apk;
    }

    std::cout << ">> [RES] Obfuscating Resources (AndResGuard)..." << std::endl;

    fs::path resguard_out = bin_dir / "resguard_out";
    if (fs::exists(resguard_out)) fs::remove_all(resguard_out);

    std::vector<std::string> args = {
        java_bin, "-jar", fs::absolute(resguard_jar).string(),
        fs::absolute(in_apk).string(),
        "-out", fs::absolute(resguard_out).string(),
        "-config", fs::absolute(config_xml).string()
    };

    run_func(args, "AndResGuard optimization failed");

    // Search for the optimized APK in the ResGuard output tree
    for (const auto& entry : fs::recursive_directory_iterator(resguard_out)) {
        if (entry.path().extension() == ".apk") {
            return entry.path();
        }
    }

    std::cerr << "!! AndResGuard output APK not found. Falling back to original." << std::endl;
    return in_apk;
}