#include "LibraryView.h"
#include "../../widgets/FormatBadge.h"
#include "../../core/ThemeManager.h"
#include "../../core/Settings.h"
#include "../../core/library/LibraryScanner.h"
#include "../../core/audio/MetadataReader.h"
#include "../../metadata/MetadataService.h"
#include "../dialogs/TagEditorDialog.h"
#include "../services/NavigationService.h"
#include "../dialogs/MetadataSearchDialog.h"
#include "../../core/library/LibraryDatabase.h"
#include "../services/MetadataFixService.h"

#include <QKeyEvent>
#include <QShowEvent>
#include <QFileDialog>
#include <QFileInfo>
#include "../dialogs/StyledMessageBox.h"
#include <QGraphicsOpacityEffect>
#include <QElapsedTimer>
#include <QTimer>
#include <algorithm>
#include <QRandomGenerator>
#include <QtConcurrent>

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

LibraryView::LibraryView(QWidget* parent)
    : QWidget(parent)
    , m_searchInput(nullptr)
    , m_openFilesBtn(nullptr)
    , m_trackTable(nullptr)
    , m_headerLabel(nullptr)
    , m_countLabel(nullptr)
    , m_metadataFixService(new MetadataFixService(this))
{
    setupUI();
    populateTracks();

    // ── Connect signals ────────────────────────────────────────────
    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(200);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, [this]() {
        onSearchChanged(m_searchInput->lineEdit()->text());
    });
    connect(m_searchInput->lineEdit(), &QLineEdit::textChanged,
            this, [this]() { m_searchDebounceTimer->start(); });
    m_searchInput->lineEdit()->installEventFilter(this);
    connect(PlaybackState::instance(), &PlaybackState::trackChanged,
            this, &LibraryView::onTrackChanged);
    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &LibraryView::refreshTheme);
    connect(MusicDataProvider::instance(), &MusicDataProvider::libraryUpdated,
            this, [this]() {
                if (!isVisible()) { m_libraryDirty = true; return; }
                onLibraryUpdated();
            });

    // ── Scanner progress feedback ──────────────────────────────────
    auto* scanner = LibraryScanner::instance();
    connect(scanner, &LibraryScanner::scanStarted, this, [this]() {
        m_scanBtn->setEnabled(false);
        m_scanBtn->setToolTip(QStringLiteral("Scanning..."));
    });
    connect(scanner, &LibraryScanner::scanProgress, this, [this](int current, int total) {
        m_scanBtn->setToolTip(QStringLiteral("Scanning... %1/%2").arg(current).arg(total));
    });
    connect(scanner, &LibraryScanner::scanFinished, this, [this](int tracksFound) {
        m_scanBtn->setEnabled(true);
        m_scanBtn->setToolTip(QStringLiteral("Rescan Library"));
        Q_UNUSED(tracksFound);
        MusicDataProvider::instance()->reloadFromDatabase();
    });

    // ── MetadataService progress feedback ───────────────────────────
    auto* metaSvc = MetadataService::instance();
    connect(metaSvc, &MetadataService::fetchProgress, this,
            [this](int current, int total, const QString& status) {
        if (MetadataService::instance()->isFingerprintBatch()) {
            m_identifyAudioBtn->setEnabled(false);
            m_identifyAudioBtn->setToolTip(
                QStringLiteral("Identifying: %1/%2\n%3").arg(current).arg(total).arg(status));
        } else {
            m_fetchMetadataBtn->setEnabled(false);
            m_fetchMetadataBtn->setToolTip(
                QStringLiteral("Fetching: %1/%2\n%3").arg(current).arg(total).arg(status));
        }
    });
    connect(metaSvc, &MetadataService::fetchComplete, this, [this]() {
        m_fetchMetadataBtn->setEnabled(true);
        m_fetchMetadataBtn->setToolTip(QStringLiteral("Fetch Missing Metadata"));
        m_identifyAudioBtn->setEnabled(true);
        m_identifyAudioBtn->setToolTip(
            QStringLiteral("Identify by Audio (Fingerprint)\nFor files with missing/wrong tags"));
    });

    // ── Single-track identify feedback ────────────────────────────
    connect(metaSvc, &MetadataService::identifyFailed, this,
            [this](const QString& /*trackId*/, const QString& message) {
        m_statusLabel->setText(message);
        m_statusLabel->setVisible(true);
        QTimer::singleShot(5000, this, [this]() {
            m_statusLabel->setVisible(false);
        });
    });

    connect(metaSvc, &MetadataService::metadataUpdated, this,
            [this](const QString& /*trackId*/, const Track& updated) {
        // Show success message for identified track
        QString msg = QStringLiteral("Identified: %1 - %2")
                          .arg(updated.artist, updated.title);
        m_statusLabel->setText(msg);
        m_statusLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; padding: 4px 0;")
                .arg(ThemeManager::instance()->colors().success));
        m_statusLabel->setVisible(true);
        QTimer::singleShot(5000, this, [this]() {
            m_statusLabel->setVisible(false);
            // Reset to warning color for next use
            m_statusLabel->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; padding: 4px 0;")
                    .arg(ThemeManager::instance()->colors().warning));
        });
    });

    // ── Initialize highlight for current track ─────────────────────
    const Track current = PlaybackState::instance()->currentTrack();
    if (!current.id.isEmpty()) {
        onTrackChanged(current);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI
// ═════════════════════════════════════════════════════════════════════

void LibraryView::setupUI()
{
    setObjectName(QStringLiteral("LibraryView"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(16);

    // ────────────────────────────────────────────────────────────────
    //  Header Section — unified toolbar (30px buttons, 8px spacing)
    // ────────────────────────────────────────────────────────────────
    const int NAV_SIZE = 30;
    const int BTN_H = 30;
    const QSize headerIconSize(UISizes::buttonIconSize, UISizes::buttonIconSize);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(8);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    auto c = ThemeManager::instance()->colors();

    m_headerLabel = new QLabel(QStringLiteral("Library"), this);
    m_headerLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 20px; font-weight: 600;")
            .arg(c.foreground));
    m_headerLabel->setFixedHeight(BTN_H);
    m_headerLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    headerLayout->addWidget(m_headerLabel);

    // ── Global navigation ← → ─────────────────────────────────────
    headerLayout->addSpacing(4);

    m_navBackBtn = new QPushButton(this);
    m_navBackBtn->setObjectName(QStringLiteral("hdrNavBack"));
    m_navBackBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_navBackBtn->setIconSize(headerIconSize);
    m_navBackBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navBackBtn->setCursor(Qt::PointingHandCursor);
    m_navBackBtn->setToolTip(QStringLiteral("Back"));
    m_navBackBtn->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(m_navBackBtn);

    m_navForwardBtn = new QPushButton(this);
    m_navForwardBtn->setObjectName(QStringLiteral("hdrNavForward"));
    m_navForwardBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));
    m_navForwardBtn->setIconSize(headerIconSize);
    m_navForwardBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navForwardBtn->setCursor(Qt::PointingHandCursor);
    m_navForwardBtn->setToolTip(QStringLiteral("Forward"));
    m_navForwardBtn->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(m_navForwardBtn);

    headerLayout->addSpacing(8);

    m_showAllBtn = new StyledButton("Show All", "outline");
    m_showAllBtn->setObjectName(QStringLiteral("hdrShowAll"));
    m_showAllBtn->setFixedHeight(BTN_H);
    m_showAllBtn->setFocusPolicy(Qt::NoFocus);
    m_showAllBtn->setVisible(false);
    connect(m_showAllBtn, &QPushButton::clicked,
            this, &LibraryView::showAllTracks);
    headerLayout->addWidget(m_showAllBtn);

    // Header button styles — uniform box model for pixel-perfect height.
    // ALL buttons use border: 1px solid (transparent or visible) so that
    // the box model is identical: 1px border + 28px content + 1px border = 30px.
    // Height is locked via C++ setFixedHeight(30) once and never touched again.
    const QString hdrGhostStyle = QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid transparent; border-radius: 6px;"
        "  color: %1; font-size: 13px; padding: 0px 12px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:pressed { background: %3; }").arg(c.foreground, c.hover, c.pressed);
    const QString hdrPrimaryStyle = QStringLiteral(
        "QPushButton { background: %1; border: 1px solid transparent; border-radius: 6px;"
        "  color: %2; font-size: 13px; padding: 0px 12px; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }").arg(c.accent, c.foregroundInverse, c.accentHover, c.accentPressed);
    const QString hdrOutlineStyle = QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; border-radius: 6px;"
        "  color: %2; font-size: 13px; padding: 0px 12px; }"
        "QPushButton:hover { background: %3; border-color: %4; }"
        "QPushButton:pressed { background: %5; }").arg(c.border, c.foreground, c.hover, c.foregroundMuted, c.pressed);

    m_showAllBtn->setStyleSheet(hdrOutlineStyle);

    // Play All button
    m_playAllBtn = new QPushButton(QStringLiteral("Play All"), this);
    m_playAllBtn->setObjectName(QStringLiteral("hdrPlayAll"));
    m_playAllBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/play.svg")));
    m_playAllBtn->setIconSize(headerIconSize);
    m_playAllBtn->setFixedHeight(BTN_H);
    m_playAllBtn->setCursor(Qt::PointingHandCursor);
    m_playAllBtn->setFocusPolicy(Qt::NoFocus);
    m_playAllBtn->setStyleSheet(hdrPrimaryStyle);

    connect(m_playAllBtn, &QPushButton::clicked,
            this, &LibraryView::onPlayAllClicked);
    headerLayout->addWidget(m_playAllBtn);

    // Scan / Rescan button
    m_scanBtn = new QPushButton(QStringLiteral("Rescan"), this);
    m_scanBtn->setObjectName(QStringLiteral("hdrRescan"));
    m_scanBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/refresh-cw.svg"));
    m_scanBtn->setIconSize(headerIconSize);
    m_scanBtn->setFixedHeight(BTN_H);
    m_scanBtn->setToolTip(QStringLiteral("Rescan Library"));
    m_scanBtn->setCursor(Qt::PointingHandCursor);
    m_scanBtn->setFocusPolicy(Qt::NoFocus);
    m_scanBtn->setStyleSheet(hdrGhostStyle);

    connect(m_scanBtn, &QPushButton::clicked,
            this, &LibraryView::onScanClicked);
    headerLayout->addWidget(m_scanBtn);

    // Fetch Metadata button (download icon)
    m_fetchMetadataBtn = new QPushButton(QStringLiteral("Metadata"), this);
    m_fetchMetadataBtn->setObjectName(QStringLiteral("hdrMetadata"));
    m_fetchMetadataBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/download.svg"));
    m_fetchMetadataBtn->setIconSize(headerIconSize);
    m_fetchMetadataBtn->setFixedHeight(BTN_H);
    m_fetchMetadataBtn->setToolTip(QStringLiteral("Fetch Missing Metadata"));
    m_fetchMetadataBtn->setCursor(Qt::PointingHandCursor);
    m_fetchMetadataBtn->setFocusPolicy(Qt::NoFocus);
    m_fetchMetadataBtn->setStyleSheet(hdrGhostStyle);

    connect(m_fetchMetadataBtn, &QPushButton::clicked,
            this, &LibraryView::onFetchMetadataClicked);
    headerLayout->addWidget(m_fetchMetadataBtn);

    // Identify by Audio button (fingerprint / music icon)
    m_identifyAudioBtn = new QPushButton(QStringLiteral("Identify"), this);
    m_identifyAudioBtn->setObjectName(QStringLiteral("hdrIdentify"));
    m_identifyAudioBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/music.svg"));
    m_identifyAudioBtn->setIconSize(headerIconSize);
    m_identifyAudioBtn->setFixedHeight(BTN_H);
    m_identifyAudioBtn->setToolTip(QStringLiteral("Identify by Audio (Fingerprint)\nFor files with missing/wrong tags"));
    m_identifyAudioBtn->setCursor(Qt::PointingHandCursor);
    m_identifyAudioBtn->setFocusPolicy(Qt::NoFocus);
    m_identifyAudioBtn->setStyleSheet(hdrGhostStyle);

    connect(m_identifyAudioBtn, &QPushButton::clicked,
            this, &LibraryView::onIdentifyAudioClicked);
    headerLayout->addWidget(m_identifyAudioBtn);

    headerLayout->addStretch();

    m_openFilesBtn = new StyledButton("Open Files", "default");
    m_openFilesBtn->setObjectName(QStringLiteral("hdrOpenFiles"));
    m_openFilesBtn->setFixedHeight(BTN_H);
    m_openFilesBtn->setFocusPolicy(Qt::NoFocus);
    m_openFilesBtn->setStyleSheet(hdrPrimaryStyle);

    connect(m_openFilesBtn, &QPushButton::clicked,
            this, &LibraryView::onOpenFilesClicked);
    headerLayout->addWidget(m_openFilesBtn);

    m_countLabel = new QLabel(QStringLiteral("0 tracks"), this);
    m_countLabel->setFixedHeight(BTN_H);
    m_countLabel->setAlignment(Qt::AlignVCenter);
    m_countLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(c.foregroundMuted));
    headerLayout->addWidget(m_countLabel);

    auto* nav = NavigationService::instance();
    auto updateNavBtnStyle = [this, nav]() {
        auto c = ThemeManager::instance()->colors();
        auto navStyle = [&c](bool enabled) {
            Q_UNUSED(enabled)
            return QStringLiteral(
                "QPushButton { background: transparent; border: none; border-radius: 4px; }"
                "QPushButton:hover { background: %1; }"
                "QPushButton:disabled { background: transparent; }").arg(c.hover);
        };
        bool canBack = nav->canGoBack();
        bool canFwd = nav->canGoForward();
        m_navBackBtn->setEnabled(canBack);
        m_navForwardBtn->setEnabled(canFwd);
        m_navBackBtn->setStyleSheet(navStyle(canBack));
        m_navForwardBtn->setStyleSheet(navStyle(canFwd));
    };
    updateNavBtnStyle();

    connect(m_navBackBtn, &QPushButton::clicked, nav, &NavigationService::navigateBack);
    connect(m_navForwardBtn, &QPushButton::clicked, nav, &NavigationService::navigateForward);
    connect(nav, &NavigationService::navChanged, this, updateNavBtnStyle);

    mainLayout->addLayout(headerLayout);

    // ── Search Input ───────────────────────────────────────────────
    m_searchInput = new StyledInput(QStringLiteral("Search tracks..."),
                                     QStringLiteral(""),
                                     this);
    mainLayout->addWidget(m_searchInput);

    // ── Status label (for identify feedback) ─────────────────────
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; padding: 4px 0;")
            .arg(c.warning));
    m_statusLabel->setVisible(false);
    mainLayout->addWidget(m_statusLabel);

    // ────────────────────────────────────────────────────────────────
    //  Track Table (scrollable, resizable columns)
    // ────────────────────────────────────────────────────────────────
    m_trackTable = new TrackTableView(libraryConfig(), this);

    connect(m_trackTable, &TrackTableView::trackDoubleClicked,
            this, [this](const Track& t) {
        // Build queue from visible TrackIndex entries
        auto* model = m_trackTable->hybridModel();
        QVector<Track> queue;
        queue.reserve(model->visibleCount());
        for (int i = 0; i < model->visibleCount(); ++i)
            queue.append(trackFromIndex(model->indexAt(i)));
        PlaybackState::instance()->setQueue(queue);
        PlaybackState::instance()->playTrack(t);
    });

    connect(m_trackTable, &TrackTableView::editTagsRequested,
            this, [this](const Track& t) {
        if (t.filePath.isEmpty()) return;
        auto* dlg = new TagEditorDialog(t.filePath, this);
        connect(dlg, &TagEditorDialog::tagsUpdated, this, &LibraryView::onLibraryUpdated);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->open();
    });

    m_metadataFixService->connectToTable(m_trackTable, this);

    // ── Artist / Album click navigation ─────────────────────────────
    connect(m_trackTable, &TrackTableView::artistClicked,
            this, &LibraryView::filterByArtist);
    connect(m_trackTable, &TrackTableView::albumClicked,
            this, &LibraryView::filterByAlbum);

    mainLayout->addWidget(m_trackTable, 1);
}

