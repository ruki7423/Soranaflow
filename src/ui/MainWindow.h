#pragma once
#include <QMainWindow>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShortcut>
#include <QStack>
#include <QLabel>
#include <QProgressBar>
#include "AppSidebar.h"
#include "PlaybackBar.h"

// Forward declarations — views created lazily
class NowPlayingView;
class LibraryView;
class AlbumsView;
class AlbumDetailView;
class ArtistsView;
class ArtistDetailView;
class PlaylistsView;
class PlaylistDetailView;
class QueueView;
class SettingsView;
class AppleMusicView;
// class TidalView;  // TODO: restore when Tidal API available
class FolderBrowserView;
class SearchResultsView;
class QKeyEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    static MainWindow* instance();

    void initializeDeferred();
    void performQuit();

    // Returns current visible content widget (for keyboard handling)
    QWidget* currentContentWidget() const;

    // Global navigation — called by view toolbar buttons
    void navigateBack();
    void navigateForward();
    bool canGoBack() const;
    bool canGoForward() const;

signals:
    void globalNavChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onNavigationChanged(int index);
    void onAlbumSelected(const QString& albumId);
    void onArtistSelected(const QString& artistId);
    void onPlaylistSelected(const QString& playlistId);
    void onBackFromAlbumDetail();
    void onBackFromArtistDetail();
    void onBackFromPlaylistDetail();
    void onQueueToggled(bool visible);
    void onFolderSelected(const QString& folderPath);

private:
    void setupUI();
    void connectSignals();
    int sidebarIndexForView(QWidget* view);

    // Lazy view accessors — create on first use
    NowPlayingView*    ensureNowPlayingView();
    LibraryView*       ensureLibraryView();
    AlbumsView*        ensureAlbumsView();
    AlbumDetailView*   ensureAlbumDetailView();
    ArtistsView*       ensureArtistsView();
    ArtistDetailView*  ensureArtistDetailView();
    PlaylistsView*     ensurePlaylistsView();
    PlaylistDetailView* ensurePlaylistDetailView();
    AppleMusicView*    ensureAppleMusicView();
    // TidalView*         ensureTidalView();  // TODO: restore when Tidal API available
    FolderBrowserView* ensureFolderBrowserView();
    QueueView*         ensureQueueView();
    SettingsView*      ensureSettingsView();
    SearchResultsView* ensureSearchResultsView();
    void onSearch(const QString& query);
    void onSearchCleared();

    // Layout
    AppSidebar* m_sidebar = nullptr;
    QStackedWidget* m_viewStack = nullptr;
    PlaybackBar* m_playbackBar = nullptr;

    // Views — created lazily (nullptr until needed)
    NowPlayingView* m_nowPlayingView = nullptr;
    LibraryView* m_libraryView = nullptr;
    AlbumsView* m_albumsView = nullptr;
    AlbumDetailView* m_albumDetailView = nullptr;
    ArtistsView* m_artistsView = nullptr;
    ArtistDetailView* m_artistDetailView = nullptr;
    PlaylistsView* m_playlistsView = nullptr;
    PlaylistDetailView* m_playlistDetailView = nullptr;
    AppleMusicView* m_appleMusicView = nullptr;
    // TidalView* m_tidalView = nullptr;  // TODO: restore when Tidal API available
    FolderBrowserView* m_folderBrowserView = nullptr;
    QueueView* m_queueView = nullptr;
    SettingsView* m_settingsView = nullptr;
    SearchResultsView* m_searchResultsView = nullptr;

    QWidget* m_previousView = nullptr;
    bool m_initialized = false;

    // Scan progress indicator
    QWidget* m_scanOverlay = nullptr;
    QLabel* m_scanStatusLabel = nullptr;
    QProgressBar* m_scanProgress = nullptr;
    QTimer* m_scanShowTimer = nullptr;
    QString m_pendingScanMsg;
    void showScanIndicator(const QString& msg);
    void hideScanIndicator();

    // Global navigation history
    QStack<int> m_viewHistory;
    QStack<int> m_viewForwardHistory;
    int m_currentNavIndex = -1;
    bool m_navigating = false;  // prevent recursive history pushes

    static MainWindow* s_instance;
};
