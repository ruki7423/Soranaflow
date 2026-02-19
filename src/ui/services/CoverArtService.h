#pragma once

#include <QObject>
#include <QPixmap>
#include <QImage>
#include <QCache>
#include <QMutex>
#include <functional>

struct Track;

class CoverArtService : public QObject {
    Q_OBJECT
public:
    static CoverArtService* instance();
    QPixmap getCoverArt(const Track& track, int size);
    void getCoverArtAsync(const Track& track, int size,
                          std::function<void(const QPixmap&)> callback);

    // Evict disk cache entries older than given days
    void evictDiskCache(int maxAgeDays = 30);

private:
    explicit CoverArtService(QObject* parent = nullptr);

    static QImage discoverCoverArtImage(const QString& coverUrl, const QString& filePath);
    static QImage scanFolderForArt(const QString& dirPath);
    static QImage extractEmbeddedArt(const QString& filePath);

    // Disk cache helpers
    static QString diskCacheDir();
    static QString diskCachePath(const QString& cacheKey);
    static QPixmap loadFromDisk(const QString& cacheKey);
    static void saveToDisk(const QString& cacheKey, const QPixmap& pixmap);

    QCache<QString, QPixmap> m_cache;
    QMutex m_mutex;
};
