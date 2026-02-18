#include "EqualizerProcessor.h"
#include <algorithm>
#include <complex>
#include <cstring>

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
#endif
}

// ── prepare ─────────────────────────────────────────────────────────
void EqualizerProcessor::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate;
    m_channels = std::min(channels, MAX_CHANNELS);
    for (int i = 0; i < MAX_BANDS; ++i)
        recalcCoeffs(i);
    reset();

#ifdef __APPLE__
    if (m_phaseMode == LinearPhase) {
        allocateLinearPhaseBuffers();
        m_firDirty.store(true, std::memory_order_relaxed);
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
    // Clear OLA state
    for (auto& ch : m_olaState) {
        if (!ch.inputBuf.empty())
            std::memset(ch.inputBuf.data(), 0, ch.inputBuf.size() * sizeof(float));
        if (!ch.overlapBuf.empty())
            std::memset(ch.overlapBuf.data(), 0, ch.overlapBuf.size() * sizeof(float));
        if (!ch.outputBuf.empty())
            std::memset(ch.outputBuf.data(), 0, ch.outputBuf.size() * sizeof(float));
        ch.position = 0;
        ch.hasOutput = false;
    }
#endif
}

// ── process ─────────────────────────────────────────────────────────
void EqualizerProcessor::process(float* buf, int frames, int channels)
{
    if (!m_enabled) return;

    // Check for pending phase mode switch (only when not already transitioning)
    if (m_transitionPhase == 0) {
        int pending = m_pendingPhaseMode.exchange(-1, std::memory_order_acquire);
        if (pending >= 0) {
            m_transitionTarget = static_cast<PhaseMode>(pending);
            m_transitionPhase = 1;  // start fade-out
            m_transitionPos = 0;
        }
    }

    // ── Phase transition: fade-out → switch+silence → fade-in ──

    if (m_transitionPhase == 1) {
        // Process with CURRENT mode
#ifdef __APPLE__
        if (m_phaseMode == LinearPhase && m_fftSize > 0)
            processLinearPhase(buf, frames, channels);
        else
#endif
            processMinimumPhase(buf, frames, channels);

        // Apply fade-out ramp
        for (int i = 0; i < frames; ++i) {
            float t = static_cast<float>(m_transitionPos + i) / TRANSITION_FADE_SAMPLES;
            float gain = (t >= 1.0f) ? 0.0f : 1.0f - t;
            for (int c = 0; c < channels; ++c)
                buf[i * channels + c] *= gain;
        }
        m_transitionPos += frames;
        if (m_transitionPos >= TRANSITION_FADE_SAMPLES)
            m_transitionPhase = 2;
        return;
    }

    if (m_transitionPhase == 2) {
        // Switch mode + clear ALL processing state
        m_phaseMode = m_transitionTarget;

        for (auto& bandStates : m_state) {
            for (auto& s : bandStates) { s = BiquadState{}; }
        }
#ifdef __APPLE__
        for (auto& ola : m_olaState) {
            if (!ola.inputBuf.empty())
                std::memset(ola.inputBuf.data(), 0, ola.inputBuf.size() * sizeof(float));
            if (!ola.overlapBuf.empty())
                std::memset(ola.overlapBuf.data(), 0, ola.overlapBuf.size() * sizeof(float));
            if (!ola.outputBuf.empty())
                std::memset(ola.outputBuf.data(), 0, ola.outputBuf.size() * sizeof(float));
            ola.position = 0;
            ola.hasOutput = false;
        }
        if (m_phaseMode == LinearPhase)
            m_firDirty.store(true, std::memory_order_relaxed);
#endif

        // Output silence for this block
        std::memset(buf, 0, frames * channels * sizeof(float));

        m_transitionPhase = 3;
        m_transitionPos = 0;
        return;
    }

    if (m_transitionPhase == 3) {
        // Process with NEW mode
#ifdef __APPLE__
        if (m_phaseMode == LinearPhase && m_fftSize > 0)
            processLinearPhase(buf, frames, channels);
        else
#endif
            processMinimumPhase(buf, frames, channels);

        // Apply fade-in ramp
        for (int i = 0; i < frames; ++i) {
            float t = static_cast<float>(m_transitionPos + i) / TRANSITION_FADE_SAMPLES;
            float gain = (t >= 1.0f) ? 1.0f : t;
            for (int c = 0; c < channels; ++c)
                buf[i * channels + c] *= gain;
        }
        m_transitionPos += frames;
        if (m_transitionPos >= TRANSITION_FADE_SAMPLES)
            m_transitionPhase = 0;  // transition complete
        return;
    }

    // ── Normal processing ──
#ifdef __APPLE__
    if (m_phaseMode == LinearPhase && m_fftSize > 0) {
        processLinearPhase(buf, frames, channels);
        return;
    }
#endif

    processMinimumPhase(buf, frames, channels);
}

