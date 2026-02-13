#include "AlbumsView.h"

#include "../MainWindow.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include "../../core/ThemeManager.h"
#include "../../core/PlaybackState.h"
#include "../../core/audio/MetadataReader.h"
#include "../../core/library/LibraryDatabase.h"
#include "../../metadata/CoverArtProvider.h"
#include <QtConcurrent>

// ═══════════════════════════════════════════════════════════════════
//  Constructor
// ═══════════════════════════════════════════════════════════════════
AlbumsView::AlbumsView(QWidget* parent)
    : QWidget(parent)
{
    setupUI();

    m_resizeDebounceTimer = new QTimer(this);
    m_resizeDebounceTimer->setSingleShot(true);
    m_resizeDebounceTimer->setInterval(150);
    connect(m_resizeDebounceTimer, &QTimer::timeout, this, &AlbumsView::relayoutGrid);

    // Library change: defer if invisible
    connect(MusicDataProvider::instance(), &MusicDataProvider::libraryUpdated,
            this, [this]() {
                if (!isVisible()) { m_libraryDirty = true; return; }
                m_coverCache.clear();
                reloadAlbums();
            });

    // Theme change: rebuild cards using cached covers (no re-extraction)
    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &AlbumsView::reloadAlbums);

    // Deferred initial load
    QTimer::singleShot(300, this, &AlbumsView::reloadAlbums);
}

