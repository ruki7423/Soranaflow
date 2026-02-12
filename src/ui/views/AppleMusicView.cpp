#include "AppleMusicView.h"
#include "../../apple/AppleMusicManager.h"
#include "../../apple/MusicKitPlayer.h"
#include "../../core/PlaybackState.h"
#include "../../core/ThemeManager.h"
#include "../../widgets/StyledInput.h"
#include "../../widgets/StyledButton.h"
#ifdef Q_OS_MACOS
#include "../../platform/macos/AudioProcessTap.h"
#include "../../core/audio/AudioEngine.h"
#endif

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
#include <QMenu>
#include <QContextMenuEvent>
#include "../MainWindow.h"
#include "../../core/library/PlaylistManager.h"
#include "../../ui/dialogs/NewPlaylistDialog.h"

// ── Constructor ─────────────────────────────────────────────────────
AppleMusicView::AppleMusicView(QWidget* parent)
    : QWidget(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("AppleMusicView"));
    setupUI();

    auto* am = AppleMusicManager::instance();
    connect(am, &AppleMusicManager::searchResultsReady,
            this, &AppleMusicView::onSearchResults);
    connect(am, &AppleMusicManager::artistSongsReady,
            this, &AppleMusicView::onArtistSongs);
    connect(am, &AppleMusicManager::artistAlbumsReady,
            this, &AppleMusicView::onArtistAlbums);
    connect(am, &AppleMusicManager::albumTracksReady,
            this, &AppleMusicView::onAlbumTracks);
    connect(am, &AppleMusicManager::errorOccurred,
            this, &AppleMusicView::onError);
    connect(am, &AppleMusicManager::authorizationStatusChanged,
            this, [this](AppleMusicManager::AuthStatus status) {
                updateAuthStatus();
                // On disconnect, clear cached token so reconnect uses fresh one
                if (status == AppleMusicManager::NotDetermined) {
                    qDebug() << "[AppleMusicView] Auth revoked — clearing cached Music User Token";
                    m_musicUserToken.clear();
                }
            });

    updateAuthStatus();

    // ── Music User Token flow ────────────────────────────────────────
    auto* player = MusicKitPlayer::instance();

    // Token obtained from native MusicKit -> inject into JS
    connect(am, &AppleMusicManager::musicUserTokenReady,
        this, [this, player](const QString& token) {
            qDebug() << "[AppleMusicView] Music User Token received, length:" << token.length();
            m_musicUserToken = token;
            player->injectMusicUserToken(token);
        });

    // Token request failed -> continue with previews
    connect(am, &AppleMusicManager::musicUserTokenFailed,
        this, [](const QString& error) {
            qDebug() << "[AppleMusicView] Music User Token FAILED:" << error;
            qDebug() << "[AppleMusicView] Continuing with 30-second previews";
        });

    // MusicKit JS ready -> inject cached token if available
    connect(player, &MusicKitPlayer::musicKitReady,
        this, [this, player]() {
            qDebug() << "[AppleMusicView] MusicKit JS is ready";
            if (!m_musicUserToken.isEmpty()) {
                qDebug() << "[AppleMusicView] Injecting cached Music User Token";
                player->injectMusicUserToken(m_musicUserToken);
            } else {
                qDebug() << "[AppleMusicView] No cached token yet, will inject when available";
            }
#ifdef Q_OS_MACOS
            // Pre-create ProcessTap for faster start when user plays
            auto* tap = AudioProcessTap::instance();
            if (tap->isSupported() && !tap->isPrepared() && !tap->isActive()) {
                tap->setDSPPipeline(AudioEngine::instance()->dspPipeline());
                tap->prepareForPlayback();
            }
#endif
        });

    // Full playback confirmed — also refresh auth status label
    connect(player, &MusicKitPlayer::fullPlaybackAvailable,
        this, [this]() {
            qDebug() << "[AppleMusicView] Full playback mode confirmed!";
            updateAuthStatus();
        });

    // Preview only (no subscription)
    connect(player, &MusicKitPlayer::previewOnlyMode,
        this, []() {
            qDebug() << "[AppleMusicView] Preview only mode (check Apple Music subscription)";
        });

    // Token expired -> re-request from native
    connect(player, &MusicKitPlayer::tokenExpired,
        this, [this, am]() {
            qDebug() << "[AppleMusicView] Token expired, re-requesting...";
            m_musicUserToken.clear();
            am->requestMusicUserToken();
        });

    // Do NOT request Music User Token at startup — wait for user to Connect
    qDebug() << "[AppleMusicView] Waiting for manual Connect (no auto-token request)";

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &AppleMusicView::refreshTheme);
}

