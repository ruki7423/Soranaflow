#include "AppearanceSettingsTab.h"
#include "SettingsUtils.h"
#include "../../../core/ThemeManager.h"
#include "../../../core/Settings.h"
#include "../../../widgets/StyledScrollArea.h"
#include "../../../widgets/StyledComboBox.h"
#include "../../../widgets/StyledSwitch.h"
#include "../../../widgets/StyledButton.h"
#include "../../dialogs/StyledMessageBox.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

AppearanceSettingsTab::AppearanceSettingsTab(QWidget* parent)
    : QWidget(parent)
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 16, 12, 16);
    layout->setSpacing(0);

    // ── Section: Theme ─────────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Theme")));

    auto* themeCardsWidget = new QWidget();
    auto* themeCardsLayout = new QHBoxLayout(themeCardsWidget);
    themeCardsLayout->setContentsMargins(0, 8, 0, 8);
    themeCardsLayout->setSpacing(16);

    // Determine current theme for highlight
    ThemeManager::Theme currentTheme = ThemeManager::instance()->currentTheme();

    struct ThemeOption {
        QString name;
        QString iconPath;
        ThemeManager::Theme theme;
    };

    const ThemeOption themeOptions[] = {
        { QStringLiteral("Light"),  QStringLiteral(":/icons/sun.svg"),     ThemeManager::Light },
        { QStringLiteral("Dark"),   QStringLiteral(":/icons/moon.svg"),    ThemeManager::Dark },
        { QStringLiteral("System"), QStringLiteral(":/icons/monitor.svg"), ThemeManager::System }
    };

    for (const auto& opt : themeOptions) {
        auto* card = new QWidget(themeCardsWidget);
        card->setFixedSize(120, 100);
        card->setCursor(Qt::PointingHandCursor);

        bool isSelected = (opt.theme == currentTheme);
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

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setAlignment(Qt::AlignCenter);
        cardLayout->setSpacing(8);

        auto* iconLabel = new QLabel(card);
        iconLabel->setPixmap(ThemeManager::instance()->cachedIcon(opt.iconPath).pixmap(32, 32));
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setStyleSheet(QStringLiteral("border: none;"));
        cardLayout->addWidget(iconLabel);

        auto* nameLabel = new QLabel(opt.name, card);
        nameLabel->setAlignment(Qt::AlignCenter);
        nameLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; border: none;")
                .arg(ThemeManager::instance()->colors().foreground));
        cardLayout->addWidget(nameLabel);

        // Connect click via event filter or make it a button-like behavior
        ThemeManager::Theme themeVal = opt.theme;
        card->setProperty("themeValue", static_cast<int>(themeVal));

        // Use a transparent button overlay for click handling
        auto* clickBtn = new StyledButton(QStringLiteral(""),
                                           QStringLiteral("ghost"),
                                           card);
        clickBtn->setFixedSize(120, 100);
        clickBtn->move(0, 0);
        clickBtn->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; border: none; }"));
        clickBtn->raise();

        connect(clickBtn, &QPushButton::clicked, this, [themeVal]() {
            ThemeManager::instance()->setTheme(themeVal);
            Settings::instance()->setThemeIndex(static_cast<int>(themeVal));
        });

        themeCardsLayout->addWidget(card);
    }

    themeCardsLayout->addStretch();
    layout->addWidget(themeCardsWidget);

    // ── Section: Display ───────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Display")));

    auto* formatBadgesSwitch = new StyledSwitch();
    formatBadgesSwitch->setChecked(true);
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Show format badges"),
        QString(),
        formatBadgesSwitch));

    auto* albumArtSwitch = new StyledSwitch();
    albumArtSwitch->setChecked(true);
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Show album art"),
        QString(),
        albumArtSwitch));

    auto* compactModeSwitch = new StyledSwitch();
    compactModeSwitch->setChecked(false);
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Compact mode"),
        QStringLiteral("Reduce spacing for more content"),
        compactModeSwitch));

    // ── Section: Language ────────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(tr("Language")));

    auto* langCombo = new StyledComboBox();
    langCombo->addItem(tr("System Default"), QStringLiteral("auto"));
    langCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    langCombo->addItem(QString::fromUtf8("\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4"), QStringLiteral("ko"));
    langCombo->addItem(QString::fromUtf8("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"), QStringLiteral("ja"));
    langCombo->addItem(QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87"), QStringLiteral("zh"));

    // Select current language
    QString currentLang = Settings::instance()->language();
    for (int i = 0; i < langCombo->count(); ++i) {
        if (langCombo->itemData(i).toString() == currentLang) {
            langCombo->setCurrentIndex(i);
            break;
        }
    }

    connect(langCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [langCombo](int index) {
        QString lang = langCombo->itemData(index).toString();
        Settings::instance()->setLanguage(lang);
        StyledMessageBox::info(nullptr,
            tr("Language Changed"),
            tr("Please restart the application for the language change to take effect."));
    });

    layout->addWidget(SettingsUtils::createSettingRow(
        tr("Language"),
        tr("Select the display language"),
        langCombo));

    layout->addStretch();

    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);
}
