#include "TrackRow.h"
#include "StyledButton.h"
#include "FormatBadge.h"
#include "../core/ThemeManager.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>

TrackRow::TrackRow(const Track& track,
                   int rowNumber,
                   bool showAlbum,
                   QWidget* parent)
    : QWidget(parent)
    , m_track(track)
    , m_rowNumber(rowNumber)
    , m_highlighted(false)
    , m_hovered(false)
    , m_numberLabel(nullptr)
    , m_playIconLabel(nullptr)
    , m_titleLabel(nullptr)
    , m_artistLabel(nullptr)
    , m_albumLabel(nullptr)
    , m_durationLabel(nullptr)
    , m_formatBadge(nullptr)
    , m_menuButton(nullptr)
{
    setObjectName(QStringLiteral("TrackRow"));
    setFixedHeight(UISizes::rowHeight);
    setCursor(Qt::PointingHandCursor);

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(8, 0, 8, 0);
    mainLayout->setSpacing(12);

    // ── Number / Play area (40px fixed) ──────────────────────────────
    auto* numberWidget = new QWidget(this);
    numberWidget->setFixedWidth(UISizes::thumbnailSize);
    auto* numberStack = new QHBoxLayout(numberWidget);
    numberStack->setContentsMargins(0, 0, 0, 0);
    numberStack->setAlignment(Qt::AlignCenter);

    auto c = ThemeManager::instance()->colors();

    m_numberLabel = new QLabel(QString::number(rowNumber), numberWidget);
    m_numberLabel->setAlignment(Qt::AlignCenter);
    m_numberLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foregroundMuted));
    numberStack->addWidget(m_numberLabel);

    m_playIconLabel = new QLabel(QStringLiteral("\u25B6"), numberWidget);
    m_playIconLabel->setAlignment(Qt::AlignCenter);
    m_playIconLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foreground));
    m_playIconLabel->setVisible(false);
    numberStack->addWidget(m_playIconLabel);

    mainLayout->addWidget(numberWidget);

    // ── Title + Artist area (stretch 2) ──────────────────────────────
    auto* titleArtistWidget = new QWidget(this);
    auto* titleArtistLayout = new QVBoxLayout(titleArtistWidget);
    titleArtistLayout->setContentsMargins(0, 4, 0, 4);
    titleArtistLayout->setSpacing(2);

    m_titleLabel = new QLabel(track.title, titleArtistWidget);
    m_titleLabel->setStyleSheet(QString("color: %1; font-weight: bold; font-size: 13px;").arg(c.foreground));
    m_titleLabel->setWordWrap(false);
    titleArtistLayout->addWidget(m_titleLabel);

    m_artistLabel = new QLabel(track.artist, titleArtistWidget);
    m_artistLabel->setStyleSheet(QString("color: %1; font-size: 12px;").arg(c.foregroundSecondary));
    m_artistLabel->setWordWrap(false);
    titleArtistLayout->addWidget(m_artistLabel);

    mainLayout->addWidget(titleArtistWidget, 2);

    // ── Album area (stretch 1, optional) ─────────────────────────────
    if (showAlbum) {
        m_albumLabel = new QLabel(track.album, this);
        m_albumLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foregroundSecondary));
        m_albumLabel->setWordWrap(false);
        mainLayout->addWidget(m_albumLabel, 1);
    }

    // ── Format badge ─────────────────────────────────────────────────
    m_formatBadge = new FormatBadge(track.format,
                                    track.sampleRate,
                                    track.bitDepth,
                                    track.bitrate,
                                    this);
    mainLayout->addWidget(m_formatBadge);

    // ── Duration (60px fixed) ────────────────────────────────────────
    m_durationLabel = new QLabel(formatDuration(track.duration), this);
    m_durationLabel->setFixedWidth(60);
    m_durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_durationLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foregroundMuted));
    mainLayout->addWidget(m_durationLabel);

    // ── Menu button (32px fixed, hidden by default) ──────────────────
    m_menuButton = new StyledButton(QStringLiteral("\u22EF"), QStringLiteral("icon"), this);
    m_menuButton->setButtonSize(QStringLiteral("icon"));
    m_menuButton->setFixedSize(UISizes::transportButtonSize, UISizes::transportButtonSize);
    m_menuButton->setVisible(false);
    m_menuButton->setToolTip(QStringLiteral("More options"));
    connect(m_menuButton, &QPushButton::clicked, this, [this]() {
        emit menuClicked(m_track);
    });
    mainLayout->addWidget(m_menuButton);

    setLayout(mainLayout);

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &TrackRow::refreshTheme);
}

