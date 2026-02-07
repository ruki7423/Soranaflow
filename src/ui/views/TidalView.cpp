#include "TidalView.h"
#include "../../tidal/TidalManager.h"
#include "../../core/ThemeManager.h"
#include "../../widgets/StyledInput.h"
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledScrollArea.h"

#include <QLineEdit>
#include <QPushButton>
#include <QJsonObject>
#include <QDebug>
#include <QNetworkReply>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QEvent>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineHistory>
#include <QCoreApplication>

// ── Custom page class for JS console logging ────────────────────────
class TidalWebEnginePage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                   const QString &message,
                                   int lineNumber,
                                   const QString &sourceID) override {
        Q_UNUSED(level); Q_UNUSED(lineNumber); Q_UNUSED(sourceID);
        qDebug() << "[TidalJS]" << message;
    }
};

// ── Constructor ─────────────────────────────────────────────────────
TidalView::TidalView(QWidget* parent)
    : QWidget(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("TidalView"));
    setupUI();

    auto* tm = TidalManager::instance();

    // === API SIGNALS DISABLED — openapi.tidal.com returning 404 (2025-02) ===
    // Uncomment when Tidal restores API endpoints
    // connect(tm, &TidalManager::searchResultsReady,
    //         this, &TidalView::onSearchResults);
    // connect(tm, &TidalManager::artistTopTracksReady,
    //         this, &TidalView::onArtistTopTracks);
    // connect(tm, &TidalManager::artistAlbumsReady,
    //         this, &TidalView::onArtistAlbums);
    // connect(tm, &TidalManager::albumTracksReady,
    //         this, &TidalView::onAlbumTracks);
    // connect(tm, &TidalManager::networkError,
    //         this, &TidalView::onError);

    // API authentication (for search) — kept for future API restoration
    connect(tm, &TidalManager::authenticated,
            this, [this]() { updateAuthStatus(); });
    connect(tm, &TidalManager::authError,
            this, [this](const QString& error) {
        qDebug() << "[TidalView] Auth error:" << error;
        updateAuthStatus();
    });

    // User login (for full playback - future)
    connect(tm, &TidalManager::userLoggedIn,
            this, [this](const QString&) { updateAuthStatus(); });
    connect(tm, &TidalManager::userLoggedOut,
            this, [this]() { updateAuthStatus(); });

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &TidalView::refreshTheme);

    // Auto-authenticate on construction (kept for future API restoration)
    tm->authenticate();

    // Initial auth status update (after setupUI)
    QTimer::singleShot(0, this, [this]() { updateAuthStatus(); });

    // Initialize hidden preview WebView
    initPreviewWebView();
}

// ── initPreviewWebView — hidden WebView for Tidal SDK playback ──────
void TidalView::initPreviewWebView()
{
    qDebug() << "[TidalView] Initializing preview WebView...";

    // Point QtWebEngine to the bundled helper process
    QString helperPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/../Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/"
                         "QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess");
    qputenv("QTWEBENGINEPROCESS_PATH", helperPath.toUtf8());

    // Create hidden WebView - must have reasonable size for embed to load
    auto* profile = QWebEngineProfile::defaultProfile();
    auto* page = new QWebEnginePage(profile, this);
    m_previewWebView = new QWebEngineView();
    m_previewWebView->setPage(page);
    // Size needs to be large enough for Tidal embed to load properly
    m_previewWebView->setFixedSize(400, 165);
    m_previewWebView->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_previewWebView->show();

    // Enable required settings
    auto* settings = page->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);

    // Auto-grant audio permissions
    connect(page, &QWebEnginePage::featurePermissionRequested,
            this, [page](const QUrl& securityOrigin, QWebEnginePage::Feature feature) {
        if (feature == QWebEnginePage::MediaAudioCapture ||
            feature == QWebEnginePage::MediaAudioVideoCapture) {
            page->setFeaturePermission(securityOrigin, feature,
                                       QWebEnginePage::PermissionGrantedByUser);
            qDebug() << "[TidalView] Auto-granted audio permission";
        }
    });

    // Monitor load completion
    connect(m_previewWebView, &QWebEngineView::loadFinished,
            this, [this](bool ok) {
        qDebug() << "[TidalView] Preview page loaded:" << ok;
        if (ok) {
            // Check if SDK is ready after a brief delay for JS to initialize
            QTimer::singleShot(500, this, [this]() {
                m_previewWebView->page()->runJavaScript(
                    QStringLiteral("window.tidalReady === true"),
                    [this](const QVariant& result) {
                        m_previewSdkReady = result.toBool();
                        qDebug() << "[TidalView] Preview player ready:" << m_previewSdkReady;
                    });
            });
        }
    });

    // Embedded HTML with Tidal embed player
    // Uses Tidal's official embed widget which provides 30-second previews
    // The embed must be visible (not display:none) for playback to work
    const QString htmlContent = QStringLiteral(R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
html, body { margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; background: transparent; }
#embedContainer {
    position: fixed;
    top: 0;
    left: 0;
    width: 400px;
    height: 165px;
    overflow: hidden;
}
#embedContainer iframe {
    border: none;
    width: 100%;
    height: 100%;
}
</style>
</head>
<body>
<div id="embedContainer"></div>
<script>
var currentTrackId = null;
var embedContainer = document.getElementById('embedContainer');
var currentIframe = null;
var playbackStarted = false;

