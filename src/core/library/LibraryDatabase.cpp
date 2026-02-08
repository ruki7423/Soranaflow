#include "LibraryDatabase.h"

#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QUuid>
#include <QDateTime>
#include <QVariant>
#include <QElapsedTimer>

// ── Singleton ───────────────────────────────────────────────────────
LibraryDatabase* LibraryDatabase::instance()
{
    static LibraryDatabase s;
    return &s;
}

LibraryDatabase::LibraryDatabase(QObject* parent)
    : QObject(parent)
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    m_dbPath = dataDir + QStringLiteral("/library.db");
}

LibraryDatabase::~LibraryDatabase()
{
    close();
}

// ── open / close ────────────────────────────────────────────────────
bool LibraryDatabase::open()
{
    if (m_db.isOpen()) return true;

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("library"));
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qWarning() << "LibraryDatabase: Failed to open:" << m_db.lastError().text();
        return false;
    }

    // Enable WAL mode for better concurrency
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    createTables();
    createIndexes();

    // ── Migration: add MBID columns to existing databases ────────
    {
        QSqlQuery pragma(m_db);
        pragma.exec(QStringLiteral("PRAGMA table_info(tracks)"));
        QStringList existingColumns;
        while (pragma.next()) {
            existingColumns.append(pragma.value(1).toString());
        }

        auto addColumnIfMissing = [&](const QString& colName) {
            if (!existingColumns.contains(colName)) {
                QSqlQuery alter(m_db);
                alter.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN %1 TEXT").arg(colName));
                qDebug() << "LibraryDatabase: Added column" << colName;
            }
        };

        addColumnIfMissing(QStringLiteral("recording_mbid"));
        addColumnIfMissing(QStringLiteral("artist_mbid"));
        addColumnIfMissing(QStringLiteral("album_mbid"));
        addColumnIfMissing(QStringLiteral("release_group_mbid"));
    }

    qDebug() << "LibraryDatabase: Opened at" << m_dbPath;
    return true;
}

void LibraryDatabase::close()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
}

// ── createTables ────────────────────────────────────────────────────
void LibraryDatabase::createTables()
{
    QSqlQuery q(m_db);

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS tracks ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  artist TEXT,"
        "  album TEXT,"
        "  album_id TEXT,"
        "  artist_id TEXT,"
        "  duration INTEGER DEFAULT 0,"
        "  format TEXT,"
        "  sample_rate TEXT,"
        "  bit_depth TEXT,"
        "  bitrate TEXT,"
        "  cover_url TEXT,"
        "  track_number INTEGER DEFAULT 0,"
        "  disc_number INTEGER DEFAULT 1,"
        "  file_path TEXT UNIQUE,"
        "  recording_mbid TEXT,"
        "  artist_mbid TEXT,"
        "  album_mbid TEXT,"
        "  release_group_mbid TEXT,"
        "  channel_count INTEGER DEFAULT 2,"
        "  added_at TEXT DEFAULT (datetime('now')),"
        "  play_count INTEGER DEFAULT 0"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS albums ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  artist TEXT,"
        "  artist_id TEXT,"
        "  year INTEGER DEFAULT 0,"
        "  cover_url TEXT,"
        "  format TEXT,"
        "  total_tracks INTEGER DEFAULT 0,"
        "  duration INTEGER DEFAULT 0,"
        "  genres TEXT"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS artists ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  cover_url TEXT,"
        "  genres TEXT"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS playlists ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  description TEXT,"
        "  cover_url TEXT,"
        "  is_smart INTEGER DEFAULT 0,"
        "  created_at TEXT DEFAULT (datetime('now'))"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS playlist_tracks ("
        "  playlist_id TEXT NOT NULL,"
        "  track_id TEXT NOT NULL,"
        "  position INTEGER NOT NULL,"
        "  PRIMARY KEY (playlist_id, track_id),"
        "  FOREIGN KEY (playlist_id) REFERENCES playlists(id) ON DELETE CASCADE,"
        "  FOREIGN KEY (track_id) REFERENCES tracks(id) ON DELETE CASCADE"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS play_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  track_id TEXT NOT NULL,"
        "  played_at TEXT DEFAULT (datetime('now')),"
        "  FOREIGN KEY (track_id) REFERENCES tracks(id) ON DELETE CASCADE"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS metadata_backups ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  track_id TEXT NOT NULL,"
        "  title TEXT,"
        "  artist TEXT,"
        "  album TEXT,"
        "  track_number INTEGER,"
        "  disc_number INTEGER,"
        "  recording_mbid TEXT,"
        "  artist_mbid TEXT,"
        "  album_mbid TEXT,"
        "  release_group_mbid TEXT,"
        "  backed_up_at TEXT DEFAULT (datetime('now')),"
        "  FOREIGN KEY (track_id) REFERENCES tracks(id) ON DELETE CASCADE"
        ")"
    ));
}

void LibraryDatabase::createIndexes()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_tracks_filepath ON tracks(file_path)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_album_id ON tracks(album_id)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_artist_id ON tracks(artist_id)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_playlist_tracks_playlist ON playlist_tracks(playlist_id)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_play_history_track ON play_history(track_id)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_play_history_date ON play_history(played_at)"));

    // Migration: add R128 loudness columns (safe to call multiple times)
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN r128_loudness REAL DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN r128_peak REAL DEFAULT 0"));

    // Migration: add channel_count column
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN channel_count INTEGER DEFAULT 2"));
}

