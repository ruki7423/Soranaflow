#include "SpatialSettingsWidget.h"
#include "SettingsUtils.h"
#include "../../../core/Settings.h"
#include "../../../core/ThemeManager.h"
#include "../../../widgets/StyledSwitch.h"
#include "../../../widgets/StyledComboBox.h"
#include "../../../widgets/StyledButton.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSlider>
#include <QFileDialog>

SpatialSettingsWidget::SpatialSettingsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Section: Headphone Crossfeed ─────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Headphone Crossfeed")));

    auto* crossfeedSwitch = new StyledSwitch();
    crossfeedSwitch->setChecked(Settings::instance()->crossfeedEnabled());
    connect(crossfeedSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setCrossfeedEnabled(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Enable Crossfeed"),
        QStringLiteral("Blends stereo channels to simulate speaker listening on headphones"),
        crossfeedSwitch));

    auto* crossfeedLevelCombo = new StyledComboBox();
    crossfeedLevelCombo->addItem(QStringLiteral("Light (subtle, -6 dB)"), 0);
    crossfeedLevelCombo->addItem(QStringLiteral("Medium (natural, -4.5 dB)"), 1);
    crossfeedLevelCombo->addItem(QStringLiteral("Strong (speaker-like, -3 dB)"), 2);
    crossfeedLevelCombo->setCurrentIndex(Settings::instance()->crossfeedLevel());
    connect(crossfeedLevelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [](int idx) {
        Settings::instance()->setCrossfeedLevel(idx);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Crossfeed Intensity"),
        QStringLiteral("Controls how much stereo channel blending is applied"),
        crossfeedLevelCombo));

    // ── Section: Convolution (Room Correction) ────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Convolution / Room Correction")));

    auto* convolutionSwitch = new StyledSwitch();
    convolutionSwitch->setChecked(Settings::instance()->convolutionEnabled());
    connect(convolutionSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setConvolutionEnabled(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Enable Convolution"),
        QStringLiteral("Apply impulse response for room correction or speaker emulation"),
        convolutionSwitch));

    // IR file path row with browse button
    auto* irPathRow = new QWidget();
    auto* irPathLayout = new QHBoxLayout(irPathRow);
    irPathLayout->setContentsMargins(0, 0, 0, 0);
    irPathLayout->setSpacing(8);

    auto* irPathEdit = new QLineEdit();
    irPathEdit->setReadOnly(true);
    irPathEdit->setPlaceholderText(QStringLiteral("No IR file loaded"));
    irPathEdit->setText(Settings::instance()->convolutionIRPath());
    irPathEdit->setFixedHeight(28);
    irPathEdit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    irPathEdit->setStyleSheet(ThemeManager::instance()->inputStyle()
        + QStringLiteral(" QLineEdit { border-radius: 8px; min-height: 0px; padding: 4px 8px; font-size: 12px; }"));

    auto* irBrowseBtn = new StyledButton(QStringLiteral("Browse..."));
    irBrowseBtn->setFixedHeight(28);
    irBrowseBtn->setFixedWidth(100);
    irBrowseBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    irBrowseBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary)
        + QStringLiteral(" QPushButton { border-radius: 8px; min-height: 0px; padding: 4px 8px; font-size: 12px; }"));

    auto* irClearBtn = new StyledButton(QStringLiteral("Clear"));
    irClearBtn->setFixedHeight(28);
    irClearBtn->setFixedWidth(70);
    irClearBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    irClearBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary)
        + QStringLiteral(" QPushButton { border-radius: 8px; min-height: 0px; padding: 4px 8px; font-size: 12px; }"));

    irPathEdit->setFixedWidth(214);  // 400 - 100 - 70 - 16 spacing
    irPathLayout->addWidget(irPathEdit, 0);
    irPathLayout->addWidget(irBrowseBtn, 0);
    irPathLayout->addWidget(irClearBtn, 0);
    irPathRow->setFixedWidth(400);

    connect(irBrowseBtn, &QPushButton::clicked, this, [irPathEdit]() {
        QString path = QFileDialog::getOpenFileName(
            nullptr,
            QStringLiteral("Select Impulse Response File"),
            QString(),
            QStringLiteral("WAV Files (*.wav);;All Files (*)"));
        if (!path.isEmpty()) {
            Settings::instance()->setConvolutionIRPath(path);
            irPathEdit->setText(path);
        }
    });

    connect(irClearBtn, &QPushButton::clicked, this, [irPathEdit]() {
        Settings::instance()->setConvolutionIRPath(QString());
        irPathEdit->clear();
    });

    auto* irSettingRow = SettingsUtils::createSettingRow(
        QStringLiteral("Impulse Response File"),
        QStringLiteral("Load a WAV file containing the room correction impulse response"),
        irPathRow);
    irSettingRow->setMinimumHeight(28 + 16);
    irSettingRow->layout()->setContentsMargins(0, 2, 0, 2);
    layout->addWidget(irSettingRow);

    // ── Section: HRTF (Binaural Spatial Audio) ────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("HRTF / Binaural Spatial Audio")));

    auto* hrtfSwitch = new StyledSwitch();
    hrtfSwitch->setChecked(Settings::instance()->hrtfEnabled());
    // HRTF and Crossfeed mutual exclusion enforced in Settings setters;
    // UI switches react to Settings signals to stay in sync.
    connect(hrtfSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setHrtfEnabled(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Enable HRTF"),
        QStringLiteral("Simulate speaker playback through headphones using SOFA HRTF data"),
        hrtfSwitch));

    // Sync UI switches when Settings enforces mutual exclusion
    connect(Settings::instance(), &Settings::hrtfChanged, this, [hrtfSwitch]() {
        bool on = Settings::instance()->hrtfEnabled();
        if (hrtfSwitch->isChecked() != on) {
            hrtfSwitch->blockSignals(true);
            hrtfSwitch->setChecked(on);
            hrtfSwitch->blockSignals(false);
        }
    });
    connect(Settings::instance(), &Settings::crossfeedChanged, this, [crossfeedSwitch]() {
        bool on = Settings::instance()->crossfeedEnabled();
        if (crossfeedSwitch->isChecked() != on) {
            crossfeedSwitch->blockSignals(true);
            crossfeedSwitch->setChecked(on);
            crossfeedSwitch->blockSignals(false);
        }
    });

    // SOFA file path row with browse button
    auto* sofaPathRow = new QWidget();
    auto* sofaPathLayout = new QHBoxLayout(sofaPathRow);
    sofaPathLayout->setContentsMargins(0, 0, 0, 0);
    sofaPathLayout->setSpacing(8);

    auto* sofaPathEdit = new QLineEdit();
    sofaPathEdit->setReadOnly(true);
    sofaPathEdit->setPlaceholderText(QStringLiteral("No SOFA file loaded"));
    sofaPathEdit->setText(Settings::instance()->hrtfSofaPath());
    sofaPathEdit->setFixedHeight(28);
    sofaPathEdit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    sofaPathEdit->setStyleSheet(ThemeManager::instance()->inputStyle()
        + QStringLiteral(" QLineEdit { border-radius: 8px; min-height: 0px; padding: 4px 8px; font-size: 12px; }"));

    auto* sofaBrowseBtn = new StyledButton(QStringLiteral("Browse..."));
    sofaBrowseBtn->setFixedHeight(28);
    sofaBrowseBtn->setFixedWidth(100);
    sofaBrowseBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    sofaBrowseBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary)
        + QStringLiteral(" QPushButton { border-radius: 8px; min-height: 0px; padding: 4px 8px; font-size: 12px; }"));

    auto* sofaClearBtn = new StyledButton(QStringLiteral("Clear"));
    sofaClearBtn->setFixedHeight(28);
    sofaClearBtn->setFixedWidth(70);
    sofaClearBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    sofaClearBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary)
        + QStringLiteral(" QPushButton { border-radius: 8px; min-height: 0px; padding: 4px 8px; font-size: 12px; }"));

    sofaPathEdit->setFixedWidth(214);  // 400 - 100 - 70 - 16 spacing
    sofaPathLayout->addWidget(sofaPathEdit, 0);
    sofaPathLayout->addWidget(sofaBrowseBtn, 0);
    sofaPathLayout->addWidget(sofaClearBtn, 0);
    sofaPathRow->setFixedWidth(400);

    connect(sofaBrowseBtn, &QPushButton::clicked, this, [sofaPathEdit]() {
        QString path = QFileDialog::getOpenFileName(
            nullptr,
            QStringLiteral("Select SOFA HRTF File"),
            QString(),
            QStringLiteral("SOFA Files (*.sofa);;All Files (*)"));
        if (!path.isEmpty()) {
            Settings::instance()->setHrtfSofaPath(path);
            sofaPathEdit->setText(path);
        }
    });

    connect(sofaClearBtn, &QPushButton::clicked, this, [sofaPathEdit]() {
        Settings::instance()->setHrtfSofaPath(QString());
        sofaPathEdit->clear();
    });

    auto* sofaSettingRow = SettingsUtils::createSettingRow(
        QStringLiteral("SOFA HRTF File"),
        QStringLiteral("Load a SOFA file containing head-related transfer function data"),
        sofaPathRow);
    sofaSettingRow->setMinimumHeight(28 + 16);
    sofaSettingRow->layout()->setContentsMargins(0, 2, 0, 2);
    layout->addWidget(sofaSettingRow);

    // Speaker angle slider
    auto* speakerAngleRow = new QWidget();
    auto* speakerAngleLayout = new QHBoxLayout(speakerAngleRow);
    speakerAngleLayout->setContentsMargins(0, 0, 0, 0);
    speakerAngleLayout->setSpacing(8);
    speakerAngleLayout->setAlignment(Qt::AlignVCenter);

    auto* speakerAngleSlider = new QSlider(Qt::Horizontal);
    speakerAngleSlider->setRange(10, 90);
    speakerAngleSlider->setValue(static_cast<int>(Settings::instance()->hrtfSpeakerAngle()));
    speakerAngleSlider->setFixedHeight(24);

    auto* speakerAngleLabel = new QLabel(QStringLiteral("%1°").arg(speakerAngleSlider->value()));
    speakerAngleLabel->setFixedWidth(40);
    speakerAngleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    speakerAngleLayout->addWidget(speakerAngleSlider, 1);
    speakerAngleLayout->addWidget(speakerAngleLabel, 0);

    connect(speakerAngleSlider, &QSlider::valueChanged, this, [speakerAngleLabel](int value) {
        speakerAngleLabel->setText(QStringLiteral("%1°").arg(value));
        Settings::instance()->setHrtfSpeakerAngle(static_cast<float>(value));
    });

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Virtual Speaker Angle"),
        QStringLiteral("Angle of virtual speakers from center (10° to 90°, default 30°)"),
        speakerAngleRow));
}
