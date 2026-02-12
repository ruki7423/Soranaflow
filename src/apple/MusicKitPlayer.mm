#include "MusicKitPlayer.h"
#include "AppleMusicManager.h"
#include "../core/audio/AudioDeviceManager.h"
#include "../core/Settings.h"
#ifdef Q_OS_MACOS
#include "../platform/macos/AudioProcessTap.h"
#endif

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>

#import <WebKit/WebKit.h>
#import <AppKit/AppKit.h>

// ═════════════════════════════════════════════════════════════════════
//  Objective-C helper: WKScriptMessageHandler + WKNavigationDelegate
// ═════════════════════════════════════════════════════════════════════

@interface MusicKitMessageHandler : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, assign) MusicKitPlayer* player;
@property (nonatomic, strong) NSPanel* authPanel;
@end

@implementation MusicKitMessageHandler

- (void)userContentController:(WKUserContentController*)ucc
      didReceiveScriptMessage:(WKScriptMessage*)message
{
    MusicKitPlayer* p = self.player;
    if (!p) return;

    NSString* name = message.name;
    id body = message.body;

    if ([name isEqualToString:@"musicKitReady"]) {
        QMetaObject::invokeMethod(p, [p]() { p->onMusicKitReady(); },
                                  Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"playbackState"]) {
        bool playing = [body boolValue];
        QMetaObject::invokeMethod(p, [p, playing]() { p->onPlaybackStateChanged(playing); },
                                  Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"nowPlaying"]) {
        NSDictionary* dict = (NSDictionary*)body;
        QString title   = QString::fromNSString(dict[@"title"] ?: @"");
        QString artist  = QString::fromNSString(dict[@"artist"] ?: @"");
        QString album   = QString::fromNSString(dict[@"album"] ?: @"");
        double duration = [dict[@"duration"] doubleValue];
        QMetaObject::invokeMethod(p, [p, title, artist, album, duration]() {
            p->onNowPlayingChanged(title, artist, album, duration);
        }, Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"playbackTime"]) {
        NSDictionary* dict = (NSDictionary*)body;
        double currentTime = [dict[@"currentTime"] doubleValue];
        double totalTime   = [dict[@"totalTime"] doubleValue];
        QMetaObject::invokeMethod(p, [p, currentTime, totalTime]() {
            p->onPlaybackTimeChanged(currentTime, totalTime);
        }, Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"error"]) {
        QString err = QString::fromNSString([body description]);
        QMetaObject::invokeMethod(p, [p, err]() { p->onError(err); },
                                  Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"authStatus"]) {
        QString json = QString::fromNSString([body description]);
        QMetaObject::invokeMethod(p, [p, json]() { p->onAuthStatusChanged(json); },
                                  Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"playbackStarted"]) {
        // body is a JSON string
        QString json = QString::fromNSString([body description]);
        QMetaObject::invokeMethod(p, [p, json]() { p->onPlaybackStarted(json); },
                                  Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"tokenExpired"]) {
        QMetaObject::invokeMethod(p, [p]() { p->onTokenExpired(); },
                                  Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"playbackEnded"]) {
        QMetaObject::invokeMethod(p, [p]() { p->onPlaybackEnded(); },
                                  Qt::QueuedConnection);
    }
    else if ([name isEqualToString:@"log"]) {
        QString msg = QString::fromNSString([body description]);
        qDebug() << "[MusicKit JS]" << msg;
    }
    else if ([name isEqualToString:@"tokenInjectionResult"]) {
        // Handled via evaluateJavaScript completion — log only
        QString result = QString::fromNSString([body description]);
        qDebug() << "[MusicKit JS] tokenInjectionResult:" << result;
    }
    else if ([name isEqualToString:@"authWillPrompt"]) {
        qDebug() << "[MusicKitPlayer] Auth prompt incoming — bringing app to front";
        // Bring app to front so macOS "Allow Access" dialog is visible
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp activateIgnoringOtherApps:YES];
        });
        // System dialog may appear after a short delay — re-activate to ensure visibility
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            [NSApp activateIgnoringOtherApps:YES];
        });
        QMetaObject::invokeMethod(p, [p]() { emit p->authorizationPending(); },
                                  Qt::QueuedConnection);
    }
}

