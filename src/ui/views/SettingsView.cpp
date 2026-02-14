#include "SettingsView.h"
#include "../SoranaFlowLogo.h"
#include "../../core/ThemeManager.h"
#include "../../core/Settings.h"
#include "../../core/library/LibraryScanner.h"
#include "../../core/library/LibraryDatabase.h"
#include "../../core/MusicData.h"
#include "../../core/audio/AudioEngine.h"
#include "../../core/audio/AudioDeviceManager.h"
#include "../../core/dsp/DSPPipeline.h"
#include "../../core/dsp/UpsamplerProcessor.h"
#include "../../plugins/VST3Host.h"
#include "../../plugins/VST2Host.h"
#include "../../plugins/VST2Plugin.h"
#ifdef Q_OS_MACOS
#include "../../platform/macos/BookmarkManager.h"
#include "../../platform/macos/SparkleUpdater.h"
#include "../../platform/macos/AudioProcessTap.h"
#include "../../apple/AppleMusicManager.h"
#include "../../apple/MusicKitPlayer.h"
#endif
// #include "../../tidal/TidalManager.h"  // TODO: restore when Tidal API available
#include <QGridLayout>
#include <QFrame>
#include <QDir>
#include <QFileDialog>
#include <QTimer>
#include <QListWidget>
#include "../dialogs/StyledMessageBox.h"
#include <QSlider>
#include <QCoreApplication>
#include <QDial>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextStream>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <cmath>
#include "../../widgets/StyledInput.h"

// ═════════════════════════════════════════════════════════════════════
//  EQGraphWidget — Frequency response curve
// ═════════════════════════════════════════════════════════════════════

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

bool SettingsView::eventFilter(QObject* obj, QEvent* event)
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
//  Constructor
// ═════════════════════════════════════════════════════════════════════

SettingsView::SettingsView(QWidget* parent)
    : QWidget(parent)
    , m_tabWidget(nullptr)
{
    setupUI();

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SettingsView::refreshTheme);

    // Connect scanner signals
    connect(LibraryScanner::instance(), &LibraryScanner::scanProgress,
            this, &SettingsView::onScanProgress);
    connect(LibraryScanner::instance(), &LibraryScanner::scanFinished,
            this, &SettingsView::onScanFinished);
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

    m_tabWidget->addTab(createAudioTab(), tr("Audio"));
    m_tabWidget->addTab(createLibraryTab(), tr("Library"));
    m_tabWidget->addTab(createAppleMusicTab(), tr("Apple Music"));
    // m_tabWidget->addTab(createTidalTab(), tr("Tidal"));  // TODO: restore when Tidal API available
    m_tabWidget->addTab(createAppearanceTab(), tr("Appearance"));
    m_tabWidget->addTab(createAboutTab(), tr("About"));

    mainLayout->addWidget(m_tabWidget, 1);
}

// ═════════════════════════════════════════════════════════════════════
//  createSettingRow
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createSettingRow(const QString& label,
                                         const QString& description,
                                         QWidget* control)
{
    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("settingRow"));
    row->setMinimumHeight(UISizes::rowHeight);
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    row->setStyleSheet(
        QStringLiteral("#settingRow { border-bottom: 1px solid %1; }")
            .arg(ThemeManager::instance()->colors().borderSubtle));

    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 8, 0, 8);
    rowLayout->setSpacing(16);

    // Left side: label + description
    auto* textLayout = new QVBoxLayout();
    textLayout->setSpacing(2);

    auto* labelWidget = new QLabel(label, row);
    labelWidget->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: bold; border: none;")
            .arg(ThemeManager::instance()->colors().foreground));
    textLayout->addWidget(labelWidget);

    if (!description.isEmpty()) {
        auto* descWidget = new QLabel(description, row);
        descWidget->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;")
                .arg(ThemeManager::instance()->colors().foregroundMuted));
        descWidget->setWordWrap(true);
        textLayout->addWidget(descWidget);
    }

    rowLayout->addLayout(textLayout, 1);

    // Right side: control — vertically centered in the row
    if (control) {
        rowLayout->addWidget(control, 0, Qt::AlignVCenter);
    }

    return row;
}

// ═════════════════════════════════════════════════════════════════════
//  createSectionHeader
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createSectionHeader(const QString& title)
{
    auto* header = new QLabel(title);
    header->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;"
                       " border: none; padding: 0px;")
            .arg(ThemeManager::instance()->colors().foreground));
    header->setContentsMargins(0, 16, 0, 8);
    return header;
}

