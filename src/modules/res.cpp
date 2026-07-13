#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"

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
        UI::error("Resource directory not found context check dropped matching path", res_dir.string());
        return;
    }

    // Case 1: First build or force-all rebuild (changed_res_files is nullptr)
    if (changed_res_files == nullptr) {
        UI::stage(UI::Msg::RES_STAGE, "Compiling all localized targets via aapt2");
        
        std::vector<std::string> args = {
            AAPT2, "compile",
            "--dir", fs::absolute(res_dir).string(),
            "-o", fs::absolute(flat_dir).string()
        };
        run_func(args, "Full resource compilation failed");

    } 
    // Case 2: Incremental build (Batching specific files)
    else if (!changed_res_files->empty()) {
        UI::stage(UI::Msg::RES_STAGE, "Batch compilation pass for " + std::to_string(changed_res_files->size()) + " files");

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
        UI::info("No resource modifications tracked by change engine.");
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
    UI::stage("Resource Linker", debug ? "Assembling development variant (DEBUG)" : "Assembling production variant");

    // 1. Dependency Existence Checks (Robustness)
    // Prevents AAPT2 from executing and returning obscure backend errors if core files are missing.
    if (!fs::exists(manifest)) {
        throw std::runtime_error("Manifest missing: Cannot link resources without a valid AndroidManifest.xml at " + manifest.string());
    }
    if (!fs::exists(android_jar)) {
        throw std::runtime_error("SDK missing: android.jar not found at " + android_jar.string());
    }

    fs::path flat_dir = bin_dir / "flat_res";
    if (!fs::exists(flat_dir) || fs::is_empty(flat_dir)) {
        throw std::runtime_error("Compilation path context empty: No verified intermediate .flat asset data ready for link passes.");
    }

    // 2. Generation Directory Fix (Bug Patch)
    // Ensures R.java is output to the bin directory where the Java compiler expects it, 
    // rather than polluting the user's source tree.
    fs::path gen_dir = bin_dir / "gen";
    fs::create_directories(gen_dir);

    // 3. Command Construction
    std::vector<std::string> args = {
        AAPT2, "link",
        "-o", fs::absolute(unsigned_apk).string(),
        "-I", fs::absolute(android_jar).string(),
        "--manifest", fs::absolute(manifest).string(),
        "--java", fs::absolute(gen_dir).string(), // Patched: Targets bin_dir/gen
        "--auto-add-overlay"
    };

    // 4. Secure File Gathering
    // Added is_regular_file() check to prevent AAPT2 from choking on rogue directories.
    for (const auto& entry : fs::directory_iterator(flat_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".flat") {
            args.push_back(fs::absolute(entry.path()).string());
        }
    }

    if (debug) {
        args.push_back("--debug-mode");
    }

    run_func(args, "Manifest asset linking generation dropped errors.");
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
        UI::info("AndResGuard tool signature missing inside current profile. Skipping footprint shrinking optimizations.");
        return in_apk;
    }

    UI::stage("Obfuscator", "Executing asset minification routines via AndResGuard");

    fs::path resguard_out = bin_dir / "resguard_out";
    if (fs::exists(resguard_out)) fs::remove_all(resguard_out);

    std::vector<std::string> args = {
        java_bin, "-jar", fs::absolute(resguard_jar).string(),
        fs::absolute(in_apk).string(),
        "-out", fs::absolute(resguard_out).string(),
        "-config", fs::absolute(config_xml).string()
    };

    run_func(args, "AndResGuard alignment package translation failure.");

    // Search for the optimized APK in the ResGuard output tree
    for (const auto& entry : fs::recursive_directory_iterator(resguard_out)) {
        if (entry.path().extension() == ".apk") {
            return entry.path();
        }
    }

    UI::warn("AndResGuard process finished execution but target path container map returned empty. Reverting to base package.");
    return in_apk;
}