void TrackRow::setHighlighted(bool highlighted)
{
    if (m_highlighted == highlighted)
        return;
    m_highlighted = highlighted;
    updateStyle();
}

bool TrackRow::isHighlighted() const
{
    return m_highlighted;
}

void TrackRow::setSelected(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    updateStyle();
    update(); // trigger repaint for selection indicator
}

void TrackRow::updateStyle()
{
    auto c = ThemeManager::instance()->colors();

    if (m_selected) {
        setStyleSheet(QString("TrackRow { background-color: %1; border-radius: %2px; }").arg(c.selected).arg(UISizes::buttonRadius));
    } else if (m_highlighted) {
        setStyleSheet(QString("TrackRow { background-color: %1; }").arg(c.accentMuted));
    } else if (m_hovered) {
        setStyleSheet(QString("TrackRow { background-color: %1; }").arg(c.hover));
    } else {
        setStyleSheet(QString());
    }
}

void TrackRow::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    if (m_selected) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(0, 4, 3, height() - 8, QColor(ThemeManager::instance()->colors().selectedBorder));
    }
}

void TrackRow::setRowNumber(int number)
{
    m_rowNumber = number;
    m_numberLabel->setText(QString::number(number));
}

void TrackRow::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    updateHoverState(true);
    QWidget::enterEvent(event);
}

void TrackRow::leaveEvent(QEvent* event)
{
    m_hovered = false;
    updateHoverState(false);
    QWidget::leaveEvent(event);
}

void TrackRow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        // Click on play icon area (first ~48px) while hovering -> play immediately
        if (m_hovered && event->position().x() < 48) {
            emit trackDoubleClicked(m_track);
        } else {
            emit trackClicked(m_track);  // select only
        }
    } else if (event->button() == Qt::RightButton) {
        QMenu contextMenu(this);
        contextMenu.setStyleSheet(ThemeManager::instance()->menuStyle());

        QAction* editTagsAction = contextMenu.addAction(tr("Edit Tags..."));
        contextMenu.addSeparator();
        QAction* playAction = contextMenu.addAction(tr("Play"));

        QAction* chosen = contextMenu.exec(event->globalPosition().toPoint());
        if (chosen == editTagsAction) {
            emit editTagsRequested(m_track);
        } else if (chosen == playAction) {
            emit trackDoubleClicked(m_track);  // play via context menu
        }
    }
    QWidget::mousePressEvent(event);
}

void TrackRow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit trackDoubleClicked(m_track);  // play on double-click
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TrackRow::updateHoverState(bool hovered)
{
    // Toggle number vs play icon
    m_numberLabel->setVisible(!hovered);
    m_playIconLabel->setVisible(hovered);

    // Toggle menu button
    m_menuButton->setVisible(hovered);

    // Update background style
    updateStyle();
}

void TrackRow::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    m_numberLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foregroundMuted));
    m_playIconLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foreground));
    m_titleLabel->setStyleSheet(QString("color: %1; font-weight: bold; font-size: 13px;").arg(c.foreground));
    m_artistLabel->setStyleSheet(QString("color: %1; font-size: 12px;").arg(c.foregroundSecondary));
    if (m_albumLabel) {
        m_albumLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foregroundSecondary));
    }
    m_durationLabel->setStyleSheet(QString("color: %1; font-size: 13px;").arg(c.foregroundMuted));

    // Re-apply background
    updateStyle();
}