// ═════════════════════════════════════════════════════════════════════
//  createAudioTab
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createAudioTab()
{
    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 16, 12, 16);
    layout->setSpacing(0);

    // ── Section: Output ────────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Output")));

    auto* outputDeviceCombo = new StyledComboBox();
    // Populate with real devices from AudioEngine
    auto devices = AudioEngine::instance()->availableDevices();
    int savedDeviceIdx = 0;
    uint32_t savedDeviceId = Settings::instance()->outputDeviceId();
    for (size_t i = 0; i < devices.size(); ++i) {
        outputDeviceCombo->addItem(QString::fromStdString(devices[i].name),
                                    QVariant(devices[i].deviceId));
        if (devices[i].deviceId == savedDeviceId)
            savedDeviceIdx = static_cast<int>(i);
    }
    if (devices.empty()) {
        outputDeviceCombo->addItem(QStringLiteral("No Output Devices"));
    } else {
        outputDeviceCombo->setCurrentIndex(savedDeviceIdx);
    }
    connect(outputDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [outputDeviceCombo](int index) {
        if (index < 0) return;
        QVariant data = outputDeviceCombo->itemData(index);
        if (data.isValid()) {
            uint32_t deviceId = data.toUInt();
            AudioEngine::instance()->setOutputDevice(deviceId);
            Settings::instance()->setOutputDeviceId(deviceId);
            // Save persistent UID and name
            auto info = AudioDeviceManager::instance()->deviceById(deviceId);
            Settings::instance()->setOutputDeviceUID(info.uid);
            Settings::instance()->setOutputDeviceName(info.name);
            // Route Apple Music WebView audio to the new device
            MusicKitPlayer::instance()->updateOutputDevice();
        }
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Output Device"),
        QString(),
        outputDeviceCombo));

    // Device info label (sample rate, buffer size, channels)
    auto* deviceInfoLabel = new QLabel();
    deviceInfoLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 2px 0 8px 0;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    auto updateDeviceInfoLabel = [deviceInfoLabel, outputDeviceCombo]() {
        QVariant data = outputDeviceCombo->currentData();
        if (!data.isValid()) return;
        uint32_t devId = data.toUInt();
        auto* mgr = AudioDeviceManager::instance();
        auto info = mgr->deviceById(devId);
        double rate = mgr->currentSampleRate(devId);
        uint32_t buf = mgr->currentBufferSize(devId);
        QString rateStr = (rate >= 1000.0)
            ? QStringLiteral("%1 kHz").arg(rate / 1000.0, 0, 'f', 1)
            : QStringLiteral("%1 Hz").arg(rate, 0, 'f', 0);
        deviceInfoLabel->setText(
            QStringLiteral("%1 | %2 | Buffer: %3 frames | %4 ch")
                .arg(info.manufacturer.isEmpty() ? info.name : info.manufacturer,
                     rateStr)
                .arg(buf)
                .arg(info.outputChannels));
    };
    updateDeviceInfoLabel();
    connect(outputDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [updateDeviceInfoLabel](int) { updateDeviceInfoLabel(); });
    connect(AudioEngine::instance(), &AudioEngine::signalPathChanged,
            this, [updateDeviceInfoLabel]() { updateDeviceInfoLabel(); });
    layout->addWidget(deviceInfoLabel);

    // ── Device Capabilities ────────────────────────────────────────
    auto* capsFrame = new QFrame();
    capsFrame->setStyleSheet(
        QStringLiteral("QFrame { background: %1; border-radius: 6px; border: none; }")
            .arg(ThemeManager::instance()->colors().backgroundTertiary));
    auto* capsLayout = new QVBoxLayout(capsFrame);
    capsLayout->setContentsMargins(12, 10, 12, 10);
    capsLayout->setSpacing(6);

    auto* capsTitle = new QLabel(QStringLiteral("Supported Capabilities"));
    capsTitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; font-weight: bold; border: none;")
            .arg(ThemeManager::instance()->colors().foreground));
    capsLayout->addWidget(capsTitle);

    auto* capsRatesLabel = new QLabel();
    capsRatesLabel->setWordWrap(true);
    capsRatesLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; border: none;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    capsLayout->addWidget(capsRatesLabel);

    auto* capsBufLabel = new QLabel();
    capsBufLabel->setWordWrap(true);
    capsBufLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; border: none;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    capsLayout->addWidget(capsBufLabel);

    auto updateCapsSection = [capsRatesLabel, capsBufLabel, outputDeviceCombo]() {
        QVariant data = outputDeviceCombo->currentData();
        if (!data.isValid()) return;
        uint32_t devId = data.toUInt();
        auto* mgr = AudioDeviceManager::instance();

        // Sample rates
        auto rates = mgr->supportedSampleRates(devId);
        QStringList rateStrs;
        for (double r : rates) {
            if (r >= 1000.0)
                rateStrs << QStringLiteral("%1 kHz").arg(r / 1000.0, 0, 'f', 1);
            else
                rateStrs << QStringLiteral("%1 Hz").arg(r, 0, 'f', 0);
        }
        capsRatesLabel->setText(
            QStringLiteral("Sample rates: %1")
                .arg(rateStrs.isEmpty() ? QStringLiteral("N/A") : rateStrs.join(QStringLiteral(", "))));

        // Buffer sizes with latency
        auto bsRange = mgr->supportedBufferSizes(devId);
        double curRate = mgr->currentSampleRate(devId);
        if (curRate <= 0) curRate = 44100.0;

        QStringList bufStrs;
        static const uint32_t standardSizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
        for (uint32_t sz : standardSizes) {
            if (sz >= bsRange.minimum && sz <= bsRange.maximum) {
                double latencyMs = (double)sz / curRate * 1000.0;
                bufStrs << QStringLiteral("%1 (%2 ms)").arg(sz).arg(latencyMs, 0, 'f', 1);
            }
        }
        capsBufLabel->setText(
            QStringLiteral("Buffer sizes: %1")
                .arg(bufStrs.isEmpty() ? QStringLiteral("N/A") : bufStrs.join(QStringLiteral(", "))));
    };
    updateCapsSection();
    connect(outputDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [updateCapsSection](int) { updateCapsSection(); });
    layout->addWidget(capsFrame);

    auto* exclusiveModeSwitch = new StyledSwitch();
    exclusiveModeSwitch->setChecked(Settings::instance()->exclusiveMode());
    connect(exclusiveModeSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        AudioEngine::instance()->setExclusiveMode(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Exclusive Mode"),
        QStringLiteral("Take exclusive control of the audio device (hog mode), preventing other apps from using it"),
        exclusiveModeSwitch));

    auto* gaplessSwitch = new StyledSwitch();
    gaplessSwitch->setChecked(Settings::instance()->gaplessPlayback());
    connect(gaplessSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setGaplessPlayback(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Gapless Playback"),
        QStringLiteral("Seamless transitions between consecutive tracks without silence gaps"),
        gaplessSwitch));

    auto* crossfadeCombo = new StyledComboBox();
    crossfadeCombo->addItem(QStringLiteral("Off (Gapless)"), 0);
    crossfadeCombo->addItem(QStringLiteral("1 second"), 1000);
    crossfadeCombo->addItem(QStringLiteral("2 seconds"), 2000);
    crossfadeCombo->addItem(QStringLiteral("3 seconds"), 3000);
    crossfadeCombo->addItem(QStringLiteral("5 seconds"), 5000);
    crossfadeCombo->addItem(QStringLiteral("10 seconds"), 10000);
    int savedCfMs = Settings::instance()->crossfadeDurationMs();
    for (int i = 0; i < crossfadeCombo->count(); i++) {
        if (crossfadeCombo->itemData(i).toInt() == savedCfMs) {
            crossfadeCombo->setCurrentIndex(i);
            break;
        }
    }
    connect(crossfadeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [crossfadeCombo](int idx) {
        int ms = crossfadeCombo->itemData(idx).toInt();
        Settings::instance()->setCrossfadeDurationMs(ms);
        AudioEngine::instance()->setCrossfadeDuration(ms);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Crossfade"),
        QStringLiteral("Smoothly blend between tracks using an equal-power curve. Disabled for DSD and upsampled playback."),
        crossfadeCombo));

    // ── Section: Autoplay / Radio ──────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Autoplay / Radio")));

    auto* autoplaySwitch = new StyledSwitch();
    autoplaySwitch->setChecked(Settings::instance()->autoplayEnabled());
    connect(autoplaySwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setAutoplayEnabled(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Autoplay / Radio"),
        QStringLiteral("When the queue ends, automatically find and play similar tracks using Last.fm recommendations with local library fallback"),
        autoplaySwitch));

    // ── Section: Volume Leveling ───────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Volume Leveling")));

    auto* levelingSwitch = new StyledSwitch();
    levelingSwitch->setChecked(Settings::instance()->volumeLeveling());
    connect(levelingSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setVolumeLeveling(checked);
    });
    layout->addWidget(createSettingRow(
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
    layout->addWidget(createSettingRow(
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
    layout->addWidget(createSettingRow(
        QStringLiteral("Target Loudness"),
        QStringLiteral("Reference loudness level for normalization"),
        targetCombo));

    // ── Section: Headroom Management ────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Headroom Management")));

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

    auto* manualHeadroomRow = createSettingRow(
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

    layout->addWidget(createSettingRow(
        QStringLiteral("Headroom Mode"),
        QStringLiteral("Reduces signal level before DSP to prevent clipping. Auto adjusts based on active effects"),
        headroomModeCombo));
    layout->addWidget(manualHeadroomRow);

    // ── Section: Headphone Crossfeed ─────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Headphone Crossfeed")));

    auto* crossfeedSwitch = new StyledSwitch();
    crossfeedSwitch->setChecked(Settings::instance()->crossfeedEnabled());
    connect(crossfeedSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setCrossfeedEnabled(checked);
    });
    layout->addWidget(createSettingRow(
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
    layout->addWidget(createSettingRow(
        QStringLiteral("Crossfeed Intensity"),
        QStringLiteral("Controls how much stereo channel blending is applied"),
        crossfeedLevelCombo));

    // ── Section: Convolution (Room Correction) ────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Convolution / Room Correction")));

    auto* convolutionSwitch = new StyledSwitch();
    convolutionSwitch->setChecked(Settings::instance()->convolutionEnabled());
    connect(convolutionSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setConvolutionEnabled(checked);
    });
    layout->addWidget(createSettingRow(
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

    auto* irSettingRow = createSettingRow(
        QStringLiteral("Impulse Response File"),
        QStringLiteral("Load a WAV file containing the room correction impulse response"),
        irPathRow);
    irSettingRow->setMinimumHeight(28 + 16);
    irSettingRow->layout()->setContentsMargins(0, 2, 0, 2);
    layout->addWidget(irSettingRow);

    // ── Section: HRTF (Binaural Spatial Audio) ────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("HRTF / Binaural Spatial Audio")));

    auto* hrtfSwitch = new StyledSwitch();
    hrtfSwitch->setChecked(Settings::instance()->hrtfEnabled());
    // HRTF and Crossfeed mutual exclusion enforced in Settings setters;
    // UI switches react to Settings signals to stay in sync.
    connect(hrtfSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setHrtfEnabled(checked);
    });
    layout->addWidget(createSettingRow(
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

    auto* sofaSettingRow = createSettingRow(
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

    layout->addWidget(createSettingRow(
        QStringLiteral("Virtual Speaker Angle"),
        QStringLiteral("Angle of virtual speakers from center (10° to 90°, default 30°)"),
        speakerAngleRow));

    // ── Section: Processing ────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Processing")));

    auto* bufferSizeCombo = new StyledComboBox();
    // Helper: populate combo and select actual device buffer
    auto syncBufferCombo = [bufferSizeCombo, outputDeviceCombo]() {
        bufferSizeCombo->blockSignals(true);
        bufferSizeCombo->clear();

        auto* devMgr = AudioDeviceManager::instance();
        QVariant devData = outputDeviceCombo->currentData();
        uint32_t curDevId = devData.isValid() ? devData.toUInt()
                                              : Settings::instance()->outputDeviceId();
        uint32_t actualBuf = devMgr->currentBufferSize(curDevId);

        static const uint32_t standardSizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
        double sampleRate = devMgr->currentSampleRate(curDevId);
        if (sampleRate <= 0) sampleRate = 44100.0;

        int selIdx = -1;
        for (uint32_t sz : standardSizes) {
            double latencyMs = static_cast<double>(sz) / sampleRate * 1000.0;
            bufferSizeCombo->addItem(
                QStringLiteral("%1 samples (~%2ms)").arg(sz).arg(latencyMs, 0, 'f', 1), sz);
            if (sz == actualBuf) selIdx = bufferSizeCombo->count() - 1;
        }
        // If actual buffer not in standard list, add it
        if (selIdx < 0) {
            double latencyMs = static_cast<double>(actualBuf) / sampleRate * 1000.0;
            bufferSizeCombo->addItem(
                QStringLiteral("%1 samples (~%2ms)").arg(actualBuf).arg(latencyMs, 0, 'f', 1), actualBuf);
            selIdx = bufferSizeCombo->count() - 1;
        }
        bufferSizeCombo->setCurrentIndex(selIdx);
        bufferSizeCombo->blockSignals(false);
    };
    syncBufferCombo();  // Initial sync from actual device

    connect(bufferSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [bufferSizeCombo, updateDeviceInfoLabel](int index) {
        if (index < 0) return;
        uint32_t frames = bufferSizeCombo->itemData(index).toUInt();
        if (frames > 0) {
            AudioDeviceManager::instance()->setBufferSize(frames);
            AudioEngine::instance()->setBufferSize(frames);
            Settings::instance()->setBufferSize(bufferSizeCombo->currentText());
            // Delay refresh — give CoreAudio time to apply the buffer change
            QTimer::singleShot(150, [updateDeviceInfoLabel]() {
                updateDeviceInfoLabel();
            });
        }
    });
    // When device changes, refresh buffer combo to show new device's actual buffer
    connect(outputDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [syncBufferCombo](int) {
        // Delay slightly so device switch completes first
        QTimer::singleShot(200, [syncBufferCombo]() {
            syncBufferCombo();
        });
    });
    // When CoreAudio confirms buffer size, sync combo to actual value
    connect(AudioDeviceManager::instance(), &AudioDeviceManager::bufferSizeChanged,
            this, [bufferSizeCombo](uint32_t newSize) {
        bufferSizeCombo->blockSignals(true);
        for (int i = 0; i < bufferSizeCombo->count(); ++i) {
            if (bufferSizeCombo->itemData(i).toUInt() == newSize) {
                bufferSizeCombo->setCurrentIndex(i);
                bufferSizeCombo->blockSignals(false);
                return;
            }
        }
        // If not found in list, update the current text
        bufferSizeCombo->setCurrentText(QString::number(newSize));
        bufferSizeCombo->blockSignals(false);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Buffer Size"),
        QString(),
        bufferSizeCombo));

    auto* sampleRateConvCombo = new StyledComboBox();
    sampleRateConvCombo->addItems({
        QStringLiteral("None"),
        QStringLiteral("SoX High Quality"),
        QStringLiteral("SoX Very High Quality")
    });
    {
        QString savedConv = Settings::instance()->sampleRateConversion();
        int convIdx = sampleRateConvCombo->findText(savedConv);
        sampleRateConvCombo->setCurrentIndex(convIdx >= 0 ? convIdx : 1);
    }
    connect(sampleRateConvCombo, &QComboBox::currentTextChanged,
            this, [](const QString& text) {
        Settings::instance()->setSampleRateConversion(text);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Sample Rate Conversion"),
        QString(),
        sampleRateConvCombo));

    // ── Section: DSD ───────────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("DSD")));

    auto* dsdPlaybackCombo = new StyledComboBox();
    dsdPlaybackCombo->addItem(
        QStringLiteral("PCM Conversion (Recommended)"), QStringLiteral("pcm"));
    dsdPlaybackCombo->addItem(
        QStringLiteral("Native DoP (External DAC only)"), QStringLiteral("dop"));

    // Restore saved setting
    QString savedDsdMode = Settings::instance()->dsdPlaybackMode();
    int dsdModeIdx = dsdPlaybackCombo->findData(savedDsdMode);
    if (dsdModeIdx >= 0) dsdPlaybackCombo->setCurrentIndex(dsdModeIdx);

    connect(dsdPlaybackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [dsdPlaybackCombo](int index) {
        QString mode = dsdPlaybackCombo->itemData(index).toString();
        Settings::instance()->setDsdPlaybackMode(mode);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("DSD Playback"),
        QStringLiteral("PCM works with all speakers. DoP requires a compatible external DAC."),
        dsdPlaybackCombo));

    // ── Section: Quality ───────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Quality")));

    auto* bitPerfectSwitch = new StyledSwitch();
    bitPerfectSwitch->setChecked(Settings::instance()->bitPerfectMode());
    connect(bitPerfectSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        AudioEngine::instance()->setBitPerfectMode(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Bit-Perfect Mode"),
        QStringLiteral("Bypass all DSP processing (gain, EQ, plugins) for purest output"),
        bitPerfectSwitch));

    auto* autoSampleRateSwitch = new StyledSwitch();
    autoSampleRateSwitch->setChecked(Settings::instance()->autoSampleRate());
    connect(autoSampleRateSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        AudioEngine::instance()->setAutoSampleRate(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Auto Sample Rate"),
        QStringLiteral("Match output sample rate to source file rate when supported by DAC"),
        autoSampleRateSwitch));

    // Max DAC rate info
    double maxRate = AudioEngine::instance()->maxDeviceSampleRate();
    QString maxRateStr = (maxRate >= 1000.0)
        ? QStringLiteral("%1 kHz").arg(maxRate / 1000.0, 0, 'f', 1)
        : QStringLiteral("%1 Hz").arg(maxRate, 0, 'f', 0);
    auto* maxRateLabel = new QLabel(
        QStringLiteral("Current DAC max rate: %1").arg(maxRateStr));
    maxRateLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 4px 0;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    layout->addWidget(maxRateLabel);

    auto* dsdOutputQualityCombo = new StyledComboBox();
    dsdOutputQualityCombo->addItem(
        QStringLiteral("Standard (44.1 kHz)"), QStringLiteral("44100"));
    dsdOutputQualityCombo->addItem(
        QStringLiteral("High (88.2 kHz)"), QStringLiteral("88200"));
    dsdOutputQualityCombo->addItem(
        QStringLiteral("Very High (176.4 kHz)"), QStringLiteral("176400"));
    dsdOutputQualityCombo->addItem(
        QStringLiteral("Maximum (352.8 kHz)"), QStringLiteral("352800"));

    // Restore saved setting
    QString savedDsdQuality = Settings::instance()->dsdOutputQuality();
    int dsdQualIdx = dsdOutputQualityCombo->findData(savedDsdQuality);
    if (dsdQualIdx >= 0) dsdOutputQualityCombo->setCurrentIndex(dsdQualIdx);

    connect(dsdOutputQualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [dsdOutputQualityCombo](int index) {
        QString quality = dsdOutputQualityCombo->itemData(index).toString();
        Settings::instance()->setDsdOutputQuality(quality);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("DSD Output Quality"),
        QStringLiteral("Target PCM sample rate for DSD-to-PCM conversion"),
        dsdOutputQualityCombo));

    // ── Section: Upsampling ──────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Upsampling")));

    auto* upsampler = AudioEngine::instance()->upsampler();

    auto* upsamplingSwitch = new StyledSwitch();
    upsamplingSwitch->setChecked(Settings::instance()->upsamplingEnabled());

    // Mode combo
    auto* upsamplingModeCombo = new StyledComboBox();
    upsamplingModeCombo->addItem(QStringLiteral("None"),         (int)UpsamplingMode::None);
    upsamplingModeCombo->addItem(QStringLiteral("2x"),           (int)UpsamplingMode::Double);
    upsamplingModeCombo->addItem(QStringLiteral("4x"),           (int)UpsamplingMode::Quadruple);
    upsamplingModeCombo->addItem(QStringLiteral("Max DAC Rate"), (int)UpsamplingMode::MaxRate);
    upsamplingModeCombo->addItem(QStringLiteral("Power of 2"),   (int)UpsamplingMode::PowerOf2);
    upsamplingModeCombo->addItem(QStringLiteral("DSD256 Rate"),  (int)UpsamplingMode::DSD256Rate);
    upsamplingModeCombo->addItem(QStringLiteral("Fixed Rate"),   (int)UpsamplingMode::Fixed);
    {
        int savedMode = Settings::instance()->upsamplingMode();
        int modeIdx = upsamplingModeCombo->findData(savedMode);
        if (modeIdx >= 0) upsamplingModeCombo->setCurrentIndex(modeIdx);
    }

    // Quality combo
    auto* upsamplingQualityCombo = new StyledComboBox();
    upsamplingQualityCombo->addItem(QStringLiteral("Quick"),     (int)UpsamplingQuality::Quick);
    upsamplingQualityCombo->addItem(QStringLiteral("Low"),       (int)UpsamplingQuality::Low);
    upsamplingQualityCombo->addItem(QStringLiteral("Medium"),    (int)UpsamplingQuality::Medium);
    upsamplingQualityCombo->addItem(QStringLiteral("High"),      (int)UpsamplingQuality::High);
    upsamplingQualityCombo->addItem(QStringLiteral("Very High"), (int)UpsamplingQuality::VeryHigh);
    {
        int savedQuality = Settings::instance()->upsamplingQuality();
        int qualIdx = upsamplingQualityCombo->findData(savedQuality);
        if (qualIdx >= 0) upsamplingQualityCombo->setCurrentIndex(qualIdx);
    }

    // Filter combo
    auto* upsamplingFilterCombo = new StyledComboBox();
    upsamplingFilterCombo->addItem(QStringLiteral("Linear Phase"),  (int)UpsamplingFilter::LinearPhase);
    upsamplingFilterCombo->addItem(QStringLiteral("Minimum Phase"), (int)UpsamplingFilter::MinimumPhase);
    upsamplingFilterCombo->addItem(QStringLiteral("Steep"),         (int)UpsamplingFilter::SteepFilter);
    upsamplingFilterCombo->addItem(QStringLiteral("Slow Rolloff"),  (int)UpsamplingFilter::SlowRolloff);
    {
        int savedFilter = Settings::instance()->upsamplingFilter();
        int filterIdx = upsamplingFilterCombo->findData(savedFilter);
        if (filterIdx >= 0) upsamplingFilterCombo->setCurrentIndex(filterIdx);
    }

    // Fixed rate combo (only visible when mode=Fixed)
    auto* fixedRateCombo = new StyledComboBox();
    fixedRateCombo->addItem(QStringLiteral("88.2 kHz"),  88200);
    fixedRateCombo->addItem(QStringLiteral("96 kHz"),    96000);
    fixedRateCombo->addItem(QStringLiteral("176.4 kHz"), 176400);
    fixedRateCombo->addItem(QStringLiteral("192 kHz"),   192000);
    fixedRateCombo->addItem(QStringLiteral("352.8 kHz"), 352800);
    fixedRateCombo->addItem(QStringLiteral("384 kHz"),   384000);
    {
        int savedFixed = Settings::instance()->upsamplingFixedRate();
        int fixedIdx = fixedRateCombo->findData(savedFixed);
        if (fixedIdx >= 0) fixedRateCombo->setCurrentIndex(fixedIdx);
    }

    // Info label showing current upsampling state
    auto* upsamplingInfoLabel = new QLabel();
    upsamplingInfoLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 2px 0 8px 0;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));

    // Create fixed rate row early so the lambda can capture it
    auto* fixedRateRow = createSettingRow(
        QStringLiteral("Fixed Rate"),
        QString(),
        fixedRateCombo);

    // Helper to update visibility and info label
    auto updateUpsamplingUI = [=]() {
        bool enabled = upsamplingSwitch->isChecked();
        int modeVal = upsamplingModeCombo->currentData().toInt();
        bool isFixed = (modeVal == (int)UpsamplingMode::Fixed);

        upsamplingModeCombo->setEnabled(enabled);
        upsamplingQualityCombo->setEnabled(enabled);
        upsamplingFilterCombo->setEnabled(enabled);
        fixedRateCombo->setEnabled(enabled && isFixed);
        fixedRateRow->setVisible(isFixed);

        QString desc = upsampler->getDescription();
        if (!enabled) {
            upsamplingInfoLabel->setText(QStringLiteral("Upsampling disabled"));
        } else if (upsampler->isActive()) {
            upsamplingInfoLabel->setText(desc);
        } else if (upsampler->isEnabled()) {
            upsamplingInfoLabel->setText(QStringLiteral("Enabled (takes effect on next track)"));
        } else {
            upsamplingInfoLabel->setText(QStringLiteral("Upsampling disabled"));
        }
    };

    // Connections
    auto* engine = AudioEngine::instance();

    connect(upsamplingSwitch, &StyledSwitch::toggled, this,
            [=](bool checked) {
        Settings::instance()->setUpsamplingEnabled(checked);
        upsampler->setEnabled(checked);
        updateUpsamplingUI();
        engine->applyUpsamplingChange();
    });

    connect(upsamplingModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [=](int index) {
        if (index < 0) return;
        int mode = upsamplingModeCombo->itemData(index).toInt();
        Settings::instance()->setUpsamplingMode(mode);
        upsampler->setMode(static_cast<UpsamplingMode>(mode));
        updateUpsamplingUI();
        engine->applyUpsamplingChange();
    });

    connect(upsamplingQualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [=](int index) {
        if (index < 0) return;
        int quality = upsamplingQualityCombo->itemData(index).toInt();
        Settings::instance()->setUpsamplingQuality(quality);
        upsampler->setQuality(static_cast<UpsamplingQuality>(quality));
        updateUpsamplingUI();
        engine->applyUpsamplingChange();
    });

    connect(upsamplingFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [=](int index) {
        if (index < 0) return;
        int filter = upsamplingFilterCombo->itemData(index).toInt();
        Settings::instance()->setUpsamplingFilter(filter);
        upsampler->setFilter(static_cast<UpsamplingFilter>(filter));
        updateUpsamplingUI();
        engine->applyUpsamplingChange();
    });

    connect(fixedRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [=](int index) {
        if (index < 0) return;
        int rate = fixedRateCombo->itemData(index).toInt();
        Settings::instance()->setUpsamplingFixedRate(rate);
        upsampler->setFixedRate(rate);
        updateUpsamplingUI();
        engine->applyUpsamplingChange();
    });

    // Update info label when signal path changes (track change)
    connect(AudioEngine::instance(), &AudioEngine::signalPathChanged,
            this, [=]() { updateUpsamplingUI(); });

    // Add widgets to layout
    layout->addWidget(createSettingRow(
        QStringLiteral("Enable Upsampling"),
        QStringLiteral("Upsample audio using SoX Resampler (libsoxr) for higher resolution output"),
        upsamplingSwitch));

    layout->addWidget(createSettingRow(
        QStringLiteral("Upsampling Mode"),
        QStringLiteral("Target output rate strategy"),
        upsamplingModeCombo));

    layout->addWidget(createSettingRow(
        QStringLiteral("Quality"),
        QStringLiteral("Higher quality uses more CPU"),
        upsamplingQualityCombo));

    layout->addWidget(createSettingRow(
        QStringLiteral("Filter Type"),
        QString(),
        upsamplingFilterCombo));

    layout->addWidget(fixedRateRow);

    layout->addWidget(upsamplingInfoLabel);

    updateUpsamplingUI();

    // ── DSP Pipeline Card ─────────────────────────────────────────
    createDSPCard(layout);

    // ── VST3 Plugins Card ──────────────────────────────────────────
    createVSTCard(layout);

    // Load saved active VST plugins
    loadVstPlugins();

    layout->addStretch();

    scrollArea->setWidget(content);
    return scrollArea;
}

// ═════════════════════════════════════════════════════════════════════
//  createLibraryTab
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createLibraryTab()
{
    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 16, 12, 16);
    layout->setSpacing(0);

    // ── Section: Monitored Folders ─────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Monitored Folders")));

    // Dynamic folder list
    m_foldersContainer = new QWidget();
    m_foldersLayout = new QVBoxLayout(m_foldersContainer);
    m_foldersLayout->setContentsMargins(0, 0, 0, 0);
    m_foldersLayout->setSpacing(4);

    rebuildFolderList();

    layout->addWidget(m_foldersContainer);

    // Add Folder button
    auto* addFolderBtn = new StyledButton(QStringLiteral("Add Folder"),
                                           QStringLiteral("outline"));
    addFolderBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/folder.svg")));
    connect(addFolderBtn, &QPushButton::clicked, this, &SettingsView::onAddFolderClicked);
    layout->addWidget(addFolderBtn);

    // ── Section: Scanning ──────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Scanning")));

    auto* autoScanSwitch = new StyledSwitch();
    autoScanSwitch->setChecked(Settings::instance()->autoScanOnStartup());
    connect(autoScanSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setAutoScanOnStartup(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Auto-scan on startup"),
        QString(),
        autoScanSwitch));

    auto* watchChangesSwitch = new StyledSwitch();
    watchChangesSwitch->setChecked(Settings::instance()->watchForChanges());
    connect(watchChangesSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setWatchForChanges(checked);
        LibraryScanner::instance()->setWatchEnabled(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Watch for changes"),
        QStringLiteral("Automatically detect new files"),
        watchChangesSwitch));

    // Scan Now button + status
    auto* scanRow = new QWidget();
    auto* scanRowLayout = new QHBoxLayout(scanRow);
    scanRowLayout->setContentsMargins(0, 8, 0, 8);
    scanRowLayout->setSpacing(12);

    m_scanNowBtn = new StyledButton(QStringLiteral("Scan Now"), QStringLiteral("default"));
    m_scanNowBtn->setObjectName(QStringLiteral("ScanNowButton"));
    m_scanNowBtn->setFixedSize(130, UISizes::buttonHeight);
    m_scanNowBtn->setFocusPolicy(Qt::NoFocus);
    m_scanNowBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Primary)
        + QStringLiteral(
        " QPushButton#ScanNowButton {"
        "  min-width: 130px; max-width: 130px;"
        "  min-height: 32px; max-height: 32px;"
        "  padding: 0px 16px;"
        "}"
    ));
    connect(m_scanNowBtn, &QPushButton::clicked, this, &SettingsView::onScanNowClicked);
    scanRowLayout->addWidget(m_scanNowBtn);

    m_fullRescanBtn = new StyledButton(QStringLiteral("Full Rescan"), QStringLiteral("default"), scanRow);
    m_fullRescanBtn->setObjectName(QStringLiteral("FullRescanButton"));
    m_fullRescanBtn->setFixedSize(130, UISizes::buttonHeight);
    m_fullRescanBtn->setFocusPolicy(Qt::NoFocus);
    m_fullRescanBtn->setCursor(Qt::PointingHandCursor);
    m_fullRescanBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Destructive)
        + QStringLiteral(
        " QPushButton#FullRescanButton {"
        "  min-width: 130px; max-width: 130px;"
        "  min-height: 32px; max-height: 32px;"
        "  padding: 0px 16px;"
        "}"
    ));
    connect(m_fullRescanBtn, &QPushButton::clicked, this, &SettingsView::onFullRescanClicked);
    scanRowLayout->addWidget(m_fullRescanBtn);

    m_scanStatusLabel = new QLabel(QStringLiteral(""), scanRow);
    m_scanStatusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    scanRowLayout->addWidget(m_scanStatusLabel, 1);

    layout->addWidget(scanRow);

    auto* scanIntervalCombo = new StyledComboBox();
    scanIntervalCombo->addItems({
        QStringLiteral("Manual"),
        QStringLiteral("Every hour"),
        QStringLiteral("Every 6 hours"),
        QStringLiteral("Daily")
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Scan interval"),
        QString(),
        scanIntervalCombo));

    // Ignored file extensions
    auto* ignoreEdit = new QLineEdit();
    ignoreEdit->setText(Settings::instance()->ignoreExtensions().join(QStringLiteral("; ")));
    ignoreEdit->setPlaceholderText(QStringLiteral("cue; log; txt; ..."));
    ignoreEdit->setStyleSheet(
        QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                        "border-radius: 6px; padding: 4px 8px; font-size: 12px; }")
            .arg(ThemeManager::instance()->colors().backgroundSecondary,
                 ThemeManager::instance()->colors().foreground,
                 ThemeManager::instance()->colors().border));
    connect(ignoreEdit, &QLineEdit::editingFinished, this, [ignoreEdit]() {
        QStringList exts;
        for (const QString& ext : ignoreEdit->text().split(QRegularExpression(QStringLiteral("[;,\\s]+")),
                                                            Qt::SkipEmptyParts))
            exts.append(ext.trimmed().toLower());
        Settings::instance()->setIgnoreExtensions(exts);
    });

    auto* resetIgnoreBtn = new StyledButton(QStringLiteral("Reset"), QStringLiteral("outline"));
    resetIgnoreBtn->setFixedWidth(70);
    connect(resetIgnoreBtn, &QPushButton::clicked, this, [ignoreEdit]() {
        Settings::instance()->setIgnoreExtensions({});
        ignoreEdit->setText(Settings::instance()->ignoreExtensions().join(QStringLiteral("; ")));
    });

    auto* ignoreRow = new QWidget();
    auto* ignoreRowLayout = new QHBoxLayout(ignoreRow);
    ignoreRowLayout->setContentsMargins(0, 0, 0, 0);
    ignoreRowLayout->setSpacing(8);
    ignoreRowLayout->addWidget(ignoreEdit, 1);
    ignoreRowLayout->addWidget(resetIgnoreBtn);

    layout->addWidget(createSettingRow(
        QStringLiteral("Ignored file extensions"),
        QStringLiteral("Extensions to skip during scan (semicolon-separated)"),
        ignoreRow));

    // ── Section: Organization ──────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Organization")));

    auto* namingPatternCombo = new StyledComboBox();
    namingPatternCombo->addItems({
        QStringLiteral("{artist}/{album}/{track} - {title}"),
        QStringLiteral("{artist} - {album}/{track}. {title}"),
        QStringLiteral("{album}/{track} - {title}")
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Naming Pattern"),
        QString(),
        namingPatternCombo));

    auto* groupCompSwitch = new StyledSwitch();
    groupCompSwitch->setChecked(true);
    layout->addWidget(createSettingRow(
        QStringLiteral("Group compilations"),
        QString(),
        groupCompSwitch));

    // ── Section: Auto-Organize ────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Auto-Organize")));

    auto* autoOrgSwitch = new StyledSwitch();
    autoOrgSwitch->setChecked(Settings::instance()->autoOrganizeOnImport());
    connect(autoOrgSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setAutoOrganizeOnImport(checked);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Auto-organize on import"),
        QStringLiteral("Rename and move files to match metadata"),
        autoOrgSwitch));

    auto* orgPatternCombo = new StyledComboBox();
    orgPatternCombo->setEditable(true);
    orgPatternCombo->addItems({
        QStringLiteral("%artist%/%album%/%track% - %title%"),
        QStringLiteral("%artist% - %album%/%track%. %title%"),
        QStringLiteral("%genre%/%artist%/%album%/%track% - %title%")
    });
    orgPatternCombo->setCurrentText(Settings::instance()->organizePattern());
    connect(orgPatternCombo, &QComboBox::currentTextChanged, this, [](const QString& text) {
        Settings::instance()->setOrganizePattern(text);
    });
    layout->addWidget(createSettingRow(
        QStringLiteral("Organize pattern"),
        QStringLiteral("Tokens: %artist%, %album%, %title%, %track%, %year%, %genre%"),
        orgPatternCombo));

    // ── Pattern preview example ─────────────────────────────────────
    auto* previewLabel = new QLabel(this);
    previewLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 4px 0;")
            .arg(ThemeManager::instance()->colors().accent));
    auto updatePreview = [previewLabel, orgPatternCombo]() {
        QString example = orgPatternCombo->currentText();
        example.replace(QStringLiteral("%artist%"), QStringLiteral("Adele"));
        example.replace(QStringLiteral("%album%"), QStringLiteral("25"));
        example.replace(QStringLiteral("%title%"), QStringLiteral("Hello"));
        example.replace(QStringLiteral("%track%"), QStringLiteral("01"));
        example.replace(QStringLiteral("%year%"), QStringLiteral("2015"));
        example.replace(QStringLiteral("%genre%"), QStringLiteral("Pop"));
        previewLabel->setText(QStringLiteral("Example: %1.flac").arg(example));
    };
    connect(orgPatternCombo, &QComboBox::currentTextChanged, this, updatePreview);
    updatePreview();
    layout->addWidget(previewLabel);

    // ── Section: Library Cleanup ─────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Library Cleanup")));

    auto* cleanupDesc = new QLabel(
        QStringLiteral("Remove duplicate tracks and entries for files that no longer exist."), this);
    cleanupDesc->setWordWrap(true);
    cleanupDesc->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 4px 0;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    layout->addWidget(cleanupDesc);

    auto* cleanupBtn = new StyledButton(QStringLiteral("Clean Up Library"),
                                         QStringLiteral("default"));
    cleanupBtn->setFixedHeight(UISizes::buttonHeight);
    cleanupBtn->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Destructive));
    connect(cleanupBtn, &QPushButton::clicked, this, []() {
        LibraryDatabase::instance()->removeDuplicates();
        MusicDataProvider::instance()->reloadFromDatabase();
    });
    layout->addWidget(cleanupBtn);

    // ── Section: Library Rollback ────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Library Rollback")));

    auto* rollbackDesc = new QLabel(
        QStringLiteral("Restore library data from before the last rescan or metadata rebuild. "
                       "Your music files are never modified."), this);
    rollbackDesc->setWordWrap(true);
    rollbackDesc->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none; padding: 4px 0;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    layout->addWidget(rollbackDesc);

    m_restoreButton = new StyledButton(QStringLiteral("Restore Previous Library Data"),
                                        QStringLiteral("default"));
    m_restoreButton->setFixedHeight(UISizes::buttonHeight);
    m_restoreButton->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary));
    m_restoreButton->setEnabled(LibraryDatabase::instance()->hasBackup());
    connect(m_restoreButton, &QPushButton::clicked, this, [this]() {
        auto* db = LibraryDatabase::instance();
        QDateTime ts = db->backupTimestamp();
        QString timeStr = ts.isValid() ? ts.toString(QStringLiteral("yyyy-MM-dd hh:mm")) : QStringLiteral("unknown");

        if (!StyledMessageBox::confirm(this,
                QStringLiteral("Restore Library Data"),
                QStringLiteral("Restore library data from %1?\n\n"
                               "This will undo the last metadata rebuild or rescan.\n"
                               "Your music files will not be affected.").arg(timeStr)))
            return;

        bool ok = db->restoreFromBackup();
        if (ok) {
            MusicDataProvider::instance()->reloadFromDatabase();
            StyledMessageBox::info(this,
                QStringLiteral("Restored"),
                QStringLiteral("Library data restored successfully."));
            m_restoreButton->setEnabled(db->hasBackup());
        } else {
            StyledMessageBox::warning(this,
                QStringLiteral("Restore Failed"),
                QStringLiteral("Could not restore from backup."));
        }
    });
    layout->addWidget(m_restoreButton);

    // Update restore button when database changes
    connect(LibraryDatabase::instance(), &LibraryDatabase::databaseChanged,
            this, [this]() {
        if (m_restoreButton)
            m_restoreButton->setEnabled(LibraryDatabase::instance()->hasBackup());
    });

    layout->addStretch();

    scrollArea->setWidget(content);
    return scrollArea;
}

