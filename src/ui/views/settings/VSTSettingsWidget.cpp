#include "VSTSettingsWidget.h"
#include "SettingsUtils.h"
#include "../../../core/ThemeManager.h"
#include "../../../core/Settings.h"
#include "../../../core/audio/AudioEngine.h"
#include "../../../core/dsp/DSPPipeline.h"
#include "../../../plugins/VST3Host.h"
#include "../../../plugins/VST2Host.h"
#include "../../../plugins/VST2Plugin.h"
#include "../../../widgets/StyledButton.h"
#include "../../../widgets/StyledComboBox.h"
#include "../../dialogs/StyledMessageBox.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QFileDialog>
#include <QTimer>
#include <QLabel>
#include <QFrame>

VSTSettingsWidget::VSTSettingsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    createVSTCard(layout);
    loadVstPlugins();
}

// ═════════════════════════════════════════════════════════════════════
//  saveVstPlugins / loadVstPlugins
// ═════════════════════════════════════════════════════════════════════

void VSTSettingsWidget::saveVstPlugins()
{
    QStringList paths;
    for (int i = 0; i < m_vst3ActiveList->count(); ++i) {
        auto* item = m_vst3ActiveList->item(i);
        QString path = item->data(Qt::UserRole + 1).toString();
        if (!path.isEmpty())
            paths.append(path);
    }
    Settings::instance()->setActiveVstPlugins(paths);
}

