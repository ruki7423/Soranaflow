#include "MusicKitPlayer.h"
#include "AppleMusicManager.h"
#include "../core/audio/AudioDeviceManager.h"
#include "../core/Settings.h"
#ifdef Q_OS_MACOS
#include "../platform/macos/AudioProcessTap.h"
#endif

#include <QCoreApplication>
#include <QWebEngineView>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QDebug>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>

// ── Custom page to capture JS console output ────────────────────────
class MusicKitPage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString& message, int lineNumber,
                                  const QString&) override
    {
        const char* prefix = "INFO";
        if (level == WarningMessageLevel) prefix = "WARN";
        else if (level == ErrorMessageLevel) prefix = "ERROR";
        qDebug() << "[MusicKit JS]" << prefix << "line" << lineNumber << ":" << message;
    }
};

// ── Singleton ───────────────────────────────────────────────────────
MusicKitPlayer* MusicKitPlayer::instance()
{
    static MusicKitPlayer s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
MusicKitPlayer::MusicKitPlayer(QObject* parent)
    : QObject(parent)
{
}

// ── cleanup — explicit shutdown before app exit ─────────────────────
void MusicKitPlayer::cleanup()
{
    if (m_webView) {
        // Stop any active playback
        if (m_ready)
            runJS(QStringLiteral("if(music) music.stop()"));

        // Delete the page first (while profile is still alive)
        QWebEnginePage* page = m_webView->page();
        m_webView->setPage(nullptr);
        delete page;

        m_webView->hide();
        delete m_webView;
        m_webView = nullptr;
    }
    delete m_channel;
    m_channel = nullptr;
    m_ready = false;
    m_initialized = false;
    m_webViewReady = false;
}

// ── Destructor — clean up WebEngineView before profile teardown ─────
MusicKitPlayer::~MusicKitPlayer()
{
    cleanup();
}

// ── ensureWebView — lazy initialization ─────────────────────────────
void MusicKitPlayer::ensureWebView()
{
    if (m_initialized) return;
    m_initialized = true;

    qDebug() << "[MusicKitPlayer] Initializing WebView...";

    // Point QtWebEngine to the bundled helper process
    QString helperPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/../Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/"
                         "QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess");
    qputenv("QTWEBENGINEPROCESS_PATH", helperPath.toUtf8());
    qDebug() << "[MusicKitPlayer] Helper path:" << helperPath;

    // Use custom page subclass to capture JS console output
    auto* profile = QWebEngineProfile::defaultProfile();
    auto* page = new MusicKitPage(profile, this);
    m_webView = new QWebEngineView();
    m_webView->setPage(page);
    m_webView->setMinimumSize(1, 1);
    m_webView->setMaximumSize(1, 1);
    // WebEngine needs the view to be "shown" for JS to execute properly
    m_webView->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_webView->show();

    // Enable required settings
    auto* settings = page->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);

    // Also set on the profile level
    profile->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);

    // Set up QWebChannel for JS ↔ C++ communication
    m_channel = new QWebChannel(this);
    m_channel->registerObject(QStringLiteral("musicKitBridge"), this);
    page->setWebChannel(m_channel);

    // Monitor load progress
    connect(m_webView, &QWebEngineView::loadProgress,
            this, [](int progress) {
        qDebug() << "[MusicKitPlayer] Load progress:" << progress;
    });

    // Auto-grant audio permissions so enumerateDevices() returns labeled devices
    // and setSinkId() can route audio to the selected output device.
    connect(page, &QWebEnginePage::featurePermissionRequested,
            this, [page](const QUrl& securityOrigin, QWebEnginePage::Feature feature) {
        qDebug() << "[MusicKitPlayer] Permission requested:" << feature << "from" << securityOrigin;
        if (feature == QWebEnginePage::MediaAudioCapture ||
            feature == QWebEnginePage::MediaAudioVideoCapture) {
            page->setFeaturePermission(securityOrigin, feature,
                                        QWebEnginePage::PermissionGrantedByUser);
            qDebug() << "[MusicKitPlayer] Auto-granted audio permission";
        }
    });

    // Monitor render process crashes — auto-recover
    connect(page, &QWebEnginePage::renderProcessTerminated,
            this, [this](QWebEnginePage::RenderProcessTerminationStatus status, int code) {
        qDebug() << "[MusicKitPlayer] Render process terminated! Status:" << status << "Code:" << code;
        QTimer::singleShot(1000, this, [this]() {
            qDebug() << "[MusicKitPlayer] Auto-recovering WebEngine...";
            if (m_webView) {
                m_webView->deleteLater();
                m_webView = nullptr;
            }
            m_ready = false;
            m_initialized = false;
            m_webViewReady = false;
            m_pendingUserToken.clear();
            m_pendingSongId.clear();
            emit errorOccurred(QStringLiteral("WebEngine crashed — will reinitialize on next play"));
            qDebug() << "[MusicKitPlayer] State reset, ready for re-initialization";
        });
    });

    // Monitor load completion
    connect(m_webView, &QWebEngineView::loadFinished,
            this, [this](bool ok) {
        qDebug() << "[MusicKitPlayer] WebView loadFinished:" << ok;
        if (ok) {
            m_webViewReady = true;

            // If a token was received before the WebView was ready, inject it now
            if (!m_pendingUserToken.isEmpty()) {
                qDebug() << "[MusicKitPlayer] WebView ready, injecting pending token";
                QTimer::singleShot(500, this, [this]() {
                    injectMusicUserToken(m_pendingUserToken);
                    m_pendingUserToken.clear();
                });
            }
        } else {
            qDebug() << "[MusicKitPlayer] WebView URL:" << m_webView->url();
            qDebug() << "[MusicKitPlayer] WebView title:" << m_webView->title();
            m_webView->page()->toHtml([](const QString& html) {
                qDebug() << "[MusicKitPlayer] Page HTML length:" << html.length();
                if (html.length() < 500) {
                    qDebug() << "[MusicKitPlayer] Page HTML:" << html;
                }
            });
            emit errorOccurred(QStringLiteral("Failed to load MusicKit player page"));
        }
    });

    // Get developer token
    auto* am = AppleMusicManager::instance();
    QString token = am->developerToken();
    qDebug() << "[MusicKitPlayer] Developer token length:" << token.length();

    if (token.isEmpty()) {
        qDebug() << "[MusicKitPlayer] WARNING: No developer token available!";
    }

    // Defer HTML loading to let the render process fully initialize.
    // Loading immediately after QWebEngineView creation can cause a SIGSEGV
    // when the render process hasn't finished spawning.
    QTimer::singleShot(100, this, [this]() {
        if (!m_webView) return;  // Destroyed before timer fired
        QString html = generateHTML();
        m_webView->setHtml(html, QUrl(QStringLiteral("https://sorana.local")));
        qDebug() << "[MusicKitPlayer] HTML loading started (deferred)";
    });
}

