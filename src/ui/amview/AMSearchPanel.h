#pragma once

#include "AMContentPanel.h"

class AMSearchPanel : public AMContentPanel {
    Q_OBJECT
public:
    explicit AMSearchPanel(QWidget* parent = nullptr);
    void setResults(const QJsonArray& songs, const QJsonArray& albums, const QJsonArray& artists);
};
