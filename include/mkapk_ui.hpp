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

    // 2. CENTRALIZED STRING & MESSAGE REGISTRY
    namespace Msg {
        // Lifecycle & Core Status
        const std::string BUILD_START          = "Starting mkapk build";
        const std::string BUILD_SUCCESS        = "mkapk: Build finished successfully.";
        const std::string BUILD_UP_TO_DATE     = "Project is already up-to-date.";
        const std::string CLEAN_START          = "Cleaning build cache directories";
        const std::string CLEAN_SUCCESS        = "Clean finished successfully.";
        const std::string ARTIFACT_LOC         = "Artifact Location: ";
        
        // Mode Subtitles
        const std::string MODE_RELEASE         = "Release Mode";
        const std::string MODE_DEBUG           = "Debug Mode";
        const std::string MULTI_ABI            = "Multi-ABI Output";
        const std::string ABI_PREFIX           = "ABI: ";

        // Pipeline Stages
        const std::string STAGE_RES            = "Processing Resources";
        const std::string STAGE_RES_LINK       = "Linking Resources";
        const std::string STAGE_SOURCE         = "Processing Source code";
        const std::string STAGE_JAVA           = "Java Compiler";
        const std::string STAGE_KOTLIN         = "Kotlin Compiler";
        const std::string STAGE_NATIVE         = "Native Compiler";
        const std::string STAGE_PACK           = "Packaging";
        const std::string STAGE_SIGN           = "Apk Signer";
        const std::string STAGE_ALIGN          = "Alignment";
        const std::string STAGE_OBFUSCATE      = "Resource Obfuscation";
        const std::string STAGE_DEX            = "Dexing";
        const std::string STAGE_MINIFY         = "Minification";
        const std::string STAGE_NDK_LIBS       = "NDK Dependencies";
        const std::string STAGE_INIT           = "Initialization";
        
        // Stage Subtitles & Operations
        const std::string OP_COMPILE_RES       = "Compiling resources";
        const std::string OP_COMPILING_JVM     = "Compiling Java/Kotlin source files";
        const std::string OP_COMPILING_NATIVE  = "Compiling native source code";
        const std::string OP_RESOLVING_LIBS    = "Auto-resolving native dependencies";
        const std::string OP_PREPARING_APK     = "Preparing and assembling APK archive";
        const std::string OP_OBFUSCATING       = "Obfuscating resources";
        const std::string OP_ALIGNING          = "Aligning apk";
        const std::string OP_SIGNING           = "Signing apk";
        const std::string OP_R8_OPTIMIZE       = "Dexing and optimizing bytecode";
        const std::string OP_CC_COMPILE        = "Compiling native code";
        const std::string OP_LD_LINK           = "Linking shared object libraries";
        const std::string OP_LIB_AUTOPLACED    = "Added library: ";
        
        // Notices & Warnings
        const std::string WARN_CONTAINER_OK    = "Packaged artifacts are up-to-date.";
        const std::string WARN_RESGUARD_MISS   = "AndResGuard tool or configuration XML missing. Skipping obfuscation.";
        const std::string WARN_RESGUARD_FAIL   = "AndResGuard execution failed.";
        const std::string WARN_STRIP_FAIL      = "Binary code striping failed.";
        const std::string WARN_KEYSTORE_FAIL   = "Automated debug keystore generation failed.";

        // Fatal & Error Strings
        const std::string DAEMON_FAIL          = "Daemon Error: Handshake failed or JVM process died.";
        const std::string CONFIG_MISSING       = "Configuration file 'config.json' not found. Run 'mkapk init'.";
        const std::string FATAL_INTERNAL       = "A fatal internal compilation error occurred.";
        const std::string ERR_MANIFEST_MISSING = "Cannot link resources without a valid AndroidManifest.xml.";
        const std::string ERR_SDK_MISSING      = "Android SDK android.jar dependency could not be resolved.";
        const std::string ERR_KEYSTORE_MISSING = "Keystore is missing.";
    }

    // ============================================================================
    // 3. CORE LOGGER INTERFACE
    // ============================================================================
    class ILogger {
    public:
        virtual ~ILogger() = default;
        virtual void info(const std::string& message) = 0;
        virtual void raw(const std::string& message) = 0;
        virtual void stage(const std::string& stage_name, const std::string& details = "") = 0;
        virtual void success(const std::string& message, const std::string& prefix = "✨ ") = 0;
        virtual void warn(const std::string& message) = 0;
        virtual void error(const std::string& message, const std::string& details = "") = 0;
        virtual std::mutex& get_console_mutex() = 0; 
    };

    // ============================================================================
    // 4. CONCRETE CONSOLE IMPLEMENTATION
    // ============================================================================
    class ConsoleLogger : public ILogger {
    private:
        std::mutex console_mutex;

    public:
        void info(const std::string& message) override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << CYAN << "• " << RESET << message << '\n';
            std::cout.flush();
        }

        void raw(const std::string& message) override {
            std::cout << message << '\n';
            std::cout.flush();
        }

        void stage(const std::string& stage_name, const std::string& details = "") override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << BLUE << "» " << RESET << BOLD << stage_name << RESET;
            if (!details.empty()) std::cout << " (" << details << ")";
            std::cout << "...\n";
            std::cout.flush();
        }

        void success(const std::string& message, const std::string& prefix = "✨ ") override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << GREEN << prefix << BOLD << message << RESET << '\n';
            std::cout.flush();
        }

        void warn(const std::string& message) override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::clog << YELLOW << "⚠️  Warning: " << RESET << message << '\n';
            std::clog.flush();
        }

        void error(const std::string& message, const std::string& details = "") override {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cerr << RED << "✘ Error: " << RESET << BOLD << message << RESET << '\n';
            if (!details.empty()) {
                std::cerr << RED << "  Details: " << RESET << details << '\n';
            }
            std::cerr.flush();
        }

        std::mutex& get_console_mutex() override {
            return console_mutex;
        }
    };

    // ============================================================================
    // 5. LOGGER MANAGER (Service Locator / Mayer's Singleton)
    // ============================================================================
    class LoggerManager {
    private:
        ILogger* active_logger;
        ConsoleLogger default_logger;

        LoggerManager() : active_logger(&default_logger) {}

    public:
        static LoggerManager& get() {
            static LoggerManager instance;
            return instance;
        }

        void set_logger(ILogger* new_logger) {
            if (new_logger) active_logger = new_logger;
        }

        ILogger& logger() {
            return *active_logger;
        }
    };

    // ============================================================================
    // 6. GLOBAL ROUTING FORWARDERS
    // ============================================================================
    inline std::mutex& get_console_mutex() {
        return LoggerManager::get().logger().get_console_mutex();
    }

    inline void info(const std::string& message) {
        LoggerManager::get().logger().info(message);
    }

    inline void raw(const std::string& message) {
        LoggerManager::get().logger().raw(message);
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

#endif // MKAPK_UI_HPP
