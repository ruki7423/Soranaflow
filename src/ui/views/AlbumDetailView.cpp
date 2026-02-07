#include "AlbumDetailView.h"
#include <QFrame>
#include <QRandomGenerator>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QResizeEvent>
#include <QtConcurrent>
#include <QFutureWatcher>
#include "../../core/ThemeManager.h"
#include "../../core/audio/MetadataReader.h"
#include "../../core/library/LibraryDatabase.h"
#include "../../metadata/MetadataService.h"
#include "../dialogs/MetadataSearchDialog.h"

// ── Constructor ────────────────────────────────────────────────────
AlbumDetailView::AlbumDetailView(QWidget* parent)
    : QWidget(parent)
    , m_heroBackground(nullptr)
    , m_heroSection(nullptr)
    , m_coverLabel(nullptr)
    , m_titleLabel(nullptr)
    , m_artistLabel(nullptr)
    , m_yearLabel(nullptr)
    , m_statsLabel(nullptr)
    , m_formatBadge(nullptr)
    , m_formatContainer(nullptr)
    , m_playAllBtn(nullptr)
    , m_shuffleBtn(nullptr)
    , m_addQueueBtn(nullptr)
    , m_trackTable(nullptr)
    , m_backBtn(nullptr)
    , m_scrollArea(nullptr)
    , m_mainLayout(nullptr)
{
    setupUI();

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &AlbumDetailView::refreshTheme);
}