// ── setupUI ─────────────────────────────────────────────────────────
void AppleMusicView::setupUI()
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

        // ── Navigation ← → (left side) ──────────────────────────────
        m_backBtn = new QPushButton(this);
        m_backBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
        m_backBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
        m_backBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
        m_backBtn->setCursor(Qt::PointingHandCursor);
        m_backBtn->setToolTip(QStringLiteral("Back"));
        m_backBtn->setFocusPolicy(Qt::NoFocus);
        headerRow->addWidget(m_backBtn);

        m_forwardBtn = new QPushButton(this);
        m_forwardBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));
        m_forwardBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
        m_forwardBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
        m_forwardBtn->setCursor(Qt::PointingHandCursor);
        m_forwardBtn->setToolTip(QStringLiteral("Forward"));
        m_forwardBtn->setFocusPolicy(Qt::NoFocus);
        headerRow->addWidget(m_forwardBtn);

        headerRow->addSpacing(4);

        m_titleLabel = new QLabel(QStringLiteral("Apple Music"), this);
        QFont titleFont = m_titleLabel->font();
        titleFont.setPixelSize(24);
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);
        m_titleLabel->setStyleSheet(
            QStringLiteral("color: %1;").arg(c.foreground));
        headerRow->addWidget(m_titleLabel);

        m_authStatusLabel = new QLabel(this);
        m_authStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
        headerRow->addWidget(m_authStatusLabel);

        headerRow->addStretch();

        m_connectBtn = new StyledButton(QStringLiteral("Connect"), QStringLiteral("primary"), this);
        m_connectBtn->setObjectName(QStringLiteral("amConnectBtn"));
        m_connectBtn->setFixedSize(120, 30);
        connect(m_connectBtn, &QPushButton::clicked, this, []() {
            AppleMusicManager::instance()->requestAuthorization();
        });
        headerRow->addWidget(m_connectBtn);

        // Back: internal nav first, then global
        connect(m_backBtn, &QPushButton::clicked, this, [this]() {
            if (!m_backStack.isEmpty()) {
                navigateBack();
            } else if (auto* mw = MainWindow::instance()) {
                mw->navigateBack();
            }
        });
        // Forward: internal nav first, then global
        connect(m_forwardBtn, &QPushButton::clicked, this, [this]() {
            if (!m_forwardStack.isEmpty()) {
                navigateForward();
            } else if (auto* mw = MainWindow::instance()) {
                mw->navigateForward();
            }
        });

        if (auto* mw = MainWindow::instance()) {
            connect(mw, &MainWindow::globalNavChanged, this, [this]() { updateNavBar(); });
        }

        mainLayout->addLayout(headerRow);
    }

    // ── Search row ──────────────────────────────────────────────────
    {
        auto* searchRow = new QHBoxLayout();
        searchRow->setSpacing(8);

        m_searchInput = new StyledInput(
            QStringLiteral("Search songs, albums, artists..."),
            QStringLiteral(":/icons/search.svg"), this);
        searchRow->addWidget(m_searchInput, 1);

        m_searchBtn = new StyledButton(QStringLiteral("Search"),
                                        QStringLiteral("primary"), this);
        m_searchBtn->setObjectName(QStringLiteral("amSearchBtn"));
        m_searchBtn->setFixedSize(100, 30);
        searchRow->addWidget(m_searchBtn);

        connect(m_searchBtn, &QPushButton::clicked, this, &AppleMusicView::onSearch);
        connect(m_searchInput->lineEdit(), &QLineEdit::returnPressed,
                this, &AppleMusicView::onSearch);

        mainLayout->addLayout(searchRow);
    }

    // ── Context title (shows current sub-view info) ─────────────────
    {
        m_navBar = new QWidget(this);
        m_navBar->setFixedHeight(28);
        auto* navLayout = new QHBoxLayout(m_navBar);
        navLayout->setContentsMargins(0, 0, 0, 0);
        navLayout->setSpacing(0);

        m_navTitleLabel = new QLabel(m_navBar);
        m_navTitleLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundSecondary));
        navLayout->addWidget(m_navTitleLabel, 1);

        m_navBar->setVisible(false);
        mainLayout->addWidget(m_navBar);
    }

    // ── Loading indicator ───────────────────────────────────────────
    m_loadingLabel = new QLabel(QStringLiteral("Searching..."), this);
    m_loadingLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;").arg(c.foregroundMuted));
    m_loadingLabel->setVisible(false);
    mainLayout->addWidget(m_loadingLabel);

    // ── No results label ────────────────────────────────────────────
    m_noResultsLabel = new QLabel(QStringLiteral("No results found"), this);
    m_noResultsLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));
    m_noResultsLabel->setAlignment(Qt::AlignCenter);
    m_noResultsLabel->setVisible(false);
    mainLayout->addWidget(m_noResultsLabel);

    // ── Results scroll area ─────────────────────────────────────────
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }") +
        ThemeManager::instance()->scrollbarStyle());

    m_resultsContainer = new QWidget(m_scrollArea);
    m_resultsContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_resultsContainer->setFocusPolicy(Qt::NoFocus);
    m_resultsContainer->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_resultsLayout = new QVBoxLayout(m_resultsContainer);
    m_resultsLayout->setContentsMargins(0, 0, 0, 0);
    m_resultsLayout->setSpacing(16);
    m_resultsLayout->addStretch();

    m_scrollArea->setWidget(m_resultsContainer);
    mainLayout->addWidget(m_scrollArea, 1);
    // DSP note removed — ProcessTap now routes Apple Music through DSP pipeline
}

// ═════════════════════════════════════════════════════════════════════
//  Navigation — back/forward history
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::pushNavState()
{
    NavEntry entry;
    entry.state = m_currentState;
    entry.searchTerm = m_lastSearchTerm;
    entry.songs = m_lastSongs;
    entry.albums = m_lastAlbums;
    entry.artists = m_lastArtists;
    entry.detailId = m_currentDetailId;
    entry.detailName = m_currentDetailName;
    entry.detailSubName = m_currentDetailSubName;
    m_backStack.push(entry);
    m_forwardStack.clear();  // new action invalidates forward
    updateNavBar();
}

void AppleMusicView::navigateBack()
{
    if (m_backStack.isEmpty()) return;

    // Save current state to forward stack
    NavEntry fwd;
    fwd.state = m_currentState;
    fwd.searchTerm = m_lastSearchTerm;
    fwd.songs = m_lastSongs;
    fwd.albums = m_lastAlbums;
    fwd.artists = m_lastArtists;
    fwd.detailId = m_currentDetailId;
    fwd.detailName = m_currentDetailName;
    fwd.detailSubName = m_currentDetailSubName;
    m_forwardStack.push(fwd);

    restoreNavEntry(m_backStack.pop());
}

