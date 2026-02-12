#include "ArtistDetailView.h"
#include "ApiKeys.h"
#include <QCoreApplication>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFontMetrics>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QTimer>
#include "../../core/ThemeManager.h"
#include "../../core/audio/MetadataReader.h"
#include "../../core/library/LibraryDatabase.h"
#include "../../metadata/MetadataService.h"
#include "../../metadata/FanartTvProvider.h"
#include "../../metadata/MusicBrainzProvider.h"
#include "../dialogs/MetadataSearchDialog.h"

ArtistDetailView::ArtistDetailView(QWidget* parent)
    : QWidget(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_heroBackground(nullptr)
    , m_backBtn(nullptr)
    , m_artistImage(nullptr)
    , m_nameLabel(nullptr)
    , m_statsLabel(nullptr)
    , m_genreBadgesContainer(nullptr)
    , m_playAllBtn(nullptr)
    , m_shuffleBtn(nullptr)
    , m_popularTracksTable(nullptr)
    , m_bioHeader(nullptr)
    , m_bioLabel(nullptr)
    , m_albumsContainer(nullptr)
    , m_albumsGridLayout(nullptr)
    , m_scrollArea(nullptr)
{
    setupUI();

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &ArtistDetailView::refreshTheme);

    // Fanart.tv image signals
    connect(FanartTvProvider::instance(), &FanartTvProvider::artistThumbDownloaded,
            this, &ArtistDetailView::onArtistThumbDownloaded);
    connect(FanartTvProvider::instance(), &FanartTvProvider::artistBackgroundDownloaded,
            this, &ArtistDetailView::onArtistBackgroundDownloaded);

    // When cached images are available immediately via artistImagesFetched
    connect(FanartTvProvider::instance(), &FanartTvProvider::artistImagesFetched,
            this, [this](const QString& mbid, const ArtistImages& images) {
        if (mbid != m_artistMbid) return;

        // Load cached thumb if file exists on disk
        if (!images.artistThumb.isEmpty() && QFile::exists(images.artistThumb)) {
            QPixmap pix(images.artistThumb);
            if (!pix.isNull()) applyCircularPixmap(pix);
        }
        // Load cached background if file exists on disk
        if (!images.artistBackground.isEmpty() && QFile::exists(images.artistBackground)) {
            QPixmap pix(images.artistBackground);
            if (!pix.isNull()) {
                m_heroFromFanart = true;
                applyHeroPixmap(pix);
            }
        }

        // Only fall back if Fanart.tv truly has NO images for this artist.
        // If URLs exist but aren't cached yet, downloads are in progress —
        // onArtistBackgroundDownloaded / onArtistThumbDownloaded will fire later.
        bool hasAnyImages = !images.allThumbs.isEmpty() || !images.allBackgrounds.isEmpty();
        if (!hasAnyImages) {
            applyAlbumArtFallback();
        }
    });

    // Fanart.tv returned 404 or no images for this artist
    connect(FanartTvProvider::instance(), &FanartTvProvider::artistImagesNotFound,
            this, [this](const QString& mbid) {
        if (mbid != m_artistMbid) return;
        applyAlbumArtFallback();
    });
}

