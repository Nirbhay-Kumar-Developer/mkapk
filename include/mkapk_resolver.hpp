#ifndef MKAPK_RESOLVER_HPP
#define MKAPK_RESOLVER_HPP

#include <string>
#include <vector>
#include "mkapk_config.hpp"

namespace MkapkResolver {
    /**
     * Spawns a dedicated connection loop to the Java Daemon, requests 
     * recursive dependency graph resolution, and translates absolute file paths.
     * 
     * @param coordinates The target Maven strings matrix
     * @param config The structural configuration reference container.
     * @return A vector list containing paths to cached on-device .aar and .jar artifacts.
     */
    std::vector<std::string> resolve_dependencies(const std::vector<std::string>& coordinates, const MkapkConfig& config);
}

#endif // MKAPK_RESOLVER_HPP