// ═════════════════════════════════════════════════════════════════════
//  rebuildFolderList
// ═════════════════════════════════════════════════════════════════════

void SettingsView::rebuildFolderList()
{
    // Clear existing folder widgets
    while (m_foldersLayout->count() > 0) {
        QLayoutItem* item = m_foldersLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const QStringList folders = Settings::instance()->libraryFolders();

    if (folders.isEmpty()) {
        auto* emptyLabel = new QLabel(
            QStringLiteral("No folders added yet. Click \"Add Folder\" to get started."),
            m_foldersContainer);
        emptyLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; border: none; padding: 8px 0;")
                .arg(ThemeManager::instance()->colors().foregroundMuted));
        m_foldersLayout->addWidget(emptyLabel);
        return;
    }

    for (const QString& folder : folders) {
        auto* folderWidget = new QWidget(m_foldersContainer);
        auto* folderLayout = new QHBoxLayout(folderWidget);
        folderLayout->setContentsMargins(0, 4, 0, 4);
        folderLayout->setSpacing(8);

        auto* folderLabel = new QLabel(folder, folderWidget);
        folderLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; border: none;")
                .arg(ThemeManager::instance()->colors().foreground));
        folderLayout->addWidget(folderLabel, 1);

        auto* removeBtn = new StyledButton(QStringLiteral(""),
                                            QStringLiteral("ghost"),
                                            folderWidget);
        removeBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/trash-2.svg")));
        removeBtn->setFixedSize(UISizes::smallButtonSize, UISizes::smallButtonSize);
        removeBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));

        QString folderPath = folder; // capture for lambda
        connect(removeBtn, &QPushButton::clicked, this, [this, folderPath]() {
            onRemoveFolderClicked(folderPath);
        });
        folderLayout->addWidget(removeBtn);

        m_foldersLayout->addWidget(folderWidget);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Folder management slots
