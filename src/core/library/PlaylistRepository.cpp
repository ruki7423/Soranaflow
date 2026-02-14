#include "PlaylistRepository.h"
#include "DatabaseContext.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>

PlaylistRepository::PlaylistRepository(DatabaseContext* ctx)
    : m_ctx(ctx)
{
}

bool PlaylistRepository::insertPlaylist(const Playlist& playlist)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO playlists (id, name, description, cover_url, is_smart, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?)"
    ));

    QString id = playlist.id.isEmpty() ? m_ctx->generateId() : playlist.id;
    q.addBindValue(id);
    q.addBindValue(playlist.name);
    q.addBindValue(playlist.description);
    q.addBindValue(playlist.coverUrl);
    q.addBindValue(playlist.isSmartPlaylist ? 1 : 0);
    q.addBindValue(playlist.createdAt.isEmpty()
        ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        : playlist.createdAt);

    if (!q.exec()) {
        qWarning() << "PlaylistRepository::insertPlaylist failed:" << q.lastError().text();
        return false;
    }

    // Insert playlist tracks
    if (!playlist.tracks.isEmpty()) {
        QSqlQuery del(*m_ctx->writeDb);
        del.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id = ?"));
        del.addBindValue(id);
        del.exec();

        for (int i = 0; i < playlist.tracks.size(); ++i) {
            addTrackToPlaylist(id, playlist.tracks[i].id, i);
        }
    }

    return true;
}

bool PlaylistRepository::updatePlaylist(const Playlist& playlist)
{
    QMutexLocker lock(m_ctx->writeMutex);
    return insertPlaylist(playlist);
}

bool PlaylistRepository::removePlaylist(const QString& id)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral("DELETE FROM playlists WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec()) return false;

    // Cascade should handle playlist_tracks, but just in case
    QSqlQuery q2(*m_ctx->writeDb);
    q2.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id = ?"));
    q2.addBindValue(id);
    q2.exec();

    return true;
}

QVector<Playlist> PlaylistRepository::allPlaylists() const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<Playlist> result;
    QSqlQuery q(*m_ctx->readDb);
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
        QSqlQuery tq(*m_ctx->readDb);
        tq.prepare(QStringLiteral(
            "SELECT t.* FROM tracks t "
            "JOIN playlist_tracks pt ON t.id = pt.track_id "
            "WHERE pt.playlist_id = ? "
            "ORDER BY pt.position"
        ));
        tq.addBindValue(p.id);
        if (tq.exec()) {
            while (tq.next()) {
                p.tracks.append(m_ctx->trackFromQuery(tq));
            }
        }

        result.append(p);
    }
    return result;
}

Playlist PlaylistRepository::playlistById(const QString& id) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
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
        QSqlQuery tq(*m_ctx->readDb);
        tq.prepare(QStringLiteral(
            "SELECT t.* FROM tracks t "
            "JOIN playlist_tracks pt ON t.id = pt.track_id "
            "WHERE pt.playlist_id = ? "
            "ORDER BY pt.position"
        ));
        tq.addBindValue(p.id);
        if (tq.exec()) {
            while (tq.next()) {
                p.tracks.append(m_ctx->trackFromQuery(tq));
            }
        }
        return p;
    }
    return Playlist{};
}

bool PlaylistRepository::addTrackToPlaylist(const QString& playlistId, const QString& trackId, int position)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);

    if (position < 0) {
        // Append at end
        QSqlQuery maxQ(*m_ctx->writeDb);
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

bool PlaylistRepository::removeTrackFromPlaylist(const QString& playlistId, const QString& trackId)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "DELETE FROM playlist_tracks WHERE playlist_id = ? AND track_id = ?"));
    q.addBindValue(playlistId);
    q.addBindValue(trackId);
    return q.exec();
}

bool PlaylistRepository::reorderPlaylistTrack(const QString& playlistId, int fromPos, int toPos)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
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
        QSqlQuery shift(*m_ctx->writeDb);
        shift.prepare(QStringLiteral(
            "UPDATE playlist_tracks SET position = position - 1 "
            "WHERE playlist_id = ? AND position > ? AND position <= ?"));
        shift.addBindValue(playlistId);
        shift.addBindValue(fromPos);
        shift.addBindValue(toPos);
        shift.exec();
    } else {
        QSqlQuery shift(*m_ctx->writeDb);
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
