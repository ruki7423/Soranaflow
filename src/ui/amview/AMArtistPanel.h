#pragma once

#include "AMContentPanel.h"

class AMArtistPanel : public AMContentPanel {
    Q_OBJECT
public:
    explicit AMArtistPanel(QWidget* parent = nullptr);
    void setSongs(const QString& artistName, const QJsonArray& songs);
    void setAlbums(const QJsonArray& albums);

private:
    void rebuild();
    QString m_artistName;
    QJsonArray m_songs;
    QJsonArray m_albums;
};
