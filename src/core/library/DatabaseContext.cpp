#include "DatabaseContext.h"

#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

QString DatabaseContext::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString DatabaseContext::audioFormatToString(AudioFormat fmt) const
{
    switch (fmt) {
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

AudioFormat DatabaseContext::audioFormatFromString(const QString& str) const
{
    if (str == QStringLiteral("FLAC"))    return AudioFormat::FLAC;
    if (str == QStringLiteral("DSD64"))   return AudioFormat::DSD64;
    if (str == QStringLiteral("DSD128"))  return AudioFormat::DSD128;
    if (str == QStringLiteral("DSD256"))  return AudioFormat::DSD256;
    if (str == QStringLiteral("DSD512"))  return AudioFormat::DSD512;
    if (str == QStringLiteral("DSD1024")) return AudioFormat::DSD1024;
    if (str == QStringLiteral("DSD2048")) return AudioFormat::DSD2048;
    if (str == QStringLiteral("ALAC"))    return AudioFormat::ALAC;
    if (str == QStringLiteral("WAV"))     return AudioFormat::WAV;
    if (str == QStringLiteral("MP3"))     return AudioFormat::MP3;
    if (str == QStringLiteral("AAC"))     return AudioFormat::AAC;
    return AudioFormat::FLAC; // fallback
}

Track DatabaseContext::trackFromQuery(const QSqlQuery& query) const
{
    Track t;
    t.id          = query.value(QStringLiteral("id")).toString();
    t.title       = query.value(QStringLiteral("title")).toString();
    t.artist      = query.value(QStringLiteral("artist")).toString();
    t.album       = query.value(QStringLiteral("album")).toString();
    t.albumId     = query.value(QStringLiteral("album_id")).toString();

    // album_artist (migration column — may not exist in old DBs)
    int aaIdx = query.record().indexOf(QStringLiteral("album_artist"));
    if (aaIdx >= 0)
        t.albumArtist = query.value(aaIdx).toString();
    t.artistId    = query.value(QStringLiteral("artist_id")).toString();
    t.duration    = query.value(QStringLiteral("duration")).toInt();
    t.format      = audioFormatFromString(query.value(QStringLiteral("format")).toString());
    t.sampleRate  = query.value(QStringLiteral("sample_rate")).toString();
    t.bitDepth    = query.value(QStringLiteral("bit_depth")).toString();
    t.bitrate     = query.value(QStringLiteral("bitrate")).toString();
    t.coverUrl    = query.value(QStringLiteral("cover_url")).toString();
    t.trackNumber = query.value(QStringLiteral("track_number")).toInt();
    t.discNumber       = query.value(QStringLiteral("disc_number")).toInt();
    t.filePath         = query.value(QStringLiteral("file_path")).toString();
    t.recordingMbid    = query.value(QStringLiteral("recording_mbid")).toString();
    t.artistMbid       = query.value(QStringLiteral("artist_mbid")).toString();
    t.albumMbid        = query.value(QStringLiteral("album_mbid")).toString();
    t.releaseGroupMbid = query.value(QStringLiteral("release_group_mbid")).toString();

    // Year (migration column — may not exist in old DBs)
    int yearIdx = query.record().indexOf(QStringLiteral("year"));
    if (yearIdx >= 0)
        t.year = query.value(yearIdx).toInt();

    // Channel count
    int chIdx = query.record().indexOf(QStringLiteral("channel_count"));
    if (chIdx >= 0) {
        int ch = query.value(chIdx).toInt();
        t.channelCount = (ch > 0) ? ch : 2;
    }

    // Load cached R128 loudness if available
    int r128Idx = query.record().indexOf(QStringLiteral("r128_loudness"));
    if (r128Idx >= 0) {
        t.r128Loudness = query.value(r128Idx).toDouble();
        t.r128Peak = query.value(QStringLiteral("r128_peak")).toDouble();
        if (t.r128Loudness != 0.0) t.hasR128 = true;
    }

    // File size/mtime for scan skip
    int fsIdx = query.record().indexOf(QStringLiteral("file_size"));
    if (fsIdx >= 0) t.fileSize = query.value(fsIdx).toLongLong();
    int mtIdx = query.record().indexOf(QStringLiteral("file_mtime"));
    if (mtIdx >= 0) t.fileMtime = query.value(mtIdx).toLongLong();

    return t;
}
