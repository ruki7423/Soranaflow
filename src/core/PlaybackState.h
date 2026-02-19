#pragma once
#include <QObject>
#include <QVector>
#include <QTimer>
#include "MusicData.h"

class AudioEngine;
class AutoplayManager;
class QueueManager;
class QueuePersistence;

class PlaybackState : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playStateChanged)
    Q_PROPERTY(int currentTime READ currentTime NOTIFY timeChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)

public:
    enum RepeatMode { Off, All, One };
    Q_ENUM(RepeatMode)

    enum PlaybackSource { Local, AppleMusic };
    Q_ENUM(PlaybackSource)
    PlaybackSource currentSource() const { return m_currentSource; }

    static PlaybackState* instance();

    bool isPlaying() const { return m_playing; }
    int currentTime() const { return m_currentTime; }
    int volume() const { return m_volume; }
    bool shuffleEnabled() const;
    RepeatMode repeatMode() const;
    Track currentTrack() const { return m_currentTrack; }
    QVector<Track> queue() const;
    QVector<Track> displayQueue() const;
    Track peekNextTrack() const;
    int queueIndex() const;

    QueueManager* queueManager() const { return m_queueMgr; }

public slots:
    void playPause();
    void next();
    void previous();
    void seek(int position);
    void setVolume(int vol);
    void toggleShuffle();
    void cycleRepeat();
    void playTrack(const Track& track);
    void setCurrentTrackInfo(const Track& track);
    void setQueue(const QVector<Track>& tracks);
    void addToQueue(const Track& track);
    void addToQueue(const QVector<Track>& tracks);
    void insertNext(const Track& track);
    void insertNext(const QVector<Track>& tracks);
    void removeFromQueue(int index);
    void removeFromUserQueue(int index);
    void moveTo(int fromIndex, int toIndex);
    void clearQueue();
    void clearUpcoming();
    void saveQueueToSettings();
    void restoreQueueFromSettings();
    void flushPendingSaves();

signals:
    void playStateChanged(bool playing);
    void trackChanged(const Track& track);
    void timeChanged(int seconds);
    void queueChanged();
    void volumeChanged(int volume);
    void shuffleChanged(bool enabled);
    void repeatChanged(RepeatMode mode);
    void autoplayTrackStarted();
    void queueExhausted();  // emitted when end of queue reached (repeat=off, no autoplay)

private:
    explicit PlaybackState(QObject* parent = nullptr);
    void connectToAudioEngine();
    void connectToMusicKitPlayer();
    void playNextTrack(bool userInitiated = false);
    void loadAndPlayTrack(const Track& track);  // Audio loading only — no queue state changes
    void emitQueueChangedDebounced();
    void scheduleGaplessPrepare();
    void onGaplessTransition();

    QueueManager* m_queueMgr = nullptr;
    QueuePersistence* m_queuePersist = nullptr;
    QTimer* m_queueChangeDebounce = nullptr;
    QTimer* m_volumeSaveTimer = nullptr;
    bool m_playing = false;
    int m_currentTime = 0;
    int m_volume = 75;
    Track m_currentTrack;
    PlaybackSource m_currentSource = Local;
    AutoplayManager* m_autoplay = nullptr;
    bool m_autoplayActive = false;
    bool m_trackTransitionPending = false;  // guards against double-advance race
};
