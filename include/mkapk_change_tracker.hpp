#ifndef MKAPK_CHANGE_TRACKER_HPP
#define MKAPK_CHANGE_TRACKER_HPP

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include "mkapk_config.hpp"

namespace fs = std::filesystem;

// Explicit booleans controlling discrete pipeline requirements
struct BuildDecisionMatrix {
    bool needs_res_compile       = false; // XML/drawables changed
    bool needs_manifest_link     = false; // AndroidManifest.xml changed -> re-link APK
    bool needs_jvm_compile       = false; // Java/Kotlin code modified
    bool needs_dex               = false; // Bytecode modified
    bool needs_dex_release       = false; // Proguard rules changed -> re-run R8/Dex
    bool needs_res_obfuscation   = false; // AndResGuard config or resources changed (release only)
    bool needs_repackage         = false; // Assets, DEX, or .so changed
    bool needs_sign              = false; // Repackaged or re-linked container
};

// Interface for clean change evaluation and stage isolation
class IChangeDetector {
public:
    virtual ~IChangeDetector() = default;
    virtual std::pair<BuildDecisionMatrix, std::map<std::string, std::string>> evaluate(
        const fs::path& build_dir,
        const MkapkConfig& config,
        bool force_all,
        bool is_release
    ) = 0;
};

#endif // MKAPK_CHANGE_TRACKER_HPP
