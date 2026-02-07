#include "MetadataService.h"
#include "MusicBrainzProvider.h"
#include "CoverArtProvider.h"
#include "FanartTvProvider.h"
#include "AcoustIdProvider.h"
#include "AudioFingerprinter.h"
#include "../core/library/LibraryDatabase.h"

#include <QDebug>
#include <QFileInfo>
#include <QTimer>

MetadataService* MetadataService::instance()
{
    static MetadataService s_instance;
    return &s_instance;
}

MetadataService::MetadataService(QObject* parent)
    : QObject(parent)
    , m_musicBrainz(MusicBrainzProvider::instance())
    , m_coverArt(CoverArtProvider::instance())
    , m_fanartTv(FanartTvProvider::instance())
    , m_acoustId(AcoustIdProvider::instance())
    , m_fingerprinter(AudioFingerprinter::instance())
{
    qDebug() << "[MetadataService] Initialized";

    // ── MusicBrainz: track found → update DB and chain fetches ──────
    connect(m_musicBrainz, &MusicBrainzProvider::trackFound,
            this, [this](const MusicBrainzResult& result) {

        qDebug() << "[MetadataService] trackFound signal received"
                 << "| rgMbid:" << result.releaseGroupMbid
                 << "| albumMbid:" << result.albumMbid
                 << "| artistMbid:" << result.artistMbid;

        if (m_currentIndex > 0 && m_currentIndex <= m_pendingTracks.size()) {
            Track& track = m_pendingTracks[m_currentIndex - 1];

            // Update fields that exist on Track
            if (!result.title.isEmpty())  track.title  = result.title;
            if (!result.artist.isEmpty()) track.artist = result.artist;
            if (!result.album.isEmpty())  track.album  = result.album;
            if (result.trackNumber > 0)   track.trackNumber = result.trackNumber;
            if (result.discNumber > 0)    track.discNumber  = result.discNumber;

            // Store MBIDs on the track
            if (!result.mbid.isEmpty())             track.recordingMbid    = result.mbid;
            if (!result.artistMbid.isEmpty())       track.artistMbid       = result.artistMbid;
            if (!result.albumMbid.isEmpty())        track.albumMbid        = result.albumMbid;
            if (!result.releaseGroupMbid.isEmpty()) track.releaseGroupMbid = result.releaseGroupMbid;

            // Persist via the existing updateTrack() method
            LibraryDatabase::instance()->updateTrack(track);

            emit metadataUpdated(track.id, track);

            // Chain: fetch album art — prefer release-group, fall back to release
            if (!result.releaseGroupMbid.isEmpty()) {
                qDebug() << "[MetadataService] -> fetchAlbumArt (release-group)"
                         << result.releaseGroupMbid;
                m_coverArt->fetchAlbumArt(result.releaseGroupMbid, true);
            } else if (!result.albumMbid.isEmpty()) {
                qDebug() << "[MetadataService] -> fetchAlbumArt (release)"
                         << result.albumMbid;
                m_coverArt->fetchAlbumArt(result.albumMbid, false);
            } else {
                qDebug() << "[MetadataService] -> No album MBID, skipping art";
            }

            // Chain: fetch artist images via Fanart.tv
            if (!result.artistMbid.isEmpty()) {
                qDebug() << "[MetadataService] -> fetchArtistImages"
                         << result.artistMbid;
                m_fanartTv->fetchArtistImages(result.artistMbid);
            } else {
                qDebug() << "[MetadataService] -> No artist MBID, skipping images";
            }
        }

        processNextInQueue();
    });

    connect(m_musicBrainz, &MusicBrainzProvider::noResultsFound,
            this, [this]() {
        qDebug() << "[MetadataService] No MusicBrainz results for current track";
        processNextInQueue();
    });

    connect(m_musicBrainz, &MusicBrainzProvider::searchError,
            this, [this](const QString& error) {
        qWarning() << "[MetadataService] MusicBrainz error:" << error;
        processNextInQueue();
    });

    // ── Cover Art Archive signals ───────────────────────────────────
    connect(m_coverArt, &CoverArtProvider::albumArtFetched, this,
            [this](const QString& mbid, const QPixmap& pixmap,
                   const QString& path) {
        qDebug() << "[MetadataService] Album art fetched:"
                 << mbid << path << "valid:" << !pixmap.isNull();
        emit albumArtUpdated(mbid, path);
    });

    connect(m_coverArt, &CoverArtProvider::albumArtNotFound, this,
            [](const QString& mbid) {
        qDebug() << "[MetadataService] Album art not found:" << mbid;
    });

    connect(m_coverArt, &CoverArtProvider::fetchError, this,
            [](const QString& mbid, const QString& error) {
        qWarning() << "[MetadataService] CoverArt error:"
                   << mbid << error;
    });

    // ── Fanart.tv signals ───────────────────────────────────────────
    connect(m_fanartTv, &FanartTvProvider::artistImagesFetched, this,
            [](const QString& mbid, const ArtistImages& images) {
        qDebug() << "[MetadataService] Artist images fetched:"
                 << mbid
                 << "thumb:" << images.artistThumb
                 << "bg:" << images.artistBackground;
    });

    connect(m_fanartTv, &FanartTvProvider::artistThumbDownloaded, this,
            [this](const QString& mbid, const QPixmap& pixmap,
                   const QString& path) {
        qDebug() << "[MetadataService] Artist thumb downloaded:"
                 << mbid << path << "valid:" << !pixmap.isNull();
        emit artistImageUpdated(mbid, path);
    });

    connect(m_fanartTv, &FanartTvProvider::artistImagesNotFound, this,
            [](const QString& mbid) {
        qDebug() << "[MetadataService] Artist images not found:" << mbid;
    });

    connect(m_fanartTv, &FanartTvProvider::fetchError, this,
            [](const QString& mbid, const QString& error) {
        qWarning() << "[MetadataService] Fanart.tv error:"
                   << mbid << error;
    });

    // ── Fingerprinter signals (permanent connections) ─────────────────
    connect(m_fingerprinter, &AudioFingerprinter::fingerprintReady,
            this, [this](const QString& /*fp*/, const QString& fingerprint, int duration) {
        if (m_currentProcessingTrackId.isEmpty()) return;

        QString trackId = m_currentProcessingTrackId;
        qDebug() << "[Batch] Fingerprint ready for trackId:" << trackId
                 << "length:" << fingerprint.length() << "duration:" << duration << "s";

        m_acoustId->lookup(fingerprint, duration, trackId);
    });

    connect(m_fingerprinter, &AudioFingerprinter::fingerprintError,
            this, [this](const QString& /*fp*/, const QString& error) {
        QString trackId = m_currentProcessingTrackId;
        qWarning() << "[Batch] Fingerprint error for trackId:" << trackId << ":" << error;

        if (m_isFingerprintBatch) {
            m_isProcessingTrack = false;
            QTimer::singleShot(300, this, &MetadataService::processNextFingerprint);
        } else {
            m_isProcessingTrack = false;
            m_currentProcessingTrackId.clear();
            emit identifyFailed(trackId, QStringLiteral("Fingerprint failed: ") + error);
        }
    });

    // ── AcoustID signals (permanent connections) ──────────────────────
    connect(m_acoustId, &AcoustIdProvider::trackIdentified,
            this, [this](const MusicBrainzResult& result, const QString& trackId) {
        qDebug() << "[Batch] AcoustID identified trackId:" << trackId;
        handleFingerprintResult(trackId, result);

        if (m_isFingerprintBatch) {
            m_isProcessingTrack = false;
            QTimer::singleShot(300, this, &MetadataService::processNextFingerprint);
        } else {
            m_isProcessingTrack = false;
            m_currentProcessingTrackId.clear();
            MusicDataProvider::instance()->reloadFromDatabase();
        }
    });

    connect(m_acoustId, &AcoustIdProvider::noMatch,
            this, [this](const QString& trackId) {
        qDebug() << "[Batch] AcoustID: no match for trackId:" << trackId;

        if (m_isFingerprintBatch) {
            m_isProcessingTrack = false;
            QTimer::singleShot(300, this, &MetadataService::processNextFingerprint);
        } else {
            m_isProcessingTrack = false;
            m_currentProcessingTrackId.clear();

            // Find track name for user-friendly message
            QString trackName;
            for (const Track& t : m_fingerprintQueue) {
                if (t.id == trackId) {
                    trackName = t.title.isEmpty()
                        ? QFileInfo(t.filePath).fileName() : t.title;
                    break;
                }
            }
            if (trackName.isEmpty()) {
                auto opt = LibraryDatabase::instance()->trackById(trackId);
                if (opt.has_value()) {
                    const Track& t = opt.value();
                    trackName = t.title.isEmpty()
                        ? QFileInfo(t.filePath).fileName() : t.title;
                }
            }

            QString msg = QStringLiteral("No match found in AcoustID database");
            if (!trackName.isEmpty())
                msg += QStringLiteral(" for: ") + trackName;
            emit identifyFailed(trackId, msg);
        }
    });

    connect(m_acoustId, &AcoustIdProvider::lookupError,
            this, [this](const QString& error, const QString& trackId) {
        qWarning() << "[Batch] AcoustID error for trackId:" << trackId << error;

        if (m_isFingerprintBatch) {
            m_isProcessingTrack = false;
            QTimer::singleShot(300, this, &MetadataService::processNextFingerprint);
        } else {
            m_isProcessingTrack = false;
            m_currentProcessingTrackId.clear();
            emit identifyFailed(trackId, QStringLiteral("AcoustID lookup error: ") + error);
        }
    });
}

