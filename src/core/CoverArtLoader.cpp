#include "CoverArtLoader.h"
#include "audio/MetadataReader.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDebug>

// ── Singleton ───────────────────────────────────────────────────────
CoverArtLoader* CoverArtLoader::instance()
{
    static CoverArtLoader s;
    return &s;
}

CoverArtLoader::CoverArtLoader(QObject* parent)
    : QObject(parent)
    , m_cache(50) // LRU cache: up to 50 cover art entries
{
}

// ── requestCoverArt ─────────────────────────────────────────────────
void CoverArtLoader::requestCoverArt(const QString& trackPath, const QString& coverUrl, int size)
{
    // Build a cache key from the album folder (all tracks in same folder share art)
    QString cacheKey = QFileInfo(trackPath).absolutePath() + QString::number(size);

    // Check cache first
    if (QPixmap* cached = m_cache.object(cacheKey)) {
        emit coverArtReady(trackPath, *cached);
        return;
    }

    // Run disk I/O on a worker thread
    QThread* worker = QThread::create([this, trackPath, coverUrl, size, cacheKey]() {
        QPixmap pix;

        // 1. Try coverUrl
        if (!coverUrl.isEmpty()) {
            QString loadPath = coverUrl;
            if (loadPath.startsWith(QStringLiteral("qrc:")))
                loadPath = loadPath.mid(3);
            if (QFile::exists(loadPath)) {
                pix.load(loadPath);
                if (!pix.isNull()) {
                    pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                }
            }
        }

        // 2. Try well-known cover art filenames
        if (pix.isNull() && !trackPath.isEmpty()) {
            QString folder = QFileInfo(trackPath).absolutePath();
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
            for (const QString& n : names) {
                QString path = folder + QStringLiteral("/") + n;
                if (QFile::exists(path)) {
                    pix.load(path);
                    if (!pix.isNull()) {
                        pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                        break;
                    }
                }
            }
        }

        // 3. Try embedded cover art
        if (pix.isNull() && !trackPath.isEmpty()) {
            pix = MetadataReader::extractCoverArt(trackPath);
            if (!pix.isNull()) {
                pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            }
        }

        // 4. Scan directory for any image
        if (pix.isNull() && !trackPath.isEmpty()) {
            QDir dir(QFileInfo(trackPath).absolutePath());
            QStringList images = dir.entryList(
                {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"), QStringLiteral("*.webp")},
                QDir::Files, QDir::Name);
            if (!images.isEmpty()) {
                pix.load(dir.filePath(images.first()));
                if (!pix.isNull()) {
                    pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                }
            }
        }

        // Deliver result on the main thread
        QMetaObject::invokeMethod(this, [this, trackPath, pix, cacheKey]() {
            if (!pix.isNull()) {
                m_cache.insert(cacheKey, new QPixmap(pix));
            }
            emit coverArtReady(trackPath, pix);
        }, Qt::QueuedConnection);
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
