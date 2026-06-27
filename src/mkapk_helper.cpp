#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>
#include <sstream>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include <poll.h>
#include <cerrno>

namespace fs = std::filesystem;

/**
 * SECTION: DAEMON STATE MANAGEMENT
 */
static int pipe_to_jvm[2] = {-1, -1};   // C++ writes to JVM
static int pipe_from_jvm[2] = {-1, -1}; // C++ reads from JVM
static int pipe_err_from_jvm[2];
static pid_t daemon_pid = -1;

/**
 * Permanently initializes the Java Daemon as a subprocess.
 * Replaces JNI_CreateJavaVM to bypass Android memory tagging issues.
 */
void start_daemon(const std::string& classpath) {
    if (pipe(pipe_to_jvm) == -1 || pipe(pipe_from_jvm) == -1 || pipe(pipe_err_from_jvm) == -1) {
        throw std::runtime_error("System resource allocation failure: Failed to create IPC pipes.");
    }

    daemon_pid = fork();

    if (daemon_pid == 0) { // Child Process: The JVM
        dup2(pipe_to_jvm[0], STDIN_FILENO);
        dup2(pipe_from_jvm[1], STDOUT_FILENO);
        dup2(pipe_err_from_jvm[1], STDERR_FILENO);

        close(pipe_to_jvm[1]);
        close(pipe_from_jvm[0]);
        close(pipe_err_from_jvm[0]);

        execlp("java", "java", 
               "-Djava.security.manager=allow",
               "-cp", classpath.c_str(), 
               "com.mkapk.tools.MkapkTools", 
               nullptr);
        
        _exit(127);
    } 
    else if (daemon_pid > 0) { // Parent Process: mkapk Native
        close(pipe_to_jvm[0]);
        close(pipe_from_jvm[1]);
        close(pipe_err_from_jvm[1]);

        std::string start_cmd = "START_DAEMON\n";
        if (write(pipe_to_jvm[1], start_cmd.c_str(), start_cmd.length()) == -1) {
             throw std::runtime_error("System write synchronization failed on JVM entry command.");
        }

        // --- UPDATED ERROR SQUELCH FILTER SEQUENCES ---
        const std::vector<std::string> TARGET_SEQUENCE = {
            "WARNING: A terminally deprecated method in java.lang.System has been called",
            "WARNING: System::setSecurityManager has been called by com.mkapk.tools.MkapkTools (file:/data/data/com.termux/files/usr/share/mkapk/mkapk-coordinator.jar)",
            "WARNING: Please consider reporting this to the maintainers of com.mkapk.tools.MkapkTools",
            "WARNING: System::setSecurityManager will be removed in a future release"
        };

        std::vector<std::string> buffered_err_lines;
        std::string current_err_line;
        int sequence_state = 0;

        auto flush_err_lines = [&]() {
            for (const auto& line : buffered_err_lines) {
                // Inline filter step to discard the 2-line dynamic JAnsi UnsatisfiedLinkError block
                if (line.rfind("Failed to load native library:jansi-", 0) == 0) {
                    continue; 
                }
                if (line.find("java.lang.UnsatisfiedLinkError:") != std::string::npos && 
                    line.find("libjansi.so: dlopen failed: library \"libc.so.6\" not found") != std::string::npos) {
                    continue;
                }
                UI::warn("JVM Internal Trace: " + line);
            }
            buffered_err_lines.clear();
            sequence_state = 0;
        };

        char buffer[256];
        ssize_t n = read(pipe_from_jvm[0], buffer, sizeof(buffer) - 1);
        
        if (n > 0) {
            buffer[n] = '\0';
            std::string resp(buffer);
            
            if (resp.find("MKAPK_DAEMON_STARTED") == std::string::npos) {
                char err_buf[1024];
                ssize_t err_n = read(pipe_err_from_jvm[0], err_buf, sizeof(err_buf) - 1);
                std::string err_msg = "";
                if (err_n > 0) {
                    err_buf[err_n] = '\0';
                    err_msg = " Stderr context: " + std::string(err_buf);
                }
                throw std::runtime_error(UI::Msg::DAEMON_FAIL + " Handshake mismatch. Response output: " + resp + err_msg);
            }

            char err_chunk[4096];
            struct timeval tv{0, 50000};
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(pipe_err_from_jvm[0], &rfds);

            if (select(pipe_err_from_jvm[0] + 1, &rfds, nullptr, nullptr, &tv) > 0) {
                ssize_t err_bytes = read(pipe_err_from_jvm[0], err_chunk, sizeof(err_chunk) - 1);
                if (err_bytes > 0) {
                    err_chunk[err_bytes] = '\0';
                    
                    for (ssize_t i = 0; i < err_bytes; ++i) {
                        if (err_chunk[i] == '\n') {
                            if (sequence_state < 4 && current_err_line == TARGET_SEQUENCE[sequence_state]) {
                                buffered_err_lines.push_back(current_err_line);
                                sequence_state++;
                                if (sequence_state == 4) {
                                    buffered_err_lines.clear();
                                    sequence_state = 0;
                                }
                            } else {
                                buffered_err_lines.push_back(current_err_line);
                                flush_err_lines();
                            }
                            current_err_line.clear();
                        } else if (err_chunk[i] != '\r') {
                            current_err_line.push_back(err_chunk[i]);
                        }
                    }
                    if (!current_err_line.empty()) {
                        buffered_err_lines.push_back(current_err_line);
                        flush_err_lines();
                    }
                }
            }
        } else {
            throw std::runtime_error(UI::Msg::DAEMON_FAIL + " JVM subprocess terminated abruptly during runtime sequence bootstrapping.");
        }
    } else {
        throw std::runtime_error("System execution fork failure routing background processes.");
    }
}

/**
 * Signals the Daemon to shut down gracefully.
 */
void stop_daemon() {
    if (daemon_pid > 0) {
        std::string stop_cmd = "STOP_DAEMON\n";
        if (write(pipe_to_jvm[1], stop_cmd.c_str(), stop_cmd.length()) == -1) {
            UI::warn("Failed to transmit tear-down sequence to operational background daemon.");
        }
        
        close(pipe_to_jvm[1]);
        close(pipe_from_jvm[0]);
        
        int status;
        waitpid(daemon_pid, &status, 0);
        daemon_pid = -1;
    }
}

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

    std::string tool_name = fs::path(args[0]).filename().string();
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