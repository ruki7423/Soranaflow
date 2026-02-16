#include "MainWindow.h"
#include "MenuBarManager.h"
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
#include "../core/MusicData.h"
#include "../core/CoverArtLoader.h"
#include <QtConcurrent>
#include "../core/ThemeManager.h"
#ifdef Q_OS_MAC
#include "../platform/macos/MacMediaIntegration.h"
#endif
#include "dialogs/StyledMessageBox.h"
#include <QFileInfo>
#include <QDebug>
#include <QTimer>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QMenuBar>
#include <QThread>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QPropertyAnimation>

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

    // One-time migration: clear stale Stretch-era header widths (v1.5.1 → Interactive)
    {
        QSettings settings(Settings::settingsPath(), QSettings::IniFormat);
        if (!settings.value(QStringLiteral("migrations/headerStretchFixed")).toBool()) {
            settings.remove(QStringLiteral("trackTable"));
            settings.setValue(QStringLiteral("migrations/headerStretchFixed"), true);
            settings.sync();
            qDebug() << "[Migration] Cleared stale header state from Stretch era";
        }
    }

    setupUI();
    connectSignals();

    setWindowTitle("Sorana Flow");
    resize(1400, 900);
    setMinimumSize(900, 600);

    // Menu bar, keyboard shortcuts, macOS media integration
    auto* menuMgr = new MenuBarManager(this);
    connect(menuMgr, &MenuBarManager::quitRequested, this, [this]() {
        qDebug() << "[MainWindow] Cmd+Q — real quit requested";
        m_reallyQuit = true;
        close();
    });
    connect(menuMgr, &MenuBarManager::focusSearchRequested, this, [this]() {
        m_sidebar->focusSearch();
    });

    // Global Escape: install app-level event filter to catch Escape
    // before child widgets (QTableView, QScrollArea) consume it
    qApp->installEventFilter(this);

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

    // Inline scan progress bar (hidden by default)
    m_scanBar = new QWidget();
    m_scanBar->setFixedHeight(32);
    m_scanBar->setVisible(false);
    auto* scanLay = new QHBoxLayout(m_scanBar);
    scanLay->setContentsMargins(16, 0, 16, 0);
    scanLay->setSpacing(12);
    m_scanLabel = new QLabel;
    m_scanLabel->setFixedWidth(220);
    m_scanProgressBar = new QProgressBar;
    m_scanProgressBar->setFixedHeight(4);
    m_scanProgressBar->setTextVisible(false);
    scanLay->addWidget(m_scanLabel);
    scanLay->addWidget(m_scanProgressBar, 1);
    rightLayout->addWidget(m_scanBar, 0);

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

    // Sidebar search -> global search results view (debounced 200ms)
    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(200);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, [this]() {
        onSearch(m_pendingSearchQuery);
    });
    connect(m_sidebar, &AppSidebar::searchRequested,
            this, [this](const QString& query) {
        if (query.trimmed().isEmpty()) {
            m_searchDebounceTimer->stop();
            m_pendingSearchQuery.clear();
            onSearchCleared();
        } else {
            m_pendingSearchQuery = query.trimmed();
            m_searchDebounceTimer->start();  // restarts 200ms countdown
        }
    });

    // ── Scan progress bar ──────────────────────────────────────────────
    auto* scanner = LibraryScanner::instance();
    auto* db = LibraryDatabase::instance();

    // Delayed show — only display bar if scan takes >500ms (prevents flash)
    m_scanShowTimer = new QTimer(this);
    m_scanShowTimer->setSingleShot(true);

    // Auto-hide timer for completion toast
    m_scanHideTimer = new QTimer(this);
    m_scanHideTimer->setSingleShot(true);
    connect(m_scanHideTimer, &QTimer::timeout, this, [this]() {
        hideScanBar();
    });

    connect(scanner, &LibraryScanner::scanStarted, this, [this]() {
        // Start delay timer — bar appears after 500ms if still scanning
        m_scanShowTimer->start(500);
    });
    connect(m_scanShowTimer, &QTimer::timeout, this, [this]() {
        showScanBar(0, 0);  // indeterminate until first progress signal
    });

    connect(scanner, &LibraryScanner::scanProgress, this, [this](int current, int total) {
        if (m_scanShowTimer->isActive()) {
            // First progress arrived before 500ms — show bar now
            m_scanShowTimer->stop();
        }
        showScanBar(current, total);
    });

    // Debounce timer for library reloads — coalesces rapid signals into one reload
    m_libraryReloadTimer = new QTimer(this);
    m_libraryReloadTimer->setSingleShot(true);
    m_libraryReloadTimer->setInterval(2000);
    connect(m_libraryReloadTimer, &QTimer::timeout, this, &MainWindow::doLibraryReload);

    connect(scanner, &LibraryScanner::scanFinished, this, [this](int totalTracks) {
        m_scanShowTimer->stop();
        m_libraryReloadTimer->stop();  // Cancel pending debounce
        showScanComplete(totalTracks);
        // Final reload with albums/artists (m_scanning is now false)
        MusicDataProvider::instance()->reloadFromDatabase();
    });

    // Progressive playback — debounce reloads during scan (max 1 per 2s)
    connect(scanner, &LibraryScanner::batchReady, this, [this](int processed, int total) {
        Q_UNUSED(processed); Q_UNUSED(total);
        if (!m_libraryReloadTimer->isActive()) {
            m_libraryReloadTimer->start();
        }
    });

    connect(db, &LibraryDatabase::rebuildStarted, this, [this]() {
        if (!m_scanShowTimer->isActive() && (!m_scanBar || !m_scanBar->isVisible()))
            m_scanShowTimer->start(500);
        else if (m_scanBar && m_scanBar->isVisible()) {
            m_scanLabel->setText(QStringLiteral("Rebuilding library..."));
            m_scanProgressBar->setRange(0, 0);  // indeterminate
        }
    });
    connect(db, &LibraryDatabase::rebuildFinished, this, [this]() {
        hideScanBar();
    });

    // Theme-aware styling
    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        updateScanBarTheme();
    });
    updateScanBarTheme();
}

