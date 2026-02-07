#include "StyledSwitch.h"
#include "../core/ThemeManager.h"
#include <QPainter>
#include <QMouseEvent>

StyledSwitch::StyledSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setFixedSize(UISizes::switchWidth, UISizes::switchHeight);
    setCursor(Qt::PointingHandCursor);
    setObjectName(QStringLiteral("StyledSwitch"));
}

QSize StyledSwitch::sizeHint() const
{
    return QSize(UISizes::switchWidth, UISizes::switchHeight);
}

void StyledSwitch::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Background rounded rect
    auto c = ThemeManager::instance()->colors();
    const QColor bgColor = isChecked() ? QColor(c.accent)
                                       : QColor(c.backgroundTertiary);
    p.setPen(Qt::NoPen);
    p.setBrush(bgColor);
    p.drawRoundedRect(rect(), height() / 2.0, height() / 2.0);

    // White circle thumb (20px diameter)
    const int thumbDiameter = 20;
    const int margin = (height() - thumbDiameter) / 2;
    const int thumbX = isChecked() ? (width() - thumbDiameter - margin) : margin;
    const int thumbY = margin;

    p.setBrush(Qt::white);
    p.drawEllipse(thumbX, thumbY, thumbDiameter, thumbDiameter);
}

void StyledSwitch::mouseReleaseEvent(QMouseEvent* event)
{
    // Let QAbstractButton handle toggle + signal emission.
    // Do NOT call toggle() manually — the base class already does it
    // for checkable buttons, and double-toggling cancels itself out.
    QAbstractButton::mouseReleaseEvent(event);
    update(); // repaint to show new checked state
}