// ── play ────────────────────────────────────────────────────────────
void MusicKitPlayer::play(const QString& songId)
{
    qDebug() << "[MusicKitPlayer] play() called with songId:" << songId;

    if (!m_initialized) {
        m_pendingSongId = songId;
        ensureWebView();
        return;
    }

    if (!m_ready) {
        qDebug() << "[MusicKitPlayer] MusicKit not ready yet, queuing songId:" << songId;
        m_pendingSongId = songId;
        return;
    }

    qDebug() << "[MusicKitPlayer] Calling JS playSong()...";
    runJS(QStringLiteral("playSong('%1')").arg(songId));
}

// ── pause ───────────────────────────────────────────────────────────
void MusicKitPlayer::pause()
{
    if (m_ready)
        runJS(QStringLiteral("pausePlayback()"));
}

// ── resume ──────────────────────────────────────────────────────────
void MusicKitPlayer::resume()
{
    if (m_ready)
        runJS(QStringLiteral("resumePlayback()"));
}

// ── togglePlayPause ─────────────────────────────────────────────────
void MusicKitPlayer::togglePlayPause()
{
    if (m_ready)
        runJS(QStringLiteral("togglePlayback()"));
}

// ── stop ────────────────────────────────────────────────────────────
void MusicKitPlayer::stop()
{
    if (m_ready)
        runJS(QStringLiteral("stopPlayback()"));
}