// ═════════════════════════════════════════════════════════════════════

void SettingsView::onAddFolderClicked()
{
    QString folder = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Music Folder"),
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!folder.isEmpty()) {
#ifdef Q_OS_MACOS
        BookmarkManager::instance()->saveBookmark(folder);
#endif
        Settings::instance()->addLibraryFolder(folder);
        rebuildFolderList();

        // Auto-scan all folders including newly added one
        QStringList folders = Settings::instance()->libraryFolders();
        LibraryScanner::instance()->scanFolders(folders);
        qDebug() << "[Settings] Folder added — auto-scan triggered:" << folder;
    }
}

void SettingsView::onRemoveFolderClicked(const QString& folder)
{
#ifdef Q_OS_MACOS
    BookmarkManager::instance()->removeBookmark(folder);
#endif
    Settings::instance()->removeLibraryFolder(folder);
    rebuildFolderList();
}

void SettingsView::onScanNowClicked()
{
    QStringList folders = Settings::instance()->libraryFolders();
    if (folders.isEmpty()) {
        m_scanStatusLabel->setText(QStringLiteral("No folders to scan. Add a folder first."));
        return;
    }

    m_scanNowBtn->setEnabled(false);
    m_fullRescanBtn->setEnabled(false);
    m_scanStatusLabel->setText(QStringLiteral("Scanning..."));

    LibraryScanner::instance()->scanFolders(folders);
}

