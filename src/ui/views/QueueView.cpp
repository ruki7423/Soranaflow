#include "QueueView.h"
#include <QSpacerItem>
#include <QPushButton>
#include <QMouseEvent>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QDebug>
#include <QDrag>
#include <QGraphicsOpacityEffect>
#include <QMimeData>
#include <QApplication>
#include "../../core/ThemeManager.h"
#include "../../core/audio/MetadataReader.h"

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

QueueView::QueueView(QWidget* parent)
    : QWidget(parent)
    , m_currentSection(nullptr)
    , m_currentCover(nullptr)
    , m_currentTitle(nullptr)
    , m_currentArtist(nullptr)
    , m_currentFormat(nullptr)
    , m_currentFormatContainer(nullptr)
    , m_queueHeader(nullptr)
    , m_queueListContainer(nullptr)
    , m_queueListLayout(nullptr)
    , m_historyHeader(nullptr)
    , m_historyListContainer(nullptr)
    , m_historyListLayout(nullptr)
    , m_titleLabel(nullptr)
    , m_clearBtn(nullptr)
    , m_shuffleBtn(nullptr)
    , m_scrollArea(nullptr)
    , m_emptyLabel(nullptr)
    , m_nowPlayingLabel(nullptr)
{
    setupUI();

    // ── Connect signals ────────────────────────────────────────────
    connect(PlaybackState::instance(), &PlaybackState::trackChanged,
            this, &QueueView::onTrackChanged);
    connect(PlaybackState::instance(), &PlaybackState::queueChanged,
            this, &QueueView::onQueueChanged);
    connect(m_shuffleBtn, &QPushButton::clicked,
            PlaybackState::instance(), &PlaybackState::toggleShuffle);
    connect(m_clearBtn, &QPushButton::clicked,
            PlaybackState::instance(), &PlaybackState::clearUpcoming);

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &QueueView::refreshTheme);

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

