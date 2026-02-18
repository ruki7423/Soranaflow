#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

#include "AudioFormat.h"
#include "SignalPathInfo.h"
#include "AudioRenderChain.h"
#include "GaplessManager.h"
#include "../MusicData.h"
#include "../../platform/IAudioOutput.h"

struct AudioDeviceInfo;
class AudioDecoder;
class DSDDecoder;
class DSPPipeline;
class UpsamplerProcessor;
class VolumeLevelingManager;

class AudioEngine : public QObject {
    // Prevent implicit destructor from being generated in the header
    // (where AudioDecoder is incomplete). Defined in .cpp.
public:
    ~AudioEngine() override;
    Q_OBJECT

public:
    enum State { Stopped, Playing, Paused };
    Q_ENUM(State)

    static AudioEngine* instance();

    State state() const { return m_state.load(std::memory_order_acquire); }
    double duration() const { return m_duration; }
    double position() const;

    std::vector<AudioDeviceInfo> availableDevices() const;
    bool setOutputDevice(uint32_t deviceId);
    bool setBufferSize(uint32_t frames);
    void setSampleRate(double newRate);

    // Bit-perfect mode: skip DSP pipeline entirely
    void setBitPerfectMode(bool enabled);
    bool bitPerfectMode() const { return m_bitPerfect.load(std::memory_order_relaxed); }

    // Exclusive mode (hog mode): take exclusive control of audio device
    void setExclusiveMode(bool enabled);
    bool exclusiveMode() const;

    // Auto sample rate: match output rate to source file rate
    void setAutoSampleRate(bool enabled);
    bool autoSampleRate() const { return m_autoSampleRate; }

    // Query max DAC sample rate for current device
    double maxDeviceSampleRate() const;

    // Upsampling
    UpsamplerProcessor* upsampler() const;
    void applyUpsamplingChange();

    // Gapless playback / crossfade
    void prepareNextTrack(const QString& filePath);
    void cancelNextTrack();
    bool isNextTrackReady() const { return m_gapless.isNextTrackReady(); }
    void setCrossfadeDuration(int ms);
    int crossfadeDurationMs() const { return m_gapless.crossfadeDurationMs(); }

    // Volume leveling
    void setCurrentTrack(const Track& track);
    void updateLevelingGain();
    float levelingGainDb() const;

    // Headroom management
    void updateHeadroomGain();

    // Signal path visualization
    SignalPathInfo getSignalPath() const;

    // Returns the actual DSD format detected by the decoder at runtime,
    // or AudioFormat::FLAC if not playing DSD via the DSD decoder.
    // Used to resolve metadata vs runtime format mismatches.
    AudioFormat actualDsdFormat() const;

    // Pre-destroy DSP resources while Qt is still alive.
    // Must be called from aboutToQuit, before static destructors run.
    void prepareForShutdown();

public slots:
    bool load(const QString& filePath);
    void play();
    void pause();
    void stop();
    void seek(double secs);
    void setVolume(float vol);  // 0.0 - 1.0

signals:
    void stateChanged(AudioEngine::State state);
    void positionChanged(double secs);
    void durationChanged(double secs);
    void playbackFinished();
    void errorOccurred(const QString& message);
    void signalPathChanged();
    void gaplessTransitionOccurred();

private:
    explicit AudioEngine(QObject* parent = nullptr);

    int renderAudio(float* buf, int maxFrames);
    void onPositionTimer();

    std::atomic<State> m_state{Stopped};
    std::atomic<double> m_duration{0.0};

    std::unique_ptr<AudioDecoder>    m_decoder;
    std::unique_ptr<DSDDecoder>      m_dsdDecoder;
    std::unique_ptr<IAudioOutput> m_output;
    std::unique_ptr<DSPPipeline>     m_dspPipeline;
    std::unique_ptr<UpsamplerProcessor> m_upsampler;
    std::mutex                       m_decoderMutex;
    std::atomic<int64_t>             m_framesRendered{0};
    std::atomic<double>              m_sampleRate{44100.0};
    std::atomic<int>                 m_channels{2};
    std::atomic<bool>                m_usingDSDDecoder{false};

    QTimer* m_positionTimer = nullptr;
    float   m_volume = 1.0f;
    uint32_t m_currentDeviceId = 0;
    std::atomic<bool>                m_bitPerfect{false};
    bool    m_autoSampleRate = false;
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<bool> m_destroyed{false};
    std::atomic<bool> m_renderingInProgress{false};
    QString m_currentFilePath;
    mutable std::mutex m_filePathMutex;
    std::vector<float> m_decodeBuf;  // Pre-allocated buffer for decoded source frames

    // Volume leveling
    VolumeLevelingManager* m_levelingManager;

    // DSP render chain (crossfeed, convolution, HRTF, headroom, limiter)
    AudioRenderChain m_renderChain;

    // Gapless / crossfade management
    GaplessManager m_gapless{&m_decoderMutex};

    // RT-safe signaling flags (set on audio thread, polled on main thread)
    std::atomic<bool> m_rtGaplessFlag{false};
    std::atomic<bool> m_rtPlaybackEndFlag{false};

public:
    DSPPipeline* dspPipeline() { return m_dspPipeline.get(); }
};
