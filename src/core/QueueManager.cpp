#include "QueueManager.h"
#include <QRandomGenerator>
#include <QDebug>
#include <algorithm>

void QueueManager::setQueue(const QVector<Track>& tracks)
{
    m_queue = tracks;
    m_queueIndex = tracks.isEmpty() ? -1 : 0;
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

    m_queue.removeAt(index);

    if (m_queue.isEmpty()) {
        m_queueIndex = -1;
    } else if (index < m_queueIndex) {
        m_queueIndex--;
    } else if (index == m_queueIndex) {
        if (m_queueIndex >= m_queue.size())
            m_queueIndex = m_queue.size() - 1;
    }
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

    if (m_shuffle) rebuildShuffleOrder();
}

void QueueManager::clearQueue()
{
    m_queue.clear();
    m_userQueue.clear();
    m_queueIndex = -1;
}

void QueueManager::clearUpcoming()
{
    m_userQueue.clear();
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size())
        m_queue.resize(m_queueIndex + 1);
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
        else if (m_repeat == 1)  // All
            nextIdx = 0;
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

QueueManager::AdvanceResult QueueManager::advance()
{
    if (m_queue.isEmpty() && m_userQueue.isEmpty()) return EndOfQueue;
    if (m_repeat == 2) return RepeatOne;  // One

    // User queue takes priority — consume next user-queued track
    if (!m_userQueue.isEmpty()) {
        Track next = m_userQueue.takeFirst();
        int insertPos = (m_queueIndex >= 0) ? m_queueIndex + 1 : 0;
        if (insertPos > m_queue.size()) insertPos = m_queue.size();
        m_queue.insert(insertPos, next);
        m_queueIndex = insertPos;
        if (m_shuffle) rebuildShuffleOrder();
        qDebug() << "[Queue] Playing user-queued track:" << next.title
                 << "at index" << m_queueIndex;
        return Advanced;
    }

    if (m_shuffle) {
        if (m_shuffledIndices.isEmpty())
            rebuildShuffleOrder();

        if (!m_shuffledIndices.isEmpty()) {
            m_queueIndex = m_shuffledIndices.takeFirst();
        } else {
            if (m_repeat == 1) {  // All
                rebuildShuffleOrder();
                if (!m_shuffledIndices.isEmpty())
                    m_queueIndex = m_shuffledIndices.takeFirst();
            } else {
                return EndOfQueue;
            }
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

bool QueueManager::retreat()
{
    if (m_queue.isEmpty()) return false;
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
    if (m_shuffle)
        rebuildShuffleOrder();
    else
        m_shuffledIndices.clear();
}

void QueueManager::setShuffle(bool enabled)
{
    m_shuffle = enabled;
    if (enabled && !m_queue.isEmpty())
        rebuildShuffleOrder();
    else if (!enabled)
        m_shuffledIndices.clear();
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
