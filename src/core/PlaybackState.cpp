#include "PlaybackState.h"
#include "audio/AudioEngine.h"
#include "Settings.h"
#include "../apple/MusicKitPlayer.h"
#include "../radio/AutoplayManager.h"
#ifdef Q_OS_MACOS
#include "../platform/macos/AudioProcessTap.h"
#endif

#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>

// ── Singleton ───────────────────────────────────────────────────────
PlaybackState* PlaybackState::instance()
{
    static PlaybackState s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
PlaybackState::PlaybackState(QObject* parent)
    : QObject(parent)
{
    m_queueChangeDebounce = new QTimer(this);
    m_queueChangeDebounce->setSingleShot(true);
    m_queueChangeDebounce->setInterval(50);
    connect(m_queueChangeDebounce, &QTimer::timeout, this, [this]() {
        emit queueChanged();
    });

    connectToAudioEngine();
    connectToMusicKitPlayer();

    // Debounced async queue save — coalesces multiple changes into one write
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500);
    connect(m_saveTimer, &QTimer::timeout, this, &PlaybackState::doSave);

    // Debounced volume save — avoids QSettings write on every slider tick
    m_volumeSaveTimer = new QTimer(this);
    m_volumeSaveTimer->setSingleShot(true);
    m_volumeSaveTimer->setInterval(300);
    connect(m_volumeSaveTimer, &QTimer::timeout, this, [this]() {
        Settings::instance()->setVolume(m_volume);
    });

    // Autoplay manager
    m_autoplay = AutoplayManager::instance();
    m_autoplay->setEnabled(Settings::instance()->autoplayEnabled());

    connect(Settings::instance(), &Settings::autoplayEnabledChanged,
            this, [this](bool enabled) {
        m_autoplay->setEnabled(enabled);
    });

    connect(m_autoplay, &AutoplayManager::trackRecommended,
            this, [this](const Track& track) {
        qDebug() << "[Autoplay] Got recommendation:" << track.artist << "-" << track.title;
        addToQueue(track);
        m_autoplayActive = true;
        playNextTrack();
        emit autoplayTrackStarted();
    });

    connect(m_autoplay, &AutoplayManager::noRecommendation,
            this, [this]() {
        qDebug() << "[Autoplay] No recommendation — stopping";
        auto* engine = AudioEngine::instance();
        m_playing = false;
        if (m_currentSource == AppleMusic)
            MusicKitPlayer::instance()->stop();
        else
            engine->stop();
        emit playStateChanged(m_playing);
    });

    // Restore queue from previous session
    restoreQueueFromSettings();
}

// ── connectToAudioEngine ────────────────────────────────────────────
void PlaybackState::connectToAudioEngine()
{
    auto* engine = AudioEngine::instance();

    connect(engine, &AudioEngine::positionChanged, this, [this](double secs) {
        int intSecs = static_cast<int>(secs);
        if (intSecs != m_currentTime) {
            m_currentTime = intSecs;
            emit timeChanged(m_currentTime);
        }
    });

    connect(engine, &AudioEngine::playbackFinished, this, [this]() {
        playNextTrack();
    });

    connect(engine, &AudioEngine::stateChanged, this, [this](AudioEngine::State st) {
        bool nowPlaying = (st == AudioEngine::Playing);
        if (nowPlaying != m_playing) {
            m_playing = nowPlaying;
            emit playStateChanged(m_playing);
        }
    });

    connect(engine, &AudioEngine::gaplessTransitionOccurred, this, [this]() {
        onGaplessTransition();
    });
}

