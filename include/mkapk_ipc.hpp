#ifndef MKAPK_IPC_HPP
#define MKAPK_IPC_HPP

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "mkapk_result.hpp"

namespace IpcClient {
    // Initializes the background JVM daemon.
    Result<void> start_daemon(const std::string& classpath);

    // Returns a thread-safe value copy of the most recent daemon output lines
    std::vector<std::string> get_last_daemon_output();

    /**
     * Gracefully tears down the background daemon.
     */
    void stop_daemon();

    /**
     * Dispatches a structured JSON command to the daemon.
     * @param command The tool identifier (e.g., "javac", "d8", "resolve").
     * @param args The command-line arguments for the tool.
     * @param out_logs Buffer to store raw log lines for further processing.
     */
    Result<void> call_java_tool(const std::vector<std::string>& args);
}

#endif // MKAPK_IPC_HPP
