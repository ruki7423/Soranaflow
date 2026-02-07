#include "MusicBrainzProvider.h"
#include "RateLimiter.h"

#include <QNetworkReply>
#include <QUrlQuery>
#include <QJsonArray>
#include <QDebug>
#include <QSet>
#include <QSharedPointer>

const QString MusicBrainzProvider::API_BASE =
    QStringLiteral("https://musicbrainz.org/ws/2");
const QString MusicBrainzProvider::USER_AGENT =
    QStringLiteral("SoranaFlow/1.2.0 (https://github.com/soranaflow)");

MusicBrainzProvider* MusicBrainzProvider::instance()
{
    static MusicBrainzProvider s_instance;
    return &s_instance;
}

MusicBrainzProvider::MusicBrainzProvider(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_rateLimiter(new RateLimiter(1, this))   // 1 req/sec per MB policy
{
}

// ── Generic request helper ──────────────────────────────────────────
void MusicBrainzProvider::makeRequest(
    const QString& endpoint,
    const QMap<QString, QString>& params,
    std::function<void(const QJsonDocument&)> callback)
{
    m_rateLimiter->enqueue([=, this]() {
        QUrl url(API_BASE + endpoint);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
        for (auto it = params.cbegin(); it != params.cend(); ++it)
            query.addQueryItem(it.key(), it.value());
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", USER_AGENT.toUtf8());
        request.setRawHeader("Accept", "application/json");
        request.setTransferTimeout(15000);

        qDebug() << "[MusicBrainz] GET" << url.toString();

        QNetworkReply* reply = m_network->get(request);
        connect(reply, &QNetworkReply::finished, this, [=, this]() {
            reply->deleteLater();
            int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (reply->error() != QNetworkReply::NoError) {
                qWarning() << "[MusicBrainz] HTTP" << status
                           << reply->errorString();
                emit searchError(reply->errorString());
                return;
            }

            QByteArray body = reply->readAll();
            qDebug() << "[MusicBrainz] HTTP" << status
                     << "size:" << body.size();
            QJsonDocument doc = QJsonDocument::fromJson(body);
            callback(doc);
        });
    });
}

// ── Search track ────────────────────────────────────────────────────
void MusicBrainzProvider::searchTrack(const QString& title,
                                       const QString& artist,
                                       const QString& album)
{
    QStringList parts;
    if (!title.trimmed().isEmpty())
        parts << QStringLiteral("recording:\"%1\"").arg(title.trimmed());
    if (!artist.trimmed().isEmpty())
        parts << QStringLiteral("artist:\"%1\"").arg(artist.trimmed());
    if (!album.trimmed().isEmpty())
        parts << QStringLiteral("release:\"%1\"").arg(album.trimmed());

    if (parts.isEmpty()) {
        emit noResultsFound();
        return;
    }

    QString q = parts.join(QStringLiteral(" AND "));

    QMap<QString, QString> params;
    params[QStringLiteral("query")] = q;
    params[QStringLiteral("limit")] = QStringLiteral("5");

    makeRequest(QStringLiteral("/recording"), params,
                [this](const QJsonDocument& doc) {
        QJsonArray recordings =
            doc.object()[QStringLiteral("recordings")].toArray();
        if (recordings.isEmpty()) {
            qDebug() << "[MusicBrainz] No recordings found";
            emit noResultsFound();
            return;
        }

        QJsonObject best = recordings[0].toObject();
        MusicBrainzResult result;
        result.mbid  = best[QStringLiteral("id")].toString();
        result.title = best[QStringLiteral("title")].toString();
        result.score = best[QStringLiteral("score")].toDouble();

        // Artist
        QJsonArray artists =
            best[QStringLiteral("artist-credit")].toArray();
        if (!artists.isEmpty()) {
            QJsonObject creditObj = artists[0].toObject();
            QJsonObject aObj =
                creditObj[QStringLiteral("artist")].toObject();
            // Use credited name first, fall back to canonical name
            result.artist = creditObj[QStringLiteral("name")].toString();
            if (result.artist.isEmpty())
                result.artist = aObj[QStringLiteral("name")].toString();
            result.artistMbid = aObj[QStringLiteral("id")].toString();
        }

        // Release (album) — walk all releases for best data
        QJsonArray releases =
            best[QStringLiteral("releases")].toArray();
        for (const auto& relVal : releases) {
            QJsonObject rel = relVal.toObject();

            if (result.albumMbid.isEmpty()) {
                result.album    = rel[QStringLiteral("title")].toString();
                result.albumMbid = rel[QStringLiteral("id")].toString();

                QJsonObject rg =
                    rel[QStringLiteral("release-group")].toObject();
                result.releaseGroupMbid =
                    rg[QStringLiteral("id")].toString();

                QString date = rel[QStringLiteral("date")].toString();
                if (date.length() >= 4)
                    result.year = date.left(4).toInt();

                QJsonArray media =
                    rel[QStringLiteral("media")].toArray();
                if (!media.isEmpty()) {
                    QJsonArray tracks = media[0].toObject()
                        [QStringLiteral("track")].toArray();
                    if (!tracks.isEmpty())
                        result.trackNumber = tracks[0].toObject()
                            [QStringLiteral("number")].toString().toInt();
                }
            }

            // If we got a release-group MBID, stop searching
            if (!result.releaseGroupMbid.isEmpty())
                break;
        }

        qDebug() << "[MusicBrainz] Result:"
                 << result.artist << "-" << result.title
                 << "| album:" << result.album
                 << "| albumMbid:" << result.albumMbid
                 << "| rgMbid:" << result.releaseGroupMbid
                 << "| artistMbid:" << result.artistMbid
                 << "| score:" << result.score;

        emit trackFound(result);
    });
}

// ── Parse recordings from JSON response ─────────────────────────────
QVector<MusicBrainzResult> MusicBrainzProvider::parseRecordings(
    const QJsonDocument& doc)
{
    QVector<MusicBrainzResult> results;
    QJsonArray recordings =
        doc.object()[QStringLiteral("recordings")].toArray();

    for (const auto& recVal : recordings) {
        QJsonObject rec = recVal.toObject();
        MusicBrainzResult result;
        result.mbid  = rec[QStringLiteral("id")].toString();
        result.title = rec[QStringLiteral("title")].toString();
        result.score = rec[QStringLiteral("score")].toDouble();

        // Artist — join all artist credits
        QJsonArray artists =
            rec[QStringLiteral("artist-credit")].toArray();
        QStringList artistNames;
        for (const auto& creditVal : artists) {
            QJsonObject creditObj = creditVal.toObject();
            QJsonObject aObj =
                creditObj[QStringLiteral("artist")].toObject();
            QString name = creditObj[QStringLiteral("name")].toString();
            if (name.isEmpty())
                name = aObj[QStringLiteral("name")].toString();
            if (!name.isEmpty())
                artistNames << name;
            if (result.artistMbid.isEmpty())
                result.artistMbid = aObj[QStringLiteral("id")].toString();
            // Append joinphrase (e.g. " feat. ", " & ")
            QString join = creditObj[QStringLiteral("joinphrase")].toString();
            if (!join.isEmpty() && !artistNames.isEmpty())
                artistNames.last().append(join);
        }
        result.artist = artistNames.join(QString());

        // Release (album)
        QJsonArray releases =
            rec[QStringLiteral("releases")].toArray();
        for (const auto& relVal : releases) {
            QJsonObject rel = relVal.toObject();
            if (result.albumMbid.isEmpty()) {
                result.album     = rel[QStringLiteral("title")].toString();
                result.albumMbid = rel[QStringLiteral("id")].toString();

                QJsonObject rg =
                    rel[QStringLiteral("release-group")].toObject();
                result.releaseGroupMbid =
                    rg[QStringLiteral("id")].toString();

                QString date = rel[QStringLiteral("date")].toString();
                if (date.length() >= 4)
                    result.year = date.left(4).toInt();

                QJsonArray media =
                    rel[QStringLiteral("media")].toArray();
                if (!media.isEmpty()) {
                    QJsonArray tracks = media[0].toObject()
                        [QStringLiteral("track")].toArray();
                    if (!tracks.isEmpty())
                        result.trackNumber = tracks[0].toObject()
                            [QStringLiteral("number")].toString().toInt();
                }
            }
            if (!result.releaseGroupMbid.isEmpty())
                break;
        }

        results.append(result);
    }
    return results;
}

// ── Search track (multiple results) ─────────────────────────────────
void MusicBrainzProvider::searchTrackMultiple(const QString& title,
                                               const QString& artist,
                                               const QString& album)
{
    QString titleT  = title.trimmed();
    QString artistT = artist.trimmed();
    QString albumT  = album.trimmed();

    if (titleT.isEmpty() && artistT.isEmpty() && albumT.isEmpty()) {
        qDebug() << "[MusicBrainz] Search: all fields empty, skipping";
        emit noResultsFound();
        return;
    }

    // Build field-qualified query (Strategy A)
    QStringList qualifiedParts;
    if (!titleT.isEmpty())
        qualifiedParts << QStringLiteral("recording:\"%1\"").arg(titleT);
    if (!artistT.isEmpty())
        qualifiedParts << QStringLiteral("artist:\"%1\"").arg(artistT);
    if (!albumT.isEmpty())
        qualifiedParts << QStringLiteral("release:\"%1\"").arg(albumT);

    QString queryA = qualifiedParts.join(QStringLiteral(" AND "));

    // Dual search when both title AND artist are provided
    // Strategy B uses unqualified terms which search across all indexed
    // fields including artist sort-name and aliases — needed for
    // romanized artist names (e.g. "Kenshi Yonezu" → 米津玄師)
    bool dualSearch = !titleT.isEmpty() && !artistT.isEmpty();

    if (!dualSearch) {
        // Single strategy — higher limit for single-field searches
        int limit = (qualifiedParts.size() == 1) ? 50 : 25;
        QMap<QString, QString> params;
        params[QStringLiteral("query")] = queryA;
        params[QStringLiteral("limit")] = QString::number(limit);

        qDebug() << "[MusicBrainz] Single search:" << queryA
                 << "limit:" << limit;

        makeRequest(QStringLiteral("/recording"), params,
                    [this](const QJsonDocument& doc) {
            QVector<MusicBrainzResult> results = parseRecordings(doc);
            qDebug() << "[MusicBrainz] Results:" << results.size();
            if (results.isEmpty())
                emit noResultsFound();
            else
                emit multipleTracksFound(results);
        });
        return;
    }

    // ── Dual search strategy ──────────────────────────────────────
    // Strategy B: unqualified phrases search all fields
    QString queryB = QStringLiteral("\"%1\" \"%2\"").arg(titleT, artistT);

    qDebug() << "[MusicBrainz] Dual search A:" << queryA;
    qDebug() << "[MusicBrainz] Dual search B:" << queryB;

    // Shared state — both callbacks write here, last one emits signal
    struct SearchState {
        QVector<MusicBrainzResult> results;
        QSet<QString> seenMbids;
        int pending = 2;
    };
    auto state = QSharedPointer<SearchState>::create();

    auto mergeBatch = [this, state](const QJsonDocument& doc) {
        QVector<MusicBrainzResult> batch = parseRecordings(doc);
        for (const auto& r : batch) {
            if (!state->seenMbids.contains(r.mbid)) {
                state->seenMbids.insert(r.mbid);
                state->results.append(r);
            }
        }
        state->pending--;
        if (state->pending == 0) {
            // Sort by score descending
            std::sort(state->results.begin(), state->results.end(),
                [](const MusicBrainzResult& a, const MusicBrainzResult& b) {
                    return a.score > b.score;
                });
            qDebug() << "[MusicBrainz] Dual search merged:"
                     << state->results.size() << "unique results";
            if (state->results.isEmpty())
                emit noResultsFound();
            else
                emit multipleTracksFound(state->results);
        }
    };

    // Fire both queries — RateLimiter spaces them ≥1s apart
    auto fireQuery = [this, mergeBatch, state](const QString& query) {
        m_rateLimiter->enqueue([=, this]() {
            QUrl url(API_BASE + QStringLiteral("/recording"));
            QUrlQuery urlQuery;
            urlQuery.addQueryItem(QStringLiteral("fmt"),
                                  QStringLiteral("json"));
            urlQuery.addQueryItem(QStringLiteral("query"), query);
            urlQuery.addQueryItem(QStringLiteral("limit"),
                                  QStringLiteral("20"));
            url.setQuery(urlQuery);

            QNetworkRequest request(url);
            request.setRawHeader("User-Agent", USER_AGENT.toUtf8());
            request.setRawHeader("Accept", "application/json");
            request.setTransferTimeout(15000);

            qDebug() << "[MusicBrainz] GET" << url.toString();

            QNetworkReply* reply = m_network->get(request);
            connect(reply, &QNetworkReply::finished, this,
                    [=, this]() {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    qWarning() << "[MusicBrainz] HTTP error:"
                               << reply->errorString();
                    // Decrement pending even on error so merge still fires
                    state->pending--;
                    if (state->pending == 0) {
                        if (state->results.isEmpty())
                            emit noResultsFound();
                        else
                            emit multipleTracksFound(state->results);
                    }
                    return;
                }
                QByteArray body = reply->readAll();
                qDebug() << "[MusicBrainz] HTTP"
                         << reply->attribute(
                                QNetworkRequest::HttpStatusCodeAttribute).toInt()
                         << "size:" << body.size();
                QJsonDocument doc = QJsonDocument::fromJson(body);
                mergeBatch(doc);
            });
        });
    };

    fireQuery(queryA);
    fireQuery(queryB);
}

