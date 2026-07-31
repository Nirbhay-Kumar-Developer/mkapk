#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <csignal>
#include <nlohmann/json.hpp>
#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_result.hpp"
#include "mkapk_log_sanitizer.hpp"

static int pipe_to_jvm[2] = {-1, -1};   // C++ writes to JVM
static int pipe_from_jvm[2] = {-1, -1}; // C++ reads from JVM
static int pipe_err_from_jvm[2] = {-1, -1};
static pid_t daemon_pid = -1;

static std::vector<std::string> g_last_daemon_output;
static LogSanitizer daemon_logger; // Global sanitizer instance

/**
 * Returns the recorded stdout lines from the most recent call_java_tool execution.
 */
const std::vector<std::string>& get_last_daemon_output() {
    return g_last_daemon_output;
}

Result<void> start_daemon(const std::string& classpath) {
    signal(SIGPIPE, SIG_IGN);

    if (pipe(pipe_to_jvm) == -1 || pipe(pipe_from_jvm) == -1 || pipe(pipe_err_from_jvm) == -1) {
        return Result<void>::error("System resource allocation failure: Failed to create IPC pipes.");
    }

    daemon_pid = fork();

    if (daemon_pid == 0) { // Child Process: The JVM
        // BUG FIX: Ensure the kernel kills the JVM if the C++ orchestrator terminates abruptly
        prctl(PR_SET_PDEATHSIG, SIGKILL);

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

        auto set_cloexec = [](int fd) {
            int flags = fcntl(fd, F_GETFD);
            if (flags != -1) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        };
        set_cloexec(pipe_to_jvm[1]);
        set_cloexec(pipe_from_jvm[0]);
        set_cloexec(pipe_err_from_jvm[0]);

        auto abort_bootstrap = [&](const std::string& err_msg) -> Result<void> {
            if (daemon_pid > 0) {
                kill(daemon_pid, SIGKILL);
                waitpid(daemon_pid, nullptr, 0);
                daemon_pid = -1;
            }
            if (pipe_to_jvm[1] != -1) { close(pipe_to_jvm[1]); pipe_to_jvm[1] = -1; }
            if (pipe_from_jvm[0] != -1) { close(pipe_from_jvm[0]); pipe_from_jvm[0] = -1; }
            if (pipe_err_from_jvm[0] != -1) { close(pipe_err_from_jvm[0]); pipe_err_from_jvm[0] = -1; }
            return Result<void>::error(err_msg);
        };

        // JSON Refactor: Build and dispatch structured startup payload
        nlohmann::json boot_msg;
        boot_msg["command"] = "START_DAEMON";
        std::string start_cmd = boot_msg.dump() + "\n";

        ssize_t total_written = 0;
        while (total_written < static_cast<ssize_t>(start_cmd.length())) {
            ssize_t bytes_written = write(pipe_to_jvm[1], start_cmd.c_str() + total_written, start_cmd.length() - total_written);
            if (bytes_written == -1) {
                if (errno == EINTR) continue;
                return abort_bootstrap("System write synchronization failed on JVM entry command.");
            }
            total_written += bytes_written;
        }

        std::string current_err_line;
        std::string out_accumulator;
        out_accumulator.reserve(4096);
        
        bool handshake_complete = false;

        struct pollfd fds[2];
        fds[0].fd = pipe_from_jvm[0];
        fds[0].events = POLLIN;
        fds[1].fd = pipe_err_from_jvm[0];
        fds[1].events = POLLIN;

        auto start_time = std::chrono::steady_clock::now();
        const int TIMEOUT_MS = 10000;

        while (!handshake_complete) {
            auto now = std::chrono::steady_clock::now();
            int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            int remaining_timeout = TIMEOUT_MS - elapsed;
            
            if (remaining_timeout <= 0) {
                return abort_bootstrap(UI::Msg::DAEMON_FAIL + " JVM bootstrap timed out.");
            }

            int ret = poll(fds, 2, remaining_timeout);
            if (ret < 0) {
                if (errno == EINTR) continue;
                return abort_bootstrap("IPC synchronization failure during daemon bootstrap.");
            }
            if (ret == 0) {
                return abort_bootstrap(UI::Msg::DAEMON_FAIL + " JVM bootstrap timed out (No response).");
            }

            // Read STDOUT for JSON Handshake
            if (fds[0].revents & POLLIN) {
                char buffer[4096]; 
                ssize_t n = read(fds[0].fd, buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    buffer[n] = '\0';
                    out_accumulator += buffer;
                    
                    size_t newline_pos;
                    while ((newline_pos = out_accumulator.find('\n')) != std::string::npos) {
                        std::string line = out_accumulator.substr(0, newline_pos);
                        out_accumulator = out_accumulator.substr(newline_pos + 1);
                        
                        try {
                            auto j = nlohmann::json::parse(line);
                            if (j.value("type", "") == "HANDSHAKE_OK") {
                                handshake_complete = true;
                                break;
                            }
                        } catch (...) {
                            // Drop non-JSON fragments safely during JVM boot sequence
                        }
                    }
                } else if (n == 0) {
                    return abort_bootstrap(UI::Msg::DAEMON_FAIL + " JVM terminated unexpectedly.");
                }
            } else if (fds[0].revents & (POLLERR | POLLHUP)) {
                return abort_bootstrap(UI::Msg::DAEMON_FAIL + " Daemon stdout pipe disconnected.");
            }

            if (fds[1].revents & POLLIN) {
                char err_chunk[4096];
                ssize_t err_bytes = read(fds[1].fd, err_chunk, sizeof(err_chunk) - 1);
                if (err_bytes > 0) {
                    for (ssize_t i = 0; i < err_bytes; ++i) {
                        if (err_chunk[i] == '\n') {
                            daemon_logger.process_stderr_line(current_err_line);
                            current_err_line.clear();
                        } else if (err_chunk[i] != '\r') {
                            current_err_line.push_back(err_chunk[i]);
                        }
                    }
                }
            }
        }

        if (!current_err_line.empty()) {
            daemon_logger.process_stderr_line(current_err_line);
        }
        daemon_logger.flush();

    } else {
        return Result<void>::error("System execution fork failure routing background processes.");
    }
    
    return Result<void>::success();
}

