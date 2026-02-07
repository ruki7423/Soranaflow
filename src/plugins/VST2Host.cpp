#include "VST2Host.h"
#include "VST2Plugin.h"
#include "../core/dsp/IDSPProcessor.h"

#include <QDebug>

#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════
//  VST2Host implementation
// ═══════════════════════════════════════════════════════════════════════

VST2Host* VST2Host::instance()
{
    static VST2Host s;
    return &s;
}

void VST2Host::scanPlugins()
{
    m_plugins.clear();

    const char* home = std::getenv("HOME");
    if (home) {
        scanDirectory(std::string(home) + "/Library/Audio/Plug-Ins/VST");
    }
    scanDirectory("/Library/Audio/Plug-Ins/VST");
}

void VST2Host::scanDirectory(const std::string& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;

    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;

        std::string path = entry.path().string();
        std::string ext = entry.path().extension().string();

        if (ext != ".vst") continue;

        // Lightweight scan: name from filename only
        VST2PluginInfo info;
        info.path = path;
        info.name = entry.path().stem().string();
        info.vendor = "";

        qDebug() << "VST2 found:" << QString::fromStdString(info.name)
                 << "at" << QString::fromStdString(info.path);

        m_plugins.push_back(std::move(info));
    }

    std::sort(m_plugins.begin(), m_plugins.end(),
              [](const VST2PluginInfo& a, const VST2PluginInfo& b) {
                  return a.name < b.name;
              });
}

std::shared_ptr<IDSPProcessor> VST2Host::createProcessor(int pluginIndex)
{
    if (pluginIndex < 0 || pluginIndex >= (int)m_plugins.size())
        return nullptr;

    auto plugin = std::make_shared<VST2Plugin>();
    if (!plugin->loadFromPath(m_plugins[pluginIndex].path)) {
        qWarning() << "VST2: Failed to create processor for"
                    << QString::fromStdString(m_plugins[pluginIndex].name);
        return nullptr;
    }

    m_loadedPlugins.push_back(plugin);
    return plugin;
}

std::shared_ptr<IDSPProcessor> VST2Host::createProcessorFromPath(const std::string& path)
{
    for (int i = 0; i < (int)m_plugins.size(); ++i) {
        if (m_plugins[i].path == path)
            return createProcessor(i);
    }

    // Plugin not in scanned list — try loading directly
    auto plugin = std::make_shared<VST2Plugin>();
    if (!plugin->loadFromPath(path)) {
        return nullptr;
    }
    m_loadedPlugins.push_back(plugin);
    return plugin;
}

void VST2Host::unloadAll()
{
    qDebug() << "VST2Host: unloading all plugins";
    m_loadedPlugins.clear();
    m_plugins.clear();
}
