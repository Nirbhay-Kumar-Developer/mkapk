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

/**
 * Smart Command Runner: Routes tools to either the persistent Daemon or native Fork/Exec.
 */
void smart_run(const std::vector<std::string>& args, const std::string& err_msg) {
    if (args.empty()) return;

    std::string tool_path = args[0];
    size_t last_slash = tool_path.find_last_of('/');
    std::string tool_name = (last_slash == std::string::npos) ? tool_path : tool_path.substr(last_slash + 1);

    // [ANTI-FREEZE PATCH]: Bypass the isolated Java Daemon for apksigner on Release builds.
    // This allows the native fork/exec child process to cleanly inherit the parent's terminal 
    // stdin context so you can type your password securely.
    bool is_release_signing = (tool_name == "apksigner" && 
                               std::find(args.begin(), args.end(), "androiddebugkey") == args.end());

    bool use_daemon = (tool_name == "d8" || tool_name == "r8" || 
                       tool_name == "javac" || tool_name == "resguard" ||
                       (tool_name == "apksigner" && !is_release_signing) || 
                       tool_name == "kotlinc");

    if (use_daemon) {
        std::vector<std::string> daemon_args = args;
        daemon_args[0] = tool_name; 
        call_java_tool(daemon_args);
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process inherits standard stdin/stdout/stderr automatically
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