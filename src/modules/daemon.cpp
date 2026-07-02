#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <string>
#include <sstream>
#include <sys/wait.h>
#include <csignal>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"

static int pipe_to_jvm[2] = {-1, -1};   // C++ writes to JVM
static int pipe_from_jvm[2] = {-1, -1}; // C++ reads from JVM
static int pipe_err_from_jvm[2] = {-1, -1};
static pid_t daemon_pid = -1;

void start_daemon(const std::string& classpath) {
    // 1. SECURITY: Ignore SIGPIPE globally. Prevents the C++ process from being 
    // instantly killed by the OS if it attempts to write to a crashed JVM.
    signal(SIGPIPE, SIG_IGN);

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

        // PREVENT FD LEAKS: Ensure subprocesses do not inherit daemon pipes
        auto set_cloexec = [](int fd) {
            int flags = fcntl(fd, F_GETFD);
            if (flags != -1) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        };
        set_cloexec(pipe_to_jvm[1]);
        set_cloexec(pipe_from_jvm[0]);
        set_cloexec(pipe_err_from_jvm[0]);

        // RAII CLEANUP: Ensures we don't leave zombie JVMs if the handshake fails
        auto abort_bootstrap = [&](const std::string& err_msg) {
            if (daemon_pid > 0) {
                kill(daemon_pid, SIGKILL);
                waitpid(daemon_pid, nullptr, 0);
                daemon_pid = -1;
            }
            if (pipe_to_jvm[1] != -1) { close(pipe_to_jvm[1]); pipe_to_jvm[1] = -1; }
            if (pipe_from_jvm[0] != -1) { close(pipe_from_jvm[0]); pipe_from_jvm[0] = -1; }
            if (pipe_err_from_jvm[0] != -1) { close(pipe_err_from_jvm[0]); pipe_err_from_jvm[0] = -1; }
            throw std::runtime_error(err_msg);
        };

        // ROBUST WRITE: Handle partial writes and interrupts safely
        std::string start_cmd = "START_DAEMON\n";
        ssize_t total_written = 0;
        while (total_written < static_cast<ssize_t>(start_cmd.length())) {
            ssize_t bytes_written = write(pipe_to_jvm[1], start_cmd.c_str() + total_written, start_cmd.length() - total_written);
            if (bytes_written == -1) {
                if (errno == EINTR) continue;
                abort_bootstrap("System write synchronization failed on JVM entry command.");
            }
            total_written += bytes_written;
        }

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
                if (line.rfind("Failed to load native library:jansi-", 0) == 0) continue; 
                if (line.find("java.lang.UnsatisfiedLinkError:") != std::string::npos && 
                    line.find("libjansi.so: dlopen failed: library \"libc.so.6\" not found") != std::string::npos) {
                    continue;
                }
                UI::warn("JVM Internal Trace: " + line);
            }
            buffered_err_lines.clear();
            sequence_state = 0;
        };

        // PERFORMANCE: Pre-allocate capacity to prevent heap fragmentation
        std::string out_accumulator;
        out_accumulator.reserve(4096);
        std::string err_accumulator;
        err_accumulator.reserve(4096);
        
        bool handshake_complete = false;

        struct pollfd fds[2];
        fds[0].fd = pipe_from_jvm[0];
        fds[0].events = POLLIN;
        fds[1].fd = pipe_err_from_jvm[0];
        fds[1].events = POLLIN;

        auto start_time = std::chrono::steady_clock::now();
        const int TIMEOUT_MS = 10000; // 10-second hard timeout for JVM bootstrap

        while (!handshake_complete) {
            auto now = std::chrono::steady_clock::now();
            int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            int remaining_timeout = TIMEOUT_MS - elapsed;
            
            if (remaining_timeout <= 0) {
                abort_bootstrap(UI::Msg::DAEMON_FAIL + " JVM bootstrap timed out.");
            }

            int ret = poll(fds, 2, remaining_timeout);
            if (ret < 0) {
                if (errno == EINTR) continue;
                abort_bootstrap("IPC synchronization failure during daemon bootstrap.");
            }
            if (ret == 0) {
                abort_bootstrap(UI::Msg::DAEMON_FAIL + " JVM bootstrap timed out (No response).");
            }

            // Read STDOUT for handshake
            if (fds[0].revents & POLLIN) {
                char buffer[512];
                ssize_t n = read(fds[0].fd, buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    buffer[n] = '\0';
                    out_accumulator += buffer;
                    if (out_accumulator.find("MKAPK_DAEMON_STARTED") != std::string::npos) {
                        handshake_complete = true;
                    }
                } else if (n == 0) {
                    abort_bootstrap(UI::Msg::DAEMON_FAIL + " JVM terminated unexpectedly. Stderr: " + err_accumulator);
                }
            } else if (fds[0].revents & (POLLERR | POLLHUP)) {
                abort_bootstrap(UI::Msg::DAEMON_FAIL + " Daemon stdout pipe disconnected.");
            }

            // Drain STDERR to prevent pipe buffer filling
            if (fds[1].revents & POLLIN) {
                char err_chunk[512];
                ssize_t err_bytes = read(fds[1].fd, err_chunk, sizeof(err_chunk) - 1);
                if (err_bytes > 0) {
                    err_chunk[err_bytes] = '\0';
                    err_accumulator += err_chunk;
                    
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
                }
            }
        }

        if (!current_err_line.empty()) {
            buffered_err_lines.push_back(current_err_line);
            flush_err_lines();
        }

    } else {
        throw std::runtime_error("System execution fork failure routing background processes.");
    }
}