// WKNavigationDelegate
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation
{
    qDebug() << "[MusicKitPlayer] WKWebView loadFinished";
    // Notify player that the page is loaded
    if (self.player) {
        QMetaObject::invokeMethod(self.player, "webViewDidFinishLoad",
                                  Qt::QueuedConnection);
    }
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation
      withError:(NSError*)error
{
    QString err = QString::fromNSString(error.localizedDescription);
    qDebug() << "[MusicKitPlayer] WKWebView navigation error:" << err;
    if (self.player) {
        MusicKitPlayer* p = self.player;
        QMetaObject::invokeMethod(p, [p, err]() {
            emit p->errorOccurred(QStringLiteral("Failed to load MusicKit player page: ") + err);
        }, Qt::QueuedConnection);
    }
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
      withError:(NSError*)error
{
    QString err = QString::fromNSString(error.localizedDescription);
    qDebug() << "[MusicKitPlayer] WKWebView provisional navigation error:" << err;
    if (self.player) {
        MusicKitPlayer* p = self.player;
        QMetaObject::invokeMethod(p, [p, err]() {
            emit p->errorOccurred(QStringLiteral("Failed to load MusicKit player page: ") + err);
        }, Qt::QueuedConnection);
    }
}

// WKUIDelegate — handle authorize() popup (window.open)
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
    forNavigationAction:(WKNavigationAction*)navigationAction
    windowFeatures:(WKWindowFeatures*)windowFeatures
{
    WKWebView* popup = [[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 500, 600)
                                          configuration:configuration];
    popup.navigationDelegate = self;
    popup.UIDelegate = self;

    NSPanel* panel = [[NSPanel alloc]
        initWithContentRect:CGRectMake(0, 0, 500, 600)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    panel.contentView = popup;
    panel.title = @"Apple Music Sign In";
    [panel center];
    [panel setLevel:NSFloatingWindowLevel];
    [panel makeKeyAndOrderFront:nil];
    panel.releasedWhenClosed = NO;
    self.authPanel = panel;

    qDebug() << "[MusicKitPlayer] Auth popup created at floating level";

    // Re-raise after auth page transitions (Apple ID sign-in → Access Request)
    // Can't use __weak (MRC), so capture the panel pointer directly.
    // The delayed blocks only call methods if the panel is still visible.
    NSPanel* panelRef = panel;
    for (double delay : {0.5, 1.5, 3.0}) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            if (panelRef && [panelRef isVisible]) {
                [NSApp activateIgnoringOtherApps:YES];
                [panelRef makeKeyAndOrderFront:nil];
            }
        });
    }

    return popup;
}

// WKUIDelegate — popup closed itself (window.close from JS)
- (void)webViewDidClose:(WKWebView*)webView
{
    if (self.authPanel) {
        [self.authPanel setLevel:NSNormalWindowLevel];
        [self.authPanel orderOut:nil];
        [self.authPanel close];
        self.authPanel = nil;
        qDebug() << "[MusicKitPlayer] Auth popup closed (webViewDidClose)";
    }
}

- (void)closeAuthPanel
{
    if (self.authPanel) {
        [self.authPanel setLevel:NSNormalWindowLevel];
        [self.authPanel orderOut:nil];
        [self.authPanel close];
        self.authPanel = nil;
        qDebug() << "[MusicKitPlayer] Auth popup closed";
    }
}

@end

// ═════════════════════════════════════════════════════════════════════
//  Private data structure holding WKWebView + ObjC handler
// ═════════════════════════════════════════════════════════════════════

struct MusicKitWebViewPrivate {
    WKWebView* webView = nil;
    MusicKitMessageHandler* handler = nil;

    ~MusicKitWebViewPrivate() {
        if (webView) {
            [webView stopLoading];
            [webView.configuration.userContentController removeAllScriptMessageHandlers];
            [webView removeFromSuperview];
            webView = nil;
        }
        handler = nil;
    }
};

