#include "AppleMusicView.h"
#include "../amview/AMSearchPanel.h"
#include "../amview/AMArtistPanel.h"
#include "../amview/AMAlbumPanel.h"
#include "../amview/AMContentPanel.h"
#include "../../apple/AppleMusicManager.h"
#include "../../apple/MusicKitPlayer.h"
#include "../../core/PlaybackState.h"
#include "../../core/ThemeManager.h"
#include "../../widgets/StyledInput.h"
#include "../../widgets/StyledButton.h"
#include "../services/NavigationService.h"
#include "../../core/library/PlaylistManager.h"
#include "../../ui/dialogs/NewPlaylistDialog.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QDebug>
#include <QMenu>

// ═════════════════════════════════════════════════════════════════════
//  Constructor — signal wiring + token flow
// ═════════════════════════════════════════════════════════════════════

AppleMusicView::AppleMusicView(QWidget* parent)
    : QWidget(parent)
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
                if (status == AppleMusicManager::NotDetermined) {
                    qDebug() << "[AppleMusicView] Auth revoked — clearing cached Music User Token";
                    m_musicUserToken.clear();
                }
            });

    updateAuthStatus();

    // ── Music User Token flow ────────────────────────────────────────
    auto* player = MusicKitPlayer::instance();

    connect(am, &AppleMusicManager::musicUserTokenReady,
        this, [this, player](const QString& token) {
            qDebug() << "[AppleMusicView] Music User Token received, length:" << token.length();
            m_musicUserToken = token;
            player->injectMusicUserToken(token);
        });

    connect(am, &AppleMusicManager::musicUserTokenFailed,
        this, [](const QString& error) {
            qDebug() << "[AppleMusicView] Music User Token FAILED:" << error;
            qDebug() << "[AppleMusicView] Continuing with 30-second previews";
        });

    connect(player, &MusicKitPlayer::musicKitReady,
        this, [this, player]() {
            qDebug() << "[AppleMusicView] MusicKit JS is ready";
            if (!m_musicUserToken.isEmpty()) {
                qDebug() << "[AppleMusicView] Injecting cached Music User Token";
                player->injectMusicUserToken(m_musicUserToken);
            } else {
                qDebug() << "[AppleMusicView] No cached token yet, will inject when available";
            }
        });

    connect(player, &MusicKitPlayer::fullPlaybackAvailable,
        this, [this]() {
            qDebug() << "[AppleMusicView] Full playback mode confirmed!";
            updateAuthStatus();
        });

    connect(player, &MusicKitPlayer::previewOnlyMode,
        this, []() {
            qDebug() << "[AppleMusicView] Preview only mode (check Apple Music subscription)";
        });

    connect(player, &MusicKitPlayer::tokenExpired,
        this, [this, am]() {
            qDebug() << "[AppleMusicView] Token expired, re-requesting...";
            m_musicUserToken.clear();
            am->requestMusicUserToken();
        });

    qDebug() << "[AppleMusicView] Waiting for manual Connect (no auto-token request)";

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &AppleMusicView::refreshTheme);
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI — header, search, nav bar, stacked panels
// ═════════════════════════════════════════════════════════════════════

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

        connect(m_backBtn, &QPushButton::clicked, this, [this]() {
            if (!m_backStack.isEmpty()) {
                navigateBack();
            } else {
                NavigationService::instance()->navigateBack();
            }
        });
        connect(m_forwardBtn, &QPushButton::clicked, this, [this]() {
            if (!m_forwardStack.isEmpty()) {
                navigateForward();
            } else {
                NavigationService::instance()->navigateForward();
            }
        });

        connect(NavigationService::instance(), &NavigationService::navChanged,
                this, [this]() { updateNavBar(); });

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

    // ── Context title bar ────────────────────────────────────────────
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

    // ── Loading / no-results labels ──────────────────────────────────
    m_loadingLabel = new QLabel(QStringLiteral("Searching..."), this);
    m_loadingLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;").arg(c.foregroundMuted));
    m_loadingLabel->setVisible(false);
    mainLayout->addWidget(m_loadingLabel);

    m_noResultsLabel = new QLabel(QStringLiteral("No results found"), this);
    m_noResultsLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));
    m_noResultsLabel->setAlignment(Qt::AlignCenter);
    m_noResultsLabel->setVisible(false);
    mainLayout->addWidget(m_noResultsLabel);

    // ── Content panels (stacked) ─────────────────────────────────────
    m_stackedWidget = new QStackedWidget(this);
    m_searchPanel = new AMSearchPanel(m_stackedWidget);
    m_artistPanel = new AMArtistPanel(m_stackedWidget);
    m_albumPanel = new AMAlbumPanel(m_stackedWidget);

    m_stackedWidget->addWidget(m_searchPanel);
    m_stackedWidget->addWidget(m_artistPanel);
    m_stackedWidget->addWidget(m_albumPanel);

    wirePanelSignals(m_searchPanel);
    wirePanelSignals(m_artistPanel);
    wirePanelSignals(m_albumPanel);

    mainLayout->addWidget(m_stackedWidget, 1);
}

