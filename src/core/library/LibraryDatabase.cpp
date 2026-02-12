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
#include <QThread>
#include <QCoreApplication>
#include <QSet>

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
    QMutexLocker lock(&m_writeMutex);
    if (m_db.isOpen()) return true;

    // Write connection (scanner, inserts, updates)
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("library_write"));
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qWarning() << "LibraryDatabase: Failed to open write connection:" << m_db.lastError().text();
        return false;
    }

    // Enable WAL mode + performance PRAGMAs
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    pragma.exec(QStringLiteral("PRAGMA mmap_size=268435456"));   // 256MB mmap window
    pragma.exec(QStringLiteral("PRAGMA cache_size=-65536"));     // 64MB page cache
    pragma.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));     // temp tables in RAM

    // Read connection (MDP, search, UI queries) — separate from writer for WAL concurrency
    m_readDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("library_read"));
    m_readDb.setDatabaseName(m_dbPath);

    if (!m_readDb.open()) {
        qWarning() << "LibraryDatabase: Failed to open read connection:" << m_readDb.lastError().text();
        return false;
    }

    QSqlQuery readPragma(m_readDb);
    readPragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    readPragma.exec(QStringLiteral("PRAGMA mmap_size=268435456"));
    readPragma.exec(QStringLiteral("PRAGMA cache_size=-65536"));
    readPragma.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
    readPragma.exec(QStringLiteral("PRAGMA query_only=ON"));

    qDebug() << "[LibraryDB] Dual-connection: WAL + mmap 256MB + cache 64MB (read/write split)";

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
    bool readWasOpen = false, writeWasOpen = false;
    {
        QMutexLocker lock(&m_readMutex);
        if (m_readDb.isOpen()) {
            m_readDb.close();
            readWasOpen = true;
        }
        m_readDb = QSqlDatabase();  // drop reference before removeDatabase
    }
    {
        QMutexLocker lock(&m_writeMutex);
        if (m_db.isOpen()) {
            m_db.close();
            writeWasOpen = true;
        }
        m_db = QSqlDatabase();      // drop reference before removeDatabase
    }
    // Only remove connections if they were actually open —
    // prevents "QSqlDatabase requires QCoreApplication" when
    // static destructor runs after QCoreApplication is gone
    if (readWasOpen)
        QSqlDatabase::removeDatabase(QStringLiteral("library_read"));
    if (writeWasOpen)
        QSqlDatabase::removeDatabase(QStringLiteral("library_write"));
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

    // Performance indexes for large libraries (GROUP BY in rebuild, search)
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title)"));

    // FTS5 full-text search index (replaces LIKE '%keyword%' table scan)
    q.exec(QStringLiteral(
        "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
        "  title, artist, album,"
        "  content=tracks,"
        "  content_rowid=rowid"
        ")"
    ));
    qDebug() << "[LibraryDB] FTS5 index created/verified";

    // Migration: add R128 loudness columns (safe to call multiple times)
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN r128_loudness REAL DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN r128_peak REAL DEFAULT 0"));

    // Migration: add channel_count column
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN channel_count INTEGER DEFAULT 2"));

    // Migration: add file_size and file_mtime for mtime-based scan skip
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN file_size INTEGER DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN file_mtime INTEGER DEFAULT 0"));

    // Covering index for batch skip-check query (path+size+mtime in one B-tree scan)
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_tracks_path_size_mtime "
        "ON tracks(file_path, file_size, file_mtime)"));
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

    // File size/mtime for scan skip
    int fsIdx = query.record().indexOf(QStringLiteral("file_size"));
    if (fsIdx >= 0) t.fileSize = query.value(fsIdx).toLongLong();
    int mtIdx = query.record().indexOf(QStringLiteral("file_mtime"));
    if (mtIdx >= 0) t.fileMtime = query.value(mtIdx).toLongLong();

    return t;
}