void QueueView::setupUI()
{
    setObjectName(QStringLiteral("QueueView"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ────────────────────────────────────────────────────────────────
    //  Header (outside scroll area)
    // ────────────────────────────────────────────────────────────────
    auto* headerWidget = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(24, 24, 24, 0);
    headerLayout->setSpacing(8);

    m_titleLabel = new QLabel(QStringLiteral("Queue"), headerWidget);
    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 24px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    headerLayout->addWidget(m_titleLabel);

    headerLayout->addStretch();

    m_shuffleBtn = new StyledButton(QStringLiteral("Shuffle"),
                                     QStringLiteral("ghost"),
                                     headerWidget);
    m_shuffleBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/shuffle.svg")));
    m_shuffleBtn->setIconSize(QSize(16, 16));
    headerLayout->addWidget(m_shuffleBtn);

    m_clearBtn = new StyledButton(QStringLiteral("Clear"),
                                   QStringLiteral("ghost"),
                                   headerWidget);
    m_clearBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/trash-2.svg")));
    m_clearBtn->setIconSize(QSize(16, 16));
    headerLayout->addWidget(m_clearBtn);

    mainLayout->addWidget(headerWidget);

    // ────────────────────────────────────────────────────────────────
    //  Scrollable Content
    // ────────────────────────────────────────────────────────────────
    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);

    auto* scrollContent = new QWidget(m_scrollArea);
    scrollContent->setObjectName(QStringLiteral("QueueScrollContent"));

    auto* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(24, 16, 24, 24);
    contentLayout->setSpacing(16);

    // ────────────────────────────────────────────────────────────────
    //  Now Playing Section
    // ────────────────────────────────────────────────────────────────
    m_currentSection = new QWidget(scrollContent);
    m_currentSection->setObjectName(QStringLiteral("NowPlayingSection"));
    m_currentSection->setStyleSheet(
        QStringLiteral("QWidget#NowPlayingSection {"
                       "  background-color: transparent;"
                       "  border-radius: 0px;"
                       "  padding: 0px;"
                       "}"));

    auto* currentSectionLayout = new QVBoxLayout(m_currentSection);
    currentSectionLayout->setContentsMargins(16, 16, 16, 16);
    currentSectionLayout->setSpacing(12);

    m_nowPlayingLabel = new QLabel(QStringLiteral("NOW PLAYING"), m_currentSection);
    m_nowPlayingLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; text-transform: uppercase;"
                       " letter-spacing: 2px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    currentSectionLayout->addWidget(m_nowPlayingLabel);

    // Current track info row
    auto* currentInfoWidget = new QWidget(m_currentSection);
    auto* currentInfoLayout = new QHBoxLayout(currentInfoWidget);
    currentInfoLayout->setContentsMargins(0, 0, 0, 0);
    currentInfoLayout->setSpacing(16);

    // Cover art
    m_currentCover = new QLabel(currentInfoWidget);
    m_currentCover->setFixedSize(64, 64);
    m_currentCover->setAlignment(Qt::AlignCenter);
    {
        auto c = ThemeManager::instance()->colors();
        m_currentCover->setStyleSheet(
            QStringLiteral("QLabel {"
                           "  background-color: %1;"
                           "  border-radius: 8px;"
                           "  color: %2;"
                           "  font-size: 24px;"
                           "}")
                .arg(c.backgroundSecondary, c.foregroundMuted));
    }
    m_currentCover->setText(QStringLiteral("\u266B"));
    currentInfoLayout->addWidget(m_currentCover);

    // Track info
    auto* infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(4);

    m_currentTitle = new QLabel(QStringLiteral("No Track"), currentInfoWidget);
    m_currentTitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    infoLayout->addWidget(m_currentTitle);

    m_currentArtist = new QLabel(QStringLiteral("\u2014"), currentInfoWidget);
    m_currentArtist->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    infoLayout->addWidget(m_currentArtist);

    currentInfoLayout->addLayout(infoLayout, 1);

    // Format badge container
    m_currentFormatContainer = new QWidget(currentInfoWidget);
    auto* formatLayout = new QHBoxLayout(m_currentFormatContainer);
    formatLayout->setContentsMargins(0, 0, 0, 0);
    formatLayout->setSpacing(0);
    currentInfoLayout->addWidget(m_currentFormatContainer);

    // Duration placeholder
    auto* currentDuration = new QLabel(QStringLiteral("--:--"), currentInfoWidget);
    currentDuration->setObjectName(QStringLiteral("currentDurationLabel"));
    currentDuration->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    currentDuration->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    currentInfoLayout->addWidget(currentDuration);

    currentSectionLayout->addWidget(currentInfoWidget);

    contentLayout->addWidget(m_currentSection);

    // ────────────────────────────────────────────────────────────────
    //  Up Next Section
    // ────────────────────────────────────────────────────────────────
    m_queueHeader = new QLabel(QStringLiteral("Up Next \u00B7 0 tracks"), scrollContent);
    m_queueHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    contentLayout->addWidget(m_queueHeader);

    m_queueListContainer = new QWidget(scrollContent);
    m_queueListContainer->setAcceptDrops(true);
    m_queueListContainer->installEventFilter(this);
    m_queueListLayout = new QVBoxLayout(m_queueListContainer);
    m_queueListLayout->setContentsMargins(0, 0, 0, 0);
    m_queueListLayout->setSpacing(0);
    contentLayout->addWidget(m_queueListContainer);

    m_emptyLabel = new QLabel(
        QStringLiteral("Queue is empty. Add tracks to get started."),
        scrollContent);
    m_emptyLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setVisible(false);
    contentLayout->addWidget(m_emptyLabel);

    // ────────────────────────────────────────────────────────────────
    //  History Section
    // ────────────────────────────────────────────────────────────────
    m_historyHeader = new QLabel(QStringLiteral("History"), scrollContent);
    m_historyHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    m_historyHeader->setVisible(false);
    contentLayout->addWidget(m_historyHeader);

    m_historyListContainer = new QWidget(scrollContent);
    m_historyListLayout = new QVBoxLayout(m_historyListContainer);
    m_historyListLayout->setContentsMargins(0, 0, 0, 0);
    m_historyListLayout->setSpacing(0);
    m_historyListContainer->setVisible(false);
    contentLayout->addWidget(m_historyListContainer);

    contentLayout->addStretch();

    // ── Finalize scroll area ───────────────────────────────────────
    m_scrollArea->setWidget(scrollContent);
    mainLayout->addWidget(m_scrollArea, 1);
}

// ═════════════════════════════════════════════════════════════════════
//  Cover art helpers
// ═════════════════════════════════════════════════════════════════════

static QPixmap findTrackCoverArt(const Track& track, int size)
{
    QString cacheKey = QStringLiteral("qcover_%1_%2").arg(track.id).arg(size);
    QPixmap cached;
    if (QPixmapCache::find(cacheKey, &cached))
        return cached;

    QPixmap pix;

    if (!track.coverUrl.isEmpty()) {
        QString loadPath = track.coverUrl;
        if (loadPath.startsWith(QStringLiteral("qrc:")))
            loadPath = loadPath.mid(3);
        if (QFile::exists(loadPath)) {
            pix.load(loadPath);
        } else if (loadPath.startsWith(QStringLiteral(":/"))) {
            pix.load(loadPath);
        }
        if (!pix.isNull()) {
            pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            QPixmapCache::insert(cacheKey, pix);
            return pix;
        }
    }

    if (!track.filePath.isEmpty()) {
        QString folder = QFileInfo(track.filePath).absolutePath();
        static const QStringList names = {
            QStringLiteral("cover.jpg"),  QStringLiteral("cover.png"),
            QStringLiteral("folder.jpg"), QStringLiteral("folder.png"),
            QStringLiteral("front.jpg"),  QStringLiteral("front.png"),
            QStringLiteral("Cover.jpg"),  QStringLiteral("Cover.png"),
        };
        for (const QString& n : names) {
            QString path = folder + QStringLiteral("/") + n;
            if (QFile::exists(path)) {
                pix.load(path);
                if (!pix.isNull()) {
                    pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    QPixmapCache::insert(cacheKey, pix);
                    return pix;
                }
            }
        }
    }

    if (!track.filePath.isEmpty()) {
        pix = MetadataReader::extractCoverArt(track.filePath);
        if (!pix.isNull()) {
            pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            QPixmapCache::insert(cacheKey, pix);
            return pix;
        }
    }

    if (!track.filePath.isEmpty()) {
        QString folder = QFileInfo(track.filePath).absolutePath();
        QDir dir(folder);
        QStringList imageFilters = {
            QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
            QStringLiteral("*.png"), QStringLiteral("*.bmp")
        };
        QStringList images = dir.entryList(imageFilters, QDir::Files, QDir::Name);
        for (const QString& img : images) {
            pix.load(dir.filePath(img));
            if (!pix.isNull()) {
                pix = pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                QPixmapCache::insert(cacheKey, pix);
                return pix;
            }
        }
    }

    QPixmapCache::insert(cacheKey, QPixmap());
    return QPixmap();
}

static void setCoverArt(QLabel* label, const QPixmap& pix, int size, int radius,
                        const QString& fallbackText, const QString& surfaceColor)
{
    if (!pix.isNull()) {
        QPixmap scaled = pix;
        if (scaled.width() > size || scaled.height() > size) {
            int x = (scaled.width() - size) / 2;
            int y = (scaled.height() - size) / 2;
            scaled = scaled.copy(x, y, size, size);
        }
        QPixmap rounded(size, size);
        rounded.fill(Qt::transparent);
        QPainter painter(&rounded);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(0, 0, size, size, radius, radius);
        painter.setClipPath(path);
        painter.drawPixmap(0, 0, scaled);
        painter.end();
        label->setPixmap(rounded);
        label->setStyleSheet(QStringLiteral("border: none;"));
    } else {
        label->setText(fallbackText);
        label->setStyleSheet(
            QStringLiteral("background: %1; border-radius: %2px; color: %4; font-size: %3px; border: none;")
                .arg(surfaceColor)
                .arg(radius)
                .arg(size > 48 ? 24 : 16)
                .arg(ThemeManager::instance()->colors().foregroundMuted));
    }
}

// ═════════════════════════════════════════════════════════════════════
//  createQueueItem
// ═════════════════════════════════════════════════════════════════════

QWidget* QueueView::createQueueItem(const Track& track, int index, bool isCurrent, bool isHistory)
{
    auto* tm = ThemeManager::instance();

    auto* item = new QWidget();
    item->setObjectName(QStringLiteral("QueueItem"));
    item->setFixedHeight(56);

    auto c = tm->colors();
    if (isCurrent) {
        item->setStyleSheet(
            QStringLiteral("QWidget#QueueItem {"
                           "  background-color: %1;"
                           "  border-left: 3px solid %2;"
                           "  border-radius: 4px;"
                           "}").arg(c.accentMuted, c.accent));
    } else if (isHistory) {
        item->setStyleSheet(
            QStringLiteral("QWidget#QueueItem {"
                           "  border-bottom: 1px solid %1;"
                           "  opacity: 0.7;"
                           "}"
                           "QWidget#QueueItem:hover {"
                           "  background-color: %2;"
                           "  border-radius: 4px;"
                           "}")
                .arg(c.borderSubtle, c.hover));
    } else {
        item->setStyleSheet(
            QStringLiteral("QWidget#QueueItem {"
                           "  border-bottom: 1px solid %1;"
                           "}"
                           "QWidget#QueueItem:hover {"
                           "  background-color: %2;"
                           "  border-radius: 4px;"
                           "}")
                .arg(c.borderSubtle, c.hover));
    }

    // Store queue index on the widget for drag reorder
    item->setProperty("queueIndex", index);
    item->setProperty("isHistory", isHistory);
    item->setAcceptDrops(!isHistory && !isCurrent);
    item->installEventFilter(this);

    auto* itemLayout = new QHBoxLayout(item);
    itemLayout->setContentsMargins(8, 4, 8, 4);
    itemLayout->setSpacing(12);

    // Drag handle for upcoming tracks (not history, not current)
    if (!isHistory && !isCurrent) {
        auto* dragHandle = new QLabel(QStringLiteral("\u2261"), item);
        dragHandle->setFixedWidth(16);
        dragHandle->setAlignment(Qt::AlignCenter);
        dragHandle->setStyleSheet(
            QStringLiteral("color: %1; font-size: 16px; border: none; background: transparent;")
                .arg(c.foregroundMuted));
        dragHandle->setCursor(Qt::OpenHandCursor);
        itemLayout->addWidget(dragHandle);
    }

    // Album art thumbnail (40x40)
    auto* artLabel = new QLabel(item);
    artLabel->setFixedSize(40, 40);
    artLabel->setAlignment(Qt::AlignCenter);

    QPixmap coverPix = findTrackCoverArt(track, 40);
    QString fallback = track.album.isEmpty()
        ? QStringLiteral("\u266B")
        : track.album.left(1).toUpper();
    setCoverArt(artLabel, coverPix, 40, 4, fallback, c.backgroundSecondary);
    itemLayout->addWidget(artLabel);

    // Track info
    auto* infoWidget = new QWidget(item);
    infoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    infoWidget->setMinimumWidth(40);
    auto* infoLayout = new QVBoxLayout(infoWidget);
    infoLayout->setSpacing(2);
    infoLayout->setContentsMargins(0, 0, 0, 0);

    auto* titleLabel = new QLabel(track.title, infoWidget);
    titleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: bold; border: none;")
            .arg(isHistory ? c.foregroundMuted : c.foreground));
    titleLabel->setWordWrap(false);
    titleLabel->setMinimumWidth(0);
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    infoLayout->addWidget(titleLabel);

    auto* artistLabel = new QLabel(track.artist, infoWidget);
    artistLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; border: none;")
            .arg(c.foregroundMuted));
    artistLabel->setWordWrap(false);
    artistLabel->setMinimumWidth(0);
    artistLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    infoLayout->addWidget(artistLabel);

    itemLayout->addWidget(infoWidget, 1);

    // Duration
    auto* durationLabel = new QLabel(formatDuration(track.duration), item);
    durationLabel->setFixedWidth(50);
    durationLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    durationLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; font-family: 'Menlo', 'Courier New'; border: none;")
            .arg(c.foregroundMuted));
    itemLayout->addWidget(durationLabel);

    // Remove button (only for upcoming tracks, not history)
    if (!isHistory && !isCurrent) {
        auto* removeBtn = new QPushButton(item);
        removeBtn->setObjectName(QStringLiteral("queueRemoveBtn"));
        removeBtn->setFlat(true);
        removeBtn->setFixedSize(24, 24);
        removeBtn->setText(QStringLiteral("\u00D7"));
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
            "}"
            "QPushButton#queueRemoveBtn:focus {"
            "  outline: none;"
            "}")
            .arg(c.foregroundMuted, c.hover, c.foregroundSecondary, c.pressed, c.foreground));
        const int queueIndex = index;
        connect(removeBtn, &QPushButton::clicked, this, [queueIndex]() {
            PlaybackState::instance()->removeFromQueue(queueIndex);
        });
        itemLayout->addWidget(removeBtn);
    }

    return item;
}

