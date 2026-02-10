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
#include <QWebEngineCookieStore>
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
            // Token is already embedded in HTML via %3 — no JS injection needed
            qDebug() << "[MusicKitPlayer] WebView ready (token embedded in HTML:"
                     << !m_pendingUserToken.isEmpty() << ")";
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
        // Only create WebView if we have a token — otherwise wait for token
        if (m_pendingUserToken.isEmpty()) {
            qDebug() << "[MusicKitPlayer] No token yet — queuing song, waiting for token";
            return;
        }
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

    // If token was pre-set via __musicUserToken JS global, check if
    // MusicKit.configure() actually picked it up (isAuthorized == true).
    if (!m_pendingUserToken.isEmpty()) {
        qDebug() << "[MusicKitPlayer] MusicKit ready — checking if token was included in configure";
        m_webView->page()->runJavaScript(
            QStringLiteral("music ? music.isAuthorized : false"),
            [this](const QVariant& result) {
                bool isAuth = result.toBool();
                qDebug() << "[MusicKitPlayer] Post-configure auth check: isAuthorized =" << isAuth;
                if (isAuth) {
                    qDebug() << "[MusicKitPlayer] Token was in MusicKit.configure() — full playback available";
                    m_pendingUserToken.clear();
                    emit fullPlaybackAvailable();
                    if (!m_pendingSongId.isEmpty()) {
                        QString id = m_pendingSongId;
                        m_pendingSongId.clear();
                        qDebug() << "[MusicKitPlayer] Playing pending song:" << id;
                        play(id);
                    }
                } else {
                    // Token wasn't in configure (race: CDN loaded before JS global was set)
                    qDebug() << "[MusicKitPlayer] Token was NOT in configure — trying injection";
                    QString token = m_pendingUserToken;
                    m_pendingUserToken.clear();
                    injectMusicUserToken(token);
                }
            });
        return;  // Async — pending song handled in callback
    }

    // No token pending — play pending song directly
    if (!m_pendingSongId.isEmpty()) {
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
        qDebug() << "[MusicKitPlayer] WebView is null, storing token";
        m_pendingUserToken = token;
        // Create WebView with token embedded in HTML — token is guaranteed in configure()
        if (!m_initialized) {
            qDebug() << "[MusicKitPlayer] Token received — creating WebView with token in HTML";
            ensureWebView();
        }
        return;
    }

    if (!m_ready) {
        qDebug() << "[MusicKitPlayer] MusicKit not ready yet, storing as pending";
        m_pendingUserToken = token;
        // WebView loaded but MusicKit CDN still loading — set JS global
        // so configureMusicKit() will include the token
        if (m_webViewReady) {
            qDebug() << "[MusicKitPlayer] WebView ready, setting __musicUserToken JS global";
            QString escaped = token;
            escaped.replace(QLatin1String("\\"), QLatin1String("\\\\"));
            escaped.replace(QLatin1Char('\''), QLatin1String("\\'"));
            escaped.replace(QLatin1Char('\n'), QLatin1String("\\n"));
            escaped.replace(QLatin1Char('\r'), QLatin1String("\\r"));
            m_webView->page()->runJavaScript(
                QStringLiteral("__musicUserToken = '%1'; "
                    "console.log('[MusicKit] __musicUserToken set by C++ (late, length: ' + __musicUserToken.length + ')');")
                .arg(escaped));
        }
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

    m_webView->page()->runJavaScript(js, [this, token](const QVariant& result) {
        QString status = result.toString();
        qDebug() << "[MusicKitPlayer] Token injection JS returned:" << status;

        if (status == QLatin1String("ok")) {
            qDebug() << "[MusicKitPlayer] Token injection SUCCEEDED — full playback available";
            emit fullPlaybackAvailable();
            if (!m_pendingSongId.isEmpty()) {
                QString id = m_pendingSongId;
                m_pendingSongId.clear();
                qDebug() << "[MusicKitPlayer] Token ready, playing pending song:" << id;
                play(id);
            } else {
                // Replay current song with full-access token
                runJS(QStringLiteral(
                    "(function() {"
                    "  if (!music || !music.nowPlayingItem) return;"
                    "  var id = music.nowPlayingItem.id;"
                    "  if (id) {"
                    "    console.log('[MusicKit] Token injected during playback — replaying: ' + id);"
                    "    music.setQueue({ song: id }).then(function() { return music.play(); })"
                    "    .catch(function(e) { console.error('[MusicKit] Replay error:', e); });"
                    "  }"
                    "})()"
                ));
            }
        } else if (status == QLatin1String("needs_reinit")) {
            qDebug() << "[MusicKitPlayer] Direct token set failed — reinitializing with token in configure()";
            // Store token for the new WebView's loadFinished handler
            m_pendingUserToken = token;
            // Tear down current WebView
            if (m_webView) {
                m_webView->disconnect();
                m_webView->deleteLater();
                m_webView = nullptr;
            }
            delete m_channel;
            m_channel = nullptr;
            m_ready = false;
            m_initialized = false;
            m_webViewReady = false;
            // Recreate — loadFinished will pre-set __musicUserToken before configure
            qDebug() << "[MusicKitPlayer] Recreating WebView with token in configure path";
            ensureWebView();
        } else {
            qDebug() << "[MusicKitPlayer] Token injection returned unexpected:" << status;
        }
    });
}