// ── Helpers ─────────────────────────────────────────────────────────
QString LibraryDatabase::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString LibraryDatabase::audioFormatToString(AudioFormat fmt) const
{
    switch (fmt) {
    case AudioFormat::FLAC:    return QStringLiteral("FLAC");
    case AudioFormat::DSD64:   return QStringLiteral("DSD64");
    case AudioFormat::DSD128:  return QStringLiteral("DSD128");
    case AudioFormat::DSD256:  return QStringLiteral("DSD256");
    case AudioFormat::DSD512:  return QStringLiteral("DSD512");
    case AudioFormat::DSD1024: return QStringLiteral("DSD1024");
    case AudioFormat::DSD2048: return QStringLiteral("DSD2048");
    case AudioFormat::ALAC:    return QStringLiteral("ALAC");
    case AudioFormat::WAV:     return QStringLiteral("WAV");
    case AudioFormat::MP3:     return QStringLiteral("MP3");
    case AudioFormat::AAC:     return QStringLiteral("AAC");
    }
    return QStringLiteral("Unknown");
}

AudioFormat LibraryDatabase::audioFormatFromString(const QString& str) const
{
    if (str == QStringLiteral("FLAC"))    return AudioFormat::FLAC;
    if (str == QStringLiteral("DSD64"))   return AudioFormat::DSD64;
    if (str == QStringLiteral("DSD128"))  return AudioFormat::DSD128;
    if (str == QStringLiteral("DSD256"))  return AudioFormat::DSD256;
    if (str == QStringLiteral("DSD512"))  return AudioFormat::DSD512;
    if (str == QStringLiteral("DSD1024")) return AudioFormat::DSD1024;
    if (str == QStringLiteral("DSD2048")) return AudioFormat::DSD2048;
    if (str == QStringLiteral("ALAC"))    return AudioFormat::ALAC;
    if (str == QStringLiteral("WAV"))     return AudioFormat::WAV;
    if (str == QStringLiteral("MP3"))     return AudioFormat::MP3;
    if (str == QStringLiteral("AAC"))     return AudioFormat::AAC;
    return AudioFormat::FLAC; // fallback
}

Track LibraryDatabase::trackFromQuery(const QSqlQuery& query) const
{
    Track t;
    t.id          = query.value(QStringLiteral("id")).toString();
    t.title       = query.value(QStringLiteral("title")).toString();
    t.artist      = query.value(QStringLiteral("artist")).toString();
    t.album       = query.value(QStringLiteral("album")).toString();
    t.albumId     = query.value(QStringLiteral("album_id")).toString();
    t.artistId    = query.value(QStringLiteral("artist_id")).toString();
    t.duration    = query.value(QStringLiteral("duration")).toInt();
    t.format      = audioFormatFromString(query.value(QStringLiteral("format")).toString());
    t.sampleRate  = query.value(QStringLiteral("sample_rate")).toString();
    t.bitDepth    = query.value(QStringLiteral("bit_depth")).toString();
    t.bitrate     = query.value(QStringLiteral("bitrate")).toString();
    t.coverUrl    = query.value(QStringLiteral("cover_url")).toString();
    t.trackNumber = query.value(QStringLiteral("track_number")).toInt();
    t.discNumber       = query.value(QStringLiteral("disc_number")).toInt();
    t.filePath         = query.value(QStringLiteral("file_path")).toString();
    t.recordingMbid    = query.value(QStringLiteral("recording_mbid")).toString();
    t.artistMbid       = query.value(QStringLiteral("artist_mbid")).toString();
    t.albumMbid        = query.value(QStringLiteral("album_mbid")).toString();
    t.releaseGroupMbid = query.value(QStringLiteral("release_group_mbid")).toString();

    // Channel count
    int chIdx = query.record().indexOf(QStringLiteral("channel_count"));
    if (chIdx >= 0) {
        int ch = query.value(chIdx).toInt();
        t.channelCount = (ch > 0) ? ch : 2;
    }

    // Load cached R128 loudness if available
    int r128Idx = query.record().indexOf(QStringLiteral("r128_loudness"));
    if (r128Idx >= 0) {
        t.r128Loudness = query.value(r128Idx).toDouble();
        t.r128Peak = query.value(QStringLiteral("r128_peak")).toDouble();
        if (t.r128Loudness != 0.0) t.hasR128 = true;
    }

    return t;
}

// ── Tracks ──────────────────────────────────────────────────────────
bool LibraryDatabase::trackExists(const QString& filePath) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() > 0;
    }
    return false;
}

void LibraryDatabase::removeDuplicates()
{
    qDebug() << "=== LibraryDatabase::removeDuplicates ===";

    QSqlQuery countBefore(m_db);
    countBefore.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    int before = 0;
    if (countBefore.next()) before = countBefore.value(0).toInt();
    qDebug() << "  Tracks before cleanup:" << before;

    // 1) Remove exact duplicates by file_path (keep first inserted)
    QSqlQuery q1(m_db);
    q1.exec(QStringLiteral(
        "DELETE FROM tracks WHERE id NOT IN ("
        "  SELECT MIN(id) FROM tracks GROUP BY file_path"
        ")"
    ));
    qDebug() << "  Removed by file_path:" << q1.numRowsAffected();

    // 2) Remove duplicates by metadata match (title+artist+album+duration)
    QSqlQuery q2(m_db);
    q2.exec(QStringLiteral(
        "DELETE FROM tracks WHERE id NOT IN ("
        "  SELECT MIN(id) FROM tracks "
        "  GROUP BY LOWER(title), LOWER(artist), LOWER(album), CAST(duration AS INTEGER)"
        ")"
    ));
    qDebug() << "  Removed by metadata match:" << q2.numRowsAffected();

    // 3) Remove tracks whose files no longer exist on disk
    QSqlQuery selectAll(m_db);
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
            QSqlQuery del(m_db);
            del.prepare(QStringLiteral("DELETE FROM tracks WHERE id = ?"));
            del.addBindValue(id);
            del.exec();
        }
        qDebug() << "  Removed missing files:" << toRemove.size();
    }

    QSqlQuery countAfter(m_db);
    countAfter.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    int after = 0;
    if (countAfter.next()) after = countAfter.value(0).toInt();
    qDebug() << "  Tracks after cleanup:" << after;
    qDebug() << "  Total removed:" << (before - after);
    qDebug() << "=== Duplicate removal complete ===";

    if (before != after) {
        emit databaseChanged();
    }
}

