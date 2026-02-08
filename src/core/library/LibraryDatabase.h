#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QDateTime>
#include <QVector>
#include <optional>

#include "../MusicData.h"

class LibraryDatabase : public QObject {
    Q_OBJECT

public:
    static LibraryDatabase* instance();
    ~LibraryDatabase() override;

    bool open();
    void close();

    // ── Tracks ───────────────────────────────────────────────────────
    bool trackExists(const QString& filePath) const;
    void removeDuplicates();
    bool insertTrack(const Track& track);
    bool updateTrack(const Track& track);
    bool updateTrackMetadata(const QString& trackId,
                             const QString& title, const QString& artist,
                             const QString& album,
                             const QString& recordingMbid,
                             const QString& artistMbid,
                             const QString& albumMbid,
                             const QString& releaseGroupMbid);
    bool removeTrack(const QString& id);
    bool removeTrackByPath(const QString& filePath);
    std::optional<Track> trackById(const QString& id) const;
    std::optional<Track> trackByPath(const QString& filePath) const;
    QVector<Track> allTracks() const;
    QVector<Track> searchTracks(const QString& query) const;
    int trackCount() const;

    // ── Albums ───────────────────────────────────────────────────────
    bool insertAlbum(const Album& album);
    bool updateAlbum(const Album& album);
    QVector<Album> allAlbums() const;
    Album albumById(const QString& id) const;
    QVector<Album> searchAlbums(const QString& query) const;

    // ── Artists ──────────────────────────────────────────────────────
    bool insertArtist(const Artist& artist);
    bool updateArtist(const Artist& artist);
    QVector<Artist> allArtists() const;
    Artist artistById(const QString& id) const;
    QVector<Artist> searchArtists(const QString& query) const;

    // ── Playlists ────────────────────────────────────────────────────
    bool insertPlaylist(const Playlist& playlist);
    bool updatePlaylist(const Playlist& playlist);
    bool removePlaylist(const QString& id);
    QVector<Playlist> allPlaylists() const;
    Playlist playlistById(const QString& id) const;

    bool addTrackToPlaylist(const QString& playlistId, const QString& trackId, int position = -1);
    bool removeTrackFromPlaylist(const QString& playlistId, const QString& trackId);
    bool reorderPlaylistTrack(const QString& playlistId, int fromPos, int toPos);

    // ── Play History ─────────────────────────────────────────────────
    void recordPlay(const QString& trackId);
    QVector<Track> recentlyPlayed(int limit = 50) const;
    QVector<Track> mostPlayed(int limit = 50) const;
    QVector<Track> recentlyAdded(int limit = 50) const;

    // ── Volume Leveling ───────────────────────────────────────────────
    void updateR128Loudness(const QString& filePath, double loudness, double peak);

    // ── MBID helpers ──────────────────────────────────────────────────
    QString releaseGroupMbidForAlbum(const QString& albumId) const;
    QString artistMbidForArtist(const QString& artistId) const;

    // ── Metadata backup / undo ──────────────────────────────────────
    void backupTrackMetadata(const QString& trackId);
    bool undoLastMetadataChange(const QString& trackId);
    bool hasMetadataBackup(const QString& trackId) const;

    // ── Transaction helpers ──────────────────────────────────────────
    bool beginTransaction();
    bool commitTransaction();

    // ── Rebuild helpers ──────────────────────────────────────────────
    void rebuildAlbumsAndArtists();

    // ── Database backup / rollback ───────────────────────────────────
    bool createBackup();
    bool restoreFromBackup();
    bool hasBackup() const;
    QDateTime backupTimestamp() const;

signals:
    void databaseChanged();

private:
    explicit LibraryDatabase(QObject* parent = nullptr);
    void createTables();
    void createIndexes();

    QString generateId() const;
    Track trackFromQuery(const QSqlQuery& query) const;
    AudioFormat audioFormatFromString(const QString& str) const;
    QString audioFormatToString(AudioFormat fmt) const;

    QSqlDatabase m_db;
    QString m_dbPath;
};
