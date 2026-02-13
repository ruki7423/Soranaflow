#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
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

    // State machine states
    enum class AMState { Idle, Loading, Playing, Stalled, Stopping };
    AMState amState() const { return m_amState; }

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
    void onNowPlayingChanged(const QString& title, const QString& artist,
                              const QString& album, double duration);
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
};