bool LibraryDatabase::insertTrack(const Track& track)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO tracks "
        "(id, title, artist, album, album_id, artist_id, duration, format, "
        "sample_rate, bit_depth, bitrate, cover_url, track_number, disc_number, file_path, "
        "recording_mbid, artist_mbid, album_mbid, release_group_mbid, channel_count) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    ));

    QString id = track.id.isEmpty() ? generateId() : track.id;
    q.addBindValue(id);
    q.addBindValue(track.title);
    q.addBindValue(track.artist);
    q.addBindValue(track.album);
    q.addBindValue(track.albumId);
    q.addBindValue(track.artistId);
    q.addBindValue(track.duration);
    q.addBindValue(audioFormatToString(track.format));
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

    if (!q.exec()) {
        qWarning() << "LibraryDatabase::insertTrack failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryDatabase::updateTrack(const Track& track)
{
    if (track.id.isEmpty()) {
        qWarning() << "LibraryDatabase::updateTrack - track has no ID, falling back to insertTrack";
        return insertTrack(track);
    }

    qDebug() << "=== LibraryDatabase::updateTrack ===";
    qDebug() << "  ID:" << track.id;
    qDebug() << "  Title:" << track.title;
    qDebug() << "  Artist:" << track.artist;
    qDebug() << "  Album:" << track.album;
    qDebug() << "  FilePath:" << track.filePath;
    qDebug() << "  Recording MBID:" << track.recordingMbid;
    qDebug() << "  Artist MBID:" << track.artistMbid;
    qDebug() << "  Album MBID:" << track.albumMbid;
    qDebug() << "  ReleaseGroup MBID:" << track.releaseGroupMbid;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE tracks SET "
        "title = ?, artist = ?, album = ?, album_id = ?, artist_id = ?, "
        "duration = ?, format = ?, sample_rate = ?, bit_depth = ?, bitrate = ?, "
        "cover_url = ?, track_number = ?, disc_number = ?, file_path = ?, "
        "recording_mbid = ?, artist_mbid = ?, album_mbid = ?, release_group_mbid = ?, "
        "channel_count = ? "
        "WHERE id = ?"
    ));

    q.addBindValue(track.title);
    q.addBindValue(track.artist);
    q.addBindValue(track.album);
    q.addBindValue(track.albumId);
    q.addBindValue(track.artistId);
    q.addBindValue(track.duration);
    q.addBindValue(audioFormatToString(track.format));
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
    q.addBindValue(track.id);

    if (!q.exec()) {
        qWarning() << "  UPDATE FAILED:" << q.lastError().text();
        return false;
    }

    int rows = q.numRowsAffected();
    qDebug() << "  UPDATE SUCCESS: rows affected:" << rows;

    if (rows == 0) {
        qWarning() << "  No rows matched id=" << track.id << ", falling back to insertTrack";
        return insertTrack(track);
    }

    return true;
}

bool LibraryDatabase::updateTrackMetadata(const QString& trackId,
                                           const QString& title,
                                           const QString& artist,
                                           const QString& album,
                                           const QString& recordingMbid,
                                           const QString& artistMbid,
                                           const QString& albumMbid,
                                           const QString& releaseGroupMbid)
{
    if (trackId.isEmpty()) {
        qWarning() << "LibraryDatabase::updateTrackMetadata - empty track ID";
        return false;
    }

    qDebug() << "=== LibraryDatabase::updateTrackMetadata ===";
    qDebug() << "  ID:" << trackId;
    qDebug() << "  Title:" << title << "Artist:" << artist << "Album:" << album;
    qDebug() << "  MBIDs: rec=" << recordingMbid << "artist=" << artistMbid
             << "album=" << albumMbid << "rg=" << releaseGroupMbid;

    QSqlQuery q(m_db);
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

bool LibraryDatabase::removeTrack(const QString& id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE id = ?"));
    q.addBindValue(id);
    return q.exec();
}

bool LibraryDatabase::removeTrackByPath(const QString& filePath)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    return q.exec();
}

std::optional<Track> LibraryDatabase::trackById(const QString& id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM tracks WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return trackFromQuery(q);
    }
    return std::nullopt;
}

std::optional<Track> LibraryDatabase::trackByPath(const QString& filePath) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    if (q.exec() && q.next()) {
        return trackFromQuery(q);
    }
    return std::nullopt;
}

QVector<Track> LibraryDatabase::allTracks() const
{
    QVector<Track> result;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT * FROM tracks ORDER BY artist, album, disc_number, track_number"));
    while (q.next()) {
        result.append(trackFromQuery(q));
    }
    return result;
}

QVector<Track> LibraryDatabase::searchTracks(const QString& query) const
{
    QVector<Track> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT * FROM tracks WHERE "
        "title LIKE ? OR artist LIKE ? OR album LIKE ? "
        "ORDER BY artist, album, track_number"
    ));
    QString pattern = QStringLiteral("%%1%").arg(query);
    q.addBindValue(pattern);
    q.addBindValue(pattern);
    q.addBindValue(pattern);

    if (q.exec()) {
        while (q.next()) {
            result.append(trackFromQuery(q));
        }
    }
    return result;
}

