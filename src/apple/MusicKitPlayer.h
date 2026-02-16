#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <optional>

// Opaque pointer for Objective-C++ WKWebView implementation
struct MusicKitWebViewPrivate;

class MusicKitPlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ isReady NOTIFY ready)

public:
    static MusicKitPlayer* instance();
    ~MusicKitPlayer() override;

    bool isReady() const { return m_ready; }

    // Pre-warm WebView at startup (call from main.cpp after window shown)
    void preInitialize();

    // State machine states
    enum class AMState { Idle, Loading, Playing, Stalled, Stopping };
    AMState amState() const { return m_amState; }

    // Async play state tracking (for cross-source cancellation)
    enum class AMPlayState {
        Idle,       // No play in progress
        Pending,    // playSong called, waiting for MusicKit
        Buffering,  // MusicKit started loading
        Playing,    // Actually playing audio
        Error,      // Play failed
        Cancelled   // User switched source before play completed
    };
    AMPlayState amPlayState() const { return m_amPlayState; }
    void cancelPendingPlay();

    // Playback controls
    Q_INVOKABLE void play(const QString& songId);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(double position);
    Q_INVOKABLE void setVolume(double volume);
    void setPlaybackQuality(const QString& quality);

    // Route WebView audio to the app's selected output device
    void updateOutputDevice();

    // Clean shutdown — call before application exit
    void cleanup();

    // Music User Token injection
    void injectMusicUserToken(const QString& token);
    void clearMusicUserToken();

    // Called from JS via WKScriptMessageHandler
    void onMusicKitReady();
    void onMusicKitStateChanged(int state);
    void onNowPlayingChanged(const QString& songId, const QString& title,
                              const QString& artist, const QString& album,
                              double duration);
    void onPlaybackTimeChanged(double currentTime, double totalTime);
    void onError(const QString& error);
    void onAuthStatusChanged(const QString& statusJson);
    void onPlaybackStarted(const QString& infoJson);
    void onTokenExpired();
    void onPlaybackEnded();

signals:
    void ready();
    void musicKitReady();
    void playbackStateChanged(bool playing);
    void nowPlayingChanged(const QString& title, const QString& artist,
                           const QString& album, double duration);
    void playbackTimeChanged(double currentTime, double totalTime);
    void errorOccurred(const QString& error);
    void fullPlaybackAvailable();
    void previewOnlyMode();
    void tokenExpired();
    void playbackEnded();
    void authorizationPending();   // system "Allow Access" dialog is about to appear
    void amStateChanged(int state);  // 0=Idle,1=Loading,2=Playing,3=Stalled,4=Stopping
    void amPlayStateChanged(AMPlayState state);

private:
    explicit MusicKitPlayer(QObject* parent = nullptr);
    void ensureWebView();
    QString generateHTML();
    void runJS(const QString& js);
    Q_INVOKABLE void webViewDidFinishLoad();

    // State machine
    AMState m_amState = AMState::Idle;
    void setAMState(AMState newState);
    void processStateTransition(int musicKitState);
    void processPendingPlay();

    // Command queue (last-wins, max 1)
    struct PendingPlay { QString songId; };
    std::optional<PendingPlay> m_pendingPlay;

    // Internal play execution (separated from public play)
    void executePlay(const QString& songId);

    // Timeout timer
    QTimer* m_stateTimeoutTimer = nullptr;
    void startStateTimeout(int ms);
    void onStateTimeout();

    MusicKitWebViewPrivate* m_wk = nullptr;
    bool m_ready = false;
    bool m_initialized = false;
    bool m_webViewReady = false;
    bool m_cleanedUp = false;
    QString m_pendingSongId;
    QString m_pendingUserToken;
    QElapsedTimer m_loadTimer;

    // Async play state
    AMPlayState m_amPlayState = AMPlayState::Idle;
    QString m_pendingPlaySongId;
    QElapsedTimer m_playRequestTimer;
};
