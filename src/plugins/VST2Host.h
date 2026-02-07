#pragma once

#include <string>
#include <vector>
#include <memory>

class IDSPProcessor;
class VST2Plugin;

struct VST2PluginInfo {
    std::string name;
    std::string vendor;
    std::string path;   // .vst bundle path
};

// Scans and loads VST2 plugins on macOS.
// VST2 bundles (.vst) are searched in standard locations:
//   ~/Library/Audio/Plug-Ins/VST/
//   /Library/Audio/Plug-Ins/VST/
class VST2Host {
public:
    static VST2Host* instance();

    void scanPlugins();
    const std::vector<VST2PluginInfo>& plugins() const { return m_plugins; }

    std::shared_ptr<IDSPProcessor> createProcessor(int pluginIndex);
    std::shared_ptr<IDSPProcessor> createProcessorFromPath(const std::string& path);

    void unloadAll();

private:
    VST2Host() = default;
    void scanDirectory(const std::string& dir);

    std::vector<VST2PluginInfo> m_plugins;
    std::vector<std::weak_ptr<VST2Plugin>> m_loadedPlugins;
};
