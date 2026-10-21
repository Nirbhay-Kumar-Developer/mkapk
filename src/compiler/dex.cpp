#include <zip.h>
#include <vector>
#include <zipconf.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <set>
#include <cstdlib>
#include <utility>

#include "mkapk_helpers.hpp"
#include "mkapk_tools.hpp"
#include "mkapk_result.hpp"
#include "mkapk_config.hpp"
#include "mkapk_ui.hpp"

namespace fs = std::filesystem;

/**
 * Utility to collect all compiled .class files.
 */
std::vector<std::string> get_all_class_files(const fs::path& bin_dir) {
    fs::path resolved_bin = fs::absolute(bin_dir);
    fs::path class_dir = resolved_bin / "classes" / "java_classes";

    std::vector<std::string> all_files;
    if (fs::exists(class_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(class_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".class") {
                all_files.push_back(fs::absolute(entry.path()).string());
            }
        }
    }
    return all_files;
}

/**
 * Converts .class files to .dex incrementally using D8 with Java 8+ desugaring
 * and supports external dependency classpaths (AAR/JAR).
 */
Result<void> run_incremental_dex(
    const std::string& D8,
    const fs::path& android_jar,
    const fs::path& src_path,
    const fs::path& java_out,
    const fs::path& dex_cache,
    const std::vector<fs::path>& files_to_dex,
    const std::vector<fs::path>& extra_jvm_classpaths,
    RunFunc run,
    const std::string& min_sdk)
{
    if (files_to_dex.empty()) return Result<void>::success();

    std::string min_api_val = min_sdk.empty() ? "21" : min_sdk;

    fs::path abs_src_path = fs::absolute(src_path);
    fs::path abs_java_out = fs::absolute(java_out);
    fs::path abs_gen_path = fs::absolute(abs_java_out.parent_path().parent_path() / "gen");

    std::vector<std::string> batched_gen_classes;
    std::set<std::string> seen_gen_families;
    std::set<std::string> seen_app_families;

    for (const auto& src_file : files_to_dex) {
        fs::path abs_file = fs::absolute(src_file);
        std::string abs_str = abs_file.string();

        bool is_gen = (abs_str.rfind(abs_gen_path.string(), 0) == 0);
        bool is_class_out = (abs_str.rfind(abs_java_out.string(), 0) == 0);

        fs::path rel_path;
        if (is_gen) {
            rel_path = fs::relative(abs_file, abs_gen_path);
        } else if (is_class_out) {
            rel_path = fs::relative(abs_file, abs_java_out);
        } else {
            rel_path = fs::relative(abs_file, abs_src_path);
        }

        fs::path class_dir = abs_java_out / rel_path.parent_path();
        std::string base_name = rel_path.stem().string();
        size_t dollar_pos = base_name.find('$');
        if (dollar_pos != std::string::npos) {
            base_name = base_name.substr(0, dollar_pos);
        }

        // Canonicalize base name so Foo and FooKt share the same family key
        std::string canonical_base = base_name;
        if (canonical_base.size() > 2 && canonical_base.rfind("Kt") == canonical_base.size() - 2) {
            canonical_base = canonical_base.substr(0, canonical_base.size() - 2);
        }

        std::string family_key = (rel_path.parent_path() / canonical_base).string();

        // Recognize R/Manifest classes even when passed as .class paths from java_out
        bool is_gen_family = is_gen || fs::exists(abs_gen_path / rel_path.parent_path() / (base_name + ".java"));

        // Helper to match classes belonging to this family (including Kotlin *Kt classes)
        auto is_member_of_family = [&](const std::string& filename) {
            if (filename == base_name + ".class" || filename.rfind(base_name + "$", 0) == 0) {
                return true;
            }
            std::string kt_name = base_name + "Kt";
            if (filename == kt_name + ".class" || filename.rfind(kt_name + "$", 0) == 0) {
                return true;
            }
            if (!canonical_base.empty() && canonical_base != base_name) {
                if (filename == canonical_base + ".class" || filename.rfind(canonical_base + "$", 0) == 0) {
                    return true;
                }
                std::string c_kt = canonical_base + "Kt";
                if (filename == c_kt + ".class" || filename.rfind(c_kt + "$", 0) == 0) {
                    return true;
                }
            }
            return false;
        };

        // Collect generated R.java / Manifest.java classes for a single batched D8 pass
        if (is_gen_family) {
            if (!seen_gen_families.insert(family_key).second) continue;
            if (fs::exists(class_dir)) {
                for (const auto& entry : fs::directory_iterator(class_dir)) {
                    std::string filename = entry.path().filename().string();
                    if (is_member_of_family(filename)) {
                        batched_gen_classes.push_back(fs::absolute(entry.path()).string());
                    }
                }
            }
            continue;
        }

        if (!seen_app_families.insert(family_key).second) continue;

        std::vector<std::string> family_classes;
        if (fs::exists(class_dir)) {
            for (const auto& entry : fs::directory_iterator(class_dir)) {
                std::string filename = entry.path().filename().string();
                if (is_member_of_family(filename)) {
                    family_classes.push_back(fs::absolute(entry.path()).string());
                }
            }
        }

        if (!family_classes.empty()) {
            fs::path target_dex_dir = dex_cache / rel_path.parent_path();
            fs::create_directories(target_dex_dir);

            // Clean up old DEX files for both base_name and base_nameKt
            for (const auto& entry : fs::directory_iterator(target_dex_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                    std::string stem = entry.path().stem().string();
                    bool is_stale = (stem == base_name || stem.rfind(base_name + "_", 0) == 0 ||
                                     stem == (base_name + "Kt") || stem.rfind(base_name + "Kt_", 0) == 0);
                    if (!canonical_base.empty() && canonical_base != base_name) {
                        if (stem == canonical_base || stem.rfind(canonical_base + "_", 0) == 0) {
                            is_stale = true;
                        }
                    }
                    if (is_stale) {
                        fs::remove(entry.path());
                    }
                }
            }

            fs::path temp_dex_dir = target_dex_dir / ("_tmp_" + base_name);
            if (fs::exists(temp_dex_dir)) fs::remove_all(temp_dex_dir);
            fs::create_directories(temp_dex_dir);

            std::vector<std::string> d8_args = {
                D8,
                "--min-api", min_api_val,
                "--lib", fs::absolute(android_jar).string(),
                "--classpath", abs_java_out.string(),
                "--output", fs::absolute(temp_dex_dir).string()
            };

            for (const auto& jar : extra_jvm_classpaths) {
                d8_args.push_back("--classpath");
                d8_args.push_back(fs::absolute(jar).string());
            }

            for (const auto& cls : family_classes) {
                d8_args.push_back(cls);
            }

            auto res = run(d8_args, "Incremental D8 failed for: " + base_name);
            if (res.is_err()) {
                fs::remove_all(temp_dex_dir);
                return res;
            }

            if (fs::exists(temp_dex_dir)) {
                for (const auto& entry : fs::directory_iterator(temp_dex_dir)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                        std::string filename = entry.path().filename().string();
                        fs::path dest_file;

                        if (filename == "classes.dex") {
                            dest_file = target_dex_dir / (base_name + ".dex");
                        } else if (filename.rfind("classes", 0) == 0) {
                            std::string suffix = filename.substr(7, filename.size() - 7 - 4);
                            dest_file = target_dex_dir / (base_name + "_" + suffix + ".dex");
                        } else {
                            dest_file = target_dex_dir / (base_name + "_" + filename);
                        }

                        fs::rename(entry.path(), dest_file);
                    }
                }
                fs::remove_all(temp_dex_dir);
            }
        }
    }

    // Batch-compile all generated R.class and Manifest.class files in a single D8 call
    if (!batched_gen_classes.empty()) {
        std::sort(batched_gen_classes.begin(), batched_gen_classes.end());
        batched_gen_classes.erase(std::unique(batched_gen_classes.begin(), batched_gen_classes.end()), batched_gen_classes.end());

        fs::path temp_gen_dir = dex_cache / "_tmp_gen_r";
        if (fs::exists(temp_gen_dir)) fs::remove_all(temp_gen_dir);
        fs::create_directories(temp_gen_dir);

        fs::path gen_list_file = dex_cache / "d8_gen_input.txt";
        std::ofstream gen_out(gen_list_file);
        for (const auto& cls : batched_gen_classes) {
            gen_out << cls << "\n";
        }
        gen_out.close();

        std::vector<std::string> d8_gen_args = {
            D8,
            "--min-api", min_api_val,
            "--lib", fs::absolute(android_jar).string(),
            "--output", fs::absolute(temp_gen_dir).string(),
            "@" + fs::absolute(gen_list_file).string()
        };

        auto res = run(d8_gen_args, "Batch D8 compilation failed for generated R classes");
        fs::remove(gen_list_file);
        if (res.is_err()) {
            fs::remove_all(temp_gen_dir);
            return res;
        }

        // Purge previous gen_r_classes*.dex before renaming fresh outputs
        if (fs::exists(dex_cache)) {
            for (const auto& entry : fs::directory_iterator(dex_cache)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                    if (entry.path().stem().string().rfind("gen_r_classes", 0) == 0) {
                        fs::remove(entry.path());
                    }
                }
            }
        }

        int gen_idx = 1;
        for (const auto& entry : fs::directory_iterator(temp_gen_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                fs::path target_name = (gen_idx == 1)
                    ? (dex_cache / "gen_r_classes.dex")
                    : (dex_cache / ("gen_r_classes" + std::to_string(gen_idx) + ".dex"));
                fs::rename(entry.path(), target_name);
                gen_idx++;
            }
        }
        fs::remove_all(temp_gen_dir);
    }

    return Result<void>::success();
}