void AppleMusicView::navigateForward()
{
    if (m_forwardStack.isEmpty()) return;

    // Save current state to back stack
    NavEntry back;
    back.state = m_currentState;
    back.searchTerm = m_lastSearchTerm;
    back.songs = m_lastSongs;
    back.albums = m_lastAlbums;
    back.artists = m_lastArtists;
    back.detailId = m_currentDetailId;
    back.detailName = m_currentDetailName;
    back.detailSubName = m_currentDetailSubName;
    m_backStack.push(back);

    restoreNavEntry(m_forwardStack.pop());
}

void AppleMusicView::restoreNavEntry(const NavEntry& entry)
{
    m_currentState = entry.state;
    m_lastSearchTerm = entry.searchTerm;
    m_lastSongs = entry.songs;
    m_lastAlbums = entry.albums;
    m_lastArtists = entry.artists;
    m_currentDetailId = entry.detailId;
    m_currentDetailName = entry.detailName;
    m_currentDetailSubName = entry.detailSubName;

    clearResults();
    m_loadingLabel->setVisible(false);
    m_noResultsLabel->setVisible(false);

    switch (m_currentState) {
    case AMViewState::Search:
        if (!m_lastSongs.isEmpty())
            buildSongsSection(m_lastSongs);
        if (!m_lastAlbums.isEmpty())
            buildAlbumsSection(m_lastAlbums);
        if (!m_lastArtists.isEmpty())
            buildArtistsSection(m_lastArtists);
        if (m_lastSongs.isEmpty() && m_lastAlbums.isEmpty() && m_lastArtists.isEmpty())
            m_noResultsLabel->setVisible(true);
        break;

    case AMViewState::ArtistDetail:
        if (!m_lastSongs.isEmpty()) {
            m_resultsLayout->addWidget(createSectionHeader(
                QStringLiteral("Songs by %1 (%2)").arg(m_currentDetailName).arg(m_lastSongs.size())));
            for (const auto& val : m_lastSongs)
                m_resultsLayout->addWidget(createSongRow(val.toObject()));
        }
        if (!m_lastAlbums.isEmpty())
            buildAlbumsSection(m_lastAlbums);
        break;

    case AMViewState::AlbumDetail:
        if (!m_lastSongs.isEmpty()) {
            m_resultsLayout->addWidget(createSectionHeader(
                QStringLiteral("%1 \u2014 %2 (%3)")
                    .arg(m_currentDetailName, m_currentDetailSubName)
                    .arg(m_lastSongs.size())));
            for (const auto& val : m_lastSongs)
                m_resultsLayout->addWidget(createSongRow(val.toObject()));
        }
        break;
    }

    m_resultsLayout->addStretch();
    updateNavBar();
}

void AppleMusicView::updateNavBar()
{
    auto c = ThemeManager::instance()->colors();

    // Show context title bar when in a sub-view
    bool showNav = m_currentState != AMViewState::Search;
    m_navBar->setVisible(showNav);

    // Back: enabled if internal stack has entries OR global can go back
    auto* mw = MainWindow::instance();
    bool canBack = !m_backStack.isEmpty() || (mw && mw->canGoBack());
    bool canFwd = !m_forwardStack.isEmpty() || (mw && mw->canGoForward());

    m_backBtn->setEnabled(canBack);
    m_forwardBtn->setEnabled(canFwd);
    m_backBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_forwardBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));

    auto navStyle = [&c](bool enabled) {
        Q_UNUSED(enabled)
        return QStringLiteral(
            "QPushButton { background: transparent; border: none; border-radius: 4px; }"
            "QPushButton:hover { background: %1; }"
            "QPushButton:disabled { background: transparent; }").arg(c.hover);
    };
    m_backBtn->setStyleSheet(navStyle(canBack));
    m_forwardBtn->setStyleSheet(navStyle(canFwd));

    switch (m_currentState) {
    case AMViewState::Search:
        m_navTitleLabel->setText(QString());
        break;
    case AMViewState::ArtistDetail:
        m_navTitleLabel->setText(QStringLiteral("%1 \u2014 Discography").arg(m_currentDetailName));
        break;
    case AMViewState::AlbumDetail:
        m_navTitleLabel->setText(QStringLiteral("%1 \u2014 %2")
            .arg(m_currentDetailName, m_currentDetailSubName));
        break;
    }
}

// ── updateAuthStatus ────────────────────────────────────────────────
void AppleMusicView::updateAuthStatus()
{
    auto* am = AppleMusicManager::instance();
    auto c = ThemeManager::instance()->colors();

    if (am->isAuthorized()) {
        m_authStatusLabel->setText(QStringLiteral("Connected"));
        m_authStatusLabel->setStyleSheet(
            QStringLiteral("color: #4CAF50; font-size: 12px; font-weight: bold;"));
        m_connectBtn->setVisible(false);
    } else {
        m_authStatusLabel->setText(QStringLiteral("Not connected"));
        m_authStatusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
        m_connectBtn->setVisible(true);
    }
}

// ── onSearch ────────────────────────────────────────────────────────
void AppleMusicView::onSearch()
{
    QString term = m_searchInput->lineEdit()->text().trimmed();
    if (term.isEmpty()) return;

    // Push current state before navigating
    if (!m_lastSongs.isEmpty() || !m_lastAlbums.isEmpty() || !m_lastArtists.isEmpty()
        || m_currentState != AMViewState::Search)
        pushNavState();

    m_currentState = AMViewState::Search;
    m_lastSearchTerm = term;
    m_lastSongs = QJsonArray();
    m_lastAlbums = QJsonArray();
    m_lastArtists = QJsonArray();
    m_currentDetailId.clear();
    m_currentDetailName.clear();
    m_currentDetailSubName.clear();

    clearResults();
    m_loadingLabel->setText(QStringLiteral("Searching..."));
    m_loadingLabel->setVisible(true);
    m_noResultsLabel->setVisible(false);
    updateNavBar();

    AppleMusicManager::instance()->searchCatalog(term);
}

