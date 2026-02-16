#include "AMAlbumPanel.h"

AMAlbumPanel::AMAlbumPanel(QWidget* parent)
    : AMContentPanel(parent)
{
}

void AMAlbumPanel::setTracks(const QString& albumName, const QString& artistName,
                             const QJsonArray& tracks)
{
    clear();

    buildSongsSection(
        QStringLiteral("%1 \u2014 %2 (%3)").arg(albumName, artistName).arg(tracks.size()),
        tracks);

    m_resultsLayout->addStretch();
}
