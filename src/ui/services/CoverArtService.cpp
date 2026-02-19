#include "CoverArtService.h"
#include "../../core/MusicData.h"
#include "../../core/audio/MetadataReader.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QDateTime>
#include <QtConcurrent>
#include <QFutureWatcher>

CoverArtService* CoverArtService::instance()
{
    static CoverArtService s;
    return &s;
}

CoverArtService::CoverArtService(QObject* parent)
    : QObject(parent)
    , m_cache(50)
{
    QDir().mkpath(diskCacheDir());
}

// ── Disk cache helpers ──────────────────────────────────────────────

QString CoverArtService::diskCacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/cover_art_thumbs");
}

QString CoverArtService::diskCachePath(const QString& cacheKey)
{
    QByteArray hash = QCryptographicHash::hash(
        cacheKey.toUtf8(), QCryptographicHash::Md5);
    return diskCacheDir() + QStringLiteral("/") + hash.toHex() + QStringLiteral(".jpg");
}

QPixmap CoverArtService::loadFromDisk(const QString& cacheKey)
{
    QString path = diskCachePath(cacheKey);
    QPixmap pm;
    if (QFile::exists(path))
        pm.load(path);
    return pm;
}

void CoverArtService::saveToDisk(const QString& cacheKey, const QPixmap& pixmap)
{
    if (pixmap.isNull()) return;
    pixmap.save(diskCachePath(cacheKey), "JPEG", 85);
}

void CoverArtService::evictDiskCache(int maxAgeDays)
{
    QDir dir(diskCacheDir());
    if (!dir.exists()) return;
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxAgeDays);
    for (const auto& info : dir.entryInfoList(QDir::Files)) {
        if (info.lastModified() < cutoff)
            QFile::remove(info.filePath());
    }
}

// ── Cover art retrieval (3-layer: memory → disk → discovery) ────────

QPixmap CoverArtService::getCoverArt(const Track& track, int size)
{
    QString cacheKey = QStringLiteral("%1@%2").arg(track.filePath).arg(size);

    // Layer 1: Memory cache
    {
        QMutexLocker locker(&m_mutex);
        if (QPixmap* cached = m_cache.object(cacheKey))
            return *cached;
    }

    // Layer 2: Disk cache
    QPixmap diskPix = loadFromDisk(cacheKey);
    if (!diskPix.isNull()) {
        QMutexLocker locker(&m_mutex);
        m_cache.insert(cacheKey, new QPixmap(diskPix));
        return diskPix;
    }

    // Layer 3: Discovery (file/folder/embedded)
    QImage img = discoverCoverArtImage(track.coverUrl, track.filePath);
    QPixmap pix;
    if (!img.isNull()) {
        pix = QPixmap::fromImage(img);
        if (size > 0)
            pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }

    {
        QMutexLocker locker(&m_mutex);
        m_cache.insert(cacheKey, new QPixmap(pix));
    }

    // Save to disk cache for next startup
    saveToDisk(cacheKey, pix);

    return pix;
}

void CoverArtService::getCoverArtAsync(const Track& track, int size,
                                        std::function<void(const QPixmap&)> callback)
{
    QString cacheKey = QStringLiteral("%1@%2").arg(track.filePath).arg(size);

    // Layer 1: Memory cache
    {
        QMutexLocker locker(&m_mutex);
        if (QPixmap* cached = m_cache.object(cacheKey)) {
            QPixmap result = *cached;
            locker.unlock();
            if (callback) callback(result);
            return;
        }
    }

    // Layer 2: Disk cache (fast check on main thread)
    QPixmap diskPix = loadFromDisk(cacheKey);
    if (!diskPix.isNull()) {
        {
            QMutexLocker locker(&m_mutex);
            m_cache.insert(cacheKey, new QPixmap(diskPix));
        }
        if (callback) callback(diskPix);
        return;
    }

    // Layer 3: Discovery (async on thread pool)
    QString coverUrl = track.coverUrl;
    QString filePath = track.filePath;

    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, cacheKey, size, callback]() {
        QImage img = watcher->result();
        watcher->deleteLater();

        QPixmap pix;
        if (!img.isNull()) {
            pix = QPixmap::fromImage(img);
            if (size > 0)
                pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        }

        {
            QMutexLocker locker(&m_mutex);
            m_cache.insert(cacheKey, new QPixmap(pix));
        }

        saveToDisk(cacheKey, pix);

        if (callback) callback(pix);
    });

    watcher->setFuture(QtConcurrent::run([coverUrl, filePath]() -> QImage {
        return discoverCoverArtImage(coverUrl, filePath);
    }));
}

