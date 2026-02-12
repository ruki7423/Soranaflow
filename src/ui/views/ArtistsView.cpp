#include "ArtistsView.h"
#include "../MainWindow.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFontMetrics>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include "../../core/ThemeManager.h"
#include "../../core/audio/MetadataReader.h"
#include "../../core/library/LibraryDatabase.h"
#include "../../metadata/FanartTvProvider.h"
#include <QtConcurrent>

ArtistsView::ArtistsView(QWidget* parent)
    : QWidget(parent)
    , m_searchInput(nullptr)
    , m_gridContainer(nullptr)
    , m_gridLayout(nullptr)
    , m_scrollArea(nullptr)
    , m_headerLabel(nullptr)
    , m_countLabel(nullptr)
{
    setupUI();
    refreshTheme();

    m_resizeDebounceTimer = new QTimer(this);
    m_resizeDebounceTimer->setSingleShot(true);
    m_resizeDebounceTimer->setInterval(150);
    connect(m_resizeDebounceTimer, &QTimer::timeout, this, &ArtistsView::relayoutGrid);

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &ArtistsView::refreshTheme);
    connect(MusicDataProvider::instance(), &MusicDataProvider::libraryUpdated,
            this, [this]() {
                if (!isVisible()) { m_libraryDirty = true; return; }
                m_coverCache.clear();
                onLibraryUpdated();
            });

    // Deferred initial load
    QTimer::singleShot(200, this, [this]() {
        if (m_artistCards.isEmpty()) {
            populateArtists();
        }
    });
}

// ── showEvent ──────────────────────────────────────────────────────
void ArtistsView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_firstShow) {
        m_firstShow = false;
        onLibraryUpdated();
    } else if (m_libraryDirty) {
        m_libraryDirty = false;
        m_coverCache.clear();
        onLibraryUpdated();
    }
}

void ArtistsView::setupUI()
{
    setObjectName(QStringLiteral("ArtistsView"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    // ── Header row — unified toolbar (30px buttons, 8px spacing) ────
    const int NAV_SIZE = 30;

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    auto c = ThemeManager::instance()->colors();

    m_headerLabel = new QLabel(QStringLiteral("Artists"), this);
    m_headerLabel->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: bold; color: %1;")
        .arg(c.foreground));
    headerLayout->addWidget(m_headerLabel);

    // ── Global navigation ← → ─────────────────────────────────────
    headerLayout->addSpacing(4);

    m_navBackBtn = new QPushButton(this);
    m_navBackBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_navBackBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_navBackBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navBackBtn->setCursor(Qt::PointingHandCursor);
    m_navBackBtn->setToolTip(QStringLiteral("Back"));
    m_navBackBtn->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(m_navBackBtn);

    m_navForwardBtn = new QPushButton(this);
    m_navForwardBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));
    m_navForwardBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_navForwardBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navForwardBtn->setCursor(Qt::PointingHandCursor);
    m_navForwardBtn->setToolTip(QStringLiteral("Forward"));
    m_navForwardBtn->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(m_navForwardBtn);

    headerLayout->addStretch();

    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;")
        .arg(c.foregroundMuted));
    headerLayout->addWidget(m_countLabel);

    headerLayout->addSpacing(12);

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
    headerLayout->addWidget(m_largeIconBtn);

    m_smallIconBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), this);
    m_smallIconBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/grid-3x3.svg")));
    m_smallIconBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_smallIconBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_smallIconBtn->setToolTip(QStringLiteral("Small Icons"));
    m_smallIconBtn->setStyleSheet(viewBtnStyle(false));
    headerLayout->addWidget(m_smallIconBtn);

    m_listBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), this);
    m_listBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/list.svg")));
    m_listBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_listBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_listBtn->setToolTip(QStringLiteral("List"));
    m_listBtn->setStyleSheet(viewBtnStyle(false));
    headerLayout->addWidget(m_listBtn);

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

    connect(m_navBackBtn, &QPushButton::clicked, this, [this]() {
        if (auto* mw = MainWindow::instance()) mw->navigateBack();
    });
    connect(m_navForwardBtn, &QPushButton::clicked, this, [this]() {
        if (auto* mw = MainWindow::instance()) mw->navigateForward();
    });
    connect(MainWindow::instance(), &MainWindow::globalNavChanged,
            this, updateNavBtnStyle);

    mainLayout->addLayout(headerLayout);

    // ── Search input ──────────────────────────────────────────────────
    m_searchInput = new StyledInput(QStringLiteral("Search artists..."), QString(), this);
    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(200);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, [this]() {
        onSearchChanged(m_searchInput->lineEdit()->text());
    });
    connect(m_searchInput->lineEdit(), &QLineEdit::textChanged,
            this, [this]() { m_searchDebounceTimer->start(); });
    m_searchInput->lineEdit()->installEventFilter(this);
    mainLayout->addWidget(m_searchInput);

    // ── Scroll area with grid ─────────────────────────────────────────
    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_gridContainer = new QWidget(m_scrollArea);
    m_gridContainer->setObjectName(QStringLiteral("ArtistsGridContainer"));
    m_gridLayout = new QGridLayout(m_gridContainer);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setSpacing(20);
    m_gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    m_scrollArea->setWidget(m_gridContainer);
    mainLayout->addWidget(m_scrollArea, 1);
}

