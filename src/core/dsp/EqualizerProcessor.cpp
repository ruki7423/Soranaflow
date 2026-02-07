#include "EqualizerProcessor.h"
#include <algorithm>
#include <complex>

static constexpr double PI = 3.14159265358979323846;

// ── Constructor ─────────────────────────────────────────────────────
EqualizerProcessor::EqualizerProcessor()
{
    // Default: logarithmically spaced from 20Hz to 20kHz across 20 bands
    for (int i = 0; i < MAX_BANDS; ++i) {
        double logMin = std::log10(20.0);
        double logMax = std::log10(20000.0);
        double logFreq = logMin + (logMax - logMin) * i / 19.0;
        float freq = static_cast<float>(std::pow(10.0, logFreq));
        m_bands[i] = {true, EQBand::Peak, freq, 0.0f, 1.0f};
    }

    for (int i = 0; i < MAX_BANDS; ++i)
        recalcCoeffs(i);
}

// ── prepare ─────────────────────────────────────────────────────────
void EqualizerProcessor::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate;
    m_channels = std::min(channels, MAX_CHANNELS);
    for (int i = 0; i < MAX_BANDS; ++i)
        recalcCoeffs(i);
    reset();
}

// ── reset ───────────────────────────────────────────────────────────
void EqualizerProcessor::reset()
{
    for (auto& bandStates : m_state) {
        for (auto& s : bandStates) {
            s = BiquadState{};
        }
    }
}

// ── process ─────────────────────────────────────────────────────────
void EqualizerProcessor::process(float* buf, int frames, int channels)
{
    if (!m_enabled) return;

    int ch = std::min(channels, MAX_CHANNELS);

    for (int band = 0; band < m_activeBands; ++band) {
        if (!m_bands[band].enabled) continue;
        // Skip peak/shelf bands with 0 dB gain
        if (m_bands[band].type <= EQBand::HighShelf && m_bands[band].gainDb == 0.0f) continue;

        const auto& c = m_coeffs[band];

        for (int frame = 0; frame < frames; ++frame) {
            for (int j = 0; j < ch; ++j) {
                auto& s = m_state[band][j];
                float x = buf[frame * channels + j];

                double y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2
                         - c.a1 * s.y1 - c.a2 * s.y2;

                s.x2 = s.x1;
                s.x1 = x;
                s.y2 = s.y1;
                s.y1 = y;

                buf[frame * channels + j] = static_cast<float>(y);
            }
        }
    }
}

// ── setBand (full) ──────────────────────────────────────────────────
void EqualizerProcessor::setBand(int band, const EQBand& params)
{
    if (band < 0 || band >= MAX_BANDS) return;
    m_bands[band] = params;
    recalcCoeffs(band);
}

// ── setBand (simple: freq, gain, q — legacy compatibility) ─────────
void EqualizerProcessor::setBand(int band, float freqHz, float gainDb, float q)
{
    if (band < 0 || band >= MAX_BANDS) return;
    m_bands[band].frequency = freqHz;
    m_bands[band].gainDb = gainDb;
    m_bands[band].q = q;
    recalcCoeffs(band);
}

// ── getBand ─────────────────────────────────────────────────────────
EQBand EqualizerProcessor::getBand(int band) const
{
    if (band < 0 || band >= MAX_BANDS) return {};
    return m_bands[band];
}

// ── setActiveBands ──────────────────────────────────────────────────
void EqualizerProcessor::setActiveBands(int count)
{
    m_activeBands = std::clamp(count, 1, MAX_BANDS);
}

// ── getFrequencyResponse ────────────────────────────────────────────
std::vector<double> EqualizerProcessor::getFrequencyResponse(int numPoints) const
{
    std::vector<double> response(numPoints, 0.0);

    for (int i = 0; i < numPoints; ++i) {
        // Logarithmic frequency scale from 20Hz to 20kHz
        double logMin = std::log10(20.0);
        double logMax = std::log10(20000.0);
        double freq = std::pow(10.0, logMin + (logMax - logMin) * i / (numPoints - 1));
        double w = 2.0 * PI * freq / m_sampleRate;

        double totalDb = 0.0;

        for (int band = 0; band < m_activeBands; ++band) {
            if (!m_bands[band].enabled) continue;
            if (m_bands[band].type <= EQBand::HighShelf && m_bands[band].gainDb == 0.0f) continue;

            const auto& c = m_coeffs[band];

            // Evaluate H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
            std::complex<double> z = std::exp(std::complex<double>(0, w));
            std::complex<double> z1 = 1.0 / z;
            std::complex<double> z2 = z1 * z1;

            std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
            std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;

            double mag = std::abs(num / den);
            if (mag > 0.0) {
                totalDb += 20.0 * std::log10(mag);
            }
        }

        response[i] = totalDb;
    }

    return response;
}

