#include "MetadataReader.h"

#include <QFileInfo>
#include <QUuid>
#include <QDebug>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/audioproperties.h>

#include <taglib/flacfile.h>
#include <taglib/flacproperties.h>
#include <taglib/wavfile.h>
#include <taglib/wavproperties.h>
#include <taglib/aifffile.h>
#include <taglib/aiffproperties.h>
#include <taglib/mpegfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4properties.h>
#include <taglib/dsffile.h>
#include <taglib/dsfproperties.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsdiffproperties.h>
#include <taglib/opusfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/apefile.h>
#include <taglib/apeproperties.h>
#include <taglib/wavpackfile.h>
#include <taglib/wavpackproperties.h>

// Cover art extraction
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacpicture.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>

static AudioFormat detectFormat(const QString& ext, int bitsPerSample)
{
    if (ext == "dsf" || ext == "dff")
        return AudioFormat::DSD64;  // refined below by sample rate
    if (ext == "flac")  return AudioFormat::FLAC;
    if (ext == "m4a") {
        // ALAC files report actual bit depth (16/24/32); AAC reports 0
        return (bitsPerSample > 0) ? AudioFormat::ALAC : AudioFormat::AAC;
    }
    if (ext == "alac")  return AudioFormat::ALAC;
    if (ext == "wav")   return AudioFormat::WAV;
    if (ext == "mp3")   return AudioFormat::MP3;
    if (ext == "aac")   return AudioFormat::AAC;
    if (ext == "ogg")   return AudioFormat::AAC;  // OGG Vorbis → AAC bucket
    if (ext == "opus")  return AudioFormat::AAC;
    if (ext == "ape")   return AudioFormat::FLAC;  // APE → lossless bucket
    if (ext == "wv")    return AudioFormat::FLAC;  // WavPack → lossless bucket
    if (ext == "aif" || ext == "aiff") return AudioFormat::WAV;
    return AudioFormat::FLAC;
}