// ── findArtistCoverArt ─────────────────────────────────────────────
QPixmap ArtistsView::findArtistCoverArt(const Artist& artist)
{
    QPixmap pixmap;

    // Try coverUrl (local file or Qt resource)
    if (!artist.coverUrl.isEmpty()) {
        QString loadPath = artist.coverUrl;
        if (loadPath.startsWith(QStringLiteral("qrc:")))
            loadPath = loadPath.mid(3);
        if (QFile::exists(loadPath)) {
            pixmap.load(loadPath);
            if (!pixmap.isNull()) return pixmap;
        }
    }

    // Try cached Fanart.tv artist thumbnail via MBID
    {
        QString mbid = LibraryDatabase::instance()->artistMbidForArtist(artist.id);
        if (!mbid.isEmpty()) {
            QString cachedPath = FanartTvProvider::instance()->getCachedArtistThumb(mbid);
            if (!cachedPath.isEmpty() && QFile::exists(cachedPath)) {
                pixmap.load(cachedPath);
                if (!pixmap.isNull()) return pixmap;
            }
        }
    }

    // Get folder path from pre-computed map (O(1) instead of O(n) allTracks copy)
    QString folderPath;
    QString firstTrackPath = MusicDataProvider::instance()->artistFirstTrackPath(artist.id);

    // Fallback: before rebuildAlbumsAndArtists, track.artistId may be empty.
    // Match by artist name instead (rare — only during initial scan).
    if (firstTrackPath.isEmpty()) {
        const auto tracks = MusicDataProvider::instance()->allTracks();
        QString artistLower = artist.name.toLower().trimmed();
        for (const auto& track : tracks) {
            if (track.artist.toLower().trimmed() == artistLower && !track.filePath.isEmpty()) {
                firstTrackPath = track.filePath;
                break;
            }
        }
    }

    if (!firstTrackPath.isEmpty())
        folderPath = QFileInfo(firstTrackPath).absolutePath();

    // Look for cover images in folder
    if (!folderPath.isEmpty()) {
        static const QStringList coverNames = {
            QStringLiteral("cover.jpg"), QStringLiteral("cover.png"),
            QStringLiteral("folder.jpg"), QStringLiteral("folder.png"),
            QStringLiteral("artist.jpg"), QStringLiteral("artist.png"),
            QStringLiteral("front.jpg"), QStringLiteral("front.png"),
            QStringLiteral("Cover.jpg"), QStringLiteral("Cover.png"),
            QStringLiteral("Folder.jpg"), QStringLiteral("Front.jpg")
        };
        for (const QString& name : coverNames) {
            QString coverPath = folderPath + QStringLiteral("/") + name;
            if (QFile::exists(coverPath)) {
                pixmap.load(coverPath);
                if (!pixmap.isNull()) return pixmap;
            }
        }
    }

    // Extract embedded cover from the artist's first track
    if (!firstTrackPath.isEmpty()) {
        pixmap = MetadataReader::extractCoverArt(firstTrackPath);
        if (!pixmap.isNull()) return pixmap;
    }

    // Fallback: any image file in the folder
    if (!folderPath.isEmpty()) {
        QDir dir(folderPath);
        QStringList imageFilters = {
            QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
            QStringLiteral("*.png"), QStringLiteral("*.bmp")
        };
        QStringList images = dir.entryList(imageFilters, QDir::Files, QDir::Name);
        if (!images.isEmpty()) {
            pixmap.load(dir.filePath(images.first()));
            if (!pixmap.isNull()) return pixmap;
        }
    }

    return pixmap; // null = no cover
}

