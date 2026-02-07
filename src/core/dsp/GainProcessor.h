#pragma once

#include "IDSPProcessor.h"
#include <cmath>
#include <atomic>

// Simple gain/preamp processor. Adjusts volume in dB.
class GainProcessor : public IDSPProcessor {
public:
    GainProcessor() = default;

    void process(float* buf, int frames, int channels) override {
        if (!m_enabled) return;
        float gain = m_linearGain.load(std::memory_order_relaxed);
        if (gain == 1.0f) return;  // unity gain, no-op
        int totalSamples = frames * channels;
        for (int i = 0; i < totalSamples; ++i) {
            buf[i] *= gain;
        }
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

    void reset() override {}

private:
    bool m_enabled = true;
    float m_gainDb = 0.0f;
    std::atomic<float> m_linearGain{1.0f};
};