// ═══════════════════════════════════════════════════════════════════
//  setupUI — build the layout once
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::setupUI()
{
    setObjectName(QStringLiteral("AlbumsView"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    // ── Header row — unified toolbar (30px buttons, 8px spacing) ────
    const int NAV_SIZE = 30;

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(8);

    auto c = ThemeManager::instance()->colors();

    m_headerLabel = new QLabel(QStringLiteral("Albums"), this);
    m_headerLabel->setStyleSheet(QStringLiteral(
        "font-size: 24px; font-weight: bold; color: %1;")
            .arg(c.foreground));
    headerRow->addWidget(m_headerLabel);

    m_countLabel = new QLabel(QStringLiteral("0 albums"), this);
    m_countLabel->setStyleSheet(QStringLiteral(
        "font-size: 14px; color: %1; padding-top: 6px;")
            .arg(c.foregroundMuted));
    headerRow->addWidget(m_countLabel);

    // ── Global navigation ← → ─────────────────────────────────────
    headerRow->addSpacing(4);

    m_navBackBtn = new QPushButton(this);
    m_navBackBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_navBackBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_navBackBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navBackBtn->setCursor(Qt::PointingHandCursor);
    m_navBackBtn->setToolTip(QStringLiteral("Back"));
    m_navBackBtn->setFocusPolicy(Qt::NoFocus);
    headerRow->addWidget(m_navBackBtn);

    m_navForwardBtn = new QPushButton(this);
    m_navForwardBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));
    m_navForwardBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_navForwardBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navForwardBtn->setCursor(Qt::PointingHandCursor);
    m_navForwardBtn->setToolTip(QStringLiteral("Forward"));
    m_navForwardBtn->setFocusPolicy(Qt::NoFocus);
    headerRow->addWidget(m_navForwardBtn);

    headerRow->addStretch();

    // View mode buttons
    const QString viewBtnBase = QStringLiteral(
        "  border: none; border-radius: 4px; padding: 0px;"
        "  min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px;");
    auto viewBtnStyle = [&c, &viewBtnBase](bool active) -> QString {
        if (active) {
            return QStringLiteral("QPushButton { background: %1;").arg(c.accent) + viewBtnBase + QStringLiteral("}"
                "QPushButton:hover { background: %1; }").arg(c.accentHover);
        }
        return QStringLiteral("QPushButton { background: transparent;") + viewBtnBase + QStringLiteral("}"
            "QPushButton:hover { background: %1; }").arg(c.hover);
    };

    m_largeIconBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), this);
    m_largeIconBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/grid-2x2.svg")));
    m_largeIconBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_largeIconBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_largeIconBtn->setToolTip(QStringLiteral("Large Icons"));
    m_largeIconBtn->setStyleSheet(viewBtnStyle(true));
    headerRow->addWidget(m_largeIconBtn);

    m_smallIconBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), this);
    m_smallIconBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/grid-3x3.svg")));
    m_smallIconBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_smallIconBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_smallIconBtn->setToolTip(QStringLiteral("Small Icons"));
    m_smallIconBtn->setStyleSheet(viewBtnStyle(false));
    headerRow->addWidget(m_smallIconBtn);

    m_listBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), this);
    m_listBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/list.svg")));
    m_listBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_listBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_listBtn->setToolTip(QStringLiteral("List"));
    m_listBtn->setStyleSheet(viewBtnStyle(false));
    headerRow->addWidget(m_listBtn);

    connect(m_largeIconBtn, &QPushButton::clicked, this, [this]() { setViewMode(LargeIcons); });
    connect(m_smallIconBtn, &QPushButton::clicked, this, [this]() { setViewMode(SmallIcons); });
    connect(m_listBtn, &QPushButton::clicked, this, [this]() { setViewMode(ListView); });

    auto updateNavBtnStyle = [this]() {
        auto* mw = MainWindow::instance();
        auto c = ThemeManager::instance()->colors();
        auto navStyle = [&c](bool enabled) {
            Q_UNUSED(enabled)
            return QStringLiteral(
                "QPushButton { background: transparent; border: none; border-radius: 4px; }"
                "QPushButton:hover { background: %1; }"
                "QPushButton:disabled { background: transparent; }").arg(c.hover);
        };
        bool canBack = mw && mw->canGoBack();
        bool canFwd = mw && mw->canGoForward();
        m_navBackBtn->setEnabled(canBack);
        m_navForwardBtn->setEnabled(canFwd);
        m_navBackBtn->setStyleSheet(navStyle(canBack));
        m_navForwardBtn->setStyleSheet(navStyle(canFwd));
    };
    updateNavBtnStyle();

    connect(m_navBackBtn, &QPushButton::clicked, this, []() {
        if (auto* mw = MainWindow::instance()) mw->navigateBack();
    });
    connect(m_navForwardBtn, &QPushButton::clicked, this, []() {
        if (auto* mw = MainWindow::instance()) mw->navigateForward();
    });
    connect(MainWindow::instance(), &MainWindow::globalNavChanged,
            this, updateNavBtnStyle);

    mainLayout->addLayout(headerRow);

    // ── Inline filter ───────────────────────────────────────────────
    m_filterInput = new StyledInput(QStringLiteral("Filter albums..."), QString(), this);
    m_filterInput->setFixedHeight(32);
    m_filterDebounceTimer = new QTimer(this);
    m_filterDebounceTimer->setSingleShot(true);
    m_filterDebounceTimer->setInterval(200);
    connect(m_filterDebounceTimer, &QTimer::timeout, this, [this]() {
        onFilterChanged(m_filterInput->lineEdit()->text());
    });
    connect(m_filterInput->lineEdit(), &QLineEdit::textChanged,
            this, [this]() { m_filterDebounceTimer->start(); });
    m_filterInput->lineEdit()->installEventFilter(this);
    mainLayout->addWidget(m_filterInput);

    // ── Scroll area + grid ──────────────────────────────────────────
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; }") + ThemeManager::instance()->scrollbarStyle());

    m_gridContainer = new QWidget();
    m_gridContainer->setFocusPolicy(Qt::NoFocus);
    m_gridContainer->setStyleSheet(QStringLiteral("background: transparent;"));

    m_gridLayout = new QGridLayout(m_gridContainer);
    m_gridLayout->setSpacing(20);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    m_scrollArea->setWidget(m_gridContainer);
    mainLayout->addWidget(m_scrollArea, 1);
}

// ═══════════════════════════════════════════════════════════════════
//  clearGrid — remove all cards
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::clearGrid()
{
    // Remove items from layout first
    while (m_gridLayout->count() > 0) {
        QLayoutItem* item = m_gridLayout->takeAt(0);
        delete item;  // just the layout item, not the widget
    }
    // Delete all card widgets
    qDeleteAll(m_cards);
    m_cards.clear();
}

