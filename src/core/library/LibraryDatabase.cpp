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

    // Initialize DatabaseContext — points to our owned DB connections & mutexes
    m_ctx.writeDb = &m_db;
    m_ctx.readDb = &m_readDb;
    m_ctx.writeMutex = &m_writeMutex;
    m_ctx.readMutex = &m_readMutex;

    // Create repositories
    m_trackRepo = new TrackRepository(&m_ctx);
    m_albumRepo = new AlbumRepository(&m_ctx);
    m_artistRepo = new ArtistRepository(&m_ctx);
    m_playlistRepo = new PlaylistRepository(&m_ctx);
}

LibraryDatabase::~LibraryDatabase()
{
    close();
    delete m_trackRepo;
    delete m_albumRepo;
    delete m_artistRepo;
    delete m_playlistRepo;
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
    if (pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL")) && pragma.next()) {
        QString mode = pragma.value(0).toString().toLower();
        if (mode != QStringLiteral("wal"))
            qWarning() << "[LibraryDB] WAL mode not activated, got:" << mode;
    }
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

    // Integrity check — detect corruption early
    {
        QSqlQuery check(m_readDb);
        if (check.exec(QStringLiteral("PRAGMA quick_check")) && check.next()) {
            QString result = check.value(0).toString();
            if (result != QStringLiteral("ok")) {
                qWarning() << "[LibraryDB] Integrity check FAILED:" << result;
                m_readDb.close();
                m_db.close();
                QString backupPath = m_dbPath + QStringLiteral(".corrupt.")
                    + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
                QFile::rename(m_dbPath, backupPath);
                qWarning() << "[LibraryDB] Corrupt DB backed up to:" << backupPath;
                // Reopen — creates fresh DB, scanner will repopulate
                m_db.open();
                QSqlQuery p2(m_db);
                p2.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
                p2.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
                m_readDb.open();
            } else {
                qDebug() << "[LibraryDB] Integrity check: OK";
            }
        }
    }

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

    // Migration: add album_artist for compilations / VA sorting
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN album_artist TEXT"));

    // Migration: add year to tracks (extracted from DATE/YEAR tags)
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN year INTEGER DEFAULT 0"));

    // Migration: add album_artist to albums (preferred sort field from ALBUMARTIST tag)
    q.exec(QStringLiteral("ALTER TABLE albums ADD COLUMN album_artist TEXT"));

    // Migration: add ReplayGain columns (Bug B fix — v1.7.2)
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN replay_gain_track REAL DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN replay_gain_album REAL DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN replay_gain_track_peak REAL DEFAULT 1.0"));
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN replay_gain_album_peak REAL DEFAULT 1.0"));
    q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN has_replay_gain INTEGER DEFAULT 0"));

    // Covering index for batch skip-check query (path+size+mtime in one B-tree scan)
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_tracks_path_size_mtime "
        "ON tracks(file_path, file_size, file_mtime)"));
}

// ── Helpers moved to DatabaseContext.cpp ─────────────────────────────

// ── Tracks (delegated to TrackRepository) ───────────────────────────
bool LibraryDatabase::trackExists(const QString& filePath) const { return m_trackRepo->trackExists(filePath); }
QHash<QString, QPair<qint64, qint64>> LibraryDatabase::allTrackFileMeta() const { return m_trackRepo->allTrackFileMeta(); }

void LibraryDatabase::removeDuplicates()
{
    if (m_trackRepo->removeDuplicates())
        emit databaseChanged();
}

void LibraryDatabase::clearAllData(bool preservePlaylists)
{
    m_trackRepo->clearAllData(preservePlaylists);
    clearIncrementalCaches();
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
        if (!track.artist.trimmed().isEmpty() && artistId.isEmpty())
            artistId = findOrCreateArtist(track.artist);
        if (!track.album.trimmed().isEmpty() && albumId.isEmpty())
            albumId = findOrCreateAlbum(track.album, track.artist, artistId);
    }

    bool ok = m_trackRepo->insertTrack(track, artistId, albumId);

    // Update album stats after successful insert (skip in batch mode)
    if (ok && !m_batchMode && !albumId.isEmpty())
        updateAlbumStatsIncremental(albumId);

    return ok;
}

bool LibraryDatabase::updateTrack(const Track& track)
{
    if (track.id.isEmpty()) {
        qWarning() << "LibraryDatabase::updateTrack - track has no ID, falling back to insertTrack";
        return insertTrack(track);
    }
    if (!m_trackRepo->updateTrack(track))
        return insertTrack(track);
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
    return m_trackRepo->updateTrackMetadata(trackId, title, artist, album,
                                            recordingMbid, artistMbid, albumMbid, releaseGroupMbid);
}

bool LibraryDatabase::removeTrack(const QString& id)
{
    QMutexLocker lock(&m_writeMutex);

    // Capture album/artist before deletion for incremental cleanup (skip in batch mode)
    std::optional<Track> existing;
    if (!m_batchMode)
        existing = m_trackRepo->trackById(id);

    bool ok = m_trackRepo->removeTrack(id);

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
        existing = m_trackRepo->trackByPath(filePath);

    bool ok = m_trackRepo->removeTrackByPath(filePath);

    if (!m_batchMode && ok && existing.has_value()) {
        if (!existing->albumId.isEmpty())
            updateAlbumStatsIncremental(existing->albumId);
        cleanOrphanedAlbumsAndArtists();
    }
    return ok;
}