// ── processMinimumPhase ─────────────────────────────────────────────
void EqualizerProcessor::processMinimumPhase(float* buf, int frames, int channels)
{
    int ch = std::min(channels, MAX_CHANNELS);

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
    m_bands[band] = params;
    recalcCoeffs(band);
#ifdef __APPLE__
    m_firDirty.store(true, std::memory_order_relaxed);
#endif
}

// ── setBand (simple: freq, gain, q — legacy compatibility) ─────────
void EqualizerProcessor::setBand(int band, float freqHz, float gainDb, float q)
{
    if (band < 0 || band >= MAX_BANDS) return;
    m_bands[band].frequency = freqHz;
    m_bands[band].gainDb = gainDb;
    m_bands[band].q = q;
    recalcCoeffs(band);
#ifdef __APPLE__
    m_firDirty.store(true, std::memory_order_relaxed);
#endif
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
#ifdef __APPLE__
    m_firDirty.store(true, std::memory_order_relaxed);
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
        m_firDirty.store(true, std::memory_order_relaxed);
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
#ifdef __APPLE__
    m_firDirty.store(true, std::memory_order_relaxed);
#endif
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

    // FFT size: next power of 2 >= firLen + LP_PARTITION_SIZE
    m_fftSize = nextPow2(m_firLen + LP_PARTITION_SIZE);
    m_fftHalf = m_fftSize / 2;
    m_fftLog2n = ilog2(m_fftSize);

    // Kernel frequency-domain storage
    m_firKernelFDReal.assign(m_fftHalf, 0.0f);
    m_firKernelFDImag.assign(m_fftHalf, 0.0f);
    m_firKernelFD.realp = m_firKernelFDReal.data();
    m_firKernelFD.imagp = m_firKernelFDImag.data();

    // Per-channel OLA state
    int overlapLen = m_fftSize - LP_PARTITION_SIZE;
    m_olaState.resize(MAX_CHANNELS);
    for (auto& ch : m_olaState) {
        ch.inputBuf.assign(LP_PARTITION_SIZE, 0.0f);
        ch.overlapBuf.assign(overlapLen, 0.0f);
        ch.outputBuf.assign(LP_PARTITION_SIZE, 0.0f);
        ch.position = 0;
        ch.hasOutput = false;
    }

    // FFT scratch buffers
    m_lpFftInBuf.assign(m_fftSize, 0.0f);
    m_lpSplitReal.assign(m_fftHalf, 0.0f);
    m_lpSplitImag.assign(m_fftHalf, 0.0f);
    m_lpFftSplit.realp = m_lpSplitReal.data();
    m_lpFftSplit.imagp = m_lpSplitImag.data();

    m_lpAccumReal.assign(m_fftHalf, 0.0f);
    m_lpAccumImag.assign(m_fftHalf, 0.0f);
    m_lpAccumSplit.realp = m_lpAccumReal.data();
    m_lpAccumSplit.imagp = m_lpAccumImag.data();

    m_lpIfftOut.assign(m_fftSize, 0.0f);

    // Kernel build scratch
    m_lpKernelBuildBuf.assign(m_fftSize, 0.0f);
}