// ── getParameters ───────────────────────────────────────────────────
std::vector<DSPParameter> EqualizerProcessor::getParameters() const
{
    std::vector<DSPParameter> params;
    for (int i = 0; i < m_activeBands; ++i) {
        std::string prefix = "Band " + std::to_string(i + 1) + " ";
        params.push_back({prefix + "Freq", m_bands[i].frequency, 20.0f, 20000.0f, m_bands[i].frequency, "Hz"});
        params.push_back({prefix + "Gain", m_bands[i].gainDb, -24.0f, 24.0f, 0.0f, "dB"});
        params.push_back({prefix + "Q", m_bands[i].q, 0.1f, 30.0f, 1.0f, ""});
    }
    return params;
}

// ── setParameter ────────────────────────────────────────────────────
void EqualizerProcessor::setParameter(int index, float value)
{
    int band = index / 3;
    int param = index % 3;
    if (band < 0 || band >= MAX_BANDS) return;

    switch (param) {
    case 0: m_bands[band].frequency = value; break;
    case 1: m_bands[band].gainDb = value; break;
    case 2: m_bands[band].q = value; break;
    }
    recalcCoeffs(band);
}

// ── getParameter ────────────────────────────────────────────────────
float EqualizerProcessor::getParameter(int index) const
{
    int band = index / 3;
    int param = index % 3;
    if (band < 0 || band >= MAX_BANDS) return 0.0f;

    switch (param) {
    case 0: return m_bands[band].frequency;
    case 1: return m_bands[band].gainDb;
    case 2: return m_bands[band].q;
    }
    return 0.0f;
}

// ── recalcCoeffs ────────────────────────────────────────────────────
void EqualizerProcessor::recalcCoeffs(int band)
{
    if (band < 0 || band >= MAX_BANDS) return;
    m_coeffs[band] = calcBiquad(m_sampleRate, m_bands[band]);
}

// ── calcBiquad — Audio EQ Cookbook (Robert Bristow-Johnson) ─────────
BiquadCoeffs EqualizerProcessor::calcBiquad(double sampleRate, const EQBand& band)
{
    double q = band.q;
    double freqHz = band.frequency;
    if (q <= 0.0) q = 0.1;
    if (freqHz <= 0.0) freqHz = 1000.0;

    double A = std::pow(10.0, band.gainDb / 40.0);
    double w0 = 2.0 * PI * freqHz / sampleRate;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha = sinw0 / (2.0 * q);

    double b0, b1, b2, a0, a1, a2;

    switch (band.type) {
    case EQBand::Peak:
    default:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cosw0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cosw0;
        a2 = 1.0 - alpha / A;
        break;

    case EQBand::LowShelf:
        b0 = A * ((A + 1) - (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha);
        b1 = 2 * A * ((A - 1) - (A + 1) * cosw0);
        b2 = A * ((A + 1) - (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha);
        a0 = (A + 1) + (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha;
        a1 = -2 * ((A - 1) + (A + 1) * cosw0);
        a2 = (A + 1) + (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha;
        break;

    case EQBand::HighShelf:
        b0 = A * ((A + 1) + (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha);
        b1 = -2 * A * ((A - 1) + (A + 1) * cosw0);
        b2 = A * ((A + 1) + (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha);
        a0 = (A + 1) - (A - 1) * cosw0 + 2 * std::sqrt(A) * alpha;
        a1 = 2 * ((A - 1) - (A + 1) * cosw0);
        a2 = (A + 1) - (A - 1) * cosw0 - 2 * std::sqrt(A) * alpha;
        break;

    case EQBand::LowPass:
        b0 = (1 - cosw0) / 2;
        b1 = 1 - cosw0;
        b2 = (1 - cosw0) / 2;
        a0 = 1 + alpha;
        a1 = -2 * cosw0;
        a2 = 1 - alpha;
        break;

    case EQBand::HighPass:
        b0 = (1 + cosw0) / 2;
        b1 = -(1 + cosw0);
        b2 = (1 + cosw0) / 2;
        a0 = 1 + alpha;
        a1 = -2 * cosw0;
        a2 = 1 - alpha;
        break;

    case EQBand::Notch:
        b0 = 1;
        b1 = -2 * cosw0;
        b2 = 1;
        a0 = 1 + alpha;
        a1 = -2 * cosw0;
        a2 = 1 - alpha;
        break;

    case EQBand::BandPass:
        b0 = alpha;
        b1 = 0;
        b2 = -alpha;
        a0 = 1 + alpha;
        a1 = -2 * cosw0;
        a2 = 1 - alpha;
        break;
    }

    // Normalize by a0
    BiquadCoeffs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}