// ── Single-track fetch ──────────────────────────────────────────────
void MetadataService::fetchMetadata(const Track& track)
{
    m_pendingTracks.clear();
    m_pendingTracks.append(track);
    m_currentIndex = 0;
    m_isFetching   = true;

    emit fetchProgress(0, 1, QStringLiteral("Fetching metadata..."));
    processNextInQueue();
}

// ── Batch fetch ─────────────────────────────────────────────────────
void MetadataService::fetchMissingMetadata(const QVector<Track>& tracks)
{
    m_pendingTracks = tracks;
    m_currentIndex  = 0;
    m_isFetching    = true;

    qDebug() << "[MetadataService] Starting batch fetch for"
             << tracks.size() << "tracks";

    emit fetchProgress(0, tracks.size(),
                       QStringLiteral("Starting metadata fetch..."));
    processNextInQueue();
}

// ── Queue processor ─────────────────────────────────────────────────
void MetadataService::processNextInQueue()
{
    if (m_currentIndex >= m_pendingTracks.size()) {
        m_isFetching = false;
        qDebug() << "[MetadataService] Batch complete";
        emit fetchComplete();
        return;
    }

    const Track& track = m_pendingTracks[m_currentIndex];
    m_currentIndex++;

    qDebug() << "[MetadataService] Processing" << m_currentIndex
             << "/" << m_pendingTracks.size()
             << ":" << track.artist << "-" << track.title;

    emit fetchProgress(m_currentIndex, m_pendingTracks.size(),
                       QStringLiteral("Fetching: %1 - %2")
                           .arg(track.artist, track.title));

    m_musicBrainz->searchTrack(track.title, track.artist, track.album);
}