// ── buildFIRKernel ──────────────────────────────────────────────────
// Called from RT thread when m_firDirty is set.
// Computes zero-phase FIR kernel from current biquad coefficients.
void EqualizerProcessor::buildFIRKernel()
{
    if (!m_fftSetup || m_fftSize <= 0) return;

    // ── Step 1: Compute combined magnitude at each frequency bin ────
    // We need fftHalf+1 bins (0..fftHalf), but store DC at real[0]
    // and Nyquist at imag[0] in vDSP packed format.
    std::vector<float> magBins(m_fftHalf + 1, 1.0f);

    for (int k = 0; k <= m_fftHalf; ++k) {
        double w = 2.0 * PI * k / m_fftSize;
        double combinedMag = 1.0;

        for (int band = 0; band < m_activeBands; ++band) {
            if (!m_bands[band].enabled) continue;
            if (m_bands[band].type <= EQBand::HighShelf && m_bands[band].gainDb == 0.0f) continue;

            const auto& c = m_coeffs[band];

            // H(z) at z = e^jw
            std::complex<double> z = std::exp(std::complex<double>(0, w));
            std::complex<double> z1 = 1.0 / z;
            std::complex<double> z2 = z1 * z1;

            std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
            std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;

            double mag = std::abs(num / den);
            combinedMag *= mag;
        }

        magBins[k] = static_cast<float>(combinedMag);
    }

    // ── Step 2: Pack magnitude into vDSP split-complex (zero phase) ─
    // real[0] = DC, imag[0] = Nyquist, real[k] = mag[k], imag[k] = 0
    std::vector<float> specReal(m_fftHalf, 0.0f);
    std::vector<float> specImag(m_fftHalf, 0.0f);

    specReal[0] = magBins[0];          // DC
    specImag[0] = magBins[m_fftHalf];  // Nyquist
    for (int k = 1; k < m_fftHalf; ++k) {
        specReal[k] = magBins[k];
        specImag[k] = 0.0f;  // zero phase
    }

    DSPSplitComplex specSplit;
    specSplit.realp = specReal.data();
    specSplit.imagp = specImag.data();

    // ── Step 3: Inverse FFT → zero-phase impulse response ───────────
    vDSP_fft_zrip(m_fftSetup, &specSplit, 1, m_fftLog2n, kFFTDirection_Inverse);

    // Unpack to real
    vDSP_ztoc(&specSplit, 1,
              reinterpret_cast<DSPComplex*>(m_lpKernelBuildBuf.data()), 2, m_fftHalf);

    // Normalize: vDSP inverse gives unnormalized IDFT.
    // For zero-phase spectrum → time domain, scale by 1/fftSize.
    float ifftScale = 1.0f / static_cast<float>(m_fftSize);
    vDSP_vsmul(m_lpKernelBuildBuf.data(), 1, &ifftScale,
               m_lpKernelBuildBuf.data(), 1, m_fftSize);

    // ── Step 4: Circular shift to center — make causal ──────────────
    // The zero-phase IR is symmetric about sample 0:
    //   Right half: samples 0..fftSize/2
    //   Left half (negative time): samples fftSize-firLen/2..fftSize-1
    // We extract firLen samples centered around sample 0.
    int halfFir = m_firLen / 2;
    std::vector<float> kernel(m_firLen, 0.0f);

    // Left half: from the end of the buffer (negative time samples)
    for (int i = 0; i < halfFir; ++i) {
        int srcIdx = m_fftSize - halfFir + i;
        if (srcIdx >= 0 && srcIdx < m_fftSize)
            kernel[i] = m_lpKernelBuildBuf[srcIdx];
    }
    // Right half: from the beginning
    for (int i = 0; i < halfFir; ++i) {
        if (i < m_fftSize)
            kernel[halfFir + i] = m_lpKernelBuildBuf[i];
    }

    // ── Step 5: Apply Blackman-Harris window ────────────────────────
    for (int n = 0; n < m_firLen; ++n) {
        double t = static_cast<double>(n) / (m_firLen - 1);
        double w = 0.35875
                 - 0.48829 * std::cos(2.0 * PI * t)
                 + 0.14128 * std::cos(4.0 * PI * t)
                 - 0.01168 * std::cos(6.0 * PI * t);
        kernel[n] *= static_cast<float>(w);
    }

    // ── Step 6: Zero-pad to fftSize and forward FFT → store kernel FD
    std::memset(m_lpKernelBuildBuf.data(), 0, m_fftSize * sizeof(float));
    std::memcpy(m_lpKernelBuildBuf.data(), kernel.data(), m_firLen * sizeof(float));

    // Pack into split complex
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_lpKernelBuildBuf.data()), 2,
              &m_firKernelFD, 1, m_fftHalf);

    // Forward FFT
    vDSP_fft_zrip(m_fftSetup, &m_firKernelFD, 1, m_fftLog2n, kFFTDirection_Forward);

    // ── Done ────────────────────────────────────────────────────────
    m_firDirty.store(false, std::memory_order_relaxed);
}

