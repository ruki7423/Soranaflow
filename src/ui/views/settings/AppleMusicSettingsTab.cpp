#include "AppleMusicSettingsTab.h"
#include "SettingsUtils.h"

#include "../../../core/ThemeManager.h"
#include "../../../widgets/StyledButton.h"
#include "../../../widgets/StyledComboBox.h"
#include "../../../widgets/StyledScrollArea.h"

#include <QVBoxLayout>

#ifdef Q_OS_MACOS
#include "../../../apple/AppleMusicManager.h"
#include "../../../apple/MusicKitPlayer.h"
#endif

AppleMusicSettingsTab::AppleMusicSettingsTab(QWidget* parent)
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

    auto c = ThemeManager::instance()->colors();

#ifdef Q_OS_MACOS
    auto* am = AppleMusicManager::instance();

    // ── Connection section ──────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Connection")));

    // Status row
    m_appleMusicStatusLabel = new QLabel(QStringLiteral("Not connected"), content);
    m_appleMusicStatusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; border: none;").arg(c.foregroundMuted));

    auto updateAuthUI = [this, c](AppleMusicManager::AuthStatus status) {
        switch (status) {
        case AppleMusicManager::Authorized:
            m_appleMusicStatusLabel->setText(QStringLiteral("Connected"));
            m_appleMusicStatusLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; font-weight: bold; border: none;").arg(c.success));
            m_appleMusicConnectBtn->setText(QStringLiteral("Disconnect"));
            m_appleMusicConnectBtn->setEnabled(true);
            m_appleMusicConnectBtn->setFixedSize(200, UISizes::buttonHeight);
            m_appleMusicConnectBtn->setStyleSheet(QStringLiteral(
                "QPushButton {"
                "  background-color: %1;"
                "  border: none;"
                "  border-radius: %2px;"
                "  color: %3;"
                "  font-size: %4px;"
                "  font-weight: 500;"
                "}"
                "QPushButton:hover { background-color: %5; }"
                "QPushButton:pressed { background-color: %1; }")
                .arg(c.error)
                .arg(UISizes::buttonRadius)
                .arg(c.foregroundInverse)
                .arg(UISizes::fontSizeMD)
                .arg(c.errorHover));
            break;
        case AppleMusicManager::Denied:
            m_appleMusicStatusLabel->setText(QStringLiteral("Access denied — enable in System Settings → Privacy"));
            m_appleMusicStatusLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; border: none;").arg(c.error));
            break;
        case AppleMusicManager::Restricted:
            m_appleMusicStatusLabel->setText(QStringLiteral("Access restricted"));
            m_appleMusicStatusLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; border: none;").arg(c.foregroundMuted));
            break;
        default:
            m_appleMusicStatusLabel->setText(QStringLiteral("Not connected"));
            m_appleMusicStatusLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; border: none;").arg(c.foregroundMuted));
            m_appleMusicConnectBtn->setText(QStringLiteral("Connect Apple Music"));
            m_appleMusicConnectBtn->setEnabled(true);
            m_appleMusicConnectBtn->setStyleSheet(QString());  // Reset to StyledButton default
            break;
        }
    };

    // Connect button
    m_appleMusicConnectBtn = new StyledButton(QStringLiteral("Connect Apple Music"),
                                               QStringLiteral("primary"), content);
    m_appleMusicConnectBtn->setObjectName(QStringLiteral("settingsAppleConnectBtn"));
    m_appleMusicConnectBtn->setFixedSize(200, UISizes::buttonHeight);

    connect(m_appleMusicConnectBtn, &QPushButton::clicked, this, [am]() {
        if (am->authorizationStatus() == AppleMusicManager::Authorized) {
            am->disconnectAppleMusic();
        } else {
            am->requestAuthorization();
        }
    });

    connect(am, &AppleMusicManager::authorizationStatusChanged,
            this, [updateAuthUI](AppleMusicManager::AuthStatus status) {
                updateAuthUI(status);
            });

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Apple Music"),
        QStringLiteral("Connect to search and browse the Apple Music catalog"),
        m_appleMusicConnectBtn));
    layout->addWidget(m_appleMusicStatusLabel);

    // ── Subscription status ─────────────────────────────────────────
    m_appleMusicSubLabel = new QLabel(content);
    m_appleMusicSubLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.foregroundMuted));
    m_appleMusicSubLabel->setVisible(false);
    layout->addWidget(m_appleMusicSubLabel);

    connect(am, &AppleMusicManager::subscriptionStatusChanged,
            this, [this](bool hasSub) {
                m_appleMusicSubLabel->setVisible(true);
                if (hasSub) {
                    m_appleMusicSubLabel->setText(QStringLiteral("Active Apple Music subscription detected"));
                    m_appleMusicSubLabel->setStyleSheet(
                        QStringLiteral("color: %1; font-size: 12px; border: none;")
                            .arg(ThemeManager::instance()->colors().success));
                } else {
                    m_appleMusicSubLabel->setText(
                        QStringLiteral("No active subscription — search works, playback requires subscription"));
                    m_appleMusicSubLabel->setStyleSheet(
                        QStringLiteral("color: %1; font-size: 12px; border: none;")
                            .arg(ThemeManager::instance()->colors().foregroundMuted));
                }
            });

    // ── Playback quality ────────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Playback")));

    auto* appleMusicQualityCombo = new StyledComboBox();
    appleMusicQualityCombo->addItem(QStringLiteral("High (256 kbps)"), QStringLiteral("high"));
    appleMusicQualityCombo->addItem(QStringLiteral("Standard (64 kbps)"), QStringLiteral("standard"));
    appleMusicQualityCombo->setCurrentIndex(0);

    connect(appleMusicQualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [appleMusicQualityCombo](int idx) {
        if (idx < 0) return;
        QString quality = appleMusicQualityCombo->itemData(idx).toString();
        MusicKitPlayer::instance()->setPlaybackQuality(quality);
    });

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Stream Quality"),
        QStringLiteral("MusicKit JS max: 256kbps AAC. Lossless requires the Apple Music app."),
        appleMusicQualityCombo));

    // ── Developer token status ─────────────────────────────────────
    auto* tokenStatusLabel = new QLabel(content);
    tokenStatusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.foregroundMuted));
    if (am->hasDeveloperToken()) {
        tokenStatusLabel->setText(QStringLiteral("Developer token loaded (REST API search available)"));
        tokenStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.success));
    } else {
        tokenStatusLabel->setText(
            QStringLiteral("No developer token — place AuthKey .p8 file next to the app for search fallback"));
    }
    layout->addWidget(tokenStatusLabel);

    // Set initial state if already authorized
    updateAuthUI(am->authorizationStatus());

#else
    // Non-macOS: show unavailable message
    auto* unavailLabel = new QLabel(
        QStringLiteral("Apple Music integration is only available on macOS 13.0 or later."), content);
    unavailLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));
    unavailLabel->setWordWrap(true);
    unavailLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(unavailLabel);
#endif

    layout->addStretch();
    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);
}
