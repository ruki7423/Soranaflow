#include "EqualizerProcessor.h"
#include <algorithm>
#include <complex>
#include <cstring>
#include <thread>
#include <QDebug>

static constexpr double PI = 3.14159265358979323846;

// ── Helper: next power of 2 ≥ n ────────────────────────────────────
static int nextPow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

static int ilog2(int n) {
    int log = 0;
    while ((1 << log) < n) ++log;
    return log;
}

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

    // Sync pending state with active
    m_pendingBands = m_bands;
    m_pendingCoeffs = m_coeffs;
    m_pendingActiveBands = m_activeBands;

#ifdef __APPLE__
    m_fftSetup = vDSP_create_fftsetup(LP_MAX_FFT_LOG2N, kFFTRadix2);
#endif
}

// ── Destructor ──────────────────────────────────────────────────────
EqualizerProcessor::~EqualizerProcessor()
{
#ifdef __APPLE__
    if (m_fftSetup) {
        vDSP_destroy_fftsetup(m_fftSetup);
        m_fftSetup = nullptr;
    }
    if (m_stageFftSetup) {
        vDSP_destroy_fftsetup(m_stageFftSetup);
        m_stageFftSetup = nullptr;
    }
    if (m_biquadSetup) {
        vDSP_biquad_DestroySetup(m_biquadSetup);
        m_biquadSetup = nullptr;
    }
    if (m_stagedBiquadSetup) {
        vDSP_biquad_DestroySetup(m_stagedBiquadSetup);
        m_stagedBiquadSetup = nullptr;
    }
#endif
}

// ── prepare ─────────────────────────────────────────────────────────
// THREAD SAFETY: prepare() writes m_sampleRate, m_channels, resizes vectors.
// It must ONLY be called when the audio render callback is stopped.
// All callers (AudioEngine::load, setSampleRate) stop audio first.
void EqualizerProcessor::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate;
    m_channels = std::min(channels, MAX_CHANNELS);
    for (int i = 0; i < MAX_BANDS; ++i)
        recalcCoeffs(i);

    // Sync pending state
    m_pendingBands = m_bands;
    m_pendingCoeffs = m_coeffs;
    m_pendingActiveBands = m_activeBands;
    m_bandsDirty.store(false, std::memory_order_relaxed);

    // Pre-allocate fade buffers (4096 frames max)
    m_enableFadeBuf.resize(4096 * m_channels);
    m_coeffFadeBuf.resize(4096 * m_channels);

    reset();

#ifdef __APPLE__
    // Pre-allocate biquad delay arrays (max size for any section count)
    for (int c = 0; c < MAX_CHANNELS; ++c)
        m_biquadDelay[c].assign(2 + 2 * MAX_BANDS, 0.0f);
    buildBiquadSetup();

    if (m_phaseMode == LinearPhase) {
        allocateLinearPhaseBuffers();
        m_firDirty.store(true, std::memory_order_relaxed);
        buildFIRKernelStaged();
    }
#endif
}

// ── reset ───────────────────────────────────────────────────────────
void EqualizerProcessor::reset()
{
    for (auto& bandStates : m_state) {
        for (auto& s : bandStates) {
            s = BiquadState{};
        }
    }

#ifdef __APPLE__
    resetOLAState();
#endif
}

