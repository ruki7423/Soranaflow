#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QPixmap>

struct ArtistImages {
    QString artistThumb;        // Small thumbnail
    QString artistBackground;   // 1920x1080 background
    QString hdMusicLogo;        // Transparent logo
    QStringList allThumbs;
    QStringList allBackgrounds;
};

class FanartTvProvider : public QObject {
    Q_OBJECT
public:
    static FanartTvProvider* instance();

    void fetchArtistImages(const QString& artistMbid);
    QString getCachedArtistThumb(const QString& artistMbid);
    QString getCachedArtistBackground(const QString& artistMbid);

signals:
    void artistImagesFetched(const QString& artistMbid,
                             const ArtistImages& images);
    void artistThumbDownloaded(const QString& artistMbid,
                               const QPixmap& pixmap,
                               const QString& savedPath);
    void artistBackgroundDownloaded(const QString& artistMbid,
                                    const QPixmap& pixmap,
                                    const QString& savedPath);
    void artistImagesNotFound(const QString& artistMbid);
    void fetchError(const QString& artistMbid, const QString& error);

private:
    explicit FanartTvProvider(QObject* parent = nullptr);

    void downloadImage(const QString& url, const QString& artistMbid,
                       const QString& type);
    QString cacheDir();

    QNetworkAccessManager* m_network;

    static const QString API_BASE;
    static const QString API_KEY;
};
