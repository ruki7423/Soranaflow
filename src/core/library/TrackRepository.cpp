#include "TrackRepository.h"
#include "DatabaseContext.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QSet>

// ── Korean jamo normalization for FTS5 search ────────────────────────
// Compatibility jamo (U+3131-U+3163) are standalone characters that don't
// match composed Hangul syllables in FTS5.  Convert them to conjoining jamo
// (Choseong U+1100+ / Jungseong U+1161+) so NFC can compose syllable blocks.
// E.g. ㄱ(3131)+ㅓ(3153) → ᄀ(1100)+ᅥ(1165) → NFC → 거(AC70)
static QString normalizeKoreanForSearch(const QString& input)
{
    QString s = input;
    s.remove(QLatin1Char('\\'));  // strip stray backslashes (macOS IME artifact)
    if (s.isEmpty()) return s;

    bool hasCompatJamo = false;
    for (const QChar& ch : s) {
        ushort c = ch.unicode();
        if (c >= 0x3131 && c <= 0x3163) { hasCompatJamo = true; break; }
    }
    if (!hasCompatJamo) return s;

    // Consonant → Choseong map (index = code - 0x3131, 0 = no mapping)
    static const ushort kToChoseong[30] = {
        0x1100, 0x1101, 0,      0x1102, 0,      0,      // ㄱㄲㄳㄴㄵㄶ
        0x1103, 0x1104, 0x1105, 0,      0,      0,      // ㄷㄸㄹㄺㄻㄼ
        0,      0,      0,      0,      0x1106, 0x1107,  // ㄽㄾㄿㅀㅁㅂ
        0x1108, 0,      0x1109, 0x110A, 0x110B, 0x110C,  // ㅃㅄㅅㅆㅇㅈ
        0x110D, 0x110E, 0x110F, 0x1110, 0x1111, 0x1112   // ㅉㅊㅋㅌㅍㅎ
    };

    QString mapped;
    mapped.reserve(s.size());
    for (const QChar& ch : s) {
        ushort c = ch.unicode();
        if (c >= 0x3131 && c <= 0x314E) {
            ushort m = kToChoseong[c - 0x3131];
            mapped.append(m ? QChar(m) : ch);
        } else if (c >= 0x314F && c <= 0x3163) {
            // Vowels: sequential mapping ㅏ(314F)→ᅡ(1161) ... ㅣ(3163)→ᅵ(1175)
            mapped.append(QChar(static_cast<ushort>(0x1161 + (c - 0x314F))));
        } else {
            mapped.append(ch);
        }
    }

    return mapped.normalized(QString::NormalizationForm_C);
}

// ── String pool for deduplicating artist/album names ─────────────────
namespace {
class StringPool {
public:
    const QString& intern(const QString& s) {
        auto it = m_pool.find(s);
        if (it != m_pool.end()) return *it;
        auto result = m_pool.insert(s);
        return *result;
    }
    int uniqueCount() const { return m_pool.size(); }
private:
    QSet<QString> m_pool;
};
} // anonymous namespace

// ── Constructor ──────────────────────────────────────────────────────
TrackRepository::TrackRepository(DatabaseContext* ctx)
    : m_ctx(ctx)
{
}

// ── Existence / metadata ─────────────────────────────────────────────
bool TrackRepository::trackExists(const QString& filePath) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() > 0;
    }
    return false;
}

QHash<QString, QPair<qint64, qint64>> TrackRepository::allTrackFileMeta() const
{
    QMutexLocker lock(m_ctx->readMutex);
    QElapsedTimer t; t.start();
    QHash<QString, QPair<qint64, qint64>> result;
    QSqlQuery q(*m_ctx->readDb);
    q.exec(QStringLiteral("SELECT file_path, file_size, file_mtime FROM tracks"));
    result.reserve(10000);
    while (q.next()) {
        result.insert(q.value(0).toString(),
                      qMakePair(q.value(1).toLongLong(), q.value(2).toLongLong()));
    }
    qDebug() << "[TIMING] allTrackFileMeta:" << result.size() << "entries in" << t.elapsed() << "ms";
    return result;
}