window.tidalPlay = function(trackId) {
    console.log('[Tidal Preview] Play request:', trackId);

    // Stop any existing playback
    window.tidalStop();

    // Create new embed iframe
    // Tidal embed format: https://embed.tidal.com/tracks/{id}
    // layout=gridify for compact view, autoplay for automatic playback
    currentIframe = document.createElement('iframe');
    currentIframe.src = 'https://embed.tidal.com/tracks/' + trackId + '?layout=gridify&disableAnalytics=true';
    currentIframe.allow = 'autoplay *; encrypted-media *;';
    currentIframe.allowFullscreen = false;

    embedContainer.innerHTML = '';
    embedContainer.appendChild(currentIframe);
    currentTrackId = trackId;
    playbackStarted = true;

    console.log('[Tidal Preview] Embed created for:', trackId);

    // The Tidal embed should auto-play when loaded
    // It provides 30-second previews for non-authenticated users
    return true;
};

window.tidalStop = function() {
    console.log('[Tidal Preview] Stop');
    if (currentIframe) {
        // Remove iframe to stop playback
        currentIframe.src = 'about:blank';
        embedContainer.removeChild(currentIframe);
        currentIframe = null;
    }
    embedContainer.innerHTML = '';
    currentTrackId = null;
    playbackStarted = false;
    return true;
};

window.tidalIsPlaying = function() {
    return playbackStarted && currentTrackId !== null;
};

window.tidalCurrentTrack = function() {
    return currentTrackId;
};

// Report ready state
window.tidalReady = true;
console.log('[Tidal Preview] Embed player initialized and ready');
</script>
</body>
</html>
)HTML");

    m_previewWebView->setHtml(htmlContent, QUrl(QStringLiteral("https://sorana.local/")));
    qDebug() << "[TidalView] Preview WebView initialized";
}

