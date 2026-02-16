#include "PlaybackState.h"
#include "QueueManager.h"
#include "QueuePersistence.h"
#include "audio/AudioEngine.h"
#include "Settings.h"
#include "../apple/MusicKitPlayer.h"
#include "../radio/AutoplayManager.h"

#include <QDebug>
#include <QTimer>

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
    m_queueMgr = new QueueManager();
    m_queuePersist = new QueuePersistence(m_queueMgr, this);

    m_queueChangeDebounce = new QTimer(this);
    m_queueChangeDebounce->setSingleShot(true);
    m_queueChangeDebounce->setInterval(50);
    connect(m_queueChangeDebounce, &QTimer::timeout, this, [this]() {
        emit queueChanged();
    });

    connectToAudioEngine();
    connectToMusicKitPlayer();

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
        // Ignore stale "playing" from a cancelled AM play
        auto* mkp = MusicKitPlayer::instance();
        if (playing && mkp->amPlayState() == MusicKitPlayer::AMPlayState::Cancelled) {
            qDebug() << "[PlaybackState] Ignoring stale AM playbackStateChanged(true) — was cancelled";
            return;
        }
        if (playing != m_playing) {
            m_playing = playing;
            emit playStateChanged(m_playing);
        }
    });

    connect(mkp, &MusicKitPlayer::playbackEnded, this, [this]() {
        if (m_currentSource != AppleMusic) return;
        qDebug() << "[PlaybackState] Apple Music track ended — advancing";
        playNextTrack();
    });
}

// ── Delegated accessors ─────────────────────────────────────────────

bool PlaybackState::shuffleEnabled() const
{
    return m_queueMgr->shuffleEnabled();
}

PlaybackState::RepeatMode PlaybackState::repeatMode() const
{
    return static_cast<RepeatMode>(m_queueMgr->repeatMode());
}

QVector<Track> PlaybackState::queue() const
{
    return m_queueMgr->queue();
}

QVector<Track> PlaybackState::displayQueue() const
{
    return m_queueMgr->displayQueue();
}

Track PlaybackState::peekNextTrack() const
{
    return m_queueMgr->peekNextTrack();
}

int PlaybackState::queueIndex() const
{
    return m_queueMgr->currentIndex();
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
    if (m_queueMgr->isEmpty())
        return;

    playNextTrack();
}