// ═════════════════════════════════════════════════════════════════════
//  onTrackChanged
// ═════════════════════════════════════════════════════════════════════

void QueueView::onTrackChanged(const Track& track)
{
    Q_UNUSED(track)
    updateCurrentTrack();
    updateQueueList();
}

// ═════════════════════════════════════════════════════════════════════
//  onQueueChanged
// ═════════════════════════════════════════════════════════════════════

void QueueView::onQueueChanged()
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
//  updateCurrentTrack
// ═════════════════════════════════════════════════════════════════════

void QueueView::updateCurrentTrack()
{
    const Track current = PlaybackState::instance()->currentTrack();

    if (current.id.isEmpty()) {
        m_currentTitle->setText(QStringLiteral("No Track"));
        m_currentArtist->setText(QStringLiteral("\u2014"));
        m_currentCover->setText(QStringLiteral("\u266B"));

        auto* durationLabel = m_currentSection->findChild<QLabel*>(
            QStringLiteral("currentDurationLabel"));
        if (durationLabel) {
            durationLabel->setText(QStringLiteral("--:--"));
        }
        return;
    }

    m_currentTitle->setText(current.title);
    m_currentArtist->setText(current.artist);

    QPixmap coverPix = findTrackCoverArt(current, 64);
    QString fallbackText = current.album.isEmpty()
        ? QStringLiteral("\u266B")
        : current.album.left(1).toUpper();
    setCoverArt(m_currentCover, coverPix, 64, 8, fallbackText,
                ThemeManager::instance()->colors().backgroundSecondary);

    auto* durationLabel = m_currentSection->findChild<QLabel*>(
        QStringLiteral("currentDurationLabel"));
    if (durationLabel) {
        durationLabel->setText(formatDuration(current.duration));
    }

    auto* formatLayout = qobject_cast<QHBoxLayout*>(m_currentFormatContainer->layout());
    if (formatLayout) {
        QLayoutItem* child;
        while ((child = formatLayout->takeAt(0)) != nullptr) {
            if (QWidget* w = child->widget()) w->deleteLater();
            delete child;
        }

        m_currentFormat = new FormatBadge(current.format,
                                           current.sampleRate,
                                           current.bitDepth,
                                           current.bitrate,
                                           m_currentFormatContainer);
        formatLayout->addWidget(m_currentFormat);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  updateQueueList
// ═════════════════════════════════════════════════════════════════════

void QueueView::updateQueueList()
{
    if (m_blockRebuild) return;

    // Suspend painting during rebuild
    m_queueListContainer->setUpdatesEnabled(false);
    m_historyListContainer->setUpdatesEnabled(false);

    // Clear existing queue items
    QLayoutItem* child;
    while ((child = m_queueListLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->hide();
            child->widget()->deleteLater();
        }
        delete child;
    }

    // Clear existing history items
    while ((child = m_historyListLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->hide();
            child->widget()->deleteLater();
        }
        delete child;
    }

    const QVector<Track> queue = PlaybackState::instance()->queue();
    const int currentIdx = PlaybackState::instance()->queueIndex();

    // ── Up Next section ───────────────────────────────────────────
    int upNextCount = 0;
    if (currentIdx >= 0) {
        upNextCount = queue.size() - currentIdx - 1;
    } else {
        upNextCount = queue.size();
    }

    m_emptyLabel->setVisible(upNextCount == 0);

    m_queueHeader->setText(
        QStringLiteral("Up Next \u00B7 %1 track%2")
            .arg(upNextCount)
            .arg(upNextCount == 1 ? QStringLiteral("") : QStringLiteral("s")));

    int startIdx = (currentIdx >= 0) ? currentIdx + 1 : 0;
    for (int i = startIdx; i < queue.size(); ++i) {
        auto* item = createQueueItem(queue[i], i, false, false);
        m_queueListLayout->addWidget(item);
    }

    // ── History section ───────────────────────────────────────────
    int historyCount = (currentIdx > 0) ? currentIdx : 0;
    int historyLimit = qMin(historyCount, 20);  // Show at most 20 history items

    m_historyHeader->setVisible(historyLimit > 0);
    m_historyListContainer->setVisible(historyLimit > 0);

    if (historyLimit > 0) {
        m_historyHeader->setText(
            QStringLiteral("History \u00B7 %1 track%2")
                .arg(historyLimit)
                .arg(historyLimit == 1 ? QStringLiteral("") : QStringLiteral("s")));

        // Show most recent history first (reverse order)
        int historyStart = currentIdx - historyLimit;
        if (historyStart < 0) historyStart = 0;
        for (int i = currentIdx - 1; i >= historyStart; --i) {
            auto* item = createQueueItem(queue[i], i, false, true);
            m_historyListLayout->addWidget(item);
        }
    }

    // Resume painting
    m_queueListContainer->setUpdatesEnabled(true);
    m_historyListContainer->setUpdatesEnabled(true);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void QueueView::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 24px; font-weight: bold;")
            .arg(c.foreground));

    m_nowPlayingLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; text-transform: uppercase;"
                       " letter-spacing: 2px;")
            .arg(c.foregroundMuted));

    m_currentSection->setStyleSheet(
        QStringLiteral("QWidget#NowPlayingSection {"
                       "  background-color: transparent;"
                       "  border-radius: 0px;"
                       "  padding: 0px;"
                       "}"));

    m_shuffleBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/shuffle.svg")));
    m_clearBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/trash-2.svg")));

    m_currentCover->setStyleSheet(
        QStringLiteral("QLabel {"
                       "  background-color: %1;"
                       "  border-radius: 8px;"
                       "  color: %2;"
                       "  font-size: 24px;"
                       "}")
            .arg(c.backgroundSecondary, c.foregroundMuted));

    m_currentTitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(c.foreground));
    m_currentArtist->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(c.foregroundMuted));

    auto* durationLabel = m_currentSection->findChild<QLabel*>(
        QStringLiteral("currentDurationLabel"));
    if (durationLabel) {
        durationLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px;")
                .arg(c.foregroundMuted));
    }

    m_queueHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(c.foreground));

    m_historyHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;")
            .arg(c.foreground));

    m_emptyLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(c.foregroundMuted));

    updateQueueList();
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — double-click-to-play + drag-to-reorder
// ═════════════════════════════════════════════════════════════════════