// ── onSearchResults ─────────────────────────────────────────────────
void AppleMusicView::onSearchResults(const QJsonArray& songs,
                                      const QJsonArray& albums,
                                      const QJsonArray& artists)
{
    m_loadingLabel->setVisible(false);
    clearResults();

    // Cache results for back navigation
    m_lastSongs = songs;
    m_lastAlbums = albums;
    m_lastArtists = artists;

    if (songs.isEmpty() && albums.isEmpty() && artists.isEmpty()) {
        m_noResultsLabel->setVisible(true);
        updateNavBar();
        return;
    }

    m_noResultsLabel->setVisible(false);

    if (!songs.isEmpty())
        buildSongsSection(songs);
    if (!albums.isEmpty())
        buildAlbumsSection(albums);
    if (!artists.isEmpty())
        buildArtistsSection(artists);

    m_resultsLayout->addStretch();
    updateNavBar();
}

// ── onError ─────────────────────────────────────────────────────────
void AppleMusicView::onError(const QString& error)
{
    m_loadingLabel->setVisible(false);
    m_noResultsLabel->setText(QStringLiteral("Error: %1").arg(error));
    m_noResultsLabel->setVisible(true);
}

// ── clearResults ────────────────────────────────────────────────────
void AppleMusicView::clearResults()
{
    QLayoutItem* item;
    while ((item = m_resultsLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

// ── createSectionHeader ─────────────────────────────────────────────
QWidget* AppleMusicView::createSectionHeader(const QString& title)
{
    auto c = ThemeManager::instance()->colors();
    auto* label = new QLabel(title, m_resultsContainer);
    QFont f = label->font();
    f.setPixelSize(16);
    f.setBold(true);
    label->setFont(f);
    label->setStyleSheet(QStringLiteral("color: %1; padding: 4px 0;").arg(c.foreground));
    return label;
}

// ═════════════════════════════════════════════════════════════════════
//  Songs Section — list rows with fixed column widths
// ═════════════════════════════════════════════════════════════════════

// Column width constants for consistent alignment
static const int COL_PLAY_WIDTH   = 36;
static const int COL_ART_WIDTH    = 40;
static const int COL_ARTIST_WIDTH = 150;
static const int COL_ALBUM_WIDTH  = 200;
static const int COL_DUR_WIDTH    = 50;

void AppleMusicView::playSong(const QJsonObject& song)
{
    // Build a queue from all currently displayed songs so next/prev works
    QJsonArray songsArray = m_lastSongs;
    QVector<Track> queue;
    int playIndex = -1;

    QString targetId = song[QStringLiteral("id")].toString();

    for (int i = 0; i < songsArray.size(); ++i) {
        QJsonObject s = songsArray[i].toObject();
        Track t;
        t.id = s[QStringLiteral("id")].toString();
        t.title = s[QStringLiteral("title")].toString();
        t.artist = s[QStringLiteral("artist")].toString();
        t.album = s[QStringLiteral("album")].toString();
        t.duration = static_cast<int>(s[QStringLiteral("duration")].toDouble());
        t.coverUrl = s[QStringLiteral("artworkUrl")].toString();
        t.filePath = QString(); // empty = Apple Music source
        t.format = AudioFormat::AAC;
        t.sampleRate = QStringLiteral("44.1 kHz");
        t.bitDepth = QStringLiteral("16-bit");
        t.bitrate = QStringLiteral("256 kbps");
        queue.append(t);
        if (t.id == targetId)
            playIndex = i;
    }

    if (playIndex < 0 || queue.isEmpty()) {
        // Fallback: play single track if not found in current songs list
        Track t;
        t.id = targetId;
        t.title = song[QStringLiteral("title")].toString();
        t.artist = song[QStringLiteral("artist")].toString();
        t.album = song[QStringLiteral("album")].toString();
        t.duration = static_cast<int>(song[QStringLiteral("duration")].toDouble());
        t.coverUrl = song[QStringLiteral("artworkUrl")].toString();
        t.filePath = QString();
        t.format = AudioFormat::AAC;
        t.sampleRate = QStringLiteral("44.1 kHz");
        t.bitDepth = QStringLiteral("16-bit");
        t.bitrate = QStringLiteral("256 kbps");
        qDebug() << "[AppleMusic] Play (single):" << t.id << t.title;
        PlaybackState::instance()->playTrack(t);
        return;
    }

    qDebug() << "[AppleMusic] Play:" << targetId << queue[playIndex].title
             << "queue size:" << queue.size() << "index:" << playIndex;
    auto* ps = PlaybackState::instance();
    ps->setQueue(queue);
    ps->playTrack(queue[playIndex]);
}

void AppleMusicView::buildSongsSection(const QJsonArray& songs)
{
    m_resultsLayout->addWidget(createSectionHeader(
        QStringLiteral("Songs (%1)").arg(songs.size())));

    for (const auto& val : songs) {
        auto obj = val.toObject();
        m_resultsLayout->addWidget(createSongRow(obj));
    }
}

QWidget* AppleMusicView::createSongRow(const QJsonObject& song)
{
    auto c = ThemeManager::instance()->colors();

    auto* row = new QWidget(m_resultsContainer);
    row->setObjectName(QStringLiteral("songRow"));
    row->setFixedHeight(48);
    row->setFocusPolicy(Qt::NoFocus);
    row->setAttribute(Qt::WA_MacShowFocusRect, false);
    row->setStyleSheet(QStringLiteral(
        "#songRow, #songRow * { border: none; outline: none; }"
        "#songRow { background: transparent; border-radius: 6px; }"
        "#songRow:hover { background: %1; }"
        "#songRow QLabel { background: transparent; }"
        "#songRow QPushButton { background: transparent; }").arg(c.hover));

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(10);

    // Play button — fixed width
    auto* playBtn = new QPushButton(row);
    playBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/play.svg")));
    playBtn->setIconSize(QSize(16, 16));
    playBtn->setFixedSize(COL_PLAY_WIDTH, COL_PLAY_WIDTH);
    playBtn->setFlat(true);
    playBtn->setCursor(Qt::PointingHandCursor);
    playBtn->setFocusPolicy(Qt::NoFocus);
    playBtn->setAttribute(Qt::WA_MacShowFocusRect, false);
    playBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: %1px; outline: none; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:focus { outline: none; border: none; }"
        "QPushButton:active { outline: none; border: none; }"
        "QPushButton:pressed { outline: none; border: none; }").arg(COL_PLAY_WIDTH / 2).arg(c.accentMuted));
    layout->addWidget(playBtn);

    // Artwork thumbnail — fixed width
    auto* artLabel = new QLabel(row);
    artLabel->setFixedSize(COL_ART_WIDTH, COL_ART_WIDTH);
    artLabel->setFocusPolicy(Qt::NoFocus);
    artLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    artLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    artLabel->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 4px;").arg(c.backgroundSecondary));
    artLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(artLabel);

    QString artworkUrl = song[QStringLiteral("artworkUrl")].toString();
    if (!artworkUrl.isEmpty())
        loadArtwork(artworkUrl, artLabel, COL_ART_WIDTH);

    // Title — stretches to fill remaining space
    auto* titleLabel = new QLabel(row);
    titleLabel->setFocusPolicy(Qt::NoFocus);
    titleLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(c.foreground));
    titleLabel->setText(song[QStringLiteral("title")].toString());
    titleLabel->setMinimumWidth(100);
    titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    titleLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(titleLabel, 1);

    // Artist — fixed width, elided, CLICKABLE
    auto* artistLabel = new QLabel(row);
    artistLabel->setFocusPolicy(Qt::NoFocus);
    artistLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    artistLabel->setFixedWidth(COL_ARTIST_WIDTH);
    {
        QString artistName = song[QStringLiteral("artist")].toString();
        QFontMetrics fm(artistLabel->font());
        artistLabel->setText(fm.elidedText(artistName, Qt::ElideRight, COL_ARTIST_WIDTH));
    }
    // Clickable styling: underline on hover with accent color
    artistLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 12px; }"
        "QLabel:hover { color: %2; text-decoration: underline; }")
        .arg(c.foregroundSecondary, c.accent));
    QString songArtistId = song[QStringLiteral("artistId")].toString();
    if (!songArtistId.isEmpty()) {
        artistLabel->setCursor(Qt::PointingHandCursor);
        artistLabel->setProperty("artistId", songArtistId);
        artistLabel->setProperty("artistName", song[QStringLiteral("artist")].toString());
        artistLabel->installEventFilter(this);
    }
    layout->addWidget(artistLabel);

    // Album — fixed width, elided, CLICKABLE
    auto* albumLabel = new QLabel(row);
    albumLabel->setFocusPolicy(Qt::NoFocus);
    albumLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    albumLabel->setFixedWidth(COL_ALBUM_WIDTH);
    {
        QString albumName = song[QStringLiteral("album")].toString();
        QFontMetrics fm(albumLabel->font());
        albumLabel->setText(fm.elidedText(albumName, Qt::ElideRight, COL_ALBUM_WIDTH));
    }
    albumLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 12px; }"
        "QLabel:hover { color: %2; text-decoration: underline; }")
        .arg(c.foregroundMuted, c.accent));
    QString songAlbumId = song[QStringLiteral("albumId")].toString();
    if (!songAlbumId.isEmpty()) {
        albumLabel->setCursor(Qt::PointingHandCursor);
        albumLabel->setProperty("albumId", songAlbumId);
        albumLabel->setProperty("albumName", song[QStringLiteral("album")].toString());
        albumLabel->setProperty("albumArtist", song[QStringLiteral("artist")].toString());
        albumLabel->installEventFilter(this);
    }
    layout->addWidget(albumLabel);

    // Duration — fixed width, right-aligned
    int secs = static_cast<int>(song[QStringLiteral("duration")].toDouble());
    auto* durLabel = new QLabel(
        QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0')), row);
    durLabel->setFocusPolicy(Qt::NoFocus);
    durLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    durLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    durLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    durLabel->setFixedWidth(COL_DUR_WIDTH);
    durLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(durLabel);

    // Connect play button
    QJsonObject songCopy = song;
    connect(playBtn, &QPushButton::clicked, this, [this, songCopy]() {
        playSong(songCopy);
    });

    // Store song data as individual properties for double-click handling
    row->setProperty("songId", song[QStringLiteral("id")].toString());
    row->setProperty("songTitle", song[QStringLiteral("title")].toString());
    row->setProperty("songArtist", song[QStringLiteral("artist")].toString());
    row->setProperty("songAlbum", song[QStringLiteral("album")].toString());
    row->setProperty("songDuration", song[QStringLiteral("duration")].toDouble());
    row->setProperty("songArtwork", song[QStringLiteral("artworkUrl")].toString());
    row->installEventFilter(this);

    return row;
}

