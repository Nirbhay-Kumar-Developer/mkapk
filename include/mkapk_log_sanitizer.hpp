#ifndef MKAPK_LOG_SANITIZER_HPP
#define MKAPK_LOG_SANITIZER_HPP

#include <string>
#include <vector>

class LogSanitizer {
private:
    int sequence_state = 0;
    std::vector<std::string> buffered_err_lines;
    bool in_error_block = false;

    // The deprecation warning sequence we want to silently drop
    const std::vector<std::string> TARGET_SEQUENCE = {
        "WARNING: A terminally deprecated method in java.lang.System has been called",
        "WARNING: System::setSecurityManager has been called by com.mkapk.tools.MkapkTools (file:/data/data/com.termux/files/usr/share/mkapk/mkapk-coordinator.jar)",
        "WARNING: Please consider reporting this to the maintainers of com.mkapk.tools.MkapkTools",
        "WARNING: System::setSecurityManager will be removed in a future release"
    };

    void flush_err_lines();

public:
    void process_stderr_line(const std::string& line);
    void process_stdout_line(const std::string& line, const std::string& context);
    void flush();
};

#endif // MKAPK_LOG_SANITIZER_HPP