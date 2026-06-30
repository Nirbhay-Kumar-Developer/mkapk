#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <algorithm>
#include <functional>
#include <future>
#include <mutex>
#include <cstdlib>
#include "mkapk_helpers.hpp"

namespace fs = std::filesystem;

using RunFunc = std::function<void(const std::vector<std::string>&, const std::string&)>;

// Ported ARCH_MAP for standard Android NDK triplets
static std::map<std::string, std::string> ARCH_MAP = {
    {"aarch64-linux-android", "arm64-v8a"},
    {"armv7a-linux-androideabi", "armeabi-v7a"},
    {"i686-linux-android", "x86"},
    {"x86_64-linux-android", "x86_64"}
};

// Global synchronization mutex lock to prevent concurrent workers from scrambling stdout logs
static std::mutex console_mutex;

/**
 * (Step 0) Compiles user-defined target structures using parallel async workers.
 * UPDATED: Enforces persistent linking caches, resolves dynamic environment prefixes,
 * and secures async lambda memory scopes.
 */
bool compile_native(
    const std::string& NDK_BIN, 
    const fs::path& src_dir,
    const fs::path& bin_dir,
    const std::vector<std::string>& arch_list,
    const std::string& target_api,
    RunFunc run_func,
    const std::vector<fs::path>& changed_files,
    const std::vector<NativeTargetConfig>& native_targets) 
{
    if (native_targets.empty()) {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << ">> [NATIVE] No native compile targets specified. Skipping." << std::endl;
        return false;
    }

    // --- TERMUX NATIVE SYSTEM HEADER INJECTION ---
    std::vector<std::string> includes;
    const char* prefix_env = std::getenv("PREFIX");
    fs::path termux_usr_include = prefix_env ? fs::path(prefix_env) / "include" : "/data/data/com.termux/files/usr/include";
    
    if (fs::exists(termux_usr_include)) {
        includes.push_back("-I" + termux_usr_include.string());
    }

    std::vector<fs::path> include_paths = {src_dir, src_dir / "include", src_dir / "jni"};
    for (const auto& p : include_paths) {
        if (fs::exists(p)) includes.push_back("-I" + fs::absolute(p).string());
    }

    // Container to hold handles for asynchronous thread workers
    std::vector<std::future<void>> compile_workers;

    // --- LAUNCH TARGET-AND-ARCHITECTURE COMPILATION MATRIX IN PARALLEL ---
    for (const auto& target : native_targets) {
        
        // Filter down target sources to compute what needs an active build pass
        std::vector<fs::path> target_files_to_compile;
        bool target_has_cpp = false;

        for (const auto& src_str : target.sources) {
            fs::path src_path = fs::absolute(src_str);
            if (!fs::exists(src_path)) continue;

            std::string ext = src_path.extension().string();
            if (ext == ".cpp" || ext == ".cc" || ext == ".hpp") {
                target_has_cpp = true;
            }

            // Incremental change constraint checker logic
            if (changed_files.empty() || std::find(changed_files.begin(), changed_files.end(), src_path) != changed_files.end()) {
                target_files_to_compile.push_back(src_path);
            }
        }

        // Skip compiling this specific block completely if no files changed and it's an incremental build pass
        if (target_files_to_compile.empty() && !changed_files.empty()) {
            continue;
        }

        for (const std::string& arch : arch_list) {
            // SECURED: Capturing entirely by value [=] guarantees memory safety for detached threads
            // preventing dangling references if the stack unwinds early.
            compile_workers.push_back(std::async(std::launch::async, [=]() {
                std::string apk_lib_dir = ARCH_MAP.count(arch) ? ARCH_MAP[arch] : "unknown";
                if (apk_lib_dir == "unknown") return;

                std::string compiler_c = "clang";
                std::string compiler_cpp = "clang++";

                // Create completely isolated object caches named specifically after this library target task
                fs::path obj_cache = bin_dir / "native_objs" / target.name / apk_lib_dir;
                fs::path lib_out = bin_dir / "lib" / apk_lib_dir;
                fs::create_directories(obj_cache);
                fs::create_directories(lib_out);

                bool objects_updated = false;

                // 1. Isolated Incremental Compilation Phase (.c/.cpp -> .o)
                for (const auto& src_file : target_files_to_compile) {
                    std::string ext = src_file.extension().string();
                    std::string active_compiler = (ext == ".cpp" || ext == ".cc" || ext == ".hpp") ? compiler_cpp : compiler_c;

                    // Preserve internal folder tree matching to prevent flat object clobbering collisions
                    fs::path rel_path = fs::relative(src_file, src_dir);
                    fs::path obj_file = obj_cache / rel_path.replace_extension(".o");
                    fs::create_directories(obj_file.parent_path());

                    {
                        std::lock_guard<std::mutex> lock(console_mutex);
                        std::cout << "   [" << apk_lib_dir << " - CC] " << target.name << " <= " << src_file.filename().string() << std::endl;
                    }
                    
                    std::vector<std::string> cc_args = {
                        active_compiler, "-c", src_file.string(),
                        "-o", fs::absolute(obj_file).string(),
                        "-fPIC", "-O3", "-Wall",
                        "-target", arch + target_api
                    };

                    cc_args.insert(cc_args.end(), includes.begin(), includes.end());
                    cc_args.insert(cc_args.end(), target.extra_flags.begin(), target.extra_flags.end());

                    run_func(cc_args, "Compilation failed for [" + target.name + "] on ABI [" + apk_lib_dir + "] for file: " + src_file.filename().string());
                    
                    // Track that compilation actually occurred in this pass
                    objects_updated = true;
                }

                // 2. Isolated Target Object Discovery
                std::vector<std::string> obj_list;
                if (fs::exists(obj_cache)) {
                    for (const auto& entry : fs::recursive_directory_iterator(obj_cache)) {
                        if (entry.is_regular_file() && entry.path().extension() == ".o") {
                            obj_list.push_back(fs::absolute(entry.path()).string());
                        }
                    }
                }

                fs::path output_so = lib_out / ("lib" + target.name + ".so");

                // 3. Isolated Linking Phase (.o -> target-specific .so)
                // OPTIMIZED: Only link if objects were recompiled OR if the .so does not exist.
                if (!obj_list.empty() && (objects_updated || !fs::exists(output_so))) {
                    
                    {
                        std::lock_guard<std::mutex> lock(console_mutex);
                        std::cout << "   [" << apk_lib_dir << " - LD] lib" << target.name << ".so" << std::endl;
                    }
                    
                    std::vector<std::string> ld_args = {
                        compiler_cpp, "-shared",
                        "-target", arch + target_api,
                        "-o", fs::absolute(output_so).string()
                    };
                    ld_args.insert(ld_args.end(), obj_list.begin(), obj_list.end());

                    if (target_has_cpp) {
                        ld_args.push_back("-lc++_shared");
                    }
                    
                    ld_args.insert(ld_args.end(), target.extra_flags.begin(), target.extra_flags.end());

                    run_func(ld_args, "Linking failed for custom binary target configuration module: lib" + target.name + " (" + apk_lib_dir + ")");
                }
            }));
        }
    }

    // Synchronize barrier: Block main execution thread until every single parallel ABI target worker finishes safely
    for (auto& worker : compile_workers) {
        worker.get();
    }

    return true;
}