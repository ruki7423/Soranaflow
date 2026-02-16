#include <QtTest/QtTest>
#include <QSignalSpy>
#include "apple/MusicKitStateMachine.h"

// MusicKit playback states (from MusicKit JS API)
enum MKState {
    MK_None     = 0,
    MK_Loading  = 1,
    MK_Playing  = 2,
    MK_Paused   = 3,
    MK_Stopped  = 4,
    MK_Ended    = 5,
    MK_Seeking  = 6,
    MK_Waiting  = 7,
    MK_Stalled  = 8,
    MK_Completed = 9
};

using AMState     = MusicKitStateMachine::AMState;
using AMPlayState = MusicKitStateMachine::AMPlayState;

class tst_MusicKitStateMachine : public QObject {
    Q_OBJECT

private slots:

    // ── Initial state ────────────────────────────────────────────
    void initialState()
    {
        MusicKitStateMachine sm;
        QCOMPARE(sm.amState(), AMState::Idle);
        QCOMPARE(sm.amPlayState(), AMPlayState::Idle);
        QVERIFY(sm.pendingPlaySongId().isEmpty());
    }

    // ── requestPlay from Idle ────────────────────────────────────
    void requestPlay_fromIdle_emitsSignals()
    {
        MusicKitStateMachine sm;
        QSignalSpy execSpy(&sm, &MusicKitStateMachine::executePlayRequested);
        QSignalSpy stateSpy(&sm, &MusicKitStateMachine::amStateChanged);
        QSignalSpy playSpy(&sm, &MusicKitStateMachine::amPlayStateChanged);

        sm.requestPlay("song-1");

        QCOMPARE(sm.amState(), AMState::Loading);
        QCOMPARE(sm.amPlayState(), AMPlayState::Buffering);  // Pending→Buffering on Loading
        QCOMPARE(sm.pendingPlaySongId(), QStringLiteral("song-1"));

        QCOMPARE(execSpy.count(), 1);
        QCOMPARE(execSpy.at(0).at(0).toString(), QStringLiteral("song-1"));

        // amPlayStateChanged: Pending, then Buffering (from setAMState(Loading))
        QCOMPARE(playSpy.count(), 2);
        QCOMPARE(playSpy.at(0).at(0).value<AMPlayState>(), AMPlayState::Pending);
        QCOMPARE(playSpy.at(1).at(0).value<AMPlayState>(), AMPlayState::Buffering);
    }

    // ── Full play cycle: Idle → Loading → Playing ────────────────
    void fullPlayCycle_idle_loading_playing()
    {
        MusicKitStateMachine sm;
        QSignalSpy activeSpy(&sm, &MusicKitStateMachine::playbackActiveChanged);

        sm.requestPlay("song-1");
        QCOMPARE(sm.amState(), AMState::Loading);

        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amState(), AMState::Playing);
        QCOMPARE(sm.amPlayState(), AMPlayState::Playing);