QWidget* ArtistsView::createArtistCard(const Artist& artist, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();
    int artSize = cardWidth - 16; // account for card padding

    auto* card = new QWidget(m_gridContainer);
    card->setObjectName(QStringLiteral("ArtistCard"));
    card->setCursor(Qt::PointingHandCursor);
    card->setProperty("artistId", artist.id);
    card->setFixedWidth(cardWidth);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(8, 8, 8, 8);
    cardLayout->setSpacing(8);
    cardLayout->setAlignment(Qt::AlignHCenter);

    // ── Circular cover image ──────────────────────────────────────────
    auto* coverLabel = new QLabel(card);
    coverLabel->setFixedSize(artSize, artSize);
    coverLabel->setAlignment(Qt::AlignCenter);
    coverLabel->setProperty("isCircular", true);

    // Helper: set initials placeholder on the label
    auto setPlaceholder = [&]() {
        QString initials;
        const QStringList nameParts = artist.name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& part : nameParts) {
            if (!part.isEmpty()) {
                initials += part.at(0).toUpper();
                if (initials.length() >= 2) break;
            }
        }
        if (initials.isEmpty() && !artist.name.isEmpty())
            initials = artist.name.at(0).toUpper();
        coverLabel->setText(initials);
        coverLabel->setStyleSheet(QStringLiteral(
            "background: %1; border-radius: %3px; color: %2; font-size: 24px; font-weight: bold;")
            .arg(c.backgroundSecondary, c.foreground, QString::number(artSize / 2)));
    };

    if (m_coverCache.contains(artist.id)) {
        const QPixmap& cached = m_coverCache[artist.id];
        if (!cached.isNull()) {
            coverLabel->setPixmap(renderCircularCover(cached, artSize));
        } else {
            setPlaceholder();
        }
    } else {
        setPlaceholder();
        m_coverLabels[artist.id] = coverLabel;
    }
    cardLayout->addWidget(coverLabel, 0, Qt::AlignHCenter);

    // ── Name label ────────────────────────────────────────────────────
    auto* nameLabel = new QLabel(card);
    nameLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: bold; font-size: 15px;")
        .arg(c.foreground));
    nameLabel->setAlignment(Qt::AlignHCenter);
    nameLabel->setWordWrap(false);

    QFontMetrics fm(nameLabel->font());
    nameLabel->setText(fm.elidedText(artist.name, Qt::ElideRight, cardWidth - 16));
    nameLabel->setToolTip(artist.name);
    cardLayout->addWidget(nameLabel, 0, Qt::AlignHCenter);

    // ── Album count ───────────────────────────────────────────────────
    int albumCount = artist.albums.size();
    auto* albumCountLabel = new QLabel(
        QString::number(albumCount) + (albumCount == 1
            ? QStringLiteral(" album")
            : QStringLiteral(" albums")),
        card);
    albumCountLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
        .arg(c.foregroundMuted));
    albumCountLabel->setAlignment(Qt::AlignHCenter);
    cardLayout->addWidget(albumCountLabel, 0, Qt::AlignHCenter);

    // ── Genres ────────────────────────────────────────────────────────
    if (!artist.genres.isEmpty()) {
        QString genresText = artist.genres.join(QStringLiteral(", "));
        auto* genresLabel = new QLabel(genresText, card);
        genresLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
            .arg(c.foregroundMuted));
        genresLabel->setAlignment(Qt::AlignHCenter);
        genresLabel->setWordWrap(true);
        genresLabel->setMaximumWidth(cardWidth - 16);
        genresLabel->setMaximumHeight(32);
        cardLayout->addWidget(genresLabel, 0, Qt::AlignHCenter);
    }

    card->installEventFilter(this);
    return card;
}

void ArtistsView::populateArtists()
{
    // Flag-based debounce — queue if busy, never drop
    static bool isPopulating = false;
    static bool pendingPopulate = false;
    if (isPopulating) {
        pendingPopulate = true;
        return;
    }
    isPopulating = true;
    pendingPopulate = false;

    m_artists = MusicDataProvider::instance()->allArtists();
    qDebug() << "[ArtistsView] populateArtists:" << m_artists.size() << "artists, cache:" << m_coverCache.size();

    m_countLabel->setText(QString::number(m_artists.size()) + QStringLiteral(" artists"));

    relayoutGrid();

    QTimer::singleShot(500, this, [this]() {
        isPopulating = false;
        if (pendingPopulate) {
            pendingPopulate = false;
            populateArtists();
        }
    });
}

