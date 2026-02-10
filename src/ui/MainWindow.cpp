#include "MainWindow.h"
#include "../core/PlaybackState.h"
#include "../core/Settings.h"
#include "../core/audio/AudioEngine.h"
#include "../core/library/LibraryScanner.h"
#include "../plugins/VST3Host.h"
#include "../plugins/VST2Host.h"
#include "../core/dsp/DSPPipeline.h"
#include "../apple/MusicKitPlayer.h"
#include "views/NowPlayingView.h"
#include "views/LibraryView.h"
#include "views/AlbumsView.h"
#include "views/AlbumDetailView.h"
#include "views/ArtistsView.h"
#include "views/ArtistDetailView.h"
#include "views/PlaylistsView.h"
#include "views/PlaylistDetailView.h"
#include "views/QueueView.h"
#include "views/SettingsView.h"
#include "views/AppleMusicView.h"
// #include "views/TidalView.h"  // TODO: restore when Tidal API available
#include "views/FolderBrowserView.h"
#include "views/SearchResultsView.h"
#include "../core/library/LibraryDatabase.h"
#include "../core/CoverArtLoader.h"
#ifdef Q_OS_MAC
#include "../platform/macos/MacMediaIntegration.h"
#endif
#include <QDebug>
#include <QTimer>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QThread>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QWebEngineView>

static bool isTextInputFocused()
{
    QWidget* w = QApplication::focusWidget();

    // Check if WebEngine widget has direct focus
    if (w) {
        QString className = QString::fromLatin1(w->metaObject()->className());
        if (className.contains(QStringLiteral("WebEngine")) ||
            className.contains(QStringLiteral("RenderWidget")) ||
            className.contains(QStringLiteral("QtWebEngine"))) {
            return true;  // Let WebView handle keyboard input
        }
        if (qobject_cast<QLineEdit*>(w) ||
            qobject_cast<QTextEdit*>(w) ||
            qobject_cast<QPlainTextEdit*>(w)) {
            return true;
        }
    }

    // NOTE: Spacebar handling for Apple Music vs Local playback is now
    // based on PlaybackState::currentSource(), not the current view.
    // This allows spacebar to control local playback even when on AppleMusicView.
    return false;
}

MainWindow* MainWindow::s_instance = nullptr;

MainWindow* MainWindow::instance()
{
    return s_instance;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    s_instance = this;
    setupUI();
    connectSignals();

    setWindowTitle("Sorana Flow");
    resize(1400, 900);
    setMinimumSize(900, 600);

    // Global keyboard shortcuts — skip when a text input has focus
    auto* spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    spaceShortcut->setContext(Qt::ApplicationShortcut);
    connect(spaceShortcut, &QShortcut::activated, this, [this]() {
        if (isTextInputFocused()) return;
        // Check playback SOURCE (not view) to decide which player to control
        auto* ps = PlaybackState::instance();
        if (ps->currentSource() == PlaybackState::AppleMusic) {
            MusicKitPlayer::instance()->togglePlayPause();
        } else {
            ps->playPause();
        }
    });

    // Ctrl+Left / Ctrl+Right for prev/next
    auto* ctrlLeft = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this);
    ctrlLeft->setContext(Qt::ApplicationShortcut);
    connect(ctrlLeft, &QShortcut::activated, this, []() {
        PlaybackState::instance()->previous();
    });

    auto* ctrlRight = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this);
    ctrlRight->setContext(Qt::ApplicationShortcut);
    connect(ctrlRight, &QShortcut::activated, this, []() {
        PlaybackState::instance()->next();
    });

    // Media key shortcuts (unconditional)
    auto* mediaPlay = new QShortcut(Qt::Key_MediaPlay, this);
    mediaPlay->setContext(Qt::ApplicationShortcut);
    connect(mediaPlay, &QShortcut::activated, this, []() {
        PlaybackState::instance()->playPause();
    });

    auto* mediaNext = new QShortcut(Qt::Key_MediaNext, this);
    mediaNext->setContext(Qt::ApplicationShortcut);
    connect(mediaNext, &QShortcut::activated, this, []() {
        PlaybackState::instance()->next();
    });

    auto* mediaPrev = new QShortcut(Qt::Key_MediaPrevious, this);
    mediaPrev->setContext(Qt::ApplicationShortcut);
    connect(mediaPrev, &QShortcut::activated, this, []() {
        PlaybackState::instance()->previous();
    });

    // Cmd+F / Ctrl+F → focus search
    auto* searchShortcutF = new QShortcut(QKeySequence::Find, this);
    searchShortcutF->setContext(Qt::ApplicationShortcut);
    connect(searchShortcutF, &QShortcut::activated, this, [this]() {
        m_sidebar->focusSearch();
    });

    // Global Escape: install app-level event filter to catch Escape
    // before child widgets (QTableView, QScrollArea) consume it
    qApp->installEventFilter(this);

