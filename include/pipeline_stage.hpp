#ifndef MKAPK_PIPELINE_STAGE_HPP
#define MKAPK_PIPELINE_STAGE_HPP

#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include "mkapk_config.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_result.hpp"

struct PipelineContext {
    std::vector<std::string> raw_args;
    bool is_release = false;
    bool force_all = false;
    bool ndk_all = false;
    std::string arch_target;

    std::filesystem::path bin_dir;
    std::filesystem::path build_dir;
    std::filesystem::path src_dir;
    std::filesystem::path res_dir;
    std::filesystem::path manifest_path;
    std::filesystem::path android_jar;

    std::map<std::string, std::string> tools;
    std::map<std::string, LanguagePlugin> active_plugins;
    RunFunc run_func;

    std::vector<std::string> all_resolved_artifacts;
    std::filesystem::path active_manifest_path;
    BuildResults diff;
    std::map<std::string, std::string> new_state;
    bool resources_triggered = false;
    std::vector<std::string> compile_architectures;
    
    std::string final_output_msg;
};

class PipelineStage {
public:
    virtual ~PipelineStage() = default;
    virtual Result<void> execute(const MkapkConfig& config, PipelineContext& ctx) = 0;
};

// FIX: Declare concrete pipeline stage implementations
class DependencyStage : public PipelineStage {
public:
    Result<void> execute(const MkapkConfig& config, PipelineContext& ctx) override;
};

class ResourceStage : public PipelineStage {
public:
    Result<void> execute(const MkapkConfig& config, PipelineContext& ctx) override;
};

class NativeStage : public PipelineStage {
public:
    Result<void> execute(const MkapkConfig& config, PipelineContext& ctx) override;
};

class JvmStage : public PipelineStage {
public:
    Result<void> execute(const MkapkConfig& config, PipelineContext& ctx) override;
};

class PackageStage : public PipelineStage {
public:
    Result<void> execute(const MkapkConfig& config, PipelineContext& ctx) override;
};

#endif // MKAPK_PIPELINE_STAGE_HPP