// ── connectToMusicKitPlayer ──────────────────────────────────────────
void PlaybackState::connectToMusicKitPlayer()
{
    auto* mkp = MusicKitPlayer::instance();

    connect(mkp, &MusicKitPlayer::playbackTimeChanged, this, [this](double current, double) {
        if (m_currentSource != AppleMusic) return;
        int intSecs = static_cast<int>(current);
        if (intSecs != m_currentTime) {
            m_currentTime = intSecs;
            emit timeChanged(m_currentTime);
        }
    });

    connect(mkp, &MusicKitPlayer::playbackStateChanged, this, [this](bool playing) {
        if (m_currentSource != AppleMusic) return;
        if (playing != m_playing) {
            m_playing = playing;
            emit playStateChanged(m_playing);
#ifdef Q_OS_MACOS
            // Start process tap when Apple Music starts playing
            // NOTE: Don't stop tap on temporary pauses/stalls — only explicit stop
            // This avoids the stall-restart infinite loop
            auto* tap = AudioProcessTap::instance();
            if (playing && tap->isSupported() && !tap->isActive()) {
                tap->setDSPPipeline(AudioEngine::instance()->dspPipeline());
                qDebug() << "[Playback] Apple Music: Activating ProcessTap";
                tap->activate();
            }
#endif
        }
    });

    connect(mkp, &MusicKitPlayer::playbackEnded, this, [this]() {
        if (m_currentSource != AppleMusic) return;
        qDebug() << "[PlaybackState] Apple Music track ended — advancing";
        playNextTrack();
    });
}

// ── playPause ───────────────────────────────────────────────────────
void PlaybackState::playPause()
{
    if (m_currentSource == AppleMusic) {
        auto* mkp = MusicKitPlayer::instance();
        if (m_playing) {
            mkp->pause();
            m_playing = false;
        } else {
            mkp->resume();
            m_playing = true;
        }
        emit playStateChanged(m_playing);
        return;
    }

    auto* engine = AudioEngine::instance();

    if (m_playing) {
        engine->pause();
        m_playing = false;
    } else {
        if (!m_currentTrack.filePath.isEmpty()) {
            engine->play();
            m_playing = true;
        } else {
            return;
        }
    }

    emit playStateChanged(m_playing);
}

// ── next ────────────────────────────────────────────────────────────
void PlaybackState::next()
{
    if (m_queue.isEmpty())
        return;

    playNextTrack();
}

// ── previous ────────────────────────────────────────────────────────
void PlaybackState::previous()
{
    if (m_queue.isEmpty())
        return;

    // If more than 3 seconds in, restart the current track
    if (m_currentTime > 3) {
        seek(0);
        return;
    }

    // Otherwise go to the previous track
    if (m_queueIndex > 0) {
        m_queueIndex--;
    } else if (m_repeat == All) {
        m_queueIndex = m_queue.size() - 1;
    } else {
        // Already at the first track — just restart it
        seek(0);
        return;
    }

    m_currentTrack = m_queue.at(m_queueIndex);
    m_currentTime = 0;
    emit timeChanged(m_currentTime);
    emitQueueChangedDebounced();
    emit trackChanged(m_currentTrack);

    // Use playTrack() for proper source switching (Local ↔ Apple Music)
    playTrack(m_currentTrack);
}

// ── seek ────────────────────────────────────────────────────────────
void PlaybackState::seek(int position)
{
    m_currentTime = qBound(0, position, m_currentTrack.duration);
    emit timeChanged(m_currentTime);

    if (m_currentSource == AppleMusic) {
        MusicKitPlayer::instance()->seek(static_cast<double>(m_currentTime));
    } else if (!m_currentTrack.filePath.isEmpty()) {
        AudioEngine::instance()->seek(static_cast<double>(m_currentTime));
    }
}

// ── setVolume ───────────────────────────────────────────────────────
void PlaybackState::setVolume(int vol)
{
    int clamped = qBound(0, vol, 100);
    if (clamped == m_volume)
        return;

    m_volume = clamped;
    AudioEngine::instance()->setVolume(m_volume / 100.0f);
    MusicKitPlayer::instance()->setVolume(m_volume / 100.0);
    m_volumeSaveTimer->start();  // debounce — save after 300ms idle
    emit volumeChanged(m_volume);
}

