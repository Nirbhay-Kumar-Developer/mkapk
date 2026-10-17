#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstdint>
#include <iomanip>
#include <cctype>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "mkapk_resolver.hpp"
#include "mkapk_ui.hpp"
#include "mkapk_ipc.hpp"
#include "mkapk_config.hpp"
#include "mkapk_result.hpp"
#include "mkapk_tools.hpp"

namespace fs = std::filesystem;

namespace MkapkResolver {

constexpr uint32_t GRAPH_MAGIC = 0x4D4B4447; // "MKDG" in Little-Endian
constexpr uint16_t GRAPH_VERSION = 1;

struct GraphNode {
    std::string group_id;
    std::string artifact_id;
    std::string version;
    std::string extension;
    std::string relative_path;
    fs::path file_path;

    std::string ga_key() const {
        return group_id + ":" + artifact_id;
    }
};

struct ParsedCoordinate {
    std::string group_id;
    std::string artifact_id;
    std::string version;
    std::string extension = "aar";
};

// 1. HELPERS & STREAM READERS

static std::vector<std::string> split_tokens(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static inline uint16_t read_u16_le(std::istream& is) {
    uint8_t b[2];
    is.read(reinterpret_cast<char*>(b), 2);
    if (!is) return 0;
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}

static inline uint32_t read_u32_le(std::istream& is) {
    uint8_t b[4];
    is.read(reinterpret_cast<char*>(b), 4);
    if (!is) return 0;
    return static_cast<uint32_t>(b[0]) |
           (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

// 2. DIRECTORY CONTENT HASHING & AAR CACHE VALIDATION

/**
 * Validates that an extracted AAR directory matches its recorded extracted_aar.hash.
 */
static bool validate_aar_cache(const fs::path& aar_path) {
    fs::path dep_dir = aar_path.parent_path();
    fs::path extracted_dir = dep_dir / "extracted_aar";
    fs::path hash_file = dep_dir / "extracted_aar.hash";

    if (fs::exists(extracted_dir)) {
        if (!fs::exists(hash_file) || !fs::exists(extracted_dir / "AndroidManifest.xml")) {
            return false;
        }

        std::ifstream hf(hash_file);
        std::string expected_hash;
        hf >> expected_hash;
        if (expected_hash.empty()) return false;

        return get_dir_hash(extracted_dir) == expected_hash;
    }

    return true;
}


// 3. BINARY GRAPH DESERIALIZATION

static bool read_dependency_graph(const fs::path& graph_path, const fs::path& cache_dir, std::vector<GraphNode>& out_nodes) {
    if (!fs::exists(graph_path) || fs::file_size(graph_path) < 10) {
        return false;
    }

    std::ifstream is(graph_path, std::ios::binary);
    if (!is.is_open()) return false;

    uint32_t magic = read_u32_le(is);
    if (magic != GRAPH_MAGIC) return false;

    uint16_t version = read_u16_le(is);
    if (version != GRAPH_VERSION) return false;

    uint32_t count = read_u32_le(is);
    if (count > 50000) return false;

    std::vector<GraphNode> nodes;
    nodes.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = read_u32_le(is);
        if (len == 0 || len > 65536) return false;

        std::string serialized(len, '\0');
        is.read(&serialized[0], len);
        if (!is) return false;

        auto tokens = split_tokens(serialized, ':');
        if (tokens.size() < 5) return false;

        GraphNode node;
        node.group_id = tokens[0];
        node.artifact_id = tokens[1];
        node.version = tokens[2];
        node.extension = tokens[3];
        node.relative_path = tokens[4];
        node.file_path = cache_dir / node.relative_path;

        // 1. Verify artifact exists on disk in global cache
        if (!fs::exists(node.file_path) || fs::file_size(node.file_path) == 0) {
            return false;
        }

        // 2. For AAR artifacts, validate extracted directory hash if present
        if (node.extension == "aar" && !validate_aar_cache(node.file_path)) {
            return false;
        }

        nodes.push_back(std::move(node));
    }

    out_nodes = std::move(nodes);
    return true;
}

// 4. COORDINATE PARSER & VERSION MEDIATION

static bool parse_coordinate(const std::string& raw, ParsedCoordinate& out) {
    std::string coord = trim(raw);
    auto parts = split_tokens(coord, ':');
    if (parts.size() < 3) return false;

    out.group_id = parts[0];
    out.artifact_id = parts[1];

    if (parts.size() == 3) {
        out.extension = (out.group_id.rfind("androidx.", 0) == 0) ? "aar" : "jar";
        out.version = parts[2];
    } else if (parts.size() == 4) {
        out.extension = parts[2];
        out.version = parts[3];
    } else {
        out.extension = parts[2];
        out.version = parts.back();
    }
    return true;
}

static std::vector<std::string> split_version_tokens(const std::string& v) {
    std::vector<std::string> parts;
    std::string cur;
    bool last_is_digit = false;

    for (char c : v) {
        if (c == '.' || c == '-' || c == '_') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            if (!cur.empty() && !last_is_digit) {
                parts.push_back(cur);
                cur.clear();
            }
            cur += c;
            last_is_digit = true;
        } else {
            if (!cur.empty() && last_is_digit) {
                parts.push_back(cur);
                cur.clear();
            }
            cur += c;
            last_is_digit = false;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

static int compare_versions(const std::string& v1, const std::string& v2) {
    if (v1 == v2) return 0;
    auto p1 = split_version_tokens(v1);
    auto p2 = split_version_tokens(v2);

    size_t max_len = std::max(p1.size(), p2.size());
    for (size_t i = 0; i < max_len; ++i) {
        std::string s1 = (i < p1.size()) ? p1[i] : "0";
        std::string s2 = (i < p2.size()) ? p2[i] : "0";

        bool is_num1 = !s1.empty() && std::all_of(s1.begin(), s1.end(), ::isdigit);
        bool is_num2 = !s2.empty() && std::all_of(s2.begin(), s2.end(), ::isdigit);

        if (is_num1 && is_num2) {
            long long n1 = std::stoll(s1);
            long long n2 = std::stoll(s2);
            if (n1 != n2) return (n1 > n2) ? 1 : -1;
        } else if (is_num1 && !is_num2) {
            return 1;
        } else if (!is_num1 && is_num2) {
            return -1;
        } else {
            if (s1 != s2) return (s1 > s2) ? 1 : -1;
        }
    }
    return 0;
}

// 5. PUBLIC RESOLVER INTERFACE

std::vector<std::string> resolve_dependencies(const std::vector<std::string>& coordinates, const MkapkConfig& /*config*/) {
    std::vector<std::string> resolved_paths;
    if (coordinates.empty()) return resolved_paths;

    fs::path local_cache = MkapkEnv::get_dependency_cache_dir();

    // --- PHASE 1: CHECK GLOBAL CACHE BINARY GRAPHS ---
    bool all_cached = true;
    std::vector<GraphNode> combined_cached_nodes;

    for (const auto& raw_coord : coordinates) {
        ParsedCoordinate pc;
        if (!parse_coordinate(raw_coord, pc)) {
            all_cached = false;
            break;
        }

        std::string group_path = pc.group_id;
        std::replace(group_path.begin(), group_path.end(), '.', '/');

        fs::path dep_dir = local_cache / group_path / pc.artifact_id / pc.version;
        fs::path graph_path = dep_dir / "graph.bin";

        std::vector<GraphNode> dep_nodes;
        if (read_dependency_graph(graph_path, local_cache, dep_nodes) && !dep_nodes.empty()) {
            combined_cached_nodes.insert(combined_cached_nodes.end(), dep_nodes.begin(), dep_nodes.end());
        } else {
            all_cached = false;
            break;
        }
    }

    // --- PHASE 2: SERVE DIRECTLY FROM CACHE GRAPH IF COMPLETE ---
    if (all_cached && !combined_cached_nodes.empty()) {

        // Version mediation: Highest version wins for each groupId:artifactId
        std::map<std::string, GraphNode> best_nodes;
        for (const auto& node : combined_cached_nodes) {
            std::string key = node.ga_key();
            auto it = best_nodes.find(key);
            if (it == best_nodes.end()) {
                best_nodes[key] = node;
            } else {
                if (compare_versions(node.version, it->second.version) > 0) {
                    it->second = node;
                }
            }
        }

        for (const auto& [ga, node] : best_nodes) {
            std::string fname = node.file_path.filename().string();
            // Drop legacy split Kotlin stdlib artifacts
            if (fname.find("kotlin-stdlib-jdk7") != std::string::npos ||
                fname.find("kotlin-stdlib-jdk8") != std::string::npos) {
                continue;
            }

            if (fs::exists(node.file_path)) {
                resolved_paths.push_back(node.file_path.string());
            }
        }

        std::sort(resolved_paths.begin(), resolved_paths.end());
        resolved_paths.erase(std::unique(resolved_paths.begin(), resolved_paths.end()), resolved_paths.end());
        return resolved_paths;
    }

    // --- PHASE 3: CACHE MISS - RESOLVE VIA DAEMON & PERSIST GRAPHS ---
    std::vector<std::string> daemon_args = {"resolve"};
    daemon_args.insert(daemon_args.end(), coordinates.begin(), coordinates.end());

    auto res = IpcClient::call_java_tool(daemon_args);
    if (res.is_err()) {
        UI::warn(std::string("Resolver notice: ") + res.get_error());
    }

    const auto& outputs = IpcClient::get_last_daemon_output();
    for (const auto& line : outputs) {
        if (line.rfind("MKAPK_RESOLVED|", 0) == 0) {
            std::stringstream ss(line.substr(15));
            std::string path_token;
            while (std::getline(ss, path_token, '|')) {
                std::string clean_path = trim(path_token);
                if (!clean_path.empty() && fs::exists(clean_path)) {
                    resolved_paths.push_back(clean_path);
                }
            }
        }
    }

    // --- PHASE 4: POST-RESOLUTION FALLBACK DISCOVERY ---
    if (resolved_paths.empty()) {
        for (const auto& coordinate : coordinates) {
            ParsedCoordinate pc;
            if (parse_coordinate(coordinate, pc)) {
                std::string group_path = pc.group_id;
                std::replace(group_path.begin(), group_path.end(), '.', '/');

                fs::path target_version_dir = local_cache / group_path / pc.artifact_id / pc.version;
                if (fs::exists(target_version_dir) && fs::is_directory(target_version_dir)) {
                    for (const auto& entry : fs::recursive_directory_iterator(target_version_dir)) {
                        if (entry.is_regular_file()) {
                            std::string ext = entry.path().extension().string();
                            if (ext == ".jar" || ext == ".aar") {
                                resolved_paths.push_back(entry.path().string());
                            }
                        }
                    }
                }
            }
        }
    }

    std::sort(resolved_paths.begin(), resolved_paths.end());
    resolved_paths.erase(std::unique(resolved_paths.begin(), resolved_paths.end()), resolved_paths.end());

    return resolved_paths;
}

} // namespace MkapkResolver