void ArtistDetailView::setupUI()
{
    setObjectName(QStringLiteral("ArtistDetailView"));

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* contentWidget = new QWidget(m_scrollArea);
    auto* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(0, 0, 0, 24);
    contentLayout->setSpacing(0);

    // ── Hero background banner ────────────────────────────────────────
    m_heroBackground = new QLabel(contentWidget);
    m_heroBackground->setFixedHeight(300);
    m_heroBackground->setAlignment(Qt::AlignCenter);
    m_heroBackground->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_heroBackground->setScaledContents(false);
    m_heroBackground->setStyleSheet(
        QStringLiteral("background: %1; border-bottom-left-radius: 12px; border-bottom-right-radius: 12px;")
            .arg(ThemeManager::instance()->colors().backgroundSecondary));
    m_heroBackground->setVisible(false);
    contentLayout->addWidget(m_heroBackground);

    // Spacer after hero (or top padding when hero is hidden)
    contentLayout->addSpacing(16);

    // ── Back button ───────────────────────────────────────────────────
    auto* innerContent = new QWidget(contentWidget);
    auto* innerLayout = new QVBoxLayout(innerContent);
    innerLayout->setContentsMargins(24, 0, 24, 0);
    innerLayout->setSpacing(24);

    m_backBtn = new StyledButton(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/chevron-left.svg")),
                                 QString(), QStringLiteral("ghost"), innerContent);
    m_backBtn->setFixedSize(32, 32);
    m_backBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_backBtn->setToolTip(QStringLiteral("Back to Artists"));
    connect(m_backBtn, &QPushButton::clicked, this, &ArtistDetailView::backRequested);
    innerLayout->addWidget(m_backBtn, 0, Qt::AlignLeft);

    // ── Header section ────────────────────────────────────────────────
    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(24);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    // Artist circular image
    m_artistImage = new QLabel(innerContent);
    m_artistImage->setFixedSize(192, 192);
    m_artistImage->setAlignment(Qt::AlignCenter);
    m_artistImage->setStyleSheet(
        QStringLiteral("background: ") + ThemeManager::instance()->colors().backgroundSecondary
        + QStringLiteral(";"
        "border-radius: 96px;"
        "color: ") + ThemeManager::instance()->colors().foreground
        + QStringLiteral(";"
        "font-size: 48px;"
        "font-weight: bold;"
    ));
    headerLayout->addWidget(m_artistImage, 0, Qt::AlignTop);

    // Right info column
    auto* infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(8);
    infoLayout->setContentsMargins(0, 8, 0, 0);

    // "ARTIST" label
    auto* typeLabel = new QLabel(QStringLiteral("ARTIST"), innerContent);
    typeLabel->setStyleSheet(
        QStringLiteral("color: ") + ThemeManager::instance()->colors().foregroundMuted
        + QStringLiteral(";"
        "font-size: 11px;"
        "text-transform: uppercase;"
        "letter-spacing: 2px;"
    ));
    infoLayout->addWidget(typeLabel);

    // Name
    m_nameLabel = new QLabel(innerContent);
    m_nameLabel->setStyleSheet(
        QStringLiteral("color: ") + ThemeManager::instance()->colors().foreground
        + QStringLiteral("; font-size: 36px; font-weight: bold;"));
    m_nameLabel->setWordWrap(true);
    infoLayout->addWidget(m_nameLabel);

    // Stats line
    m_statsLabel = new QLabel(innerContent);
    m_statsLabel->setStyleSheet(
        QStringLiteral("color: ") + ThemeManager::instance()->colors().foregroundMuted
        + QStringLiteral("; font-size: 14px;"));
    infoLayout->addWidget(m_statsLabel);

    // Genre badges container
    m_genreBadgesContainer = new QWidget(innerContent);
    auto* genreBadgesLayout = new QHBoxLayout(m_genreBadgesContainer);
    genreBadgesLayout->setContentsMargins(0, 4, 0, 4);
    genreBadgesLayout->setSpacing(8);
    genreBadgesLayout->setAlignment(Qt::AlignLeft);
    infoLayout->addWidget(m_genreBadgesContainer);

    // Action buttons
    auto* actionsLayout = new QHBoxLayout();
    actionsLayout->setSpacing(12);
    actionsLayout->setContentsMargins(0, 8, 0, 0);

    const int DETAIL_BTN_H = 36;

    m_playAllBtn = new StyledButton(QStringLiteral("\u25B6  Play All"), QStringLiteral("default"), innerContent);
    m_playAllBtn->setFixedHeight(DETAIL_BTN_H);
    m_shuffleBtn = new StyledButton(QStringLiteral("\u2928  Shuffle"), QStringLiteral("outline"), innerContent);
    m_shuffleBtn->setFixedHeight(DETAIL_BTN_H);

    actionsLayout->addWidget(m_playAllBtn);
    actionsLayout->addWidget(m_shuffleBtn);
    actionsLayout->addStretch();
    infoLayout->addLayout(actionsLayout);

    infoLayout->addStretch();
    headerLayout->addLayout(infoLayout, 1);
    innerLayout->addLayout(headerLayout);

    // ── Popular Tracks section ────────────────────────────────────────
    auto* popularHeader = new QLabel(QStringLiteral("Popular Tracks"), innerContent);
    popularHeader->setStyleSheet(
        QStringLiteral("color: ") + ThemeManager::instance()->colors().foreground
        + QStringLiteral("; font-size: 18px; font-weight: bold;"));
    innerLayout->addWidget(popularHeader);

    m_popularTracksTable = new TrackTableView(artistDetailConfig(), innerContent);
    m_popularTracksTable->setEmbeddedMode(true);
    innerLayout->addWidget(m_popularTracksTable);

    // ── Biography section ─────────────────────────────────────────────
    m_bioHeader = new QLabel(QStringLiteral("About"), innerContent);
    m_bioHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 18px; font-weight: bold;")
            .arg(ThemeManager::instance()->colors().foreground));
    m_bioHeader->setVisible(false);
    innerLayout->addWidget(m_bioHeader);

    m_bioLabel = new QLabel(innerContent);
    m_bioLabel->setWordWrap(true);
    m_bioLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; line-height: 1.6;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    m_bioLabel->setVisible(false);
    m_bioLabel->setTextFormat(Qt::PlainText);
    innerLayout->addWidget(m_bioLabel);

    // ── Albums section ────────────────────────────────────────────────
    auto* albumsHeader = new QLabel(QStringLiteral("Albums"), innerContent);
    albumsHeader->setStyleSheet(
        QStringLiteral("color: ") + ThemeManager::instance()->colors().foreground
        + QStringLiteral("; font-size: 18px; font-weight: bold;"));
    innerLayout->addWidget(albumsHeader);

    m_albumsContainer = new QWidget(innerContent);
    m_albumsGridLayout = new QGridLayout(m_albumsContainer);
    m_albumsGridLayout->setContentsMargins(0, 0, 0, 0);
    m_albumsGridLayout->setSpacing(16);
    m_albumsGridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    innerLayout->addWidget(m_albumsContainer);

    innerLayout->addStretch();
    contentLayout->addWidget(innerContent, 1);

    m_scrollArea->setWidget(contentWidget);
    outerLayout->addWidget(m_scrollArea);
}