// ── toggleShuffle ───────────────────────────────────────────────────
void PlaybackState::toggleShuffle()
{
    m_shuffle = !m_shuffle;
    Settings::instance()->setShuffleEnabled(m_shuffle);
    if (m_shuffle) {
        rebuildShuffleOrder();
    } else {
        m_shuffledIndices.clear();
    }
    emit shuffleChanged(m_shuffle);
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── rebuildShuffleOrder ─────────────────────────────────────────────
void PlaybackState::rebuildShuffleOrder()
{
    m_shuffledIndices.clear();
    for (int i = 0; i < m_queue.size(); ++i) {
        if (i != m_queueIndex)
            m_shuffledIndices.append(i);
    }
    // Fisher-Yates shuffle
    for (int i = m_shuffledIndices.size() - 1; i > 0; --i) {
        int j = QRandomGenerator::global()->bounded(i + 1);
        std::swap(m_shuffledIndices[i], m_shuffledIndices[j]);
    }
}

// ── emitQueueChangedDebounced ───────────────────────────────────────
void PlaybackState::emitQueueChangedDebounced()
{
    m_queueChangeDebounce->start();
}

// ── displayQueue ────────────────────────────────────────────────────
QVector<Track> PlaybackState::displayQueue() const
{
    if (!m_shuffle || m_shuffledIndices.isEmpty())
        return m_queue;

    // Build display queue: current track first, then shuffled order
    QVector<Track> result;
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size())
        result.append(m_queue.at(m_queueIndex));
    for (int idx : m_shuffledIndices) {
        if (idx >= 0 && idx < m_queue.size())
            result.append(m_queue.at(idx));
    }
    return result;
}

// ── cycleRepeat ─────────────────────────────────────────────────────
void PlaybackState::cycleRepeat()
{
    switch (m_repeat) {
    case Off:  m_repeat = All; break;
    case All:  m_repeat = One; break;
    case One:  m_repeat = Off; break;
    }
    Settings::instance()->setRepeatMode(static_cast<int>(m_repeat));
    emit repeatChanged(m_repeat);
}

// ── setCurrentTrackInfo (instant UI update, no audio loading) ───────
void PlaybackState::setCurrentTrackInfo(const Track& track)
{
    // Reset autoplay indicator on manual track selection
    if (m_autoplayActive) {
        m_autoplayActive = false;
    }

    m_currentTrack = track;
    m_currentTime = 0;

    // Try to find the track in the current queue
    int idx = -1;
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).id == track.id) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        m_queueIndex = idx;
    } else {
        m_queueIndex = (m_queueIndex >= 0) ? m_queueIndex + 1 : 0;
        m_queue.insert(m_queueIndex, track);
    }

    scheduleSave();
    emitQueueChangedDebounced();
    emit timeChanged(m_currentTime);
    emit trackChanged(m_currentTrack);
}

// ── playTrack ───────────────────────────────────────────────────────
void PlaybackState::playTrack(const Track& track)
{
    // Update UI state immediately
    setCurrentTrackInfo(track);

    if (!m_playing) {
        m_playing = true;
        emit playStateChanged(m_playing);
    }

    // Determine source: empty filePath with a valid id = Apple Music
    if (track.filePath.isEmpty() && !track.id.isEmpty()) {
        // Stop local engine if it was playing
        if (m_currentSource == Local) {
            AudioEngine::instance()->stop();
        }
        m_currentSource = AppleMusic;

        // Start process tap so DSP applies to Apple Music audio
        // NOTE: Delay start so WebView child process exists first
#ifdef Q_OS_MACOS
        {
            auto* tap = AudioProcessTap::instance();
            if (tap->isSupported() && !tap->isActive()) {
                tap->setDSPPipeline(AudioEngine::instance()->dspPipeline());
                qDebug() << "[Playback] Apple Music: Activating ProcessTap (play)";
                tap->activate();
            }
        }
#endif

        QString songId = track.id;
        QTimer::singleShot(0, this, [songId]() {
            MusicKitPlayer::instance()->play(songId);
        });
        return;
    }

    // Local playback
    if (m_currentSource == AppleMusic) {
        MusicKitPlayer::instance()->stop();
#ifdef Q_OS_MACOS
        AudioProcessTap::instance()->deactivate();
        qDebug() << "[Playback] Apple Music: ProcessTap deactivated (stays warm)";
#endif
    }
    m_currentSource = Local;

    // Defer audio loading so the event loop can process the UI update first
    Track trackCopy = track;
    QTimer::singleShot(0, this, [this, trackCopy]() {
        if (!trackCopy.filePath.isEmpty()) {
            auto* engine = AudioEngine::instance();
            engine->setCurrentTrack(trackCopy);
            engine->load(trackCopy.filePath);
            engine->play();

            // Schedule gapless preparation for the next track
            scheduleGaplessPrepare();
        }
    });
}