// ── previous ────────────────────────────────────────────────────────
void PlaybackState::previous()
{
    if (m_queueMgr->isEmpty())
        return;

    // If more than 3 seconds in, restart the current track
    if (m_currentTime > 3) {
        seek(0);
        return;
    }

    // Otherwise go to the previous track
    if (m_queueMgr->retreat()) {
        m_currentTrack = m_queueMgr->currentTrack();
        m_currentTime = 0;
        emit timeChanged(m_currentTime);
        emitQueueChangedDebounced();
        emit trackChanged(m_currentTrack);
        playTrack(m_currentTrack);
    } else {
        // Already at the first track — just restart it
        seek(0);
    }
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
    m_queueMgr->toggleShuffle();
    Settings::instance()->setShuffleEnabled(m_queueMgr->shuffleEnabled());
    emit shuffleChanged(m_queueMgr->shuffleEnabled());
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

// ── emitQueueChangedDebounced ───────────────────────────────────────
void PlaybackState::emitQueueChangedDebounced()
{
    m_queueChangeDebounce->start();
}

// ── cycleRepeat ─────────────────────────────────────────────────────
void PlaybackState::cycleRepeat()
{
    m_queueMgr->cycleRepeat();
    RepeatMode mode = static_cast<RepeatMode>(m_queueMgr->repeatMode());
    Settings::instance()->setRepeatMode(static_cast<int>(mode));
    emit repeatChanged(mode);
}

// ── setCurrentTrackInfo (instant UI update, no audio loading) ───────
void PlaybackState::setCurrentTrackInfo(const Track& track)
{
    if (m_autoplayActive)
        m_autoplayActive = false;

    m_currentTrack = track;
    m_currentTime = 0;

    int idx = m_queueMgr->findOrInsertTrack(track);
    m_queueMgr->setCurrentIndex(idx);

    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
    emit timeChanged(m_currentTime);
    emit trackChanged(m_currentTrack);
}

// ── playTrack ───────────────────────────────────────────────────────
void PlaybackState::playTrack(const Track& track)
{
    setCurrentTrackInfo(track);

    if (!m_playing) {
        m_playing = true;
        emit playStateChanged(m_playing);
    }

    // Determine source: empty filePath with a valid id = Apple Music
    if (track.filePath.isEmpty() && !track.id.isEmpty()) {
        if (m_currentSource == Local) {
            AudioEngine::instance()->stop();
        }
        m_currentSource = AppleMusic;

        QString songId = track.id;
        QTimer::singleShot(0, this, [songId]() {
            MusicKitPlayer::instance()->play(songId);
        });
        return;
    }

    // Local playback — cancel any pending/active Apple Music play
    if (m_currentSource == AppleMusic) {
        auto* mkp = MusicKitPlayer::instance();
        if (mkp->amPlayState() != MusicKitPlayer::AMPlayState::Idle) {
            qDebug() << "[PlaybackState] Cancelling Apple Music play — switching to local";
            mkp->cancelPendingPlay();
        } else {
            mkp->stop();
        }
    }
    m_currentSource = Local;

    Track trackCopy = track;
    QTimer::singleShot(0, this, [this, trackCopy]() {
        if (!trackCopy.filePath.isEmpty()) {
            auto* engine = AudioEngine::instance();
            engine->setCurrentTrack(trackCopy);
            engine->load(trackCopy.filePath);
            engine->play();
            scheduleGaplessPrepare();
        }
    });
}

// ── Queue CRUD — delegate to QueueManager ───────────────────────────

void PlaybackState::setQueue(const QVector<Track>& tracks)
{
    m_queueMgr->setQueue(tracks);
    m_queuePersist->scheduleSave();
    emit queueChanged();
}

void PlaybackState::addToQueue(const Track& track)
{
    m_queueMgr->addToQueue(track);
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

void PlaybackState::addToQueue(const QVector<Track>& tracks)
{
    m_queueMgr->addToQueue(tracks);
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

void PlaybackState::insertNext(const Track& track)
{
    m_queueMgr->insertNext(track);
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

void PlaybackState::insertNext(const QVector<Track>& tracks)
{
    m_queueMgr->insertNext(tracks);
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

void PlaybackState::removeFromQueue(int index)
{
    bool wasCurrent = (index == m_queueMgr->currentIndex());
    m_queueMgr->removeFromQueue(index);

    if (wasCurrent && m_queueMgr->currentIndex() >= 0) {
        m_currentTrack = m_queueMgr->currentTrack();
        emit trackChanged(m_currentTrack);
    }

    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

void PlaybackState::moveTo(int fromIndex, int toIndex)
{
    m_queueMgr->moveTo(fromIndex, toIndex);
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

void PlaybackState::clearQueue()
{
    m_queueMgr->clearQueue();
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

void PlaybackState::clearUpcoming()
{
    m_queueMgr->clearUpcoming();
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
}

// ── Persistence — delegate to QueuePersistence ──────────────────────

void PlaybackState::saveQueueToSettings()
{
    m_queuePersist->saveImmediate();
}

void PlaybackState::restoreQueueFromSettings()
{
    m_queuePersist->restore();

    // Sync local state with restored queue
    if (m_queueMgr->currentIndex() >= 0 && m_queueMgr->currentIndex() < m_queueMgr->size()) {
        m_currentTrack = m_queueMgr->currentTrack();
    }

    emit queueChanged();
    emit shuffleChanged(m_queueMgr->shuffleEnabled());
    emit repeatChanged(static_cast<RepeatMode>(m_queueMgr->repeatMode()));
    if (!m_currentTrack.id.isEmpty()) {
        emit trackChanged(m_currentTrack);
    }
}

void PlaybackState::flushPendingSaves()
{
    if (m_volumeSaveTimer->isActive()) {
        m_volumeSaveTimer->stop();
        Settings::instance()->setVolume(m_volume);
        qDebug() << "[Shutdown] Flushed pending volume save:" << m_volume;
    }
    m_queuePersist->flushPending();
}

// ── playNextTrack (core next-track logic) ───────────────────────────
void PlaybackState::playNextTrack()
{
    if (m_queueMgr->isEmpty())
        return;

    auto* engine = AudioEngine::instance();

    auto result = m_queueMgr->advance();

    if (result == QueueManager::RepeatOne) {
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

    if (result == QueueManager::EndOfQueue) {
        // Try autoplay before stopping
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

    // Advanced — play the new track
    m_currentTrack = m_queueMgr->currentTrack();
    m_currentTime = 0;
    emit timeChanged(m_currentTime);
    emitQueueChangedDebounced();
    emit trackChanged(m_currentTrack);
    playTrack(m_currentTrack);
}

// ── scheduleGaplessPrepare ──────────────────────────────────────────
void PlaybackState::scheduleGaplessPrepare()
{
    bool gapless = Settings::instance()->gaplessPlayback();
    bool crossfade = AudioEngine::instance()->crossfadeDurationMs() > 0;
    if (!gapless && !crossfade) return;
    if (m_currentSource != Local) return;

    Track next = m_queueMgr->peekNextTrack();
    if (next.filePath.isEmpty()) {
        AudioEngine::instance()->cancelNextTrack();
        return;
    }

    qDebug() << "[Gapless] Scheduling prepare:"
             << "current idx=" << m_queueMgr->currentIndex()
             << "title=" << m_currentTrack.title
             << "→ next title=" << next.title
             << "path=" << next.filePath;
    AudioEngine::instance()->prepareNextTrack(next.filePath);
}

// ── onGaplessTransition ─────────────────────────────────────────────
void PlaybackState::onGaplessTransition()
{
    qDebug() << "[Gapless] Transition occurred, advancing queue";

    if (m_queueMgr->isEmpty()) return;

    auto result = m_queueMgr->advance();

    if (result == QueueManager::RepeatOne) {
        m_currentTime = 0;
        emit timeChanged(m_currentTime);
        return;
    }

    if (result == QueueManager::EndOfQueue) {
        // Gapless transition at end — nothing more to do
        return;
    }

    // Advanced — update state for the new track
    m_currentTrack = m_queueMgr->currentTrack();
    m_currentTime = 0;
    emit timeChanged(m_currentTime);
    m_queuePersist->scheduleSave();
    emitQueueChangedDebounced();
    emit trackChanged(m_currentTrack);

    // Update volume leveling for the new track
    AudioEngine::instance()->setCurrentTrack(m_currentTrack);

    // Prepare the next-next track for continued gapless playback
    scheduleGaplessPrepare();
}
