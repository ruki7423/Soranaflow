#include "CoverArtProvider.h"

#include <QNetworkReply>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QDebug>

const QString CoverArtProvider::API_BASE =
    QStringLiteral("https://coverartarchive.org");

CoverArtProvider* CoverArtProvider::instance()
{
    static CoverArtProvider s_instance;
    return &s_instance;
}

CoverArtProvider::CoverArtProvider(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    QString dir = cacheDir();
    bool ok = QDir().mkpath(dir);
    qDebug() << "[CoverArt] Cache dir:" << dir << "exists:" << ok;
}

QString CoverArtProvider::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/album_art");
}

QString CoverArtProvider::getCachedArtPath(const QString& mbid)
{
    QString path = cacheDir() + QStringLiteral("/") + mbid
                   + QStringLiteral(".jpg");
    return QFile::exists(path) ? path : QString();
}

void CoverArtProvider::fetchAlbumArt(const QString& mbid,
                                      bool isReleaseGroup)
{
    qDebug() << "[CoverArt] fetchAlbumArt mbid:" << mbid
             << "isReleaseGroup:" << isReleaseGroup;

    if (mbid.isEmpty()) {
        qDebug() << "[CoverArt] Empty MBID, skipping";
        emit albumArtNotFound(mbid);
        return;
    }

    // Check cache
    QString cached = getCachedArtPath(mbid);
    if (!cached.isEmpty()) {
        qDebug() << "[CoverArt] Using cached:" << cached;
        QPixmap pixmap(cached);
        emit albumArtFetched(mbid, pixmap, cached);
        return;
    }

    // Cover Art Archive: /release-group/{id}/front or /release/{id}/front
    // The -500 suffix requests a 500px thumbnail
    QString endpoint = isReleaseGroup
        ? QStringLiteral("/release-group/")
        : QStringLiteral("/release/");
    QString url = API_BASE + endpoint + mbid + QStringLiteral("/front-500");

    qDebug() << "[CoverArt] Fetching:" << url;
    downloadImage(url, mbid);
}

void CoverArtProvider::downloadImage(const QString& url,
                                      const QString& mbid)
{
    QUrl reqUrl(url);
    QNetworkRequest request(reqUrl);
    request.setRawHeader("User-Agent",
                         "SoranaFlow/1.2.0 (https://github.com/soranaflow)");
    request.setTransferTimeout(15000);
    // Cover Art Archive redirects to archive.org — must follow redirects
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        reply->deleteLater();

        int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        qDebug() << "[CoverArt] HTTP" << status
                 << "error:" << reply->error()
                 << reply->errorString();

        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            qDebug() << "[CoverArt] 404 — no art for" << mbid;
            emit albumArtNotFound(mbid);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[CoverArt] Network error for" << mbid
                       << ":" << reply->errorString();
            emit fetchError(mbid, reply->errorString());
            return;
        }

        QByteArray imageData = reply->readAll();
        qDebug() << "[CoverArt] Received" << imageData.size() << "bytes";

        QPixmap pixmap;
        if (!pixmap.loadFromData(imageData)) {
            qWarning() << "[CoverArt] Failed to decode image for" << mbid;
            emit fetchError(mbid, QStringLiteral("Failed to decode image"));
            return;
        }

        QString savePath = cacheDir() + QStringLiteral("/") + mbid
                           + QStringLiteral(".jpg");
        bool saved = pixmap.save(savePath, "JPEG", 90);
        qDebug() << "[CoverArt] Saved:" << saved << "to" << savePath
                 << "size:" << pixmap.size();

        emit albumArtFetched(mbid, pixmap, savePath);
    });
}