int LibraryDatabase::trackCount() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    if (q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

// ── Albums ──────────────────────────────────────────────────────────
bool LibraryDatabase::insertAlbum(const Album& album)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO albums "
        "(id, title, artist, artist_id, year, cover_url, format, total_tracks, duration, genres) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    ));

    q.addBindValue(album.id);
    q.addBindValue(album.title);
    q.addBindValue(album.artist);
    q.addBindValue(album.artistId);
    q.addBindValue(album.year);
    q.addBindValue(album.coverUrl);
    q.addBindValue(audioFormatToString(album.format));
    q.addBindValue(album.totalTracks);
    q.addBindValue(album.duration);
    q.addBindValue(album.genres.join(QStringLiteral(",")));

    if (!q.exec()) {
        qWarning() << "LibraryDatabase::insertAlbum failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryDatabase::updateAlbum(const Album& album)
{
    return insertAlbum(album);
}

QVector<Album> LibraryDatabase::allAlbums() const
{
    QVector<Album> result;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT * FROM albums ORDER BY artist, title"));

    if (q.lastError().isValid()) {
        qWarning() << "LibraryDatabase::allAlbums query error:" << q.lastError().text();
    }

    while (q.next()) {
        Album a;
        a.id          = q.value(QStringLiteral("id")).toString();
        a.title       = q.value(QStringLiteral("title")).toString();
        a.artist      = q.value(QStringLiteral("artist")).toString();
        a.artistId    = q.value(QStringLiteral("artist_id")).toString();
        a.year        = q.value(QStringLiteral("year")).toInt();
        a.coverUrl    = q.value(QStringLiteral("cover_url")).toString();
        a.format      = audioFormatFromString(q.value(QStringLiteral("format")).toString());
        a.totalTracks = q.value(QStringLiteral("total_tracks")).toInt();
        a.duration    = q.value(QStringLiteral("duration")).toInt();

        QString genresStr = q.value(QStringLiteral("genres")).toString();
        if (!genresStr.isEmpty())
            a.genres = genresStr.split(QStringLiteral(","));

        // Tracks loaded on demand via albumById() — avoids N+1 query problem
        result.append(a);
    }

    qDebug() << "LibraryDatabase::allAlbums returning" << result.size() << "albums";
    return result;
}

Album LibraryDatabase::albumById(const QString& id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM albums WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        Album a;
        a.id          = q.value(QStringLiteral("id")).toString();
        a.title       = q.value(QStringLiteral("title")).toString();
        a.artist      = q.value(QStringLiteral("artist")).toString();
        a.artistId    = q.value(QStringLiteral("artist_id")).toString();
        a.year        = q.value(QStringLiteral("year")).toInt();
        a.coverUrl    = q.value(QStringLiteral("cover_url")).toString();
        a.format      = audioFormatFromString(q.value(QStringLiteral("format")).toString());
        a.totalTracks = q.value(QStringLiteral("total_tracks")).toInt();
        a.duration    = q.value(QStringLiteral("duration")).toInt();

        QString genresStr = q.value(QStringLiteral("genres")).toString();
        if (!genresStr.isEmpty())
            a.genres = genresStr.split(QStringLiteral(","));

        // Load tracks
        QSqlQuery tq(m_db);
        tq.prepare(QStringLiteral("SELECT * FROM tracks WHERE album_id = ? ORDER BY disc_number, track_number"));
        tq.addBindValue(a.id);
        if (tq.exec()) {
            while (tq.next()) {
                a.tracks.append(trackFromQuery(tq));
            }
        }
        return a;
    }
    return Album{};
}

// ── Artists ─────────────────────────────────────────────────────────
bool LibraryDatabase::insertArtist(const Artist& artist)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO artists (id, name, cover_url, genres) "
        "VALUES (?, ?, ?, ?)"
    ));

    q.addBindValue(artist.id);
    q.addBindValue(artist.name);
    q.addBindValue(artist.coverUrl);
    q.addBindValue(artist.genres.join(QStringLiteral(",")));

    if (!q.exec()) {
        qWarning() << "LibraryDatabase::insertArtist failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryDatabase::updateArtist(const Artist& artist)
{
    return insertArtist(artist);
}

QVector<Artist> LibraryDatabase::allArtists() const
{
    QVector<Artist> result;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT * FROM artists ORDER BY name"));

    if (q.lastError().isValid()) {
        qWarning() << "LibraryDatabase::allArtists query error:" << q.lastError().text();
    }

    while (q.next()) {
        Artist a;
        a.id       = q.value(QStringLiteral("id")).toString();
        a.name     = q.value(QStringLiteral("name")).toString();
        a.coverUrl = q.value(QStringLiteral("cover_url")).toString();

        QString genresStr = q.value(QStringLiteral("genres")).toString();
        if (!genresStr.isEmpty())
            a.genres = genresStr.split(QStringLiteral(","));

        // Albums loaded on demand via artistById() — avoids N+1 query problem
        result.append(a);
    }

    qDebug() << "LibraryDatabase::allArtists returning" << result.size() << "artists";
    return result;
}

Artist LibraryDatabase::artistById(const QString& id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM artists WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        Artist a;
        a.id       = q.value(QStringLiteral("id")).toString();
        a.name     = q.value(QStringLiteral("name")).toString();
        a.coverUrl = q.value(QStringLiteral("cover_url")).toString();

        QString genresStr = q.value(QStringLiteral("genres")).toString();
        if (!genresStr.isEmpty())
            a.genres = genresStr.split(QStringLiteral(","));

        // Load albums
        QSqlQuery aq(m_db);
        aq.prepare(QStringLiteral("SELECT id FROM albums WHERE artist_id = ? ORDER BY year"));
        aq.addBindValue(a.id);
        if (aq.exec()) {
            while (aq.next()) {
                a.albums.append(albumById(aq.value(0).toString()));
            }
        }
        return a;
    }
    return Artist{};
}

