#include "DSPSettingsWidget.h"
#include "SettingsUtils.h"
#include "../../../core/Settings.h"
#include "../../../core/ThemeManager.h"
#include "../../../core/audio/AudioEngine.h"
#include "../../../core/dsp/DSPPipeline.h"
#include "../../../widgets/StyledButton.h"

#include <QGridLayout>
#include <QFrame>
#include <QDir>
#include <QFileDialog>
#include <QTimer>
#include <QSlider>
#include <QDial>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextStream>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <cmath>
#include "../../dialogs/StyledMessageBox.h"

EQGraphWidget::EQGraphWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(180);
    setMaximumHeight(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void EQGraphWidget::setResponse(const std::vector<double>& dBValues)
{
    m_response = dBValues;
    update();
}

void EQGraphWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();
    int margin = 32;
    int graphW = w - margin * 2;
    int graphH = h - margin * 2;

    // Background
    p.fillRect(rect(), QColor(0x14, 0x14, 0x14));

    // Graph area
    QRect graphRect(margin, margin, graphW, graphH);
    p.fillRect(graphRect, QColor(0x0a, 0x0a, 0x0a));

    // dB range: -24 to +24
    double dbMin = -24.0;
    double dbMax = 24.0;
    double dbRange = dbMax - dbMin;

    // Grid lines — horizontal (dB)
    p.setPen(QPen(QColor(255, 255, 255, 20), 1));
    QFont gridFont;
    gridFont.setPixelSize(9);
    p.setFont(gridFont);

    for (int db = -24; db <= 24; db += 6) {
        double y = margin + graphH * (1.0 - (db - dbMin) / dbRange);
        p.drawLine(margin, (int)y, margin + graphW, (int)y);
        p.setPen(QColor(255, 255, 255, 80));
        p.drawText(2, (int)y + 3, QStringLiteral("%1").arg(db));
        p.setPen(QPen(QColor(255, 255, 255, 20), 1));
    }

    // Grid lines — vertical (frequency)
    double freqs[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    double logMin = std::log10(20.0);
    double logMax = std::log10(20000.0);
    double logRange = logMax - logMin;

    for (double freq : freqs) {
        double x = margin + graphW * (std::log10(freq) - logMin) / logRange;
        p.drawLine((int)x, margin, (int)x, margin + graphH);
        p.setPen(QColor(255, 255, 255, 80));
        QString label;
        if (freq >= 1000)
            label = QStringLiteral("%1k").arg(freq / 1000.0, 0, 'f', freq >= 10000 ? 0 : 0);
        else
            label = QStringLiteral("%1").arg((int)freq);
        p.drawText((int)x - 10, h - 6, label);
        p.setPen(QPen(QColor(255, 255, 255, 20), 1));
    }

    // 0 dB reference line
    double zeroY = margin + graphH * (1.0 - (0.0 - dbMin) / dbRange);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
    p.drawLine(margin, (int)zeroY, margin + graphW, (int)zeroY);

    // Response curve
    if (m_response.empty()) return;

    int numPoints = (int)m_response.size();
    QPainterPath curvePath;
    QPainterPath fillPath;
    bool started = false;

    for (int i = 0; i < numPoints; ++i) {
        double t = (double)i / (numPoints - 1);
        double x = margin + graphW * t;
        double db = std::clamp(m_response[i], dbMin, dbMax);
        double y = margin + graphH * (1.0 - (db - dbMin) / dbRange);

        if (!started) {
            curvePath.moveTo(x, y);
            fillPath.moveTo(x, zeroY);
            fillPath.lineTo(x, y);
            started = true;
        } else {
            curvePath.lineTo(x, y);
            fillPath.lineTo(x, y);
        }
    }

    // Fill under curve
    fillPath.lineTo(margin + graphW, zeroY);
    fillPath.closeSubpath();

    QLinearGradient fillGrad(0, margin, 0, margin + graphH);
    fillGrad.setColorAt(0.0, QColor(74, 158, 255, 40));
    fillGrad.setColorAt(0.5, QColor(74, 158, 255, 15));
    fillGrad.setColorAt(1.0, QColor(74, 158, 255, 40));
    p.fillPath(fillPath, fillGrad);

    // Draw curve
    p.setPen(QPen(QColor(74, 158, 255), 2));
    p.drawPath(curvePath);
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — block wheel events on unfocused spinboxes
// ═════════════════════════════════════════════════════════════════════

bool DSPSettingsWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        auto* spin = qobject_cast<QAbstractSpinBox*>(obj);
        if (spin && !spin->hasFocus()) {
            event->ignore();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ═════════════════════════════════════════════════════════════════════
//  DSPSettingsWidget constructor
// ═════════════════════════════════════════════════════════════════════

DSPSettingsWidget::DSPSettingsWidget(QWidget* parent)
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

    // ── DSP Pipeline Card ─────────────────────────────────────────
    createDSPCard(layout);
}

// ═════════════════════════════════════════════════════════════════════
//  createDSPCard — 20-band Parametric EQ (REW-style)
// ═════════════════════════════════════════════════════════════════════

QWidget* DSPSettingsWidget::createDSPCard(QVBoxLayout* parentLayout)
{
    auto* dspCard = new QFrame();
    dspCard->setObjectName(QStringLiteral("DSPCard"));
    {
        auto c = ThemeManager::instance()->colors();
        dspCard->setStyleSheet(QStringLiteral(
            "QFrame#DSPCard {"
            "  background: %1;"
            "  border-radius: 12px;"
            "  border: 1px solid %2;"
            "}")
                .arg(c.backgroundSecondary, c.border));
    }

    auto* dspLayout = new QVBoxLayout(dspCard);
    dspLayout->setContentsMargins(0, 0, 0, 0);
    dspLayout->setSpacing(0);

    // ── Header bar ──────────────────────────────────────────────────
    auto* headerWidget = new QWidget(dspCard);
    headerWidget->setStyleSheet(QStringLiteral(
        "background: %1; border-top-left-radius: 12px; border-top-right-radius: 12px;")
            .arg(ThemeManager::instance()->colors().backgroundTertiary));
    auto* headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 12, 16, 12);

    auto* dspTitle = new QLabel(QStringLiteral("Parametric EQ"), dspCard);
    dspTitle->setStyleSheet(QStringLiteral(
        "font-size: 15px; font-weight: 600; color: %1; border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foreground));
    headerLayout->addWidget(dspTitle);
    headerLayout->addStretch();

    // Preset combo
    m_eqPresetCombo = new StyledComboBox(dspCard);
    m_eqPresetCombo->addItems({
        QStringLiteral("Flat"),
        QStringLiteral("Rock"),
        QStringLiteral("Pop"),
        QStringLiteral("Jazz"),
        QStringLiteral("Classical"),
        QStringLiteral("Bass Boost"),
        QStringLiteral("Treble Boost"),
        QStringLiteral("Vocal"),
        QStringLiteral("Electronic"),
        QStringLiteral("Custom")
    });
    m_eqPresetCombo->setFixedWidth(120);
    QString savedPreset = Settings::instance()->eqPreset();
    int presetIdx = m_eqPresetCombo->findText(savedPreset);
    if (presetIdx >= 0) m_eqPresetCombo->setCurrentIndex(presetIdx);
    connect(m_eqPresetCombo, &QComboBox::currentTextChanged,
            this, &DSPSettingsWidget::applyEQPreset);
    headerLayout->addWidget(m_eqPresetCombo);

    m_dspEnabledSwitch = new StyledSwitch(dspCard);
    m_dspEnabledSwitch->setChecked(Settings::instance()->dspEnabled());
    connect(m_dspEnabledSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setDspEnabled(checked);
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) pipeline->setEnabled(checked);
    });
    headerLayout->addWidget(m_dspEnabledSwitch);
    dspLayout->addWidget(headerWidget);

    // ── Row 1: Preamplification ─────────────────────────────────────
    auto* preampRow = new QWidget(dspCard);
    {
        auto c = ThemeManager::instance()->colors();
        preampRow->setStyleSheet(QStringLiteral(
            "background: %1; border-bottom: 1px solid %2;")
                .arg(c.backgroundTertiary, c.borderSubtle));
    }
    auto* preampLayout = new QHBoxLayout(preampRow);
    preampLayout->setContentsMargins(16, 10, 16, 10);
    preampLayout->setSpacing(12);

    // Row number
    auto* preampNum = new QLabel(QStringLiteral("1"), preampRow);
    preampNum->setFixedWidth(20);
    preampNum->setStyleSheet(QStringLiteral(
        "color: %1; font-weight: bold; font-size: 12px; border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().accent));
    preampLayout->addWidget(preampNum);

    auto* preampLabel = new QLabel(QStringLiteral("Preamplification"), preampRow);
    preampLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 13px; border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foreground));
    preampLayout->addWidget(preampLabel);

    // Dial style shared for all knobs — uses theme colors
    auto tcDial = ThemeManager::instance()->colors();
    QString dialStyle = QStringLiteral(
        "QDial {"
        "  background: qradialgradient(cx:0.5, cy:0.5, radius:0.5,"
        "    fx:0.5, fy:0.3, stop:0 %1, stop:0.5 %2, stop:1 %3);"
        "  border-radius: 20px;"
        "  border: 2px solid %4;"
        "}").arg(tcDial.backgroundElevated, tcDial.backgroundTertiary, tcDial.backgroundSecondary, tcDial.border);

    // Gain dial
    auto* preampDial = new QDial(preampRow);
    preampDial->setRange(-240, 240);
    float initGain = Settings::instance()->preampGain();
    preampDial->setValue(static_cast<int>(initGain * 10.0f));
    preampDial->setFixedSize(40, 40);
    preampDial->setStyleSheet(dialStyle);
    preampLayout->addWidget(preampDial);

    auto* preampGainLabel = new QLabel(QStringLiteral("Gain"), preampRow);
    preampGainLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px; border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    preampLayout->addWidget(preampGainLabel);

    m_gainSlider = nullptr;  // Not used in APO style
    m_gainValueLabel = new QLabel(
        QStringLiteral("%1 dB").arg(initGain, 0, 'f', 1), preampRow);
    m_gainValueLabel->setFixedWidth(70);
    m_gainValueLabel->setAlignment(Qt::AlignCenter);
    m_gainValueLabel->setStyleSheet(QStringLiteral(
        "QLabel { border: none; background: transparent;"
        "  padding: 3px 6px; color: %1; font-size: 12px; }")
            .arg(ThemeManager::instance()->colors().foreground));
    preampLayout->addWidget(m_gainValueLabel);

    connect(preampDial, &QDial::valueChanged, this, [this](int value) {
        float dB = value / 10.0f;
        m_gainValueLabel->setText(QStringLiteral("%1 dB").arg(dB, 0, 'f', 1));
        Settings::instance()->setPreampGain(dB);
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) {
            pipeline->gainProcessor()->setGainDb(dB);
            pipeline->notifyConfigurationChanged();
        }
    });

    preampLayout->addStretch();
    dspLayout->addWidget(preampRow);

    // ── Frequency response graph ────────────────────────────────────
    auto* graphWidget = new QWidget(dspCard);
    graphWidget->setStyleSheet(QStringLiteral("background: %1;")
        .arg(ThemeManager::instance()->colors().backgroundSecondary));
    auto* graphInnerLayout = new QVBoxLayout(graphWidget);
    graphInnerLayout->setContentsMargins(16, 8, 16, 8);

    auto* graphTitle = new QLabel(QStringLiteral("Frequency Response"), dspCard);
    graphTitle->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 11px; border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    graphInnerLayout->addWidget(graphTitle);

    m_eqGraph = new EQGraphWidget(dspCard);
    m_eqGraph->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    graphInnerLayout->addWidget(m_eqGraph);
    dspLayout->addWidget(graphWidget);

    // ── Column headers ──────────────────────────────────────────────
    auto* colHeaderWidget = new QWidget(dspCard);
    {
        auto c = ThemeManager::instance()->colors();
        colHeaderWidget->setStyleSheet(QStringLiteral(
            "background: %1; border: none; border-bottom: 1px solid %2;")
                .arg(c.backgroundTertiary, c.borderSubtle));
    }
    auto* colHeaderLayout = new QHBoxLayout(colHeaderWidget);
    colHeaderLayout->setContentsMargins(16, 6, 16, 6);
    colHeaderLayout->setSpacing(6);

    QString colStyle = QStringLiteral(
        "color: %1; font-size: 10px; font-weight: 600;"
        " border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foregroundMuted);
    auto addColHeader = [&](const QString& text, int fixedW) {
        auto* lbl = new QLabel(text, colHeaderWidget);
        lbl->setStyleSheet(colStyle);
        if (fixedW > 0) lbl->setFixedWidth(fixedW);
        colHeaderLayout->addWidget(lbl);
    };

    addColHeader(QStringLiteral(""),   24);  // enable checkbox
    addColHeader(QStringLiteral("#"),  20);  // band number
    addColHeader(QStringLiteral("TYPE"), 80);
    addColHeader(QStringLiteral(""),   40);  // freq dial
    addColHeader(QStringLiteral("FREQ (Hz)"), 90);
    addColHeader(QStringLiteral(""),   40);  // gain dial
    addColHeader(QStringLiteral("GAIN (dB)"), 80);
    addColHeader(QStringLiteral(""),   40);  // Q dial
    addColHeader(QStringLiteral("Q"),  70);
    colHeaderLayout->addStretch();
    dspLayout->addWidget(colHeaderWidget);

    // ── Band rows container (no scroll — parent audio tab scrolls) ──
    m_bandRowsContainer = new QWidget(dspCard);
    m_bandRowsContainer->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_bandRowsLayout = new QVBoxLayout(m_bandRowsContainer);
    m_bandRowsLayout->setContentsMargins(0, 0, 0, 0);
    m_bandRowsLayout->setSpacing(0);

    dspLayout->addWidget(m_bandRowsContainer);

    // ── Band count control bar (Add/Remove) ─────────────────────────
    auto* bandCountBar = new QWidget(dspCard);
    bandCountBar->setStyleSheet(QStringLiteral(
        "background: %1; border-bottom-left-radius: 12px;"
        " border-bottom-right-radius: 12px;")
            .arg(ThemeManager::instance()->colors().backgroundTertiary));
    auto* bandCountLayout = new QHBoxLayout(bandCountBar);
    bandCountLayout->setContentsMargins(16, 8, 16, 8);
    bandCountLayout->setSpacing(8);

    auto* addBandBtn = new QPushButton(QStringLiteral("+ Add Band"), dspCard);
    addBandBtn->setCursor(Qt::PointingHandCursor);
    {
        auto c = ThemeManager::instance()->colors();
        addBandBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent; color: %1;"
            "  border: 1px solid %1; border-radius: 4px;"
            "  padding: 5px 12px; font-size: 12px; font-weight: 600;"
            "}"
            "QPushButton:hover { background: %2; }")
                .arg(c.accent, c.accentMuted));
    }
    bandCountLayout->addWidget(addBandBtn);

    auto* removeBandBtn = new QPushButton(QStringLiteral("- Remove Band"), dspCard);
    removeBandBtn->setCursor(Qt::PointingHandCursor);
    {
        auto c = ThemeManager::instance()->colors();
        removeBandBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent; color: %1;"
            "  border: 1px solid %2; border-radius: 4px;"
            "  padding: 5px 12px; font-size: 12px;"
            "}"
            "QPushButton:hover { background: %3; }")
                .arg(c.foregroundSecondary, c.border, c.hover));
    }
    bandCountLayout->addWidget(removeBandBtn);

    auto* importEQBtn = new QPushButton(QStringLiteral("Import EQ"), dspCard);
    importEQBtn->setCursor(Qt::PointingHandCursor);
    {
        auto c = ThemeManager::instance()->colors();
        importEQBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent; color: %1;"
            "  border: 1px solid %2; border-radius: 4px;"
            "  padding: 5px 12px; font-size: 12px;"
            "}"
            "QPushButton:hover { background: %3; }")
                .arg(c.foregroundSecondary, c.border, c.hover));
    }
    bandCountLayout->addWidget(importEQBtn);

    bandCountLayout->addStretch();

    // Hidden spinbox for band count storage (keeps existing Settings integration)
    m_bandCountSpin = new QSpinBox(dspCard);
    m_bandCountSpin->setObjectName(QStringLiteral("eqBandCount"));
    m_bandCountSpin->setRange(1, 20);
    m_bandCountSpin->setVisible(false);
    m_activeBandCount = Settings::instance()->eqActiveBands();
    if (m_activeBandCount < 1) m_activeBandCount = 1;
    if (m_activeBandCount > 20) m_activeBandCount = 20;
    m_bandCountSpin->setValue(m_activeBandCount);

    auto* bandCountLabel = new QLabel(
        QStringLiteral("%1 bands").arg(m_activeBandCount), bandCountBar);
    bandCountLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; border: none; background: transparent;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    bandCountLayout->addWidget(bandCountLabel);

    connect(addBandBtn, &QPushButton::clicked, this, [this, bandCountLabel]() {
        if (m_activeBandCount >= 20) return;
        m_activeBandCount++;
        m_bandCountSpin->setValue(m_activeBandCount);
        Settings::instance()->setEqActiveBands(m_activeBandCount);
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) pipeline->equalizerProcessor()->setActiveBands(m_activeBandCount);
        rebuildBandRows();
        updateEQGraph();
        bandCountLabel->setText(QStringLiteral("%1 bands").arg(m_activeBandCount));
    });

    connect(removeBandBtn, &QPushButton::clicked, this, [this, bandCountLabel]() {
        if (m_activeBandCount <= 1) return;
        m_activeBandCount--;
        m_bandCountSpin->setValue(m_activeBandCount);
        Settings::instance()->setEqActiveBands(m_activeBandCount);
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) pipeline->equalizerProcessor()->setActiveBands(m_activeBandCount);
        rebuildBandRows();
        updateEQGraph();
        bandCountLabel->setText(QStringLiteral("%1 bands").arg(m_activeBandCount));
    });

    connect(importEQBtn, &QPushButton::clicked, this, [this, bandCountLabel]() {
        QString filePath = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import EQ Settings"),
            QDir::homePath(),
            QStringLiteral("EQ Files (*.txt *.cfg);;REW / AutoEQ (*.txt);;Equalizer APO (*.txt *.cfg);;All Files (*)"));
        if (filePath.isEmpty()) return;

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            StyledMessageBox::error(this, QStringLiteral("Error"),
                QStringLiteral("Could not open file."));
            return;
        }
        QTextStream in(&file);
        QString content = in.readAll();
        file.close();

        // ── Helper: map filter type string to EQBand::FilterType ──────
        auto mapFilterType = [](const QString& typeStr) -> EQBand::FilterType {
            QString t = typeStr.toUpper();
            if (t == QStringLiteral("LSQ") || t == QStringLiteral("LSC") || t == QStringLiteral("LS"))
                return EQBand::LowShelf;
            if (t == QStringLiteral("HSQ") || t == QStringLiteral("HSC") || t == QStringLiteral("HS"))
                return EQBand::HighShelf;
            if (t == QStringLiteral("LPQ") || t == QStringLiteral("LP"))
                return EQBand::LowPass;
            if (t == QStringLiteral("HPQ") || t == QStringLiteral("HP"))
                return EQBand::HighPass;
            if (t == QStringLiteral("NO") || t == QStringLiteral("NOTCH"))
                return EQBand::Notch;
            if (t == QStringLiteral("BP") || t == QStringLiteral("BPQ"))
                return EQBand::BandPass;
            // PK, PEQ, PEAK, or anything else → Peak
            return EQBand::Peak;
        };

        // ── Parse preamp (common to all formats) ──────────────────────
        float preampDb = 0.0f;
        {
            QRegularExpression preampRe(
                QStringLiteral("Preamp:\\s*([\\-\\d.]+)\\s*dB"),
                QRegularExpression::CaseInsensitiveOption);
            auto preampMatch = preampRe.match(content);
            if (preampMatch.hasMatch()) {
                preampDb = preampMatch.captured(1).toFloat();
                qDebug() << "[EQ Import] Preamp:" << preampDb << "dB";
            }
        }

        QVector<EQBand> parsedBands;
        QString formatName;

        // ── Format 1: GraphicEQ (detect first — single-line format) ──
        if (content.contains(QStringLiteral("GraphicEQ:"), Qt::CaseInsensitive)) {
            QRegularExpression geqPattern(
                QStringLiteral("GraphicEQ:\\s*(.+)"),
                QRegularExpression::CaseInsensitiveOption);
            auto geqMatch = geqPattern.match(content);
            if (geqMatch.hasMatch()) {
                QString data = geqMatch.captured(1);
                QStringList pairs = data.split(QLatin1Char(';'), Qt::SkipEmptyParts);

                QVector<EQBand> allBands;
                for (const QString& pair : pairs) {
                    QStringList parts = pair.trimmed().split(QRegularExpression(QStringLiteral("\\s+")));
                    if (parts.size() >= 2) {
                        bool freqOk, gainOk;
                        float freq = parts[0].toFloat(&freqOk);
                        float gain = parts[1].toFloat(&gainOk);
                        if (freqOk && gainOk && gain != 0.0f) {
                            EQBand band;
                            band.enabled = true;
                            band.type = EQBand::Peak;
                            band.frequency = freq;
                            band.gainDb = gain;
                            band.q = 1.41f;
                            allBands.append(band);
                        }
                    }
                }

                // If more than 20 non-zero bands, keep 20 with largest |gain|
                if (allBands.size() > 20) {
                    std::sort(allBands.begin(), allBands.end(),
                        [](const EQBand& a, const EQBand& b) {
                            return qAbs(a.gainDb) > qAbs(b.gainDb);
                        });
                    allBands = allBands.mid(0, 20);
                    // Re-sort by frequency for display
                    std::sort(allBands.begin(), allBands.end(),
                        [](const EQBand& a, const EQBand& b) {
                            return a.frequency < b.frequency;
                        });
                }

                if (!allBands.isEmpty()) {
                    parsedBands = allBands;
                    formatName = QStringLiteral("GraphicEQ");
                    qDebug() << "[EQ Import] GraphicEQ: loaded" << parsedBands.size() << "bands";
                }
            }
        }

        // ── Format 2: REW / Equalizer APO parametric ─────────────────
        if (parsedBands.isEmpty()) {
            // Strict REW format: "Filter N: ON TYPE Fc FREQ Hz Gain GAIN dB Q Q"
            QRegularExpression rewRe(
                QStringLiteral("Filter\\s+\\d+:\\s+ON\\s+(\\w+)\\s+Fc\\s+([\\d.]+)\\s*(?:Hz)?\\s+Gain\\s+([\\-\\d.]+)\\s*(?:dB)?\\s+Q\\s+([\\d.]+)"),
                QRegularExpression::CaseInsensitiveOption);
            auto it = rewRe.globalMatch(content);
            while (it.hasNext()) {
                auto m = it.next();
                EQBand band;
                band.enabled = true;
                band.frequency = m.captured(2).toFloat();
                band.gainDb = m.captured(3).toFloat();
                band.q = m.captured(4).toFloat();
                band.type = mapFilterType(m.captured(1));
                parsedBands.append(band);
            }

            // Fallback: looser APO format — optional "Filter N:" prefix
            // Catches "ON PK Fc 1000 Hz Gain 3.0 dB Q 1.41" without prefix
            if (parsedBands.isEmpty()) {
                QRegularExpression apoRe(
                    QStringLiteral("(?:Filter(?:\\s+\\d+)?:\\s+)?ON\\s+(\\w+)\\s+Fc\\s+([\\d.]+)\\s*(?:Hz)?\\s+Gain\\s+([\\-\\d.]+)\\s*(?:dB)?\\s+Q\\s+([\\d.]+)"),
                    QRegularExpression::CaseInsensitiveOption);
                auto apoIt = apoRe.globalMatch(content);
                while (apoIt.hasNext()) {
                    auto m = apoIt.next();
                    EQBand band;
                    band.enabled = true;
                    band.frequency = m.captured(2).toFloat();
                    band.gainDb = m.captured(3).toFloat();
                    band.q = m.captured(4).toFloat();
                    band.type = mapFilterType(m.captured(1));
                    parsedBands.append(band);
                }
            }

            if (!parsedBands.isEmpty()) {
                formatName = QStringLiteral("Parametric");
                qDebug() << "[EQ Import] Parametric: loaded" << parsedBands.size() << "filters";
                for (const auto& b : parsedBands) {
                    qDebug() << "[EQ Import]   Filter:" << b.type
                             << b.frequency << "Hz" << b.gainDb << "dB Q" << b.q;
                }
            }
        }

        if (parsedBands.isEmpty()) {
            qDebug() << "[EQ Import] No recognized EQ format found";
            StyledMessageBox::warning(this, QStringLiteral("Import Failed"),
                QStringLiteral("No valid EQ filters found in file.\n\n"
                    "Supported formats:\n"
                    "• REW / AutoEQ: Filter 1: ON PK Fc 1000 Hz Gain -3.5 dB Q 1.41\n"
                    "• Equalizer APO: ON PK Fc 1000 Hz Gain -3.5 dB Q 1.41\n"
                    "• GraphicEQ: 20 0.0; 32 -1.5; 50 -3.0; ..."));
            return;
        }

        // ── Apply parsed bands (cap at 20) ────────────────────────────
        int count = qMin(parsedBands.size(), 20);
        m_activeBandCount = count;
        m_bandCountSpin->setValue(count);
        Settings::instance()->setEqActiveBands(count);

        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) pipeline->equalizerProcessor()->setActiveBands(count);

        for (int i = 0; i < count; ++i) {
            const EQBand& b = parsedBands[i];
            Settings::instance()->setEqBandEnabled(i, true);
            Settings::instance()->setEqBandType(i, static_cast<int>(b.type));
            Settings::instance()->setEqBandFreq(i, b.frequency);
            Settings::instance()->setEqBandGain(i, b.gainDb);
            Settings::instance()->setEqBandQ(i, b.q);
            if (pipeline) pipeline->equalizerProcessor()->setBand(i, b);
        }

        // Apply preamp via gain slider if available
        if (m_gainSlider && preampDb != 0.0f) {
            int sliderVal = qBound(m_gainSlider->minimum(), static_cast<int>(preampDb * 10.0f), m_gainSlider->maximum());
            m_gainSlider->setValue(sliderVal);
        }

        if (m_eqPresetCombo) {
            m_eqPresetCombo->blockSignals(true);
            int customIdx = m_eqPresetCombo->findText(QStringLiteral("Custom"));
            if (customIdx >= 0) m_eqPresetCombo->setCurrentIndex(customIdx);
            m_eqPresetCombo->blockSignals(false);
        }
        Settings::instance()->setEqPreset(QStringLiteral("Custom"));

        rebuildBandRows();
        updateEQGraph();
        bandCountLabel->setText(QStringLiteral("%1 bands").arg(m_activeBandCount));

        StyledMessageBox::info(this, QStringLiteral("Import Complete"),
            QStringLiteral("Loaded %1 %2 EQ filters%3.")
                .arg(count)
                .arg(formatName)
                .arg(preampDb != 0.0f ? QStringLiteral(" with %1 dB preamp").arg(preampDb, 0, 'f', 1) : QString()));
    });

    dspLayout->addWidget(bandCountBar);

    // Build the initial band rows
    rebuildBandRows();
    updateEQGraph();

    parentLayout->addWidget(dspCard);
    return dspCard;
}

