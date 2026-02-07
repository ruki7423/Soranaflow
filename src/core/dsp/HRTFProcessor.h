#pragma once

// HRTF (Head-Related Transfer Function) processor for binaural audio.
// Simulates speaker playback on headphones using SOFA HRTF data.
//
// Requires libmysofa: https://github.com/hoene/libmysofa
// On macOS, build from source:
//   git clone https://github.com/hoene/libmysofa
//   cd libmysofa/build && cmake .. && make && sudo make install
//
// If libmysofa is not installed, this processor is disabled at compile time.

#include <QString>

#ifdef HAVE_LIBMYSOFA

#include <mysofa.h>
#include <vector>
#include <atomic>

class HRTFProcessor
{
public:
    HRTFProcessor();
    ~HRTFProcessor();

    // Load SOFA file containing HRTF data
    bool loadSOFA(const QString& filePath);
    void unloadSOFA();
    bool isLoaded() const { return m_loaded; }
    QString sofaPath() const { return m_sofaPath; }

    // Enable/disable processing
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Speaker angle from center (10° to 90°, default 30°)
    void setSpeakerAngle(float degrees);
    float speakerAngle() const { return m_speakerAngle; }

    // Prepare for given sample rate
    void setSampleRate(int rate);

    // Process interleaved stereo float samples in-place
    // Called ONLY from the audio render thread
    void process(float* buffer, int frameCount);

    // Reset filter state
    void reset();

private:
    void updateFilters();

    // SOFA data
    struct MYSOFA_EASY* m_sofa = nullptr;
    bool m_loaded = false;
    QString m_sofaPath;
    int m_sampleRate = 44100;

    // Thread-safe control
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_needsFilterUpdate{false};
    std::atomic<bool> m_needsStateReset{true};
    std::atomic<float> m_pendingAngle{-1.0f};

    // Active parameters (render thread only)
    float m_speakerAngle = 30.0f;

    // HRTF impulse responses (4 filters for stereo: LL, LR, RL, RR)
    std::vector<float> m_irLL, m_irLR, m_irRL, m_irRR;
    int m_irLength = 0;

    // FIR convolution history buffers (render thread only)
    std::vector<float> m_historyL, m_historyR;

    // Fade state
    float m_wetMix = 0.0f;
    static constexpr float FADE_STEP = 0.0005f;
};

#else // !HAVE_LIBMYSOFA

// Stub class when libmysofa is not available
class HRTFProcessor
{
public:
    HRTFProcessor() = default;
    ~HRTFProcessor() = default;

    bool loadSOFA(const QString&) { return false; }
    void unloadSOFA() {}
    bool isLoaded() const { return false; }
    QString sofaPath() const { return QString(); }

    void setEnabled(bool) {}
    bool isEnabled() const { return false; }

    void setSpeakerAngle(float) {}
    float speakerAngle() const { return 30.0f; }

    void setSampleRate(int) {}
    void process(float*, int) {}
    void reset() {}

    static bool isAvailable() { return false; }
};

#endif // HAVE_LIBMYSOFA