// ── Cleanup ──────────────────────────────────────────────────────────
bool TrackRepository::removeDuplicates()
{
    QMutexLocker lock(m_ctx->writeMutex);
    QElapsedTimer t; t.start();
    qDebug() << "=== TrackRepository::removeDuplicates ===";

    QSqlQuery countBefore(*m_ctx->writeDb);
    countBefore.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    int before = 0;
    if (countBefore.next()) before = countBefore.value(0).toInt();
    qDebug() << "  Tracks before cleanup:" << before;

    // 1) Remove exact duplicates by file_path (keep first inserted)
    QSqlQuery q1(*m_ctx->writeDb);
    q1.exec(QStringLiteral(
        "DELETE FROM tracks WHERE id NOT IN ("
        "  SELECT MIN(id) FROM tracks GROUP BY file_path"
        ")"
    ));
    qDebug() << "  Removed by file_path:" << q1.numRowsAffected();

    // 2) Remove duplicates by metadata match (title+artist+album+duration)
    QSqlQuery q2(*m_ctx->writeDb);
    q2.exec(QStringLiteral(
        "DELETE FROM tracks WHERE id NOT IN ("
        "  SELECT MIN(id) FROM tracks "
        "  GROUP BY LOWER(title), LOWER(artist), LOWER(album), CAST(duration AS INTEGER)"
        ")"
    ));
    qDebug() << "  Removed by metadata match:" << q2.numRowsAffected();

    // 3) Remove tracks whose files no longer exist on disk
    QSqlQuery selectAll(*m_ctx->writeDb);
    selectAll.exec(QStringLiteral("SELECT id, file_path FROM tracks"));

    QStringList toRemove;
    while (selectAll.next()) {
        QString id = selectAll.value(0).toString();
        QString path = selectAll.value(1).toString();
        if (!path.isEmpty() && !QFile::exists(path)) {
            toRemove.append(id);
        }
    }

    if (!toRemove.isEmpty()) {
        for (const QString& id : toRemove) {
            QSqlQuery del(*m_ctx->writeDb);
            del.prepare(QStringLiteral("DELETE FROM tracks WHERE id = ?"));
            del.addBindValue(id);
            del.exec();
        }
        qDebug() << "  Removed missing files:" << toRemove.size();
    }

    QSqlQuery countAfter(*m_ctx->writeDb);
    countAfter.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    int after = 0;
    if (countAfter.next()) after = countAfter.value(0).toInt();
    qDebug() << "  Tracks after cleanup:" << after;
    qDebug() << "  Total removed:" << (before - after);
    qDebug() << "[TIMING] removeDuplicates:" << t.elapsed() << "ms";
    qDebug() << "=== Duplicate removal complete ===";

    return (before != after);
}

bool TrackRepository::clearAllData(bool preservePlaylists)
{
    QMutexLocker lock(m_ctx->writeMutex);
    qDebug() << "=== TrackRepository::clearAllData ===" << "preservePlaylists:" << preservePlaylists;

    QSqlQuery q(*m_ctx->writeDb);
    m_ctx->writeDb->transaction();
    q.exec(QStringLiteral("DELETE FROM play_history"));
    q.exec(QStringLiteral("DELETE FROM metadata_backups"));
    if (!preservePlaylists) {
        q.exec(QStringLiteral("DELETE FROM playlist_tracks"));
        q.exec(QStringLiteral("DELETE FROM playlists"));
    }
    q.exec(QStringLiteral("DELETE FROM tracks"));
    q.exec(QStringLiteral("DELETE FROM albums"));
    q.exec(QStringLiteral("DELETE FROM artists"));
    m_ctx->writeDb->commit();
    q.exec(QStringLiteral("VACUUM"));

    qDebug() << "=== clearAllData complete ===";
    return true;
}

