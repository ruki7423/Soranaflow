#include "CoreAudioOutput.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <QDebug>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>
#include <thread>

struct CoreAudioOutput::Impl {
    AudioComponentInstance  audioUnit = nullptr;
    RenderCallback          renderCb;
    std::mutex              cbMutex;
    AudioStreamFormat       format;
    std::atomic<bool>       running{false};
    std::atomic<bool>       destroyed{false};
    std::atomic<bool>       swappingCallback{false};
    float                   volume  = 1.0f;
    float                   prevVolume = 1.0f;
    bool                    bitPerfect = false;
    std::atomic<bool>       dopPassthrough{false};
    std::atomic<bool>       transitioning{false};
    std::atomic<bool>       dopMarker{false};  // DoP marker alternation state for silence generation

    // ── DoP silence helpers ─────────────────────────────────────────────
    // When the DAC is in DoP mode, it expects every sample to have 0x05 or
    // 0xFA in the top byte (alternating).  PCM zeros (0x000000) have no
    // valid markers → the DAC exits DoP mode abruptly → crackle/noise.
    //
    // These helpers fill buffers with valid DoP silence: alternating markers
    // with 0x69 DSD idle payload.  The DAC stays in DSD mode and outputs
    // clean silence until the AudioUnit is fully stopped.
    // ─────────────────────────────────────────────────────────────────────

    // Fill the entire AudioBufferList with DoP silence.
    // Used for early-return paths (transitioning, swapping, not-running).
    static void fillDoPSilenceBuffer(AudioBufferList* ioData, Impl* self)
    {
        if (ioData->mNumberBuffers == 0 || self->format.channels <= 0) return;
        float* buf = static_cast<float*>(ioData->mBuffers[0].mData);
        int ch = self->format.channels;
        int totalFrames = (int)(ioData->mBuffers[0].mDataByteSize / (ch * sizeof(float)));
        bool m = self->dopMarker.load(std::memory_order_relaxed);
        for (int f = 0; f < totalFrames; f++) {
            uint8_t mb = m ? 0xFA : 0x05;
            m = !m;
            // DoP word: [marker][0x69][0x69] — DSD idle pattern
            uint32_t dopWord = ((uint32_t)mb << 16) | 0x6969;
            int32_t signed24 = (int32_t)dopWord;
            if (signed24 & 0x800000) signed24 |= (int32_t)0xFF000000;
            float sample = (float)signed24 / 8388608.0f;
            for (int c = 0; c < ch; c++)
                buf[f * ch + c] = sample;
        }
        self->dopMarker.store(m, std::memory_order_relaxed);
    }

    // Fill the tail of a buffer (from startFrame to endFrame) with DoP silence.
    // Used after renderCb returns fewer frames than requested.
    static void fillDoPSilenceTail(float* buf, int startFrame, int endFrame,
                                   int channels, Impl* self)
    {
        if (startFrame >= endFrame || channels <= 0) return;
        bool m = self->dopMarker.load(std::memory_order_relaxed);
        for (int f = startFrame; f < endFrame; f++) {
            uint8_t mb = m ? 0xFA : 0x05;
            m = !m;
            uint32_t dopWord = ((uint32_t)mb << 16) | 0x6969;
            int32_t signed24 = (int32_t)dopWord;
            if (signed24 & 0x800000) signed24 |= (int32_t)0xFF000000;
            float sample = (float)signed24 / 8388608.0f;
            for (int c = 0; c < channels; c++)
                buf[f * channels + c] = sample;
        }
        self->dopMarker.store(m, std::memory_order_relaxed);
    }

    static OSStatus renderCallback(void* inRefCon,
                                   AudioUnitRenderActionFlags* ioActionFlags,
                                   const AudioTimeStamp* inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList* ioData)
    {
        (void)ioActionFlags;
        (void)inTimeStamp;
        (void)inBusNumber;

        // Fill with PCM silence by default (safe fallback)
        for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
            std::memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        }

        auto* self = static_cast<Impl*>(inRefCon);

        // Safety checks — bail out early if shutting down
        if (!self) return noErr;
        if (self->destroyed.load(std::memory_order_acquire)) return noErr;

        bool isDoP = self->dopPassthrough.load(std::memory_order_relaxed);