QVector<Album> LibraryDatabase::searchAlbums(const QString& query) const
{
    QVector<Album> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT * FROM albums WHERE "
        "title LIKE ? OR artist LIKE ? "
        "ORDER BY artist, title LIMIT 20"
    ));
    QString pattern = QStringLiteral("%%1%").arg(query);
    q.addBindValue(pattern);
    q.addBindValue(pattern);

    if (q.exec()) {
        while (q.next()) {
            Album a;
            a.id          = q.value(QStringLiteral("id")).toString();
            a.title       = q.value(QStringLiteral("title")).toString();
            a.artist      = q.value(QStringLiteral("artist")).toString();
            a.artistId    = q.value(QStringLiteral("artist_id")).toString();
            a.year        = q.value(QStringLiteral("year")).toInt();
            a.coverUrl    = q.value(QStringLiteral("cover_url")).toString();
            a.format      = audioFormatFromString(q.value(QStringLiteral("format")).toString());
            a.totalTracks = q.value(QStringLiteral("total_tracks")).toInt();
            a.duration    = q.value(QStringLiteral("duration")).toInt();
            QString genresStr = q.value(QStringLiteral("genres")).toString();
            if (!genresStr.isEmpty())
                a.genres = genresStr.split(QStringLiteral(","));
            result.append(a);
        }
    }
    return result;
}

QVector<Artist> LibraryDatabase::searchArtists(const QString& query) const
{
    QVector<Artist> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT * FROM artists WHERE name LIKE ? ORDER BY name LIMIT 10"
    ));
    QString pattern = QStringLiteral("%%1%").arg(query);
    q.addBindValue(pattern);

    if (q.exec()) {
        while (q.next()) {
            Artist a;
            a.id       = q.value(QStringLiteral("id")).toString();
            a.name     = q.value(QStringLiteral("name")).toString();
            a.coverUrl = q.value(QStringLiteral("cover_url")).toString();
            QString genresStr = q.value(QStringLiteral("genres")).toString();
            if (!genresStr.isEmpty())
                a.genres = genresStr.split(QStringLiteral(","));
            result.append(a);
        }
    }
    return result;
}

// ── Playlists ───────────────────────────────────────────────────────
bool LibraryDatabase::insertPlaylist(const Playlist& playlist)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO playlists (id, name, description, cover_url, is_smart, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?)"
    ));

    QString id = playlist.id.isEmpty() ? generateId() : playlist.id;
    q.addBindValue(id);
    q.addBindValue(playlist.name);
    q.addBindValue(playlist.description);
    q.addBindValue(playlist.coverUrl);
    q.addBindValue(playlist.isSmartPlaylist ? 1 : 0);
    q.addBindValue(playlist.createdAt.isEmpty()
        ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        : playlist.createdAt);

    if (!q.exec()) {
        qWarning() << "LibraryDatabase::insertPlaylist failed:" << q.lastError().text();
        return false;
    }

    // Insert playlist tracks
    if (!playlist.tracks.isEmpty()) {
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id = ?"));
        del.addBindValue(id);
        del.exec();

        for (int i = 0; i < playlist.tracks.size(); ++i) {
            addTrackToPlaylist(id, playlist.tracks[i].id, i);
        }
    }

    return true;
}

bool LibraryDatabase::updatePlaylist(const Playlist& playlist)
{
    return insertPlaylist(playlist);
}

bool LibraryDatabase::removePlaylist(const QString& id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM playlists WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec()) return false;

    // Cascade should handle playlist_tracks, but just in case
    QSqlQuery q2(m_db);
    q2.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id = ?"));
    q2.addBindValue(id);
    q2.exec();

    return true;
}

QVector<Playlist> LibraryDatabase::allPlaylists() const
{
    QVector<Playlist> result;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT * FROM playlists ORDER BY created_at DESC"));

    while (q.next()) {
        Playlist p;
        p.id              = q.value(QStringLiteral("id")).toString();
        p.name            = q.value(QStringLiteral("name")).toString();
        p.description     = q.value(QStringLiteral("description")).toString();
        p.coverUrl        = q.value(QStringLiteral("cover_url")).toString();
        p.isSmartPlaylist = q.value(QStringLiteral("is_smart")).toBool();
        p.createdAt       = q.value(QStringLiteral("created_at")).toString();

        // Load tracks
        QSqlQuery tq(m_db);
        tq.prepare(QStringLiteral(
            "SELECT t.* FROM tracks t "
            "JOIN playlist_tracks pt ON t.id = pt.track_id "
            "WHERE pt.playlist_id = ? "
            "ORDER BY pt.position"
        ));
        tq.addBindValue(p.id);
        if (tq.exec()) {
            while (tq.next()) {
                p.tracks.append(trackFromQuery(tq));
            }
        }

        result.append(p);
    }
    return result;
}