// ── 4-tier cover art discovery (thread-safe, uses QImage) ────────────

QImage CoverArtService::discoverCoverArtImage(const QString& coverUrl, const QString& filePath)
{
    QImage img;

    // Tier 1: coverUrl — supports local files and Qt resource paths (qrc:)
    if (!coverUrl.isEmpty()) {
        QString loadPath = coverUrl;
        if (loadPath.startsWith(QStringLiteral("qrc:")))
            loadPath = loadPath.mid(3);
        if (QFile::exists(loadPath)) {
            img.load(loadPath);
            if (!img.isNull()) return img;
        }
        if (loadPath.startsWith(QStringLiteral(":/"))) {
            img.load(loadPath);
            if (!img.isNull()) return img;
        }
    }

    // Tier 2: well-known folder filenames
    if (!filePath.isEmpty()) {
        img = scanFolderForArt(QFileInfo(filePath).absolutePath());
        if (!img.isNull()) return img;
    }

    // Tier 3: embedded cover via FFmpeg
    if (!filePath.isEmpty()) {
        img = extractEmbeddedArt(filePath);
        if (!img.isNull()) return img;
    }

    // Tier 4: any image file in folder
    if (!filePath.isEmpty()) {
        QString folder = QFileInfo(filePath).absolutePath();
        QDir dir(folder);
        QStringList imageFilters = {
            QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
            QStringLiteral("*.png"), QStringLiteral("*.webp"), QStringLiteral("*.bmp")
        };
        QStringList images = dir.entryList(imageFilters, QDir::Files, QDir::Name);
        for (const QString& imgFile : images) {
            img.load(dir.filePath(imgFile));
            if (!img.isNull()) return img;
        }
    }

    return img;
}

QImage CoverArtService::scanFolderForArt(const QString& dirPath)
{
    static const QStringList names = {
        QStringLiteral("cover.jpg"),   QStringLiteral("cover.png"),
        QStringLiteral("Cover.jpg"),   QStringLiteral("Cover.png"),
        QStringLiteral("folder.jpg"),  QStringLiteral("folder.png"),
        QStringLiteral("Folder.jpg"),  QStringLiteral("Folder.png"),
        QStringLiteral("front.jpg"),   QStringLiteral("front.png"),
        QStringLiteral("Front.jpg"),   QStringLiteral("Front.png"),
        QStringLiteral("album.jpg"),   QStringLiteral("album.png"),
        QStringLiteral("Album.jpg"),   QStringLiteral("Album.png"),
        QStringLiteral("artwork.jpg"), QStringLiteral("artwork.png"),
        QStringLiteral("Artwork.jpg"), QStringLiteral("Artwork.png"),
    };

    QImage img;
    for (const QString& n : names) {
        QString path = dirPath + QStringLiteral("/") + n;
        if (QFile::exists(path)) {
            img.load(path);
            if (!img.isNull()) return img;
        }
    }
    return QImage();
}

QImage CoverArtService::extractEmbeddedArt(const QString& filePath)
{
    QPixmap pix = MetadataReader::extractCoverArt(filePath);
    if (!pix.isNull()) return pix.toImage();
    return QImage();
}
