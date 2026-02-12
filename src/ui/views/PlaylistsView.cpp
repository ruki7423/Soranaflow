#include "PlaylistsView.h"
#include <QMouseEvent>
#include "../dialogs/StyledMessageBox.h"
#include "../dialogs/NewPlaylistDialog.h"
#include "../../core/ThemeManager.h"
#include "../../core/library/PlaylistManager.h"
#include "../MainWindow.h"

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

PlaylistsView::PlaylistsView(QWidget* parent)
    : QWidget(parent)
    , m_headerLabel(nullptr)
    , m_smartHeader(nullptr)
    , m_userHeader(nullptr)
    , m_smartGrid(nullptr)
    , m_smartGridLayout(nullptr)
    , m_userGrid(nullptr)
    , m_userGridLayout(nullptr)
    , m_scrollArea(nullptr)
    , m_createBtn(nullptr)
    , m_largeIconBtn(nullptr)
    , m_smallIconBtn(nullptr)
    , m_listBtn(nullptr)
{
    setupUI();
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI
// ═════════════════════════════════════════════════════════════════════

void PlaylistsView::setupUI()
{
    setObjectName(QStringLiteral("PlaylistsView"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ────────────────────────────────────────────────────────────────
    //  Header (outside scroll)
    // ────────────────────────────────────────────────────────────────
    const int NAV_SIZE = 30;

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(24, 24, 24, 0);
    headerLayout->setSpacing(8);
    headerLayout->setAlignment(Qt::AlignVCenter);

    m_headerLabel = new QLabel(QStringLiteral("Playlists"), this);
    m_headerLabel->setStyleSheet(
        QString("color: %1; font-size: 24px; font-weight: bold;").arg(ThemeManager::instance()->colors().foreground));
    headerLayout->addWidget(m_headerLabel, 0, Qt::AlignVCenter);

    // ── Global navigation ← → ─────────────────────────────────────
    headerLayout->addSpacing(4);

    m_navBackBtn = new QPushButton(this);
    m_navBackBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_navBackBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_navBackBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navBackBtn->setCursor(Qt::PointingHandCursor);
    m_navBackBtn->setToolTip(QStringLiteral("Back"));
    m_navBackBtn->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(m_navBackBtn, 0, Qt::AlignVCenter);

    m_navForwardBtn = new QPushButton(this);
    m_navForwardBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));
    m_navForwardBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_navForwardBtn->setFixedSize(NAV_SIZE, NAV_SIZE);
    m_navForwardBtn->setCursor(Qt::PointingHandCursor);
    m_navForwardBtn->setToolTip(QStringLiteral("Forward"));
    m_navForwardBtn->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(m_navForwardBtn, 0, Qt::AlignVCenter);

    headerLayout->addStretch();

    // ── View toggle buttons (grouped) ────────────────────────────
    auto c = ThemeManager::instance()->colors();
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

    auto* viewToggleContainer = new QWidget(this);
    viewToggleContainer->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    auto* viewToggleLayout = new QHBoxLayout(viewToggleContainer);
    viewToggleLayout->setContentsMargins(0, 0, 0, 0);
    viewToggleLayout->setSpacing(4);

    m_largeIconBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), viewToggleContainer);
    m_largeIconBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/grid-2x2.svg")));
    m_largeIconBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_largeIconBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_largeIconBtn->setToolTip(QStringLiteral("Large Icons"));
    m_largeIconBtn->setFocusPolicy(Qt::NoFocus);
    m_largeIconBtn->setStyleSheet(viewBtnStyle(true));
    viewToggleLayout->addWidget(m_largeIconBtn);

    m_smallIconBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), viewToggleContainer);
    m_smallIconBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/grid-3x3.svg")));
    m_smallIconBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_smallIconBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_smallIconBtn->setToolTip(QStringLiteral("Small Icons"));
    m_smallIconBtn->setFocusPolicy(Qt::NoFocus);
    m_smallIconBtn->setStyleSheet(viewBtnStyle(false));
    viewToggleLayout->addWidget(m_smallIconBtn);

    m_listBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), viewToggleContainer);
    m_listBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/list.svg")));
    m_listBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_listBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
    m_listBtn->setToolTip(QStringLiteral("List"));
    m_listBtn->setFocusPolicy(Qt::NoFocus);
    m_listBtn->setStyleSheet(viewBtnStyle(false));
    viewToggleLayout->addWidget(m_listBtn);

    headerLayout->addWidget(viewToggleContainer, 0, Qt::AlignVCenter);
    headerLayout->addSpacing(16);

    m_createBtn = new StyledButton(QStringLiteral("New Playlist"),
                                    QStringLiteral("ghost"),
                                    this);
    m_createBtn->setObjectName(QStringLiteral("CreatePlaylistBtn"));
    m_createBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/plus.svg")));
    m_createBtn->setIconSize(QSize(UISizes::toggleIconSize, UISizes::toggleIconSize));
    m_createBtn->setFocusPolicy(Qt::NoFocus);
    m_createBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_createBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 6px;"
        "  color: %1; padding: 4px 12px; font-size: 13px; min-height: 0px; max-height: 30px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:pressed { background: %3; }").arg(c.foreground, c.hover, c.pressed));
    m_createBtn->setFixedHeight(30);
    headerLayout->addWidget(m_createBtn, 0, Qt::AlignVCenter);

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

    // ────────────────────────────────────────────────────────────────
    //  Scrollable Content
    // ────────────────────────────────────────────────────────────────
    m_scrollArea = new StyledScrollArea(this);
    m_scrollArea->setWidgetResizable(true);

    auto* scrollContent = new QWidget(m_scrollArea);
    scrollContent->setObjectName(QStringLiteral("PlaylistsScrollContent"));

    auto* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(24, 16, 24, 24);
    contentLayout->setSpacing(24);

    // ── Smart Playlists Section ─────────────────────────────────────
    m_smartHeader = new QLabel(QStringLiteral("Smart Playlists"), scrollContent);
    m_smartHeader->setStyleSheet(
        QString("color: %1; font-size: 18px; font-weight: bold;").arg(ThemeManager::instance()->colors().foreground));
    contentLayout->addWidget(m_smartHeader);

    m_smartGrid = new QWidget(scrollContent);
    m_smartGridLayout = new QGridLayout(m_smartGrid);
    m_smartGridLayout->setContentsMargins(0, 0, 0, 0);
    m_smartGridLayout->setSpacing(16);
    contentLayout->addWidget(m_smartGrid);

    // ── User Playlists Section ──────────────────────────────────────
    m_userHeader = new QLabel(QStringLiteral("Your Playlists"), scrollContent);
    m_userHeader->setStyleSheet(
        QString("color: %1; font-size: 18px; font-weight: bold;").arg(ThemeManager::instance()->colors().foreground));
    contentLayout->addWidget(m_userHeader);

    m_userGrid = new QWidget(scrollContent);
    m_userGridLayout = new QGridLayout(m_userGrid);
    m_userGridLayout->setContentsMargins(0, 0, 0, 0);
    m_userGridLayout->setSpacing(16);
    contentLayout->addWidget(m_userGrid);

    contentLayout->addStretch();

    m_scrollArea->setWidget(scrollContent);
    mainLayout->addWidget(m_scrollArea, 1);

    setLayout(mainLayout);

    // ── Populate data ───────────────────────────────────────────────
    populatePlaylists();

    // ── Connections ──────────────────────────────────────────────────
    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &PlaylistsView::refreshTheme);
    connect(m_createBtn, &QPushButton::clicked,
            this, &PlaylistsView::onCreatePlaylistClicked);
    connect(PlaylistManager::instance(), &PlaylistManager::playlistsChanged,
            this, &PlaylistsView::onPlaylistsChanged);
    connect(MusicDataProvider::instance(), &MusicDataProvider::libraryUpdated,
            this, &PlaylistsView::onPlaylistsChanged);
    connect(m_largeIconBtn, &QPushButton::clicked, this, [this]() { setViewMode(LargeIcons); });
    connect(m_smallIconBtn, &QPushButton::clicked, this, [this]() { setViewMode(SmallIcons); });
    connect(m_listBtn, &QPushButton::clicked, this, [this]() { setViewMode(ListView); });
}

