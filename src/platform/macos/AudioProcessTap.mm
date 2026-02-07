#include "AudioProcessTap.h"
#include "../../core/dsp/DSPPipeline.h"
#include <QDebug>
#include <QTimer>

#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <Foundation/Foundation.h>

#include <vector>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <chrono>
#include <libproc.h>
#include <sys/proc_info.h>

// ═══════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════

static AudioObjectID getDefaultOutputDevice()
{
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 sz = sizeof(device);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &device);
    return device;
}

static CFStringRef copyDeviceUID(AudioObjectID deviceID)
{
    CFStringRef uid = nullptr;
    UInt32 sz = sizeof(uid);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(deviceID, &addr, 0, nullptr, &sz, &uid);
    return uid;  // caller must CFRelease
}

static AudioObjectID translatePIDToProcessObject(pid_t pid)
{
    AudioObjectID processObj = kAudioObjectUnknown;
    UInt32 sz = sizeof(processObj);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyTranslatePIDToProcessObject,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                               sizeof(pid), &pid, &sz, &processObj);
    return processObj;
}

static QVector<pid_t> findChildPids(pid_t parentPid)
{
    QVector<pid_t> children;
    int bufSize = proc_listallpids(nullptr, 0);
    if (bufSize <= 0) return children;

    std::vector<pid_t> allPids(bufSize);
    int count = proc_listallpids(allPids.data(), bufSize * (int)sizeof(pid_t));
    if (count <= 0) return children;

    for (int i = 0; i < count; ++i) {
        struct proc_bsdinfo info;
        int ret = proc_pidinfo(allPids[i], PROC_PIDTBSDINFO, 0,
                               &info, sizeof(info));
        if (ret == (int)sizeof(info)
            && info.pbi_ppid == (uint32_t)parentPid)
        {
            children.append(allPids[i]);
        }
    }
    return children;
}

// ═══════════════════════════════════════════════════════════════════════
// IOProc — runs on the real-time audio thread
// NO allocations, NO ObjC, NO locks, NO Qt calls.
// ═══════════════════════════════════════════════════════════════════════

struct TapIOData {
    DSPPipeline*       pipeline  = nullptr;
    std::vector<float> interleaved;      // pre-allocated
    int                channels  = 2;
};

// ═══════════════════════════════════════════════════════════════════════
// DSP Verification — one-time logging flags (thread-safe)
// ═══════════════════════════════════════════════════════════════════════
static std::atomic<uint64_t> g_tapFrameCounter{0};
static std::atomic<bool>     g_audioDetectedLogged{false};
static std::atomic<bool>     g_pipelineNullLogged{false};
static std::atomic<uint64_t> g_lastLogFrameCount{0};
static std::atomic<bool>     g_ioProcCalledLogged{false};  // One-time IOProc entry log
static std::atomic<bool>     g_bufferInfoLogged{false};     // One-time buffer config log
static std::atomic<bool>     g_silencePathLogged{false};    // One-time silence path log
static std::atomic<int>      g_ioProcCallCount{0};           // Count first N calls for debug
static std::atomic<int>      g_audioFlowLogCount{0};         // Log first N calls with audio
constexpr uint64_t           kLogIntervalFrames = 44100 * 5;  // ~5 seconds at 44.1kHz

static void resetTapVerificationCounters()
{
    g_tapFrameCounter.store(0, std::memory_order_relaxed);
    g_audioDetectedLogged.store(false, std::memory_order_relaxed);
    g_pipelineNullLogged.store(false, std::memory_order_relaxed);
    g_lastLogFrameCount.store(0, std::memory_order_relaxed);
    g_ioProcCalledLogged.store(false, std::memory_order_relaxed);
    g_bufferInfoLogged.store(false, std::memory_order_relaxed);
    g_silencePathLogged.store(false, std::memory_order_relaxed);
    g_ioProcCallCount.store(0, std::memory_order_relaxed);
    g_audioFlowLogCount.store(0, std::memory_order_relaxed);
}

