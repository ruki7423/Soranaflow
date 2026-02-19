#include "AppSidebar.h"

#include <QStyle>
#include <QFileInfo>
#include <QLineEdit>
#include <QKeyEvent>
#include "../core/ThemeManager.h"
#include "../core/Settings.h"

// ── Constructor ─────────────────────────────────────────────────────
AppSidebar::AppSidebar(QWidget* parent)
    : QWidget(parent)
    , m_mainLayout(nullptr)
    , m_logoBar(nullptr)
    , m_searchInput(nullptr)
    , m_navContainer(nullptr)
    , m_navScroll(nullptr)
    , m_librarySection(nullptr)
    , m_settingsButton(nullptr)
    , m_collapseAnim(nullptr)
    , m_logoLabel(nullptr)
{
    setupUI();

    // Connect theme changes to refresh
    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &AppSidebar::refreshTheme);

    // Refresh folder list when library folders change
    connect(Settings::instance(), &Settings::libraryFoldersChanged,
            this, &AppSidebar::rebuildFolderButtons);
}

// ═════════════════════════════════════════════════════════════════════
//  setupUI
// ═════════════════════════════════════════════════════════════════════
void AppSidebar::setupUI()
{
    setObjectName("AppSidebar");
    setFixedWidth(UISizes::sidebarWidth);

    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    auto c = ThemeManager::instance()->colors();

    // ── 1. Logo Bar (64px height) ───────────────────────────────────
    {
        m_logoBar = new QWidget(this);
        m_logoBar->setFixedHeight(64);

        auto* logoLayout = new QHBoxLayout(m_logoBar);
        logoLayout->setContentsMargins(12, 0, 12, 0);
        logoLayout->setSpacing(8);

        auto* logo = new SoranaFlowLogo(28, m_logoBar);
        logo->setCursor(Qt::PointingHandCursor);

        m_logoLabel = new QLabel(QStringLiteral("Sorana Flow"), m_logoBar);
        QFont logoFont = m_logoLabel->font();
        logoFont.setBold(true);
        logoFont.setPixelSize(16);
        m_logoLabel->setFont(logoFont);
        m_logoLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foreground));
        m_logoLabel->setCursor(Qt::PointingHandCursor);

        // Make logo and label clickable — navigate to Now Playing
        logo->installEventFilter(this);
        logo->setProperty("logoClick", true);
        m_logoLabel->installEventFilter(this);
        m_logoLabel->setProperty("logoClick", true);

        auto* collapseBtn = new QPushButton(m_logoBar);
        collapseBtn->setObjectName("collapseButton");
        collapseBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/menu.svg"));
        collapseBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
        collapseBtn->setFixedSize(UISizes::smallButtonSize, UISizes::smallButtonSize);
        collapseBtn->setFlat(true);
        collapseBtn->setCursor(Qt::PointingHandCursor);
        collapseBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { background: %1; border-radius: 4px; }"
        ).arg(c.hover));
        connect(collapseBtn, &QPushButton::clicked, this, &AppSidebar::toggleCollapse);

        logoLayout->addWidget(logo);
        logoLayout->addWidget(m_logoLabel);
        logoLayout->addStretch();
        logoLayout->addWidget(collapseBtn);

        m_mainLayout->addWidget(m_logoBar);
    }

    // ── 2. Search Input ─────────────────────────────────────────────
    {
        auto* searchContainer = new QWidget(this);
        searchContainer->setObjectName("searchContainer");
        auto* searchLayout = new QHBoxLayout(searchContainer);
        searchLayout->setContentsMargins(12, 8, 12, 8);
        searchLayout->setSpacing(0);

        m_searchInput = new StyledInput(QStringLiteral("Search..."),
                                         QStringLiteral(":/icons/search.svg"),
                                         searchContainer);

        connect(m_searchInput->lineEdit(), &QLineEdit::textChanged,
                this, &AppSidebar::searchRequested);

        m_searchInput->lineEdit()->installEventFilter(this);

        searchLayout->addWidget(m_searchInput);

        m_mainLayout->addWidget(searchContainer);
    }

    // ── 2b. Collapsed Search Button (hidden by default) ─────────────
    {
        m_collapsedSearchBtn = new QPushButton(this);
        m_collapsedSearchBtn->setObjectName("collapsedSearchBtn");
        m_collapsedSearchBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/search.svg"));
        m_collapsedSearchBtn->setIconSize(QSize(22, 22));
        m_collapsedSearchBtn->setFixedSize(44, 44);
        m_collapsedSearchBtn->setFlat(true);
        m_collapsedSearchBtn->setCursor(Qt::PointingHandCursor);
        m_collapsedSearchBtn->setToolTip(QStringLiteral("Search"));
        m_collapsedSearchBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  border-radius: 12px;"
            "}"
            "QPushButton:hover {"
            "  background: %1;"
            "}"
        ).arg(c.hover));
        m_collapsedSearchBtn->setVisible(false);

        connect(m_collapsedSearchBtn, &QPushButton::clicked, this, [this]() {
            // Expand sidebar and focus the search input
            if (m_collapsed) {
                toggleCollapse();
            }
            m_searchInput->lineEdit()->setFocus();
        });

        // Center it: use a wrapper with centered layout
        auto* searchBtnContainer = new QWidget(this);
        searchBtnContainer->setObjectName("collapsedSearchContainer");
        auto* searchBtnLayout = new QHBoxLayout(searchBtnContainer);
        searchBtnLayout->setContentsMargins(0, 4, 0, 4);
        searchBtnLayout->setAlignment(Qt::AlignCenter);
        searchBtnLayout->addWidget(m_collapsedSearchBtn);

        m_mainLayout->addWidget(searchBtnContainer);
    }

    // ── 3. Navigation Buttons (in ScrollArea) ───────────────────────
    {
        struct NavItem {
            QString text;
            QString iconPath;
        };

        const NavItem navItems[] = {
            { tr("Now Playing"),  QStringLiteral(":/icons/radio.svg")      },
            { tr("Library"),      QStringLiteral(":/icons/library.svg")     },
            { tr("Albums"),       QStringLiteral(":/icons/disc.svg")        },
            { tr("Artists"),      QStringLiteral(":/icons/users.svg")       },
            { tr("Playlists"),    QStringLiteral(":/icons/list-music.svg")  },
            { tr("Apple Music"),  QStringLiteral(":/icons/apple-music.svg") },
            { tr("Folders"),      QStringLiteral(":/icons/folder.svg")      },
        };
        constexpr int NAV_ITEM_COUNT = 7;

        m_navContainer = new QWidget(this);
        auto* navLayout = new QVBoxLayout(m_navContainer);
        navLayout->setContentsMargins(8, 4, 8, 4);
        navLayout->setSpacing(2);

        for (int i = 0; i < NAV_ITEM_COUNT; ++i) {
            QPushButton* btn = createNavButton(navItems[i].text, navItems[i].iconPath);
            m_navButtons.append(btn);
            navLayout->addWidget(btn);

            const int index = i;
            connect(btn, &QPushButton::clicked, this, [this, index]() {
                setActiveIndex(index);
                emit navigationChanged(index);
            });
        }

        navLayout->addStretch();

        m_navScroll = new QScrollArea(this);
        m_navScroll->setObjectName("navScrollArea");
        m_navScroll->setWidget(m_navContainer);
        m_navScroll->setWidgetResizable(true);
        m_navScroll->setFrameShape(QFrame::NoFrame);
        m_navScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_navScroll->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; }") +
            ThemeManager::instance()->scrollbarStyle());

        m_navScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        m_mainLayout->addWidget(m_navScroll, 0);
    }

    // ── 4. Library Folders Section ──────────────────────────────────
    {
        m_librarySection = new QWidget(this);
        auto* sectionLayout = new QVBoxLayout(m_librarySection);
        sectionLayout->setContentsMargins(12, 8, 12, 8);
        sectionLayout->setSpacing(0);

        // Clickable header with collapse arrow
        auto* folderHeader = new QWidget(m_librarySection);
        folderHeader->setObjectName(QStringLiteral("folderHeader"));
        folderHeader->setCursor(Qt::PointingHandCursor);
        auto* headerLayout = new QHBoxLayout(folderHeader);
        headerLayout->setContentsMargins(0, 0, 0, 4);
        headerLayout->setSpacing(4);

        m_folderArrow = new QLabel(m_librarySection);
        m_folderArrow->setFixedWidth(11);
        QFont arrowFont = m_folderArrow->font();
        arrowFont.setPixelSize(9);
        m_folderArrow->setFont(arrowFont);
        m_folderArrow->setStyleSheet(QStringLiteral("color: %1;").arg(c.foregroundMuted));

        auto* sectionLabel = new QLabel(tr("LIBRARY FOLDERS"), m_librarySection);
        sectionLabel->setObjectName(QStringLiteral("librarySectionLabel"));
        QFont sectionFont = sectionLabel->font();
        sectionFont.setPixelSize(11);
        sectionFont.setBold(true);
        sectionLabel->setFont(sectionFont);
        sectionLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foregroundMuted));

        headerLayout->addWidget(m_folderArrow);
        headerLayout->addWidget(sectionLabel);
        headerLayout->addStretch();

        folderHeader->setStyleSheet(QStringLiteral(
            "#folderHeader:hover { background: rgba(255,255,255,0.05); border-radius: 4px; }"));
        folderHeader->installEventFilter(this);

        sectionLayout->addWidget(folderHeader);

        // Container for folder buttons
        m_folderListContainer = new QWidget;
        m_libLayout = new QVBoxLayout(m_folderListContainer);
        m_libLayout->setContentsMargins(0, 0, 0, 0);
        m_libLayout->setSpacing(2);

        // Scroll area wraps folder list for overflow
        m_folderScrollArea = new QScrollArea(m_librarySection);
        m_folderScrollArea->setWidget(m_folderListContainer);
        m_folderScrollArea->setWidgetResizable(false);
        m_folderScrollArea->setFrameShape(QFrame::NoFrame);
        m_folderScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_folderScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_folderScrollArea->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; }") +
            ThemeManager::instance()->scrollbarStyle());

        sectionLayout->addWidget(m_folderScrollArea, 1);

        // Restore collapsed state
        m_foldersCollapsed = Settings::instance()->value(
            QStringLiteral("ui/folderSectionCollapsed"), false).toBool();
        m_folderScrollArea->setVisible(!m_foldersCollapsed);
        m_folderArrow->setText(m_foldersCollapsed
            ? QStringLiteral("\u25B6") : QStringLiteral("\u25BC"));

        rebuildFolderButtons();

        m_mainLayout->addWidget(m_librarySection, 0);
    }

    // Spacer pushes Settings to bottom
    m_mainLayout->addStretch(1);

    // ── 5. Settings Button (bottom) ─────────────────────────────────
    {
        auto* settingsContainer = new QWidget(this);
        settingsContainer->setObjectName("settingsContainer");
        settingsContainer->setStyleSheet(QStringLiteral(
            "#settingsContainer { border-top: 1px solid %1; }"
        ).arg(c.borderSubtle));
        auto* settingsLayout = new QHBoxLayout(settingsContainer);
        settingsLayout->setContentsMargins(12, 8, 12, 8);
        settingsLayout->setSpacing(0);

        m_settingsButton = new QPushButton(settingsContainer);
        m_settingsButton->setText(tr("Settings"));
        m_settingsButton->setIcon(ThemeManager::instance()->cachedIcon(":/icons/settings.svg"));
        m_settingsButton->setIconSize(QSize(20, 20));
        m_settingsButton->setFixedHeight(UISizes::thumbnailSize);
        m_settingsButton->setFlat(true);
        m_settingsButton->setCursor(Qt::PointingHandCursor);
        m_settingsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_settingsButton->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  color: %1;"
            "  text-align: left;"
            "  padding-left: 8px;"
            "  font-size: 13px;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "  background: %3;"
            "  border-radius: 8px;"
            "}"
        ).arg(c.foregroundSecondary, c.foreground, c.hover));

        connect(m_settingsButton, &QPushButton::clicked, this, [this]() {
            emit navigationChanged(9);
        });

        settingsLayout->addWidget(m_settingsButton);
        m_mainLayout->addWidget(settingsContainer);
    }

    // ── Collapse animation ──────────────────────────────────────────
    m_collapseAnim = new QPropertyAnimation(this, "sidebarWidth", this);
    m_collapseAnim->setDuration(200);
    m_collapseAnim->setEasingCurve(QEasingCurve::InOutQuad);

    // ── Initial active state ────────────────────────────────────────
    updateNavStyles();
}

