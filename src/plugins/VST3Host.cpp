#include "VST3Host.h"
#include "VST3Plugin.h"
#include "../core/dsp/IDSPProcessor.h"
#include "../core/dsp/DSPPipeline.h"
#include "../core/audio/AudioEngine.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QDebug>

#include "public.sdk/source/vst/hosting/module.h"

#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════
//  VST3Host implementation
// ═══════════════════════════════════════════════════════════════════════

VST3Host* VST3Host::instance()
{
    static VST3Host s;
    return &s;
}

void VST3Host::scanPlugins()
{
    m_plugins.clear();

    // Standard macOS VST3 paths
    const char* home = std::getenv("HOME");
    if (home) {
        scanDirectory(std::string(home) + "/Library/Audio/Plug-Ins/VST3");
    }
    scanDirectory("/Library/Audio/Plug-Ins/VST3");
}

void VST3Host::scanDirectory(const std::string& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;

    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;

        std::string path = entry.path().string();
        std::string ext = entry.path().extension().string();

        // VST3 plugins are directories with .vst3 extension
        if (ext != ".vst3") continue;

        // Lightweight scan: extract name from filename only.
        // Do NOT load the module — loading triggers ObjC class
        // registration and iLok/PACE auth for every plugin,
        // causing duplicate-class warnings and startup delays.
        // Full metadata is read when the user loads a plugin.
        VST3PluginInfo info;
        info.path = path;
        info.name = entry.path().stem().string();
        info.vendor = "";
        info.uid = path;

        qDebug() << "VST3 found:" << QString::fromStdString(info.name)
                 << "at" << QString::fromStdString(info.path);

        m_plugins.push_back(std::move(info));
    }

    // Sort by name
    std::sort(m_plugins.begin(), m_plugins.end(),
              [](const VST3PluginInfo& a, const VST3PluginInfo& b) {
                  return a.name < b.name;
              });
}

std::shared_ptr<IDSPProcessor> VST3Host::createProcessor(int pluginIndex)
{
    if (pluginIndex < 0 || pluginIndex >= (int)m_plugins.size())
        return nullptr;

    auto plugin = std::make_shared<VST3Plugin>();
    if (!plugin->loadFromPath(m_plugins[pluginIndex].path)) {
        qWarning() << "VST3: Failed to create processor for"
                    << QString::fromStdString(m_plugins[pluginIndex].name);
        return nullptr;
    }

    // Keep a weak reference for editor access
    m_loadedPlugins.push_back(plugin);

    return plugin;
}

std::shared_ptr<IDSPProcessor> VST3Host::createProcessorFromPath(const std::string& path)
{
    for (int i = 0; i < (int)m_plugins.size(); ++i) {
        if (m_plugins[i].path == path)
            return createProcessor(i);
    }

    // Plugin not in scanned list — try loading directly
    auto plugin = std::make_shared<VST3Plugin>();
    if (!plugin->loadFromPath(path)) {
        return nullptr;
    }
    m_loadedPlugins.push_back(plugin);
    return plugin;
}

void VST3Host::closeAllEditors()
{
    qDebug() << "VST3Host: closing all plugin editors";
    for (auto it = m_loadedPlugins.begin(); it != m_loadedPlugins.end(); ) {
        if (auto sp = it->lock()) {
            sp->closeEditor();
            ++it;
        } else {
            it = m_loadedPlugins.erase(it);
        }
    }
}

void VST3Host::unloadAll()
{
    qDebug() << "VST3Host: unloading all plugins";
    closeAllEditors();
    m_loadedPlugins.clear();
    m_plugins.clear();
}

void VST3Host::openPluginEditor(int pluginIndex, QWidget* parent)
{
    if (pluginIndex < 0 || pluginIndex >= (int)m_plugins.size())
        return;

    const auto& info = m_plugins[pluginIndex];
    qDebug() << "=== VST3Host::openPluginEditor ===" << QString::fromStdString(info.name);

    // Try to find an already-loaded VST3Plugin instance
    VST3Plugin* activePlugin = nullptr;

    // 1. Check the DSP pipeline for an active processor matching this plugin
    auto* pipeline = AudioEngine::instance()->dspPipeline();
    if (pipeline) {
        for (int i = 0; i < pipeline->processorCount(); ++i) {
            auto* proc = pipeline->processor(i);
            if (proc) {
                auto* vst3proc = dynamic_cast<VST3Plugin*>(proc);
                if (vst3proc && vst3proc->pluginPath() == info.path) {
                    activePlugin = vst3proc;
                    qDebug() << "VST3: Found plugin in DSP pipeline at index" << i;
                    break;
                }
            }
        }
    }

    // 2. Check our weak references
    if (!activePlugin) {
        for (auto it = m_loadedPlugins.begin(); it != m_loadedPlugins.end(); ) {
            if (auto sp = it->lock()) {
                if (sp->pluginPath() == info.path) {
                    activePlugin = sp.get();
                    qDebug() << "VST3: Found plugin in weak reference list";
                    break;
                }
                ++it;
            } else {
                it = m_loadedPlugins.erase(it);
            }
        }
    }

    if (!activePlugin) {
        qDebug() << "VST3: No loaded instance found for" << QString::fromStdString(info.name);
        qDebug() << "VST3: Plugin must be added to active chain first (double-click in available list)";
    }

    // If we found a loaded plugin, delegate to its openEditor
    // (which handles native GUI, fallback to placeholder, and re-open)
    if (activePlugin) {
        activePlugin->openEditor(parent);
        return;
    }

    // No loaded plugin found — show a minimal info window
    auto* window = new QWidget(parent, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QString::fromStdString(info.name));
    window->resize(400, 200);

    auto* layout = new QVBoxLayout(window);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(12);
    layout->setContentsMargins(24, 24, 24, 24);

    auto* titleLabel = new QLabel(QString::fromStdString(info.name));
    titleLabel->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);

    auto* vendorLabel = new QLabel(QString::fromStdString(info.vendor));
    vendorLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #999;"));
    vendorLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(vendorLabel);

    auto* infoLabel = new QLabel(QStringLiteral(
        "Plugin is not loaded.\nAdd it to the active chain first."));
    infoLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 12px;"));
    infoLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(infoLabel);

    layout->addStretch();

    window->show();
    window->raise();
    window->activateWindow();
}
