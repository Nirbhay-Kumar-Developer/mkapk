#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <set>
#include <fstream>
#include <sstream>
#include <map>
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <spawn.h>

namespace fs = std::filesystem;

extern char** environ;

using RunFunc = std::function<Result<void>(const std::vector<std::string>&, const std::string&)>;

Result<void> compile_resources(
    const std::string& AAPT2,
    const fs::path& res_dir,
    const fs::path& bin_dir,
    RunFunc run_func,
    const std::vector<fs::path>* changed_res_files,
    const std::vector<fs::path>& extra_dependency_res_dirs)
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
            auto res = run_func(args, "Full resource compilation failed");
            if (res.is_err()) return res;
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

            auto res = run_func(args, "Batch resource compilation failed");
            if (res.is_err()) return res;
        }
    } else {
        UI::warn("Primary resource directory not located at: " + res_dir.string());
    }

    // --- PHASE 2: PROCESS EXTRA AAR LIBRARY DEPENDENCY RESOURCE TREES ---
    if (!extra_dependency_res_dirs.empty()) {
        for (const auto& extra_res : extra_dependency_res_dirs) {
            if (fs::exists(extra_res) && !fs::is_empty(extra_res)) {

                std::string lib_name = extra_res.parent_path().parent_path().filename().string();
                std::string lib_version = extra_res.parent_path().filename().string();

                fs::path lib_out_arc = flat_dir / (lib_name + "_" + lib_version + ".flata");

                if (!fs::exists(lib_out_arc)) {
                    std::vector<std::string> extra_args = {
                        AAPT2, "compile",
                        "--dir", fs::absolute(extra_res).string(),
                        "-o", fs::absolute(lib_out_arc).string()
                    };

                    UI::info("[+] Compiling library resources: " + lib_name);
                    auto res = run_func(extra_args, "Failed compilation of external dependency resource directory tree: " + extra_res.string());
                    if (res.is_err()) return res;
                }
            }
        }
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

    // 1. Discover primary application package
    std::string primary_package = "";
    if (fs::exists(manifest)) {
        std::ifstream mf(manifest);
        std::string line;
        while (std::getline(mf, line)) {
            size_t pkg_pos = line.find("package=\"");
            if (pkg_pos != std::string::npos) {
                size_t start = pkg_pos + 9;
                size_t end = line.find("\"", start);
                if (end != std::string::npos) {
                    primary_package = line.substr(start, end - start);
                }
                break;
            }
        }
    }

    // 2. Discover library packages and their corresponding R.txt files
    std::set<std::string> extra_packages;
    std::vector<std::string> library_r_txt_files;

    const char* prefix_env = std::getenv("PREFIX");
    fs::path cache_root = prefix_env
        ? fs::path(prefix_env) / "var/lib/mkapk/lib"
        : fs::path("/data/data/com.termux/files/usr/var/lib/mkapk/lib");

    if (fs::exists(cache_root)) {
        for (const auto& entry : fs::recursive_directory_iterator(cache_root)) {
            if (entry.is_regular_file() && entry.path().filename() == "AndroidManifest.xml") {
                fs::path lib_dir = entry.path().parent_path();

                std::ifstream lib_mf(entry.path());
                std::string line;
                std::string pkg = "";
                while (std::getline(lib_mf, line)) {
                    size_t pkg_pos = line.find("package=\"");
                    if (pkg_pos != std::string::npos) {
                        size_t start = pkg_pos + 9;
                        size_t end = line.find("\"", start);
                        if (end != std::string::npos) {
                            pkg = line.substr(start, end - start);
                        }
                        break;
                    }
                }

                if (!pkg.empty() && pkg != primary_package) {
                    extra_packages.insert(pkg);
                }

                fs::path lib_r_txt = lib_dir / "R.txt";
                if (fs::exists(lib_r_txt) && fs::file_size(lib_r_txt) > 0) {
                    library_r_txt_files.push_back(fs::absolute(lib_r_txt).string());
                }
            }
        }
    }

    // 3. Inject --extra-packages
    if (!extra_packages.empty()) {
        std::string extra_pkgs_str = "";
        for (auto it = extra_packages.begin(); it != extra_packages.end(); ++it) {
            extra_pkgs_str += *it;
            if (std::next(it) != extra_packages.end()) {
                extra_pkgs_str += ":";
            }
        }
        args.push_back("--extra-packages");
        args.push_back(extra_pkgs_str);
    }

    if (debug) {
        args.push_back("--debug-mode");
    } else {
        args.push_back("--enable-sparse-encoding");
    }

    // 4. Collect project resource files and compiled library archives (.flata)
    std::vector<std::string> library_archives;
    for (const auto& entry : fs::directory_iterator(flat_dir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            if (ext == ".flat") {
                args.push_back(fs::absolute(entry.path()).string());
            } else if (ext == ".flata") {
                library_archives.push_back(fs::absolute(entry.path()).string());
            }
        }
    }

    std::sort(library_archives.begin(), library_archives.end());
    std::sort(library_r_txt_files.begin(), library_r_txt_files.end());

    // Inject compiled library resources via -R
    for (const auto& arc : library_archives) {
        args.push_back("-R");
        args.push_back(arc);
    }

    // 5. Execution Wrapper: Direct filtering of aapt2 stderr noise without global system alterations
    auto filtered_aapt2_run = [](const std::vector<std::string>& cmd_args) -> Result<void> {
        int err_pipe[2];
        if (pipe(err_pipe) == -1) {
            return Result<void>::error("Failed to allocate pipe descriptors for AAPT2 link execution.");
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, err_pipe[0]);

        std::vector<char*> c_args;
        c_args.reserve(cmd_args.size() + 1);
        for (const auto& arg : cmd_args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        pid_t pid;
        int spawn_status = posix_spawnp(&pid, c_args[0], &actions, nullptr, c_args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(err_pipe[1]);

        if (spawn_status != 0) {
            close(err_pipe[0]);
            return Result<void>::error("posix_spawnp failed to launch AAPT2 binary.");
        }

        // Buffer and filter stream directly
        std::string line_buffer;
        char chunk[2048];
        ssize_t bytes_read;

        while ((bytes_read = read(err_pipe[0], chunk, sizeof(chunk) - 1)) > 0) {
            chunk[bytes_read] = '\0';
            line_buffer += chunk;

            size_t newline_idx;
            while ((newline_idx = line_buffer.find('\n')) != std::string::npos) {
                std::string line = line_buffer.substr(0, newline_idx);
                line_buffer.erase(0, newline_idx + 1);

                // Silences the internal bionic liblog package ID table dumps
                if (line.find("No package ID") != std::string::npos ||
                    line.find("E aapt2   :") != std::string::npos) {
                    continue;
                }

                // Legitimate linker errors (syntax errors, missing themes, broken manifests)
                if (!line.empty()) {
                    std::cerr << line << '\n';
                }
            }
        }
        close(err_pipe[0]);

        if (!line_buffer.empty()) {
            if (line_buffer.find("No package ID") == std::string::npos &&
                line_buffer.find("E aapt2   :") == std::string::npos) {
                std::cerr << line_buffer << '\n';
            }
        }
        std::cerr.flush();

        int wait_status;
        if (waitpid(pid, &wait_status, 0) != -1) {
            if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
                return Result<void>::success();
            }
        }
        return Result<void>::error("AAPT2 link processing failed.");
    };

    auto res = filtered_aapt2_run(args);
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
