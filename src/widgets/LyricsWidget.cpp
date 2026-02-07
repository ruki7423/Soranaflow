#include "LyricsWidget.h"
#include "../core/ThemeManager.h"

#include <QPainter>
#include <QMouseEvent>
#include <QFontMetrics>
#include <QEasingCurve>
#include <QDebug>

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

LyricsWidget::LyricsWidget(QWidget* parent)
    : QWidget(parent)
    , m_scrollAnim(new QPropertyAnimation(this, "scrollOffset", this))
{
    m_scrollAnim->setDuration(300);
    m_scrollAnim->setEasingCurve(QEasingCurve::OutCubic);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, false);

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, QOverload<>::of(&QWidget::update));
}

// ═════════════════════════════════════════════════════════════════════
//  sizeHint / minimumSizeHint — prevent layout jump when shown
// ═════════════════════════════════════════════════════════════════════

QSize LyricsWidget::sizeHint() const
{
    return QSize(400, 500);
}

QSize LyricsWidget::minimumSizeHint() const
{
    return QSize(200, 250);
}

// ═════════════════════════════════════════════════════════════════════
//  setLyrics
// ═════════════════════════════════════════════════════════════════════

void LyricsWidget::setLyrics(const QList<LyricLine>& lyrics, bool synced)
{
    // Stop animation FIRST — stop() may fire setScrollOffset() with old end value
    m_scrollAnim->stop();
    m_currentLine = -1;
    m_scrollOffset = 0.0;
    m_lineLayouts.clear();

    // Now safe to replace data
    m_lyrics = lyrics;
    m_synced = synced;
    m_layoutDirty = true;

    // Highlight first line immediately so the widget looks ready before playback
    if (m_synced && !m_lyrics.isEmpty())
        m_currentLine = 0;
    qDebug() << "[Lyrics] setLyrics:" << m_lyrics.size() << "lines,"
             << "synced:" << m_synced;
    update();
}

// ═════════════════════════════════════════════════════════════════════
//  setPosition — called from AudioEngine position updates
// ═════════════════════════════════════════════════════════════════════

void LyricsWidget::setPosition(double seconds)
{
    if (!m_synced || m_lyrics.isEmpty() || m_lineLayouts.isEmpty())
        return;

    qint64 posMs = static_cast<qint64>(seconds * 1000.0);
    int newLine = findCurrentLine(posMs);

    if (newLine == m_currentLine)
        return;

    // Bounds check before using as index
    if (newLine < 0 || newLine >= m_lyrics.size() || newLine >= m_lineLayouts.size()) {
        m_currentLine = -1;
        update();
        return;
    }

    int oldLine = m_currentLine;
    m_currentLine = newLine;

    // Animate scroll to center the current line
    double targetY = m_lineLayouts[m_currentLine].y
                   + m_lineLayouts[m_currentLine].height / 2.0
                   - height() / 2.0;
    if (targetY < 0) targetY = 0;

    m_scrollAnim->stop();
    m_scrollAnim->setStartValue(m_scrollOffset);
    m_scrollAnim->setEndValue(targetY);
    m_scrollAnim->start();

    qDebug() << "[Lyrics] Scroll to line" << newLine
             << "from" << oldLine
             << "offset:" << m_scrollOffset << "->" << targetY
             << "height:" << height();

    update();
}

void LyricsWidget::clear()
{
    // Stop animation FIRST to prevent stale callbacks
    m_scrollAnim->stop();
    m_currentLine = -1;
    m_scrollOffset = 0.0;
    m_lineLayouts.clear();

    // Now safe to clear data
    m_lyrics.clear();
    m_synced = false;
    update();
}

// ═════════════════════════════════════════════════════════════════════
//  findCurrentLine — binary search for the active lyric line
// ═════════════════════════════════════════════════════════════════════

int LyricsWidget::findCurrentLine(qint64 positionMs) const
{
    if (m_lyrics.isEmpty())
        return -1;

    // Binary search: find last line with timestampMs <= positionMs
    int lo = 0, hi = m_lyrics.size() - 1;
    int result = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m_lyrics[mid].timestampMs <= positionMs) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════
//  recalcLayout — compute y positions for each line
// ═════════════════════════════════════════════════════════════════════

void LyricsWidget::recalcLayout()
{
    m_lineLayouts.clear();
    if (m_lyrics.isEmpty())
        return;

    // Use the largest font (current line font) for all height calculations
    // so that layout stays stable when m_currentLine changes.
    QFont maxFont = font();
    maxFont.setPixelSize(18);
    maxFont.setBold(true);
    QFontMetrics fm(maxFont);

    int availWidth = qMax(width() - 32, 100);  // 16px padding each side
    double y = 0;
    const double lineSpacing = 4.0;

    for (int i = 0; i < m_lyrics.size(); ++i) {
        QRect br = fm.boundingRect(
            QRect(0, 0, availWidth, 10000),
            Qt::AlignHCenter | Qt::TextWordWrap,
            m_lyrics[i].text);
        double lineH = br.height();
        m_lineLayouts.append({y, lineH});
        y += lineH + lineSpacing;
    }
}

// ═════════════════════════════════════════════════════════════════════
//  setScrollOffset
// ═════════════════════════════════════════════════════════════════════

void LyricsWidget::setScrollOffset(double offset)
{
    m_scrollOffset = offset;
    update();
}

// ═════════════════════════════════════════════════════════════════════
//  paintEvent
// ═════════════════════════════════════════════════════════════════════