// ── process ─────────────────────────────────────────────────────────
void EqualizerProcessor::process(float* buf, int frames, int channels)
{
    // Enable/disable with crossfade to prevent pops
    bool fading = (m_enabled && m_enableFadeMix < 1.0f)
               || (!m_enabled && m_enableFadeMix > 0.0f);
    if (!m_enabled && !fading) return;

    int n = frames * channels;
    bool needBlend = fading && (int)m_enableFadeBuf.size() >= n;
    if (needBlend) {
        std::memcpy(m_enableFadeBuf.data(), buf, n * sizeof(float));
    }

    // Apply pending band parameter updates (thread-safe: UI → audio thread)
    if (m_bandsDirty.load(std::memory_order_acquire)) {
        if (!m_pendingLock.test_and_set(std::memory_order_acquire)) {
            m_bands = m_pendingBands;
            m_coeffs = m_pendingCoeffs;
            m_activeBands = m_pendingActiveBands;
            m_bandsDirty.store(false, std::memory_order_relaxed);
            m_pendingLock.clear(std::memory_order_release);

            // Clear biquad state to prevent old-state + new-coefficients discontinuity.
            // Start a short dry→processed crossfade to mask the filter restart.
            if (m_phaseMode == MinimumPhase) {
                for (auto& bandStates : m_state)
                    for (auto& s : bandStates) s = BiquadState{};
#ifdef __APPLE__
                // Swap staged vDSP biquad setup if available
                if (m_biquadSetupReady.load(std::memory_order_acquire)) {
                    if (m_biquadSetup) vDSP_biquad_DestroySetup(m_biquadSetup);
                    m_biquadSetup = m_stagedBiquadSetup;
                    m_biquadSections = m_stagedBiquadSections;
                    m_stagedBiquadSetup = nullptr;
                    m_biquadSetupReady.store(false, std::memory_order_relaxed);
                    for (int c = 0; c < MAX_CHANNELS; ++c)
                        std::fill(m_biquadDelay[c].begin(), m_biquadDelay[c].end(), 0.0f);
                }
#endif
                if ((int)m_coeffFadeBuf.size() >= n) {
                    std::memcpy(m_coeffFadeBuf.data(), buf, n * sizeof(float));
                    m_coeffFading = true;
                    m_coeffFadePos = 0;
                }
            }
            // Note: LP kernel is already built by UI thread via buildFIRKernelStaged()
            // in the setBand/setActiveBands callers. RT thread swaps via m_stagedKernelReady.
        }
        // If lock held by UI, skip — next callback will pick it up
    }

    // Check for pending phase mode switch (only when not already transitioning)
    if (m_transitionPhase == 0) {
        int pending = m_pendingPhaseMode.exchange(-1, std::memory_order_acquire);
        if (pending >= 0) {
            m_transitionTarget = static_cast<PhaseMode>(pending);
            m_transitionPhase = 1;  // start fade-out
            m_transitionPos = 0;
        }
    }

    // ── Phase 1: Fade-out current mode ──
    if (m_transitionPhase == 1) {
#ifdef __APPLE__
        if (m_phaseMode == LinearPhase && m_firLen > 0)
            processLinearPhase(buf, frames, channels);
        else
#endif
            processMinimumPhase(buf, frames, channels);

        for (int i = 0; i < frames; ++i) {
            float t = static_cast<float>(m_transitionPos + i) / TRANSITION_FADE_LEN;
            float gain = (t >= 1.0f) ? 0.0f : 1.0f - t;
            for (int c = 0; c < channels; ++c)
                buf[i * channels + c] *= gain;
        }
        m_transitionPos += frames;
        if (m_transitionPos >= TRANSITION_FADE_LEN) {
            // === Switch mode + clear ALL state ===
            m_phaseMode = m_transitionTarget;

            for (auto& bandStates : m_state)
                for (auto& s : bandStates) s = BiquadState{};
#ifdef __APPLE__
            resetOLAState();
            // Clear MP coefficient crossfade — prevents stale dry buffer
            // from leaking into LP output (fixes +105 pops at EQ_OUT)
            m_coeffFading = false;
            if (m_phaseMode == LinearPhase)
                m_firDirty.store(true, std::memory_order_relaxed);

            if (m_phaseMode == LinearPhase && m_firLen > 0) {
                int lpLatency = LP_PARTITION_SIZE + m_firLen / 2;
                int partitions = (lpLatency + LP_PARTITION_SIZE - 1) / LP_PARTITION_SIZE;
                m_warmupDuration = (partitions + 1) * LP_PARTITION_SIZE + TRANSITION_FADE_LEN;
            } else {
                m_warmupDuration = 2 * TRANSITION_FADE_LEN;
            }
#else
            m_warmupDuration = 2 * TRANSITION_FADE_LEN;
#endif
            m_transitionPhase = 2;
            m_transitionPos = 0;
        }
    }

    // ── Phase 2: Mute while new mode warms up, then fade-in ──
    else if (m_transitionPhase == 2) {
#ifdef __APPLE__
        if (m_phaseMode == LinearPhase && m_firLen > 0)
            processLinearPhase(buf, frames, channels);
        else
#endif
            processMinimumPhase(buf, frames, channels);

        int silentEnd = m_warmupDuration - TRANSITION_FADE_LEN;
        for (int i = 0; i < frames; ++i) {
            int pos = m_transitionPos + i;
            float gain;
            if (pos < silentEnd) {
                gain = 0.0f;
            } else {
                float t = static_cast<float>(pos - silentEnd) / TRANSITION_FADE_LEN;
                gain = (t >= 1.0f) ? 1.0f : t;
            }
            for (int c = 0; c < channels; ++c)
                buf[i * channels + c] *= gain;
        }
        m_transitionPos += frames;
        if (m_transitionPos >= m_warmupDuration)
            m_transitionPhase = 0;
    }

    // ── Normal processing ──
    else {
#ifdef __APPLE__
        if (m_phaseMode == LinearPhase && m_firLen > 0)
            processLinearPhase(buf, frames, channels);
        else
#endif
            processMinimumPhase(buf, frames, channels);
    }

    // ── Coefficient crossfade (dry→processed after preset change) ──
    if (m_coeffFading && (int)m_coeffFadeBuf.size() >= n) {
        for (int f = 0; f < frames; ++f) {
            float t = (float)(m_coeffFadePos + f) / (float)COEFF_FADE_LEN;
            if (t > 1.0f) t = 1.0f;
            float dry = 1.0f - t;
            for (int c = 0; c < channels; ++c) {
                int idx = f * channels + c;
                buf[idx] = m_coeffFadeBuf[idx] * dry + buf[idx] * t;
            }
        }
        m_coeffFadePos += frames;
        if (m_coeffFadePos >= COEFF_FADE_LEN)
            m_coeffFading = false;
    }

    // ── Enable/disable crossfade ──
    if (needBlend) {
        constexpr float step = 1.0f / 256.0f;
        float dir = m_enabled ? step : -step;
        for (int f = 0; f < frames; ++f) {
            m_enableFadeMix += dir;
            if (m_enableFadeMix < 0.0f) m_enableFadeMix = 0.0f;
            if (m_enableFadeMix > 1.0f) m_enableFadeMix = 1.0f;
            float dry = 1.0f - m_enableFadeMix;
            for (int c = 0; c < channels; ++c) {
                int idx = f * channels + c;
                buf[idx] = m_enableFadeBuf[idx] * dry + buf[idx] * m_enableFadeMix;
            }
        }
    } else if (m_enabled && m_enableFadeMix < 1.0f) {
        m_enableFadeMix = 1.0f;  // snap to fully on
    }

}

