#include "AboutSettingsTab.h"
#include "SettingsUtils.h"
#include "../../SoranaFlowLogo.h"
#include "../../../core/ThemeManager.h"
#include "../../../widgets/StyledScrollArea.h"
#include "../../../widgets/StyledButton.h"
#ifdef Q_OS_MACOS
#include "../../../platform/macos/SparkleUpdater.h"
#endif

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QPushButton>

AboutSettingsTab::AboutSettingsTab(QWidget* parent)
    : QWidget(parent)
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->setAlignment(Qt::AlignHCenter);

    // -- App Logo --------------------------------------------------------
    auto* logo = new SoranaFlowLogo(80, content);
    layout->addWidget(logo, 0, Qt::AlignHCenter);

    layout->addSpacing(8);

    // -- App Name --------------------------------------------------------
    auto* appName = new QLabel(QStringLiteral("Sorana Flow"), content);
    appName->setStyleSheet(
        QStringLiteral("color: %1; font-size: 24px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    appName->setAlignment(Qt::AlignCenter);
    layout->addWidget(appName);

    // -- Version ---------------------------------------------------------
    auto* versionLabel = new QLabel(
        QStringLiteral("Version ") + QCoreApplication::applicationVersion(), content);
    versionLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    versionLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(versionLabel);

    // -- Check for Updates -----------------------------------------------
#ifdef Q_OS_MACOS
    auto* updateBtn = new StyledButton(QStringLiteral("Check for Updates"), "ghost");
    updateBtn->setFixedWidth(160);
    updateBtn->setStyleSheet(
        QStringLiteral("QPushButton { color: %1; font-size: 12px; border: 1px solid %2; "
            "border-radius: 6px; padding: 4px 12px; background: transparent; }"
            "QPushButton:hover { background: %2; }")
            .arg(ThemeManager::instance()->colors().accent,
                 ThemeManager::instance()->colors().hover));
    connect(updateBtn, &QPushButton::clicked, this, []() {
        SparkleUpdater::instance()->checkForUpdates();
    });
    layout->addWidget(updateBtn, 0, Qt::AlignHCenter);
#endif

    layout->addSpacing(8);

    // -- Description -----------------------------------------------------
    auto* descLabel = new QLabel(
        QStringLiteral(
            "A premium audiophile music player designed for seamless flow.\n"
            "Experience your music collection with bit-perfect playback,\n"
            "high-resolution audio support, and intuitive navigation."),
        content);
    descLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    layout->addWidget(descLabel);

    // -- Separator -------------------------------------------------------
    auto* separator1 = new QFrame(content);
    separator1->setFrameShape(QFrame::HLine);
    separator1->setStyleSheet(
        QStringLiteral("QFrame { color: %1; }")
            .arg(ThemeManager::instance()->colors().borderSubtle));
    separator1->setFixedHeight(1);
    layout->addWidget(separator1);

    // -- Supported Formats -----------------------------------------------
    auto* formatsHeader = new QLabel(QStringLiteral("Supported Formats"), content);
    formatsHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: 600;")
            .arg(ThemeManager::instance()->colors().foreground));
    formatsHeader->setAlignment(Qt::AlignCenter);
    layout->addWidget(formatsHeader);

    // Format badges as colored pills
    auto* badgesWidget = new QWidget(content);
    auto* badgesLayout = new QHBoxLayout(badgesWidget);
    badgesLayout->setAlignment(Qt::AlignCenter);
    badgesLayout->setSpacing(8);
    badgesLayout->setContentsMargins(0, 0, 0, 0);

    struct FormatPill { QString text; QString color; };
    const FormatPill pills[] = {
        { QStringLiteral("Hi-Res FLAC"), QStringLiteral("#D4AF37") },
        { QStringLiteral("DSD"),         QStringLiteral("#9C27B0") },
        { QStringLiteral("ALAC"),        QStringLiteral("#4CAF50") },
        { QStringLiteral("WAV"),         QStringLiteral("#F59E0B") },
        { QStringLiteral("MP3"),         QStringLiteral("#9E9E9E") },
        { QStringLiteral("AAC"),         QStringLiteral("#2196F3") },
    };

    for (const auto& pill : pills) {
        auto* badge = new QLabel(pill.text, badgesWidget);
        badge->setStyleSheet(
            QStringLiteral("background: %1; color: white; font-size: 11px; "
                "font-weight: bold; padding: 4px 10px; border-radius: 10px;")
                .arg(pill.color));
        badgesLayout->addWidget(badge);
    }

    layout->addWidget(badgesWidget, 0, Qt::AlignHCenter);

    // -- Separator -------------------------------------------------------
    auto* separator2 = new QFrame(content);
    separator2->setFrameShape(QFrame::HLine);
    separator2->setStyleSheet(
        QStringLiteral("QFrame { color: %1; }")
            .arg(ThemeManager::instance()->colors().borderSubtle));
    separator2->setFixedHeight(1);
    layout->addWidget(separator2);

    // -- Links -----------------------------------------------------------
    auto* linksContainer = new QWidget(content);
    auto* linksLayout = new QHBoxLayout(linksContainer);
    linksLayout->setContentsMargins(0, 0, 0, 0);
    linksLayout->setAlignment(Qt::AlignCenter);

    auto* reportLabel = new QLabel(QStringLiteral("Report Issue"), linksContainer);
    reportLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; border: none;")
            .arg(ThemeManager::instance()->colors().accent));
    reportLabel->setCursor(Qt::PointingHandCursor);
    connect(reportLabel, &QLabel::linkActivated, this, [](const QString&) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://soranaflow.com/support")));
    });
    reportLabel->setText(QStringLiteral("<a href='report' style='color: %1; text-decoration: none;'>Report Issue</a>")
        .arg(ThemeManager::instance()->colors().accent));
    linksLayout->addWidget(reportLabel);

    layout->addWidget(linksContainer);

    // -- Copyright -------------------------------------------------------
    auto* copyrightLabel = new QLabel(
        QStringLiteral("\u00A9 2026 Sorana Flow. All rights reserved."), content);
    copyrightLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    copyrightLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(copyrightLabel);

    layout->addStretch();

    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);
}
