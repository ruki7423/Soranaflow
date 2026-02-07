#pragma once
#include <QWidget>
#include <QList>
#include <QPropertyAnimation>
#include "../core/lyrics/LyricsProvider.h"

class LyricsWidget : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(double scrollOffset READ scrollOffset WRITE setScrollOffset)

public:
    explicit LyricsWidget(QWidget* parent = nullptr);

    void setLyrics(const QList<LyricLine>& lyrics, bool synced);
    void setPosition(double seconds);
    void clear();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void seekRequested(double seconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    double scrollOffset() const { return m_scrollOffset; }
    void setScrollOffset(double offset);
    int findCurrentLine(qint64 positionMs) const;

    QList<LyricLine> m_lyrics;
    bool m_synced = false;
    int m_currentLine = -1;
    double m_scrollOffset = 0.0;
    QPropertyAnimation* m_scrollAnim = nullptr;

    // Cached line heights for layout
    struct LineLayout {
        double y;       // top of line (in content coords)
        double height;   // line height
    };
    QList<LineLayout> m_lineLayouts;
    bool m_layoutDirty = false;
    int m_lastLayoutWidth = 0;
    void recalcLayout();
};