// ── Tracks ──────────────────────────────────────────────────────────
bool LibraryDatabase::trackExists(const QString& filePath) const
{
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() > 0;
    }
    return false;
}

QHash<QString, QPair<qint64, qint64>> LibraryDatabase::allTrackFileMeta() const
{
    QMutexLocker lock(&m_readMutex);
    QElapsedTimer t; t.start();
    QHash<QString, QPair<qint64, qint64>> result;
    QSqlQuery q(m_readDb);
    q.exec(QStringLiteral("SELECT file_path, file_size, file_mtime FROM tracks"));
    result.reserve(10000);
    while (q.next()) {
        result.insert(q.value(0).toString(),
                      qMakePair(q.value(1).toLongLong(), q.value(2).toLongLong()));
    }
    qDebug() << "[TIMING] allTrackFileMeta:" << result.size() << "entries in" << t.elapsed() << "ms";
    return result;
}

void LibraryDatabase::removeDuplicates()
{
    QMutexLocker lock(&m_writeMutex);
    QElapsedTimer t; t.start();
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
    qDebug() << "[TIMING] removeDuplicates:" << t.elapsed() << "ms";
    qDebug() << "=== Duplicate removal complete ===";

    if (before != after) {
        emit databaseChanged();
    }
}

void LibraryDatabase::clearAllData(bool preservePlaylists)
{
    QMutexLocker lock(&m_writeMutex);
    qDebug() << "=== LibraryDatabase::clearAllData ===" << "preservePlaylists:" << preservePlaylists;

    QSqlQuery q(m_db);
    q.exec(QStringLiteral("DELETE FROM play_history"));
    q.exec(QStringLiteral("DELETE FROM metadata_backups"));
    if (!preservePlaylists) {
        q.exec(QStringLiteral("DELETE FROM playlist_tracks"));
        q.exec(QStringLiteral("DELETE FROM playlists"));
    }
    q.exec(QStringLiteral("DELETE FROM tracks"));
    q.exec(QStringLiteral("DELETE FROM albums"));
    q.exec(QStringLiteral("DELETE FROM artists"));
    q.exec(QStringLiteral("VACUUM"));

    clearIncrementalCaches();

    qDebug() << "=== clearAllData complete ===";
    emit databaseChanged();
}

bool LibraryDatabase::insertTrack(const Track& track)
{
    QMutexLocker lock(&m_writeMutex);

    // Incremental: ensure album/artist rows exist and get IDs
    // Skip during batch mode (scanner bulk insert) — rebuildAlbumsAndArtists handles it
    QString artistId = track.artistId;
    QString albumId = track.albumId;

    if (!m_batchMode) {
        if (!track.artist.trimmed().isEmpty() && artistId.isEmpty()) {
            artistId = findOrCreateArtist(track.artist);
        }
        if (!track.album.trimmed().isEmpty() && albumId.isEmpty()) {
            albumId = findOrCreateAlbum(track.album, track.artist, artistId);
        }
    }

    QSqlQuery q(m_db);
    if (!q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO tracks "
        "(id, title, artist, album, album_id, artist_id, duration, format, "
        "sample_rate, bit_depth, bitrate, cover_url, track_number, disc_number, file_path, "
        "recording_mbid, artist_mbid, album_mbid, release_group_mbid, channel_count, "
        "file_size, file_mtime) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    ))) {
        qWarning() << "LibraryDatabase::insertTrack PREPARE failed:" << q.lastError().text();
        return false;
    }

    QString id = track.id.isEmpty() ? generateId() : track.id;
    q.addBindValue(id);
    q.addBindValue(track.title);
    q.addBindValue(track.artist);
    q.addBindValue(track.album);
    q.addBindValue(albumId);
    q.addBindValue(artistId);
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
    q.addBindValue(track.fileSize);
    q.addBindValue(track.fileMtime);

    if (!q.exec()) {
        qWarning() << "LibraryDatabase::insertTrack failed:" << q.lastError().text();
        return false;
    }

    // Update album stats after successful insert (skip in batch mode)
    if (!m_batchMode && !albumId.isEmpty()) {
        updateAlbumStatsIncremental(albumId);
    }

    return true;
}