// ── CRUD ─────────────────────────────────────────────────────────────
bool TrackRepository::insertTrack(const Track& track,
                                  const QString& resolvedArtistId,
                                  const QString& resolvedAlbumId)
{
    QMutexLocker lock(m_ctx->writeMutex);

    QSqlQuery q(*m_ctx->writeDb);
    if (!q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO tracks "
        "(id, title, artist, album, album_id, artist_id, duration, format, "
        "sample_rate, bit_depth, bitrate, cover_url, track_number, disc_number, file_path, "
        "recording_mbid, artist_mbid, album_mbid, release_group_mbid, channel_count, "
        "file_size, file_mtime, album_artist, year, "
        "replay_gain_track, replay_gain_album, replay_gain_track_peak, replay_gain_album_peak, has_replay_gain, "
        "composer) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    ))) {
        qWarning() << "TrackRepository::insertTrack PREPARE failed:" << q.lastError().text();
        return false;
    }

    QString id = track.id.isEmpty() ? m_ctx->generateId() : track.id;
    q.addBindValue(id);
    q.addBindValue(track.title);
    q.addBindValue(track.artist);
    q.addBindValue(track.album);
    q.addBindValue(resolvedAlbumId);
    q.addBindValue(resolvedArtistId);
    q.addBindValue(track.duration);
    q.addBindValue(m_ctx->audioFormatToString(track.format));
    q.addBindValue(track.sampleRate);
    q.addBindValue(track.bitDepth);
    q.addBindValue(track.bitrate);
    q.addBindValue(track.coverUrl);
    q.addBindValue(track.trackNumber);
    q.addBindValue(track.discNumber);
    q.addBindValue(track.filePath);
    q.addBindValue(track.recordingMbid);
    q.addBindValue(track.artistMbid);
    q.addBindValue(track.albumMbid);
    q.addBindValue(track.releaseGroupMbid);
    q.addBindValue(track.channelCount);
    q.addBindValue(track.fileSize);
    q.addBindValue(track.fileMtime);
    q.addBindValue(track.albumArtist);
    q.addBindValue(track.year);
    q.addBindValue(track.replayGainTrack);
    q.addBindValue(track.replayGainAlbum);
    q.addBindValue(track.replayGainTrackPeak);
    q.addBindValue(track.replayGainAlbumPeak);
    q.addBindValue(track.hasReplayGain ? 1 : 0);
    q.addBindValue(track.composer);

    if (!q.exec()) {
        qWarning() << "TrackRepository::insertTrack failed:" << q.lastError().text();
        return false;
    }

    return true;
}

bool TrackRepository::updateTrack(const Track& track)
{
    QMutexLocker lock(m_ctx->writeMutex);
    if (track.id.isEmpty()) {
        qWarning() << "TrackRepository::updateTrack - track has no ID";
        return false;
    }

    qDebug() << "=== TrackRepository::updateTrack ===";
    qDebug() << "  ID:" << track.id;
    qDebug() << "  Title:" << track.title;
    qDebug() << "  Artist:" << track.artist;
    qDebug() << "  Album:" << track.album;
    qDebug() << "  FilePath:" << track.filePath;
    qDebug() << "  Recording MBID:" << track.recordingMbid;
    qDebug() << "  Artist MBID:" << track.artistMbid;
    qDebug() << "  Album MBID:" << track.albumMbid;
    qDebug() << "  ReleaseGroup MBID:" << track.releaseGroupMbid;

    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "UPDATE tracks SET "
        "title = ?, artist = ?, album = ?, album_id = ?, artist_id = ?, "
        "duration = ?, format = ?, sample_rate = ?, bit_depth = ?, bitrate = ?, "
        "cover_url = ?, track_number = ?, disc_number = ?, file_path = ?, "
        "recording_mbid = ?, artist_mbid = ?, album_mbid = ?, release_group_mbid = ?, "
        "channel_count = ?, file_size = ?, file_mtime = ?, album_artist = ?, year = ?, "
        "replay_gain_track = ?, replay_gain_album = ?, replay_gain_track_peak = ?, "
        "replay_gain_album_peak = ?, has_replay_gain = ?, composer = ? "
        "WHERE id = ?"
    ));

    q.addBindValue(track.title);
    q.addBindValue(track.artist);
    q.addBindValue(track.album);
    q.addBindValue(track.albumId);
    q.addBindValue(track.artistId);
    q.addBindValue(track.duration);
    q.addBindValue(m_ctx->audioFormatToString(track.format));
    q.addBindValue(track.sampleRate);
    q.addBindValue(track.bitDepth);
    q.addBindValue(track.bitrate);
    q.addBindValue(track.coverUrl);
    q.addBindValue(track.trackNumber);
    q.addBindValue(track.discNumber);
    q.addBindValue(track.filePath);
    q.addBindValue(track.recordingMbid);
    q.addBindValue(track.artistMbid);
    q.addBindValue(track.albumMbid);
    q.addBindValue(track.releaseGroupMbid);
    q.addBindValue(track.channelCount);
    q.addBindValue(track.fileSize);
    q.addBindValue(track.fileMtime);
    q.addBindValue(track.albumArtist);
    q.addBindValue(track.year);
    q.addBindValue(track.replayGainTrack);
    q.addBindValue(track.replayGainAlbum);
    q.addBindValue(track.replayGainTrackPeak);
    q.addBindValue(track.replayGainAlbumPeak);
    q.addBindValue(track.hasReplayGain ? 1 : 0);
    q.addBindValue(track.composer);
    q.addBindValue(track.id);

    if (!q.exec()) {
        qWarning() << "  UPDATE FAILED:" << q.lastError().text();
        return false;
    }

    int rows = q.numRowsAffected();
    qDebug() << "  UPDATE SUCCESS: rows affected:" << rows;

    return (rows > 0);
}

