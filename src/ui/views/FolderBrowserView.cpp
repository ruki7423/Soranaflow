#include "FolderBrowserView.h"
#include "../../core/PlaybackState.h"
#include "../../core/MusicData.h"
#include "../../core/Settings.h"
#include "../../core/ThemeManager.h"
#include "../../widgets/TrackTableView.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileInfo>
#include <QDir>
#include <QPixmap>
#include <QTimer>
#include <QDebug>
#include <algorithm>

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

FolderBrowserView::FolderBrowserView(QWidget* parent)
    : QWidget(parent)
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);

    // ── Left: Folder tree ───────────────────────────────────────────
    m_folderTree = new QTreeWidget();
    m_folderTree->setHeaderHidden(true);
    m_folderTree->setIndentation(16);
    m_folderTree->setAnimated(true);
    m_folderTree->setMinimumWidth(200);
    m_folderTree->setFrameShape(QFrame::NoFrame);
    m_splitter->addWidget(m_folderTree);

    // ── Right: Track panel ──────────────────────────────────────────
    auto* rightPanel = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(16, 16, 16, 0);
    rightLayout->setSpacing(8);

    // Path breadcrumb
    m_pathLabel = new QLabel();
    QFont pathFont = m_pathLabel->font();
    pathFont.setPixelSize(20);
    pathFont.setBold(true);
    m_pathLabel->setFont(pathFont);
    m_pathLabel->setText(QStringLiteral("Select a folder"));
    rightLayout->addWidget(m_pathLabel);

    // Track count
    m_countLabel = new QLabel();
    QFont countFont = m_countLabel->font();
    countFont.setPixelSize(13);
    m_countLabel->setFont(countFont);
    rightLayout->addWidget(m_countLabel);

    // Track table
    m_trackTable = new TrackTableView(libraryConfig());
    rightLayout->addWidget(m_trackTable, 1);

    m_splitter->addWidget(rightPanel);
    m_splitter->setSizes({280, 800});

    mainLayout->addWidget(m_splitter);

    // ── Connections ─────────────────────────────────────────────────
    connect(MusicDataProvider::instance(), &MusicDataProvider::libraryUpdated,
            this, &FolderBrowserView::reloadFolders);

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &FolderBrowserView::applyTheme);

    connect(m_folderTree, &QTreeWidget::itemClicked,
            this, &FolderBrowserView::onFolderSelected);

    connect(m_trackTable, &TrackTableView::trackDoubleClicked,
            this, &FolderBrowserView::onTrackDoubleClicked);

    connect(m_trackTable, &TrackTableView::albumClicked,
            this, &FolderBrowserView::albumSelected);

    connect(m_trackTable, &TrackTableView::artistClicked,
            this, &FolderBrowserView::artistSelected);

    // Highlight currently playing track
    connect(PlaybackState::instance(), &PlaybackState::trackChanged,
            this, [this](const Track& t) {
        m_trackTable->setHighlightedTrackId(t.id);
    });

    // Deferred initial load
    QTimer::singleShot(300, this, &FolderBrowserView::reloadFolders);

    applyTheme();
}

// ═════════════════════════════════════════════════════════════════════
//  reloadFolders — rebuild folder map + tree from track data
// ═════════════════════════════════════════════════════════════════════