// ── setupUI ─────────────────────────────────────────────────────────
void TidalView::setupUI()
{
    auto c = ThemeManager::instance()->colors();

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    // ── Header row ──────────────────────────────────────────────────
    {
        const int NAV_SIZE = 30;

        auto* headerRow = new QHBoxLayout();
        headerRow->setSpacing(8);

        // Navigation ← →
        m_backBtn = new QPushButton(this);
        m_backBtn->setIcon(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/chevron-left.svg")));
        m_backBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
        m_backBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
        m_backBtn->setCursor(Qt::PointingHandCursor);
        m_backBtn->setToolTip(QStringLiteral("Back"));
        m_backBtn->setFocusPolicy(Qt::NoFocus);
        headerRow->addWidget(m_backBtn);

        m_forwardBtn = new QPushButton(this);
        m_forwardBtn->setIcon(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/chevron-right.svg")));
        m_forwardBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
        m_forwardBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
        m_forwardBtn->setCursor(Qt::PointingHandCursor);
        m_forwardBtn->setToolTip(QStringLiteral("Forward"));
        m_forwardBtn->setFocusPolicy(Qt::NoFocus);
        headerRow->addWidget(m_forwardBtn);

        connect(m_backBtn, &QPushButton::clicked, this, &TidalView::navigateBack);
        connect(m_forwardBtn, &QPushButton::clicked, this, &TidalView::navigateForward);

        headerRow->addSpacing(4);

        m_titleLabel = new QLabel(QStringLiteral("Tidal"), this);
        QFont titleFont = m_titleLabel->font();
        titleFont.setPixelSize(24);
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);
        m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foreground));
        headerRow->addWidget(m_titleLabel);

        // Auth status label (same pattern as Apple Music)
        m_authStatusLabel = new QLabel(this);
        m_authStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
        headerRow->addWidget(m_authStatusLabel);

        headerRow->addStretch();

        // Connect button (same style as Apple Music)
        m_connectBtn = new StyledButton(QStringLiteral("Connect"), QStringLiteral("primary"), this);
        m_connectBtn->setObjectName(QStringLiteral("tidalConnectBtn"));
        m_connectBtn->setFixedSize(120, 30);
        connect(m_connectBtn, &QPushButton::clicked, this, []() {
            TidalManager::instance()->loginWithBrowser();
        });
        headerRow->addWidget(m_connectBtn);

        mainLayout->addLayout(headerRow);

        // Navigation bar button style
        QString navBtnStyle = QStringLiteral(
            "QPushButton { background: %1; border: none; border-radius: 6px; }"
            "QPushButton:hover { background: %2; }"
            "QPushButton:disabled { opacity: 0.4; }"
        ).arg(c.backgroundSecondary, c.backgroundTertiary);
        m_backBtn->setStyleSheet(navBtnStyle);
        m_forwardBtn->setStyleSheet(navBtnStyle);
    }

    // ── Search bar (same layout as Apple Music) ─────────────────────
    {
        auto* searchRow = new QHBoxLayout();
        searchRow->setSpacing(8);

        m_searchInput = new StyledInput(
            QStringLiteral("Search songs, albums, artists..."),
            QStringLiteral(":/icons/search.svg"), this);
        searchRow->addWidget(m_searchInput, 1);  // stretch factor 1

        m_searchBtn = new StyledButton(QStringLiteral("Search"),
                                        QStringLiteral("primary"), this);
        m_searchBtn->setObjectName(QStringLiteral("tidalSearchBtn"));
        m_searchBtn->setFixedSize(100, 30);
        connect(m_searchBtn, &StyledButton::clicked, this, &TidalView::onSearch);
        connect(m_searchInput->lineEdit(), &QLineEdit::returnPressed, this, &TidalView::onSearch);
        searchRow->addWidget(m_searchBtn);

        mainLayout->addLayout(searchRow);
    }

    // ── Loading / No results ────────────────────────────────────────
    m_loadingLabel = new QLabel(QStringLiteral("Searching..."), this);
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    m_loadingLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));
    m_loadingLabel->hide();
    mainLayout->addWidget(m_loadingLabel);

    m_noResultsLabel = new QLabel(QStringLiteral("No results found"), this);
    m_noResultsLabel->setAlignment(Qt::AlignCenter);
    m_noResultsLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));
    m_noResultsLabel->hide();
    mainLayout->addWidget(m_noResultsLabel);

    // ── Browse WebView (listen.tidal.com) ──────────────────────────
    // Create persistent profile for cookies
    m_browseProfile = new QWebEngineProfile(QStringLiteral("TidalBrowse"), this);
    m_browseProfile->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);

    auto* browsePage = new TidalWebEnginePage(m_browseProfile, this);
    m_browseWebView = new QWebEngineView(this);
    m_browseWebView->setPage(browsePage);

    // Enable required settings for Tidal web player
    auto* settings = browsePage->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, true);

    // Auto-grant all media permissions for Tidal web player
    connect(browsePage, &QWebEnginePage::featurePermissionRequested,
            this, [browsePage](const QUrl& securityOrigin, QWebEnginePage::Feature feature) {
        qDebug() << "[TidalView] Permission requested:" << feature
                 << "from" << securityOrigin.host();
        // Grant all media-related permissions for Tidal
        if (feature == QWebEnginePage::MediaAudioCapture ||
            feature == QWebEnginePage::MediaAudioVideoCapture ||
            feature == QWebEnginePage::MediaVideoCapture ||
            feature == QWebEnginePage::DesktopVideoCapture ||
            feature == QWebEnginePage::DesktopAudioVideoCapture ||
            feature == QWebEnginePage::Notifications) {
            browsePage->setFeaturePermission(securityOrigin, feature,
                                             QWebEnginePage::PermissionGrantedByUser);
            qDebug() << "[TidalView] Granted permission:" << feature;
        }
    });

    // Load Tidal web player
    m_browseWebView->setUrl(QUrl(QStringLiteral("https://listen.tidal.com/")));
    qDebug() << "[TidalView] Browse WebView loading listen.tidal.com";

    // Update nav buttons when page loads + logging
    connect(m_browseWebView, &QWebEngineView::loadFinished,
            this, [this](bool ok) {
        qDebug() << "[TidalView] Browse page loaded:" << ok
                 << "url:" << m_browseWebView->url().toString();
        updateNavBar();

        // DRM diagnostic — check if Widevine is available
        if (ok) {
            m_browseWebView->page()->runJavaScript(R"(
                (async function() {
                    try {
                        const config = [{
                            initDataTypes: ['cenc'],
                            videoCapabilities: [{
                                contentType: 'video/mp4; codecs="avc1.42E01E"',
                                robustness: 'SW_SECURE_DECODE'
                            }],
                            audioCapabilities: [{
                                contentType: 'audio/mp4; codecs="mp4a.40.2"',
                                robustness: 'SW_SECURE_CRYPTO'
                            }]
                        }];
                        const access = await navigator.requestMediaKeySystemAccess('com.widevine.alpha', config);
                        console.log('[DRM-DIAG] Widevine available: ' + access.keySystem);
                    } catch(e) {
                        console.log('[DRM-DIAG] Widevine NOT available: ' + e.message);
                    }
                })();
            )", [](const QVariant&){});

            // Monitor all media elements for playback events
            m_browseWebView->page()->runJavaScript(R"(
                (function() {
                    // Monitor existing and future audio/video elements
                    const observer = new MutationObserver(function(mutations) {
                        document.querySelectorAll('audio, video').forEach(function(el) {
                            if (el._monitored) return;
                            el._monitored = true;
                            console.log('[TIDAL-MEDIA] Found media element: ' + el.tagName + ' src=' + (el.src || el.currentSrc || 'none'));

                            ['play', 'playing', 'pause', 'error', 'stalled', 'waiting',
                             'canplay', 'loadeddata', 'ended', 'volumechange', 'emptied'].forEach(function(evt) {
                                el.addEventListener(evt, function(e) {
                                    var info = el.tagName + ' ' + evt;
                                    if (evt === 'error' && el.error) {
                                        info += ' code=' + el.error.code + ' msg=' + el.error.message;
                                    }
                                    if (evt === 'volumechange') {
                                        info += ' vol=' + el.volume + ' muted=' + el.muted;
                                    }
                                    if (evt === 'playing' || evt === 'play') {
                                        info += ' duration=' + el.duration + ' currentTime=' + el.currentTime;
                                    }
                                    console.log('[TIDAL-MEDIA] ' + info);
                                });
                            });
                        });
                    });
                    observer.observe(document.body, { childList: true, subtree: true });

                    // Also check immediately
                    document.querySelectorAll('audio, video').forEach(function(el) {
                        if (!el._monitored) {
                            el._monitored = true;
                            console.log('[TIDAL-MEDIA] Initial media element: ' + el.tagName);
                        }
                    });

                    // Monitor EME license requests
                    if (navigator.requestMediaKeySystemAccess) {
                        const orig = navigator.requestMediaKeySystemAccess.bind(navigator);
                        navigator.requestMediaKeySystemAccess = function(keySystem, configs) {
                            console.log('[TIDAL-DRM] requestMediaKeySystemAccess: ' + keySystem);
                            return orig(keySystem, configs).then(function(access) {
                                console.log('[TIDAL-DRM] MediaKeySystemAccess granted: ' + access.keySystem);
                                return access;
                            }).catch(function(err) {
                                console.log('[TIDAL-DRM] MediaKeySystemAccess DENIED: ' + err.message);
                                throw err;
                            });
                        };
                    }

                    console.log('[TIDAL-MEDIA] Monitoring active');
                })();
            )", [](const QVariant&){});
        }
    });

    mainLayout->addWidget(m_browseWebView, 1);

    // ── Results scroll area (hidden — kept for API restoration) ─────
    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->hide();  // Hidden while using WebView browse

    m_resultsContainer = new QWidget(m_scrollArea);
    m_resultsLayout = new QVBoxLayout(m_resultsContainer);
    m_resultsLayout->setContentsMargins(0, 0, 0, 0);
    m_resultsLayout->setSpacing(24);
    m_resultsLayout->addStretch();

    m_scrollArea->setWidget(m_resultsContainer);
    // DSP note removed — ProcessTap now routes Tidal through DSP pipeline

    updateNavBar();
}