void SettingsView::onFullRescanClicked()
{
    QStringList folders = Settings::instance()->libraryFolders();
    if (folders.isEmpty()) {
        m_scanStatusLabel->setText(QStringLiteral("No folders to scan. Add a folder first."));
        return;
    }

    if (!StyledMessageBox::confirm(this,
            QStringLiteral("Full Rescan"),
            QStringLiteral("This will clear your library and rescan all files.\nPlaylists will be preserved.\n\nContinue?")))
        return;

    m_scanNowBtn->setEnabled(false);
    m_fullRescanBtn->setEnabled(false);
    m_scanStatusLabel->setText(QStringLiteral("Backing up and rescanning..."));

    // Auto-backup before destructive operation
    auto* db = LibraryDatabase::instance();
    db->createBackup();
    if (m_restoreButton)
        m_restoreButton->setEnabled(db->hasBackup());
    db->clearAllData(true);  // preserves playlists

    LibraryScanner::instance()->scanFolders(folders);
}

void SettingsView::onScanProgress(int current, int total)
{
    m_scanStatusLabel->setText(
        QStringLiteral("Scanning... %1 / %2 files").arg(current).arg(total));
}

void SettingsView::onScanFinished(int tracksFound)
{
    m_scanNowBtn->setEnabled(true);
    m_fullRescanBtn->setEnabled(true);
    m_scanStatusLabel->setText(
        QStringLiteral("Scan complete. %1 tracks found.").arg(tracksFound));
    // reloadFromDatabase() already triggered by rebuildAlbumsAndArtists → databaseChanged signal
}