        // playbackActiveChanged(true) emitted
        QVERIFY(activeSpy.count() >= 1);
        QCOMPARE(activeSpy.last().at(0).toBool(), true);
    }

    // ── Playing → paused (mkState 3) → Idle ─────────────────────
    void playing_paused_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amState(), AMState::Playing);

        QSignalSpy activeSpy(&sm, &MusicKitStateMachine::playbackActiveChanged);
        sm.onMusicKitStateChanged(MK_Paused);
        QCOMPARE(sm.amState(), AMState::Idle);
        QCOMPARE(sm.amPlayState(), AMPlayState::Idle);

        QCOMPARE(activeSpy.count(), 1);
        QCOMPARE(activeSpy.at(0).at(0).toBool(), false);
    }

    // ── Playing → stopped (mkState 4) → Idle ────────────────────
    void playing_stopped_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);

        sm.onMusicKitStateChanged(MK_Stopped);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── Playing → ended (mkState 5) → Idle ──────────────────────
    void playing_ended_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);

        sm.onMusicKitStateChanged(MK_Ended);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── Playing → completed (mkState 9) → Idle ──────────────────
    void playing_completed_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);

        sm.onMusicKitStateChanged(MK_Completed);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── Playing → stalled (mkState 8) → Stalled ─────────────────
    void playing_stalled()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);

        sm.onMusicKitStateChanged(MK_Stalled);
        QCOMPARE(sm.amState(), AMState::Stalled);
    }

    // ── Stall recovery: Stalled → mkState 2 → Playing ───────────
    void stallRecovery()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.onMusicKitStateChanged(MK_Stalled);
        QCOMPARE(sm.amState(), AMState::Stalled);

        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amState(), AMState::Playing);
    }

    // ── Stalled → stopped → Idle ────────────────────────────────
    void stalled_stopped_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.onMusicKitStateChanged(MK_Stalled);

        sm.onMusicKitStateChanged(MK_Stopped);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── requestPlay while Playing → queue + Stopping ─────────────
    void requestPlay_whilePlaying_queuesAndStops()
    {
        MusicKitStateMachine sm;
        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);

        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amState(), AMState::Playing);

        sm.requestPlay("song-2");
        QCOMPARE(sm.amState(), AMState::Stopping);
        QCOMPARE(stopSpy.count(), 1);
    }

    // ── requestPlay while Loading → queue + Stopping ─────────────
    void requestPlay_whileLoading_queuesAndStops()
    {
        MusicKitStateMachine sm;
        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);

        sm.requestPlay("song-1");
        QCOMPARE(sm.amState(), AMState::Loading);

        sm.requestPlay("song-2");
        QCOMPARE(sm.amState(), AMState::Stopping);
        QCOMPARE(stopSpy.count(), 1);
    }

    // ── requestPlay while Stopping → updates queue only ──────────
    void requestPlay_whileStopping_updatesQueue()
    {
        MusicKitStateMachine sm;
        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);

        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.requestPlay("song-2");  // → Stopping
        QCOMPARE(sm.amState(), AMState::Stopping);
        int stopCount = stopSpy.count();

        sm.requestPlay("song-3");  // update queue, stay Stopping
        QCOMPARE(sm.amState(), AMState::Stopping);
        QCOMPARE(stopSpy.count(), stopCount);  // no extra stop
    }

    // ── Pending play dequeue after stop completes ────────────────
    void pendingPlayDequeue_afterStopComplete()
    {
        MusicKitStateMachine sm;
        QSignalSpy execSpy(&sm, &MusicKitStateMachine::executePlayRequested);

        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.requestPlay("song-2");  // queued, → Stopping
        QCOMPARE(sm.amState(), AMState::Stopping);
        QCOMPARE(execSpy.count(), 1);  // only song-1

        // Stop confirmed
        sm.onMusicKitStateChanged(MK_Stopped);

        // Should have dequeued and started song-2
        QCOMPARE(sm.amState(), AMState::Loading);
        QCOMPARE(sm.pendingPlaySongId(), QStringLiteral("song-2"));
        QCOMPARE(execSpy.count(), 2);
        QCOMPARE(execSpy.at(1).at(0).toString(), QStringLiteral("song-2"));
    }

    // ── requestStop from Playing → Stopping ──────────────────────
    void requestStop_fromPlaying()
    {
        MusicKitStateMachine sm;
        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);

        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);

        sm.requestStop();
        QCOMPARE(sm.amState(), AMState::Stopping);
        QCOMPARE(stopSpy.count(), 1);
    }

    // ── requestStop from Idle → emits stop but stays Idle ────────
    void requestStop_fromIdle()
    {
        MusicKitStateMachine sm;
        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);

        sm.requestStop();
        QCOMPARE(sm.amState(), AMState::Idle);
        QCOMPARE(stopSpy.count(), 1);
    }

    // ── cancelPendingPlay → Cancelled + stop ─────────────────────
    void cancelPendingPlay_whilePending()
    {
        MusicKitStateMachine sm;
        QSignalSpy playSpy(&sm, &MusicKitStateMachine::amPlayStateChanged);
        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);

        sm.requestPlay("song-1");
        playSpy.clear();

        sm.cancelPendingPlay();
        QCOMPARE(sm.amPlayState(), AMPlayState::Cancelled);
        QVERIFY(sm.pendingPlaySongId().isEmpty());

        // Should have emitted Cancelled
        bool foundCancelled = false;
        for (const auto& args : playSpy) {
            if (args.at(0).value<AMPlayState>() == AMPlayState::Cancelled)
                foundCancelled = true;
        }
        QVERIFY(foundCancelled);

        // Should have requested stop (via requestStop)
        QVERIFY(stopSpy.count() >= 1);
    }

    // ── cancelPendingPlay when Idle → no-op ──────────────────────
    void cancelPendingPlay_whenIdle_noop()
    {
        MusicKitStateMachine sm;
        QSignalSpy playSpy(&sm, &MusicKitStateMachine::amPlayStateChanged);

        sm.cancelPendingPlay();
        QCOMPARE(sm.amPlayState(), AMPlayState::Idle);
        QCOMPARE(playSpy.count(), 0);
    }

    // ── Cancelled guard: mkState 2 while Cancelled → stop ────────
    void cancelledGuard_playArrivesAfterCancel()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.cancelPendingPlay();
        QCOMPARE(sm.amPlayState(), AMPlayState::Cancelled);

        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);
        QSignalSpy playSpy(&sm, &MusicKitStateMachine::amPlayStateChanged);

        sm.onMusicKitStateChanged(MK_Playing);

        // Should have emitted stop and reset to Idle
        QVERIFY(stopSpy.count() >= 1);
        QCOMPARE(sm.amPlayState(), AMPlayState::Idle);
    }

    // ── onPlayError while Pending → Error ────────────────────────
    void onPlayError_whilePending()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        // AMPlayState is Buffering (Pending→Buffering on Loading transition)
        QCOMPARE(sm.amPlayState(), AMPlayState::Buffering);

        QSignalSpy playSpy(&sm, &MusicKitStateMachine::amPlayStateChanged);
        sm.onPlayError();
        QCOMPARE(sm.amPlayState(), AMPlayState::Error);
        QCOMPARE(playSpy.count(), 1);
        QCOMPARE(playSpy.at(0).at(0).value<AMPlayState>(), AMPlayState::Error);
    }

    // ── onPlayError while Playing → no change (already past pending) ─
    void onPlayError_whilePlaying_noChange()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amPlayState(), AMPlayState::Playing);

        QSignalSpy playSpy(&sm, &MusicKitStateMachine::amPlayStateChanged);
        sm.onPlayError();
        QCOMPARE(sm.amPlayState(), AMPlayState::Playing);  // unchanged
        QCOMPARE(playSpy.count(), 0);
    }

    // ── reset() ──────────────────────────────────────────────────
    void reset_clearsEverything()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.requestPlay("song-2");  // queued, Stopping
        QCOMPARE(sm.amState(), AMState::Stopping);

        sm.reset();
        QCOMPARE(sm.amState(), AMState::Idle);
        QCOMPARE(sm.amPlayState(), AMPlayState::Idle);
        QVERIFY(sm.pendingPlaySongId().isEmpty());
    }

    // ── Loading: stalled/loading stays in Loading ────────────────
    void loading_stalled_staysLoading()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        QCOMPARE(sm.amState(), AMState::Loading);

        sm.onMusicKitStateChanged(MK_Stalled);
        QCOMPARE(sm.amState(), AMState::Loading);  // stays

        sm.onMusicKitStateChanged(MK_Loading);
        QCOMPARE(sm.amState(), AMState::Loading);  // stays
    }

    // ── Loading: stopped unexpectedly → Idle ─────────────────────
    void loading_stoppedUnexpectedly_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");

        sm.onMusicKitStateChanged(MK_Stopped);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── Loading: paused unexpectedly → Idle ──────────────────────
    void loading_pausedUnexpectedly_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");

        sm.onMusicKitStateChanged(MK_Paused);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── Idle: mkState 2 → Playing (resume bypass) ────────────────
    void idle_playingResume()
    {
        MusicKitStateMachine sm;
        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amState(), AMState::Playing);
    }

    // ── Stopping: stop confirmed → Idle ──────────────────────────
    void stopping_confirmed_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.requestStop();
        QCOMPARE(sm.amState(), AMState::Stopping);

        sm.onMusicKitStateChanged(MK_Stopped);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── Stopping: paused → Idle ──────────────────────────────────
    void stopping_paused_goesIdle()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.requestStop();

        sm.onMusicKitStateChanged(MK_Paused);
        QCOMPARE(sm.amState(), AMState::Idle);
    }

    // ── Stopping: other states → stays Stopping ──────────────────
    void stopping_otherStates_staysStopping()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.requestStop();
        QCOMPARE(sm.amState(), AMState::Stopping);

        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amState(), AMState::Stopping);

        sm.onMusicKitStateChanged(MK_Stalled);
        QCOMPARE(sm.amState(), AMState::Stopping);
    }

    // ── requestPlay replaces queue during Stopping ───────────────
    void requestPlay_replacesQueueDuringStopping()
    {
        MusicKitStateMachine sm;
        QSignalSpy execSpy(&sm, &MusicKitStateMachine::executePlayRequested);

        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.requestPlay("song-2");  // queued
        sm.requestPlay("song-3");  // replaces queue

        // Stop confirmed → should dequeue song-3 (not song-2)
        sm.onMusicKitStateChanged(MK_Stopped);
        QCOMPARE(sm.pendingPlaySongId(), QStringLiteral("song-3"));
        QCOMPARE(execSpy.last().at(0).toString(), QStringLiteral("song-3"));
    }

    // ── Playing: mkState 2 while already Playing → no change ─────
    void playing_playAgain_ignored()
    {
        MusicKitStateMachine sm;
        QSignalSpy stateSpy(&sm, &MusicKitStateMachine::amStateChanged);

        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        stateSpy.clear();

        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amState(), AMState::Playing);
        QCOMPARE(stateSpy.count(), 0);  // no duplicate signal
    }

    // ── Stalled: re-stalled stays Stalled ────────────────────────
    void stalled_restalled_staysStalled()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.onMusicKitStateChanged(MK_Stalled);

        QSignalSpy stateSpy(&sm, &MusicKitStateMachine::amStateChanged);
        sm.onMusicKitStateChanged(MK_Stalled);
        QCOMPARE(sm.amState(), AMState::Stalled);
        QCOMPARE(stateSpy.count(), 0);  // no re-entry
    }

    // ── AMPlayState syncs on Playing→Idle transition ─────────────
    void amPlayState_syncsOnIdleTransition()
    {
        MusicKitStateMachine sm;
        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        QCOMPARE(sm.amPlayState(), AMPlayState::Playing);

        sm.onMusicKitStateChanged(MK_Paused);
        QCOMPARE(sm.amPlayState(), AMPlayState::Idle);
    }

    // ── requestPlay while Stalled → queue + Stopping ─────────────
    void requestPlay_whileStalled_queuesAndStops()
    {
        MusicKitStateMachine sm;
        QSignalSpy stopSpy(&sm, &MusicKitStateMachine::stopPlaybackRequested);

        sm.requestPlay("song-1");
        sm.onMusicKitStateChanged(MK_Playing);
        sm.onMusicKitStateChanged(MK_Stalled);
        QCOMPARE(sm.amState(), AMState::Stalled);

        sm.requestPlay("song-2");
        QCOMPARE(sm.amState(), AMState::Stopping);
        QVERIFY(stopSpy.count() >= 1);
    }
};

QTEST_MAIN(tst_MusicKitStateMachine)
#include "tst_MusicKitStateMachine.moc"
