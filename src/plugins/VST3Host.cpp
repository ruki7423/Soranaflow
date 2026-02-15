#include "VST3Host.h"
#include "VST3Plugin.h"
#include "../core/dsp/IDSPProcessor.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QDebug>

#include "public.sdk/source/vst/hosting/module.h"

#include <filesystem>
#include <algorithm>
#include <set>

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

    // Dedup by canonical path + UID (resolves symlinks between user/system dirs)
    // Use path+uid as key since one bundle can contain multiple classes
    std::set<std::string> seen;
    auto it = m_plugins.begin();
    while (it != m_plugins.end()) {
        std::error_code ec;
        std::string canonical = fs::weakly_canonical(it->path, ec).string();
        if (ec) canonical = it->path;
        std::string key = canonical + "|" + it->uid;

        if (seen.count(key)) {
            qDebug() << "[VST3] Skipping duplicate:" << QString::fromStdString(it->name)
                     << "at" << QString::fromStdString(it->path);
            it = m_plugins.erase(it);
        } else {
            seen.insert(key);
            ++it;
        }
    }

    // Sort by name (once, after all directories scanned + deduped)
    std::sort(m_plugins.begin(), m_plugins.end(),
              [](const VST3PluginInfo& a, const VST3PluginInfo& b) {
                  return a.name < b.name;
              });

    qDebug() << "[VST3] Scan complete:" << m_plugins.size() << "unique plugins";
}

void VST3Host::scanDirectory(const std::string& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;

    // Recurse into subdirectories (e.g. /VST3/Vendor/Plugin.vst3)
    // but skip descending into .vst3 bundles themselves
    auto it = fs::recursive_directory_iterator(
        dir, fs::directory_options::skip_permission_denied, ec);
    for (; it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_directory()) continue;

        std::string ext = it->path().extension().string();

        // VST3 plugins are directories with .vst3 extension
        if (ext != ".vst3") continue;

        // .vst3 bundle found — don't recurse into it
        it.disable_recursion_pending();

        std::string bundlePath = it->path().string();
        std::string bundleName = it->path().stem().string();

        // Load the module to enumerate audio classes and read real metadata.
        // This gives us per-class entries (e.g. "Serum2 (Fx)" + "Serum2 (Instrument)")
        // and accurate vendor/UID information.
        std::string errorDesc;
        auto module = VST3::Hosting::Module::create(bundlePath, errorDesc);
        if (!module) {
            // Module failed to load — fall back to filename-only entry
            qDebug() << "VST3 scan: cannot load" << QString::fromStdString(bundleName)
                     << "-" << QString::fromStdString(errorDesc)
                     << "(adding as filename-only entry)";
            VST3PluginInfo info;
            info.path = bundlePath;
            info.name = bundleName;
            info.uid = bundlePath;
            m_plugins.push_back(std::move(info));
            continue;
        }

        auto& factory = module->getFactory();
        auto classInfos = factory.classInfos();

        // Collect audio effect classes
        std::vector<int> audioClassIndices;
        for (int i = 0; i < (int)classInfos.size(); ++i) {
            if (classInfos[i].category() == kVstAudioEffectClass) {
                audioClassIndices.push_back(i);
            }
        }

        if (audioClassIndices.empty()) {
            // No audio classes — add as filename-only entry
            qDebug() << "VST3 found:" << QString::fromStdString(bundleName)
                     << "(no audio classes) at" << QString::fromStdString(bundlePath);
            VST3PluginInfo info;
            info.path = bundlePath;
            info.name = bundleName;
            info.uid = bundlePath;
            m_plugins.push_back(std::move(info));
            continue;
        }

        bool multiClass = (audioClassIndices.size() > 1);

        for (int idx : audioClassIndices) {
            auto& ci = classInfos[idx];
            auto sub = ci.subCategoriesString();

            VST3PluginInfo info;
            info.path = bundlePath;
            info.vendor = ci.vendor();
            info.uid = ci.ID().toString();
            info.classIndex = idx;

            // Determine category from subcategories
            bool isInstrument = (sub.find("Instrument") != std::string::npos);
            bool isFx = (sub.find("Fx") != std::string::npos);
            info.isInstrument = isInstrument;
            info.isEffect = !isInstrument;

            if (isInstrument) info.category = "Instrument";
            else if (isFx) info.category = "Fx";

            // Name: append category only if bundle has multiple audio classes
            if (multiClass && !info.category.empty()) {
                info.name = ci.name() + " (" + info.category + ")";
            } else {
                info.name = ci.name();
            }

            qDebug() << "VST3 found:" << QString::fromStdString(info.name)
                     << "vendor:" << QString::fromStdString(info.vendor)
                     << "class:" << idx << "at" << QString::fromStdString(bundlePath);

            m_plugins.push_back(std::move(info));
        }

        // Module is released here — we only needed it for metadata
    }
}

std::shared_ptr<IDSPProcessor> VST3Host::createProcessor(int pluginIndex)
{
    if (pluginIndex < 0 || pluginIndex >= (int)m_plugins.size())
        return nullptr;

    auto plugin = std::make_shared<VST3Plugin>();
    if (!plugin->loadFromPath(m_plugins[pluginIndex].path,
                              m_plugins[pluginIndex].classIndex)) {
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

    // Find an already-loaded VST3Plugin instance via our weak references
    // (createProcessor/createProcessorFromPath always registers here)
    VST3Plugin* activePlugin = nullptr;
    for (auto it = m_loadedPlugins.begin(); it != m_loadedPlugins.end(); ) {
        if (auto sp = it->lock()) {
            if (sp->pluginPath() == info.path &&
                (info.uid.empty() || sp->pluginUID() == info.uid)) {
                activePlugin = sp.get();
                qDebug() << "VST3: Found loaded plugin instance";
                break;
            }
            ++it;
        } else {
            it = m_loadedPlugins.erase(it);
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
