#pragma once

#include <QWidget>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QEnterEvent>
#include <QMenu>
#include <QPainter>
#include "../core/MusicData.h"

class StyledButton;
class FormatBadge;

class TrackRow : public QWidget
{
    Q_OBJECT

public:
    explicit TrackRow(const Track& track,
                      int rowNumber,
                      bool showAlbum = true,
                      QWidget* parent = nullptr);

    void setHighlighted(bool highlighted);
    bool isHighlighted() const;
    void setSelected(bool selected);
    bool isSelected() const { return m_selected; }
    void setRowNumber(int number);

    const Track& track() const { return m_track; }

signals:
    void trackClicked(const Track& track);
    void trackDoubleClicked(const Track& track);
    void menuClicked(const Track& track);
    void editTagsRequested(const Track& track);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void updateHoverState(bool hovered);
    void updateStyle();
    void refreshTheme();

    Track         m_track;
    int           m_rowNumber;
    bool          m_highlighted;
    bool          m_selected = false;
    bool          m_hovered;

    QLabel*       m_numberLabel;
    QLabel*       m_playIconLabel;
    QLabel*       m_titleLabel;
    QLabel*       m_artistLabel;
    QLabel*       m_albumLabel;
    QLabel*       m_durationLabel;
    FormatBadge*  m_formatBadge;
    StyledButton* m_menuButton;
};