void ArtistDetailView::setArtist(const QString& artistId)
{
    m_artist = MusicDataProvider::instance()->artistById(artistId);
    updateDisplay();
}

void ArtistDetailView::updateDisplay()
{
    // ── Cancel pending Last.fm request ──────────────────────────────────
    if (m_pendingLastFmReply) {
        m_pendingLastFmReply->abort();
        m_pendingLastFmReply = nullptr;
    }

    // ── Reset hero + bio ────────────────────────────────────────────────
    m_heroBackground->setVisible(false);
    m_heroBackground->clear();
    m_heroFromFanart = false;
    m_bioHeader->setVisible(false);
    m_bioLabel->setVisible(false);
    m_bioLabel->clear();

    // ── Artist image ────────────────────────────────────────────────────
    loadArtistImage();

    // ── Fanart.tv + biography fetch ─────────────────────────────────────
    m_artistMbid = LibraryDatabase::instance()->artistMbidForArtist(m_artist.id);
    if (!m_artistMbid.isEmpty()) {
        fetchFanartImages();
        fetchBiography();
    } else {
        // Search MusicBrainz for MBID
        connect(MusicBrainzProvider::instance(), &MusicBrainzProvider::artistFound,
                this, [this](const QString& mbid, const QJsonObject&) {
            if (!mbid.isEmpty()) {
                m_artistMbid = mbid;
                fetchFanartImages();
                fetchBiography();
            }
        }, Qt::SingleShotConnection);
        MusicBrainzProvider::instance()->searchArtist(m_artist.name);
    }

    // ── Name ──────────────────────────────────────────────────────────
    m_nameLabel->setText(m_artist.name);

    // ── Calculate stats ───────────────────────────────────────────────
    int totalTracks = 0;
    int totalDuration = 0;
    for (const Album& album : std::as_const(m_artist.albums)) {
        totalTracks += album.tracks.size();
        totalDuration += album.duration;
    }

    int hours = totalDuration / 3600;
    int minutes = (totalDuration % 3600) / 60;
    QString durationStr;
    if (hours > 0)
        durationStr = QString::number(hours) + QStringLiteral("h ") + QString::number(minutes) + QStringLiteral("m");
    else
        durationStr = QString::number(minutes) + QStringLiteral("m");

    m_statsLabel->setText(
        QString::number(m_artist.albums.size()) + QStringLiteral(" albums \u00B7 ") +
        QString::number(totalTracks) + QStringLiteral(" tracks \u00B7 ") +
        durationStr
    );

    // ── Genre badges ──────────────────────────────────────────────────
    QLayout* genreLayout = m_genreBadgesContainer->layout();
    QLayoutItem* child;
    while ((child = genreLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = child->widget()) w->deleteLater();
        delete child;
    }

    for (const QString& genre : std::as_const(m_artist.genres)) {
        auto c = ThemeManager::instance()->colors();
        auto* badge = new QLabel(genre, m_genreBadgesContainer);
        badge->setStyleSheet(
            QStringLiteral("background: %1;"
            "border-radius: 12px;"
            "padding: 4px 12px;"
            "color: %2;"
            "font-size: 12px;")
                .arg(c.hover, c.foreground));
        genreLayout->addWidget(badge);
    }

    // ── Popular tracks ────────────────────────────────────────────────
    QVector<Track> allTracks;
    for (const Album& album : std::as_const(m_artist.albums)) {
        allTracks.append(album.tracks);
    }

    int trackLimit = qMin(allTracks.size(), 10);
    QVector<Track> popularTracks = allTracks.mid(0, trackLimit);
    m_popularTracksTable->setTracks(popularTracks);

    // Connect double-click
    disconnect(m_popularTracksTable, &TrackTableView::trackDoubleClicked, nullptr, nullptr);
    connect(m_popularTracksTable, &TrackTableView::trackDoubleClicked, this, [this, allTracks](const Track& track) {
        PlaybackState::instance()->setQueue(allTracks);
        PlaybackState::instance()->playTrack(track);
    });

    disconnect(m_popularTracksTable, &TrackTableView::fixMetadataRequested, nullptr, nullptr);
    connect(m_popularTracksTable, &TrackTableView::fixMetadataRequested, this, [this](const Track& t) {
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
            db->updateAlbumsAndArtistsForTrack(updated);

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

    disconnect(m_popularTracksTable, &TrackTableView::undoMetadataRequested, nullptr, nullptr);
    connect(m_popularTracksTable, &TrackTableView::undoMetadataRequested, this, [](const Track& t) {
        auto* db = LibraryDatabase::instance();
        auto fresh = db->trackById(t.id);
        if (fresh.has_value())
            db->updateAlbumsAndArtistsForTrack(fresh.value());
        MusicDataProvider::instance()->reloadFromDatabase();
    });

    disconnect(m_popularTracksTable, &TrackTableView::identifyByAudioRequested, nullptr, nullptr);
    connect(m_popularTracksTable, &TrackTableView::identifyByAudioRequested, this, [this](const Track& t) {
        MetadataService::instance()->identifyByFingerprint(t);
    });

    // ── Albums grid ───────────────────────────────────────────────────
    QLayoutItem* albumChild;
    while ((albumChild = m_albumsGridLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = albumChild->widget()) w->deleteLater();
        delete albumChild;
    }

    const int columns = 4;
    int row = 0;
    int col = 0;

    for (const Album& album : std::as_const(m_artist.albums)) {
        auto* albumCard = new QWidget(m_albumsContainer);
        albumCard->setObjectName(QStringLiteral("ArtistAlbumCard"));
        albumCard->setCursor(Qt::PointingHandCursor);

        auto* albumCardLayout = new QVBoxLayout(albumCard);
        albumCardLayout->setContentsMargins(0, 0, 0, 0);
        albumCardLayout->setSpacing(8);

        auto* coverLabel = new QLabel(albumCard);
        coverLabel->setFixedSize(160, 160);
        coverLabel->setAlignment(Qt::AlignCenter);

        // Try to load album cover art
        QPixmap albumPix = findAlbumCoverArt(album);
        if (!albumPix.isNull()) {
            QPixmap scaled = albumPix.scaled(160, 160, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            if (scaled.width() > 160 || scaled.height() > 160) {
                int cx = (scaled.width() - 160) / 2;
                int cy = (scaled.height() - 160) / 2;
                scaled = scaled.copy(cx, cy, 160, 160);
            }
            QPixmap rounded(160, 160);
            rounded.fill(Qt::transparent);
            QPainter coverPainter(&rounded);
            coverPainter.setRenderHint(QPainter::Antialiasing);
            QPainterPath coverPath;
            coverPath.addRoundedRect(0, 0, 160, 160, 8, 8);
            coverPainter.setClipPath(coverPath);
            coverPainter.drawPixmap(0, 0, scaled);
            coverPainter.end();
            coverLabel->setPixmap(rounded);
            coverLabel->setStyleSheet(QStringLiteral("background: transparent; border-radius: 8px;"));
        } else {
            {
                auto c = ThemeManager::instance()->colors();
                coverLabel->setStyleSheet(
                    QStringLiteral("background: %1;"
                    "border-radius: 8px;"
                    "color: %2;"
                    "font-size: 14px;")
                        .arg(c.backgroundSecondary, c.foregroundMuted));
            }
            coverLabel->setText(QStringLiteral("\u266B"));
        }
        albumCardLayout->addWidget(coverLabel);

        auto* titleLabel = new QLabel(albumCard);
        titleLabel->setStyleSheet(
            QStringLiteral("color: ") + ThemeManager::instance()->colors().foreground
            + QStringLiteral("; font-weight: bold; font-size: 14px;"));
        titleLabel->setWordWrap(false);
        QFontMetrics fm(titleLabel->font());
        titleLabel->setText(fm.elidedText(album.title, Qt::ElideRight, 160));
        titleLabel->setToolTip(album.title);
        albumCardLayout->addWidget(titleLabel);

        QString metaText;
        if (album.year > 0)
            metaText += QString::number(album.year) + QStringLiteral(" \u00B7 ");
        metaText += QString::number(album.totalTracks) + QStringLiteral(" tracks");
        auto* metaLabel = new QLabel(metaText, albumCard);
        metaLabel->setStyleSheet(
            QStringLiteral("color: ") + ThemeManager::instance()->colors().foregroundMuted
            + QStringLiteral("; font-size: 12px;"));
        albumCardLayout->addWidget(metaLabel);

        QString albumId = album.id;
        albumCard->setProperty("albumId", albumId);
        albumCard->installEventFilter(this);

        m_albumsGridLayout->addWidget(albumCard, row, col);

        col++;
        if (col >= columns) {
            col = 0;
            row++;
        }
    }

    // ── Play All button connection ────────────────────────────────────
    disconnect(m_playAllBtn, nullptr, nullptr, nullptr);
    connect(m_playAllBtn, &QPushButton::clicked, this, [this, allTracks]() {
        if (!allTracks.isEmpty()) {
            PlaybackState::instance()->setQueue(allTracks);
            PlaybackState::instance()->playTrack(allTracks.first());
        }
    });

    // ── Shuffle button connection ─────────────────────────────────────
    disconnect(m_shuffleBtn, nullptr, nullptr, nullptr);
    connect(m_shuffleBtn, &QPushButton::clicked, this, [this, allTracks]() {
        if (!allTracks.isEmpty()) {
            PlaybackState::instance()->setQueue(allTracks);
            if (!PlaybackState::instance()->shuffleEnabled())
                PlaybackState::instance()->toggleShuffle();
            PlaybackState::instance()->playTrack(allTracks.first());
        }
    });
}

// ── findAlbumCoverArt ────────────────────────────────────────────────
QPixmap ArtistDetailView::findAlbumCoverArt(const Album& album)
{
    QPixmap pix;

    // Tier 1: coverUrl
    if (!album.coverUrl.isEmpty()) {
        QString loadPath = album.coverUrl;
        if (loadPath.startsWith(QStringLiteral("qrc:")))
            loadPath = loadPath.mid(3);
        if (QFile::exists(loadPath)) {
            pix.load(loadPath);
            if (!pix.isNull()) return pix;
        }
    }

    QString firstTrackPath;
    for (const auto& t : album.tracks) {
        if (!t.filePath.isEmpty()) {
            firstTrackPath = t.filePath;
            break;
        }
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

    // Tier 4: any image file in folder
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

    return pix;
}

// ── loadArtistImage ─────────────────────────────────────────────────
void ArtistDetailView::loadArtistImage()
{
    constexpr int sz = 192;
    QPixmap pix;

    // Try artist coverUrl first
    if (!m_artist.coverUrl.isEmpty()) {
        QString loadPath = m_artist.coverUrl;
        if (loadPath.startsWith(QStringLiteral("qrc:")))
            loadPath = loadPath.mid(3);
        if (QFile::exists(loadPath))
            pix.load(loadPath);
    }

    // Try cover art from the first album
    if (pix.isNull() && !m_artist.albums.isEmpty()) {
        for (const Album& album : std::as_const(m_artist.albums)) {
            // Tier 1: album coverUrl
            if (!album.coverUrl.isEmpty()) {
                QString loadPath = album.coverUrl;
                if (loadPath.startsWith(QStringLiteral("qrc:")))
                    loadPath = loadPath.mid(3);
                if (QFile::exists(loadPath)) {
                    pix.load(loadPath);
                    if (!pix.isNull()) break;
                }
            }

            // Find first track path
            QString firstTrackPath;
            for (const auto& t : album.tracks) {
                if (!t.filePath.isEmpty()) {
                    firstTrackPath = t.filePath;
                    break;
                }
            }
            if (firstTrackPath.isEmpty()) continue;

            // Tier 2: folder image files
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
                    if (!pix.isNull()) break;
                }
            }
            if (!pix.isNull()) break;

            // Tier 3: embedded cover via FFmpeg
            pix = MetadataReader::extractCoverArt(firstTrackPath);
            if (!pix.isNull()) break;

            // Tier 4: any image in folder
            QDir dir(folder);
            QStringList imageFilters = {
                QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                QStringLiteral("*.png"), QStringLiteral("*.bmp")
            };
            QStringList images = dir.entryList(imageFilters, QDir::Files, QDir::Name);
            if (!images.isEmpty()) {
                pix.load(dir.filePath(images.first()));
                if (!pix.isNull()) break;
            }
        }
    }

    if (!pix.isNull()) {
        // Scale and crop to square
        QPixmap scaled = pix.scaled(sz, sz, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        if (scaled.width() > sz || scaled.height() > sz) {
            int x = (scaled.width() - sz) / 2;
            int y = (scaled.height() - sz) / 2;
            scaled = scaled.copy(x, y, sz, sz);
        }
        // Apply circular clipping
        QPixmap circular(sz, sz);
        circular.fill(Qt::transparent);
        QPainter painter(&circular);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addEllipse(0, 0, sz, sz);
        painter.setClipPath(path);
        painter.drawPixmap(0, 0, scaled);
        painter.end();

        m_artistImage->setPixmap(circular);
        m_artistImage->setStyleSheet(QStringLiteral("background: transparent; border-radius: 96px;"));
    } else {
        // Fallback: show initials
        QString initials;
        const QStringList nameParts = m_artist.name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& part : nameParts) {
            if (!part.isEmpty()) {
                initials += part.at(0).toUpper();
                if (initials.length() >= 2)
                    break;
            }
        }
        if (initials.isEmpty() && !m_artist.name.isEmpty())
            initials = m_artist.name.at(0).toUpper();
        m_artistImage->setText(initials);
        m_artistImage->setStyleSheet(
            QStringLiteral("background: ") + ThemeManager::instance()->colors().backgroundSecondary
            + QStringLiteral(";"
            "border-radius: 96px;"
            "color: ") + ThemeManager::instance()->colors().foreground
            + QStringLiteral(";"
            "font-size: 48px;"
            "font-weight: bold;"
        ));
    }
}

bool ArtistDetailView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget* card = qobject_cast<QWidget*>(obj);
        if (card && card->property("albumId").isValid()) {
            emit albumSelected(card->property("albumId").toString());
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ── Fanart.tv integration ────────────────────────────────────────────
void ArtistDetailView::fetchFanartImages()
{
    if (m_artistMbid.isEmpty()) return;

    // Check cache first — load immediately if available
    QString cachedThumb = FanartTvProvider::instance()->getCachedArtistThumb(m_artistMbid);
    if (!cachedThumb.isEmpty()) {
        QPixmap pix(cachedThumb);
        if (!pix.isNull()) applyCircularPixmap(pix);
    }

    QString cachedBg = FanartTvProvider::instance()->getCachedArtistBackground(m_artistMbid);
    if (!cachedBg.isEmpty()) {
        QPixmap pix(cachedBg);
        if (!pix.isNull()) applyHeroPixmap(pix);
    }

    // Fetch from network (will use cache internally if both exist)
    FanartTvProvider::instance()->fetchArtistImages(m_artistMbid);
}

void ArtistDetailView::fetchBiography()
{
    if (m_artistMbid.isEmpty()) {
        // No MBID — go straight to Last.fm by name
        fetchLastFmBio(m_artist.name);
        return;
    }

    connect(MusicBrainzProvider::instance(), &MusicBrainzProvider::artistFound,
            this, [this](const QString& mbid, const QJsonObject& data) {
        if (mbid != m_artistMbid) return;
        QString annotation = data[QStringLiteral("annotation")].toString().trimmed();
        if (!annotation.isEmpty()) {
            // Strip basic wiki markup if present
            annotation.remove(QRegularExpression(QStringLiteral("\\[\\[([^|\\]]*\\|)?([^\\]]*)\\]\\]"),
                                                  QRegularExpression::NoPatternOption));
            m_bioHeader->setVisible(true);
            m_bioLabel->setVisible(true);
            m_bioLabel->setText(annotation);
            qDebug() << "[ArtistDetail] MusicBrainz bio loaded for" << m_artistMbid
                     << "length:" << annotation.length();
        } else {
            // MusicBrainz annotation empty → fall back to Last.fm
            qDebug() << "[ArtistDetail] MusicBrainz annotation empty, trying Last.fm";
            fetchLastFmBio(m_artist.name);
        }
    }, Qt::SingleShotConnection);

    MusicBrainzProvider::instance()->lookupArtist(m_artistMbid);
}

void ArtistDetailView::onArtistThumbDownloaded(const QString& mbid,
                                                 const QPixmap& pix,
                                                 const QString&)
{
    if (mbid != m_artistMbid) return;
    applyCircularPixmap(pix);
}

void ArtistDetailView::onArtistBackgroundDownloaded(const QString& mbid,
                                                      const QPixmap& pix,
                                                      const QString&)
{
    if (mbid != m_artistMbid) return;
    m_heroFromFanart = true;
    applyHeroPixmap(pix);
}

void ArtistDetailView::applyCircularPixmap(const QPixmap& pix)
{
    constexpr int sz = 192;
    QPixmap scaled = pix.scaled(sz, sz, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() > sz || scaled.height() > sz) {
        int x = (scaled.width() - sz) / 2;
        int y = (scaled.height() - sz) / 2;
        scaled = scaled.copy(x, y, sz, sz);
    }
    QPixmap circular(sz, sz);
    circular.fill(Qt::transparent);
    QPainter painter(&circular);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addEllipse(0, 0, sz, sz);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, scaled);
    painter.end();

    m_artistImage->setPixmap(circular);
    m_artistImage->setStyleSheet(QStringLiteral("background: transparent; border-radius: 96px;"));
}

void ArtistDetailView::applyHeroPixmap(const QPixmap& pix)
{
    int heroW = m_heroBackground->width();
    if (heroW <= 0) heroW = width();
    if (heroW <= 0) heroW = 800; // fallback

    QPixmap scaled = pix.scaled(heroW, 300,
                                Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    int cx = (scaled.width() - heroW) / 2;
    int cy = (scaled.height() - 300) / 2;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    QPixmap cropped = scaled.copy(cx, cy, heroW, 300);

    // Apply dark gradient overlay for text readability
    QPainter p(&cropped);
    QLinearGradient grad(0, 0, 0, 300);
    grad.setColorAt(0, QColor(0, 0, 0, 0));
    grad.setColorAt(0.5, QColor(0, 0, 0, 80));
    grad.setColorAt(1, QColor(0, 0, 0, 180));
    p.fillRect(cropped.rect(), grad);
    p.end();

    m_heroBackground->setPixmap(cropped);
    m_heroBackground->setVisible(true);
    qDebug() << "[ArtistDetail] Hero background applied, size:" << cropped.size();
}

void ArtistDetailView::applyAlbumArtFallback()
{
    // Find best album cover from this artist
    QPixmap albumCover;
    for (const Album& album : std::as_const(m_artist.albums)) {
        albumCover = findAlbumCoverArt(album);
        if (!albumCover.isNull()) break;
    }
    if (albumCover.isNull()) {
        qDebug() << "[ArtistDetail] No album art for fallback hero";
        return;
    }

    // Hero: blur via scale-down-then-up trick + darken
    int heroW = m_heroBackground->width();
    if (heroW < 400) heroW = width();
    if (heroW < 400 && m_scrollArea) heroW = m_scrollArea->viewport()->width();
    if (heroW < 400) heroW = 1200;  // absolute fallback for pre-layout
    constexpr int heroH = 300;

    // Scale to tiny square first (IgnoreAspectRatio ensures square output for uniform blur)
    QPixmap small = albumCover.scaled(48, 48,
        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // Scale back up to fill hero area
    QPixmap blurred = small.scaled(heroW, heroH,
        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    // Center-crop to exact hero dimensions
    int cx = qMax(0, (blurred.width() - heroW) / 2);
    int cy = qMax(0, (blurred.height() - heroH) / 2);
    QPixmap cropped = blurred.copy(cx, cy, heroW, heroH);

    // Dark overlay
    QPainter p(&cropped);
    p.fillRect(cropped.rect(), QColor(0, 0, 0, 140));
    p.end();

    m_heroBackground->setPixmap(cropped);
    m_heroBackground->setVisible(true);
    m_heroFromFanart = false;
    qDebug() << "[ArtistDetail] Album art fallback hero applied, size:" << cropped.size();

    // If widget wasn't laid out yet, schedule a re-apply after layout
    if (m_heroBackground->width() < 400) {
        QTimer::singleShot(50, this, [this]() {
            if (!m_heroFromFanart && m_heroBackground->isVisible()
                && m_heroBackground->width() >= 400) {
                applyAlbumArtFallback();
            }
        });
    }
}

// ── Last.fm biography fallback ──────────────────────────────────────

void ArtistDetailView::fetchLastFmBio(const QString& artistName)
{
    QUrl url(QStringLiteral("https://ws.audioscrobbler.com/2.0/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("method"), QStringLiteral("artist.getinfo"));
    q.addQueryItem(QStringLiteral("artist"), artistName);
    q.addQueryItem(QStringLiteral("api_key"), QStringLiteral(LASTFM_API_KEY));
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("autocorrect"), QStringLiteral("1"));
    url.setQuery(q);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
        QStringLiteral("SoranaFlow/%1").arg(QCoreApplication::applicationVersion()).toUtf8());
    request.setTransferTimeout(10000);

    // Cancel any prior pending request
    if (m_pendingLastFmReply) {
        m_pendingLastFmReply->abort();
        m_pendingLastFmReply = nullptr;
    }

    QString currentMbid = m_artistMbid;
    auto* reply = m_network->get(request);
    m_pendingLastFmReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, currentMbid]() {
        reply->deleteLater();
        if (m_pendingLastFmReply == reply)
            m_pendingLastFmReply = nullptr;

        // Guard: artist may have changed while request was in flight
        if (currentMbid != m_artistMbid) return;

        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "[ArtistDetail] Last.fm error:" << reply->errorString();
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QString bio = doc.object()
                          [QStringLiteral("artist")].toObject()
                          [QStringLiteral("bio")].toObject()
                          [QStringLiteral("content")].toString();

        // Strip HTML tags
        bio.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
        // Strip "User-contributed text..." boilerplate
        int cutIdx = bio.indexOf(QStringLiteral("User-contributed text"));
        if (cutIdx > 0)
            bio = bio.left(cutIdx).trimmed();
        else
            bio = bio.trimmed();

        if (!bio.isEmpty()) {
            m_bioHeader->setVisible(true);
            m_bioLabel->setVisible(true);
            m_bioLabel->setText(bio);
            qDebug() << "[ArtistDetail] Last.fm bio loaded:" << bio.left(60) << "...";
        } else {
            qDebug() << "[ArtistDetail] Last.fm bio empty for" << m_artist.name;
        }
    });
}

void ArtistDetailView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // Re-apply hero background if visible
    if (m_heroBackground->isVisible() && !m_heroBackground->pixmap().isNull()) {
        if (m_heroFromFanart) {
            QString cachedBg = FanartTvProvider::instance()->getCachedArtistBackground(m_artistMbid);
            if (!cachedBg.isEmpty()) {
                QPixmap pix(cachedBg);
                if (!pix.isNull()) applyHeroPixmap(pix);
            }
        } else {
            // Re-apply album art fallback at new width
            applyAlbumArtFallback();
        }
    }
}

// ── refreshTheme ────────────────────────────────────────────────────
void ArtistDetailView::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    m_artistImage->setStyleSheet(
        QStringLiteral("background: %1;"
        "border-radius: 96px;"
        "color: %2;"
        "font-size: 48px;"
        "font-weight: bold;")
            .arg(c.backgroundSecondary, c.foreground));

    m_nameLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 36px; font-weight: bold;")
            .arg(c.foreground));

    m_statsLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(c.foregroundMuted));

    m_heroBackground->setStyleSheet(
        QStringLiteral("background: %1; border-bottom-left-radius: 12px; border-bottom-right-radius: 12px;")
            .arg(c.backgroundSecondary));

    m_bioHeader->setStyleSheet(
        QStringLiteral("color: %1; font-size: 18px; font-weight: bold;")
            .arg(c.foreground));

    m_bioLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; line-height: 1.6;")
            .arg(c.foregroundMuted));

    // Re-render display to update dynamic elements (genre badges, album cards)
    // Only if an artist is loaded — avoid crashes when view was created but never navigated to
    if (!m_artist.id.isEmpty())
        updateDisplay();
}