// ═════════════════════════════════════════════════════════════════════
//  populateTracks
// ═════════════════════════════════════════════════════════════════════

void LibraryView::populateTracks()
{
    QElapsedTimer t; t.start();
    auto indexes = MusicDataProvider::instance()->allTrackIndexes();
    qDebug() << "[TIMING] LibraryView allTrackIndexes():" << t.elapsed() << "ms";
    t.restart();
    m_trackTable->setIndexes(std::move(indexes));
    qDebug() << "[TIMING] LibraryView setIndexes():" << t.elapsed() << "ms";
    m_countLabel->setText(QStringLiteral("%1 tracks").arg(m_trackTable->visibleCount()));
    qDebug() << "[TIMING] LibraryView populateTracks DONE — user can now play";
}

// ═════════════════════════════════════════════════════════════════════
//  filterTracks
// ═════════════════════════════════════════════════════════════════════

void LibraryView::filterTracks(const QString& query)
{
    auto* model = m_trackTable->hybridModel();
    if (query.isEmpty())
        model->clearFilter();
    else
        model->setFilter(query);
    m_countLabel->setText(QStringLiteral("%1 tracks").arg(m_trackTable->visibleCount()));
}

// ═════════════════════════════════════════════════════════════════════
//  onSearchChanged
// ═════════════════════════════════════════════════════════════════════

