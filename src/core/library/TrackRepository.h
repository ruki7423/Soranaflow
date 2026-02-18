#pragma once

#include <QHash>
#include <QPair>
#include <QVector>
#include <QString>
#include <optional>
#include "../MusicData.h"

struct DatabaseContext;

class TrackRepository {
public:
    explicit TrackRepository(DatabaseContext* ctx);

    // ── Existence / metadata ─────────────────────────────────────────
    bool trackExists(const QString& filePath) const;
    QHash<QString, QPair<qint64, qint64>> allTrackFileMeta() const;  // path -> (size, mtime)

    // ── Cleanup ──────────────────────────────────────────────────────
    bool removeDuplicates();       // returns true if data changed
    bool clearAllData(bool preservePlaylists = true);  // returns true always (caller emits signal)

    // ── CRUD ─────────────────────────────────────────────────────────
    bool insertTrack(const Track& track,
                     const QString& resolvedArtistId,
                     const QString& resolvedAlbumId);
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

    // ── Queries ──────────────────────────────────────────────────────
    std::optional<Track> trackById(const QString& id) const;
    std::optional<Track> trackByPath(const QString& filePath) const;
    QVector<Track> allTracks() const;
    QVector<TrackIndex> allTrackIndexes() const;
    QVector<Track> searchTracks(const QString& query) const;
    QVector<QString> searchTracksFTS(const QString& query) const;
    int trackCount() const;

    // ── FTS ──────────────────────────────────────────────────────────
    void rebuildFTSIndex();

    // ── Volume Leveling ──────────────────────────────────────────────
    void updateR128Loudness(const QString& filePath, double loudness, double peak);
    void updateReplayGain(const QString& filePath,
                          double trackGainDb, double albumGainDb,
                          double trackPeakLinear, double albumPeakLinear);

    // ── Play History ─────────────────────────────────────────────────
    void recordPlay(const QString& trackId);
    QVector<Track> recentlyPlayed(int limit = 50) const;
    QVector<Track> mostPlayed(int limit = 50) const;
    QVector<Track> recentlyAdded(int limit = 50) const;

    // ── Metadata Backup / Undo ───────────────────────────────────────
    void backupTrackMetadata(const QString& trackId);
    bool undoLastMetadataChange(const QString& trackId);
    bool hasMetadataBackup(const QString& trackId) const;

private:
    DatabaseContext* m_ctx;
};
