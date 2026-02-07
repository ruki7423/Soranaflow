#include "CrossfeedProcessor.h"
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CrossfeedProcessor::CrossfeedProcessor()
{
    recalculate();
}

void CrossfeedProcessor::setEnabled(bool enabled)
{
    if (enabled && !m_enabled.load(std::memory_order_relaxed)) {
        m_needsStateReset.store(true, std::memory_order_relaxed);
    }
    m_enabled.store(enabled, std::memory_order_relaxed);
}

void CrossfeedProcessor::setLevel(Level level)
{
    m_pendingLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

void CrossfeedProcessor::setSampleRate(int rate)
{
    m_sampleRate = rate;
    m_needsRecalc.store(true, std::memory_order_relaxed);
}

void CrossfeedProcessor::recalculate()
{
    // Crossfeed parameters based on level (bs2b-inspired)
    // Light:  -6 dB crossfeed, 700 Hz cutoff
    // Medium: -4.5 dB crossfeed, 700 Hz cutoff  (bs2b default)
    // Strong: -3 dB crossfeed, 650 Hz cutoff
    float crossfeedDB = -4.5f;
    float cutoffHz = 700.0f;

    switch (m_level) {
    case Light:
        crossfeedDB = -6.0f;
        cutoffHz = 700.0f;
        break;
    case Medium:
        crossfeedDB = -4.5f;
        cutoffHz = 700.0f;
        break;
    case Strong:
        crossfeedDB = -3.0f;
        cutoffHz = 650.0f;
        break;
    }

    float rawCrossfeed = std::pow(10.0f, crossfeedDB / 20.0f);

    // Normalize so worst case (mono/correlated) never exceeds 1.0:
    // direct + crossfeed = 1.0, preserving the ratio
    m_directGain = 1.0f / (1.0f + rawCrossfeed);
    m_crossfeedGain = rawCrossfeed / (1.0f + rawCrossfeed);

    // Low-pass filter coefficient (1-pole): a = exp(-2*pi*fc/fs)
    float w = 2.0f * static_cast<float>(M_PI) * cutoffHz / static_cast<float>(m_sampleRate);
    m_lpCoeff = std::exp(-w);

    // Delay: ~300 microseconds (interaural time difference)
    float delaySec = 0.0003f;
    m_delayLen = static_cast<int>(delaySec * m_sampleRate + 0.5f);
    m_delayLen = std::min(m_delayLen, MAX_DELAY - 1);
    m_delayLen = std::max(m_delayLen, 1);
}

void CrossfeedProcessor::process(float* buffer, int frameCount)
{
    bool wantEnabled = m_enabled.load(std::memory_order_relaxed);

    // If disabled and fully faded out, skip entirely
    if (!wantEnabled && m_wetMix <= 0.0f) return;

    // Clear filter state on enable (render thread safe, atomic flag)
    if (m_needsStateReset.exchange(false, std::memory_order_relaxed)) {
        m_lpStateL = 0.0f;
        m_lpStateR = 0.0f;
        std::memset(m_delayL, 0, sizeof(m_delayL));
        std::memset(m_delayR, 0, sizeof(m_delayR));
        m_delayIdx = 0;
        m_wetMix = 0.0f;
        m_prefillCount = m_delayLen;
    }

    // Apply pending level change (render thread owns the parameters)
    int pending = m_pendingLevel.exchange(-1, std::memory_order_relaxed);
    if (pending >= 0) {
        m_level = static_cast<Level>(pending);
        recalculate();
        // No state reset — coefficients change smoothly
    }

    // Apply pending sample rate recalculation
    if (m_needsRecalc.exchange(false, std::memory_order_relaxed)) {
        recalculate();
    }

    for (int i = 0; i < frameCount; i++) {
        float L = buffer[i * 2];
        float R = buffer[i * 2 + 1];

        // Pre-fill: run LP filter and fill delay buffer, output stays dry
        if (m_prefillCount > 0) {
            m_lpStateL = L * (1.0f - m_lpCoeff) + m_lpStateL * m_lpCoeff;
            m_lpStateR = R * (1.0f - m_lpCoeff) + m_lpStateR * m_lpCoeff;
            m_delayL[m_delayIdx] = m_lpStateL;
            m_delayR[m_delayIdx] = m_lpStateR;
            m_delayIdx = (m_delayIdx + 1) % MAX_DELAY;
            m_prefillCount--;
            continue;
        }

        // Smooth fade in/out
        if (wantEnabled && m_wetMix < 1.0f) {
            m_wetMix = std::min(1.0f, m_wetMix + FADE_STEP);
        } else if (!wantEnabled && m_wetMix > 0.0f) {
            m_wetMix = std::max(0.0f, m_wetMix - FADE_STEP);
        }

        // Low-pass filter the crossfeed signal
        m_lpStateL = L * (1.0f - m_lpCoeff) + m_lpStateL * m_lpCoeff;
        m_lpStateR = R * (1.0f - m_lpCoeff) + m_lpStateR * m_lpCoeff;

        // Get delayed crossfeed signal
        int readIdx = (m_delayIdx - m_delayLen + MAX_DELAY) % MAX_DELAY;
        float delayedL = m_delayL[readIdx];
        float delayedR = m_delayR[readIdx];

        // Write current LP-filtered signal to delay buffer
        m_delayL[m_delayIdx] = m_lpStateL;
        m_delayR[m_delayIdx] = m_lpStateR;
        m_delayIdx = (m_delayIdx + 1) % MAX_DELAY;

        // Crossfeed mix
        float wetL = L * m_directGain + delayedR * m_crossfeedGain;
        float wetR = R * m_directGain + delayedL * m_crossfeedGain;

        // Blend dry/wet based on fade position
        buffer[i * 2]     = L * (1.0f - m_wetMix) + wetL * m_wetMix;
        buffer[i * 2 + 1] = R * (1.0f - m_wetMix) + wetR * m_wetMix;
    }

    // If fully faded out, clear filter state for clean restart
    if (m_wetMix <= 0.0f) {
        m_lpStateL = 0.0f;
        m_lpStateR = 0.0f;
        std::memset(m_delayL, 0, sizeof(m_delayL));
        std::memset(m_delayR, 0, sizeof(m_delayR));
        m_delayIdx = 0;
    }
}
