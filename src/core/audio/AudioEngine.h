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
#include "../MusicData.h"
#include "../dsp/CrossfeedProcessor.h"
#include "../dsp/ConvolutionProcessor.h"
#include "../dsp/HRTFProcessor.h"
#include "../../platform/IAudioOutput.h"

class AudioDecoder;
class DSDDecoder;
class DSPPipeline;
class UpsamplerProcessor;

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

    State state() const { return m_state; }
    double duration() const { return m_duration; }
    double position() const;

    std::vector<AudioDevice> availableDevices() const;
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
    bool isNextTrackReady() const { return m_nextTrackReady.load(std::memory_order_relaxed); }
    void setCrossfadeDuration(int ms);

    // Volume leveling
    void setCurrentTrack(const Track& track);
    void updateLevelingGain();
    float levelingGainDb() const;

    // Headroom management
    void updateHeadroomGain();

    // Signal path visualization
    SignalPathInfo getSignalPath() const;

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

    State m_state = Stopped;
    double m_duration = 0.0;

    std::unique_ptr<AudioDecoder>    m_decoder;
    std::unique_ptr<DSDDecoder>      m_dsdDecoder;
    std::unique_ptr<IAudioOutput> m_output;
    std::unique_ptr<DSPPipeline>     m_dspPipeline;
    std::unique_ptr<UpsamplerProcessor> m_upsampler;
    std::mutex                       m_decoderMutex;
    std::atomic<int64_t>             m_framesRendered{0};
    std::atomic<double>              m_sampleRate{44100.0};
    std::atomic<int>                 m_channels{2};
    bool                             m_usingDSDDecoder = false;

    // Gapless playback: pre-decoded next track
    std::unique_ptr<AudioDecoder>    m_nextDecoder;
    std::unique_ptr<DSDDecoder>      m_nextDsdDecoder;
    std::atomic<bool>                m_nextTrackReady{false};
    bool                             m_nextUsingDSD = false;
    AudioStreamFormat                m_nextFormat{};
    QString                          m_nextFilePath;

    QTimer* m_positionTimer = nullptr;
    float   m_volume = 1.0f;
    uint32_t m_currentDeviceId = 0;
    std::atomic<bool>                m_bitPerfect{false};
    bool    m_autoSampleRate = false;
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<bool> m_destroyed{false};
    std::atomic<bool> m_renderingInProgress{false};
    QString m_currentFilePath;
    std::vector<float> m_decodeBuf;  // Pre-allocated buffer for decoded source frames

    // Volume leveling
    Track m_currentTrack;
    std::atomic<float> m_levelingGain{1.0f};

    // Headroom management
    std::atomic<float> m_headroomGain{1.0f};

    // Crossfade (track transitions)
    std::atomic<int>  m_crossfadeDurationMs{0};   // 0 = off (gapless only)
    bool              m_crossfading = false;
    int               m_crossfadeProgress = 0;    // frames mixed so far
    int               m_crossfadeTotalFrames = 0; // total frames to crossfade
    std::vector<float> m_crossfadeBuf;            // buffer for outgoing track

    // Crossfeed
    CrossfeedProcessor m_crossfeed;

    // Convolution (Room Correction / IR)
    ConvolutionProcessor m_convolution;

    // HRTF (Binaural Spatial Audio)
    HRTFProcessor m_hrtf;

    // Render diagnostics
    std::atomic<bool> m_renderDiagOnce{false};

public:
    DSPPipeline* dspPipeline() { return m_dspPipeline.get(); }
};
