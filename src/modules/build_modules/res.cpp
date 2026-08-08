#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<Result<void>(const std::vector<std::string>&, const std::string&)>;

Result<void> compile_resources(
    const std::string& AAPT2,
    const fs::path& res_dir,
    const fs::path& bin_dir,
    RunFunc run_func,
    const std::vector<fs::path>* changed_res_files)
{
    fs::path flat_dir = bin_dir / "flat_res";
    fs::create_directories(flat_dir);

    // --- PHASE 1: PROCESS CORE APPLICATION RESOURCES ---
    if (fs::exists(res_dir)) {
        if (changed_res_files == nullptr) {
            UI::stage(UI::Msg::RES_STAGE, "Compiling all localized targets via aapt2");
            
            std::vector<std::string> args = {
                AAPT2, "compile",
                "--dir", fs::absolute(res_dir).string(),
                "-o", fs::absolute(flat_dir).string()
            };
            run_func(args, "Full resource compilation failed");
        } 
        else if (!changed_res_files->empty()) {
            UI::stage(UI::Msg::RES_STAGE, "Batch compilation pass for " + std::to_string(changed_res_files->size()) + " files");

            std::vector<std::string> args = {
                AAPT2, "compile",
                "-o", fs::absolute(flat_dir).string()
            };

            for (const auto& f : *changed_res_files) {
                args.push_back(fs::absolute(f).string());
            }
            
            run_func(args, "Batch resource compilation failed");
        } 
        else {
            UI::info("No core resource modifications tracked by change engine.");
        }
    } else {
        UI::warn("Primary resource directory not located at: " + res_dir.string());
    }
    
    return Result<void>::success();
}

Result<void> link_manifest(
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

    fs::path gen_dir = bin_dir / "gen";
    fs::create_directories(gen_dir);

    std::vector<std::string> args = {
        AAPT2, "link",
        "-o", fs::absolute(unsigned_apk).string(),
        "-I", fs::absolute(android_jar).string(),
        "--manifest", fs::absolute(manifest).string(),
        "--java", fs::absolute(gen_dir).string(), 
        "--auto-add-overlay"
    };

    if (debug) {
        args.push_back("--debug-mode");
    }

    auto res = run_func(args, "Manifest asset linking generation dropped errors.");
    if (res.is_err()) return res;

    return Result<void>::success();
}

fs::path obfuscate_resources(
    const std::string& RESGUARD_TOOL,
    const fs::path& in_apk,
    const fs::path& build_dir,
    RunFunc run_func) 
{
    UI::stage("Obfuscator", "Executing asset minification routines via AndResGuard");

    fs::path resguard_out = build_dir / "resguard_out";
    if (fs::exists(resguard_out)) fs::remove_all(resguard_out);

    fs::path config_xml = fs::current_path() / "andresguard.xml";

    std::vector<std::string> args = {
        RESGUARD_TOOL,
        fs::absolute(in_apk).string(),
        "-out", fs::absolute(resguard_out).string()
    };

    if (fs::exists(config_xml)) {
        args.push_back("-config");
        args.push_back(fs::absolute(config_xml).string());
    }

    run_func(args, "AndResGuard resource obfuscation failed.");

    if (fs::exists(resguard_out)) {
        for (const auto& entry : fs::recursive_directory_iterator(resguard_out)) {
            if (entry.path().extension() == ".apk") {
                return entry.path();
            }
        }
    }

    UI::warn("AndResGuard execution completed but no output APK was found. Reverting to base package.");
    return in_apk;
}