// ═════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════
//  rebuildBandRows — create/show/hide rows for active band count
// ═════════════════════════════════════════════════════════════════════

void DSPSettingsWidget::rebuildBandRows()
{
    // Clear existing
    while (m_bandRowsLayout->count() > 0) {
        QLayoutItem* item = m_bandRowsLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    for (int i = 0; i < 20; ++i)
        m_bandRows[i] = {};

    // Shared styles
    auto c = ThemeManager::instance()->colors();
    QString spinStyle = QStringLiteral(
        "QDoubleSpinBox {"
        "  background: %1; color: %2;"
        "  border: 1px solid %3; border-radius: 4px;"
        "  padding: 3px 6px; font-size: 11px;"
        "}"
        "QDoubleSpinBox:focus { border-color: %4; }"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; height: 0; }")
            .arg(c.backgroundSecondary, c.foreground, c.border, c.borderFocus);

    QString comboStyle = QStringLiteral(
        "QComboBox {"
        "  background: %1; color: %2;"
        "  border: 1px solid %3; border-radius: 4px;"
        "  padding: 3px 6px; font-size: 11px;"
        "}"
        "QComboBox:hover { border-color: %4; background: %5; }"
        "QComboBox:focus { border-color: %6; }"
        "QComboBox::drop-down { border: none; width: 16px; background: transparent; }"
        "QComboBox::down-arrow { image: none; width: 0; height: 0;"
        "  border-left: 3px solid transparent; border-right: 3px solid transparent;"
        "  border-top: 4px solid %7; }"
        "QComboBox QAbstractItemView {"
        "  background: %8; color: %2;"
        "  border: 1px solid %4; border-radius: 4px;"
        "  padding: 4px; outline: none; selection-background-color: %9;"
        "}"
        "QComboBox QAbstractItemView::item {"
        "  padding: 6px 8px; border-radius: 4px; color: %2;"
        "}"
        "QComboBox QAbstractItemView::item:hover {"
        "  background: %10;"
        "}"
        "QComboBox QAbstractItemView::item:selected {"
        "  background: %9; color: %2;"
        "}")
            .arg(c.backgroundSecondary, c.foreground, c.border,
                 c.borderFocus, c.backgroundTertiary, c.borderFocus,
                 c.foregroundMuted, c.backgroundElevated, c.accentMuted, c.hover);

    QString dialStyle = QStringLiteral(
        "QDial {"
        "  background: qradialgradient(cx:0.5, cy:0.5, radius:0.5,"
        "    fx:0.5, fy:0.3, stop:0 %1, stop:0.5 %2, stop:1 %3);"
        "  border-radius: 14px;"
        "  border: 2px solid %4;"
        "}").arg(c.backgroundElevated, c.backgroundTertiary, c.backgroundSecondary, c.border);

    // Get the EQ processor to read current band settings
    EqualizerProcessor* eq = nullptr;
    auto* pipeline = AudioEngine::instance()->dspPipeline();
    if (pipeline) eq = pipeline->equalizerProcessor();

    for (int i = 0; i < m_activeBandCount; ++i) {
        auto* row = new QWidget();
        bool even = (i % 2 == 0);
        row->setStyleSheet(QStringLiteral(
            "background: %1; border-bottom: 1px solid %2;")
            .arg(even ? c.backgroundTertiary : c.backgroundSecondary)
            .arg(c.borderSubtle));
        row->setFixedHeight(40);

        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 2, 12, 2);
        rowLayout->setSpacing(6);

        // Read current band from processor (or defaults)
        EQBand band;
        if (eq) band = eq->getBand(i);

        // Restore from settings if available
        float savedFreq = Settings::instance()->eqBandFreq(i);
        float savedGain = Settings::instance()->eqBandGain(i);
        float savedQ = Settings::instance()->eqBandQ(i);
        int savedType = Settings::instance()->eqBandType(i);
        bool savedEnabled = Settings::instance()->eqBandEnabled(i);

        if (savedFreq > 0.0f) {
            band.frequency = savedFreq;
            band.gainDb = savedGain;
            band.q = savedQ > 0.0f ? savedQ : 1.0f;
            band.type = static_cast<EQBand::FilterType>(savedType);
            band.enabled = savedEnabled;
        }

        // Enable checkbox
        auto* enableCheck = new QCheckBox(row);
        enableCheck->setChecked(band.enabled);
        enableCheck->setFixedWidth(24);
        enableCheck->setStyleSheet(QStringLiteral(
            "QCheckBox::indicator {"
            "  width: 14px; height: 14px; border-radius: 3px;"
            "  border: 1px solid %1;"
            "  background: transparent;"
            "}"
            "QCheckBox::indicator:checked {"
            "  background: %2; border-color: %2;"
            "}")
                .arg(c.border, c.accent));
        rowLayout->addWidget(enableCheck);

        // Band number (row 2+)
        auto* bandLabel = new QLabel(QStringLiteral("%1").arg(i + 1), row);
        bandLabel->setFixedWidth(20);
        bandLabel->setAlignment(Qt::AlignCenter);
        bandLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 11px; font-weight: bold;"
            " border: none; background: transparent;").arg(c.accent));
        rowLayout->addWidget(bandLabel);

        // Filter type combo
        auto* typeCombo = new QComboBox(row);
        typeCombo->addItems({
            QStringLiteral("Peak"),
            QStringLiteral("Low Shelf"),
            QStringLiteral("High Shelf"),
            QStringLiteral("Low Pass"),
            QStringLiteral("High Pass"),
            QStringLiteral("Notch"),
            QStringLiteral("Band Pass")
        });
        typeCombo->setCurrentIndex(static_cast<int>(band.type));
        typeCombo->setFixedWidth(80);
        typeCombo->setStyleSheet(comboStyle);
        rowLayout->addWidget(typeCombo);

        // Frequency dial + spinbox
        auto* freqDial = new QDial(row);
        freqDial->setRange(20, 20000);
        freqDial->setValue(static_cast<int>(band.frequency));
        freqDial->setFixedSize(28, 28);
        freqDial->setStyleSheet(dialStyle);
        rowLayout->addWidget(freqDial);

        auto* freqSpin = new QDoubleSpinBox(row);
        freqSpin->setRange(20.0, 20000.0);
        freqSpin->setDecimals(1);
        freqSpin->setValue(band.frequency);
        freqSpin->setFixedWidth(90);
        freqSpin->setStyleSheet(spinStyle);
        rowLayout->addWidget(freqSpin);

        // Gain dial + spinbox
        auto* gainDial = new QDial(row);
        gainDial->setRange(-240, 240);
        gainDial->setValue(static_cast<int>(band.gainDb * 10.0f));
        gainDial->setFixedSize(28, 28);
        gainDial->setStyleSheet(dialStyle);
        rowLayout->addWidget(gainDial);

        auto* gainSpin = new QDoubleSpinBox(row);
        gainSpin->setRange(-24.0, 24.0);
        gainSpin->setDecimals(1);
        gainSpin->setSingleStep(0.5);
        gainSpin->setValue(band.gainDb);
        gainSpin->setFixedWidth(80);
        gainSpin->setStyleSheet(spinStyle);
        rowLayout->addWidget(gainSpin);

        // Q dial + spinbox
        auto* qDial = new QDial(row);
        qDial->setRange(10, 3000);  // 0.1 to 30.0 * 100
        qDial->setValue(static_cast<int>(band.q * 100.0f));
        qDial->setFixedSize(28, 28);
        qDial->setStyleSheet(dialStyle);
        rowLayout->addWidget(qDial);

        auto* qSpin = new QDoubleSpinBox(row);
        qSpin->setRange(0.1, 30.0);
        qSpin->setDecimals(2);
        qSpin->setSingleStep(0.1);
        qSpin->setValue(band.q);
        qSpin->setFixedWidth(70);
        qSpin->setStyleSheet(spinStyle);
        rowLayout->addWidget(qSpin);

        rowLayout->addStretch();

        // Block accidental wheel changes on unfocused spinboxes
        freqSpin->setFocusPolicy(Qt::StrongFocus);
        gainSpin->setFocusPolicy(Qt::StrongFocus);
        qSpin->setFocusPolicy(Qt::StrongFocus);
        freqSpin->installEventFilter(this);
        gainSpin->installEventFilter(this);
        qSpin->installEventFilter(this);

        // Connect dials <-> spinboxes
        connect(freqDial, &QDial::valueChanged, freqSpin, [freqSpin](int v) {
            freqSpin->setValue(static_cast<double>(v));
        });
        connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                freqDial, [freqDial](double v) { freqDial->setValue(static_cast<int>(v)); });

        connect(gainDial, &QDial::valueChanged, gainSpin, [gainSpin](int v) {
            gainSpin->setValue(v / 10.0);
        });
        connect(gainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                gainDial, [gainDial](double v) { gainDial->setValue(static_cast<int>(v * 10)); });

        connect(qDial, &QDial::valueChanged, qSpin, [qSpin](int v) {
            qSpin->setValue(v / 100.0);
        });
        connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                qDial, [qDial](double v) { qDial->setValue(static_cast<int>(v * 100)); });

        // Store references
        m_bandRows[i] = {row, enableCheck, bandLabel, typeCombo, freqSpin, gainSpin, qSpin};

        // Connect signals for DSP updates
        int bandIdx = i;
        auto onBandChanged = [this, bandIdx]() {
            syncBandToProcessor(bandIdx);
            updateEQGraph();

            if (m_eqPresetCombo && m_eqPresetCombo->currentText() != QStringLiteral("Custom")) {
                m_eqPresetCombo->blockSignals(true);
                m_eqPresetCombo->setCurrentText(QStringLiteral("Custom"));
                m_eqPresetCombo->blockSignals(false);
                Settings::instance()->setEqPreset(QStringLiteral("Custom"));
            }
        };

        connect(enableCheck, &QCheckBox::toggled, this, onBandChanged);
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onBandChanged);
        connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onBandChanged);
        connect(gainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onBandChanged);
        connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onBandChanged);

        // Sync initial state to processor
        syncBandToProcessor(i);

        m_bandRowsLayout->addWidget(row);
    }
}

