#pragma once

#include <atomic>
#include "../dsp/CrossfeedProcessor.h"
#include "../dsp/ConvolutionProcessor.h"
#include "../dsp/HRTFProcessor.h"

class DSPPipeline;
class VolumeLevelingManager;

// Owns the spatial audio processors (crossfeed, convolution, HRTF)
// and headroom gain. Provides a single process() method that applies
// the full DSP chain in order:
//   headroom → crossfeed → convolution → HRTF → DSP → leveling → limiter
//
// Configuration methods run on the main thread.
// process() is RT-safe (called from the audio render callback).
class AudioRenderChain {
public:
    AudioRenderChain();

    // ── Configuration (main thread) ──────────────────────────────────
    void setSampleRate(int rate);
    void updateHeadroomGain();  // reads Settings, updates atomic
    float headroomGainLinear() const { return m_headroomGain.load(std::memory_order_relaxed); }

    // ── Processor access ─────────────────────────────────────────────
    CrossfeedProcessor& crossfeed() { return m_crossfeed; }
    ConvolutionProcessor& convolution() { return m_convolution; }
    HRTFProcessor& hrtf() { return m_hrtf; }
    const CrossfeedProcessor& crossfeed() const { return m_crossfeed; }
    const ConvolutionProcessor& convolution() const { return m_convolution; }
    const HRTFProcessor& hrtf() const { return m_hrtf; }

    // ── RT-safe processing ───────────────────────────────────────────
    // Applies: headroom → crossfeed → convolution → HRTF → DSP → leveling → limiter
    // dopPassthrough: skip all processing (DSD data must stay bit-perfect)
    // bitPerfect: skip DSP pipeline
    void process(float* buf, int frames, int channels,
                 DSPPipeline* dsp, VolumeLevelingManager* leveling,
                 bool dopPassthrough, bool bitPerfect);

private:
    CrossfeedProcessor m_crossfeed;
    ConvolutionProcessor m_convolution;
    HRTFProcessor m_hrtf;
    std::atomic<float> m_headroomGain{1.0f};
    float m_prevLevelingGain = 1.0f;  // render-thread only, for smooth ramping
};