// ── Album art by name ───────────────────────────────────────────────
void MetadataService::fetchAlbumArt(const QString& albumMbid,
                                     bool isReleaseGroup)
{
    m_coverArt->fetchAlbumArt(albumMbid, isReleaseGroup);
}

void MetadataService::fetchAlbumArtByInfo(const QString& album,
                                           const QString& artist)
{
    // Disconnect any previous one-shot connection before making a new one
    disconnect(m_musicBrainz, &MusicBrainzProvider::albumFound, this, nullptr);

    connect(m_musicBrainz, &MusicBrainzProvider::albumFound, this,
            [this](const QString& mbid, const QString& rgMbid,
                   const QJsonObject&) {
        m_coverArt->fetchAlbumArt(
            rgMbid.isEmpty() ? mbid : rgMbid, !rgMbid.isEmpty());
    }, Qt::SingleShotConnection);

    m_musicBrainz->searchAlbum(album, artist);
}

// ── Artist images by MBID or name ───────────────────────────────────
void MetadataService::fetchArtistImages(const QString& artistMbid)
{
    m_fanartTv->fetchArtistImages(artistMbid);
}

void MetadataService::fetchArtistImagesByName(const QString& artistName)
{
    // Disconnect any previous one-shot connection before making a new one
    disconnect(m_musicBrainz, &MusicBrainzProvider::artistFound, this, nullptr);

    connect(m_musicBrainz, &MusicBrainzProvider::artistFound, this,
            [this](const QString& mbid, const QJsonObject&) {
        m_fanartTv->fetchArtistImages(mbid);
    }, Qt::SingleShotConnection);

    m_musicBrainz->searchArtist(artistName);
}

