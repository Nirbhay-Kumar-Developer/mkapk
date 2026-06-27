#ifndef MKAPK_UI_HPP
#define MKAPK_UI_HPP

#include <string>
#include <iostream>
#include <mutex>
#include <memory>

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

    // ============================================================================
    // 3. CORE LOGGER INTERFACE
    // ============================================================================
    class ILogger {
    public:
        virtual ~ILogger() = default;
        virtual void info(const std::string& message) = 0;
        virtual void stage(const std::string& stage_name, const std::string& details = "") = 0;
        virtual void success(const std::string& message, const std::string& prefix = "✨ ") = 0;
        virtual void warn(const std::string& message) = 0;
        virtual void error(const std::string& message, const std::string& details = "") = 0;
        
        // Required for legacy stream injections (e.g., direct std::cerr manipulation)
        virtual std::mutex& get_console_mutex() = 0; 
    };

    // ============================================================================
    // 4. CONCRETE CONSOLE IMPLEMENTATION
    // ============================================================================
    class ConsoleLogger : public ILogger {
    private:
        // Mutex is now safely bound to the object instance, preventing SIOF crashes.
        std::mutex console_mutex;

    public:
        void info(const std::string& message) override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << CYAN << "• " << RESET << message << std::endl;
        }

        void stage(const std::string& stage_name, const std::string& details = "") override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << BLUE << "» " << RESET << BOLD << stage_name << RESET;
            if (!details.empty()) std::cout << " (" << details << ")";
            std::cout << "..." << std::endl;
        }

        void success(const std::string& message, const std::string& prefix = "✨ ") override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << GREEN << prefix << BOLD << message << RESET << std::endl;
        }

        void warn(const std::string& message) override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::clog << YELLOW << "⚠️  Warning: " << RESET << message << std::endl;
        }

        void error(const std::string& message, const std::string& details = "") override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cerr << RED << "✘ Error: " << RESET << BOLD << message << RESET << std::endl;
            if (!details.empty()) {
                std::cerr << RED << "  Details: " << RESET << details << std::endl;
            }
        }

        std::mutex& get_console_mutex() override {
            return console_mutex;
        }
    };

    // ============================================================================
    // 5. LOGGER MANAGER (Service Locator / Singleton)
    // ============================================================================
    class LoggerManager {
    private:
        ILogger* active_logger;
        ConsoleLogger default_logger;

        // Private constructor initializes with standard console output safely
        LoggerManager() : active_logger(&default_logger) {}

    public:
        // Mayer's Singleton: Thread-safe and guarantees safe initialization order
        static LoggerManager& get() {
            static LoggerManager instance;
            return instance;
        }

        // Allows you to dynamically swap the logger to a GUI or File logger later
        void set_logger(ILogger* new_logger) {
            if (new_logger) active_logger = new_logger;
        }

        ILogger& logger() {
            return *active_logger;
        }
    };

    // ============================================================================
    // 6. BACKWARD COMPATIBILITY FORWARDERS
    // ============================================================================
    // These inline functions bridge all existing UI:: calls in the codebase 
    // seamlessly to the actively managed logger.

    inline std::mutex& get_console_mutex() {
        return LoggerManager::get().logger().get_console_mutex();
    }

    inline void info(const std::string& message) {
        LoggerManager::get().logger().info(message);
    }

    inline void stage(const std::string& stage_name, const std::string& details = "") {
        LoggerManager::get().logger().stage(stage_name, details);
    }

    inline void success(const std::string& message, const std::string& prefix = "✨ ") {
        LoggerManager::get().logger().success(message, prefix);
    }

    inline void warn(const std::string& message) {
        LoggerManager::get().logger().warn(message);
    }

    inline void error(const std::string& message, const std::string& details = "") {
        LoggerManager::get().logger().error(message, details);
    }
}

#endif