// ═══════════════════════════════════════════════════════════════════
//  reloadAlbums — the main data-loading entry point
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::reloadAlbums()
{
    // Flag-based debounce — queue if busy, never drop
    static bool isReloading = false;
    static bool pendingReload = false;
    if (isReloading) {
        pendingReload = true;
        return;
    }
    isReloading = true;
    pendingReload = false;

    // Refresh view toggle icons for current theme
    {
        auto* tmIcons = ThemeManager::instance();
        m_largeIconBtn->setIcon(tmIcons->cachedIcon(QStringLiteral(":/icons/grid-2x2.svg")));
        m_smallIconBtn->setIcon(tmIcons->cachedIcon(QStringLiteral(":/icons/grid-3x3.svg")));
        m_listBtn->setIcon(tmIcons->cachedIcon(QStringLiteral(":/icons/list.svg")));
        m_navBackBtn->setIcon(tmIcons->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
        m_navForwardBtn->setIcon(tmIcons->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));
        setViewMode(m_viewMode);
    }

    // ── Step 1: Get albums from MusicDataProvider ────────────────────
    QVector<Album> albums = MusicDataProvider::instance()->allAlbums();

    // ── Step 2: If provider is empty, try database directly ─────────
    if (albums.isEmpty()) {
        auto* db = LibraryDatabase::instance();
        if (db) {
            albums = db->allAlbums();
        }
    }

    // Single allTracks() fetch — reused for albumMap + trackPaths
    const QVector<Track> tracks = MusicDataProvider::instance()->allTracks();

    // ── Step 3: If still empty, build from tracks as last resort ────
    if (albums.isEmpty()) {
        QMap<QString, Album> albumMap;
        for (const Track& t : tracks) {
            if (t.album.isEmpty()) continue;

            QString key = t.album.toLower() + QStringLiteral("||") + t.artist.toLower();
            if (!albumMap.contains(key)) {
                Album a;
                a.id          = t.albumId.isEmpty()
                    ? QStringLiteral("synth_") + QString::number(qHash(key))
                    : t.albumId;
                a.title       = t.album;
                a.artist      = t.artist;
                a.artistId    = t.artistId;
                a.coverUrl    = t.coverUrl;
                a.format      = t.format;
                a.totalTracks = 0;
                a.duration    = 0;
                albumMap[key] = a;
            }
            albumMap[key].totalTracks++;
            albumMap[key].duration += t.duration;
            albumMap[key].tracks.append(t);
        }
        albums = albumMap.values().toVector();
    }

    // Build albumId → firstTrackPath map for cover art discovery
    // (allAlbums() returns albums without tracks to save memory)
    m_albumTrackPaths.clear();
    {
        // Fast path: match by albumId (populated after rebuildAlbumsAndArtists)
        for (const auto& t : tracks) {
            if (!t.albumId.isEmpty() && !t.filePath.isEmpty()
                && !m_albumTrackPaths.contains(t.albumId)) {
                m_albumTrackPaths[t.albumId] = t.filePath;
            }
        }

        // Fallback: before rebuildAlbumsAndArtists runs, tracks have empty
        // albumId. Match by album title + artist name instead.
        if (m_albumTrackPaths.isEmpty() && !tracks.isEmpty() && !albums.isEmpty()) {
            QHash<QString, QString> nameToId;
            for (const auto& a : albums) {
                QString key = a.title.toLower() + QStringLiteral("||") + a.artist.toLower();
                nameToId[key] = a.id;
            }
            for (const auto& t : tracks) {
                if (t.album.isEmpty() || t.filePath.isEmpty()) continue;
                QString key = t.album.toLower() + QStringLiteral("||") + t.artist.toLower();
                QString aId = nameToId.value(key);
                if (!aId.isEmpty() && !m_albumTrackPaths.contains(aId)) {
                    m_albumTrackPaths[aId] = t.filePath;
                }
            }
        }
    }
    qDebug() << "[AlbumsView] reloadAlbums:" << albums.size() << "albums,"
             << m_albumTrackPaths.size() << "trackPaths, cache:" << m_coverCache.size();

    // Cache albums for re-layout on resize
    m_albums = albums;

    // Update header labels with current theme
    auto c2 = ThemeManager::instance()->colors();
    m_headerLabel->setStyleSheet(QStringLiteral(
        "font-size: 24px; font-weight: bold; color: %1;")
            .arg(c2.foreground));
    m_countLabel->setStyleSheet(QStringLiteral(
        "font-size: 14px; color: %1; padding-top: 6px;")
            .arg(c2.foregroundMuted));
    m_countLabel->setText(QString::number(albums.size()) + QStringLiteral(" albums"));

    // Build cards with responsive layout
    relayoutGrid();

    QTimer::singleShot(500, this, [this]() {
        isReloading = false;
        if (pendingReload) {
            pendingReload = false;
            reloadAlbums();
        }
    });

    // Re-apply filter if active
    if (!m_filterText.isEmpty())
        onFilterChanged(m_filterText);
}

