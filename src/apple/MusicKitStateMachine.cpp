#include "MusicKitStateMachine.h"
#include <QDebug>

MusicKitStateMachine::MusicKitStateMachine(QObject* parent)
    : QObject(parent)
{
    m_stateTimeoutTimer = new QTimer(this);
    m_stateTimeoutTimer->setSingleShot(true);
    connect(m_stateTimeoutTimer, &QTimer::timeout,
            this, &MusicKitStateMachine::onStateTimeout);
}

// ═════════════════════════════════════════════════════════════════════
//  Public input methods
// ═════════════════════════════════════════════════════════════════════

void MusicKitStateMachine::requestPlay(const QString& songId)
{
    switch (m_amState) {
    case AMState::Idle: {
        // Ready — begin play immediately
        if (m_amPlayState == AMPlayState::Pending ||
            m_amPlayState == AMPlayState::Buffering) {
            qDebug() << "[MusicKitPlayer] Cancelling previous pending play:"
                     << m_pendingPlaySongId;
        }
        m_amPlayState = AMPlayState::Pending;
        m_pendingPlaySongId = songId;
        m_playRequestTimer.start();
        emit amPlayStateChanged(m_amPlayState);
        setAMState(AMState::Loading);
        emit executePlayRequested(songId);
        break;
    }
    case AMState::Loading:
    case AMState::Playing:
    case AMState::Stalled:
        // Busy — queue and stop current
        m_pendingPlay = PendingPlay{songId};
        qDebug() << "[MusicKit] Queued:" << songId << "— stopping current";
        setAMState(AMState::Stopping);
        emit stopPlaybackRequested();
        break;

    case AMState::Stopping:
        // Already stopping — just update queue
        m_pendingPlay = PendingPlay{songId};
        qDebug() << "[MusicKit] Queued:" << songId << "— already stopping";
        break;
    }
}

void MusicKitStateMachine::requestStop()
{
    if (m_amState == AMState::Idle || m_amState == AMState::Stopping) {
        emit stopPlaybackRequested();
        return;
    }
    setAMState(AMState::Stopping);
    emit stopPlaybackRequested();
}

void MusicKitStateMachine::cancelPendingPlay()
{
    if (m_amPlayState == AMPlayState::Idle ||
        m_amPlayState == AMPlayState::Cancelled) {
        return;  // Nothing to cancel
    }

    qDebug() << "[MusicKitPlayer] CANCELLING play:" << m_pendingPlaySongId
             << "after" << m_playRequestTimer.elapsed() << "ms"
             << "(was" << static_cast<int>(m_amPlayState) << ")";

    m_amPlayState = AMPlayState::Cancelled;
    m_pendingPlaySongId.clear();
    emit amPlayStateChanged(m_amPlayState);

    // Tell MusicKit to stop — handles both queued and in-progress plays
    requestStop();
}

void MusicKitStateMachine::onMusicKitStateChanged(int mkState)
{
    // Cross-source cancellation guard
    if (m_amPlayState == AMPlayState::Cancelled && mkState == 2) {
        qDebug() << "[MusicKitPlayer] Play arrived but was CANCELLED — stopping immediately";
        emit stopPlaybackRequested();
        m_amPlayState = AMPlayState::Idle;
        emit amPlayStateChanged(m_amPlayState);
        return;
    }
    processStateTransition(mkState);
}

void MusicKitStateMachine::onPlayError()
{
    if (m_amPlayState == AMPlayState::Pending ||
        m_amPlayState == AMPlayState::Buffering) {
        m_amPlayState = AMPlayState::Error;
        emit amPlayStateChanged(m_amPlayState);
    }
}

void MusicKitStateMachine::reset()
{
    m_amState = AMState::Idle;
    m_amPlayState = AMPlayState::Idle;
    m_pendingPlay.reset();
    m_pendingPlaySongId.clear();
    m_stateTimeoutTimer->stop();
}

// ═════════════════════════════════════════════════════════════════════
//  State machine internals
// ═════════════════════════════════════════════════════════════════════

void MusicKitStateMachine::setAMState(AMState newState)
{
    if (m_amState == newState) return;

    AMState oldState = m_amState;
    m_amState = newState;

    const char* stateNames[] = {"Idle", "Loading", "Playing", "Stalled", "Stopping"};
    qDebug() << "[MusicKit] State:" << stateNames[static_cast<int>(oldState)]
             << "→" << stateNames[static_cast<int>(newState)];

    emit amStateChanged(static_cast<int>(newState));

    switch (newState) {
    case AMState::Playing:
        // Sync AMPlayState
        if (m_amPlayState == AMPlayState::Pending ||
            m_amPlayState == AMPlayState::Buffering) {
            m_amPlayState = AMPlayState::Playing;
            qDebug() << "[MusicKitPlayer] Now playing, took"
                     << m_playRequestTimer.elapsed() << "ms";
            emit amPlayStateChanged(m_amPlayState);
        }
        emit playbackActiveChanged(true);
        m_stateTimeoutTimer->stop();
        break;

    case AMState::Idle:
        // Sync AMPlayState
        if (m_amPlayState == AMPlayState::Playing ||
            m_amPlayState == AMPlayState::Pending ||
            m_amPlayState == AMPlayState::Buffering) {
            m_amPlayState = AMPlayState::Idle;
            emit amPlayStateChanged(m_amPlayState);
        }
        m_stateTimeoutTimer->stop();
        if (m_pendingPlay) {
            // Don't emit false — about to play next song
            processPendingPlay();
        } else {
            emit playbackActiveChanged(false);
        }
        break;

    case AMState::Loading:
        // Sync AMPlayState to Buffering if transitioning from Pending
        if (m_amPlayState == AMPlayState::Pending) {
            m_amPlayState = AMPlayState::Buffering;
            emit amPlayStateChanged(m_amPlayState);
        }
        startStateTimeout(30000);  // MusicKit DRM + CDN can take 15-20s
        break;

    case AMState::Stalled:
        startStateTimeout(30000);  // Buffering can be slow
        break;

    case AMState::Stopping:
        startStateTimeout(5000);
        break;
    }
}

