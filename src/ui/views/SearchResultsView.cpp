#include "SearchResultsView.h"

#include <QDebug>
#include <QPainter>
#include <QPainterPath>
#include <QFileInfo>
#include <QDir>
#include <QMouseEvent>
#include <QTimer>

#include "../../core/ThemeManager.h"
#include "../../core/PlaybackState.h"
#include "../../core/audio/MetadataReader.h"
#include "../../core/library/LibraryDatabase.h"

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

SearchResultsView::SearchResultsView(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    refreshTheme();
    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SearchResultsView::refreshTheme);
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI
// ═════════════════════════════════════════════════════════════════════

void SearchResultsView::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_contentWidget = new QWidget();
    m_contentLayout = new QVBoxLayout(m_contentWidget);
    m_contentLayout->setContentsMargins(24, 24, 24, 24);
    m_contentLayout->setSpacing(16);

    // Query label
    m_queryLabel = new QLabel();
    m_queryLabel->setObjectName(QStringLiteral("searchQueryLabel"));
    m_contentLayout->addWidget(m_queryLabel);

    // Empty state
    m_emptyLabel = new QLabel(QStringLiteral("No results found"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setVisible(false);
    m_contentLayout->addWidget(m_emptyLabel);

    // ── Artists section ──
    m_artistsHeader = new QLabel(QStringLiteral("ARTISTS"));
    m_artistsHeader->setVisible(false);
    m_contentLayout->addWidget(m_artistsHeader);

    m_artistsContainer = new QWidget();
    auto* artistsLayout = new QHBoxLayout(m_artistsContainer);
    artistsLayout->setContentsMargins(0, 0, 0, 0);
    artistsLayout->setSpacing(12);
    artistsLayout->setAlignment(Qt::AlignLeft);
    m_artistsContainer->setVisible(false);
    m_contentLayout->addWidget(m_artistsContainer);

    // ── Albums section ──
    m_albumsHeader = new QLabel(QStringLiteral("ALBUMS"));
    m_albumsHeader->setVisible(false);
    m_contentLayout->addWidget(m_albumsHeader);

    m_albumsContainer = new QWidget();
    auto* albumsLayout = new QHBoxLayout(m_albumsContainer);
    albumsLayout->setContentsMargins(0, 0, 0, 0);
    albumsLayout->setSpacing(12);
    albumsLayout->setAlignment(Qt::AlignLeft);
    m_albumsContainer->setVisible(false);
    m_contentLayout->addWidget(m_albumsContainer);

    // ── Tracks section ──
    m_tracksHeader = new QLabel(QStringLiteral("TRACKS"));
    m_tracksHeader->setVisible(false);
    m_contentLayout->addWidget(m_tracksHeader);

    m_trackTable = new TrackTableView(libraryConfig(), this);
    m_trackTable->setVisible(false);
    m_contentLayout->addWidget(m_trackTable);

    connect(m_trackTable, &TrackTableView::trackDoubleClicked,
            this, [this](const Track& t) {
        PlaybackState::instance()->setQueue(m_searchTracks);
        PlaybackState::instance()->playTrack(t);
    });

    m_contentLayout->addStretch(1);

    m_scrollArea->setWidget(m_contentWidget);
    mainLayout->addWidget(m_scrollArea);
}

// ═════════════════════════════════════════════════════════════════════
//  setResults
// ═════════════════════════════════════════════════════════════════════

void SearchResultsView::setResults(const QString& query,
                                    const QVector<Artist>& artists,
                                    const QVector<Album>& albums,
                                    const QVector<Track>& tracks)
{
    m_lastQuery = query;
    m_searchTracks = tracks;
    auto c = ThemeManager::instance()->colors();

    m_queryLabel->setText(QStringLiteral("Results for \"%1\"").arg(query));
    m_queryLabel->setVisible(true);

    bool empty = artists.isEmpty() && albums.isEmpty() && tracks.isEmpty();
    m_emptyLabel->setVisible(empty);

    // ── Artists ──
    bool hasArtists = !artists.isEmpty();
    m_artistsHeader->setText(QStringLiteral("ARTISTS (%1)").arg(artists.size()));
    m_artistsHeader->setVisible(hasArtists);
    m_artistsContainer->setVisible(hasArtists);
    if (hasArtists)
        buildArtistCards(artists);

    // ── Albums ──
    bool hasAlbums = !albums.isEmpty();
    m_albumsHeader->setText(QStringLiteral("ALBUMS (%1)").arg(albums.size()));
    m_albumsHeader->setVisible(hasAlbums);
    m_albumsContainer->setVisible(hasAlbums);
    if (hasAlbums)
        buildAlbumCards(albums);

    // ── Tracks ──
    bool hasTracks = !tracks.isEmpty();
    m_tracksHeader->setText(QStringLiteral("TRACKS (%1)").arg(tracks.size()));
    m_tracksHeader->setVisible(hasTracks);
    m_trackTable->setVisible(hasTracks);
    if (hasTracks)
        m_trackTable->setTracks(tracks);

    qDebug() << "[Search]" << query << "→"
             << artists.size() << "artists,"
             << albums.size() << "albums,"
             << tracks.size() << "tracks";
}

// ═════════════════════════════════════════════════════════════════════
//  clearResults
// ═════════════════════════════════════════════════════════════════════

void SearchResultsView::clearResults()
{
    m_lastQuery.clear();
    m_queryLabel->setVisible(false);
    m_emptyLabel->setVisible(false);
    m_artistsHeader->setVisible(false);
    m_artistsContainer->setVisible(false);
    m_albumsHeader->setVisible(false);
    m_albumsContainer->setVisible(false);
    m_tracksHeader->setVisible(false);
    m_trackTable->setVisible(false);
}

// ═════════════════════════════════════════════════════════════════════
//  buildArtistCards
// ═════════════════════════════════════════════════════════════════════

void SearchResultsView::buildArtistCards(const QVector<Artist>& artists)
{
    // Clear old cards
    auto* layout = m_artistsContainer->layout();
    while (layout->count() > 0) {
        auto* item = layout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    auto c = ThemeManager::instance()->colors();

    for (const auto& artist : artists) {
        auto* card = new QWidget();
        card->setFixedSize(140, 170);
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(QStringLiteral(
            "QWidget { background: %1; border-radius: 8px; }"
            "QWidget:hover { background: %2; }").arg(c.backgroundSecondary, c.hover));

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 10, 10, 10);
        cardLayout->setSpacing(8);
        cardLayout->setAlignment(Qt::AlignHCenter);

        // Circular placeholder
        auto* avatarLabel = new QLabel();
        avatarLabel->setFixedSize(96, 96);
        avatarLabel->setAlignment(Qt::AlignCenter);

        QPixmap avatar(96, 96);
        avatar.fill(Qt::transparent);
        QPainter p(&avatar);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(c.backgroundTertiary));
        p.setPen(Qt::NoPen);
        p.drawEllipse(0, 0, 96, 96);

        // Draw initial
        p.setPen(QColor(c.foregroundMuted));
        QFont f;
        f.setPixelSize(32);
        f.setBold(true);
        p.setFont(f);
        QString initial = artist.name.left(1).toUpper();
        p.drawText(QRect(0, 0, 96, 96), Qt::AlignCenter, initial);
        p.end();

        avatarLabel->setPixmap(avatar);
        cardLayout->addWidget(avatarLabel, 0, Qt::AlignHCenter);

        auto* nameLabel = new QLabel(artist.name);
        nameLabel->setAlignment(Qt::AlignCenter);
        nameLabel->setWordWrap(true);
        nameLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 13px; font-weight: 500;").arg(c.foreground));
        cardLayout->addWidget(nameLabel);

        // Click handler
        QString artistId = artist.id;
        card->installEventFilter(this);
        card->setProperty("artistId", artistId);

        layout->addWidget(card);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  buildAlbumCards
// ═════════════════════════════════════════════════════════════════════

void SearchResultsView::buildAlbumCards(const QVector<Album>& albums)
{
    auto* layout = m_albumsContainer->layout();
    while (layout->count() > 0) {
        auto* item = layout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    m_pendingCoverLabels.clear();
    m_pendingAlbums = albums;
    m_coverLoadIndex = 0;

    auto c = ThemeManager::instance()->colors();

    for (const auto& album : albums) {
        auto* card = new QWidget();
        card->setFixedSize(160, 210);
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(QStringLiteral(
            "QWidget { background: %1; border-radius: 8px; }"
            "QWidget:hover { background: %2; }").arg(c.backgroundSecondary, c.hover));

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(8, 8, 8, 8);
        cardLayout->setSpacing(6);

        // Cover art — use cache or show placeholder
        auto* coverLabel = new QLabel();
        coverLabel->setFixedSize(144, 144);
        coverLabel->setAlignment(Qt::AlignCenter);

        if (m_albumCoverCache.contains(album.id)) {
            coverLabel->setPixmap(m_albumCoverCache[album.id]);
        } else {
            // Placeholder — register for async batch loading
            coverLabel->setText(album.title.left(1).toUpper());
            coverLabel->setStyleSheet(QStringLiteral(
                "background: %1; border-radius: 6px; font-size: 40px; font-weight: bold; color: %2;")
                .arg(c.backgroundTertiary, c.foregroundMuted));
            m_pendingCoverLabels[album.id] = coverLabel;
        }

        cardLayout->addWidget(coverLabel, 0, Qt::AlignHCenter);

        auto* titleLabel = new QLabel(album.title);
        titleLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 12px; font-weight: 500;").arg(c.foreground));
        titleLabel->setMaximumWidth(144);
        titleLabel->setWordWrap(false);
        QFontMetrics fm(titleLabel->font());
        titleLabel->setText(fm.elidedText(album.title, Qt::ElideRight, 140));
        cardLayout->addWidget(titleLabel);

        auto* artistLabel = new QLabel(album.artist);
        artistLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 11px;").arg(c.foregroundMuted));
        artistLabel->setMaximumWidth(144);
        QFontMetrics fm2(artistLabel->font());
        artistLabel->setText(fm2.elidedText(album.artist, Qt::ElideRight, 140));
        cardLayout->addWidget(artistLabel);

        // Click handler
        QString albumId = album.id;
        card->setProperty("albumId", albumId);
        card->installEventFilter(this);

        layout->addWidget(card);
    }

    // Start async batch loading for uncached covers
    if (!m_pendingCoverLabels.isEmpty()) {
        QTimer::singleShot(0, this, &SearchResultsView::loadNextCoverBatch);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  loadAlbumCover — quick cover lookup (coverUrl → file → embedded)
// ═════════════════════════════════════════════════════════════════════

QPixmap SearchResultsView::loadAlbumCover(const Album& album)
{
    // Try coverUrl first
    if (!album.coverUrl.isEmpty() && QFileInfo::exists(album.coverUrl)) {
        QPixmap pix;
        if (pix.load(album.coverUrl))
            return pix;
    }

    // Get first track for this album via albumById (loads tracks)
    Album full = LibraryDatabase::instance()->albumById(album.id);
    if (full.tracks.isEmpty())
        return QPixmap();

    const QString& filePath = full.tracks.first().filePath;
    if (filePath.isEmpty())
        return QPixmap();

    // Look for cover in folder
    QString folder = QFileInfo(filePath).absolutePath();
    QDir dir(folder);
    QStringList imageFilters = {
        QStringLiteral("cover.*"), QStringLiteral("folder.*"),
        QStringLiteral("front.*"), QStringLiteral("album.*")
    };
    dir.setNameFilters(imageFilters);
    auto entries = dir.entryInfoList(QDir::Files);
    if (!entries.isEmpty()) {
        QPixmap pix;
        if (pix.load(entries.first().absoluteFilePath()))
            return pix;
    }

    // Embedded art
    QPixmap pix = MetadataReader::extractCoverArt(filePath);
    if (!pix.isNull())
        return pix;

    return QPixmap();
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — click on artist/album cards
// ═════════════════════════════════════════════════════════════════════
//  loadNextCoverBatch — async batch cover loading (5 per event-loop tick)
// ═════════════════════════════════════════════════════════════════════

void SearchResultsView::loadNextCoverBatch()
{
    int processed = 0;
    while (m_coverLoadIndex < m_pendingAlbums.size() && processed < 5) {
        const auto& album = m_pendingAlbums[m_coverLoadIndex];
        m_coverLoadIndex++;

        if (m_albumCoverCache.contains(album.id)) continue;

        QPixmap cover = loadAlbumCover(album);

        // Render: scale, crop, round corners
        if (!cover.isNull()) {
            cover = cover.scaled(144, 144, Qt::KeepAspectRatioByExpanding,
                                 Qt::SmoothTransformation);
            cover = cover.copy((cover.width() - 144) / 2,
                               (cover.height() - 144) / 2, 144, 144);

            QPixmap rounded(144, 144);
            rounded.fill(Qt::transparent);
            QPainter rp(&rounded);
            rp.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(0, 0, 144, 144), 6, 6);
            rp.setClipPath(path);
            rp.drawPixmap(0, 0, cover);
            rp.end();
            cover = rounded;
        }

        m_albumCoverCache[album.id] = cover;
        processed++;

        // Update label if it still exists (QPointer auto-nulls on deletion)
        QLabel* label = m_pendingCoverLabels.value(album.id);
        if (label && !cover.isNull()) {
            label->setPixmap(cover);
            label->setStyleSheet(QString());
        }
    }

    if (m_coverLoadIndex < m_pendingAlbums.size()) {
        QTimer::singleShot(0, this, &SearchResultsView::loadNextCoverBatch);
    } else {
        m_pendingCoverLabels.clear();
        m_pendingAlbums.clear();
    }
}

// ═════════════════════════════════════════════════════════════════════

bool SearchResultsView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* widget = qobject_cast<QWidget*>(obj);
        if (widget) {
            QString artistId = widget->property("artistId").toString();
            if (!artistId.isEmpty()) {
                emit artistClicked(artistId);
                return true;
            }
            QString albumId = widget->property("albumId").toString();
            if (!albumId.isEmpty()) {
                emit albumClicked(albumId);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void SearchResultsView::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    m_scrollArea->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }") +
        ThemeManager::instance()->scrollbarStyle());

    m_queryLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 24px; font-weight: bold;").arg(c.foreground));

    m_emptyLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 16px; padding: 40px;").arg(c.foregroundMuted));

    const QString sectionStyle = QStringLiteral(
        "color: %1; font-size: 12px; font-weight: 600; letter-spacing: 1px;")
        .arg(c.foregroundMuted);
    m_artistsHeader->setStyleSheet(sectionStyle);
    m_albumsHeader->setStyleSheet(sectionStyle);
    m_tracksHeader->setStyleSheet(sectionStyle);

    // Rebuild cards if we have results displayed
    if (!m_lastQuery.isEmpty()) {
        auto artists = LibraryDatabase::instance()->searchArtists(m_lastQuery);
        auto albums = LibraryDatabase::instance()->searchAlbums(m_lastQuery);
        if (!artists.isEmpty()) buildArtistCards(artists);
        if (!albums.isEmpty()) buildAlbumCards(albums);
    }
}