/**
 * Optimizes and shrinks bytecode for release using R8:
 * - Employs Whole-Program Tree Shaking & Inlining
 * - Automates keep-rules generation by merging AAPT2 and user rules
 * - Exports mapping.txt for de-obfuscation
 * - Uses Arg-Files to prevent E2BIG overflow on large codebases
 */

Result<void> run_dex_r8(
    const std::string& R8_TOOL,
    const fs::path& android_jar,
    const MkapkConfig& config,
    const fs::path& bin_dir,
    RunFunc run_func,
    bool no_obs,
    const std::vector<fs::path>& extra_dependency_jars)
{
    fs::path bin_dir_path = fs::absolute(bin_dir);
    std::vector<std::string> class_files = get_all_class_files(bin_dir_path);

    if (class_files.empty()) {
        return Result<void>::error(UI::Msg::FATAL_INTERNAL);
    }

    const char* prefix_env = std::getenv("PREFIX");
    fs::path kotlin_lib_root = prefix_env ? fs::path(prefix_env) / "opt/kotlin/lib/"
                                          : "/data/data/com.termux/files/usr/opt/kotlin/lib/";
    fs::path annotations_jar = kotlin_lib_root / "annotations-13.0.jar";

    std::string min_api_val = config.min_sdk.empty() ? "21" : config.min_sdk;

    std::vector<std::string> args = {
        R8_TOOL,
        "--release",
        "--min-api", min_api_val,
        "--lib", fs::absolute(android_jar).string(),
        "--output", bin_dir_path.string()
    };

    if (fs::exists(annotations_jar)) {
        args.push_back("--lib");
        args.push_back(fs::absolute(annotations_jar).string());
    }

    // Baseline fallback rules to suppress optional annotations and allow whole-program tree shaking
    fs::path baseline_rules = bin_dir_path / "mkapk_baseline_rules.pro";
    std::ofstream baseline_file(baseline_rules);
    if (baseline_file.is_open()) {
        baseline_file << "-dontwarn **\n";
        baseline_file << "-ignorewarnings\n";
        baseline_file.close();
        args.push_back("--pg-conf");
        args.push_back(baseline_rules.string());
    }

    if (!no_obs) {
        // 1. AAPT2 generated keep rules (manifest components, XML view references)
        fs::path aapt_rules = bin_dir_path / "aapt_rules.pro";
        if (fs::exists(aapt_rules)) {
            args.push_back("--pg-conf");
            args.push_back(fs::absolute(aapt_rules).string());
        }

        // 2. Discover and forward all library consumer rules for active project dependencies
        std::set<std::string> seen_rules;
        std::vector<std::string> rule_filenames = {
            "proguard.txt",
            "consumer-rules.pro",
            "proguard.pro"
        };

        // Path A: Inspect directories of supplied dependency JARs/AARs (<dependency_dir>/extracted_aar/)
        for (const auto& jar : extra_dependency_jars) {
            fs::path dir = jar.parent_path();
            for (const auto& name : rule_filenames) {
                fs::path candidate = dir / name;
                if (fs::exists(candidate) && fs::file_size(candidate) > 0) {
                    std::string abs_rule = fs::absolute(candidate).string();
                    if (seen_rules.insert(abs_rule).second) {
                        args.push_back("--pg-conf");
                        args.push_back(abs_rule);
                    }
                }
            }

            // Path B: Extract embedded ProGuard rules from META-INF/proguard/ inside JAR files
            if (fs::exists(jar) && jar.extension() == ".jar") {
                int err = 0;
                zip_t* archive = zip_open(jar.string().c_str(), 0, &err);
                if (archive) {
                    zip_int64_t num_entries = zip_get_num_entries(archive, 0);
                    for (zip_int64_t i = 0; i < num_entries; ++i) {
                        const char* entry_name = zip_get_name(archive, i, 0);
                        if (!entry_name) continue;

                        std::string entry_str(entry_name);
                        if (entry_str.rfind("META-INF/proguard/", 0) == 0 && entry_str.back() != '/') {
                            fs::path rule_dest_dir = bin_dir_path / "extracted_proguard_rules" / jar.stem();
                            fs::create_directories(rule_dest_dir);

                            fs::path out_rule_path = rule_dest_dir / fs::path(entry_str).filename();
                            zip_file_t* zf = zip_fopen_index(archive, i, 0);
                            if (zf) {
                                std::ofstream out_f(out_rule_path, std::ios::binary);
                                if (out_f.is_open()) {
                                    char buf[4096];
                                    zip_int64_t bytes = 0;
                                    while ((bytes = zip_fread(zf, buf, sizeof(buf))) > 0) {
                                        out_f.write(buf, bytes);
                                    }
                                    out_f.close();
                                }
                                zip_fclose(zf);

                                if (fs::exists(out_rule_path) && fs::file_size(out_rule_path) > 0) {
                                    std::string abs_rule = fs::absolute(out_rule_path).string();
                                    if (seen_rules.insert(abs_rule).second) {
                                        args.push_back("--pg-conf");
                                        args.push_back(abs_rule);
                                    }
                                }
                            }
                        }
                    }
                    zip_close(archive);
                }
            }
        }

        // 3. User-defined ProGuard rules
        if (!config.proguard_rules.empty()) {
            fs::path pg_rules = fs::absolute(MkapkEnv::resolve_path(config.proguard_rules));
            if (fs::exists(pg_rules)) {
                args.push_back("--pg-conf");
                args.push_back(pg_rules.string());
            } else if (fs::exists(fs::current_path() / config.proguard_rules)) {
                args.push_back("--pg-conf");
                args.push_back(fs::absolute(fs::current_path() / config.proguard_rules).string());
            }
        }

        // 4. Obfuscation mapping output
        fs::path mapping_file = bin_dir_path / "mapping.txt";
        args.push_back("--pg-map-output");
        args.push_back(mapping_file.string());
    }

    // 5. Batch program inputs into response file
    fs::path input_list_file = bin_dir_path / "r8_inputs.txt";
    std::ofstream input_file(input_list_file);
    if (input_file.is_open()) {
        for (const auto& file : class_files) {
            input_file << file << "\n";
        }
        std::set<std::string> seen_jars;
        for (const auto& dep_jar : extra_dependency_jars) {
            if (fs::exists(dep_jar)) {
                std::string abs_jar = fs::absolute(dep_jar).string();
                if (seen_jars.insert(abs_jar).second) {
                    input_file << abs_jar << "\n";
                }
            }
        }
        input_file.close();
        args.push_back("@" + input_list_file.string());
    } else {
        for (const auto& file : class_files) {
            args.push_back(file);
        }
        for (const auto& dep_jar : extra_dependency_jars) {
            if (fs::exists(dep_jar)) {
                args.push_back(fs::absolute(dep_jar).string());
            }
        }
    }

    auto res = run_func(args, "R8 optimization failed");
    if (res.is_err()) return res;

    return Result<void>::success();
}

