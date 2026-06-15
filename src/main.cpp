#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

#ifndef SHARE_PATH
   #define SHARE_PATH "/data/data/com.termux/files/usr/share/mkapk/"
#endif

class JavaWarningFilterBuf : public std::streambuf {
private:
    std::streambuf* orig_buf;
    std::string line_buffer;
    
     int sequence_state = 0;
    std::vector<std::string> buffered_lines;

    const std::vector<std::string> TARGET_SEQUENCE = {
        "WARNING: A terminally deprecated method in java.lang.System has been called",
        "WARNING: System::setSecurityManager has been called by com.mkapk.tools.MkapkTools (file:/data/data/com.termux/files/usr/share/mkapk/mkapk-coordinator.jar)",
        "WARNING: Please consider reporting this to the maintainers of com.mkapk.tools.MkapkTools",
        "WARNING: System::setSecurityManager will be removed in a future release"
    };

    void flush_held_lines() {
        for (const auto& line : buffered_lines) {
            orig_buf->sputn(line.data(), line.size());
            orig_buf->sputc('\n');
        }
        buffered_lines.clear();
        sequence_state = 0;
    }

protected:
    int overflow(int c) override {
        if (c == EOF) return EOF;

        if (c == '\n') {
            // Check if the current line matches the expected step in our 4-line block
            if (sequence_state < 4 && line_buffer == TARGET_SEQUENCE[sequence_state]) {
                buffered_lines.push_back(line_buffer);
                sequence_state++;

                // If we hit exactly 4 consecutive lines matching perfectly, discard them completely
                if (sequence_state == 4) {
                    buffered_lines.clear();
                    sequence_state = 0;
                }
            } else {
                // Sequence broken! This is a real error or an unrelated warning.
                // Store the current line, flush everything we held, and write it out.
                buffered_lines.push_back(line_buffer);
                flush_held_lines();
            }
            line_buffer.clear();
        } else {
            line_buffer.push_back(static_cast<char>(c));
        }
        return c;
    }

    int sync() override {
        if (!line_buffer.empty()) {
            buffered_lines.push_back(line_buffer);
            line_buffer.clear();
        }
        flush_held_lines();
        return orig_buf->pubsync();
    }

public:
    explicit JavaWarningFilterBuf(std::streambuf* original) : orig_buf(original) {}
    
    ~JavaWarningFilterBuf() override {
        sync();
    }
};

void show_help() {
    std::cout << R"(
mkapk - A lightweight Android Build Tool
Usage: mkapk [command] [options]

Commands:
  init          Initialize a new project structure from templates.
  build         Build the project (defaults to an incremental Debug build).
  clean         Remove build artifacts (keeps keystores).
  install <pl>  Unpack and install a signed language plugin package (.pl).
  uninstall <n> Remove a language plugin configuration and its footprint cache.
  help          Show this help menu.

Options:
  -release      Build a production-ready package with optimizations and obfuscation.
  -all          Force a complete rebuild from scratch.
  -no-obs       Disable obfuscation when building in release mode.
  -arch <abi>   Target specific architecture (e.g., aarch64, armv7a, x86_64) or 'universal' / 'u'.
  -ndk-all      Generate split architecture APKs along with a universal standalone package.
    )" << std::endl;
}