void VSTSettingsWidget::loadVstPlugins()
{
    QStringList paths = Settings::instance()->activeVstPlugins();
    if (paths.isEmpty()) return;

    // Scan plugins first so we can match paths
    auto* host = VST3Host::instance();
    if (host->plugins().empty())
        host->scanPlugins();

    // Also ensure VST2 plugins are scanned
    auto* vst2host = VST2Host::instance();
    if (vst2host->plugins().empty())
        vst2host->scanPlugins();

    // If plugins were already loaded at startup (initializeDeferred),
    // skip pipeline insertion — only populate the UI list.
    auto* pipeline = AudioEngine::instance()->dspPipeline();
    bool alreadyLoaded = pipeline && pipeline->processorCount() > 0;

    for (const QString& path : paths) {
        bool isVst2 = path.endsWith(QStringLiteral(".vst"));

        // Only create + add processor if not loaded at startup
        if (!alreadyLoaded) {
            std::shared_ptr<IDSPProcessor> proc;
            if (isVst2) {
                proc = vst2host->createProcessorFromPath(path.toStdString());
            } else {
                proc = host->createProcessorFromPath(path.toStdString());
            }
            if (!proc) continue;
            if (pipeline) {
                pipeline->addProcessor(proc);
            }
        }

        // Find the plugin info for display name
        QString displayName = path;
        int pluginIndex = -1;

        if (isVst2) {
            const auto& plugins = vst2host->plugins();
            for (int i = 0; i < (int)plugins.size(); ++i) {
                if (plugins[i].path == path.toStdString()) {
                    displayName = QString::fromStdString(plugins[i].name);
                    pluginIndex = i;
                    break;
                }
            }
        } else {
            const auto& plugins = host->plugins();
            for (int i = 0; i < (int)plugins.size(); ++i) {
                if (plugins[i].path == path.toStdString()) {
                    displayName = QString::fromStdString(plugins[i].name);
                    if (!plugins[i].vendor.empty())
                        displayName += QStringLiteral(" (")
                            + QString::fromStdString(plugins[i].vendor)
                            + QStringLiteral(")");
                    pluginIndex = i;
                    break;
                }
            }
        }

        auto* activeItem = new QListWidgetItem(displayName);
        activeItem->setData(Qt::UserRole, pluginIndex);
        activeItem->setData(Qt::UserRole + 1, path);
        activeItem->setCheckState(Qt::Checked);
        m_vst3ActiveList->addItem(activeItem);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  createVSTCard — modern VST3 Plugins card
// ═════════════════════════════════════════════════════════════════════

QWidget* VSTSettingsWidget::createVSTCard(QVBoxLayout* parentLayout)
{
    auto* vstCard = new QFrame();
    vstCard->setObjectName(QStringLiteral("VSTCard"));
    {
        auto c = ThemeManager::instance()->colors();
        vstCard->setStyleSheet(QStringLiteral(
            "QFrame#VSTCard {"
            "  background: %1;"
            "  border-radius: 16px;"
            "  border: 1px solid %2;"
            "}")
                .arg(c.backgroundSecondary, c.border));
    }

    auto* vstLayout = new QVBoxLayout(vstCard);
    vstLayout->setContentsMargins(24, 24, 24, 24);
    vstLayout->setSpacing(16);

    // VST Header
    auto* vstTitle = new QLabel(QStringLiteral("Plugins"), vstCard);
    vstTitle->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: bold; color: %1; border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foreground));
    vstLayout->addWidget(vstTitle);

    // Scan button - scans both VST2 and VST3
    auto* scanPluginsBtn = new StyledButton(QStringLiteral("Scan for Plugins"),
                                             QStringLiteral("default"), vstCard);
    scanPluginsBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Primary));

    connect(scanPluginsBtn, &QPushButton::clicked, this, [this]() {
        // Scan VST3
        VST3Host::instance()->scanPlugins();
        if (m_vst3AvailableList) {
            m_vst3AvailableList->clear();
            const auto& plugins = VST3Host::instance()->plugins();
            for (int i = 0; i < (int)plugins.size(); ++i) {
                QString displayName = QString::fromStdString(plugins[i].name);
                if (!plugins[i].vendor.empty())
                    displayName += QStringLiteral(" (")
                        + QString::fromStdString(plugins[i].vendor)
                        + QStringLiteral(")");
                auto* item = new QListWidgetItem(displayName);
                item->setData(Qt::UserRole, i);
                item->setData(Qt::UserRole + 1,
                    QString::fromStdString(plugins[i].path));
                m_vst3AvailableList->addItem(item);
            }
            if (plugins.empty()) {
                auto* hint = new QListWidgetItem(QStringLiteral("No VST3 plugins found"));
                hint->setFlags(Qt::NoItemFlags);
                hint->setForeground(QColor(128, 128, 128));
                m_vst3AvailableList->addItem(hint);
            }
        }
        // Scan VST2
        VST2Host::instance()->scanPlugins();
        if (m_vst2AvailableList) {
            m_vst2AvailableList->clear();
            const auto& plugins = VST2Host::instance()->plugins();
            for (int i = 0; i < (int)plugins.size(); ++i) {
                auto* item = new QListWidgetItem(
                    QString::fromStdString(plugins[i].name));
                item->setData(Qt::UserRole, i);
                item->setData(Qt::UserRole + 1,
                    QString::fromStdString(plugins[i].path));
                m_vst2AvailableList->addItem(item);
            }
            if (plugins.empty()) {
                auto* hint = new QListWidgetItem(QStringLiteral("No VST2 plugins found"));
                hint->setFlags(Qt::NoItemFlags);
                hint->setForeground(QColor(128, 128, 128));
                m_vst2AvailableList->addItem(hint);
            }
        }
    });
    vstLayout->addWidget(scanPluginsBtn);

    // Modern list style
    auto c = ThemeManager::instance()->colors();
    QString vstListStyle = QStringLiteral(
        "QListWidget {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 12px;"
        "  padding: 8px;"
        "}"
        "QListWidget::item {"
        "  background: transparent;"
        "  border-radius: 8px;"
        "  padding: 10px;"
        "  margin: 2px 0;"
        "  color: %3;"
        "}"
        "QListWidget::item:hover {"
        "  background: %4;"
        "}"
        "QListWidget::item:selected {"
        "  background: %5;"
        "  border: 1px solid %6;"
        "}")
            .arg(c.background, c.border, c.foreground, c.hover, c.accentMuted, c.accent);

    // ── VST3 Available ──
    auto* vst3Label = new QLabel(QStringLiteral("VST3"), vstCard);
    vst3Label->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 600; color: %1;"
        " border: none; background: transparent;")
            .arg(c.foregroundSecondary));
    vstLayout->addWidget(vst3Label);

    m_vst3AvailableList = new QListWidget(vstCard);
    m_vst3AvailableList->setMinimumHeight(80);
    m_vst3AvailableList->setMaximumHeight(150);
    m_vst3AvailableList->setStyleSheet(vstListStyle);
    {
        auto* hint = new QListWidgetItem(
            QStringLiteral("Click \"Scan for Plugins\" to detect installed VST3 plugins"));
        hint->setFlags(Qt::NoItemFlags);
        hint->setForeground(QColor(128, 128, 128));
        m_vst3AvailableList->addItem(hint);
    }
    vstLayout->addWidget(m_vst3AvailableList);

    // Double-click to add
    connect(m_vst3AvailableList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsEnabled)) return;

        QString pluginPath = item->data(Qt::UserRole + 1).toString();
        if (pluginPath.isEmpty()) return;

        // Skip if already in active list
        for (int i = 0; i < m_vst3ActiveList->count(); ++i) {
            if (m_vst3ActiveList->item(i)->data(Qt::UserRole + 1).toString() == pluginPath)
                return;
        }

        int pluginIndex = item->data(Qt::UserRole).toInt();
        QString pluginName = item->text();

        auto* host = VST3Host::instance();
        auto proc = host->createProcessor(pluginIndex);
        if (!proc) {
            qWarning() << "[VST3] Double-click: failed to create processor for"
                        << pluginName;
            StyledMessageBox::warning(this, QStringLiteral("VST3 Plugin Error"),
                QStringLiteral("Failed to load \"%1\".\n\n"
                "The plugin may be incompatible, damaged, or blocked by macOS security.\n"
                "Try right-clicking the plugin in Finder → Open to allow it.").arg(pluginName));
            return;
        }

        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) pipeline->addProcessor(proc);

        auto* activeItem = new QListWidgetItem(pluginName);
        activeItem->setData(Qt::UserRole, pluginIndex);
        activeItem->setData(Qt::UserRole + 1, pluginPath);
        activeItem->setCheckState(Qt::Checked);
        m_vst3ActiveList->addItem(activeItem);

        saveVstPlugins();
    });

    // ── VST2 Available ──
    auto* vst2Label = new QLabel(QStringLiteral("VST2"), vstCard);
    vst2Label->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 600; color: %1;"
        " border: none; background: transparent;")
            .arg(c.foregroundSecondary));
    vstLayout->addWidget(vst2Label);

    m_vst2AvailableList = new QListWidget(vstCard);
    m_vst2AvailableList->setMinimumHeight(80);
    m_vst2AvailableList->setMaximumHeight(150);
    m_vst2AvailableList->setStyleSheet(vstListStyle);
    {
        auto* hint = new QListWidgetItem(
            QStringLiteral("Click \"Scan for Plugins\" to detect installed VST2 plugins"));
        hint->setFlags(Qt::NoItemFlags);
        hint->setForeground(QColor(128, 128, 128));
        m_vst2AvailableList->addItem(hint);
    }
    vstLayout->addWidget(m_vst2AvailableList);

    // Double-click VST2 to add
    connect(m_vst2AvailableList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsEnabled)) return;

        QString pluginPath = item->data(Qt::UserRole + 1).toString();
        if (pluginPath.isEmpty()) return;

        // Skip if already in active list
        for (int i = 0; i < m_vst3ActiveList->count(); ++i) {
            if (m_vst3ActiveList->item(i)->data(Qt::UserRole + 1).toString() == pluginPath)
                return;
        }

        int pluginIndex = item->data(Qt::UserRole).toInt();
        QString pluginName = item->text();

        auto proc = VST2Host::instance()->createProcessor(pluginIndex);
        if (!proc) {
            qWarning() << "[VST2] Double-click: failed to create processor for"
                        << pluginName;
            StyledMessageBox::warning(this, QStringLiteral("VST2 Plugin Error"),
                QStringLiteral("Failed to load \"%1\".\n\n"
                "The plugin may be incompatible, damaged, or blocked by macOS security.\n"
                "Try right-clicking the plugin in Finder → Open to allow it.").arg(pluginName));
            return;
        }

        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) pipeline->addProcessor(proc);

        auto* activeItem = new QListWidgetItem(pluginName);
        activeItem->setData(Qt::UserRole, pluginIndex);
        activeItem->setData(Qt::UserRole + 1, pluginPath);
        activeItem->setCheckState(Qt::Checked);
        m_vst3ActiveList->addItem(activeItem);

        saveVstPlugins();
    });

    // Active plugins label
    auto* activeLabel = new QLabel(QStringLiteral("Active Plugins"), vstCard);
    activeLabel->setStyleSheet(QStringLiteral(
        "font-size: 14px; font-weight: 600; color: %1;"
        " border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foreground));
    vstLayout->addWidget(activeLabel);

    // Active plugins list (with hint overlay)
    auto* activeContainer = new QWidget(vstCard);
    activeContainer->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    auto* activeStack = new QVBoxLayout(activeContainer);
    activeStack->setContentsMargins(0, 0, 0, 0);
    activeStack->setSpacing(0);

    m_vst3ActiveList = new QListWidget(activeContainer);
    m_vst3ActiveList->setMinimumHeight(60);
    m_vst3ActiveList->setMaximumHeight(120);
    m_vst3ActiveList->setDragDropMode(QAbstractItemView::InternalMove);
    m_vst3ActiveList->setStyleSheet(vstListStyle);
    activeStack->addWidget(m_vst3ActiveList);

    auto* activeHintLabel = new QLabel(
        QStringLiteral("Double-click a scanned plugin to activate it"), activeContainer);
    activeHintLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-style: italic; font-size: 12px; padding: 8px;"
        " background: transparent; border: none;")
            .arg(c.foregroundMuted));
    activeHintLabel->setAlignment(Qt::AlignCenter);
    activeStack->addWidget(activeHintLabel);

    // Hide hint when active list has items, show when empty
    auto updateHint = [activeHintLabel, this]() {
        activeHintLabel->setVisible(m_vst3ActiveList->count() == 0);
    };
    connect(m_vst3ActiveList->model(), &QAbstractItemModel::rowsInserted,
            activeHintLabel, updateHint);
    connect(m_vst3ActiveList->model(), &QAbstractItemModel::rowsRemoved,
            activeHintLabel, updateHint);

    vstLayout->addWidget(activeContainer);

    // Enable/disable via checkbox
    connect(m_vst3ActiveList, &QListWidget::itemChanged,
            this, [](QListWidgetItem* item) {
        if (!item) return;
        int pipelineIdx = item->listWidget()->row(item);
        bool enabled = item->checkState() == Qt::Checked;
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) {
            auto* proc = pipeline->processor(pipelineIdx);
            if (proc) proc->setEnabled(enabled);
            pipeline->notifyConfigurationChanged();
        }
    });

    // Double-click to open editor (VST3 or VST2)
    connect(m_vst3ActiveList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
        if (!item) return;
        QString pluginPath = item->data(Qt::UserRole + 1).toString();
        int row = m_vst3ActiveList->row(item);
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (!pipeline) return;
        auto* proc = pipeline->processor(row);
        if (!proc) return;

        if (auto* vst2 = dynamic_cast<VST2Plugin*>(proc)) {
            if (vst2->hasEditor()) vst2->openEditor(this);
        } else {
            // VST3 — use host's editor open (handles loaded instance lookup)
            int pluginIndex = item->data(Qt::UserRole).toInt();
            VST3Host::instance()->openPluginEditor(pluginIndex, this);
        }
    });

    // Button row
    auto* btnRow = new QWidget(vstCard);
    btnRow->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    auto* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(0, 4, 0, 0);
    btnLayout->setSpacing(8);

    auto* openEditorBtn = new StyledButton(QStringLiteral("Open Editor"),
                                            QStringLiteral("outline"), vstCard);
    openEditorBtn->setFixedSize(110, 32);
    openEditorBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    openEditorBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary)
        + QStringLiteral(" QPushButton { min-width: 110px; max-width: 110px; min-height: 32px; max-height: 32px; }"));
    connect(openEditorBtn, &QPushButton::clicked, this, [this]() {
        auto* item = m_vst3ActiveList->currentItem();
        if (!item) return;
        int row = m_vst3ActiveList->row(item);
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (!pipeline) return;
        auto* proc = pipeline->processor(row);
        if (!proc) return;

        if (auto* vst2 = dynamic_cast<VST2Plugin*>(proc)) {
            if (vst2->hasEditor()) vst2->openEditor(this);
        } else {
            int pluginIndex = item->data(Qt::UserRole).toInt();
            VST3Host::instance()->openPluginEditor(pluginIndex, this);
        }
    });
    btnLayout->addWidget(openEditorBtn);

    auto* removePluginBtn = new StyledButton(QStringLiteral("Remove"),
                                              QStringLiteral("outline"), vstCard);
    removePluginBtn->setFixedSize(90, 32);
    removePluginBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    removePluginBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Destructive)
        + QStringLiteral(" QPushButton { min-width: 90px; max-width: 90px; min-height: 32px; max-height: 32px; }"));
    connect(removePluginBtn, &QPushButton::clicked, this, [this]() {
        auto* item = m_vst3ActiveList->currentItem();
        if (!item) return;
        int row = m_vst3ActiveList->row(item);
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) pipeline->removeProcessor(row);
        delete m_vst3ActiveList->takeItem(row);
        saveVstPlugins();
    });
    btnLayout->addWidget(removePluginBtn);
    btnLayout->addStretch();

    vstLayout->addWidget(btnRow);

    parentLayout->addWidget(vstCard);
    return vstCard;
}