// ═════════════════════════════════════════════════════════════════════
//  createPlaylistCard
// ═════════════════════════════════════════════════════════════════════

QWidget* PlaylistsView::createPlaylistCard(const Playlist& playlist, int coverSize)
{
    auto c = ThemeManager::instance()->colors();

    // ── Gradient palette per playlist ───────────────────────────────
    static const struct { const char* from; const char* to; } gradients[] = {
        {"#667eea", "#764ba2"},  // Purple-violet
        {"#6a85b6", "#bac8e0"},  // Steel blue
        {"#4facfe", "#00c6fb"},  // Blue-cyan
        {"#89609e", "#c479a2"},  // Muted purple-pink
        {"#4ca1af", "#c4e0e5"},  // Teal
        {"#7f7fd5", "#86a8e7"},  // Soft purple-blue
        {"#5c6bc0", "#7986cb"},  // Indigo
        {"#26a69a", "#80cbc4"},  // Teal-mint
    };
    static constexpr int gradientCount = sizeof(gradients) / sizeof(gradients[0]);

    auto* card = new QWidget();
    card->setObjectName(QStringLiteral("PlaylistCard"));
    card->setFixedSize(coverSize + 8, coverSize + 66);
    card->setCursor(Qt::PointingHandCursor);
    card->setProperty("playlistId", playlist.id);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(8);

    // ── Cover area with shadow ──────────────────────────────────────
    auto* coverLabel = new QLabel(card);
    coverLabel->setObjectName(QStringLiteral("PlaylistCover"));
    coverLabel->setFixedSize(coverSize, coverSize);
    coverLabel->setAlignment(Qt::AlignCenter);
    coverLabel->setText(QStringLiteral("\u266B"));

    QString gradient;
    if (playlist.isSmartPlaylist) {
        gradient = QStringLiteral(
            "qlineargradient(x1:0, y1:0, x2:1, y2:1,"
            " stop:0 #4A9EFF, stop:1 #7C3AED)");
    } else {
        // Pick a gradient based on hash of the playlist id
        uint hash = qHash(playlist.id);
        int idx = static_cast<int>(hash % gradientCount);
        gradient = QStringLiteral(
            "qlineargradient(x1:0, y1:0, x2:1, y2:1,"
            " stop:0 %1, stop:1 %2)")
            .arg(QLatin1String(gradients[idx].from),
                 QLatin1String(gradients[idx].to));
    }

    coverLabel->setStyleSheet(
        QStringLiteral(
            "QLabel#PlaylistCover {"
            "  background: %1;"
            "  border-radius: 12px;"
            "  color: rgba(255, 255, 255, 0.85);"
            "  font-size: 40px;"
            "}").arg(gradient));

    cardLayout->addWidget(coverLabel);

    // ── Name ────────────────────────────────────────────────────────
    auto* nameLabel = new QLabel(playlist.name, card);
    nameLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: 600;").arg(c.foreground));
    nameLabel->setWordWrap(false);
    nameLabel->setMaximumWidth(coverSize);
    QFontMetrics nameFm(nameLabel->font());
    nameLabel->setText(nameFm.elidedText(playlist.name, Qt::ElideRight, coverSize));
    cardLayout->addWidget(nameLabel);

    // ── Bottom row ──────────────────────────────────────────────────
    auto* bottomRow = new QHBoxLayout();
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(8);

    auto* trackCountLabel = new QLabel(
        QStringLiteral("%1 tracks").arg(playlist.tracks.size()), card);
    trackCountLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    bottomRow->addWidget(trackCountLabel);

    if (playlist.isSmartPlaylist) {
        auto* smartBadge = new QLabel(QStringLiteral("Smart"), card);
        smartBadge->setStyleSheet(
            QStringLiteral(
                "QLabel {"
                "  background-color: %1;"
                "  color: %2;"
                "  border-radius: 4px;"
                "  padding: 2px 6px;"
                "  font-size: 10px;"
                "}").arg(c.accent, c.foregroundInverse));
        bottomRow->addWidget(smartBadge);
    }

    // ── Delete button (only for user playlists) ────────────────────
    if (!playlist.isSmartPlaylist) {
        auto* deleteBtn = new StyledButton(QStringLiteral(""),
                                            QStringLiteral("ghost"), card);
        deleteBtn->setText(QStringLiteral("\u2715"));
        deleteBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
        deleteBtn->setToolTip(QStringLiteral("Delete playlist"));
        deleteBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; background: transparent; border: none; font-size: 14px; }"
            "QPushButton:hover { color: %2; background: %3; border-radius: 4px; }")
                .arg(c.foregroundMuted, c.error, c.hover));
        connect(deleteBtn, &QPushButton::clicked, this, [this, id = playlist.id]() {
            onDeletePlaylistClicked(id);
        });
        bottomRow->addWidget(deleteBtn);
    } else {
        bottomRow->addStretch();
    }

    cardLayout->addLayout(bottomRow);

    // ── Install event filter for click + hover handling ─────────────
    card->installEventFilter(this);

    return card;
}

