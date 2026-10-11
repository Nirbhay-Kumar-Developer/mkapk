#include "mkapk_log_sanitizer.hpp"
#include "mkapk_ui.hpp"

void LogSanitizer::flush_err_lines() {
    for (const auto& line : buffered_err_lines) {
        if (line.rfind("Failed to load native library:jansi-", 0) == 0) continue; 
        if (line.find("java.lang.UnsatisfiedLinkError:") != std::string::npos && 
            line.find("libjansi.so: dlopen failed: library \"libc.so.6\" not found") != std::string::npos) {
            continue;
        }
        UI::warn(line);
    }
    buffered_err_lines.clear();
    sequence_state = 0;
}

void LogSanitizer::process_stderr_line(const std::string& line) {
    if (sequence_state < 4 && line == TARGET_SEQUENCE[sequence_state]) {
        buffered_err_lines.push_back(line);
        sequence_state++;
        
        if (sequence_state == 4) {
            buffered_err_lines.clear();
            sequence_state = 0;
        }
    } else {
        buffered_err_lines.push_back(line);
        flush_err_lines();
    }
}

void LogSanitizer::process_stdout_line(const std::string& line, const std::string& /* context */) {
    if (line.empty()) return;

    // --- 1. GENERIC MULTI-LINE PROGUARD / R8 NOTICES ---
    // Detect start of any unused keep rule notice from any project/library
    if (line.rfind("Info in ", 0) == 0 || 
        line.find("Proguard configuration rule does not match anything") != std::string::npos) {
        in_proguard_info_block = true;
        return;
    }

    // Swallow all rule lines until the closing delimiter
    if (in_proguard_info_block) {
        if (line == "}" || line == "}`" || line.rfind("}`", line.size() >= 2 ? line.size() - 2 : 0) != std::string::npos) {
            in_proguard_info_block = false;
        }
        return;
    }

    // --- 2. JAVAC DIAGNOSTIC NOTICES ---
    if (line.rfind("Note:", 0) == 0) return;
    if (line.find("system modules path not set in conjunction with -source") != std::string::npos) return;
    if (line.find("warning") != std::string::npos && line.size() < 16) return; // e.g. "1 warning"

    // --- 3. GENERIC ANDRESGUARD PROGRESS & XML WARNINGS ---
    if (line.rfind("unknown tag ", 0) == 0) return; // Ignores any unsupported/deprecated XML tag
    if (line.rfind("special ", 0) == 0) return;
    if (line.rfind("reading config file", 0) == 0) return;
    if (line.rfind("mKeepRoot", 0) == 0) return;
    if (line.rfind("convertToPatternString", 0) == 0) return;
    if (line.rfind("[AndResGuard]", 0) == 0) return;
    if (line.rfind("unziping apk", 0) == 0) return;
    if (line.rfind("decoding resources", 0) == 0) return;
    if (line.rfind("parse to get", 0) == 0) return;
    if (line.rfind("reading packagename", 0) == 0) return;
    if (line.rfind("resources mapping file", 0) == 0) return;
    if (line.rfind("writing new resources", 0) == 0) return;
    if (line.rfind("resources.arsc", 0) == 0) return;
    if (line.rfind("General unsigned apk", 0) == 0) return;
    if (line.rfind("DestResDir", 0) == 0) return;

    // --- 4. STRUCTURED DAEMON IPC DIRECTIVES ---
    if (line.rfind("[ERROR]|", 0) == 0) {
        UI::error(line.substr(8));
        return;
    } 
    
    if (line.rfind("[WARN]|", 0) == 0) {
        UI::warn(line.substr(7));
        return;
    }

    if (line.rfind("warning:", 0) == 0 || line.find(": warning:") != std::string::npos) {
        UI::warn(line);
        return;
    }

    if (line.rfind("error:", 0) == 0 || line.find(": error:") != std::string::npos) {
        UI::error(line);
        return;
    }

    // Stream genuine compiler outputs
    UI::raw(line);
}

void LogSanitizer::flush() {
    if (!buffered_err_lines.empty()) {
        flush_err_lines();
    }
    in_proguard_info_block = false;
    std::cout.flush();
}
