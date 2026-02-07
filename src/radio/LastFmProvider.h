#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QPair>
#include <QList>
#include <QStringList>

class RateLimiter;

class LastFmProvider : public QObject {
    Q_OBJECT
public:
    static LastFmProvider* instance();

    void fetchSimilarTracks(const QString& artist, const QString& title);
    void fetchSimilarArtists(const QString& artist);

signals:
    void similarTracksFetched(const QList<QPair<QString, QString>>& tracks);
    void similarArtistsFetched(const QStringList& artists);
    void fetchError(const QString& error);

private:
    explicit LastFmProvider(QObject* parent = nullptr);

    QNetworkAccessManager* m_network;
    RateLimiter* m_rateLimiter;

    static const QString API_KEY;
    static const QString BASE_URL;
};