bool TrackRepository::updateTrackMetadata(const QString& trackId,
                                           const QString& title,
                                           const QString& artist,
                                           const QString& album,
                                           const QString& recordingMbid,
                                           const QString& artistMbid,
                                           const QString& albumMbid,
                                           const QString& releaseGroupMbid)
{
    QMutexLocker lock(m_ctx->writeMutex);
    if (trackId.isEmpty()) {
        qWarning() << "TrackRepository::updateTrackMetadata - empty track ID";
        return false;
    }

    qDebug() << "=== TrackRepository::updateTrackMetadata ===";
    qDebug() << "  ID:" << trackId;
    qDebug() << "  Title:" << title << "Artist:" << artist << "Album:" << album;
    qDebug() << "  MBIDs: rec=" << recordingMbid << "artist=" << artistMbid
             << "album=" << albumMbid << "rg=" << releaseGroupMbid;

    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "UPDATE tracks SET "
        "title = ?, artist = ?, album = ?, "
        "recording_mbid = ?, artist_mbid = ?, album_mbid = ?, release_group_mbid = ? "
        "WHERE id = ?"
    ));

    q.addBindValue(title);
    q.addBindValue(artist);
    q.addBindValue(album);
    q.addBindValue(recordingMbid);
    q.addBindValue(artistMbid);
    q.addBindValue(albumMbid);
    q.addBindValue(releaseGroupMbid);
    q.addBindValue(trackId);

    if (!q.exec()) {
        qWarning() << "  UPDATE FAILED:" << q.lastError().text();
        return false;
    }

    int rows = q.numRowsAffected();
    qDebug() << "  UPDATE SUCCESS: rows affected:" << rows;
    return rows > 0;
}

bool TrackRepository::removeTrack(const QString& id)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE id = ?"));
    q.addBindValue(id);
    return q.exec();
}

bool TrackRepository::removeTrackByPath(const QString& filePath)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    return q.exec();
}

// ── Queries ──────────────────────────────────────────────────────────
std::optional<Track> TrackRepository::trackById(const QString& id) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral("SELECT * FROM tracks WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return m_ctx->trackFromQuery(q);
    }
    return std::nullopt;
}

std::optional<Track> TrackRepository::trackByPath(const QString& filePath) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral("SELECT * FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    if (q.exec() && q.next()) {
        return m_ctx->trackFromQuery(q);
    }
    return std::nullopt;
}