#ifdef Q_OS_MAC
    // ── macOS Now Playing + Media Keys ──────────────────────────────
    auto& macMedia = MacMediaIntegration::instance();
    macMedia.initialize();

    connect(&macMedia, &MacMediaIntegration::playPauseRequested, this, []() {
        PlaybackState::instance()->playPause();
    });
    connect(&macMedia, &MacMediaIntegration::nextRequested, this, []() {
        PlaybackState::instance()->next();
    });
    connect(&macMedia, &MacMediaIntegration::previousRequested, this, []() {
        PlaybackState::instance()->previous();
    });
    connect(&macMedia, &MacMediaIntegration::seekRequested, this, [](double pos) {
        PlaybackState::instance()->seek(static_cast<int>(pos));
    });

    auto* ps = PlaybackState::instance();

    // Track changed → update Now Playing metadata
    connect(ps, &PlaybackState::trackChanged, this, [](const Track& track) {
        MacMediaIntegration::instance().updateNowPlaying(
            track.title, track.artist, track.album,
            static_cast<double>(track.duration), 0.0, true);
    });

    // Play/pause state changed → update playback rate
    connect(ps, &PlaybackState::playStateChanged, this, [ps](bool playing) {
        const Track& t = ps->currentTrack();
        if (t.title.isEmpty()) return;
        MacMediaIntegration::instance().updateNowPlaying(
            t.title, t.artist, t.album,
            static_cast<double>(t.duration),
            static_cast<double>(ps->currentTime()), playing);
    });

    // Cover art loaded → update Now Playing artwork
    connect(CoverArtLoader::instance(), &CoverArtLoader::coverArtReady,
            this, [ps](const QString& trackPath, const QPixmap& pixmap) {
        // Only update artwork for the currently playing track
        if (ps->currentTrack().filePath == trackPath && !pixmap.isNull()) {
            MacMediaIntegration::instance().updateArtwork(pixmap.toImage());
        }
    });
#endif

    // macOS: reopen window when Dock icon clicked
    connect(qApp, &QApplication::applicationStateChanged,
        this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && !isVisible()) {
                show();
                raise();
                activateWindow();
            }
        });
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI — only creates sidebar, stack, playback bar, and NowPlaying
// ═════════════════════════════════════════════════════════════════════

void MainWindow::setupUI()
{
    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QHBoxLayout* mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Left: Sidebar
    m_sidebar = new AppSidebar(this);
    mainLayout->addWidget(m_sidebar);

    // Right: Content area
    QVBoxLayout* rightLayout = new QVBoxLayout();
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // View Stack — only create NowPlaying initially
    m_viewStack = new QStackedWidget();

    m_nowPlayingView = new NowPlayingView();
    m_viewStack->addWidget(m_nowPlayingView);
    m_viewStack->setCurrentWidget(m_nowPlayingView);

    rightLayout->addWidget(m_viewStack, 1);

    // PlaybackBar
    m_playbackBar = new PlaybackBar();
    rightLayout->addWidget(m_playbackBar, 0);

    mainLayout->addLayout(rightLayout, 1);
}

// ═════════════════════════════════════════════════════════════════════
//  connectSignals — only connects what exists at startup
// ═════════════════════════════════════════════════════════════════════