// ═════════════════════════════════════════════════════════════════════
//  createNavButton
// ═════════════════════════════════════════════════════════════════════
QPushButton* AppSidebar::createNavButton(const QString& text, const QString& iconPath)
{
    auto* btn = new QPushButton(this);
    btn->setText(text);
    auto* tm = ThemeManager::instance();
    // Brand icons keep their original colors
    // Apple Music: gradient works on both themes
    // Brand icons keep their original colors
    if (iconPath.contains(QStringLiteral("apple-music"))) {
        btn->setIcon(QIcon(iconPath));
    } else {
        btn->setIcon(tm->cachedIcon(iconPath));
    }
    btn->setIconSize(QSize(20, 20));
    btn->setFixedHeight(UISizes::thumbnailSize);
    btn->setFlat(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto c = ThemeManager::instance()->colors();
    btn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent;"
        "  border: none;"
        "  color: %1;"
        "  text-align: left;"
        "  padding-left: 8px;"
        "  font-size: 13px;"
        "  border-radius: 8px;"
        "}"
        "QPushButton:hover {"
        "  color: %2;"
        "  background: %3;"
        "}"
    ).arg(c.foregroundSecondary, c.foreground, c.hover));

    return btn;
}

// ═════════════════════════════════════════════════════════════════════
//  toggleCollapse
// ═════════════════════════════════════════════════════════════════════
void AppSidebar::toggleCollapse()
{
    m_collapsed = !m_collapsed;

    // Animate width between expanded (240) and collapsed (64)
    m_collapseAnim->stop();
    m_collapseAnim->setStartValue(width());
    m_collapseAnim->setEndValue(m_collapsed ? 64 : UISizes::sidebarWidth);
    m_collapseAnim->start();

    // Find the collapse button
    auto* collapseBtn = m_logoBar->findChild<QPushButton*>("collapseButton");

    if (m_collapsed) {
        // Hide the collapse arrow button
        if (collapseBtn)
            collapseBtn->hide();

        // Hide elements that need full width
        m_logoLabel->hide();
        m_searchInput->parentWidget()->hide();
        m_librarySection->hide();

        // Show collapsed search icon
        m_collapsedSearchBtn->setVisible(true);
        m_collapsedSearchBtn->parentWidget()->setVisible(true);

        // ── Center the logo in the collapsed bar ─────────────────
        // Logo is 28px; collapsed width is 64px; left margin = (64-28)/2 = 18
        if (auto* logoLayout = qobject_cast<QHBoxLayout*>(m_logoBar->layout()))
            logoLayout->setContentsMargins(18, 0, 0, 0);

        // ── Nav container: zero margins, center alignment ────────
        if (auto* navLayout = qobject_cast<QVBoxLayout*>(m_navContainer->layout())) {
            navLayout->setContentsMargins(0, 8, 0, 8);
            navLayout->setAlignment(Qt::AlignHCenter);
            navLayout->setSpacing(4);
        }

        // Nav buttons: fixed square size, centered icon
        for (QPushButton* btn : m_navButtons) {
            btn->setText(QString());
            btn->setFixedSize(44, 44);
            btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            btn->setIconSize(QSize(22, 22));
        }

        // ── Settings button: fixed square, centered ──────────────
        m_settingsButton->setText(QString());
        m_settingsButton->setFixedSize(44, 44);
        m_settingsButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_settingsButton->setIconSize(QSize(22, 22));

        if (auto* settingsLayout = m_settingsButton->parentWidget()->layout()) {
            settingsLayout->setContentsMargins(0, 8, 0, 8);
            if (auto* hLayout = qobject_cast<QHBoxLayout*>(settingsLayout))
                hLayout->setAlignment(Qt::AlignCenter);
        }

        // Make entire sidebar clickable to expand
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Click to expand sidebar"));

    } else {
        // Show the collapse arrow button
        if (collapseBtn) {
            collapseBtn->show();
            collapseBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/menu.svg"));
        }

        // Show elements
        m_logoLabel->show();
        m_searchInput->parentWidget()->show();
        m_librarySection->show();

        // Hide collapsed search icon
        m_collapsedSearchBtn->setVisible(false);
        m_collapsedSearchBtn->parentWidget()->setVisible(false);

        // ── Restore logo bar ─────────────────────────────────────
        if (auto* logoLayout = qobject_cast<QHBoxLayout*>(m_logoBar->layout()))
            logoLayout->setContentsMargins(12, 0, 12, 0);

        // ── Restore nav container ────────────────────────────────
        if (auto* navLayout = qobject_cast<QVBoxLayout*>(m_navContainer->layout())) {
            navLayout->setContentsMargins(8, 4, 8, 4);
            navLayout->setAlignment({});
            navLayout->setSpacing(2);
        }

        // Restore nav buttons to expanded state
        const QString navTexts[] = {
            tr("Now Playing"),
            tr("Library"),
            tr("Albums"),
            tr("Artists"),
            tr("Playlists"),
            tr("Apple Music"),
            tr("Folders"),
        };
        for (int i = 0; i < m_navButtons.size() && i < 7; ++i) {
            m_navButtons[i]->setText(navTexts[i]);
            m_navButtons[i]->setFixedSize(QWIDGETSIZE_MAX, UISizes::thumbnailSize);
            m_navButtons[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            m_navButtons[i]->setIconSize(QSize(20, 20));
        }

        // ── Restore settings button ──────────────────────────────
        m_settingsButton->setText(tr("Settings"));
        m_settingsButton->setFixedSize(QWIDGETSIZE_MAX, UISizes::thumbnailSize);
        m_settingsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_settingsButton->setIconSize(QSize(20, 20));

        if (auto* settingsLayout = m_settingsButton->parentWidget()->layout()) {
            settingsLayout->setContentsMargins(12, 8, 12, 8);
            if (auto* hLayout = qobject_cast<QHBoxLayout*>(settingsLayout))
                hLayout->setAlignment({});
        }

        // Remove clickable cursor from sidebar
        setCursor(Qt::ArrowCursor);
        setToolTip(QString());
    }

    // Re-apply active nav styles
    updateNavStyles();

    emit collapseToggled(m_collapsed);
}

// ═════════════════════════════════════════════════════════════════════
//  setActiveIndex
// ═════════════════════════════════════════════════════════════════════
void AppSidebar::setActiveIndex(int index)
{
    m_activeIndex = index;
    updateNavStyles();
}

// ═════════════════════════════════════════════════════════════════════
//  updateNavStyles
// ═════════════════════════════════════════════════════════════════════
void AppSidebar::updateNavStyles()
{
    auto c = ThemeManager::instance()->colors();

    for (int i = 0; i < m_navButtons.size(); ++i) {
        QPushButton* btn = m_navButtons[i];
        bool active = (i == m_activeIndex);
        btn->setProperty("active", active ? "true" : "false");

        if (m_collapsed) {
            // Collapsed: square buttons, no text, centered icon
            if (active) {
                btn->setStyleSheet(QStringLiteral(
                    "QPushButton {"
                    "  background: %1;"
                    "  border: none;"
                    "  color: %2;"
                    "  border-radius: 12px;"
                    "}"
                    "QPushButton:hover {"
                    "  background: %3;"
                    "}"
                ).arg(c.accentMuted, c.accent, c.selected));
            } else {
                btn->setStyleSheet(QStringLiteral(
                    "QPushButton {"
                    "  background: transparent;"
                    "  border: none;"
                    "  color: %1;"
                    "  border-radius: 12px;"
                    "}"
                    "QPushButton:hover {"
                    "  color: %2;"
                    "  background: %3;"
                    "}"
                ).arg(c.foregroundSecondary, c.foreground, c.hover));
            }
        } else {
            // Expanded: full-width buttons with text
            if (active) {
                btn->setStyleSheet(QStringLiteral(
                    "QPushButton {"
                    "  background: %1;"
                    "  border: none;"
                    "  color: %2;"
                    "  text-align: left;"
                    "  padding-left: 8px;"
                    "  font-size: 13px;"
                    "  border-radius: 8px;"
                    "}"
                    "QPushButton:hover {"
                    "  background: %3;"
                    "}"
                ).arg(c.accentMuted, c.accent, c.selected));
            } else {
                btn->setStyleSheet(QStringLiteral(
                    "QPushButton {"
                    "  background: transparent;"
                    "  border: none;"
                    "  color: %1;"
                    "  text-align: left;"
                    "  padding-left: 8px;"
                    "  font-size: 13px;"
                    "  border-radius: 8px;"
                    "}"
                    "QPushButton:hover {"
                    "  color: %2;"
                    "  background: %3;"
                    "}"
                ).arg(c.foregroundSecondary, c.foreground, c.hover));
            }
        }

        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
    }

    // Also update settings button style
    if (m_collapsed) {
        m_settingsButton->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  color: %1;"
            "  border-radius: 12px;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "  background: %3;"
            "}"
        ).arg(c.foregroundSecondary, c.foreground, c.hover));
    } else {
        m_settingsButton->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  color: %1;"
            "  text-align: left;"
            "  padding-left: 8px;"
            "  font-size: 13px;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "  background: %3;"
            "  border-radius: 8px;"
            "}"
        ).arg(c.foregroundSecondary, c.foreground, c.hover));
    }
}

