#include "ProcessingSettingsWidget.h"
#include "SettingsUtils.h"
#include "../../../core/Settings.h"
#include "../../../core/dsp/ReplayGainScanner.h"
#include "../../../widgets/StyledSwitch.h"
#include "../../../widgets/StyledComboBox.h"

#include <QVBoxLayout>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <cmath>

ProcessingSettingsWidget::ProcessingSettingsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Section: Volume Leveling ───────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Volume Leveling")));

    auto* levelingSwitch = new StyledSwitch();
    levelingSwitch->setChecked(Settings::instance()->volumeLeveling());
    connect(levelingSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setVolumeLeveling(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Enable Volume Leveling"),
        QStringLiteral("Normalizes loudness using ReplayGain tags or EBU R128 analysis"),
        levelingSwitch));

    auto* levelingModeCombo = new StyledComboBox();
    levelingModeCombo->addItem(QStringLiteral("Track"));
    levelingModeCombo->addItem(QStringLiteral("Album"));
    levelingModeCombo->setCurrentIndex(Settings::instance()->levelingMode());
    connect(levelingModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [](int idx) {
        Settings::instance()->setLevelingMode(idx);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Leveling Mode"),
        QStringLiteral("Track mode normalizes each track individually; Album preserves relative dynamics within an album"),
        levelingModeCombo));

    auto* targetCombo = new StyledComboBox();
    targetCombo->addItem(QStringLiteral("-14 LUFS (Spotify / YouTube)"), -14.0);
    targetCombo->addItem(QStringLiteral("-16 LUFS (Apple Music)"), -16.0);
    targetCombo->addItem(QStringLiteral("-18 LUFS (ReplayGain reference)"), -18.0);
    targetCombo->addItem(QStringLiteral("-23 LUFS (EBU broadcast)"), -23.0);
    {
        double currentTarget = Settings::instance()->targetLoudness();
        for (int i = 0; i < targetCombo->count(); ++i) {
            if (std::abs(targetCombo->itemData(i).toDouble() - currentTarget) < 0.5) {
                targetCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    connect(targetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [targetCombo](int idx) {
        Settings::instance()->setTargetLoudness(targetCombo->itemData(idx).toDouble());
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Target Loudness"),
        QStringLiteral("Reference loudness level for normalization"),
        targetCombo));

    // ── Scan Library for ReplayGain ─────────────────────────────────
    auto* scanWidget = new QWidget();
    auto* scanLayout = new QHBoxLayout(scanWidget);
    scanLayout->setContentsMargins(0, 0, 0, 0);
    scanLayout->setSpacing(8);

    m_scanButton = new QPushButton(QStringLiteral("Scan Library"));
    m_scanButton->setFixedWidth(120);
    m_scanButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: none; border-radius: 4px;"
        " padding: 6px 12px; font-size: 13px; font-weight: bold; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:disabled { opacity: 0.5; }")
        .arg(QStringLiteral("#4a9eff"),
             QStringLiteral("#ffffff"),
             QStringLiteral("#5aadff")));

    m_scanProgress = new QProgressBar();
    m_scanProgress->setFixedHeight(6);
    m_scanProgress->setTextVisible(false);
    m_scanProgress->setVisible(false);
    m_scanProgress->setStyleSheet(QStringLiteral(
        "QProgressBar { background: %1; border: none; border-radius: 3px; }"
        "QProgressBar::chunk { background: #4a9eff; border-radius: 3px; }")
        .arg(QStringLiteral("#2a2a2a")));

    m_scanStatusLabel = new QLabel();
    m_scanStatusLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; border: none;")
        .arg(QStringLiteral("#888888")));
    m_scanStatusLabel->setVisible(false);

    auto* scanInfoLayout = new QVBoxLayout();
    scanInfoLayout->setSpacing(4);
    scanInfoLayout->addWidget(m_scanProgress);
    scanInfoLayout->addWidget(m_scanStatusLabel);

    scanLayout->addWidget(m_scanButton);
    scanLayout->addLayout(scanInfoLayout, 1);

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("ReplayGain Scanner"),
        QStringLiteral("Analyze library tracks for loudness normalization (EBU R128)"),
        scanWidget));

    // Scanner connections
    auto* scanner = ReplayGainScanner::instance();

    connect(m_scanButton, &QPushButton::clicked, this, [this, scanner]() {
        if (scanner->isScanning()) {
            scanner->stopScan();
        } else {
            scanner->startScan();
        }
    });

    connect(scanner, &ReplayGainScanner::scanStarted, this, [this]() {
        m_scanButton->setText(QStringLiteral("Stop Scan"));
        m_scanProgress->setVisible(true);
        m_scanProgress->setValue(0);
        m_scanStatusLabel->setVisible(true);
        m_scanStatusLabel->setText(QStringLiteral("Preparing..."));
    });

    connect(scanner, &ReplayGainScanner::scanProgress, this, [this](int current, int total) {
        if (total > 0) {
            m_scanProgress->setMaximum(total);
            m_scanProgress->setValue(current);
            m_scanStatusLabel->setText(
                QStringLiteral("%1 / %2 tracks analyzed").arg(current).arg(total));
        }
    });

    connect(scanner, &ReplayGainScanner::scanFinished, this, [this](int scannedCount, int albumCount) {
        m_scanButton->setText(QStringLiteral("Scan Library"));
        m_scanProgress->setVisible(false);
        m_scanStatusLabel->setVisible(true);
        if (scannedCount > 0) {
            m_scanStatusLabel->setText(
                QStringLiteral("Done: %1 tracks, %2 albums").arg(scannedCount).arg(albumCount));
        } else {
            m_scanStatusLabel->setText(QStringLiteral("All tracks up to date"));
        }
    });

    // ── Section: Headroom Management ────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Headroom Management")));

    auto* headroomModeCombo = new StyledComboBox();
    headroomModeCombo->addItem(QStringLiteral("Off"));
    headroomModeCombo->addItem(QStringLiteral("Auto"));
    headroomModeCombo->addItem(QStringLiteral("Manual"));
    headroomModeCombo->setCurrentIndex(static_cast<int>(Settings::instance()->headroomMode()));

    auto* manualHeadroomCombo = new StyledComboBox();
    manualHeadroomCombo->addItem(QStringLiteral("-1.0 dB"),  -1.0);
    manualHeadroomCombo->addItem(QStringLiteral("-2.0 dB"),  -2.0);
    manualHeadroomCombo->addItem(QStringLiteral("-3.0 dB"),  -3.0);
    manualHeadroomCombo->addItem(QStringLiteral("-4.0 dB"),  -4.0);
    manualHeadroomCombo->addItem(QStringLiteral("-6.0 dB"),  -6.0);
    manualHeadroomCombo->addItem(QStringLiteral("-8.0 dB"),  -8.0);
    manualHeadroomCombo->addItem(QStringLiteral("-10.0 dB"), -10.0);
    manualHeadroomCombo->addItem(QStringLiteral("-12.0 dB"), -12.0);
    {
        double currentHR = Settings::instance()->manualHeadroom();
        for (int i = 0; i < manualHeadroomCombo->count(); ++i) {
            if (std::abs(manualHeadroomCombo->itemData(i).toDouble() - currentHR) < 0.05) {
                manualHeadroomCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    auto* manualHeadroomRow = SettingsUtils::createSettingRow(
        QStringLiteral("Manual Headroom"),
        QStringLiteral("Fixed gain reduction applied before DSP processing"),
        manualHeadroomCombo);
    manualHeadroomRow->setVisible(headroomModeCombo->currentIndex() == 2);

    connect(headroomModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [manualHeadroomRow](int idx) {
        Settings::instance()->setHeadroomMode(static_cast<Settings::HeadroomMode>(idx));
        manualHeadroomRow->setVisible(idx == 2);
    });

    connect(manualHeadroomCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [manualHeadroomCombo](int idx) {
        Settings::instance()->setManualHeadroom(manualHeadroomCombo->itemData(idx).toDouble());
    });

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Headroom Mode"),
        QStringLiteral("Reduces signal level before DSP to prevent clipping. Auto adjusts based on active effects"),
        headroomModeCombo));
    layout->addWidget(manualHeadroomRow);
}