// ── Shared result handler ─────────────────────────────────────────────
void MetadataService::handleFingerprintResult(const QString& trackId,
                                               const MusicBrainzResult& result)
{
    // Look up the track by ID — first check the batch queue, then fall back to DB
    Track track;
    bool found = false;
    for (const auto& t : m_fingerprintQueue) {
        if (t.id == trackId) { track = t; found = true; break; }
    }
    if (!found) {
        auto opt = LibraryDatabase::instance()->trackById(trackId);
        if (opt.has_value()) { track = opt.value(); found = true; }
    }
    if (!found) {
        qWarning() << "[MetadataService] handleFingerprintResult: track not found for id:" << trackId;
        return;
    }

    // Skip if track already has good metadata — don't overwrite correct tags
    auto isGoodTitle = [](const QString& t) {
        if (t.isEmpty() || t == QStringLiteral("Unknown")) return false;
        // Bare track-number filenames like "01 ...", "01_...", "01-..."
        if (t.length() >= 2 && t[0].isDigit() && t[1].isDigit()) {
            if (t.length() == 2) return false;
            QChar c = t[2];
            if (c == QLatin1Char(' ') || c == QLatin1Char('_') || c == QLatin1Char('-'))
                return false;
        }
        return true;
    };
    auto isGoodArtist = [](const QString& a) {
        return !a.isEmpty()
            && a != QStringLiteral("Unknown Artist")
            && a != QStringLiteral("Unknown");
    };

    if (isGoodTitle(track.title) && isGoodArtist(track.artist)) {
        qDebug() << "[Batch] Track already has good metadata, skipping:";
        qDebug() << "  Current:" << track.artist << "-" << track.title;
        qDebug() << "  AcoustID would set:" << result.artist << "-" << result.title;
        return;
    }

    qDebug() << "=== AcoustID Result for track" << track.id << "===";
    qDebug() << "  File:" << track.filePath;
    qDebug() << "  Before: title=" << track.title << "artist=" << track.artist;
    qDebug() << "  AcoustID: title=" << result.title << "artist=" << result.artist
             << "album=" << result.album << "score=" << result.score;
    qDebug() << "  MBIDs: rec=" << result.mbid << "artist=" << result.artistMbid
             << "album=" << result.albumMbid << "rg=" << result.releaseGroupMbid;

    // Merge: use AcoustID result if non-empty, otherwise keep original
    QString newTitle  = result.title.isEmpty()  ? track.title  : result.title;
    QString newArtist = result.artist.isEmpty() ? track.artist : result.artist;
    QString newAlbum  = result.album.isEmpty()  ? track.album  : result.album;
    QString newRecMbid = result.mbid.isEmpty()             ? track.recordingMbid    : result.mbid;
    QString newArtMbid = result.artistMbid.isEmpty()       ? track.artistMbid       : result.artistMbid;
    QString newAlbMbid = result.albumMbid.isEmpty()        ? track.albumMbid        : result.albumMbid;
    QString newRgMbid  = result.releaseGroupMbid.isEmpty() ? track.releaseGroupMbid : result.releaseGroupMbid;

    qDebug() << "  After merge -> title=" << newTitle
             << "artist=" << newArtist << "album=" << newAlbum;

    // Targeted UPDATE: only modify metadata columns, leave everything else untouched
    auto* db = LibraryDatabase::instance();
    db->backupTrackMetadata(track.id);
    bool ok = db->updateTrackMetadata(track.id,
                                       newTitle, newArtist, newAlbum,
                                       newRecMbid, newArtMbid, newAlbMbid, newRgMbid);
    qDebug() << "  DB updateTrackMetadata returned:" << ok << "for id:" << track.id;

    // Only rebuild albums/artists for single-track identification, not during batch
    // (batch calls rebuildAlbumsAndArtists once at the end)
    if (!m_isFingerprintBatch) {
        db->rebuildAlbumsAndArtists();
    }

    // Build an updated Track object for signals (read fresh from DB if possible)
    Track updated = track;
    updated.title = newTitle;
    updated.artist = newArtist;
    updated.album = newAlbum;
    updated.recordingMbid = newRecMbid;
    updated.artistMbid = newArtMbid;
    updated.albumMbid = newAlbMbid;
    updated.releaseGroupMbid = newRgMbid;

    emit metadataUpdated(updated.id, updated);

    if (!newRgMbid.isEmpty())
        m_coverArt->fetchAlbumArt(newRgMbid, true);
    else if (!newAlbMbid.isEmpty())
        m_coverArt->fetchAlbumArt(newAlbMbid, false);

    if (!newArtMbid.isEmpty())
        m_fanartTv->fetchArtistImages(newArtMbid);
}

