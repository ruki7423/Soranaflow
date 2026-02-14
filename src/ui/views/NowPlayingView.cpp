#include "NowPlayingView.h"
#include <QGridLayout>
#include <QSpacerItem>
#include <QPushButton>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QPainter>
#include <QPainterPath>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include "../../core/ThemeManager.h"
#include "../../core/audio/AudioEngine.h"
#include "../services/CoverArtService.h"

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

NowPlayingView::NowPlayingView(QWidget* parent)
    : QWidget(parent)
    , m_albumArt(nullptr)
    , m_titleLabel(nullptr)
    , m_artistLabel(nullptr)
    , m_albumLabel(nullptr)
    , m_formatContainer(nullptr)
    , m_metadataContainer(nullptr)
    , m_signalPathWidget(nullptr)
    , m_leftColumn(nullptr)
    , m_lyricsHeader(nullptr)
    , m_lyricsWidget(nullptr)
    , m_lyricsProvider(new LyricsProvider(this))
    , m_queueContainer(nullptr)
    , m_queueLayout(nullptr)
    , m_queueTitle(nullptr)
{
    setupUI();

    // ── Connect signals ────────────────────────────────────────────
    connect(PlaybackState::instance(), &PlaybackState::trackChanged,
            this, &NowPlayingView::onTrackChanged);
    connect(PlaybackState::instance(), &PlaybackState::queueChanged,
            this, &NowPlayingView::onQueueChanged);

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &NowPlayingView::refreshTheme);

    connect(AudioEngine::instance(), &AudioEngine::signalPathChanged,
            this, &NowPlayingView::onSignalPathChanged);

    // ── Lyrics connections ──────────────────────────────────────────
    connect(AudioEngine::instance(), &AudioEngine::positionChanged,
            this, &NowPlayingView::onPositionChanged);

    connect(m_lyricsProvider, &LyricsProvider::lyricsReady,
            this, &NowPlayingView::onLyricsReady);
    connect(m_lyricsProvider, &LyricsProvider::lyricsNotFound,
            this, &NowPlayingView::onLyricsNotFound);

    connect(m_lyricsWidget, &LyricsWidget::seekRequested,
            this, [](double secs) {
        AudioEngine::instance()->seek(secs);
    });

    // ── Initialize with current data ───────────────────────────────
    const Track current = PlaybackState::instance()->currentTrack();
    if (!current.id.isEmpty()) {
        onTrackChanged(current);
    }
    onQueueChanged();
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::setupUI()
{
    setObjectName(QStringLiteral("NowPlayingView"));

    // ── Outer layout wrapping a scroll area ────────────────────────
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto* scrollArea = new StyledScrollArea(this);
    scrollArea->setWidgetResizable(true);

    auto* scrollContent = new QWidget(scrollArea);
    scrollContent->setObjectName(QStringLiteral("NowPlayingScrollContent"));

    auto* mainLayout = new QHBoxLayout(scrollContent);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(24);

    // ────────────────────────────────────────────────────────────────
    //  LEFT COLUMN — Album Art + Lyrics (no resize handle)
    // ────────────────────────────────────────────────────────────────
    m_leftColumn = new QWidget(scrollContent);
    auto* leftLayout = new QVBoxLayout(m_leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    // ── Top section: Album Art (responsive) ─────────────────────────
    m_albumArt = new QLabel(m_leftColumn);
    m_albumArt->setObjectName(QStringLiteral("NowPlayingAlbumArt"));
    m_albumArt->setMinimumSize(200, 200);
    m_albumArt->setMaximumSize(400, 400);
    m_albumArt->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_albumArt->setScaledContents(true);
    m_albumArt->setAlignment(Qt::AlignCenter);
    {
        auto c = ThemeManager::instance()->colors();
        m_albumArt->setStyleSheet(
            QStringLiteral("QLabel {"
                           "  background-color: %1;"
                           "  border-radius: 12px;"
                           "  color: %2;"
                           "  font-size: 48px;"
                           "}").arg(c.backgroundSecondary, c.foregroundMuted));
    }
    m_albumArt->setText(QStringLiteral("\u266B")); // music note placeholder
    leftLayout->addWidget(m_albumArt);

    // ── Bottom section: Lyrics (fills remaining space) ──────────────
    m_lyricsHeader = new QLabel(QStringLiteral("LYRICS"), m_leftColumn);
    m_lyricsHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; font-weight: bold;"
                       " letter-spacing: 1px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    m_lyricsHeader->setVisible(false);
    leftLayout->addWidget(m_lyricsHeader);

    m_lyricsWidget = new LyricsWidget(m_leftColumn);
    m_lyricsWidget->setMinimumHeight(80);
    m_lyricsWidget->setMaximumHeight(300);
    m_lyricsWidget->setVisible(false);
    leftLayout->addWidget(m_lyricsWidget, 0);

    mainLayout->addWidget(m_leftColumn, 1);

    // ────────────────────────────────────────────────────────────────
    //  CENTER COLUMN — Track Info (stretch 1)
    // ────────────────────────────────────────────────────────────────
    auto* centerColumn = new QVBoxLayout();
    centerColumn->setSpacing(16);

    m_titleLabel = new QLabel(QStringLiteral("No Track Playing"), scrollContent);
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 32px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));

    m_artistLabel = new QLabel(QStringLiteral("—"), scrollContent);
    m_artistLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 18px;")
            .arg(ThemeManager::instance()->colors().accent));
    m_artistLabel->setCursor(Qt::PointingHandCursor);
    m_artistLabel->installEventFilter(this);

    m_albumLabel = new QLabel(QStringLiteral("—"), scrollContent);
    m_albumLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));

    centerColumn->addWidget(m_titleLabel);
    centerColumn->addWidget(m_artistLabel);
    centerColumn->addWidget(m_albumLabel);

    // Spacer (8px)
    centerColumn->addSpacing(8);

    // Format badge container
    m_formatContainer = new QWidget(scrollContent);
    auto* formatLayout = new QHBoxLayout(m_formatContainer);
    formatLayout->setContentsMargins(0, 0, 0, 0);
    formatLayout->setSpacing(8);
    formatLayout->addStretch();
    centerColumn->addWidget(m_formatContainer);

    // Spacer (16px)
    centerColumn->addSpacing(16);

    // Metadata grid container
    m_metadataContainer = new QWidget(scrollContent);
    auto* metaGrid = new QGridLayout(m_metadataContainer);
    metaGrid->setContentsMargins(0, 0, 0, 0);
    metaGrid->setHorizontalSpacing(24);
    metaGrid->setVerticalSpacing(8);

    // Default metadata placeholders (2-column grid)
    const QStringList metaKeys = {
        QStringLiteral("Sample Rate"), QStringLiteral("Bit Depth"),
        QStringLiteral("Bitrate"),     QStringLiteral("Format"),
        QStringLiteral("Duration"),    QStringLiteral("Track"),
        QStringLiteral("Channels")
    };
    const QStringList metaValues = {
        QStringLiteral("—"), QStringLiteral("—"),
        QStringLiteral("—"), QStringLiteral("—"),
        QStringLiteral("—"), QStringLiteral("—"),
        QStringLiteral("—")
    };

    for (int i = 0; i < metaKeys.size(); ++i) {
        int row = i / 2;
        int col = (i % 2) * 2;

        auto* keyLabel = new QLabel(metaKeys[i] + QStringLiteral(":"), m_metadataContainer);
        keyLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
        keyLabel->setObjectName(QStringLiteral("metaKey_%1").arg(i));
        metaGrid->addWidget(keyLabel, row, col);

        auto* valueLabel = new QLabel(metaValues[i], m_metadataContainer);
        valueLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
            .arg(ThemeManager::instance()->colors().foreground));
        valueLabel->setObjectName(QStringLiteral("metaValue_%1").arg(i));
        metaGrid->addWidget(valueLabel, row, col + 1);
    }

    centerColumn->addWidget(m_metadataContainer);

    // Signal path widget
    centerColumn->addSpacing(16);
    m_signalPathWidget = new SignalPathWidget(scrollContent);
    centerColumn->addWidget(m_signalPathWidget);

    centerColumn->addStretch();

    mainLayout->addLayout(centerColumn, 1);

    // ────────────────────────────────────────────────────────────────
    //  RIGHT COLUMN — Queue Preview (fixed 300px)
    // ────────────────────────────────────────────────────────────────
    m_queueContainer = new QWidget(scrollContent);
    m_queueContainer->setMinimumWidth(200);
    m_queueContainer->setMaximumWidth(350);
    m_queueContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto* rightColumn = new QVBoxLayout(m_queueContainer);
    rightColumn->setContentsMargins(0, 0, 0, 0);
    rightColumn->setSpacing(8);

    m_queueTitle = new QLabel(QStringLiteral("Up Next"), m_queueContainer);
    m_queueTitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    rightColumn->addWidget(m_queueTitle);

    // Scrollable queue list
    auto* queueScroll = new StyledScrollArea(m_queueContainer);
    queueScroll->setWidgetResizable(true);

    auto* queueScrollContent = new QWidget(queueScroll);
    m_queueLayout = new QVBoxLayout(queueScrollContent);
    m_queueLayout->setContentsMargins(0, 0, 0, 0);
    m_queueLayout->setSpacing(0);
    m_queueLayout->addStretch();

    queueScroll->setWidget(queueScrollContent);
    rightColumn->addWidget(queueScroll, 1);

    mainLayout->addWidget(m_queueContainer, 0);

    // ── Finalize scroll area ───────────────────────────────────────
    scrollArea->setWidget(scrollContent);
    outerLayout->addWidget(scrollArea);
}

