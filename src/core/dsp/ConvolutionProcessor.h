#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include "IDSPProcessor.h"
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

class ConvolutionProcessor : public IDSPProcessor
{
public:
    ConvolutionProcessor();
    ~ConvolutionProcessor() override;

    static constexpr int MAX_CHANNELS = 24;

    // IDSPProcessor interface
    void process(float* buf, int frames, int channels) override;
    std::string getName() const override { return "Convolution"; }
    bool isEnabled() const override { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled) override;

    void setSampleRate(int rate);

    // Load IR from any audio file via FFmpeg (called from background thread)
    bool loadIR(const std::string& filePath);
    void clearIR();
    bool hasIR() const { return m_hasIR.load(std::memory_order_relaxed); }
    std::string irFilePath() const;

    // Self-test: verify convolution math on startup
    static bool selfTest();

private:
    static constexpr int FFT_ORDER = 10;           // 2^10 = 1024 samples per partition
    static constexpr int PARTITION_SIZE = 1 << FFT_ORDER;  // 1024
    static constexpr int FFT_SIZE = PARTITION_SIZE * 2;     // 2048 (zero-padded)
    static constexpr int FFT_HALF = FFT_SIZE / 2;          // 1024
    // vDSP_fft_zrip log2(FFT_SIZE) = FFT_ORDER + 1 = 11
    static constexpr int FFT_LOG2N = FFT_ORDER + 1;        // 11
    static constexpr float FADE_STEP = 0.0005f;    // ~45ms ramp at 44.1k

#ifdef __APPLE__
    struct IRData {
        // Partitioned frequency-domain IR per channel [ch][partition]
        std::vector<std::vector<DSPSplitComplex>> partitions;
        // Backing storage for split complex data [ch][partition][bin]
        std::vector<std::vector<std::vector<float>>> reals, imags;
        int numPartitions = 0;
        int channelCount = 0;
        int sampleRate = 0;
    };

    void resetState();

    // Build frequency-domain IR partitions from time-domain per-channel samples
    void buildIRPartitions(IRData* dest,
                           const std::vector<std::vector<float>>& irChannels,
                           int irSampleRate);
#endif

    // Thread-safe control
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_hasIR{false};
    std::atomic<bool> m_needsStateReset{true};

#ifdef __APPLE__
    // Active IR data (render thread reads, staged swap writes)
    IRData m_irSlotA;
    IRData* m_activeIR = nullptr;  // &m_irSlotA after first swap (or local in selfTest)

    // Staged IR + FDL (built by background thread, swapped by RT thread)
    struct StagedConvIR {
        IRData irData;
        std::vector<std::vector<DSPSplitComplex>> fdl;
        std::vector<std::vector<std::vector<float>>> fdlReals, fdlImags;
    };
    StagedConvIR m_staged;
    std::atomic<bool> m_stagedReady{false};
#endif
    std::string m_irFilePath;
    std::mutex m_irFilePathMutex;

    std::atomic<int> m_sampleRate{44100};
    std::atomic<bool> m_needsRecalc{false};

#ifdef __APPLE__
    // vDSP FFT setup
    FFTSetup m_fftSetup = nullptr;

    // Render-thread-only state (overlap-add) — per channel [ch][PARTITION_SIZE]
    std::vector<std::vector<float>> m_input;      // [MAX_CHANNELS][PARTITION_SIZE]
    int m_phase = 0;              // Current position within partition (0..PARTITION_SIZE-1)
    bool m_hasOutput = false;     // True after first convolution completes
    int m_activeChannels = 2;     // Current playback channel count
    int m_irChannelCount = 0;     // Channels in loaded IR

    // Frequency-domain delay line per channel [ch][partition]
    std::vector<std::vector<DSPSplitComplex>> m_fdl;
    std::vector<std::vector<std::vector<float>>> m_fdlReals, m_fdlImags;
    int m_fdlIdx = 0;

    // FFT scratch buffers — all sized FFT_HALF for split complex
    std::vector<float> m_fftInBuf;     // FFT_SIZE
    DSPSplitComplex m_fftSplit;
    std::vector<float> m_fftSplitReal, m_fftSplitImag;
    DSPSplitComplex m_accumSplit;
    std::vector<float> m_accumReal, m_accumImag;
    DSPSplitComplex m_tempSplit;
    std::vector<float> m_tempReal, m_tempImag;
    std::vector<float> m_ifftOut;      // FFT_SIZE

    // Overlap-add tail from previous block per channel [ch][PARTITION_SIZE]
    std::vector<std::vector<float>> m_overlap;

    // Output buffer for processed samples per channel [ch][PARTITION_SIZE]
    std::vector<std::vector<float>> m_output;

    // Fade state
    float m_wetMix = 0.0f;

    void convolveChannel(
        const std::vector<DSPSplitComplex>& irPartitions,
        std::vector<DSPSplitComplex>& fdl,
        int fdlIdx, int numPartitions,
        const float* input, float* output, float* overlap);
#endif
};