// ── Search ──────────────────────────────────────────────────────────
void TidalView::onSearch()
{
    QString term = m_searchInput->lineEdit()->text().trimmed();
    if (term.isEmpty()) return;

    // Navigate WebView to Tidal search URL
    // API is down (openapi.tidal.com returning 404, 2025-02), using WebView instead
    QString searchUrl = TidalManager::getSearchUrl(term);
    qDebug() << "[TidalView] Navigating to:" << searchUrl;

    if (m_browseWebView) {
        m_browseWebView->setUrl(QUrl(searchUrl));
    }

    m_lastSearchTerm = term;
    updateNavBar();
}

void TidalView::onSearchResults(const QJsonObject& results)
{
    m_loadingLabel->hide();
    clearResults();

    // v1 API format: direct arrays under tracks/albums/artists keys
    QJsonArray tracks = results[QStringLiteral("tracks")].toArray();
    QJsonArray albums = results[QStringLiteral("albums")].toArray();
    QJsonArray artists = results[QStringLiteral("artists")].toArray();

    m_lastTracks = tracks;
    m_lastAlbums = albums;
    m_lastArtists = artists;

    qDebug() << "[TidalView] Search results — tracks:" << tracks.size()
             << "albums:" << albums.size()
             << "artists:" << artists.size();

    if (tracks.isEmpty() && albums.isEmpty() && artists.isEmpty()) {
        m_noResultsLabel->show();
        return;
    }

    // Build sections
    if (!tracks.isEmpty()) buildTracksSection(tracks);
    if (!albums.isEmpty()) buildAlbumsSection(albums);
    if (!artists.isEmpty()) buildArtistsSection(artists);
}

// ── Artist / Album Detail ───────────────────────────────────────────
void TidalView::onArtistTopTracks(const QString& artistId, const QJsonArray& tracks)
{
    Q_UNUSED(artistId)
    m_loadingLabel->hide();

    if (tracks.isEmpty()) {
        m_noResultsLabel->setText(QStringLiteral("No tracks found"));
        m_noResultsLabel->show();
        return;
    }

    buildTracksSection(tracks);
}

void TidalView::onArtistAlbums(const QString& artistId, const QJsonArray& albums)
{
    Q_UNUSED(artistId)

    if (!albums.isEmpty()) {
        buildAlbumsSection(albums);
    }
}

void TidalView::onAlbumTracks(const QString& albumId, const QJsonArray& tracks)
{
    Q_UNUSED(albumId)
    m_loadingLabel->hide();

    if (tracks.isEmpty()) {
        m_noResultsLabel->setText(QStringLiteral("No tracks found"));
        m_noResultsLabel->show();
        return;
    }

    buildTracksSection(tracks);
}

void TidalView::onError(const QString& error)
{
    m_loadingLabel->hide();
    m_noResultsLabel->setText(QStringLiteral("Error: %1").arg(error));
    m_noResultsLabel->show();
    qDebug() << "[TidalView] Error:" << error;
}