        if (!self->running.load(std::memory_order_acquire)) {
            // During stop(): output DoP silence so DAC doesn't see markerless
            // zeros and exit DoP mode with a crackle
            if (isDoP) fillDoPSilenceBuffer(ioData, self);
            return noErr;
        }

        // If main thread is swapping the callback, output silence this cycle
        if (self->swappingCallback.load(std::memory_order_acquire)) {
            if (isDoP) fillDoPSilenceBuffer(ioData, self);
            return noErr;
        }

        // If transitioning between tracks (format change), output silence
        if (self->transitioning.load(std::memory_order_acquire)) {
            if (isDoP) fillDoPSilenceBuffer(ioData, self);
            return noErr;
        }

        float* outBuf = static_cast<float*>(ioData->mBuffers[0].mData);
        int totalSamples = inNumberFrames * self->format.channels;

        int framesRead = 0;
        {
            // try_lock — never block the realtime audio thread
            std::unique_lock<std::mutex> lock(self->cbMutex, std::try_to_lock);
            if (lock.owns_lock() && self->renderCb) {
                framesRead = self->renderCb(outBuf, (int)inNumberFrames);
            }
            // If lock failed, framesRead stays 0 → silence this cycle
        }

        // Fill remaining samples with appropriate silence
        if (isDoP) {
            // Advance marker tracker by actual DoP frames decoded
            if (framesRead > 0 && (framesRead % 2))
                self->dopMarker.store(!self->dopMarker.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
            // Fill tail with DoP silence (valid markers + idle payload)
            // so DAC stays in DSD mode instead of seeing PCM zeros
            if (framesRead < (int)inNumberFrames)
                fillDoPSilenceTail(outBuf, framesRead, (int)inNumberFrames,
                                   self->format.channels, self);
        } else {
            // PCM: zero remaining samples
            if (framesRead < (int)inNumberFrames) {
                int samplesWritten = framesRead * self->format.channels;
                std::memset(outBuf + samplesWritten, 0,
                            (totalSamples - samplesWritten) * sizeof(float));
            }
        }

        // Apply volume with smooth ramp (skip for DoP — scaling destroys DoP markers)
        if (!isDoP) {
            float vol = self->volume;
            float prev = self->prevVolume;
            if (prev != vol) {
                // Ramp from prevVolume to volume over this buffer
                int ch = self->format.channels;
                int frames = (int)inNumberFrames;
                if (ch > 0 && frames > 0) {
                    float step = (vol - prev) / (float)frames;
                    float g = prev;
                    for (int f = 0; f < frames; ++f) {
                        g += step;
                        for (int c = 0; c < ch; ++c)
                            outBuf[f * ch + c] *= g;
                    }
                }
                self->prevVolume = vol;
            } else if (vol < 1.0f) {
                for (int i = 0; i < totalSamples; ++i)
                    outBuf[i] *= vol;
            }
        }

        return noErr;
    }
};

CoreAudioOutput::CoreAudioOutput()
    : m_impl(std::make_unique<Impl>())
{
}

CoreAudioOutput::~CoreAudioOutput()
{
    // 1. Set ALL flags to stop — render callback checks these first
    m_impl->destroyed.store(true, std::memory_order_release);
    m_impl->running.store(false, std::memory_order_release);

    // 2. Clear the std::function callback immediately
    {
        std::lock_guard<std::mutex> lock(m_impl->cbMutex);
        m_impl->renderCb = nullptr;
    }

    auto& d = *m_impl;

    // Release hog mode before tearing down
    releaseHogMode();

    if (d.audioUnit) {
        // 3. Stop the audio unit
        AudioOutputUnitStop(d.audioUnit);

        // 4. Replace render callback with a pure-silence C function
        //    This ensures CoreAudio never calls back into our destroyed objects
        AURenderCallbackStruct silentCb = {};
        silentCb.inputProc = [](void*, AudioUnitRenderActionFlags*,
                                const AudioTimeStamp*, UInt32, UInt32,
                                AudioBufferList* ioData) -> OSStatus {
            for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
                memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
            }
            return noErr;
        };
        silentCb.inputProcRefCon = nullptr;
        AudioUnitSetProperty(d.audioUnit,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0,
            &silentCb, sizeof(silentCb));

        // 5. Wait for any in-flight callbacks to finish
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 6. Uninitialize and dispose
        AudioUnitUninitialize(d.audioUnit);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        AudioComponentInstanceDispose(d.audioUnit);
        d.audioUnit = nullptr;
    }
}

