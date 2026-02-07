#pragma once

#include <QString>
#include <QImage>

struct TrackMetadata {
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    int trackNumber = 0;
    int discNumber = 0;
    int year = 0;
    QString genre;
    QString composer;
    QString comment;
    QImage albumArt;
};

class TagWriter {
public:
    static bool readTags(const QString& filePath, TrackMetadata& meta);
    static bool writeTags(const QString& filePath, const TrackMetadata& meta);
    static bool writeAlbumArt(const QString& filePath, const QImage& image);
    static QImage readAlbumArt(const QString& filePath);
};