// ── seek ────────────────────────────────────────────────────────────
void MusicKitPlayer::seek(double position)
{
    if (m_ready)
        runJS(QStringLiteral("seekTo(%1)").arg(position));
}

// ── setVolume ───────────────────────────────────────────────────────
void MusicKitPlayer::setVolume(double volume)
{
    if (m_ready)
        runJS(QStringLiteral("setVolume(%1)").arg(volume));
}

// ── setPlaybackQuality ──────────────────────────────────────────────
void MusicKitPlayer::setPlaybackQuality(const QString& quality)
{
    if (m_ready)
        runJS(QStringLiteral("setPlaybackBitrate('%1')").arg(quality));
}

// ═════════════════════════════════════════════════════════════════════
//  JS → C++ callbacks (called via QWebChannel)
// ═════════════════════════════════════════════════════════════════════

void MusicKitPlayer::onMusicKitReady()
{
    m_ready = true;
    qDebug() << "[MusicKitPlayer] MusicKit JS ready!";
    emit ready();
    emit musicKitReady();

    // Route audio to the app's selected output device
    updateOutputDevice();

    // Inject pending user token if available
    bool tokenInjectionPending = false;
    if (!m_pendingUserToken.isEmpty()) {
        qDebug() << "[MusicKitPlayer] MusicKit ready, injecting pending user token";
        tokenInjectionPending = true;
        injectMusicUserToken(m_pendingUserToken);
        m_pendingUserToken.clear();
        // NOTE: If m_pendingSongId is set, it will be played in the token
        // injection callback AFTER the token is actually injected into JS.
    }

    // Play pending song ONLY if no token injection is happening
    // (otherwise the callback in injectMusicUserToken handles it)
    if (!tokenInjectionPending && !m_pendingSongId.isEmpty()) {
        QString id = m_pendingSongId;
        m_pendingSongId.clear();
        qDebug() << "[MusicKitPlayer] Playing pending song (no token pending):" << id;
        play(id);
    }
}

void MusicKitPlayer::onPlaybackStateChanged(bool playing)
{
    qDebug() << "[MusicKitPlayer] Playback state changed:" << playing;
    emit playbackStateChanged(playing);
}

void MusicKitPlayer::onPlaybackEnded()
{
    qDebug() << "[MusicKitPlayer] Track ended";
    emit playbackEnded();
}

void MusicKitPlayer::onNowPlayingChanged(const QString& title, const QString& artist,
                                           const QString& album, double duration)
{
    qDebug() << "[MusicKitPlayer] Now playing:" << title << "-" << artist;
    emit nowPlayingChanged(title, artist, album, duration);
}

void MusicKitPlayer::onPlaybackTimeChanged(double currentTime, double totalTime)
{
    emit playbackTimeChanged(currentTime, totalTime);
}

void MusicKitPlayer::onError(const QString& error)
{
    qDebug() << "[MusicKitPlayer] Error:" << error;
    emit errorOccurred(error);
}

