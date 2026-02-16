#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "../MusicData.h"

class PlaylistManager : public QObject {
    Q_OBJECT

public:
    static PlaylistManager* instance();

    // ── CRUD ─────────────────────────────────────────────────────────
    QString createPlaylist(const QString& name, const QString& description = QString());
    bool renamePlaylist(const QString& id, const QString& newName);
    bool deletePlaylist(const QString& id);

    // ── Import ────────────────────────────────────────────────────────
    // Import a .m3u/.m3u8 playlist file. Returns playlist ID on success.
    QString importM3U(const QString& filePath);

    // ── Track management ─────────────────────────────────────────────
    bool addTrack(const QString& playlistId, const Track& track);
    bool removeTrack(const QString& playlistId, const QString& trackId);
    bool reorderTrack(const QString& playlistId, int fromPos, int toPos);

    // ── Queries ──────────────────────────────────────────────────────
    QVector<Playlist> allPlaylists() const;
    Playlist playlistById(const QString& id) const;

    // ── Smart Playlists ──────────────────────────────────────────────
    Playlist recentlyPlayedPlaylist() const;
    Playlist mostPlayedPlaylist() const;
    Playlist recentlyAddedPlaylist() const;

signals:
    void playlistCreated(const QString& id);
    void playlistDeleted(const QString& id);
    void playlistUpdated(const QString& id);
    void playlistsChanged();

private:
    explicit PlaylistManager(QObject* parent = nullptr);
};