bool CoreAudioOutput::open(const AudioStreamFormat& format, uint32_t deviceId)
{
    close();

    auto& d = *m_impl;
    d.format = format;

    // Describe the output audio unit
    AudioComponentDescription desc = {};
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) return false;

    if (AudioComponentInstanceNew(comp, &d.audioUnit) != noErr)
        return false;

    // Set output device if specified, fall back to default on failure
    if (deviceId != 0) {
        AudioDeviceID devId = (AudioDeviceID)deviceId;
        OSStatus devErr = AudioUnitSetProperty(d.audioUnit,
                             kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global,
                             0, &devId, sizeof(devId));
        if (devErr != noErr) {
            fprintf(stderr, "CoreAudioOutput: Failed to set device %u (OSStatus %d), using default\n",
                    deviceId, (int)devErr);
        }
    }

    // Force the device nominal sample rate to match the stream format.
    // This is critical for DoP (DSD over PCM) playback where the DAC
    // must run at the exact DoP rate (e.g., 176400 Hz for DSD64).
    {
        AudioDeviceID currentDevice = 0;
        UInt32 devSize = sizeof(currentDevice);
        AudioUnitGetProperty(d.audioUnit,
                             kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global,
                             0, &currentDevice, &devSize);

        if (currentDevice == 0) {
            // Get system default output device
            AudioObjectPropertyAddress defaultProp = {
                kAudioHardwarePropertyDefaultOutputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 sz = sizeof(currentDevice);
            AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                        0, nullptr, &sz, &currentDevice);
        }

        if (currentDevice != 0) {
            AudioObjectPropertyAddress rateProp = {
                kAudioDevicePropertyNominalSampleRate,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };

            Float64 currentRate = 0.0;
            UInt32 rateSize = sizeof(currentRate);
            AudioObjectGetPropertyData(currentDevice, &rateProp,
                                        0, nullptr, &rateSize, &currentRate);

            bool builtIn = isBuiltInDevice(currentDevice);
            Float64 targetRate = format.sampleRate;

            fprintf(stderr, "[CoreAudio] open() requested rate: %.0f  current device rate: %.0f  deviceId: %u  built-in: %s  bit-perfect: %s\n",
                    targetRate, currentRate, (unsigned)currentDevice, builtIn ? "YES" : "NO",
                    d.bitPerfect ? "YES" : "NO");

            if (std::abs(currentRate - targetRate) > 0.5) {
                if (builtIn && !d.bitPerfect) {
                    fprintf(stderr, "CoreAudioOutput: Built-in device, skipping sample rate switch %.0f -> %.0f Hz\n",
                            currentRate, targetRate);
                    // CoreAudio will resample internally for built-in speakers
                } else {
                    fprintf(stderr, "CoreAudioOutput: Switching device sample rate %.0f -> %.0f Hz\n",
                            currentRate, targetRate);
                    OSStatus rateErr = AudioObjectSetPropertyData(currentDevice, &rateProp,
                                                                   0, nullptr, sizeof(targetRate), &targetRate);
                    if (rateErr != noErr) {
                        fprintf(stderr, "CoreAudioOutput: Failed to set sample rate (OSStatus %d)\n",
                                (int)rateErr);
                    } else {
                        // Wait for the hardware to stabilize at the new rate
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));

                        // Verify the rate was set
                        Float64 actualRate = 0.0;
                        AudioObjectGetPropertyData(currentDevice, &rateProp,
                                                    0, nullptr, &rateSize, &actualRate);
                        fprintf(stderr, "CoreAudioOutput: Device sample rate now: %.0f Hz\n", actualRate);
                    }
                }
            }
        }
    }

    // Set stream format: interleaved Float32
    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate       = format.sampleRate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel   = 32;
    asbd.mChannelsPerFrame = format.channels;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = sizeof(float) * format.channels;
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;

    if (AudioUnitSetProperty(d.audioUnit,
                             kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input,
                             0, &asbd, sizeof(asbd)) != noErr) {
        AudioComponentInstanceDispose(d.audioUnit);
        d.audioUnit = nullptr;
        return false;
    }

    // Set channel layout for multichannel output
    {
        AudioChannelLayoutTag layoutTag;
        switch (format.channels) {
        case 1:  layoutTag = kAudioChannelLayoutTag_Mono; break;
        case 2:  layoutTag = kAudioChannelLayoutTag_Stereo; break;
        case 3:  layoutTag = kAudioChannelLayoutTag_MPEG_3_0_A; break;
        case 4:  layoutTag = kAudioChannelLayoutTag_Quadraphonic; break;
        case 6:  layoutTag = kAudioChannelLayoutTag_MPEG_5_1_A; break;
        case 8:  layoutTag = kAudioChannelLayoutTag_MPEG_7_1_A; break;
        default: layoutTag = kAudioChannelLayoutTag_DiscreteInOrder | format.channels; break;
        }

        AudioChannelLayout layout = {};
        layout.mChannelLayoutTag = layoutTag;

        OSStatus err = AudioUnitSetProperty(d.audioUnit,
                                            kAudioUnitProperty_AudioChannelLayout,
                                            kAudioUnitScope_Input,
                                            0, &layout, sizeof(layout));
        if (err != noErr) {
            qDebug() << "[CoreAudio] Channel layout set failed for" << format.channels
                     << "ch (non-fatal, continuing)";
        }
    }

    // Set render callback
    AURenderCallbackStruct callbackStruct = {};
    callbackStruct.inputProc       = Impl::renderCallback;
    callbackStruct.inputProcRefCon = m_impl.get();

    if (AudioUnitSetProperty(d.audioUnit,
                             kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input,
                             0, &callbackStruct, sizeof(callbackStruct)) != noErr) {
        AudioComponentInstanceDispose(d.audioUnit);
        d.audioUnit = nullptr;
        return false;
    }

    if (AudioUnitInitialize(d.audioUnit) != noErr) {
        AudioComponentInstanceDispose(d.audioUnit);
        d.audioUnit = nullptr;
        return false;
    }

    return true;
}

