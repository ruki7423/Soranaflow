#include "SettingsView.h"
#include "settings/AudioSettingsTab.h"
#include "settings/LibrarySettingsTab.h"
#include "settings/AppleMusicSettingsTab.h"
#include "settings/AppearanceSettingsTab.h"
#include "settings/AboutSettingsTab.h"
#include "../../core/ThemeManager.h"
#include <QVBoxLayout>
#include <QLabel>

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

SettingsView::SettingsView(QWidget* parent)
    : QWidget(parent)
{
    setupUI();

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SettingsView::refreshTheme);
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI
// ═════════════════════════════════════════════════════════════════════

void SettingsView::setupUI()
{
    setObjectName(QStringLiteral("SettingsView"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    // ── Header ─────────────────────────────────────────────────────
    auto* headerLabel = new QLabel(QStringLiteral("Settings"), this);
    headerLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 24px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    mainLayout->addWidget(headerLabel);

    // ── Tab Widget ─────────────────────────────────────────────────
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setObjectName(QStringLiteral("SettingsTabWidget"));

    m_tabWidget->addTab(new AudioSettingsTab(), tr("Audio"));
    m_tabWidget->addTab(new LibrarySettingsTab(), tr("Library"));
    m_tabWidget->addTab(new AppleMusicSettingsTab(), tr("Apple Music"));
    // m_tabWidget->addTab(createTidalTab(), tr("Tidal"));  // TODO: restore when Tidal API available
    m_tabWidget->addTab(new AppearanceSettingsTab(), tr("Appearance"));
    m_tabWidget->addTab(new AboutSettingsTab(), tr("About"));

    mainLayout->addWidget(m_tabWidget, 1);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void SettingsView::refreshTheme()
{
    // Remember which tab was active before rebuild
    int savedTabIndex = m_tabWidget ? m_tabWidget->currentIndex() : 0;

    // Rebuild the entire UI to pick up new theme colors
    QLayout* oldLayout = layout();
    if (oldLayout) {
        QLayoutItem* child;
        while ((child = oldLayout->takeAt(0)) != nullptr) {
            if (child->widget()) {
                child->widget()->deleteLater();
            }
            delete child;
        }
        delete oldLayout;
    }
    m_tabWidget = nullptr;

    // Rebuild
    setupUI();

    // Restore the previously active tab
    if (m_tabWidget && savedTabIndex < m_tabWidget->count()) {
        m_tabWidget->setCurrentIndex(savedTabIndex);
    }

    // Update theme card selection borders in the Appearance tab
    if (m_tabWidget) {
        QWidget* appearanceTab = m_tabWidget->widget(3); // Appearance is index 3 (Audio, Library, Apple Music, Appearance, About)
        if (appearanceTab) {
            ThemeManager::Theme currentTheme = ThemeManager::instance()->currentTheme();
            auto cards = appearanceTab->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively);
            for (auto* card : cards) {
                QVariant val = card->property("themeValue");
                if (val.isValid()) {
                    bool isSelected = (static_cast<ThemeManager::Theme>(val.toInt()) == currentTheme);
                    QString borderStyle = isSelected
                        ? QStringLiteral("border: 2px solid %1;").arg(ThemeManager::instance()->colors().accent)
                        : QStringLiteral("border: 2px solid transparent;");
                    card->setStyleSheet(
                        QStringLiteral("QWidget {"
                                       "  background-color: %1;"
                                       "  border-radius: 8px;"
                                       "  %2"
                                       "}")
                            .arg(ThemeManager::instance()->colors().backgroundSecondary)
                            .arg(borderStyle));
                }
            }
        }
    }
}