void stop_daemon() {
    if (daemon_pid > 0) {
        // JSON Refactor: Build and dispatch structured teardown payload
        nlohmann::json stop_msg;
        stop_msg["command"] = "STOP_DAEMON";
        std::string stop_cmd = stop_msg.dump() + "\n";
        
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

Result<void> call_java_tool(const std::vector<std::string>& args) {
    g_last_daemon_output.clear();

    if (daemon_pid <= 0) {
        return Result<void>::error("Daemon connection tracking flag inactive: JVM execution thread terminated.");
    }

    // JSON Refactor: Build and dispatch structured tool execution payload
    nlohmann::json payload;
    payload["command"] = args.empty() ? "" : args[0];
    
    std::vector<std::string> tool_args;
    if (args.size() > 1) {
        tool_args.assign(args.begin() + 1, args.end());
    }
    payload["args"] = tool_args;
    
    std::string msg = payload.dump() + "\n";

    ssize_t total_written = 0;
    while (total_written < static_cast<ssize_t>(msg.length())) {
        ssize_t bytes_written = write(pipe_to_jvm[1], msg.c_str() + total_written, msg.length() - total_written);
        if (bytes_written == -1) {
            if (errno == EINTR) continue;
            return Result<void>::error("IPC channel tracking broken link: Writing transaction dropped on active JVM pipeline execution.");
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

    struct pollfd fds[2];
    fds[0].fd = pipe_from_jvm[0];
    fds[0].events = POLLIN;
    fds[1].fd = pipe_err_from_jvm[0];
    fds[1].events = POLLIN;

    while (!task_completed) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue; 
            return Result<void>::error("IPC synchronization failure: poll() returned a fatal error.");
        }

        // CHANNEL 1: STANDARD OUTPUT (JSON Parsing)
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(fds[0].fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) {
                return Result<void>::error("IPC connection dropped out of scope: Daemon closed stdout channel prematurely.");
            }
            
            buffer[n] = '\0';
            out_accumulator += buffer;

            size_t newline_pos;
            while ((newline_pos = out_accumulator.find('\n')) != std::string::npos) {
                std::string line = out_accumulator.substr(0, newline_pos);
                out_accumulator = out_accumulator.substr(newline_pos + 1);
                
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.empty()) continue;

                // Attempt to parse the structured IPC response payload
                try {
                    auto j = nlohmann::json::parse(line);
                    std::string type = j.value("type", "");

                    if (type == "DONE") {
                        task_completed = true;
                    } else if (type == "FAILED") {
                        task_completed = true;
                        task_failed = true;
                    } else if (type == "LOG") {
                        std::string level = j.value("level", "INFO");
                        std::string message = j.value("message", "");
                        
                        // Map back to legacy log routing format expected by LogSanitizer
                        if (level == "ERROR") {
                            g_last_daemon_output.push_back("[ERROR]|" + message);
                            daemon_logger.process_stdout_line("[ERROR]|" + message, args[0]);
                        } else if (level == "WARN") {
                            g_last_daemon_output.push_back("[WARN]|" + message);
                            daemon_logger.process_stdout_line("[WARN]|" + message, args[0]);
                        } else {
                            g_last_daemon_output.push_back(message);
                            daemon_logger.process_stdout_line(message, args[0]);
                        }
                    } else if (type == "DATA") {
                        // Crucial for MkapkResolver: Retain the expected output prefix for dynamic resolution mapping
                        std::string payload_data = j.value("payload", "");
                        std::string legacy_data = "MKAPK_RESOLVED|" + payload_data;
                        g_last_daemon_output.push_back(legacy_data);
                    }
                } catch (const nlohmann::json::parse_error&) {
                    // Fail-safe: Route raw unparsed strings (e.g. JVM internal crash dumps) through normally
                    g_last_daemon_output.push_back(line);
                    daemon_logger.process_stdout_line(line, args[0]);
                }
            }
        }
        else if (fds[0].revents & (POLLERR | POLLHUP)) {
             return Result<void>::error("IPC channel tracking broken link: Daemon stdout pipe disconnected.");
        }

        // CHANNEL 2: STANDARD ERROR (Passed straight into LogSanitizer for Jansi noise checks)
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(fds[1].fd, buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = '\0';
                err_accumulator += buffer;
                
                size_t newline_pos;
                while ((newline_pos = err_accumulator.find('\n')) != std::string::npos) {
                    std::string line = err_accumulator.substr(0, newline_pos);
                    err_accumulator = err_accumulator.substr(newline_pos + 1);
                    
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) {
                        daemon_logger.process_stderr_line(line);
                    }
                }
            }
        }
    }

    if (!err_accumulator.empty() && err_accumulator.find_first_not_of(" \t\r\n") != std::string::npos) {
        daemon_logger.process_stderr_line(err_accumulator);
    }
    
    daemon_logger.flush();

    if (task_failed) {
        return Result<void>::error("JVM Daemon reported a fatal execution failure during task: " + args[0]);
    }
    
    return Result<void>::success();
}