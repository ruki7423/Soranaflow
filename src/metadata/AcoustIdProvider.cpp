#include "AcoustIdProvider.h"
#include "RateLimiter.h"

#include <QNetworkReply>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

static const QString ACOUSTID_API_URL =
    QStringLiteral("https://api.acoustid.org/v2/lookup");

AcoustIdProvider* AcoustIdProvider::instance()
{
    static AcoustIdProvider s_instance;
    return &s_instance;
}

AcoustIdProvider::AcoustIdProvider(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_rateLimiter(new RateLimiter(3, this))   // AcoustID allows ~3 req/s
    , m_apiKey(QStringLiteral("z0F579krDa"))
{
    qDebug() << "[AcoustIdProvider] Initialized";
}

void AcoustIdProvider::lookup(const QString& fingerprint, int duration, const QString& trackId)
{
    m_rateLimiter->enqueue([this, fingerprint, duration, trackId]() {
        performLookup(fingerprint, duration, trackId);
    });
}

void AcoustIdProvider::performLookup(const QString& fingerprint, int duration, const QString& trackId)
{
    QUrl url(ACOUSTID_API_URL);

    // Use POST body — fingerprints are too long for GET query strings
    QUrlQuery postData;
    postData.addQueryItem(QStringLiteral("client"), m_apiKey);
    postData.addQueryItem(QStringLiteral("duration"), QString::number(duration));
    postData.addQueryItem(QStringLiteral("fingerprint"), fingerprint);
    postData.addQueryItem(QStringLiteral("meta"),
                          QStringLiteral("recordings releases releasegroups artists"));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SoranaFlow/1.3.1"));
    request.setTransferTimeout(15000);

    qDebug() << "[AcoustIdProvider] POST to AcoustID, duration:" << duration
             << "fingerprint length:" << fingerprint.length();

    QNetworkReply* reply = m_nam->post(request,
                                       postData.toString(QUrl::FullyEncoded).toUtf8());

    connect(reply, &QNetworkReply::finished, this, [this, reply, trackId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[AcoustIdProvider] Network error:" << reply->errorString();
            emit lookupError(reply->errorString(), trackId);
            return;
        }

        QByteArray data = reply->readAll();
        qDebug() << "[AcoustIdProvider] Response size:" << data.size() << "bytes";

        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject root = doc.object();

        qDebug() << "[AcoustIdProvider] Status:" << root[QStringLiteral("status")].toString();

        if (root[QStringLiteral("status")].toString() != QStringLiteral("ok")) {
            QString errMsg = root[QStringLiteral("error")]
                                 .toObject()[QStringLiteral("message")]
                                 .toString(QStringLiteral("Unknown AcoustID error"));
            qWarning() << "[AcoustIdProvider] API error:" << errMsg;
            emit lookupError(errMsg, trackId);
            return;
        }

        QJsonArray results = root[QStringLiteral("results")].toArray();
        qDebug() << "[AcoustIdProvider] Number of results:" << results.size();

        if (results.isEmpty()) {
            qDebug() << "[AcoustIdProvider] No results";
            emit noMatch(trackId);
            return;
        }

        // Suffixes that indicate non-studio versions — prefer results without these
        static const QStringList excludeSuffixes = {
            QStringLiteral("demo"), QStringLiteral("remix"), QStringLiteral("live"),
            QStringLiteral("acoustic"), QStringLiteral("instrumental"),
            QStringLiteral("karaoke"), QStringLiteral("a cappella"),
            QStringLiteral("radio edit"), QStringLiteral("radio mix"),
            QStringLiteral("club mix"), QStringLiteral("extended mix"),
            QStringLiteral("unplugged"), QStringLiteral("stripped"),
            QStringLiteral("piano version"), QStringLiteral("orchestral"),
            QStringLiteral("8-bit"), QStringLiteral("8bit"),
            QStringLiteral("cover"), QStringLiteral("tribute"),
            QStringLiteral("remaster")
        };

        auto isNonStudioTitle = [&](const QString& title) -> bool {
            QString lower = title.toLower();
            for (const QString& suffix : excludeSuffixes) {
                if (lower.contains(suffix)) return true;
            }
            return false;
        };

        // Scan all results and recordings to find the best match
        // Prefer: high score + no demo/remix/live suffix
        MusicBrainzResult bestResult;
        double bestScore = -1.0;
        bool bestIsClean = false;

        for (int ri = 0; ri < results.size(); ++ri) {
            QJsonObject res = results[ri].toObject();
            QJsonArray recordings = res[QStringLiteral("recordings")].toArray();
            double score = res[QStringLiteral("score")].toDouble();

            qDebug() << "[AcoustIdProvider] Result" << ri
                     << "score:" << score
                     << "recordings:" << recordings.size();

            if (recordings.isEmpty()) continue;

            for (int ci = 0; ci < recordings.size(); ++ci) {
                QJsonObject rec = recordings[ci].toObject();
                QString title = rec[QStringLiteral("id")].toString().isEmpty()
                              ? QString() : rec[QStringLiteral("title")].toString();

                bool isClean = !isNonStudioTitle(title);

                // Skip this candidate if we already have a clean result with
                // a higher or equal score
                if (bestScore >= 0) {
                    if (bestIsClean && !isClean) continue;
                    if (bestIsClean == isClean && score <= bestScore) continue;
                }
                // A clean result always beats a non-clean one
                if (!bestIsClean && isClean && bestScore >= 0) {
                    // Accept clean even at lower score
                } else if (score <= bestScore && bestIsClean == isClean) {
                    continue;
                }

                MusicBrainzResult candidate;
                candidate.mbid  = rec[QStringLiteral("id")].toString();
                candidate.title = rec[QStringLiteral("title")].toString();
                candidate.score = score * 100.0;

                // Artists
                QJsonArray artists = rec[QStringLiteral("artists")].toArray();
                if (!artists.isEmpty()) {
                    QJsonObject artist = artists[0].toObject();
                    candidate.artist     = artist[QStringLiteral("name")].toString();
                    candidate.artistMbid = artist[QStringLiteral("id")].toString();
                }

                // Release groups — prefer non-compilation, non-live
                QJsonArray releaseGroups = rec[QStringLiteral("releasegroups")].toArray();
                if (!releaseGroups.isEmpty()) {
                    // Try to find an "Album" type release group, fall back to first
                    QJsonObject bestRg = releaseGroups[0].toObject();
                    for (int rgi = 0; rgi < releaseGroups.size(); ++rgi) {
                        QJsonObject rg = releaseGroups[rgi].toObject();
                        QString rgType = rg[QStringLiteral("type")].toString().toLower();
                        if (rgType == QStringLiteral("album")) {
                            bestRg = rg;
                            break;
                        }
                    }

                    candidate.album            = bestRg[QStringLiteral("title")].toString();
                    candidate.releaseGroupMbid = bestRg[QStringLiteral("id")].toString();

                    QJsonArray releases = bestRg[QStringLiteral("releases")].toArray();
                    if (!releases.isEmpty()) {
                        candidate.albumMbid = releases[0].toObject()
                                                  [QStringLiteral("id")].toString();
                    }
                }

                qDebug() << "[AcoustIdProvider]   Candidate:" << candidate.artist
                         << "-" << candidate.title << "score:" << candidate.score
                         << "clean:" << isClean;

                bestResult = candidate;
                bestScore  = score;
                bestIsClean = isClean;
            }
        }

        if (bestScore >= 0) {
            // Reject results with empty title or artist — treat as no match
            if (bestResult.title.trimmed().isEmpty() || bestResult.artist.trimmed().isEmpty()) {
                qDebug() << "[AcoustIdProvider] Rejecting empty result:"
                         << "title=" << bestResult.title << "artist=" << bestResult.artist;
                emit noMatch(trackId);
                return;
            }

            qDebug() << "[AcoustIdProvider] Selected:"
                     << bestResult.artist << "-" << bestResult.title
                     << "score:" << bestResult.score << "clean:" << bestIsClean;
            emit trackIdentified(bestResult, trackId);
        } else {
            qDebug() << "[AcoustIdProvider] Results had no usable recordings";
            emit noMatch(trackId);
        }
    });
}