// ═══════════════════════════════════════════════════════════════════
//  onFilterChanged — filter displayed album cards by title/artist
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::onFilterChanged(const QString& text)
{
    m_filterText = text.trimmed().toLower();
    int visibleCount = 0;
    for (int i = 0; i < m_cards.size() && i < m_albums.size(); ++i) {
        bool visible = m_filterText.isEmpty()
            || m_albums[i].title.toLower().contains(m_filterText)
            || m_albums[i].artist.toLower().contains(m_filterText);
        m_cards[i]->setVisible(visible);
        if (visible) visibleCount++;
    }
    m_countLabel->setText(QString::number(visibleCount) + QStringLiteral(" albums"));
}

// ═══════════════════════════════════════════════════════════════════
//  setViewMode
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::setViewMode(ViewMode mode)
{
    m_viewMode = mode;

    auto c = ThemeManager::instance()->colors();
    const QString base = QStringLiteral(
        "  border: none; border-radius: 4px; padding: 0px;"
        "  min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px;");
    auto activeStyle = QStringLiteral("QPushButton { background: %1;").arg(c.accent) + base + QStringLiteral("}"
        "QPushButton:hover { background: %1; }").arg(c.accentHover);
    auto inactiveStyle = QStringLiteral("QPushButton { background: transparent;") + base + QStringLiteral("}"
        "QPushButton:hover { background: %1; }").arg(c.hover);

    m_largeIconBtn->setStyleSheet(mode == LargeIcons ? activeStyle : inactiveStyle);
    m_smallIconBtn->setStyleSheet(mode == SmallIcons ? activeStyle : inactiveStyle);
    m_listBtn->setStyleSheet(mode == ListView ? activeStyle : inactiveStyle);

    relayoutGrid();
}