QVector<Track> TrackRepository::allTracks() const
{
    QMutexLocker lock(m_ctx->readMutex);
    QElapsedTimer t; t.start();
    QVector<Track> result;
    QSqlQuery q(*m_ctx->readDb);
    q.exec(QStringLiteral("SELECT * FROM tracks ORDER BY artist, album, disc_number, track_number"));
    while (q.next()) {
        result.append(m_ctx->trackFromQuery(q));
    }
    qDebug() << "[TIMING] allTracks (FULL):" << result.size() << "tracks in" << t.elapsed() << "ms";
    return result;
}

QVector<TrackIndex> TrackRepository::allTrackIndexes() const
{
    QMutexLocker lock(m_ctx->readMutex);
    QElapsedTimer t; t.start();
    QVector<TrackIndex> result;
    StringPool pool;  // deduplicates artist/album names (~60% memory savings)

    QSqlQuery q(*m_ctx->readDb);
    q.exec(QStringLiteral(
        "SELECT id, title, artist, album, duration, format, sample_rate, bit_depth, "
        "track_number, disc_number, file_path, r128_loudness, r128_peak, album_artist, composer "
        "FROM tracks ORDER BY artist, album, disc_number, track_number"));
    result.reserve(100000);
    while (q.next()) {
        TrackIndex ti;
        ti.id          = q.value(0).toString();
        ti.title       = q.value(1).toString();
        ti.artist      = pool.intern(q.value(2).toString());  // pooled
        ti.album       = pool.intern(q.value(3).toString());  // pooled
        ti.duration    = q.value(4).toInt();
        ti.format      = m_ctx->audioFormatFromString(q.value(5).toString());
        ti.sampleRate  = q.value(6).toString();
        ti.bitDepth    = q.value(7).toString();
        ti.trackNumber = q.value(8).toInt();
        ti.discNumber  = q.value(9).toInt();
        ti.filePath    = q.value(10).toString();
        ti.r128Loudness = q.value(11).toDouble();
        ti.r128Peak     = q.value(12).toDouble();
        ti.hasR128      = (ti.r128Loudness != 0.0);
        ti.albumArtist  = pool.intern(q.value(13).toString());  // pooled
        ti.composer     = pool.intern(q.value(14).toString());  // pooled
        result.append(std::move(ti));
    }
    qDebug() << "[TIMING] allTrackIndexes:" << result.size() << "tracks in" << t.elapsed() << "ms";
    qDebug() << "[LibraryDB] Loaded" << result.size() << "track indexes,"
             << "unique strings:" << pool.uniqueCount();
    return result;
}

QVector<Track> TrackRepository::searchTracks(const QString& query) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<Track> result;
    if (query.isEmpty()) return result;

    // Normalize Korean jamo to composed syllables, strip backslashes
    QString normalized = normalizeKoreanForSearch(query);
    if (normalized.isEmpty()) return result;

    if (normalized.length() >= 2) {
        // Use FTS5 for 2+ char queries (< 1ms vs table scan)
        QString ftsQuery = normalized;
        ftsQuery.replace(QLatin1Char('\''), QStringLiteral("''"));
        ftsQuery += QStringLiteral("*");

        QSqlQuery q(*m_ctx->readDb);
        q.prepare(QStringLiteral(
            "SELECT t.* FROM tracks t "
            "INNER JOIN tracks_fts f ON t.rowid = f.rowid "
            "WHERE tracks_fts MATCH :query "
            "ORDER BY rank LIMIT 200"
        ));
        q.bindValue(QStringLiteral(":query"), ftsQuery);

        if (q.exec()) {
            while (q.next()) {
                result.append(m_ctx->trackFromQuery(q));
            }
        }
        qDebug() << "[LibraryDB] FTS5 search:" << normalized << "->" << result.size() << "tracks";
    } else {
        // 1 char: fallback to LIKE (FTS5 too broad for single chars)
        QSqlQuery q(*m_ctx->readDb);
        q.prepare(QStringLiteral(
            "SELECT * FROM tracks WHERE "
            "title LIKE ? OR artist LIKE ? OR album LIKE ? "
            "ORDER BY artist, album, track_number"
        ));
        QString pattern = QStringLiteral("%%1%").arg(normalized);
        q.addBindValue(pattern);
        q.addBindValue(pattern);
        q.addBindValue(pattern);

        if (q.exec()) {
            while (q.next()) {
                result.append(m_ctx->trackFromQuery(q));
            }
        }
    }
    return result;
}