// ═════════════════════════════════════════════════════════════════════
//  rebuildFolderButtons — dynamically populate from Settings
// ═════════════════════════════════════════════════════════════════════
void AppSidebar::rebuildFolderButtons()
{
    // Remove existing folder buttons and spacers
    for (auto* btn : m_folderButtons) {
        m_libLayout->removeWidget(btn);
        btn->deleteLater();
    }
    m_folderButtons.clear();
    // Remove any leftover stretch/spacer items
    while (m_libLayout->count()) {
        auto* item = m_libLayout->takeAt(0);
        delete item;
    }

    QStringList folders = Settings::instance()->libraryFolders();

    if (folders.isEmpty()) {
        // Show a placeholder message
        auto* placeholder = new QPushButton(m_folderListContainer);
        placeholder->setText(QStringLiteral("No folders added"));
        placeholder->setEnabled(false);
        placeholder->setFlat(true);
        placeholder->setFixedHeight(UISizes::buttonHeight);
        placeholder->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  color: %1;"
            "  text-align: left;"
            "  padding-left: 4px;"
            "  font-size: 12px;"
            "  font-style: italic;"
            "}"
        ).arg(ThemeManager::instance()->colors().foregroundMuted));
        m_libLayout->addWidget(placeholder);
        m_folderButtons.append(placeholder);
        m_folderListContainer->adjustSize();
        return;
    }

    auto* tm = ThemeManager::instance();
    auto c = tm->colors();
    for (const QString& folder : folders) {
        auto* folderBtn = new QPushButton(m_folderListContainer);
        // Show just the folder name, not the full path
        QFileInfo fi(folder);
        folderBtn->setText(fi.fileName().isEmpty() ? folder : fi.fileName());
        folderBtn->setToolTip(folder);
        folderBtn->setIcon(tm->cachedIcon(":/icons/folder.svg"));
        folderBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
        folderBtn->setFixedHeight(UISizes::buttonHeight);
        folderBtn->setFlat(true);
        folderBtn->setCursor(Qt::PointingHandCursor);
        folderBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  color: %1;"
            "  text-align: left;"
            "  padding-left: 4px;"
            "  font-size: 13px;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "  background: %3;"
            "  border-radius: 6px;"
            "}"
        ).arg(c.foregroundSecondary, c.foreground, c.hover));

        connect(folderBtn, &QPushButton::clicked, this, [this, folder]() {
            emit folderSelected(folder);
        });

        m_libLayout->addWidget(folderBtn);
        m_folderButtons.append(folderBtn);
    }

    m_folderListContainer->adjustSize();
}

