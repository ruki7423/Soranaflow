#pragma once

#include "IDSPProcessor.h"
#include <cmath>
#include <array>
#include <vector>

// Biquad filter coefficients
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

// Per-channel biquad state
struct BiquadState {
    double x1 = 0.0, x2 = 0.0;  // input history
    double y1 = 0.0, y2 = 0.0;  // output history
};

// EQ band parameters with filter type
struct EQBand {
    enum FilterType { Peak = 0, LowShelf, HighShelf, LowPass, HighPass, Notch, BandPass };

    bool enabled     = true;
    FilterType type  = Peak;
    float frequency  = 1000.0f;   // Hz
    float gainDb     = 0.0f;      // dB (-24 to +24)
    float q          = 1.0f;      // Quality factor (0.1 to 30)
};

// 20-band parametric EQ processor (REW-style)
class EqualizerProcessor : public IDSPProcessor {
public:
    static constexpr int MAX_BANDS = 20;
    static constexpr int MAX_CHANNELS = 24;

    EqualizerProcessor();

    void process(float* buf, int frames, int channels) override;

    std::string getName() const override { return "Parametric EQ"; }

    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool enabled) override { m_enabled = enabled; }

    void prepare(double sampleRate, int channels) override;
    void reset() override;

    // Band access
    void setBand(int band, const EQBand& params);
    void setBand(int band, float freqHz, float gainDb, float q);
    EQBand getBand(int band) const;
    int activeBands() const { return m_activeBands; }
    void setActiveBands(int count);

    // Frequency response for graph visualization (returns dB values)
    std::vector<double> getFrequencyResponse(int numPoints = 512) const;

    // Parameters interface
    std::vector<DSPParameter> getParameters() const override;
    void setParameter(int index, float value) override;
    float getParameter(int index) const override;

private:
    void recalcCoeffs(int band);
    static BiquadCoeffs calcBiquad(double sampleRate, const EQBand& band);

    bool m_enabled = true;
    double m_sampleRate = 44100.0;
    int m_channels = 2;
    int m_activeBands = 10;

    std::array<EQBand, MAX_BANDS> m_bands;
    std::array<BiquadCoeffs, MAX_BANDS> m_coeffs;
    // Per-band, per-channel biquad state
    std::array<std::array<BiquadState, MAX_CHANNELS>, MAX_BANDS> m_state;
};
