#pragma once

#include <QScrollArea>

class StyledScrollArea : public QScrollArea
{
    Q_OBJECT

public:
    explicit StyledScrollArea(QWidget* parent = nullptr);
};
