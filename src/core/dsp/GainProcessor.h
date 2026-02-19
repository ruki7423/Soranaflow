#pragma once

#include "IDSPProcessor.h"
#include <cmath>
#include <atomic>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

// Simple gain/preamp processor. Adjusts volume in dB.
// Uses per-sample linear ramp to prevent clicks on gain change.
class GainProcessor : public IDSPProcessor {
public:
    GainProcessor() = default;

    void process(float* buf, int frames, int channels) override {
        if (!m_enabled) return;
        float target = m_linearGain.load(std::memory_order_relaxed);
        float prev = m_prevGain;
        if (target == 1.0f && prev == 1.0f) return;

        // Per-sample linear ramp from previous gain to target gain
#ifdef __APPLE__
        if (prev != target) {
            if (frames > 1) {
                float step = (target - prev) / (float)(frames - 1);
                for (int c = 0; c < channels; ++c) {
                    float start = prev;
                    vDSP_vrampmul(buf + c, channels, &start, &step, buf + c, channels, frames);
                }
            } else {
                vDSP_vsmul(buf, 1, &target, buf, 1, frames * channels);
            }
        } else {
            vDSP_vsmul(buf, 1, &target, buf, 1, frames * channels);
        }
#else
        if (prev != target) {
            for (int f = 0; f < frames; ++f) {
                float t = (frames > 1) ? (float)f / (float)(frames - 1) : 1.0f;
                float gain = prev + (target - prev) * t;
                for (int c = 0; c < channels; ++c)
                    buf[f * channels + c] *= gain;
            }
        } else {
            int n = frames * channels;
            for (int i = 0; i < n; ++i)
                buf[i] *= target;
        }
#endif
        m_prevGain = target;
    }

    std::string getName() const override { return "Preamp/Gain"; }

    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool enabled) override { m_enabled = enabled; }

    // Gain in dB (-24 to +24)
    void setGainDb(float dB) {
        m_gainDb = dB;
        m_linearGain.store(std::pow(10.0f, dB / 20.0f), std::memory_order_relaxed);
    }

    float gainDb() const { return m_gainDb; }

    std::vector<DSPParameter> getParameters() const override {
        return {{
            "Gain", m_gainDb, -24.0f, 24.0f, 0.0f, "dB"
        }};
    }

    void setParameter(int index, float value) override {
        if (index == 0) setGainDb(value);
    }

    float getParameter(int index) const override {
        if (index == 0) return m_gainDb;
        return 0.0f;
    }

    void reset() override { m_prevGain = 1.0f; }

private:
    std::atomic<bool> m_enabled{true};
    float m_gainDb = 0.0f;
    std::atomic<float> m_linearGain{1.0f};
    float m_prevGain = 1.0f;  // render-thread only, for smooth ramping
};