void MusicKitStateMachine::processStateTransition(int mkState)
{
    // MusicKit states:
    // 0=none, 1=loading, 2=playing, 3=paused, 4=stopped,
    // 5=ended, 6=seeking, 7=waiting, 8=stalled, 9=completed

    switch (m_amState) {

    case AMState::Idle:
        if (mkState == 2) {
            // Playing while Idle — e.g. resume() bypassed state machine
            setAMState(AMState::Playing);
        }
        break;

    case AMState::Loading:
        if (mkState == 2) {
            setAMState(AMState::Playing);
        } else if (mkState == 8 || mkState == 1) {
            // stalled or loading — MusicKit is buffering, stay in Loading
            startStateTimeout(30000);  // reset timeout — still actively loading
        } else if (mkState == 4 || mkState == 0) {
            qDebug() << "[MusicKit] Loading: stopped unexpectedly";
            setAMState(AMState::Idle);
        } else if (mkState == 3) {
            qDebug() << "[MusicKit] Loading: paused unexpectedly";
            setAMState(AMState::Idle);
        }
        break;

    case AMState::Playing:
        if (mkState == 8 || mkState == 1) {
            setAMState(AMState::Stalled);
        } else if (mkState == 3) {
            setAMState(AMState::Idle);
        } else if (mkState == 4 || mkState == 0 || mkState == 9 || mkState == 5) {
            // stopped, none, completed, ended
            setAMState(AMState::Idle);
        }
        // mkState == 2 while Playing → ignore (no change)
        break;

    case AMState::Stalled:
        if (mkState == 2) {
            // Recovered!
            setAMState(AMState::Playing);
        } else if (mkState == 4 || mkState == 0 || mkState == 3) {
            setAMState(AMState::Idle);
        } else if (mkState == 1 || mkState == 8) {
            // loading or re-stalled — MusicKit is actively buffering
            startStateTimeout(30000);  // reset timeout
        }
        break;

    case AMState::Stopping:
        if (mkState == 4 || mkState == 0 || mkState == 3) {
            // stopped, none, paused — stop confirmed
            setAMState(AMState::Idle);
        }
        // Any other state while Stopping → keep waiting
        break;
    }
}

void MusicKitStateMachine::processPendingPlay()
{
    if (!m_pendingPlay) return;

    auto next = *m_pendingPlay;
    m_pendingPlay.reset();

    qDebug() << "[MusicKit] Processing queued:" << next.songId;

    // Begin play
    if (m_amPlayState == AMPlayState::Pending ||
        m_amPlayState == AMPlayState::Buffering) {
        qDebug() << "[MusicKitPlayer] Cancelling previous pending play:"
                 << m_pendingPlaySongId;
    }
    m_amPlayState = AMPlayState::Pending;
    m_pendingPlaySongId = next.songId;
    m_playRequestTimer.start();
    emit amPlayStateChanged(m_amPlayState);
    setAMState(AMState::Loading);
    emit executePlayRequested(next.songId);
}

void MusicKitStateMachine::startStateTimeout(int ms)
{
    m_stateTimeoutTimer->start(ms);
}

void MusicKitStateMachine::onStateTimeout()
{
    const char* stateNames[] = {"Idle", "Loading", "Playing", "Stalled", "Stopping"};
    qDebug() << "[MusicKit] State timeout in"
             << stateNames[static_cast<int>(m_amState)] << "— forcing Idle";
    m_amState = AMState::Idle;  // bypass setAMState to avoid re-entrant timeout
    m_stateTimeoutTimer->stop();

    // Reset AMPlayState on timeout
    if (m_amPlayState != AMPlayState::Idle &&
        m_amPlayState != AMPlayState::Cancelled) {
        m_amPlayState = AMPlayState::Error;
        emit amPlayStateChanged(m_amPlayState);
        m_amPlayState = AMPlayState::Idle;
        emit amPlayStateChanged(m_amPlayState);
    }

    if (m_pendingPlay) {
        processPendingPlay();
    } else {
        emit playbackActiveChanged(false);
    }
}