bool QueueView::eventFilter(QObject* obj, QEvent* event)
{
    // ── Paint drop indicator on queue list container ────────────
    if (obj == m_queueListContainer && event->type() == QEvent::Paint && m_dropIndicatorIndex >= 0) {
        // Let the container paint itself first
        QWidget::eventFilter(obj, event);
        int currentIdx = PlaybackState::instance()->queueIndex();
        int localRow = m_dropIndicatorIndex - (currentIdx + 1);
        int y = 0;
        if (localRow >= 0 && localRow < m_queueListLayout->count()) {
            auto* item = m_queueListLayout->itemAt(localRow);
            if (item && item->widget())
                y = item->widget()->geometry().top();
        } else if (localRow >= m_queueListLayout->count() && m_queueListLayout->count() > 0) {
            auto* item = m_queueListLayout->itemAt(m_queueListLayout->count() - 1);
            if (item && item->widget())
                y = item->widget()->geometry().bottom();
        }
        QPainter p(m_queueListContainer);
        p.setPen(QPen(QColor(ThemeManager::instance()->colors().accent), 2));
        p.drawLine(0, y, m_queueListContainer->width(), y);
        p.end();
        return true;
    }

    // ── DragEnter/DragMove/DragLeave on the container itself ────
    if (obj == m_queueListContainer) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasFormat(QStringLiteral("application/x-sorana-queue-index"))) {
                de->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::DragLeave) {
            m_dropIndicatorIndex = -1;
            m_queueListContainer->update();
            // Drag left the container — clear opacity on source widget
            if (m_dragSourceWidget) {
                m_dragSourceWidget->setGraphicsEffect(nullptr);
                m_dragSourceWidget = nullptr;
            }
        }
        return QWidget::eventFilter(obj, event);
    }

    auto* widget = qobject_cast<QWidget*>(obj);
    if (!widget || !widget->property("queueIndex").isValid())
        return QWidget::eventFilter(obj, event);

    int idx = widget->property("queueIndex").toInt();
    bool isHistory = widget->property("isHistory").toBool();
    int currentIdx = PlaybackState::instance()->queueIndex();
    bool isUpcoming = !isHistory && idx > currentIdx;

    switch (event->type()) {

    // ── Double-click to play (Up Next and History) ──────────────
    case QEvent::MouseButtonDblClick: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            // Ignore double-click on remove button area (far-right 40px)
            if (me->position().x() >= widget->width() - 40)
                break;
            const QVector<Track> q = PlaybackState::instance()->queue();
            if (idx >= 0 && idx < q.size()) {
                PlaybackState::instance()->playTrack(q[idx]);
                qDebug() << "[Queue] Double-click play:" << idx;
            }
            return true;
        }
        break;
    }

    // ── Mouse press: start tracking drag for upcoming tracks ────
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && isUpcoming) {
            m_dragStartPos = me->pos();
            m_dragSourceIndex = idx;
        }
        break;
    }

    // ── Mouse move: initiate drag with semi-transparent pixmap ──
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (m_dragSourceIndex >= 0 && (me->buttons() & Qt::LeftButton)) {
            if ((me->pos() - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
                // Capture row as semi-transparent drag pixmap
                QPixmap rowSnap = widget->grab();
                QPixmap transparent(rowSnap.size());
                transparent.fill(Qt::transparent);
                QPainter painter(&transparent);
                painter.setOpacity(0.7);
                painter.drawPixmap(0, 0, rowSnap);
                painter.end();

                // Dim the source row while dragging
                m_dragSourceWidget = widget;
                auto* dimEffect = new QGraphicsOpacityEffect(widget);
                dimEffect->setOpacity(0.3);
                widget->setGraphicsEffect(dimEffect);

                // Start drag
                auto* drag = new QDrag(this);
                auto* mimeData = new QMimeData();
                mimeData->setData(QStringLiteral("application/x-sorana-queue-index"),
                                  QByteArray::number(m_dragSourceIndex));
                drag->setMimeData(mimeData);
                drag->setPixmap(transparent);
                drag->setHotSpot(me->pos());

                Qt::DropAction result = drag->exec(Qt::MoveAction);
                Q_UNUSED(result)

                // Restore source row opacity
                if (m_dragSourceWidget) {
                    m_dragSourceWidget->setGraphicsEffect(nullptr);
                    m_dragSourceWidget = nullptr;
                }
                // Clear drop indicator
                if (m_dropIndicatorIndex >= 0) {
                    m_dropIndicatorIndex = -1;
                    m_queueListContainer->update();
                }
                m_dragSourceIndex = -1;
                return true;
            }
        }
        break;
    }

    case QEvent::MouseButtonRelease: {
        m_dragSourceIndex = -1;
        break;
    }

    // ── Drag enter: accept sorana queue drags ────────────────────
    case QEvent::DragEnter: {
        auto* de = static_cast<QDragEnterEvent*>(event);
        if (de->mimeData()->hasFormat(QStringLiteral("application/x-sorana-queue-index"))) {
            de->acceptProposedAction();
            return true;
        }
        break;
    }

    // ── Drag move: update drop indicator position ───────────────
    case QEvent::DragMove: {
        auto* de = static_cast<QDragMoveEvent*>(event);
        if (de->mimeData()->hasFormat(QStringLiteral("application/x-sorana-queue-index"))) {
            // Calculate which half of the widget the cursor is over
            int halfH = widget->height() / 2;
            int localY = static_cast<int>(de->position().y());
            int targetIdx = (localY < halfH) ? idx : idx + 1;
            if (m_dropIndicatorIndex != targetIdx) {
                m_dropIndicatorIndex = targetIdx;
                m_queueListContainer->update();
            }
            de->acceptProposedAction();
            return true;
        }
        break;
    }

    // ── Drag leave: clear drop indicator ────────────────────────
    case QEvent::DragLeave: {
        m_dropIndicatorIndex = -1;
        m_queueListContainer->update();
        break;
    }

    // ── Drop: perform reorder ───────────────────────────────────
    case QEvent::Drop: {
        auto* de = static_cast<QDropEvent*>(event);
        if (de->mimeData()->hasFormat(QStringLiteral("application/x-sorana-queue-index"))) {
            int fromIdx = de->mimeData()->data(QStringLiteral("application/x-sorana-queue-index")).toInt();
            // Use drop indicator position if available, otherwise fall back to widget index
            int toIdx = (m_dropIndicatorIndex >= 0) ? m_dropIndicatorIndex : idx;
            // Adjust: if dropping below the source, account for removal shift
            if (toIdx > fromIdx) toIdx--;
            m_dropIndicatorIndex = -1;

            // Clear opacity effect BEFORE rebuild destroys the widget.
            // drag->exec() runs a nested event loop where deleteLater()
            // can fire, so we must null this out here — the post-drag
            // cleanup in MouseMove will see nullptr and skip.
            if (m_dragSourceWidget) {
                m_dragSourceWidget->setGraphicsEffect(nullptr);
                m_dragSourceWidget = nullptr;
            }

            if (fromIdx != toIdx && toIdx > currentIdx) {
                m_blockRebuild = true;
                PlaybackState::instance()->moveTo(fromIdx, toIdx);
                m_blockRebuild = false;
                // Force rebuild after blockRebuild is cleared
                updateQueueList();
                qDebug() << "[Queue] Drag reorder:" << fromIdx << "->" << toIdx;
            }
            de->acceptProposedAction();
            return true;
        }
        break;
    }

    default:
        break;
    }

    return QWidget::eventFilter(obj, event);
}