// ── Search album ────────────────────────────────────────────────────
void MusicBrainzProvider::searchAlbum(const QString& album,
                                       const QString& artist)
{
    QString q = QStringLiteral("release:\"%1\"").arg(album);
    if (!artist.isEmpty())
        q += QStringLiteral(" AND artist:\"%1\"").arg(artist);

    QMap<QString, QString> params;
    params[QStringLiteral("query")] = q;
    params[QStringLiteral("limit")] = QStringLiteral("1");

    makeRequest(QStringLiteral("/release"), params,
                [this](const QJsonDocument& doc) {
        QJsonArray releases =
            doc.object()[QStringLiteral("releases")].toArray();
        if (releases.isEmpty()) {
            emit noResultsFound();
            return;
        }
        QJsonObject rel = releases[0].toObject();
        QString mbid   = rel[QStringLiteral("id")].toString();
        QString rgMbid = rel[QStringLiteral("release-group")]
                             .toObject()[QStringLiteral("id")].toString();
        qDebug() << "[MusicBrainz] Album found:"
                 << rel[QStringLiteral("title")].toString()
                 << "mbid:" << mbid << "rgMbid:" << rgMbid;
        emit albumFound(mbid, rgMbid, rel);
    });
}

// ── Search artist ───────────────────────────────────────────────────
void MusicBrainzProvider::searchArtist(const QString& artist)
{
    QMap<QString, QString> params;
    params[QStringLiteral("query")] =
        QStringLiteral("artist:\"%1\"").arg(artist);
    params[QStringLiteral("limit")] = QStringLiteral("1");

    makeRequest(QStringLiteral("/artist"), params,
                [this](const QJsonDocument& doc) {
        QJsonArray artists =
            doc.object()[QStringLiteral("artists")].toArray();
        if (artists.isEmpty()) {
            emit noResultsFound();
            return;
        }
        QJsonObject obj = artists[0].toObject();
        emit artistFound(obj[QStringLiteral("id")].toString(), obj);
    });
}

// ── Lookup artist by MBID ───────────────────────────────────────────
void MusicBrainzProvider::lookupArtist(const QString& mbid)
{
    makeRequest(QStringLiteral("/artist/") + mbid,
                {{QStringLiteral("inc"), QStringLiteral("annotation+url-rels")}},
                [this, mbid](const QJsonDocument& doc) {
        emit artistFound(mbid, doc.object());
    });
}
