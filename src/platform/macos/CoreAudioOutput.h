#pragma once

#include "../IAudioOutput.h"

class CoreAudioOutput : public IAudioOutput {
public:
    CoreAudioOutput();
    ~CoreAudioOutput() override;

    // Lifecycle
    bool open(const AudioStreamFormat& format, uint32_t deviceId = 0) override;
    bool start() override;
    void stop() override;
    void close() override;
    bool isRunning() const override;

    void setRenderCallback(RenderCallback cb) override;
    void setVolume(float vol) override;
    bool setDevice(uint32_t deviceId) override;
    bool setBufferSize(uint32_t frames) override;
    bool setSampleRate(double rate) override;

    // Device queries (virtual overrides — delegate to static helpers)
    std::vector<AudioDevice> enumerateDevices() const override;
    double getMaxSampleRate(uint32_t deviceId = 0) const override;
    double findNearestSupportedRate(double targetRate, uint32_t deviceId = 0) const override;
    bool isBuiltInDevice(uint32_t deviceId) const override;

    // Exclusive mode (hog mode)
    bool setHogMode(bool enabled) override;
    void releaseHogMode() override;
    bool isHogModeSupported() const override;
    bool isExclusiveMode() const override;

    // Signal path info
    std::string deviceName() const override;
    double currentSampleRate() const override;
    double deviceNominalSampleRate() const override;
    bool isBuiltInOutput() const override;

    // Bit-perfect mode
    void setBitPerfectMode(bool enabled) override;
    bool bitPerfectMode() const override;

    // DoP passthrough — skip volume scaling for DSD-over-PCM
    void setDoPPassthrough(bool enabled) override;

    // Transition mute — silences render callback during format changes
    void setTransitioning(bool enabled) override;

    // Static helpers (used by virtual wrappers and internally)
    static std::vector<AudioDevice> enumerateDevicesStatic();
    static double getMaxSampleRateStatic(uint32_t deviceId = 0);
    static double findNearestSupportedRateStatic(double targetRate, uint32_t deviceId = 0);
    static bool isBuiltInDeviceStatic(uint32_t deviceId);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
