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
    void removeFromUserQueue(int index);
    void moveTo(int fromIndex, int toIndex);
    void clearQueue();
    void clearUpcoming();

    // Queue access
    QVector<Track> queue() const { return m_queue; }
    QVector<Track> userQueue() const { return m_userQueue; }
    QVector<Track> displayQueue() const;
    int currentIndex() const { return m_queueIndex; }
    int userQueueSize() const { return m_userQueue.size(); }
    Track currentTrack() const;
    Track peekNextTrack() const;
    bool isEmpty() const { return m_queue.isEmpty() && m_userQueue.isEmpty(); }
    int size() const { return m_queue.size() + m_userQueue.size(); }

    // Navigation — advance/retreat through the queue
    // userInitiated=true when user presses Next/Previous (bypasses Repeat One)
    // userInitiated=false for auto-advance (track finished naturally)
    AdvanceResult advance(bool userInitiated = false);
    bool retreat(bool userInitiated = false);

    // Track lookup — find by ID or insert after current index
    int findOrInsertTrack(const Track& track);

    // Shuffle
    void toggleShuffle();
    bool shuffleEnabled() const { return m_shuffle; }
    void setShuffle(bool enabled);
    void invalidateShuffleOrder();  // Rebuild shuffle excluding current (after direct track selection)

    // Repeat (values match PlaybackState::RepeatMode: 0=Off, 1=All, 2=One)
    void cycleRepeat();
    int repeatMode() const { return m_repeat; }
    void setRepeatMode(int mode) { m_repeat = mode; }

    // Direct state access for restore
    void setCurrentIndex(int idx) { m_queueIndex = idx; }
    void restoreState(const QVector<Track>& tracks, int idx,
                      bool shuffle, int repeat,
                      const QVector<Track>& userQueue = {});

    // Shuffle order persistence
    QVector<int> shuffledIndices() const { return m_shuffledIndices; }
    QVector<QString> shuffleHistory() const { return m_shuffleHistory; }
    void setShuffledIndices(const QVector<int>& indices) { m_shuffledIndices = indices; }
    void setShuffleHistory(const QVector<QString>& history) { m_shuffleHistory = history; }

private:
    void rebuildShuffleOrder();

    QVector<Track> m_queue;
    QVector<Track> m_userQueue;    // User-added tracks — survive setQueue(), play first
    QVector<int> m_shuffledIndices;
    QVector<QString> m_shuffleHistory;  // Previously-played filePaths for "previous" in shuffle mode
    int m_queueIndex = -1;
    bool m_shuffle = false;
    int m_repeat = 0;  // 0=Off, 1=All, 2=One
};
