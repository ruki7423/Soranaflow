#include "TagWriter.h"

#include <QBuffer>
#include <QDebug>
#include <QFileInfo>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
#include <taglib/dsffile.h>
#include <taglib/dsdifffile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacpicture.h>
#include <taglib/xiphcomment.h>

// ═══════════════════════════════════════════════════════════════════════
//  readTags
// ═══════════════════════════════════════════════════════════════════════

bool TagWriter::readTags(const QString& filePath, TrackMetadata& meta)
{
    TagLib::FileRef f(filePath.toUtf8().constData());
    if (f.isNull() || !f.tag())
        return false;

    TagLib::Tag* tag = f.tag();
    meta.title = QString::fromStdString(tag->title().to8Bit(true));
    meta.artist = QString::fromStdString(tag->artist().to8Bit(true));
    meta.album = QString::fromStdString(tag->album().to8Bit(true));
    meta.year = tag->year();
    meta.trackNumber = tag->track();
    meta.genre = QString::fromStdString(tag->genre().to8Bit(true));
    meta.comment = QString::fromStdString(tag->comment().to8Bit(true));

    // Extended properties
    TagLib::PropertyMap props = f.file()->properties();
    if (props.contains("ALBUMARTIST")) {
        auto vals = props["ALBUMARTIST"];
        if (!vals.isEmpty())
            meta.albumArtist = QString::fromStdString(vals.front().to8Bit(true));
    }
    if (props.contains("COMPOSER")) {
        auto vals = props["COMPOSER"];
        if (!vals.isEmpty())
            meta.composer = QString::fromStdString(vals.front().to8Bit(true));
    }
    if (props.contains("DISCNUMBER")) {
        auto vals = props["DISCNUMBER"];
        if (!vals.isEmpty())
            meta.discNumber = QString::fromStdString(vals.front().to8Bit(true)).toInt();
    }

    // Read album art
    meta.albumArt = readAlbumArt(filePath);

    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  writeTags
// ═══════════════════════════════════════════════════════════════════════

bool TagWriter::writeTags(const QString& filePath, const TrackMetadata& meta)
{
    TagLib::FileRef f(filePath.toUtf8().constData());
    if (f.isNull() || !f.tag())
        return false;

    TagLib::Tag* tag = f.tag();
    tag->setTitle(TagLib::String(meta.title.toStdString(), TagLib::String::UTF8));
    tag->setArtist(TagLib::String(meta.artist.toStdString(), TagLib::String::UTF8));
    tag->setAlbum(TagLib::String(meta.album.toStdString(), TagLib::String::UTF8));
    tag->setYear(meta.year);
    tag->setTrack(meta.trackNumber);
    tag->setGenre(TagLib::String(meta.genre.toStdString(), TagLib::String::UTF8));
    tag->setComment(TagLib::String(meta.comment.toStdString(), TagLib::String::UTF8));

    // Extended properties via PropertyMap
    TagLib::PropertyMap props = f.file()->properties();
    props["ALBUMARTIST"] = TagLib::StringList(
        TagLib::String(meta.albumArtist.toStdString(), TagLib::String::UTF8));
    props["COMPOSER"] = TagLib::StringList(
        TagLib::String(meta.composer.toStdString(), TagLib::String::UTF8));
    props["DISCNUMBER"] = TagLib::StringList(
        TagLib::String(QString::number(meta.discNumber).toStdString(), TagLib::String::UTF8));
    f.file()->setProperties(props);

    return f.save();
}

// ═══════════════════════════════════════════════════════════════════════
//  readAlbumArt
// ═══════════════════════════════════════════════════════════════════════

QImage TagWriter::readAlbumArt(const QString& filePath)
{
    QFileInfo fi(filePath);
    QString ext = fi.suffix().toLower();

    // MPEG (MP3) — ID3v2
    if (ext == "mp3") {
        TagLib::MPEG::File mpegFile(filePath.toUtf8().constData());
        auto* id3v2 = mpegFile.ID3v2Tag();
        if (id3v2) {
            auto frames = id3v2->frameList("APIC");
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    QImage img;
                    img.loadFromData(
                        reinterpret_cast<const uchar*>(pic->picture().data()),
                        pic->picture().size());
                    return img;
                }
            }
        }
    }
    // FLAC — embedded picture
    else if (ext == "flac") {
        TagLib::FLAC::File flacFile(filePath.toUtf8().constData());
        auto pics = flacFile.pictureList();
        if (!pics.isEmpty()) {
            auto* pic = pics.front();
            QImage img;
            img.loadFromData(
                reinterpret_cast<const uchar*>(pic->data().data()),
                pic->data().size());
            return img;
        }
    }
    // M4A/AAC — MP4 cover art
    else if (ext == "m4a" || ext == "aac" || ext == "mp4") {
        TagLib::MP4::File mp4File(filePath.toUtf8().constData());
        auto* mp4Tag = mp4File.tag();
        if (mp4Tag && mp4Tag->contains("covr")) {
            auto coverList = mp4Tag->item("covr").toCoverArtList();
            if (!coverList.isEmpty()) {
                auto& cover = coverList.front();
                QImage img;
                img.loadFromData(
                    reinterpret_cast<const uchar*>(cover.data().data()),
                    cover.data().size());
                return img;
            }
        }
    }
    // WAV — ID3v2 tag
    else if (ext == "wav") {
        TagLib::RIFF::WAV::File wavFile(filePath.toUtf8().constData());
        auto* id3v2 = wavFile.ID3v2Tag();
        if (id3v2) {
            auto frames = id3v2->frameList("APIC");
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    QImage img;
                    img.loadFromData(
                        reinterpret_cast<const uchar*>(pic->picture().data()),
                        pic->picture().size());
                    return img;
                }
            }
        }
    }

    return {};
}