// ── setupUI ────────────────────────────────────────────────────────
void AlbumDetailView::setupUI()
{
    setObjectName(QStringLiteral("AlbumDetailView"));

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);

    auto* scrollContent = new QWidget(m_scrollArea);
    scrollContent->setObjectName(QStringLiteral("AlbumDetailContent"));
    m_mainLayout = new QVBoxLayout(scrollContent);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    // ── Hero background (full-width blurred album art) ──────────
    m_heroBackground = new QLabel(scrollContent);
    m_heroBackground->setFixedHeight(300);
    m_heroBackground->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_heroBackground->setScaledContents(false);
    m_heroBackground->setAlignment(Qt::AlignCenter);
    m_heroBackground->setVisible(false);
    {
        auto c = ThemeManager::instance()->colors();
        m_heroBackground->setStyleSheet(
            QStringLiteral("background: %1; border-bottom-left-radius: 12px; border-bottom-right-radius: 12px;")
                .arg(c.backgroundSecondary));
    }
    m_mainLayout->addWidget(m_heroBackground);

    // ── Back button ────────────────────────────────────────────────
    auto* backRow = new QWidget(scrollContent);
    auto* backRowLayout = new QHBoxLayout(backRow);
    backRowLayout->setContentsMargins(24, 16, 0, 0);
    backRowLayout->setAlignment(Qt::AlignLeft);

    m_backBtn = new StyledButton(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/chevron-left.svg")),
                                 QString(), QStringLiteral("ghost"), backRow);
    m_backBtn->setFixedSize(32, 32);
    m_backBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_backBtn->setToolTip(QStringLiteral("Back to albums"));
    backRowLayout->addWidget(m_backBtn);

    connect(m_backBtn, &QPushButton::clicked, this, [this]() {
        emit backRequested();
    });

    m_mainLayout->addWidget(backRow);

    // ── Hero section ───────────────────────────────────────────────
    m_heroSection = new QWidget(scrollContent);
    m_heroSection->setObjectName(QStringLiteral("AlbumHeroSection"));
    m_heroSection->setMinimumHeight(280);
    m_heroSection->setStyleSheet(
        QStringLiteral("#AlbumHeroSection { background: transparent; }"));

    auto* heroLayout = new QHBoxLayout(m_heroSection);
    heroLayout->setContentsMargins(24, 0, 24, 24);
    heroLayout->setSpacing(24);
    heroLayout->setAlignment(Qt::AlignTop);

    // ── Left: Album cover ──────────────────────────────────────────
    m_coverLabel = new QLabel(m_heroSection);
    m_coverLabel->setFixedSize(240, 240);
    m_coverLabel->setAlignment(Qt::AlignCenter);
    {
        auto c = ThemeManager::instance()->colors();
        m_coverLabel->setStyleSheet(
            QStringLiteral("background-color: %1; border-radius: 12px; "
            "font-size: 64px; font-weight: bold; color: %2;")
                .arg(c.backgroundSecondary, c.foregroundMuted));
    }
    heroLayout->addWidget(m_coverLabel, 0, Qt::AlignTop);

    // ── Right: Info column ─────────────────────────────────────────
    auto* infoColumn = new QWidget(m_heroSection);
    auto* infoLayout = new QVBoxLayout(infoColumn);
    infoLayout->setContentsMargins(0, 8, 0, 0);
    infoLayout->setSpacing(8);

    // "ALBUM" label
    auto* albumTypeLabel = new QLabel(QStringLiteral("ALBUM"), infoColumn);
    albumTypeLabel->setStyleSheet(
        QStringLiteral("font-size: 11px; color: ") + ThemeManager::instance()->colors().foregroundMuted
        + QStringLiteral("; letter-spacing: 2px; "
        "text-transform: uppercase; font-weight: bold;"));
    infoLayout->addWidget(albumTypeLabel);

    // Title
    m_titleLabel = new QLabel(infoColumn);
    m_titleLabel->setStyleSheet(
        QStringLiteral("font-size: 36px; font-weight: bold; color: ")
        + ThemeManager::instance()->colors().foreground + QStringLiteral(";"));
    m_titleLabel->setWordWrap(true);
    infoLayout->addWidget(m_titleLabel);

    // Artist (clickable)
    m_artistLabel = new QLabel(infoColumn);
    m_artistLabel->setStyleSheet(QStringLiteral(
        "font-size: 18px; color: %1; font-weight: 500;").arg(ThemeManager::instance()->colors().accent));
    m_artistLabel->setCursor(Qt::PointingHandCursor);
    infoLayout->addWidget(m_artistLabel);

    // Year + track count + duration row
    m_yearLabel = new QLabel(infoColumn);
    m_yearLabel->setStyleSheet(
        QStringLiteral("font-size: 14px; color: ")
        + ThemeManager::instance()->colors().foregroundMuted + QStringLiteral(";"));
    infoLayout->addWidget(m_yearLabel);

    // Format badge container
    m_formatContainer = new QWidget(infoColumn);
    auto* formatLayout = new QHBoxLayout(m_formatContainer);
    formatLayout->setContentsMargins(0, 0, 0, 0);
    formatLayout->setSpacing(8);
    formatLayout->setAlignment(Qt::AlignLeft);
    m_formatBadge = nullptr;
    infoLayout->addWidget(m_formatContainer);

    // Stats (genre info)
    m_statsLabel = new QLabel(infoColumn);
    m_statsLabel->setStyleSheet(
        QStringLiteral("font-size: 13px; color: ")
        + ThemeManager::instance()->colors().foregroundMuted + QStringLiteral(";"));
    infoLayout->addWidget(m_statsLabel);

    // ── Action buttons row ─────────────────────────────────────────
    auto* actionsRow = new QHBoxLayout;
    actionsRow->setSpacing(12);
    actionsRow->setContentsMargins(0, 8, 0, 0);
    actionsRow->setAlignment(Qt::AlignLeft);

    const int DETAIL_BTN_H = 36;
    const QSize DETAIL_ICON(16, 16);

    m_playAllBtn = new StyledButton(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/play.svg")),
                                    QStringLiteral("Play All"),
                                    QStringLiteral("default"), infoColumn);
    m_playAllBtn->setIconSize(DETAIL_ICON);
    m_playAllBtn->setFixedHeight(DETAIL_BTN_H);
    actionsRow->addWidget(m_playAllBtn);

    m_shuffleBtn = new StyledButton(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/shuffle.svg")),
                                    QStringLiteral("Shuffle"),
                                    QStringLiteral("outline"), infoColumn);
    m_shuffleBtn->setIconSize(DETAIL_ICON);
    m_shuffleBtn->setFixedHeight(DETAIL_BTN_H);
    actionsRow->addWidget(m_shuffleBtn);

    m_addQueueBtn = new StyledButton(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/plus.svg")),
                                     QStringLiteral("Add to Queue"),
                                     QStringLiteral("ghost"), infoColumn);
    m_addQueueBtn->setIconSize(DETAIL_ICON);
    m_addQueueBtn->setFixedHeight(DETAIL_BTN_H);
    actionsRow->addWidget(m_addQueueBtn);

    infoLayout->addLayout(actionsRow);
    infoLayout->addStretch();

    heroLayout->addWidget(infoColumn, 1);
    m_mainLayout->addWidget(m_heroSection);

    // ── Separator line ─────────────────────────────────────────────
    auto* separator = new QFrame(scrollContent);
    separator->setFrameShape(QFrame::HLine);
    separator->setStyleSheet(
        QStringLiteral("background-color: %1; max-height: 1px; border: none;")
            .arg(ThemeManager::instance()->colors().borderSubtle));
    m_mainLayout->addWidget(separator);

    // ── Track table (embedded, no Album column) ───────────────────
    m_trackTable = new TrackTableView(albumDetailConfig(), scrollContent);
    m_trackTable->setEmbeddedMode(true);
    m_mainLayout->addWidget(m_trackTable);

    m_mainLayout->addStretch();

    m_scrollArea->setWidget(scrollContent);
    outerLayout->addWidget(m_scrollArea);

    // ── Artist click connection ────────────────────────────────────
    m_artistLabel->installEventFilter(this);
}

