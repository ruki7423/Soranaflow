#include "AMSearchPanel.h"

AMSearchPanel::AMSearchPanel(QWidget* parent)
    : AMContentPanel(parent)
{
}

void AMSearchPanel::setResults(const QJsonArray& songs, const QJsonArray& albums,
                               const QJsonArray& artists)
{
    clear();

    if (!songs.isEmpty())
        buildSongsSection(QStringLiteral("Songs (%1)").arg(songs.size()), songs);
    if (!albums.isEmpty())
        buildAlbumsGrid(QStringLiteral("Albums (%1)").arg(albums.size()), albums);
    if (!artists.isEmpty())
        buildArtistsGrid(QStringLiteral("Artists (%1)").arg(artists.size()), artists);

    m_resultsLayout->addStretch();
}
