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
        throw std::runtime_error("!! System Error: Failed to create IPC pipes.");
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
        
        std::cerr << "!! Daemon Error: Could not launch OpenJDK 'java' binary." << std::endl;
        _exit(1);
    } 
    else if (daemon_pid > 0) { // Parent Process: mkapk Native
        close(pipe_to_jvm[0]);
        close(pipe_from_jvm[1]);
        close(pipe_err_from_jvm[1]);

        std::string start_cmd = "START_DAEMON\n";
        if (write(pipe_to_jvm[1], start_cmd.c_str(), start_cmd.length()) == -1) {
             throw std::runtime_error("!! System Error: Failed to write to JVM pipe.");
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
                std::cerr << line << "\n";
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
                    err_msg = "\nStderr: " + std::string(err_buf);
                }
                throw std::runtime_error("!! Daemon Error: Handshake failed.\nOutput: " + resp + err_msg);
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
            throw std::runtime_error("!! Daemon Error: JVM process died immediately after start.");
        }
    } else {
        throw std::runtime_error("!! System Error: Failed to fork JVM Daemon.");
    }
}

/**
 * Signals the Daemon to shut down gracefully.
 */
void stop_daemon() {
    if (daemon_pid > 0) {
        std::string stop_cmd = "STOP_DAEMON\n";
        write(pipe_to_jvm[1], stop_cmd.c_str(), stop_cmd.length());
        
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
        throw std::runtime_error("!! Daemon Error: JVM process is not running.");
    }

    std::stringstream ss;
    for (size_t i = 0; i < args.size(); ++i) {
        ss << args[i] << (i == args.size() - 1 ? "" : "|");
    }
    std::string msg = ss.str() + "\n";

    if (write(pipe_to_jvm[1], msg.c_str(), msg.length()) == -1) {
        throw std::runtime_error("!! Daemon Error: Broken pipe to JVM.");
    }

    char buffer[4096];
    while (true) {
        ssize_t n = read(pipe_from_jvm[0], buffer, sizeof(buffer) - 1);
        if (n <= 0) throw std::runtime_error("!! Daemon Error: JVM terminated unexpectedly.");
        
        buffer[n] = '\0';
        std::string resp(buffer);
        
        if (resp.find("MKAPK_TASK_DONE") != std::string::npos) break;
        std::cout << resp; 
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
                       tool_name == "apksigner");

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
                throw std::runtime_error("!! Build Failed: " + err_msg + " (" + tool_name + ")");
            }
        }
    }
}