void FolderBrowserView::reloadFolders()
{
    m_folderTracks.clear();

    // Group all tracks by directory
    const auto allTracks = MusicDataProvider::instance()->allTracks();
    for (const auto& track : allTracks) {
        if (track.filePath.isEmpty()) continue;
        QString dir = QFileInfo(track.filePath).absolutePath();
        m_folderTracks[dir].append(track);
    }

    buildTree();

    // If we had a folder selected, re-select it
    if (!m_currentFolder.isEmpty()) {
        // Find and click the item for the current folder
        QTreeWidgetItemIterator it(m_folderTree);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == m_currentFolder) {
                m_folderTree->setCurrentItem(*it);
                onFolderSelected(*it, 0);
                break;
            }
            ++it;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════
//  buildTree — construct tree widget from folder map + library roots
// ═════════════════════════════════════════════════════════════════════

void FolderBrowserView::buildTree()
{
    m_folderTree->clear();

    QStringList roots = Settings::instance()->libraryFolders();
    if (roots.isEmpty()) {
        auto* placeholder = new QTreeWidgetItem(m_folderTree);
        placeholder->setText(0, QStringLiteral("No library folders configured"));
        placeholder->setFlags(Qt::NoItemFlags);
        return;
    }

    // For each library root, create a top-level item and populate children
    for (const QString& root : roots) {
        QString rootPath = QDir(root).absolutePath();
        QFileInfo rootInfo(rootPath);
        QString rootName = rootInfo.fileName().isEmpty() ? rootPath : rootInfo.fileName();

        // Count tracks in this root
        int rootCount = 0;
        for (auto it = m_folderTracks.constBegin(); it != m_folderTracks.constEnd(); ++it) {
            if (it.key().startsWith(rootPath)) {
                rootCount += it.value().size();
            }
        }

        auto* rootItem = new QTreeWidgetItem(m_folderTree);
        rootItem->setText(0, QStringLiteral("%1 (%2)").arg(rootName).arg(rootCount));
        rootItem->setData(0, Qt::UserRole, rootPath);
        rootItem->setIcon(0, ThemeManager::instance()->cachedIcon(":/icons/folder.svg"));

        // Collect all subdirectories that belong to this root
        QStringList dirs;
        for (auto it = m_folderTracks.constBegin(); it != m_folderTracks.constEnd(); ++it) {
            if (it.key().startsWith(rootPath) && it.key() != rootPath) {
                dirs.append(it.key());
            }
        }
        dirs.sort();

        // Build intermediate tree items
        for (const QString& dir : dirs) {
            findOrCreatePath(rootPath, dir, rootItem);
        }

        rootItem->setExpanded(true);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  findOrCreatePath — create tree items for intermediate directories
// ═════════════════════════════════════════════════════════════════════

QTreeWidgetItem* FolderBrowserView::findOrCreatePath(const QString& rootPath,
                                                      const QString& fullPath,
                                                      QTreeWidgetItem* rootItem)
{
    // Get relative path from root
    QString rel = fullPath.mid(rootPath.length());
    if (rel.startsWith(QLatin1Char('/')))
        rel = rel.mid(1);

    QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QTreeWidgetItem* parent = rootItem;
    QString currentPath = rootPath;

    for (const QString& part : parts) {
        currentPath += QLatin1Char('/') + part;

        // Check if this child already exists
        QTreeWidgetItem* found = nullptr;
        for (int i = 0; i < parent->childCount(); ++i) {
            if (parent->child(i)->data(0, Qt::UserRole).toString() == currentPath) {
                found = parent->child(i);
                break;
            }
        }

        if (found) {
            parent = found;
        } else {
            // Count tracks in this directory (direct only, not recursive)
            int count = m_folderTracks.value(currentPath).size();

            auto* item = new QTreeWidgetItem(parent);
            QString label = count > 0
                ? QStringLiteral("%1 (%2)").arg(part).arg(count)
                : part;
            item->setText(0, label);
            item->setData(0, Qt::UserRole, currentPath);
            item->setIcon(0, ThemeManager::instance()->cachedIcon(":/icons/folder.svg"));
            parent = item;
        }
    }

    return parent;
}

// ═════════════════════════════════════════════════════════════════════
//  onFolderSelected — show tracks for selected folder + subdirectories
// ═════════════════════════════════════════════════════════════════════

void FolderBrowserView::onFolderSelected(QTreeWidgetItem* item, int /*column*/)
{
    QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;

    m_currentFolder = path;

    // Collect tracks from this folder and all subdirectories
    m_currentTracks.clear();
    for (auto it = m_folderTracks.constBegin(); it != m_folderTracks.constEnd(); ++it) {
        if (it.key() == path || it.key().startsWith(path + QLatin1Char('/'))) {
            m_currentTracks.append(it.value());
        }
    }

    // Sort by track number, then filename
    std::sort(m_currentTracks.begin(), m_currentTracks.end(),
              [](const Track& a, const Track& b) {
        if (a.discNumber != b.discNumber) return a.discNumber < b.discNumber;
        if (a.trackNumber != b.trackNumber) return a.trackNumber < b.trackNumber;
        return QFileInfo(a.filePath).fileName() < QFileInfo(b.filePath).fileName();
    });

    // Update labels
    QFileInfo fi(path);
    m_pathLabel->setText(fi.fileName().isEmpty() ? path : fi.fileName());
    m_countLabel->setText(QStringLiteral("%1 tracks").arg(m_currentTracks.size()));

    // Update table
    m_trackTable->setTracks(m_currentTracks);

    // Highlight currently playing track if in this folder
    auto currentTrack = PlaybackState::instance()->currentTrack();
    if (!currentTrack.id.isEmpty()) {
        m_trackTable->setHighlightedTrackId(currentTrack.id);
    }
}

// ═════════════════════════════════════════════════════════════════════
//  onTrackDoubleClicked — set queue and play
// ═════════════════════════════════════════════════════════════════════

void FolderBrowserView::onTrackDoubleClicked(const Track& track)
{
    PlaybackState::instance()->setQueue(m_currentTracks);
    PlaybackState::instance()->playTrack(track);
}

// ═════════════════════════════════════════════════════════════════════
//  applyTheme — style tree, labels, splitter
// ═════════════════════════════════════════════════════════════════════

void FolderBrowserView::applyTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    // Generate themed branch indicator pixmaps (tinted for current theme)
    QPixmap closedPix = tm->cachedIcon(":/icons/chevron-right.svg").pixmap(12, 12);
    QPixmap openPix   = tm->cachedIcon(":/icons/chevron-down.svg").pixmap(12, 12);

    static const QString closedPath = QDir::tempPath() + QStringLiteral("/sorana_branch_closed.png");
    static const QString openPath   = QDir::tempPath() + QStringLiteral("/sorana_branch_open.png");
    closedPix.save(closedPath, "PNG");
    openPix.save(openPath, "PNG");

    // Tree widget styling with themed branch arrows
    m_folderTree->setStyleSheet(QStringLiteral(
        "QTreeWidget {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  font-size: 13px;"
        "}"
        "QTreeWidget::item {"
        "  padding: 4px 8px;"
        "  border-radius: 4px;"
        "}"
        "QTreeWidget::item:selected {"
        "  background: %3;"
        "  color: %4;"
        "}"
        "QTreeWidget::item:hover:!selected {"
        "  background: %5;"
        "}"
        "QTreeWidget::branch { background: transparent; }"
        "QTreeWidget::branch:has-children:!has-siblings:closed,"
        "QTreeWidget::branch:closed:has-children:has-siblings {"
        "  image: url(%6);"
        "}"
        "QTreeWidget::branch:open:has-children:!has-siblings,"
        "QTreeWidget::branch:open:has-children:has-siblings {"
        "  image: url(%7);"
        "}"
    ).arg(c.backgroundSecondary, c.foreground, c.accentMuted,
          c.accent, c.hover, closedPath, openPath));

    // Labels
    m_pathLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foreground));
    m_countLabel->setStyleSheet(QStringLiteral("color: %1;").arg(c.foregroundMuted));

    // Splitter handle
    m_splitter->setStyleSheet(QStringLiteral(
        "QSplitter::handle { background: %1; width: 1px; }"
    ).arg(c.borderSubtle));

    // Background
    setStyleSheet(QStringLiteral("background: %1;").arg(c.background));

    // Re-theme tree icons
    QTreeWidgetItemIterator it(m_folderTree);
    while (*it) {
        (*it)->setIcon(0, tm->cachedIcon(":/icons/folder.svg"));
        ++it;
    }
}