void LyricsWidget::paintEvent(QPaintEvent*)
{
    // Recalculate layout on first paint or when width changed
    if (m_layoutDirty || m_lastLayoutWidth != width()) {
        recalcLayout();
        m_layoutDirty = false;
        m_lastLayoutWidth = width();

        // Re-center current line after layout recalculation
        if (m_currentLine >= 0 && m_currentLine < m_lineLayouts.size()) {
            double targetY = m_lineLayouts[m_currentLine].y
                           + m_lineLayouts[m_currentLine].height / 2.0
                           - height() / 2.0;
            if (targetY < 0) targetY = 0;
            m_scrollOffset = targetY;
        }
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    if (m_lyrics.isEmpty()) {
        // "No lyrics available" message
        auto c = ThemeManager::instance()->colors();
        QFont f = font();
        f.setPixelSize(14);
        p.setFont(f);
        p.setPen(ThemeColors::toQColor(c.foregroundMuted));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No lyrics available"));
        return;
    }

    auto c = ThemeManager::instance()->colors();
    bool dark = ThemeManager::instance()->isDark();
    int padding = 16;
    int availWidth = width() - padding * 2;

    // Theme-aware text colors — use explicit RGB to avoid rgba() parsing issues
    QColor currentColor  = dark ? QColor(255, 255, 255)      : QColor(0, 0, 0);
    QColor adjacentColor = dark ? QColor(255, 255, 255, 153)  : QColor(0, 0, 0, 153);
    QColor dimColor      = dark ? QColor(255, 255, 255, 77)   : QColor(0, 0, 0, 77);
    QColor bgColor       = ThemeColors::toQColor(c.background);

    // Font definitions
    QFont currentFont = font();
    currentFont.setPixelSize(18);
    currentFont.setBold(true);

    QFont adjacentFont = font();
    adjacentFont.setPixelSize(16);
    adjacentFont.setBold(false);

    QFont normalFont = font();
    normalFont.setPixelSize(14);
    normalFont.setBold(false);

    // If unsynced, show all lines statically without scrolling
    if (!m_synced) {
        double y = padding;
        QColor unsyncedColor = dark ? QColor(255, 255, 255, 200) : QColor(0, 0, 0, 200);
        for (int i = 0; i < m_lyrics.size(); ++i) {
            p.setFont(normalFont);
            p.setPen(unsyncedColor);
            QRect textRect(padding, static_cast<int>(y), availWidth, 10000);
            QRect br;
            p.drawText(textRect, Qt::AlignLeft | Qt::TextWordWrap,
                       m_lyrics[i].text, &br);
            y += br.height() + 8;
        }
        return;
    }

    // Synced lyrics — draw with scroll offset and highlighting
    for (int i = 0; i < m_lyrics.size() && i < m_lineLayouts.size(); ++i) {
        double lineY = m_lineLayouts[i].y - m_scrollOffset + padding;
        double lineH = m_lineLayouts[i].height;

        // Cull off-screen lines
        if (lineY + lineH < -50 || lineY > height() + 50)
            continue;

        // Determine style based on distance from current line
        int dist = (m_currentLine >= 0) ? std::abs(i - m_currentLine) : 999;

        if (i == m_currentLine) {
            p.setFont(currentFont);
            p.setPen(currentColor);
        } else if (dist <= 2) {
            p.setFont(adjacentFont);
            p.setPen(adjacentColor);
        } else {
            p.setFont(normalFont);
            p.setPen(dimColor);
        }

        QRect textRect(padding, static_cast<int>(lineY),
                       availWidth, static_cast<int>(lineH + 20));
        p.drawText(textRect, Qt::AlignHCenter | Qt::TextWordWrap,
                   m_lyrics[i].text);
    }

    // Fade edges (top and bottom gradient) — must match actual background
    QLinearGradient topFade(0, 0, 0, 40);
    topFade.setColorAt(0, bgColor);
    topFade.setColorAt(1, QColor(bgColor.red(), bgColor.green(), bgColor.blue(), 0));
    p.fillRect(0, 0, width(), 40, topFade);

    QLinearGradient bottomFade(0, height() - 40, 0, height());
    bottomFade.setColorAt(0, QColor(bgColor.red(), bgColor.green(), bgColor.blue(), 0));
    bottomFade.setColorAt(1, bgColor);
    p.fillRect(0, height() - 40, width(), 40, bottomFade);
}

// ═════════════════════════════════════════════════════════════════════
//  mousePressEvent — click to seek
// ═════════════════════════════════════════════════════════════════════

void LyricsWidget::mousePressEvent(QMouseEvent* event)
{
    if (!m_synced || m_lyrics.isEmpty() || m_lineLayouts.isEmpty()) {
        QWidget::mousePressEvent(event);
        return;
    }

    int clickY = static_cast<int>(event->position().y());
    int padding = 16;

    // Find which line was clicked
    for (int i = 0; i < m_lyrics.size() && i < m_lineLayouts.size(); ++i) {
        double lineY = m_lineLayouts[i].y - m_scrollOffset + padding;
        double lineH = m_lineLayouts[i].height;

        if (clickY >= lineY && clickY <= lineY + lineH) {
            if (m_lyrics[i].timestampMs < 0) return;  // skip unsynced lines
            double seekSec = m_lyrics[i].timestampMs / 1000.0;
            emit seekRequested(seekSec);
            qDebug() << "[Lyrics] Click seek to line" << i
                     << "at" << seekSec << "sec";
            return;
        }
    }

    QWidget::mousePressEvent(event);
}

// ═════════════════════════════════════════════════════════════════════
//  resizeEvent — recalculate layout on resize
// ═════════════════════════════════════════════════════════════════════

void LyricsWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_layoutDirty = true;
    // update() is implicit from resize; paintEvent will recalcLayout()
    // and re-center current line
}
