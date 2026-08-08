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
 * Smart Command Runner: Routes tools to either the persistent Daemon or native posix_spawn.
 */
Result<void> smart_run(const std::vector<std::string>& args, const std::string& err_msg) {
    if (args.empty()) {
        return Result<void>::success();
    }

    std::string tool_path = args[0];
    size_t last_slash = tool_path.find_last_of('/');
    std::string tool_name = (last_slash == std::string::npos) ? tool_path : tool_path.substr(last_slash + 1);

    // [ANTI-FREEZE PATCH]: Bypass the isolated Java Daemon for apksigner on Release builds.
    // This allows the native child process to cleanly inherit the parent's terminal 
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
        
        // Return the IPC execution result so failures are propagated up the pipeline
        return call_java_tool(daemon_args);
    } else {
        // Delegate to the centralized POSIX spawn execution handler
        bool success = MkapkEnv::run_system_cmd(args);
        
        if (!success) {
            return Result<void>::error("External tool runtime execution anomaly flags raised inside (" + 
                                       tool_name + "). Message mapping reference context: " + err_msg);
        }
        
        return Result<void>::success();
    }
}