// ═════════════════════════════════════════════════════════════════════
//  Singleton
// ═════════════════════════════════════════════════════════════════════

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
    if (m_cleanedUp) return;
    m_cleanedUp = true;
    qDebug() << "[MusicKitPlayer] cleanup START";
    if (m_wk) {
        // Stop loading FIRST — prevents WKWebView dealloc from blocking
        // on active network requests or media buffering
        if (m_wk->webView)
            [m_wk->webView stopLoading];

        if (m_ready)
            runJS(QStringLiteral("if(music) music.stop()"));

        // Disconnect handler to prevent stale callbacks
        if (m_wk->handler)
            m_wk->handler.player = nil;

        delete m_wk;
        m_wk = nullptr;
    }
    m_ready = false;
    m_initialized = false;
    m_webViewReady = false;
    qDebug() << "[MusicKitPlayer] cleanup DONE";
}

// ── Destructor ──────────────────────────────────────────────────────
MusicKitPlayer::~MusicKitPlayer()
{
    cleanup();
}

// ── webViewDidFinishLoad — called from ObjC delegate ────────────────
// (Q_INVOKABLE so QMetaObject::invokeMethod can find it by name)
void MusicKitPlayer::webViewDidFinishLoad()
{
    m_webViewReady = true;
    qDebug() << "[MusicKitPlayer] WKWebView ready (token embedded in HTML:"
             << !m_pendingUserToken.isEmpty() << ")";
}

// ── ensureWebView — lazy initialization ─────────────────────────────
void MusicKitPlayer::ensureWebView()
{
    if (m_initialized) return;
    m_initialized = true;

    qDebug() << "[MusicKitPlayer] Initializing WKWebView...";

    @autoreleasepool {
        m_wk = new MusicKitWebViewPrivate;

        // Create message handler
        m_wk->handler = [[MusicKitMessageHandler alloc] init];
        m_wk->handler.player = this;

        // Configure WKWebView
        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;

        WKWebpagePreferences* pagePrefs = [[WKWebpagePreferences alloc] init];
        pagePrefs.allowsContentJavaScript = YES;
        config.defaultWebpagePreferences = pagePrefs;

        // Register message handlers for JS → C++ bridge
        WKUserContentController* ucc = [[WKUserContentController alloc] init];
        NSArray* handlerNames = @[
            @"musicKitReady", @"playbackState", @"nowPlaying",
            @"playbackTime", @"error", @"authStatus", @"playbackStarted",
            @"tokenExpired", @"playbackEnded", @"log", @"tokenInjectionResult",
            @"authWillPrompt"
        ];
        for (NSString* name in handlerNames) {
            [ucc addScriptMessageHandler:m_wk->handler name:name];
        }
        config.userContentController = ucc;

        // Create 1x1 hidden WKWebView — must be in window hierarchy for audio
        m_wk->webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)
                                            configuration:config];
        m_wk->webView.navigationDelegate = m_wk->handler;
        m_wk->webView.UIDelegate = m_wk->handler;
        m_wk->webView.hidden = YES;

        // Attach to key window so audio playback works
        // (WKWebView requires being in a window hierarchy to play media)
        NSWindow* keyWindow = [NSApp keyWindow];
        if (!keyWindow) keyWindow = [[NSApp windows] firstObject];
        if (keyWindow && keyWindow.contentView) {
            [keyWindow.contentView addSubview:m_wk->webView];
            m_wk->webView.frame = NSMakeRect(-1, -1, 1, 1);
            qDebug() << "[MusicKitPlayer] WKWebView attached to window";
        } else {
            qDebug() << "[MusicKitPlayer] WARNING: No window available — "
                        "WKWebView may not play audio until attached";
            // Retry attachment after a delay
            QTimer::singleShot(500, this, [this]() {
                if (!m_wk || !m_wk->webView) return;
                NSWindow* win = [NSApp keyWindow];
                if (!win) win = [[NSApp windows] firstObject];
                if (win && win.contentView) {
                    [win.contentView addSubview:m_wk->webView];
                    m_wk->webView.frame = NSMakeRect(-1, -1, 1, 1);
                    qDebug() << "[MusicKitPlayer] WKWebView attached (deferred)";
                }
            });
        }

        // Get developer token
        auto* am = AppleMusicManager::instance();
        QString token = am->developerToken();
        qDebug() << "[MusicKitPlayer] Developer token length:" << token.length();

        if (token.isEmpty()) {
            qDebug() << "[MusicKitPlayer] WARNING: No developer token available!";
        }

        // Load HTML
        QString html = generateHTML();
        NSString* htmlNS = html.toNSString();
        NSURL* baseURL = [NSURL URLWithString:@"https://sorana.local"];
        [m_wk->webView loadHTMLString:htmlNS baseURL:baseURL];
        qDebug() << "[MusicKitPlayer] HTML loading started";
    }
}