// ── UI Building ─────────────────────────────────────────────────────
void TidalView::clearResults()
{
    QLayoutItem* child;
    while ((child = m_resultsLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = child->widget()) {
            w->deleteLater();
        }
        delete child;
    }
    m_resultsLayout->addStretch();
}

QWidget* TidalView::createSectionHeader(const QString& title)
{
    auto c = ThemeManager::instance()->colors();
    auto* header = new QLabel(title, m_resultsContainer);
    header->setStyleSheet(
        QStringLiteral("color: %1; font-size: 18px; font-weight: bold; padding: 8px 0;")
            .arg(c.foreground));
    return header;
}

void TidalView::buildTracksSection(const QJsonArray& tracks)
{
    // Insert before the stretch
    int insertIdx = m_resultsLayout->count() - 1;

    m_resultsLayout->insertWidget(insertIdx++, createSectionHeader(QStringLiteral("Tracks")));

    for (const QJsonValue& val : tracks) {
        QJsonObject track = val.toObject();
        auto* row = createTrackRow(track);
        m_resultsLayout->insertWidget(insertIdx++, row);
    }
}

void TidalView::buildAlbumsSection(const QJsonArray& albums)
{
    int insertIdx = m_resultsLayout->count() - 1;

    m_resultsLayout->insertWidget(insertIdx++, createSectionHeader(QStringLiteral("Albums")));

    // Grid of album cards
    auto* gridWidget = new QWidget(m_resultsContainer);
    auto* grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(16);

    const int CARD_WIDTH = 180;
    const int COLS = 5;

    for (int i = 0; i < albums.size(); ++i) {
        QJsonObject album = albums[i].toObject();
        auto* card = createAlbumCard(album, CARD_WIDTH);
        grid->addWidget(card, i / COLS, i % COLS);
    }

    m_resultsLayout->insertWidget(insertIdx, gridWidget);
}

void TidalView::buildArtistsSection(const QJsonArray& artists)
{
    int insertIdx = m_resultsLayout->count() - 1;

    m_resultsLayout->insertWidget(insertIdx++, createSectionHeader(QStringLiteral("Artists")));

    // Grid of artist cards
    auto* gridWidget = new QWidget(m_resultsContainer);
    auto* grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(16);

    const int CARD_WIDTH = 160;
    const int COLS = 6;

    for (int i = 0; i < artists.size(); ++i) {
        QJsonObject artist = artists[i].toObject();
        auto* card = createArtistCard(artist, CARD_WIDTH);
        grid->addWidget(card, i / COLS, i % COLS);
    }

    m_resultsLayout->insertWidget(insertIdx, gridWidget);
}

QWidget* TidalView::createTrackRow(const QJsonObject& track)
{
    auto c = ThemeManager::instance()->colors();

    // v1 API format: direct fields at root level
    // id can be int or string, title at root, artists array, album object, duration int
    QString id = track.contains(QStringLiteral("id"))
        ? (track[QStringLiteral("id")].isDouble()
            ? QString::number(track[QStringLiteral("id")].toInteger())
            : track[QStringLiteral("id")].toString())
        : QString();
    QString title = track[QStringLiteral("title")].toString();
    int duration = track[QStringLiteral("duration")].toInt();

    // Artist name from artists array
    QString artist;
    QJsonArray artists = track[QStringLiteral("artists")].toArray();
    if (!artists.isEmpty()) {
        artist = artists[0].toObject()[QStringLiteral("name")].toString();
    }

    auto* row = new QWidget(m_resultsContainer);
    row->setObjectName(QStringLiteral("TidalTrackRow"));
    row->setCursor(Qt::PointingHandCursor);
    row->setProperty("trackData", QVariant::fromValue(track));
    row->installEventFilter(this);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(12);

    // Artwork placeholder
    auto* artwork = new QLabel(row);
    artwork->setFixedSize(48, 48);
    artwork->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(c.backgroundTertiary));
    artwork->setAlignment(Qt::AlignCenter);
    layout->addWidget(artwork);

    // Load artwork from album.cover (v1 format)
    QJsonObject albumObj = track[QStringLiteral("album")].toObject();
    QString coverImg = albumObj[QStringLiteral("cover")].toString();
    if (!coverImg.isEmpty()) {
        loadArtwork(TidalManager::coverArtUrl(coverImg, 160), artwork, 48);
    }

    // Track info
    auto* infoWidget = new QWidget(row);
    auto* infoLayout = new QVBoxLayout(infoWidget);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(2);

    auto* titleLabel = new QLabel(title, infoWidget);
    titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; font-weight: 500;").arg(c.foreground));

    auto* artistLabel = new QLabel(artist, infoWidget);
    artistLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundSecondary));

    infoLayout->addWidget(titleLabel);
    infoLayout->addWidget(artistLabel);
    layout->addWidget(infoWidget, 1);

    // Duration
    QString durationStr = QStringLiteral("%1:%2")
        .arg(duration / 60)
        .arg(duration % 60, 2, 10, QLatin1Char('0'));
    auto* durationLabel = new QLabel(durationStr, row);
    durationLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    layout->addWidget(durationLabel);

    // Play button
    auto* playBtn = new QPushButton(row);
    playBtn->setIcon(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/play.svg")));
    playBtn->setIconSize(QSize(16, 16));
    playBtn->setFixedSize(32, 32);
    playBtn->setCursor(Qt::PointingHandCursor);
    playBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; border: none; border-radius: 16px; }"
                       "QPushButton:hover { background: %2; }")
        .arg(c.accent, c.accentHover));
    connect(playBtn, &QPushButton::clicked, this, [this, track]() {
        playTrack(track);
    });
    layout->addWidget(playBtn);

    // Row hover style
    row->setStyleSheet(
        QStringLiteral("#TidalTrackRow { background: transparent; border-radius: 8px; }"
                       "#TidalTrackRow:hover { background: %1; }")
        .arg(c.backgroundSecondary));

    return row;
}

