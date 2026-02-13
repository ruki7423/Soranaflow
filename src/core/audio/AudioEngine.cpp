#include "AudioEngine.h"
#include "AudioDecoder.h"
#include "DSDDecoder.h"
#include "../dsp/DSPPipeline.h"
#include "../dsp/UpsamplerProcessor.h"
#include "../dsp/LoudnessAnalyzer.h"
#include "../Settings.h"
#include "../library/LibraryDatabase.h"

#ifdef Q_OS_MACOS
#include "../../platform/macos/CoreAudioOutput.h"
#endif
#ifdef Q_OS_WIN
#include "../../platform/windows/WASAPIOutput.h"
#endif

#include <QDebug>
#include <QFileInfo>
#include <QtConcurrent>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>

static std::unique_ptr<IAudioOutput> createPlatformAudioOutput() {
#ifdef Q_OS_MACOS
    return std::make_unique<CoreAudioOutput>();
#elif defined(Q_OS_WIN)
    return std::make_unique<WASAPIOutput>();
#else
    return nullptr;
#endif
}

AudioEngine::~AudioEngine()
{
    qDebug() << "=== AudioEngine destructor START ===";

    // Set flags immediately — render callback checks these
    m_shuttingDown.store(true, std::memory_order_release);
    m_destroyed.store(true, std::memory_order_release);

    // Stop position timer
    if (m_positionTimer) {
        m_positionTimer->stop();
        m_positionTimer->deleteLater();
        m_positionTimer = nullptr;
    }

    // IMPORTANT: Stop and destroy output FIRST before decoders
    // The output holds the render callback that references the decoders
    if (m_output) {
        qDebug() << "Stopping audio output...";
        m_output->setRenderCallback(nullptr);
        m_output->stop();

        // Wait for any in-flight render callback to finish (up to 500ms)
        for (int i = 0; i < 50 && m_renderingInProgress.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        qDebug() << "Destroying audio output...";
        m_output.reset();
    }

    // Now safe to destroy decoders — no callback can reference them
    qDebug() << "Destroying decoders...";
    {
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        m_decoder.reset();
        m_dsdDecoder.reset();
        m_nextDecoder.reset();
        m_nextDsdDecoder.reset();
        m_nextTrackReady.store(false, std::memory_order_relaxed);
    }

    // Destroy upsampler and DSP pipeline
    qDebug() << "Destroying upsampler and DSP pipeline...";
    m_upsampler.reset();
    m_dspPipeline.reset();

    qDebug() << "=== AudioEngine destructor DONE ===";
}

// ── Singleton ───────────────────────────────────────────────────────
AudioEngine* AudioEngine::instance()
{
    static AudioEngine s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
    , m_decoder(std::make_unique<AudioDecoder>())
    , m_dsdDecoder(std::make_unique<DSDDecoder>())
    , m_output(createPlatformAudioOutput())
    , m_dspPipeline(std::make_unique<DSPPipeline>())
    , m_upsampler(std::make_unique<UpsamplerProcessor>(this))
    , m_nextDecoder(std::make_unique<AudioDecoder>())
    , m_nextDsdDecoder(std::make_unique<DSDDecoder>())
{
    m_positionTimer = new QTimer(this);
    m_positionTimer->setInterval(50);
    connect(m_positionTimer, &QTimer::timeout, this, &AudioEngine::onPositionTimer);

    // Forward DSP configuration changes to signal path
    connect(m_dspPipeline.get(), &DSPPipeline::configurationChanged,
            this, &AudioEngine::signalPathChanged);

    // Forward upsampler configuration changes to signal path
    connect(m_upsampler.get(), &UpsamplerProcessor::configurationChanged,
            this, &AudioEngine::signalPathChanged);

    // Re-apply headroom gain when settings change mid-playback
    connect(Settings::instance(), &Settings::headroomChanged,
            this, [this]() {
        qDebug() << "[Headroom] Settings changed";
        updateHeadroomGain();
        emit signalPathChanged();
    });

    // Re-apply volume leveling gain when settings change mid-playback
    connect(Settings::instance(), &Settings::volumeLevelingChanged,
            this, [this](bool enabled) {
        qDebug() << "[Volume Leveling] Toggled:" << enabled;
        updateLevelingGain();
        updateHeadroomGain();  // Auto headroom depends on active DSP
    });
    connect(Settings::instance(), &Settings::levelingModeChanged,
            this, [this](int) {
        qDebug() << "[Volume Leveling] Mode changed";
        updateLevelingGain();
    });
    connect(Settings::instance(), &Settings::targetLoudnessChanged,
            this, [this](double lufs) {
        qDebug() << "[Volume Leveling] Target changed:" << lufs << "LUFS";
        updateLevelingGain();
    });

    // Load persisted settings
    m_bitPerfect = Settings::instance()->bitPerfectMode();
    m_output->setBitPerfectMode(m_bitPerfect.load(std::memory_order_relaxed));
    m_autoSampleRate = Settings::instance()->autoSampleRate();
    m_crossfadeDurationMs.store(Settings::instance()->crossfadeDurationMs(), std::memory_order_relaxed);

    // Apply exclusive mode if saved
    if (Settings::instance()->exclusiveMode()) {
        m_output->setHogMode(true);
    }

    // Restore upsampling settings
    auto* settings = Settings::instance();
    m_upsampler->setMaxDacRate((int)m_output->getMaxSampleRate(m_currentDeviceId));
    m_upsampler->setMode(static_cast<UpsamplingMode>(settings->upsamplingMode()));
    m_upsampler->setQuality(static_cast<UpsamplingQuality>(settings->upsamplingQuality()));
    m_upsampler->setFilter(static_cast<UpsamplingFilter>(settings->upsamplingFilter()));
    m_upsampler->setFixedRate(settings->upsamplingFixedRate());
    if (settings->upsamplingEnabled()) {
        m_upsampler->setEnabled(true);
    }

    // Load crossfeed settings
    m_crossfeed.setLevel(
        static_cast<CrossfeedProcessor::Level>(Settings::instance()->crossfeedLevel()));
    m_crossfeed.setEnabled(Settings::instance()->crossfeedEnabled());

    connect(Settings::instance(), &Settings::crossfeedChanged,
            this, [this]() {
        m_crossfeed.setEnabled(Settings::instance()->crossfeedEnabled());
        m_crossfeed.setLevel(
            static_cast<CrossfeedProcessor::Level>(Settings::instance()->crossfeedLevel()));
        updateHeadroomGain();  // Auto headroom may change
        emit signalPathChanged();
        qDebug() << "[Crossfeed]" << (m_crossfeed.isEnabled() ? "ON" : "OFF")
                 << "level:" << m_crossfeed.level();
    });

    // Verify convolution math on startup
    ConvolutionProcessor::selfTest();

    // Load convolution settings
    {
        auto* s = Settings::instance();
        m_convolution.setEnabled(s->convolutionEnabled());
        QString irPath = s->convolutionIRPath();
        if (!irPath.isEmpty()) {
            (void)QtConcurrent::run([this, irPath]() {
                bool ok = m_convolution.loadIR(irPath.toStdString());
                qDebug() << "[Convolution] IR load:" << irPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        }
    }

    connect(Settings::instance(), &Settings::convolutionChanged,
            this, [this]() {
        auto* s = Settings::instance();
        m_convolution.setEnabled(s->convolutionEnabled());
        QString irPath = s->convolutionIRPath();
        if (!irPath.isEmpty() && irPath.toStdString() != m_convolution.irFilePath()) {
            (void)QtConcurrent::run([this, irPath]() {
                bool ok = m_convolution.loadIR(irPath.toStdString());
                qDebug() << "[Convolution] IR reload:" << irPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        } else {
            updateHeadroomGain();
            emit signalPathChanged();
        }
        qDebug() << "[Convolution]" << (m_convolution.isEnabled() ? "ON" : "OFF");
    });

    // Load HRTF settings
    {
        auto* s = Settings::instance();
        m_hrtf.setEnabled(s->hrtfEnabled());
        m_hrtf.setSpeakerAngle(s->hrtfSpeakerAngle());
        QString sofaPath = s->hrtfSofaPath();
        if (!sofaPath.isEmpty()) {
            (void)QtConcurrent::run([this, sofaPath]() {
                bool ok = m_hrtf.loadSOFA(sofaPath);
                qDebug() << "[HRTF] SOFA load:" << sofaPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        }
    }

    connect(Settings::instance(), &Settings::hrtfChanged,
            this, [this]() {
        auto* s = Settings::instance();
        m_hrtf.setEnabled(s->hrtfEnabled());
        m_hrtf.setSpeakerAngle(s->hrtfSpeakerAngle());
        QString sofaPath = s->hrtfSofaPath();
        if (!sofaPath.isEmpty() && sofaPath != m_hrtf.sofaPath()) {
            (void)QtConcurrent::run([this, sofaPath]() {
                bool ok = m_hrtf.loadSOFA(sofaPath);
                qDebug() << "[HRTF] SOFA reload:" << sofaPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        } else {
            updateHeadroomGain();
            emit signalPathChanged();
        }
        qDebug() << "[HRTF]" << (m_hrtf.isEnabled() ? "ON" : "OFF")
                 << "angle:" << m_hrtf.speakerAngle();
    });

    // Initialize headroom gain from persisted settings
    updateHeadroomGain();
}

// ── load ────────────────────────────────────────────────────────────
bool AudioEngine::load(const QString& filePath)
{
    qDebug() << "=== AudioEngine::load ===" << filePath;

    // Safety checks
    if (filePath.isEmpty()) {
        qWarning() << "AudioEngine::load: empty file path";
        emit errorOccurred(QStringLiteral("No file path provided"));
        return false;
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        qWarning() << "AudioEngine::load: file does not exist:" << filePath;
        LibraryDatabase::instance()->removeTrackByPath(filePath);
        emit errorOccurred(QStringLiteral("File not found: %1").arg(filePath));
        return false;
    }
    if (!fi.isReadable()) {
        qWarning() << "AudioEngine::load: file not readable:" << filePath;
        emit errorOccurred(QStringLiteral("File not readable: %1").arg(filePath));
        return false;
    }
    if (fi.size() == 0) {
        qWarning() << "AudioEngine::load: file is empty:" << filePath;
        emit errorOccurred(QStringLiteral("File is empty: %1").arg(filePath));
        return false;
    }

    stop();

    std::lock_guard<std::mutex> lock(m_decoderMutex);

    // Ensure both decoders are cleanly closed before loading
    m_decoder->close();
    m_dsdDecoder->close();
    m_usingDSDDecoder = false;

    // Detect DSD files by extension
    QString ext = QFileInfo(filePath).suffix().toLower();
    bool isDSD = (ext == QStringLiteral("dsf") || ext == QStringLiteral("dff"));

    qDebug() << "Extension:" << ext << "isDSD:" << isDSD;

    AudioStreamFormat fmt;

    if (isDSD) {
        QString dsdMode = Settings::instance()->dsdPlaybackMode();
        qDebug() << "DSD mode:" << dsdMode;

        if (dsdMode == QStringLiteral("dop")) {
            // Native DoP mode — encode DSD into DoP frames at DSD_rate/16
            // DAC sample rate will be set to the DoP rate by CoreAudioOutput
            if (m_dsdDecoder->open(filePath.toStdString(), true)) {
                fmt = m_dsdDecoder->format();
                // Check if the output device supports the required DoP rate
                double maxRate = m_output->getMaxSampleRate(m_currentDeviceId);
                if (maxRate > 0 && fmt.sampleRate > maxRate) {
                    qDebug() << "DSD: DoP requires" << fmt.sampleRate
                             << "Hz but device max is" << maxRate
                             << "Hz — falling back to PCM conversion";
                    m_dsdDecoder->close();
                } else {
                    m_usingDSDDecoder = true;
                    qDebug() << "DSD: Using native DoP encoder,"
                             << fmt.sampleRate << "Hz," << fmt.channels << "ch,"
                             << fmt.durationSecs << "sec";
                }
            } else {
                qDebug() << "DSD: DoP encoder failed, falling back to FFmpeg PCM";
            }
        }

        if (!m_usingDSDDecoder) {
            // PCM mode (default) — use FFmpeg for clean DSD-to-PCM conversion
            if (!m_decoder->open(filePath.toStdString())) {
                qDebug() << "DSD: FFmpeg failed to open file";
                emit errorOccurred(QStringLiteral("Failed to open DSD file: %1").arg(filePath));
                return false;
            }
            fmt = m_decoder->format();
            qDebug() << "DSD: Using FFmpeg PCM conversion,"
                     << fmt.sampleRate << "Hz," << fmt.channels << "ch,"
                     << fmt.durationSecs << "sec";
        }
    } else {
        // Regular PCM file — always use FFmpeg
        if (!m_decoder->open(filePath.toStdString())) {
            qDebug() << "FFmpeg failed to open file";
            emit errorOccurred(QStringLiteral("Failed to open: %1").arg(filePath));
            return false;
        }
        fmt = m_decoder->format();
        qDebug() << "PCM:" << fmt.sampleRate << "Hz," << fmt.channels << "ch,"
                 << fmt.durationSecs << "sec";
    }

    m_sampleRate.store(fmt.sampleRate, std::memory_order_relaxed);
    m_channels.store(fmt.channels, std::memory_order_relaxed);
    m_crossfeed.setSampleRate(static_cast<int>(fmt.sampleRate));
    m_convolution.setSampleRate(static_cast<int>(fmt.sampleRate));
    m_hrtf.setSampleRate(static_cast<int>(fmt.sampleRate));
    m_duration = fmt.durationSecs;
    m_framesRendered.store(0, std::memory_order_relaxed);

    // ── Auto sample rate: find target output rate for lossless files ──
    double autoTargetRate = 0.0;
    if (m_autoSampleRate && !m_usingDSDDecoder) {
        QString codec = m_decoder->codecName();
        // Only auto-switch for lossless codecs (skip MP3/AAC/OGG/etc.)
        bool isLossy = (codec == "mp3" || codec == "aac" || codec == "vorbis"
                        || codec == "opus" || codec == "wmav2"
                        || codec == "ac3" || codec == "eac3");
        if (!codec.isEmpty() && !isLossy) {
            autoTargetRate = m_output->findNearestSupportedRate(
                fmt.sampleRate, m_currentDeviceId);
            if (autoTargetRate > 0 && std::abs(autoTargetRate - fmt.sampleRate) > 0.5) {
                qDebug() << "[Audio] Auto rate:" << fmt.sampleRate
                         << "not supported, nearest:" << autoTargetRate;
            } else if (autoTargetRate > 0) {
                qDebug() << "[Audio] Auto rate: target" << autoTargetRate << "Hz";
            }
        }
    }

    // Configure upsampler with source format (skip for DSD DoP — has its own path)
    AudioStreamFormat outputFmt = fmt;
    if (!m_usingDSDDecoder && !m_bitPerfect.load(std::memory_order_relaxed)
        && m_upsampler->isEnabled() && m_upsampler->mode() != UpsamplingMode::None) {
        bool builtIn = m_output->isBuiltInDevice(m_currentDeviceId);
        double deviceRate = m_output->getMaxSampleRate(m_currentDeviceId);
        if (builtIn) {
            // For built-in devices, use the nominal rate (e.g., 96kHz) as max
            double nominalRate = m_output->deviceNominalSampleRate();
            if (nominalRate > 0) deviceRate = nominalRate;
        }
        // Auto sample rate: tell upsampler the device is at source rate
        // so upsampling becomes a no-op (source rate == device rate)
        if (autoTargetRate > 0) deviceRate = autoTargetRate;
        m_upsampler->setDeviceIsBuiltIn(builtIn);
        m_upsampler->setMaxDacRate((int)deviceRate);
        m_upsampler->setInputFormat((int)fmt.sampleRate, fmt.channels);
        if (m_upsampler->isActive()) {
            outputFmt.sampleRate = m_upsampler->outputSampleRate();
            qDebug() << "[AudioEngine] Upsampling:" << fmt.sampleRate
                     << "->" << outputFmt.sampleRate << "Hz"
                     << "(builtIn:" << builtIn << "deviceRate:" << deviceRate << ")";
        }
    }

    // Auto sample rate: ensure output rate matches target even if upsampler is off
    if (autoTargetRate > 0 && std::abs(outputFmt.sampleRate - autoTargetRate) > 0.5) {
        qDebug() << "[Audio] Auto rate: output" << outputFmt.sampleRate
                 << "->" << autoTargetRate << "Hz";
        outputFmt.sampleRate = autoTargetRate;
    }

    // Pre-allocate decode buffer for the render callback (avoids allocation on audio thread)
    // Size for worst case: 4096 output frames worth of source data
    {
        double ratio = (outputFmt.sampleRate > fmt.sampleRate)
            ? (double)outputFmt.sampleRate / fmt.sampleRate : 1.0;
        int maxSourceFrames = (int)(4096.0 / ratio) + 64;
        m_decodeBuf.resize(maxSourceFrames * fmt.channels);
    }

    // Pre-allocate crossfade buffer (generous — covers any CoreAudio buffer size)
    m_crossfadeBuf.resize(16384 * fmt.channels);
    m_crossfading = false;
    m_crossfadeProgress = 0;

    // Prepare DSP pipeline at the output rate (post-upsampling)
    m_dspPipeline->prepare(outputFmt.sampleRate, fmt.channels);

    // Open audio output at the (potentially upsampled) rate
    m_output->close();

    m_output->setRenderCallback([this](float* buf, int frames) -> int {
        return renderAudio(buf, frames);
    });
    m_output->setVolume(m_volume);

    if (!m_output->open(outputFmt, m_currentDeviceId)) {
        // If a specific device was requested and failed, fall back to default
        if (m_currentDeviceId != 0) {
            qWarning() << "AudioEngine: Failed to open device" << m_currentDeviceId
                       << "- falling back to default output device";
            m_currentDeviceId = 0;
            if (!m_output->open(outputFmt, 0)) {
                if (m_usingDSDDecoder) m_dsdDecoder->close();
                else m_decoder->close();
                qWarning() << "AudioEngine: Failed to open default audio output";
                emit errorOccurred(QStringLiteral("Failed to open audio output"));
                return false;
            }
        } else {
            if (m_usingDSDDecoder) m_dsdDecoder->close();
            else m_decoder->close();
            qWarning() << "AudioEngine: Failed to open audio output";
            emit errorOccurred(QStringLiteral("Failed to open audio output"));
            return false;
        }
    }

    { std::lock_guard<std::mutex> lock(m_filePathMutex); m_currentFilePath = filePath; }

    updateHeadroomGain();

    qDebug() << "=== AudioEngine::load OK ===";
    emit durationChanged(m_duration);
    emit signalPathChanged();
    return true;
}

// ── play ────────────────────────────────────────────────────────────
void AudioEngine::play()
{
    qDebug() << "AudioEngine::play() state:" << m_state
             << "deviceId:" << m_currentDeviceId;

    if (m_state == Playing) {
        qDebug() << "AudioEngine::play() - already playing, ignoring";
        return;
    }
    bool hasSource = m_usingDSDDecoder ? m_dsdDecoder->isOpen() : m_decoder->isOpen();
    if (!hasSource) {
        qWarning() << "AudioEngine::play() - no source open, cannot play";
        return;
    }

    if (m_output->start()) {
        m_state = Playing;
        m_positionTimer->start();
        qDebug() << "AudioEngine::play() - started successfully";
        emit stateChanged(m_state);
        emit signalPathChanged();
    } else {
        qWarning() << "AudioEngine::play() - output->start() FAILED";
    }
}

// ── pause ───────────────────────────────────────────────────────────
void AudioEngine::pause()
{
    if (m_state != Playing) return;

    m_output->stop();
    m_positionTimer->stop();
    m_state = Paused;
    emit stateChanged(m_state);
}

// ── stop ────────────────────────────────────────────────────────────
void AudioEngine::stop()
{
    m_shuttingDown.store(true, std::memory_order_release);

    // Clear render callback first so audio thread stops calling into decoders
    if (m_output) {
        m_output->setRenderCallback(nullptr);
        m_output->stop();
        m_output->close();
    }

    if (m_positionTimer) {
        m_positionTimer->stop();
    }

    {
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        m_decoder->close();
        m_dsdDecoder->close();
        m_nextDecoder->close();
        m_nextDsdDecoder->close();
        m_nextTrackReady.store(false, std::memory_order_relaxed);
        m_nextUsingDSD = false;
        m_nextFilePath.clear();
    }
    m_usingDSDDecoder = false;
    m_framesRendered.store(0, std::memory_order_relaxed);
    m_dspPipeline->reset();
    { std::lock_guard<std::mutex> lock(m_filePathMutex); m_currentFilePath.clear(); }
    m_state = Stopped;

    m_shuttingDown.store(false, std::memory_order_release);
    emit stateChanged(m_state);
    emit signalPathChanged();
}

// ── seek ────────────────────────────────────────────────────────────
void AudioEngine::seek(double secs)
{
    std::lock_guard<std::mutex> lock(m_decoderMutex);
    bool seekOk = false;
    if (m_usingDSDDecoder && m_dsdDecoder->isOpen()) {
        seekOk = m_dsdDecoder->seek(secs);
    } else if (m_decoder->isOpen()) {
        seekOk = m_decoder->seek(secs);
    }
    if (seekOk) {
        m_framesRendered.store((int64_t)(secs * m_sampleRate), std::memory_order_relaxed);
        m_dspPipeline->reset();
        emit positionChanged(secs);
    }
}

// ── setVolume ───────────────────────────────────────────────────────
void AudioEngine::setVolume(float vol)
{
    m_volume = (vol < 0.0f) ? 0.0f : (vol > 1.0f ? 1.0f : vol);
    m_output->setVolume(m_volume);
}

// ── position ────────────────────────────────────────────────────────
double AudioEngine::position() const
{
    double sr = m_sampleRate.load(std::memory_order_relaxed);
    if (sr <= 0) return 0.0;
    return (double)m_framesRendered.load(std::memory_order_relaxed) / sr;
}

// ── availableDevices ────────────────────────────────────────────────
std::vector<AudioDevice> AudioEngine::availableDevices() const
{
    return m_output->enumerateDevices();
}

// ── setOutputDevice ─────────────────────────────────────────────────
bool AudioEngine::setOutputDevice(uint32_t deviceId)
{
    qDebug() << "AudioEngine::setOutputDevice(" << deviceId << ")";

    // Validate the device exists before storing it
    if (deviceId != 0) {
        auto devices = m_output->enumerateDevices();
        bool found = false;
        for (const auto& dev : devices) {
            if (dev.deviceId == deviceId) {
                found = true;
                break;
            }
        }
        if (!found) {
            qWarning() << "AudioEngine::setOutputDevice - device" << deviceId
                       << "not found, using default";
            deviceId = 0;
        }
    }

    // Release hog mode on old device before switching
    bool hadHog = m_output->isExclusiveMode();
    if (hadHog) {
        m_output->releaseHogMode();
    }

    m_currentDeviceId = deviceId;
    bool ok = true;
    if (m_output->isRunning()) {
        ok = m_output->setDevice(deviceId);
    }

    // Re-acquire hog mode on new device if exclusive mode is enabled
    if (Settings::instance()->exclusiveMode()) {
        m_output->setHogMode(true);
        emit signalPathChanged();
    }

    // Update max DAC rate for upsampler
    m_upsampler->setMaxDacRate((int)m_output->getMaxSampleRate(m_currentDeviceId));

    return ok;
}

// ── setBufferSize ────────────────────────────────────────────────────
bool AudioEngine::setBufferSize(uint32_t frames)
{
    qDebug() << "AudioEngine::setBufferSize(" << frames << ")";
    if (m_output) {
        return m_output->setBufferSize(frames);
    }
    return false;
}

// ── setSampleRate ────────────────────────────────────────────────────
void AudioEngine::setSampleRate(double newRate)
{
    qDebug() << "AudioEngine::setSampleRate(" << newRate << ")";
    if (!m_output) return;

    if (m_state == Playing || m_state == Paused) {
        // Save position, stop, reconfigure, restart, seek back
        double pos = position();
        bool wasPlaying = (m_state == Playing);

        m_positionTimer->stop();
        m_output->stop();
        m_output->setSampleRate(newRate);
        m_sampleRate.store(newRate, std::memory_order_relaxed);
        m_dspPipeline->prepare(newRate, m_channels.load(std::memory_order_relaxed));

        if (wasPlaying) {
            m_output->start();
            m_positionTimer->start();
            m_state = Playing;
        } else {
            m_state = Paused;
        }

        // Restore position
        m_framesRendered.store(static_cast<int64_t>(pos * newRate), std::memory_order_relaxed);
        qDebug() << "AudioEngine::setSampleRate: resumed at" << pos << "sec";
    } else {
        // Not playing — just reconfigure for next playback
        m_output->setSampleRate(newRate);
        m_sampleRate.store(newRate, std::memory_order_relaxed);
    }
}

// ── setBitPerfectMode ────────────────────────────────────────────────
void AudioEngine::setBitPerfectMode(bool enabled)
{
    m_bitPerfect.store(enabled, std::memory_order_relaxed);
    m_output->setBitPerfectMode(enabled);
    Settings::instance()->setBitPerfectMode(enabled);
    qDebug() << "Bit-perfect mode:" << (enabled ? "ON" : "OFF");
}

// ── setAutoSampleRate ───────────────────────────────────────────────
void AudioEngine::setAutoSampleRate(bool enabled)
{
    m_autoSampleRate = enabled;
    Settings::instance()->setAutoSampleRate(enabled);
    qDebug() << "Auto sample rate:" << (enabled ? "ON" : "OFF");

    // Apply immediately if currently playing — switch device to file's native rate
    if (enabled && (m_state == Playing || m_state == Paused)) {
        double fileRate = 0.0;
        {
            std::lock_guard<std::mutex> lock(m_decoderMutex);
            if (m_usingDSDDecoder)
                fileRate = m_dsdDecoder->format().sampleRate;
            else if (m_decoder->isOpen())
                fileRate = m_decoder->format().sampleRate;
        }
        double currentRate = m_sampleRate.load(std::memory_order_relaxed);
        if (fileRate > 0 && std::abs(fileRate - currentRate) > 0.5) {
            qDebug() << "Auto sample rate: switching" << currentRate << "->" << fileRate;
            setSampleRate(fileRate);
        }
    }
}

// ── setExclusiveMode ─────────────────────────────────────────────────
void AudioEngine::setExclusiveMode(bool enabled)
{
    Settings::instance()->setExclusiveMode(enabled);
    bool success = m_output->setHogMode(enabled);
    if (success) {
        qDebug() << "Exclusive mode:" << (enabled ? "ON" : "OFF");
    } else {
        qWarning() << "Exclusive mode: failed to" << (enabled ? "acquire" : "release");
    }
    emit signalPathChanged();
}

bool AudioEngine::exclusiveMode() const
{
    return m_output->isExclusiveMode();
}

// ── maxDeviceSampleRate ─────────────────────────────────────────────
double AudioEngine::maxDeviceSampleRate() const
{
    return m_output->getMaxSampleRate(m_currentDeviceId);
}

// ── upsampler ──────────────────────────────────────────────────────
UpsamplerProcessor* AudioEngine::upsampler() const
{
    return m_upsampler.get();
}

// ── applyUpsamplingChange ────────────────────────────────────────────
void AudioEngine::applyUpsamplingChange()
{
    // Only meaningful if we have an active source
    QString path;
    {
        std::lock_guard<std::mutex> lock(m_filePathMutex);
        if (m_state == Stopped || m_currentFilePath.isEmpty()) {
            emit signalPathChanged();
            return;
        }
        path = m_currentFilePath;
    }

    // Re-load the current track to apply the new upsampling config
    // Save current position and state, then reload and seek back
    double pos = position();
    bool wasPlaying = (m_state == Playing);

    qDebug() << "[AudioEngine] applyUpsamplingChange: reloading at position" << pos;

    if (load(path)) {
        seek(pos);
        if (wasPlaying) {
            play();
        }
    }
}

// ── setCurrentTrack (volume leveling) ────────────────────────────────
void AudioEngine::setCurrentTrack(const Track& track)
{
    m_currentTrack = track;
    updateLevelingGain();

    // Background R128 analysis if no gain data and leveling is enabled
    if (Settings::instance()->volumeLeveling()
        && !m_currentTrack.hasReplayGain
        && !m_currentTrack.hasR128
        && !m_currentTrack.filePath.isEmpty()) {

        QString path = m_currentTrack.filePath;
        QString title = m_currentTrack.title;
        (void)QtConcurrent::run([this, path, title]() {
            LoudnessResult r = LoudnessAnalyzer::analyze(path);
            if (r.valid) {
                QMetaObject::invokeMethod(this, [this, r, path]() {
                    // Only update if still the same track
                    if (m_currentTrack.filePath == path) {
                        m_currentTrack.r128Loudness = r.integratedLoudness;
                        m_currentTrack.r128Peak = r.truePeak;
                        m_currentTrack.hasR128 = true;
                        updateLevelingGain();
                    }
                    // Cache in DB
                    LibraryDatabase::instance()->updateR128Loudness(
                        path, r.integratedLoudness, r.truePeak);
                }, Qt::QueuedConnection);
            }
        });
    }
}

// ── updateHeadroomGain ──────────────────────────────────────────────
void AudioEngine::updateHeadroomGain()
{
    auto mode = Settings::instance()->headroomMode();
    double dB = 0.0;

    switch (mode) {
    case Settings::HeadroomMode::Off:
        dB = 0.0;
        break;
    case Settings::HeadroomMode::Auto: {
        bool anyDspActive = Settings::instance()->volumeLeveling()
                         || Settings::instance()->crossfeedEnabled()
                         || (Settings::instance()->convolutionEnabled() && m_convolution.hasIR());
        dB = anyDspActive ? -3.0 : 0.0;
        break;
    }
    case Settings::HeadroomMode::Manual:
        dB = Settings::instance()->manualHeadroom();
        break;
    }

    dB = qBound(-12.0, dB, 0.0);
    float linear = static_cast<float>(std::pow(10.0, dB / 20.0));
    m_headroomGain.store(linear, std::memory_order_relaxed);

    qDebug() << "[Headroom] Mode:" << static_cast<int>(mode)
             << "gain:" << dB << "dB linear:" << linear;
}

// ── updateLevelingGain ──────────────────────────────────────────────
void AudioEngine::updateLevelingGain()
{
    if (!Settings::instance()->volumeLeveling() || m_currentTrack.filePath.isEmpty()) {
        m_levelingGain.store(1.0f, std::memory_order_relaxed);
        emit signalPathChanged();
        return;
    }

    double targetLUFS = Settings::instance()->targetLoudness();
    int mode = Settings::instance()->levelingMode();
    double gainDB = 0.0;

    if (m_currentTrack.hasReplayGain) {
        // ReplayGain: value is already a gain adjustment
        // RG reference = -18 LUFS, adjust to our target
        double rgGain = (mode == 1 && m_currentTrack.replayGainAlbum != 0.0)
            ? m_currentTrack.replayGainAlbum
            : m_currentTrack.replayGainTrack;
        double rgRef = -18.0;
        gainDB = rgGain + (targetLUFS - rgRef);

        // Peak limiting
        double peak = (mode == 1 && m_currentTrack.replayGainAlbumPeak != 1.0)
            ? m_currentTrack.replayGainAlbumPeak
            : m_currentTrack.replayGainTrackPeak;
        double linearGain = std::pow(10.0, gainDB / 20.0);
        if (peak > 0.0 && peak * linearGain > 1.0) {
            linearGain = 1.0 / peak;
            gainDB = 20.0 * std::log10(linearGain);
        }
    } else if (m_currentTrack.hasR128 && m_currentTrack.r128Loudness != 0.0) {
        gainDB = targetLUFS - m_currentTrack.r128Loudness;
    } else {
        // No gain data available
        m_levelingGain.store(1.0f, std::memory_order_relaxed);
        emit signalPathChanged();
        return;
    }

    // Clamp to safe range
    gainDB = std::max(-12.0, std::min(12.0, gainDB));
    float linear = static_cast<float>(std::pow(10.0, gainDB / 20.0));
    m_levelingGain.store(linear, std::memory_order_relaxed);

    qDebug() << "[Volume Leveling]" << m_currentTrack.title
             << "gain:" << gainDB << "dB linear:" << linear
             << (m_currentTrack.hasReplayGain ? "ReplayGain" :
                 m_currentTrack.hasR128 ? "R128" : "None");

    emit signalPathChanged();
}

// ── levelingGainDb ──────────────────────────────────────────────────
float AudioEngine::levelingGainDb() const
{
    float linear = m_levelingGain.load(std::memory_order_relaxed);
    if (linear <= 0.0f || linear == 1.0f) return 0.0f;
    return 20.0f * std::log10(linear);
}

// ── prepareNextTrack (gapless pre-decode) ────────────────────────────
void AudioEngine::prepareNextTrack(const QString& filePath)
{
    if (filePath.isEmpty()) return;

    // Don't prepare if both gapless and crossfade are disabled
    if (!Settings::instance()->gaplessPlayback()
        && m_crossfadeDurationMs.load(std::memory_order_relaxed) <= 0) return;

    qDebug() << "[Gapless] Preparing next track:" << filePath;

    std::lock_guard<std::mutex> lock(m_decoderMutex);

    // Close any previously prepared next track
    m_nextDecoder->close();
    m_nextDsdDecoder->close();
    m_nextTrackReady.store(false, std::memory_order_relaxed);
    m_nextUsingDSD = false;
    m_nextFilePath.clear();

    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isReadable() || fi.size() == 0) {
        qDebug() << "[Gapless] Next track file invalid:" << filePath;
        return;
    }

    QString ext = fi.suffix().toLower();
    bool isDSD = (ext == QStringLiteral("dsf") || ext == QStringLiteral("dff"));

    if (isDSD) {
        QString dsdMode = Settings::instance()->dsdPlaybackMode();
        if (dsdMode == QStringLiteral("dop")) {
            if (m_nextDsdDecoder->open(filePath.toStdString(), true)) {
                AudioStreamFormat fmt = m_nextDsdDecoder->format();
                double maxRate = m_output->getMaxSampleRate(m_currentDeviceId);
                if (maxRate > 0 && fmt.sampleRate > maxRate) {
                    m_nextDsdDecoder->close();
                } else {
                    m_nextUsingDSD = true;
                    m_nextFormat = fmt;
                }
            }
        }

        if (!m_nextUsingDSD) {
            if (m_nextDecoder->open(filePath.toStdString())) {
                m_nextFormat = m_nextDecoder->format();
            } else {
                qDebug() << "[Gapless] Failed to open next track:" << filePath;
                return;
            }
        }
    } else {
        if (!m_nextDecoder->open(filePath.toStdString())) {
            qDebug() << "[Gapless] Failed to open next track:" << filePath;
            return;
        }
        m_nextFormat = m_nextDecoder->format();
    }

    m_nextFilePath = filePath;

    // Check format compatibility: sample rate and channels must match for seamless transition
    double currentRate = m_sampleRate.load(std::memory_order_relaxed);
    int currentCh = m_channels.load(std::memory_order_relaxed);
    bool formatMatch = (std::abs(m_nextFormat.sampleRate - currentRate) < 1.0)
                       && (m_nextFormat.channels == currentCh)
                       && (m_nextUsingDSD == m_usingDSDDecoder);

    if (!formatMatch) {
        qDebug() << "[Gapless] Format mismatch — will use normal transition"
                 << "current:" << currentRate << "Hz" << currentCh << "ch DSD:" << m_usingDSDDecoder
                 << "next:" << m_nextFormat.sampleRate << "Hz" << m_nextFormat.channels << "ch DSD:" << m_nextUsingDSD;
        // Keep the decoder open — we'll reuse it in load() to avoid double-open
        m_nextTrackReady.store(false, std::memory_order_relaxed);
        return;
    }

    m_nextTrackReady.store(true, std::memory_order_release);
    qDebug() << "[Gapless] Next track ready:" << filePath;
}

// ── cancelNextTrack ──────────────────────────────────────────────────
void AudioEngine::cancelNextTrack()
{
    std::lock_guard<std::mutex> lock(m_decoderMutex);
    m_nextTrackReady.store(false, std::memory_order_relaxed);
    m_nextDecoder->close();
    m_nextDsdDecoder->close();
    m_nextUsingDSD = false;
    m_nextFilePath.clear();
    qDebug() << "[Gapless] Next track cancelled";
}

void AudioEngine::setCrossfadeDuration(int ms)
{
    m_crossfadeDurationMs.store(ms, std::memory_order_relaxed);
    qDebug() << "[Crossfade] Duration set to" << ms << "ms";
}

// ── renderAudio (called from audio thread) ──────────────────────────
int AudioEngine::renderAudio(float* buf, int maxFrames)
{
    // Check all safety flags first
    int channels = m_channels.load(std::memory_order_relaxed);

    if (m_destroyed.load(std::memory_order_acquire) ||
        m_shuttingDown.load(std::memory_order_acquire)) {
        std::memset(buf, 0, maxFrames * channels * sizeof(float));
        return 0;
    }

    m_renderingInProgress.store(true, std::memory_order_release);

    // try_lock — never block the realtime audio thread
    std::unique_lock<std::mutex> lock(m_decoderMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // Main thread holds mutex (load/seek/stop) — output silence this cycle
        std::memset(buf, 0, maxFrames * channels * sizeof(float));
        m_renderingInProgress.store(false, std::memory_order_release);
        return 0;
    }

    bool upsamplerActive = m_upsampler && m_upsampler->isActive()
                           && !m_bitPerfect.load(std::memory_order_relaxed)
                           && !m_usingDSDDecoder;

    int framesRead = 0;
    bool crossfadeHandledFrames = false;  // true when crossfade manages m_framesRendered

    if (upsamplerActive) {
        // Calculate how many source frames to decode for the requested output frames.
        // CoreAudio requests maxFrames at the OUTPUT rate; we decode fewer at SOURCE rate.
        double ratio = (double)m_upsampler->outputSampleRate() / (double)m_upsampler->inputSampleRate();
        int sourceFrames = (int)std::ceil((double)maxFrames / ratio);

        // Use pre-allocated decode buffer (sized in load(), no allocation here)
        int bufSamples = sourceFrames * channels;
        if ((int)m_decodeBuf.size() < bufSamples) {
            // Buffer too small — cap frames to avoid RT allocation
            sourceFrames = (int)m_decodeBuf.size() / channels;
            bufSamples = sourceFrames * channels;
            if (sourceFrames <= 0) {
                std::memset(buf, 0, maxFrames * channels * sizeof(float));
                m_renderingInProgress.store(false, std::memory_order_release);
                return 0;
            }
        }

        // 1. Decode source frames into separate buffer
        if (m_decoder->isOpen()) {
            framesRead = m_decoder->read(m_decodeBuf.data(), sourceFrames);
        }

        if (framesRead > 0) {
            // 2. Upsample: framesRead source frames -> output frames written directly to buf
            size_t generated = m_upsampler->processUpsampling(
                m_decodeBuf.data(), framesRead, channels,
                buf, maxFrames
            );

            int outputFrames = (int)generated;

            // 3. Zero-pad if fewer frames generated than requested
            if (outputFrames < maxFrames) {
                std::memset(buf + outputFrames * channels, 0,
                            (maxFrames - outputFrames) * channels * sizeof(float));
            }

            // 3b. Apply headroom (before DSP)
            {
                float hr = m_headroomGain.load(std::memory_order_relaxed);
                if (hr != 1.0f) {
                    int n = outputFrames * channels;
                    for (int i = 0; i < n; ++i) buf[i] *= hr;
                }
            }

            // 3c. Apply crossfeed (stereo only, after headroom)
            //     Mutually exclusive with HRTF — if both enabled, skip crossfeed (HRTF wins)
            if (channels == 2 && !(m_hrtf.isEnabled() && m_crossfeed.isEnabled())) {
                m_crossfeed.process(buf, outputFrames);
            }

            // 3d. Apply convolution (room correction / IR)
            m_convolution.process(buf, outputFrames, channels);

            // 3e. Apply HRTF (binaural spatial audio)
            if (channels == 2) {
                m_hrtf.process(buf, outputFrames);
            }

            // 4. Apply DSP pipeline at upsampled rate
            m_dspPipeline->process(buf, outputFrames, channels);

            // 4b. Apply volume leveling gain
            {
                float lg = m_levelingGain.load(std::memory_order_relaxed);
                if (lg != 1.0f) {
                    int n = outputFrames * channels;
                    for (int i = 0; i < n; ++i) buf[i] *= lg;
                }
            }

            // 4c. Peak limiter (safety net after all DSP)
            {
                int n = outputFrames * channels;
                for (int i = 0; i < n; ++i) {
                    float s = buf[i];
                    if (s > 0.95f) buf[i] = 0.95f + 0.05f * std::tanh((s - 0.95f) / 0.05f);
                    else if (s < -0.95f) buf[i] = -0.95f - 0.05f * std::tanh((-s - 0.95f) / 0.05f);
                }
            }

            // 5. Track position in SOURCE frames (not output frames)
            m_framesRendered.fetch_add(framesRead, std::memory_order_relaxed);
            m_renderingInProgress.store(false, std::memory_order_release);
            return outputFrames;
        }
    } else {
        // Normal path — no upsampling
        if (m_usingDSDDecoder && m_dsdDecoder->isOpen()) {
            framesRead = m_dsdDecoder->read(buf, maxFrames);
        } else if (m_decoder->isOpen()) {
            framesRead = m_decoder->read(buf, maxFrames);
        } else {
            m_renderingInProgress.store(false, std::memory_order_release);
            return 0;
        }

        // ── Crossfade mixing (before DSP chain) ─────────────────────────
        // Only for PCM tracks with matching sample rate/channels
        if (!m_usingDSDDecoder) {
            int cfMs = m_crossfadeDurationMs.load(std::memory_order_relaxed);
            double sr = m_sampleRate.load(std::memory_order_relaxed);

            // Start crossfade if approaching end of track
            if (!m_crossfading && cfMs > 0 && framesRead > 0
                && m_nextTrackReady.load(std::memory_order_acquire)
                && !m_nextUsingDSD
                && m_nextFormat.sampleRate == sr
                && m_nextFormat.channels == channels) {
                int64_t totalFrames = (int64_t)(m_duration * sr);
                int64_t cfFrames = (int64_t)(cfMs * sr / 1000.0);
                int64_t pos = m_framesRendered.load(std::memory_order_relaxed);
                if (totalFrames > cfFrames && pos >= totalFrames - cfFrames) {
                    m_crossfading = true;
                    m_crossfadeProgress = (int)(pos - (totalFrames - cfFrames));
                    m_crossfadeTotalFrames = (int)cfFrames;
                }
            }

            // Mix incoming track during crossfade
            if (m_crossfading) {
                crossfadeHandledFrames = true;
                if (framesRead == 0) {
                    // Outgoing track ended mid-crossfade — read only incoming
                    framesRead = m_nextDecoder->isOpen()
                        ? m_nextDecoder->read(buf, maxFrames) : 0;
                    // Apply fade-in gain to incoming-only frames
                    for (int f = 0; f < framesRead; f++) {
                        float t = (float)(m_crossfadeProgress + f) / (float)m_crossfadeTotalFrames;
                        t = std::min(t, 1.0f);
                        float gainIn = std::sin(t * (float)M_PI_2);
                        for (int ch = 0; ch < channels; ch++)
                            buf[f * channels + ch] *= gainIn;
                    }
                } else {
                    // Both tracks active — read incoming and mix
                    // Cap to available buffer capacity (avoid RT allocation)
                    int maxNextFrames = std::min(framesRead,
                        (int)m_crossfadeBuf.size() / std::max(channels, 1));
                    int nextRead = (m_nextDecoder->isOpen() && maxNextFrames > 0)
                        ? m_nextDecoder->read(m_crossfadeBuf.data(), maxNextFrames) : 0;
                    for (int f = 0; f < framesRead; f++) {
                        float t = (float)(m_crossfadeProgress + f) / (float)m_crossfadeTotalFrames;
                        t = std::min(t, 1.0f);
                        float gainOut = std::cos(t * (float)M_PI_2);
                        float gainIn  = std::sin(t * (float)M_PI_2);
                        for (int ch = 0; ch < channels; ch++) {
                            int idx = f * channels + ch;
                            float incoming = (f < nextRead) ? m_crossfadeBuf[idx] : 0.0f;
                            buf[idx] = buf[idx] * gainOut + incoming * gainIn;
                        }
                    }
                    // Track outgoing position (these frames were from the outgoing track)
                    m_framesRendered.fetch_add(framesRead, std::memory_order_relaxed);
                }
                m_crossfadeProgress += framesRead;

                // Crossfade complete — swap to incoming track
                if (m_crossfadeProgress >= m_crossfadeTotalFrames) {
                    m_decoder.swap(m_nextDecoder);
                    m_dsdDecoder.swap(m_nextDsdDecoder);
                    m_usingDSDDecoder = m_nextUsingDSD;
                    { std::lock_guard<std::mutex> lock(m_filePathMutex); m_currentFilePath = m_nextFilePath; }
                    m_duration = m_nextFormat.durationSecs;
                    m_sampleRate.store(m_nextFormat.sampleRate, std::memory_order_relaxed);
                    m_channels.store(m_nextFormat.channels, std::memory_order_relaxed);
                    m_framesRendered.store(m_crossfadeProgress, std::memory_order_relaxed);

                    m_nextDecoder->close();
                    m_nextDsdDecoder->close();
                    m_nextTrackReady.store(false, std::memory_order_relaxed);
                    m_nextUsingDSD = false;
                    m_nextFilePath.clear();
                    m_crossfading = false;
                    m_crossfadeProgress = 0;

                    // Signal main thread via atomic flag (RT-safe)
                    m_rtGaplessFlag.store(true, std::memory_order_release);
                }
            }
        }

        // DoP passthrough — DSD data must be bit-perfect, skip ALL DSP
        const bool dopPassthrough = m_usingDSDDecoder && m_dsdDecoder->isDoPMode();

        // Apply headroom (before DSP)
        if (framesRead > 0 && !dopPassthrough) {
            float hr = m_headroomGain.load(std::memory_order_relaxed);
            if (hr != 1.0f) {
                int n = framesRead * channels;
                for (int i = 0; i < n; ++i) buf[i] *= hr;
            }
        }

        // Apply crossfeed (stereo only, after headroom)
        //   Mutually exclusive with HRTF — if both enabled, skip crossfeed (HRTF wins)
        if (framesRead > 0 && !dopPassthrough && channels == 2
            && !(m_hrtf.isEnabled() && m_crossfeed.isEnabled())) {
            m_crossfeed.process(buf, framesRead);
        }

        // Apply convolution (room correction / IR)
        if (framesRead > 0 && !dopPassthrough) {
            m_convolution.process(buf, framesRead, channels);
        }

        // Apply HRTF (binaural spatial audio)
        if (framesRead > 0 && !dopPassthrough && channels == 2) {
            m_hrtf.process(buf, framesRead);
        }

        // Apply DSP pipeline (gain, EQ, plugins) — skip in bit-perfect mode or DoP
        if (framesRead > 0 && !dopPassthrough && !m_bitPerfect.load(std::memory_order_relaxed)) {
            (void)m_renderDiagOnce.exchange(true, std::memory_order_relaxed);
            m_dspPipeline->process(buf, framesRead, channels);
        }

        // Apply volume leveling gain
        if (framesRead > 0 && !dopPassthrough) {
            float lg = m_levelingGain.load(std::memory_order_relaxed);
            if (lg != 1.0f) {
                int n = framesRead * channels;
                for (int i = 0; i < n; ++i) buf[i] *= lg;
            }
        }

        // Peak limiter (safety net after all DSP)
        if (framesRead > 0 && !dopPassthrough) {
            int n = framesRead * channels;
            for (int i = 0; i < n; ++i) {
                float s = buf[i];
                if (s > 0.95f) buf[i] = 0.95f + 0.05f * std::tanh((s - 0.95f) / 0.05f);
                else if (s < -0.95f) buf[i] = -0.95f - 0.05f * std::tanh((-s - 0.95f) / 0.05f);
            }
        }
    }

    if (!crossfadeHandledFrames)
        m_framesRendered.fetch_add(framesRead, std::memory_order_relaxed);

    if (framesRead == 0 && !m_crossfading) {
        // Current track ended — check if we can do a gapless transition
        if (m_nextTrackReady.load(std::memory_order_acquire)) {
            // Swap next decoder into current
            m_decoder.swap(m_nextDecoder);
            m_dsdDecoder.swap(m_nextDsdDecoder);
            m_usingDSDDecoder = m_nextUsingDSD;
            { std::lock_guard<std::mutex> lock(m_filePathMutex); m_currentFilePath = m_nextFilePath; }
            m_duration = m_nextFormat.durationSecs;
            m_sampleRate.store(m_nextFormat.sampleRate, std::memory_order_relaxed);
            m_channels.store(m_nextFormat.channels, std::memory_order_relaxed);
            m_framesRendered.store(0, std::memory_order_relaxed);

            // Clean up the old decoder (now in the "next" slot)
            m_nextDecoder->close();
            m_nextDsdDecoder->close();
            m_nextTrackReady.store(false, std::memory_order_relaxed);
            m_nextUsingDSD = false;
            m_nextFilePath.clear();

            // Read from the new decoder to fill the buffer for this callback
            int newFrames = 0;
            if (upsamplerActive && m_decoder->isOpen() && !m_usingDSDDecoder) {
                // Upsampler path: decode to temp buffer, then upsample to output
                double ratio = (double)m_upsampler->outputSampleRate() / (double)m_upsampler->inputSampleRate();
                int sourceFrames = (int)std::ceil((double)maxFrames / ratio);
                int bufSamples = sourceFrames * channels;
                if ((int)m_decodeBuf.size() < bufSamples) {
                    // Cap frames to available buffer — avoid RT allocation
                    sourceFrames = (int)m_decodeBuf.size() / channels;
                    bufSamples = sourceFrames * channels;
                }
                int srcRead = (sourceFrames > 0)
                    ? m_decoder->read(m_decodeBuf.data(), sourceFrames) : 0;
                if (srcRead > 0) {
                    size_t generated = m_upsampler->processUpsampling(
                        m_decodeBuf.data(), srcRead, channels, buf, maxFrames);
                    newFrames = (int)generated;
                    m_dspPipeline->process(buf, newFrames, channels);
                    m_framesRendered.fetch_add(srcRead, std::memory_order_relaxed);
                }
            } else {
                if (m_usingDSDDecoder && m_dsdDecoder->isOpen()) {
                    newFrames = m_dsdDecoder->read(buf, maxFrames);
                } else if (m_decoder->isOpen()) {
                    newFrames = m_decoder->read(buf, maxFrames);
                }

                if (newFrames > 0 && !m_bitPerfect.load(std::memory_order_relaxed)) {
                    m_dspPipeline->process(buf, newFrames, channels);
                }
                m_framesRendered.fetch_add(newFrames, std::memory_order_relaxed);
            }

            // Signal main thread via atomic flag (RT-safe)
            m_rtGaplessFlag.store(true, std::memory_order_release);

            m_renderingInProgress.store(false, std::memory_order_release);
            return newFrames;
        }

        // Signal main thread via atomic flag (RT-safe)
        m_rtPlaybackEndFlag.store(true, std::memory_order_release);
    }

    m_renderingInProgress.store(false, std::memory_order_release);
    return framesRead;
}

// ── onPositionTimer (called on main thread every 50ms) ─────────────
void AudioEngine::onPositionTimer()
{
    // Poll RT-safe flags set by the audio thread
    if (m_rtGaplessFlag.exchange(false, std::memory_order_acquire)) {
        emit durationChanged(m_duration);
        emit gaplessTransitionOccurred();
    }
    if (m_rtPlaybackEndFlag.exchange(false, std::memory_order_acquire)) {
        m_output->stop();
        m_positionTimer->stop();
        m_state = Stopped;
        emit playbackFinished();
        return;  // timer is stopped, no need to emit position
    }

    double pos = position();
    emit positionChanged(pos);
}

// ── getSignalPath ──────────────────────────────────────────────────

static QString channelDescription(int ch) {
    switch (ch) {
    case 1:  return QStringLiteral("Mono");
    case 2:  return QStringLiteral("Stereo");
    case 3:  return QStringLiteral("3.0");
    case 4:  return QStringLiteral("4.0");
    case 6:  return QStringLiteral("5.1");
    case 8:  return QStringLiteral("7.1");
    default: return QStringLiteral("%1ch").arg(ch);
    }
}

SignalPathInfo AudioEngine::getSignalPath() const
{
    SignalPathInfo info;

    {
        std::lock_guard<std::mutex> lock(m_filePathMutex);
        if (m_state == Stopped && m_currentFilePath.isEmpty()) {
            return info;
        }
    }

    double sr = m_sampleRate.load(std::memory_order_relaxed);
    int ch = m_channels.load(std::memory_order_relaxed);

    // ── 1. Source node ──────────────────────────────────────────────
    SignalPathNode sourceNode;
    sourceNode.label = QStringLiteral("Source");

    if (m_usingDSDDecoder) {
        // DSD source
        QString dsdRate;
        if (m_dsdDecoder->isDSD64())       dsdRate = QStringLiteral("DSD64");
        else if (m_dsdDecoder->isDSD128()) dsdRate = QStringLiteral("DSD128");
        else if (m_dsdDecoder->isDSD256()) dsdRate = QStringLiteral("DSD256");
        else if (m_dsdDecoder->isDSD512()) dsdRate = QStringLiteral("DSD512");
        else dsdRate = QStringLiteral("DSD");

        sourceNode.detail = QStringLiteral("%1 • %2").arg(dsdRate, channelDescription(ch));
        sourceNode.sublabel = QStringLiteral("%.1f MHz").arg(m_dsdDecoder->dsdSampleRate() / 1000000.0);
        sourceNode.quality = SignalPathNode::HighRes;
    } else if (m_decoder->isOpen()) {
        QString codec = m_decoder->codecName().toUpper();
        AudioStreamFormat fmt = m_decoder->format();

        // Detect DSD codecs decoded via FFmpeg (PCM conversion mode)
        bool isDSDCodec = codec.startsWith(QStringLiteral("DSD_"));

        if (isDSDCodec) {
            // DSD file decoded to PCM via FFmpeg — show native DSD info
            // Determine DSD type from the PCM output rate (176.4kHz = DSD64)
            int dsdMultiplier = 64;
            double dsdNativeRate = 2822400.0;
            if (fmt.sampleRate >= 352800) {
                dsdMultiplier = 128; dsdNativeRate = 5644800.0;
            }

            QString dsdLabel = QStringLiteral("DSD%1").arg(dsdMultiplier);
            sourceNode.detail = QStringLiteral("%1 • %2").arg(dsdLabel, channelDescription(fmt.channels));
            sourceNode.sublabel = QStringLiteral("%1 MHz").arg(dsdNativeRate / 1000000.0, 0, 'f', 1);
            sourceNode.quality = SignalPathNode::HighRes;
        } else {
            // Regular PCM codec
            bool lossless = (codec == QStringLiteral("FLAC") || codec == QStringLiteral("ALAC")
                          || codec == QStringLiteral("WAV")  || codec == QStringLiteral("PCM_S16LE")
                          || codec == QStringLiteral("PCM_S24LE") || codec == QStringLiteral("PCM_S32LE")
                          || codec.startsWith(QStringLiteral("PCM_")));

            if (lossless && (fmt.sampleRate > 44100 || fmt.bitsPerSample > 16)) {
                sourceNode.quality = SignalPathNode::HighRes;
            } else if (lossless) {
                sourceNode.quality = SignalPathNode::Lossless;
            } else {
                sourceNode.quality = SignalPathNode::Lossy;
            }

            // Format codec name for display
            QString displayCodec = codec;
            if (codec.startsWith(QStringLiteral("PCM_"))) displayCodec = QStringLiteral("PCM/WAV");

            sourceNode.detail = QStringLiteral("%1 • %2-bit / %3 kHz • %4")
                .arg(displayCodec)
                .arg(fmt.bitsPerSample)
                .arg(fmt.sampleRate / 1000.0, 0, 'g', 4)
                .arg(channelDescription(fmt.channels));
        }
    }
    info.nodes.append(sourceNode);

    // ── 2. Decoder node ─────────────────────────────────────────────
    SignalPathNode decoderNode;
    decoderNode.label = QStringLiteral("Decoder");

    if (m_usingDSDDecoder && m_dsdDecoder->isDoPMode()) {
        decoderNode.detail = QStringLiteral("DoP Passthrough");
        decoderNode.sublabel = QStringLiteral("DSD over PCM at %1 kHz").arg(sr / 1000.0, 0, 'g', 4);
        decoderNode.quality = SignalPathNode::HighRes;
    } else if (m_usingDSDDecoder) {
        decoderNode.detail = QStringLiteral("DSD to PCM");
        decoderNode.quality = SignalPathNode::Lossless;
    } else {
        bool lossless = false;
        bool isDSDCodec = false;
        if (m_decoder->isOpen()) {
            QString codec = m_decoder->codecName().toUpper();
            isDSDCodec = codec.startsWith(QStringLiteral("DSD_"));
            lossless = isDSDCodec
                    || codec == QStringLiteral("FLAC") || codec == QStringLiteral("ALAC")
                    || codec == QStringLiteral("WAV") || codec.startsWith(QStringLiteral("PCM_"));
        }
        if (isDSDCodec) {
            AudioStreamFormat fmt = m_decoder->format();
            decoderNode.detail = QStringLiteral("DSD to PCM Conversion");
            decoderNode.sublabel = QStringLiteral("Output at %1 kHz")
                .arg(fmt.sampleRate / 1000.0, 0, 'f', 1);
            decoderNode.quality = SignalPathNode::Enhanced;
        } else {
            decoderNode.detail = lossless ? QStringLiteral("Lossless Decode")
                                           : QStringLiteral("Lossy Decode");
            decoderNode.quality = lossless ? SignalPathNode::Lossless : SignalPathNode::Lossy;
        }
    }
    info.nodes.append(decoderNode);

    // ── 3. Upsampler node ───────────────────────────────────────────
    if (m_upsampler && m_upsampler->isActive()
        && !m_bitPerfect.load(std::memory_order_relaxed)
        && !m_usingDSDDecoder) {
        SignalPathNode upsampleNode;
        upsampleNode.label = QStringLiteral("Upsampling");
        upsampleNode.detail = QStringLiteral("SoX Resampler (libsoxr)");
        upsampleNode.sublabel = m_upsampler->getDescription();
        upsampleNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(upsampleNode);
    }

    // ── 3b. Headroom node ────────────────────────────────────────────
    {
        float hr = m_headroomGain.load(std::memory_order_relaxed);
        auto hrMode = Settings::instance()->headroomMode();
        if (hrMode != Settings::HeadroomMode::Off && hr != 1.0f) {
            SignalPathNode hrNode;
            hrNode.label = QStringLiteral("Headroom");
            double db = 20.0 * std::log10(static_cast<double>(hr));
            QString modeStr = (hrMode == Settings::HeadroomMode::Auto)
                ? QStringLiteral("Auto") : QStringLiteral("Manual");
            hrNode.sublabel = modeStr + QStringLiteral(" · ")
                + QString::number(db, 'f', 1) + QStringLiteral(" dB");
            hrNode.quality = SignalPathNode::Enhanced;
            info.nodes.append(hrNode);
        }
    }

    // ── 3c. Crossfeed node ────────────────────────────────────────────
    if (m_crossfeed.isEnabled() && ch == 2) {
        SignalPathNode cfNode;
        cfNode.label = QStringLiteral("Crossfeed");
        const char* levels[] = {"Light", "Medium", "Strong"};
        int lvl = static_cast<int>(m_crossfeed.level());
        cfNode.sublabel = QStringLiteral("Headphone · %1").arg(QLatin1String(levels[lvl]));
        cfNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(cfNode);
    }

    // ── 3d. Convolution node ────────────────────────────────────────────
    if (m_convolution.isEnabled() && m_convolution.hasIR()) {
        SignalPathNode convNode;
        convNode.label = QStringLiteral("Convolution");
        std::string irPath = m_convolution.irFilePath();
        QString irName = QString::fromStdString(irPath);
        int lastSlash = irName.lastIndexOf(QLatin1Char('/'));
        if (lastSlash >= 0) irName = irName.mid(lastSlash + 1);
        convNode.sublabel = QStringLiteral("Room Correction · ") + irName;
        convNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(convNode);
    }

    // ── 3e. HRTF node ────────────────────────────────────────────────
    if (m_hrtf.isEnabled() && m_hrtf.isLoaded() && ch == 2) {
        SignalPathNode hrtfNode;
        hrtfNode.label = QStringLiteral("HRTF");
        QString sofaName = m_hrtf.sofaPath();
        int lastSlash = sofaName.lastIndexOf(QLatin1Char('/'));
        if (lastSlash >= 0) sofaName = sofaName.mid(lastSlash + 1);
        hrtfNode.sublabel = QStringLiteral("Binaural · %1° · %2")
            .arg(static_cast<int>(m_hrtf.speakerAngle()))
            .arg(sofaName);
        hrtfNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(hrtfNode);
    }

    // ── 4. DSP nodes (per active processor) ─────────────────────────
    bool hasDSP = false;
    if (!m_bitPerfect.load(std::memory_order_relaxed) && m_dspPipeline->isEnabled()) {
        // Gain
        auto* gain = m_dspPipeline->gainProcessor();
        if (gain && gain->isEnabled() && std::abs(gain->gainDb()) > 0.01f) {
            SignalPathNode gainNode;
            gainNode.label = QStringLiteral("DSP");
            gainNode.detail = QStringLiteral("Preamp/Gain: %1%2 dB")
                .arg(gain->gainDb() > 0 ? QStringLiteral("+") : QString())
                .arg(gain->gainDb(), 0, 'f', 1);
            gainNode.quality = SignalPathNode::Enhanced;
            info.nodes.append(gainNode);
            hasDSP = true;
        }

        // EQ
        auto* eq = m_dspPipeline->equalizerProcessor();
        if (eq && eq->isEnabled()) {
            SignalPathNode eqNode;
            eqNode.label = QStringLiteral("DSP");
            eqNode.detail = QStringLiteral("Parametric Equalizer");
            eqNode.quality = SignalPathNode::Enhanced;
            info.nodes.append(eqNode);
            hasDSP = true;
        }

        // Plugin processors
        for (int i = 0; i < m_dspPipeline->processorCount(); ++i) {
            auto* proc = m_dspPipeline->processor(i);
            if (proc && proc->isEnabled()) {
                SignalPathNode pluginNode;
                pluginNode.label = QStringLiteral("DSP");
                pluginNode.detail = QString::fromStdString(proc->getName());
                pluginNode.quality = SignalPathNode::Enhanced;
                info.nodes.append(pluginNode);
                hasDSP = true;
            }
        }
    }

    // ── 4b. Volume Leveling node ───────────────────────────────────
    float lg = m_levelingGain.load(std::memory_order_relaxed);
    if (Settings::instance()->volumeLeveling() && lg != 1.0f) {
        SignalPathNode levelNode;
        levelNode.label = QStringLiteral("Volume Leveling");
        double db = 20.0 * std::log10(static_cast<double>(lg));
        QString src = m_currentTrack.hasReplayGain ? QStringLiteral("ReplayGain")
                    : m_currentTrack.hasR128 ? QStringLiteral("R128")
                    : QStringLiteral("Analyzing...");
        QString gainStr = (db >= 0 ? QStringLiteral("+") : QString())
                        + QString::number(db, 'f', 1) + QStringLiteral(" dB");
        levelNode.detail = src;
        levelNode.sublabel = gainStr;
        levelNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(levelNode);
        hasDSP = true;
    }

    // ── 5. Output node ──────────────────────────────────────────────
    SignalPathNode outputNode;
    outputNode.label = QStringLiteral("Output");

    std::string devName = m_output->deviceName();
    double asbdRate = m_output->currentSampleRate();
    double nominalRate = m_output->deviceNominalSampleRate();
    double displayRate = (nominalRate > 0) ? nominalRate : asbdRate;
    bool builtIn = m_output->isBuiltInOutput();
    bool bitPerfect = m_bitPerfect.load(std::memory_order_relaxed);

    outputNode.detail = QStringLiteral("%1 • %2 kHz")
        .arg(QString::fromStdString(devName))
        .arg(displayRate > 0 ? displayRate / 1000.0 : sr / 1000.0, 0, 'g', 4);

    // The rate actually fed to CoreAudio — upsampler output if active, else source
    double rateToOutput = sr;
    if (m_upsampler && m_upsampler->isActive()
        && !m_bitPerfect.load(std::memory_order_relaxed)
        && !m_usingDSDDecoder) {
        rateToOutput = m_upsampler->outputSampleRate();
    }

    bool ratesMatch = (nominalRate > 0)
        ? (std::abs(nominalRate - rateToOutput) < 1.0)
        : true;

    bool exclusive = m_output->isExclusiveMode();

    if (!hasDSP && ratesMatch && bitPerfect) {
        outputNode.sublabel = QStringLiteral("Bit-Perfect");
        outputNode.quality = exclusive ? SignalPathNode::BitPerfect : decoderNode.quality;
    } else if (builtIn && !bitPerfect && !ratesMatch) {
        outputNode.sublabel = QStringLiteral("Resampled from %1 kHz")
            .arg(rateToOutput / 1000.0, 0, 'f', 1);
        outputNode.quality = SignalPathNode::Enhanced;
    } else if (hasDSP) {
        outputNode.quality = SignalPathNode::Enhanced;
    } else {
        outputNode.quality = decoderNode.quality;
    }

    if (exclusive) {
        outputNode.sublabel += (outputNode.sublabel.isEmpty() ? QString() : QStringLiteral(" \u2022 "));
        outputNode.sublabel += QStringLiteral("Exclusive Mode");
    }

    info.nodes.append(outputNode);

    return info;
}
