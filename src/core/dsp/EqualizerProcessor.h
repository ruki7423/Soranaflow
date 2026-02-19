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
    int activeBands() const { return m_pendingActiveBands; }
    void setActiveBands(int count);

    // Batch updates: defer kernel builds until endBatchUpdate()
    void beginBatchUpdate();
    void endBatchUpdate();

    // Phase mode
    void setPhaseMode(PhaseMode mode);
    PhaseMode phaseMode() const { return m_phaseMode; }
    int latencySamples() const;

    // Self-test: verifies LP OLA correctness (called at startup)
    static bool selfTest();

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

    std::atomic<bool> m_enabled{true};
    // Enable/disable fade (crossfade between dry and processed over ~6ms)
    float m_enableFadeMix = 1.0f;        // RT-only: 0.0=dry, 1.0=processed
    std::vector<float> m_enableFadeBuf;   // pre-allocated dry copy
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

    // Coefficient crossfade (prevents pops on preset/band changes in MP mode)
    bool m_coeffFading = false;
    int m_coeffFadePos = 0;
    static constexpr int COEFF_FADE_LEN = 256;  // ~6ms crossfade from dry to processed
    std::vector<float> m_coeffFadeBuf;           // pre-allocated dry copy

    // Thread-safe pending state (UI writes here, audio thread copies to active)
    std::atomic_flag m_pendingLock = ATOMIC_FLAG_INIT;
    std::array<EQBand, MAX_BANDS> m_pendingBands;
    std::array<BiquadCoeffs, MAX_BANDS> m_pendingCoeffs;
    int m_pendingActiveBands = 10;
    std::atomic<bool> m_bandsDirty{false};
    bool m_deferKernelBuild = false;  // batch mode: skip kernel/biquad builds

    // ── Linear Phase — partitioned convolution (matches ConvolutionProcessor) ─
#ifdef __APPLE__
    static constexpr int LP_PARTITION_SIZE = 1024;
    static constexpr int LP_MAX_FFT_LOG2N = 15;  // fftSetup supports up to 2^15

    // Convolution FFT (2× partition, same as ConvolutionProcessor)
    static constexpr int CONV_FFT_SIZE   = 2 * LP_PARTITION_SIZE;  // 2048
    static constexpr int CONV_FFT_HALF   = LP_PARTITION_SIZE;       // 1024
    static constexpr int CONV_FFT_LOG2N  = 11;                     // ilog2(2048)
    static constexpr int LP_TRANS_FADE_LEN = 128;  // ~3ms crossfade at 44.1kHz

    std::atomic<bool> m_firDirty{true};

    int m_firLen = 0;
    // Kernel build FFT (≥ firLen, for magnitude sampling)
    int m_firBuildFftSize = 0;
    int m_firBuildFftHalf = 0;
    int m_firBuildFftLog2n = 0;

    FFTSetup m_fftSetup = nullptr;
    int m_numKernelPartitions = 0;

    // Per-channel OLA + FDL state
    struct ChannelOLA {
        std::vector<float> inputBuf;    // LP_PARTITION_SIZE
        std::vector<float> overlapBuf;  // LP_PARTITION_SIZE
        std::vector<float> outputBuf;   // LP_PARTITION_SIZE
        std::vector<std::vector<float>> fdlReals;  // [numPart][CONV_FFT_HALF]
        std::vector<std::vector<float>> fdlImags;
        std::vector<DSPSplitComplex>    fdl;
    };

    // Double-buffered OLA instances for seamless kernel transitions
    struct OLAInstance {
        std::vector<ChannelOLA> channels;
        int phase = 0;
        int fdlIdx = 0;
        bool hasOutput = false;
        int partitionsProcessed = 0;  // counts complete partitions since warmup start
        // Per-instance kernel partitions
        std::vector<std::vector<float>> kernReals;   // [numPart][CONV_FFT_HALF]
        std::vector<std::vector<float>> kernImags;
        std::vector<DSPSplitComplex>    kernParts;
    };
    OLAInstance m_olaSlots[2];
    int m_curSlot = 0;
    int m_nextSlot = -1;     // -1 = no warmup in progress
    bool m_crossfading = false;
    int m_xfadePos = 0;

    // Scratch for double-buffer processing (pre-allocated)
    std::vector<float> m_dryBuf;    // dry input copy during dual processing
    std::vector<float> m_nextBuf;   // next slot's output during crossfade

    // FFT scratch (CONV_FFT_SIZE) — shared between both OLA instances (sequential use)
    std::vector<float> m_lpFftInBuf;
    std::vector<float> m_lpSplitReal, m_lpSplitImag;
    DSPSplitComplex m_lpFftSplit{};
    std::vector<float> m_lpAccumReal, m_lpAccumImag;
    DSPSplitComplex m_lpAccumSplit{};
    std::vector<float> m_lpIfftOut;

    // Kernel build scratch (m_firBuildFftSize)
    std::vector<float> m_lpKernelBuildBuf;
    std::vector<float> m_lpMagBins;
    std::vector<float> m_lpSpecReal;
    std::vector<float> m_lpSpecImag;
    std::vector<float> m_lpKernelTimeBuf;

    // ── Staged kernel (built by UI thread, swapped by RT thread) ──────
    std::vector<std::vector<float>> m_stagedPartReals;   // [numPart][CONV_FFT_HALF]
    std::vector<std::vector<float>> m_stagedPartImags;
    std::atomic<bool> m_stagedKernelReady{false};
    std::atomic_flag m_stagedLock = ATOMIC_FLAG_INIT;

    // UI-thread-only scratch buffers
    std::vector<float> m_stageMagBins;
    std::vector<float> m_stageSpecReal, m_stageSpecImag;
    std::vector<float> m_stageKernelBuildBuf;
    std::vector<float> m_stageKernelTimeBuf;
    std::vector<float> m_stageFftInBuf;
    FFTSetup m_stageFftSetup = nullptr;

    // ── vDSP biquad for Minimum Phase mode ──────
    vDSP_biquad_Setup m_biquadSetup = nullptr;
    int m_biquadSections = 0;
    std::array<std::vector<float>, MAX_CHANNELS> m_biquadDelay;
    vDSP_biquad_Setup m_stagedBiquadSetup = nullptr;
    int m_stagedBiquadSections = 0;
    std::atomic<bool> m_biquadSetupReady{false};

    void allocateLinearPhaseBuffers();
    void buildFIRKernelStaged();   // UI thread only — builds into staged buffers
    void buildBiquadSetup();       // UI thread only — builds vDSP biquad setup
    void resetOLAState();
    void processLinearPhase(float* buf, int frames, int channels);
    void processOLAInstance(OLAInstance& inst, const float* inBuf, float* outBuf,
                            int frames, int channels);
#endif
};