// ── play ────────────────────────────────────────────────────────────
void MusicKitPlayer::play(const QString& songId)
{
    qDebug() << "[MusicKitPlayer] play() called with songId:" << songId;

    if (!m_initialized) {
        m_pendingSongId = songId;
        if (m_pendingUserToken.isEmpty()) {
            qDebug() << "[MusicKitPlayer] No token yet — queuing song, waiting for token";
            emit playbackStateChanged(false); // signal UI: not playing yet
            return;
        }
        qDebug() << "[MusicKitPlayer] Token available, creating WebView for queued song";
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
//  JS → C++ callbacks (called via WKScriptMessageHandler)
// ═════════════════════════════════════════════════════════════════════

void MusicKitPlayer::onMusicKitReady()
{
    m_ready = true;
    qDebug() << "[MusicKitPlayer] MusicKit JS ready!";
    emit ready();
    emit musicKitReady();

    // Route audio to the app's selected output device
    updateOutputDevice();

    // If token was pre-set via __musicUserToken JS global, check auth
    if (!m_pendingUserToken.isEmpty()) {
        qDebug() << "[MusicKitPlayer] MusicKit ready — checking if token was included in configure";
        NSString* js = @"music ? music.isAuthorized : false";
        [m_wk->webView evaluateJavaScript:js completionHandler:^(id result, NSError* error) {
            bool isAuth = [result boolValue];
            QMetaObject::invokeMethod(this, [this, isAuth]() {
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
                    qDebug() << "[MusicKitPlayer] Token was NOT in configure — trying injection";
                    QString token = m_pendingUserToken;
                    m_pendingUserToken.clear();
                    injectMusicUserToken(token);
                }
            }, Qt::QueuedConnection);
        }];
        return;
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

    if (token.isEmpty()) {
        qDebug() << "[MusicKitPlayer] IGNORED: empty token injection (would reset auth)";
        return;
    }

    if (!m_wk || !m_wk->webView) {
        qDebug() << "[MusicKitPlayer] WebView is null, storing token";
        m_pendingUserToken = token;
        if (!m_initialized) {
            if (!m_pendingSongId.isEmpty())
                qDebug() << "[MusicKitPlayer] Token received — creating WebView (pending song:" << m_pendingSongId << ")";
            else
                qDebug() << "[MusicKitPlayer] Token received — creating WebView with token in HTML";
            ensureWebView();
        }
        return;
    }

    if (!m_ready) {
        qDebug() << "[MusicKitPlayer] MusicKit not ready yet, storing as pending";
        m_pendingUserToken = token;
        if (m_webViewReady) {
            qDebug() << "[MusicKitPlayer] WebView ready, setting __musicUserToken JS global";
            QString escaped = token;
            escaped.replace(QLatin1String("\\"), QLatin1String("\\\\"));
            escaped.replace(QLatin1Char('\''), QLatin1String("\\'"));
            escaped.replace(QLatin1Char('\n'), QLatin1String("\\n"));
            escaped.replace(QLatin1Char('\r'), QLatin1String("\\r"));
            NSString* js = QStringLiteral(
                "__musicUserToken = '%1'; "
                "console.log('[MusicKit] __musicUserToken set by C++ (late, length: ' + __musicUserToken.length + ')');")
                .arg(escaped).toNSString();
            [m_wk->webView evaluateJavaScript:js completionHandler:nil];
        }
        return;
    }

    // Escape for safe JS string embedding
    QString escapedToken = token;
    escapedToken.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    escapedToken.replace(QLatin1Char('\''), QLatin1String("\\'"));
    escapedToken.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    escapedToken.replace(QLatin1Char('\r'), QLatin1String("\\r"));

    QString jsQ = QStringLiteral("injectMusicUserToken('%1')").arg(escapedToken);
    qDebug() << "[MusicKitPlayer] Running JS injection (token length:" << token.length() << ")";

    NSString* js = jsQ.toNSString();
    [m_wk->webView evaluateJavaScript:js completionHandler:^(id result, NSError* error) {
        QString status = QString::fromNSString([result description]);
        QMetaObject::invokeMethod(this, [this, status, token]() {
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
                m_pendingUserToken = token;
                // Tear down current WebView
                if (m_wk) {
                    if (m_wk->handler)
                        m_wk->handler.player = nil;
                    delete m_wk;
                    m_wk = nullptr;
                }
                m_ready = false;
                m_initialized = false;
                m_webViewReady = false;
                qDebug() << "[MusicKitPlayer] Recreating WebView with token in configure path";
                ensureWebView();
            } else {
                qDebug() << "[MusicKitPlayer] Token injection returned unexpected:" << status;
            }
        }, Qt::QueuedConnection);
    }];
}

void MusicKitPlayer::clearMusicUserToken()
{
    qDebug() << "[MusicKitPlayer] Clearing Music User Token — full WebView teardown";
    m_pendingUserToken.clear();
    m_pendingSongId.clear();

    if (m_wk && m_wk->webView) {
        if (m_ready)
            runJS(QStringLiteral("if(music) music.stop()"));

        // Clear cookies/storage via WKWebsiteDataStore
        WKWebsiteDataStore* store = m_wk->webView.configuration.websiteDataStore;
        NSSet* types = [WKWebsiteDataStore allWebsiteDataTypes];
        [store removeDataOfTypes:types
                   modifiedSince:[NSDate distantPast]
               completionHandler:^{}];

        if (m_wk->handler)
            m_wk->handler.player = nil;
        delete m_wk;
        m_wk = nullptr;
    }
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
        if (m_wk && m_wk->handler) {
            [m_wk->handler closeAuthPanel];
        }
        emit fullPlaybackAvailable();
    } else if (isAuthorized && previewOnly) {
        qDebug() << "[MusicKitPlayer] Preview only mode (no active subscription?)";
        if (m_wk && m_wk->handler) {
            [m_wk->handler closeAuthPanel];
        }
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
    // WKWebView uses the system default audio output — no setSinkId available
    // When ProcessTap is active, it handles routing. Otherwise, macOS routes
    // WKWebView audio to the system default output automatically.
    if (!m_ready || !m_wk || !m_wk->webView) return;

#ifdef Q_OS_MACOS
    if (AudioProcessTap::instance()->isActive()) {
        qDebug() << "[MusicKitPlayer] ProcessTap active — WKWebView audio captured by tap";
        return;
    }
#endif

    qDebug() << "[MusicKitPlayer] WKWebView uses system default audio output"
             << "(no setSinkId in WKWebView)";
}

// ── runJS ───────────────────────────────────────────────────────────
void MusicKitPlayer::runJS(const QString& js)
{
    if (!m_wk || !m_wk->webView) return;

    NSString* jsNS = js.toNSString();
    NSString* jsLeft = [jsNS length] > 40 ? [jsNS substringToIndex:40] : jsNS;

    [m_wk->webView evaluateJavaScript:jsNS completionHandler:^(id result, NSError* error) {
        if (result && ![result isKindOfClass:[NSNull class]]) {
            QString r = QString::fromNSString([result description]);
            qDebug() << "[MusicKitPlayer] JS result for"
                     << QString::fromNSString(jsLeft) << ":" << r;
        }
        if (error) {
            qDebug() << "[MusicKitPlayer] JS error:"
                     << QString::fromNSString(error.localizedDescription);
        }
    }];
}

// ═════════════════════════════════════════════════════════════════════
//  generateHTML — embedded MusicKit JS page (WKWebView native bridge)
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
</head>
<body>
<script>
var music = null;
var playbackStartedEmitted = false;
var __musicUserToken = '%3' || null;

// ── Native bridge helpers ──────────────────────────────────────────
function msg(name, body) {
    try { window.webkit.messageHandlers[name].postMessage(body); } catch(e) {
        console.error('[MusicKit] msg(' + name + ') failed:', e);
    }
}

function log(text) { msg('log', text); }

log('[MusicKit] Loading MusicKit JS from CDN...');

// Load MusicKit JS dynamically
var script = document.createElement('script');
script.src = 'https://js-cdn.music.apple.com/musickit/v3/musickit.js';
script.setAttribute('data-web-components', '');
script.onload = function() {
    log('[MusicKit] MusicKit JS loaded from CDN');
    configureMusicKit();
};
script.onerror = function(e) {
    log('[MusicKit] Failed to load MusicKit JS from CDN');
    msg('error', 'Failed to load MusicKit JS from CDN');
};
document.head.appendChild(script);

async function configureMusicKit() {
    try {
        var token = '%1';
        log('[MusicKit] Configuring with token length: ' + token.length);

        if (!token || token === '' || token === 'DEVELOPER_TOKEN') {
            log('[MusicKit] No valid developer token!');
            msg('error', 'No valid developer token');
            return;
        }

        var config = {
            developerToken: token,
            app: {
                name: 'Sorana Flow',
                build: '%2'
            }
        };

        if (__musicUserToken) {
            config.musicUserToken = __musicUserToken;
            log('[MusicKit] Including pre-set Music User Token in configure (length: ' + __musicUserToken.length + ')');
        } else {
            log('[MusicKit] No Music User Token available at configure time');
        }

        await MusicKit.configure(config);
        log('[MusicKit] MusicKit configured');

        music = MusicKit.getInstance();
        log('[MusicKit] Got MusicKit instance');

        music.bitrate = MusicKit.PlaybackBitrate.HIGH;
        log('[MusicKit] Bitrate set to HIGH (256kbps)');

        // If token was in configure() but isAuthorized is still false,
        // try setting it directly on the instance
        if (!music.isAuthorized && __musicUserToken) {
            try {
                music.musicUserToken = __musicUserToken;
                log('[MusicKit] Set musicUserToken directly on instance (length: ' + __musicUserToken.length + ')');
            } catch (tokenErr) {
                log('[MusicKit] musicUserToken direct set failed (read-only): ' + tokenErr);
            }
        }

        if (music.isAuthorized) {
            log('[MusicKit] Already authorized — skipping authorize()');
        } else if (__musicUserToken) {
            // Token present but isAuthorized still false — authorize() may succeed silently
            log('[MusicKit] Token present but not yet authorized — calling authorize()');
            try {
                msg('authWillPrompt', true);
                await music.authorize();
                log('[MusicKit] authorize() succeeded — isAuthorized: ' + music.isAuthorized);
            } catch (authErr) {
                log('[MusicKit] authorize() error (non-fatal): ' + authErr);
            }
        } else {
            // No token at all — first-time user, authorize popup expected
            log('[MusicKit] No token — calling authorize() for first-time setup');
            try {
                msg('authWillPrompt', true);
                await music.authorize();
                log('[MusicKit] authorize() succeeded — isAuthorized: ' + music.isAuthorized);
            } catch (authErr) {
                log('[MusicKit] authorize() error (non-fatal): ' + authErr);
            }
        }
        log('[MusicKit] Auth check done — isAuthorized: ' + music.isAuthorized);

        // Event listeners
        music.addEventListener('playbackStateDidChange', function(event) {
            var state = event.state;
            var names = ['none','loading','playing','paused','stopped',
                         'ended','seeking','waiting','stalled','completed'];
            var name = names[state] || 'unknown(' + state + ')';
            var isPlaying = (state === MusicKit.PlaybackStates.playing);
            log('[MusicKit] playbackStateDidChange: ' + state + ' (' + name + ') playing: ' + isPlaying);
            msg('playbackState', isPlaying);

            if (state === MusicKit.PlaybackStates.ended ||
                state === MusicKit.PlaybackStates.completed) {
                log('[MusicKit] Track ended — notifying bridge');
                msg('playbackEnded', true);
            }

            if (isPlaying && music.nowPlayingItem && !playbackStartedEmitted) {
                playbackStartedEmitted = true;
                var item = music.nowPlayingItem;
                var dur = (item.playbackDuration || music.currentPlaybackDuration || 0) / 1000;
                log('[MusicKit] Emitting playbackStarted: duration=' + dur);
                msg('playbackStarted', JSON.stringify({
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
            if (item) {
                log('[MusicKit] nowPlayingItemDidChange: ' + item.title);
                msg('nowPlaying', {
                    title: item.title || '',
                    artist: item.artistName || '',
                    album: item.albumName || '',
                    duration: (item.playbackDuration || 0) / 1000
                });
            }
        });

        music.addEventListener('playbackTimeDidChange', function(event) {
            msg('playbackTime', {
                currentTime: music.currentPlaybackTime || 0,
                totalTime: music.currentPlaybackDuration || 0
            });
        });

        music.addEventListener('mediaPlaybackError', function(event) {
            log('[MusicKit] mediaPlaybackError: ' + JSON.stringify(event));
            msg('error', 'Playback error: ' + JSON.stringify(event));
        });

        music.addEventListener('authorizationStatusDidChange', function(event) {
            log('[MusicKit] authorizationStatusDidChange: ' + JSON.stringify(event));
        });

        music.addEventListener('playbackDurationDidChange', function(event) {
            log('[MusicKit] duration: ' + music.currentPlaybackDuration);
        });

        log('[MusicKit] All event listeners registered');
        log('[MusicKit] Waiting for Music User Token...');
        msg('musicKitReady', true);
        log('[MusicKit] Ready signal sent to C++');

    } catch (err) {
        log('[MusicKit] Configure error: ' + (err.message || String(err)));
        msg('error', 'MusicKit configure error: ' + (err.message || String(err)));
    }
}

// ── Music User Token injection ──────────────────────────────────
function injectMusicUserToken(token) {
    if (!music) {
        log('[MusicKit] ERROR: MusicKit instance not initialized');
        msg('error', 'MusicKit not initialized when injecting token');
        return 'no_instance';
    }

    log('[MusicKit] Injecting Music User Token (length: ' + token.length + ')');

    music.musicUserToken = token;

    var isAuth = music.isAuthorized;
    var previewOnly = music.previewOnly || false;
    var hasToken = music.musicUserToken ? true : false;

    log('[MusicKit] After direct set:');
    log('[MusicKit]   isAuthorized: ' + isAuth);
    log('[MusicKit]   previewOnly: ' + previewOnly);
    log('[MusicKit]   hasToken: ' + hasToken);

    if (hasToken && isAuth) {
        msg('authStatus', JSON.stringify({
            isAuthorized: true, previewOnly: false, hasToken: true, tokenLength: token.length
        }));
        return 'ok';
    }

    log('[MusicKit] Direct token set FAILED — musicUserToken is read-only in this MusicKit version');
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

// ── Playback controls ──────────────────────────────────────────
async function playSong(songId) {
    try {
        playbackStartedEmitted = false;

        log('[MusicKit] ========================================');
        log('[MusicKit] playSong called with: ' + songId);
        log('[MusicKit] Pre-play diagnostics:');
        log('[MusicKit]   isAuthorized: ' + music.isAuthorized);
        log('[MusicKit]   previewOnly: ' + (music.previewOnly || false));
        log('[MusicKit]   musicUserToken: ' +
            (music.musicUserToken ? 'present (len=' + music.musicUserToken.length + ')' : 'ABSENT'));

        if (!music) {
            log('[MusicKit] music instance is null!');
            msg('error', 'MusicKit not initialized');
            return;
        }

        log('[MusicKit] Setting queue...');
        await music.setQueue({ song: songId });
        log('[MusicKit] Calling music.play()...');
        try {
            await music.play();
        } catch (playErr) {
            log('[MusicKit] play() failed: ' + playErr + ' — retrying in 1s...');
            await new Promise(function(r) { setTimeout(r, 1000); });
            await music.play();
        }
        log('[MusicKit] music.play() returned, state: ' + music.playbackState);

        // Stall recovery: if stuck in loading/stalled after 5s, retry play
        setTimeout(function() {
            if (!music) return;
            var st = music.playbackState;
            if (st === MusicKit.PlaybackStates.stalled ||
                st === MusicKit.PlaybackStates.loading) {
                log('[MusicKit] Stall detected after 5s (state=' + st + ') — auto-recovering');
                music.play().catch(function(e) {
                    log('[MusicKit] Stall recovery failed: ' + e);
                });
            }
        }, 5000);

    } catch (err) {
        log('[MusicKit] PLAY ERROR: ' + err.name + ': ' + err.message);
        msg('error', 'Play error: ' + err.name + ': ' + err.message);
    }
}

async function pausePlayback() {
    try {
        if (music) await music.pause();
    } catch (err) {
        log('[MusicKit] Pause error: ' + (err.message || String(err)));
        msg('error', 'Pause error: ' + (err.message || String(err)));
    }
}

async function resumePlayback() {
    try {
        if (music) await music.play();
    } catch (err) {
        log('[MusicKit] Resume error: ' + (err.message || String(err)));
        msg('error', 'Resume error: ' + (err.message || String(err)));
    }
}

async function togglePlayback() {
    try {
        if (!music) return;
        var state = music.playbackState;
        if (state === MusicKit.PlaybackStates.playing) {
            log('[MusicKit] togglePlayback: pausing');
            await music.pause();
        } else if (state === MusicKit.PlaybackStates.paused) {
            log('[MusicKit] togglePlayback: resuming');
            await music.play();
        } else {
            log('[MusicKit] togglePlayback: state=' + state + ', no action');
        }
    } catch (err) {
        log('[MusicKit] Toggle error: ' + (err.message || String(err)));
        msg('error', 'Toggle error: ' + (err.message || String(err)));
    }
}

async function stopPlayback() {
    try {
        if (music) await music.stop();
    } catch (err) {
        log('[MusicKit] Stop error: ' + (err.message || String(err)));
        msg('error', 'Stop error: ' + (err.message || String(err)));
    }
}

async function seekTo(position) {
    try {
        if (music) await music.seekToTime(position);
    } catch (err) {
        log('[MusicKit] Seek error: ' + (err.message || String(err)));
        msg('error', 'Seek error: ' + (err.message || String(err)));
    }
}

function setVolume(vol) {
    try {
        if (music) music.volume = Math.max(0, Math.min(1, vol));
    } catch (err) {
        log('[MusicKit] Volume error: ' + (err.message || String(err)));
        msg('error', 'Volume error: ' + (err.message || String(err)));
    }
}

function setPlaybackBitrate(quality) {
    if (!music) return JSON.stringify({ success: false, error: 'not initialized' });
    music.bitrate = (quality === 'high')
        ? MusicKit.PlaybackBitrate.HIGH
        : MusicKit.PlaybackBitrate.STANDARD;
    log('[MusicKit] Bitrate set to: ' + quality);
    return JSON.stringify({ success: true, bitrate: quality });
}
</script>
</body>
</html>
)HTML").arg(devToken, QCoreApplication::applicationVersion(), userToken);

    return html;
}