// ── setAlbum ───────────────────────────────────────────────────────
void AlbumDetailView::setAlbum(const QString& albumId)
{
    m_album = MusicDataProvider::instance()->albumById(albumId);
    updateDisplay();
}

// ── updateDisplay ──────────────────────────────────────────────────
void AlbumDetailView::updateDisplay()
{
    // ── Update cover art ────────────────────────────────────────────
    loadCoverArt();

    // ── Update text labels ─────────────────────────────────────────
    m_titleLabel->setText(m_album.title);
    m_artistLabel->setText(m_album.artist);

    // Year + track count + total duration
    int totalDuration = m_album.duration;
    int trackCount = m_album.totalTracks;
    QString yearInfo;
    if (m_album.year > 0)
        yearInfo += QString::number(m_album.year) + QStringLiteral("  \u00B7  ");
    yearInfo += QString::number(trackCount)
              + QStringLiteral(" tracks  \u00B7  ")
              + formatDuration(totalDuration);
    m_yearLabel->setText(yearInfo);

    // ── Update format badge ────────────────────────────────────────
    auto* formatLayout = qobject_cast<QHBoxLayout*>(m_formatContainer->layout());
    if (formatLayout) {
        QLayoutItem* item;
        while ((item = formatLayout->takeAt(0)) != nullptr) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }

        m_formatBadge = new FormatBadge(m_album.format, QString(), QString(),
                                        QString(), m_formatContainer);
        formatLayout->addWidget(m_formatBadge);
        formatLayout->addStretch();
    }

    // ── Stats / genre info ─────────────────────────────────────────
    if (!m_album.genres.isEmpty()) {
        m_statsLabel->setText(m_album.genres.join(QStringLiteral(", ")));
        m_statsLabel->setVisible(true);
    } else {
        m_statsLabel->setVisible(false);
    }

    // ── Update track table ─────────────────────────────────────────
    m_trackTable->setTracks(m_album.tracks);

    // ── Reconnect track table signals ──────────────────────────────
    disconnect(m_trackTable, &TrackTableView::trackDoubleClicked, nullptr, nullptr);
    connect(m_trackTable, &TrackTableView::trackDoubleClicked, this, [this](const Track& t) {
        PlaybackState::instance()->setQueue(m_album.tracks);
        PlaybackState::instance()->playTrack(t);
    });

    disconnect(m_trackTable, &TrackTableView::fixMetadataRequested, nullptr, nullptr);
    connect(m_trackTable, &TrackTableView::fixMetadataRequested, this, [this](const Track& t) {
        auto* dlg = new MetadataSearchDialog(t, this);
        connect(dlg, &QDialog::accepted, this, [this, dlg, t]() {
            MusicBrainzResult result = dlg->selectedResult();
            Track updated = t;
            if (!result.title.isEmpty())  updated.title  = result.title;
            if (!result.artist.isEmpty()) updated.artist = result.artist;
            if (!result.album.isEmpty())  updated.album  = result.album;
            if (result.trackNumber > 0)   updated.trackNumber = result.trackNumber;
            if (result.discNumber > 0)    updated.discNumber  = result.discNumber;
            if (!result.mbid.isEmpty())             updated.recordingMbid    = result.mbid;
            if (!result.artistMbid.isEmpty())       updated.artistMbid       = result.artistMbid;
            if (!result.albumMbid.isEmpty())        updated.albumMbid        = result.albumMbid;
            if (!result.releaseGroupMbid.isEmpty()) updated.releaseGroupMbid = result.releaseGroupMbid;

            auto* db = LibraryDatabase::instance();
            db->backupTrackMetadata(t.id);
            db->updateTrack(updated);
            db->rebuildAlbumsAndArtists();

            if (!result.releaseGroupMbid.isEmpty())
                MetadataService::instance()->fetchAlbumArt(result.releaseGroupMbid, true);
            else if (!result.albumMbid.isEmpty())
                MetadataService::instance()->fetchAlbumArt(result.albumMbid, false);
            if (!result.artistMbid.isEmpty())
                MetadataService::instance()->fetchArtistImages(result.artistMbid);

            MusicDataProvider::instance()->reloadFromDatabase();
        });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });

    disconnect(m_trackTable, &TrackTableView::undoMetadataRequested, nullptr, nullptr);
    connect(m_trackTable, &TrackTableView::undoMetadataRequested, this, [this](const Track&) {
        LibraryDatabase::instance()->rebuildAlbumsAndArtists();
        MusicDataProvider::instance()->reloadFromDatabase();
    });

    disconnect(m_trackTable, &TrackTableView::identifyByAudioRequested, nullptr, nullptr);
    connect(m_trackTable, &TrackTableView::identifyByAudioRequested, this, [this](const Track& t) {
        MetadataService::instance()->identifyByFingerprint(t);
    });

    // ── Reconnect action buttons ───────────────────────────────────
    disconnect(m_playAllBtn, nullptr, nullptr, nullptr);
    disconnect(m_shuffleBtn, nullptr, nullptr, nullptr);
    disconnect(m_addQueueBtn, nullptr, nullptr, nullptr);

    connect(m_playAllBtn, &QPushButton::clicked, this, [this]() {
        if (!m_album.tracks.isEmpty()) {
            PlaybackState::instance()->setQueue(m_album.tracks);
            PlaybackState::instance()->playTrack(m_album.tracks.first());
        }
    });

    connect(m_shuffleBtn, &QPushButton::clicked, this, [this]() {
        if (!m_album.tracks.isEmpty()) {
            PlaybackState::instance()->setQueue(m_album.tracks);
            if (!PlaybackState::instance()->shuffleEnabled()) {
                PlaybackState::instance()->toggleShuffle();
            }
            int randomIndex = QRandomGenerator::global()->bounded(m_album.tracks.size());
            PlaybackState::instance()->playTrack(m_album.tracks[randomIndex]);
        }
    });

    connect(m_addQueueBtn, &QPushButton::clicked, this, [this]() {
        for (const auto& track : m_album.tracks) {
            PlaybackState::instance()->addToQueue(track);
        }
    });
}