// ── processLinearPhase ──────────────────────────────────────────────
// Mirrors ConvolutionProcessor's sample-by-sample overlap-add pattern.
void EqualizerProcessor::processLinearPhase(float* buf, int frames, int channels)
{
    if (!m_fftSetup || m_fftSize <= 0) return;

    // Rebuild kernel if dirty (band params changed)
    if (m_firDirty.load(std::memory_order_relaxed)) {
        buildFIRKernel();
    }

    int ch = std::min(channels, MAX_CHANNELS);
    int pos = 0;

    while (pos < frames) {
        // How many samples until partition is full
        int firstChPos = m_olaState.empty() ? 0 : m_olaState[0].position;
        int avail = std::min(frames - pos, LP_PARTITION_SIZE - firstChPos);

        for (int i = 0; i < avail; ++i) {
            int baseIdx = (pos + i) * channels;

            // Deinterleave input into per-channel partition buffers
            for (int c = 0; c < ch; ++c) {
                m_olaState[c].inputBuf[firstChPos + i] = buf[baseIdx + c];
            }

            // Output previously convolved result
            if (m_olaState[0].hasOutput) {
                for (int c = 0; c < ch; ++c) {
                    buf[baseIdx + c] = m_olaState[c].outputBuf[firstChPos + i];
                }
            }
            // else: first partition — output silence (inherent latency)
        }

        // Update positions
        for (int c = 0; c < ch; ++c) {
            m_olaState[c].position = firstChPos + avail;
        }
        pos += avail;

        // When partition is full, convolve
        if (m_olaState[0].position >= LP_PARTITION_SIZE) {
            for (int c = 0; c < ch; ++c) {
                auto& ola = m_olaState[c];

                // Zero-pad partition to fftSize
                std::memcpy(m_lpFftInBuf.data(), ola.inputBuf.data(),
                           LP_PARTITION_SIZE * sizeof(float));
                std::memset(m_lpFftInBuf.data() + LP_PARTITION_SIZE, 0,
                           (m_fftSize - LP_PARTITION_SIZE) * sizeof(float));

                // Pack and forward FFT
                vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_lpFftInBuf.data()), 2,
                          &m_lpFftSplit, 1, m_fftHalf);
                vDSP_fft_zrip(m_fftSetup, &m_lpFftSplit, 1, m_fftLog2n,
                              kFFTDirection_Forward);

                // Complex multiply with kernel FD
                // Bin 0: DC and Nyquist (packed separately)
                m_lpAccumReal[0] = m_lpSplitReal[0] * m_firKernelFDReal[0];
                m_lpAccumImag[0] = m_lpSplitImag[0] * m_firKernelFDImag[0];

                // Bins 1..fftHalf-1: standard complex multiply
                for (int k = 1; k < m_fftHalf; ++k) {
                    float ar = m_lpSplitReal[k], ai = m_lpSplitImag[k];
                    float br = m_firKernelFDReal[k], bi = m_firKernelFDImag[k];
                    m_lpAccumReal[k] = ar * br - ai * bi;
                    m_lpAccumImag[k] = ar * bi + ai * br;
                }

                // Inverse FFT
                vDSP_fft_zrip(m_fftSetup, &m_lpAccumSplit, 1, m_fftLog2n,
                              kFFTDirection_Inverse);

                // Unpack to real
                vDSP_ztoc(&m_lpAccumSplit, 1,
                          reinterpret_cast<DSPComplex*>(m_lpIfftOut.data()), 2, m_fftHalf);

                // Scale: 1/(fftSize*4), same as ConvolutionProcessor.
                // Both input and kernel were forward-FFT'd by vDSP (each ×2),
                // and vDSP inverse doesn't divide by N.  Total spurious factor = 4N.
                float scale = 1.0f / static_cast<float>(m_fftSize * 4);
                vDSP_vsmul(m_lpIfftOut.data(), 1, &scale, m_lpIfftOut.data(), 1, m_fftSize);

                // Overlap-add: output = result[0..PART-1] + overlap[0..PART-1]
                vDSP_vadd(m_lpIfftOut.data(), 1, ola.overlapBuf.data(), 1,
                         ola.outputBuf.data(), 1, LP_PARTITION_SIZE);

                // Update overlap: shift consumed portion, add new IFFT tail.
                // The overlap buffer is longer than one partition (fftSize > 2*partSize)
                // so we must preserve the unconsumed tail from previous blocks.
                int overlapLen = m_fftSize - LP_PARTITION_SIZE;
                int carryLen = overlapLen - LP_PARTITION_SIZE;
                std::memmove(ola.overlapBuf.data(),
                            ola.overlapBuf.data() + LP_PARTITION_SIZE,
                            carryLen * sizeof(float));
                std::memset(ola.overlapBuf.data() + carryLen, 0,
                           LP_PARTITION_SIZE * sizeof(float));
                vDSP_vadd(m_lpIfftOut.data() + LP_PARTITION_SIZE, 1,
                         ola.overlapBuf.data(), 1,
                         ola.overlapBuf.data(), 1, overlapLen);

                ola.position = 0;

                if (!ola.hasOutput) {
                    ola.hasOutput = true;
                }
            }
        }
    }
}

#endif // __APPLE__