// ═════════════════════════════════════════════════════════════════════
//  Albums Section — responsive grid
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::buildAlbumsSection(const QJsonArray& albums)
{
    m_resultsLayout->addWidget(createSectionHeader(
        QStringLiteral("Albums (%1)").arg(albums.size())));

    auto* flowContainer = new QWidget(m_resultsContainer);
    auto* flowLayout = new QGridLayout(flowContainer);
    flowLayout->setContentsMargins(0, 0, 0, 0);
    flowLayout->setSpacing(12);

    int cols = qMax(2, (m_scrollArea->viewport()->width() - 24) / 172);
    int cardWidth = 160;

    for (int i = 0; i < albums.size(); ++i) {
        auto obj = albums[i].toObject();
        auto* card = createAlbumCard(obj, cardWidth);
        flowLayout->addWidget(card, i / cols, i % cols);
    }

    m_resultsLayout->addWidget(flowContainer);
}

QWidget* AppleMusicView::createAlbumCard(const QJsonObject& album, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();
    int textWidth = cardWidth - 16; // account for card padding

    auto* card = new QWidget(m_resultsContainer);
    card->setObjectName(QStringLiteral("albumCard"));
    card->setFixedWidth(cardWidth);
    card->setCursor(Qt::PointingHandCursor);
    card->setStyleSheet(QStringLiteral(
        "#albumCard { background: transparent; border-radius: 8px; }"
        "#albumCard:hover { background: %1; }").arg(c.hover));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Artwork
    int artSize = cardWidth - 16;
    auto* artLabel = new QLabel(card);
    artLabel->setFixedSize(artSize, artSize);
    artLabel->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 8px;").arg(c.backgroundSecondary));
    artLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(artLabel, 0, Qt::AlignCenter);

    QString artworkUrl = album[QStringLiteral("artworkUrl")].toString();
    if (!artworkUrl.isEmpty())
        loadArtwork(artworkUrl, artLabel, artSize);

    // Title — max 2 lines with proper elision
    QString titleText = album[QStringLiteral("title")].toString();
    auto* titleLabel = new QLabel(card);
    titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: bold;").arg(c.foreground));
    titleLabel->setFixedWidth(textWidth);
    titleLabel->setWordWrap(true);
    {
        QFontMetrics fm(titleLabel->font());
        int lineHeight = fm.height();
        titleLabel->setFixedHeight(lineHeight * 2 + 2);
        // Elide text to fit within ~2 lines worth of width
        QString elided = fm.elidedText(titleText, Qt::ElideRight, textWidth * 2 - fm.averageCharWidth());
        titleLabel->setText(elided);
    }
    layout->addWidget(titleLabel);

    // Artist — single line, elided
    QString artistText = album[QStringLiteral("artist")].toString();
    auto* artistLabel = new QLabel(card);
    artistLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted));
    artistLabel->setFixedWidth(textWidth);
    QFontMetrics fm(artistLabel->font());
    artistLabel->setText(fm.elidedText(artistText, Qt::ElideRight, textWidth));
    layout->addWidget(artistLabel);

    // Click handler — stored as properties, handled in eventFilter
    QString albumId = album[QStringLiteral("id")].toString();
    card->installEventFilter(this);
    card->setProperty("albumId", albumId);
    card->setProperty("albumName", titleText);
    card->setProperty("albumArtist", artistText);

    return card;
}