void LibraryView::onSearchChanged(const QString& text)
{
    filterTracks(text);
}

// ═════════════════════════════════════════════════════════════════════
//  onTrackChanged
// ═════════════════════════════════════════════════════════════════════

void LibraryView::onTrackChanged(const Track& track)
{
    m_trackTable->setHighlightedTrackId(track.id);
}

// ═════════════════════════════════════════════════════════════════════
//  onOpenFilesClicked
// ═════════════════════════════════════════════════════════════════════

void LibraryView::onOpenFilesClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Open Audio Files"),
        QString(),
        QStringLiteral("Audio Files (*.flac *.mp3 *.wav *.aac *.m4a *.ogg *.alac *.aiff *.aif *.opus *.dsf *.dff *.wma);;All Files (*)")
    );

    if (!files.isEmpty()) {
        addTracksFromFiles(files);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  onScanClicked
// ═════════════════════════════════════════════════════════════════════

void LibraryView::onScanClicked()
{
    QStringList folders = Settings::instance()->libraryFolders();
    if (folders.isEmpty()) {
        // No folders configured — let user pick one
        QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select Music Folder"));
        if (dir.isEmpty()) return;
        Settings::instance()->addLibraryFolder(dir);
        folders.append(dir);
    }
    LibraryScanner::instance()->scanFolders(folders);
}

// ═════════════════════════════════════════════════════════════════════
//  onPlayAllClicked
// ═════════════════════════════════════════════════════════════════════

void LibraryView::onPlayAllClicked()
{
    auto* model = m_trackTable->hybridModel();
    if (model->visibleCount() == 0) return;

    QVector<Track> queue;
    queue.reserve(model->visibleCount());
    for (int i = 0; i < model->visibleCount(); ++i)
        queue.append(trackFromIndex(model->indexAt(i)));

    auto* ps = PlaybackState::instance();
    ps->setQueue(queue);

    if (ps->shuffleEnabled()) {
        int startIdx = QRandomGenerator::global()->bounded(queue.size());
        ps->playTrack(queue.at(startIdx));
    } else {
        ps->playTrack(queue.first());
    }
}

// ═════════════════════════════════════════════════════════════════════
//  onFetchMetadataClicked
// ═════════════════════════════════════════════════════════════════════

void LibraryView::onFetchMetadataClicked()
{
    auto allTracks = MusicDataProvider::instance()->allTracks();
    if (allTracks.isEmpty()) {
        StyledMessageBox::info(this, QStringLiteral("Metadata"),
            QStringLiteral("No tracks in library to fetch metadata for."));
        return;
    }

    if (StyledMessageBox::confirm(this, QStringLiteral("Fetch Metadata"),
            QStringLiteral("Fetch metadata for %1 tracks from MusicBrainz?\n\n"
                            "This may take a while due to API rate limits (1 request/sec).")
                .arg(allTracks.size()))) {
        MetadataService::instance()->fetchMissingMetadata(allTracks);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  onIdentifyAudioClicked
// ═════════════════════════════════════════════════════════════════════

void LibraryView::onIdentifyAudioClicked()
{
    auto allTracks = MusicDataProvider::instance()->allTracks();
    if (allTracks.isEmpty()) {
        StyledMessageBox::info(this, QStringLiteral("Identify by Audio"),
            QStringLiteral("No tracks in library to identify."));
        return;
    }

    // Find tracks that need identification
    QVector<Track> tracksToIdentify;
    for (const Track& track : allTracks) {
        bool needsId =
            track.title.isEmpty() ||
            track.title == QStringLiteral("Unknown") ||
            track.artist.isEmpty() ||
            track.artist == QStringLiteral("Unknown Artist") ||
            track.recordingMbid.isEmpty();

        if (needsId && !track.filePath.isEmpty())
            tracksToIdentify.append(track);
    }

    if (tracksToIdentify.isEmpty()) {
        StyledMessageBox::info(this, QStringLiteral("Identify by Audio"),
            QStringLiteral("All tracks already have metadata.\n\n"
                           "Tip: Right-click a specific track and select "
                           "'Identify by Audio...' to force re-identification."));
        return;
    }

    if (StyledMessageBox::confirm(this, QStringLiteral("Identify by Audio"),
            QStringLiteral("Found %1 tracks with missing or incomplete metadata.\n\n"
                            "This will analyze audio fingerprints to identify songs.\n"
                            "It may take a while (~2 seconds per track).\n\n"
                            "Continue?").arg(tracksToIdentify.size()))) {
        MetadataService::instance()->identifyByFingerprintBatch(tracksToIdentify);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  addTracksFromFiles
// ═════════════════════════════════════════════════════════════════════

void LibraryView::addTracksFromFiles(const QStringList& files)
{
    // Run MetadataReader I/O off main thread
    (void)QtConcurrent::run([files]() {
        auto* db = LibraryDatabase::instance();
        for (const QString& filePath : files) {
            auto trackOpt = MetadataReader::readTrack(filePath);
            if (!trackOpt.has_value()) continue;
            db->insertTrack(trackOpt.value());
        }
        // Reload on main thread after all inserts complete
        QMetaObject::invokeMethod(MusicDataProvider::instance(), [&]() {
            MusicDataProvider::instance()->reloadFromDatabase();
        }, Qt::QueuedConnection);
    });
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — Escape in filter field
// ═════════════════════════════════════════════════════════════════════

bool LibraryView::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && obj == m_searchInput->lineEdit()) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_searchInput->lineEdit()->clear();
            m_searchInput->lineEdit()->clearFocus();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void LibraryView::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    // Labels — color only
    m_headerLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 20px; font-weight: 600;")
            .arg(c.foreground));
    m_countLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px;")
            .arg(c.foregroundMuted));

    // Icons — re-tint for current theme
    m_navBackBtn->setIcon(tm->cachedIcon(":/icons/chevron-left.svg"));
    m_navForwardBtn->setIcon(tm->cachedIcon(":/icons/chevron-right.svg"));
    m_scanBtn->setIcon(tm->cachedIcon(":/icons/refresh-cw.svg"));
    m_fetchMetadataBtn->setIcon(tm->cachedIcon(":/icons/download.svg"));
    m_identifyAudioBtn->setIcon(tm->cachedIcon(":/icons/music.svg"));

    // Header button styles — uniform box model for pixel-perfect height.
    // ALL buttons use border: 1px solid (transparent or visible) so that
    // the box model is identical: 1px + 28px content + 1px = 30px total.
    const QString hdrGhost = QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid transparent; border-radius: 6px;"
        "  color: %1; font-size: 13px; padding: 0px 12px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:pressed { background: %3; }").arg(c.foreground, c.hover, c.pressed);
    const QString hdrPrimary = QStringLiteral(
        "QPushButton { background: %1; border: 1px solid transparent; border-radius: 6px;"
        "  color: %2; font-size: 13px; padding: 0px 12px; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }").arg(c.accent, c.foregroundInverse, c.accentHover, c.accentPressed);
    const QString hdrOutline = QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; border-radius: 6px;"
        "  color: %2; font-size: 13px; padding: 0px 12px; }"
        "QPushButton:hover { background: %3; border-color: %4; }"
        "QPushButton:pressed { background: %5; }").arg(c.border, c.foreground, c.hover, c.foregroundMuted, c.pressed);

    m_showAllBtn->setStyleSheet(hdrOutline);
    m_scanBtn->setStyleSheet(hdrGhost);
    m_fetchMetadataBtn->setStyleSheet(hdrGhost);
    m_identifyAudioBtn->setStyleSheet(hdrGhost);
    m_playAllBtn->setStyleSheet(hdrPrimary);
    m_openFilesBtn->setStyleSheet(hdrPrimary);
}