QVector<QString> TrackRepository::searchTracksFTS(const QString& query) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<QString> ids;
    if (query.isEmpty()) return ids;

    // Normalize Korean jamo to composed syllables, strip backslashes
    QString normalized = normalizeKoreanForSearch(query);
    if (normalized.isEmpty()) return ids;

    // FTS5 query: prefix search with *
    QString ftsQuery = normalized;
    ftsQuery.replace(QLatin1Char('\''), QStringLiteral("''"));
    ftsQuery += QStringLiteral("*");

    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral(
        "SELECT t.id FROM tracks t "
        "INNER JOIN tracks_fts f ON t.rowid = f.rowid "
        "WHERE tracks_fts MATCH :query "
        "ORDER BY rank LIMIT 5000"
    ));
    q.bindValue(QStringLiteral(":query"), ftsQuery);

    if (q.exec()) {
        ids.reserve(1000);
        while (q.next()) {
            ids.append(q.value(0).toString());
        }
    }
    qDebug() << "[LibraryDB] FTS5 search:" << normalized << "->" << ids.size() << "results";
    return ids;
}

int TrackRepository::trackCount() const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
    q.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    if (q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

// ── FTS ──────────────────────────────────────────────────────────────
void TrackRepository::rebuildFTSIndex()
{
    QMutexLocker lock(m_ctx->writeMutex);
    QElapsedTimer t; t.start();
    QSqlQuery q(*m_ctx->writeDb);
    m_ctx->writeDb->transaction();
    q.exec(QStringLiteral("DELETE FROM tracks_fts"));
    q.exec(QStringLiteral(
        "INSERT INTO tracks_fts(rowid, title, artist, album, composer) "
        "SELECT rowid, title, artist, album, composer FROM tracks"
    ));
    m_ctx->writeDb->commit();
    qDebug() << "[TIMING] rebuildFTSIndex internal:" << t.elapsed() << "ms";
    qDebug() << "[LibraryDB] FTS5 index rebuilt";
}

// ── Volume Leveling ──────────────────────────────────────────────────
void TrackRepository::updateR128Loudness(const QString& filePath, double loudness, double peak)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "UPDATE tracks SET r128_loudness = ?, r128_peak = ? WHERE file_path = ?"));
    q.addBindValue(loudness);
    q.addBindValue(peak);
    q.addBindValue(filePath);
    if (!q.exec()) {
        qWarning() << "TrackRepository::updateR128Loudness failed:" << q.lastError().text();
    }
}

void TrackRepository::updateReplayGain(const QString& filePath,
                                       double trackGainDb, double albumGainDb,
                                       double trackPeakLinear, double albumPeakLinear)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "UPDATE tracks SET "
        "replay_gain_track = ?, replay_gain_album = ?, "
        "replay_gain_track_peak = ?, replay_gain_album_peak = ?, "
        "has_replay_gain = 1 "
        "WHERE file_path = ?"));
    q.addBindValue(trackGainDb);
    q.addBindValue(albumGainDb);
    q.addBindValue(trackPeakLinear);
    q.addBindValue(albumPeakLinear);
    q.addBindValue(filePath);
    if (!q.exec()) {
        qWarning() << "TrackRepository::updateReplayGain failed:" << q.lastError().text();
    }
}

// ── Play History ─────────────────────────────────────────────────────
void TrackRepository::recordPlay(const QString& trackId)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral("INSERT INTO play_history (track_id) VALUES (?)"));
    q.addBindValue(trackId);
    q.exec();

    // Increment play_count
    QSqlQuery q2(*m_ctx->writeDb);
    q2.prepare(QStringLiteral("UPDATE tracks SET play_count = play_count + 1 WHERE id = ?"));
    q2.addBindValue(trackId);
    q2.exec();
}