// ═════════════════════════════════════════════════════════════════════
//  Artists Section — responsive grid with circular art
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::buildArtistsSection(const QJsonArray& artists)
{
    m_resultsLayout->addWidget(createSectionHeader(
        QStringLiteral("Artists (%1)").arg(artists.size())));

    auto* flowContainer = new QWidget(m_resultsContainer);
    auto* flowLayout = new QGridLayout(flowContainer);
    flowLayout->setContentsMargins(0, 0, 0, 0);
    flowLayout->setSpacing(12);

    int cols = qMax(2, (m_scrollArea->viewport()->width() - 24) / 142);
    int cardWidth = 130;

    for (int i = 0; i < artists.size(); ++i) {
        auto obj = artists[i].toObject();
        auto* card = createArtistCard(obj, cardWidth);
        flowLayout->addWidget(card, i / cols, i % cols);
    }

    m_resultsLayout->addWidget(flowContainer);
}

QWidget* AppleMusicView::createArtistCard(const QJsonObject& artist, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();

    auto* card = new QWidget(m_resultsContainer);
    card->setObjectName(QStringLiteral("artistCard"));
    card->setFixedWidth(cardWidth);
    card->setCursor(Qt::PointingHandCursor);
    card->setStyleSheet(QStringLiteral(
        "#artistCard { background: transparent; border-radius: 8px; }"
        "#artistCard:hover { background: %1; }").arg(c.hover));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignHCenter);

    // Circular artwork
    int artSize = cardWidth - 24;
    auto* artLabel = new QLabel(card);
    artLabel->setFixedSize(artSize, artSize);
    artLabel->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: %2px;")
            .arg(c.backgroundSecondary).arg(artSize / 2));
    artLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(artLabel, 0, Qt::AlignCenter);

    QString artworkUrl = artist[QStringLiteral("artworkUrl")].toString();
    if (!artworkUrl.isEmpty())
        loadArtwork(artworkUrl, artLabel, artSize, true);

    // Name
    auto* nameLabel = new QLabel(artist[QStringLiteral("name")].toString(), card);
    nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: bold;").arg(c.foreground));
    nameLabel->setAlignment(Qt::AlignCenter);
    nameLabel->setWordWrap(true);
    nameLabel->setMaximumHeight(32);
    layout->addWidget(nameLabel);

    // Click to view discography
    QString artistId = artist[QStringLiteral("id")].toString();
    QString artistName = artist[QStringLiteral("name")].toString();
    card->installEventFilter(this);
    card->setProperty("artistId", artistId);
    card->setProperty("artistName", artistName);

    return card;
}