// ═════════════════════════════════════════════════════════════════════
//  doLibraryReload — debounced library reload during scan
// ═════════════════════════════════════════════════════════════════════

void MainWindow::doLibraryReload()
{
    qDebug() << "[ProgressiveScan] Debounced reload firing";
    MusicDataProvider::instance()->reloadFromDatabase();
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
            QStringList failedPlugins;
            for (const QString& path : paths) {
                bool isVst2 = path.endsWith(QStringLiteral(".vst"));
                std::shared_ptr<IDSPProcessor> proc;
                if (isVst2)
                    proc = vst2host->createProcessorFromPath(path.toStdString());
                else
                    proc = vst3host->createProcessorFromPath(path.toStdString());
                if (!proc) {
                    qDebug() << "[STARTUP] VST load FAILED:" << path;
                    failedPlugins.append(QFileInfo(path).completeBaseName());
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

            // Restore saved plugin states
            if (loaded > 0 && pipeline) {
                auto* settings = Settings::instance();
                for (int i = 0; i < pipeline->processorCount(); ++i) {
                    auto* proc = pipeline->processor(i);
                    if (!proc) continue;

                    std::string plugPath = proc->getPluginPath();
                    if (plugPath.empty()) continue;

                    QString path = QString::fromStdString(plugPath);
                    QString key = QStringLiteral("vst/pluginStates/")
                        + QString::fromUtf8(path.toUtf8().toBase64());
                    QByteArray stateB64 = settings->value(key).toByteArray();
                    if (stateB64.isEmpty()) continue;

                    QByteArray state = QByteArray::fromBase64(stateB64);
                    if (state.isEmpty()) continue;

                    bool ok = proc->restoreState(state);
                    if (ok) {
                        qDebug() << "[STARTUP] Restored plugin state:"
                                 << QString::fromStdString(proc->getName())
                                 << "(" << state.size() << "bytes)";
                    } else {
                        qWarning() << "[STARTUP] Failed to restore state for"
                                   << QString::fromStdString(proc->getName());
                    }
                }
            }

            if (!failedPlugins.isEmpty()) {
                QTimer::singleShot(500, this, [this, failedPlugins]() {
                    StyledMessageBox::warning(this, QStringLiteral("VST Plugin Loading"),
                        QStringLiteral("The following plugins could not be loaded:\n\n%1\n\n"
                        "They may be incompatible, damaged, or blocked by macOS security.\n"
                        "Try right-clicking each plugin in Finder → Open to allow it.")
                            .arg(failedPlugins.join(QStringLiteral("\n"))));
                });
            }
        }
    }

    qDebug() << "[STARTUP] Deferred init complete";
}

// ═════════════════════════════════════════════════════════════════════
//  Inline scan progress bar
// ═════════════════════════════════════════════════════════════════════

void MainWindow::showScanBar(int current, int total)
{
    m_scanHideTimer->stop();
    if (total > 0) {
        m_scanProgressBar->setRange(0, total);
        m_scanProgressBar->setValue(current);
        int pct = static_cast<int>(100.0 * current / total);
        m_scanLabel->setText(QStringLiteral("Scanning: %1 / %2 files (%3%)")
                                 .arg(current).arg(total).arg(pct));
    } else {
        m_scanProgressBar->setRange(0, 0);  // indeterminate
        m_scanLabel->setText(QStringLiteral("Scanning library..."));
    }
    if (!m_scanBar->isVisible()) {
        m_scanBar->setMaximumHeight(32);
        m_scanBar->setVisible(true);
        qDebug() << "[MainWindow] Scan bar shown";
    }
}