QVector<Track> TrackRepository::recentlyPlayed(int limit) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<Track> result;
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral(
        "SELECT DISTINCT t.* FROM tracks t "
        "JOIN play_history ph ON t.id = ph.track_id "
        "ORDER BY ph.played_at DESC LIMIT ?"
    ));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append(m_ctx->trackFromQuery(q));
        }
    }
    return result;
}

QVector<Track> TrackRepository::mostPlayed(int limit) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<Track> result;
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral(
        "SELECT * FROM tracks WHERE play_count > 0 "
        "ORDER BY play_count DESC LIMIT ?"
    ));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append(m_ctx->trackFromQuery(q));
        }
    }
    return result;
}

QVector<Track> TrackRepository::recentlyAdded(int limit) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<Track> result;
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral(
        "SELECT * FROM tracks ORDER BY added_at DESC LIMIT ?"
    ));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append(m_ctx->trackFromQuery(q));
        }
    }
    return result;
}

// ── Metadata Backup / Undo ───────────────────────────────────────────
void TrackRepository::backupTrackMetadata(const QString& trackId)
{
    QMutexLocker lock(m_ctx->writeMutex);
    if (trackId.isEmpty()) return;

    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "INSERT INTO metadata_backups "
        "(track_id, title, artist, album, track_number, disc_number, "
        " recording_mbid, artist_mbid, album_mbid, release_group_mbid) "
        "SELECT id, title, artist, album, track_number, disc_number, "
        "       recording_mbid, artist_mbid, album_mbid, release_group_mbid "
        "FROM tracks WHERE id = ?"));
    q.addBindValue(trackId);
    if (q.exec()) {
        qDebug() << "[LibraryDB] Backed up metadata for track:" << trackId;
    } else {
        qWarning() << "[LibraryDB] Failed to backup metadata:" << q.lastError().text();
    }
}

bool TrackRepository::undoLastMetadataChange(const QString& trackId)
{
    QMutexLocker lock(m_ctx->writeMutex);
    if (trackId.isEmpty()) return false;

    // Get the most recent backup
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "SELECT title, artist, album, track_number, disc_number, "
        "       recording_mbid, artist_mbid, album_mbid, release_group_mbid "
        "FROM metadata_backups WHERE track_id = ? "
        "ORDER BY id DESC LIMIT 1"));
    q.addBindValue(trackId);

    if (!q.exec() || !q.next()) {
        qDebug() << "[LibraryDB] No metadata backup found for track:" << trackId;
        return false;
    }

    // Restore
    QSqlQuery up(*m_ctx->writeDb);
    up.prepare(QStringLiteral(
        "UPDATE tracks SET title = ?, artist = ?, album = ?, "
        "track_number = ?, disc_number = ?, "
        "recording_mbid = ?, artist_mbid = ?, album_mbid = ?, release_group_mbid = ? "
        "WHERE id = ?"));
    up.addBindValue(q.value(0));  // title
    up.addBindValue(q.value(1));  // artist
    up.addBindValue(q.value(2));  // album
    up.addBindValue(q.value(3));  // track_number
    up.addBindValue(q.value(4));  // disc_number
    up.addBindValue(q.value(5));  // recording_mbid
    up.addBindValue(q.value(6));  // artist_mbid
    up.addBindValue(q.value(7));  // album_mbid
    up.addBindValue(q.value(8));  // release_group_mbid
    up.addBindValue(trackId);

    if (!up.exec()) {
        qWarning() << "[LibraryDB] Failed to undo metadata:" << up.lastError().text();
        return false;
    }

    // Remove the used backup
    QSqlQuery del(*m_ctx->writeDb);
    del.prepare(QStringLiteral("DELETE FROM metadata_backups WHERE track_id = ? "
        "AND id = (SELECT MAX(id) FROM metadata_backups WHERE track_id = ?)"));
    del.addBindValue(trackId);
    del.addBindValue(trackId);
    del.exec();

    qDebug() << "[LibraryDB] Restored metadata for track:" << trackId;
    return true;
}

bool TrackRepository::hasMetadataBackup(const QString& trackId) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM metadata_backups WHERE track_id = ?"));
    q.addBindValue(trackId);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() > 0;
    }
    return false;
}