Playlist LibraryDatabase::playlistById(const QString& id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM playlists WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        Playlist p;
        p.id              = q.value(QStringLiteral("id")).toString();
        p.name            = q.value(QStringLiteral("name")).toString();
        p.description     = q.value(QStringLiteral("description")).toString();
        p.coverUrl        = q.value(QStringLiteral("cover_url")).toString();
        p.isSmartPlaylist = q.value(QStringLiteral("is_smart")).toBool();
        p.createdAt       = q.value(QStringLiteral("created_at")).toString();

        // Load tracks
        QSqlQuery tq(m_db);
        tq.prepare(QStringLiteral(
            "SELECT t.* FROM tracks t "
            "JOIN playlist_tracks pt ON t.id = pt.track_id "
            "WHERE pt.playlist_id = ? "
            "ORDER BY pt.position"
        ));
        tq.addBindValue(p.id);
        if (tq.exec()) {
            while (tq.next()) {
                p.tracks.append(trackFromQuery(tq));
            }
        }
        return p;
    }
    return Playlist{};
}

bool LibraryDatabase::addTrackToPlaylist(const QString& playlistId, const QString& trackId, int position)
{
    QSqlQuery q(m_db);

    if (position < 0) {
        // Append at end
        QSqlQuery maxQ(m_db);
        maxQ.prepare(QStringLiteral(
            "SELECT COALESCE(MAX(position), -1) FROM playlist_tracks WHERE playlist_id = ?"));
        maxQ.addBindValue(playlistId);
        if (maxQ.exec() && maxQ.next()) {
            position = maxQ.value(0).toInt() + 1;
        } else {
            position = 0;
        }
    }

    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO playlist_tracks (playlist_id, track_id, position) "
        "VALUES (?, ?, ?)"
    ));
    q.addBindValue(playlistId);
    q.addBindValue(trackId);
    q.addBindValue(position);
    return q.exec();
}

bool LibraryDatabase::removeTrackFromPlaylist(const QString& playlistId, const QString& trackId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM playlist_tracks WHERE playlist_id = ? AND track_id = ?"));
    q.addBindValue(playlistId);
    q.addBindValue(trackId);
    return q.exec();
}

bool LibraryDatabase::reorderPlaylistTrack(const QString& playlistId, int fromPos, int toPos)
{
    QSqlQuery q(m_db);
    // Get the track at fromPos
    q.prepare(QStringLiteral(
        "SELECT track_id FROM playlist_tracks WHERE playlist_id = ? AND position = ?"));
    q.addBindValue(playlistId);
    q.addBindValue(fromPos);
    if (!q.exec() || !q.next()) return false;
    QString trackId = q.value(0).toString();

    // Remove from old position
    removeTrackFromPlaylist(playlistId, trackId);

    // Shift other tracks
    if (toPos > fromPos) {
        QSqlQuery shift(m_db);
        shift.prepare(QStringLiteral(
            "UPDATE playlist_tracks SET position = position - 1 "
            "WHERE playlist_id = ? AND position > ? AND position <= ?"));
        shift.addBindValue(playlistId);
        shift.addBindValue(fromPos);
        shift.addBindValue(toPos);
        shift.exec();
    } else {
        QSqlQuery shift(m_db);
        shift.prepare(QStringLiteral(
            "UPDATE playlist_tracks SET position = position + 1 "
            "WHERE playlist_id = ? AND position >= ? AND position < ?"));
        shift.addBindValue(playlistId);
        shift.addBindValue(toPos);
        shift.addBindValue(fromPos);
        shift.exec();
    }

    // Insert at new position
    return addTrackToPlaylist(playlistId, trackId, toPos);
}

// ── Volume Leveling ─────────────────────────────────────────────────
void LibraryDatabase::updateR128Loudness(const QString& filePath, double loudness, double peak)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE tracks SET r128_loudness = ?, r128_peak = ? WHERE file_path = ?"));
    q.addBindValue(loudness);
    q.addBindValue(peak);
    q.addBindValue(filePath);
    if (!q.exec()) {
        qWarning() << "LibraryDatabase::updateR128Loudness failed:" << q.lastError().text();
    }
}

// ── Play History ────────────────────────────────────────────────────
void LibraryDatabase::recordPlay(const QString& trackId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO play_history (track_id) VALUES (?)"));
    q.addBindValue(trackId);
    q.exec();

    // Increment play_count
    QSqlQuery q2(m_db);
    q2.prepare(QStringLiteral("UPDATE tracks SET play_count = play_count + 1 WHERE id = ?"));
    q2.addBindValue(trackId);
    q2.exec();
}

QVector<Track> LibraryDatabase::recentlyPlayed(int limit) const
{
    QVector<Track> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT DISTINCT t.* FROM tracks t "
        "JOIN play_history ph ON t.id = ph.track_id "
        "ORDER BY ph.played_at DESC LIMIT ?"
    ));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append(trackFromQuery(q));
        }
    }
    return result;
}

QVector<Track> LibraryDatabase::mostPlayed(int limit) const
{
    QVector<Track> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT * FROM tracks WHERE play_count > 0 "
        "ORDER BY play_count DESC LIMIT ?"
    ));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append(trackFromQuery(q));
        }
    }
    return result;
}

QVector<Track> LibraryDatabase::recentlyAdded(int limit) const
{
    QVector<Track> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT * FROM tracks ORDER BY added_at DESC LIMIT ?"
    ));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append(trackFromQuery(q));
        }
    }
    return result;
}

// ── MBID Helpers ─────────────────────────────────────────────────────
QString LibraryDatabase::releaseGroupMbidForAlbum(const QString& albumId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT release_group_mbid, album_mbid FROM tracks "
        "WHERE album_id = ? AND (release_group_mbid IS NOT NULL AND release_group_mbid != '') "
        "LIMIT 1"));
    q.addBindValue(albumId);
    if (q.exec() && q.next()) {
        QString rgMbid = q.value(0).toString();
        if (!rgMbid.isEmpty()) return rgMbid;
        return q.value(1).toString(); // fallback to album_mbid
    }
    // Fallback: try album_mbid
    q.prepare(QStringLiteral(
        "SELECT album_mbid FROM tracks "
        "WHERE album_id = ? AND album_mbid IS NOT NULL AND album_mbid != '' "
        "LIMIT 1"));
    q.addBindValue(albumId);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return {};
}