bool CoreAudioOutput::start()
{
    auto& d = *m_impl;
    if (!d.audioUnit) return false;
    if (d.running.load(std::memory_order_acquire)) return true;

    // Flush CoreAudio's internal ring buffers to prevent stale data
    // from the previous format/rate bleeding into the new stream.
    // Critical for DSD↔PCM transitions where sample rate changes dramatically.
    OSStatus resetErr = AudioUnitReset(d.audioUnit, kAudioUnitScope_Global, 0);
    if (resetErr != noErr) {
        fprintf(stderr, "CoreAudioOutput::start: AudioUnitReset failed (OSStatus %d)\n",
                (int)resetErr);
    }

    if (AudioOutputUnitStart(d.audioUnit) != noErr)
        return false;

    d.running.store(true, std::memory_order_release);
    d.transitioning.store(false, std::memory_order_release);
    return true;
}

void CoreAudioOutput::stop()
{
    auto& d = *m_impl;
    if (!d.audioUnit || !d.running.load(std::memory_order_acquire)) return;

    d.running.store(false, std::memory_order_release);
    AudioOutputUnitStop(d.audioUnit);

    // Wait for any in-flight render callbacks to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void CoreAudioOutput::close()
{
    auto& d = *m_impl;
    stop();
    releaseHogMode();
    if (d.audioUnit && !d.destroyed.load(std::memory_order_acquire)) {
        AudioUnitUninitialize(d.audioUnit);
        AudioComponentInstanceDispose(d.audioUnit);
        d.audioUnit = nullptr;
    }
}

bool CoreAudioOutput::isRunning() const
{
    return m_impl->running.load(std::memory_order_acquire);
}

void CoreAudioOutput::setRenderCallback(RenderCallback cb)
{
    // Signal the render callback to skip this cycle so it won't block on the mutex
    m_impl->swappingCallback.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_impl->cbMutex);
        m_impl->renderCb = std::move(cb);
    }
    m_impl->swappingCallback.store(false, std::memory_order_release);
}

void CoreAudioOutput::setVolume(float vol)
{
    m_impl->volume = (vol < 0.0f) ? 0.0f : (vol > 1.0f ? 1.0f : vol);
}