/**
 * Merges cached incremental DEX files or raw class files using D8.
 */
Result<void> run_dex_d8(
    const std::string& D8_TOOL,
    const fs::path& android_jar,
    const fs::path& bin_dir,
    const fs::path& dex_cache,
    RunFunc run_func)
{
    fs::path bin_dir_path = fs::absolute(bin_dir);
    std::vector<std::string> inputs;

    // 1. Gather all cached incremental .dex files
    if (fs::exists(dex_cache)) {
        for (const auto& entry : fs::recursive_directory_iterator(dex_cache)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dex") {
                inputs.push_back(fs::absolute(entry.path()).string());
            }
        }
    }

    // 2. Fallback: If cache is empty, gather all compiled .class files
    if (inputs.empty()) {
        inputs = get_all_class_files(bin_dir_path);
    }

    if (inputs.empty()) {
        return Result<void>::error("No .dex or .class files found for D8. Check compiler output.");
    }

    std::vector<std::string> args = {
        D8_TOOL,
        "--min-api", "21", // Automates Java 8+ language feature desugaring
        "--lib", fs::absolute(android_jar).string(),
        "--output", bin_dir_path.string()
    };

    // Prevent ARG_MAX overflow during intermediate DEX merge
    fs::path merge_list_file = bin_dir_path / "d8_inputs.txt";
    std::ofstream merge_file(merge_list_file);
    if (merge_file.is_open()) {
        for (const auto& input : inputs) {
            merge_file << input << "\n";
        }
        merge_file.close();
        args.push_back("@" + merge_list_file.string());
    } else {
        for (const auto& input : inputs) {
            args.push_back(input);
        }
    }

    auto res = run_func(args, "D8 merge failed");
    if (res.is_err()) return res;

    return Result<void>::success();
}
