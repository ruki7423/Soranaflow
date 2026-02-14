#pragma once

#include <QVector>
#include <QString>
#include "../MusicData.h"

struct DatabaseContext;

class ArtistRepository {
public:
    explicit ArtistRepository(DatabaseContext* ctx);

    bool insertArtist(const Artist& artist);
    bool updateArtist(const Artist& artist);
    QVector<Artist> allArtists() const;
    Artist artistById(const QString& id) const;
    QVector<Artist> searchArtists(const QString& query) const;
    QString artistMbidForArtist(const QString& artistId) const;

private:
    DatabaseContext* m_ctx;
};
