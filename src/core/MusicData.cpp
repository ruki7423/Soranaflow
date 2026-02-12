#include "MusicData.h"
#include "library/LibraryDatabase.h"
#include "library/LibraryScanner.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>
#include <QtConcurrent>

// ── Color Constants ─────────────────────────────────────────────────
static const QColor HIRES_COLOR("#D4AF37");
static const QColor DSD_COLOR("#9B59B6");
static const QColor LOSSLESS_COLOR("#2ECC71");
static const QColor LOSSY_COLOR("#95A5A6");

// ═════════════════════════════════════════════════════════════════════
//  Utility Functions
// ═════════════════════════════════════════════════════════════════════

QString formatDuration(int seconds)
{
    int m = seconds / 60;
    int s = seconds % 60;
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QColor getFormatColor(AudioFormat format)
{
    switch (format) {
    case AudioFormat::FLAC:    return HIRES_COLOR;
    case AudioFormat::DSD64:   return DSD_COLOR;
    case AudioFormat::DSD128:  return DSD_COLOR;
    case AudioFormat::DSD256:  return DSD_COLOR;
    case AudioFormat::DSD512:  return DSD_COLOR;
    case AudioFormat::DSD1024: return DSD_COLOR;
    case AudioFormat::DSD2048: return DSD_COLOR;
    case AudioFormat::ALAC:    return LOSSLESS_COLOR;
    case AudioFormat::WAV:     return HIRES_COLOR;
    case AudioFormat::MP3:     return LOSSY_COLOR;
    case AudioFormat::AAC:     return LOSSY_COLOR;
    }
    return LOSSY_COLOR;
}

QString getFormatLabel(AudioFormat format)
{
    switch (format) {
    case AudioFormat::FLAC:    return QStringLiteral("FLAC");
    case AudioFormat::DSD64:   return QStringLiteral("DSD64");
    case AudioFormat::DSD128:  return QStringLiteral("DSD128");
    case AudioFormat::DSD256:  return QStringLiteral("DSD256");
    case AudioFormat::DSD512:  return QStringLiteral("DSD512");
    case AudioFormat::DSD1024: return QStringLiteral("DSD1024");
    case AudioFormat::DSD2048: return QStringLiteral("DSD2048");
    case AudioFormat::ALAC:    return QStringLiteral("ALAC");
    case AudioFormat::WAV:     return QStringLiteral("WAV");
    case AudioFormat::MP3:     return QStringLiteral("MP3");
    case AudioFormat::AAC:     return QStringLiteral("AAC");
    }
    return QStringLiteral("Unknown");
}

QString getFormatSpecs(AudioFormat format,
                       const QString& sampleRate,
                       const QString& bitDepth,
                       const QString& bitrate)
{
    QString label = getFormatLabel(format);

    switch (format) {
    case AudioFormat::DSD64:
    case AudioFormat::DSD128:
    case AudioFormat::DSD256:
    case AudioFormat::DSD512:
    case AudioFormat::DSD1024:
    case AudioFormat::DSD2048:
        return QString("%1 | %2").arg(label, bitrate);
    case AudioFormat::MP3:
    case AudioFormat::AAC:
        return QString("%1 | %2").arg(label, bitrate);
    default:
        break;
    }
    // FLAC, ALAC, WAV – show sample rate / bit depth / bitrate
    return QString("%1 | %2 / %3 / %4").arg(label, sampleRate, bitDepth, bitrate);
}

// ── Audio Quality Classification ────────────────────────────────────

AudioQuality classifyAudioQuality(AudioFormat format,
                                  const QString& sampleRate,
                                  const QString& bitDepth)
{
    // DSD detection
    switch (format) {
    case AudioFormat::DSD64:
    case AudioFormat::DSD128:
    case AudioFormat::DSD256:
    case AudioFormat::DSD512:
    case AudioFormat::DSD1024:
    case AudioFormat::DSD2048:
        return AudioQuality::DSD;
    case AudioFormat::MP3:
    case AudioFormat::AAC:
        return AudioQuality::Lossy;
    default:
        break;
    }

    // FLAC, ALAC, WAV — parse strings to detect Hi-Res vs Lossless
    int bits = 0;
    if (!bitDepth.isEmpty()) {
        // "24-bit" -> 24, "1-bit" -> 1
        int dashPos = bitDepth.indexOf(QLatin1Char('-'));
        bits = (dashPos > 0) ? bitDepth.left(dashPos).toInt()
                             : bitDepth.toInt();
    }

    double rateHz = 0;
    if (sampleRate.contains(QStringLiteral("MHz"))) {
        int spacePos = sampleRate.indexOf(QLatin1Char(' '));
        rateHz = sampleRate.left(spacePos > 0 ? spacePos : sampleRate.size()).toDouble() * 1000000.0;
    } else if (sampleRate.contains(QStringLiteral("kHz"))) {
        int spacePos = sampleRate.indexOf(QLatin1Char(' '));
        rateHz = sampleRate.left(spacePos > 0 ? spacePos : sampleRate.size()).toDouble() * 1000.0;
    } else if (!sampleRate.isEmpty()) {
        int spacePos = sampleRate.indexOf(QLatin1Char(' '));
        rateHz = sampleRate.left(spacePos > 0 ? spacePos : sampleRate.size()).toDouble();
    }

    if (bits > 16 || rateHz > 48000) {
        return AudioQuality::HiRes;
    }

    return AudioQuality::Lossless;
}

QString getQualityLabel(AudioQuality quality)
{
    switch (quality) {
    case AudioQuality::DSD:      return QStringLiteral("DSD");
    case AudioQuality::HiRes:    return QStringLiteral("Hi-Res");
    case AudioQuality::Lossless: return QStringLiteral("Lossless");
    case AudioQuality::Lossy:    return QStringLiteral("Lossy");
    default:                     return QString();
    }
}

QColor getQualityColor(AudioQuality quality)
{
    switch (quality) {
    case AudioQuality::DSD:      return DSD_COLOR;      // purple
    case AudioQuality::HiRes:    return HIRES_COLOR;    // gold
    case AudioQuality::Lossless: return LOSSLESS_COLOR;  // green
    case AudioQuality::Lossy:    return LOSSY_COLOR;     // gray
    default:                     return LOSSY_COLOR;
    }
}

// ═════════════════════════════════════════════════════════════════════
//  MusicDataProvider – Singleton
// ═════════════════════════════════════════════════════════════════════

MusicDataProvider* MusicDataProvider::instance()
{
    static MusicDataProvider s_instance;
    return &s_instance;
}

MusicDataProvider::MusicDataProvider(QObject* parent)
    : QObject(parent)
{
}

QVector<Track> MusicDataProvider::allTracks() const {
    QReadLocker l(&m_lock);
    if (m_tracks.isEmpty() && !m_useMockData) {
        // Lazy-load from DB on demand (not kept in memory after post-scan reload)
        l.unlock();
        auto* db = LibraryDatabase::instance();
        if (db) return db->allTracks();
        return {};
    }
    return m_tracks;
}
QVector<Album>    MusicDataProvider::allAlbums()    const { QReadLocker l(&m_lock); return m_albums; }
QVector<Artist>   MusicDataProvider::allArtists()   const { QReadLocker l(&m_lock); return m_artists; }
QVector<Playlist> MusicDataProvider::allPlaylists() const { QReadLocker l(&m_lock); return m_playlists; }

QVector<TrackIndex> MusicDataProvider::allTrackIndexes() const
{
    auto* db = LibraryDatabase::instance();
    if (!db) return {};
    return db->allTrackIndexes();
}

Album MusicDataProvider::albumById(const QString& id) const
{
    // Always load from DB — it populates tracks on demand without duplication
    if (!m_useMockData) {
        return LibraryDatabase::instance()->albumById(id);
    }

    // Mock data path
    {
        QReadLocker l(&m_lock);
        for (const auto& a : m_albums)
            if (a.id == id) return a;
    }
    return Album{};
}

Artist MusicDataProvider::artistById(const QString& id) const
{
    // Always load from DB — it populates albums on demand without duplication
    if (!m_useMockData) {
        return LibraryDatabase::instance()->artistById(id);
    }

    // Mock data path
    {
        QReadLocker l(&m_lock);
        for (const auto& a : m_artists)
            if (a.id == id) return a;
    }
    return Artist{};
}

Playlist MusicDataProvider::playlistById(const QString& id) const
{
    {
        QReadLocker l(&m_lock);
        for (const auto& p : m_playlists)
            if (p.id == id) return p;
    }

    // Fall back to database
    if (!m_useMockData) {
        return LibraryDatabase::instance()->playlistById(id);
    }
    return Playlist{};
}

bool MusicDataProvider::hasDatabaseTracks() const
{
    return !m_useMockData;
}

void MusicDataProvider::reloadFromDatabase()
{
    // Atomic re-entrancy guard (thread-safe, replaces static bool)
    if (m_reloading.exchange(true)) {
        m_pendingReload.store(true);
        qDebug() << "MusicDataProvider: Skipping - reload already in progress (pending)";
        return;
    }

    // First load is synchronous (startup needs data immediately)
    if (!m_firstLoadDone) {
        loadFromDatabase();
        m_firstLoadDone = true;
        m_reloading.store(false);
        emit libraryUpdated();
        return;
    }

    // Subsequent loads run DB queries off main thread
    // Skip allTracks() — LibraryView uses allTrackIndexes() directly.
    // allTracks() is lazy-loaded from DB on demand when needed.
    QtConcurrent::run([this]() {
        QElapsedTimer mdpTimer; mdpTimer.start();
        qDebug() << "[TIMING] MDP reloadFromDatabase (async) START";
        auto* db = LibraryDatabase::instance();
        if (!db) {
            m_reloading.store(false);
            return;
        }

        int trackCount = db->trackCount();

        // Always query albums/artists — keep cached data if DB returns 0
        // (during scan the tables may be temporarily empty after DELETE)
        QVector<Album>    dbAlbums    = db->allAlbums();
        QVector<Artist>   dbArtists   = db->allArtists();
        QVector<Playlist> dbPlaylists = db->allPlaylists();
        qDebug() << "[TIMING] MDP async DB queries:" << mdpTimer.elapsed() << "ms";

        qDebug() << "MusicDataProvider::reloadFromDatabase (async) — tracks:" << trackCount
                 << "albums:" << dbAlbums.size() << "artists:" << dbArtists.size();

        if (trackCount == 0) {
            QMetaObject::invokeMethod(this, [this]() {
                {
                    QWriteLocker l(&m_lock);
                    m_useMockData = true;
                }
                m_reloading.store(false);
                emit libraryUpdated();
                if (m_pendingReload.exchange(false)) {
                    reloadFromDatabase();
                }
            }, Qt::QueuedConnection);
            return;
        }

        // Link album metadata to artists (cheap — no track copies)
        for (auto& artist : dbArtists) {
            for (const auto& album : dbAlbums) {
                if (album.artistId == artist.id)
                    artist.albums.append(album);
            }
        }

        // Move results to main thread for swap + signal
        QMetaObject::invokeMethod(this, [this,
                albums  = std::move(dbAlbums),
                artists = std::move(dbArtists),
                playlists = std::move(dbPlaylists)]() mutable {
            {
                QWriteLocker l(&m_lock);
                m_useMockData = false;
                m_tracks.clear();  // Clear cached tracks — lazy-loaded on demand
                // Keep cached albums/artists if DB returned 0 (scan in progress)
                if (!albums.isEmpty()) {
                    m_albums = std::move(albums);
                } else if (!m_albums.isEmpty()) {
                    qDebug() << "[MDP] Keeping cached" << m_albums.size() << "albums (DB returned 0)";
                }
                if (!artists.isEmpty()) {
                    m_artists = std::move(artists);
                } else if (!m_artists.isEmpty()) {
                    qDebug() << "[MDP] Keeping cached" << m_artists.size() << "artists (DB returned 0)";
                }
                if (!playlists.isEmpty()) {
                    m_playlists = std::move(playlists);
                }
            }
            qDebug() << "MusicDataProvider: Reloaded"
                     << m_albums.size() << "albums," << m_artists.size() << "artists";
            m_reloading.store(false);
            qDebug() << "[TIMING] MDP reloadFromDatabase DONE — emitting libraryUpdated";
            emit libraryUpdated();
            if (m_pendingReload.exchange(false)) {
                reloadFromDatabase();
            }
        }, Qt::QueuedConnection);
    });
}

void MusicDataProvider::loadFromDatabase()
{
    QElapsedTimer t; t.start();
    qDebug() << "[TIMING] MDP loadFromDatabase (sync) START";
    auto* db = LibraryDatabase::instance();
    if (!db) {
        qDebug() << "MusicDataProvider: LibraryDatabase is null!";
        return;
    }

    // Load all data outside the lock (DB queries can be slow)
    QVector<Track> dbTracks = db->allTracks();
    qDebug() << "MusicDataProvider::loadFromDatabase - tracks in DB:" << dbTracks.size();

    if (dbTracks.isEmpty()) {
        m_useMockData = true;
        qDebug() << "MusicDataProvider: No database tracks, using mock data";
        return;
    }

    QVector<Album> dbAlbums = db->allAlbums();
    qDebug() << "MusicDataProvider::loadFromDatabase - albums in DB:" << dbAlbums.size();

    QVector<Artist> dbArtists = db->allArtists();
    qDebug() << "MusicDataProvider::loadFromDatabase - artists in DB:" << dbArtists.size();

    QVector<Playlist> dbPlaylists = db->allPlaylists();
    qDebug() << "MusicDataProvider::loadFromDatabase - playlists in DB:" << dbPlaylists.size();

    // NOTE: We do NOT copy tracks into albums here.
    // albumById() loads tracks on demand from the DB.
    // This avoids O(Tracks × Albums) duplication that caused 30GB+ RAM.

    // Link album metadata (without tracks) to their artists — cheap since
    // albums have empty tracks vectors (~200 bytes each, not thousands of Track copies)
    for (auto& artist : dbArtists) {
        for (const auto& album : dbAlbums) {
            if (album.artistId == artist.id) {
                artist.albums.append(album);
            }
        }
    }

    // Swap under write lock — blocks readers briefly
    {
        QWriteLocker l(&m_lock);
        m_useMockData = false;
        m_tracks    = std::move(dbTracks);
        m_albums    = std::move(dbAlbums);
        m_artists   = std::move(dbArtists);
        m_playlists = std::move(dbPlaylists);
    }

    qDebug() << "MusicDataProvider: Loaded" << m_tracks.size() << "tracks,"
             << m_albums.size() << "albums," << m_artists.size() << "artists";

    // Pre-warm lightweight track index (activates string pooling + mmap cache)
    db->allTrackIndexes();
    qDebug() << "[TIMING] MDP loadFromDatabase (sync) TOTAL:" << t.elapsed() << "ms";
}

// ═════════════════════════════════════════════════════════════════════
//  Mock Data Builder
// ═════════════════════════════════════════════════════════════════════

void MusicDataProvider::buildMockData()
{
    // ── Helper lambdas ──────────────────────────────────────────────
    auto makeTrack = [](const QString& id,
                        const QString& title,
                        const QString& artist,
                        const QString& album,
                        const QString& albumId,
                        const QString& artistId,
                        int duration,
                        AudioFormat format,
                        const QString& sampleRate,
                        const QString& bitDepth,
                        const QString& bitrate,
                        const QString& coverUrl,
                        int trackNumber,
                        int discNumber) -> Track
    {
        return Track{id, title, artist, album, albumId, artistId,
                     duration, format, sampleRate, bitDepth, bitrate,
                     coverUrl, trackNumber, discNumber};
    };

    // ────────────────────────────────────────────────────────────────
    //  ARTIST 1 – Aurora Synthwave  (Electronic)
    // ────────────────────────────────────────────────────────────────
    const QString art1 = QStringLiteral("artist_01");
    const QString art1Name = QStringLiteral("Aurora Synthwave");
    const QString art1Cover = QStringLiteral("qrc:/images/artists/aurora_synthwave.jpg");

    // Album 1-1: "Neon Horizons"
    const QString alb1 = QStringLiteral("album_01");
    const QString alb1Cover = QStringLiteral("qrc:/images/albums/neon_horizons.jpg");
    QVector<Track> alb1Tracks;
    alb1Tracks.append(makeTrack("t_0101", "Electric Dawn",        art1Name, "Neon Horizons", alb1, art1, 284, AudioFormat::FLAC,  "96kHz", "24-bit", "4608 kbps", alb1Cover, 1, 1));
    alb1Tracks.append(makeTrack("t_0102", "Retrograde Pulse",     art1Name, "Neon Horizons", alb1, art1, 312, AudioFormat::FLAC,  "96kHz", "24-bit", "4608 kbps", alb1Cover, 2, 1));
    alb1Tracks.append(makeTrack("t_0103", "Chromatic Drift",      art1Name, "Neon Horizons", alb1, art1, 257, AudioFormat::FLAC,  "96kHz", "24-bit", "4608 kbps", alb1Cover, 3, 1));
    alb1Tracks.append(makeTrack("t_0104", "Skyline Runner",       art1Name, "Neon Horizons", alb1, art1, 345, AudioFormat::FLAC,  "96kHz", "24-bit", "4608 kbps", alb1Cover, 4, 1));
    alb1Tracks.append(makeTrack("t_0105", "Vapor Cascade",        art1Name, "Neon Horizons", alb1, art1, 298, AudioFormat::FLAC,  "96kHz", "24-bit", "4608 kbps", alb1Cover, 5, 1));

    int alb1Duration = 0;
    for (const auto& t : alb1Tracks) alb1Duration += t.duration;

    Album album1{alb1, "Neon Horizons", art1Name, art1, 2024, alb1Cover,
                 AudioFormat::FLAC, static_cast<int>(alb1Tracks.size()), alb1Duration, alb1Tracks,
                 {"Electronic", "Synthwave"}};

    // Album 1-2: "Digital Mirage"
    const QString alb2 = QStringLiteral("album_02");
    const QString alb2Cover = QStringLiteral("qrc:/images/albums/digital_mirage.jpg");
    QVector<Track> alb2Tracks;
    alb2Tracks.append(makeTrack("t_0201", "Hologram City",        art1Name, "Digital Mirage", alb2, art1, 267, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb2Cover, 1, 1));
    alb2Tracks.append(makeTrack("t_0202", "Parallax Shift",       art1Name, "Digital Mirage", alb2, art1, 330, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb2Cover, 2, 1));
    alb2Tracks.append(makeTrack("t_0203", "Binary Sunset",        art1Name, "Digital Mirage", alb2, art1, 295, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb2Cover, 3, 1));
    alb2Tracks.append(makeTrack("t_0204", "Quantum Leap",         art1Name, "Digital Mirage", alb2, art1, 278, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb2Cover, 4, 1));

    int alb2Duration = 0;
    for (const auto& t : alb2Tracks) alb2Duration += t.duration;

    Album album2{alb2, "Digital Mirage", art1Name, art1, 2023, alb2Cover,
                 AudioFormat::DSD64, static_cast<int>(alb2Tracks.size()), alb2Duration, alb2Tracks,
                 {"Electronic", "Ambient"}};

    Artist artist1{art1, art1Name, art1Cover, {album1, album2}, {"Electronic", "Synthwave", "Ambient"}};

    // ────────────────────────────────────────────────────────────────
    //  ARTIST 2 – The Midnight Cascade  (Jazz)
    // ────────────────────────────────────────────────────────────────
    const QString art2 = QStringLiteral("artist_02");
    const QString art2Name = QStringLiteral("The Midnight Cascade");
    const QString art2Cover = QStringLiteral("qrc:/images/artists/midnight_cascade.jpg");

    // Album 2-1: "Velvet Underground Sessions"
    const QString alb3 = QStringLiteral("album_03");
    const QString alb3Cover = QStringLiteral("qrc:/images/albums/velvet_sessions.jpg");
    QVector<Track> alb3Tracks;
    alb3Tracks.append(makeTrack("t_0301", "Smoky Room Blues",     art2Name, "Velvet Underground Sessions", alb3, art2, 425, AudioFormat::DSD128, "5.6MHz", "1-bit", "5644 kbps", alb3Cover, 1, 1));
    alb3Tracks.append(makeTrack("t_0302", "Midnight Waltz",       art2Name, "Velvet Underground Sessions", alb3, art2, 378, AudioFormat::DSD128, "5.6MHz", "1-bit", "5644 kbps", alb3Cover, 2, 1));
    alb3Tracks.append(makeTrack("t_0303", "Cascading Notes",      art2Name, "Velvet Underground Sessions", alb3, art2, 356, AudioFormat::DSD128, "5.6MHz", "1-bit", "5644 kbps", alb3Cover, 3, 1));
    alb3Tracks.append(makeTrack("t_0304", "Bourbon Street Swing", art2Name, "Velvet Underground Sessions", alb3, art2, 290, AudioFormat::DSD128, "5.6MHz", "1-bit", "5644 kbps", alb3Cover, 4, 1));
    alb3Tracks.append(makeTrack("t_0305", "After Hours",          art2Name, "Velvet Underground Sessions", alb3, art2, 445, AudioFormat::DSD128, "5.6MHz", "1-bit", "5644 kbps", alb3Cover, 5, 1));
    alb3Tracks.append(makeTrack("t_0306", "Blue Satin",           art2Name, "Velvet Underground Sessions", alb3, art2, 310, AudioFormat::DSD128, "5.6MHz", "1-bit", "5644 kbps", alb3Cover, 6, 1));

    int alb3Duration = 0;
    for (const auto& t : alb3Tracks) alb3Duration += t.duration;

    Album album3{alb3, "Velvet Underground Sessions", art2Name, art2, 2024, alb3Cover,
                 AudioFormat::DSD128, static_cast<int>(alb3Tracks.size()), alb3Duration, alb3Tracks,
                 {"Jazz", "Blues"}};

    Artist artist2{art2, art2Name, art2Cover, {album3}, {"Jazz", "Blues"}};

    // ────────────────────────────────────────────────────────────────
    //  ARTIST 3 – Luna Eclipse  (Ambient / Downtempo)
    // ────────────────────────────────────────────────────────────────
    const QString art3 = QStringLiteral("artist_03");
    const QString art3Name = QStringLiteral("Luna Eclipse");
    const QString art3Cover = QStringLiteral("qrc:/images/artists/luna_eclipse.jpg");

    // Album 3-1: "Tidal Resonance"
    const QString alb4 = QStringLiteral("album_04");
    const QString alb4Cover = QStringLiteral("qrc:/images/albums/tidal_resonance.jpg");
    QVector<Track> alb4Tracks;
    alb4Tracks.append(makeTrack("t_0401", "Ocean Frequency",      art3Name, "Tidal Resonance", alb4, art3, 487, AudioFormat::FLAC,  "192kHz", "24-bit", "9216 kbps", alb4Cover, 1, 1));
    alb4Tracks.append(makeTrack("t_0402", "Deep Current",          art3Name, "Tidal Resonance", alb4, art3, 523, AudioFormat::FLAC,  "192kHz", "24-bit", "9216 kbps", alb4Cover, 2, 1));
    alb4Tracks.append(makeTrack("t_0403", "Bioluminescence",       art3Name, "Tidal Resonance", alb4, art3, 398, AudioFormat::FLAC,  "192kHz", "24-bit", "9216 kbps", alb4Cover, 3, 1));
    alb4Tracks.append(makeTrack("t_0404", "Abyssal Meditation",    art3Name, "Tidal Resonance", alb4, art3, 612, AudioFormat::FLAC,  "192kHz", "24-bit", "9216 kbps", alb4Cover, 4, 1));
    alb4Tracks.append(makeTrack("t_0405", "Coral Whisper",         art3Name, "Tidal Resonance", alb4, art3, 445, AudioFormat::FLAC,  "192kHz", "24-bit", "9216 kbps", alb4Cover, 5, 1));

    int alb4Duration = 0;
    for (const auto& t : alb4Tracks) alb4Duration += t.duration;

    Album album4{alb4, "Tidal Resonance", art3Name, art3, 2024, alb4Cover,
                 AudioFormat::FLAC, static_cast<int>(alb4Tracks.size()), alb4Duration, alb4Tracks,
                 {"Ambient", "Downtempo"}};

    // Album 3-2: "Ephemeral Light"
    const QString alb5 = QStringLiteral("album_05");
    const QString alb5Cover = QStringLiteral("qrc:/images/albums/ephemeral_light.jpg");
    QVector<Track> alb5Tracks;
    alb5Tracks.append(makeTrack("t_0501", "Dawn Particles",        art3Name, "Ephemeral Light", alb5, art3, 356, AudioFormat::ALAC, "48kHz", "24-bit", "2304 kbps", alb5Cover, 1, 1));
    alb5Tracks.append(makeTrack("t_0502", "Golden Hour Drift",     art3Name, "Ephemeral Light", alb5, art3, 412, AudioFormat::ALAC, "48kHz", "24-bit", "2304 kbps", alb5Cover, 2, 1));
    alb5Tracks.append(makeTrack("t_0503", "Twilight Dissolve",     art3Name, "Ephemeral Light", alb5, art3, 389, AudioFormat::ALAC, "48kHz", "24-bit", "2304 kbps", alb5Cover, 3, 1));
    alb5Tracks.append(makeTrack("t_0504", "Starfield Lullaby",     art3Name, "Ephemeral Light", alb5, art3, 478, AudioFormat::ALAC, "48kHz", "24-bit", "2304 kbps", alb5Cover, 4, 1));

    int alb5Duration = 0;
    for (const auto& t : alb5Tracks) alb5Duration += t.duration;

    Album album5{alb5, "Ephemeral Light", art3Name, art3, 2023, alb5Cover,
                 AudioFormat::ALAC, static_cast<int>(alb5Tracks.size()), alb5Duration, alb5Tracks,
                 {"Ambient", "Chillout"}};

    Artist artist3{art3, art3Name, art3Cover, {album4, album5}, {"Ambient", "Downtempo", "Chillout"}};

    // ────────────────────────────────────────────────────────────────
    //  ARTIST 4 – Digital Horizons  (Progressive Rock)
    // ────────────────────────────────────────────────────────────────
    const QString art4 = QStringLiteral("artist_04");
    const QString art4Name = QStringLiteral("Digital Horizons");
    const QString art4Cover = QStringLiteral("qrc:/images/artists/digital_horizons.jpg");

    // Album 4-1: "Fractal Architecture"
    const QString alb6 = QStringLiteral("album_06");
    const QString alb6Cover = QStringLiteral("qrc:/images/albums/fractal_architecture.jpg");
    QVector<Track> alb6Tracks;
    alb6Tracks.append(makeTrack("t_0601", "Recursive Dreams",      art4Name, "Fractal Architecture", alb6, art4, 478, AudioFormat::WAV,  "96kHz",  "24-bit", "4608 kbps", alb6Cover, 1, 1));
    alb6Tracks.append(makeTrack("t_0602", "Mandelbrot Suite",      art4Name, "Fractal Architecture", alb6, art4, 562, AudioFormat::WAV,  "96kHz",  "24-bit", "4608 kbps", alb6Cover, 2, 1));
    alb6Tracks.append(makeTrack("t_0603", "Fibonacci Spiral",      art4Name, "Fractal Architecture", alb6, art4, 390, AudioFormat::WAV,  "96kHz",  "24-bit", "4608 kbps", alb6Cover, 3, 1));
    alb6Tracks.append(makeTrack("t_0604", "Tessellation",          art4Name, "Fractal Architecture", alb6, art4, 445, AudioFormat::WAV,  "96kHz",  "24-bit", "4608 kbps", alb6Cover, 4, 1));
    alb6Tracks.append(makeTrack("t_0605", "Penrose Steps",         art4Name, "Fractal Architecture", alb6, art4, 512, AudioFormat::WAV,  "96kHz",  "24-bit", "4608 kbps", alb6Cover, 5, 1));
    alb6Tracks.append(makeTrack("t_0606", "Chaos Theory",          art4Name, "Fractal Architecture", alb6, art4, 634, AudioFormat::WAV,  "96kHz",  "24-bit", "4608 kbps", alb6Cover, 6, 1));
    alb6Tracks.append(makeTrack("t_0607", "Strange Attractor",     art4Name, "Fractal Architecture", alb6, art4, 489, AudioFormat::WAV,  "96kHz",  "24-bit", "4608 kbps", alb6Cover, 7, 1));

    int alb6Duration = 0;
    for (const auto& t : alb6Tracks) alb6Duration += t.duration;

    Album album6{alb6, "Fractal Architecture", art4Name, art4, 2024, alb6Cover,
                 AudioFormat::WAV, static_cast<int>(alb6Tracks.size()), alb6Duration, alb6Tracks,
                 {"Progressive Rock", "Art Rock"}};

    Artist artist4{art4, art4Name, art4Cover, {album6}, {"Progressive Rock", "Art Rock"}};

    // ────────────────────────────────────────────────────────────────
    //  ARTIST 5 – Sakura Dreams  (Classical Crossover)
    // ────────────────────────────────────────────────────────────────
    const QString art5 = QStringLiteral("artist_05");
    const QString art5Name = QStringLiteral("Sakura Dreams");
    const QString art5Cover = QStringLiteral("qrc:/images/artists/sakura_dreams.jpg");

    // Album 5-1: "Petals in the Wind"
    const QString alb7 = QStringLiteral("album_07");
    const QString alb7Cover = QStringLiteral("qrc:/images/albums/petals_wind.jpg");
    QVector<Track> alb7Tracks;
    alb7Tracks.append(makeTrack("t_0701", "Cherry Blossom Prelude",  art5Name, "Petals in the Wind", alb7, art5, 312, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb7Cover, 1, 1));
    alb7Tracks.append(makeTrack("t_0702", "Koi Pond Reflections",   art5Name, "Petals in the Wind", alb7, art5, 287, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb7Cover, 2, 1));
    alb7Tracks.append(makeTrack("t_0703", "Zen Garden Suite",       art5Name, "Petals in the Wind", alb7, art5, 456, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb7Cover, 3, 1));
    alb7Tracks.append(makeTrack("t_0704", "Wisteria Waltz",         art5Name, "Petals in the Wind", alb7, art5, 345, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb7Cover, 4, 1));
    alb7Tracks.append(makeTrack("t_0705", "Moonlit Temple",         art5Name, "Petals in the Wind", alb7, art5, 398, AudioFormat::DSD64, "2.8MHz", "1-bit", "2822 kbps", alb7Cover, 5, 1));

    int alb7Duration = 0;
    for (const auto& t : alb7Tracks) alb7Duration += t.duration;

    Album album7{alb7, "Petals in the Wind", art5Name, art5, 2023, alb7Cover,
                 AudioFormat::DSD64, static_cast<int>(alb7Tracks.size()), alb7Duration, alb7Tracks,
                 {"Classical Crossover", "World"}};

    // Album 5-2: "Silk Road Echoes"
    const QString alb8 = QStringLiteral("album_08");
    const QString alb8Cover = QStringLiteral("qrc:/images/albums/silk_road_echoes.jpg");
    QVector<Track> alb8Tracks;
    alb8Tracks.append(makeTrack("t_0801", "Caravan at Sunrise",     art5Name, "Silk Road Echoes", alb8, art5, 334, AudioFormat::MP3,  "44.1kHz", "16-bit", "320 kbps", alb8Cover, 1, 1));
    alb8Tracks.append(makeTrack("t_0802", "Bazaar of Wonders",     art5Name, "Silk Road Echoes", alb8, art5, 289, AudioFormat::MP3,  "44.1kHz", "16-bit", "320 kbps", alb8Cover, 2, 1));
    alb8Tracks.append(makeTrack("t_0803", "Oasis Nocturne",        art5Name, "Silk Road Echoes", alb8, art5, 367, AudioFormat::MP3,  "44.1kHz", "16-bit", "320 kbps", alb8Cover, 3, 1));
    alb8Tracks.append(makeTrack("t_0804", "Sandstorm Interlude",   art5Name, "Silk Road Echoes", alb8, art5, 198, AudioFormat::MP3,  "44.1kHz", "16-bit", "320 kbps", alb8Cover, 4, 1));
    alb8Tracks.append(makeTrack("t_0805", "Jade Palace",           art5Name, "Silk Road Echoes", alb8, art5, 412, AudioFormat::MP3,  "44.1kHz", "16-bit", "320 kbps", alb8Cover, 5, 1));
    alb8Tracks.append(makeTrack("t_0806", "Lotus Garden Finale",   art5Name, "Silk Road Echoes", alb8, art5, 378, AudioFormat::MP3,  "44.1kHz", "16-bit", "320 kbps", alb8Cover, 6, 1));

    int alb8Duration = 0;
    for (const auto& t : alb8Tracks) alb8Duration += t.duration;

    Album album8{alb8, "Silk Road Echoes", art5Name, art5, 2022, alb8Cover,
                 AudioFormat::MP3, static_cast<int>(alb8Tracks.size()), alb8Duration, alb8Tracks,
                 {"Classical Crossover", "World"}};

    Artist artist5{art5, art5Name, art5Cover, {album7, album8}, {"Classical Crossover", "World"}};

    // ────────────────────────────────────────────────────────────────
    //  Assemble Albums & Artists
    // ────────────────────────────────────────────────────────────────
    m_albums  = { album1, album2, album3, album4, album5, album6, album7, album8 };
    m_artists = { artist1, artist2, artist3, artist4, artist5 };

    // Flatten all tracks
    m_tracks.clear();
    for (const auto& album : m_albums)
        m_tracks.append(album.tracks);

    // ────────────────────────────────────────────────────────────────
    //  Playlists
    // ────────────────────────────────────────────────────────────────

    // Playlist 1: "Audiophile Essentials" (smart)
    {
        QVector<Track> pl;
        pl.append(alb1Tracks[0]);  // Electric Dawn – FLAC 96/24
        pl.append(alb3Tracks[0]);  // Smoky Room Blues – DSD128
        pl.append(alb4Tracks[0]);  // Ocean Frequency – FLAC 192/24
        pl.append(alb6Tracks[0]);  // Recursive Dreams – WAV 96/24
        pl.append(alb7Tracks[0]);  // Cherry Blossom Prelude – DSD64
        pl.append(alb4Tracks[3]);  // Abyssal Meditation – FLAC 192/24
        pl.append(alb3Tracks[4]);  // After Hours – DSD128
        pl.append(alb6Tracks[5]);  // Chaos Theory – WAV 96/24
        pl.append(alb2Tracks[2]);  // Binary Sunset – DSD64
        pl.append(alb1Tracks[3]);  // Skyline Runner – FLAC 96/24

        m_playlists.append(Playlist{
            "playlist_01",
            "Audiophile Essentials",
            "The finest recordings in hi-res formats, curated for critical listening.",
            "qrc:/images/playlists/audiophile_essentials.jpg",
            pl,
            true,
            "2024-01-15T10:30:00Z"
        });
    }

    // Playlist 2: "Late Night Sessions" (smart)
    {
        QVector<Track> pl;
        pl.append(alb3Tracks[0]);  // Smoky Room Blues
        pl.append(alb3Tracks[1]);  // Midnight Waltz
        pl.append(alb5Tracks[3]);  // Starfield Lullaby
        pl.append(alb3Tracks[4]);  // After Hours
        pl.append(alb4Tracks[1]);  // Deep Current
        pl.append(alb5Tracks[2]);  // Twilight Dissolve
        pl.append(alb3Tracks[5]);  // Blue Satin
        pl.append(alb7Tracks[4]);  // Moonlit Temple

        m_playlists.append(Playlist{
            "playlist_02",
            "Late Night Sessions",
            "Smooth jazz and ambient textures for quiet evenings.",
            "qrc:/images/playlists/late_night_sessions.jpg",
            pl,
            true,
            "2024-02-20T22:15:00Z"
        });
    }

    // Playlist 3: "Road Trip Mix" (user)
    {
        QVector<Track> pl;
        pl.append(alb1Tracks[0]);  // Electric Dawn
        pl.append(alb1Tracks[3]);  // Skyline Runner
        pl.append(alb6Tracks[2]);  // Fibonacci Spiral
        pl.append(alb8Tracks[0]);  // Caravan at Sunrise
        pl.append(alb2Tracks[0]);  // Hologram City
        pl.append(alb1Tracks[1]);  // Retrograde Pulse
        pl.append(alb6Tracks[4]);  // Penrose Steps
        pl.append(alb8Tracks[1]);  // Bazaar of Wonders
        pl.append(alb6Tracks[3]);  // Tessellation
        pl.append(alb1Tracks[4]);  // Vapor Cascade

        m_playlists.append(Playlist{
            "playlist_03",
            "Road Trip Mix",
            "Energetic tracks for long drives and adventures.",
            "qrc:/images/playlists/road_trip_mix.jpg",
            pl,
            false,
            "2024-03-10T14:00:00Z"
        });
    }

    // Playlist 4: "Focus Flow" (user)
    {
        QVector<Track> pl;
        pl.append(alb4Tracks[0]);  // Ocean Frequency
        pl.append(alb5Tracks[0]);  // Dawn Particles
        pl.append(alb4Tracks[2]);  // Bioluminescence
        pl.append(alb7Tracks[2]);  // Zen Garden Suite
        pl.append(alb5Tracks[1]);  // Golden Hour Drift
        pl.append(alb4Tracks[4]);  // Coral Whisper
        pl.append(alb7Tracks[3]);  // Wisteria Waltz
        pl.append(alb4Tracks[3]);  // Abyssal Meditation

        m_playlists.append(Playlist{
            "playlist_04",
            "Focus Flow",
            "Ambient and downtempo soundscapes to maintain concentration.",
            "qrc:/images/playlists/focus_flow.jpg",
            pl,
            false,
            "2024-04-05T09:45:00Z"
        });
    }
}
