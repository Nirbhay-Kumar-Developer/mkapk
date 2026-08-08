#include "mkapk_ipc.hpp"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <sys/wait.h>
#include <csignal>
#include "mkapk_ui.hpp"

namespace IpcClient {

    static int pipe_to_jvm[2] = {-1, -1};
    static int pipe_from_jvm[2] = {-1, -1};
    static int pipe_err_from_jvm[2] = {-1, -1};
    static pid_t daemon_pid = -1;

    Result<void> start_daemon(const std::string& classpath) {
        signal(SIGPIPE, SIG_IGN);

        if (pipe(pipe_to_jvm) == -1 || pipe(pipe_from_jvm) == -1 || pipe(pipe_err_from_jvm) == -1) {
            return Result<void>::error("System resource allocation failure: Failed to create IPC pipes.");
        }

        daemon_pid = fork();

        if (daemon_pid == 0) {
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
        else if (daemon_pid > 0) {
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

            // Send JSON bootstrap command
            nlohmann::json boot_msg;
            boot_msg["command"] = "START_DAEMON";
            std::string start_cmd = boot_msg.dump() + "\n";
            
            ssize_t total_written = 0;
            while (total_written < static_cast<ssize_t>(start_cmd.length())) {
                ssize_t bytes = write(pipe_to_jvm[1], start_cmd.c_str() + total_written, start_cmd.length() - total_written);
                if (bytes == -1) {
                    if (errno == EINTR) continue;
                    return abort_bootstrap("System write synchronization failed on JVM entry command.");
                }
                total_written += bytes;
            }

            struct pollfd fds[1];
            fds[0].fd = pipe_from_jvm[0];
            fds[0].events = POLLIN;

            auto start_time = std::chrono::steady_clock::now();
            std::string out_accumulator;
            bool handshake_complete = false;

            while (!handshake_complete) {
                int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
                if (10000 - elapsed <= 0) return abort_bootstrap("JVM bootstrap timed out.");

                int ret = poll(fds, 1, 10000 - elapsed);
                if (ret < 0 && errno != EINTR) return abort_bootstrap("IPC sync failure during bootstrap.");
                if (ret == 0) return abort_bootstrap("JVM bootstrap timed out (No response).");

                if (fds[0].revents & POLLIN) {
                    char buffer[512];
                    ssize_t n = read(fds[0].fd, buffer, sizeof(buffer) - 1);
                    if (n <= 0) return abort_bootstrap("JVM terminated unexpectedly.");
                    
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
                        } catch (...) { /* Ignore non-JSON output during boot */ }
                    }
                }
            }
            return Result<void>::success();
        }
        return Result<void>::error("Fork failure.");
    }

    Result<void> stop_daemon() {
        if (daemon_pid > 0) {
            nlohmann::json stop_msg;
            stop_msg["command"] = "STOP_DAEMON";
            std::string stop_cmd = stop_msg.dump() + "\n";
            
            write(pipe_to_jvm[1], stop_cmd.c_str(), stop_cmd.length());
            close(pipe_to_jvm[1]);
            close(pipe_from_jvm[0]);
            close(pipe_err_from_jvm[0]);
            
            waitpid(daemon_pid, nullptr, 0);
            daemon_pid = -1;
        }
        return Result<void>::success();
    }

    Result<void> execute_tool(const std::string& command, const std::vector<std::string>& args, std::vector<std::string>& out_logs) {
        if (daemon_pid <= 0) return Result<void>::error("JVM execution thread terminated.");

        out_logs.clear();

        // 1. Serialize command to JSON
        nlohmann::json payload;
        payload["command"] = command;
        payload["args"] = args;
        std::string msg = payload.dump() + "\n";

        // 2. Transmit
        ssize_t total_written = 0;
        while (total_written < static_cast<ssize_t>(msg.length())) {
            ssize_t bytes = write(pipe_to_jvm[1], msg.c_str() + total_written, msg.length() - total_written);
            if (bytes == -1) {
                if (errno == EINTR) continue;
                return Result<void>::error("IPC channel broken: Write failed.");
            }
            total_written += bytes;
        }

        // 3. Receive JSON stream
        char buffer[4096];
        std::string out_accumulator;
        bool task_completed = false;
        bool task_failed = false;
        struct pollfd fds[1] = {{pipe_from_jvm[0], POLLIN, 0}};

        while (!task_completed) {
            int ret = poll(fds, 1, -1);
            if (ret < 0 && errno != EINTR) return Result<void>::error("IPC poll failure.");

            if (fds[0].revents & POLLIN) {
                ssize_t n = read(fds[0].fd, buffer, sizeof(buffer) - 1);
                if (n <= 0) return Result<void>::error("Daemon closed stdout prematurely.");
                
                buffer[n] = '\0';
                out_accumulator += buffer;

                size_t newline_pos;
                while ((newline_pos = out_accumulator.find('\n')) != std::string::npos) {
                    std::string line = out_accumulator.substr(0, newline_pos);
                    out_accumulator = out_accumulator.substr(newline_pos + 1);
                    if (line.empty()) continue;

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
                            
                            // Format for the UI Logger / Sanitizer
                            out_logs.push_back("[" + level + "] " + message);
                        } else if (type == "DATA") {
                            // Specialized payload parsing (e.g. Resolver outputs)
                            std::string data = j.value("payload", "");
                            out_logs.push_back("[DATA] " + data);
                        }
                    } catch (const nlohmann::json::parse_error&) {
                        // Forward raw stdout crashes that bypassed the JSON formatter
                        out_logs.push_back("[RAW] " + line);
                    }
                }
            } else if (fds[0].revents & (POLLERR | POLLHUP)) {
                return Result<void>::error("Daemon stdout pipe disconnected.");
            }
        }

        if (task_failed) return Result<void>::error("JVM Daemon reported execution failure.");
        return Result<void>::success();
    }
}