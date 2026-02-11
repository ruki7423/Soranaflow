#include "PlaylistDetailView.h"
#include <QRandomGenerator>
#include "../../core/ThemeManager.h"
#include "../../core/library/LibraryDatabase.h"
#include "../../metadata/MetadataService.h"
#include "../dialogs/MetadataSearchDialog.h"

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

PlaylistDetailView::PlaylistDetailView(QWidget* parent)
    : QWidget(parent)
    , m_backBtn(nullptr)
    , m_coverLabel(nullptr)
    , m_typeLabel(nullptr)
    , m_nameLabel(nullptr)
    , m_descLabel(nullptr)
    , m_statsLabel(nullptr)
    , m_playAllBtn(nullptr)
    , m_shuffleBtn(nullptr)
    , m_trackTable(nullptr)
    , m_scrollArea(nullptr)
{
    setupUI();
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI
// ═════════════════════════════════════════════════════════════════════

void PlaylistDetailView::setupUI()
{
    setObjectName(QStringLiteral("PlaylistDetailView"));

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // ── Scroll Area ─────────────────────────────────────────────────
    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);

    auto* scrollContent = new QWidget(m_scrollArea);
    scrollContent->setObjectName(QStringLiteral("PlaylistDetailScrollContent"));

    auto* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(24, 16, 24, 24);
    contentLayout->setSpacing(24);

    // ────────────────────────────────────────────────────────────────
    //  Back Button
    // ────────────────────────────────────────────────────────────────
    m_backBtn = new StyledButton(ThemeManager::instance()->themedIcon(QStringLiteral(":/icons/chevron-left.svg")),
                                  QString(), QStringLiteral("ghost"),
                                  scrollContent);
    m_backBtn->setFixedSize(32, 32);
    m_backBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_backBtn->setToolTip(QStringLiteral("Back to Playlists"));
    connect(m_backBtn, &QPushButton::clicked,
            this, &PlaylistDetailView::backRequested);
    contentLayout->addWidget(m_backBtn, 0, Qt::AlignLeft);

    // ────────────────────────────────────────────────────────────────
    //  Header Section
    // ────────────────────────────────────────────────────────────────
    auto* headerWidget = new QWidget(scrollContent);
    auto* headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(24);

    // ── Cover Art ───────────────────────────────────────────────────
    m_coverLabel = new QLabel(headerWidget);
    m_coverLabel->setFixedSize(192, 192);
    m_coverLabel->setAlignment(Qt::AlignCenter);
    m_coverLabel->setText(QStringLiteral("\u266B"));
    m_coverLabel->setStyleSheet(
        QStringLiteral(
            "QLabel {"
            "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
            "    stop:0 #4A9EFF, stop:1 #7C3AED);"
            "  border-radius: 12px;"
            "  color: rgba(255, 255, 255, 0.8);"
            "  font-size: 48px;"
            "}"));
    headerLayout->addWidget(m_coverLabel, 0, Qt::AlignTop);

    // ── Right Info Column ───────────────────────────────────────────
    auto* infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(8);

    m_typeLabel = new QLabel(QStringLiteral("PLAYLIST"), headerWidget);
    m_typeLabel->setStyleSheet(
        QString("color: %1; font-size: 11px; text-transform: uppercase;"
            " letter-spacing: 1px;").arg(ThemeManager::instance()->colors().foregroundMuted));
    infoLayout->addWidget(m_typeLabel);

    m_nameLabel = new QLabel(headerWidget);
    m_nameLabel->setStyleSheet(
        QString("color: %1; font-size: 32px; font-weight: bold;").arg(ThemeManager::instance()->colors().foreground));
    m_nameLabel->setWordWrap(true);
    infoLayout->addWidget(m_nameLabel);

    m_descLabel = new QLabel(headerWidget);
    m_descLabel->setStyleSheet(
        QString("color: %1; font-size: 14px;").arg(ThemeManager::instance()->colors().foregroundMuted));
    m_descLabel->setWordWrap(true);
    m_descLabel->setMaximumHeight(60);
    infoLayout->addWidget(m_descLabel);

    m_statsLabel = new QLabel(headerWidget);
    m_statsLabel->setStyleSheet(
        QString("color: %1; font-size: 13px;").arg(ThemeManager::instance()->colors().foregroundMuted));
    infoLayout->addWidget(m_statsLabel);

    // ── Action Buttons ──────────────────────────────────────────────
    auto* actionsLayout = new QHBoxLayout();
    actionsLayout->setSpacing(12);

    m_playAllBtn = new StyledButton(QStringLiteral("Play All"),
                                     QStringLiteral("default"),
                                     headerWidget);
    m_playAllBtn->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-start")));
    actionsLayout->addWidget(m_playAllBtn);

    m_shuffleBtn = new StyledButton(QStringLiteral("Shuffle"),
                                     QStringLiteral("outline"),
                                     headerWidget);
    m_shuffleBtn->setIcon(QIcon::fromTheme(QStringLiteral("media-playlist-shuffle")));
    actionsLayout->addWidget(m_shuffleBtn);

    actionsLayout->addStretch();
    infoLayout->addLayout(actionsLayout);

    infoLayout->addStretch();
    headerLayout->addLayout(infoLayout, 1);

    contentLayout->addWidget(headerWidget);

    // ────────────────────────────────────────────────────────────────
    //  Track Table (embedded inside scroll area)
    // ────────────────────────────────────────────────────────────────
    m_trackTable = new TrackTableView(playlistDetailConfig(), scrollContent);
    m_trackTable->setEmbeddedMode(true);
    contentLayout->addWidget(m_trackTable);

    contentLayout->addStretch();

    // ── Finalize scroll area ────────────────────────────────────────
    m_scrollArea->setWidget(scrollContent);
    outerLayout->addWidget(m_scrollArea);
    setLayout(outerLayout);

    // ── Theme ────────────────────────────────────────────────────────
    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &PlaylistDetailView::refreshTheme);
}

