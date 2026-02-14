#include "MetadataFixService.h"
#include "../../widgets/TrackTableView.h"
#include "../../core/MusicData.h"
#include "../../core/library/LibraryDatabase.h"
#include "../../metadata/MetadataService.h"
#include "../../metadata/MusicBrainzProvider.h"
#include "../dialogs/MetadataSearchDialog.h"

MetadataFixService::MetadataFixService(QObject* parent)
    : QObject(parent)
{
}

void MetadataFixService::connectToTable(TrackTableView* table, QWidget* dialogParent)
{
    if (!table) return;

    connect(table, &TrackTableView::fixMetadataRequested,
            this, [this, dialogParent](const Track& t) {
        auto* dlg = new MetadataSearchDialog(t, dialogParent);
        connect(dlg, &QDialog::accepted, this, [this, dlg, t]() {
            MusicBrainzResult result = dlg->selectedResult();
            Track updated = t;
            if (!result.title.isEmpty())  updated.title  = result.title;
            if (!result.artist.isEmpty()) updated.artist = result.artist;
            if (!result.album.isEmpty())  updated.album  = result.album;
            if (result.trackNumber > 0)   updated.trackNumber = result.trackNumber;
            if (result.discNumber > 0)    updated.discNumber  = result.discNumber;
            if (!result.mbid.isEmpty())             updated.recordingMbid    = result.mbid;
            if (!result.artistMbid.isEmpty())       updated.artistMbid       = result.artistMbid;
            if (!result.albumMbid.isEmpty())        updated.albumMbid        = result.albumMbid;
            if (!result.releaseGroupMbid.isEmpty()) updated.releaseGroupMbid = result.releaseGroupMbid;

            auto* db = LibraryDatabase::instance();
            db->backupTrackMetadata(t.id);
            db->updateTrack(updated);
            db->updateAlbumsAndArtistsForTrack(updated);

            if (!result.releaseGroupMbid.isEmpty())
                MetadataService::instance()->fetchAlbumArt(result.releaseGroupMbid, true);
            else if (!result.albumMbid.isEmpty())
                MetadataService::instance()->fetchAlbumArt(result.albumMbid, false);
            if (!result.artistMbid.isEmpty())
                MetadataService::instance()->fetchArtistImages(result.artistMbid);

            MusicDataProvider::instance()->reloadFromDatabase();
        });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });

    connect(table, &TrackTableView::undoMetadataRequested,
            this, [](const Track& t) {
        auto* db = LibraryDatabase::instance();
        auto fresh = db->trackById(t.id);
        if (fresh.has_value())
            db->updateAlbumsAndArtistsForTrack(fresh.value());
        MusicDataProvider::instance()->reloadFromDatabase();
    });

    connect(table, &TrackTableView::identifyByAudioRequested,
            this, [](const Track& t) {
        MetadataService::instance()->identifyByFingerprint(t);
    });
}

void MetadataFixService::disconnectFromTable(TrackTableView* table)
{
    if (!table) return;
    disconnect(table, &TrackTableView::fixMetadataRequested, this, nullptr);
    disconnect(table, &TrackTableView::undoMetadataRequested, this, nullptr);
    disconnect(table, &TrackTableView::identifyByAudioRequested, this, nullptr);
}
