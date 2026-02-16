#include "NowPlayingInfo.h"
#include "../../core/ThemeManager.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

NowPlayingInfo::NowPlayingInfo(QWidget* parent)
    : QWidget(parent)
{
    auto c = ThemeManager::instance()->colors();

    setMinimumWidth(200);
    setMaximumWidth(280);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 8);
    layout->setSpacing(12);
    layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Album art 56x56
    m_coverArtLabel = new QLabel();
    m_coverArtLabel->setFixedSize(56, 56);
    m_coverArtLabel->setAlignment(Qt::AlignCenter);
    m_coverArtLabel->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 4px;"
                        " color: %2; font-size: 22px;")
            .arg(c.backgroundTertiary, c.foregroundMuted));
    m_coverArtLabel->setText(QStringLiteral("\u266B"));
    layout->addWidget(m_coverArtLabel);

    // Track info column
    auto* trackInfoWidget = new QWidget();
    auto* trackInfoLayout = new QVBoxLayout(trackInfoWidget);
    trackInfoLayout->setSpacing(2);
    trackInfoLayout->setContentsMargins(0, 0, 0, 0);

    m_trackTitleLabel = new QLabel(QStringLiteral("Not Playing"));
    m_trackTitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 500;")
            .arg(c.foreground));
    m_trackTitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_trackTitleLabel->setMinimumWidth(60);
    trackInfoLayout->addWidget(m_trackTitleLabel);

    m_subtitleLabel = new QLabel();
    m_subtitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;")
            .arg(c.foregroundMuted));
    m_subtitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_subtitleLabel->setMinimumWidth(60);
    m_subtitleLabel->setCursor(Qt::PointingHandCursor);
    m_subtitleLabel->installEventFilter(this);
    trackInfoLayout->addWidget(m_subtitleLabel);

    // Signal path row: [●] FLAC 44.1kHz/24bit
    auto* signalRow = new QWidget();
    auto* signalLayout = new QHBoxLayout(signalRow);
    signalLayout->setContentsMargins(0, 2, 0, 0);
    signalLayout->setSpacing(5);

    m_signalPathDot = new QWidget();
    m_signalPathDot->setFixedSize(8, 8);
    m_signalPathDot->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 4px;").arg(c.foregroundMuted));
    m_signalPathDot->setVisible(false);
    signalLayout->addWidget(m_signalPathDot);

    m_formatLabel = new QLabel();
    m_formatLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px;")
            .arg(c.foregroundMuted));
    m_formatLabel->setVisible(false);
    signalLayout->addWidget(m_formatLabel);

    m_autoplayLabel = new QLabel(QStringLiteral("Autoplay"));
    {
        QColor accentColor(c.accent);
        accentColor.setAlphaF(0.7);
        m_autoplayLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 10px;")
                .arg(accentColor.name(QColor::HexArgb)));
    }
    m_autoplayLabel->setVisible(false);
    signalLayout->addWidget(m_autoplayLabel);

    signalLayout->addStretch();

    trackInfoLayout->addWidget(signalRow);
    layout->addWidget(trackInfoWidget);
    layout->addStretch();
}

