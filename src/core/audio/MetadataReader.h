#pragma once

#include <QString>
#include <QPixmap>
#include <optional>
#include "../MusicData.h"

class MetadataReader {
public:
    static std::optional<Track> readTrack(const QString& filePath);
    static QPixmap extractCoverArt(const QString& filePath);
};