// ═════════════════════════════════════════════════════════════════════
//  saveVstPlugins / loadVstPlugins
// ═════════════════════════════════════════════════════════════════════

void SettingsView::saveVstPlugins()
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

void SettingsView::loadVstPlugins()
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
                    displayName = QString::fromStdString(plugins[i].name) +
                        QStringLiteral(" (") +
                        QString::fromStdString(plugins[i].vendor) +
                        QStringLiteral(")");
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
//  createDSPCard — 20-band Parametric EQ (REW-style)
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createDSPCard(QVBoxLayout* parentLayout)
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
            this, &SettingsView::applyEQPreset);
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
//  rebuildBandRows — create/show/hide rows for active band count
// ═════════════════════════════════════════════════════════════════════

void SettingsView::rebuildBandRows()
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
//  syncBandToProcessor — push UI values to DSP and settings
// ═════════════════════════════════════════════════════════════════════

void SettingsView::syncBandToProcessor(int bandIndex)
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

void SettingsView::updateEQGraph()
{
    if (!m_eqGraph) return;

    auto* pipeline = AudioEngine::instance()->dspPipeline();
    if (pipeline && pipeline->equalizerProcessor()) {
        auto response = pipeline->equalizerProcessor()->getFrequencyResponse(512);
        m_eqGraph->setResponse(response);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  createVSTCard — modern VST3 Plugins card
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createVSTCard(QVBoxLayout* parentLayout)
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
                auto* item = new QListWidgetItem(
                    QString::fromStdString(plugins[i].name) +
                    QStringLiteral(" (") +
                    QString::fromStdString(plugins[i].vendor) +
                    QStringLiteral(")"));
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

// ═════════════════════════════════════════════════════════════════════
//  applyEQPreset — set EQ bands from a named preset (10-band presets)
// ═════════════════════════════════════════════════════════════════════

void SettingsView::applyEQPreset(const QString& presetName)
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

// ═════════════════════════════════════════════════════════════════════
//  createAppleMusicTab
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createAppleMusicTab()
{
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
    layout->addWidget(createSectionHeader(QStringLiteral("Connection")));

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

    layout->addWidget(createSettingRow(
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
    layout->addWidget(createSectionHeader(QStringLiteral("Playback")));

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

    layout->addWidget(createSettingRow(
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
    return scrollArea;
}

/* TODO: restore when Tidal API available
// ═════════════════════════════════════════════════════════════════════
//  createTidalTab
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createTidalTab()
{
    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto c = ThemeManager::instance()->colors();
    auto* tm = TidalManager::instance();

    // ── Connection section ──────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Connection")));

    // Status label
    m_tidalStatusLabel = new QLabel(QStringLiteral("Not connected"), content);
    m_tidalStatusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; border: none;").arg(c.foregroundMuted));

    auto updateTidalUI = [this, c](bool loggedIn, const QString& username) {
        if (loggedIn) {
            m_tidalStatusLabel->setText(QStringLiteral("Connected as %1").arg(username));
            m_tidalStatusLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; font-weight: bold; border: none;").arg(c.success));
            m_tidalConnectBtn->hide();
            m_tidalLogoutBtn->show();
        } else {
            m_tidalStatusLabel->setText(QStringLiteral("Not connected"));
            m_tidalStatusLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; border: none;").arg(c.foregroundMuted));
            m_tidalConnectBtn->show();
            m_tidalLogoutBtn->hide();
        }
    };

    // Connect button
    m_tidalConnectBtn = new StyledButton(QStringLiteral("Connect Tidal"),
                                          QStringLiteral("primary"), content);
    m_tidalConnectBtn->setFixedSize(200, UISizes::buttonHeight);

    connect(m_tidalConnectBtn, &QPushButton::clicked, this, [tm]() {
        tm->loginWithBrowser();
    });

    // Logout button
    m_tidalLogoutBtn = new StyledButton(QStringLiteral("Logout"),
                                         QStringLiteral("default"), content);
    m_tidalLogoutBtn->setFixedSize(200, UISizes::buttonHeight);
    m_tidalLogoutBtn->hide();

    connect(m_tidalLogoutBtn, &QPushButton::clicked, this, [tm]() {
        tm->logout();
    });

    // Button container
    auto* btnContainer = new QWidget(content);
    auto* btnLayout = new QHBoxLayout(btnContainer);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(8);
    btnLayout->addWidget(m_tidalConnectBtn);
    btnLayout->addWidget(m_tidalLogoutBtn);
    btnLayout->addStretch();

    // Signal connections
    connect(tm, &TidalManager::userLoggedIn, this, [this, updateTidalUI](const QString& username) {
        updateTidalUI(true, username);
    });

    connect(tm, &TidalManager::userLoggedOut, this, [this, updateTidalUI]() {
        updateTidalUI(false, QString());
    });

    connect(tm, &TidalManager::loginError, this, [this, c](const QString& error) {
        m_tidalStatusLabel->setText(QStringLiteral("Login failed: %1").arg(error));
        m_tidalStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; border: none;").arg(c.error));
    });

    layout->addWidget(createSettingRow(
        QStringLiteral("Tidal Account"),
        QStringLiteral("Connect to search and browse the Tidal catalog"),
        btnContainer));
    layout->addWidget(m_tidalStatusLabel);

    // ── Subscription status ─────────────────────────────────────────
    m_tidalSubLabel = new QLabel(content);
    m_tidalSubLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.foregroundMuted));
    m_tidalSubLabel->setVisible(false);
    layout->addWidget(m_tidalSubLabel);

    // Update subscription label when logged in
    auto updateSubLabel = [this, c](bool loggedIn, const QString& country) {
        m_tidalSubLabel->setVisible(loggedIn);
        if (loggedIn) {
            m_tidalSubLabel->setText(QStringLiteral("Tidal account connected (country: %1)").arg(country));
            m_tidalSubLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.success));
        }
    };

    connect(tm, &TidalManager::userLoggedIn, this, [tm, updateSubLabel](const QString&) {
        updateSubLabel(true, tm->countryCode());
    });

    connect(tm, &TidalManager::userLoggedOut, this, [this, c]() {
        m_tidalSubLabel->setVisible(false);
    });

    // Initialize UI state
    if (tm->isUserLoggedIn()) {
        updateTidalUI(true, tm->username());
        updateSubLabel(true, tm->countryCode());
    }

    // ── Playback section ────────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Playback")));

    auto* tidalQualityLabel = new QLabel(
        QStringLiteral("30-second previews"),
        content);
    tidalQualityLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: bold; border: none;").arg(c.foreground));

    layout->addWidget(createSettingRow(
        QStringLiteral("Stream Quality"),
        QStringLiteral("Preview playback via Tidal Embed Player. Full playback coming in a future update."),
        tidalQualityLabel));

    // ── API Status ─────────────────────────────────────────────────
    layout->addSpacing(16);
    layout->addWidget(createSectionHeader(QStringLiteral("API Status")));

    auto* apiStatusLabel = new QLabel(content);
    if (tm->isAuthenticated()) {
        apiStatusLabel->setText(QStringLiteral("API authenticated (search available)"));
        apiStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.success));
    } else {
        apiStatusLabel->setText(QStringLiteral("API not authenticated"));
        apiStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.foregroundMuted));
    }

    connect(tm, &TidalManager::authenticated, this, [apiStatusLabel, c]() {
        apiStatusLabel->setText(QStringLiteral("API authenticated (search available)"));
        apiStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;").arg(c.success));
    });

    layout->addWidget(apiStatusLabel);

    layout->addStretch();
    scrollArea->setWidget(content);
    return scrollArea;
}
*/

// ═════════════════════════════════════════════════════════════════════
//  createAppearanceTab
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createAppearanceTab()
{
    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 16, 12, 16);
    layout->setSpacing(0);

    // ── Section: Theme ─────────────────────────────────────────────
    layout->addWidget(createSectionHeader(QStringLiteral("Theme")));

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
    layout->addWidget(createSectionHeader(QStringLiteral("Display")));

    auto* formatBadgesSwitch = new StyledSwitch();
    formatBadgesSwitch->setChecked(true);
    layout->addWidget(createSettingRow(
        QStringLiteral("Show format badges"),
        QString(),
        formatBadgesSwitch));

    auto* albumArtSwitch = new StyledSwitch();
    albumArtSwitch->setChecked(true);
    layout->addWidget(createSettingRow(
        QStringLiteral("Show album art"),
        QString(),
        albumArtSwitch));

    auto* compactModeSwitch = new StyledSwitch();
    compactModeSwitch->setChecked(false);
    layout->addWidget(createSettingRow(
        QStringLiteral("Compact mode"),
        QStringLiteral("Reduce spacing for more content"),
        compactModeSwitch));

    // ── Section: Language ────────────────────────────────────────────
    layout->addWidget(createSectionHeader(tr("Language")));

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

    layout->addWidget(createSettingRow(
        tr("Language"),
        tr("Select the display language"),
        langCombo));

    layout->addStretch();

    scrollArea->setWidget(content);
    return scrollArea;
}

// ═════════════════════════════════════════════════════════════════════
//  createAboutTab
// ═════════════════════════════════════════════════════════════════════

QWidget* SettingsView::createAboutTab()
{
    auto* scrollArea = new StyledScrollArea();
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->setAlignment(Qt::AlignHCenter);

    // ── App Logo ───────────────────────────────────────────────────
    auto* logo = new SoranaFlowLogo(80, content);
    layout->addWidget(logo, 0, Qt::AlignHCenter);

    layout->addSpacing(8);

    // ── App Name ───────────────────────────────────────────────────
    auto* appName = new QLabel(QStringLiteral("Sorana Flow"), content);
    appName->setStyleSheet(
        QStringLiteral("color: %1; font-size: 24px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    appName->setAlignment(Qt::AlignCenter);
    layout->addWidget(appName);

    // ── Version ────────────────────────────────────────────────────
    auto* versionLabel = new QLabel(
        QStringLiteral("Version ") + QCoreApplication::applicationVersion(), content);
    versionLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    versionLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(versionLabel);

    // ── Check for Updates ─────────────────────────────────────────
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

    // ── Description ────────────────────────────────────────────────
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

    // ── Separator ──────────────────────────────────────────────────
    auto* separator1 = new QFrame(content);
    separator1->setFrameShape(QFrame::HLine);
    separator1->setStyleSheet(
        QStringLiteral("QFrame { color: %1; }")
            .arg(ThemeManager::instance()->colors().borderSubtle));
    separator1->setFixedHeight(1);
    layout->addWidget(separator1);

    // ── Supported Formats ──────────────────────────────────────────
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

    // ── Separator ──────────────────────────────────────────────────
    auto* separator2 = new QFrame(content);
    separator2->setFrameShape(QFrame::HLine);
    separator2->setStyleSheet(
        QStringLiteral("QFrame { color: %1; }")
            .arg(ThemeManager::instance()->colors().borderSubtle));
    separator2->setFixedHeight(1);
    layout->addWidget(separator2);

    // ── Links ──────────────────────────────────────────────────────
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

    // ── Copyright ──────────────────────────────────────────────────
    auto* copyrightLabel = new QLabel(
        QStringLiteral("\u00A9 2026 Sorana Flow. All rights reserved."), content);
    copyrightLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    copyrightLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(copyrightLabel);

    layout->addStretch();

    scrollArea->setWidget(content);
    return scrollArea;
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
        QWidget* appearanceTab = m_tabWidget->widget(2); // Appearance is index 2
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