// ── loadCoverArt ────────────────────────────────────────────────────
void AlbumDetailView::loadCoverArt()
{
    constexpr int sz = 240;
    constexpr int radius = 12;

    // Show placeholder immediately — no UI freeze
    m_heroSourcePixmap = QPixmap();
    m_heroBackground->setVisible(false);
    m_coverLabel->setPixmap(QPixmap());
    m_coverLabel->setText(m_album.title.left(1).toUpper());
    {
        auto c = ThemeManager::instance()->colors();
        m_coverLabel->setStyleSheet(
            QStringLiteral("background-color: %1; border-radius: 12px; "
            "font-size: 64px; font-weight: bold; color: %2;")
                .arg(c.backgroundSecondary, c.foregroundMuted));
    }

    // Gather data for background thread
    QString coverUrl = m_album.coverUrl;
    QString firstTrackPath;
    for (const auto& t : m_album.tracks) {
        if (!t.filePath.isEmpty()) {
            firstTrackPath = t.filePath;
            break;
        }
    }

    if (coverUrl.isEmpty() && firstTrackPath.isEmpty())
        return; // Nothing to load

    // Invalidate any previous in-flight loads
    int loadId = ++m_coverLoadId;

    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, loadId, sz, radius]() {
        watcher->deleteLater();
        if (loadId != m_coverLoadId) return; // Album changed — discard

        QImage img = watcher->result();
        if (img.isNull()) return; // Keep placeholder

        QPixmap pix = QPixmap::fromImage(img);
        m_heroSourcePixmap = pix;
        applyHeroBackground(pix);

        // Scale and crop to square
        QPixmap scaled = pix.scaled(sz, sz, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        if (scaled.width() > sz || scaled.height() > sz) {
            int x = (scaled.width() - sz) / 2;
            int y = (scaled.height() - sz) / 2;
            scaled = scaled.copy(x, y, sz, sz);
        }
        // Apply rounded corners
        QPixmap rounded(sz, sz);
        rounded.fill(Qt::transparent);
        QPainter painter(&rounded);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath clipPath;
        clipPath.addRoundedRect(0, 0, sz, sz, radius, radius);
        painter.setClipPath(clipPath);
        painter.drawPixmap(0, 0, scaled);
        painter.end();

        m_coverLabel->setPixmap(rounded);
        m_coverLabel->setStyleSheet(QStringLiteral("background: transparent; border-radius: 12px;"));
    });

    // Run 4-tier cover lookup on background thread (returns QImage)
    watcher->setFuture(QtConcurrent::run([coverUrl, firstTrackPath]() -> QImage {
        QImage img;

        // Tier 1: coverUrl
        if (!coverUrl.isEmpty()) {
            QString loadPath = coverUrl;
            if (loadPath.startsWith(QStringLiteral("qrc:")))
                loadPath = loadPath.mid(3);
            if (QFile::exists(loadPath))
                img.load(loadPath);
        }

        // Tier 2: folder image files
        if (img.isNull() && !firstTrackPath.isEmpty()) {
            QString folder = QFileInfo(firstTrackPath).absolutePath();
            static const QStringList names = {
                QStringLiteral("cover.jpg"),  QStringLiteral("cover.png"),
                QStringLiteral("folder.jpg"), QStringLiteral("folder.png"),
                QStringLiteral("album.jpg"),  QStringLiteral("album.png"),
                QStringLiteral("front.jpg"),  QStringLiteral("front.png"),
                QStringLiteral("Cover.jpg"),  QStringLiteral("Cover.png"),
                QStringLiteral("Folder.jpg"), QStringLiteral("Front.jpg")
            };
            for (const QString& n : names) {
                QString p = folder + QStringLiteral("/") + n;
                if (QFile::exists(p)) {
                    img.load(p);
                    if (!img.isNull()) break;
                }
            }
        }

        // Tier 3: embedded cover via FFmpeg
        if (img.isNull() && !firstTrackPath.isEmpty()) {
            QPixmap pix = MetadataReader::extractCoverArt(firstTrackPath);
            if (!pix.isNull()) img = pix.toImage();
        }

        // Tier 4: any image file in the folder
        if (img.isNull() && !firstTrackPath.isEmpty()) {
            QString folder = QFileInfo(firstTrackPath).absolutePath();
            QDir dir(folder);
            QStringList imageFilters = {
                QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                QStringLiteral("*.png"), QStringLiteral("*.bmp")
            };
            QStringList images = dir.entryList(imageFilters, QDir::Files, QDir::Name);
            if (!images.isEmpty())
                img.load(dir.filePath(images.first()));
        }

        return img;
    }));
}

