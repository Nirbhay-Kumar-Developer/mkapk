#include "mkapk_log_sanitizer.hpp"
#include "mkapk_ui.hpp"
#include <mutex>
#include <iostream>

void LogSanitizer::flush_err_lines() {
    for (const auto& line : buffered_err_lines) {
        // Filter out expected Jansi library loading noise
        if (line.rfind("Failed to load native library:jansi-", 0) == 0) continue; 
        if (line.find("java.lang.UnsatisfiedLinkError:") != std::string::npos && 
            line.find("libjansi.so: dlopen failed: library \"libc.so.6\" not found") != std::string::npos) {
            continue;
        }
        UI::warn("[JVM STDERR] " + line);
    }
    buffered_err_lines.clear();
    sequence_state = 0;
}

void LogSanitizer::process_stderr_line(const std::string& line) {
    if (sequence_state < 4 && line == TARGET_SEQUENCE[sequence_state]) {
        buffered_err_lines.push_back(line);
        sequence_state++;
        
        // If the full 4-line sequence is matched, silently drop it
        if (sequence_state == 4) {
            buffered_err_lines.clear();
            sequence_state = 0;
        }
    } else {
        buffered_err_lines.push_back(line);
        flush_err_lines();
    }
}

void LogSanitizer::process_stdout_line(const std::string& line, const std::string& context) {
    if (line.empty()) return;

    if (line.rfind("[ERROR]|", 0) == 0) {
        std::string clean_line = line.substr(8);
        
        if (!in_error_block) {
            UI::error(clean_line);
            in_error_block = true;
        } else {
            // Indent subsequent lines of a multi-line error block cleanly
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
        UI::info("[" + context + " stdout] " + line);
    }
}

void LogSanitizer::flush() {
    if (!buffered_err_lines.empty()) {
        flush_err_lines();
    }
    in_error_block = false;
}