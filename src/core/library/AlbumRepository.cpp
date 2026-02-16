#include "AlbumRepository.h"
#include "DatabaseContext.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QMutex>
#include <QDebug>
#include <QElapsedTimer>

AlbumRepository::AlbumRepository(DatabaseContext* ctx)
    : m_ctx(ctx)
{
}

// ── Albums ──────────────────────────────────────────────────────────
bool AlbumRepository::insertAlbum(const Album& album)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO albums "
        "(id, title, artist, artist_id, year, cover_url, format, total_tracks, duration, genres, album_artist) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    ));

    q.addBindValue(album.id);
    q.addBindValue(album.title);
    q.addBindValue(album.artist);
    q.addBindValue(album.artistId);
    q.addBindValue(album.year);
    q.addBindValue(album.coverUrl);
    q.addBindValue(m_ctx->audioFormatToString(album.format));
    q.addBindValue(album.totalTracks);
    q.addBindValue(album.duration);
    q.addBindValue(album.genres.join(QStringLiteral(",")));
    q.addBindValue(album.albumArtist);

    if (!q.exec()) {
        qWarning() << "AlbumRepository::insertAlbum failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool AlbumRepository::updateAlbum(const Album& album)
{
    QMutexLocker lock(m_ctx->writeMutex);
    return insertAlbum(album);
}

QVector<Album> AlbumRepository::allAlbums() const
{
    QMutexLocker lock(m_ctx->readMutex);
    QElapsedTimer t; t.start();
    QVector<Album> result;
    QSqlQuery q(*m_ctx->readDb);
    q.exec(QStringLiteral(
        "SELECT a.*, "
        "(SELECT MAX(t.added_at) FROM tracks t WHERE t.album_id = a.id) AS date_added "
        "FROM albums a ORDER BY a.artist, a.title"));

    if (q.lastError().isValid()) {
        qWarning() << "AlbumRepository::allAlbums query error:" << q.lastError().text();
    }

    while (q.next()) {
        Album a;
        a.id          = q.value(QStringLiteral("id")).toString();
        a.title       = q.value(QStringLiteral("title")).toString();
        a.artist      = q.value(QStringLiteral("artist")).toString();
        a.artistId    = q.value(QStringLiteral("artist_id")).toString();
        a.year        = q.value(QStringLiteral("year")).toInt();
        a.coverUrl    = q.value(QStringLiteral("cover_url")).toString();
        a.format      = m_ctx->audioFormatFromString(q.value(QStringLiteral("format")).toString());
        a.totalTracks = q.value(QStringLiteral("total_tracks")).toInt();
        a.duration    = q.value(QStringLiteral("duration")).toInt();

        // album_artist (migration column — may not exist in old DBs)
        int aaIdx = q.record().indexOf(QStringLiteral("album_artist"));
        if (aaIdx >= 0)
            a.albumArtist = q.value(aaIdx).toString();

        QString genresStr = q.value(QStringLiteral("genres")).toString();
        if (!genresStr.isEmpty())
            a.genres = genresStr.split(QStringLiteral(","));

        // date_added from subquery (max added_at of tracks in this album)
        int daIdx = q.record().indexOf(QStringLiteral("date_added"));
        if (daIdx >= 0)
            a.dateAdded = q.value(daIdx).toString();

        // Tracks loaded on demand via albumById() — avoids N+1 query problem
        result.append(a);
    }

    qDebug() << "[TIMING] allAlbums:" << result.size() << "in" << t.elapsed() << "ms";
    qDebug() << "AlbumRepository::allAlbums returning" << result.size() << "albums";
    return result;
}

Album AlbumRepository::albumById(const QString& id) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
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
        a.format      = m_ctx->audioFormatFromString(q.value(QStringLiteral("format")).toString());
        a.totalTracks = q.value(QStringLiteral("total_tracks")).toInt();
        a.duration    = q.value(QStringLiteral("duration")).toInt();

        int aaIdx = q.record().indexOf(QStringLiteral("album_artist"));
        if (aaIdx >= 0)
            a.albumArtist = q.value(aaIdx).toString();

        QString genresStr = q.value(QStringLiteral("genres")).toString();
        if (!genresStr.isEmpty())
            a.genres = genresStr.split(QStringLiteral(","));

        // Load tracks (use readDb — we already hold readMutex)
        QSqlQuery tq(*m_ctx->readDb);
        tq.prepare(QStringLiteral("SELECT * FROM tracks WHERE album_id = ? ORDER BY disc_number, track_number"));
        tq.addBindValue(a.id);
        if (tq.exec()) {
            while (tq.next()) {
                a.tracks.append(m_ctx->trackFromQuery(tq));
            }
        }
        return a;
    }
    return Album{};
}

QVector<Album> AlbumRepository::searchAlbums(const QString& query) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<Album> result;
    QSqlQuery q(*m_ctx->readDb);
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
            a.format      = m_ctx->audioFormatFromString(q.value(QStringLiteral("format")).toString());
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

// ── MBID Helpers ─────────────────────────────────────────────────────
QString AlbumRepository::releaseGroupMbidForAlbum(const QString& albumId) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
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

// ── Cover art helpers ────────────────────────────────────────────────
QString AlbumRepository::firstTrackPathForAlbum(const QString& albumId) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
    q.prepare(QStringLiteral(
        "SELECT file_path FROM tracks "
        "WHERE album_id = ? AND file_path IS NOT NULL AND file_path != '' "
        "LIMIT 1"));
    q.addBindValue(albumId);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}
