#pragma once

#include <QObject>
#include <QString>

class QWebEngineView;
class QWebChannel;

class MusicKitPlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ isReady NOTIFY ready)

public:
    static MusicKitPlayer* instance();
    ~MusicKitPlayer() override;

    bool isReady() const { return m_ready; }

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

    // Clean shutdown — call before application exit to avoid WebEngine warnings
    void cleanup();

    // Music User Token injection
    void injectMusicUserToken(const QString& token);
    void clearMusicUserToken();

    // Called from JS via QWebChannel
    Q_INVOKABLE void onMusicKitReady();
    Q_INVOKABLE void onPlaybackStateChanged(bool playing);
    Q_INVOKABLE void onNowPlayingChanged(const QString& title, const QString& artist,
                                          const QString& album, double duration);
    Q_INVOKABLE void onPlaybackTimeChanged(double currentTime, double totalTime);
    Q_INVOKABLE void onError(const QString& error);
    Q_INVOKABLE void onAuthStatusChanged(const QString& statusJson);
    Q_INVOKABLE void onPlaybackStarted(const QString& infoJson);
    Q_INVOKABLE void onTokenExpired();
    Q_INVOKABLE void onPlaybackEnded();

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

private:
    explicit MusicKitPlayer(QObject* parent = nullptr);
    void ensureWebView();
    QString generateHTML();
    void runJS(const QString& js);

    QWebEngineView* m_webView = nullptr;
    QWebChannel* m_channel = nullptr;
    bool m_ready = false;
    bool m_initialized = false;
    bool m_webViewReady = false;
    QString m_pendingSongId;
    QString m_pendingUserToken;
};
