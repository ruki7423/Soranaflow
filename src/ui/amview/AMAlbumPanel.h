#pragma once

#include "AMContentPanel.h"

class AMAlbumPanel : public AMContentPanel {
    Q_OBJECT
public:
    explicit AMAlbumPanel(QWidget* parent = nullptr);
    void setTracks(const QString& albumName, const QString& artistName, const QJsonArray& tracks);
};