bool LibraryDatabase::updateTrack(const Track& track)
{
    QMutexLocker lock(&m_writeMutex);
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
        "channel_count = ?, file_size = ?, file_mtime = ? "
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
    q.addBindValue(track.fileSize);
    q.addBindValue(track.fileMtime);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);

    // Capture album/artist before deletion for incremental cleanup (skip in batch mode)
    std::optional<Track> existing;
    if (!m_batchMode)
        existing = trackById(id);

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE id = ?"));
    q.addBindValue(id);
    bool ok = q.exec();

    if (!m_batchMode && ok && existing.has_value()) {
        if (!existing->albumId.isEmpty())
            updateAlbumStatsIncremental(existing->albumId);
        cleanOrphanedAlbumsAndArtists();
    }
    return ok;
}

bool LibraryDatabase::removeTrackByPath(const QString& filePath)
{
    QMutexLocker lock(&m_writeMutex);

    // Capture album/artist before deletion for incremental cleanup (skip in batch mode)
    std::optional<Track> existing;
    if (!m_batchMode)
        existing = trackByPath(filePath);

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    bool ok = q.exec();

    if (!m_batchMode && ok && existing.has_value()) {
        if (!existing->albumId.isEmpty())
            updateAlbumStatsIncremental(existing->albumId);
        cleanOrphanedAlbumsAndArtists();
    }
    return ok;
}