QWidget* TidalView::createAlbumCard(const QJsonObject& album, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();

    // v1 API format: id (int), title, artists array, cover string
    QString id = album.contains(QStringLiteral("id"))
        ? (album[QStringLiteral("id")].isDouble()
            ? QString::number(album[QStringLiteral("id")].toInteger())
            : album[QStringLiteral("id")].toString())
        : QString();
    QString title = album[QStringLiteral("title")].toString();

    QString artist;
    QJsonArray artists = album[QStringLiteral("artists")].toArray();
    if (!artists.isEmpty()) {
        artist = artists[0].toObject()[QStringLiteral("name")].toString();
    }

    auto* card = new QWidget(m_resultsContainer);
    card->setFixedWidth(cardWidth);
    card->setCursor(Qt::PointingHandCursor);
    card->setProperty("albumId", id);
    card->setProperty("albumName", title);
    card->setProperty("artistName", artist);
    card->installEventFilter(this);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Artwork
    auto* artwork = new QLabel(card);
    artwork->setFixedSize(cardWidth, cardWidth);
    artwork->setStyleSheet(QStringLiteral("background: %1; border-radius: 8px;").arg(c.backgroundTertiary));
    artwork->setAlignment(Qt::AlignCenter);
    layout->addWidget(artwork);

    // Load artwork from cover (v1 format)
    QString coverImg = album[QStringLiteral("cover")].toString();
    if (!coverImg.isEmpty()) {
        loadArtwork(TidalManager::coverArtUrl(coverImg, 320), artwork, cardWidth);
    }

    // Title (truncate)
    auto* titleLabel = new QLabel(title, card);
    titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 500;").arg(c.foreground));
    titleLabel->setWordWrap(false);
    QFontMetrics fm(titleLabel->font());
    titleLabel->setText(fm.elidedText(title, Qt::ElideRight, cardWidth));
    layout->addWidget(titleLabel);

    // Artist
    auto* artistLabel = new QLabel(artist, card);
    artistLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundSecondary));
    artistLabel->setWordWrap(false);
    artistLabel->setText(fm.elidedText(artist, Qt::ElideRight, cardWidth));
    layout->addWidget(artistLabel);

    return card;
}

QWidget* TidalView::createArtistCard(const QJsonObject& artist, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();

    // v1 API format: id (int), name, picture (string UUID)
    QString id = artist.contains(QStringLiteral("id"))
        ? (artist[QStringLiteral("id")].isDouble()
            ? QString::number(artist[QStringLiteral("id")].toInteger())
            : artist[QStringLiteral("id")].toString())
        : QString();
    QString name = artist[QStringLiteral("name")].toString();

    auto* card = new QWidget(m_resultsContainer);
    card->setFixedWidth(cardWidth);
    card->setCursor(Qt::PointingHandCursor);
    card->setProperty("artistId", id);
    card->setProperty("artistName", name);
    card->installEventFilter(this);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->setAlignment(Qt::AlignHCenter);

    // Circular artwork
    auto* artwork = new QLabel(card);
    artwork->setFixedSize(cardWidth, cardWidth);
    artwork->setStyleSheet(QStringLiteral("background: %1; border-radius: %2px;")
        .arg(c.backgroundTertiary)
        .arg(cardWidth / 2));
    artwork->setAlignment(Qt::AlignCenter);
    layout->addWidget(artwork);

    // Load artwork from picture (v1 format - string UUID)
    QString pictureImg = artist[QStringLiteral("picture")].toString();
    if (!pictureImg.isEmpty()) {
        loadArtwork(TidalManager::coverArtUrl(pictureImg, 320), artwork, cardWidth, true);
    }

    // Name
    auto* nameLabel = new QLabel(name, card);
    nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 500;").arg(c.foreground));
    nameLabel->setAlignment(Qt::AlignCenter);
    QFontMetrics fm(nameLabel->font());
    nameLabel->setText(fm.elidedText(name, Qt::ElideRight, cardWidth));
    layout->addWidget(nameLabel);

    return card;
}

