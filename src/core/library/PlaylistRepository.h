#pragma once
#include <QVector>
#include <QString>
#include "../MusicData.h"

struct DatabaseContext;

class PlaylistRepository {
public:
    explicit PlaylistRepository(DatabaseContext* ctx);

    bool insertPlaylist(const Playlist& playlist);
    bool updatePlaylist(const Playlist& playlist);
    bool removePlaylist(const QString& id);
    QVector<Playlist> allPlaylists() const;
    Playlist playlistById(const QString& id) const;
    bool addTrackToPlaylist(const QString& playlistId, const QString& trackId, int position = -1);
    bool removeTrackFromPlaylist(const QString& playlistId, const QString& trackId);
    bool reorderPlaylistTrack(const QString& playlistId, int fromPos, int toPos);

private:
    DatabaseContext* m_ctx;
};