std::optional<Track> LibraryDatabase::trackById(const QString& id) const
{
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
    q.prepare(QStringLiteral("SELECT * FROM tracks WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return trackFromQuery(q);
    }
    return std::nullopt;
}

std::optional<Track> LibraryDatabase::trackByPath(const QString& filePath) const
{
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
    q.prepare(QStringLiteral("SELECT * FROM tracks WHERE file_path = ?"));
    q.addBindValue(filePath);
    if (q.exec() && q.next()) {
        return trackFromQuery(q);
    }
    return std::nullopt;
}

QVector<Track> LibraryDatabase::allTracks() const
{
    QMutexLocker lock(&m_readMutex);
    QElapsedTimer t; t.start();
    QVector<Track> result;
    QSqlQuery q(m_readDb);
    q.exec(QStringLiteral("SELECT * FROM tracks ORDER BY artist, album, disc_number, track_number"));
    while (q.next()) {
        result.append(trackFromQuery(q));
    }
    qDebug() << "[TIMING] allTracks (FULL):" << result.size() << "tracks in" << t.elapsed() << "ms";
    return result;
}

QVector<TrackIndex> LibraryDatabase::allTrackIndexes() const
{
    QMutexLocker lock(&m_readMutex);
    QElapsedTimer t; t.start();
    QVector<TrackIndex> result;
    StringPool pool;  // deduplicates artist/album names (~60% memory savings)

    QSqlQuery q(m_readDb);
    q.exec(QStringLiteral(
        "SELECT id, title, artist, album, duration, format, sample_rate, bit_depth, "
        "track_number, disc_number, file_path, r128_loudness, r128_peak "
        "FROM tracks ORDER BY artist, album, disc_number, track_number"));
    result.reserve(100000);
    while (q.next()) {
        TrackIndex ti;
        ti.id          = q.value(0).toString();
        ti.title       = q.value(1).toString();
        ti.artist      = pool.intern(q.value(2).toString());  // pooled
        ti.album       = pool.intern(q.value(3).toString());  // pooled
        ti.duration    = q.value(4).toInt();
        ti.format      = audioFormatFromString(q.value(5).toString());
        ti.sampleRate  = q.value(6).toString();
        ti.bitDepth    = q.value(7).toString();
        ti.trackNumber = q.value(8).toInt();
        ti.discNumber  = q.value(9).toInt();
        ti.filePath    = q.value(10).toString();
        ti.r128Loudness = q.value(11).toDouble();
        ti.r128Peak     = q.value(12).toDouble();
        ti.hasR128      = (ti.r128Loudness != 0.0);
        result.append(std::move(ti));
    }
    qDebug() << "[TIMING] allTrackIndexes:" << result.size() << "tracks in" << t.elapsed() << "ms";
    qDebug() << "[LibraryDB] Loaded" << result.size() << "track indexes,"
             << "unique strings:" << pool.uniqueCount();
    return result;
}

QVector<QString> LibraryDatabase::searchTracksFTS(const QString& query) const
{
    QMutexLocker lock(&m_readMutex);
    QVector<QString> ids;
    if (query.isEmpty()) return ids;

    // FTS5 query: prefix search with *
    QString ftsQuery = query;
    ftsQuery.replace(QLatin1Char('\''), QStringLiteral("''"));
    ftsQuery += QStringLiteral("*");

    QSqlQuery q(m_readDb);
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
    qDebug() << "[LibraryDB] FTS5 search:" << query << "→" << ids.size() << "results";
    return ids;
}

void LibraryDatabase::rebuildFTSIndex()
{
    QMutexLocker lock(&m_writeMutex);
    QElapsedTimer t; t.start();
    QSqlQuery q(m_db);
    m_db.transaction();
    q.exec(QStringLiteral("DELETE FROM tracks_fts"));
    q.exec(QStringLiteral(
        "INSERT INTO tracks_fts(rowid, title, artist, album) "
        "SELECT rowid, title, artist, album FROM tracks"
    ));
    m_db.commit();
    qDebug() << "[TIMING] rebuildFTSIndex internal:" << t.elapsed() << "ms";
    qDebug() << "[LibraryDB] FTS5 index rebuilt";
}

QVector<Track> LibraryDatabase::searchTracks(const QString& query) const
{
    QMutexLocker lock(&m_readMutex);
    QVector<Track> result;
    if (query.isEmpty()) return result;

    if (query.length() >= 2) {
        // Use FTS5 for 2+ char queries (< 1ms vs table scan)
        QString ftsQuery = query;
        ftsQuery.replace(QLatin1Char('\''), QStringLiteral("''"));
        ftsQuery += QStringLiteral("*");

        QSqlQuery q(m_readDb);
        q.prepare(QStringLiteral(
            "SELECT t.* FROM tracks t "
            "INNER JOIN tracks_fts f ON t.rowid = f.rowid "
            "WHERE tracks_fts MATCH :query "
            "ORDER BY rank LIMIT 200"
        ));
        q.bindValue(QStringLiteral(":query"), ftsQuery);

        if (q.exec()) {
            while (q.next()) {
                result.append(trackFromQuery(q));
            }
        }
        qDebug() << "[LibraryDB] FTS5 search:" << query << "→" << result.size() << "tracks";
    } else {
        // 1 char: fallback to LIKE (FTS5 too broad for single chars)
        QSqlQuery q(m_readDb);
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
    }
    return result;
}

int LibraryDatabase::trackCount() const
{
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
    q.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    if (q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

// ── Albums ──────────────────────────────────────────────────────────
bool LibraryDatabase::insertAlbum(const Album& album)
{
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
    return insertAlbum(album);
}

QVector<Album> LibraryDatabase::allAlbums() const
{
    QMutexLocker lock(&m_readMutex);
    QElapsedTimer t; t.start();
    QVector<Album> result;
    QSqlQuery q(m_readDb);
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

    qDebug() << "[TIMING] allAlbums:" << result.size() << "in" << t.elapsed() << "ms";
    qDebug() << "LibraryDatabase::allAlbums returning" << result.size() << "albums";
    return result;
}

Album LibraryDatabase::albumById(const QString& id) const
{
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
    return insertArtist(artist);
}

QVector<Artist> LibraryDatabase::allArtists() const
{
    QMutexLocker lock(&m_readMutex);
    QElapsedTimer t; t.start();
    QVector<Artist> result;
    QSqlQuery q(m_readDb);
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

    qDebug() << "[TIMING] allArtists:" << result.size() << "in" << t.elapsed() << "ms";
    qDebug() << "LibraryDatabase::allArtists returning" << result.size() << "artists";
    return result;
}

Artist LibraryDatabase::artistById(const QString& id) const
{
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_readMutex);
    QVector<Album> result;
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_readMutex);
    QVector<Artist> result;
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
    return insertPlaylist(playlist);
}

bool LibraryDatabase::removePlaylist(const QString& id)
{
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_readMutex);
    QVector<Playlist> result;
    QSqlQuery q(m_readDb);
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
        QSqlQuery tq(m_readDb);
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
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
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
        QSqlQuery tq(m_readDb);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM playlist_tracks WHERE playlist_id = ? AND track_id = ?"));
    q.addBindValue(playlistId);
    q.addBindValue(trackId);
    return q.exec();
}

