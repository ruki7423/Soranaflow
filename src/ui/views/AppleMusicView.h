#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonObject>
#include <QStack>
#include <QStackedWidget>

class StyledInput;
class StyledButton;
class AMSearchPanel;
class AMArtistPanel;
class AMAlbumPanel;

class AppleMusicView : public QWidget {
    Q_OBJECT
public:
    explicit AppleMusicView(QWidget* parent = nullptr);

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
        QString detailSubName;
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
    void updateAuthStatus();
    void showArtistDiscography(const QString& artistId, const QString& artistName);
    void showAlbumTracks(const QString& albumId, const QString& albumName, const QString& artistName);
    void playSong(const QJsonObject& song);
    void showSongContextMenu(const QPoint& globalPos, const QJsonObject& song);
    void refreshTheme();
    void wirePanelSignals(QWidget* panel);

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

    // Content panels
    QStackedWidget* m_stackedWidget = nullptr;
    AMSearchPanel* m_searchPanel = nullptr;
    AMArtistPanel* m_artistPanel = nullptr;
    AMAlbumPanel* m_albumPanel = nullptr;

    // Music User Token
    QString m_musicUserToken;
};
