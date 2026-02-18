#include "QueueManager.h"
#include <QRandomGenerator>
#include <QDebug>
#include <algorithm>

void QueueManager::setQueue(const QVector<Track>& tracks)
{
    m_queue = tracks;
    m_queueIndex = tracks.isEmpty() ? -1 : 0;
    m_shuffleHistory.clear();
    // m_userQueue intentionally NOT cleared — user-added tracks persist
    if (m_shuffle && !tracks.isEmpty())
        rebuildShuffleOrder();
}

void QueueManager::addToQueue(const Track& track)
{
    m_userQueue.append(track);
    qDebug() << "[Queue] Added to user queue:" << track.title
             << "(" << m_userQueue.size() << "pending)";
}

void QueueManager::addToQueue(const QVector<Track>& tracks)
{
    m_userQueue.append(tracks);
    qDebug() << "[Queue] Added" << tracks.size() << "tracks to user queue"
             << "(" << m_userQueue.size() << "pending)";
}

void QueueManager::insertNext(const Track& track)
{
    m_userQueue.prepend(track);
}

void QueueManager::insertNext(const QVector<Track>& tracks)
{
    for (int i = tracks.size() - 1; i >= 0; --i)
        m_userQueue.prepend(tracks[i]);
}

void QueueManager::removeFromQueue(int index)
{
    if (index < 0 || index >= m_queue.size()) return;

    QString removedPath = m_queue[index].filePath;
    m_queue.removeAt(index);

    if (m_queue.isEmpty()) {
        m_queueIndex = -1;
    } else if (index < m_queueIndex) {
        m_queueIndex--;
    } else if (index == m_queueIndex) {
        if (m_queueIndex >= m_queue.size())
            m_queueIndex = m_queue.size() - 1;
    }

    // Remove only the deleted track from history, not all history
    m_shuffleHistory.removeAll(removedPath);
    if (m_shuffle && !m_queue.isEmpty())
        rebuildShuffleOrder();
}

void QueueManager::removeFromUserQueue(int index)
{
    if (index < 0 || index >= m_userQueue.size()) return;
    m_userQueue.removeAt(index);
}

void QueueManager::moveTo(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_queue.size()) return;
    if (toIndex < 0 || toIndex >= m_queue.size()) return;
    if (fromIndex == toIndex) return;

    Track track = m_queue.takeAt(fromIndex);
    m_queue.insert(toIndex, track);

    if (m_queueIndex == fromIndex) {
        m_queueIndex = toIndex;
    } else {
        if (fromIndex < m_queueIndex && toIndex >= m_queueIndex)
            m_queueIndex--;
        else if (fromIndex > m_queueIndex && toIndex <= m_queueIndex)
            m_queueIndex++;
    }

    // Do NOT clear m_shuffleHistory — filePaths don't change on reorder
    if (m_shuffle) rebuildShuffleOrder();
}

void QueueManager::clearQueue()
{
    m_queue.clear();
    m_userQueue.clear();
    m_queueIndex = -1;
    m_shuffledIndices.clear();
    m_shuffleHistory.clear();
}

void QueueManager::clearUpcoming()
{
    m_userQueue.clear();
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size())
        m_queue.resize(m_queueIndex + 1);
    m_shuffleHistory.clear();
    m_shuffledIndices.clear();
}

Track QueueManager::currentTrack() const
{
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size())
        return m_queue.at(m_queueIndex);
    return Track();
}

QVector<Track> QueueManager::displayQueue() const
{
    QVector<Track> result;

    // Current track
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size())
        result.append(m_queue.at(m_queueIndex));

    // User queue items play next (priority)
    result.append(m_userQueue);

    // Remaining main queue
    if (!m_shuffle || m_shuffledIndices.isEmpty()) {
        int startIdx = (m_queueIndex >= 0) ? m_queueIndex + 1 : 0;
        for (int i = startIdx; i < m_queue.size(); ++i)
            result.append(m_queue.at(i));
    } else {
        for (int idx : m_shuffledIndices) {
            if (idx >= 0 && idx < m_queue.size())
                result.append(m_queue.at(idx));
        }
    }

    return result;
}

Track QueueManager::peekNextTrack() const
{
    if (m_queue.isEmpty() && m_userQueue.isEmpty()) return Track();
    if (m_repeat == 2) return currentTrack();  // One

    // User queue takes priority
    if (!m_userQueue.isEmpty())
        return m_userQueue.first();

    int nextIdx = -1;
    if (m_shuffle) {
        if (!m_shuffledIndices.isEmpty())
            nextIdx = m_shuffledIndices.first();
        else if (m_repeat == 1 && !m_queue.isEmpty())
            nextIdx = 0;  // Repeat All: next cycle will start from some track
        else
            return Track();  // No repeat or empty queue
    } else {
        nextIdx = m_queueIndex + 1;
        if (nextIdx >= m_queue.size()) {
            if (m_repeat == 1)  // All
                nextIdx = 0;
            else
                return Track();
        }
    }

    if (nextIdx >= 0 && nextIdx < m_queue.size())
        return m_queue.at(nextIdx);
    return Track();
}