void MainWindow::connectSignals()
{
    // Sidebar navigation
    connect(m_sidebar, &AppSidebar::navigationChanged,
            this, &MainWindow::onNavigationChanged);

    // PlaybackBar -> queue toggle
    connect(m_playbackBar, &PlaybackBar::queueToggled,
            this, &MainWindow::onQueueToggled);

    // PlaybackBar -> artist click
    connect(m_playbackBar, &PlaybackBar::artistClicked,
            this, &MainWindow::onArtistSelected);

    // NowPlayingView -> artist click
    connect(m_nowPlayingView, &NowPlayingView::artistClicked,
            this, &MainWindow::onArtistSelected);

    // Sidebar folder selected -> navigate to Library view
    connect(m_sidebar, &AppSidebar::folderSelected,
            this, &MainWindow::onFolderSelected);

    // Sidebar search -> global search results view
    connect(m_sidebar, &AppSidebar::searchRequested,
            this, [this](const QString& query) {
        if (query.trimmed().isEmpty()) {
            onSearchCleared();
        } else {
            onSearch(query.trimmed());
        }
    });

    // ── Scan progress indicator ────────────────────────────────────────
    auto* scanner = LibraryScanner::instance();
    auto* db = LibraryDatabase::instance();

    // Delayed scan indicator — only show if operation takes >500ms
    m_scanShowTimer = new QTimer(this);
    m_scanShowTimer->setSingleShot(true);
    connect(m_scanShowTimer, &QTimer::timeout, this, [this]() {
        showScanIndicator(m_pendingScanMsg);
    });

    connect(scanner, &LibraryScanner::scanStarted, this, [this]() {
        m_pendingScanMsg = QStringLiteral("Scanning library...");
        m_scanShowTimer->start(500);
    });
    connect(scanner, &LibraryScanner::scanFinished, this, [this](int) {
        hideScanIndicator();
    });
    connect(db, &LibraryDatabase::rebuildStarted, this, [this]() {
        m_pendingScanMsg = QStringLiteral("Rebuilding library...");
        if (!m_scanShowTimer->isActive() && (!m_scanOverlay || !m_scanOverlay->isVisible()))
            m_scanShowTimer->start(500);
        else if (m_scanOverlay && m_scanOverlay->isVisible())
            showScanIndicator(m_pendingScanMsg);
    });
    connect(db, &LibraryDatabase::rebuildFinished, this, [this]() {
        hideScanIndicator();
    });
}

// ═════════════════════════════════════════════════════════════════════
//  initializeDeferred — called after window is shown
// ═════════════════════════════════════════════════════════════════════

void MainWindow::initializeDeferred()
{
    if (m_initialized) return;
    m_initialized = true;

    // ── Load saved VST plugins into DSP pipeline at startup ─────────
    // SettingsView is lazy (created only when user opens Settings),
    // so we must load saved plugins here to apply them from the first buffer.
    {
        QStringList paths = Settings::instance()->activeVstPlugins();
        if (!paths.isEmpty()) {
            auto* vst3host = VST3Host::instance();
            if (vst3host->plugins().empty()) vst3host->scanPlugins();
            auto* vst2host = VST2Host::instance();
            if (vst2host->plugins().empty()) vst2host->scanPlugins();

            auto* pipeline = AudioEngine::instance()->dspPipeline();
            int loaded = 0;
            for (const QString& path : paths) {
                bool isVst2 = path.endsWith(QStringLiteral(".vst"));
                std::shared_ptr<IDSPProcessor> proc;
                if (isVst2)
                    proc = vst2host->createProcessorFromPath(path.toStdString());
                else
                    proc = vst3host->createProcessorFromPath(path.toStdString());
                if (!proc) {
                    qDebug() << "[STARTUP] VST load FAILED:" << path;
                    continue;
                }
                if (pipeline) {
                    pipeline->addProcessor(proc);
                    ++loaded;
                    qDebug() << "[STARTUP] VST loaded:"
                             << QString::fromStdString(proc->getName());
                }
            }
            qDebug() << "[STARTUP] VST plugins loaded:" << loaded
                     << "of" << paths.size();
        }
    }

    qDebug() << "[STARTUP] Deferred init complete";
}

// ═════════════════════════════════════════════════════════════════════
//  Scan progress indicator
// ═════════════════════════════════════════════════════════════════════

