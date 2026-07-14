#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"

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
            if (sequence_state < 4 && line_buffer == TARGET_SEQUENCE[sequence_state]) {
                buffered_lines.push_back(line_buffer);
                sequence_state++;

                if (sequence_state == 4) {
                    buffered_lines.clear();
                    sequence_state = 0;
                }
            } else {
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
    std::lock_guard<std::mutex> lock(UI::get_console_mutex());
    std::cout << UI::BOLD << "mkapk" << UI::RESET << " - A lightweight Android Build Tool\n"
              << UI::CYAN << "Usage:" << UI::RESET << " mkapk [command] [options]\n\n"
              << UI::CYAN << "Commands:\n" << UI::RESET
              << "  init          Initialize a new project structure from templates.\n"
              << "  build         Build the project (defaults to an incremental Debug build).\n"
              << "  clean         Remove build artifacts.\n"
              << "                  -d  Wipe debug compilation cache (build/debug)\n"
              << "                  -r  Wipe release optimization cache (build/release)\n"
              << "  install <pl>  Unpack and install a signed language plugin package (.pl).\n"
              << "  uninstall <n> Remove a language plugin configuration and its footprint cache.\n"
              << "  help          Show this help menu.\n\n"
              << UI::CYAN << "Options:\n" << UI::RESET
              << "  -release      Build a production-ready package with optimizations and obfuscation.\n"
              << "  -all          Force a complete rebuild from scratch.\n"
              << "  -no-obs       Disable obfuscation when building in release mode.\n"
              << "  -arch <abi>   Target specific architecture (e.g., aarch64, armv7a, x86_64) or 'universal' / 'u'.\n"
              << "  -ndk-all      Generate split architecture APKs along with a universal standalone package.\n"
              << std::endl;
}

bool clean_build_directory(bool target_release, bool target_debug) {
    size_t deleted_count = 0;
    
    // If no flags are passed explicitly, clean both by default for safety
    if (!target_release && !target_debug) {
        target_release = true;
        target_debug = true;
    }

    auto clear_path = [&](const fs::path& target) {
        if (fs::exists(target)) {
            if (fs::is_directory(target)) {
                try {
                    deleted_count += fs::remove_all(target);
                    UI::success("Cleaned target workspace tree segment: " + target.string());
                } catch (const fs::filesystem_error& e) {
                    UI::error("Filesystem sweep error targeting: " + target.string(), e.what());
                }
            } else {
                UI::error("Target path mismatch (not a directory): " + target.string());
            }
        }
    };

    UI::info("Starting targeted operational workspace cleanup pipeline...");
    
    if (target_debug) {
        clear_path(fs::absolute("build/debug"));
    }
    if (target_release) {
        clear_path(fs::absolute("build/release"));
    }

    UI::success("Wipe operation finished. Removed " + std::to_string(deleted_count) + " layout item(s).");
    return true;
}

int main(int argc, char* argv[]) {
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
            bool success = MkapkEnv::init_project();
            std::cerr.rdbuf(old_cerr_buf);
            return success ? 0 : 1;
        }

        if (command == "clean") {
            bool target_release = std::find(args.begin(), args.end(), "-r") != args.end();
            bool target_debug = std::find(args.begin(), args.end(), "-d") != args.end();
            
            bool success = clean_build_directory(target_release, target_debug);
            std::cerr.rdbuf(old_cerr_buf);
            return success ? 0 : 1;
        }

        if (command == "install") {
            if (args.size() < 2) {
                UI::error("Missing target package path token. Usage: mkapk install <plugin.pl>");
                std::cerr.rdbuf(old_cerr_buf);
                return 1;
            }
            bool success = MkapkEnv::install_plugin(args[1]);
            std::cerr.rdbuf(old_cerr_buf);
            return success ? 0 : 1;
        }

        if (command == "uninstall") {
            if (args.size() < 2) {
                UI::error("Missing target language handle name. Usage: mkapk uninstall <plugin_name>");
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

            // LOAD CONFIGURATION STRUCT
            MkapkConfig config = MkapkEnv::load_config();
            if (!config.is_valid) {
                UI::error(UI::Msg::CONFIG_MISSING);
                std::cerr.rdbuf(old_cerr_buf);
                return 1;
            }

            bool is_release = std::find(args.begin(), args.end(), "-release") != args.end();
            bool ndk_all = std::find(args.begin(), args.end(), "-ndk-all") != args.end();
            
            std::string arch_target = "";
            auto arch_it = std::find(args.begin(), args.end(), "-arch");
            if (arch_it != args.end() && (arch_it + 1) != args.end()) {
                arch_target = *(arch_it + 1);
            }

            // Create context meta details variant layout dynamically
            std::string build_details = is_release ? "Release Mode" : "Debug Mode";
            if (ndk_all) {
                build_details += ", Multi-ABI Output";
            } else if (!arch_target.empty()) {
                build_details += ", ABI: " + arch_target;
            }

            UI::stage(UI::Msg::BUILD_START, build_details);
            
            // PASS CONFIG STRUCT TO HELPERS
            std::string daemon_classpath = MkapkEnv::get_jni_classpath(config);
            start_daemon(daemon_classpath); 
            
            try {
                // PASS CONFIG STRUCT TO BUILD PIPELINE
                std::string result = perform_build(args, config);
                stop_daemon();

                if (result == "up-to-date") {
                    UI::success(UI::Msg::BUILD_UP_TO_DATE, "");
                } else {
                    UI::success(UI::Msg::BUILD_SUCCESS);
                    UI::info("Artifact Location: " + result);
                }
            } 
            catch (const std::exception& build_err) {
                stop_daemon();
                UI::error("Build pipeline run interrupted.", build_err.what());
                std::cerr.rdbuf(old_cerr_buf);
                return 1; 
            } 
            catch (...) {
                stop_daemon();
                UI::error(UI::Msg::FATAL_INTERNAL);
                std::cerr.rdbuf(old_cerr_buf);
                return 1;
            }
        } 
        else {
            UI::error("Unknown execution instruction command sequence: '" + command + "'. Use 'mkapk help'.");
            std::cerr.rdbuf(old_cerr_buf);
            return 1;
        }
    } 
    catch (const std::exception& e) {
        UI::error("Fatal system failure boundary level broken.", e.what());
        std::cerr.rdbuf(old_cerr_buf);
        return 1;
    }

    std::cerr.rdbuf(old_cerr_buf);
    return 0;
}