bool CoreAudioOutput::setBufferSize(uint32_t frames)
{
    auto& d = *m_impl;
    if (!d.audioUnit) return false;

    bool wasRunning = d.running.load(std::memory_order_acquire);
    if (wasRunning) stop();

    // Get the current device from the AudioUnit
    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(d.audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);

    OSStatus err = noErr;

    // Set buffer size on the device directly (more reliable)
    if (currentDevice != 0) {
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        // Query supported range and clamp
        AudioObjectPropertyAddress rangeProp = {
            kAudioDevicePropertyBufferFrameSizeRange,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioValueRange range = {0, 0};
        UInt32 rangeSize = sizeof(range);
        OSStatus rangeErr = AudioObjectGetPropertyData(currentDevice, &rangeProp,
                                                        0, nullptr, &rangeSize, &range);
        if (rangeErr == noErr && range.mMinimum > 0 && range.mMaximum > 0) {
            UInt32 clamped = frames;
            if (clamped < static_cast<UInt32>(range.mMinimum))
                clamped = static_cast<UInt32>(range.mMinimum);
            if (clamped > static_cast<UInt32>(range.mMaximum))
                clamped = static_cast<UInt32>(range.mMaximum);
            if (clamped != frames) {
                fprintf(stderr, "CoreAudioOutput: clamped buffer size %u -> %u (range %u–%u)\n",
                        frames, clamped,
                        (unsigned)range.mMinimum, (unsigned)range.mMaximum);
                frames = clamped;
            }
        }

        UInt32 bufSize = static_cast<UInt32>(frames);
        err = AudioObjectSetPropertyData(currentDevice, &prop,
                                          0, nullptr, sizeof(UInt32), &bufSize);
        fprintf(stderr, "CoreAudioOutput: setBufferSize(%u) on device %u -> OSStatus %d\n",
                frames, (unsigned)currentDevice, (int)err);

        // Read back actual value
        UInt32 actual = 0;
        UInt32 actualSize = sizeof(actual);
        AudioObjectGetPropertyData(currentDevice, &prop, 0, nullptr, &actualSize, &actual);
        fprintf(stderr, "CoreAudioOutput: actual buffer size after set: %u\n", (unsigned)actual);
    } else {
        fprintf(stderr, "CoreAudioOutput: setBufferSize(%u) — no current device!\n", frames);
    }

    // Also set on the AudioUnit
    UInt32 bufferSize = static_cast<UInt32>(frames);
    OSStatus auErr = AudioUnitSetProperty(d.audioUnit,
                                           kAudioDevicePropertyBufferFrameSize,
                                           kAudioUnitScope_Global,
                                           0, &bufferSize, sizeof(bufferSize));
    fprintf(stderr, "CoreAudioOutput: AudioUnit setBufferSize -> OSStatus %d\n", (int)auErr);

    if (wasRunning) start();
    return err == noErr || auErr == noErr;
}

bool CoreAudioOutput::setSampleRate(double rate)
{
    auto& d = *m_impl;
    if (!d.audioUnit) return false;

    bool wasRunning = d.running.load(std::memory_order_acquire);
    if (wasRunning) stop();

    fprintf(stderr, "CoreAudioOutput: setSampleRate(%.0f)\n", rate);

    // Uninitialize the AudioUnit before reconfiguring
    AudioUnitUninitialize(d.audioUnit);

    // Set the device nominal sample rate
    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(d.audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);

    if (currentDevice != 0) {
        bool builtIn = isBuiltInDevice(currentDevice);
        if (builtIn && !m_impl->bitPerfect) {
            fprintf(stderr, "CoreAudioOutput: setSampleRate — built-in device, skipping nominal rate change\n");
        } else {
            AudioObjectPropertyAddress rateProp = {
                kAudioDevicePropertyNominalSampleRate,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            Float64 targetRate = rate;
            OSStatus rateErr = AudioObjectSetPropertyData(currentDevice, &rateProp,
                                                           0, nullptr, sizeof(targetRate), &targetRate);
            if (rateErr != noErr) {
                fprintf(stderr, "CoreAudioOutput: Failed to set device rate (OSStatus %d)\n", (int)rateErr);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    // Reconfigure the ASBD with the new rate
    d.format.sampleRate = rate;

    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate       = rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel   = 32;
    asbd.mChannelsPerFrame = d.format.channels;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = sizeof(float) * d.format.channels;
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;

    OSStatus err = AudioUnitSetProperty(d.audioUnit,
                                         kAudioUnitProperty_StreamFormat,
                                         kAudioUnitScope_Input,
                                         0, &asbd, sizeof(asbd));
    fprintf(stderr, "CoreAudioOutput: setSampleRate ASBD -> OSStatus %d\n", (int)err);

    OSStatus initErr = AudioUnitInitialize(d.audioUnit);
    if (initErr != noErr)
        fprintf(stderr, "CoreAudioOutput: AudioUnitInitialize failed: %d\n", (int)initErr);

    if (wasRunning) start();
    return err == noErr;
}

bool CoreAudioOutput::setDevice(uint32_t deviceId)
{
    auto& d = *m_impl;
    if (!d.audioUnit) return false;

    bool wasRunning = d.running.load(std::memory_order_acquire);
    if (wasRunning) stop();

    AudioDeviceID devId = (AudioDeviceID)deviceId;
    OSStatus err = AudioUnitSetProperty(d.audioUnit,
                                        kAudioOutputUnitProperty_CurrentDevice,
                                        kAudioUnitScope_Global,
                                        0, &devId, sizeof(devId));

    if (wasRunning) start();
    return err == noErr;
}

double CoreAudioOutput::getMaxSampleRate(uint32_t deviceId) const { return getMaxSampleRateStatic(deviceId); }

double CoreAudioOutput::getMaxSampleRateStatic(uint32_t deviceId)
{
    AudioDeviceID devId = (AudioDeviceID)deviceId;

    // If no device specified, get default output device
    if (devId == 0) {
        AudioObjectPropertyAddress prop = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(devId);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop,
                                        0, nullptr, &sz, &devId) != noErr) {
            return 44100.0;
        }
    }

    // Query available nominal sample rates
    AudioObjectPropertyAddress rateProp = {
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(devId, &rateProp, 0, nullptr, &dataSize) != noErr) {
        return 44100.0;
    }

    int rangeCount = dataSize / sizeof(AudioValueRange);
    if (rangeCount == 0) return 44100.0;

    std::vector<AudioValueRange> ranges(rangeCount);
    if (AudioObjectGetPropertyData(devId, &rateProp, 0, nullptr,
                                    &dataSize, ranges.data()) != noErr) {
        return 44100.0;
    }

    double maxRate = 44100.0;
    for (const auto& range : ranges) {
        if (range.mMaximum > maxRate) {
            maxRate = range.mMaximum;
        }
    }

    return maxRate;
}

double CoreAudioOutput::findNearestSupportedRate(double targetRate, uint32_t deviceId) const { return findNearestSupportedRateStatic(targetRate, deviceId); }

double CoreAudioOutput::findNearestSupportedRateStatic(double targetRate, uint32_t deviceId)
{
    AudioDeviceID devId = (AudioDeviceID)deviceId;

    if (devId == 0) {
        AudioObjectPropertyAddress prop = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(devId);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop,
                                        0, nullptr, &sz, &devId) != noErr) {
            return targetRate;
        }
    }

    AudioObjectPropertyAddress rateProp = {
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(devId, &rateProp, 0, nullptr, &dataSize) != noErr)
        return targetRate;

    int rangeCount = dataSize / sizeof(AudioValueRange);
    if (rangeCount == 0) return targetRate;

    std::vector<AudioValueRange> ranges(rangeCount);
    if (AudioObjectGetPropertyData(devId, &rateProp, 0, nullptr,
                                    &dataSize, ranges.data()) != noErr)
        return targetRate;

    // Check for exact match first
    for (const auto& range : ranges) {
        if (targetRate >= range.mMinimum && targetRate <= range.mMaximum)
            return targetRate;
    }

    // Collect all discrete rate values and find nearest
    double nearest = 0;
    double minDiff = 1e15;
    for (const auto& range : ranges) {
        double candidates[] = { range.mMinimum, range.mMaximum };
        for (double candidate : candidates) {
            double diff = std::abs(candidate - targetRate);
            if (diff < minDiff) {
                minDiff = diff;
                nearest = candidate;
            }
        }
    }

    if (nearest > 0) {
        fprintf(stderr, "[Audio] Rate %.0f not supported, nearest: %.0f\n",
                targetRate, nearest);
    }
    return (nearest > 0) ? nearest : targetRate;
}

// ── Hog Mode (Exclusive Access) ─────────────────────────────────────

static AudioDeviceID getDeviceFromUnit(AudioComponentInstance audioUnit)
{
    if (!audioUnit) return 0;
    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);
    if (currentDevice == 0) {
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(currentDevice);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                    0, nullptr, &sz, &currentDevice);
    }
    return currentDevice;
}

