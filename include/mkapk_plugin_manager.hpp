#ifndef MKAPK_PLUGIN_MANAGER_HPP
#define MKAPK_PLUGIN_MANAGER_HPP

#include <string>
#include <map>
#include "mkapk_tools.hpp" // Required for the LanguagePlugin struct definition

namespace MkapkPluginManager {

    /**
     * Unpacks, cryptographically validates, resolves dependencies via apt,
     * and writes verified plugin definitions to the storage cache registry.
     * 
     * @param pl_package_path The path to the downloaded .pl bundle.
     * @return true if successful, false otherwise.
     */
    bool install_plugin(const std::string& pl_package_path);

    /**
     * Clears systemic structural cache footprints of an isolated plugin safely.
     * 
     * @param plugin_name The identifier of the plugin to remove.
     * @return true if successful, false otherwise.
     */
    bool uninstall_plugin(const std::string& plugin_name);

    /**
     * Scans and initializes the active collection of LanguagePlugin objects from cache directory files.
     * 
     * @return A map of source extensions (e.g., ".java") to their LanguagePlugin configurations.
     */
    std::map<std::string, LanguagePlugin> load_installed_plugins();

}

#endif // MKAPK_PLUGIN_MANAGER_HPP