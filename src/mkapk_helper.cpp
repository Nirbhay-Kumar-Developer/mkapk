#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <functional>
#include <poll.h>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>

#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"

static int pipe_to_jvm[2] = {-1, -1};   // C++ writes to JVM
static int pipe_from_jvm[2] = {-1, -1}; // C++ reads from JVM
static int pipe_err_from_jvm[2];
static pid_t daemon_pid = -1;

/**
 * Executes a tool command by sending a delimited string to the Daemon.
 */
void call_java_tool(const std::vector<std::string>& args) {
    if (daemon_pid <= 0) {
        throw std::runtime_error("Daemon connection tracking flag inactive: JVM execution thread terminated.");
    }

    std::stringstream ss;
    for (size_t i = 0; i < args.size(); ++i) {
        ss << args[i] << (i == args.size() - 1 ? "" : "|");
    }
    std::string msg = ss.str() + "\n";

    if (write(pipe_to_jvm[1], msg.c_str(), msg.length()) == -1) {
        throw std::runtime_error("IPC channel tracking broken link: Writing transaction dropped on active JVM pipeline execution.");
    }

    char buffer[4096];
    std::string out_accumulator;
    std::string err_accumulator;
    bool task_completed = false;
    bool in_error_block = false; // State flag to prevent repeating the Error header

    // Setup poll descriptors for multiplexed I/O
    struct pollfd fds[2];
    fds[0].fd = pipe_from_jvm[0];
    fds[0].events = POLLIN;
    fds[1].fd = pipe_err_from_jvm[0];
    fds[1].events = POLLIN;

    while (!task_completed) {
        // Block until at least one pipe has data to read (or an error occurs)
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue; // Safely ignore OS signal interruptions
            throw std::runtime_error("IPC synchronization failure: poll() returned a fatal error.");
        }

        // ====================================================================
        // CHANNEL 1: STANDARD OUTPUT (JVM Protocol & Task Flags)
        // ====================================================================
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(fds[0].fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) {
                throw std::runtime_error("IPC connection dropped out of scope: Daemon closed stdout channel before execution milestone reached.");
            }
            
            buffer[n] = '\0';
            std::string resp(buffer);
            
            size_t done_pos = resp.find("MKAPK_TASK_DONE");
            if (done_pos != std::string::npos) {
                task_completed = true;
                resp.erase(done_pos, 15); 
            }

            out_accumulator += resp;
            size_t newline_pos;
            
            while ((newline_pos = out_accumulator.find('\n')) != std::string::npos) {
                std::string line = out_accumulator.substr(0, newline_pos);
                out_accumulator = out_accumulator.substr(newline_pos + 1);
                
                if (line.empty()) continue;

                // --- INTERCEPT PROTOCOL LOGGING TAGS FROM JAVA DAEMON ---
                if (line.rfind("[ERROR]|", 0) == 0) {
                    std::string clean_line = line.substr(8);
                    
                    if (!in_error_block) {
                        UI::error(clean_line);
                        in_error_block = true;
                    } else {
                        std::lock_guard<std::mutex> lock(UI::get_console_mutex());
                        std::cerr << "         " << clean_line << std::endl;
                    }
                } 
                else if (line.rfind("[WARN]|", 0) == 0) {
                    in_error_block = false; 
                    UI::warn(line.substr(7));
                } 
                else {
                    in_error_block = false; 
                    UI::info("[" + args[0] + " stdout] " + line);
                }
            }
        }
        else if (fds[0].revents & (POLLERR | POLLHUP)) {
             throw std::runtime_error("IPC channel tracking broken link: Daemon stdout pipe disconnected.");
        }

        // ====================================================================
        // CHANNEL 2: STANDARD ERROR (Crash Dumps, Stack Traces, Native Logs)
        // ====================================================================
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(fds[1].fd, buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = '\0';
                err_accumulator += buffer;
                
                size_t newline_pos;
                while ((newline_pos = err_accumulator.find('\n')) != std::string::npos) {
                    std::string line = err_accumulator.substr(0, newline_pos);
                    err_accumulator = err_accumulator.substr(newline_pos + 1);
                    if (!line.empty()) {
                        UI::warn("[JVM STDERR] " + line); 
                    }
                }
            }
        }
    }

    // Flush any trailing fragments remaining inside the STDOUT accumulator
    if (!out_accumulator.empty() && out_accumulator.find_first_not_of(" \t\r\n") != std::string::npos) {
        if (out_accumulator.rfind("[ERROR]|", 0) == 0) {
            std::string clean_line = out_accumulator.substr(8);
            if (!in_error_block) {
                UI::error(clean_line);
            } else {
                std::lock_guard<std::mutex> lock(UI::get_console_mutex());
                std::cerr << "         " << clean_line << std::endl;
            }
        } 
        else if (out_accumulator.rfind("[WARN]|", 0) == 0) {
            UI::warn(out_accumulator.substr(7));
        } 
        else {
            UI::info("[" + args[0] + " stdout] " + out_accumulator);
        }
    }

    // Flush any trailing fragments remaining inside the STDERR accumulator
    if (!err_accumulator.empty() && err_accumulator.find_first_not_of(" \t\r\n") != std::string::npos) {
        UI::warn("[JVM STDERR] " + err_accumulator);
    }
}


/**
 * Smart Command Runner: Routes tools to either the persistent Daemon or native Fork/Exec.
 */
void smart_run(const std::vector<std::string>& args, const std::string& err_msg) {
    if (args.empty()) return;

    // Lightweight path filename resolution without full std::filesystem compilation overhead
    std::string tool_path = args[0];
    size_t last_slash = tool_path.find_last_of('/');
    std::string tool_name = (last_slash == std::string::npos) ? tool_path : tool_path.substr(last_slash + 1);

    bool use_daemon = (tool_name == "d8" || tool_name == "r8" || 
                       tool_name == "javac" || tool_name == "resguard" ||
                       tool_name == "apksigner" || tool_name == "kotlinc");

    if (use_daemon) {
        std::vector<std::string> daemon_args = args;
        daemon_args[0] = tool_name; 
        call_java_tool(daemon_args);
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            std::vector<char*> c_args;
            for (const auto& arg : args) c_args.push_back(const_cast<char*>(arg.c_str()));
            c_args.push_back(nullptr);
            execvp(c_args[0], c_args.data());
            _exit(127);
        } else {
            int status;
            waitpid(pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                throw std::runtime_error("External tool runtime execution anomaly flags raised inside (" + tool_name + "). Message mapping reference context: " + err_msg + " (Exit code: " + std::to_string(exit_code) + ")");
            }
        }
    }
}