// ── setViewMode ───────────────────────────────────────────────────
void ArtistsView::setViewMode(ViewMode mode)
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

// ── createArtistListRow ───────────────────────────────────────────
QWidget* ArtistsView::createArtistListRow(const Artist& artist)
{
    auto c = ThemeManager::instance()->colors();
    auto* row = new QWidget(m_gridContainer);
    row->setObjectName(QStringLiteral("ArtistCard"));
    row->setCursor(Qt::PointingHandCursor);
    row->setProperty("artistId", artist.id);
    row->setFixedHeight(56);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(12);

    // Circular cover art
    auto* coverLabel = new QLabel(row);
    coverLabel->setFixedSize(UISizes::rowHeight, UISizes::rowHeight);
    coverLabel->setAlignment(Qt::AlignCenter);
    coverLabel->setProperty("isCircular", true);

    auto setRowPlaceholder = [&]() {
        QString initials;
        const QStringList nameParts = artist.name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& part : nameParts) {
            if (!part.isEmpty()) {
                initials += part.at(0).toUpper();
                if (initials.length() >= 2) break;
            }
        }
        if (initials.isEmpty() && !artist.name.isEmpty())
            initials = artist.name.at(0).toUpper();
        coverLabel->setText(initials);
        coverLabel->setStyleSheet(QStringLiteral(
            "background: %1; border-radius: 24px; color: %2; font-size: 16px; font-weight: bold;")
            .arg(c.backgroundSecondary, c.foreground));
    };

    if (m_coverCache.contains(artist.id)) {
        const QPixmap& cached = m_coverCache[artist.id];
        if (!cached.isNull()) {
            coverLabel->setPixmap(renderCircularCover(cached, UISizes::rowHeight));
        } else {
            setRowPlaceholder();
        }
    } else {
        setRowPlaceholder();
        m_coverLabels[artist.id] = coverLabel;
    }
    layout->addWidget(coverLabel);

    // Name + album count stacked
    auto* infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(2);
    auto* nameLabel = new QLabel(artist.name, row);
    nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; font-weight: bold;")
        .arg(c.foreground));
    infoLayout->addWidget(nameLabel);

    int albumCount = artist.albums.size();
    auto* countLabel = new QLabel(
        QString::number(albumCount) + (albumCount == 1 ? QStringLiteral(" album") : QStringLiteral(" albums")), row);
    countLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    infoLayout->addWidget(countLabel);
    layout->addLayout(infoLayout, 1);

    // Genres
    if (!artist.genres.isEmpty()) {
        auto* genresLabel = new QLabel(artist.genres.join(QStringLiteral(", ")), row);
        genresLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted));
        layout->addWidget(genresLabel);
    }

    row->installEventFilter(this);
    return row;
}

// ── relayoutGrid — responsive grid layout ─────────────────────────
void ArtistsView::relayoutGrid()
{
    m_coverLabels.clear();  // old labels about to be deleted

    // Clear grid layout first, then delete card widgets
    while (m_gridLayout->count() > 0) {
        auto* item = m_gridLayout->takeAt(0);
        delete item;
    }
    qDeleteAll(m_artistCards);
    m_artistCards.clear();

    if (m_artists.isEmpty()) return;

    int availableWidth = m_scrollArea->viewport()->width() - 8;
    if (availableWidth < 200) availableWidth = width() - 48;

    if (m_viewMode == ListView) {
        for (int i = 0; i < m_artists.size(); ++i) {
            QWidget* row = createArtistListRow(m_artists[i]);
            m_gridLayout->addWidget(row, i, 0);
            m_artistCards.append(row);
        }
    } else {
        const int minCardWidth = (m_viewMode == SmallIcons) ? 120 : 160;
        const int spacing = (m_viewMode == SmallIcons) ? UISizes::spacingMD : 20;
        m_gridLayout->setSpacing(spacing);

        int columns = qMax(1, (availableWidth + spacing) / (minCardWidth + spacing));
        int cardWidth = (availableWidth - (columns - 1) * spacing) / columns;
        cardWidth = qMax(cardWidth, minCardWidth);

        int row = 0, col = 0;

        for (const Artist& artist : m_artists) {
            QWidget* card = createArtistCard(artist, cardWidth);
            m_gridLayout->addWidget(card, row, col);
            m_artistCards.append(card);

            col++;
            if (col >= columns) {
                col = 0;
                row++;
            }
        }
    }

    // Start async cover loading for uncached artists
    if (!m_coverLabels.isEmpty()) {
        m_coverLoadIndex = 0;
        QTimer::singleShot(0, this, &ArtistsView::loadNextCoverBatch);
    }
}

