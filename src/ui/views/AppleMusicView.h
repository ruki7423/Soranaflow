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
#include <QFontMetrics>

class StyledInput;
class StyledButton;

class AppleMusicView : public QWidget {
    Q_OBJECT
public:
    explicit AppleMusicView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onSearch();
    void onSearchResults(const QJsonArray& songs, const QJsonArray& albums, const QJsonArray& artists);
    void onArtistSongs(const QString& artistId, const QJsonArray& songs);
    void onArtistAlbums(const QString& artistId, const QJsonArray& albums);
    void onAlbumTracks(const QString& albumId, const QJsonArray& tracks);
    void onError(const QString& error);

private:
    // ── Navigation ───────────────────────────────────────────────────
    enum class AMViewState { Search, ArtistDetail, AlbumDetail };

    struct NavEntry {
        AMViewState state;
        QString searchTerm;
        QJsonArray songs;
        QJsonArray albums;
        QJsonArray artists;
        QString detailId;
        QString detailName;
        QString detailSubName;  // artist name for album views
    };

    void pushNavState();
    void navigateBack();
    void navigateForward();
    void restoreNavEntry(const NavEntry& entry);
    void updateNavBar();

    QStack<NavEntry> m_backStack;
    QStack<NavEntry> m_forwardStack;
    AMViewState m_currentState = AMViewState::Search;

    // Cached data for current view
    QString m_lastSearchTerm;
    QJsonArray m_lastSongs;
    QJsonArray m_lastAlbums;
    QJsonArray m_lastArtists;
    QString m_currentDetailId;
    QString m_currentDetailName;
    QString m_currentDetailSubName;

    // ── UI Setup ─────────────────────────────────────────────────────
    void setupUI();
    void clearResults();
    void buildSongsSection(const QJsonArray& songs);
    void buildAlbumsSection(const QJsonArray& albums);
    void buildArtistsSection(const QJsonArray& artists);
    QWidget* createSongRow(const QJsonObject& song);
    QWidget* createAlbumCard(const QJsonObject& album, int cardWidth);
    QWidget* createArtistCard(const QJsonObject& artist, int cardWidth);
    QWidget* createSectionHeader(const QString& title);
    void loadArtwork(const QString& url, QLabel* target, int size, bool circular = false);
    void updateAuthStatus();
    void showArtistDiscography(const QString& artistId, const QString& artistName);
    void showAlbumTracks(const QString& albumId, const QString& albumName, const QString& artistName);
    void playSong(const QJsonObject& song);
    void showSongContextMenu(QWidget* songRow, const QPoint& globalPos);
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

    // Music User Token
    QString m_musicUserToken;

    // Custom double-click detection (immune to macOS activation timing)
    QObject* m_lastClickedRow = nullptr;
    qint64 m_lastClickTime = 0;
    qint64 m_lastPlayTime = 0;
};