static OSStatus tapIOProc(AudioObjectID           /*inDevice*/,
                          const AudioTimeStamp*   /*inNow*/,
                          const AudioBufferList*  inInputData,
                          const AudioTimeStamp*   /*inInputTime*/,
                          AudioBufferList*        outOutputData,
                          const AudioTimeStamp*   /*inOutputTime*/,
                          void*                   inClientData)
{
    // Log first 10 IOProc calls for debugging
    int callNum = g_ioProcCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (callNum <= 10) {
        fprintf(stderr, "[ProcessTap] IOProc call #%d: inBufs=%d outBufs=%d\n",
                callNum,
                inInputData ? (int)inInputData->mNumberBuffers : -1,
                outOutputData ? (int)outOutputData->mNumberBuffers : -1);
    }

    // One-time entry log to confirm IOProc is being called
    if (!g_ioProcCalledLogged.exchange(true)) {
        fprintf(stderr, "[ProcessTap] IOProc callback entered — CoreAudio is calling us\n");

        // ── DETAILED DIAGNOSTIC (one-time) ──────────────────────────
        fprintf(stderr, "[ProcessTap] === IOProc DIAGNOSTIC (one-time) ===\n");

        // Input analysis
        if (!inInputData) {
            fprintf(stderr, "[ProcessTap] inInputData: NULL pointer!\n");
        } else {
            fprintf(stderr, "[ProcessTap] inInputData buffers: %u\n", inInputData->mNumberBuffers);
            for (UInt32 i = 0; i < inInputData->mNumberBuffers && i < 4; i++) {
                fprintf(stderr, "[ProcessTap]   buf[%u]: data=%p size=%u channels=%u\n",
                        i,
                        inInputData->mBuffers[i].mData,
                        inInputData->mBuffers[i].mDataByteSize,
                        inInputData->mBuffers[i].mNumberChannels);
                // Check if data has audio
                if (inInputData->mBuffers[i].mData && inInputData->mBuffers[i].mDataByteSize > 0) {
                    float* samples = (float*)inInputData->mBuffers[i].mData;
                    int sampleCount = inInputData->mBuffers[i].mDataByteSize / sizeof(float);
                    float maxVal = 0;
                    for (int s = 0; s < sampleCount && s < 1024; s++) {
                        float v = samples[s] < 0 ? -samples[s] : samples[s];
                        if (v > maxVal) maxVal = v;
                    }
                    fprintf(stderr, "[ProcessTap]   buf[%u] peak: %.6f %s\n",
                            i, maxVal, maxVal > 0.0001f ? "HAS AUDIO" : "SILENCE");
                }
            }
        }

        // Output analysis
        if (!outOutputData) {
            fprintf(stderr, "[ProcessTap] outOutputData: NULL pointer!\n");
        } else {
            fprintf(stderr, "[ProcessTap] outOutputData buffers: %u\n", outOutputData->mNumberBuffers);
            for (UInt32 i = 0; i < outOutputData->mNumberBuffers && i < 4; i++) {
                fprintf(stderr, "[ProcessTap]   out[%u]: data=%p size=%u channels=%u\n",
                        i,
                        outOutputData->mBuffers[i].mData,
                        outOutputData->mBuffers[i].mDataByteSize,
                        outOutputData->mBuffers[i].mNumberChannels);
            }
        }
        fprintf(stderr, "[ProcessTap] === END DIAGNOSTIC ===\n");
    }

    auto* io = static_cast<TapIOData*>(inClientData);

    if (!outOutputData || outOutputData->mNumberBuffers == 0)
        return noErr;

    // ── No input → silence ──────────────────────────────────────────
    if (!inInputData || inInputData->mNumberBuffers == 0) {
        if (!g_silencePathLogged.exchange(true)) {
            fprintf(stderr, "[ProcessTap] IOProc: No input data (silence path), outBuffers=%u\n",
                    outOutputData->mNumberBuffers);
        }
        for (UInt32 i = 0; i < outOutputData->mNumberBuffers; ++i)
            memset(outOutputData->mBuffers[i].mData, 0,
                   outOutputData->mBuffers[i].mDataByteSize);
        return noErr;
    }

    // One-time log of buffer configuration
    if (!g_bufferInfoLogged.exchange(true)) {
        UInt32 inBytes = inInputData->mBuffers[0].mDataByteSize;
        UInt32 inChans = inInputData->mBuffers[0].mNumberChannels;
        fprintf(stderr, "[ProcessTap] Buffer config: inBufs=%u outBufs=%u bytes=%u chansPerBuf=%u\n",
                inInputData->mNumberBuffers, outOutputData->mNumberBuffers, inBytes, inChans);
    }

    const int ch = io->channels;

    // ── Deinterleaved (one buffer per channel) ──────────────────────
    if (inInputData->mNumberBuffers >= (UInt32)ch &&
        outOutputData->mNumberBuffers >= (UInt32)ch)
    {
        const UInt32 framesPerBuf =
            inInputData->mBuffers[0].mDataByteSize / sizeof(float);
        const UInt32 totalSamples = framesPerBuf * (UInt32)ch;

        float* buf = io->interleaved.data();
        if (io->interleaved.size() < totalSamples)
            return noErr;  // safety — buffer too small (should never happen)

        // Interleave input channels
        for (UInt32 f = 0; f < framesPerBuf; ++f)
            for (int c = 0; c < ch; ++c)
                buf[f * ch + c] =
                    static_cast<float*>(inInputData->mBuffers[c].mData)[f];

        // DSP
        if (io->pipeline) {
            io->pipeline->process(buf, (int)framesPerBuf, ch);
        } else if (!g_pipelineNullLogged.exchange(true)) {
            // One-time warning: DSP pipeline not set
            fprintf(stderr, "[ProcessTap] WARNING: DSP pipeline is null, audio passing through unprocessed\n");
        }

        // ── DSP Verification: detect audio and count frames ─────────────
        g_tapFrameCounter.fetch_add(framesPerBuf, std::memory_order_relaxed);

        // One-time audio detection log (check peak sample)
        if (!g_audioDetectedLogged.load(std::memory_order_relaxed)) {
            float peak = 0.0f;
            for (UInt32 s = 0; s < totalSamples && peak < 0.001f; ++s)
                peak = std::max(peak, std::fabs(buf[s]));
            if (peak >= 0.001f && !g_audioDetectedLogged.exchange(true)) {
                fprintf(stderr, "[ProcessTap] Audio detected — peak: %.6f, DSP routing active\n", peak);
            }
        }

        // Throttled frame counter log (~5 seconds)
        uint64_t currentFrames = g_tapFrameCounter.load(std::memory_order_relaxed);
        uint64_t lastLog = g_lastLogFrameCount.load(std::memory_order_relaxed);
        if (currentFrames - lastLog >= kLogIntervalFrames) {
            if (g_lastLogFrameCount.compare_exchange_weak(lastLog, currentFrames)) {
                fprintf(stderr, "[ProcessTap] Frames processed: %llu (~%.1f sec)\n",
                        currentFrames, (double)currentFrames / 44100.0);
            }
        }

        // Write SILENCE to output (CATapUnmuted: audio plays via direct path)
        for (UInt32 c = 0; c < outOutputData->mNumberBuffers; ++c)
            memset(outOutputData->mBuffers[c].mData, 0,
                   outOutputData->mBuffers[c].mDataByteSize);

        // ── Audio flow diagnostic: check input and DSP-processed peaks ──────
        {
            int flowNum = g_audioFlowLogCount.load(std::memory_order_relaxed);
            if (flowNum < 5) {
                // Compute input peak (from original input buffer)
                float inPeak = 0.0f;
                for (UInt32 f = 0; f < framesPerBuf && f < 1024; ++f) {
                    float v = std::fabs(static_cast<float*>(inInputData->mBuffers[0].mData)[f]);
                    if (v > inPeak) inPeak = v;
                }
                // Compute DSP-processed peak (from interleaved buffer)
                float dspPeak = 0.0f;
                for (UInt32 s = 0; s < totalSamples && s < 2048; ++s) {
                    float v = std::fabs(buf[s]);
                    if (v > dspPeak) dspPeak = v;
                }
                if (inPeak > 0.0001f || dspPeak > 0.0001f) {
                    flowNum = g_audioFlowLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (flowNum <= 5) {
                        fprintf(stderr, "[ProcessTap] AUDIO FLOW #%d: inPeak=%.6f dspPeak=%.6f "
                                "frames=%u chans=%d inBufs=%u outBufs=%u\n",
                                flowNum, inPeak, dspPeak, framesPerBuf, ch,
                                inInputData->mNumberBuffers, outOutputData->mNumberBuffers);
                    }
                }
            }
        }
    }
    // ── Interleaved (single buffer) ─────────────────────────────────
    else if (inInputData->mNumberBuffers == 1 &&
             outOutputData->mNumberBuffers == 1)
    {
        UInt32 bytes = inInputData->mBuffers[0].mDataByteSize;
        UInt32 frames = bytes / (sizeof(float) * ch);
        float* inBuf = static_cast<float*>(inInputData->mBuffers[0].mData);
        UInt32 totalSamples = frames * (UInt32)ch;

        // One-time interleaved path diagnostic
        static std::atomic<bool> g_interleavedDiagLogged{false};
        if (!g_interleavedDiagLogged.exchange(true)) {
            float inMax = 0.0f;
            int nonZero = 0;
            for (UInt32 s = 0; s < totalSamples && s < 2048; s++) {
                float v = inBuf[s] < 0 ? -inBuf[s] : inBuf[s];
                if (v > inMax) inMax = v;
                if (v > 0.0001f) nonZero++;
            }
            fprintf(stderr, "[ProcessTap] INTERLEAVED DIAG: frames=%u ch=%d bytes=%u "
                    "inPeak=%.6f nonZeroSamples=%d/%u\n",
                    frames, ch, bytes, inMax, nonZero, totalSamples);
        }

        // Copy input to temp buffer and process through DSP pipeline
        if (io->pipeline && io->interleaved.size() >= totalSamples) {
            memcpy(io->interleaved.data(), inBuf, bytes);
            io->pipeline->process(io->interleaved.data(), (int)frames, ch);
        } else if (!g_pipelineNullLogged.exchange(true)) {
            fprintf(stderr, "[ProcessTap] WARNING: DSP pipeline is null, audio passing through unprocessed\n");
        }

        // Write SILENCE to output (CATapUnmuted: audio plays via direct path)
        memset(outOutputData->mBuffers[0].mData, 0,
               outOutputData->mBuffers[0].mDataByteSize);

        // ── DSP Verification: detect audio and count frames ─────────────
        g_tapFrameCounter.fetch_add(frames, std::memory_order_relaxed);

        if (!g_audioDetectedLogged.load(std::memory_order_relaxed)) {
            float peak = 0.0f;
            for (UInt32 s = 0; s < totalSamples && peak < 0.001f; ++s)
                peak = std::max(peak, std::fabs(inBuf[s]));
            if (peak >= 0.001f && !g_audioDetectedLogged.exchange(true)) {
                fprintf(stderr, "[ProcessTap] Audio detected — peak: %.6f, DSP routing active\n", peak);
            }
        }

        uint64_t currentFrames = g_tapFrameCounter.load(std::memory_order_relaxed);
        uint64_t lastLog = g_lastLogFrameCount.load(std::memory_order_relaxed);
        if (currentFrames - lastLog >= kLogIntervalFrames) {
            if (g_lastLogFrameCount.compare_exchange_weak(lastLog, currentFrames)) {
                fprintf(stderr, "[ProcessTap] Frames processed: %llu (~%.1f sec)\n",
                        currentFrames, (double)currentFrames / 44100.0);
            }
        }
    }
    // ── Unexpected layout — write silence ────────────────
    else {
        for (UInt32 i = 0; i < outOutputData->mNumberBuffers; ++i) {
            memset(outOutputData->mBuffers[i].mData, 0,
                   outOutputData->mBuffers[i].mDataByteSize);
        }
    }

    return noErr;
}

