#pragma once

#include <string>
#include <vector>
#include <memory>

class QWidget;
class IDSPProcessor;
class VST3Plugin;

// Info about a discovered VST3 plugin
struct VST3PluginInfo {
    std::string name;
    std::string vendor;
    std::string path;       // .vst3 bundle path
    std::string uid;        // unique plugin ID
    bool isInstrument = false;
    bool isEffect = true;
};

// Scans and loads VST3 plugins on macOS.
// VST3 bundles are searched in standard locations:
//   ~/Library/Audio/Plug-Ins/VST3/
//   /Library/Audio/Plug-Ins/VST3/
class VST3Host {
public:
    static VST3Host* instance();

    // Scan standard VST3 directories for plugins.
    // Uses the VST3 SDK Module API to load each bundle and read real metadata.
    void scanPlugins();

    // Get list of discovered plugins
    const std::vector<VST3PluginInfo>& plugins() const { return m_plugins; }

    // Create a real VST3 processor wrapper for a plugin by index.
    // Returns a fully loaded VST3Plugin that implements IDSPProcessor.
    std::shared_ptr<IDSPProcessor> createProcessor(int pluginIndex);

    // Create a real VST3 processor wrapper for a plugin by path.
    std::shared_ptr<IDSPProcessor> createProcessorFromPath(const std::string& path);

    // Open the plugin's native VST3 editor GUI window.
    // If the plugin supports IPlugView, opens the real GUI via NSView embedding.
    // Otherwise falls back to a placeholder window with bypass toggle.
    void openPluginEditor(int pluginIndex, QWidget* parent = nullptr);

    // Shutdown helpers: close all open editor windows and unload all plugins
    void closeAllEditors();
    void unloadAll();

private:
    VST3Host() = default;
    void scanDirectory(const std::string& dir);

    std::vector<VST3PluginInfo> m_plugins;

    // Keep references to loaded plugins for editor access
    std::vector<std::weak_ptr<VST3Plugin>> m_loadedPlugins;
};