std::optional<Track> MetadataReader::readTrack(const QString& filePath)
{
    TagLib::FileRef f(filePath.toUtf8().constData());
    if (f.isNull())
        return std::nullopt;

    Track track;
    track.id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    track.filePath = filePath;

    // ── Basic tags ──────────────────────────────────────────────────
    if (TagLib::Tag* tag = f.tag()) {
        track.title  = QString::fromStdString(tag->title().to8Bit(true));
        track.artist = QString::fromStdString(tag->artist().to8Bit(true));
        track.album  = QString::fromStdString(tag->album().to8Bit(true));
        track.trackNumber = static_cast<int>(tag->track());
    }

    // ── Extended tags via PropertyMap ────────────────────────────────
    TagLib::PropertyMap props = f.file()->properties();

    if (props.contains("DISCNUMBER")) {
        auto vals = props["DISCNUMBER"];
        if (!vals.isEmpty()) {
            QString dn = QString::fromStdString(vals.front().to8Bit(true));
            int slashPos = dn.indexOf('/');
            track.discNumber = (slashPos > 0) ? dn.left(slashPos).toInt() : dn.toInt();
        }
    }

    // ReplayGain
    if (props.contains("REPLAYGAIN_TRACK_GAIN")) {
        auto vals = props["REPLAYGAIN_TRACK_GAIN"];
        if (!vals.isEmpty()) {
            track.replayGainTrack = QString::fromStdString(vals.front().to8Bit(true))
                .remove(QStringLiteral("dB"), Qt::CaseInsensitive).trimmed().toDouble();
            track.hasReplayGain = true;
        }
    }
    if (props.contains("REPLAYGAIN_ALBUM_GAIN")) {
        auto vals = props["REPLAYGAIN_ALBUM_GAIN"];
        if (!vals.isEmpty()) {
            track.replayGainAlbum = QString::fromStdString(vals.front().to8Bit(true))
                .remove(QStringLiteral("dB"), Qt::CaseInsensitive).trimmed().toDouble();
            if (!track.hasReplayGain) track.hasReplayGain = true;
        }
    }
    if (props.contains("REPLAYGAIN_TRACK_PEAK")) {
        auto vals = props["REPLAYGAIN_TRACK_PEAK"];
        if (!vals.isEmpty())
            track.replayGainTrackPeak = QString::fromStdString(vals.front().to8Bit(true)).trimmed().toDouble();
    }
    if (props.contains("REPLAYGAIN_ALBUM_PEAK")) {
        auto vals = props["REPLAYGAIN_ALBUM_PEAK"];
        if (!vals.isEmpty())
            track.replayGainAlbumPeak = QString::fromStdString(vals.front().to8Bit(true)).trimmed().toDouble();
    }
    // R128 tags (Opus)
    if (props.contains("R128_TRACK_GAIN")) {
        auto vals = props["R128_TRACK_GAIN"];
        if (!vals.isEmpty()) {
            int r128val = QString::fromStdString(vals.front().to8Bit(true)).trimmed().toInt();
            track.r128Loudness = -23.0 - (r128val / 256.0);
            track.hasR128 = true;
        }
    }

    // ── Fallbacks ───────────────────────────────────────────────────
    if (track.title.isEmpty())
        track.title = QFileInfo(filePath).completeBaseName();
    if (track.artist.isEmpty())
        track.artist = QStringLiteral("Unknown Artist");
    if (track.album.isEmpty())
        track.album = QStringLiteral("Unknown Album");

    // ── Audio properties ────────────────────────────────────────────
    TagLib::AudioProperties* ap = f.audioProperties();
    if (!ap)
        return std::nullopt;

    track.duration    = ap->lengthInSeconds();
    int sr            = ap->sampleRate();
    track.channelCount = ap->channels();
    if (track.channelCount < 1) track.channelCount = 2;

    int br = ap->bitrate();  // already in kbps
    if (br > 0)
        track.bitrate = QStringLiteral("%1 kbps").arg(br);

    // ── Bit depth via format-specific dynamic_cast ──────────────────
    int bitsPerSample = 0;
    TagLib::File* file = f.file();

    if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file))
        bitsPerSample = flac->audioProperties()->bitsPerSample();
    else if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(file))
        bitsPerSample = wav->audioProperties()->bitsPerSample();
    else if (auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(file))
        bitsPerSample = aiff->audioProperties()->bitsPerSample();
    else if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file))
        bitsPerSample = mp4->audioProperties()->bitsPerSample();
    else if (auto* dsf = dynamic_cast<TagLib::DSF::File*>(file))
        bitsPerSample = dsf->audioProperties()->bitsPerSample();
    else if (auto* dsdiff = dynamic_cast<TagLib::DSDIFF::File*>(file))
        bitsPerSample = dsdiff->audioProperties()->bitsPerSample();
    else if (auto* ape = dynamic_cast<TagLib::APE::File*>(file))
        bitsPerSample = ape->audioProperties()->bitsPerSample();
    else if (auto* wv = dynamic_cast<TagLib::WavPack::File*>(file))
        bitsPerSample = wv->audioProperties()->bitsPerSample();
    // MP3, OGG, Opus: no bitsPerSample — leave 0

    // ── Format detection ────────────────────────────────────────────
    QString ext = QFileInfo(filePath).suffix().toLower();
    track.format = detectFormat(ext, bitsPerSample);

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

    // ── Sample rate string ──────────────────────────────────────────
    if (isDSDFormat && sr >= 1000000) {
        track.sampleRate = QStringLiteral("%1 MHz").arg(sr / 1000000.0, 0, 'f', 1);
    } else if (sr >= 1000) {
        track.sampleRate = QStringLiteral("%1 kHz").arg(sr / 1000.0, 0, 'f', (sr % 1000 == 0) ? 0 : 1);
    } else {
        track.sampleRate = QStringLiteral("%1 Hz").arg(sr);
    }

    // ── Bit depth string ────────────────────────────────────────────
    if (isDSDFormat) {
        track.bitDepth = QStringLiteral("1-bit");
    } else if (bitsPerSample > 0) {
        track.bitDepth = QStringLiteral("%1-bit").arg(bitsPerSample);
    }

    return track;
}