// ── injectMusicUserToken ────────────────────────────────────────────
void MusicKitPlayer::injectMusicUserToken(const QString& token)
{
    qDebug() << "[MusicKitPlayer] injectMusicUserToken called, length:" << token.length();

    // Guard: never inject an empty token — it resets MusicKit authorization
    if (token.isEmpty()) {
        qDebug() << "[MusicKitPlayer] IGNORED: empty token injection (would reset auth)";
        return;
    }

    if (!m_webView) {
        qDebug() << "[MusicKitPlayer] WebView is null, storing as pending";
        m_pendingUserToken = token;
        return;
    }

    if (!m_ready) {
        qDebug() << "[MusicKitPlayer] MusicKit not ready yet, storing as pending";
        m_pendingUserToken = token;
        return;
    }

    // Escape for safe JS string embedding
    QString escapedToken = token;
    escapedToken.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    escapedToken.replace(QLatin1Char('\''), QLatin1String("\\'"));
    escapedToken.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    escapedToken.replace(QLatin1Char('\r'), QLatin1String("\\r"));

    QString js = QStringLiteral("injectMusicUserToken('%1')").arg(escapedToken);
    qDebug() << "[MusicKitPlayer] Running JS injection (token length:" << token.length() << ")";

    m_webView->page()->runJavaScript(js, [this](const QVariant& result) {
        qDebug() << "[MusicKitPlayer] Token injection JS returned:" << result;
        if (result.toBool()) {
            qDebug() << "[MusicKitPlayer] Token injection SUCCEEDED — full playback available";
            // Play pending song NOW that token is injected
            if (!m_pendingSongId.isEmpty()) {
                QString id = m_pendingSongId;
                m_pendingSongId.clear();
                qDebug() << "[MusicKitPlayer] Token ready, playing pending song:" << id;
                play(id);
            }
        } else {
            qDebug() << "[MusicKitPlayer] Token injection returned false (check JS console)";
        }
    });
}

void MusicKitPlayer::clearMusicUserToken()
{
    qDebug() << "[MusicKitPlayer] Clearing Music User Token";
    m_pendingUserToken.clear();
    if (m_webView && m_webView->page()) {
        m_webView->page()->runJavaScript(
            QStringLiteral("if(window.music){music.unauthorize();}"));
    }
}

void MusicKitPlayer::onAuthStatusChanged(const QString& statusJson)
{
    qDebug() << "[MusicKitPlayer] Auth status changed:" << statusJson;

    QJsonDocument doc = QJsonDocument::fromJson(statusJson.toUtf8());
    if (doc.isNull()) {
        qDebug() << "[MusicKitPlayer] ERROR: Invalid JSON in auth status";
        return;
    }

    QJsonObject obj = doc.object();
    bool isAuthorized = obj[QStringLiteral("isAuthorized")].toBool();
    bool previewOnly = obj[QStringLiteral("previewOnly")].toBool();
    bool hasToken = obj[QStringLiteral("hasToken")].toBool();

    qDebug() << "[MusicKitPlayer] isAuthorized:" << isAuthorized
             << "previewOnly:" << previewOnly
             << "hasToken:" << hasToken;

    if (isAuthorized && !previewOnly) {
        qDebug() << "[MusicKitPlayer] === FULL PLAYBACK AVAILABLE ===";
        emit fullPlaybackAvailable();
    } else if (isAuthorized && previewOnly) {
        qDebug() << "[MusicKitPlayer] Preview only mode (no active subscription?)";
        emit previewOnlyMode();
    }
}

void MusicKitPlayer::onPlaybackStarted(const QString& infoJson)
{
    qDebug() << "[MusicKitPlayer] Playback started:" << infoJson;

    QJsonDocument doc = QJsonDocument::fromJson(infoJson.toUtf8());
    if (!doc.isNull()) {
        QJsonObject obj = doc.object();
        bool isFullPlayback = obj[QStringLiteral("isFullPlayback")].toBool();
        double duration = obj[QStringLiteral("duration")].toDouble();
        qDebug() << "[MusicKitPlayer] Duration:" << duration
                 << "Full playback:" << isFullPlayback;
    }
}

void MusicKitPlayer::onTokenExpired()
{
    qDebug() << "[MusicKitPlayer] Music User Token has expired";
    emit tokenExpired();
}