std::optional<Track> LibraryDatabase::trackById(const QString& id) const { return m_trackRepo->trackById(id); }
std::optional<Track> LibraryDatabase::trackByPath(const QString& filePath) const { return m_trackRepo->trackByPath(filePath); }
QVector<Track> LibraryDatabase::allTracks() const { return m_trackRepo->allTracks(); }
QVector<TrackIndex> LibraryDatabase::allTrackIndexes() const { return m_trackRepo->allTrackIndexes(); }
QVector<QString> LibraryDatabase::searchTracksFTS(const QString& query) const { return m_trackRepo->searchTracksFTS(query); }
void LibraryDatabase::rebuildFTSIndex() { m_trackRepo->rebuildFTSIndex(); }
QVector<Track> LibraryDatabase::searchTracks(const QString& query) const { return m_trackRepo->searchTracks(query); }
int LibraryDatabase::trackCount() const { return m_trackRepo->trackCount(); }

// ── Albums (delegated to AlbumRepository) ───────────────────────────
bool LibraryDatabase::insertAlbum(const Album& album) { return m_albumRepo->insertAlbum(album); }
bool LibraryDatabase::updateAlbum(const Album& album) { return m_albumRepo->updateAlbum(album); }
QVector<Album> LibraryDatabase::allAlbums() const { return m_albumRepo->allAlbums(); }
Album LibraryDatabase::albumById(const QString& id) const { return m_albumRepo->albumById(id); }
QVector<Album> LibraryDatabase::searchAlbums(const QString& query) const { return m_albumRepo->searchAlbums(query); }

// ── Artists (delegated to ArtistRepository) ─────────────────────────
bool LibraryDatabase::insertArtist(const Artist& artist) { return m_artistRepo->insertArtist(artist); }
bool LibraryDatabase::updateArtist(const Artist& artist) { return m_artistRepo->updateArtist(artist); }
QVector<Artist> LibraryDatabase::allArtists() const { return m_artistRepo->allArtists(); }
Artist LibraryDatabase::artistById(const QString& id) const { return m_artistRepo->artistById(id); }
QVector<Artist> LibraryDatabase::searchArtists(const QString& query) const { return m_artistRepo->searchArtists(query); }

// ── Playlists (delegated to PlaylistRepository) ─────────────────────
bool LibraryDatabase::insertPlaylist(const Playlist& playlist) { return m_playlistRepo->insertPlaylist(playlist); }
bool LibraryDatabase::updatePlaylist(const Playlist& playlist) { return m_playlistRepo->updatePlaylist(playlist); }
bool LibraryDatabase::removePlaylist(const QString& id) { return m_playlistRepo->removePlaylist(id); }
QVector<Playlist> LibraryDatabase::allPlaylists() const { return m_playlistRepo->allPlaylists(); }
Playlist LibraryDatabase::playlistById(const QString& id) const { return m_playlistRepo->playlistById(id); }
bool LibraryDatabase::addTrackToPlaylist(const QString& playlistId, const QString& trackId, int position) { return m_playlistRepo->addTrackToPlaylist(playlistId, trackId, position); }
bool LibraryDatabase::removeTrackFromPlaylist(const QString& playlistId, const QString& trackId) { return m_playlistRepo->removeTrackFromPlaylist(playlistId, trackId); }
bool LibraryDatabase::reorderPlaylistTrack(const QString& playlistId, int fromPos, int toPos) { return m_playlistRepo->reorderPlaylistTrack(playlistId, fromPos, toPos); }

// ── Volume Leveling (delegated to TrackRepository) ──────────────────
void LibraryDatabase::updateR128Loudness(const QString& filePath, double loudness, double peak) { m_trackRepo->updateR128Loudness(filePath, loudness, peak); }

// ── Play History (delegated to TrackRepository) ─────────────────────
void LibraryDatabase::recordPlay(const QString& trackId) { m_trackRepo->recordPlay(trackId); }
QVector<Track> LibraryDatabase::recentlyPlayed(int limit) const { return m_trackRepo->recentlyPlayed(limit); }
QVector<Track> LibraryDatabase::mostPlayed(int limit) const { return m_trackRepo->mostPlayed(limit); }
QVector<Track> LibraryDatabase::recentlyAdded(int limit) const { return m_trackRepo->recentlyAdded(limit); }

// ── MBID Helpers (delegated to AlbumRepository / ArtistRepository) ──
QString LibraryDatabase::releaseGroupMbidForAlbum(const QString& albumId) const { return m_albumRepo->releaseGroupMbidForAlbum(albumId); }
QString LibraryDatabase::artistMbidForArtist(const QString& artistId) const { return m_artistRepo->artistMbidForArtist(artistId); }

// ── Cover Art Helpers (delegated to AlbumRepository) ────────────────
QString LibraryDatabase::firstTrackPathForAlbum(const QString& albumId) const { return m_albumRepo->firstTrackPathForAlbum(albumId); }

// ── Metadata Backup / Undo (delegated to TrackRepository) ───────────
void LibraryDatabase::backupTrackMetadata(const QString& trackId) { m_trackRepo->backupTrackMetadata(trackId); }
bool LibraryDatabase::undoLastMetadataChange(const QString& trackId) { return m_trackRepo->undoLastMetadataChange(trackId); }
bool LibraryDatabase::hasMetadataBackup(const QString& trackId) const { return m_trackRepo->hasMetadataBackup(trackId); }

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
            "  SUM(duration) as total_duration, "
            "  MAX(CASE WHEN year > 0 THEN year ELSE 0 END) as album_year, "
            "  MAX(CASE WHEN album_artist IS NOT NULL AND TRIM(album_artist) != '' "
            "    THEN TRIM(album_artist) ELSE '' END) as album_artist "
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
            a.format      = m_ctx.audioFormatFromString(q.value(4).toString());
            a.coverUrl    = q.value(5).toString();
            a.totalTracks = q.value(6).toInt();
            a.duration    = q.value(7).toInt();
            a.year        = q.value(8).toInt();
            a.albumArtist = q.value(9).toString();

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