// ── extractCoverArt ────────────────────────────────────────────────────
QPixmap MetadataReader::extractCoverArt(const QString& filePath)
{
    QPixmap pixmap;
    QFileInfo fi(filePath);
    QString ext = fi.suffix().toLower();

    // ── MP3 — ID3v2 APIC ────────────────────────────────────────────
    if (ext == "mp3") {
        TagLib::MPEG::File mpegFile(filePath.toUtf8().constData());
        auto* id3v2 = mpegFile.ID3v2Tag();
        if (id3v2) {
            auto frames = id3v2->frameList("APIC");
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    pixmap.loadFromData(
                        reinterpret_cast<const uchar*>(pic->picture().data()),
                        pic->picture().size());
                    return pixmap;
                }
            }
        }
    }
    // ── FLAC — pictureList ──────────────────────────────────────────
    else if (ext == "flac") {
        TagLib::FLAC::File flacFile(filePath.toUtf8().constData());
        auto pics = flacFile.pictureList();
        if (!pics.isEmpty()) {
            auto* pic = pics.front();
            pixmap.loadFromData(
                reinterpret_cast<const uchar*>(pic->data().data()),
                pic->data().size());
            return pixmap;
        }
    }
    // ── M4A/AAC/MP4 — covr ─────────────────────────────────────────
    else if (ext == "m4a" || ext == "aac" || ext == "mp4") {
        TagLib::MP4::File mp4File(filePath.toUtf8().constData());
        auto* mp4Tag = mp4File.tag();
        if (mp4Tag && mp4Tag->contains("covr")) {
            auto coverList = mp4Tag->item("covr").toCoverArtList();
            if (!coverList.isEmpty()) {
                auto& cover = coverList.front();
                pixmap.loadFromData(
                    reinterpret_cast<const uchar*>(cover.data().data()),
                    cover.data().size());
                return pixmap;
            }
        }
    }
    // ── WAV — ID3v2 ─────────────────────────────────────────────────
    else if (ext == "wav") {
        TagLib::RIFF::WAV::File wavFile(filePath.toUtf8().constData());
        auto* id3v2 = wavFile.ID3v2Tag();
        if (id3v2) {
            auto frames = id3v2->frameList("APIC");
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    pixmap.loadFromData(
                        reinterpret_cast<const uchar*>(pic->picture().data()),
                        pic->picture().size());
                    return pixmap;
                }
            }
        }
    }
    // ── AIFF — ID3v2 ───────────────────────────────────────────────
    else if (ext == "aif" || ext == "aiff") {
        TagLib::RIFF::AIFF::File aiffFile(filePath.toUtf8().constData());
        auto* id3v2 = aiffFile.tag();
        if (id3v2) {
            auto frames = id3v2->frameList("APIC");
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    pixmap.loadFromData(
                        reinterpret_cast<const uchar*>(pic->picture().data()),
                        pic->picture().size());
                    return pixmap;
                }
            }
        }
    }
    // ── DSF — ID3v2 ─────────────────────────────────────────────────
    else if (ext == "dsf") {
        TagLib::DSF::File dsfFile(filePath.toUtf8().constData());
        auto* id3v2 = dsfFile.tag();
        if (id3v2) {
            auto frames = id3v2->frameList("APIC");
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    pixmap.loadFromData(
                        reinterpret_cast<const uchar*>(pic->picture().data()),
                        pic->picture().size());
                    return pixmap;
                }
            }
        }
    }
    // ── DSDIFF (DFF) — no standard cover art container ──────────────
    // DFF files don't have a standard tag container for embedded art

    return pixmap;
}