// ═════════════════════════════════════════════════════════════════════
//  eventFilter — logo click → navigate to Now Playing
// ═════════════════════════════════════════════════════════════════════
bool AppSidebar::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(obj);
        if (widget && widget->property("logoClick").toBool()) {
            setActiveIndex(0);
            emit navigationChanged(0);
            return true;
        }
        // Folder section header click → toggle collapse
        if (widget && widget->objectName() == QStringLiteral("folderHeader")) {
            m_foldersCollapsed = !m_foldersCollapsed;
            m_folderScrollArea->setVisible(!m_foldersCollapsed);
            m_folderArrow->setText(m_foldersCollapsed
                ? QStringLiteral("\u25B6") : QStringLiteral("\u25BC"));
            Settings::instance()->setValue(
                QStringLiteral("ui/folderSectionCollapsed"), m_foldersCollapsed);
            return true;
        }
    }

    // Escape in search field → clear text and unfocus
    if (event->type() == QEvent::KeyPress && obj == m_searchInput->lineEdit()) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            m_searchInput->lineEdit()->clear();
            m_searchInput->lineEdit()->clearFocus();
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

void AppSidebar::focusSearch()
{
    if (m_collapsed)
        toggleCollapse();
    m_searchInput->lineEdit()->setFocus();
    m_searchInput->lineEdit()->selectAll();
}