QString LibraryDatabase::artistMbidForArtist(const QString& artistId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT artist_mbid FROM tracks "
        "WHERE artist_id = ? AND artist_mbid IS NOT NULL AND artist_mbid != '' "
        "LIMIT 1"));
    q.addBindValue(artistId);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return {};
}

// ── Metadata Backup / Undo ────────────────────────────────────────────

void LibraryDatabase::backupTrackMetadata(const QString& trackId)
{
    if (trackId.isEmpty()) return;

    QSqlQuery q(m_db);
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

bool LibraryDatabase::undoLastMetadataChange(const QString& trackId)
{
    if (trackId.isEmpty()) return false;

    // Get the most recent backup
    QSqlQuery q(m_db);
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
    QSqlQuery up(m_db);
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
    QSqlQuery del(m_db);
    del.prepare(QStringLiteral("DELETE FROM metadata_backups WHERE track_id = ? "
        "AND id = (SELECT MAX(id) FROM metadata_backups WHERE track_id = ?)"));
    del.addBindValue(trackId);
    del.addBindValue(trackId);
    del.exec();

    qDebug() << "[LibraryDB] Restored metadata for track:" << trackId;
    return true;
}

bool LibraryDatabase::hasMetadataBackup(const QString& trackId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM metadata_backups WHERE track_id = ?"));
    q.addBindValue(trackId);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() > 0;
    }
    return false;
}

// ── Transaction helpers ──────────────────────────────────────────────
bool LibraryDatabase::beginTransaction() { return m_db.transaction(); }
bool LibraryDatabase::commitTransaction() { return m_db.commit(); }

// ── Database backup / rollback ───────────────────────────────────────

bool LibraryDatabase::createBackup()
{
    QString backupFile = m_dbPath + QStringLiteral(".backup");

    if (QFile::exists(backupFile))
        QFile::remove(backupFile);

    bool ok = QFile::copy(m_dbPath, backupFile);
    if (ok)
        qDebug() << "[LibraryDatabase] Backup created:" << backupFile;
    else
        qWarning() << "[LibraryDatabase] Backup FAILED for" << m_dbPath;
    return ok;
}

bool LibraryDatabase::restoreFromBackup()
{
    QString backupFile = m_dbPath + QStringLiteral(".backup");
    if (!QFile::exists(backupFile)) {
        qWarning() << "[LibraryDatabase] No backup found at" << backupFile;
        return false;
    }

    // Close the database connection
    m_db.close();

    // Replace DB with backup
    QFile::remove(m_dbPath);
    bool ok = QFile::copy(backupFile, m_dbPath);

    // Reopen the database
    m_db.open();

    if (ok) {
        qDebug() << "[LibraryDatabase] Restored from backup";
        emit databaseChanged();
    } else {
        qWarning() << "[LibraryDatabase] Restore FAILED";
    }
    return ok;
}

bool LibraryDatabase::hasBackup() const
{
    return QFile::exists(m_dbPath + QStringLiteral(".backup"));
}

QDateTime LibraryDatabase::backupTimestamp() const
{
    QFileInfo info(m_dbPath + QStringLiteral(".backup"));
    return info.exists() ? info.lastModified() : QDateTime();
}