// ═════════════════════════════════════════════════════════════════════
//  createPlaylistListRow
// ═════════════════════════════════════════════════════════════════════

QWidget* PlaylistsView::createPlaylistListRow(const Playlist& playlist)
{
    auto c = ThemeManager::instance()->colors();

    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("PlaylistCard"));
    row->setFixedHeight(56);
    row->setCursor(Qt::PointingHandCursor);
    row->setProperty("playlistId", playlist.id);
    row->setStyleSheet(QString(
        "QWidget#PlaylistCard { background: transparent; border-bottom: 1px solid %1; }"
        "QWidget#PlaylistCard:hover { background: %2; }")
        .arg(c.borderSubtle, c.hover));

    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(4, 4, 4, 4);
    rowLayout->setSpacing(12);

    // Thumbnail
    auto* thumb = new QLabel(row);
    thumb->setFixedSize(UISizes::rowHeight, UISizes::rowHeight);
    thumb->setAlignment(Qt::AlignCenter);
    thumb->setText(QStringLiteral("\u266B"));
    QString gradient;
    if (playlist.isSmartPlaylist) {
        gradient = QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #4A9EFF, stop:1 #7C3AED)");
    } else {
        static const struct { const char* from; const char* to; } rowGradients[] = {
            {"#667eea", "#764ba2"}, {"#6a85b6", "#bac8e0"}, {"#4facfe", "#00c6fb"},
            {"#89609e", "#c479a2"}, {"#4ca1af", "#c4e0e5"}, {"#7f7fd5", "#86a8e7"},
            {"#5c6bc0", "#7986cb"}, {"#26a69a", "#80cbc4"},
        };
        uint hash = qHash(playlist.id);
        int idx = static_cast<int>(hash % 8);
        gradient = QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 %1, stop:1 %2)")
            .arg(QLatin1String(rowGradients[idx].from), QLatin1String(rowGradients[idx].to));
    }
    thumb->setStyleSheet(QStringLiteral(
        "QLabel { background: %1; border-radius: 6px; color: rgba(255,255,255,0.85); font-size: 18px; }")
        .arg(gradient));
    rowLayout->addWidget(thumb);

    // Info
    auto* infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(2);
    auto* nameLabel = new QLabel(playlist.name, row);
    nameLabel->setStyleSheet(QString("color: %1; font-size: 14px; font-weight: bold;").arg(c.foreground));
    auto* countLabel = new QLabel(QStringLiteral("%1 tracks").arg(playlist.tracks.size()), row);
    countLabel->setStyleSheet(QString("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    infoLayout->addWidget(nameLabel);
    infoLayout->addWidget(countLabel);
    rowLayout->addLayout(infoLayout, 1);

    if (playlist.isSmartPlaylist) {
        auto* smartBadge = new QLabel(QStringLiteral("Smart"), row);
        smartBadge->setStyleSheet(QStringLiteral(
            "QLabel { background-color: %1; color: %2; border-radius: 4px; padding: 2px 6px; font-size: 10px; }")
            .arg(ThemeManager::instance()->colors().accent,
                 ThemeManager::instance()->colors().foregroundInverse));
        rowLayout->addWidget(smartBadge);
    }

    if (!playlist.isSmartPlaylist) {
        auto* deleteBtn = new StyledButton(QStringLiteral(""), QStringLiteral("ghost"), row);
        deleteBtn->setText(QStringLiteral("\u2715"));
        deleteBtn->setFixedSize(UISizes::toggleButtonSize, UISizes::toggleButtonSize);
        deleteBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; background: transparent; border: none; font-size: 14px; }"
            "QPushButton:hover { color: %2; background: %3; border-radius: 4px; }")
                .arg(c.foregroundMuted, c.error, c.hover));
        connect(deleteBtn, &QPushButton::clicked, this, [this, id = playlist.id]() {
            onDeletePlaylistClicked(id);
        });
        rowLayout->addWidget(deleteBtn);
    }

    row->installEventFilter(this);
    return row;
}