void TidalView::loadArtwork(const QString& url, QLabel* target, int size, bool circular)
{
    if (url.isEmpty()) return;

    QPointer<QLabel> safeTarget = target;
    QNetworkRequest request{QUrl(url)};
    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [reply, safeTarget, size, circular]() {
        reply->deleteLater();
        if (!safeTarget) return;

        if (reply->error() == QNetworkReply::NoError) {
            QPixmap pix;
            pix.loadFromData(reply->readAll());
            if (!pix.isNull()) {
                pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

                if (circular) {
                    QPixmap circularPix(size, size);
                    circularPix.fill(Qt::transparent);
                    QPainter painter(&circularPix);
                    painter.setRenderHint(QPainter::Antialiasing);
                    QPainterPath path;
                    path.addEllipse(0, 0, size, size);
                    painter.setClipPath(path);
                    painter.drawPixmap(0, 0, pix);
                    safeTarget->setPixmap(circularPix);
                } else {
                    // Round corners
                    QPixmap rounded(size, size);
                    rounded.fill(Qt::transparent);
                    QPainter painter(&rounded);
                    painter.setRenderHint(QPainter::Antialiasing);
                    QPainterPath path;
                    path.addRoundedRect(0, 0, size, size, 8, 8);
                    painter.setClipPath(path);
                    painter.drawPixmap(0, 0, pix);
                    safeTarget->setPixmap(rounded);
                }
            }
        }
    });
}

// ── Navigation ──────────────────────────────────────────────────────

// === API NAVIGATION DISABLED — openapi.tidal.com returning 404 (2025-02) ===
// Users browse directly in the WebView instead
// Uncomment when Tidal restores API endpoints

void TidalView::showArtistDetail(const QString& artistId, const QString& artistName)
{
    Q_UNUSED(artistId)
    Q_UNUSED(artistName)
    // API is down — users navigate in the WebView instead
    qDebug() << "[TidalView] showArtistDetail disabled (API down)";
}

void TidalView::showAlbumDetail(const QString& albumId, const QString& albumName, const QString& artistName)
{
    Q_UNUSED(albumId)
    Q_UNUSED(albumName)
    Q_UNUSED(artistName)
    // API is down — users navigate in the WebView instead
    qDebug() << "[TidalView] showAlbumDetail disabled (API down)";
}

#if 0  // === API NAVIGATION DISABLED ===
void TidalView::showArtistDetail_API(const QString& artistId, const QString& artistName)
{
    pushNavState();

    m_currentState = TidalViewState::ArtistDetail;
    m_currentDetailId = artistId;
    m_currentDetailName = artistName;

    clearResults();
    m_loadingLabel->show();
    m_noResultsLabel->hide();

    TidalManager::instance()->getArtistTopTracks(artistId);
    TidalManager::instance()->getArtistAlbums(artistId);

    updateNavBar();
}

void TidalView::showAlbumDetail_API(const QString& albumId, const QString& albumName, const QString& artistName)
{
    pushNavState();

    m_currentState = TidalViewState::AlbumDetail;
    m_currentDetailId = albumId;
    m_currentDetailName = albumName;
    m_currentDetailSubName = artistName;

    clearResults();
    m_loadingLabel->show();
    m_noResultsLabel->hide();

    TidalManager::instance()->getAlbumTracks(albumId);

    updateNavBar();
}
#endif  // === END API NAVIGATION DISABLED ===

void TidalView::pushNavState()
{
    NavEntry entry;
    entry.state = m_currentState;
    entry.searchTerm = m_lastSearchTerm;
    entry.tracks = m_lastTracks;
    entry.albums = m_lastAlbums;
    entry.artists = m_lastArtists;
    entry.detailId = m_currentDetailId;
    entry.detailName = m_currentDetailName;
    entry.detailSubName = m_currentDetailSubName;

    m_backStack.push(entry);
    m_forwardStack.clear();
}

void TidalView::navigateBack()
{
    // Use WebView's native back navigation
    if (m_browseWebView && m_browseWebView->history()->canGoBack()) {
        m_browseWebView->back();
        qDebug() << "[TidalView] WebView navigate back";
    }
}

void TidalView::navigateForward()
{
    // Use WebView's native forward navigation
    if (m_browseWebView && m_browseWebView->history()->canGoForward()) {
        m_browseWebView->forward();
        qDebug() << "[TidalView] WebView navigate forward";
    }
}

void TidalView::restoreNavEntry(const NavEntry& entry)
{
    m_currentState = entry.state;
    m_lastSearchTerm = entry.searchTerm;
    m_lastTracks = entry.tracks;
    m_lastAlbums = entry.albums;
    m_lastArtists = entry.artists;
    m_currentDetailId = entry.detailId;
    m_currentDetailName = entry.detailName;
    m_currentDetailSubName = entry.detailSubName;

    // With WebView browse, just navigate back/forward in WebView
    // The old API-based state restoration is disabled

    if (m_currentState == TidalViewState::Search && !m_lastSearchTerm.isEmpty()) {
        m_searchInput->lineEdit()->setText(m_lastSearchTerm);
        // Navigate WebView to search URL
        if (m_browseWebView) {
            m_browseWebView->setUrl(QUrl(TidalManager::getSearchUrl(m_lastSearchTerm)));
        }
    }

    updateNavBar();
}

void TidalView::updateNavBar()
{
    // Use WebView's history for back/forward button state
    if (m_browseWebView) {
        m_backBtn->setEnabled(m_browseWebView->history()->canGoBack());
        m_forwardBtn->setEnabled(m_browseWebView->history()->canGoForward());
    } else {
        m_backBtn->setEnabled(false);
        m_forwardBtn->setEnabled(false);
    }

    // Title stays "Tidal" — users navigate in WebView
    m_titleLabel->setText(QStringLiteral("Tidal"));
}