// ═════════════════════════════════════════════════════════════════════
//  setPlaylist
// ═════════════════════════════════════════════════════════════════════

void PlaylistDetailView::setPlaylist(const QString& playlistId)
{
    m_playlist = MusicDataProvider::instance()->playlistById(playlistId);
    updateDisplay();
}

// ═════════════════════════════════════════════════════════════════════
//  updateDisplay
// ═════════════════════════════════════════════════════════════════════

void PlaylistDetailView::updateDisplay()
{
    // ── Update labels ───────────────────────────────────────────────
    m_nameLabel->setText(m_playlist.name);
    m_descLabel->setText(m_playlist.description);

    // ── Calculate total duration ────────────────────────────────────
    int totalSeconds = 0;
    for (const auto& track : m_playlist.tracks) {
        totalSeconds += track.duration;
    }
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;

    QString statsText;
    if (hours > 0) {
        statsText = QStringLiteral("%1 tracks \u00B7 %2h %3m")
                        .arg(m_playlist.tracks.size())
                        .arg(hours)
                        .arg(minutes);
    } else {
        statsText = QStringLiteral("%1 tracks \u00B7 %2m")
                        .arg(m_playlist.tracks.size())
                        .arg(minutes);
    }
    m_statsLabel->setText(statsText);

    // ── Update cover gradient ───────────────────────────────────────
    QString gradient;
    if (m_playlist.isSmartPlaylist) {
        gradient = QStringLiteral(
            "qlineargradient(x1:0, y1:0, x2:1, y2:1,"
            " stop:0 #4A9EFF, stop:1 #7C3AED)");
    } else {
        gradient = QStringLiteral(
            "qlineargradient(x1:0, y1:0, x2:1, y2:1,"
            " stop:0 #2ECC71, stop:1 #4A9EFF)");
    }
    m_coverLabel->setStyleSheet(
        QStringLiteral(
            "QLabel {"
            "  background: %1;"
            "  border-radius: 12px;"
            "  color: rgba(255, 255, 255, 0.8);"
            "  font-size: 48px;"
            "}").arg(gradient));

    // ── Update track table ──────────────────────────────────────────
    m_trackTable->setTracks(m_playlist.tracks);

    // ── Reconnect track table signals ───────────────────────────────
    disconnect(m_trackTable, &TrackTableView::trackDoubleClicked, nullptr, nullptr);
    connect(m_trackTable, &TrackTableView::trackDoubleClicked, this, [this](const Track& t) {
        QVector<Track> queue = m_playlist.tracks;
        PlaybackState::instance()->setQueue(queue);
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

    disconnect(m_trackTable, &TrackTableView::undoMetadataRequested, nullptr, nullptr);
    connect(m_trackTable, &TrackTableView::undoMetadataRequested, this, [](const Track& t) {
        auto* db = LibraryDatabase::instance();
        auto fresh = db->trackById(t.id);
        if (fresh.has_value())
            db->updateAlbumsAndArtistsForTrack(fresh.value());
        MusicDataProvider::instance()->reloadFromDatabase();
    });

    disconnect(m_trackTable, &TrackTableView::identifyByAudioRequested, nullptr, nullptr);
    connect(m_trackTable, &TrackTableView::identifyByAudioRequested, this, [this](const Track& t) {
        MetadataService::instance()->identifyByFingerprint(t);
    });

    // ── Connect play all / shuffle buttons ──────────────────────────
    disconnect(m_playAllBtn, nullptr, nullptr, nullptr);
    disconnect(m_shuffleBtn, nullptr, nullptr, nullptr);

    connect(m_playAllBtn, &QPushButton::clicked, this, [this]() {
        if (!m_playlist.tracks.isEmpty()) {
            PlaybackState::instance()->setQueue(m_playlist.tracks);
            PlaybackState::instance()->playTrack(m_playlist.tracks.first());
        }
    });

    connect(m_shuffleBtn, &QPushButton::clicked, this, [this]() {
        if (!m_playlist.tracks.isEmpty()) {
            QVector<Track> shuffled = m_playlist.tracks;
            for (int i = shuffled.size() - 1; i > 0; --i) {
                int j = QRandomGenerator::global()->bounded(i + 1);
                shuffled.swapItemsAt(i, j);
            }
            PlaybackState::instance()->setQueue(shuffled);
            PlaybackState::instance()->playTrack(shuffled.first());
        }
    });
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void PlaylistDetailView::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();

    m_typeLabel->setStyleSheet(
        QString("color: %1; font-size: 11px; text-transform: uppercase;"
            " letter-spacing: 1px;").arg(c.foregroundMuted));
    m_nameLabel->setStyleSheet(
        QString("color: %1; font-size: 32px; font-weight: bold;").arg(c.foreground));
    m_descLabel->setStyleSheet(
        QString("color: %1; font-size: 14px;").arg(c.foregroundMuted));
    m_statsLabel->setStyleSheet(
        QString("color: %1; font-size: 13px;").arg(c.foregroundMuted));
}
