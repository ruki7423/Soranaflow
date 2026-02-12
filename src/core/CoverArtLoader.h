#pragma once

#include <QObject>
#include <QPixmap>
#include <QCache>
#include <QString>

class CoverArtLoader : public QObject {
    Q_OBJECT

public:
    static CoverArtLoader* instance();

    void requestCoverArt(const QString& trackPath, const QString& coverUrl, int size);

signals:
    void coverArtReady(const QString& trackPath, const QPixmap& pixmap);

private:
    explicit CoverArtLoader(QObject* parent = nullptr);

    QCache<QString, QPixmap> m_cache;
};
