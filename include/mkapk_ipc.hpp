#ifndef MKAPK_IPC_HPP
#define MKAPK_IPC_HPP

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "mkapk_result.hpp"

namespace IpcClient {

    /**
     * Initializes the background JVM daemon.
     * @param classpath The assembled JNI classpath for the Java process.
     */
    Result<void> start_daemon(const std::string& classpath);

    /**
     * Gracefully tears down the background daemon.
     */
    Result<void> stop_daemon();

    /**
     * Dispatches a structured JSON command to the daemon.
     * @param command The tool identifier (e.g., "javac", "d8", "resolve").
     * @param args The command-line arguments for the tool.
     * @param out_logs Buffer to store raw log lines for further processing.
     */
    Result<void> execute_tool(const std::string& command, 
                              const std::vector<std::string>& args, 
                              std::vector<std::string>& out_logs);

}

#endif // MKAPK_IPC_HPP