// ═══════════════════════════════════════════════════════════════════
//  createAlbumListRow — list view row
// ═══════════════════════════════════════════════════════════════════
QWidget* AlbumsView::createAlbumListRow(const Album& album)
{
    auto c = ThemeManager::instance()->colors();
    auto* row = new QWidget(m_gridContainer);
    row->setObjectName(QStringLiteral("AlbumCard"));
    row->setCursor(Qt::PointingHandCursor);
    row->setProperty("albumId", album.id);
    row->setFixedHeight(56);
    row->setFocusPolicy(Qt::NoFocus);
    row->setAttribute(Qt::WA_MacShowFocusRect, false);
    row->setAutoFillBackground(false);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(12);

    // Cover art thumbnail
    auto* coverLabel = new QLabel(row);
    coverLabel->setFixedSize(UISizes::rowHeight, UISizes::rowHeight);
    coverLabel->setAlignment(Qt::AlignCenter);
    coverLabel->setProperty("coverRadius", 6);

    if (m_coverCache.contains(album.id)) {
        const QPixmap& cached = m_coverCache[album.id];
        if (!cached.isNull()) {
            coverLabel->setPixmap(renderRoundedCover(cached, UISizes::rowHeight, 6));
        } else {
            coverLabel->setText(album.title.left(1).toUpper());
            coverLabel->setStyleSheet(QStringLiteral(
                "background: %1; border-radius: 6px; font-size: 18px; font-weight: bold; color: %2;")
                    .arg(c.backgroundSecondary, c.foregroundMuted));
        }
    } else {
        coverLabel->setText(album.title.left(1).toUpper());
        coverLabel->setStyleSheet(QStringLiteral(
            "background: %1; border-radius: 6px; font-size: 18px; font-weight: bold; color: %2;")
                .arg(c.backgroundSecondary, c.foregroundMuted));
        m_coverLabels[album.id] = coverLabel;
    }
    layout->addWidget(coverLabel);

    // Title + artist stacked
    auto* infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(2);
    auto* titleLabel = new QLabel(album.title, row);
    titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; font-weight: bold;")
        .arg(c.foreground));
    infoLayout->addWidget(titleLabel);

    auto* artistLabel = new QLabel(album.artist, row);
    artistLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    infoLayout->addWidget(artistLabel);
    layout->addLayout(infoLayout, 1);

    // Track count
    auto* tracksLabel = new QLabel(
        QString::number(album.totalTracks) + QStringLiteral(" tracks"), row);
    tracksLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    layout->addWidget(tracksLabel);

    row->installEventFilter(this);
    return row;
}

// ═══════════════════════════════════════════════════════════════════
//  relayoutGrid — responsive grid layout
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::relayoutGrid()
{
    m_coverLabels.clear();  // old labels about to be deleted
    clearGrid();

    if (m_albums.isEmpty()) return;

    int availableWidth = m_scrollArea->viewport()->width() - 8;
    if (availableWidth < 200) availableWidth = width() - 48;

    if (m_viewMode == ListView) {
        // List mode: single column
        for (int i = 0; i < m_albums.size(); ++i) {
            QWidget* row = createAlbumListRow(m_albums[i]);
            m_gridLayout->addWidget(row, i, 0);
            m_cards.append(row);
        }
    } else {
        // Grid modes: calculate card width based on view mode
        const int minCardWidth = (m_viewMode == SmallIcons) ? 120 : 160;
        const int spacing = (m_viewMode == SmallIcons) ? UISizes::spacingMD : 20;
        m_gridLayout->setSpacing(spacing);

        int columns = qMax(1, (availableWidth + spacing) / (minCardWidth + spacing));
        int cardWidth = (availableWidth - (columns - 1) * spacing) / columns;
        cardWidth = qMax(cardWidth, minCardWidth);

        int row = 0, col = 0;

        for (const Album& album : m_albums) {
            QWidget* card = createAlbumCard(album, cardWidth);
            m_gridLayout->addWidget(card, row, col);
            m_cards.append(card);

            if (++col >= columns) {
                col = 0;
                row++;
            }
        }
    }

    // Start async cover loading for uncached albums
    if (!m_coverLabels.isEmpty()) {
        m_coverLoadIndex = 0;
        QTimer::singleShot(0, this, &AlbumsView::loadNextCoverBatch);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  resizeEvent — re-layout grid on window resize
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_libraryDirty) {
        m_libraryDirty = false;
        m_coverCache.clear();
        reloadAlbums();
    }
}

void AlbumsView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_albums.isEmpty()) {
        m_resizeDebounceTimer->start();  // restarts 150ms countdown
    }
}

