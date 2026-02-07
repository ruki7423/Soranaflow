#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMap>
#include <functional>

struct MusicBrainzResult {
    QString mbid;               // MusicBrainz recording ID
    QString title;
    QString artist;
    QString artistMbid;
    QString album;
    QString albumMbid;          // release ID
    QString releaseGroupMbid;
    int     year = 0;
    int     trackNumber = 0;
    int     discNumber = 0;
    QString genre;
    double  score = 0.0;        // Match confidence 0-100
};

class RateLimiter;

class MusicBrainzProvider : public QObject {
    Q_OBJECT
public:
    static MusicBrainzProvider* instance();

    void searchTrack(const QString& title, const QString& artist,
                     const QString& album = QString());
    void searchAlbum(const QString& album, const QString& artist);
    void searchArtist(const QString& artist);

    void lookupArtist(const QString& mbid);

    void searchTrackMultiple(const QString& title, const QString& artist,
                             const QString& album = QString());

signals:
    void trackFound(const MusicBrainzResult& result);
    void multipleTracksFound(const QVector<MusicBrainzResult>& results);
    void albumFound(const QString& mbid, const QString& releaseGroupMbid,
                    const QJsonObject& data);
    void artistFound(const QString& mbid, const QJsonObject& data);
    void searchError(const QString& error);
    void noResultsFound();

private:
    explicit MusicBrainzProvider(QObject* parent = nullptr);

    void makeRequest(const QString& endpoint,
                     const QMap<QString, QString>& params,
                     std::function<void(const QJsonDocument&)> callback);

    QVector<MusicBrainzResult> parseRecordings(const QJsonDocument& doc);

    QNetworkAccessManager* m_network;
    RateLimiter* m_rateLimiter;

    static const QString API_BASE;
    static const QString USER_AGENT;
};