QueueManager::AdvanceResult QueueManager::advance(bool userInitiated)
{
    if (m_queue.isEmpty() && m_userQueue.isEmpty()) return EndOfQueue;
    // Repeat One: only auto-repeat on natural track end, not manual Next
    if (m_repeat == 2 && !userInitiated) return RepeatOne;

    // User queue takes priority — consume next user-queued track
    if (!m_userQueue.isEmpty()) {
        Track next = m_userQueue.takeFirst();
        int insertPos = (m_queueIndex >= 0) ? m_queueIndex + 1 : 0;
        if (insertPos > m_queue.size()) insertPos = m_queue.size();
        m_queue.insert(insertPos, next);
        if (m_shuffle && m_queueIndex >= 0 && m_queueIndex < m_queue.size())
            m_shuffleHistory.append(m_queue[m_queueIndex].filePath);
        m_queueIndex = insertPos;
        if (m_shuffle) rebuildShuffleOrder();
        qDebug() << "[Queue] Playing user-queued track:" << next.title
                 << "at index" << m_queueIndex;
        return Advanced;
    }

    if (m_shuffle) {
        if (m_shuffledIndices.isEmpty()) {
            if (m_repeat != 1)  // Not "Repeat All" — all songs played
                return EndOfQueue;

            // Smart shuffle: new cycle — include ALL tracks, prevent back-to-back
            int lastPlayed = m_queueIndex;
            m_shuffledIndices.clear();
            for (int i = 0; i < m_queue.size(); ++i)
                m_shuffledIndices.append(i);
            // Fisher-Yates shuffle
            auto* rng = QRandomGenerator::global();
            for (int i = m_shuffledIndices.size() - 1; i > 0; --i) {
                int j = rng->bounded(i + 1);
                std::swap(m_shuffledIndices[i], m_shuffledIndices[j]);
            }
            // Back-to-back prevention: first of new cycle != last of old cycle
            if (m_shuffledIndices.size() > 1 && m_shuffledIndices.first() == lastPlayed) {
                int swapWith = 1 + rng->bounded(m_shuffledIndices.size() - 1);
                std::swap(m_shuffledIndices[0], m_shuffledIndices[swapWith]);
            }
            qDebug() << "[Shuffle] New cycle —" << m_shuffledIndices.size() << "tracks reshuffled";
        }

        if (!m_shuffledIndices.isEmpty()) {
            if (m_queueIndex >= 0 && m_queueIndex < m_queue.size())
                m_shuffleHistory.append(m_queue[m_queueIndex].filePath);
            m_queueIndex = m_shuffledIndices.takeFirst();
        } else {
            return EndOfQueue;  // Queue has 0 tracks
        }
    } else {
        m_queueIndex++;
        if (m_queueIndex >= m_queue.size()) {
            if (m_repeat == 1) {  // All
                m_queueIndex = 0;
            } else {
                m_queueIndex = m_queue.size() - 1;
                return EndOfQueue;
            }
        }
    }

    return Advanced;
}

bool QueueManager::retreat(bool userInitiated)
{
    Q_UNUSED(userInitiated)  // retreat doesn't have RepeatOne logic, but keep signature consistent
    if (m_queue.isEmpty()) return false;

    if (m_shuffle) {
        if (m_shuffleHistory.isEmpty()) return false;
        QString prevPath = m_shuffleHistory.takeLast();

        // Find the track by filePath in current queue
        for (int i = 0; i < m_queue.size(); ++i) {
            if (m_queue[i].filePath == prevPath) {
                m_shuffledIndices.prepend(m_queueIndex);
                m_queueIndex = i;
                return true;
            }
        }

        // Track no longer in queue — try next in history
        if (!m_shuffleHistory.isEmpty())
            return retreat();
        return false;
    }

    if (m_queueIndex > 0) {
        m_queueIndex--;
        return true;
    }
    if (m_repeat == 1) {  // All
        m_queueIndex = m_queue.size() - 1;
        return true;
    }
    return false;
}

int QueueManager::findOrInsertTrack(const Track& track)
{
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).id == track.id)
            return i;
    }
    int insertPos = (m_queueIndex >= 0) ? m_queueIndex + 1 : 0;
    m_queue.insert(insertPos, track);
    return insertPos;
}

void QueueManager::toggleShuffle()
{
    m_shuffle = !m_shuffle;
    m_shuffleHistory.clear();
    if (m_shuffle && !m_queue.isEmpty())
        rebuildShuffleOrder();
    else if (!m_shuffle)
        m_shuffledIndices.clear();
}

void QueueManager::setShuffle(bool enabled)
{
    m_shuffle = enabled;
    m_shuffleHistory.clear();
    if (enabled && !m_queue.isEmpty())
        rebuildShuffleOrder();
    else if (!enabled)
        m_shuffledIndices.clear();
}

void QueueManager::invalidateShuffleOrder()
{
    if (m_shuffle && !m_queue.isEmpty()) {
        m_shuffleHistory.clear();
        rebuildShuffleOrder();
    }
}

void QueueManager::cycleRepeat()
{
    switch (m_repeat) {
    case 0: m_repeat = 1; break;  // Off → All
    case 1: m_repeat = 2; break;  // All → One
    case 2: m_repeat = 0; break;  // One → Off
    }
}

void QueueManager::restoreState(const QVector<Track>& tracks, int idx,
                                bool shuffle, int repeat,
                                const QVector<Track>& userQueue)
{
    m_queue = tracks;
    m_queueIndex = idx;
    m_shuffle = shuffle;
    m_repeat = repeat;
    m_userQueue = userQueue;
    m_shuffleHistory.clear();
    if (m_shuffle && !m_queue.isEmpty())
        rebuildShuffleOrder();
}

void QueueManager::rebuildShuffleOrder()
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
