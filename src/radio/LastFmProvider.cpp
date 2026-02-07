#include "LastFmProvider.h"
#include "RateLimiter.h"

#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QDebug>

const QString LastFmProvider::API_KEY = QStringLiteral("7ab675085fa7b32a894631f2643b6a6f");
const QString LastFmProvider::BASE_URL = QStringLiteral("https://ws.audioscrobbler.com/2.0/");

// ── Singleton ───────────────────────────────────────────────────────
LastFmProvider* LastFmProvider::instance()
{
    static LastFmProvider s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
LastFmProvider::LastFmProvider(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_rateLimiter(new RateLimiter(5, this))  // 5 requests/sec
{
}

// ── fetchSimilarTracks ──────────────────────────────────────────────
void LastFmProvider::fetchSimilarTracks(const QString& artist, const QString& title)
{
    if (artist.isEmpty() || title.isEmpty()) {
        emit fetchError(QStringLiteral("Artist or title is empty"));
        return;
    }

    m_rateLimiter->enqueue([this, artist, title]() {
        QUrl url(BASE_URL);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("method"), QStringLiteral("track.getsimilar"));
        query.addQueryItem(QStringLiteral("artist"), artist);
        query.addQueryItem(QStringLiteral("track"), title);
        query.addQueryItem(QStringLiteral("api_key"), API_KEY);
        query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("SoranaFlow/1.0 (contact@soranaflow.com)"));

        auto* reply = m_network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                qDebug() << "[LastFm] Network error:" << reply->errorString();
                emit fetchError(reply->errorString());
                return;
            }

            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonObject root = doc.object();

            if (root.contains(QStringLiteral("error"))) {
                QString msg = root.value(QStringLiteral("message")).toString();
                qDebug() << "[LastFm] API error:" << msg;
                emit fetchError(msg);
                return;
            }

            QList<QPair<QString, QString>> results;
            QJsonArray tracks = root.value(QStringLiteral("similartracks"))
                                    .toObject()
                                    .value(QStringLiteral("track"))
                                    .toArray();

            for (const QJsonValue& v : tracks) {
                QJsonObject t = v.toObject();
                QString trackArtist = t.value(QStringLiteral("artist"))
                                       .toObject()
                                       .value(QStringLiteral("name"))
                                       .toString();
                QString trackName = t.value(QStringLiteral("name")).toString();
                if (!trackArtist.isEmpty() && !trackName.isEmpty()) {
                    results.append({trackArtist, trackName});
                }
            }

            qDebug() << "[LastFm] Similar tracks:" << results.size();
            emit similarTracksFetched(results);
        });
    });
}

// ── fetchSimilarArtists ─────────────────────────────────────────────
void LastFmProvider::fetchSimilarArtists(const QString& artist)
{
    if (artist.isEmpty()) {
        emit fetchError(QStringLiteral("Artist is empty"));
        return;
    }

    m_rateLimiter->enqueue([this, artist]() {
        QUrl url(BASE_URL);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("method"), QStringLiteral("artist.getsimilar"));
        query.addQueryItem(QStringLiteral("artist"), artist);
        query.addQueryItem(QStringLiteral("api_key"), API_KEY);
        query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("SoranaFlow/1.0 (contact@soranaflow.com)"));

        auto* reply = m_network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                qDebug() << "[LastFm] Network error:" << reply->errorString();
                emit fetchError(reply->errorString());
                return;
            }

            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonObject root = doc.object();

            if (root.contains(QStringLiteral("error"))) {
                QString msg = root.value(QStringLiteral("message")).toString();
                qDebug() << "[LastFm] API error:" << msg;
                emit fetchError(msg);
                return;
            }

            QStringList results;
            QJsonArray artists = root.value(QStringLiteral("similarartists"))
                                     .toObject()
                                     .value(QStringLiteral("artist"))
                                     .toArray();

            for (const QJsonValue& v : artists) {
                QString name = v.toObject().value(QStringLiteral("name")).toString();
                if (!name.isEmpty()) {
                    results.append(name);
                }
            }

            qDebug() << "[LastFm] Similar artists:" << results.size();
            emit similarArtistsFetched(results);
        });
    });
}