bool CoreAudioOutput::setHogMode(bool enabled)
{
    auto& d = *m_impl;
    AudioDeviceID currentDevice = getDeviceFromUnit(d.audioUnit);
    if (currentDevice == 0) {
        fprintf(stderr, "CoreAudioOutput::setHogMode: no device available\n");
        return false;
    }

    AudioObjectPropertyAddress hogProp = {
        kAudioDevicePropertyHogMode,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    if (enabled) {
        // Acquire hog mode — set PID to our process
        pid_t pid = getpid();
        OSStatus err = AudioObjectSetPropertyData(currentDevice, &hogProp,
                                                   0, nullptr, sizeof(pid), &pid);
        if (err != noErr) {
            fprintf(stderr, "CoreAudioOutput::setHogMode: failed to acquire hog mode (OSStatus %d)\n", (int)err);
            return false;
        }
        // Verify
        pid_t hogPid = -1;
        UInt32 hogSize = sizeof(hogPid);
        AudioObjectGetPropertyData(currentDevice, &hogProp, 0, nullptr, &hogSize, &hogPid);
        bool success = (hogPid == getpid());
        fprintf(stderr, "CoreAudioOutput::setHogMode: hog mode %s (pid=%d, hogPid=%d)\n",
                success ? "ACQUIRED" : "FAILED", (int)getpid(), (int)hogPid);
        return success;
    } else {
        // Release hog mode — set PID to -1
        pid_t releasePid = -1;
        OSStatus err = AudioObjectSetPropertyData(currentDevice, &hogProp,
                                                   0, nullptr, sizeof(releasePid), &releasePid);
        if (err != noErr) {
            fprintf(stderr, "CoreAudioOutput::setHogMode: failed to release hog mode (OSStatus %d)\n", (int)err);
            return false;
        }
        fprintf(stderr, "CoreAudioOutput::setHogMode: hog mode RELEASED\n");
        return true;
    }
}

void CoreAudioOutput::releaseHogMode()
{
    auto& d = *m_impl;
    AudioDeviceID currentDevice = getDeviceFromUnit(d.audioUnit);
    if (currentDevice == 0) return;

    // Only release if we currently own it
    AudioObjectPropertyAddress hogProp = {
        kAudioDevicePropertyHogMode,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    pid_t hogPid = -1;
    UInt32 hogSize = sizeof(hogPid);
    if (AudioObjectGetPropertyData(currentDevice, &hogProp, 0, nullptr, &hogSize, &hogPid) == noErr) {
        if (hogPid == getpid()) {
            pid_t releasePid = -1;
            AudioObjectSetPropertyData(currentDevice, &hogProp,
                                        0, nullptr, sizeof(releasePid), &releasePid);
            fprintf(stderr, "CoreAudioOutput::releaseHogMode: released\n");
        }
    }
}

bool CoreAudioOutput::isHogModeSupported() const
{
    auto& d = *m_impl;
    AudioDeviceID currentDevice = getDeviceFromUnit(d.audioUnit);
    if (currentDevice == 0) return false;

    AudioObjectPropertyAddress hogProp = {
        kAudioDevicePropertyHogMode,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    return AudioObjectHasProperty(currentDevice, &hogProp);
}

std::string CoreAudioOutput::deviceName() const
{
    auto& d = *m_impl;
    if (!d.audioUnit) return {};

    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(d.audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);

    if (currentDevice == 0) {
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(currentDevice);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                    0, nullptr, &sz, &currentDevice);
    }

    if (currentDevice == 0) return "System Default";

    AudioObjectPropertyAddress nameProp = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef cfName = nullptr;
    UInt32 nameSize = sizeof(cfName);
    if (AudioObjectGetPropertyData(currentDevice, &nameProp, 0, nullptr,
                                    &nameSize, &cfName) == noErr && cfName) {
        char nameBuf[256] = {};
        CFStringGetCString(cfName, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
        CFRelease(cfName);
        return nameBuf;
    }
    return "Unknown Device";
}

double CoreAudioOutput::currentSampleRate() const
{
    auto& d = *m_impl;
    if (!d.audioUnit) return 0.0;

    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(d.audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);

    if (currentDevice == 0) {
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(currentDevice);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                    0, nullptr, &sz, &currentDevice);
    }

    if (currentDevice == 0) return 0.0;

    AudioObjectPropertyAddress rateProp = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    Float64 rate = 0.0;
    UInt32 rateSize = sizeof(rate);
    AudioObjectGetPropertyData(currentDevice, &rateProp,
                                0, nullptr, &rateSize, &rate);
    return rate;
}

double CoreAudioOutput::deviceNominalSampleRate() const
{
    auto& d = *m_impl;
    if (!d.audioUnit) return 0.0;

    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(d.audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);

    if (currentDevice == 0) {
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(currentDevice);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                    0, nullptr, &sz, &currentDevice);
    }

    if (currentDevice == 0) return 0.0;

    AudioObjectPropertyAddress rateProp = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    Float64 rate = 0.0;
    UInt32 rateSize = sizeof(rate);
    if (AudioObjectGetPropertyData(currentDevice, &rateProp,
                                    0, nullptr, &rateSize, &rate) == noErr) {
        return rate;
    }
    return 0.0;
}

bool CoreAudioOutput::isExclusiveMode() const
{
    auto& d = *m_impl;
    if (!d.audioUnit) return false;

    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(d.audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);

    if (currentDevice == 0) return false;

    // Check hog mode PID — if it matches our process, we have exclusive access
    AudioObjectPropertyAddress hogProp = {
        kAudioDevicePropertyHogMode,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    pid_t hogPid = -1;
    UInt32 hogSize = sizeof(hogPid);
    if (AudioObjectGetPropertyData(currentDevice, &hogProp,
                                    0, nullptr, &hogSize, &hogPid) == noErr) {
        return hogPid == getpid();
    }
    return false;
}

bool CoreAudioOutput::isBuiltInDevice(uint32_t deviceId) const { return isBuiltInDeviceStatic(deviceId); }

bool CoreAudioOutput::isBuiltInDeviceStatic(uint32_t deviceId)
{
    AudioDeviceID devId = (AudioDeviceID)deviceId;

    // Resolve default device if needed
    if (devId == 0) {
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(devId);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                        0, nullptr, &sz, &devId) != noErr) {
            return false;
        }
    }

    // Check transport type
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyTransportType,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 transportType = 0;
    UInt32 size = sizeof(transportType);
    if (AudioObjectGetPropertyData(devId, &addr, 0, nullptr, &size, &transportType) == noErr) {
        return (transportType == kAudioDeviceTransportTypeBuiltIn);
    }

    return false;
}

bool CoreAudioOutput::isBuiltInOutput() const
{
    auto& d = *m_impl;
    if (!d.audioUnit) return false;

    AudioDeviceID currentDevice = 0;
    UInt32 devSize = sizeof(currentDevice);
    AudioUnitGetProperty(d.audioUnit,
                         kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global,
                         0, &currentDevice, &devSize);

    if (currentDevice == 0) {
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = sizeof(currentDevice);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                    0, nullptr, &sz, &currentDevice);
    }

    return isBuiltInDevice(currentDevice);
}

void CoreAudioOutput::setBitPerfectMode(bool enabled)
{
    m_impl->bitPerfect = enabled;
}

bool CoreAudioOutput::bitPerfectMode() const
{
    return m_impl->bitPerfect;
}

void CoreAudioOutput::setDoPPassthrough(bool enabled)
{
    m_impl->dopPassthrough.store(enabled, std::memory_order_release);
    if (enabled) {
        // Sync marker state with decoder start (always begins with 0x05 = false)
        m_impl->dopMarker.store(false, std::memory_order_relaxed);
    }
}

void CoreAudioOutput::setTransitioning(bool enabled)
{
    m_impl->transitioning.store(enabled, std::memory_order_release);
}

