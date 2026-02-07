#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Parameter descriptor for DSP processors
struct DSPParameter {
    std::string name;
    float       value    = 0.0f;
    float       minValue = 0.0f;
    float       maxValue = 1.0f;
    float       defaultValue = 0.0f;
    std::string unit;      // e.g., "dB", "Hz", "%"
};

// Abstract interface for all DSP processors in the signal chain.
// All processing is done in 32-bit interleaved float.
class IDSPProcessor {
public:
    virtual ~IDSPProcessor() = default;

    // Process audio buffer in-place.
    // buf: interleaved float32 samples
    // frames: number of frames (each frame = channels samples)
    // channels: number of audio channels
    virtual void process(float* buf, int frames, int channels) = 0;

    // Processor identity
    virtual std::string getName() const = 0;

    // Enable/disable (bypass)
    virtual bool isEnabled() const = 0;
    virtual void setEnabled(bool enabled) = 0;

    // Parameter access
    virtual std::vector<DSPParameter> getParameters() const { return {}; }
    virtual void setParameter(int index, float value) { (void)index; (void)value; }
    virtual float getParameter(int index) const { (void)index; return 0.0f; }

    // Called when sample rate or channel count changes
    virtual void prepare(double sampleRate, int channels) { (void)sampleRate; (void)channels; }

    // Reset internal state (e.g., filter histories)
    virtual void reset() {}
};
