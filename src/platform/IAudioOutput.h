#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "AudioDevice.h"
#include "../core/audio/AudioFormat.h"

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    IAudioOutput(const IAudioOutput&) = delete;
    IAudioOutput& operator=(const IAudioOutput&) = delete;

    using RenderCallback = std::function<int(float*, int)>;

    // Lifecycle
    virtual bool open(const AudioStreamFormat& format, uint32_t deviceId = 0) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    virtual bool isRunning() const = 0;

    // Callback
    virtual void setRenderCallback(RenderCallback cb) = 0;

    // Volume
    virtual void setVolume(float vol) = 0;

    // Device control
    virtual bool setDevice(uint32_t deviceId) = 0;
    virtual bool setBufferSize(uint32_t frames) = 0;
    virtual bool setSampleRate(double rate) = 0;

    // Exclusive mode
    virtual bool setHogMode(bool enabled) = 0;
    virtual void releaseHogMode() = 0;
    virtual bool isHogModeSupported() const = 0;
    virtual bool isExclusiveMode() const = 0;

    // Signal path info
    virtual std::string deviceName() const = 0;
    virtual double currentSampleRate() const = 0;
    virtual double deviceNominalSampleRate() const = 0;
    virtual bool isBuiltInOutput() const = 0;

    // Bit-perfect
    virtual void setBitPerfectMode(bool enabled) = 0;
    virtual bool bitPerfectMode() const = 0;

    // DoP passthrough — disables volume scaling so DoP markers survive intact
    virtual void setDoPPassthrough(bool) {}

    // Transition mute — silences render callback during format changes
    // Prevents stale DoP data from reaching DAC during DSD teardown
    virtual void setTransitioning(bool) {}

    // Device queries (were static on CoreAudioOutput — const since they are pure queries)
    virtual std::vector<AudioDevice> enumerateDevices() const = 0;
    virtual double getMaxSampleRate(uint32_t deviceId = 0) const = 0;
    virtual double findNearestSupportedRate(double targetRate, uint32_t deviceId = 0) const = 0;
    virtual bool isBuiltInDevice(uint32_t deviceId) const = 0;

protected:
    IAudioOutput() = default;
};