// ═════════════════════════════════════════════════════════════════════
//  onTrackChanged
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::onTrackChanged(const Track& track)
{
    m_currentTrack = track;

    // ── Update labels ──────────────────────────────────────────────
    m_titleLabel->setText(track.title);
    m_artistLabel->setText(track.artist);
    m_albumLabel->setText(track.album);

    // ── Update album art ───────────────────────────────────────────
    // Use fixed size (400) for consistent layout regardless of content
    int artSize = 400;

    QPixmap coverPix = CoverArtService::instance()->getCoverArt(track, artSize);
    if (!coverPix.isNull()) {
        // Crop to square center
        int s = qMin(coverPix.width(), coverPix.height());
        int x = (coverPix.width() - s) / 2;
        int y = (coverPix.height() - s) / 2;
        QPixmap square = coverPix.copy(x, y, s, s);

        // Round corners
        QPixmap rounded(artSize, artSize);
        rounded.fill(Qt::transparent);
        QPainter painter(&rounded);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(0, 0, artSize, artSize, 12, 12);
        painter.setClipPath(path);
        painter.drawPixmap(0, 0, artSize, artSize, square);
        painter.end();

        m_albumArt->setPixmap(rounded);
        m_albumArt->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    } else {
        // Placeholder with gradient and first letter
        QString letter = QStringLiteral("\u266B");
        if (!track.album.isEmpty()) {
            letter = track.album.left(1).toUpper();
        } else if (!track.title.isEmpty()) {
            letter = track.title.left(1).toUpper();
        }
        m_albumArt->clear();
        m_albumArt->setText(letter);
        m_albumArt->setAlignment(Qt::AlignCenter);
        auto c = ThemeManager::instance()->colors();
        m_albumArt->setStyleSheet(
            QStringLiteral("QLabel {"
                           "  background: %1;"
                           "  border-radius: 12px;"
                           "  color: %2;"
                           "  font-size: 72px;"
                           "  font-weight: 300;"
                           "}")
                .arg(c.backgroundTertiary, c.foregroundMuted));
    }

    // ── Correct DSD display values ────────────────────────────────
    // DSD files may have cached database values from before the MetadataReader
    // fix (e.g., "2822.4 kHz" instead of "2.8 MHz", "8-bit" instead of "1-bit").
    // Always override at display time for DSD formats.
    QString displaySampleRate = track.sampleRate;
    QString displayBitDepth = track.bitDepth;
    bool isDSD = (track.format == AudioFormat::DSD64  || track.format == AudioFormat::DSD128
               || track.format == AudioFormat::DSD256 || track.format == AudioFormat::DSD512
               || track.format == AudioFormat::DSD1024 || track.format == AudioFormat::DSD2048);
    if (isDSD) {
        displayBitDepth = QStringLiteral("1-bit");
        // Map format enum to native DSD rate
        double nativeRate = 2822400.0;
        switch (track.format) {
        case AudioFormat::DSD128:  nativeRate = 5644800.0;  break;
        case AudioFormat::DSD256:  nativeRate = 11289600.0; break;
        case AudioFormat::DSD512:  nativeRate = 22579200.0; break;
        case AudioFormat::DSD1024: nativeRate = 45158400.0; break;
        case AudioFormat::DSD2048: nativeRate = 90316800.0; break;
        default: break;
        }
        displaySampleRate = QStringLiteral("%1 MHz").arg(nativeRate / 1000000.0, 0, 'f', 1);
    }

    // ── Update format badge ────────────────────────────────────────
    auto* formatLayout = qobject_cast<QHBoxLayout*>(m_formatContainer->layout());
    if (formatLayout) {
        QLayoutItem* child;
        while ((child = formatLayout->takeAt(0)) != nullptr) {
            if (QWidget* w = child->widget()) w->deleteLater();
            delete child;
        }

        auto* badge = new FormatBadge(track.format,
                                       displaySampleRate,
                                       displayBitDepth,
                                       track.bitrate,
                                       m_formatContainer);
        formatLayout->addWidget(badge);
        formatLayout->addStretch();
    }

    // ── Update metadata grid ───────────────────────────────────────
    int totalTracks = 0;
    const Album album = MusicDataProvider::instance()->albumById(track.albumId);
    if (!album.id.isEmpty()) {
        totalTracks = album.totalTracks;
    }

    // Update album label with year (prefer track.year, fallback to album.year)
    int displayYear = track.year > 0 ? track.year : album.year;
    if (displayYear > 0)
        m_albumLabel->setText(track.album + QStringLiteral(" (%1)").arg(displayYear));
    else
        m_albumLabel->setText(track.album);

    // Validate trackNumber (avoid garbage values from uninitialized data)
    QString trackDisplay;
    if (track.trackNumber > 0 && track.trackNumber < 10000) {
        if (totalTracks > 0 && totalTracks < 10000) {
            trackDisplay = QStringLiteral("%1 of %2").arg(track.trackNumber).arg(totalTracks);
        } else {
            trackDisplay = QString::number(track.trackNumber);
        }
    } else {
        trackDisplay = QStringLiteral("\u2014");
    }

    // Channel label
    QString channelLabel;
    switch (track.channelCount) {
    case 1:  channelLabel = QStringLiteral("Mono"); break;
    case 2:  channelLabel = QStringLiteral("Stereo"); break;
    case 3:  channelLabel = QStringLiteral("3.0"); break;
    case 4:  channelLabel = QStringLiteral("4.0 Quad"); break;
    case 6:  channelLabel = QStringLiteral("5.1"); break;
    case 8:  channelLabel = QStringLiteral("7.1"); break;
    default: channelLabel = QStringLiteral("%1ch").arg(track.channelCount); break;
    }

    const QStringList newValues = {
        displaySampleRate.isEmpty() ? QStringLiteral("\u2014") : displaySampleRate,
        displayBitDepth.isEmpty()   ? QStringLiteral("\u2014") : displayBitDepth,
        track.bitrate.isEmpty()    ? QStringLiteral("\u2014") : track.bitrate,
        getFormatLabel(track.format),
        formatDuration(track.duration),
        trackDisplay,
        channelLabel
    };

    for (int i = 0; i < newValues.size(); ++i) {
        auto* valueLabel = m_metadataContainer->findChild<QLabel*>(
            QStringLiteral("metaValue_%1").arg(i));
        if (valueLabel) {
            valueLabel->setText(newValues[i]);
        }
    }

    // ── Fetch lyrics ──────────────────────────────────────────────
    m_lyricsWidget->clear();
    m_lyricsHeader->setVisible(false);
    m_lyricsWidget->setVisible(false);
    if (!track.id.isEmpty()) {
        m_lyricsProvider->fetchLyrics(
            track.filePath, track.title, track.artist,
            track.album, track.duration / 1000);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  onQueueChanged
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::onQueueChanged()
{
    auto newQueue = PlaybackState::instance()->displayQueue();
    if (newQueue.size() == m_cachedDisplayQueue.size()) {
        bool same = true;
        for (int i = 0; i < newQueue.size(); ++i) {
            if (newQueue[i].id != m_cachedDisplayQueue[i].id) {
                same = false;
                break;
            }
        }
        if (same) return;
    }
    m_cachedDisplayQueue = newQueue;
    updateQueueList();
}

// ═════════════════════════════════════════════════════════════════════
//  updateQueueList
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::updateQueueList()
{
    // Suspend painting during rebuild
    m_queueContainer->setUpdatesEnabled(false);

    // Remove all existing items (except the trailing stretch)
    QLayoutItem* child;
    while ((child = m_queueLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->hide();
            child->widget()->deleteLater();
        }
        delete child;
    }

    const QVector<Track> queue = PlaybackState::instance()->displayQueue();
    const Track current = PlaybackState::instance()->currentTrack();

    // Find the index of the current track in the display queue
    int currentIdx = -1;
    for (int i = 0; i < queue.size(); ++i) {
        if (queue[i].id == current.id) {
            currentIdx = i;
            break;
        }
    }

    // Show up to 10 items after the current track
    int startIdx = currentIdx + 1;
    int count = 0;

    for (int i = startIdx; i < queue.size() && count < 10; ++i, ++count) {
        const Track& t = queue[i];

        auto* itemWidget = new QWidget();
        itemWidget->setFixedHeight(48);
        itemWidget->setCursor(Qt::PointingHandCursor);
        itemWidget->setProperty("queueIndex", i);
        itemWidget->installEventFilter(this);
        auto c = ThemeManager::instance()->colors();
        itemWidget->setStyleSheet(
            QStringLiteral("QWidget { border-bottom: 1px solid %1; }")
                .arg(c.borderSubtle));

        auto* itemLayout = new QHBoxLayout(itemWidget);
        itemLayout->setContentsMargins(4, 4, 4, 4);
        itemLayout->setSpacing(8);

        // Title + Artist stacked — must shrink so right-side widgets stay pinned
        auto* textWidget = new QWidget(itemWidget);
        textWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        textWidget->setMinimumWidth(40);
        auto* textLayout = new QVBoxLayout(textWidget);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(2);

        auto* titleLabel = new QLabel(t.title, textWidget);
        titleLabel->setStyleSheet(
            QStringLiteral("color: %1; font-weight: bold; font-size: 13px; border: none;")
                .arg(ThemeManager::instance()->colors().foreground));
        titleLabel->setWordWrap(false);
        titleLabel->setMinimumWidth(0);
        titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        titleLabel->setToolTip(t.title);
        textLayout->addWidget(titleLabel);

        auto* artistLabel = new QLabel(t.artist, textWidget);
        artistLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;")
                .arg(ThemeManager::instance()->colors().foregroundMuted));
        artistLabel->setWordWrap(false);
        artistLabel->setMinimumWidth(0);
        artistLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        artistLabel->setToolTip(t.artist);
        textLayout->addWidget(artistLabel);

        itemLayout->addWidget(textWidget, 1);

        // Duration (fixed width so it never shifts)
        auto* durationLabel = new QLabel(formatDuration(t.duration), itemWidget);
        durationLabel->setFixedWidth(45);
        durationLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;")
                .arg(ThemeManager::instance()->colors().foregroundMuted));
        durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        itemLayout->addWidget(durationLabel);

        // Remove button — plain QPushButton, never blue
        const int queueIndex = i;
        auto* removeBtn = new QPushButton(QStringLiteral("\u00D7"), itemWidget);
        removeBtn->setObjectName(QStringLiteral("queueRemoveBtn"));
        removeBtn->setFlat(true);
        removeBtn->setFixedSize(24, 24);
        removeBtn->setCursor(Qt::PointingHandCursor);
        removeBtn->setToolTip(QStringLiteral("Remove from queue"));
        removeBtn->setStyleSheet(QStringLiteral(
            "QPushButton#queueRemoveBtn {"
            "  background-color: transparent;"
            "  border: none;"
            "  border-radius: 12px;"
            "  color: %1;"
            "  font-size: 18px;"
            "  font-weight: 300;"
            "  padding: 0px;"
            "}"
            "QPushButton#queueRemoveBtn:hover {"
            "  background-color: %2;"
            "  color: %3;"
            "}"
            "QPushButton#queueRemoveBtn:pressed {"
            "  background-color: %4;"
            "  color: %5;"
            "}")
            .arg(c.foregroundMuted, c.hover, c.foregroundSecondary, c.pressed, c.foreground));
        connect(removeBtn, &QPushButton::clicked, this, [queueIndex]() {
            PlaybackState::instance()->removeFromQueue(queueIndex);
        });
        itemLayout->addWidget(removeBtn);

        m_queueLayout->addWidget(itemWidget);
    }

    m_queueLayout->addStretch();

    // Resume painting
    m_queueContainer->setUpdatesEnabled(true);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 32px; font-weight: bold;")
            .arg(c.foreground));

    m_albumLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(c.foregroundMuted));

    m_queueTitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(c.foreground));

    // Only update placeholder style if not showing an image
    if (m_albumArt->pixmap().isNull()) {
        m_albumArt->setStyleSheet(
            QStringLiteral("QLabel {"
                           "  background-color: %1;"
                           "  border-radius: 12px;"
                           "  color: %2;"
                           "  font-size: 72px;"
                           "  font-weight: 300;"
                           "}").arg(c.backgroundTertiary, c.foregroundMuted));
    }

    // Refresh metadata key/value labels
    for (int i = 0; i < 6; ++i) {
        auto* keyLabel = m_metadataContainer->findChild<QLabel*>(
            QStringLiteral("metaKey_%1").arg(i));
        if (keyLabel) {
            keyLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                .arg(c.foregroundMuted));
        }
        auto* valueLabel = m_metadataContainer->findChild<QLabel*>(
            QStringLiteral("metaValue_%1").arg(i));
        if (valueLabel) {
            valueLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                .arg(c.foreground));
        }
    }

    if (m_artistLabel) {
        m_artistLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 18px; }"
                           "QLabel:hover { color: %2; }")
                .arg(c.accent, c.accentHover));
    }

    m_lyricsHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: bold;"
                       " text-transform: uppercase; letter-spacing: 1px;")
            .arg(c.foregroundMuted));

    // Refresh queue list (easiest to rebuild)
    updateQueueList();
}

