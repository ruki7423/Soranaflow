#pragma once

#include <QObject>
#include <QVector>
#include "../core/MusicData.h"

class MusicBrainzProvider;
class CoverArtProvider;
class FanartTvProvider;
class AcoustIdProvider;
class AudioFingerprinter;
struct MusicBrainzResult;

class MetadataService : public QObject {
    Q_OBJECT
public:
    static MetadataService* instance();

    // Fetch metadata for a single track
    void fetchMetadata(const Track& track);

    // Fetch album art
    void fetchAlbumArt(const QString& albumMbid, bool isReleaseGroup = false);
    void fetchAlbumArtByInfo(const QString& album, const QString& artist);

    // Fetch artist images
    void fetchArtistImages(const QString& artistMbid);
    void fetchArtistImagesByName(const QString& artistName);

    // Batch
    void fetchMissingMetadata(const QVector<Track>& tracks);

    // Audio fingerprint identification
    void identifyByFingerprint(const Track& track);
    void identifyByFingerprintBatch(const QVector<Track>& tracks);
    void autoIdentify(const QVector<Track>& tracks);

    bool isFetching() const { return m_isFetching; }
    bool isFingerprintBatch() const { return m_isFingerprintBatch; }

signals:
    void metadataUpdated(const QString& trackId, const Track& updatedTrack);
    void albumArtUpdated(const QString& albumMbid, const QString& imagePath);
    void artistImageUpdated(const QString& artistMbid, const QString& imagePath);

    void fetchProgress(int current, int total, const QString& status);
    void fetchComplete();
    void fetchError(const QString& error);
    void identifyFailed(const QString& trackId, const QString& message);

private:
    explicit MetadataService(QObject* parent = nullptr);

    void processNextInQueue();
    void processNextFingerprint();
    void handleFingerprintResult(const QString& trackId, const MusicBrainzResult& result);

    MusicBrainzProvider* m_musicBrainz;
    CoverArtProvider*    m_coverArt;
    FanartTvProvider*    m_fanartTv;
    AcoustIdProvider*    m_acoustId;
    AudioFingerprinter*  m_fingerprinter;

    QVector<Track> m_pendingTracks;
    int  m_currentIndex = 0;
    bool m_isFetching   = false;

    QVector<Track> m_fingerprintQueue;
    int  m_fingerprintIndex = 0;
    bool m_isFingerprintBatch = false;
    bool m_isProcessingTrack = false;
    QString m_currentProcessingTrackId;
};