// ═══════════════════════════════════════════════════════════════════════
//  writeAlbumArt
// ═══════════════════════════════════════════════════════════════════════

bool TagWriter::writeAlbumArt(const QString& filePath, const QImage& image)
{
    if (image.isNull())
        return false;

    // Convert QImage to JPEG bytes
    QByteArray jpegData;
    {
        QBuffer buffer(&jpegData);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPEG", 90);
    }

    TagLib::ByteVector artData(jpegData.constData(), jpegData.size());

    QFileInfo fi(filePath);
    QString ext = fi.suffix().toLower();

    // MPEG (MP3) — ID3v2
    if (ext == "mp3") {
        TagLib::MPEG::File mpegFile(filePath.toUtf8().constData());
        auto* id3v2 = mpegFile.ID3v2Tag(true);
        if (!id3v2) return false;

        // Remove existing APIC frames
        id3v2->removeFrames("APIC");

        auto* picFrame = new TagLib::ID3v2::AttachedPictureFrame();
        picFrame->setMimeType("image/jpeg");
        picFrame->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
        picFrame->setPicture(artData);
        id3v2->addFrame(picFrame);

        return mpegFile.save();
    }
    // FLAC
    else if (ext == "flac") {
        TagLib::FLAC::File flacFile(filePath.toUtf8().constData());

        // Remove existing pictures
        flacFile.removePictures();

        auto* pic = new TagLib::FLAC::Picture();
        pic->setMimeType("image/jpeg");
        pic->setType(TagLib::FLAC::Picture::FrontCover);
        pic->setData(artData);
        flacFile.addPicture(pic);

        return flacFile.save();
    }
    // M4A/AAC
    else if (ext == "m4a" || ext == "aac" || ext == "mp4") {
        TagLib::MP4::File mp4File(filePath.toUtf8().constData());
        auto* mp4Tag = mp4File.tag();
        if (!mp4Tag) return false;

        TagLib::MP4::CoverArt cover(TagLib::MP4::CoverArt::JPEG, artData);
        TagLib::MP4::CoverArtList coverList;
        coverList.append(cover);
        mp4Tag->setItem("covr", TagLib::MP4::Item(coverList));

        return mp4File.save();
    }
    // WAV
    else if (ext == "wav") {
        TagLib::RIFF::WAV::File wavFile(filePath.toUtf8().constData());
        auto* id3v2 = wavFile.ID3v2Tag();
        if (!id3v2) return false;

        id3v2->removeFrames("APIC");

        auto* picFrame = new TagLib::ID3v2::AttachedPictureFrame();
        picFrame->setMimeType("image/jpeg");
        picFrame->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
        picFrame->setPicture(artData);
        id3v2->addFrame(picFrame);

        return wavFile.save();
    }

    return false;
}