// ═══════════════════════════════════════════════════════════════════
//  findCoverArt — 3-tier cover discovery
// ═══════════════════════════════════════════════════════════════════
QPixmap AlbumsView::findCoverArt(const Album& album)
{
    QPixmap pix;

    // Tier 1: coverUrl (local file or Qt resource)
    if (!album.coverUrl.isEmpty()) {
        QString loadPath = album.coverUrl;
        if (loadPath.startsWith(QStringLiteral("qrc:")))
            loadPath = loadPath.mid(3);
        if (QFile::exists(loadPath)) {
            pix.load(loadPath);
            if (!pix.isNull()) return pix;
        }
    }

    // Tier 1.5: cached Cover Art Archive image via MBID
    {
        QString mbid = LibraryDatabase::instance()->releaseGroupMbidForAlbum(album.id);
        if (!mbid.isEmpty()) {
            QString cachedPath = CoverArtProvider::instance()->getCachedArtPath(mbid);
            if (!cachedPath.isEmpty() && QFile::exists(cachedPath)) {
                pix.load(cachedPath);
                if (!pix.isNull()) return pix;
            }
        }
    }

    // Find the first track with a file path
    QString firstTrackPath;
    for (const auto& t : album.tracks) {
        if (!t.filePath.isEmpty()) {
            firstTrackPath = t.filePath;
            break;
        }
    }

    // Fallback: albums from cache have empty tracks — use pre-built map
    if (firstTrackPath.isEmpty()) {
        firstTrackPath = m_albumTrackPaths.value(album.id);
    }

    // Tier 2: folder image files
    if (!firstTrackPath.isEmpty()) {
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
            QString path = folder + QStringLiteral("/") + n;
            if (QFile::exists(path)) {
                pix.load(path);
                if (!pix.isNull()) return pix;
            }
        }
    }

    // Tier 3: embedded cover via FFmpeg
    if (!firstTrackPath.isEmpty()) {
        pix = MetadataReader::extractCoverArt(firstTrackPath);
        if (!pix.isNull()) return pix;
    }

    // Tier 4: any image file in the folder
    if (!firstTrackPath.isEmpty()) {
        QString folder = QFileInfo(firstTrackPath).absolutePath();
        QDir dir(folder);
        QStringList imageFilters = {
            QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
            QStringLiteral("*.png"), QStringLiteral("*.bmp")
        };
        QStringList images = dir.entryList(imageFilters, QDir::Files, QDir::Name);
        if (!images.isEmpty()) {
            pix.load(dir.filePath(images.first()));
            if (!pix.isNull()) return pix;
        }
    }

    return pix;  // null = no cover found
}

// ═══════════════════════════════════════════════════════════════════
//  createAlbumCard — build one card widget
// ═══════════════════════════════════════════════════════════════════
QWidget* AlbumsView::createAlbumCard(const Album& album, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();
    int artSize = cardWidth; // square cover art

    auto* card = new QWidget(m_gridContainer);
    card->setObjectName(QStringLiteral("AlbumCard"));
    card->setCursor(Qt::PointingHandCursor);
    card->setProperty("albumId", album.id);
    card->setFixedWidth(cardWidth);
    card->setFocusPolicy(Qt::NoFocus);
    card->setAttribute(Qt::WA_MacShowFocusRect, false);
    card->setAutoFillBackground(false);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // ── Cover image (square with rounded corners) ────────────────────
    auto* coverLabel = new QLabel(card);
    coverLabel->setFixedSize(artSize, artSize);
    coverLabel->setAlignment(Qt::AlignCenter);
    coverLabel->setFocusPolicy(Qt::NoFocus);
    coverLabel->setProperty("coverRadius", UISizes::cardRadius);

    if (m_coverCache.contains(album.id)) {
        // Cache hit — use pre-loaded cover
        const QPixmap& cached = m_coverCache[album.id];
        if (!cached.isNull()) {
            coverLabel->setPixmap(renderRoundedCover(cached, artSize, UISizes::cardRadius));
        } else {
            coverLabel->setText(album.title.left(1).toUpper());
            coverLabel->setStyleSheet(QStringLiteral(
                "background: %1; border-radius: 8px; font-size: 48px; font-weight: bold; color: %2;")
                    .arg(c.backgroundSecondary, c.foregroundMuted));
        }
    } else {
        // Cache miss — show placeholder, register for async loading
        coverLabel->setText(album.title.left(1).toUpper());
        coverLabel->setStyleSheet(QStringLiteral(
            "background: %1; border-radius: 8px; font-size: 48px; font-weight: bold; color: %2;")
                .arg(c.backgroundSecondary, c.foregroundMuted));
        m_coverLabels[album.id] = coverLabel;
    }
    layout->addWidget(coverLabel);

    // ── Title ───────────────────────────────────────────────────────
    auto* titleLabel = new QLabel(card);
    titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 14px; font-weight: bold; color: %1;")
            .arg(c.foreground));
    QFontMetrics fm(titleLabel->font());
    titleLabel->setText(fm.elidedText(album.title, Qt::ElideRight, cardWidth));
    titleLabel->setToolTip(album.title);
    layout->addWidget(titleLabel);

    // ── Artist ──────────────────────────────────────────────────────
    auto* artistLabel = new QLabel(card);
    artistLabel->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: %1;")
            .arg(c.foregroundSecondary));
    QFontMetrics fm2(artistLabel->font());
    artistLabel->setText(fm2.elidedText(album.artist, Qt::ElideRight, cardWidth));
    artistLabel->setToolTip(album.artist);
    layout->addWidget(artistLabel);

    // ── Click handler via event filter ──────────────────────────────
    card->installEventFilter(this);

    return card;
}

