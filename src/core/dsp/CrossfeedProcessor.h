#pragma once

#include <atomic>
#include <cmath>

class CrossfeedProcessor
{
public:
    enum Level { Light = 0, Medium = 1, Strong = 2 };

    CrossfeedProcessor();

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
    void setLevel(Level level);
    Level level() const { return m_level; }
    void setSampleRate(int rate);

    // Process interleaved stereo float samples in-place
    // Called ONLY from the audio render thread
    void process(float* buffer, int frameCount);

private:
    void recalculate();

    // Thread-safe control (written by main thread, read by render thread)
    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_pendingLevel{-1};    // -1 = no change pending
    std::atomic<bool> m_needsRecalc{false};     // set by setSampleRate
    std::atomic<bool> m_needsStateReset{true}; // reset on first process / re-enable

    // Active parameters (only modified by render thread via pending updates)
    Level m_level = Medium;
    int m_sampleRate = 44100;

    // DSP coefficients (only modified in recalculate(), called from render thread)
    float m_crossfeedGain = 0.0f;
    float m_directGain = 1.0f;
    float m_lpCoeff = 0.0f;
    int m_delayLen = 0;

    // Fade state (render thread only)
    float m_wetMix = 0.0f;  // 0.0 = bypass, 1.0 = full crossfeed
    static constexpr float FADE_STEP = 0.0005f;  // ~2000 sample ramp (~45ms at 44.1k)

    // Filter state (render thread only)
    float m_lpStateL = 0.0f;
    float m_lpStateR = 0.0f;

    // Delay buffer (render thread only)
    static constexpr int MAX_DELAY = 64;
    float m_delayL[MAX_DELAY] = {};
    float m_delayR[MAX_DELAY] = {};
    int m_delayIdx = 0;
    int m_prefillCount = 0;  // frames to pre-fill before fade-in starts
};
