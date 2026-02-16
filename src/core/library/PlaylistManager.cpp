#include "PlaylistManager.h"
#include "LibraryDatabase.h"

#include <QUuid>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>
#include <QDebug>

// ── Singleton ───────────────────────────────────────────────────────
PlaylistManager* PlaylistManager::instance()
{
    static PlaylistManager s;
    return &s;
}

PlaylistManager::PlaylistManager(QObject* parent)
    : QObject(parent)
{
}

// ── createPlaylist ──────────────────────────────────────────────────
QString PlaylistManager::createPlaylist(const QString& name, const QString& description)
{
    Playlist p;
    p.id              = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name            = name;
    p.description     = description;
    p.isSmartPlaylist = false;
    p.createdAt       = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (LibraryDatabase::instance()->insertPlaylist(p)) {
        emit playlistCreated(p.id);
        emit playlistsChanged();
        return p.id;
    }
    return QString();
}

// ── renamePlaylist ──────────────────────────────────────────────────
bool PlaylistManager::renamePlaylist(const QString& id, const QString& newName)
{
    auto* db = LibraryDatabase::instance();
    Playlist p = db->playlistById(id);
    if (p.id.isEmpty()) return false;

    p.name = newName;
    if (db->updatePlaylist(p)) {
        emit playlistUpdated(id);
        emit playlistsChanged();
        return true;
    }
    return false;
}

// ── deletePlaylist ──────────────────────────────────────────────────
bool PlaylistManager::deletePlaylist(const QString& id)
{
    if (LibraryDatabase::instance()->removePlaylist(id)) {
        emit playlistDeleted(id);
        emit playlistsChanged();
        return true;
    }
    return false;
}

// ── addTrack ────────────────────────────────────────────────────────
bool PlaylistManager::addTrack(const QString& playlistId, const Track& track)
{
    if (LibraryDatabase::instance()->addTrackToPlaylist(playlistId, track.id)) {
        emit playlistUpdated(playlistId);
        emit playlistsChanged();
        return true;
    }
    return false;
}

// ── removeTrack ─────────────────────────────────────────────────────
bool PlaylistManager::removeTrack(const QString& playlistId, const QString& trackId)
{
    if (LibraryDatabase::instance()->removeTrackFromPlaylist(playlistId, trackId)) {
        emit playlistUpdated(playlistId);
        emit playlistsChanged();
        return true;
    }
    return false;
}

// ── reorderTrack ────────────────────────────────────────────────────
bool PlaylistManager::reorderTrack(const QString& playlistId, int fromPos, int toPos)
{
    if (LibraryDatabase::instance()->reorderPlaylistTrack(playlistId, fromPos, toPos)) {
        emit playlistUpdated(playlistId);
        return true;
    }
    return false;
}

// ── importM3U ────────────────────────────────────────────────────────
QString PlaylistManager::importM3U(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[PlaylistManager] Failed to open m3u file:" << filePath;
        return QString();
    }

    // Derive playlist name from filename (without extension)
    QString playlistName = QFileInfo(filePath).completeBaseName();
    if (playlistName.isEmpty())
        playlistName = QStringLiteral("Imported Playlist");

    QDir baseDir = QFileInfo(filePath).absoluteDir();
    QTextStream in(&file);

    QStringList trackPaths;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        // Resolve relative paths against the m3u file's directory
        QString resolved = line;
        if (!QFileInfo(line).isAbsolute())
            resolved = baseDir.absoluteFilePath(line);

        // Normalize path
        resolved = QFileInfo(resolved).canonicalFilePath();
        if (resolved.isEmpty())
            resolved = QDir::cleanPath(baseDir.absoluteFilePath(line));

        trackPaths.append(resolved);
    }

    if (trackPaths.isEmpty()) {
        qWarning() << "[PlaylistManager] No tracks found in m3u:" << filePath;
        return QString();
    }

    // Create the playlist
    QString playlistId = createPlaylist(playlistName);
    if (playlistId.isEmpty())
        return QString();

    auto* db = LibraryDatabase::instance();
    int matched = 0;
    int skipped = 0;

    for (const QString& path : trackPaths) {
        auto track = db->trackByPath(path);
        if (track.has_value()) {
            db->addTrackToPlaylist(playlistId, track->id);
            matched++;
        } else {
            skipped++;
            qDebug() << "[PlaylistManager] m3u track not in library:" << path;
        }
    }

    qDebug() << "[PlaylistManager] Imported" << playlistName
             << "— matched:" << matched << "skipped:" << skipped;

    emit playlistUpdated(playlistId);
    emit playlistsChanged();
    return playlistId;
}

// ── Queries ─────────────────────────────────────────────────────────
QVector<Playlist> PlaylistManager::allPlaylists() const
{
    return LibraryDatabase::instance()->allPlaylists();
}

Playlist PlaylistManager::playlistById(const QString& id) const
{
    return LibraryDatabase::instance()->playlistById(id);
}

// ── Smart Playlists ─────────────────────────────────────────────────
Playlist PlaylistManager::recentlyPlayedPlaylist() const
{
    Playlist p;
    p.id              = QStringLiteral("smart_recently_played");
    p.name            = QStringLiteral("Recently Played");
    p.description     = QStringLiteral("Tracks you've listened to recently");
    p.isSmartPlaylist = true;
    p.tracks          = LibraryDatabase::instance()->recentlyPlayed(50);
    return p;
}

Playlist PlaylistManager::mostPlayedPlaylist() const
{
    Playlist p;
    p.id              = QStringLiteral("smart_most_played");
    p.name            = QStringLiteral("Most Played");
    p.description     = QStringLiteral("Your most played tracks");
    p.isSmartPlaylist = true;
    p.tracks          = LibraryDatabase::instance()->mostPlayed(50);
    return p;
}

Playlist PlaylistManager::recentlyAddedPlaylist() const
{
    Playlist p;
    p.id              = QStringLiteral("smart_recently_added");
    p.name            = QStringLiteral("Recently Added");
    p.description     = QStringLiteral("Tracks recently added to your library");
    p.isSmartPlaylist = true;
    p.tracks          = LibraryDatabase::instance()->recentlyAdded(50);
    return p;
}