void MainWindow::showScanIndicator(const QString& msg)
{
    if (!m_scanOverlay) {
        m_scanOverlay = new QWidget(this);
        m_scanOverlay->setObjectName(QStringLiteral("scanOverlay"));
        m_scanOverlay->setStyleSheet(QStringLiteral(
            "QWidget#scanOverlay { background: rgba(0,0,0,0.7); border-radius: 6px; }"
            "QLabel { color: white; font-size: 12px; }"
            "QProgressBar { background: rgba(255,255,255,0.2); border: none; border-radius: 2px; max-height: 4px; }"
            "QProgressBar::chunk { background: #FF6B35; border-radius: 2px; }"));
        auto* lay = new QHBoxLayout(m_scanOverlay);
        lay->setContentsMargins(12, 6, 12, 6);
        lay->setSpacing(8);
        m_scanStatusLabel = new QLabel;
        m_scanProgress = new QProgressBar;
        m_scanProgress->setRange(0, 0);  // indeterminate
        m_scanProgress->setFixedHeight(4);
        m_scanProgress->setTextVisible(false);
        lay->addWidget(m_scanStatusLabel);
        lay->addWidget(m_scanProgress, 1);
    }
    m_scanStatusLabel->setText(msg);
    m_scanOverlay->setGeometry(0, height() - 40, width(), 40);
    m_scanOverlay->show();
    m_scanOverlay->raise();
    qDebug() << "[MainWindow] Scan indicator:" << msg;
}

