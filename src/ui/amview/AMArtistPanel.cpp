#include "AMArtistPanel.h"

AMArtistPanel::AMArtistPanel(QWidget* parent)
    : AMContentPanel(parent)
{
}

void AMArtistPanel::setSongs(const QString& artistName, const QJsonArray& songs)
{
    m_artistName = artistName;
    m_songs = songs;
    rebuild();
}

void AMArtistPanel::setAlbums(const QJsonArray& albums)
{
    m_albums = albums;
    rebuild();
}

void AMArtistPanel::rebuild()
{
    clear();

    if (!m_songs.isEmpty())
        buildSongsSection(
            QStringLiteral("Songs by %1 (%2)").arg(m_artistName).arg(m_songs.size()),
            m_songs);

    if (!m_albums.isEmpty())
        buildAlbumsGrid(
            QStringLiteral("Albums (%1)").arg(m_albums.size()),
            m_albums);

    m_resultsLayout->addStretch();
}