// ── setQueue ────────────────────────────────────────────────────────
void PlaybackState::setQueue(const QVector<Track>& tracks)
{
    m_queue = tracks;
    m_queueIndex = tracks.isEmpty() ? -1 : 0;
    if (m_shuffle && !tracks.isEmpty()) {
        rebuildShuffleOrder();
    }
    scheduleSave();
    emit queueChanged();

    // NOTE: Do NOT call scheduleGaplessPrepare() here.
    // setQueue() resets m_queueIndex to 0, which makes peekNextTrack()
    // return the wrong track. The caller (playTrack) handles gapless
    // preparation after setting the correct queue index.
}

// ── addToQueue ──────────────────────────────────────────────────────
void PlaybackState::addToQueue(const Track& track)
{
    m_queue.append(track);
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── addToQueue (multiple) ───────────────────────────────────────────
void PlaybackState::addToQueue(const QVector<Track>& tracks)
{
    m_queue.append(tracks);
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── insertNext ──────────────────────────────────────────────────────
void PlaybackState::insertNext(const Track& track)
{
    int insertPos = m_queueIndex + 1;
    if (insertPos < 0) insertPos = 0;
    if (insertPos > m_queue.size()) insertPos = m_queue.size();
    m_queue.insert(insertPos, track);
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── insertNext (multiple) ───────────────────────────────────────────
void PlaybackState::insertNext(const QVector<Track>& tracks)
{
    int insertPos = m_queueIndex + 1;
    if (insertPos < 0) insertPos = 0;
    if (insertPos > m_queue.size()) insertPos = m_queue.size();
    for (int i = 0; i < tracks.size(); ++i) {
        m_queue.insert(insertPos + i, tracks[i]);
    }
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── removeFromQueue ─────────────────────────────────────────────────
void PlaybackState::removeFromQueue(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;

    m_queue.removeAt(index);

    // Adjust the current queue index
    if (m_queue.isEmpty()) {
        m_queueIndex = -1;
    } else if (index < m_queueIndex) {
        m_queueIndex--;
    } else if (index == m_queueIndex) {
        // The currently-playing track was removed
        if (m_queueIndex >= m_queue.size())
            m_queueIndex = m_queue.size() - 1;
        if (m_queueIndex >= 0) {
            m_currentTrack = m_queue.at(m_queueIndex);
            emit trackChanged(m_currentTrack);
        }
    }

    scheduleSave();
    emitQueueChangedDebounced();
}

// ── clearQueue ──────────────────────────────────────────────────────
void PlaybackState::clearQueue()
{
    m_queue.clear();
    m_queueIndex = -1;
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── clearUpcoming ───────────────────────────────────────────────────
void PlaybackState::clearUpcoming()
{
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        // Keep tracks up to and including current
        m_queue.resize(m_queueIndex + 1);
    }
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── moveTo ──────────────────────────────────────────────────────────
void PlaybackState::moveTo(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_queue.size()) return;
    if (toIndex < 0 || toIndex >= m_queue.size()) return;
    if (fromIndex == toIndex) return;

    Track track = m_queue.takeAt(fromIndex);
    m_queue.insert(toIndex, track);

    // Adjust m_queueIndex if it was affected
    if (m_queueIndex == fromIndex) {
        m_queueIndex = toIndex;
    } else {
        if (fromIndex < m_queueIndex && toIndex >= m_queueIndex)
            m_queueIndex--;
        else if (fromIndex > m_queueIndex && toIndex <= m_queueIndex)
            m_queueIndex++;
    }

    if (m_shuffle) rebuildShuffleOrder();
    scheduleSave();
    emitQueueChangedDebounced();
}

// ── saveQueueToSettings ─────────────────────────────────────────────
void PlaybackState::saveQueueToSettings()
{
    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);

    settings.beginWriteArray(QStringLiteral("queue/tracks"), m_queue.size());
    for (int i = 0; i < m_queue.size(); ++i) {
        settings.setArrayIndex(i);
        const Track& t = m_queue[i];
        settings.setValue(QStringLiteral("id"), t.id);
        settings.setValue(QStringLiteral("title"), t.title);
        settings.setValue(QStringLiteral("artist"), t.artist);
        settings.setValue(QStringLiteral("album"), t.album);
        settings.setValue(QStringLiteral("albumId"), t.albumId);
        settings.setValue(QStringLiteral("duration"), t.duration);
        settings.setValue(QStringLiteral("filePath"), t.filePath);
        settings.setValue(QStringLiteral("trackNumber"), t.trackNumber);
        settings.setValue(QStringLiteral("format"), static_cast<int>(t.format));
        settings.setValue(QStringLiteral("sampleRate"), t.sampleRate);
        settings.setValue(QStringLiteral("bitDepth"), t.bitDepth);
        settings.setValue(QStringLiteral("bitrate"), t.bitrate);
        settings.setValue(QStringLiteral("coverUrl"), t.coverUrl);
    }
    settings.endArray();

    settings.setValue(QStringLiteral("queue/currentIndex"), m_queueIndex);
    settings.setValue(QStringLiteral("queue/shuffle"), m_shuffle);
    settings.setValue(QStringLiteral("queue/repeat"), static_cast<int>(m_repeat));

    qDebug() << "[Queue] Saved" << m_queue.size() << "tracks, index:" << m_queueIndex;
}

// ── scheduleSave (debounced — coalesces multiple changes) ───────────
void PlaybackState::scheduleSave()
{
    if (m_restoring) return;
    m_saveTimer->start();  // restarts the 500ms timer
}

// ── doSave (async — snapshot on main thread, write on worker) ───────
void PlaybackState::doSave()
{
    // Snapshot queue data (fast — just copies QVector of value types)
    QVector<Track> snapshot = m_queue;
    int idx = m_queueIndex;
    bool shuffle = m_shuffle;
    int repeat = static_cast<int>(m_repeat);

    QtConcurrent::run([snapshot, idx, shuffle, repeat]() {
        static QMutex mutex;
        QMutexLocker lock(&mutex);

        QElapsedTimer timer;
        timer.start();

        QSettings settings(Settings::settingsPath(), QSettings::IniFormat);

        settings.beginWriteArray(QStringLiteral("queue/tracks"), snapshot.size());
        for (int i = 0; i < snapshot.size(); ++i) {
            settings.setArrayIndex(i);
            const Track& t = snapshot[i];
            settings.setValue(QStringLiteral("id"), t.id);
            settings.setValue(QStringLiteral("title"), t.title);
            settings.setValue(QStringLiteral("artist"), t.artist);
            settings.setValue(QStringLiteral("album"), t.album);
            settings.setValue(QStringLiteral("albumId"), t.albumId);
            settings.setValue(QStringLiteral("duration"), t.duration);
            settings.setValue(QStringLiteral("filePath"), t.filePath);
            settings.setValue(QStringLiteral("trackNumber"), t.trackNumber);
            settings.setValue(QStringLiteral("format"), static_cast<int>(t.format));
            settings.setValue(QStringLiteral("sampleRate"), t.sampleRate);
            settings.setValue(QStringLiteral("bitDepth"), t.bitDepth);
            settings.setValue(QStringLiteral("bitrate"), t.bitrate);
            settings.setValue(QStringLiteral("coverUrl"), t.coverUrl);
        }
        settings.endArray();

        settings.setValue(QStringLiteral("queue/currentIndex"), idx);
        settings.setValue(QStringLiteral("queue/shuffle"), shuffle);
        settings.setValue(QStringLiteral("queue/repeat"), repeat);

        qDebug() << "[Queue] Saved" << snapshot.size() << "tracks in"
                 << timer.elapsed() << "ms (async, index:" << idx << ")";
    });
}

// ── restoreQueueFromSettings ────────────────────────────────────────
void PlaybackState::restoreQueueFromSettings()
{
    m_restoring = true;

    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);

    int count = settings.beginReadArray(QStringLiteral("queue/tracks"));
    QVector<Track> tracks;
    tracks.reserve(count);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        Track t;
        t.id = settings.value(QStringLiteral("id")).toString();
        t.title = settings.value(QStringLiteral("title")).toString();
        t.artist = settings.value(QStringLiteral("artist")).toString();
        t.album = settings.value(QStringLiteral("album")).toString();
        t.albumId = settings.value(QStringLiteral("albumId")).toString();
        t.duration = settings.value(QStringLiteral("duration")).toInt();
        t.filePath = settings.value(QStringLiteral("filePath")).toString();
        t.trackNumber = settings.value(QStringLiteral("trackNumber")).toInt();
        t.format = static_cast<AudioFormat>(settings.value(QStringLiteral("format")).toInt());
        t.sampleRate = settings.value(QStringLiteral("sampleRate")).toString();
        t.bitDepth = settings.value(QStringLiteral("bitDepth")).toString();
        t.bitrate = settings.value(QStringLiteral("bitrate")).toString();
        t.coverUrl = settings.value(QStringLiteral("coverUrl")).toString();
        tracks.append(t);
    }
    settings.endArray();

    m_queue = tracks;
    m_queueIndex = settings.value(QStringLiteral("queue/currentIndex"), -1).toInt();
    m_shuffle = settings.value(QStringLiteral("queue/shuffle"), false).toBool();
    m_repeat = static_cast<RepeatMode>(settings.value(QStringLiteral("queue/repeat"), 0).toInt());

    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        m_currentTrack = m_queue.at(m_queueIndex);
    }

    if (m_shuffle && !m_queue.isEmpty()) {
        rebuildShuffleOrder();
    }

    qDebug() << "[Queue] Restored" << m_queue.size() << "tracks, index:" << m_queueIndex;

    emit queueChanged();
    emit shuffleChanged(m_shuffle);
    emit repeatChanged(m_repeat);
    if (!m_currentTrack.id.isEmpty()) {
        emit trackChanged(m_currentTrack);
    }

    m_restoring = false;
}