/**
 * Signals the Daemon to shut down gracefully and ensures resource cleanup.
 */
void stop_daemon() {
    if (daemon_pid > 0) {
        std::string stop_cmd = "STOP_DAEMON\n";
        
        // ROBUST WRITE: Ensure full transmission of shutdown signal
        ssize_t total_written = 0;
        while (total_written < static_cast<ssize_t>(stop_cmd.length())) {
            ssize_t bytes_written = write(pipe_to_jvm[1], stop_cmd.c_str() + total_written, stop_cmd.length() - total_written);
            if (bytes_written == -1) {
                if (errno == EINTR) continue;
                UI::warn("Failed to transmit tear-down sequence to operational background daemon (Broken Pipe).");
                break;
            }
            total_written += bytes_written;
        }
        
        close(pipe_to_jvm[1]);
        close(pipe_from_jvm[0]);
        if (pipe_err_from_jvm[0] != -1) close(pipe_err_from_jvm[0]);
        
        int status;
        waitpid(daemon_pid, &status, 0);
        daemon_pid = -1;
    }
}

/**
 * Executes a tool command by sending a delimited string to the Daemon.
 * Intercepts MKAPK_TASK_FAILED and throws exceptions to gracefully tear down threads.
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

    // ROBUST WRITE: Protects IPC pipeline against fragmentation and OS interrupts
    ssize_t total_written = 0;
    while (total_written < static_cast<ssize_t>(msg.length())) {
        ssize_t bytes_written = write(pipe_to_jvm[1], msg.c_str() + total_written, msg.length() - total_written);
        if (bytes_written == -1) {
            if (errno == EINTR) continue;
            throw std::runtime_error("IPC channel tracking broken link: Writing transaction dropped on active JVM pipeline execution.");
        }
        total_written += bytes_written;
    }

    char buffer[4096];
    std::string out_accumulator;
    out_accumulator.reserve(8192);
    
    std::string err_accumulator;
    err_accumulator.reserve(4096);
    
    bool task_completed = false;
    bool task_failed = false;
    bool in_error_block = false;

    struct pollfd fds[2];
    fds[0].fd = pipe_from_jvm[0];
    fds[0].events = POLLIN;
    fds[1].fd = pipe_err_from_jvm[0];
    fds[1].events = POLLIN;

    while (!task_completed) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue; 
            throw std::runtime_error("IPC synchronization failure: poll() returned a fatal error.");
        }

        // ====================================================================
        // CHANNEL 1: STANDARD OUTPUT (JVM Protocol & Task Flags)
        // ====================================================================
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(fds[0].fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) {
                throw std::runtime_error("IPC connection dropped out of scope: Daemon closed stdout channel prematurely.");
            }
            
            buffer[n] = '\0';
            std::string resp(buffer);
            
            // TOKEN INTERCEPTION: Evaluate the operational result
            size_t done_pos = resp.find("MKAPK_TASK_DONE");
            size_t fail_pos = resp.find("MKAPK_TASK_FAILED");
            
            if (done_pos != std::string::npos) {
                task_completed = true;
                resp.erase(done_pos, 15); 
            } else if (fail_pos != std::string::npos) {
                task_completed = true;
                task_failed = true;
                resp.erase(fail_pos, 17);
            }

            out_accumulator += resp;
            size_t newline_pos;
            
            while ((newline_pos = out_accumulator.find('\n')) != std::string::npos) {
                std::string line = out_accumulator.substr(0, newline_pos);
                out_accumulator = out_accumulator.substr(newline_pos + 1);
                
                if (line.empty()) continue;

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

    // Flush trailing fragments
    if (!out_accumulator.empty() && out_accumulator.find_first_not_of(" \t\r\n") != std::string::npos) {
        if (out_accumulator.rfind("[ERROR]|", 0) == 0) {
            UI::error(out_accumulator.substr(8));
        } else if (out_accumulator.rfind("[WARN]|", 0) == 0) {
            UI::warn(out_accumulator.substr(7));
        } else {
            UI::info("[" + args[0] + " stdout] " + out_accumulator);
        }
    }

    if (!err_accumulator.empty() && err_accumulator.find_first_not_of(" \t\r\n") != std::string::npos) {
        UI::warn("[JVM STDERR] " + err_accumulator);
    }

    // PIPELINE COLLAPSE: Gracefully reject the thread execution back up to std::async
    if (task_failed) {
        throw std::runtime_error("JVM Daemon reported a fatal execution failure during task: " + args[0]);
    }
}