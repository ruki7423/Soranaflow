#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QStack>

class StyledInput;
class StyledButton;
class QWebEngineView;
class QWebEngineProfile;

class TidalView : public QWidget {
    Q_OBJECT
public:
    explicit TidalView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onSearch();
    void onSearchResults(const QJsonObject& results);
    void onArtistTopTracks(const QString& artistId, const QJsonArray& tracks);
    void onArtistAlbums(const QString& artistId, const QJsonArray& albums);
    void onAlbumTracks(const QString& albumId, const QJsonArray& tracks);
    void onError(const QString& error);

private:
    // ── Navigation ───────────────────────────────────────────────────
    enum class TidalViewState { Search, ArtistDetail, AlbumDetail };

    struct NavEntry {
        TidalViewState state;
        QString searchTerm;
        QJsonArray tracks;
        QJsonArray albums;
        QJsonArray artists;
        QString detailId;
        QString detailName;
        QString detailSubName;
    };

    void pushNavState();
    void navigateBack();
    void navigateForward();
    void restoreNavEntry(const NavEntry& entry);
    void updateNavBar();

    QStack<NavEntry> m_backStack;
    QStack<NavEntry> m_forwardStack;
    TidalViewState m_currentState = TidalViewState::Search;

    // Cached data
    QString m_lastSearchTerm;
    QJsonArray m_lastTracks;
    QJsonArray m_lastAlbums;
    QJsonArray m_lastArtists;
    QString m_currentDetailId;
    QString m_currentDetailName;
    QString m_currentDetailSubName;

    // ── UI Setup ─────────────────────────────────────────────────────
    void setupUI();
    void clearResults();
    void buildTracksSection(const QJsonArray& tracks);
    void buildAlbumsSection(const QJsonArray& albums);
    void buildArtistsSection(const QJsonArray& artists);
    QWidget* createTrackRow(const QJsonObject& track);
    QWidget* createAlbumCard(const QJsonObject& album, int cardWidth);
    QWidget* createArtistCard(const QJsonObject& artist, int cardWidth);
    QWidget* createSectionHeader(const QString& title);
    void loadArtwork(const QString& url, QLabel* target, int size, bool circular = false);
    void showArtistDetail(const QString& artistId, const QString& artistName);
    void showAlbumDetail(const QString& albumId, const QString& albumName, const QString& artistName);
    void playTrack(const QJsonObject& track);
    void stopPreview();
    void initPreviewWebView();
    void updateAuthStatus();
    void refreshTheme();

    // Header
    QLabel* m_titleLabel = nullptr;
    QLabel* m_authStatusLabel = nullptr;
    StyledButton* m_connectBtn = nullptr;

    // Navigation bar
    QPushButton* m_backBtn = nullptr;
    QPushButton* m_forwardBtn = nullptr;
    QLabel* m_navTitleLabel = nullptr;
    QWidget* m_navBar = nullptr;

    // Search
    StyledInput* m_searchInput = nullptr;
    StyledButton* m_searchBtn = nullptr;

    // Loading / results
    QLabel* m_loadingLabel = nullptr;
    QLabel* m_noResultsLabel = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_resultsContainer = nullptr;
    QVBoxLayout* m_resultsLayout = nullptr;

    QNetworkAccessManager* m_networkManager = nullptr;

    // Preview playback (hidden WebEngineView)
    QWebEngineView* m_previewWebView = nullptr;
    bool m_previewSdkReady = false;
    QString m_currentPreviewTrackId;
    bool m_isPlaying = false;

    // Browse WebView (listen.tidal.com — replaces API search)
    QWebEngineProfile* m_browseProfile = nullptr;
    QWebEngineView* m_browseWebView = nullptr;
};