// ── playNextTrack (core next-track logic with repeat/shuffle) ───────
void PlaybackState::playNextTrack()
{
    if (m_queue.isEmpty())
        return;

    auto* engine = AudioEngine::instance();

    // Repeat One — replay the same track
    if (m_repeat == One) {
        m_currentTime = 0;
        emit timeChanged(m_currentTime);
        if (m_currentSource == AppleMusic) {
            MusicKitPlayer::instance()->seek(0.0);
        } else if (!m_currentTrack.filePath.isEmpty()) {
            engine->seek(0.0);
            engine->play();
        }
        if (!m_playing) {
            m_playing = true;
            emit playStateChanged(m_playing);
        }
        return;
    }

    // Shuffle — use pre-computed shuffle order
    if (m_shuffle) {
        if (m_shuffledIndices.isEmpty()) {
            rebuildShuffleOrder();
        }
        if (!m_shuffledIndices.isEmpty()) {
            m_queueIndex = m_shuffledIndices.takeFirst();
        } else {
            // Only one track or shuffle exhausted
            if (m_repeat == All) {
                rebuildShuffleOrder();
                if (!m_shuffledIndices.isEmpty())
                    m_queueIndex = m_shuffledIndices.takeFirst();
            } else {
                // Shuffle exhausted, Repeat Off — try autoplay
                if (m_autoplay && m_autoplay->isEnabled()) {
                    m_autoplay->requestNextTrack(m_currentTrack.artist, m_currentTrack.title);
                    return;
                }
                m_playing = false;
                if (m_currentSource == AppleMusic)
                    MusicKitPlayer::instance()->stop();
                else
                    engine->stop();
                emit playStateChanged(m_playing);
                return;
            }
        }
    } else {
        // Sequential advance
        m_queueIndex++;

        if (m_queueIndex >= m_queue.size()) {
            if (m_repeat == All) {
                m_queueIndex = 0;
            } else {
                // Repeat Off and we've reached the end — try autoplay
                m_queueIndex = m_queue.size() - 1;
                if (m_autoplay && m_autoplay->isEnabled()) {
                    m_autoplay->requestNextTrack(m_currentTrack.artist, m_currentTrack.title);
                    return;
                }
                m_currentTime = 0;
                m_playing = false;
                if (m_currentSource == AppleMusic)
                    MusicKitPlayer::instance()->stop();
                else
                    engine->stop();
                emit timeChanged(m_currentTime);
                emit playStateChanged(m_playing);
                return;
            }
        }
    }

    m_currentTrack = m_queue.at(m_queueIndex);
    m_currentTime = 0;
    emit timeChanged(m_currentTime);
    emitQueueChangedDebounced();
    emit trackChanged(m_currentTrack);

    // Use playTrack() for proper source switching (Local ↔ Apple Music)
    playTrack(m_currentTrack);
}

