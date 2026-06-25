#ifndef MKAPK_UI_HPP
#define MKAPK_UI_HPP

#include <string>
#include <iostream>
#include <mutex>

namespace UI {
    // 1. ANSI COLOR CODES
    const std::string RESET       = "\033[0m";
    const std::string BOLD        = "\033[1m";
    const std::string RED         = "\033[31m";
    const std::string GREEN       = "\033[32m";
    const std::string YELLOW      = "\033[33m";
    const std::string BLUE        = "\033[34m";
    const std::string PURPLE      = "\033[35m";
    const std::string CYAN        = "\033[36m";
    const std::string WHITE       = "\033[37m";

    // 2. CENTRALIZED STRING REGISTRY (Change strings here to update the whole app)
    namespace Msg {
        // Build Lifecycle
        const std::string BUILD_START      = "Starting operational build pipeline";
        const std::string BUILD_SUCCESS    = "mkapk: Build finished successfully.";
        const std::string BUILD_UP_TO_DATE = "Project is already up-to-date.";
        const std::string CLEAN_START      = "Cleaning 'bin/' directory (preserving keystores)...";
        const std::string CLEAN_SUCCESS    = "Clean finished successfully.";
        
        // Modules
        const std::string RES_STAGE        = "[RES] Linking Manifest & Resources";
        const std::string JAVA_STAGE       = "[JAVA] Compiling Java source files";
        const std::string KOTLIN_STAGE     = "[KOTLIN] Compiling Kotlin source files";
        const std::string NATIVE_STAGE     = "[NATIVE] Processing Native ABI compilation matrix";
        const std::string PACK_STAGE       = "[PACK] Assembling variant target container";
        const std::string SIGN_STAGE       = "[SIGN] Signing production APK packages";
        
        // Daemon & Errors
        const std::string DAEMON_FAIL      = "Daemon Error: Handshake failed or JVM process died.";
        const std::string CONFIG_MISSING   = "Configuration file 'config.json' not found. Run 'mkapk init'.";
        const std::string FATAL_INTERNAL   = "A fatal internal compilation error occurred.";
    }

    // 3. THREAD-SAFE CENTRALISED OUTPUT ENGINE
     inline std::mutex& get_console_mutex() {
        static std::mutex console_mutex;
        return console_mutex;
    }

    inline void info(const std::string& message) {
        std::lock_guard<std::mutex> lock(get_console_mutex());
        std::cout << CYAN << "• " << RESET << message << std::endl;
    }

    inline void stage(const std::string& stage_name, const std::string& details = "") {
        std::lock_guard<std::mutex> lock(get_console_mutex());
        std::cout << BLUE << "» " << RESET << BOLD << stage_name << RESET;
        if (!details.empty()) std::cout << " (" << details << ")";
        std::cout << "..." << std::endl;
    }

    inline void success(const std::string& message, const std::string& prefix = "✨ ") {
        std::lock_guard<std::mutex> lock(get_console_mutex());
        std::cout << GREEN << prefix << BOLD << message << RESET << std::endl;
    }

    inline void warn(const std::string& message) {
        std::lock_guard<std::mutex> lock(get_console_mutex());
        std::clog << YELLOW << "⚠️  Warning: " << RESET << message << std::endl;
    }

    inline void error(const std::string& message, const std::string& details = "") {
        std::lock_guard<std::mutex> lock(get_console_mutex());
        std::cerr << RED << "✘ Error: " << RESET << BOLD << message << RESET << std::endl;
        if (!details.empty()) {
            std::cerr << RED << "  Details: " << RESET << details << std::endl;
        }
    }
}

#endif