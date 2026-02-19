#include "GaplessManager.h"
#include "AudioDecoder.h"
#include "DSDDecoder.h"
#include "../Settings.h"

#include <QDebug>
#include <QFileInfo>
#include <cmath>
#include <cstring>

GaplessManager::GaplessManager(std::mutex* decoderMutex)
    : m_decoderMutex(decoderMutex)
    , m_nextDecoder(std::make_unique<AudioDecoder>())
    , m_nextDsdDecoder(std::make_unique<DSDDecoder>())
{
}

GaplessManager::~GaplessManager() = default;

// ── Main thread ──────────────────────────────────────────────────────

void GaplessManager::prepareNextTrack(const QString& filePath, double outputMaxRate,
                                       double currentRate, int currentChannels,
                                       bool currentUsingDSD)
{
    if (filePath.isEmpty()) return;

    // Don't prepare if both gapless and crossfade are disabled
    if (!Settings::instance()->gaplessPlayback()
        && m_crossfadeDurationMs.load(std::memory_order_relaxed) <= 0) return;

    qDebug() << "[Gapless] Preparing next track:" << filePath;

    std::lock_guard<std::mutex> lock(*m_decoderMutex);

    // Close any previously prepared next track
    m_nextDecoder->close();
    m_nextDsdDecoder->close();
    m_nextTrackReady.store(false, std::memory_order_relaxed);
    m_nextUsingDSD = false;
    m_nextFilePath.clear();

    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isReadable() || fi.size() == 0) {
        qDebug() << "[Gapless] Next track file invalid:" << filePath;
        return;
    }

    QString ext = fi.suffix().toLower();
    bool isDSD = (ext == QStringLiteral("dsf") || ext == QStringLiteral("dff"));

    if (isDSD) {
        QString dsdMode = Settings::instance()->dsdPlaybackMode();
        if (dsdMode == QStringLiteral("dop")) {
            if (m_nextDsdDecoder->openDSD(filePath.toStdString(), true)) {
                AudioStreamFormat fmt = m_nextDsdDecoder->format();
                if (outputMaxRate > 0 && fmt.sampleRate > outputMaxRate) {
                    m_nextDsdDecoder->close();
                } else {
                    m_nextUsingDSD = true;
                    m_nextFormat = fmt;
                }
            }
        }

        if (!m_nextUsingDSD) {
            if (m_nextDecoder->open(filePath.toStdString())) {
                m_nextFormat = m_nextDecoder->format();
            } else {
                qDebug() << "[Gapless] Failed to open next track:" << filePath;
                return;
            }
        }
    } else {
        if (!m_nextDecoder->open(filePath.toStdString())) {
            qDebug() << "[Gapless] Failed to open next track:" << filePath;
            return;
        }
        m_nextFormat = m_nextDecoder->format();
    }

    m_nextFilePath = filePath;

    // Check format compatibility: sample rate and channels must match for seamless transition
    bool formatMatch = (std::abs(m_nextFormat.sampleRate - currentRate) < 1.0)
                       && (m_nextFormat.channels == currentChannels)
                       && (m_nextUsingDSD == currentUsingDSD);

    if (!formatMatch) {
        qDebug() << "[Gapless] Format mismatch — will use normal transition"
                 << "current:" << currentRate << "Hz" << currentChannels << "ch DSD:" << currentUsingDSD
                 << "next:" << m_nextFormat.sampleRate << "Hz" << m_nextFormat.channels << "ch DSD:" << m_nextUsingDSD;
        // Keep the decoder open — we'll reuse it in load() to avoid double-open
        m_nextTrackReady.store(false, std::memory_order_relaxed);
        return;
    }

    m_nextTrackReady.store(true, std::memory_order_release);
    qDebug() << "[Gapless] Next track ready:" << filePath;
}

void GaplessManager::cancelNextTrack()
{
    std::lock_guard<std::mutex> lock(*m_decoderMutex);
    m_nextTrackReady.store(false, std::memory_order_relaxed);
    m_nextDecoder->close();
    m_nextDsdDecoder->close();
    m_nextUsingDSD = false;
    m_nextFilePath.clear();
    qDebug() << "[Gapless] Next track cancelled";
}

