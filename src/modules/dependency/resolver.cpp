#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <cerrno>

#include "mkapk_resolver.hpp"
#include "mkapk_helpers.hpp"
#include "mkapk_ui.hpp"

namespace MkapkResolver {

/**
 * Clean token splitter to tokenize JVM arrays bounded by pipe delimiters.
 */
static std::vector<std::string> split_tokens(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

std::vector<std::string> resolve_dependencies(const std::string& coordinate, const MkapkConfig& config) {
    std::vector<std::string> resolved_paths;

    // 1. Resolve Classpath & Daemon Targets
    std::string classpath = MkapkEnv::get_jni_classpath(config);
    
    int pipe_to_jvm[2];
    int pipe_from_jvm[2];

    if (pipe(pipe_to_jvm) == -1 || pipe(pipe_from_jvm) == -1) {
        UI::error("Failed to allocate systemic tracking descriptor pipes for dependency resolution.");
        return resolved_paths;
    }

    // 2. Fork Process safely mirroring the daemon.cpp implementation context
    pid_t pid = fork();

    if (pid == 0) { // Child Process: Target JVM Execution
        dup2(pipe_to_jvm[0], STDIN_FILENO);
        dup2(pipe_from_jvm[1], STDOUT_FILENO);

        close(pipe_to_jvm[1]);
        close(pipe_from_jvm[0]);

        // Execute one-shot resolution using Java execution rules
        execlp("java", "java", 
               "-Djava.security.manager=allow",
               "-cp", classpath.c_str(),
               "com.mkapk.tools.MkapkTools",
               "START_DAEMON", nullptr);
        
        _exit(127);
    } 
    else if (pid > 0) { // Parent Process: C++ Orchestration
        close(pipe_to_jvm[0]);
        close(pipe_from_jvm[1]);

        // Generate the transaction block protocol token string
        std::string request_token = "RESOLVE|" + coordinate + "\n";
        
        // Write out structural verification transaction payload securely
        ssize_t total_written = 0;
        while (total_written < static_cast<ssize_t>(request_token.length())) {
            ssize_t bytes_written = write(pipe_to_jvm[1], request_token.c_str() + total_written, request_token.length() - total_written);
            if (bytes_written == -1) {
                if (errno == EINTR) continue;
                close(pipe_to_jvm[1]);
                close(pipe_from_jvm[0]);
                UI::error("IPC handshake drop: Writing transaction rejected by active JVM pipeline execution.");
                return resolved_paths;
            }
            total_written += bytes_written;
        }

        // Close the write pipe to signal EOF to Java's reader loop cleanly
        close(pipe_to_jvm[1]);

        // 3. Implement Stream Accumulator Parsing Loop
        std::string out_accumulator;
        out_accumulator.reserve(4096);
        char buffer[1024];
        
        struct pollfd fds[1];
        fds[0].fd = pipe_from_jvm[0];
        fds[0].events = POLLIN;

        bool resolution_finished = false;

        while (!resolution_finished) {
            int poll_ret = poll(fds, 1, 15000); // 15-second tracking timeout limit boundary
            if (poll_ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (poll_ret == 0) {
                UI::error("Dependency graph compiler timed out waiting for remote remote index mappings lists.");
                break;
            }

            if (fds[0].revents & POLLIN) {
                ssize_t bytes_read = read(fds[0].fd, buffer, sizeof(buffer) - 1);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    out_accumulator += buffer;

                    size_t newline_pos;
                    while ((newline_pos = out_accumulator.find('\n')) != std::string::npos) {
                        std::string line = out_accumulator.substr(0, newline_pos);
                        out_accumulator = out_accumulator.substr(newline_pos + 1);

                        // Sanitize carriage return markers from strings
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.empty()) continue;

                        // Intercept target lines securely
                        if (line.rfind("MKAPK_RESOLVED|", 0) == 0) {
                            std::string payload = line.substr(15);
                            resolved_paths = split_tokens(payload, '|');
                            resolution_finished = true;
                        } 
                        else if (line.rfind("[ERROR]|", 0) == 0) {
                            UI::error("Resolver Engine Exception", line.substr(8));
                            resolution_finished = true;
                        }
                    }
                } else if (bytes_read == 0) {
                    // Daemon closed output pipe context safely
                    resolution_finished = true;
                }
            } 
            else if (fds[0].revents & (POLLERR | POLLHUP)) {
                break;
            }
        }

        close(pipe_from_jvm[0]);
        
        int status;
        waitpid(pid, &status, 0);
    }

    return resolved_paths;
}

} // namespace MkapkResolver