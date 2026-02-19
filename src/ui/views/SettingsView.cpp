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
    m_tabWidget->addTab(new AppearanceSettingsTab(), tr("Appearance"));
    m_tabWidget->addTab(new AboutSettingsTab(), tr("About"));

    mainLayout->addWidget(m_tabWidget, 1);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void SettingsView::refreshTheme()
{
    if (!m_tabWidget) return;

    // Update header label color
    if (auto* header = findChild<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) {
        header->setStyleSheet(
            QStringLiteral("color: %1; font-size: 24px; font-weight: bold;")
                .arg(ThemeManager::instance()->colors().foreground));
    }

    // Replace tab contents in-place (avoids layout destruction/rebuild)
    int savedTabIndex = m_tabWidget->currentIndex();
    while (m_tabWidget->count() > 0) {
        QWidget* tab = m_tabWidget->widget(0);
        m_tabWidget->removeTab(0);
        delete tab;
    }
    m_tabWidget->addTab(new AudioSettingsTab(), tr("Audio"));
    m_tabWidget->addTab(new LibrarySettingsTab(), tr("Library"));
    m_tabWidget->addTab(new AppleMusicSettingsTab(), tr("Apple Music"));
    m_tabWidget->addTab(new AppearanceSettingsTab(), tr("Appearance"));
    m_tabWidget->addTab(new AboutSettingsTab(), tr("About"));

    if (savedTabIndex >= 0 && savedTabIndex < m_tabWidget->count())
        m_tabWidget->setCurrentIndex(savedTabIndex);

    // Update theme card selection borders in the Appearance tab
    QWidget* appearanceTab = m_tabWidget->widget(3);
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
