#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QDateTime>
#include <QVector>
#include <QHash>
#include <QMutex>
#include <optional>
#include <atomic>

#include "../MusicData.h"
#include "DatabaseContext.h"
#include "TrackRepository.h"
#include "AlbumRepository.h"
#include "ArtistRepository.h"
#include "PlaylistRepository.h"

class LibraryDatabase : public QObject {
    Q_OBJECT

public:
    static LibraryDatabase* instance();
    explicit LibraryDatabase(const QString& dbPath, QObject* parent = nullptr);
    ~LibraryDatabase() override;

    bool open();
    void close();

    // ── Tracks ───────────────────────────────────────────────────────
    bool trackExists(const QString& filePath) const;
    QHash<QString, QPair<qint64, qint64>> allTrackFileMeta() const;  // path → (size, mtime)
    void removeDuplicates();
    void clearAllData(bool preservePlaylists = true);
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
    QVector<TrackIndex> allTrackIndexes() const;
    QVector<Track> searchTracks(const QString& query) const;
    QVector<QString> searchTracksFTS(const QString& query) const;
    void rebuildFTSIndex();
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
    void updateReplayGain(const QString& filePath,
                          double trackGainDb, double albumGainDb,
                          double trackPeakLinear, double albumPeakLinear);

    // ── MBID helpers ──────────────────────────────────────────────────
    QString releaseGroupMbidForAlbum(const QString& albumId) const;
    QString artistMbidForArtist(const QString& artistId) const;

    // ── Cover art helpers ─────────────────────────────────────────────
    QString firstTrackPathForAlbum(const QString& albumId) const;

    // ── Metadata backup / undo ──────────────────────────────────────
    void backupTrackMetadata(const QString& trackId);
    bool undoLastMetadataChange(const QString& trackId);
    bool hasMetadataBackup(const QString& trackId) const;

    // ── Transaction helpers ──────────────────────────────────────────
    bool beginTransaction();
    bool commitTransaction();

    // ── Rebuild helpers ──────────────────────────────────────────────
    void rebuildAlbumsAndArtists();

    // ── Incremental album/artist management ─────────────────────────
    QString findOrCreateArtist(const QString& artistName);
    QString findOrCreateAlbum(const QString& albumTitle, const QString& artistName, const QString& artistId);
    void updateAlbumStatsIncremental(const QString& albumId);
    void updateAlbumsAndArtistsForTrack(const Track& track);
    void cleanOrphanedAlbumsAndArtists();
    void clearIncrementalCaches();
    void refreshAlbumMetadataFromTracks(const QString& albumId);

    // ── Database backup / rollback ───────────────────────────────────
    bool createBackup();
    bool restoreFromBackup();
    bool hasBackup() const;
    QDateTime backupTimestamp() const;

signals:
    void databaseChanged();
    void rebuildStarted();
    void rebuildFinished();

private:
    explicit LibraryDatabase(QObject* parent = nullptr);
    void createTables();
    void createIndexes();
    void verifyFTSIndex();
    void doRebuildInternal();
    bool createBackup_nolock();  // called under existing m_writeMutex

    QSqlDatabase m_db;        // write connection (scanner, inserts, updates)
    QSqlDatabase m_readDb;    // read connection (MDP, search, UI queries)
    QString m_dbPath;
    mutable QRecursiveMutex m_writeMutex;  // protects m_db (writes)
    mutable QRecursiveMutex m_readMutex;   // protects m_readDb (reads)
    std::atomic<bool> m_rebuildPending{false};

    // Batch mode: skip incremental album/artist work during bulk scan
    bool m_batchMode = false;

    // Incremental caches
    QHash<QString, QString> m_artistNameToIdCache;  // lowercase name → artistId
    QHash<QString, QString> m_albumKeyToIdCache;     // "album||artist" → albumId

    // Repository delegation
    DatabaseContext m_ctx;
    TrackRepository* m_trackRepo = nullptr;
    AlbumRepository* m_albumRepo = nullptr;
    ArtistRepository* m_artistRepo = nullptr;
    PlaylistRepository* m_playlistRepo = nullptr;
};
