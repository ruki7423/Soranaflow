#include "ArtistRepository.h"
#include "DatabaseContext.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>

ArtistRepository::ArtistRepository(DatabaseContext* ctx)
    : m_ctx(ctx)
{
}

// ── Artists ─────────────────────────────────────────────────────────
bool ArtistRepository::insertArtist(const Artist& artist)
{
    QMutexLocker lock(m_ctx->writeMutex);
    QSqlQuery q(*m_ctx->writeDb);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO artists (id, name, cover_url, genres) "
        "VALUES (?, ?, ?, ?)"
    ));

    q.addBindValue(artist.id);
    q.addBindValue(artist.name);
    q.addBindValue(artist.coverUrl);
    q.addBindValue(artist.genres.join(QStringLiteral(",")));

    if (!q.exec()) {
        qWarning() << "ArtistRepository::insertArtist failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool ArtistRepository::updateArtist(const Artist& artist)
{
    QMutexLocker lock(m_ctx->writeMutex);
    return insertArtist(artist);
}

QVector<Artist> ArtistRepository::allArtists() const
{
    QMutexLocker lock(m_ctx->readMutex);
    QElapsedTimer t; t.start();
    QVector<Artist> result;
    QSqlQuery q(*m_ctx->readDb);
    q.exec(QStringLiteral("SELECT * FROM artists ORDER BY name"));

    if (q.lastError().isValid()) {
        qWarning() << "ArtistRepository::allArtists query error:" << q.lastError().text();
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
    qDebug() << "ArtistRepository::allArtists returning" << result.size() << "artists";
    return result;
}

Artist ArtistRepository::artistById(const QString& id) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
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

        // Load albums (inlined from albumById to avoid cross-repository dependency)
        QSqlQuery aq(*m_ctx->writeDb);
        aq.prepare(QStringLiteral("SELECT id FROM albums WHERE artist_id = ? ORDER BY year"));
        aq.addBindValue(a.id);
        if (aq.exec()) {
            while (aq.next()) {
                QString albumId = aq.value(0).toString();

                // Inline albumById logic
                QSqlQuery abq(*m_ctx->readDb);
                abq.prepare(QStringLiteral("SELECT * FROM albums WHERE id = ?"));
                abq.addBindValue(albumId);
                if (abq.exec() && abq.next()) {
                    Album alb;
                    alb.id          = abq.value(QStringLiteral("id")).toString();
                    alb.title       = abq.value(QStringLiteral("title")).toString();
                    alb.artist      = abq.value(QStringLiteral("artist")).toString();
                    alb.artistId    = abq.value(QStringLiteral("artist_id")).toString();
                    alb.year        = abq.value(QStringLiteral("year")).toInt();
                    alb.coverUrl    = abq.value(QStringLiteral("cover_url")).toString();
                    alb.format      = m_ctx->audioFormatFromString(abq.value(QStringLiteral("format")).toString());
                    alb.totalTracks = abq.value(QStringLiteral("total_tracks")).toInt();
                    alb.duration    = abq.value(QStringLiteral("duration")).toInt();

                    QString albGenresStr = abq.value(QStringLiteral("genres")).toString();
                    if (!albGenresStr.isEmpty())
                        alb.genres = albGenresStr.split(QStringLiteral(","));

                    // Load tracks
                    QSqlQuery tq(*m_ctx->writeDb);
                    tq.prepare(QStringLiteral("SELECT * FROM tracks WHERE album_id = ? ORDER BY disc_number, track_number"));
                    tq.addBindValue(alb.id);
                    if (tq.exec()) {
                        while (tq.next()) {
                            alb.tracks.append(m_ctx->trackFromQuery(tq));
                        }
                    }
                    a.albums.append(alb);
                }
            }
        }
        return a;
    }
    return Artist{};
}

QVector<Artist> ArtistRepository::searchArtists(const QString& query) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QVector<Artist> result;
    QSqlQuery q(*m_ctx->readDb);
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

QString ArtistRepository::artistMbidForArtist(const QString& artistId) const
{
    QMutexLocker lock(m_ctx->readMutex);
    QSqlQuery q(*m_ctx->readDb);
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