// ═══════════════════════════════════════════════════════════════════════
// Impl
// ═══════════════════════════════════════════════════════════════════════

class AudioProcessTap::Impl {
public:
    AudioObjectID        aggregateID = kAudioObjectUnknown;
    AudioObjectID        tapID       = kAudioObjectUnknown;  // The created tap object
    AudioDeviceIOProcID  ioProcID    = nullptr;
    TapIOData            ioData;
    bool                 active      = false;
    bool                 prepared    = false;  // Resources pre-created, ready for fast start
    Float64              deviceRate  = 44100.0;
    NSString*            tapUUID     = nil;
    int                  stallCount  = 0;
    bool                 recreating  = false;
};

// ═══════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════

AudioProcessTap* AudioProcessTap::instance()
{
    static AudioProcessTap s;
    return &s;
}

AudioProcessTap::AudioProcessTap(QObject* parent)
    : QObject(parent)
    , d(new Impl)
{
}

AudioProcessTap::~AudioProcessTap()
{
    stop();
    delete d;
}

bool AudioProcessTap::isSupported() const
{
    // CATapDescription + aggregate tap support require macOS 14.2+
    if (@available(macOS 14.2, *)) {
        return true;
    }
    return false;
}

bool AudioProcessTap::isActive() const
{
    return d->active;
}