void GaplessManager::resetLocked()
{
    // Caller MUST hold m_decoderMutex
    m_nextDecoder->close();
    m_nextDsdDecoder->close();
    m_nextTrackReady.store(false, std::memory_order_relaxed);
    m_nextUsingDSD = false;
    m_nextFilePath.clear();
    m_crossfading = false;
    m_crossfadeProgress = 0;
}

void GaplessManager::destroyDecodersLocked()
{
    // Caller MUST hold m_decoderMutex
    m_nextDecoder.reset();
    m_nextDsdDecoder.reset();
    m_nextTrackReady.store(false, std::memory_order_relaxed);
}

void GaplessManager::setCrossfadeDuration(int ms)
{
    m_crossfadeDurationMs.store(ms, std::memory_order_relaxed);
    qDebug() << "[Crossfade] Duration set to" << ms << "ms";
}

void GaplessManager::preallocateCrossfadeBuffer(int channels)
{
    m_crossfadeBuf.resize(16384 * channels);
    m_crossfading = false;
    m_crossfadeProgress = 0;
}

// ── Audio thread (RT-safe, caller holds decoderMutex) ────────────────

void GaplessManager::startCrossfade(int64_t framesRendered, int64_t totalFrames, int64_t cfFrames)
{
    m_crossfading = true;
    m_crossfadeProgress = (int)(framesRendered - (totalFrames - cfFrames));
    m_crossfadeTotalFrames = (int)cfFrames;
}

void GaplessManager::advanceCrossfade(int frames)
{
    m_crossfadeProgress += frames;
}

void GaplessManager::endCrossfade()
{
    m_crossfading = false;
    m_crossfadeProgress = 0;
}

GaplessManager::TransitionResult GaplessManager::swapToCurrent(
    std::unique_ptr<AudioDecoder>& currentDecoder,
    std::unique_ptr<DSDDecoder>& currentDsdDecoder,
    std::atomic<bool>& currentUsingDSD,
    std::mutex& filePathMutex,
    QString& currentFilePath)
{
    TransitionResult result;

    // Safety: if next decoders were destroyed (shutdown), return empty result
    if (!m_nextDecoder && !m_nextDsdDecoder) {
        result.newDuration = 0;
        result.newSampleRate = 0;
        result.newChannels = 0;
        result.newUsingDSD = false;
        return result;
    }

    result.newDuration = m_nextFormat.durationSecs;
    result.newSampleRate = m_nextFormat.sampleRate;
    result.newChannels = m_nextFormat.channels;
    result.newUsingDSD = m_nextUsingDSD;
    result.newFilePath = m_nextFilePath;

    // Swap decoders: next → current, current → next (old)
    currentDecoder.swap(m_nextDecoder);
    currentDsdDecoder.swap(m_nextDsdDecoder);
    currentUsingDSD.store(m_nextUsingDSD, std::memory_order_relaxed);

    // Transfer DoP marker state for seamless DSD→DSD gapless transition.
    // After swap: currentDsdDecoder = new decoder, m_nextDsdDecoder = old decoder.
    // The old decoder has the correct marker alternation state.
    if (m_nextUsingDSD && currentDsdDecoder->isDoPMode()
        && m_nextDsdDecoder->isDoPMode()) {
        currentDsdDecoder->setDoPMarkerState(m_nextDsdDecoder->dopMarkerState());
    }

    // Update file path (try_lock to avoid blocking RT thread)
    { std::unique_lock<std::mutex> lock(filePathMutex, std::try_to_lock);
      if (lock.owns_lock()) currentFilePath = m_nextFilePath; }

    // Clean up old decoder (now in next slot)
    m_nextDecoder->close();
    m_nextDsdDecoder->close();
    m_nextTrackReady.store(false, std::memory_order_relaxed);
    m_nextUsingDSD = false;
    m_nextFilePath.clear();
    m_crossfading = false;
    m_crossfadeProgress = 0;

    return result;
}
