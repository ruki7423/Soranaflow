#include "MetadataReader.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
}

#include <QFileInfo>
#include <QUuid>
#include <QDebug>

static AudioFormat detectFormat(const QString& filePath, AVCodecID codecId)
{
    QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext == "dsf" || ext == "dff") {
        // Will be refined to DSD64/DSD128 below based on sample rate
        return AudioFormat::DSD64;
    }
    if (ext == "flac")                              return AudioFormat::FLAC;
    if (ext == "alac" || ext == "m4a") {
        if (codecId == AV_CODEC_ID_ALAC)            return AudioFormat::ALAC;
        return AudioFormat::AAC;
    }
    if (ext == "wav")                               return AudioFormat::WAV;
    if (ext == "mp3")                               return AudioFormat::MP3;
    if (ext == "aac")                               return AudioFormat::AAC;

    // Fallback by codec
    switch (codecId) {
    case AV_CODEC_ID_FLAC:    return AudioFormat::FLAC;
    case AV_CODEC_ID_ALAC:    return AudioFormat::ALAC;
    case AV_CODEC_ID_MP3:     return AudioFormat::MP3;
    case AV_CODEC_ID_AAC:     return AudioFormat::AAC;
    case AV_CODEC_ID_VORBIS:  return AudioFormat::AAC; // map OGG vorbis to AAC bucket
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_F32LE:
        return AudioFormat::WAV;
    default:
        return AudioFormat::FLAC;
    }
}

static QString getTag(AVDictionary* dict, const char* key)
{
    AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
    return entry ? QString::fromUtf8(entry->value) : QString();
}