// ═════════════════════════════════════════════════════════════════════
//  onLibraryUpdated
// ═════════════════════════════════════════════════════════════════════

void LibraryView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_libraryDirty) {
        m_libraryDirty = false;
        onLibraryUpdated();
    }
}

void LibraryView::onLibraryUpdated()
{
    qDebug() << "[TIMING] LibraryView::onLibraryUpdated triggered";
    populateTracks();

    // Re-apply active filter if one is set
    if (!m_activeFolder.isEmpty()) {
        filterByFolder(m_activeFolder);
    } else if (!m_activeArtist.isEmpty()) {
        filterByArtist(m_activeArtist);
    } else if (!m_activeAlbum.isEmpty()) {
        filterByAlbum(m_activeAlbum);
    }

    // Re-highlight current track
    const Track current = PlaybackState::instance()->currentTrack();
    if (!current.id.isEmpty()) {
        onTrackChanged(current);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  filterByFolder — show only tracks from a specific folder, sorted by track number
// ═════════════════════════════════════════════════════════════════════
void LibraryView::filterByFolder(const QString& folderPath)
{
    m_activeFolder = folderPath;
    m_activeArtist.clear();
    m_activeAlbum.clear();
    m_searchInput->lineEdit()->clear();

    auto* model = m_trackTable->hybridModel();
    model->setFilterFolder(folderPath);
    model->sortByColumn(TrackColumn::Number, Qt::AscendingOrder);

    m_countLabel->setText(QStringLiteral("%1 tracks").arg(m_trackTable->visibleCount()));
    m_headerLabel->setText(QStringLiteral("Library — %1").arg(QFileInfo(folderPath).fileName()));
    m_showAllBtn->setVisible(true);
}

// ═════════════════════════════════════════════════════════════════════
//  filterByArtist — show only tracks matching the given artist
// ═════════════════════════════════════════════════════════════════════
void LibraryView::filterByArtist(const QString& artistName)
{
    m_activeFolder.clear();
    m_activeAlbum.clear();
    m_activeArtist = artistName;
    m_searchInput->lineEdit()->clear();

    auto* model = m_trackTable->hybridModel();
    model->setFilterArtist(artistName);

    m_countLabel->setText(QStringLiteral("%1 tracks").arg(m_trackTable->visibleCount()));
    m_headerLabel->setText(QStringLiteral("Library — %1").arg(artistName));
    m_showAllBtn->setVisible(true);
}

// ═════════════════════════════════════════════════════════════════════
//  filterByAlbum — show only tracks matching the given album
// ═════════════════════════════════════════════════════════════════════
void LibraryView::filterByAlbum(const QString& albumName)
{
    m_activeFolder.clear();
    m_activeArtist.clear();
    m_activeAlbum = albumName;
    m_searchInput->lineEdit()->clear();

    auto* model = m_trackTable->hybridModel();
    model->setFilterAlbum(albumName);
    model->sortByColumn(TrackColumn::Number, Qt::AscendingOrder);

    m_countLabel->setText(QStringLiteral("%1 tracks").arg(m_trackTable->visibleCount()));
    m_headerLabel->setText(QStringLiteral("Library — %1").arg(albumName));
    m_showAllBtn->setVisible(true);
}

// ═════════════════════════════════════════════════════════════════════
//  showAllTracks — clear folder filter, show all tracks
// ═════════════════════════════════════════════════════════════════════
void LibraryView::showAllTracks()
{
    m_activeFolder.clear();
    m_activeArtist.clear();
    m_activeAlbum.clear();
    m_showAllBtn->setVisible(false);
    m_headerLabel->setText(QStringLiteral("Library"));

    auto* model = m_trackTable->hybridModel();
    model->clearFilter();
    model->clearSort();
    m_countLabel->setText(QStringLiteral("%1 tracks").arg(m_trackTable->visibleCount()));
}