// ── resizeEvent ───────────────────────────────────────────────────
void ArtistsView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_artists.isEmpty()) {
        m_resizeDebounceTimer->start();  // restarts 150ms countdown
    }
}

void ArtistsView::onSearchChanged(const QString& text)
{
    const QString query = text.trimmed().toLower();
    int visibleCount = 0;

    for (QWidget* card : std::as_const(m_artistCards)) {
        bool visible = false;
        if (query.isEmpty()) {
            visible = true;
        } else {
            // Check all child QLabels for text match
            const QList<QLabel*> labels = card->findChildren<QLabel*>();
            for (const QLabel* label : labels) {
                if (label->text().toLower().contains(query)) {
                    visible = true;
                    break;
                }
            }
        }
        card->setVisible(visible);
        if (visible)
            visibleCount++;
    }

    m_countLabel->setText(QString::number(visibleCount) + QStringLiteral(" artists"));
}

void ArtistsView::onLibraryUpdated()
{
    populateArtists();
}

void ArtistsView::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    m_headerLabel->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: bold; color: %1;")
        .arg(c.foreground));
    m_countLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;")
        .arg(c.foregroundMuted));

    // Refresh view toggle button icons for new theme color
    m_largeIconBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/grid-2x2.svg")));
    m_smallIconBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/grid-3x3.svg")));
    m_listBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/list.svg")));
    m_navBackBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_navForwardBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));

    // Re-apply view mode styles for current theme
    setViewMode(m_viewMode);

    // Rebuild artist cards to pick up new theme colors
    populateArtists();
}

bool ArtistsView::eventFilter(QObject* obj, QEvent* event)
{
    // Escape in filter field → clear text and unfocus
    if (event->type() == QEvent::KeyPress && obj == m_searchInput->lineEdit()) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_searchInput->lineEdit()->clear();
            m_searchInput->lineEdit()->clearFocus();
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        QWidget* card = qobject_cast<QWidget*>(obj);
        if (card && card->property("artistId").isValid()) {
            emit artistSelected(card->property("artistId").toString());
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ── renderCircularCover — scale + crop + circular mask ────────────
QPixmap ArtistsView::renderCircularCover(const QPixmap& src, int size)
{
    QPixmap scaled = src.scaled(size, size,
        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() > size || scaled.height() > size) {
        int x = (scaled.width() - size) / 2;
        int y = (scaled.height() - size) / 2;
        scaled = scaled.copy(x, y, size, size);
    }
    QPixmap circular(size, size);
    circular.fill(Qt::transparent);
    QPainter painter(&circular);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, scaled);
    return circular;
}

// ── loadNextCoverBatch — async cover loading on worker thread ────────
void ArtistsView::loadNextCoverBatch()
{
    // Collect batch of artists needing covers
    QVector<QPair<QString, Artist>> batch;
    while (m_coverLoadIndex < m_artists.size() && batch.size() < 5) {
        const auto& artist = m_artists[m_coverLoadIndex];
        m_coverLoadIndex++;
        if (m_coverCache.contains(artist.id)) continue;
        batch.append({artist.id, artist});
    }

    if (batch.isEmpty()) {
        if (m_coverLoadIndex < m_artists.size()) {
            QTimer::singleShot(0, this, &ArtistsView::loadNextCoverBatch);
        } else {
            m_coverLabels.clear();
        }
        return;
    }

    // Process cover art extraction on worker thread (MetadataReader I/O off main thread)
    QtConcurrent::run([this, batch]() {
        QVector<QPair<QString, QPixmap>> results;
        for (const auto& job : batch)
            results.append({job.first, findArtistCoverArt(job.second)});

        QMetaObject::invokeMethod(this, [this, results]() {
            for (const auto& r : results) {
                m_coverCache[r.first] = r.second;
                QLabel* label = m_coverLabels.value(r.first);
                if (label && !r.second.isNull()) {
                    int size = label->width();
                    label->setPixmap(renderCircularCover(r.second, size));
                    label->setStyleSheet(QString());
                }
            }
            if (m_coverLoadIndex < m_artists.size()) {
                loadNextCoverBatch();
            } else {
                m_coverLabels.clear();
            }
        }, Qt::QueuedConnection);
    });
}
