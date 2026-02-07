#include "StyledSlider.h"
#include "../core/ThemeManager.h"
#include <QEnterEvent>
#include <QStyle>

StyledSlider::StyledSlider(QWidget* parent)
    : QSlider(Qt::Horizontal, parent)
    , m_showHandle(false)
{
    setObjectName(QStringLiteral("StyledSlider"));
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(20);
    setProperty("showHandle", false);
}

bool StyledSlider::showHandle() const
{
    return m_showHandle;
}

void StyledSlider::setShowHandle(bool show)
{
    if (m_showHandle == show)
        return;
    m_showHandle = show;
    setProperty("showHandle", show);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void StyledSlider::enterEvent(QEnterEvent* event)
{
    setShowHandle(true);
    QSlider::enterEvent(event);
}

void StyledSlider::leaveEvent(QEvent* event)
{
    setShowHandle(false);
    QSlider::leaveEvent(event);
}