void MusicKitPlayer::clearMusicUserToken()
{
    qDebug() << "[MusicKitPlayer] Clearing Music User Token — full WebView teardown";
    m_pendingUserToken.clear();
    m_pendingSongId.clear();

    // Destroy WebView entirely — clears cookies, storage, cached JS auth state
    if (m_webView) {
        if (m_ready)
            runJS(QStringLiteral("if(music) music.stop()"));

        QWebEnginePage* page = m_webView->page();
        // Clear all browsing data (cookies, localStorage, sessionStorage)
        if (page && page->profile()) {
            page->profile()->clearHttpCache();
            page->profile()->cookieStore()->deleteAllCookies();
        }

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

    qDebug() << "[MusicKitPlayer] WebView destroyed — reconnect will create fresh instance";
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
    devToken.replace(QLatin1Char('\''), QStringLiteral("\\'"));

    // Escape music user token for safe embedding in HTML/JS
    QString userToken = m_pendingUserToken;
    userToken.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    userToken.replace(QLatin1Char('\''), QLatin1String("\\'"));
    userToken.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    userToken.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    qDebug() << "[MusicKitPlayer] Embedding token in HTML, length:" << m_pendingUserToken.length();

    QString html = QStringLiteral(R"HTML(
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
var __musicUserToken = '%3' || null;  // Embedded by C++ at HTML generation

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

        var config = {
            developerToken: token,
            app: {
                name: 'Sorana Flow',
                build: '%2'
            }
        };

        // Include Music User Token if C++ pre-set it before configure
        if (__musicUserToken) {
            config.musicUserToken = __musicUserToken;
            console.log('[MusicKit] Including pre-set Music User Token in configure (length: ' + __musicUserToken.length + ')');
        } else {
            console.log('[MusicKit] No Music User Token available at configure time');
        }

        await MusicKit.configure(config);
        console.log('[MusicKit] MusicKit configured');

        music = MusicKit.getInstance();
        console.log('[MusicKit] Got MusicKit instance');

        // Set HIGH quality (256kbps AAC) as default
        music.bitrate = MusicKit.PlaybackBitrate.HIGH;
        console.log('[MusicKit] Bitrate set to HIGH (256kbps)');

        // Call authorize() — required for DRM stream access even when
        // musicUserToken was passed in configure(). In WebView context with
        // a valid token, this completes immediately without showing a popup.
        try {
            await music.authorize();
            console.log('[MusicKit] authorize() succeeded — isAuthorized: ' + music.isAuthorized);
        } catch (authErr) {
            console.log('[MusicKit] authorize() error (non-fatal): ' + authErr);
        }

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
        return 'no_instance';
    }

    console.log('[MusicKit] Injecting Music User Token (length: ' + token.length + ')');

    // Try direct property assignment
    music.musicUserToken = token;

    var isAuth = music.isAuthorized;
    var previewOnly = music.previewOnly || false;
    var hasToken = music.musicUserToken ? true : false;

    console.log('[MusicKit] After direct set:');
    console.log('[MusicKit]   isAuthorized: ' + isAuth);
    console.log('[MusicKit]   previewOnly: ' + previewOnly);
    console.log('[MusicKit]   hasToken: ' + hasToken);
    console.log('[MusicKit]   tokenLength: ' + (music.musicUserToken ? music.musicUserToken.length : 0));

    if (hasToken && isAuth) {
        if (bridge) bridge.onAuthStatusChanged(JSON.stringify({
            isAuthorized: true, previewOnly: false, hasToken: true, tokenLength: token.length
        }));
        return 'ok';
    }

    // Direct set failed — store token and return 'needs_reinit' so C++ can
    // tear down and reconfigure MusicKit with the token passed in configure()
    console.log('[MusicKit] Direct token set FAILED — musicUserToken is read-only in this MusicKit version');
    __musicUserToken = token;
    return 'needs_reinit';
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
)HTML").arg(devToken, QCoreApplication::applicationVersion(), userToken);

    return html;
}
