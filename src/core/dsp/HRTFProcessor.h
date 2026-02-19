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
#include "IDSPProcessor.h"

#ifdef HAVE_LIBMYSOFA

#include <mysofa.h>
#include <vector>
#include <atomic>
#include <mutex>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

class HRTFProcessor : public IDSPProcessor
{
public:
    HRTFProcessor();
    ~HRTFProcessor() override;

    // Load SOFA file containing HRTF data
    bool loadSOFA(const QString& filePath);
    void unloadSOFA();
    bool isLoaded() const { std::lock_guard<std::mutex> lock(m_metaMutex); return m_loaded; }
    QString sofaPath() const { std::lock_guard<std::mutex> lock(m_metaMutex); return m_sofaPath; }

    // IDSPProcessor interface
    void process(float* buf, int frames, int channels) override;
    std::string getName() const override { return "HRTF"; }
    bool isEnabled() const override { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled) override;
    void reset() override;

    // Speaker angle from center (10° to 90°, default 30°)
    void setSpeakerAngle(float degrees);
    float speakerAngle() const { return m_speakerAngle; }

    // Prepare for given sample rate
    void setSampleRate(int rate);

    // Direct stereo process — called by AudioEngine render path
    void process(float* buffer, int frameCount);

private:
    void buildStagedFilters(float angle);

    // SOFA data (m_loaded, m_sofaPath, m_irLength protected by m_metaMutex for non-RT reads)
    mutable std::mutex m_metaMutex;
    struct MYSOFA_EASY* m_sofa = nullptr;
    bool m_loaded = false;
    QString m_sofaPath;
    int m_sampleRate = 44100;

    // Thread-safe control
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_needsStateReset{true};

    // Active parameters (render thread only)
    float m_speakerAngle = 30.0f;

    // HRTF impulse responses (4 filters for stereo: LL, LR, RL, RR)
    std::vector<float> m_irLL, m_irLR, m_irRL, m_irRR;
    int m_irLength = 0;

    // FIR convolution history buffers (render thread only)
    std::vector<float> m_historyL, m_historyR;

    // Staged filter data (built by UI thread, swapped by RT thread)
    struct StagedFilters {
        std::vector<float> irLL, irLR, irRL, irRR;
#ifdef __APPLE__
        std::vector<float> revIrLL, revIrLR, revIrRL, revIrRR;  // reversed for vDSP_conv
        std::vector<float> extL, extR;   // extended signal [history | current]
        std::vector<float> outL, outR;   // block FIR output
        std::vector<float> tempFir;      // temp accumulation
#else
        std::vector<float> historyL, historyR;
#endif
        int irLength = 0;
        float angle = 0.0f;
    };
    StagedFilters m_staged;
    std::atomic<bool> m_stagedReady{false};

#ifdef __APPLE__
    // Block-based FIR buffers (RT-only, swapped from staged)
    std::vector<float> m_revIrLL, m_revIrLR, m_revIrRL, m_revIrRR;
    std::vector<float> m_extL, m_extR;
    std::vector<float> m_outL, m_outR;
    std::vector<float> m_tempFir;
#else
    std::vector<float> m_historyL, m_historyR;
#endif

    // Fade state
    float m_wetMix = 0.0f;
    static constexpr float FADE_STEP = 0.0005f;
};

#else // !HAVE_LIBMYSOFA

// Stub class when libmysofa is not available
class HRTFProcessor : public IDSPProcessor
{
public:
    HRTFProcessor() = default;
    ~HRTFProcessor() override = default;

    bool loadSOFA(const QString&) { return false; }
    void unloadSOFA() {}
    bool isLoaded() const { return false; }
    QString sofaPath() const { return QString(); }

    void process(float*, int, int) override {}
    std::string getName() const override { return "HRTF"; }
    bool isEnabled() const override { return false; }
    void setEnabled(bool) override {}
    void reset() override {}

    void setSpeakerAngle(float) {}
    float speakerAngle() const { return 30.0f; }

    void setSampleRate(int) {}
    void process(float*, int) {}

    static bool isAvailable() { return false; }
};

#endif // HAVE_LIBMYSOFA