// ── updateOutputDevice ──────────────────────────────────────────────
void MusicKitPlayer::updateOutputDevice()
{
    if (!m_ready || !m_webView) return;

#ifdef Q_OS_MACOS
    // Skip setSinkId when ProcessTap is active — tap handles audio routing
    // Calling setSinkId while tap is capturing causes "operation aborted" errors
    if (AudioProcessTap::instance()->isActive()) {
        qDebug() << "[MusicKitPlayer] ProcessTap active — skipping setSinkId"
                 << "(tap handles audio routing)";
        return;
    }
#endif

    // Look up the currently selected device name
    uint32_t deviceId = Settings::instance()->outputDeviceId();
    QString deviceName;

    if (deviceId != 0) {
        auto info = AudioDeviceManager::instance()->deviceById(deviceId);
        deviceName = info.name;
    }

    if (deviceName.isEmpty()) {
        auto info = AudioDeviceManager::instance()->defaultOutputDevice();
        deviceName = info.name;
    }

    if (deviceName.isEmpty()) {
        qDebug() << "[MusicKitPlayer] No device name found, skipping setSinkId";
        return;
    }

    // Escape single quotes for JS string
    QString escaped = deviceName;
    escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));

    qDebug() << "[MusicKitPlayer] Routing WebView audio to:" << deviceName;
    runJS(QStringLiteral("setOutputDevice('%1')").arg(escaped));
}

// ── runJS ───────────────────────────────────────────────────────────
void MusicKitPlayer::runJS(const QString& js)
{
    if (m_webView && m_webView->page()) {
        m_webView->page()->runJavaScript(js, [js](const QVariant& result) {
            if (result.isValid())
                qDebug() << "[MusicKitPlayer] JS result for" << js.left(40) << ":" << result;
        });
    }
}

// ═════════════════════════════════════════════════════════════════════
//  generateHTML — embedded MusicKit JS page
// ═════════════════════════════════════════════════════════════════════