bool LibraryDatabase::reorderPlaylistTrack(const QString& playlistId, int fromPos, int toPos)
{
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_readMutex);
    QVector<Track> result;
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_readMutex);
    QVector<Track> result;
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_readMutex);
    QVector<Track> result;
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_readMutex);
    QSqlQuery q(m_readDb);
    q.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM metadata_backups WHERE track_id = ?"));
    q.addBindValue(trackId);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() > 0;
    }
    return false;
}

// ── Transaction helpers ──────────────────────────────────────────────
bool LibraryDatabase::beginTransaction() { QMutexLocker lock(&m_writeMutex); m_batchMode = true; return m_db.transaction(); }
bool LibraryDatabase::commitTransaction() { QMutexLocker lock(&m_writeMutex); m_batchMode = false; return m_db.commit(); }

// ── Database backup / rollback ───────────────────────────────────────

bool LibraryDatabase::createBackup()
{
    QMutexLocker lock(&m_writeMutex);
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
    QMutexLocker lock(&m_writeMutex);
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

// ── Incremental Album/Artist Management ──────────────────────────────

QString LibraryDatabase::findOrCreateArtist(const QString& artistName)
{
    QMutexLocker lock(&m_writeMutex);
    if (artistName.trimmed().isEmpty()) return {};

    QString normalizedName = artistName.trimmed().toLower();

    // 1. Check cache
    auto it = m_artistNameToIdCache.constFind(normalizedName);
    if (it != m_artistNameToIdCache.constEnd())
        return it.value();

    // 2. Check DB
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM artists WHERE LOWER(TRIM(name)) = ?"));
    q.addBindValue(normalizedName);
    if (q.exec() && q.next()) {
        QString id = q.value(0).toString();
        m_artistNameToIdCache.insert(normalizedName, id);
        return id;
    }

    // 3. Create new artist with hash-based ID (matches doRebuildInternal formula)
    QString artistId = QStringLiteral("artist_") +
        QString::number(qHash(normalizedName), 16).rightJustified(8, QLatin1Char('0'));

    QSqlQuery ins(m_db);
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO artists (id, name, cover_url, genres) VALUES (?, ?, '', '')"));
    ins.addBindValue(artistId);
    ins.addBindValue(artistName.trimmed());
    if (!ins.exec()) {
        qWarning() << "[LibraryDB] findOrCreateArtist INSERT failed:" << ins.lastError().text();
    }

    m_artistNameToIdCache.insert(normalizedName, artistId);
    return artistId;
}

QString LibraryDatabase::findOrCreateAlbum(const QString& albumTitle, const QString& artistName, const QString& artistId)
{
    QMutexLocker lock(&m_writeMutex);
    if (albumTitle.trimmed().isEmpty()) return {};

    QString key = albumTitle.trimmed().toLower() + QStringLiteral("||") + artistName.trimmed().toLower();

    // 1. Check cache
    auto it = m_albumKeyToIdCache.constFind(key);
    if (it != m_albumKeyToIdCache.constEnd())
        return it.value();

    // 2. Check DB
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM albums WHERE LOWER(TRIM(title)) = ? AND artist_id = ?"));
    q.addBindValue(albumTitle.trimmed().toLower());
    q.addBindValue(artistId);
    if (q.exec() && q.next()) {
        QString id = q.value(0).toString();
        m_albumKeyToIdCache.insert(key, id);
        return id;
    }

    // 3. Create new album with hash-based ID (matches doRebuildInternal formula)
    QString albumId = QStringLiteral("album_") +
        QString::number(qHash(key), 16).rightJustified(8, QLatin1Char('0'));

    QSqlQuery ins(m_db);
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO albums (id, title, artist, artist_id, year, cover_url, format, total_tracks, duration, genres) "
        "VALUES (?, ?, ?, ?, 0, '', '', 0, 0, '')"));
    ins.addBindValue(albumId);
    ins.addBindValue(albumTitle.trimmed());
    ins.addBindValue(artistName.trimmed());
    ins.addBindValue(artistId);
    if (!ins.exec()) {
        qWarning() << "[LibraryDB] findOrCreateAlbum INSERT failed:" << ins.lastError().text();
    }

    m_albumKeyToIdCache.insert(key, albumId);
    return albumId;
}