// ── eventFilter (for clickable artist label) ───────────────────────
bool AlbumDetailView::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_artistLabel && event->type() == QEvent::MouseButtonPress) {
        if (!m_album.artistId.isEmpty()) {
            emit artistClicked(m_album.artistId);
        }
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

// ── applyHeroBackground ─────────────────────────────────────────────
void AlbumDetailView::applyHeroBackground(const QPixmap& pix)
{
    if (pix.isNull()) {
        m_heroBackground->setVisible(false);
        return;
    }

    int heroW = m_heroBackground->width();
    if (heroW < 400) heroW = width();
    if (heroW < 400 && m_scrollArea) heroW = m_scrollArea->viewport()->width();
    if (heroW < 400) heroW = 1200;
    constexpr int heroH = 300;

    // Scale down to create blur seed, then scale back up
    QPixmap small = pix.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QPixmap blurred = small.scaled(heroW, heroH, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    int cx = qMax(0, (blurred.width() - heroW) / 2);
    int cy = qMax(0, (blurred.height() - heroH) / 2);
    QPixmap cropped = blurred.copy(cx, cy, heroW, heroH);

    // Dark gradient overlay for readability
    QPainter p(&cropped);
    QLinearGradient grad(0, 0, 0, heroH);
    grad.setColorAt(0, QColor(0, 0, 0, 0));
    grad.setColorAt(0.5, QColor(0, 0, 0, 80));
    grad.setColorAt(1, QColor(0, 0, 0, 180));
    p.fillRect(cropped.rect(), grad);
    p.end();

    m_heroBackground->setPixmap(cropped);
    m_heroBackground->setVisible(true);
}

// ── resizeEvent ────────────────────────────────────────────────────
void AlbumDetailView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_heroBackground && m_heroBackground->isVisible() && !m_heroSourcePixmap.isNull()) {
        applyHeroBackground(m_heroSourcePixmap);
    }
}