QString MusicKitPlayer::generateHTML()
{
    QString devToken = AppleMusicManager::instance()->developerToken();

    // Escape any single quotes in the token (JWT tokens shouldn't have them, but safety)
    devToken.replace(QLatin1Char('\''), QStringLiteral("\\'"));

    return QStringLiteral(R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>MusicKit Player</title>
<script src="qrc:///qtwebchannel/qwebchannel.js"></script>
</head>
<body>
<script>
var bridge = null;
var music = null;
var playbackStartedEmitted = false;

console.log('[MusicKit] Initializing QWebChannel...');

// Initialize QWebChannel
new QWebChannel(qt.webChannelTransport, function(channel) {
    bridge = channel.objects.musicKitBridge;
    console.log('[MusicKit] QWebChannel connected, bridge:', bridge ? 'OK' : 'MISSING');
    loadMusicKitScript();
});

function loadMusicKitScript() {
    console.log('[MusicKit] Loading MusicKit JS from CDN...');
    var script = document.createElement('script');
    script.src = 'https://js-cdn.music.apple.com/musickit/v3/musickit.js';
    script.setAttribute('data-web-components', '');
    script.onload = function() {
        console.log('[MusicKit] MusicKit JS loaded from CDN');
        configureMusicKit();
    };
    script.onerror = function(e) {
        console.error('[MusicKit] Failed to load MusicKit JS from CDN:', e);
        if (bridge) bridge.onError('Failed to load MusicKit JS from CDN');
    };
    document.head.appendChild(script);
}

async function configureMusicKit() {
    try {
        var token = '%1';
        console.log('[MusicKit] Configuring with token length:', token.length);

        if (!token || token === '' || token === 'DEVELOPER_TOKEN') {
            console.error('[MusicKit] No valid developer token!');
            if (bridge) bridge.onError('No valid developer token');
            return;
        }

        await MusicKit.configure({
            developerToken: token,
            app: {
                name: 'Sorana Flow',
                build: '1.2.2'
            }
        });
        console.log('[MusicKit] MusicKit configured');

        music = MusicKit.getInstance();
        console.log('[MusicKit] Got MusicKit instance');

        // Set HIGH quality (256kbps AAC) as default
        music.bitrate = MusicKit.PlaybackBitrate.HIGH;
        console.log('[MusicKit] Bitrate set to HIGH (256kbps)');

        // Skip music.authorize() — it tries to open a login popup which
        // cannot work in a hidden WebView. The user is already authorized
        // at the OS level via native MusicKit (AppleMusicManager).
        console.log('[MusicKit] Skipping authorize (using system credentials)');

        // Event listeners
        music.addEventListener('playbackStateDidChange', function(event) {
            var state = event.state;
            var names = ['none','loading','playing','paused','stopped',
                         'ended','seeking','waiting','stalled','completed'];
            var name = names[state] || 'unknown(' + state + ')';
            var isPlaying = (state === MusicKit.PlaybackStates.playing);
            console.log('[MusicKit] playbackStateDidChange: ' + state + ' (' + name + ') playing: ' + isPlaying);
            if (bridge) bridge.onPlaybackStateChanged(isPlaying);

            // Detect track ended → trigger next track
            if (state === MusicKit.PlaybackStates.ended ||
                state === MusicKit.PlaybackStates.completed) {
                console.log('[MusicKit] Track ended — notifying bridge');
                if (bridge) bridge.onPlaybackEnded();
            }

            // Emit playbackStarted once when playing starts and nowPlayingItem is available
            if (isPlaying && music.nowPlayingItem && !playbackStartedEmitted) {
                playbackStartedEmitted = true;
                var item = music.nowPlayingItem;
                var dur = (item.playbackDuration || music.currentPlaybackDuration || 0) / 1000;
                console.log('[MusicKit] Emitting playbackStarted: duration=' + dur);
                if (bridge) bridge.onPlaybackStarted(JSON.stringify({
                    songId: item.id || '',
                    title: item.title || '',
                    artist: item.artistName || '',
                    album: item.albumName || '',
                    duration: dur,
                    isFullPlayback: dur > 35,
                    artworkUrl: item.artwork ? item.artwork.url : ''
                }));
            }
        });

        music.addEventListener('nowPlayingItemDidChange', function(event) {
            var item = music.nowPlayingItem;
            if (item && bridge) {
                console.log('[MusicKit] nowPlayingItemDidChange:', item.title);
                bridge.onNowPlayingChanged(
                    item.title || '',
                    item.artistName || '',
                    item.albumName || '',
                    (item.playbackDuration || 0) / 1000
                );
            }
        });

        music.addEventListener('playbackTimeDidChange', function(event) {
            if (bridge) {
                bridge.onPlaybackTimeChanged(
                    music.currentPlaybackTime || 0,
                    music.currentPlaybackDuration || 0
                );
            }
        });

        music.addEventListener('mediaPlaybackError', function(event) {
            console.log('[MusicKit] mediaPlaybackError: ' + JSON.stringify(event));
            if (bridge) bridge.onError('Playback error: ' + JSON.stringify(event));
        });

        music.addEventListener('authorizationStatusDidChange', function(event) {
            console.log('[MusicKit] authorizationStatusDidChange: ' + JSON.stringify(event));
        });

        music.addEventListener('playbackDurationDidChange', function(event) {
            console.log('[MusicKit] duration: ' + music.currentPlaybackDuration);
        });

        console.log('[MusicKit] All event listeners registered');
        console.log('[MusicKit] Waiting for Music User Token...');
        if (bridge) bridge.onMusicKitReady();
        console.log('[MusicKit] Ready signal sent to C++');

    } catch (err) {
        console.error('[MusicKit] Configure error:', err);
        if (bridge) bridge.onError('MusicKit configure error: ' + (err.message || String(err)));
    }
}

// ── Music User Token injection from native MusicKit ──────────────
function injectMusicUserToken(token) {
    if (!music) {
        console.log('[MusicKit] ERROR: MusicKit instance not initialized');
        if (bridge) bridge.onError('MusicKit not initialized when injecting token');
        return false;
    }

    console.log('[MusicKit] Injecting Music User Token (length: ' + token.length + ')');

    // Direct property assignment - documented approach
    music.musicUserToken = token;

    // Verify the injection worked
    var isAuth = music.isAuthorized;
    var previewOnly = music.previewOnly || false;
    var hasToken = music.musicUserToken ? true : false;

    console.log('[MusicKit] After injection:');
    console.log('[MusicKit]   isAuthorized: ' + isAuth);
    console.log('[MusicKit]   previewOnly: ' + previewOnly);
    console.log('[MusicKit]   hasToken: ' + hasToken);
    console.log('[MusicKit]   tokenLength: ' + (music.musicUserToken ? music.musicUserToken.length : 0));

    var status = {
        isAuthorized: isAuth,
        previewOnly: previewOnly,
        hasToken: hasToken,
        tokenLength: token.length
    };

    if (bridge) bridge.onAuthStatusChanged(JSON.stringify(status));

    return (isAuth && !previewOnly);
}

function getAuthStatus() {
    if (!music) {
        return JSON.stringify({ error: 'MusicKit not initialized' });
    }
    return JSON.stringify({
        isAuthorized: music.isAuthorized,
        previewOnly: music.previewOnly || false,
        hasToken: music.musicUserToken ? true : false,
        tokenLength: music.musicUserToken ? music.musicUserToken.length : 0,
        playbackState: music.playbackState
    });
}

// ── Enhanced playSong with full diagnostics ──────────────────────
async function playSong(songId) {
    try {
        playbackStartedEmitted = false;

        console.log('[MusicKit] ========================================');
        console.log('[MusicKit] playSong called with: ' + songId);
        console.log('[MusicKit] Pre-play diagnostics:');
        console.log('[MusicKit]   isAuthorized: ' + music.isAuthorized);
        console.log('[MusicKit]   previewOnly: ' + (music.previewOnly || false));
        console.log('[MusicKit]   musicUserToken: ' +
            (music.musicUserToken ? 'present (len=' + music.musicUserToken.length + ')' : 'ABSENT'));

        if (!music) {
            console.error('[MusicKit] music instance is null!');
            if (bridge) bridge.onError('MusicKit not initialized');
            return;
        }

        console.log('[MusicKit] Setting queue...');
        await music.setQueue({ song: songId });
        console.log('[MusicKit] Calling music.play()...');
        await music.play();
        console.log('[MusicKit] music.play() returned, state: ' + music.playbackState);
        // playbackStarted is emitted from playbackStateDidChange listener
        // when nowPlayingItem is populated (avoids duration:0 bug)

    } catch (err) {
        console.log('[MusicKit] PLAY ERROR: ' + err.name + ': ' + err.message);
        if (bridge) bridge.onError('Play error: ' + err.name + ': ' + err.message);
    }
}

async function pausePlayback() {
    try {
        if (music) await music.pause();
    } catch (err) {
        console.error('[MusicKit] Pause error:', err);
        if (bridge) bridge.onError('Pause error: ' + (err.message || String(err)));
    }
}

async function resumePlayback() {
    try {
        if (music) await music.play();
    } catch (err) {
        console.error('[MusicKit] Resume error:', err);
        if (bridge) bridge.onError('Resume error: ' + (err.message || String(err)));
    }
}

async function togglePlayback() {
    try {
        if (!music) return;
        var state = music.playbackState;
        // playing = 2, paused = 3
        if (state === MusicKit.PlaybackStates.playing) {
            console.log('[MusicKit] togglePlayback: pausing');
            await music.pause();
        } else if (state === MusicKit.PlaybackStates.paused) {
            console.log('[MusicKit] togglePlayback: resuming');
            await music.play();
        } else {
            console.log('[MusicKit] togglePlayback: state=' + state + ', no action');
        }
    } catch (err) {
        console.error('[MusicKit] Toggle error:', err);
        if (bridge) bridge.onError('Toggle error: ' + (err.message || String(err)));
    }
}

async function stopPlayback() {
    try {
        if (music) await music.stop();
    } catch (err) {
        console.error('[MusicKit] Stop error:', err);
        if (bridge) bridge.onError('Stop error: ' + (err.message || String(err)));
    }
}

async function seekTo(position) {
    try {
        if (music) await music.seekToTime(position);
    } catch (err) {
        console.error('[MusicKit] Seek error:', err);
        if (bridge) bridge.onError('Seek error: ' + (err.message || String(err)));
    }
}

function setVolume(vol) {
    try {
        if (music) music.volume = Math.max(0, Math.min(1, vol));
    } catch (err) {
        console.error('[MusicKit] Volume error:', err);
        if (bridge) bridge.onError('Volume error: ' + (err.message || String(err)));
    }
}

function setPlaybackBitrate(quality) {
    if (!music) return JSON.stringify({ success: false, error: 'not initialized' });
    music.bitrate = (quality === 'high')
        ? MusicKit.PlaybackBitrate.HIGH
        : MusicKit.PlaybackBitrate.STANDARD;
    console.log('[MusicKit] Bitrate set to: ' + quality);
    return JSON.stringify({ success: true, bitrate: quality });
}

// ── Audio output device routing via setSinkId() ──────────────────
var _targetDeviceId = null;
var _sinkObserver = null;

async function setOutputDevice(deviceLabel) {
    try {
        console.log('[MusicKit] setOutputDevice called with: ' + deviceLabel);

        // Request temporary mic access to get labeled device list
        try {
            var stream = await navigator.mediaDevices.getUserMedia({ audio: true });
            stream.getTracks().forEach(function(t) { t.stop(); });
        } catch (permErr) {
            console.log('[MusicKit] getUserMedia for labels failed: ' + permErr.message);
        }

        var devices = await navigator.mediaDevices.enumerateDevices();
        var outputs = devices.filter(function(d) { return d.kind === 'audiooutput'; });
        console.log('[MusicKit] Available outputs: ' + outputs.map(function(d) {
            return d.label + ' (' + d.deviceId + ')';
        }).join(', '));

        // Find device matching the label (case-insensitive substring)
        var labelLower = deviceLabel.toLowerCase();
        var target = outputs.find(function(d) {
            return d.label.toLowerCase().indexOf(labelLower) >= 0;
        });

        if (!target) {
            // Try matching individual words from the label
            var words = deviceLabel.split(/\s+/);
            target = outputs.find(function(d) {
                var dl = d.label.toLowerCase();
                return words.every(function(w) { return dl.indexOf(w.toLowerCase()) >= 0; });
            });
        }

        if (!target) {
            console.log('[MusicKit] Device not found: ' + deviceLabel);
            console.log('[MusicKit] Available: ' + outputs.map(function(d) { return d.label; }).join(', '));
            return false;
        }

        _targetDeviceId = target.deviceId;
        console.log('[MusicKit] Target device: ' + target.label + ' id: ' + _targetDeviceId);

        // Apply setSinkId to all existing audio/video elements
        await applySinkToAll();

        // Set up MutationObserver to catch dynamically created audio elements
        if (_sinkObserver) _sinkObserver.disconnect();
        _sinkObserver = new MutationObserver(function(mutations) {
            for (var i = 0; i < mutations.length; i++) {
                var added = mutations[i].addedNodes;
                for (var j = 0; j < added.length; j++) {
                    var node = added[j];
                    if (node.tagName === 'AUDIO' || node.tagName === 'VIDEO') {
                        applySinkToElement(node);
                    }
                    // Also check children of added nodes
                    if (node.querySelectorAll) {
                        var els = node.querySelectorAll('audio, video');
                        for (var k = 0; k < els.length; k++) {
                            applySinkToElement(els[k]);
                        }
                    }
                }
            }
        });
        _sinkObserver.observe(document, { childList: true, subtree: true });

        return true;
    } catch (e) {
        console.log('[MusicKit] setOutputDevice error: ' + e.message);
        return false;
    }
}

async function applySinkToAll() {
    if (!_targetDeviceId) return;
    var els = document.querySelectorAll('audio, video');
    console.log('[MusicKit] Found ' + els.length + ' audio/video elements');
    for (var i = 0; i < els.length; i++) {
        await applySinkToElement(els[i]);
    }
}

async function applySinkToElement(el) {
    if (!_targetDeviceId || !el.setSinkId) return;
    try {
        await el.setSinkId(_targetDeviceId);
        console.log('[MusicKit] setSinkId OK for <' + el.tagName + '> src=' + (el.src || '(none)').substring(0, 60));
    } catch (err) {
        console.log('[MusicKit] setSinkId failed for <' + el.tagName + '>: ' + err.message);
    }
}
</script>
</body>
</html>
)HTML").arg(devToken);
}
