#pragma once
#include <QVector>
#include "MusicData.h"

class QueueManager {
public:
    enum AdvanceResult { Advanced, RepeatOne, EndOfQueue };

    QueueManager() = default;

    // Queue CRUD
    void setQueue(const QVector<Track>& tracks);
    void addToQueue(const Track& track);
    void addToQueue(const QVector<Track>& tracks);
    void insertNext(const Track& track);
    void insertNext(const QVector<Track>& tracks);
    void removeFromQueue(int index);
    void moveTo(int fromIndex, int toIndex);
    void clearQueue();
    void clearUpcoming();

    // Queue access
    QVector<Track> queue() const { return m_queue; }
    QVector<Track> displayQueue() const;
    int currentIndex() const { return m_queueIndex; }
    Track currentTrack() const;
    Track peekNextTrack() const;
    bool isEmpty() const { return m_queue.isEmpty(); }
    int size() const { return m_queue.size(); }

    // Navigation — advance/retreat through the queue
    AdvanceResult advance();
    bool retreat();

    // Track lookup — find by ID or insert after current index
    int findOrInsertTrack(const Track& track);

    // Shuffle
    void toggleShuffle();
    bool shuffleEnabled() const { return m_shuffle; }
    void setShuffle(bool enabled);

    // Repeat (values match PlaybackState::RepeatMode: 0=Off, 1=All, 2=One)
    void cycleRepeat();
    int repeatMode() const { return m_repeat; }
    void setRepeatMode(int mode) { m_repeat = mode; }

    // Direct state access for restore
    void setCurrentIndex(int idx) { m_queueIndex = idx; }
    void restoreState(const QVector<Track>& tracks, int idx,
                      bool shuffle, int repeat);

private:
    void rebuildShuffleOrder();

    QVector<Track> m_queue;
    QVector<int> m_shuffledIndices;
    int m_queueIndex = -1;
    bool m_shuffle = false;
    int m_repeat = 0;  // 0=Off, 1=All, 2=One
};
