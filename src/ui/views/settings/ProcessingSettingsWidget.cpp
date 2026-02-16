#include "ProcessingSettingsWidget.h"
#include "SettingsUtils.h"
#include "../../../core/Settings.h"
#include "../../../widgets/StyledSwitch.h"
#include "../../../widgets/StyledComboBox.h"

#include <QVBoxLayout>
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