// ── Single-track fingerprint identification ──────────────────────────
void MetadataService::identifyByFingerprint(const Track& track)
{
    if (track.filePath.isEmpty()) {
        emit fetchError(QStringLiteral("No file path for fingerprinting"));
        return;
    }

    qDebug() << "[MetadataService] Fingerprinting:" << track.filePath
             << "trackId:" << track.id
             << "currentTitle:" << track.title
             << "currentArtist:" << track.artist;

    // Use the same permanent connections as batch — they check m_isFingerprintBatch
    // to decide whether to advance the queue or just emit fetchError/reload
    m_currentProcessingTrackId = track.id;
    m_isProcessingTrack = true;
    m_fingerprinter->generateFingerprint(track.filePath);
}

// ── Batch fingerprint identification ─────────────────────────────────
void MetadataService::identifyByFingerprintBatch(const QVector<Track>& tracks)
{
    if (tracks.isEmpty()) return;

    m_fingerprintQueue = tracks;
    m_fingerprintIndex = 0;
    m_isFingerprintBatch = true;

    qDebug() << "[MetadataService] Starting batch fingerprint for"
             << tracks.size() << "tracks";

    emit fetchProgress(0, tracks.size(),
                       QStringLiteral("Starting audio identification..."));
    processNextFingerprint();
}

void MetadataService::processNextFingerprint()
{
    if (m_isProcessingTrack) {
        qWarning() << "[Batch] Already processing a track, skipping";
        return;
    }

    if (m_fingerprintIndex >= m_fingerprintQueue.size()) {
        m_isFingerprintBatch = false;
        m_isProcessingTrack = false;
        m_currentProcessingTrackId.clear();
        m_fingerprintQueue.clear();
        qDebug() << "[Batch] Batch fingerprint complete, rebuilding albums/artists";
        LibraryDatabase::instance()->rebuildAlbumsAndArtists();
        MusicDataProvider::instance()->reloadFromDatabase();
        emit fetchComplete();
        return;
    }

    m_isProcessingTrack = true;

    const Track& track = m_fingerprintQueue[m_fingerprintIndex];
    m_currentProcessingTrackId = track.id;
    m_fingerprintIndex++;

    QString shortName = QFileInfo(track.filePath).fileName();
    if (shortName.length() > 30)
        shortName = shortName.left(27) + QStringLiteral("...");

    qDebug() << "[Batch] Processing" << m_fingerprintIndex
             << "of" << m_fingerprintQueue.size()
             << "| Track ID:" << track.id << "| file:" << shortName;

    emit fetchProgress(m_fingerprintIndex, m_fingerprintQueue.size(),
                       QStringLiteral("Analyzing: %1").arg(shortName));

    // No dynamic connections — permanent connections in the constructor handle everything.
    // The permanent handlers use m_currentProcessingTrackId and m_isFingerprintBatch
    // to route results correctly and advance the queue.
    m_fingerprinter->generateFingerprint(track.filePath);
}

void MetadataService::autoIdentify(const QVector<Track>& tracks)
{
    QVector<Track> needsId;
    for (const auto& track : tracks) {
        if (track.title.isEmpty() || track.artist.isEmpty()) {
            needsId.append(track);
        }
    }
    if (!needsId.isEmpty())
        identifyByFingerprintBatch(needsId);
}
