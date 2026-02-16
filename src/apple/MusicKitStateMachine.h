#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <optional>

class MusicKitStateMachine : public QObject {
    Q_OBJECT
public:
    enum class AMState { Idle, Loading, Playing, Stalled, Stopping };
    Q_ENUM(AMState)

    enum class AMPlayState {
        Idle,       // No play in progress
        Pending,    // playSong called, waiting for MusicKit
        Buffering,  // MusicKit started loading
        Playing,    // Actually playing audio
        Error,      // Play failed
        Cancelled   // User switched source before play completed
    };
    Q_ENUM(AMPlayState)

    explicit MusicKitStateMachine(QObject* parent = nullptr);

    AMState amState() const { return m_amState; }
    AMPlayState amPlayState() const { return m_amPlayState; }
    QString pendingPlaySongId() const { return m_pendingPlaySongId; }

    // ── Input methods (called by MusicKitPlayer) ─────────────────────
    void requestPlay(const QString& songId);
    void requestStop();
    void cancelPendingPlay();
    void onMusicKitStateChanged(int mkState);
    void onPlayError();
    void reset();

signals:
    // State notifications
    void amStateChanged(int state);
    void amPlayStateChanged(MusicKitStateMachine::AMPlayState state);
    void playbackActiveChanged(bool playing);

    // Action requests — owner connects to JS execution
    void executePlayRequested(const QString& songId);
    void stopPlaybackRequested();

private:
    void setAMState(AMState newState);
    void processStateTransition(int mkState);
    void processPendingPlay();
    void startStateTimeout(int ms);
    void onStateTimeout();

    AMState m_amState = AMState::Idle;
    AMPlayState m_amPlayState = AMPlayState::Idle;

    struct PendingPlay { QString songId; };
    std::optional<PendingPlay> m_pendingPlay;

    QString m_pendingPlaySongId;
    QElapsedTimer m_playRequestTimer;
    QTimer* m_stateTimeoutTimer = nullptr;
};
