#include "AudioEngine.h"
#include "AudioDecoder.h"
#include "AudioDeviceManager.h"
#include "DSDDecoder.h"
#include "SignalPathBuilder.h"
#include "VolumeLevelingManager.h"
#include "../dsp/DSPPipeline.h"
#include "../dsp/UpsamplerProcessor.h"
#include "../dsp/GainProcessor.h"
#include "../dsp/EqualizerProcessor.h"
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

void AudioEngine::prepareForShutdown()
{
    // Called from aboutToQuit while Qt is still alive.
    // Destroys DSP pipeline (and its VST plugins) NOW, so the static
    // destructor doesn't try to destroy them after Qt is torn down
    // (which causes recursive_mutex crash from plugin cleanup).
    qDebug() << "[SHUTDOWN] AudioEngine::prepareForShutdown START";

    m_shuttingDown.store(true, std::memory_order_release);
    m_destroyed.store(true, std::memory_order_release);

    if (m_output) {
        m_output->setRenderCallback(nullptr);
        m_output->stop();
        for (int i = 0; i < 50 && m_renderingInProgress.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        m_output.reset();
    }

    if (m_positionTimer) {
        m_positionTimer->stop();
        m_positionTimer->deleteLater();
        m_positionTimer = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        m_decoder.reset();
        m_dsdDecoder.reset();
        m_gapless.destroyDecodersLocked();
    }

    m_upsampler.reset();
    m_dspPipeline.reset();

    qDebug() << "[SHUTDOWN] AudioEngine::prepareForShutdown DONE";
}

AudioEngine::~AudioEngine()
{
    // If prepareForShutdown() was called, members are already null.
    // If not (abnormal exit), do best-effort cleanup.
    m_shuttingDown.store(true, std::memory_order_release);
    m_destroyed.store(true, std::memory_order_release);

    if (m_output) {
        m_output->setRenderCallback(nullptr);
        m_output->stop();
        m_output.reset();
    }
    // Don't lock mutexes in static destructor — they may be destroyed.
    // unique_ptrs will self-destruct if still non-null.
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
{
    m_positionTimer = new QTimer(this);
    m_positionTimer->setInterval(50);
    connect(m_positionTimer, &QTimer::timeout, this, &AudioEngine::onPositionTimer);

    // Volume leveling manager
    m_levelingManager = new VolumeLevelingManager(this);
    connect(m_levelingManager, &VolumeLevelingManager::gainChanged,
            this, &AudioEngine::signalPathChanged);

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
        m_renderChain.updateHeadroomGain();
        emit signalPathChanged();
    });

    // Re-apply volume leveling gain when settings change mid-playback
    connect(Settings::instance(), &Settings::volumeLevelingChanged,
            this, [this](bool enabled) {
        qDebug() << "[Volume Leveling] Toggled:" << enabled;
        updateLevelingGain();
        m_renderChain.updateHeadroomGain();  // Auto headroom depends on active DSP
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
    m_gapless.setCrossfadeDuration(Settings::instance()->crossfadeDurationMs());

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
    m_renderChain.crossfeed().setLevel(
        static_cast<CrossfeedProcessor::Level>(Settings::instance()->crossfeedLevel()));
    m_renderChain.crossfeed().setEnabled(Settings::instance()->crossfeedEnabled());

    connect(Settings::instance(), &Settings::crossfeedChanged,
            this, [this]() {
        m_renderChain.crossfeed().setEnabled(Settings::instance()->crossfeedEnabled());
        m_renderChain.crossfeed().setLevel(
            static_cast<CrossfeedProcessor::Level>(Settings::instance()->crossfeedLevel()));
        m_renderChain.updateHeadroomGain();  // Auto headroom may change
        emit signalPathChanged();
        qDebug() << "[Crossfeed]" << (m_renderChain.crossfeed().isEnabled() ? "ON" : "OFF")
                 << "level:" << m_renderChain.crossfeed().level();
    });

    // Verify convolution math on startup
    ConvolutionProcessor::selfTest();

    // Load convolution settings
    {
        auto* s = Settings::instance();
        m_renderChain.convolution().setEnabled(s->convolutionEnabled());
        QString irPath = s->convolutionIRPath();
        if (!irPath.isEmpty()) {
            (void)QtConcurrent::run([this, irPath]() {
                bool ok = m_renderChain.convolution().loadIR(irPath.toStdString());
                qDebug() << "[Convolution] IR load:" << irPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        m_renderChain.updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        }
    }

    connect(Settings::instance(), &Settings::convolutionChanged,
            this, [this]() {
        auto* s = Settings::instance();
        m_renderChain.convolution().setEnabled(s->convolutionEnabled());
        QString irPath = s->convolutionIRPath();
        if (!irPath.isEmpty() && irPath.toStdString() != m_renderChain.convolution().irFilePath()) {
            (void)QtConcurrent::run([this, irPath]() {
                bool ok = m_renderChain.convolution().loadIR(irPath.toStdString());
                qDebug() << "[Convolution] IR reload:" << irPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        m_renderChain.updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        } else {
            m_renderChain.updateHeadroomGain();
            emit signalPathChanged();
        }
        qDebug() << "[Convolution]" << (m_renderChain.convolution().isEnabled() ? "ON" : "OFF");
    });

    // Load HRTF settings
    {
        auto* s = Settings::instance();
        m_renderChain.hrtf().setEnabled(s->hrtfEnabled());
        m_renderChain.hrtf().setSpeakerAngle(s->hrtfSpeakerAngle());
        QString sofaPath = s->hrtfSofaPath();
        if (!sofaPath.isEmpty()) {
            (void)QtConcurrent::run([this, sofaPath]() {
                bool ok = m_renderChain.hrtf().loadSOFA(sofaPath);
                qDebug() << "[HRTF] SOFA load:" << sofaPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        m_renderChain.updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        }
    }

    connect(Settings::instance(), &Settings::hrtfChanged,
            this, [this]() {
        auto* s = Settings::instance();
        m_renderChain.hrtf().setEnabled(s->hrtfEnabled());
        m_renderChain.hrtf().setSpeakerAngle(s->hrtfSpeakerAngle());
        QString sofaPath = s->hrtfSofaPath();
        if (!sofaPath.isEmpty() && sofaPath != m_renderChain.hrtf().sofaPath()) {
            (void)QtConcurrent::run([this, sofaPath]() {
                bool ok = m_renderChain.hrtf().loadSOFA(sofaPath);
                qDebug() << "[HRTF] SOFA reload:" << sofaPath << (ok ? "OK" : "FAILED");
                if (ok) {
                    QMetaObject::invokeMethod(this, [this]() {
                        m_renderChain.updateHeadroomGain();
                        emit signalPathChanged();
                    }, Qt::QueuedConnection);
                }
            });
        } else {
            m_renderChain.updateHeadroomGain();
            emit signalPathChanged();
        }
        qDebug() << "[HRTF]" << (m_renderChain.hrtf().isEnabled() ? "ON" : "OFF")
                 << "angle:" << m_renderChain.hrtf().speakerAngle();
    });

    // Initialize headroom gain from persisted settings
    m_renderChain.updateHeadroomGain();
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

    // Capture previous DSD state before stop() closes decoders
    bool prevWasDoP = m_usingDSDDecoder.load(std::memory_order_relaxed)
                      && m_dsdDecoder && m_dsdDecoder->isDoPMode();

    // Silence the render callback BEFORE teardown to prevent stale DoP data
    // from reaching the DAC during the transition window.
    // Critical for DSD→PCM and DSD→DSD transitions where the callback may
    // fire one more time with old DoP state while format is being changed.
    m_output->setTransitioning(true);

    // Keep dopPassthrough=true during stop() so the render callback outputs
    // DoP silence (valid markers) instead of PCM zeros.  The DAC stays in
    // DSD mode and produces clean silence until the AudioUnit fully stops.
    stop();

    // Clear DoP passthrough AFTER stop — AudioUnit is stopped, no more callbacks
    if (prevWasDoP) {
        m_output->setDoPPassthrough(false);
    }

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
            if (m_dsdDecoder->openDSD(filePath.toStdString(), true)) {
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
    m_renderChain.setSampleRate(static_cast<int>(fmt.sampleRate));
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
    m_gapless.preallocateCrossfadeBuffer(fmt.channels);

    // Prepare DSP pipeline at the output rate (post-upsampling)
    m_dspPipeline->prepare(outputFmt.sampleRate, fmt.channels);

    // Open audio output at the (potentially upsampled) rate
    // (stop() already closed the output — open() also calls close() internally)
    m_output->setRenderCallback([this](float* buf, int frames) -> int {
        return renderAudio(buf, frames);
    });
    m_output->setVolume(m_volume);

    // DoP passthrough: skip volume scaling in CoreAudio callback so DoP markers survive
    bool isDoPMode = m_usingDSDDecoder && m_dsdDecoder->isDoPMode();
    m_output->setDoPPassthrough(isDoPMode);

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

    m_renderChain.updateHeadroomGain();

    // Log DSD-involved transitions
    bool nextIsDoP = m_usingDSDDecoder && m_dsdDecoder->isDoPMode();
    if (prevWasDoP || nextIsDoP) {
        qDebug() << "[AudioEngine] DSD transition:"
                 << (prevWasDoP ? "DoP" : "PCM") << "->"
                 << (nextIsDoP ? "DoP" : "PCM")
                 << "— transitioning mute + AudioUnitReset";
    }

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
        m_gapless.resetLocked();
    }
    m_usingDSDDecoder = false;
    m_framesRendered.store(0, std::memory_order_relaxed);
    m_dspPipeline->reset();

    // Zero pre-allocated buffer to prevent stale data on next track start
    if (!m_decodeBuf.empty())
        std::memset(m_decodeBuf.data(), 0, m_decodeBuf.size() * sizeof(float));

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
std::vector<AudioDeviceInfo> AudioEngine::availableDevices() const
{
    return AudioDeviceManager::instance()->outputDevices();
}

// ── setOutputDevice ─────────────────────────────────────────────────
bool AudioEngine::setOutputDevice(uint32_t deviceId)
{
    qDebug() << "AudioEngine::setOutputDevice(" << deviceId << ")";

    // Validate the device exists before storing it
    if (deviceId != 0) {
        auto info = AudioDeviceManager::instance()->deviceById(deviceId);
        if (info.deviceId == 0) {
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
            // Do NOT emit signalPathChanged() inside the lock —
            // receivers call getSignalPath() which also locks m_filePathMutex,
            // causing a same-thread deadlock on the non-recursive std::mutex.
        } else {
            path = m_currentFilePath;
        }
    }

    if (path.isEmpty()) {
        emit signalPathChanged();
        return;
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
    m_levelingManager->setCurrentTrack(track);
}

// ── updateHeadroomGain ──────────────────────────────────────────────
void AudioEngine::updateHeadroomGain()
{
    m_renderChain.updateHeadroomGain();
}

// ── updateLevelingGain ──────────────────────────────────────────────
void AudioEngine::updateLevelingGain()
{
    m_levelingManager->updateGain();
}

// ── levelingGainDb ──────────────────────────────────────────────────
float AudioEngine::levelingGainDb() const
{
    return m_levelingManager->gainDb();
}

// ── prepareNextTrack (delegated to GaplessManager) ───────────────────
void AudioEngine::prepareNextTrack(const QString& filePath)
{
    m_gapless.prepareNextTrack(filePath,
        m_output->getMaxSampleRate(m_currentDeviceId),
        m_sampleRate.load(std::memory_order_relaxed),
        m_channels.load(std::memory_order_relaxed),
        m_usingDSDDecoder.load(std::memory_order_relaxed));
}

void AudioEngine::cancelNextTrack() { m_gapless.cancelNextTrack(); }
void AudioEngine::setCrossfadeDuration(int ms) { m_gapless.setCrossfadeDuration(ms); }

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

            // 3. Apply full DSP chain (headroom → crossfeed → convolution → HRTF → DSP → leveling → limiter)
            m_renderChain.process(buf, outputFrames, channels,
                                  m_dspPipeline.get(), m_levelingManager,
                                  false /*dopPassthrough*/, false /*bitPerfect*/);

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
            double sr = m_sampleRate.load(std::memory_order_relaxed);

            // Start crossfade if approaching end of track
            if (framesRead > 0) {
                int cfMs = m_gapless.crossfadeDurationMs();
                if (!m_gapless.isCrossfading() && cfMs > 0
                    && m_gapless.isNextTrackReady()
                    && !m_gapless.nextUsingDSD()
                    && std::abs(m_gapless.nextFormat().sampleRate - sr) < 1.0
                    && m_gapless.nextFormat().channels == channels) {
                    int64_t totalFrames = (int64_t)(m_duration * sr);
                    int64_t cfFrames = (int64_t)(cfMs * sr / 1000.0);
                    int64_t pos = m_framesRendered.load(std::memory_order_relaxed);
                    if (totalFrames > cfFrames && pos >= totalFrames - cfFrames) {
                        m_gapless.startCrossfade(pos, totalFrames, cfFrames);
                    }
                }
            }

            // Mix incoming track during crossfade
            if (m_gapless.isCrossfading()) {
                crossfadeHandledFrames = true;
                int cfProgress = m_gapless.crossfadeProgress();
                int cfTotal = m_gapless.crossfadeTotalFrames();

                if (framesRead == 0) {
                    // Outgoing track ended mid-crossfade — read only incoming
                    framesRead = m_gapless.nextDecoder()->isOpen()
                        ? m_gapless.nextDecoder()->read(buf, maxFrames) : 0;
                    // Apply fade-in gain to incoming-only frames
                    for (int f = 0; f < framesRead; f++) {
                        float t = (float)(cfProgress + f) / (float)cfTotal;
                        t = std::min(t, 1.0f);
                        float gainIn = std::sin(t * (float)M_PI_2);
                        for (int ch = 0; ch < channels; ch++)
                            buf[f * channels + ch] *= gainIn;
                    }
                } else {
                    // Both tracks active — read incoming and mix
                    // Cap to available buffer capacity (avoid RT allocation)
                    int maxNextFrames = std::min(framesRead, m_gapless.crossfadeBufCapacity(channels));
                    int nextRead = (m_gapless.nextDecoder()->isOpen() && maxNextFrames > 0)
                        ? m_gapless.nextDecoder()->read(m_gapless.crossfadeBufData(), maxNextFrames) : 0;
                    for (int f = 0; f < framesRead; f++) {
                        float t = (float)(cfProgress + f) / (float)cfTotal;
                        t = std::min(t, 1.0f);
                        float gainOut = std::cos(t * (float)M_PI_2);
                        float gainIn  = std::sin(t * (float)M_PI_2);
                        for (int ch = 0; ch < channels; ch++) {
                            int idx = f * channels + ch;
                            float incoming = (f < nextRead) ? m_gapless.crossfadeBufData()[idx] : 0.0f;
                            buf[idx] = buf[idx] * gainOut + incoming * gainIn;
                        }
                    }
                    // Track outgoing position (these frames were from the outgoing track)
                    m_framesRendered.fetch_add(framesRead, std::memory_order_relaxed);
                }
                m_gapless.advanceCrossfade(framesRead);

                // Crossfade complete — swap to incoming track
                if (m_gapless.crossfadeProgress() >= m_gapless.crossfadeTotalFrames()) {
                    int cfProg = m_gapless.crossfadeProgress();
                    auto tr = m_gapless.swapToCurrent(m_decoder, m_dsdDecoder,
                                                       m_usingDSDDecoder,
                                                       m_filePathMutex, m_currentFilePath);
                    m_duration = tr.newDuration;
                    m_sampleRate.store(tr.newSampleRate, std::memory_order_relaxed);
                    m_channels.store(tr.newChannels, std::memory_order_relaxed);
                    m_framesRendered.store(cfProg, std::memory_order_relaxed);

                    // Signal main thread via atomic flag (RT-safe)
                    m_rtGaplessFlag.store(true, std::memory_order_release);
                }
            }
        }

        // Apply full DSP chain (handles DoP passthrough and bit-perfect internally)
        {
            const bool dopPassthrough = m_usingDSDDecoder && m_dsdDecoder->isDoPMode();
            m_renderChain.process(buf, framesRead, channels,
                                  m_dspPipeline.get(), m_levelingManager,
                                  dopPassthrough, m_bitPerfect.load(std::memory_order_relaxed));
        }
    }

    // ── Fade-in ramp for first ~10ms of new track (PCM only) ──────────
    // Prevents DC offset clicks and DAC settling crackle at track boundaries.
    // Skip for DoP passthrough — modifying DoP data corrupts markers.
    {
        const bool dopPassthroughNow = m_usingDSDDecoder && m_dsdDecoder->isDoPMode();
        if (framesRead > 0 && !dopPassthroughNow) {
            double sr = m_sampleRate.load(std::memory_order_relaxed);
            int fadeFrames = (int)(sr * 0.010); // 10ms ramp
            if (fadeFrames < 1) fadeFrames = 1;
            int64_t rendered = m_framesRendered.load(std::memory_order_relaxed);
            if (rendered < fadeFrames) {
                int startFrame = (int)rendered;
                for (int f = 0; f < framesRead && (startFrame + f) < fadeFrames; ++f) {
                    float gain = (float)(startFrame + f) / (float)fadeFrames;
                    for (int c = 0; c < channels; ++c) {
                        buf[f * channels + c] *= gain;
                    }
                }
            }
        }
    }

    if (!crossfadeHandledFrames)
        m_framesRendered.fetch_add(framesRead, std::memory_order_relaxed);

    if (framesRead == 0 && !m_gapless.isCrossfading()) {
        // Current track ended — check if we can do a gapless transition
        if (m_gapless.isNextTrackReady()) {
            auto tr = m_gapless.swapToCurrent(m_decoder, m_dsdDecoder,
                                               m_usingDSDDecoder,
                                               m_filePathMutex, m_currentFilePath);
            m_duration = tr.newDuration;
            m_sampleRate.store(tr.newSampleRate, std::memory_order_relaxed);
            m_channels.store(tr.newChannels, std::memory_order_relaxed);
            m_framesRendered.store(0, std::memory_order_relaxed);

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

                bool gaplessDoPPassthrough = m_usingDSDDecoder && m_dsdDecoder->isDoPMode();
                if (newFrames > 0 && !gaplessDoPPassthrough
                    && !m_bitPerfect.load(std::memory_order_relaxed)) {
                    m_dspPipeline->process(buf, newFrames, channels);
                }
                // Update DoP passthrough flag for the new track
                m_output->setDoPPassthrough(gaplessDoPPassthrough);
                m_framesRendered.fetch_add(newFrames, std::memory_order_relaxed);
            }

            // Signal main thread via atomic flag (RT-safe)
            m_rtGaplessFlag.store(true, std::memory_order_release);

            m_renderingInProgress.store(false, std::memory_order_release);
            return newFrames;
        }

        // DoP track ended — set transitioning so the render callback outputs
        // DoP silence (valid markers + idle payload) instead of calling
        // renderCb.  Keep dopPassthrough=true so the callback knows to use
        // DoP-formatted silence rather than PCM zeros.  The DAC stays in DSD
        // mode and produces clean silence during the ~50ms until
        // onPositionTimer stops the AudioUnit.
        if (m_usingDSDDecoder && m_dsdDecoder->isDoPMode()) {
            m_output->setTransitioning(true);
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
        // Ensure output is muted before stop (audio thread may have
        // already set these, but belt-and-suspenders for safety).
        // Keep dopPassthrough=true so render callback outputs DoP silence
        // (valid markers) instead of PCM zeros during AudioOutputUnitStop.
        m_output->setTransitioning(true);
        m_output->stop();
        // Clear AFTER stop — AudioUnit is stopped, no more callbacks
        m_output->setDoPPassthrough(false);
        m_positionTimer->stop();
        m_state = Stopped;
        emit playbackFinished();
        return;  // timer is stopped, no need to emit position
    }

    double pos = position();
    emit positionChanged(pos);
}

// ── getSignalPath ────────────────────────────────────────────────────────────

SignalPathInfo AudioEngine::getSignalPath() const
{
    AudioState state;

    // Engine state
    {
        std::lock_guard<std::mutex> lock(m_filePathMutex);
        state.isStopped = (m_state == Stopped);
        state.hasFilePath = !m_currentFilePath.isEmpty();
    }

    state.sampleRate = m_sampleRate.load(std::memory_order_relaxed);
    state.channels = m_channels.load(std::memory_order_relaxed);
    state.bitPerfect = m_bitPerfect.load(std::memory_order_relaxed);

    // DSD decoder
    state.usingDSDDecoder = m_usingDSDDecoder.load(std::memory_order_relaxed);
    if (state.usingDSDDecoder && m_dsdDecoder) {
        state.isDSD64 = m_dsdDecoder->isDSD64();
        state.isDSD128 = m_dsdDecoder->isDSD128();
        state.isDSD256 = m_dsdDecoder->isDSD256();
        state.isDSD512 = m_dsdDecoder->isDSD512();
        state.dsdSampleRate = m_dsdDecoder->dsdSampleRate();
        state.isDoPMode = m_dsdDecoder->isDoPMode();
    }

    // PCM decoder
    if (m_decoder && m_decoder->isOpen()) {
        state.decoderOpen = true;
        state.codecName = m_decoder->codecName();
        state.decoderFormat = m_decoder->format();
    }

    // Upsampler
    if (m_upsampler && m_upsampler->isActive()) {
        state.upsamplerActive = true;
        state.upsamplerDescription = m_upsampler->getDescription();
        state.upsamplerOutputRate = m_upsampler->outputSampleRate();
    }

    // Headroom
    state.headroomGain = m_renderChain.headroomGainLinear();
    auto hrMode = Settings::instance()->headroomMode();
    if (hrMode == Settings::HeadroomMode::Auto)
        state.headroomMode = AudioState::HRAuto;
    else if (hrMode == Settings::HeadroomMode::Manual)
        state.headroomMode = AudioState::HRManual;
    else
        state.headroomMode = AudioState::HROff;

    // Crossfeed
    state.crossfeedEnabled = m_renderChain.crossfeed().isEnabled();
    state.crossfeedLevel = static_cast<int>(m_renderChain.crossfeed().level());

    // Convolution
    state.convolutionEnabled = m_renderChain.convolution().isEnabled();
    state.convolutionHasIR = m_renderChain.convolution().hasIR();
    if (state.convolutionHasIR) {
        state.convolutionIRPath = QString::fromStdString(m_renderChain.convolution().irFilePath());
    }

    // HRTF
    state.hrtfEnabled = m_renderChain.hrtf().isEnabled();
    state.hrtfLoaded = m_renderChain.hrtf().isLoaded();
    state.hrtfSofaPath = m_renderChain.hrtf().sofaPath();
    state.hrtfSpeakerAngle = m_renderChain.hrtf().speakerAngle();

    // DSP pipeline
    if (m_dspPipeline) {
        state.dspEnabled = m_dspPipeline->isEnabled();
        auto* gain = m_dspPipeline->gainProcessor();
        if (gain) {
            state.gainEnabled = gain->isEnabled();
            state.gainDb = gain->gainDb();
        }
        auto* eq = m_dspPipeline->equalizerProcessor();
        if (eq) {
            state.eqEnabled = eq->isEnabled();
        }
        for (int i = 0; i < m_dspPipeline->processorCount(); ++i) {
            auto* proc = m_dspPipeline->processor(i);
            if (proc) {
                AudioState::PluginInfo pi;
                pi.name = QString::fromStdString(proc->getName());
                pi.enabled = proc->isEnabled();
                state.plugins.append(pi);
            }
        }
    }

    // Volume leveling
    state.levelingGain = m_levelingManager->gainLinear();
    state.volumeLevelingEnabled = Settings::instance()->volumeLeveling();
    state.hasReplayGain = m_levelingManager->currentTrack().hasReplayGain;
    state.hasR128 = m_levelingManager->currentTrack().hasR128;

    // Output
    if (m_output) {
        state.outputDeviceName = QString::fromStdString(m_output->deviceName());
        state.outputCurrentRate = m_output->currentSampleRate();
        state.outputNominalRate = m_output->deviceNominalSampleRate();
        state.outputBuiltIn = m_output->isBuiltInOutput();
        state.outputExclusive = m_output->isExclusiveMode();
    }

    // Settings
    state.dsdPlaybackMode = Settings::instance()->dsdPlaybackMode();

    return SignalPathBuilder::build(state);
}

AudioFormat AudioEngine::actualDsdFormat() const
{
    if (!m_usingDSDDecoder.load(std::memory_order_relaxed))
        return AudioFormat::FLAC; // sentinel: not using DSD decoder

    if (m_dsdDecoder->isDSD2048()) return AudioFormat::DSD2048;
    if (m_dsdDecoder->isDSD1024()) return AudioFormat::DSD1024;
    if (m_dsdDecoder->isDSD512())  return AudioFormat::DSD512;
    if (m_dsdDecoder->isDSD256())  return AudioFormat::DSD256;
    if (m_dsdDecoder->isDSD128())  return AudioFormat::DSD128;
    if (m_dsdDecoder->isDSD64())   return AudioFormat::DSD64;
    return AudioFormat::DSD64; // fallback
}