bool AudioProcessTap::isPrepared() const
{
    return d->prepared;
}

void AudioProcessTap::setDSPPipeline(DSPPipeline* pipeline)
{
    d->ioData.pipeline = pipeline;
}

void AudioProcessTap::prepareForPlayback()
{
    if (@available(macOS 14.2, *)) {
        if (d->prepared || d->active) {
            qDebug() << "[ProcessTap] Already prepared or active, skipping";
            return;
        }

        qDebug() << "[ProcessTap] Pre-creating tap for Apple Music...";
        auto prepareStart = std::chrono::high_resolution_clock::now();

        // ── 0. Cleanup any previous tap/aggregate ────
        if (d->tapID != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(d->tapID);
            d->tapID = kAudioObjectUnknown;
        }
        if (d->aggregateID != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(d->aggregateID);
            d->aggregateID = kAudioObjectUnknown;
        }

        // ── 1. Create CATapDescription (per-process → global fallback) ──
        pid_t myPid = getpid();
        QVector<pid_t> children = findChildPids(myPid);

        NSMutableArray* processObjects = [NSMutableArray array];
        AudioObjectID myObj = translatePIDToProcessObject(myPid);
        if (myObj != kAudioObjectUnknown)
            [processObjects addObject:@(myObj)];
        for (pid_t child : children) {
            AudioObjectID childObj = translatePIDToProcessObject(child);
            if (childObj != kAudioObjectUnknown) {
                [processObjects addObject:@(childObj)];
                char pathBuf[PROC_PIDPATHINFO_MAXSIZE];
                proc_pidpath(child, pathBuf, sizeof(pathBuf));
                qDebug() << "[ProcessTap] Prepare: including child PID:" << child
                         << QString::fromUtf8(pathBuf);
            }
        }

        CATapDescription* tapDesc;
        if (processObjects.count > 0) {
            tapDesc = [[CATapDescription alloc]
                initStereoMixdownOfProcesses:processObjects];
            qDebug() << "[ProcessTap] Prepare: per-process tap,"
                     << (int)processObjects.count << "processes";
        } else {
            tapDesc = [[CATapDescription alloc]
                initStereoGlobalTapButExcludeProcesses:@[]];
            qDebug() << "[ProcessTap] Prepare: global tap fallback";
        }
        tapDesc.name = @"SoranaFlow DSP Tap";
        tapDesc.privateTap = YES;
        tapDesc.muteBehavior = CATapUnmuted;

        NSUUID* uuid = [NSUUID UUID];
        tapDesc.UUID = uuid;
        d->tapUUID = [uuid UUIDString];

        // ── 2. Create the tap object ─────────────────────────────────
        OSStatus tapErr = AudioHardwareCreateProcessTap(tapDesc, &d->tapID);
        if (tapErr != noErr || d->tapID == kAudioObjectUnknown) {
            qWarning() << "[ProcessTap] Prepare: Failed to create process tap:" << tapErr;
            return;
        }

        // Get the tap's actual UID
        CFStringRef tapUID = nullptr;
        UInt32 tapUIDSize = sizeof(tapUID);
        AudioObjectPropertyAddress tapUIDAddr = {
            kAudioTapPropertyUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        OSStatus uidErr = AudioObjectGetPropertyData(d->tapID, &tapUIDAddr,
                                                      0, nullptr, &tapUIDSize, &tapUID);
        if (uidErr == noErr && tapUID) {
            d->tapUUID = (__bridge NSString*)tapUID;
        }

        // ── 3. Get default output device ─────────────────────────────
        AudioObjectID outputDevice = getDefaultOutputDevice();
        if (outputDevice == kAudioObjectUnknown) {
            qWarning() << "[ProcessTap] Prepare: No default output device";
            AudioHardwareDestroyProcessTap(d->tapID);
            d->tapID = kAudioObjectUnknown;
            return;
        }

        CFStringRef outputUID = copyDeviceUID(outputDevice);
        if (!outputUID) {
            qWarning() << "[ProcessTap] Prepare: Failed to get output device UID";
            AudioHardwareDestroyProcessTap(d->tapID);
            d->tapID = kAudioObjectUnknown;
            return;
        }

        // ── 4. Create aggregate device ───────────────────────────────
        NSString* aggUID = [NSString stringWithFormat:@"com.soranaflow.tap.%@",
                            [[NSUUID UUID] UUIDString]];

        NSDictionary* aggDesc = @{
            @"name"         : @"SoranaFlow DSP Tap",
            @"uid"          : aggUID,
            @"private"      : @YES,
            @"subdevices"   : @[
                @{ @"uid" : (__bridge NSString*)outputUID }
            ],
            @"taps"         : @[
                @{ @"uid" : d->tapUUID }
            ],
            @"tapautostart" : @YES
        };

        OSStatus err = AudioHardwareCreateAggregateDevice(
            (__bridge CFDictionaryRef)aggDesc, &d->aggregateID);
        CFRelease(outputUID);

        if (err != noErr) {
            qWarning() << "[ProcessTap] Prepare: Failed to create aggregate device:" << err;
            AudioHardwareDestroyProcessTap(d->tapID);
            d->tapID = kAudioObjectUnknown;
            return;
        }

        // ── 5. Query sample rate + channels ──────────────────────────
        UInt32 sz = sizeof(d->deviceRate);
        AudioObjectPropertyAddress rateAddr = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(d->aggregateID, &rateAddr,
                                   0, nullptr, &sz, &d->deviceRate);
        if (d->deviceRate <= 0) d->deviceRate = 44100.0;

        int channels = 2;
        d->ioData.channels = channels;
        d->ioData.interleaved.resize(8192 * channels, 0.0f);

        d->prepared = true;

        auto prepareEnd = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(prepareEnd - prepareStart);
        qDebug() << "[ProcessTap] Tap pre-created in" << duration.count() << "ms, ready for instant start";
        emit tapPrepared();
    }
}

bool AudioProcessTap::start()
{
    if (@available(macOS 14.2, *)) {
        if (d->active) return true;

        // Reset verification counters for fresh logging
        resetTapVerificationCounters();

        auto startTime = std::chrono::high_resolution_clock::now();

        // ── Fast path: use pre-created resources ─────────────────────
        if (d->prepared && d->aggregateID != kAudioObjectUnknown) {
            qDebug() << "[ProcessTap] Using pre-created tap, starting IOProc only";

            // Prepare DSP pipeline for tap sample rate
            if (d->ioData.pipeline)
                d->ioData.pipeline->prepare(d->deviceRate, d->ioData.channels);

            // Create and start IOProc
            OSStatus err = AudioDeviceCreateIOProcID(d->aggregateID, tapIOProc,
                                            &d->ioData, &d->ioProcID);
            if (err != noErr) {
                qWarning() << "[ProcessTap] Failed to create IOProc:" << err;
                // Clean up and fall through to full creation
                d->prepared = false;
            } else {
                err = AudioDeviceStart(d->aggregateID, d->ioProcID);
                if (err != noErr) {
                    qWarning() << "[ProcessTap] Failed to start IOProc:" << err;
                    AudioDeviceDestroyIOProcID(d->aggregateID, d->ioProcID);
                    d->ioProcID = nullptr;
                    d->prepared = false;
                } else {
                    d->active = true;
                    auto endTime = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    qDebug() << "[ProcessTap] IOProc started in" << duration.count() << "ms (fast path)";
                    emit tapStarted();
                    return true;
                }
            }
        }

        // ── Full creation path (not pre-prepared) ────────────────────
        qDebug() << "[ProcessTap] Starting global tap (full creation)";

        // ── 0. Cleanup any previous tap/aggregate (stall recovery) ────
        if (d->tapID != kAudioObjectUnknown) {
            qDebug() << "[ProcessTap] Destroying previous tap:" << d->tapID;
            AudioHardwareDestroyProcessTap(d->tapID);
            d->tapID = kAudioObjectUnknown;
        }
        if (d->aggregateID != kAudioObjectUnknown) {
            qDebug() << "[ProcessTap] Destroying previous aggregate:" << d->aggregateID;
            AudioHardwareDestroyAggregateDevice(d->aggregateID);
            d->aggregateID = kAudioObjectUnknown;
        }
        d->prepared = false;

        // ── 1. Create CATapDescription (per-process → global fallback) ──
        pid_t myPid = getpid();
        QVector<pid_t> children = findChildPids(myPid);

        NSMutableArray* processObjects = [NSMutableArray array];
        AudioObjectID myObj = translatePIDToProcessObject(myPid);
        if (myObj != kAudioObjectUnknown)
            [processObjects addObject:@(myObj)];
        for (pid_t child : children) {
            AudioObjectID childObj = translatePIDToProcessObject(child);
            if (childObj != kAudioObjectUnknown) {
                [processObjects addObject:@(childObj)];
                char pathBuf[PROC_PIDPATHINFO_MAXSIZE];
                proc_pidpath(child, pathBuf, sizeof(pathBuf));
                qDebug() << "[ProcessTap] Including child PID:" << child
                         << QString::fromUtf8(pathBuf);
            }
        }

        CATapDescription* tapDesc;
        if (processObjects.count > 0) {
            tapDesc = [[CATapDescription alloc]
                initStereoMixdownOfProcesses:processObjects];
            qDebug() << "[ProcessTap] Per-process tap:"
                     << (int)processObjects.count << "processes";
        } else {
            tapDesc = [[CATapDescription alloc]
                initStereoGlobalTapButExcludeProcesses:@[]];
            qDebug() << "[ProcessTap] Global tap fallback (no process objects)";
        }
        tapDesc.name = @"SoranaFlow DSP Tap";
        tapDesc.privateTap = YES;
        // CATapUnmuted: monitoring mode — audio goes direct to speakers
        // We get a low-level copy for visualization/metering only
        tapDesc.muteBehavior = CATapUnmuted;

        // Generate a stable UUID for the tap
        NSUUID* uuid = [NSUUID UUID];
        tapDesc.UUID = uuid;
        d->tapUUID = [uuid UUIDString];

        qDebug() << "[ProcessTap] Tap UUID:"
                 << QString::fromNSString(d->tapUUID);

        // ── 2. Create the actual tap object from the description ─────
        OSStatus tapErr = AudioHardwareCreateProcessTap(tapDesc, &d->tapID);
        if (tapErr != noErr || d->tapID == kAudioObjectUnknown) {
            qWarning() << "[ProcessTap] Failed to create process tap:" << tapErr;
            emit tapError(QStringLiteral("Failed to create process tap (error %1)").arg(tapErr));
            return false;
        }
        qDebug() << "[ProcessTap] Created tap object ID:" << d->tapID;

        // ── 2b. Get the tap's ACTUAL UID (not the description UUID) ──
        // The tap object has its own UID which must be used in the aggregate device
        CFStringRef tapUID = nullptr;
        UInt32 tapUIDSize = sizeof(tapUID);
        AudioObjectPropertyAddress tapUIDAddr = {
            kAudioTapPropertyUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        OSStatus uidErr = AudioObjectGetPropertyData(d->tapID, &tapUIDAddr,
                                                      0, nullptr, &tapUIDSize, &tapUID);
        if (uidErr != noErr || !tapUID) {
            qWarning() << "[ProcessTap] Failed to get tap UID:" << uidErr;
            // Fall back to description UUID
            qDebug() << "[ProcessTap] Using description UUID as fallback";
        } else {
            d->tapUUID = (__bridge NSString*)tapUID;
            qDebug() << "[ProcessTap] Tap actual UID:" << QString::fromNSString(d->tapUUID);
        }

        // ── 2c. Diagnostic: Check tap device's own streams ───────────
        {
            AudioObjectPropertyAddress tapInputAddr = {
                kAudioDevicePropertyStreams,
                kAudioObjectPropertyScopeInput,
                kAudioObjectPropertyElementMain
            };
            UInt32 tapInputSize = 0;
            AudioObjectGetPropertyDataSize(d->tapID, &tapInputAddr, 0, nullptr, &tapInputSize);
            qDebug() << "[ProcessTap] Tap device" << d->tapID
                     << "INPUT streams:" << (tapInputSize / sizeof(AudioStreamID));

            AudioObjectPropertyAddress tapOutputAddr = {
                kAudioDevicePropertyStreams,
                kAudioObjectPropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 tapOutputSize = 0;
            AudioObjectGetPropertyDataSize(d->tapID, &tapOutputAddr, 0, nullptr, &tapOutputSize);
            qDebug() << "[ProcessTap] Tap device" << d->tapID
                     << "OUTPUT streams:" << (tapOutputSize / sizeof(AudioStreamID));
        }

        // ── 3. Get default output device UID ────────────────────────
        AudioObjectID outputDevice = getDefaultOutputDevice();
        if (outputDevice == kAudioObjectUnknown) {
            qWarning() << "[ProcessTap] No default output device";
            emit tapError(QStringLiteral("No output device available"));
            return false;
        }

        CFStringRef outputUID = copyDeviceUID(outputDevice);
        if (!outputUID) {
            qWarning() << "[ProcessTap] Failed to get output device UID";
            emit tapError(QStringLiteral("Cannot get output device UID"));
            return false;
        }

        // ── 5. Build aggregate device description ───────────────────
        // Add BOTH the output device AND the tap device as sub-devices.
        // The tap device (created by AudioHardwareCreateProcessTap) is itself
        // an audio device with input streams — adding it as a sub-device
        // gives the aggregate device input capability from the tap.

        // Unique aggregate UID per creation (avoids conflicts on restart)
        NSString* aggUID = [NSString stringWithFormat:@"com.soranaflow.tap.%@",
                            [[NSUUID UUID] UUIDString]];

        NSDictionary* aggDesc = @{
            @"name"         : @"SoranaFlow DSP Tap",
            @"uid"          : aggUID,
            @"private"      : @YES,
            @"subdevices"   : @[
                @{ @"uid" : (__bridge NSString*)outputUID }    // output (speaker)
            ],
            @"taps"         : @[
                @{ @"uid" : d->tapUUID }                       // tap (input source)
            ],
            @"tapautostart" : @YES
        };

        qDebug() << "[ProcessTap] Aggregate dict:"
                 << QString::fromNSString([aggDesc description]);

        OSStatus err = AudioHardwareCreateAggregateDevice(
            (__bridge CFDictionaryRef)aggDesc, &d->aggregateID);
        CFRelease(outputUID);

        if (err != noErr) {
            qWarning() << "[ProcessTap] Failed to create aggregate device:"
                       << err;
            AudioHardwareDestroyProcessTap(d->tapID);
            d->tapID = kAudioObjectUnknown;
            emit tapError(QStringLiteral("Failed to create aggregate device "
                                         "(error %1)").arg(err));
            return false;
        }
        qDebug() << "[ProcessTap] Aggregate device:" << d->aggregateID;

        // ── Diagnostic: Log aggregate device stream configuration ────
        {
            UInt32 streamSize = 0;
            AudioObjectPropertyAddress streamAddr = {
                kAudioDevicePropertyStreams,
                kAudioObjectPropertyScopeInput,
                kAudioObjectPropertyElementMain
            };
            AudioObjectGetPropertyDataSize(d->aggregateID, &streamAddr, 0, nullptr, &streamSize);
            UInt32 inputStreamCount = streamSize / sizeof(AudioStreamID);
            qDebug() << "[ProcessTap] Aggregate INPUT streams:" << inputStreamCount;

            streamAddr.mScope = kAudioObjectPropertyScopeOutput;
            AudioObjectGetPropertyDataSize(d->aggregateID, &streamAddr, 0, nullptr, &streamSize);
            UInt32 outputStreamCount = streamSize / sizeof(AudioStreamID);
            qDebug() << "[ProcessTap] Aggregate OUTPUT streams:" << outputStreamCount;

            // Log active sub-device list
            UInt32 subDevSize = 0;
            AudioObjectPropertyAddress subAddr = {
                kAudioAggregateDevicePropertyActiveSubDeviceList,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            OSStatus subErr = AudioObjectGetPropertyDataSize(d->aggregateID, &subAddr, 0, nullptr, &subDevSize);
            if (subErr == noErr && subDevSize > 0) {
                UInt32 subDevCount = subDevSize / sizeof(AudioObjectID);
                qDebug() << "[ProcessTap] Active sub-devices:" << subDevCount;
                std::vector<AudioObjectID> subDevs(subDevCount);
                AudioObjectGetPropertyData(d->aggregateID, &subAddr, 0, nullptr,
                                           &subDevSize, subDevs.data());
                for (UInt32 i = 0; i < subDevCount; i++) {
                    qDebug() << "[ProcessTap]   sub-device[" << i << "]:" << subDevs[i];
                }
            } else {
                qDebug() << "[ProcessTap] No active sub-devices or error:" << subErr;
            }

            // Log full sub-device list
            AudioObjectPropertyAddress fullSubAddr = {
                kAudioAggregateDevicePropertyFullSubDeviceList,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 fullSubSize = 0;
            OSStatus fullErr = AudioObjectGetPropertyDataSize(d->aggregateID, &fullSubAddr, 0, nullptr, &fullSubSize);
            if (fullErr == noErr && fullSubSize > 0) {
                UInt32 fullSubCount = fullSubSize / sizeof(AudioObjectID);
                qDebug() << "[ProcessTap] Full sub-device list:" << fullSubCount;
            }
        }

        // ── 5. Query sample rate + channels ─────────────────────────
        Float64 deviceRate = 0;
        UInt32 sz = sizeof(deviceRate);
        AudioObjectPropertyAddress rateAddr = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(d->aggregateID, &rateAddr,
                                   0, nullptr, &sz, &deviceRate);
        if (deviceRate <= 0) deviceRate = 44100.0;

        int channels = 2;  // stereo mixdown from CATapDescription
        d->ioData.channels = channels;

        // Pre-allocate interleave buffer (8192 frames max)
        d->ioData.interleaved.resize(8192 * channels, 0.0f);

        // Prepare DSP pipeline for tap sample rate
        if (d->ioData.pipeline)
            d->ioData.pipeline->prepare(deviceRate, channels);

        qDebug() << "[ProcessTap] Rate:" << deviceRate
                 << "Hz, channels:" << channels;

        // ── 6. Create and start IOProc ──────────────────────────────
        err = AudioDeviceCreateIOProcID(d->aggregateID, tapIOProc,
                                        &d->ioData, &d->ioProcID);
        if (err != noErr) {
            qWarning() << "[ProcessTap] Failed to create IOProc:" << err;
            AudioHardwareDestroyAggregateDevice(d->aggregateID);
            AudioHardwareDestroyProcessTap(d->tapID);
            d->aggregateID = kAudioObjectUnknown;
            d->tapID = kAudioObjectUnknown;
            emit tapError(QStringLiteral("Failed to create IOProc "
                                         "(error %1)").arg(err));
            return false;
        }

        err = AudioDeviceStart(d->aggregateID, d->ioProcID);
        if (err != noErr) {
            qWarning() << "[ProcessTap] Failed to start IOProc:" << err;
            AudioDeviceDestroyIOProcID(d->aggregateID, d->ioProcID);
            AudioHardwareDestroyAggregateDevice(d->aggregateID);
            AudioHardwareDestroyProcessTap(d->tapID);
            d->ioProcID = nullptr;
            d->aggregateID = kAudioObjectUnknown;
            d->tapID = kAudioObjectUnknown;
            emit tapError(QStringLiteral("Failed to start audio processing "
                                         "(error %1)").arg(err));
            return false;
        }

        d->active = true;
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        qDebug() << "[ProcessTap] IOProc started in" << duration.count() << "ms (full creation),"
                 << deviceRate << "Hz";
        emit tapStarted();
        return true;
    }

    qDebug() << "[ProcessTap] Not supported on this macOS version";
    return false;
}

void AudioProcessTap::stop()
{
    if (!d->active) return;

    qDebug() << "[ProcessTap] Stopping...";

    if (d->ioProcID && d->aggregateID != kAudioObjectUnknown) {
        AudioDeviceStop(d->aggregateID, d->ioProcID);
        AudioDeviceDestroyIOProcID(d->aggregateID, d->ioProcID);
        d->ioProcID = nullptr;
    }

    if (d->aggregateID != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(d->aggregateID);
        d->aggregateID = kAudioObjectUnknown;
    }

    // Destroy the tap object
    if (d->tapID != kAudioObjectUnknown) {
        AudioHardwareDestroyProcessTap(d->tapID);
        d->tapID = kAudioObjectUnknown;
    }

    d->tapUUID = nil;
    d->active = false;
    d->prepared = false;

    qDebug() << "[ProcessTap] Stopped";
    emit tapStopped();
}

void AudioProcessTap::onPlaybackStall()
{
    if (d->recreating) return;

    d->stallCount++;
    qDebug() << "[ProcessTap] Playback stall #" << d->stallCount;

    if (d->stallCount >= 3) {
        d->recreating = true;
        int delay = qMin(d->stallCount * 2000, 10000);
        qDebug() << "[ProcessTap] Recreating tap after" << delay
                 << "ms backoff (stall #" << d->stallCount << ")";

        stop();
        QTimer::singleShot(delay, this, [this]() {
            d->recreating = false;
            qDebug() << "[ProcessTap] Backoff complete, restarting tap";
            start();
        });
    }
}

void AudioProcessTap::onPlaybackResumed()
{
    if (d->stallCount > 0) {
        qDebug() << "[ProcessTap] Playback resumed, resetting stall counter from"
                 << d->stallCount;
        d->stallCount = 0;
    }
}
