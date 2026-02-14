#pragma once

#include <memory>
#include <atomic>
#include <vector>
#include <mutex>
#include <QString>
#include "AudioFormat.h"

class AudioDecoder;
class DSDDecoder;

// Manages gapless playback and crossfade transitions.
// Owns the "next track" decoders and crossfade state.
//
// Thread safety:
//   - Main-thread methods (prepareNextTrack, cancelNextTrack) lock the
//     shared decoder mutex internally.
//   - RT methods (accessors, swapToCurrent) assume the caller already
//     holds the decoder mutex (via try_lock in the render callback).
//   - resetLocked() / destroyDecodersLocked() assume caller holds mutex.
class GaplessManager {
public:
    // decoderMutex: shared with AudioEngine, protects all decoder access
    explicit GaplessManager(std::mutex* decoderMutex);
    ~GaplessManager();

    // ── Main thread ──────────────────────────────────────────────────

    // Prepare next track for gapless/crossfade transition.
    // outputMaxRate: from IAudioOutput::getMaxSampleRate()
    void prepareNextTrack(const QString& filePath, double outputMaxRate,
                          double currentRate, int currentChannels,
                          bool currentUsingDSD);
    void cancelNextTrack();

    // Reset all state — caller MUST hold decoderMutex
    void resetLocked();
    // Destroy decoders for shutdown — caller MUST hold decoderMutex
    void destroyDecodersLocked();

    void setCrossfadeDuration(int ms);
    int crossfadeDurationMs() const { return m_crossfadeDurationMs.load(std::memory_order_relaxed); }
    bool isNextTrackReady() const { return m_nextTrackReady.load(std::memory_order_relaxed); }

    // Pre-allocate crossfade buffer (called from load(), main thread)
    void preallocateCrossfadeBuffer(int channels);

    // ── Audio thread (caller MUST hold decoderMutex) ─────────────────

    // Decoder access for render callback
    AudioDecoder* nextDecoder() { return m_nextDecoder.get(); }
    DSDDecoder* nextDsdDecoder() { return m_nextDsdDecoder.get(); }
    bool nextUsingDSD() const { return m_nextUsingDSD; }
    const AudioStreamFormat& nextFormat() const { return m_nextFormat; }
    const QString& nextFilePath() const { return m_nextFilePath; }

    // Crossfade state
    bool isCrossfading() const { return m_crossfading; }
    float* crossfadeBufData() { return m_crossfadeBuf.data(); }
    int crossfadeBufCapacity(int channels) const {
        return channels > 0 ? (int)m_crossfadeBuf.size() / channels : 0;
    }
    int crossfadeProgress() const { return m_crossfadeProgress; }
    int crossfadeTotalFrames() const { return m_crossfadeTotalFrames; }

    void startCrossfade(int64_t framesRendered, int64_t totalFrames, int64_t cfFrames);
    void advanceCrossfade(int frames);
    void endCrossfade();

    // Swap next→current decoders. Transfers DoP marker state.
    // Cleans up old decoders (now in next slots). Updates currentFilePath.
    struct TransitionResult {
        double newDuration = 0.0;
        double newSampleRate = 0.0;
        int newChannels = 0;
        bool newUsingDSD = false;
        QString newFilePath;
    };
    TransitionResult swapToCurrent(
        std::unique_ptr<AudioDecoder>& currentDecoder,
        std::unique_ptr<DSDDecoder>& currentDsdDecoder,
        std::atomic<bool>& currentUsingDSD,
        std::mutex& filePathMutex,
        QString& currentFilePath);

private:
    std::mutex* m_decoderMutex;  // not owned, shared with AudioEngine

    std::unique_ptr<AudioDecoder> m_nextDecoder;
    std::unique_ptr<DSDDecoder> m_nextDsdDecoder;
    std::atomic<bool> m_nextTrackReady{false};
    bool m_nextUsingDSD = false;
    AudioStreamFormat m_nextFormat{};
    QString m_nextFilePath;

    std::atomic<int> m_crossfadeDurationMs{0};
    bool m_crossfading = false;
    int m_crossfadeProgress = 0;
    int m_crossfadeTotalFrames = 0;
    std::vector<float> m_crossfadeBuf;
};