// ═════════════════════════════════════════════════════════════════════
//  Wire panel signals → coordinator handlers
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::wirePanelSignals(QWidget* panel)
{
    auto* p = qobject_cast<AMContentPanel*>(panel);
    if (!p) return;

    connect(p, &AMContentPanel::songPlayRequested,
            this, &AppleMusicView::playSong);
    connect(p, &AMContentPanel::artistNavigationRequested,
            this, &AppleMusicView::showArtistDiscography);
    connect(p, &AMContentPanel::albumNavigationRequested,
            this, &AppleMusicView::showAlbumTracks);
    connect(p, &AMContentPanel::songContextMenuRequested,
            this, &AppleMusicView::showSongContextMenu);
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
    m_forwardStack.clear();
    updateNavBar();
}

void AppleMusicView::navigateBack()
{
    if (m_backStack.isEmpty()) return;

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

    m_loadingLabel->setVisible(false);
    m_noResultsLabel->setVisible(false);

    switch (m_currentState) {
    case AMViewState::Search:
        if (m_lastSongs.isEmpty() && m_lastAlbums.isEmpty() && m_lastArtists.isEmpty()) {
            m_noResultsLabel->setVisible(true);
        } else {
            m_searchPanel->setResults(m_lastSongs, m_lastAlbums, m_lastArtists);
        }
        m_stackedWidget->setCurrentWidget(m_searchPanel);
        break;

    case AMViewState::ArtistDetail:
        m_artistPanel->setSongs(m_currentDetailName, m_lastSongs);
        if (!m_lastAlbums.isEmpty())
            m_artistPanel->setAlbums(m_lastAlbums);
        m_stackedWidget->setCurrentWidget(m_artistPanel);
        break;

    case AMViewState::AlbumDetail:
        m_albumPanel->setTracks(m_currentDetailName, m_currentDetailSubName, m_lastSongs);
        m_stackedWidget->setCurrentWidget(m_albumPanel);
        break;
    }

    updateNavBar();
}

void AppleMusicView::updateNavBar()
{
    auto c = ThemeManager::instance()->colors();

    bool showNav = m_currentState != AMViewState::Search;
    m_navBar->setVisible(showNav);

    auto* nav = NavigationService::instance();
    bool canBack = !m_backStack.isEmpty() || nav->canGoBack();
    bool canFwd = !m_forwardStack.isEmpty() || nav->canGoForward();

    m_backBtn->setEnabled(canBack);
    m_forwardBtn->setEnabled(canFwd);
    m_backBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_forwardBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));

    auto navStyle = [&c](bool /*enabled*/) {
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

// ═════════════════════════════════════════════════════════════════════
//  Auth status
// ═════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════
//  Search
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::onSearch()
{
    QString term = m_searchInput->lineEdit()->text().trimmed();
    if (term.isEmpty()) return;

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

    m_searchPanel->clear();
    m_stackedWidget->setCurrentWidget(m_searchPanel);
    m_loadingLabel->setText(QStringLiteral("Searching..."));
    m_loadingLabel->setVisible(true);
    m_noResultsLabel->setVisible(false);
    updateNavBar();

    AppleMusicManager::instance()->searchCatalog(term);
}

void AppleMusicView::onSearchResults(const QJsonArray& songs,
                                      const QJsonArray& albums,
                                      const QJsonArray& artists)
{
    m_loadingLabel->setVisible(false);

    m_lastSongs = songs;
    m_lastAlbums = albums;
    m_lastArtists = artists;

    if (songs.isEmpty() && albums.isEmpty() && artists.isEmpty()) {
        m_noResultsLabel->setVisible(true);
        updateNavBar();
        return;
    }

    m_noResultsLabel->setVisible(false);
    m_searchPanel->setResults(songs, albums, artists);
    m_stackedWidget->setCurrentWidget(m_searchPanel);
    updateNavBar();
}

void AppleMusicView::onError(const QString& error)
{
    m_loadingLabel->setVisible(false);
    m_noResultsLabel->setText(QStringLiteral("Error: %1").arg(error));
    m_noResultsLabel->setVisible(true);
}

// ═════════════════════════════════════════════════════════════════════
//  Artist discography
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

    m_artistPanel->setSongs(artistName, QJsonArray());
    m_stackedWidget->setCurrentWidget(m_artistPanel);
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
    m_lastSongs = songs;

    if (songs.isEmpty()) {
        m_noResultsLabel->setText(QStringLiteral("No songs found for %1").arg(m_currentDetailName));
        m_noResultsLabel->setVisible(true);
        return;
    }

    m_noResultsLabel->setVisible(false);
    m_artistPanel->setSongs(m_currentDetailName, songs);
}

