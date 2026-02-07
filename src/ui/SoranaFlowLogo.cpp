#include "SoranaFlowLogo.h"

#include <QPainterPath>
#include <QLinearGradient>

// ── Constructor ─────────────────────────────────────────────────────
SoranaFlowLogo::SoranaFlowLogo(int size, QWidget* parent)
    : QWidget(parent)
    , m_size(size)
    , m_renderer(QStringLiteral(":/icons/sorana-logo.svg"))
{
    setFixedSize(m_size, m_size);
}

// ── sizeHint ────────────────────────────────────────────────────────
QSize SoranaFlowLogo::sizeHint() const
{
    return QSize(m_size, m_size);
}

// ── paintEvent ──────────────────────────────────────────────────────
void SoranaFlowLogo::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (m_renderer.isValid()) {
        m_renderer.render(&painter, QRectF(0, 0, width(), height()));
    } else {
        // Fallback: procedural drawing if SVG fails to load
        const qreal w = width();
        const qreal h = height();

        QLinearGradient gradient(0, 0, w, h);
        gradient.setColorAt(0.0, QColor(0x22, 0xD3, 0xEE));
        gradient.setColorAt(0.5, QColor(0x3B, 0x82, 0xF6));
        gradient.setColorAt(1.0, QColor(0x8B, 0x5C, 0xF6));

        const qreal cornerRadius = w * 0.22;
        QPainterPath bgPath;
        bgPath.addRoundedRect(QRectF(0, 0, w, h), cornerRadius, cornerRadius);

        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawPath(bgPath);

        // Play triangle
        const qreal triSize = w * 0.22;
        const qreal triX = w * 0.25;
        const qreal triY = h * 0.5;

        QPainterPath triPath;
        triPath.moveTo(triX, triY - triSize * 0.5);
        triPath.lineTo(triX + triSize * 0.85, triY);
        triPath.lineTo(triX, triY + triSize * 0.5);
        triPath.closeSubpath();

        painter.setBrush(QColor(255, 255, 255, 230));
        painter.drawPath(triPath);

        // Sound wave arcs
        painter.setBrush(Qt::NoBrush);
        const qreal cx = w * 0.55;

        QPen wavePen(QColor(255, 255, 255, 230), w * 0.05, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(wavePen);
        painter.drawArc(QRectF(cx - w * 0.12, h * 0.34, w * 0.32, h * 0.32), 45 * 16, -90 * 16);

        wavePen.setColor(QColor(255, 255, 255, 180));
        painter.setPen(wavePen);
        painter.drawArc(QRectF(cx - w * 0.08, h * 0.24, w * 0.42, h * 0.52), 50 * 16, -100 * 16);

        wavePen.setColor(QColor(255, 255, 255, 115));
        wavePen.setWidthF(w * 0.04);
        painter.setPen(wavePen);
        painter.drawArc(QRectF(cx - w * 0.04, h * 0.16, w * 0.52, h * 0.68), 55 * 16, -110 * 16);
    }
}