// ── Playback ────────────────────────────────────────────────────────
void TidalView::playTrack(const QJsonObject& track)
{
    // v1 API format: id can be int or string, title at root
    QString id = track.contains(QStringLiteral("id"))
        ? (track[QStringLiteral("id")].isDouble()
            ? QString::number(track[QStringLiteral("id")].toInteger())
            : track[QStringLiteral("id")].toString())
        : QString();
    QString title = track[QStringLiteral("title")].toString();

    qDebug() << "[TidalView] Play track:" << id << title;

    // If clicking same track that's playing, toggle stop
    if (m_isPlaying && m_currentPreviewTrackId == id) {
        stopPreview();
        return;
    }

    // Stop any currently playing preview
    if (m_isPlaying) {
        stopPreview();
    }

    if (!m_previewWebView) {
        qDebug() << "[TidalView] WebView not initialized";
        return;
    }

    if (!m_previewSdkReady) {
        qDebug() << "[TidalView] Preview player not ready yet, waiting...";
        // Try again after a short delay
        QTimer::singleShot(500, this, [this, track]() {
            if (m_previewSdkReady) {
                playTrack(track);
            } else {
                qDebug() << "[TidalView] Preview player still not ready";
            }
        });
        return;
    }

    // Use Tidal embed player for 30-second previews
    // The embed auto-plays and provides preview playback for non-authenticated users
    QString js = QStringLiteral("window.tidalPlay('%1');").arg(id);

    qDebug() << "[TidalView] Executing preview JS for track:" << id;
    m_previewWebView->page()->runJavaScript(js, [this, id, title](const QVariant& result) {
        m_currentPreviewTrackId = id;
        m_isPlaying = true;
        qDebug() << "[TidalView] Preview started for:" << title << "result:" << result;
    });
}

void TidalView::stopPreview()
{
    if (!m_previewWebView) return;

    qDebug() << "[TidalView] Stopping preview";
    m_previewWebView->page()->runJavaScript(QStringLiteral("window.tidalStop();"));
    m_currentPreviewTrackId.clear();
    m_isPlaying = false;
}

// ── Event Filter ────────────────────────────────────────────────────
bool TidalView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        QWidget* widget = qobject_cast<QWidget*>(obj);
        if (!widget) return false;

        // Album card click
        QString albumId = widget->property("albumId").toString();
        if (!albumId.isEmpty()) {
            QString albumName = widget->property("albumName").toString();
            QString artistName = widget->property("artistName").toString();
            showAlbumDetail(albumId, albumName, artistName);
            return true;
        }

        // Artist card click
        QString artistId = widget->property("artistId").toString();
        if (!artistId.isEmpty()) {
            QString artistName = widget->property("artistName").toString();
            showArtistDetail(artistId, artistName);
            return true;
        }

        // Track row double-click handling is via play button
    }

    return QWidget::eventFilter(obj, event);
}

// ── Theme ───────────────────────────────────────────────────────────
// ── updateAuthStatus — match Apple Music pattern ────────────────────
void TidalView::updateAuthStatus()
{
    auto* tm = TidalManager::instance();
    auto c = ThemeManager::instance()->colors();

    if (tm->isUserLoggedIn()) {
        // User is logged in
        m_authStatusLabel->setText(QStringLiteral("Connected as %1").arg(tm->username()));
        m_authStatusLabel->setStyleSheet(
            QStringLiteral("color: #4CAF50; font-size: 12px; font-weight: bold;"));
        m_connectBtn->setText(QStringLiteral("Disconnect"));
        m_connectBtn->disconnect();
        connect(m_connectBtn, &QPushButton::clicked, this, []() {
            TidalManager::instance()->logout();
        });
        m_connectBtn->setVisible(true);
    } else if (tm->isAuthenticated()) {
        // API authenticated but user not logged in
        m_authStatusLabel->setText(QStringLiteral("Not connected"));
        m_authStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
        m_connectBtn->setText(QStringLiteral("Connect"));
        m_connectBtn->disconnect();
        connect(m_connectBtn, &QPushButton::clicked, this, []() {
            TidalManager::instance()->loginWithBrowser();
        });
        m_connectBtn->setVisible(true);
    } else {
        // Not authenticated at all
        m_authStatusLabel->setText(QStringLiteral("Connecting..."));
        m_authStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
        m_connectBtn->setVisible(false);
    }
}

void TidalView::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foreground));
    m_loadingLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));
    m_noResultsLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));

    QString navBtnStyle = QStringLiteral(
        "QPushButton { background: %1; border: none; border-radius: 6px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:disabled { opacity: 0.4; }"
    ).arg(c.backgroundSecondary, c.backgroundTertiary);
    m_backBtn->setStyleSheet(navBtnStyle);
    m_forwardBtn->setStyleSheet(navBtnStyle);

    m_backBtn->setIcon(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_forwardBtn->setIcon(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/chevron-right.svg")));

    updateAuthStatus();
}
