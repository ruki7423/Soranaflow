#include "StyledScrollArea.h"
#include <QScrollBar>

StyledScrollArea::StyledScrollArea(QWidget* parent)
    : QScrollArea(parent)
{
    setObjectName(QStringLiteral("StyledScrollArea"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}