void LibraryDatabase::updateAlbumStatsIncremental(const QString& albumId)
{
    QMutexLocker lock(&m_writeMutex);
    if (albumId.isEmpty()) return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE albums SET "
        "  total_tracks = (SELECT COUNT(*) FROM tracks WHERE album_id = ?), "
        "  duration = (SELECT COALESCE(SUM(duration), 0) FROM tracks WHERE album_id = ?) "
        "WHERE id = ?"));
    q.addBindValue(albumId);
    q.addBindValue(albumId);
    q.addBindValue(albumId);
    if (!q.exec()) {
        qWarning() << "[LibraryDB] updateAlbumStatsIncremental failed:" << q.lastError().text();
    }
}

void LibraryDatabase::updateAlbumsAndArtistsForTrack(const Track& track)
{
    QMutexLocker lock(&m_writeMutex);

    QString artistId;
    QString albumId;

    if (!track.artist.trimmed().isEmpty()) {
        artistId = findOrCreateArtist(track.artist);
    }
    if (!track.album.trimmed().isEmpty()) {
        albumId = findOrCreateAlbum(track.album, track.artist, artistId);
    }

    // Update track's album_id and artist_id to match current names
    if (!track.id.isEmpty()) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("UPDATE tracks SET album_id = ?, artist_id = ? WHERE id = ?"));
        q.addBindValue(albumId);
        q.addBindValue(artistId);
        q.addBindValue(track.id);
        q.exec();
    }

    if (!albumId.isEmpty()) {
        updateAlbumStatsIncremental(albumId);
        refreshAlbumMetadataFromTracks(albumId);
    }

    cleanOrphanedAlbumsAndArtists();
}

void LibraryDatabase::cleanOrphanedAlbumsAndArtists()
{
    QMutexLocker lock(&m_writeMutex);

    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "DELETE FROM albums WHERE id NOT IN "
        "(SELECT DISTINCT album_id FROM tracks WHERE album_id IS NOT NULL AND album_id != '')"));
    int albumsRemoved = q.numRowsAffected();

    q.exec(QStringLiteral(
        "DELETE FROM artists WHERE id NOT IN "
        "(SELECT DISTINCT artist_id FROM tracks WHERE artist_id IS NOT NULL AND artist_id != '')"));
    int artistsRemoved = q.numRowsAffected();

    if (albumsRemoved > 0 || artistsRemoved > 0) {
        qDebug() << "[LibraryDB] Cleaned orphaned:" << albumsRemoved << "albums," << artistsRemoved << "artists";
    }
}