void MainWindow::showScanComplete(int totalTracks)
{
    m_scanProgressBar->setRange(0, 100);
    m_scanProgressBar->setValue(100);
    m_scanLabel->setText(QStringLiteral("Library scan complete \u2014 %1 tracks").arg(totalTracks));
    if (!m_scanBar->isVisible()) {
        m_scanBar->setMaximumHeight(32);
        m_scanBar->setVisible(true);
    }
    m_scanHideTimer->start(3000);
    qDebug() << "[MainWindow] Scan complete:" << totalTracks << "tracks";
}

void MainWindow::hideScanBar()
{
    if (m_scanShowTimer) m_scanShowTimer->stop();
    if (m_scanHideTimer) m_scanHideTimer->stop();
    if (m_scanBar && m_scanBar->isVisible()) {
        auto* anim = new QPropertyAnimation(m_scanBar, "maximumHeight");
        anim->setDuration(300);
        anim->setStartValue(32);
        anim->setEndValue(0);
        connect(anim, &QPropertyAnimation::finished, this, [this]() {
            m_scanBar->setVisible(false);
            m_scanBar->setMaximumHeight(32);
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
        qDebug() << "[MainWindow] Scan bar hiding (animated)";
    }
}

void MainWindow::updateScanBarTheme()
{
    if (!m_scanBar) return;
    auto c = ThemeManager::instance()->colors();
    m_scanBar->setStyleSheet(
        QStringLiteral("QWidget { background: %1; border-top: 1px solid %2; }")
            .arg(c.backgroundSecondary, c.borderSubtle));
    m_scanLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 12px; border: none; background: transparent; }")
            .arg(c.foregroundSecondary));
    m_scanProgressBar->setStyleSheet(
        QStringLiteral("QProgressBar { background: %1; border: none; border-radius: 2px; }"
                       "QProgressBar::chunk { background: %2; border-radius: 2px; }")
            .arg(c.progressTrack, c.accent));
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
    int gen = ++m_searchGeneration;

    // Only save previous view when first entering search (not on each keystroke)
    if (m_viewStack->currentWidget() != ensureSearchResultsView()) {
        m_previousView = m_viewStack->currentWidget();
    }
    m_viewStack->setCurrentWidget(m_searchResultsView);

    // Run DB queries off main thread
    (void)QtConcurrent::run([this, query, gen]() {
        auto tracks  = LibraryDatabase::instance()->searchTracks(query);
        auto albums  = LibraryDatabase::instance()->searchAlbums(query);
        auto artists = LibraryDatabase::instance()->searchArtists(query);

        QMetaObject::invokeMethod(this, [this, gen, query,
                tracks  = std::move(tracks),
                albums  = std::move(albums),
                artists = std::move(artists)]() {
            if (gen != m_searchGeneration) return;  // stale result
            if (!m_searchResultsView) return;       // view destroyed
            m_searchResultsView->setResults(query, artists, albums, tracks);
        }, Qt::QueuedConnection);
    });
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
    Settings::instance()->setWindowGeometry(saveGeometry());

    if (m_reallyQuit) {
        qDebug() << "[MainWindow] closeEvent — real quit (Cmd+Q)";
        event->accept();
        qApp->quit();
    } else {
        qDebug() << "[MainWindow] closeEvent — hiding window, playback continues";
        hide();
        event->ignore();
    }
}

// ── performQuit — full cleanup, called from aboutToQuit ────────────
void MainWindow::performQuit()
{
    qDebug() << "=== MainWindow performQuit START ===";

    // Stop scanner thread first (it writes to DB)
    LibraryScanner::instance()->stopScan();
    qDebug() << "[SHUTDOWN] Scanner stop requested";

    VST3Host::instance()->closeAllEditors();
    QThread::msleep(50);

    // Save all VST plugin states before unloading
    {
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        auto* settings = Settings::instance();
        if (pipeline) {
            for (int i = 0; i < pipeline->processorCount(); ++i) {
                auto* proc = pipeline->processor(i);
                if (!proc) continue;

                std::string plugPath = proc->getPluginPath();
                if (plugPath.empty()) continue;

                QByteArray state = proc->saveState();
                if (state.isEmpty()) continue;

                QString path = QString::fromStdString(plugPath);
                QString key = QStringLiteral("vst/pluginStates/")
                    + QString::fromUtf8(path.toUtf8().toBase64());
                settings->setValue(key, state.toBase64());

                qDebug() << "[SHUTDOWN] Saved plugin state:"
                         << QString::fromStdString(proc->getName())
                         << "(" << state.size() << "bytes)";
            }
        }
    }

#ifdef Q_OS_MAC
    MacMediaIntegration::instance().clearNowPlaying();
    qDebug() << "[SHUTDOWN] MacMedia cleared";
#endif

    // Stop MusicKit BEFORE audio engine (WKWebView stopLoading prevents hang)
    MusicKitPlayer::instance()->cleanup();

    auto* engine = AudioEngine::instance();
    engine->blockSignals(true);
    engine->prepareForShutdown();
    engine->blockSignals(false);

    QThread::msleep(100);
    VST3Host::instance()->unloadAll();
    QThread::msleep(50);
    qDebug() << "=== MainWindow performQuit DONE ===";
}