// ── Rebuild Albums & Artists from Tracks ─────────────────────────────
void LibraryDatabase::rebuildAlbumsAndArtists()
{
    // Skip if recently rebuilt
    static QElapsedTimer lastRebuild;
    if (lastRebuild.isValid() && lastRebuild.elapsed() < 5000) {
        qDebug() << "LibraryDatabase: Skipping rebuild - cooldown";
        return;
    }

    // Guard: never wipe albums/artists when tracks table is empty
    {
        QSqlQuery countQ(m_db);
        countQ.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
        if (!countQ.next() || countQ.value(0).toInt() == 0) {
            qDebug() << "LibraryDatabase: Skipping rebuild - 0 tracks in DB";
            return;
        }
    }

    lastRebuild.start();

    // Auto-backup before destructive rebuild
    createBackup();

    qDebug() << "LibraryDatabase::rebuildAlbumsAndArtists - starting rebuild...";

    m_db.transaction();

    // Clear existing albums and artists to avoid stale duplicates
    QSqlQuery clearQ(m_db);
    if (!clearQ.exec(QStringLiteral("DELETE FROM albums")) ||
        !clearQ.exec(QStringLiteral("DELETE FROM artists"))) {
        qWarning() << "  Failed to clear tables, rolling back";
        m_db.rollback();
        return;
    }

    // First, assign consistent artist IDs by normalized artist name
    QMap<QString, QString> artistNameToId; // normalized name -> artist_id

    {
        QSqlQuery q(m_db);
        q.exec(QStringLiteral(
            "SELECT DISTINCT TRIM(artist) as artist_name FROM tracks "
            "WHERE artist IS NOT NULL AND TRIM(artist) != '' ORDER BY artist_name"
        ));
        while (q.next()) {
            QString artistName = q.value(0).toString().trimmed();
            QString normalizedName = artistName.toLower();
            if (!artistNameToId.contains(normalizedName)) {
                QString artistId = QStringLiteral("artist_") +
                    QString::number(qHash(normalizedName), 16).rightJustified(8, QLatin1Char('0'));
                artistNameToId[normalizedName] = artistId;
            }
        }
    }
    qDebug() << "  Unique artists found:" << artistNameToId.size();

    // Assign consistent album IDs by (normalized album name + normalized artist name)
    QMap<QString, QString> albumKeyToId; // "album||artist" -> album_id

    {
        QSqlQuery q(m_db);
        q.exec(QStringLiteral(
            "SELECT DISTINCT TRIM(album) as album_name, TRIM(artist) as artist_name "
            "FROM tracks WHERE album IS NOT NULL AND TRIM(album) != '' "
            "ORDER BY artist_name, album_name"
        ));
        while (q.next()) {
            QString albumName = q.value(0).toString().trimmed();
            QString artistName = q.value(1).toString().trimmed();
            QString key = albumName.toLower() + QStringLiteral("||") + artistName.toLower();
            if (!albumKeyToId.contains(key)) {
                QString albumId = QStringLiteral("album_") +
                    QString::number(qHash(key), 16).rightJustified(8, QLatin1Char('0'));
                albumKeyToId[key] = albumId;
            }
        }
    }
    qDebug() << "  Unique albums found:" << albumKeyToId.size();

    // Update all tracks with consistent album_id and artist_id
    for (auto it = artistNameToId.constBegin(); it != artistNameToId.constEnd(); ++it) {
        QSqlQuery updateQ(m_db);
        updateQ.prepare(QStringLiteral(
            "UPDATE tracks SET artist_id = ? WHERE LOWER(TRIM(artist)) = ?"));
        updateQ.addBindValue(it.value());
        updateQ.addBindValue(it.key());
        if (!updateQ.exec()) {
            qWarning() << "  Failed to update artist_id:" << updateQ.lastError().text();
        }
    }

    for (auto it = albumKeyToId.constBegin(); it != albumKeyToId.constEnd(); ++it) {
        QString key = it.key();
        int sep = key.indexOf(QStringLiteral("||"));
        QString albumNameLower = key.left(sep);
        QString artistNameLower = key.mid(sep + 2);

        QSqlQuery updateQ(m_db);
        updateQ.prepare(QStringLiteral(
            "UPDATE tracks SET album_id = ? "
            "WHERE LOWER(TRIM(album)) = ? AND LOWER(TRIM(artist)) = ?"));
        updateQ.addBindValue(it.value());
        updateQ.addBindValue(albumNameLower);
        updateQ.addBindValue(artistNameLower);
        if (!updateQ.exec()) {
            qWarning() << "  Failed to update album_id:" << updateQ.lastError().text();
        }
    }

    // Build and insert albums with all fields (coverUrl, stats)
    {
        QSqlQuery q(m_db);
        q.exec(QStringLiteral(
            "SELECT album_id, "
            "  TRIM(album), "
            "  TRIM(artist), "
            "  artist_id, "
            "  MAX(format) as format, "
            "  MAX(CASE WHEN cover_url IS NOT NULL AND cover_url != '' THEN cover_url ELSE '' END) as cover_url, "
            "  COUNT(*) as track_count, "
            "  SUM(duration) as total_duration "
            "FROM tracks "
            "WHERE album IS NOT NULL AND TRIM(album) != '' AND album_id IS NOT NULL AND album_id != '' "
            "GROUP BY album_id "
            "ORDER BY artist, album"
        ));
        if (q.lastError().isValid()) {
            qWarning() << "  Album query error:" << q.lastError().text();
        }

        int albumCount = 0;
        while (q.next()) {
            Album a;
            a.id          = q.value(0).toString();
            a.title       = q.value(1).toString();
            a.artist      = q.value(2).toString();
            a.artistId    = q.value(3).toString();
            a.format      = audioFormatFromString(q.value(4).toString());
            a.coverUrl    = q.value(5).toString();
            a.totalTracks = q.value(6).toInt();
            a.duration    = q.value(7).toInt();

            insertAlbum(a);
            albumCount++;
            qDebug() << "  Inserted album:" << a.title << "by" << a.artist
                     << "tracks:" << a.totalTracks;
        }
        qDebug() << "  Total albums inserted:" << albumCount;
    }

    // Build and insert artists
    for (auto it = artistNameToId.constBegin(); it != artistNameToId.constEnd(); ++it) {
        Artist ar;
        ar.id = it.value();

        // Get the original-case name and cover from tracks
        QSqlQuery infoQ(m_db);
        infoQ.prepare(QStringLiteral(
            "SELECT TRIM(artist), "
            "  MAX(CASE WHEN cover_url IS NOT NULL AND cover_url != '' THEN cover_url ELSE '' END) "
            "FROM tracks WHERE artist_id = ?"));
        infoQ.addBindValue(ar.id);
        if (infoQ.exec() && infoQ.next()) {
            ar.name     = infoQ.value(0).toString();
            ar.coverUrl = infoQ.value(1).toString();
        }

        insertArtist(ar);
        qDebug() << "  Inserted artist:" << ar.name;
    }

    // Verify counts
    QSqlQuery countAlbums(m_db);
    countAlbums.exec(QStringLiteral("SELECT COUNT(*) FROM albums"));
    if (countAlbums.next())
        qDebug() << "  Total albums in DB after rebuild:" << countAlbums.value(0).toInt();

    QSqlQuery countArtists(m_db);
    countArtists.exec(QStringLiteral("SELECT COUNT(*) FROM artists"));
    if (countArtists.next())
        qDebug() << "  Total artists in DB after rebuild:" << countArtists.value(0).toInt();

    m_db.commit();

    emit databaseChanged();
    qDebug() << "LibraryDatabase::rebuildAlbumsAndArtists - rebuild complete";
}
