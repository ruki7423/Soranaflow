#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QPixmap>

class CoverArtProvider : public QObject {
    Q_OBJECT
public:
    static CoverArtProvider* instance();

    // Fetch album art by MusicBrainz release or release-group ID
    void fetchAlbumArt(const QString& mbid, bool isReleaseGroup = false);

    // Returns cached path, or empty string
    QString getCachedArtPath(const QString& mbid);

signals:
    void albumArtFetched(const QString& mbid, const QPixmap& pixmap,
                         const QString& savedPath);
    void albumArtNotFound(const QString& mbid);
    void fetchError(const QString& mbid, const QString& error);

private:
    explicit CoverArtProvider(QObject* parent = nullptr);

    void downloadImage(const QString& url, const QString& mbid);
    QString cacheDir();

    QNetworkAccessManager* m_network;

    static const QString API_BASE;
};
