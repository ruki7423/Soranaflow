#include <QtTest/QtTest>
#include "QueueManager.h"

static Track makeTrack(const QString& id, const QString& title = {})
{
    Track t;
    t.id = id;
    t.title = title.isEmpty() ? id : title;
    return t;
}

static QVector<Track> makeTracks(int count)
{
    QVector<Track> v;
    for (int i = 1; i <= count; ++i)
        v.append(makeTrack(QString::number(i)));
    return v;
}

class tst_QueueManager : public QObject {
    Q_OBJECT

private slots:
    // ── Basic CRUD ───────────────────────────────────────────────
    void setQueue_setsIndexToZero()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(5));
        QCOMPARE(qm.size(), 5);
        QCOMPARE(qm.currentIndex(), 0);
        QCOMPARE(qm.currentTrack().id, QStringLiteral("1"));
    }

    void setQueue_emptyList()
    {
        QueueManager qm;
        qm.setQueue({});
        QCOMPARE(qm.size(), 0);
        QCOMPARE(qm.currentIndex(), -1);
        QVERIFY(qm.isEmpty());
        QCOMPARE(qm.currentTrack().id, QString());
    }

    void addToQueue_single()
    {
        QueueManager qm;
        qm.addToQueue(makeTrack("a"));
        QCOMPARE(qm.size(), 1);
    }

    void addToQueue_batch()
    {
        QueueManager qm;
        qm.addToQueue(makeTracks(3));
        QCOMPARE(qm.size(), 3);
    }

    void insertNext_insertsIntoUserQueue()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));  // main=[1*, 2, 3]
        qm.insertNext(makeTrack("X"));  // userQueue=[X]
        QCOMPARE(qm.size(), 4);
        QCOMPARE(qm.userQueue().at(0).id, QStringLiteral("X"));
        // Advance should play X next (from user queue)
        auto r = qm.advance();
        QCOMPARE(r, QueueManager::Advanced);
        QCOMPARE(qm.currentTrack().id, QStringLiteral("X"));
    }

    void insertNext_batch()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(2));  // main=[1*, 2]
        QVector<Track> batch = {makeTrack("A"), makeTrack("B")};
        qm.insertNext(batch);  // userQueue=[A, B]
        QCOMPARE(qm.size(), 4);
        QCOMPARE(qm.userQueue().at(0).id, QStringLiteral("A"));
        QCOMPARE(qm.userQueue().at(1).id, QStringLiteral("B"));
    }

    void removeFromQueue_beforeCurrent()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(5));
        qm.setCurrentIndex(2);  // current = track "3"
        qm.removeFromQueue(0);  // remove track "1"
        QCOMPARE(qm.currentIndex(), 1);  // shifted left
        QCOMPARE(qm.currentTrack().id, QStringLiteral("3"));
    }

    void removeFromQueue_current()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));  // [1*, 2, 3]
        qm.removeFromQueue(0);  // remove current
        QCOMPARE(qm.currentIndex(), 0);
        QCOMPARE(qm.currentTrack().id, QStringLiteral("2"));
    }

    void removeFromQueue_afterCurrent()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));  // [1*, 2, 3]
        qm.removeFromQueue(2);  // remove track "3"
        QCOMPARE(qm.currentIndex(), 0);
        QCOMPARE(qm.size(), 2);
    }

    void removeFromQueue_invalidIndex()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.removeFromQueue(-1);
        qm.removeFromQueue(99);
        QCOMPARE(qm.size(), 3);
    }

    void removeFromQueue_lastElement()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(1));
        qm.removeFromQueue(0);
        QVERIFY(qm.isEmpty());
        QCOMPARE(qm.currentIndex(), -1);
    }

    void moveTo_basic()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(4));  // [1*, 2, 3, 4]
        qm.moveTo(0, 2);  // move "1" to index 2: [2, 3, 1, 4]
        QCOMPARE(qm.currentIndex(), 2);  // current follows
        QCOMPARE(qm.currentTrack().id, QStringLiteral("1"));
        QCOMPARE(qm.queue().at(0).id, QStringLiteral("2"));
    }

    void moveTo_invalidIndices()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.moveTo(-1, 1);
        qm.moveTo(0, 99);
        qm.moveTo(1, 1);  // same index
        QCOMPARE(qm.size(), 3);
    }

    void clearQueue_resetsEverything()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(5));
        qm.clearQueue();
        QVERIFY(qm.isEmpty());
        QCOMPARE(qm.currentIndex(), -1);
    }

    void clearUpcoming_keepsCurrentAndBefore()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(5));
        qm.setCurrentIndex(2);  // current = track "3"
        qm.clearUpcoming();
        QCOMPARE(qm.size(), 3);  // tracks 1, 2, 3
    }

    // ── Navigation ───────────────────────────────────────────────
    void advance_sequential()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));  // index 0
        auto r = qm.advance();
        QCOMPARE(r, QueueManager::Advanced);
        QCOMPARE(qm.currentIndex(), 1);
    }

    void advance_endOfQueue()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(2));
        qm.setCurrentIndex(1);  // last track
        auto r = qm.advance();
        QCOMPARE(r, QueueManager::EndOfQueue);
    }

    void advance_repeatAll_wraps()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.setRepeatMode(1);  // All
        qm.setCurrentIndex(2);  // last track
        auto r = qm.advance();
        QCOMPARE(r, QueueManager::Advanced);
        QCOMPARE(qm.currentIndex(), 0);  // wrapped
    }

    void advance_repeatOne()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.setRepeatMode(2);  // One
        qm.setCurrentIndex(1);
        auto r = qm.advance();
        QCOMPARE(r, QueueManager::RepeatOne);
        QCOMPARE(qm.currentIndex(), 1);  // unchanged
    }

    void advance_emptyQueue()
    {
        QueueManager qm;
        auto r = qm.advance();
        QCOMPARE(r, QueueManager::EndOfQueue);
    }

    void retreat_basic()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.setCurrentIndex(2);
        QVERIFY(qm.retreat());
        QCOMPARE(qm.currentIndex(), 1);
    }

    void retreat_atStart_repeatOff()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        QVERIFY(!qm.retreat());
        QCOMPARE(qm.currentIndex(), 0);
    }

    void retreat_atStart_repeatAll_wraps()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.setRepeatMode(1);  // All
        QVERIFY(qm.retreat());
        QCOMPARE(qm.currentIndex(), 2);  // wrapped to end
    }

    void retreat_emptyQueue()
    {
        QueueManager qm;
        QVERIFY(!qm.retreat());
    }

    // ── Peek ─────────────────────────────────────────────────────
    void peekNextTrack_sequential()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));  // at index 0
        QCOMPARE(qm.peekNextTrack().id, QStringLiteral("2"));
    }

    void peekNextTrack_atEnd_repeatOff()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(2));
        qm.setCurrentIndex(1);
        QCOMPARE(qm.peekNextTrack().id, QString());
    }

    void peekNextTrack_atEnd_repeatAll()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.setRepeatMode(1);
        qm.setCurrentIndex(2);
        QCOMPARE(qm.peekNextTrack().id, QStringLiteral("1"));
    }

    void peekNextTrack_repeatOne_returnsCurrent()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.setRepeatMode(2);
        qm.setCurrentIndex(1);
        QCOMPARE(qm.peekNextTrack().id, QStringLiteral("2"));
    }

    // ── Shuffle ──────────────────────────────────────────────────
    void shuffle_toggle()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(10));
        QVERIFY(!qm.shuffleEnabled());
        qm.toggleShuffle();
        QVERIFY(qm.shuffleEnabled());
        qm.toggleShuffle();
        QVERIFY(!qm.shuffleEnabled());
    }

    void shuffle_advanceVisitsAll()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(5));
        qm.setShuffle(true);

        QSet<QString> visited;
        visited.insert(qm.currentTrack().id);

        for (int i = 0; i < 4; ++i) {
            auto r = qm.advance();
            QCOMPARE(r, QueueManager::Advanced);
            visited.insert(qm.currentTrack().id);
        }
        QCOMPARE(visited.size(), 5);
    }

    void shuffle_excludesCurrent()
    {
        // After set, advancing shouldn't immediately repeat the first track
        QueueManager qm;
        qm.setQueue(makeTracks(20));
        qm.setShuffle(true);
        QString first = qm.currentTrack().id;
        qm.advance();
        // With 20 tracks, extremely unlikely to get same track
        // (not a hard guarantee due to randomness, but practical test)
        QVERIFY(qm.currentTrack().id != first || qm.size() == 1);
    }

    // ── Repeat ───────────────────────────────────────────────────
    void cycleRepeat_cycles()
    {
        QueueManager qm;
        QCOMPARE(qm.repeatMode(), 0);
        qm.cycleRepeat();
        QCOMPARE(qm.repeatMode(), 1);
        qm.cycleRepeat();
        QCOMPARE(qm.repeatMode(), 2);
        qm.cycleRepeat();
        QCOMPARE(qm.repeatMode(), 0);
    }

    // ── findOrInsertTrack ────────────────────────────────────────
    void findOrInsertTrack_findsExisting()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        int idx = qm.findOrInsertTrack(makeTrack("2"));
        QCOMPARE(idx, 1);
        QCOMPARE(qm.size(), 3);  // not inserted
    }

    void findOrInsertTrack_insertsNew()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        int idx = qm.findOrInsertTrack(makeTrack("X"));
        QCOMPARE(idx, 1);  // after current (0)
        QCOMPARE(qm.size(), 4);
    }

    // ── restoreState ─────────────────────────────────────────────
    void restoreState_basic()
    {
        QueueManager qm;
        qm.restoreState(makeTracks(5), 3, true, 2);
        QCOMPARE(qm.size(), 5);
        QCOMPARE(qm.currentIndex(), 3);
        QVERIFY(qm.shuffleEnabled());
        QCOMPARE(qm.repeatMode(), 2);
    }

    // ── displayQueue ─────────────────────────────────────────────
    void displayQueue_noShuffle()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        auto dq = qm.displayQueue();
        QCOMPARE(dq.size(), 3);
        QCOMPARE(dq.at(0).id, QStringLiteral("1"));
    }

    void displayQueue_withShuffle()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(5));
        qm.setShuffle(true);
        auto dq = qm.displayQueue();
        // First item is always the current track
        QCOMPARE(dq.at(0).id, qm.currentTrack().id);
        // Total items = current + shuffled remaining
        QCOMPARE(dq.size(), 5);
    }

    // ── User Queue ────────────────────────────────────────────────
    void userQueue_addToQueue_goesToUserQueue()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.addToQueue(makeTrack("U1"));
        QCOMPARE(qm.userQueueSize(), 1);
        QCOMPARE(qm.userQueue().at(0).id, QStringLiteral("U1"));
        // Main queue unchanged
        QCOMPARE(qm.queue().size(), 3);
    }

    void userQueue_survivesSetQueue()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.addToQueue(makeTrack("U1"));
        qm.addToQueue(makeTrack("U2"));
        QCOMPARE(qm.userQueueSize(), 2);

        // setQueue replaces main queue but keeps user queue
        qm.setQueue(makeTracks(5));
        QCOMPARE(qm.queue().size(), 5);
        QCOMPARE(qm.userQueueSize(), 2);
        QCOMPARE(qm.userQueue().at(0).id, QStringLiteral("U1"));
    }

    void userQueue_advancePlaysUserQueueFirst()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));  // main=[1*, 2, 3]
        qm.addToQueue(makeTrack("U1"));
        qm.addToQueue(makeTrack("U2"));

        // First advance should play U1 from user queue
        auto r1 = qm.advance();
        QCOMPARE(r1, QueueManager::Advanced);
        QCOMPARE(qm.currentTrack().id, QStringLiteral("U1"));

        // Second advance should play U2
        auto r2 = qm.advance();
        QCOMPARE(r2, QueueManager::Advanced);
        QCOMPARE(qm.currentTrack().id, QStringLiteral("U2"));

        // Third advance should continue main queue (track "2")
        auto r3 = qm.advance();
        QCOMPARE(r3, QueueManager::Advanced);
        QCOMPARE(qm.currentTrack().id, QStringLiteral("2"));
    }

    void userQueue_peekNextReturnsUserQueueFirst()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.addToQueue(makeTrack("U1"));
        QCOMPARE(qm.peekNextTrack().id, QStringLiteral("U1"));
    }

    void userQueue_displayQueueShowsUserItemsFirst()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));  // main=[1*, 2, 3]
        qm.addToQueue(makeTrack("U1"));

        auto dq = qm.displayQueue();
        // current + userQueue + remaining main
        QCOMPARE(dq.size(), 4);  // 1(current) + 1(user) + 2(remaining)
        QCOMPARE(dq.at(0).id, QStringLiteral("1"));   // current
        QCOMPARE(dq.at(1).id, QStringLiteral("U1"));  // user queue
        QCOMPARE(dq.at(2).id, QStringLiteral("2"));   // main queue
    }

    void userQueue_clearQueueClearsBoth()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(3));
        qm.addToQueue(makeTrack("U1"));
        qm.clearQueue();
        QVERIFY(qm.isEmpty());
        QCOMPARE(qm.userQueueSize(), 0);
    }

    void userQueue_clearUpcomingClearsBoth()
    {
        QueueManager qm;
        qm.setQueue(makeTracks(5));
        qm.setCurrentIndex(2);
        qm.addToQueue(makeTrack("U1"));
        qm.clearUpcoming();
        QCOMPARE(qm.queue().size(), 3);  // tracks 1, 2, 3
        QCOMPARE(qm.userQueueSize(), 0);
    }

    void userQueue_removeFromUserQueue()
    {
        QueueManager qm;
        qm.addToQueue(makeTrack("U1"));
        qm.addToQueue(makeTrack("U2"));
        qm.addToQueue(makeTrack("U3"));
        qm.removeFromUserQueue(1);  // remove U2
        QCOMPARE(qm.userQueueSize(), 2);
        QCOMPARE(qm.userQueue().at(0).id, QStringLiteral("U1"));
        QCOMPARE(qm.userQueue().at(1).id, QStringLiteral("U3"));
    }

    void userQueue_restoreWithUserQueue()
    {
        QueueManager qm;
        QVector<Track> userTracks = {makeTrack("U1"), makeTrack("U2")};
        qm.restoreState(makeTracks(3), 1, false, 0, userTracks);
        QCOMPARE(qm.queue().size(), 3);
        QCOMPARE(qm.userQueueSize(), 2);
        QCOMPARE(qm.currentIndex(), 1);
    }
};

QTEST_MAIN(tst_QueueManager)
#include "tst_QueueManager.moc"