void AppleMusicView::onArtistAlbums(const QString& /*artistId*/, const QJsonArray& albums)
{
    if (m_currentState != AMViewState::ArtistDetail) return;
    if (albums.isEmpty()) return;

    m_lastAlbums = albums;
    m_artistPanel->setAlbums(albums);
}

// ═════════════════════════════════════════════════════════════════════
//  Album tracks
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

    m_albumPanel->clear();
    m_stackedWidget->setCurrentWidget(m_albumPanel);
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
    m_lastSongs = tracks;

    if (tracks.isEmpty()) {
        m_noResultsLabel->setText(QStringLiteral("No tracks found"));
        m_noResultsLabel->setVisible(true);
        return;
    }

    m_noResultsLabel->setVisible(false);
    m_albumPanel->setTracks(m_currentDetailName, m_currentDetailSubName, tracks);
}

// ═════════════════════════════════════════════════════════════════════
//  playSong — builds queue from m_lastSongs (singleton access)
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::playSong(const QJsonObject& song)
{
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
        t.filePath = QString();
        t.format = AudioFormat::AAC;
        t.sampleRate = QStringLiteral("44.1 kHz");
        t.bitDepth = QStringLiteral("16-bit");
        t.bitrate = QStringLiteral("256 kbps");
        queue.append(t);
        if (t.id == targetId)
            playIndex = i;
    }

    if (playIndex < 0 || queue.isEmpty()) {
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

// ═════════════════════════════════════════════════════════════════════
//  Context menu (singleton access: PlaylistManager)
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::showSongContextMenu(const QPoint& globalPos, const QJsonObject& songData)
{
    QMenu menu(this);
    menu.setStyleSheet(ThemeManager::instance()->menuStyle());

    QAction* playAction = menu.addAction(QStringLiteral("Play"));
    menu.addSeparator();

    // ── Add to Playlist submenu ──
    QMenu* playlistMenu = menu.addMenu(QStringLiteral("Add to Playlist"));
    playlistMenu->setStyleSheet(ThemeManager::instance()->menuStyle());

    auto* pm = PlaylistManager::instance();
    QVector<Playlist> playlists = pm->allPlaylists();

    Track track;
    track.id = songData[QStringLiteral("id")].toString();
    track.title = songData[QStringLiteral("title")].toString();
    track.artist = songData[QStringLiteral("artist")].toString();
    track.album = songData[QStringLiteral("album")].toString();
    track.duration = static_cast<int>(songData[QStringLiteral("duration")].toDouble());
    track.coverUrl = songData[QStringLiteral("artworkUrl")].toString();
    track.filePath = QString();

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
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void AppleMusicView::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(c.foreground));
    m_navTitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundSecondary));
    m_loadingLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;").arg(c.foregroundMuted));
    m_noResultsLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;").arg(c.foregroundMuted));

    m_searchPanel->refreshScrollStyle();
    m_artistPanel->refreshScrollStyle();
    m_albumPanel->refreshScrollStyle();

    updateAuthStatus();
    updateNavBar();

    // Rebuild current content with new theme
    if (!m_lastSongs.isEmpty() || !m_lastAlbums.isEmpty() || !m_lastArtists.isEmpty()) {
        restoreNavEntry({m_currentState, m_lastSearchTerm, m_lastSongs, m_lastAlbums,
                         m_lastArtists, m_currentDetailId, m_currentDetailName, m_currentDetailSubName});
    }
}