// ── processMinimumPhase ─────────────────────────────────────────────
void EqualizerProcessor::processMinimumPhase(float* buf, int frames, int channels)
{
    int ch = std::min(channels, MAX_CHANNELS);

#ifdef __APPLE__
    // Fast path: vDSP_biquad with all active bands cascaded per channel
    if (m_biquadSetup && m_biquadSections > 0) {
        for (int c = 0; c < ch; ++c) {
            vDSP_biquad(m_biquadSetup, m_biquadDelay[c].data(),
                        buf + c, channels,    // input with stride
                        buf + c, channels,    // output with stride (in-place)
                        frames);
        }
        return;
    }
#endif

    // Fallback: manual double-precision biquad
    for (int band = 0; band < m_activeBands; ++band) {
        if (!m_bands[band].enabled) continue;
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

    // Write to pending state under lock (audio thread copies to active)
    while (m_pendingLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    m_pendingBands[band] = params;
    m_pendingCoeffs[band] = calcBiquad(m_sampleRate, params);
    m_pendingLock.clear(std::memory_order_release);
    m_bandsDirty.store(true, std::memory_order_release);

#ifdef __APPLE__
    if (!m_deferKernelBuild) {
        if (m_phaseMode == LinearPhase && m_firLen > 0)
            buildFIRKernelStaged();
        else if (m_phaseMode == MinimumPhase)
            buildBiquadSetup();
    }
#endif
}

// ── setBand (simple: freq, gain, q — legacy compatibility) ─────────
void EqualizerProcessor::setBand(int band, float freqHz, float gainDb, float q)
{
    if (band < 0 || band >= MAX_BANDS) return;

    while (m_pendingLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    m_pendingBands[band].frequency = freqHz;
    m_pendingBands[band].gainDb = gainDb;
    m_pendingBands[band].q = q;
    m_pendingCoeffs[band] = calcBiquad(m_sampleRate, m_pendingBands[band]);
    m_pendingLock.clear(std::memory_order_release);
    m_bandsDirty.store(true, std::memory_order_release);

#ifdef __APPLE__
    if (!m_deferKernelBuild) {
        if (m_phaseMode == LinearPhase && m_firLen > 0)
            buildFIRKernelStaged();
        else if (m_phaseMode == MinimumPhase)
            buildBiquadSetup();
    }
#endif
}

// ── getBand ─────────────────────────────────────────────────────────
EQBand EqualizerProcessor::getBand(int band) const
{
    if (band < 0 || band >= MAX_BANDS) return {};
    return m_pendingBands[band];
}

// ── setActiveBands ──────────────────────────────────────────────────
void EqualizerProcessor::setActiveBands(int count)
{
    while (m_pendingLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    m_pendingActiveBands = std::clamp(count, 1, MAX_BANDS);
    m_pendingLock.clear(std::memory_order_release);
    m_bandsDirty.store(true, std::memory_order_release);

#ifdef __APPLE__
    if (!m_deferKernelBuild) {
        if (m_phaseMode == LinearPhase && m_firLen > 0)
            buildFIRKernelStaged();
        else if (m_phaseMode == MinimumPhase)
            buildBiquadSetup();
    }
#endif
}

// ── beginBatchUpdate / endBatchUpdate ────────────────────────────────
void EqualizerProcessor::beginBatchUpdate()
{
    m_deferKernelBuild = true;
}

void EqualizerProcessor::endBatchUpdate()
{
    m_deferKernelBuild = false;
#ifdef __APPLE__
    if (m_phaseMode == LinearPhase && m_firLen > 0)
        buildFIRKernelStaged();
    else if (m_phaseMode == MinimumPhase)
        buildBiquadSetup();
#endif
}

// ── setPhaseMode ────────────────────────────────────────────────────
void EqualizerProcessor::setPhaseMode(PhaseMode mode)
{
    if (m_phaseMode == mode) return;

#ifdef __APPLE__
    // Allocate LP buffers BEFORE signalling the RT thread so it sees ready buffers.
    if (mode == LinearPhase) {
        allocateLinearPhaseBuffers();
        buildFIRKernelStaged();
    }
#endif

    // Defer the actual switch to the audio thread via atomic pending flag.
    // process() will fade-out → switch → fade-in to avoid clicks.
    m_pendingPhaseMode.store(static_cast<int>(mode), std::memory_order_release);
}

// ── latencySamples ──────────────────────────────────────────────────
int EqualizerProcessor::latencySamples() const
{
#ifdef __APPLE__
    if (m_phaseMode == LinearPhase && m_firLen > 0)
        return LP_PARTITION_SIZE + m_firLen / 2;
#endif
    return 0;
}

// ── getFrequencyResponse ────────────────────────────────────────────
std::vector<double> EqualizerProcessor::getFrequencyResponse(int numPoints) const
{
    std::vector<double> response(numPoints, 0.0);
    if (numPoints < 2) return response;

    for (int i = 0; i < numPoints; ++i) {
        // Logarithmic frequency scale from 20Hz to 20kHz
        double logMin = std::log10(20.0);
        double logMax = std::log10(20000.0);
        double freq = std::pow(10.0, logMin + (logMax - logMin) * i / (numPoints - 1));
        double w = 2.0 * PI * freq / m_sampleRate;

        double totalDb = 0.0;

        for (int band = 0; band < m_pendingActiveBands; ++band) {
            if (!m_pendingBands[band].enabled) continue;
            if (m_pendingBands[band].type <= EQBand::HighShelf && m_pendingBands[band].gainDb == 0.0f) continue;

            const auto& c = m_pendingCoeffs[band];

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
    int bands = m_pendingActiveBands;
    for (int i = 0; i < bands; ++i) {
        std::string prefix = "Band " + std::to_string(i + 1) + " ";
        params.push_back({prefix + "Freq", m_pendingBands[i].frequency, 20.0f, 20000.0f, m_pendingBands[i].frequency, "Hz"});
        params.push_back({prefix + "Gain", m_pendingBands[i].gainDb, -24.0f, 24.0f, 0.0f, "dB"});
        params.push_back({prefix + "Q", m_pendingBands[i].q, 0.1f, 30.0f, 1.0f, ""});
    }
    return params;
}

// ── setParameter ────────────────────────────────────────────────────
void EqualizerProcessor::setParameter(int index, float value)
{
    int band = index / 3;
    int param = index % 3;
    if (band < 0 || band >= MAX_BANDS) return;

    while (m_pendingLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    switch (param) {
    case 0: m_pendingBands[band].frequency = value; break;
    case 1: m_pendingBands[band].gainDb = value; break;
    case 2: m_pendingBands[band].q = value; break;
    }
    m_pendingCoeffs[band] = calcBiquad(m_sampleRate, m_pendingBands[band]);
    m_pendingLock.clear(std::memory_order_release);
    m_bandsDirty.store(true, std::memory_order_release);

#ifdef __APPLE__
    if (!m_deferKernelBuild) {
        if (m_phaseMode == LinearPhase && m_firLen > 0)
            buildFIRKernelStaged();
        else if (m_phaseMode == MinimumPhase)
            buildBiquadSetup();
    }
#endif
}

// ── getParameter ────────────────────────────────────────────────────
float EqualizerProcessor::getParameter(int index) const
{
    int band = index / 3;
    int param = index % 3;
    if (band < 0 || band >= MAX_BANDS) return 0.0f;

    switch (param) {
    case 0: return m_pendingBands[band].frequency;
    case 1: return m_pendingBands[band].gainDb;
    case 2: return m_pendingBands[band].q;
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
    if (sampleRate <= 0.0) sampleRate = 44100.0;
    double q = band.q;
    double freqHz = band.frequency;
    double gainDb = std::clamp(static_cast<double>(band.gainDb), -30.0, 30.0);
    if (q <= 0.0) q = 0.1;
    if (freqHz <= 0.0) freqHz = 1000.0;
    if (freqHz > sampleRate * 0.49) freqHz = sampleRate * 0.49;

    double A = std::pow(10.0, gainDb / 40.0);
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

// ═════════════════════════════════════════════════════════════════════
//  Linear Phase EQ — vDSP/Accelerate overlap-add implementation
// ═════════════════════════════════════════════════════════════════════

#ifdef __APPLE__

// ── allocateLinearPhaseBuffers ──────────────────────────────────────
void EqualizerProcessor::allocateLinearPhaseBuffers()
{
    if (m_sampleRate <= 0.0) return;

    // FIR length by sample rate
    if (m_sampleRate <= 50000.0)
        m_firLen = 4096;
    else if (m_sampleRate <= 100000.0)
        m_firLen = 8192;
    else
        m_firLen = 16384;

    // Kernel build FFT: next power of 2 >= firLen (for magnitude sampling)
    m_firBuildFftSize = nextPow2(m_firLen);
    m_firBuildFftHalf = m_firBuildFftSize / 2;
    m_firBuildFftLog2n = ilog2(m_firBuildFftSize);

    // Number of kernel partitions: ceil(firLen / LP_PARTITION_SIZE)
    m_numKernelPartitions = (m_firLen + LP_PARTITION_SIZE - 1) / LP_PARTITION_SIZE;

    // Double-buffered OLA instances
    for (int s = 0; s < 2; ++s) {
        auto& inst = m_olaSlots[s];
        inst.channels.resize(MAX_CHANNELS);
        for (auto& ch : inst.channels) {
            ch.inputBuf.assign(LP_PARTITION_SIZE, 0.0f);
            ch.overlapBuf.assign(LP_PARTITION_SIZE, 0.0f);
            ch.outputBuf.assign(LP_PARTITION_SIZE, 0.0f);
            ch.fdlReals.resize(m_numKernelPartitions);
            ch.fdlImags.resize(m_numKernelPartitions);
            ch.fdl.resize(m_numKernelPartitions);
            for (int p = 0; p < m_numKernelPartitions; ++p) {
                ch.fdlReals[p].assign(CONV_FFT_HALF, 0.0f);
                ch.fdlImags[p].assign(CONV_FFT_HALF, 0.0f);
                ch.fdl[p].realp = ch.fdlReals[p].data();
                ch.fdl[p].imagp = ch.fdlImags[p].data();
            }
        }
        inst.kernReals.resize(m_numKernelPartitions);
        inst.kernImags.resize(m_numKernelPartitions);
        inst.kernParts.resize(m_numKernelPartitions);
        for (int p = 0; p < m_numKernelPartitions; ++p) {
            inst.kernReals[p].assign(CONV_FFT_HALF, 0.0f);
            inst.kernImags[p].assign(CONV_FFT_HALF, 0.0f);
            inst.kernParts[p].realp = inst.kernReals[p].data();
            inst.kernParts[p].imagp = inst.kernImags[p].data();
        }
        inst.phase = 0;
        inst.fdlIdx = 0;
        inst.hasOutput = false;
        inst.partitionsProcessed = 0;
    }
    m_curSlot = 0;
    m_nextSlot = -1;
    m_crossfading = false;
    m_xfadePos = 0;

    // FFT scratch buffers (CONV_FFT_SIZE = 2048)
    m_lpFftInBuf.assign(CONV_FFT_SIZE, 0.0f);
    m_lpSplitReal.assign(CONV_FFT_HALF, 0.0f);
    m_lpSplitImag.assign(CONV_FFT_HALF, 0.0f);
    m_lpFftSplit.realp = m_lpSplitReal.data();
    m_lpFftSplit.imagp = m_lpSplitImag.data();

    m_lpAccumReal.assign(CONV_FFT_HALF, 0.0f);
    m_lpAccumImag.assign(CONV_FFT_HALF, 0.0f);
    m_lpAccumSplit.realp = m_lpAccumReal.data();
    m_lpAccumSplit.imagp = m_lpAccumImag.data();

    m_lpIfftOut.assign(CONV_FFT_SIZE, 0.0f);

    // Kernel build scratch (uses m_firBuildFftSize)
    m_lpKernelBuildBuf.assign(m_firBuildFftSize, 0.0f);

    // Pre-allocate RT scratch buffers
    m_lpMagBins.assign(m_firBuildFftHalf + 1, 0.0f);
    m_lpSpecReal.assign(m_firBuildFftHalf, 0.0f);
    m_lpSpecImag.assign(m_firBuildFftHalf, 0.0f);
    m_lpKernelTimeBuf.assign(m_firLen, 0.0f);

    // Staged kernel buffers (UI thread builds here, RT thread swaps from here)
    m_stagedPartReals.resize(m_numKernelPartitions);
    m_stagedPartImags.resize(m_numKernelPartitions);
    for (int p = 0; p < m_numKernelPartitions; ++p) {
        m_stagedPartReals[p].assign(CONV_FFT_HALF, 0.0f);
        m_stagedPartImags[p].assign(CONV_FFT_HALF, 0.0f);
    }
    m_stagedKernelReady.store(false, std::memory_order_relaxed);
    m_stagedLock.clear();

    // UI-thread scratch buffers
    m_stageMagBins.assign(m_firBuildFftHalf + 1, 0.0f);
    m_stageSpecReal.assign(m_firBuildFftHalf, 0.0f);
    m_stageSpecImag.assign(m_firBuildFftHalf, 0.0f);
    m_stageKernelBuildBuf.assign(m_firBuildFftSize, 0.0f);
    m_stageKernelTimeBuf.assign(m_firLen, 0.0f);
    m_stageFftInBuf.assign(CONV_FFT_SIZE, 0.0f);
    if (!m_stageFftSetup)
        m_stageFftSetup = vDSP_create_fftsetup(LP_MAX_FFT_LOG2N, kFFTRadix2);

    // Double-buffer scratch (max 4096 frames * MAX_CHANNELS)
    m_dryBuf.assign(4096 * MAX_CHANNELS, 0.0f);
    m_nextBuf.assign(4096 * MAX_CHANNELS, 0.0f);
}

// ── buildFIRKernelStaged ─────────────────────────────────────────────
// Called from UI thread. Computes zero-phase FIR kernel from pending
// biquad coefficients and writes into staged buffers for RT-safe swap.
void EqualizerProcessor::buildFIRKernelStaged()
{
    if (!m_stageFftSetup || m_firBuildFftSize <= 0) return;

    const int buildHalf = m_firBuildFftHalf;
    const int buildSize = m_firBuildFftSize;

    // Snapshot pending params under m_pendingLock
    std::array<EQBand, MAX_BANDS> snapBands;
    std::array<BiquadCoeffs, MAX_BANDS> snapCoeffs;
    int snapActive;
    while (m_pendingLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    snapBands = m_pendingBands;
    snapCoeffs = m_pendingCoeffs;
    snapActive = m_pendingActiveBands;
    m_pendingLock.clear(std::memory_order_release);

    // ── Step 1: Compute combined magnitude at each frequency bin ────
    std::fill(m_stageMagBins.begin(), m_stageMagBins.end(), 1.0f);

    for (int k = 0; k <= buildHalf; ++k) {
        double w = 2.0 * PI * k / buildSize;
        double combinedMag = 1.0;

        for (int band = 0; band < snapActive; ++band) {
            if (!snapBands[band].enabled) continue;
            if (snapBands[band].type <= EQBand::HighShelf && snapBands[band].gainDb == 0.0f) continue;

            const auto& c = snapCoeffs[band];

            std::complex<double> z = std::exp(std::complex<double>(0, w));
            std::complex<double> z1 = 1.0 / z;
            std::complex<double> z2 = z1 * z1;

            std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
            std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;

            double mag = std::abs(num / den);
            combinedMag *= mag;
        }

        m_stageMagBins[k] = static_cast<float>(combinedMag);
    }

    // ── Step 2: Pack magnitude into vDSP split-complex (zero phase) ─
    std::memset(m_stageSpecImag.data(), 0, buildHalf * sizeof(float));

    m_stageSpecReal[0] = m_stageMagBins[0];           // DC
    m_stageSpecImag[0] = m_stageMagBins[buildHalf];   // Nyquist
    for (int k = 1; k < buildHalf; ++k) {
        m_stageSpecReal[k] = m_stageMagBins[k];
    }

    DSPSplitComplex specSplit;
    specSplit.realp = m_stageSpecReal.data();
    specSplit.imagp = m_stageSpecImag.data();

    // ── Step 3: Inverse FFT → zero-phase impulse response ───────────
    vDSP_fft_zrip(m_stageFftSetup, &specSplit, 1, m_firBuildFftLog2n, kFFTDirection_Inverse);

    vDSP_ztoc(&specSplit, 1,
              reinterpret_cast<DSPComplex*>(m_stageKernelBuildBuf.data()), 2, buildHalf);

    float ifftScale = 1.0f / static_cast<float>(buildSize);
    vDSP_vsmul(m_stageKernelBuildBuf.data(), 1, &ifftScale,
               m_stageKernelBuildBuf.data(), 1, buildSize);

    // ── Step 4: Circular shift to center — make causal ──────────────
    int halfFir = m_firLen / 2;
    std::memset(m_stageKernelTimeBuf.data(), 0, m_firLen * sizeof(float));

    for (int i = 0; i < halfFir; ++i) {
        int srcIdx = buildSize - halfFir + i;
        if (srcIdx >= 0 && srcIdx < buildSize)
            m_stageKernelTimeBuf[i] = m_stageKernelBuildBuf[srcIdx];
    }
    for (int i = 0; i < halfFir; ++i) {
        if (i < buildSize)
            m_stageKernelTimeBuf[halfFir + i] = m_stageKernelBuildBuf[i];
    }

    // ── Step 5: Apply Blackman-Harris window ────────────────────────
    for (int n = 0; n < m_firLen; ++n) {
        double t = static_cast<double>(n) / (m_firLen - 1);
        double wn = 0.35875
                 - 0.48829 * std::cos(2.0 * PI * t)
                 + 0.14128 * std::cos(4.0 * PI * t)
                 - 0.01168 * std::cos(6.0 * PI * t);
        m_stageKernelTimeBuf[n] *= static_cast<float>(wn);
    }

    // ── Step 6: Partition, zero-pad, FFT — into staged buffers under lock ─
    // Build partitioned FFT data into local temporaries first
    std::vector<std::vector<float>> tmpReals(m_numKernelPartitions);
    std::vector<std::vector<float>> tmpImags(m_numKernelPartitions);

    for (int p = 0; p < m_numKernelPartitions; ++p) {
        tmpReals[p].resize(CONV_FFT_HALF);
        tmpImags[p].resize(CONV_FFT_HALF);

        int srcOffset = p * LP_PARTITION_SIZE;
        int copyLen = std::min(LP_PARTITION_SIZE, m_firLen - srcOffset);
        if (copyLen <= 0) {
            std::memset(tmpReals[p].data(), 0, CONV_FFT_HALF * sizeof(float));
            std::memset(tmpImags[p].data(), 0, CONV_FFT_HALF * sizeof(float));
            continue;
        }

        // Zero-pad partition to CONV_FFT_SIZE in scratch buffer
        std::memset(m_stageFftInBuf.data(), 0, CONV_FFT_SIZE * sizeof(float));
        std::memcpy(m_stageFftInBuf.data(), m_stageKernelTimeBuf.data() + srcOffset,
                    copyLen * sizeof(float));

        // Pack and forward FFT into tmp split
        DSPSplitComplex tmpSplit;
        tmpSplit.realp = tmpReals[p].data();
        tmpSplit.imagp = tmpImags[p].data();

        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_stageFftInBuf.data()), 2,
                  &tmpSplit, 1, CONV_FFT_HALF);
        vDSP_fft_zrip(m_stageFftSetup, &tmpSplit, 1, CONV_FFT_LOG2N,
                      kFFTDirection_Forward);
    }

    // Acquire staged lock, copy results, set ready flag
    while (m_stagedLock.test_and_set(std::memory_order_acquire)) {}
    for (int p = 0; p < m_numKernelPartitions; ++p) {
        std::memcpy(m_stagedPartReals[p].data(), tmpReals[p].data(),
                   CONV_FFT_HALF * sizeof(float));
        std::memcpy(m_stagedPartImags[p].data(), tmpImags[p].data(),
                   CONV_FFT_HALF * sizeof(float));
    }
    m_stagedKernelReady.store(true, std::memory_order_release);
    m_stagedLock.clear(std::memory_order_release);
}

// ── resetOLAState ─────────────────────────────────────────────────────
// Clears both double-buffered OLA instances: FDL, overlap, output, phase.
void EqualizerProcessor::resetOLAState()
{
    for (int s = 0; s < 2; ++s) {
        auto& inst = m_olaSlots[s];
        for (auto& ch : inst.channels) {
            if (!ch.inputBuf.empty())
                std::memset(ch.inputBuf.data(), 0, ch.inputBuf.size() * sizeof(float));
            if (!ch.overlapBuf.empty())
                std::memset(ch.overlapBuf.data(), 0, ch.overlapBuf.size() * sizeof(float));
            if (!ch.outputBuf.empty())
                std::memset(ch.outputBuf.data(), 0, ch.outputBuf.size() * sizeof(float));
            for (auto& v : ch.fdlReals)
                std::memset(v.data(), 0, v.size() * sizeof(float));
            for (auto& v : ch.fdlImags)
                std::memset(v.data(), 0, v.size() * sizeof(float));
        }
        inst.phase = 0;
        inst.fdlIdx = 0;
        inst.hasOutput = false;
        inst.partitionsProcessed = 0;
    }
    m_curSlot = 0;
    m_nextSlot = -1;
    m_crossfading = false;
    m_xfadePos = 0;
}

// ── processLinearPhase ──────────────────────────────────────────────
// Double-buffered partitioned convolution with seamless kernel crossfade.
// When a new kernel is staged, the alternate OLA instance warms up while
// the current one continues playing, then crossfades to eliminate dropouts.
void EqualizerProcessor::processLinearPhase(float* buf, int frames, int channels)
{
    if (!m_fftSetup || m_firLen <= 0) return;

    auto& cur = m_olaSlots[m_curSlot];

    // ── Staged kernel swap check (RT-safe: try-once, no blocking) ────
    if (m_stagedKernelReady.load(std::memory_order_acquire)) {
        if (!m_stagedLock.test_and_set(std::memory_order_acquire)) {
            if (!cur.hasOutput) {
                // First build — copy kernel directly into current slot
                for (int p = 0; p < m_numKernelPartitions; ++p) {
                    std::memcpy(cur.kernReals[p].data(), m_stagedPartReals[p].data(),
                               CONV_FFT_HALF * sizeof(float));
                    std::memcpy(cur.kernImags[p].data(), m_stagedPartImags[p].data(),
                               CONV_FFT_HALF * sizeof(float));
                    cur.kernParts[p].realp = cur.kernReals[p].data();
                    cur.kernParts[p].imagp = cur.kernImags[p].data();
                }
                m_stagedKernelReady.store(false, std::memory_order_relaxed);
                m_firDirty.store(false, std::memory_order_relaxed);
            } else if (m_nextSlot < 0) {
                // Start warmup on alternate slot
                int alt = 1 - m_curSlot;
                auto& next = m_olaSlots[alt];
                // Reset alternate slot OLA state
                for (auto& ch : next.channels) {
                    std::memset(ch.inputBuf.data(), 0, ch.inputBuf.size() * sizeof(float));
                    std::memset(ch.overlapBuf.data(), 0, ch.overlapBuf.size() * sizeof(float));
                    std::memset(ch.outputBuf.data(), 0, ch.outputBuf.size() * sizeof(float));
                    for (auto& v : ch.fdlReals) std::memset(v.data(), 0, v.size() * sizeof(float));
                    for (auto& v : ch.fdlImags) std::memset(v.data(), 0, v.size() * sizeof(float));
                }
                next.phase = 0;
                next.fdlIdx = 0;
                next.hasOutput = false;
                next.partitionsProcessed = 0;
                // Copy staged kernel
                for (int p = 0; p < m_numKernelPartitions; ++p) {
                    std::memcpy(next.kernReals[p].data(), m_stagedPartReals[p].data(),
                               CONV_FFT_HALF * sizeof(float));
                    std::memcpy(next.kernImags[p].data(), m_stagedPartImags[p].data(),
                               CONV_FFT_HALF * sizeof(float));
                    next.kernParts[p].realp = next.kernReals[p].data();
                    next.kernParts[p].imagp = next.kernImags[p].data();
                }
                m_nextSlot = alt;
                m_crossfading = false;
                m_xfadePos = 0;
                m_stagedKernelReady.store(false, std::memory_order_relaxed);
                m_firDirty.store(false, std::memory_order_relaxed);
            } else {
                // Already warming up — update next slot's kernel, restart warmup
                auto& next = m_olaSlots[m_nextSlot];
                for (int p = 0; p < m_numKernelPartitions; ++p) {
                    std::memcpy(next.kernReals[p].data(), m_stagedPartReals[p].data(),
                               CONV_FFT_HALF * sizeof(float));
                    std::memcpy(next.kernImags[p].data(), m_stagedPartImags[p].data(),
                               CONV_FFT_HALF * sizeof(float));
                }
                // Reset FDL and overlap (kernel changed, need fresh warmup)
                for (auto& ch : next.channels) {
                    std::memset(ch.overlapBuf.data(), 0, ch.overlapBuf.size() * sizeof(float));
                    for (auto& v : ch.fdlReals) std::memset(v.data(), 0, v.size() * sizeof(float));
                    for (auto& v : ch.fdlImags) std::memset(v.data(), 0, v.size() * sizeof(float));
                }
                next.hasOutput = false;
                next.partitionsProcessed = 0;
                m_crossfading = false;
                m_xfadePos = 0;
                m_stagedKernelReady.store(false, std::memory_order_relaxed);
                m_firDirty.store(false, std::memory_order_relaxed);
            }
            m_stagedLock.clear(std::memory_order_release);
        }
        // If lock failed: UI is building staged, try next callback
    }

    // ── Process OLA instances ────────────────────────────────────────
    int n = frames * channels;
    bool dualProcess = (m_nextSlot >= 0) && (n <= static_cast<int>(m_dryBuf.size()));

    if (dualProcess) {
        // Save input for second instance
        std::memcpy(m_dryBuf.data(), buf, n * sizeof(float));

        // Process current slot (in-place: reads buf, writes buf)
        processOLAInstance(cur, buf, buf, frames, channels);

        // Process next slot from saved input
        auto& next = m_olaSlots[m_nextSlot];
        processOLAInstance(next, m_dryBuf.data(), m_nextBuf.data(), frames, channels);

        // Start crossfade when next slot produces output
        if (next.hasOutput && !m_crossfading) {
            m_crossfading = true;
            m_xfadePos = 0;
        }

        if (m_crossfading) {
            int ch = std::min(channels, MAX_CHANNELS);
            for (int i = 0; i < frames; ++i) {
                float t = static_cast<float>(m_xfadePos + i)
                        / static_cast<float>(LP_TRANS_FADE_LEN);
                if (t > 1.0f) t = 1.0f;
                float gOld = std::sqrtf(1.0f - t);
                float gNew = std::sqrtf(t);
                for (int c = 0; c < ch; ++c) {
                    int idx = i * channels + c;
                    buf[idx] = buf[idx] * gOld + m_nextBuf[idx] * gNew;
                }
            }
            m_xfadePos += frames;
            if (m_xfadePos >= LP_TRANS_FADE_LEN) {
                // Crossfade complete — switch active slot
                m_curSlot = m_nextSlot;
                m_nextSlot = -1;
                m_crossfading = false;
                m_xfadePos = 0;
            }
        }
    } else {
        // Single instance processing (normal steady-state)
        processOLAInstance(cur, buf, buf, frames, channels);
    }
}

// ── processOLAInstance ──────────────────────────────────────────────
// Core partitioned convolution for a single OLA instance.
// Reads interleaved input from inBuf, writes interleaved output to outBuf.
// Uses vDSP_zvma for SIMD complex multiply-accumulate (TASK 3).
void EqualizerProcessor::processOLAInstance(OLAInstance& inst,
                                            const float* inBuf, float* outBuf,
                                            int frames, int channels)
{
    int ch = std::min(channels, MAX_CHANNELS);
    if (ch < 1 || inst.channels.empty()) return;

    int pos = 0;

    while (pos < frames) {
        int avail = std::min(frames - pos, LP_PARTITION_SIZE - inst.phase);

        // Deinterleave input, write previous output
        for (int i = 0; i < avail; ++i) {
            int baseIdx = (pos + i) * channels;

            for (int c = 0; c < ch; ++c)
                inst.channels[c].inputBuf[inst.phase + i] = inBuf[baseIdx + c];

            if (inst.hasOutput) {
                for (int c = 0; c < ch; ++c)
                    outBuf[baseIdx + c] = inst.channels[c].outputBuf[inst.phase + i];
            } else {
                for (int c = 0; c < ch; ++c)
                    outBuf[baseIdx + c] = 0.0f;
            }
        }

        inst.phase += avail;
        pos += avail;

        // When partition is full, convolve
        if (inst.phase >= LP_PARTITION_SIZE) {
            for (int c = 0; c < ch; ++c) {
                auto& ola = inst.channels[c];

                // Zero-pad input to CONV_FFT_SIZE: [input | zeros]
                std::memcpy(m_lpFftInBuf.data(), ola.inputBuf.data(),
                           LP_PARTITION_SIZE * sizeof(float));
                std::memset(m_lpFftInBuf.data() + LP_PARTITION_SIZE, 0,
                           LP_PARTITION_SIZE * sizeof(float));

                // Pack and forward FFT
                vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_lpFftInBuf.data()), 2,
                          &m_lpFftSplit, 1, CONV_FFT_HALF);
                vDSP_fft_zrip(m_fftSetup, &m_lpFftSplit, 1, CONV_FFT_LOG2N,
                              kFFTDirection_Forward);

                // Store in FDL
                std::memcpy(ola.fdl[inst.fdlIdx].realp, m_lpFftSplit.realp,
                           CONV_FFT_HALF * sizeof(float));
                std::memcpy(ola.fdl[inst.fdlIdx].imagp, m_lpFftSplit.imagp,
                           CONV_FFT_HALF * sizeof(float));

                // Clear accumulator
                std::memset(m_lpAccumReal.data(), 0, CONV_FFT_HALF * sizeof(float));
                std::memset(m_lpAccumImag.data(), 0, CONV_FFT_HALF * sizeof(float));

                // Accumulate: sum over all partitions of FDL[k] * kernel[k]
                for (int p = 0; p < m_numKernelPartitions; ++p) {
                    int fdlSlot = (inst.fdlIdx - p + m_numKernelPartitions) % m_numKernelPartitions;

                    const float* ar = ola.fdl[fdlSlot].realp;
                    const float* ai = ola.fdl[fdlSlot].imagp;
                    const float* br = inst.kernParts[p].realp;
                    const float* bi = inst.kernParts[p].imagp;

                    // Bin 0: DC and Nyquist (packed separately in vDSP format)
                    m_lpAccumReal[0] += ar[0] * br[0];
                    m_lpAccumImag[0] += ai[0] * bi[0];

                    // Bins 1..CONV_FFT_HALF-1: vDSP complex multiply-accumulate
                    DSPSplitComplex zvA = { const_cast<float*>(ar + 1), const_cast<float*>(ai + 1) };
                    DSPSplitComplex zvB = { const_cast<float*>(br + 1), const_cast<float*>(bi + 1) };
                    DSPSplitComplex zvC = { m_lpAccumReal.data() + 1, m_lpAccumImag.data() + 1 };
                    vDSP_zvma(&zvA, 1, &zvB, 1, &zvC, 1, &zvC, 1, CONV_FFT_HALF - 1);
                }

                // Inverse FFT
                vDSP_fft_zrip(m_fftSetup, &m_lpAccumSplit, 1, CONV_FFT_LOG2N,
                              kFFTDirection_Inverse);

                // Unpack to real
                vDSP_ztoc(&m_lpAccumSplit, 1,
                          reinterpret_cast<DSPComplex*>(m_lpIfftOut.data()), 2, CONV_FFT_HALF);

                // Scale: 1/(CONV_FFT_SIZE * 4) — same as ConvolutionProcessor
                float scale = 1.0f / static_cast<float>(CONV_FFT_SIZE * 4);
                vDSP_vsmul(m_lpIfftOut.data(), 1, &scale, m_lpIfftOut.data(), 1, CONV_FFT_SIZE);

                // Overlap-add: first half + previous overlap → output
                vDSP_vadd(m_lpIfftOut.data(), 1, ola.overlapBuf.data(), 1,
                         ola.outputBuf.data(), 1, LP_PARTITION_SIZE);

                // Save second half as overlap for next block
                std::memcpy(ola.overlapBuf.data(), m_lpIfftOut.data() + LP_PARTITION_SIZE,
                           LP_PARTITION_SIZE * sizeof(float));
            }

            // Advance FDL index after all channels
            inst.fdlIdx = (inst.fdlIdx + 1) % m_numKernelPartitions;
            inst.phase = 0;

            if (!inst.hasOutput) {
                inst.partitionsProcessed++;
                if (inst.partitionsProcessed >= m_numKernelPartitions + 1)
                    inst.hasOutput = true;  // FDL full + 1 clean overlap partition
            }
        }
    }
}

// ── buildBiquadSetup ─────────────────────────────────────────────────
// Called from UI thread. Creates a vDSP_biquad_Setup from pending
// biquad coefficients for all active enabled bands, staged for RT swap.
void EqualizerProcessor::buildBiquadSetup()
{
    // Snapshot pending params under lock
    std::array<EQBand, MAX_BANDS> snapBands;
    std::array<BiquadCoeffs, MAX_BANDS> snapCoeffs;
    int snapActive;
    while (m_pendingLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    snapBands = m_pendingBands;
    snapCoeffs = m_pendingCoeffs;
    snapActive = m_pendingActiveBands;
    m_pendingLock.clear(std::memory_order_release);

    // Count active sections (enabled bands with non-zero effect)
    int sections = 0;
    for (int i = 0; i < snapActive; ++i) {
        if (!snapBands[i].enabled) continue;
        if (snapBands[i].type <= EQBand::HighShelf && snapBands[i].gainDb == 0.0f) continue;
        sections++;
    }

    if (sections == 0) {
        if (m_stagedBiquadSetup) {
            vDSP_biquad_DestroySetup(m_stagedBiquadSetup);
            m_stagedBiquadSetup = nullptr;
        }
        m_stagedBiquadSections = 0;
        m_biquadSetupReady.store(true, std::memory_order_release);
        return;
    }

    // Build coefficient array: 5 doubles per section [b0, b1, b2, a1, a2]
    std::vector<double> coeffs(5 * sections);
    int idx = 0;
    for (int i = 0; i < snapActive; ++i) {
        if (!snapBands[i].enabled) continue;
        if (snapBands[i].type <= EQBand::HighShelf && snapBands[i].gainDb == 0.0f) continue;
        const auto& c = snapCoeffs[i];
        coeffs[idx * 5 + 0] = c.b0;
        coeffs[idx * 5 + 1] = c.b1;
        coeffs[idx * 5 + 2] = c.b2;
        coeffs[idx * 5 + 3] = c.a1;
        coeffs[idx * 5 + 4] = c.a2;
        idx++;
    }

    vDSP_biquad_Setup newSetup = vDSP_biquad_CreateSetup(coeffs.data(), sections);
    if (!newSetup) return;

    if (m_stagedBiquadSetup)
        vDSP_biquad_DestroySetup(m_stagedBiquadSetup);
    m_stagedBiquadSetup = newSetup;
    m_stagedBiquadSections = sections;
    m_biquadSetupReady.store(true, std::memory_order_release);
}

// ── Self-test ────────────────────────────────────────────────────────

bool EqualizerProcessor::selfTest()
{
    qDebug() << "[EQ SelfTest] Starting...";
    bool allPassed = true;

    // ── Test 1: Flat EQ passthrough (constant input) ──
    {
        EqualizerProcessor proc;
        proc.m_phaseMode = LinearPhase;
        proc.prepare(44100.0, 2);

        const int numBlocks = 10;
        float maxErr = 0;

        for (int b = 0; b < numBlocks; ++b) {
            std::vector<float> block(LP_PARTITION_SIZE * 2, 0.5f);
            proc.process(block.data(), LP_PARTITION_SIZE, 2);
            if (b >= 5) {
                for (int i = 0; i < LP_PARTITION_SIZE; ++i) {
                    float diffL = std::abs(block[i * 2] - 0.5f);
                    float diffR = std::abs(block[i * 2 + 1] - 0.5f);
                    if (diffL > maxErr) maxErr = diffL;
                    if (diffR > maxErr) maxErr = diffR;
                }
            }
        }

        bool pass = (maxErr < 0.01f);
        qDebug() << "[EQ SelfTest] Flat passthrough:"
                 << "maxErr=" << maxErr << (pass ? "PASS" : "FAIL");
        if (!pass) allPassed = false;
    }

    // ── Test 2: Sine wave — collect full output, compare inter vs intra block delta ──
    // Feed 1kHz sine through flat EQ.  Collect all output into a single buffer.
    // Compare max delta at OLA partition boundaries vs max delta within partitions.
    // If inter-block delta >> intra-block delta, the OLA has a discontinuity bug.
    {
        EqualizerProcessor proc;
        proc.m_phaseMode = LinearPhase;
        proc.prepare(44100.0, 2);

        const int numBlocks = 20;
        const int totalSamples = numBlocks * LP_PARTITION_SIZE;
        std::vector<float> allOutput(totalSamples); // L channel only

        for (int b = 0; b < numBlocks; ++b) {
            std::vector<float> block(LP_PARTITION_SIZE * 2);
            for (int i = 0; i < LP_PARTITION_SIZE; ++i) {
                float t = static_cast<float>(b * LP_PARTITION_SIZE + i) / 44100.0f;
                float sample = 0.5f * std::sin(2.0f * static_cast<float>(PI) * 1000.0f * t);
                block[i * 2] = sample;
                block[i * 2 + 1] = sample;
            }
            proc.process(block.data(), LP_PARTITION_SIZE, 2);
            for (int i = 0; i < LP_PARTITION_SIZE; ++i)
                allOutput[b * LP_PARTITION_SIZE + i] = block[i * 2];
        }

        // Skip first 6 partitions (latency + fade-in warmup)
        int startSample = 6 * LP_PARTITION_SIZE;
        float maxIntraDelta = 0; // max delta between consecutive samples WITHIN a partition
        float maxInterDelta = 0; // max delta at partition BOUNDARIES

        for (int n = startSample + 1; n < totalSamples; ++n) {
            float delta = std::abs(allOutput[n] - allOutput[n - 1]);
            bool isBoundary = (n % LP_PARTITION_SIZE == 0);
            if (isBoundary) {
                if (delta > maxInterDelta) maxInterDelta = delta;
            } else {
                if (delta > maxIntraDelta) maxIntraDelta = delta;
            }
        }

        // The inter-block delta should be no larger than intra-block delta + tiny epsilon.
        // If inter >> intra, the OLA is producing discontinuities at block edges.
        float ratio = (maxIntraDelta > 1e-10f) ? maxInterDelta / maxIntraDelta : 0.0f;
        bool pass = (ratio < 1.05f); // ≤5% tolerance for float precision
        qDebug() << "[EQ SelfTest] Sine OLA continuity:"
                 << "intra=" << maxIntraDelta << "inter=" << maxInterDelta
                 << "ratio=" << ratio << (pass ? "PASS" : "FAIL");
        if (!pass) allPassed = false;
    }

    // ── Test 3: +6dB EQ with 440Hz sine — same inter/intra comparison ──
    {
        EqualizerProcessor proc;
        proc.m_phaseMode = LinearPhase;
        proc.prepare(44100.0, 2);
        proc.setBand(0, 1000.0f, 6.0f, 1.0f);

        const int numBlocks = 20;
        const int totalSamples = numBlocks * LP_PARTITION_SIZE;
        std::vector<float> allOutput(totalSamples);

        for (int b = 0; b < numBlocks; ++b) {
            std::vector<float> block(LP_PARTITION_SIZE * 2);
            for (int i = 0; i < LP_PARTITION_SIZE; ++i) {
                float t = static_cast<float>(b * LP_PARTITION_SIZE + i) / 44100.0f;
                float sample = 0.25f * std::sin(2.0f * static_cast<float>(PI) * 440.0f * t);
                block[i * 2] = sample;
                block[i * 2 + 1] = sample;
            }
            proc.process(block.data(), LP_PARTITION_SIZE, 2);
            for (int i = 0; i < LP_PARTITION_SIZE; ++i)
                allOutput[b * LP_PARTITION_SIZE + i] = block[i * 2];
        }

        int startSample = 6 * LP_PARTITION_SIZE;
        float maxIntraDelta = 0;
        float maxInterDelta = 0;

        for (int n = startSample + 1; n < totalSamples; ++n) {
            float delta = std::abs(allOutput[n] - allOutput[n - 1]);
            bool isBoundary = (n % LP_PARTITION_SIZE == 0);
            if (isBoundary) {
                if (delta > maxInterDelta) maxInterDelta = delta;
            } else {
                if (delta > maxIntraDelta) maxIntraDelta = delta;
            }
        }

        float ratio = (maxIntraDelta > 1e-10f) ? maxInterDelta / maxIntraDelta : 0.0f;
        bool pass = (ratio < 1.05f);
        qDebug() << "[EQ SelfTest] EQ+6dB OLA continuity:"
                 << "intra=" << maxIntraDelta << "inter=" << maxInterDelta
                 << "ratio=" << ratio << (pass ? "PASS" : "FAIL");
        if (!pass) allPassed = false;
    }

    // ── Test 4: Mixed frame sizes with sine ──
    {
        EqualizerProcessor proc;
        proc.m_phaseMode = LinearPhase;
        proc.prepare(44100.0, 2);

        const int frameSizes[] = {512, 256, 768, 1024, 2048};
        const int totalSamples = 30 * 2048; // upper bound
        std::vector<float> allOutput;
        allOutput.reserve(totalSamples);
        int totalFrames = 0;

        for (int round = 0; round < 30; ++round) {
            int frames = frameSizes[round % 5];
            std::vector<float> block(frames * 2);
            for (int i = 0; i < frames; ++i) {
                float t = static_cast<float>(totalFrames + i) / 44100.0f;
                float sample = 0.5f * std::sin(2.0f * static_cast<float>(PI) * 440.0f * t);
                block[i * 2] = sample;
                block[i * 2 + 1] = sample;
            }
            proc.process(block.data(), frames, 2);
            for (int i = 0; i < frames; ++i)
                allOutput.push_back(block[i * 2]);
            totalFrames += frames;
        }

        int startSample = 6 * LP_PARTITION_SIZE;
        float maxIntraDelta = 0;
        float maxInterDelta = 0;

        for (int n = startSample + 1; n < static_cast<int>(allOutput.size()); ++n) {
            float delta = std::abs(allOutput[n] - allOutput[n - 1]);
            bool isBoundary = (n % LP_PARTITION_SIZE == 0);
            if (isBoundary) {
                if (delta > maxInterDelta) maxInterDelta = delta;
            } else {
                if (delta > maxIntraDelta) maxIntraDelta = delta;
            }
        }

        float ratio = (maxIntraDelta > 1e-10f) ? maxInterDelta / maxIntraDelta : 0.0f;
        bool pass = (ratio < 1.05f);
        qDebug() << "[EQ SelfTest] Mixed frames OLA:"
                 << "intra=" << maxIntraDelta << "inter=" << maxInterDelta
                 << "ratio=" << ratio << (pass ? "PASS" : "FAIL");
        if (!pass) allPassed = false;
    }

    qDebug() << "[EQ SelfTest]" << (allPassed ? "ALL PASSED" : "SOME FAILED");
    return allPassed;
}

#else // !__APPLE__

bool EqualizerProcessor::selfTest()
{
    qDebug() << "[EQ SelfTest] Skipped — vDSP not available";
    return true;
}

#endif // __APPLE__