void LibraryDatabase::clearIncrementalCaches()
{
    m_artistNameToIdCache.clear();
    m_albumKeyToIdCache.clear();
    qDebug() << "[LibraryDB] Incremental caches cleared";
}

void LibraryDatabase::refreshAlbumMetadataFromTracks(const QString& albumId)
{
    QMutexLocker lock(&m_writeMutex);
    if (albumId.isEmpty()) return;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE albums SET "
        "  cover_url = COALESCE("
        "    (SELECT cover_url FROM tracks WHERE album_id = ? AND cover_url IS NOT NULL AND cover_url != '' LIMIT 1), "
        "    cover_url), "
        "  format = COALESCE("
        "    (SELECT format FROM tracks WHERE album_id = ? LIMIT 1), "
        "    format) "
        "WHERE id = ?"));
    q.addBindValue(albumId);
    q.addBindValue(albumId);
    q.addBindValue(albumId);
    if (!q.exec()) {
        qWarning() << "[LibraryDB] refreshAlbumMetadataFromTracks failed:" << q.lastError().text();
    }
}

// ── Rebuild Albums & Artists from Tracks ─────────────────────────────
void LibraryDatabase::rebuildAlbumsAndArtists()
{
    QMutexLocker lock(&m_writeMutex);
    // Skip if recently rebuilt (5-second cooldown)
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

    // Already running guard
    if (m_rebuildPending.exchange(true)) return;

    lastRebuild.start();
    emit rebuildStarted();

    // If on main thread, dispatch to background to avoid beach ball
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
        qDebug() << "[LibraryDatabase] Rebuild dispatched to background thread";
        auto* thread = QThread::create([this]() {
            doRebuildInternal();
            QMetaObject::invokeMethod(this, [this]() {
                m_rebuildPending.store(false);
                emit databaseChanged();
                emit rebuildFinished();
                qDebug() << "LibraryDatabase::rebuildAlbumsAndArtists - rebuild complete";
            }, Qt::QueuedConnection);
        });
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    } else {
        // Already on worker thread (e.g., from LibraryScanner) — run synchronously
        doRebuildInternal();
        m_rebuildPending.store(false);
        emit databaseChanged();
        emit rebuildFinished();
        qDebug() << "LibraryDatabase::rebuildAlbumsAndArtists - rebuild complete";
    }
}

void LibraryDatabase::doRebuildInternal()
{
    QMutexLocker lock(&m_writeMutex);
    QElapsedTimer rebuildTimer; rebuildTimer.start();
    QElapsedTimer stepTimer;

    // Auto-backup before destructive rebuild
    stepTimer.start();
    createBackup();
    qDebug() << "[TIMING] doRebuild createBackup:" << stepTimer.elapsed() << "ms";

    qDebug() << "LibraryDatabase::rebuildAlbumsAndArtists - starting rebuild...";

    stepTimer.restart();
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
    qDebug() << "[TIMING] doRebuild DELETE+SELECT DISTINCT:" << stepTimer.elapsed() << "ms";
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
    stepTimer.restart();
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

    qDebug() << "[TIMING] doRebuild UPDATE track IDs:" << stepTimer.elapsed() << "ms";

    // Build and insert albums with all fields (coverUrl, stats)
    stepTimer.restart();
    int albumCount = 0;
    int artistCount = 0;
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
        }
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
        artistCount++;
    }

    qDebug() << "[TIMING] doRebuild INSERT albums+artists:" << stepTimer.elapsed() << "ms";

    stepTimer.restart();
    m_db.commit();
    qDebug() << "[TIMING] doRebuild COMMIT:" << stepTimer.elapsed() << "ms";

    qDebug() << "[LibraryDatabase] rebuildAlbumsAndArtists — inserted"
             << albumCount << "albums," << artistCount << "artists";
    qDebug() << "[TIMING] doRebuildInternal TOTAL:" << rebuildTimer.elapsed() << "ms";
}
