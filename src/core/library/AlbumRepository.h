#pragma once

#include <QVector>
#include <QString>
#include "../MusicData.h"

struct DatabaseContext;

class AlbumRepository {
public:
    explicit AlbumRepository(DatabaseContext* ctx);

    bool insertAlbum(const Album& album);
    bool updateAlbum(const Album& album);
    QVector<Album> allAlbums() const;
    Album albumById(const QString& id) const;
    QVector<Album> searchAlbums(const QString& query) const;
    QString releaseGroupMbidForAlbum(const QString& albumId) const;
    QString firstTrackPathForAlbum(const QString& albumId) const;

private:
    DatabaseContext* m_ctx;
};