std::optional<Track> MetadataReader::readTrack(const QString& filePath)
{
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toStdString().c_str(), nullptr, nullptr) < 0)
        return std::nullopt;

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return std::nullopt;
    }

    int audioIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIdx < 0) {
        avformat_close_input(&fmtCtx);
        return std::nullopt;
    }

    AVStream* stream = fmtCtx->streams[audioIdx];
    AVCodecParameters* codecPar = stream->codecpar;

    Track track;
    track.id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    track.filePath = filePath;

    // Read metadata tags
    AVDictionary* meta = fmtCtx->metadata;
    track.title  = getTag(meta, "title");
    track.artist = getTag(meta, "artist");
    track.album  = getTag(meta, "album");

    QString trackNum = getTag(meta, "track");
    if (!trackNum.isEmpty()) {
        // Handle "3/12" format
        int slashPos = trackNum.indexOf('/');
        track.trackNumber = (slashPos > 0)
            ? trackNum.left(slashPos).toInt()
            : trackNum.toInt();
    }

    QString discNum = getTag(meta, "disc");
    if (!discNum.isEmpty()) {
        int slashPos = discNum.indexOf('/');
        track.discNumber = (slashPos > 0)
            ? discNum.left(slashPos).toInt()
            : discNum.toInt();
    }

    // Fallback: use filename as title
    if (track.title.isEmpty()) {
        track.title = QFileInfo(filePath).completeBaseName();
    }
    if (track.artist.isEmpty()) {
        track.artist = QStringLiteral("Unknown Artist");
    }
    if (track.album.isEmpty()) {
        track.album = QStringLiteral("Unknown Album");
    }

    // Duration
    double durationSecs = 0;
    if (stream->duration != AV_NOPTS_VALUE) {
        durationSecs = stream->duration * av_q2d(stream->time_base);
    } else if (fmtCtx->duration != AV_NOPTS_VALUE) {
        durationSecs = fmtCtx->duration / (double)AV_TIME_BASE;
    }
    track.duration = (int)(durationSecs + 0.5);

    // Audio format info
    track.format = detectFormat(filePath, codecPar->codec_id);

    int sr = codecPar->sample_rate;

    // Refine DSD format based on sample rate (range-based detection)
    if (track.format == AudioFormat::DSD64) {
        if (sr >= 90000000)
            track.format = AudioFormat::DSD2048;
        else if (sr >= 45000000)
            track.format = AudioFormat::DSD1024;
        else if (sr >= 22000000)
            track.format = AudioFormat::DSD512;
        else if (sr >= 11000000)
            track.format = AudioFormat::DSD256;
        else if (sr >= 5600000)
            track.format = AudioFormat::DSD128;
        // else stays DSD64
    }
    bool isDSDFormat = (track.format == AudioFormat::DSD64  || track.format == AudioFormat::DSD128
                     || track.format == AudioFormat::DSD256 || track.format == AudioFormat::DSD512
                     || track.format == AudioFormat::DSD1024 || track.format == AudioFormat::DSD2048);

    if (isDSDFormat && sr >= 1000000) {
        // DSD native rate in MHz (e.g., 2822400 -> "2.8 MHz")
        track.sampleRate = QStringLiteral("%1 MHz").arg(sr / 1000000.0, 0, 'f', 1);
    } else if (sr >= 1000) {
        track.sampleRate = QStringLiteral("%1 kHz").arg(sr / 1000.0, 0, 'f', (sr % 1000 == 0) ? 0 : 1);
    } else {
        track.sampleRate = QStringLiteral("%1 Hz").arg(sr);
    }

    if (isDSDFormat) {
        // DSD is inherently 1-bit; FFmpeg reports internal representation depth
        track.bitDepth = QStringLiteral("1-bit");
    } else {
        int bits = codecPar->bits_per_raw_sample;
        if (bits <= 0) bits = codecPar->bits_per_coded_sample;
        if (bits > 0)
            track.bitDepth = QStringLiteral("%1-bit").arg(bits);
    }

    // Channel count
    track.channelCount = codecPar->ch_layout.nb_channels;
    if (track.channelCount < 1) track.channelCount = 2;

    int64_t br = codecPar->bit_rate;
    if (br <= 0) br = fmtCtx->bit_rate;
    if (br > 0)
        track.bitrate = QStringLiteral("%1 kbps").arg(br / 1000);

    // ── ReplayGain tags ─────────────────────────────────────────────
    {
        AVDictionaryEntry* e = nullptr;
        e = av_dict_get(meta, "REPLAYGAIN_TRACK_GAIN", nullptr, 0);
        if (e) {
            track.replayGainTrack = QString::fromUtf8(e->value).remove(QStringLiteral("dB"), Qt::CaseInsensitive).trimmed().toDouble();
            track.hasReplayGain = true;
        }
        e = av_dict_get(meta, "REPLAYGAIN_ALBUM_GAIN", nullptr, 0);
        if (e) {
            track.replayGainAlbum = QString::fromUtf8(e->value).remove(QStringLiteral("dB"), Qt::CaseInsensitive).trimmed().toDouble();
            if (!track.hasReplayGain) track.hasReplayGain = true;
        }
        e = av_dict_get(meta, "REPLAYGAIN_TRACK_PEAK", nullptr, 0);
        if (e) {
            track.replayGainTrackPeak = QString::fromUtf8(e->value).trimmed().toDouble();
        }
        e = av_dict_get(meta, "REPLAYGAIN_ALBUM_PEAK", nullptr, 0);
        if (e) {
            track.replayGainAlbumPeak = QString::fromUtf8(e->value).trimmed().toDouble();
        }
        // R128 tags (Opus)
        e = av_dict_get(meta, "R128_TRACK_GAIN", nullptr, 0);
        if (e) {
            // R128 tag value is in Q7.8 format: gain = value / 256.0
            track.r128Loudness = -23.0 - (QString::fromUtf8(e->value).trimmed().toInt() / 256.0);
            track.hasR128 = true;
        }
    }

    avformat_close_input(&fmtCtx);
    return track;
}

// ── extractCoverArt ────────────────────────────────────────────────────
QPixmap MetadataReader::extractCoverArt(const QString& filePath)
{
    QPixmap pixmap;

    // DSD containers (DSF/DFF) can crash in avformat_find_stream_info — skip them
    if (filePath.endsWith(QStringLiteral(".dsf"), Qt::CaseInsensitive) ||
        filePath.endsWith(QStringLiteral(".dff"), Qt::CaseInsensitive)) {
        return pixmap;
    }

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) != 0) {
        return pixmap;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return pixmap;
    }

    // Look for attached picture stream (embedded cover art)
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket& pkt = fmtCtx->streams[i]->attached_pic;
            QByteArray data(reinterpret_cast<char*>(pkt.data), pkt.size);
            pixmap.loadFromData(data);
            break;
        }
    }

    avformat_close_input(&fmtCtx);
    return pixmap;
}