void NowPlayingInfo::setTrack(const Track& track)
{
    auto c = ThemeManager::instance()->colors();

    if (track.id.isEmpty()) {
        m_trackTitleLabel->setText(QStringLiteral("Not Playing"));
        m_subtitleLabel->setText(QString());
        m_coverArtLabel->setPixmap(QPixmap());
        m_coverArtLabel->setText(QStringLiteral("\u266B"));
        m_coverArtLabel->setStyleSheet(
            QStringLiteral("background: %1; border-radius: 4px;"
                " color: %2; font-size: 22px;")
                .arg(c.backgroundTertiary, c.foregroundMuted));
    } else {
        // Elide text to fit available width
        {
            QFontMetrics fm(m_trackTitleLabel->font());
            int availW = m_trackTitleLabel->width();
            if (availW < 40) availW = 140;
            QString elided = fm.elidedText(track.title, Qt::ElideRight, availW);
            m_trackTitleLabel->setText(elided);
            m_trackTitleLabel->setToolTip(track.title);
        }

        // "Artist · Album"
        QString subtitle;
        if (!track.artist.isEmpty()) {
            subtitle = track.artist;
            if (!track.album.isEmpty())
                subtitle += QStringLiteral(" \u00B7 ") + track.album;
        } else if (!track.album.isEmpty()) {
            subtitle = track.album;
        }
        {
            QFontMetrics fm(m_subtitleLabel->font());
            int availW = m_subtitleLabel->width();
            if (availW < 40) availW = 140;
            QString elided = fm.elidedText(subtitle, Qt::ElideRight, availW);
            m_subtitleLabel->setText(elided);
            m_subtitleLabel->setToolTip(subtitle);
        }

        // Cover art placeholder
        m_currentTrackPath = track.filePath;
        {
            QString fallback = track.album.isEmpty()
                ? QStringLiteral("\u266B")
                : track.album.left(1).toUpper();
            m_coverArtLabel->setPixmap(QPixmap());
            m_coverArtLabel->setText(fallback);
            m_coverArtLabel->setStyleSheet(
                QStringLiteral("background: %1; border-radius: 4px;"
                    " color: %2; font-size: 22px;")
                    .arg(c.backgroundTertiary, c.foregroundMuted));
        }
    }

    updateSignalPath(track);
}

void NowPlayingInfo::setAutoplayVisible(bool visible)
{
    m_autoplayLabel->setVisible(visible);
}

void NowPlayingInfo::onCoverArtReady(const QString& trackPath, const QPixmap& pixmap)
{
    if (trackPath != m_currentTrackPath)
        return;

    const int artSize = 56;
    if (!pixmap.isNull()) {
        QPixmap coverPix = pixmap;
        if (coverPix.width() > artSize || coverPix.height() > artSize) {
            int x = (coverPix.width() - artSize) / 2;
            int y = (coverPix.height() - artSize) / 2;
            coverPix = coverPix.copy(x, y, artSize, artSize);
        }
        QPixmap rounded(artSize, artSize);
        rounded.fill(Qt::transparent);
        QPainter painter(&rounded);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(0, 0, artSize, artSize, 4, 4);
        painter.setClipPath(path);
        painter.drawPixmap(0, 0, coverPix);
        painter.end();
        m_coverArtLabel->setPixmap(rounded);
        m_coverArtLabel->setStyleSheet(QStringLiteral("border: none;"));
    }
}

void NowPlayingInfo::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    m_trackTitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 500;")
            .arg(c.foreground));
    m_subtitleLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; }"
                       "QLabel:hover { color: %2; }")
            .arg(c.foregroundMuted, c.foreground));
    m_formatLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px;").arg(c.foregroundMuted));
    {
        QColor accentColor(c.accent);
        accentColor.setAlphaF(0.7);
        m_autoplayLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 10px;")
                .arg(accentColor.name(QColor::HexArgb)));
    }
}

void NowPlayingInfo::updateSignalPath(const Track& track)
{
    if (track.id.isEmpty()) {
        m_signalPathDot->setVisible(false);
        m_formatLabel->setVisible(false);
        return;
    }

    // Apple Music streaming track
    if (track.filePath.isEmpty() && !track.id.isEmpty()) {
        m_signalPathDot->setStyleSheet(QStringLiteral(
            "background: #FC3C44; border-radius: 4px;"));
        m_signalPathDot->setVisible(true);
        m_formatLabel->setText(QStringLiteral("Apple Music"));
        m_formatLabel->setVisible(true);
        return;
    }

    QColor dotColor = getFormatColor(track.format);
    m_signalPathDot->setStyleSheet(QString(
        "background: %1; border-radius: 4px;"
    ).arg(dotColor.name()));
    m_signalPathDot->setVisible(true);

    QString formatText = getFormatLabel(track.format);
    if (!track.sampleRate.isEmpty())
        formatText += QStringLiteral(" ") + track.sampleRate;
    if (!track.bitDepth.isEmpty())
        formatText += QStringLiteral("/") + track.bitDepth;

    m_formatLabel->setText(formatText);
    m_formatLabel->setVisible(true);
}

bool NowPlayingInfo::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_subtitleLabel && event->type() == QEvent::MouseButtonRelease) {
        emit subtitleClicked();
        return true;
    }
    return QWidget::eventFilter(obj, event);
}