// ── refreshTheme ────────────────────────────────────────────────────
void AlbumDetailView::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    m_heroBackground->setStyleSheet(
        QStringLiteral("background: %1; border-bottom-left-radius: 12px; border-bottom-right-radius: 12px;")
            .arg(c.backgroundSecondary));

    m_heroSection->setStyleSheet(
        QStringLiteral("#AlbumHeroSection { background: transparent; }"));

    m_coverLabel->setStyleSheet(
        QStringLiteral("background-color: %1; border-radius: 12px; "
        "font-size: 64px; font-weight: bold; color: %2;")
            .arg(c.backgroundSecondary, c.foregroundMuted));

    m_titleLabel->setStyleSheet(
        QStringLiteral("font-size: 36px; font-weight: bold; color: %1;")
            .arg(c.foreground));

    m_artistLabel->setStyleSheet(
        QStringLiteral("QLabel { font-size: 18px; color: %1; font-weight: 500; }"
                       "QLabel:hover { color: %2; }")
            .arg(c.accent, c.accentHover));

    m_yearLabel->setStyleSheet(
        QStringLiteral("font-size: 14px; color: %1;")
            .arg(c.foregroundMuted));

    m_statsLabel->setStyleSheet(
        QStringLiteral("font-size: 13px; color: %1;")
            .arg(c.foregroundMuted));

    m_backBtn->setIcon(tm->themedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_playAllBtn->setIcon(tm->themedIcon(QStringLiteral(":/icons/play.svg")));
    m_shuffleBtn->setIcon(tm->themedIcon(QStringLiteral(":/icons/shuffle.svg")));
    m_addQueueBtn->setIcon(tm->themedIcon(QStringLiteral(":/icons/plus.svg")));

    // Re-apply hero background and cover art if album is loaded
    if (!m_album.id.isEmpty())
        loadCoverArt();
}