// ── peekNextTrack ───────────────────────────────────────────────────
Track PlaybackState::peekNextTrack() const
{
    if (m_queue.isEmpty()) return Track();

    // Repeat One — same track
    if (m_repeat == One) return m_currentTrack;

    int nextIdx = -1;

    if (m_shuffle) {
        if (!m_shuffledIndices.isEmpty()) {
            nextIdx = m_shuffledIndices.first();
        } else if (m_repeat == All) {
            nextIdx = 0;
        }
    } else {
        nextIdx = m_queueIndex + 1;
        if (nextIdx >= m_queue.size()) {
            if (m_repeat == All)
                nextIdx = 0;
            else
                return Track(); // end of queue
        }
    }

    if (nextIdx >= 0 && nextIdx < m_queue.size())
        return m_queue.at(nextIdx);
    return Track();
}

// ── scheduleGaplessPrepare ──────────────────────────────────────────
void PlaybackState::scheduleGaplessPrepare()
{
    if (!Settings::instance()->gaplessPlayback()) return;
    if (m_currentSource != Local) return;

    Track next = peekNextTrack();
    if (next.filePath.isEmpty()) {
        AudioEngine::instance()->cancelNextTrack();
        return;
    }

    qDebug() << "[Gapless] Scheduling prepare:"
             << "current idx=" << m_queueIndex
             << "title=" << m_currentTrack.title
             << "→ next title=" << next.title
             << "path=" << next.filePath;
    AudioEngine::instance()->prepareNextTrack(next.filePath);
}