// ═════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════
//  syncBandToProcessor — push UI values to DSP and settings
// ═════════════════════════════════════════════════════════════════════

void DSPSettingsWidget::syncBandToProcessor(int bandIndex)
{
    if (bandIndex < 0 || bandIndex >= 20) return;
    auto& r = m_bandRows[bandIndex];
    if (!r.widget) return;

    EQBand band;
    band.enabled   = r.enableCheck->isChecked();
    band.type      = static_cast<EQBand::FilterType>(r.typeCombo->currentIndex());
    band.frequency = static_cast<float>(r.freqSpin->value());
    band.gainDb    = static_cast<float>(r.gainSpin->value());
    band.q         = static_cast<float>(r.qSpin->value());

    // Save to settings
    Settings::instance()->setEqBandEnabled(bandIndex, band.enabled);
    Settings::instance()->setEqBandType(bandIndex, static_cast<int>(band.type));
    Settings::instance()->setEqBandFreq(bandIndex, band.frequency);
    Settings::instance()->setEqBandGain(bandIndex, band.gainDb);
    Settings::instance()->setEqBandQ(bandIndex, band.q);

    // Push to DSP
    auto* pipeline = AudioEngine::instance()->dspPipeline();
    if (pipeline) {
        pipeline->equalizerProcessor()->setBand(bandIndex, band);
    }
}