void AppSidebar::clearSearch()
{
    m_searchInput->lineEdit()->clear();
    m_searchInput->lineEdit()->clearFocus();
}

// ═════════════════════════════════════════════════════════════════════
//  mousePressEvent — click anywhere on collapsed sidebar to expand
// ═════════════════════════════════════════════════════════════════════
void AppSidebar::mousePressEvent(QMouseEvent* event)
{
    if (m_collapsed) {
        toggleCollapse();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

// ═════════════════════════════════════════════════════════════════════
//  refreshTheme  –  called when ThemeManager emits themeChanged
// ═════════════════════════════════════════════════════════════════════
void AppSidebar::refreshTheme()
{
    auto tm = ThemeManager::instance();
    auto c = tm->colors();

    // Logo label
    m_logoLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foreground));

    // Collapse button — hidden when collapsed, shown when expanded
    auto* collapseBtn = m_logoBar->findChild<QPushButton*>("collapseButton");
    if (collapseBtn) {
        collapseBtn->setIcon(tm->cachedIcon(":/icons/chevron-left.svg"));
        collapseBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { background: %1; border-radius: 4px; }"
        ).arg(c.hover));
        collapseBtn->setVisible(!m_collapsed);
    }

    // Scrollbars
    QString scrollStyle = QStringLiteral(
        "QScrollArea { background: transparent; border: none; }") +
        tm->scrollbarStyle();
    m_navScroll->setStyleSheet(scrollStyle);
    if (m_folderScrollArea)
        m_folderScrollArea->setStyleSheet(scrollStyle);

    // Library section label + arrow
    auto* sectionLabel = m_librarySection->findChild<QLabel*>(QStringLiteral("librarySectionLabel"));
    if (sectionLabel) {
        sectionLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foregroundMuted));
    }
    if (m_folderArrow) {
        m_folderArrow->setStyleSheet(QStringLiteral("color: %1;").arg(c.foregroundMuted));
    }
    // Folder header hover
    auto* folderHeader = m_librarySection->findChild<QWidget*>(QStringLiteral("folderHeader"));
    if (folderHeader) {
        folderHeader->setStyleSheet(QStringLiteral(
            "#folderHeader:hover { background: %1; border-radius: 4px; }").arg(c.hover));
    }

    // Rebuild folder buttons (picks up any new folders and applies theme)
    rebuildFolderButtons();

    // Settings container border — scope to the container itself
    auto* settingsContainer = m_settingsButton->parentWidget();
    if (settingsContainer) {
        settingsContainer->setObjectName("settingsContainer");
        settingsContainer->setStyleSheet(QStringLiteral(
            "#settingsContainer { border-top: 1px solid %1; }"
        ).arg(c.borderSubtle));
    }

    // Settings button icon
    m_settingsButton->setIcon(tm->cachedIcon(":/icons/settings.svg"));

    // Collapsed search button
    if (m_collapsedSearchBtn) {
        m_collapsedSearchBtn->setIcon(tm->cachedIcon(":/icons/search.svg"));
        m_collapsedSearchBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  border-radius: 12px;"
            "}"
            "QPushButton:hover {"
            "  background: %1;"
            "}"
        ).arg(c.hover));
    }

    // Nav button icons (0-4: themed icons, 5: Apple Music, 6: Folders)
    const QString iconPaths[] = {
        ":/icons/radio.svg", ":/icons/library.svg", ":/icons/disc.svg",
        ":/icons/users.svg", ":/icons/list-music.svg"
    };
    for (int i = 0; i < m_navButtons.size() && i < 5; ++i) {
        m_navButtons[i]->setIcon(tm->cachedIcon(iconPaths[i]));
    }
    // Apple Music (index 5) - gradient icon, no theme change needed
    // Folders (index 6) - themed icon
    if (m_navButtons.size() > 6) {
        m_navButtons[6]->setIcon(tm->cachedIcon(":/icons/folder.svg"));
    }

    // Re-apply active/inactive nav styles
    updateNavStyles();
}