// ═════════════════════════════════════════════════════════════════════
//  populatePlaylists
// ═════════════════════════════════════════════════════════════════════

void PlaylistsView::populatePlaylists()
{
    const QVector<Playlist> playlists =
        PlaylistManager::instance()->allPlaylists();

    QVector<Playlist> smartPlaylists;
    QVector<Playlist> userPlaylists;

    for (const auto& pl : playlists) {
        if (pl.isSmartPlaylist) {
            smartPlaylists.append(pl);
        } else {
            userPlaylists.append(pl);
        }
    }

    // Column count and cover size based on view mode
    int cols = 4;
    int coverSize = 164;
    if (m_viewMode == SmallIcons) {
        cols = 5;
        coverSize = 120;
    }

    auto populateGrid = [&](QGridLayout* grid, const QVector<Playlist>& list) {
        if (m_viewMode == ListView) {
            for (int i = 0; i < list.size(); ++i) {
                QWidget* row = createPlaylistListRow(list[i]);
                grid->addWidget(row, i, 0, 1, -1);
            }
        } else {
            for (int i = 0; i < list.size(); ++i) {
                QWidget* card = createPlaylistCard(list[i], coverSize);
                grid->addWidget(card, i / cols, i % cols);
            }
        }
    };

    populateGrid(m_smartGridLayout, smartPlaylists);
    populateGrid(m_userGridLayout, userPlaylists);

    // ── Hide sections if empty ──────────────────────────────────────
    m_smartHeader->setVisible(!smartPlaylists.isEmpty());
    m_smartGrid->setVisible(!smartPlaylists.isEmpty());
    m_userHeader->setVisible(!userPlaylists.isEmpty());
    m_userGrid->setVisible(!userPlaylists.isEmpty());
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — click handling for playlist cards
// ═════════════════════════════════════════════════════════════════════

bool PlaylistsView::eventFilter(QObject* watched, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || !widget->property("playlistId").isValid())
        return QWidget::eventFilter(watched, event);

    const QString playlistId = widget->property("playlistId").toString();

    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);

        if (mouseEvent->button() == Qt::LeftButton) {
            emit playlistSelected(playlistId);
            return true;
        }
        if (mouseEvent->button() == Qt::RightButton) {
            auto playlist = MusicDataProvider::instance()->playlistById(playlistId);
            if (!playlist.isSmartPlaylist) {
                QMenu contextMenu(this);
                QAction* deleteAction = contextMenu.addAction(QStringLiteral("Delete Playlist"));
                QAction* selected = contextMenu.exec(mouseEvent->globalPosition().toPoint());
                if (selected == deleteAction) {
                    onDeletePlaylistClicked(playlistId);
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ═════════════════════════════════════════════════════════════════════
//  setViewMode
// ═════════════════════════════════════════════════════════════════════

void PlaylistsView::setViewMode(ViewMode mode)
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

    // Repopulate with new layout
    clearPlaylistCards();
    populatePlaylists();
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme
// ═════════════════════════════════════════════════════════════════════

void PlaylistsView::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    m_headerLabel->setStyleSheet(
        QString("color: %1; font-size: 24px; font-weight: bold;").arg(c.foreground));
    m_smartHeader->setStyleSheet(
        QString("color: %1; font-size: 18px; font-weight: bold;").arg(c.foreground));
    m_userHeader->setStyleSheet(
        QString("color: %1; font-size: 18px; font-weight: bold;").arg(c.foreground));

    // Update create button
    m_createBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/plus.svg")));
    m_createBtn->setStyleSheet(tm->buttonStyle(ButtonVariant::Ghost));

    // Update view toggle buttons
    m_largeIconBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/grid-2x2.svg")));
    m_smallIconBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/grid-3x3.svg")));
    m_listBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/list.svg")));
    m_navBackBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/chevron-left.svg")));
    m_navForwardBtn->setIcon(tm->cachedIcon(QStringLiteral(":/icons/chevron-right.svg")));
    setViewMode(m_viewMode);
}

// ═════════════════════════════════════════════════════════════════════
//  onCreatePlaylistClicked
// ═════════════════════════════════════════════════════════════════════

void PlaylistsView::onCreatePlaylistClicked()
{
    NewPlaylistDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        QString name = dialog.playlistName();
        if (!name.isEmpty()) {
            PlaylistManager::instance()->createPlaylist(name);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════
//  onPlaylistsChanged
// ═════════════════════════════════════════════════════════════════════

void PlaylistsView::onPlaylistsChanged()
{
    clearPlaylistCards();
    populatePlaylists();
}

// ═════════════════════════════════════════════════════════════════════
//  clearPlaylistCards
// ═════════════════════════════════════════════════════════════════════

void PlaylistsView::onDeletePlaylistClicked(const QString& playlistId)
{
    auto playlist = MusicDataProvider::instance()->playlistById(playlistId);
    QString name = playlist.name.isEmpty() ? QStringLiteral("this playlist") : playlist.name;

    if (StyledMessageBox::confirmDelete(this, name)) {
        PlaylistManager::instance()->deletePlaylist(playlistId);
    }
}

void PlaylistsView::clearPlaylistCards()
{
    // Clear smart grid
    while (m_smartGridLayout->count() > 0) {
        QLayoutItem* item = m_smartGridLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    // Clear user grid
    while (m_userGridLayout->count() > 0) {
        QLayoutItem* item = m_userGridLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}