// ═════════════════════════════════════════════════════════════════════
//  Event filter for card clicks (artist + album)
// ═════════════════════════════════════════════════════════════════════

bool AppleMusicView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        // Artist card or artist label click → navigate to discography
        // (Labels in song rows have artistId but NOT songId)
        QString artistId = obj->property("artistId").toString();
        if (!artistId.isEmpty()) {
            QString artistName = obj->property("artistName").toString();
            showArtistDiscography(artistId, artistName);
            return true;
        }

        // Album card or album label click → navigate to album tracks
        QString albumId = obj->property("albumId").toString();
        if (!albumId.isEmpty()) {
            QString albumName = obj->property("albumName").toString();
            QString albumArtist = obj->property("albumArtist").toString();
            showAlbumTracks(albumId, albumName, albumArtist);
            return true;
        }
    }

    // Double-click on song row → play
    if (event->type() == QEvent::MouseButtonDblClick) {
        QString songId = obj->property("songId").toString();
        if (!songId.isEmpty()) {
            QJsonObject songData;
            songData[QStringLiteral("id")] = songId;
            songData[QStringLiteral("title")] = obj->property("songTitle").toString();
            songData[QStringLiteral("artist")] = obj->property("songArtist").toString();
            songData[QStringLiteral("album")] = obj->property("songAlbum").toString();
            songData[QStringLiteral("duration")] = obj->property("songDuration").toDouble();
            songData[QStringLiteral("artworkUrl")] = obj->property("songArtwork").toString();
            playSong(songData);
            return true;
        }
    }

    // Right-click on song row → context menu
    if (event->type() == QEvent::ContextMenu) {
        QString songId = obj->property("songId").toString();
        if (!songId.isEmpty()) {
            auto* cmEvent = static_cast<QContextMenuEvent*>(event);
            showSongContextMenu(qobject_cast<QWidget*>(obj), cmEvent->globalPos());
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

// ═════════════════════════════════════════════════════════════════════
//  Song row context menu (right-click)
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::showSongContextMenu(QWidget* songRow, const QPoint& globalPos)
{
    if (!songRow) return;

    // Reconstruct song JSON from row properties
    QJsonObject songData;
    songData[QStringLiteral("id")] = songRow->property("songId").toString();
    songData[QStringLiteral("title")] = songRow->property("songTitle").toString();
    songData[QStringLiteral("artist")] = songRow->property("songArtist").toString();
    songData[QStringLiteral("album")] = songRow->property("songAlbum").toString();
    songData[QStringLiteral("duration")] = songRow->property("songDuration").toDouble();
    songData[QStringLiteral("artworkUrl")] = songRow->property("songArtwork").toString();

    QMenu menu(this);
    menu.setStyleSheet(ThemeManager::instance()->menuStyle());

    QAction* playAction = menu.addAction(QStringLiteral("Play"));
    menu.addSeparator();

    // ── Add to Playlist submenu ──
    QMenu* playlistMenu = menu.addMenu(QStringLiteral("Add to Playlist"));
    playlistMenu->setStyleSheet(ThemeManager::instance()->menuStyle());

    auto* pm = PlaylistManager::instance();
    QVector<Playlist> playlists = pm->allPlaylists();

    // Build a Track from the Apple Music data
    Track track;
    track.id = songData[QStringLiteral("id")].toString();
    track.title = songData[QStringLiteral("title")].toString();
    track.artist = songData[QStringLiteral("artist")].toString();
    track.album = songData[QStringLiteral("album")].toString();
    track.duration = static_cast<int>(songData[QStringLiteral("duration")].toDouble());
    track.coverUrl = songData[QStringLiteral("artworkUrl")].toString();
    track.filePath = QString();  // empty = Apple Music source

    for (const Playlist& pl : playlists) {
        if (pl.isSmartPlaylist) continue;
        QAction* plAction = playlistMenu->addAction(pl.name);
        connect(plAction, &QAction::triggered, this, [track, pl]() {
            PlaylistManager::instance()->addTrack(pl.id, track);
            qDebug() << "[AppleMusic] Added to playlist:" << pl.name << "-" << track.title;
        });
    }

    if (!playlists.isEmpty())
        playlistMenu->addSeparator();

    QAction* newPlaylist = playlistMenu->addAction(QStringLiteral("+ New Playlist..."));
    connect(newPlaylist, &QAction::triggered, this, [this, track]() {
        NewPlaylistDialog dialog(window());
        if (dialog.exec() == QDialog::Accepted) {
            QString name = dialog.playlistName();
            if (!name.isEmpty()) {
                QString id = PlaylistManager::instance()->createPlaylist(name);
                if (!id.isEmpty()) {
                    PlaylistManager::instance()->addTrack(id, track);
                    qDebug() << "[AppleMusic] Created playlist + added:" << name << "-" << track.title;
                }
            }
        }
    });

    QAction* chosen = menu.exec(globalPos);
    if (chosen == playAction) {
        playSong(songData);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Artist discography navigation
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::showArtistDiscography(const QString& artistId, const QString& artistName)
{
    pushNavState();

    m_currentState = AMViewState::ArtistDetail;
    m_currentDetailId = artistId;
    m_currentDetailName = artistName;
    m_currentDetailSubName.clear();
    m_lastSongs = QJsonArray();
    m_lastAlbums = QJsonArray();
    m_lastArtists = QJsonArray();

    clearResults();
    m_loadingLabel->setText(QStringLiteral("Loading songs by %1...").arg(artistName));
    m_loadingLabel->setVisible(true);
    m_noResultsLabel->setVisible(false);
    updateNavBar();

    auto* am = AppleMusicManager::instance();
    am->fetchArtistSongs(artistId);
    am->fetchArtistAlbums(artistId);
}

void AppleMusicView::onArtistSongs(const QString& /*artistId*/, const QJsonArray& songs)
{
    if (m_currentState != AMViewState::ArtistDetail) return;

    m_loadingLabel->setVisible(false);
    m_lastSongs = songs;  // cache for back navigation

    if (songs.isEmpty()) {
        m_noResultsLabel->setText(QStringLiteral("No songs found for %1").arg(m_currentDetailName));
        m_noResultsLabel->setVisible(true);
        return;
    }

    m_noResultsLabel->setVisible(false);

    m_resultsLayout->insertWidget(0, createSectionHeader(
        QStringLiteral("Songs by %1 (%2)").arg(m_currentDetailName).arg(songs.size())));

    for (int i = 0; i < songs.size(); ++i) {
        auto obj = songs[i].toObject();
        m_resultsLayout->insertWidget(i + 1, createSongRow(obj));
    }

    m_resultsLayout->addStretch();
}

void AppleMusicView::onArtistAlbums(const QString& /*artistId*/, const QJsonArray& albums)
{
    if (m_currentState != AMViewState::ArtistDetail) return;
    if (albums.isEmpty()) return;

    m_lastAlbums = albums;  // cache for back navigation
    buildAlbumsSection(albums);
    m_resultsLayout->addStretch();
}

// ═════════════════════════════════════════════════════════════════════
//  Album tracks navigation
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::showAlbumTracks(const QString& albumId, const QString& albumName,
                                      const QString& artistName)
{
    pushNavState();

    m_currentState = AMViewState::AlbumDetail;
    m_currentDetailId = albumId;
    m_currentDetailName = albumName;
    m_currentDetailSubName = artistName;
    m_lastSongs = QJsonArray();
    m_lastAlbums = QJsonArray();
    m_lastArtists = QJsonArray();

    clearResults();
    m_loadingLabel->setText(QStringLiteral("Loading tracks..."));
    m_loadingLabel->setVisible(true);
    m_noResultsLabel->setVisible(false);
    updateNavBar();

    AppleMusicManager::instance()->fetchAlbumTracks(albumId);
}

void AppleMusicView::onAlbumTracks(const QString& /*albumId*/, const QJsonArray& tracks)
{
    if (m_currentState != AMViewState::AlbumDetail) return;

    m_loadingLabel->setVisible(false);
    m_lastSongs = tracks;  // cache for back navigation

    if (tracks.isEmpty()) {
        m_noResultsLabel->setText(QStringLiteral("No tracks found"));
        m_noResultsLabel->setVisible(true);
        return;
    }

    m_noResultsLabel->setVisible(false);
    clearResults();

    m_resultsLayout->addWidget(createSectionHeader(
        QStringLiteral("%1 \u2014 %2 (%3)")
            .arg(m_currentDetailName, m_currentDetailSubName)
            .arg(tracks.size())));

    for (const auto& val : tracks)
        m_resultsLayout->addWidget(createSongRow(val.toObject()));

    m_resultsLayout->addStretch();
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme — called when theme changes (light/dark switch)
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    // Persistent header widgets
    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(c.foreground));
    m_navTitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundSecondary));
    m_loadingLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;").arg(c.foregroundMuted));
    m_noResultsLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));

    m_scrollArea->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }") +
        ThemeManager::instance()->scrollbarStyle());

    // Update auth status colors
    updateAuthStatus();

    // Update nav button colors
    updateNavBar();

    // Rebuild current results with new theme colors
    // (dynamic content picks up current theme from ThemeManager::instance()->colors())
    if (!m_lastSongs.isEmpty() || !m_lastAlbums.isEmpty() || !m_lastArtists.isEmpty()) {
        clearResults();
        restoreNavEntry({m_currentState, m_lastSearchTerm, m_lastSongs, m_lastAlbums,
                         m_lastArtists, m_currentDetailId, m_currentDetailName, m_currentDetailSubName});
    }
}