// ═══════════════════════════════════════════════════════════════════
//  eventFilter — card click → emit albumSelected
// ═══════════════════════════════════════════════════════════════════
bool AlbumsView::eventFilter(QObject* obj, QEvent* event)
{
    // Escape in filter field → clear text and unfocus
    if (event->type() == QEvent::KeyPress && obj == m_filterInput->lineEdit()) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_filterInput->lineEdit()->clear();
            m_filterInput->lineEdit()->clearFocus();
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(obj);
        if (widget && widget->property("albumId").isValid()) {
            QString albumId = widget->property("albumId").toString();
            if (!albumId.isEmpty()) {
                qDebug() << ">>> AlbumsView: album clicked:" << albumId;
                emit albumSelected(albumId);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ═══════════════════════════════════════════════════════════════════
//  renderRoundedCover — scale + crop + round corners
// ═══════════════════════════════════════════════════════════════════
QPixmap AlbumsView::renderRoundedCover(const QPixmap& src, int size, int radius)
{
    QPixmap scaled = src.scaled(size, size,
        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() > size || scaled.height() > size) {
        int x = (scaled.width()  - size) / 2;
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
    return rounded;
}

// ═══════════════════════════════════════════════════════════════════
//  loadNextCoverBatch — async cover loading, 5 per event-loop tick
// ═══════════════════════════════════════════════════════════════════
void AlbumsView::loadNextCoverBatch()
{
    // Collect batch of albums needing covers
    QVector<QPair<QString, Album>> batch;
    while (m_coverLoadIndex < m_albums.size() && batch.size() < 5) {
        const auto& album = m_albums[m_coverLoadIndex];
        m_coverLoadIndex++;
        if (m_coverCache.contains(album.id)) continue;
        batch.append({album.id, album});
    }

    if (batch.isEmpty()) {
        if (m_coverLoadIndex < m_albums.size()) {
            QTimer::singleShot(0, this, &AlbumsView::loadNextCoverBatch);
        } else {
            m_coverLabels.clear();
        }
        return;
    }

    // Process cover art extraction on worker thread (MetadataReader I/O off main thread)
    (void)QtConcurrent::run([this, batch]() {
        QVector<QPair<QString, QPixmap>> results;
        for (const auto& job : batch)
            results.append({job.first, findCoverArt(job.second)});

        QMetaObject::invokeMethod(this, [this, results]() {
            for (const auto& r : results) {
                m_coverCache[r.first] = r.second;
                QLabel* label = m_coverLabels.value(r.first);
                if (label && !r.second.isNull()) {
                    int size = label->width();
                    int radius = label->property("coverRadius").toInt();
                    label->setPixmap(renderRoundedCover(r.second, size, radius));
                    label->setStyleSheet(QString());
                }
            }
            if (m_coverLoadIndex < m_albums.size()) {
                loadNextCoverBatch();
            } else {
                m_coverLabels.clear();
            }
        }, Qt::QueuedConnection);
    });
}