// ═════════════════════════════════════════════════════════════════════
//  updateEQGraph — refresh the frequency response curve
// ═════════════════════════════════════════════════════════════════════

void DSPSettingsWidget::updateEQGraph()
{
    if (!m_eqGraph) return;

    auto* pipeline = AudioEngine::instance()->dspPipeline();
    if (pipeline && pipeline->equalizerProcessor()) {
        auto response = pipeline->equalizerProcessor()->getFrequencyResponse(512);
        m_eqGraph->setResponse(response);
    }
}

// ═════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════
//  applyEQPreset — set EQ bands from a named preset (10-band presets)
// ═════════════════════════════════════════════════════════════════════

void DSPSettingsWidget::applyEQPreset(const QString& presetName)
{
    // Preset gain values for 10 standard frequencies: 32, 64, 125, 250, 500, 1k, 2k, 4k, 8k, 16k
    static const float presets[][10] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},                               // Flat
        {4.0f, 3.0f, 1.0f, -1.0f, -2.0f, 1.0f, 3.0f, 4.0f, 4.5f, 4.0f},    // Rock
        {-1.0f, 1.0f, 3.0f, 4.0f, 3.0f, 1.0f, -1.0f, -1.5f, 2.0f, 3.0f},   // Pop
        {3.0f, 2.0f, 0.5f, -1.0f, -1.5f, 0, 1.0f, 2.0f, 3.0f, 3.5f},       // Jazz
        {2.0f, 1.5f, 0, 0, 0, 0, 0, 1.0f, 2.0f, 3.0f},                      // Classical
        {6.0f, 5.0f, 3.5f, 2.0f, 0.5f, 0, 0, 0, 0, 0},                      // Bass Boost
        {0, 0, 0, 0, 0.5f, 2.0f, 3.5f, 5.0f, 6.0f, 6.5f},                   // Treble Boost
        {-2.0f, -1.0f, 0, 2.0f, 4.0f, 4.0f, 3.0f, 1.0f, 0, -1.0f},         // Vocal
        {5.0f, 4.0f, 2.0f, 0, -1.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f},        // Electronic
    };
    static const float presetFreqs[10] = {32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

    static const QStringList presetNames = {
        QStringLiteral("Flat"), QStringLiteral("Rock"), QStringLiteral("Pop"),
        QStringLiteral("Jazz"), QStringLiteral("Classical"), QStringLiteral("Bass Boost"),
        QStringLiteral("Treble Boost"), QStringLiteral("Vocal"), QStringLiteral("Electronic")
    };

    int idx = presetNames.indexOf(presetName);
    if (idx < 0) {
        Settings::instance()->setEqPreset(presetName);
        return;
    }

    Settings::instance()->setEqPreset(presetName);

    // Set band count to 10 for presets
    m_activeBandCount = 10;
    if (m_bandCountSpin) {
        m_bandCountSpin->blockSignals(true);
        m_bandCountSpin->setValue(10);
        m_bandCountSpin->blockSignals(false);
    }
    Settings::instance()->setEqActiveBands(10);

    auto* pipeline = AudioEngine::instance()->dspPipeline();
    if (pipeline) pipeline->equalizerProcessor()->setActiveBands(10);

    // Apply preset values
    for (int i = 0; i < 10; ++i) {
        EQBand band;
        band.enabled   = true;
        band.type      = EQBand::Peak;
        band.frequency = presetFreqs[i];
        band.gainDb    = presets[idx][i];
        band.q         = 1.0f;

        // Save to settings
        Settings::instance()->setEqBandEnabled(i, true);
        Settings::instance()->setEqBandType(i, 0);
        Settings::instance()->setEqBandFreq(i, band.frequency);
        Settings::instance()->setEqBandGain(i, band.gainDb);
        Settings::instance()->setEqBandQ(i, band.q);

        if (pipeline) pipeline->equalizerProcessor()->setBand(i, band);
    }

    // Rebuild rows and graph
    rebuildBandRows();
    updateEQGraph();
}