// ═════════════════════════════════════════════════════════════════════
//  loadArtwork — async network fetch
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::loadArtwork(const QString& url, QLabel* target, int size, bool circular)
{
    // Replace {w}x{h} placeholders in Apple Music artwork URLs
    QString resolvedUrl = url;
    resolvedUrl.replace(QStringLiteral("{w}"), QString::number(size * 2));
    resolvedUrl.replace(QStringLiteral("{h}"), QString::number(size * 2));

    QNetworkRequest req{QUrl(resolvedUrl)};
    QNetworkReply* reply = m_networkManager->get(req);

    QPointer<QLabel> safeTarget = target;
    connect(reply, &QNetworkReply::finished, this, [reply, safeTarget, size, circular]() {
        reply->deleteLater();
        if (!safeTarget) return;
        if (reply->error() != QNetworkReply::NoError) return;

        QPixmap pm;
        pm.loadFromData(reply->readAll());
        if (pm.isNull()) return;

        pm = pm.scaled(size * 2, size * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

        if (circular) {
            QPixmap circPm(size * 2, size * 2);
            circPm.fill(Qt::transparent);
            QPainter painter(&circPm);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addEllipse(0, 0, size * 2, size * 2);
            painter.setClipPath(path);
            painter.drawPixmap(0, 0, pm);
            painter.end();
            safeTarget->setPixmap(circPm.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            // Rounded corners
            int radius = 8;
            QPixmap rounded(size * 2, size * 2);
            rounded.fill(Qt::transparent);
            QPainter painter(&rounded);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(0, 0, size * 2, size * 2, radius * 2, radius * 2);
            painter.setClipPath(path);
            painter.drawPixmap(0, 0, pm);
            painter.end();
            safeTarget->setPixmap(rounded.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    });
}