// ═════════════════════════════════════════════════════════════════════
//  onSignalPathChanged
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::onSignalPathChanged()
{
    auto* engine = AudioEngine::instance();
    SignalPathInfo info = engine->getSignalPath();
    if (info.nodes.isEmpty()) {
        // Keep the last signal path visible — don't clear on stop.
        // The widget was hidden initially and only appears once real
        // data arrives, so leaving stale data is better than a blank
        // gap that jumps on every play/stop cycle.
        return;
    }
    m_signalPathWidget->updateSignalPath(info);

    // ── Fix DSD format badge if runtime detection disagrees with tag metadata ──
    // MetadataReader (TagLib) may report a different DSD rate than the decoder
    // actually detects from the bitstream. Trust the runtime detection.
    AudioFormat runtimeFmt = engine->actualDsdFormat();
    bool trackIsDSD = (m_currentTrack.format == AudioFormat::DSD64
                    || m_currentTrack.format == AudioFormat::DSD128
                    || m_currentTrack.format == AudioFormat::DSD256
                    || m_currentTrack.format == AudioFormat::DSD512
                    || m_currentTrack.format == AudioFormat::DSD1024
                    || m_currentTrack.format == AudioFormat::DSD2048);
    bool runtimeIsDSD = (runtimeFmt == AudioFormat::DSD64
                      || runtimeFmt == AudioFormat::DSD128
                      || runtimeFmt == AudioFormat::DSD256
                      || runtimeFmt == AudioFormat::DSD512
                      || runtimeFmt == AudioFormat::DSD1024
                      || runtimeFmt == AudioFormat::DSD2048);

    if (trackIsDSD && runtimeIsDSD && runtimeFmt != m_currentTrack.format) {
        // Rebuild the badge with the decoder's actual format
        double nativeRate = 2822400.0;
        switch (runtimeFmt) {
        case AudioFormat::DSD128:  nativeRate = 5644800.0;  break;
        case AudioFormat::DSD256:  nativeRate = 11289600.0; break;
        case AudioFormat::DSD512:  nativeRate = 22579200.0; break;
        case AudioFormat::DSD1024: nativeRate = 45158400.0; break;
        case AudioFormat::DSD2048: nativeRate = 90316800.0; break;
        default: break;
        }
        QString displaySampleRate = QStringLiteral("%1 MHz").arg(nativeRate / 1000000.0, 0, 'f', 1);

        auto* formatLayout = qobject_cast<QHBoxLayout*>(m_formatContainer->layout());
        if (formatLayout) {
            QLayoutItem* child;
            while ((child = formatLayout->takeAt(0)) != nullptr) {
                if (QWidget* w = child->widget()) w->deleteLater();
                delete child;
            }
            auto* badge = new FormatBadge(runtimeFmt,
                                           displaySampleRate,
                                           QStringLiteral("1-bit"),
                                           m_currentTrack.bitrate,
                                           m_formatContainer);
            formatLayout->addWidget(badge);
            formatLayout->addStretch();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════
//  onPositionChanged — forward to lyrics widget
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::onPositionChanged(double secs)
{
    m_lyricsWidget->setPosition(secs);
}

// ═════════════════════════════════════════════════════════════════════
//  onLyricsReady
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::onLyricsReady(const QList<LyricLine>& lyrics, bool synced)
{
    m_lyricsWidget->setLyrics(lyrics, synced);
    m_lyricsHeader->setVisible(true);
    m_lyricsWidget->setVisible(true);
}

// ═════════════════════════════════════════════════════════════════════
//  onLyricsNotFound
// ═════════════════════════════════════════════════════════════════════

void NowPlayingView::onLyricsNotFound()
{
    m_lyricsWidget->clear();
    m_lyricsHeader->setVisible(true);
    m_lyricsWidget->setVisible(true);
    // LyricsWidget::paintEvent shows "No lyrics available" when empty
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — click-to-play on queue items
// ═════════════════════════════════════════════════════════════════════

bool NowPlayingView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        // Artist label click → navigate to artist detail
        if (obj == m_artistLabel) {
            const Track current = PlaybackState::instance()->currentTrack();
            QString targetArtistId = current.artistId;

            // If track has no artistId, look up by name
            if (targetArtistId.isEmpty() && !current.artist.isEmpty()) {
                const auto artists = MusicDataProvider::instance()->allArtists();
                for (const auto& a : artists) {
                    if (a.name == current.artist) {
                        targetArtistId = a.id;
                        break;
                    }
                }
            }

            if (!targetArtistId.isEmpty()) {
                qDebug() << "[NowPlaying] Artist clicked:" << current.artist
                         << "id:" << targetArtistId;
                emit artistClicked(targetArtistId);
                return true;
            }
            qDebug() << "[NowPlaying] Artist not found in library:" << current.artist;
            return true;
        }

        auto* widget = qobject_cast<QWidget*>(obj);
        if (widget && widget->property("queueIndex").isValid()) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            // Ignore clicks on the far-right 40px (remove button area)
            if (mouseEvent->position().x() < widget->width() - 40) {
                int idx = widget->property("queueIndex").toInt();
                const QVector<Track> q = PlaybackState::instance()->displayQueue();
                if (idx >= 0 && idx < q.size()) {
                    PlaybackState::instance()->playTrack(q[idx]);
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void NowPlayingView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    // Scale album art to be square and fit within the left column's width.
    // The left column gets ~1/3 of (width - margins - spacing - queue).
    // Keep the art square: use left column's actual width clamped to [200, 400].
    if (!m_albumArt || !m_leftColumn) return;
    int available = m_leftColumn->width();
    int artSize = qBound(200, available, 400);
    m_albumArt->setFixedSize(artSize, artSize);
}