void MainWindow::hideScanIndicator()
{
    if (m_scanShowTimer) m_scanShowTimer->stop();
    if (m_scanOverlay && m_scanOverlay->isVisible()) {
        m_scanOverlay->hide();
        qDebug() << "[MainWindow] Scan indicator hidden";
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Lazy view creation — each ensure* creates the view on first call
// ═════════════════════════════════════════════════════════════════════

NowPlayingView* MainWindow::ensureNowPlayingView()
{
    // Always created in setupUI
    return m_nowPlayingView;
}

LibraryView* MainWindow::ensureLibraryView()
{
    if (!m_libraryView) {
        m_libraryView = new LibraryView();
        m_viewStack->addWidget(m_libraryView);
        connect(m_libraryView, &LibraryView::albumClicked,
                this, &MainWindow::onAlbumSelected);
        connect(m_libraryView, &LibraryView::artistClicked,
                this, &MainWindow::onArtistSelected);
    }
    return m_libraryView;
}

AlbumsView* MainWindow::ensureAlbumsView()
{
    if (!m_albumsView) {
        m_albumsView = new AlbumsView();
        m_viewStack->addWidget(m_albumsView);
        connect(m_albumsView, &AlbumsView::albumSelected,
                this, &MainWindow::onAlbumSelected);
    }
    return m_albumsView;
}

AlbumDetailView* MainWindow::ensureAlbumDetailView()
{
    if (!m_albumDetailView) {
        m_albumDetailView = new AlbumDetailView();
        m_viewStack->addWidget(m_albumDetailView);
        connect(m_albumDetailView, &AlbumDetailView::backRequested,
                this, &MainWindow::onBackFromAlbumDetail);
        connect(m_albumDetailView, &AlbumDetailView::artistClicked,
                this, &MainWindow::onArtistSelected);
    }
    return m_albumDetailView;
}

ArtistsView* MainWindow::ensureArtistsView()
{
    if (!m_artistsView) {
        m_artistsView = new ArtistsView();
        m_viewStack->addWidget(m_artistsView);
        connect(m_artistsView, &ArtistsView::artistSelected,
                this, &MainWindow::onArtistSelected);
    }
    return m_artistsView;
}

ArtistDetailView* MainWindow::ensureArtistDetailView()
{
    if (!m_artistDetailView) {
        m_artistDetailView = new ArtistDetailView();
        m_viewStack->addWidget(m_artistDetailView);
        connect(m_artistDetailView, &ArtistDetailView::backRequested,
                this, &MainWindow::onBackFromArtistDetail);
        connect(m_artistDetailView, &ArtistDetailView::albumSelected,
                this, &MainWindow::onAlbumSelected);
    }
    return m_artistDetailView;
}

PlaylistsView* MainWindow::ensurePlaylistsView()
{
    if (!m_playlistsView) {
        m_playlistsView = new PlaylistsView();
        m_viewStack->addWidget(m_playlistsView);
        connect(m_playlistsView, &PlaylistsView::playlistSelected,
                this, &MainWindow::onPlaylistSelected);
    }
    return m_playlistsView;
}

PlaylistDetailView* MainWindow::ensurePlaylistDetailView()
{
    if (!m_playlistDetailView) {
        m_playlistDetailView = new PlaylistDetailView();
        m_viewStack->addWidget(m_playlistDetailView);
        connect(m_playlistDetailView, &PlaylistDetailView::backRequested,
                this, &MainWindow::onBackFromPlaylistDetail);
    }
    return m_playlistDetailView;
}

AppleMusicView* MainWindow::ensureAppleMusicView()
{
    if (!m_appleMusicView) {
        m_appleMusicView = new AppleMusicView();
        m_viewStack->addWidget(m_appleMusicView);
    }
    return m_appleMusicView;
}

FolderBrowserView* MainWindow::ensureFolderBrowserView()
{
    if (!m_folderBrowserView) {
        m_folderBrowserView = new FolderBrowserView();
        m_viewStack->addWidget(m_folderBrowserView);
        connect(m_folderBrowserView, &FolderBrowserView::albumSelected,
                this, &MainWindow::onAlbumSelected);
        connect(m_folderBrowserView, &FolderBrowserView::artistSelected,
                this, &MainWindow::onArtistSelected);
    }
    return m_folderBrowserView;
}

/* TODO: restore when Tidal API available
TidalView* MainWindow::ensureTidalView()
{
    if (!m_tidalView) {
        m_tidalView = new TidalView();
        m_viewStack->addWidget(m_tidalView);
    }
    return m_tidalView;
}
*/

QueueView* MainWindow::ensureQueueView()
{
    if (!m_queueView) {
        m_queueView = new QueueView();
        m_viewStack->addWidget(m_queueView);
    }
    return m_queueView;
}

SettingsView* MainWindow::ensureSettingsView()
{
    if (!m_settingsView) {
        m_settingsView = new SettingsView();
        m_viewStack->addWidget(m_settingsView);
    }
    return m_settingsView;
}

SearchResultsView* MainWindow::ensureSearchResultsView()
{
    if (!m_searchResultsView) {
        m_searchResultsView = new SearchResultsView();
        m_viewStack->addWidget(m_searchResultsView);
        connect(m_searchResultsView, &SearchResultsView::artistClicked,
                this, &MainWindow::onArtistSelected);
        connect(m_searchResultsView, &SearchResultsView::albumClicked,
                this, &MainWindow::onAlbumSelected);
    }
    return m_searchResultsView;
}

void MainWindow::onSearch(const QString& query)
{
    auto tracks  = LibraryDatabase::instance()->searchTracks(query);
    auto albums  = LibraryDatabase::instance()->searchAlbums(query);
    auto artists = LibraryDatabase::instance()->searchArtists(query);

    // Only save previous view when first entering search (not on each keystroke)
    if (m_viewStack->currentWidget() != ensureSearchResultsView()) {
        m_previousView = m_viewStack->currentWidget();
    }
    m_searchResultsView->setResults(query, artists, albums, tracks);
    m_viewStack->setCurrentWidget(m_searchResultsView);
}

void MainWindow::onSearchCleared()
{
    if (m_searchResultsView && m_viewStack->currentWidget() == m_searchResultsView) {
        m_searchResultsView->clearResults();
        if (m_previousView) {
            m_viewStack->setCurrentWidget(m_previousView);
            m_sidebar->setActiveIndex(sidebarIndexForView(m_previousView));
        } else {
            m_viewStack->setCurrentWidget(ensureNowPlayingView());
            m_sidebar->setActiveIndex(0);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Navigation — uses setCurrentWidget (no hardcoded indices)
// ═════════════════════════════════════════════════════════════════════

void MainWindow::onNavigationChanged(int index)
{
    // Push current view to history (unless we're already navigating programmatically)
    if (!m_navigating && m_currentNavIndex >= 0 && m_currentNavIndex != index) {
        m_viewHistory.push(m_currentNavIndex);
        m_viewForwardHistory.clear();
    }

    m_currentNavIndex = index;

    switch (index) {
        case 0: m_viewStack->setCurrentWidget(ensureNowPlayingView()); break;
        case 1: m_viewStack->setCurrentWidget(ensureLibraryView());    break;
        case 2: m_viewStack->setCurrentWidget(ensureAlbumsView());     break;
        case 3: m_viewStack->setCurrentWidget(ensureArtistsView());    break;
        case 4: m_viewStack->setCurrentWidget(ensurePlaylistsView());  break;
        case 5: m_viewStack->setCurrentWidget(ensureAppleMusicView());   break;
        case 6: m_viewStack->setCurrentWidget(ensureFolderBrowserView()); break;
        // case 7: m_viewStack->setCurrentWidget(ensureTidalView()); break;  // TODO: restore when Tidal API available
        case 9: m_viewStack->setCurrentWidget(ensureSettingsView());   break;
    }
    m_sidebar->setActiveIndex(index);
    emit globalNavChanged();
}

// ═════════════════════════════════════════════════════════════════════
//  Global navigation — back / forward
// ═════════════════════════════════════════════════════════════════════

void MainWindow::navigateBack()
{
    if (m_viewHistory.isEmpty()) return;

    m_viewForwardHistory.push(m_currentNavIndex);

    m_navigating = true;
    int prev = m_viewHistory.pop();
    onNavigationChanged(prev);
    m_navigating = false;
}

void MainWindow::navigateForward()
{
    if (m_viewForwardHistory.isEmpty()) return;

    m_viewHistory.push(m_currentNavIndex);

    m_navigating = true;
    int next = m_viewForwardHistory.pop();
    onNavigationChanged(next);
    m_navigating = false;
}

bool MainWindow::canGoBack() const
{
    return !m_viewHistory.isEmpty();
}

bool MainWindow::canGoForward() const
{
    return !m_viewForwardHistory.isEmpty();
}

void MainWindow::onAlbumSelected(const QString& albumId)
{
    m_previousView = m_viewStack->currentWidget();
    ensureAlbumDetailView()->setAlbum(albumId);
    m_viewStack->setCurrentWidget(m_albumDetailView);
    m_sidebar->setActiveIndex(2); // Albums
}

void MainWindow::onArtistSelected(const QString& artistId)
{
    m_previousView = m_viewStack->currentWidget();
    ensureArtistDetailView()->setArtist(artistId);
    m_viewStack->setCurrentWidget(m_artistDetailView);
    m_sidebar->setActiveIndex(3); // Artists
}

void MainWindow::onPlaylistSelected(const QString& playlistId)
{
    m_previousView = m_viewStack->currentWidget();
    ensurePlaylistDetailView()->setPlaylist(playlistId);
    m_viewStack->setCurrentWidget(m_playlistDetailView);
    m_sidebar->setActiveIndex(4); // Playlists
}

void MainWindow::onBackFromAlbumDetail()
{
    if (m_previousView) {
        m_viewStack->setCurrentWidget(m_previousView);
        m_sidebar->setActiveIndex(sidebarIndexForView(m_previousView));
    } else {
        m_viewStack->setCurrentWidget(ensureAlbumsView());
        m_sidebar->setActiveIndex(2);
    }
}

void MainWindow::onBackFromArtistDetail()
{
    if (m_previousView) {
        m_viewStack->setCurrentWidget(m_previousView);
        m_sidebar->setActiveIndex(sidebarIndexForView(m_previousView));
    } else {
        m_viewStack->setCurrentWidget(ensureArtistsView());
        m_sidebar->setActiveIndex(3);
    }
}

void MainWindow::onBackFromPlaylistDetail()
{
    if (m_previousView) {
        m_viewStack->setCurrentWidget(m_previousView);
        m_sidebar->setActiveIndex(sidebarIndexForView(m_previousView));
    } else {
        m_viewStack->setCurrentWidget(ensurePlaylistsView());
        m_sidebar->setActiveIndex(4);
    }
}

int MainWindow::sidebarIndexForView(QWidget* view)
{
    if (view == m_nowPlayingView)      return 0;
    if (view == m_libraryView)         return 1;
    if (view == m_albumsView)          return 2;
    if (view == m_albumDetailView)     return 2;
    if (view == m_artistsView)         return 3;
    if (view == m_artistDetailView)    return 3;
    if (view == m_playlistsView)       return 4;
    if (view == m_playlistDetailView)  return 4;
    if (view == m_appleMusicView)      return 5;
    if (view == m_folderBrowserView)  return 6;
    // if (view == m_tidalView)           return 7;  // TODO: restore when Tidal API available
    if (view == m_queueView)           return 1;
    if (view == m_settingsView)        return 1;
    if (view == m_searchResultsView)   return -1;
    return 0;
}

void MainWindow::onQueueToggled(bool visible)
{
    if (visible) {
        m_previousView = m_viewStack->currentWidget();
        m_viewStack->setCurrentWidget(ensureQueueView());
    } else if (m_previousView) {
        m_viewStack->setCurrentWidget(m_previousView);
    }
}

void MainWindow::onFolderSelected(const QString& folderPath)
{
    m_viewStack->setCurrentWidget(ensureLibraryView());
    m_sidebar->setActiveIndex(1);
    m_libraryView->filterByFolder(folderPath);
}

// ── eventFilter (app-level) ──────────────────────────────────────
bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            // Dismiss search results view globally (any focus context)
            if (m_searchResultsView && m_viewStack->currentWidget() == m_searchResultsView) {
                m_sidebar->clearSearch();
                return true;   // consumed
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ── currentContentWidget ──────────────────────────────────────────
QWidget* MainWindow::currentContentWidget() const
{
    return m_viewStack ? m_viewStack->currentWidget() : nullptr;
}

// ── keyPressEvent ─────────────────────────────────────────────────
void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (!isTextInputFocused()) {
        switch (event->key()) {
        case Qt::Key_Space: {
            // Check playback SOURCE (not view) to decide which player to control
            auto* ps = PlaybackState::instance();
            if (ps->currentSource() == PlaybackState::AppleMusic) {
                MusicKitPlayer::instance()->togglePlayPause();
            } else {
                ps->playPause();
            }
            event->accept();
            return;
        }
        case Qt::Key_Left:
            if (event->modifiers() & Qt::ControlModifier) {
                PlaybackState::instance()->previous();
                event->accept();
                return;
            }
            break;
        case Qt::Key_Right:
            if (event->modifiers() & Qt::ControlModifier) {
                PlaybackState::instance()->next();
                event->accept();
                return;
            }
            break;
        case Qt::Key_Up:
            if (event->modifiers() & Qt::ControlModifier) {
                PlaybackState::instance()->setVolume(
                    qMin(100, PlaybackState::instance()->volume() + 5));
                event->accept();
                return;
            }
            break;
        case Qt::Key_Down:
            if (event->modifiers() & Qt::ControlModifier) {
                PlaybackState::instance()->setVolume(
                    qMax(0, PlaybackState::instance()->volume() - 5));
                event->accept();
                return;
            }
            break;
        }
    }
    QMainWindow::keyPressEvent(event);
}

// ── resizeEvent ───────────────────────────────────────────────────
void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);

    // Auto-collapse sidebar when window is narrow
    if (width() < 1050 && !m_sidebar->isCollapsed()) {
        m_sidebar->toggleCollapse();
    }
}

// ── closeEvent ─────────────────────────────────────────────────────
void MainWindow::closeEvent(QCloseEvent* event)
{
    qDebug() << "[MainWindow] closeEvent — hiding window, playback continues";
    Settings::instance()->setWindowGeometry(saveGeometry());
    hide();
    event->ignore();  // Do NOT quit — just hide
}

// ── performQuit — full cleanup, called from aboutToQuit ────────────
void MainWindow::performQuit()
{
    qDebug() << "=== MainWindow performQuit START ===";
    VST3Host::instance()->closeAllEditors();
    QThread::msleep(50);
#ifdef Q_OS_MAC
    MacMediaIntegration::instance().clearNowPlaying();
#endif
    auto* engine = AudioEngine::instance();
    engine->blockSignals(true);
    engine->stop();
    engine->blockSignals(false);
    MusicKitPlayer::instance()->cleanup();
    QThread::msleep(100);
    VST3Host::instance()->unloadAll();
    QThread::msleep(50);
    qDebug() << "=== MainWindow performQuit DONE ===";
}
