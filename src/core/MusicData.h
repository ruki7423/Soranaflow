#ifndef MUSICDATA_H
#define MUSICDATA_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QColor>
#include <QObject>
#include <QReadWriteLock>

// ── Audio Format Enum ───────────────────────────────────────────────
enum class AudioFormat {
    FLAC,
    DSD64,
    DSD128,
    DSD256,
    DSD512,
    DSD1024,
    DSD2048,
    ALAC,
    WAV,
    MP3,
    AAC
};

// ── Audio Quality Classification ────────────────────────────────────
enum class AudioQuality {
    Unknown,
    Lossy,       // MP3, AAC, OGG, WMA
    Lossless,    // CD quality: 16-bit/44.1-48kHz lossless (FLAC, ALAC, WAV)
    HiRes,       // Lossless at >16-bit or >48kHz
    DSD          // DSD64, DSD128, DSD256, DSD512, DSD1024, DSD2048
};

AudioQuality classifyAudioQuality(AudioFormat format,
                                  const QString& sampleRate,
                                  const QString& bitDepth);
QString       getQualityLabel(AudioQuality quality);
QColor        getQualityColor(AudioQuality quality);

// ── Data Structs ────────────────────────────────────────────────────
struct Track {
    QString id;
    QString title;
    QString artist;
    QString album;
    QString albumId;
    QString artistId;
    int     duration = 0;   // seconds
    AudioFormat format = AudioFormat::FLAC;
    QString sampleRate;     // e.g. "96kHz"
    QString bitDepth;       // e.g. "24-bit"
    QString bitrate;        // e.g. "4608 kbps"
    QString coverUrl;
    int     trackNumber = 0;
    int     discNumber = 0;
    QString filePath;       // empty for mock tracks
    QString recordingMbid;  // MusicBrainz recording ID
    QString artistMbid;     // MusicBrainz artist ID
    QString albumMbid;      // MusicBrainz release ID
    QString releaseGroupMbid; // MusicBrainz release-group ID

    int     channelCount = 2;  // Number of audio channels (1=mono, 2=stereo, 6=5.1, etc.)

    // Volume leveling (ReplayGain / EBU R128)
    double replayGainTrack = 0.0;      // dB
    double replayGainAlbum = 0.0;      // dB
    double replayGainTrackPeak = 1.0;  // linear
    double replayGainAlbumPeak = 1.0;  // linear
    double r128Loudness = 0.0;         // LUFS (0 = not analyzed)
    double r128Peak = 0.0;             // dBTP
    bool hasReplayGain = false;
    bool hasR128 = false;
};

struct Album {
    QString         id;
    QString         title;
    QString         artist;
    QString         artistId;
    int             year = 0;
    QString         coverUrl;
    AudioFormat     format = AudioFormat::FLAC;
    int             totalTracks = 0;
    int             duration = 0;   // total seconds
    QVector<Track>  tracks;
    QStringList     genres;
};

struct Artist {
    QString         id;
    QString         name;
    QString         coverUrl;
    QVector<Album>  albums;
    QStringList     genres;
};

struct Playlist {
    QString         id;
    QString         name;
    QString         description;
    QString         coverUrl;
    QVector<Track>  tracks;
    bool            isSmartPlaylist = false;
    QString         createdAt;
};

// ── Singleton Data Provider ─────────────────────────────────────────
class MusicDataProvider : public QObject
{
    Q_OBJECT

public:
    static MusicDataProvider* instance();

    QVector<Track>    allTracks()    const;
    QVector<Album>    allAlbums()    const;
    QVector<Artist>   allArtists()   const;
    QVector<Playlist> allPlaylists() const;

    Album    albumById(const QString& id)    const;
    Artist   artistById(const QString& id)   const;
    Playlist playlistById(const QString& id) const;

    bool hasDatabaseTracks() const;

public slots:
    void reloadFromDatabase();

signals:
    void libraryUpdated();

private:
    explicit MusicDataProvider(QObject* parent = nullptr);
    void buildMockData();
    void loadFromDatabase();

    mutable QReadWriteLock m_lock;
    QVector<Track>    m_tracks;
    QVector<Album>    m_albums;
    QVector<Artist>   m_artists;
    QVector<Playlist> m_playlists;

    bool m_useMockData = true;
};

// ── Utility Functions ───────────────────────────────────────────────
QString formatDuration(int seconds);
QColor  getFormatColor(AudioFormat format);
QString getFormatLabel(AudioFormat format);
QString getFormatSpecs(AudioFormat format,
                       const QString& sampleRate,
                       const QString& bitDepth,
                       const QString& bitrate);

#endif // MUSICDATA_H
