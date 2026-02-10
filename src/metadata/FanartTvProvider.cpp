#include "FanartTvProvider.h"
#include <QCoreApplication>

#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QDebug>

const QString FanartTvProvider::API_BASE =
    QStringLiteral("https://webservice.fanart.tv/v3/music");
const QString FanartTvProvider::API_KEY =
    QStringLiteral("767749d7fe98dbe444a96930c486f5e0");

FanartTvProvider* FanartTvProvider::instance()
{
    static FanartTvProvider s_instance;
    return &s_instance;
}

FanartTvProvider::FanartTvProvider(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    QString dir = cacheDir();
    bool ok = QDir().mkpath(dir);
    qDebug() << "[Fanart.tv] Cache dir:" << dir << "exists:" << ok;
}

QString FanartTvProvider::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/artist_images");
}

QString FanartTvProvider::getCachedArtistThumb(const QString& artistMbid)
{
    QString path = cacheDir() + QStringLiteral("/") + artistMbid
                   + QStringLiteral("_thumb.jpg");
    return QFile::exists(path) ? path : QString();
}

QString FanartTvProvider::getCachedArtistBackground(const QString& artistMbid)
{
    QString path = cacheDir() + QStringLiteral("/") + artistMbid
                   + QStringLiteral("_bg.jpg");
    return QFile::exists(path) ? path : QString();
}

void FanartTvProvider::fetchArtistImages(const QString& artistMbid)
{
    qDebug() << "[Fanart.tv] fetchArtistImages mbid:" << artistMbid;

    if (artistMbid.isEmpty()) {
        qDebug() << "[Fanart.tv] Empty artist MBID, skipping";
        emit artistImagesNotFound(artistMbid);
        return;
    }

    QString cachedThumb = getCachedArtistThumb(artistMbid);
    QString cachedBg    = getCachedArtistBackground(artistMbid);

    if (!cachedThumb.isEmpty() && !cachedBg.isEmpty()) {
        qDebug() << "[Fanart.tv] Using cached images for" << artistMbid;
        ArtistImages images;
        images.artistThumb      = cachedThumb;
        images.artistBackground = cachedBg;
        emit artistImagesFetched(artistMbid, images);
        return;
    }

    QUrl url(API_BASE + QStringLiteral("/") + artistMbid);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("api_key"), API_KEY);
    url.setQuery(query);

    qDebug() << "[Fanart.tv] Fetching:" << url.toString();

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
        QStringLiteral("SoranaFlow/%1").arg(QCoreApplication::applicationVersion()).toUtf8());
    request.setTransferTimeout(15000);

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, artistMbid, cachedThumb, cachedBg, reply]() {
        reply->deleteLater();

        int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        qDebug() << "[Fanart.tv] HTTP" << status
                 << "error:" << reply->error()
                 << reply->errorString();

        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            qDebug() << "[Fanart.tv] 404 — artist not found:" << artistMbid;
            emit artistImagesNotFound(artistMbid);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[Fanart.tv] Network error for" << artistMbid
                       << ":" << reply->errorString();
            emit fetchError(artistMbid, reply->errorString());
            return;
        }

        QByteArray body = reply->readAll();
        qDebug() << "[Fanart.tv] Response size:" << body.size();

        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonObject obj   = doc.object();

        ArtistImages images;

        // Artist thumbnails
        QJsonArray thumbs = obj[QStringLiteral("artistthumb")].toArray();
        for (const auto& t : thumbs)
            images.allThumbs.append(
                t.toObject()[QStringLiteral("url")].toString());
        if (!images.allThumbs.isEmpty())
            images.artistThumb = images.allThumbs.first();

        // Artist backgrounds
        QJsonArray bgs =
            obj[QStringLiteral("artistbackground")].toArray();
        for (const auto& b : bgs)
            images.allBackgrounds.append(
                b.toObject()[QStringLiteral("url")].toString());
        if (!images.allBackgrounds.isEmpty())
            images.artistBackground = images.allBackgrounds.first();

        // HD logo
        QJsonArray logos =
            obj[QStringLiteral("hdmusiclogo")].toArray();
        if (!logos.isEmpty())
            images.hdMusicLogo =
                logos[0].toObject()[QStringLiteral("url")].toString();

        qDebug() << "[Fanart.tv] Found:"
                 << thumbs.size() << "thumbs,"
                 << bgs.size() << "backgrounds,"
                 << logos.size() << "logos"
                 << "for" << artistMbid;

        emit artistImagesFetched(artistMbid, images);

        // Download thumb if not cached
        if (!images.artistThumb.isEmpty() && cachedThumb.isEmpty()) {
            qDebug() << "[Fanart.tv] Downloading thumb:"
                     << images.artistThumb;
            downloadImage(images.artistThumb, artistMbid,
                          QStringLiteral("thumb"));
        }

        // Download background if not cached
        if (!images.artistBackground.isEmpty() && cachedBg.isEmpty()) {
            qDebug() << "[Fanart.tv] Downloading bg:"
                     << images.artistBackground;
            downloadImage(images.artistBackground, artistMbid,
                          QStringLiteral("bg"));
        }
    });
}

void FanartTvProvider::downloadImage(const QString& url,
                                      const QString& artistMbid,
                                      const QString& type)
{
    QUrl reqUrl(url);
    QNetworkRequest request(reqUrl);
    request.setRawHeader("User-Agent",
        QStringLiteral("SoranaFlow/%1").arg(QCoreApplication::applicationVersion()).toUtf8());
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        reply->deleteLater();

        int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[Fanart.tv] Image download failed HTTP"
                       << status << reply->errorString()
                       << "for" << artistMbid << type;
            return;
        }

        QByteArray imageData = reply->readAll();
        qDebug() << "[Fanart.tv] Image downloaded:" << imageData.size()
                 << "bytes for" << artistMbid << type;

        QPixmap pixmap;
        if (!pixmap.loadFromData(imageData)) {
            qWarning() << "[Fanart.tv] Failed to decode image"
                       << artistMbid << type;
            return;
        }

        QString filename = artistMbid + QStringLiteral("_") + type
                           + QStringLiteral(".jpg");
        QString savePath = cacheDir() + QStringLiteral("/") + filename;
        bool saved = pixmap.save(savePath, "JPEG", 90);
        qDebug() << "[Fanart.tv] Saved:" << saved << "to" << savePath;

        if (type == QStringLiteral("thumb"))
            emit artistThumbDownloaded(artistMbid, pixmap, savePath);
        else if (type == QStringLiteral("bg"))
            emit artistBackgroundDownloaded(artistMbid, pixmap, savePath);
    });
}
