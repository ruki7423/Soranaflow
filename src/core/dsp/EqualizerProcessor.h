#pragma once

#include "IDSPProcessor.h"
#include <cmath>
#include <array>
#include <vector>
#include <atomic>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

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
    float q          = 0.7071f;   // Quality factor (0.1 to 30), default = 1/sqrt(2) Butterworth
};

// 20-band parametric EQ processor (REW-style)
class EqualizerProcessor : public IDSPProcessor {
public:
    static constexpr int MAX_BANDS = 20;
    static constexpr int MAX_CHANNELS = 24;

    enum PhaseMode { MinimumPhase = 0, LinearPhase = 1 };

    EqualizerProcessor();
    ~EqualizerProcessor();

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

    // Phase mode
    void setPhaseMode(PhaseMode mode);
    PhaseMode phaseMode() const { return m_phaseMode; }
    int latencySamples() const;

    // Frequency response for graph visualization (returns dB values)
    std::vector<double> getFrequencyResponse(int numPoints = 512) const;

    // Parameters interface
    std::vector<DSPParameter> getParameters() const override;
    void setParameter(int index, float value) override;
    float getParameter(int index) const override;

private:
    void recalcCoeffs(int band);
    void processMinimumPhase(float* buf, int frames, int channels);
    static BiquadCoeffs calcBiquad(double sampleRate, const EQBand& band);

    bool m_enabled = true;
    double m_sampleRate = 44100.0;
    int m_channels = 2;
    int m_activeBands = 10;
    PhaseMode m_phaseMode = MinimumPhase;
    std::atomic<int> m_pendingPhaseMode{-1};  // -1 = no pending change
    // Phase mode transition (fade-out → mute/warmup → fade-in)
    int m_transitionPhase = 0;       // 0=none, 1=fade-out, 2=mute+fade-in
    int m_transitionPos = 0;
    PhaseMode m_transitionTarget = MinimumPhase;
    int m_warmupDuration = 0;
    static constexpr int TRANSITION_FADE_LEN = 256;  // ~6ms fade

    std::array<EQBand, MAX_BANDS> m_bands;
    std::array<BiquadCoeffs, MAX_BANDS> m_coeffs;
    // Per-band, per-channel biquad state
    std::array<std::array<BiquadState, MAX_CHANNELS>, MAX_BANDS> m_state;

    // ── Linear Phase (vDSP/Accelerate overlap-add) ──────────────────
#ifdef __APPLE__
    static constexpr int LP_PARTITION_SIZE = 1024;
    static constexpr int LP_MAX_FFT_LOG2N = 15;  // supports up to 32768-point FFT

    std::atomic<bool> m_firDirty{true};  // UI thread sets, RT thread reads

    int m_firLen = 0;
    int m_fftSize = 0;
    int m_fftHalf = 0;
    int m_fftLog2n = 0;

    FFTSetup m_fftSetup = nullptr;

    // Kernel in frequency domain
    std::vector<float> m_firKernelFDReal;
    std::vector<float> m_firKernelFDImag;
    DSPSplitComplex m_firKernelFD{};

    // Per-channel OLA state
    struct ChannelOLA {
        std::vector<float> inputBuf;    // LP_PARTITION_SIZE
        std::vector<float> overlapBuf;  // fftSize - LP_PARTITION_SIZE
        std::vector<float> outputBuf;   // LP_PARTITION_SIZE
        int position = 0;
        bool hasOutput = false;
    };
    std::vector<ChannelOLA> m_olaState;  // [MAX_CHANNELS]

    // FFT scratch buffers
    std::vector<float> m_lpFftInBuf;
    std::vector<float> m_lpSplitReal, m_lpSplitImag;
    DSPSplitComplex m_lpFftSplit{};
    std::vector<float> m_lpAccumReal, m_lpAccumImag;
    DSPSplitComplex m_lpAccumSplit{};
    std::vector<float> m_lpIfftOut;

    // Kernel build scratch
    std::vector<float> m_lpKernelBuildBuf;

    void allocateLinearPhaseBuffers();
    void buildFIRKernel();
    void processLinearPhase(float* buf, int frames, int channels);
#endif
};
