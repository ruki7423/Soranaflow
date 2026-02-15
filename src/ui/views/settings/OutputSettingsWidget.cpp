#include "OutputSettingsWidget.h"
#include "SettingsUtils.h"
#include "../../../core/ThemeManager.h"
#include "../../../core/Settings.h"
#include "../../../core/audio/AudioEngine.h"
#include "../../../core/audio/AudioDeviceManager.h"
#include "../../../core/dsp/UpsamplerProcessor.h"
#include "../../../widgets/StyledSwitch.h"
#include "../../../widgets/StyledComboBox.h"
#include "../../../widgets/StyledScrollArea.h"
#ifdef Q_OS_MACOS
#include "../../../apple/MusicKitPlayer.h"
#endif
#include <QVBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QTimer>

OutputSettingsWidget::OutputSettingsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Section: Output ────────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Output")));

    auto* outputDeviceCombo = new StyledComboBox();
    // Populate with real devices from AudioDeviceManager (single source of truth)
    auto devices = AudioDeviceManager::instance()->outputDevices();
    int savedDeviceIdx = 0;
    uint32_t savedDeviceId = Settings::instance()->outputDeviceId();
    for (size_t i = 0; i < devices.size(); ++i) {
        outputDeviceCombo->addItem(devices[i].name,
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
    layout->addWidget(SettingsUtils::createSettingRow(
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
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Exclusive Mode"),
        QStringLiteral("Take exclusive control of the audio device (hog mode), preventing other apps from using it"),
        exclusiveModeSwitch));

    auto* gaplessSwitch = new StyledSwitch();
    gaplessSwitch->setChecked(Settings::instance()->gaplessPlayback());
    connect(gaplessSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setGaplessPlayback(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
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
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Crossfade"),
        QStringLiteral("Smoothly blend between tracks using an equal-power curve. Disabled for DSD and upsampled playback."),
        crossfadeCombo));

    // ── Section: Autoplay / Radio ──────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Autoplay / Radio")));

    auto* autoplaySwitch = new StyledSwitch();
    autoplaySwitch->setChecked(Settings::instance()->autoplayEnabled());
    connect(autoplaySwitch, &StyledSwitch::toggled, this, [](bool checked) {
        Settings::instance()->setAutoplayEnabled(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Autoplay / Radio"),
        QStringLiteral("When the queue ends, automatically find and play similar tracks using Last.fm recommendations with local library fallback"),
        autoplaySwitch));

    // ── Section: Processing ────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Processing")));

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
        auto bsRange = devMgr->supportedBufferSizes(curDevId);
        double sampleRate = devMgr->currentSampleRate(curDevId);
        if (sampleRate <= 0) sampleRate = 44100.0;

        int selIdx = -1;
        for (uint32_t sz : standardSizes) {
            if (sz < bsRange.minimum || sz > bsRange.maximum) continue;
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
    layout->addWidget(SettingsUtils::createSettingRow(
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
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Sample Rate Conversion"),
        QString(),
        sampleRateConvCombo));

    // ── Section: DSD ───────────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("DSD")));

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
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("DSD Playback"),
        QStringLiteral("PCM works with all speakers. DoP requires a compatible external DAC."),
        dsdPlaybackCombo));

    // ── Section: Quality ───────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Quality")));

    auto* bitPerfectSwitch = new StyledSwitch();
    bitPerfectSwitch->setChecked(Settings::instance()->bitPerfectMode());
    connect(bitPerfectSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        AudioEngine::instance()->setBitPerfectMode(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Bit-Perfect Mode"),
        QStringLiteral("Bypass all DSP processing (gain, EQ, plugins) for purest output"),
        bitPerfectSwitch));

    auto* autoSampleRateSwitch = new StyledSwitch();
    autoSampleRateSwitch->setChecked(Settings::instance()->autoSampleRate());
    connect(autoSampleRateSwitch, &StyledSwitch::toggled, this, [](bool checked) {
        AudioEngine::instance()->setAutoSampleRate(checked);
    });
    layout->addWidget(SettingsUtils::createSettingRow(
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
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("DSD Output Quality"),
        QStringLiteral("Target PCM sample rate for DSD-to-PCM conversion"),
        dsdOutputQualityCombo));

    // ── Section: Upsampling ──────────────────────────────────────────
    layout->addWidget(SettingsUtils::createSectionHeader(QStringLiteral("Upsampling")));

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
    auto* fixedRateRow = SettingsUtils::createSettingRow(
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
    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Enable Upsampling"),
        QStringLiteral("Upsample audio using SoX Resampler (libsoxr) for higher resolution output"),
        upsamplingSwitch));

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Upsampling Mode"),
        QStringLiteral("Target output rate strategy"),
        upsamplingModeCombo));

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Quality"),
        QStringLiteral("Higher quality uses more CPU"),
        upsamplingQualityCombo));

    layout->addWidget(SettingsUtils::createSettingRow(
        QStringLiteral("Filter Type"),
        QString(),
        upsamplingFilterCombo));

    layout->addWidget(fixedRateRow);

    layout->addWidget(upsamplingInfoLabel);

    updateUpsamplingUI();
}
