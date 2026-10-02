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
            UI::stage(UI::Msg::STAGE_RES, "Compiling resources");
            
            std::vector<std::string> args = {
                AAPT2, "compile",
                "--dir", fs::absolute(res_dir).string(),
                "-o", fs::absolute(flat_dir).string()
            };
            return run_func(args, "Full resource compilation failed");
        } 
        else if (!changed_res_files->empty()) {
            UI::stage(UI::Msg::STAGE_RES, "Compiling " + std::to_string(changed_res_files->size()) + " resource files");

            std::vector<std::string> args = {
                AAPT2, "compile",
                "-o", fs::absolute(flat_dir).string()
            };

            for (const auto& f : *changed_res_files) {
                args.push_back(fs::absolute(f).string());
            }
            
            return run_func(args, "Batch resource compilation failed");
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
    UI::stage(UI::Msg::STAGE_RES_LINK);

    if (!fs::exists(manifest)) {
        return Result<void>::error(UI::Msg::ERR_MANIFEST_MISSING);
    }
    if (!fs::exists(android_jar)) {
        return Result<void>::error(UI::Msg::ERR_SDK_MISSING);
    }

    // --- DECLARE FLAT_DIR HERE ---
    fs::path flat_dir = bin_dir / "flat_res";
    if (!fs::exists(flat_dir) || fs::is_empty(flat_dir)) {
        return Result<void>::error(UI::Msg::FATAL_INTERNAL);
    }

    fs::path gen_dir = bin_dir / "gen";
    fs::create_directories(gen_dir);

    fs::path aapt_proguard_rules = bin_dir / "aapt_rules.pro";
    fs::path r_txt_symbols = bin_dir / "R.txt";

    std::vector<std::string> args = {
        AAPT2, "link",
        "-o", fs::absolute(unsigned_apk).string(),
        "-I", fs::absolute(android_jar).string(),
        "--manifest", fs::absolute(manifest).string(),
        "--java", fs::absolute(gen_dir).string(), 
        "--auto-add-overlay",
        "--proguard", fs::absolute(aapt_proguard_rules).string(),
        "--output-text-symbols", fs::absolute(r_txt_symbols).string()
    };

    if (debug) {
        args.push_back("--debug-mode");
    } else {
        args.push_back("--enable-sparse-encoding");
    }

    // Pass all compiled .flat files
    for (const auto& entry : fs::recursive_directory_iterator(flat_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".flat") {
            args.push_back(fs::absolute(entry.path()).string());
        }
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
    UI::stage(UI::Msg::STAGE_OBFUSCATE, UI::Msg::OP_OBFUSCATING);
    
    fs::path resguard_out = build_dir / "resguard_out";
    if (fs::exists(resguard_out)) fs::remove_all(resguard_out);
    fs::create_directories(resguard_out);

    fs::path config_xml = fs::current_path() / "andresguard-config.xml";
    if (!fs::exists(config_xml)) {
        config_xml = fs::current_path() / "andresguard.xml";
    }

    std::vector<std::string> args = {
        RESGUARD_TOOL,
        fs::absolute(in_apk).string(),
        "-out", fs::absolute(resguard_out).string()
    };

    if (fs::exists(config_xml)) {
        args.push_back("-config");
        args.push_back(fs::absolute(config_xml).string());
    }

    // Incremental resource mapping reuse to maintain stable IDs across builds
    fs::path prev_mapping = build_dir / "resource_mapping.txt";
    if (fs::exists(prev_mapping)) {
        args.push_back("-mapping");
        args.push_back(fs::absolute(prev_mapping).string());
    }

    auto res = run_func(args, "AndResGuard resource obfuscation failed.");
    if (res.is_err()) {
        UI::warn("AndResGuard returned an error: " + res.get_error());
        return in_apk;
    }

    // Cache the mapping file for the next build pass
    fs::path generated_mapping = resguard_out / "resource_mapping.txt";
    if (fs::exists(generated_mapping)) {
        fs::copy_file(generated_mapping, prev_mapping, fs::copy_options::overwrite_existing);
    }

    // Discover the valid output APK produced by AndResGuard
    if (fs::exists(resguard_out)) {
        fs::path candidate = "";
        for (const auto& entry : fs::recursive_directory_iterator(resguard_out)) {
            if (entry.path().extension() == ".apk") {
                std::string fname = entry.path().filename().string();
                // Select unsigned or 7zip repackaged container to forward to zipalign/apksigner
                if (fname.find("_unsigned") != std::string::npos || fname.find("_7zip") != std::string::npos) {
                    return entry.path();
                }
                candidate = entry.path();
            }
        }
        if (!candidate.empty()) return candidate;
    }

    UI::warn("AndResGuard completed but output APK not found. Reverting to base package.");
    return in_apk;
}
