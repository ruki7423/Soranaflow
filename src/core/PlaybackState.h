#pragma once
#include <QObject>
#include <QVector>
#include <QTimer>
#include "MusicData.h"

class AudioEngine;
class AutoplayManager;

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
    bool shuffleEnabled() const { return m_shuffle; }
    RepeatMode repeatMode() const { return m_repeat; }
    Track currentTrack() const { return m_currentTrack; }
    QVector<Track> queue() const { return m_queue; }
    QVector<Track> displayQueue() const;
    Track peekNextTrack() const;
    int queueIndex() const { return m_queueIndex; }

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
    void moveTo(int fromIndex, int toIndex);
    void clearQueue();
    void clearUpcoming();
    void saveQueueToSettings();
    void restoreQueueFromSettings();

signals:
    void playStateChanged(bool playing);
    void trackChanged(const Track& track);
    void timeChanged(int seconds);
    void queueChanged();
    void volumeChanged(int volume);
    void shuffleChanged(bool enabled);
    void repeatChanged(RepeatMode mode);
    void autoplayTrackStarted();

private:
    explicit PlaybackState(QObject* parent = nullptr);
    void connectToAudioEngine();
    void connectToMusicKitPlayer();
    void playNextTrack();
    void rebuildShuffleOrder();
    void emitQueueChangedDebounced();
    void scheduleSave();
    void doSave();
    void scheduleGaplessPrepare();
    void onGaplessTransition();

    QTimer* m_queueChangeDebounce = nullptr;
    QTimer* m_saveTimer = nullptr;
    QTimer* m_volumeSaveTimer = nullptr;
    bool m_restoring = false;
    bool m_playing = false;
    int m_currentTime = 0;
    int m_volume = 75;
    bool m_shuffle = false;
    RepeatMode m_repeat = Off;
    Track m_currentTrack;
    QVector<Track> m_queue;
    int m_queueIndex = -1;
    QVector<int> m_shuffledIndices;  // shuffled order of indices after current
    PlaybackSource m_currentSource = Local;
    AutoplayManager* m_autoplay = nullptr;
    bool m_autoplayActive = false;
};
