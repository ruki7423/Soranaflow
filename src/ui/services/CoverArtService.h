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

private:
    explicit CoverArtService(QObject* parent = nullptr);

    static QImage discoverCoverArtImage(const QString& coverUrl, const QString& filePath);
    static QImage scanFolderForArt(const QString& dirPath);
    static QImage extractEmbeddedArt(const QString& filePath);

    QCache<QString, QPixmap> m_cache;
    QMutex m_mutex;
};