bool clean_bin_directory() {
    fs::path bin_path = "bin";

    if (!fs::exists(bin_path)) {
        std::cout << ">> 'bin/' directory does not exist. Nothing to clean." << std::endl;
        return true;
    }

    if (!fs::is_directory(bin_path)) {
        std::cerr << "!! Error: 'bin' exists but is not a directory." << std::endl;
        return false;
    }

    std::cout << ">> Cleaning 'bin/' directory (preserving keystores)..." << std::endl;
    size_t deleted_count = 0;

    try {
        for (const auto& entry : fs::directory_iterator(bin_path)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".jks" || ext == ".keystore") {
                    continue;
                }
            }
            deleted_count += fs::remove_all(entry.path());
        }
        std::cout << "✨ Clean finished successfully. Removed " << deleted_count << " item(s)." << std::endl;
        return true;
    } 
    catch (const fs::filesystem_error& e) {
        std::cerr << "!! Filesystem error during clean: " << e.what() << std::endl;
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Redirect std::cerr safely through our sequential state-machine filter
    std::streambuf* old_cerr_buf = std::cerr.rdbuf();
    JavaWarningFilterBuf filter_buf(old_cerr_buf);
    std::cerr.rdbuf(&filter_buf);

    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        show_help();
        std::cerr.rdbuf(old_cerr_buf);
        return 0;
    }

    std::string command = args[0];

    try {
        if (command == "init") {
            if (MkapkEnv::init_project()) {
                std::cerr.rdbuf(old_cerr_buf);
                return 0;
            }
            std::cerr.rdbuf(old_cerr_buf);
            return 1;
        }

        if (command == "clean") {
            bool success = clean_bin_directory();
            std::cerr.rdbuf(old_cerr_buf);
            return success ? 0 : 1;
        }

        // --- EXTENSIBLE INTERCEPTOR PLUGIN ROUTER CHANNELS ---
        if (command == "install") {
            if (args.size() < 2) {
                std::cerr << "!! Error: Missing target package path token. Usage: mkapk install <plugin.pl>" << std::endl;
                std::cerr.rdbuf(old_cerr_buf);
                return 1;
            }
            bool success = MkapkEnv::install_plugin(args[1]);
            std::cerr.rdbuf(old_cerr_buf);
            return success ? 0 : 1;
        }

        if (command == "uninstall") {
            if (args.size() < 2) {
                std::cerr << "!! Error: Missing target language handle name. Usage: mkapk uninstall <plugin_name>" << std::endl;
                std::cerr.rdbuf(old_cerr_buf);
                return 1;
            }
            bool success = MkapkEnv::uninstall_plugin(args[1]);
            std::cerr.rdbuf(old_cerr_buf);
            return success ? 0 : 1;
        }

        if (command == "build" || (command.size() > 0 && command[0] == '-')) {
            if (command == "build") {
                args.erase(args.begin());
            }

            std::string config_content = MkapkEnv::read_config_file();
            if (config_content.empty()) {
                std::cerr << "!! Error: Configuration file 'config.json' not found." << std::endl;
                std::cerr << "   Run 'mkapk init' to generate a template." << std::endl;
                std::cerr.rdbuf(old_cerr_buf);
                return 1;
            }

            bool is_release = std::find(args.begin(), args.end(), "-release") != args.end();
            bool ndk_all = std::find(args.begin(), args.end(), "-ndk-all") != args.end();
            
            // Look up flag values safely to avoid splitting parameter bindings apart
            std::string arch_target = "";
            auto arch_it = std::find(args.begin(), args.end(), "-arch");
            if (arch_it != args.end() && (arch_it + 1) != args.end()) {
                arch_target = *(arch_it + 1);
            }

            std::cout << ">> Starting " << (is_release ? "Release" : "Debug") << " Build";
            if (ndk_all) {
                std::cout << " [Multi-ABI Output Mode]";
            } else if (!arch_target.empty()) {
                std::cout << " [Target ABI: " << arch_target << "]";
            }
            std::cout << "..." << std::endl;
            
            std::string daemon_classpath = MkapkEnv::get_jni_classpath(config_content);
            start_daemon(daemon_classpath); 
            
            try {
                // Passes unmodified vector array retaining custom architecture options safely
                std::string result = perform_build(args, config_content);
                stop_daemon();

                if (result == "up-to-date") {
                    std::cout << "\n✅ Build finished: Project is already up-to-date." << std::endl;
                } else {
                    std::cout << "\n✨ mkapk: Build finished successfully." << std::endl;
                    std::cout << ">> Output: " << result << std::endl;
                }
            } 
            catch (const std::exception& build_err) {
                stop_daemon();
                std::cerr << "\n!! Build Interrupted: " << build_err.what() << std::endl;
                std::cerr.rdbuf(old_cerr_buf);
                return 1; 
            } 
            catch (...) {
                stop_daemon();
                std::cerr << "\n!! Build Interrupted: A fatal internal error occurred." << std::endl;
                std::cerr.rdbuf(old_cerr_buf);
                return 1;
            }
        } 
        else {
            std::cerr << "!! Unknown command: '" << command << "'. Use 'mkapk help'." << std::endl;
            std::cerr.rdbuf(old_cerr_buf);
            return 1;
        }
    } 
    catch (const std::exception& e) {
        std::cerr << "\n!! mkapk terminal error: " << e.what() << std::endl;
        std::cerr.rdbuf(old_cerr_buf);
        return 1;
    }

    std::cerr.rdbuf(old_cerr_buf);
    return 0;
}