// ── onGaplessTransition ─────────────────────────────────────────────
void PlaybackState::onGaplessTransition()
{
    qDebug() << "[Gapless] Transition occurred, advancing queue";

    if (m_queue.isEmpty()) return;

    if (m_repeat == One) {
        m_currentTime = 0;
        emit timeChanged(m_currentTime);
        return;
    }

    if (m_shuffle) {
        if (!m_shuffledIndices.isEmpty()) {
            m_queueIndex = m_shuffledIndices.takeFirst();
        } else if (m_repeat == All) {
            rebuildShuffleOrder();
            if (!m_shuffledIndices.isEmpty())
                m_queueIndex = m_shuffledIndices.takeFirst();
        }
    } else {
        m_queueIndex++;
        if (m_queueIndex >= m_queue.size()) {
            if (m_repeat == All)
                m_queueIndex = 0;
            else
                m_queueIndex = m_queue.size() - 1;
        }
    }

    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        m_currentTrack = m_queue.at(m_queueIndex);
        m_currentTime = 0;
        emit timeChanged(m_currentTime);
        scheduleSave();
        emitQueueChangedDebounced();
        emit trackChanged(m_currentTrack);

        // Update volume leveling for the new track
        AudioEngine::instance()->setCurrentTrack(m_currentTrack);

        // Prepare the next-next track for continued gapless playback
        scheduleGaplessPrepare();
    }
}
