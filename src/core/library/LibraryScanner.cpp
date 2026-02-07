#include "LibraryScanner.h"
#include "LibraryDatabase.h"
#include "../audio/MetadataReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDebug>
#include <QTimer>
#include <QThread>

#ifdef Q_OS_MACOS
#include "../../platform/macos/BookmarkManager.h"
#endif

const QStringList LibraryScanner::s_supportedExtensions = {
    QStringLiteral("flac"),
    QStringLiteral("mp3"),
    QStringLiteral("wav"),
    QStringLiteral("aac"),
    QStringLiteral("m4a"),
    QStringLiteral("ogg"),
    QStringLiteral("alac"),
    QStringLiteral("aiff"),
    QStringLiteral("aif"),
    QStringLiteral("wma"),
    QStringLiteral("opus"),
    QStringLiteral("dsf"),
    QStringLiteral("dff")
};

// ── Singleton ───────────────────────────────────────────────────────
LibraryScanner* LibraryScanner::instance()
{
    static LibraryScanner s;
    return &s;
}

LibraryScanner::LibraryScanner(QObject* parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &LibraryScanner::onDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &LibraryScanner::onFileChanged);
}

// ── scanFolders ─────────────────────────────────────────────────────
void LibraryScanner::scanFolders(const QStringList& folders)
{
    if (m_scanning) return;

    m_scanning = true;
    m_stopRequested = false;
    m_watchedFolders = folders;
    emit scanStarted();

    qDebug() << "LibraryScanner: Starting scan of" << folders.size() << "folders";

    // Save bookmarks for accessible folders (before entering background thread)
#ifdef Q_OS_MACOS
    for (const QString& folder : folders) {
        if (QDir(folder).exists() && !BookmarkManager::instance()->hasBookmark(folder)) {
            BookmarkManager::instance()->saveBookmark(folder);
        }
    }
#endif

    // Run heavy scanning work on a background thread
    m_workerThread = QThread::create([this, folders]() {
        // Collect all audio files first
        QStringList allFiles;
        for (const QString& folder : folders) {
            QDirIterator it(folder, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                if (m_stopRequested) break;
                it.next();
                QFileInfo fi = it.fileInfo();
                if (fi.isFile() && s_supportedExtensions.contains(fi.suffix().toLower())) {
                    allFiles.append(fi.absoluteFilePath());
                }
            }
        }

        if (m_stopRequested) {
            QMetaObject::invokeMethod(this, [this]() {
                m_scanning = false;
                emit scanFinished(0);
            }, Qt::QueuedConnection);
            return;
        }

        int total = allFiles.size();
        int tracksFound = 0;

        qDebug() << "LibraryScanner: Found" << total << "audio files to process";

        auto* db = LibraryDatabase::instance();
        db->beginTransaction();
        int insertsSinceCommit = 0;

        for (int i = 0; i < allFiles.size(); ++i) {
            if (m_stopRequested) break;

            const QString& filePath = allFiles[i];

            // Skip if already in database
            if (db->trackByPath(filePath).has_value()) {
                m_knownFiles.insert(filePath);
                tracksFound++;
            } else {
                processFile(filePath);
                tracksFound++;
                // Batch-commit every 500 inserts to limit transaction size
                if (++insertsSinceCommit >= 500) {
                    db->commitTransaction();
                    db->beginTransaction();
                    insertsSinceCommit = 0;
                }
            }

            // Emit progress on main thread periodically
            if (i % 20 == 0 || i == allFiles.size() - 1) {
                int current = i + 1;
                QMetaObject::invokeMethod(this, [this, current, total]() {
                    emit scanProgress(current, total);
                }, Qt::QueuedConnection);
            }
        }

        db->commitTransaction();

        // Rebuild album/artist tables from track data
        db->rebuildAlbumsAndArtists();

        int finalCount = tracksFound;
        QMetaObject::invokeMethod(this, [this, finalCount, folders]() {
            // Set up file watching if enabled (must be on main thread)
            if (m_watchEnabled) {
                for (const QString& folder : folders) {
                    m_watcher->addPath(folder);
                    QDirIterator dirIt(folder, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
                    while (dirIt.hasNext()) {
                        dirIt.next();
                        m_watcher->addPath(dirIt.filePath());
                    }
                }
            }

            m_scanning = false;
            m_scanCooldown.start();  // Start 5-second cooldown for directory change events
            qDebug() << "LibraryScanner: Scan complete." << finalCount << "tracks found";
            emit scanFinished(finalCount);
        }, Qt::QueuedConnection);
    });

    connect(m_workerThread, &QThread::finished, m_workerThread, &QObject::deleteLater);
    m_workerThread->start();
}

void LibraryScanner::stopScan()
{
    m_stopRequested = true;
}

// ── processFile ─────────────────────────────────────────────────────
void LibraryScanner::processFile(const QString& filePath)
{
    // Skip if already in database
    if (LibraryDatabase::instance()->trackExists(filePath)) {
        m_knownFiles.insert(filePath);
        return;
    }

    auto trackOpt = MetadataReader::readTrack(filePath);
    if (!trackOpt.has_value()) {
        qDebug() << "LibraryScanner: Failed to read metadata from" << filePath;
        return;
    }

    Track track = trackOpt.value();
    LibraryDatabase::instance()->insertTrack(track);
    m_knownFiles.insert(filePath);
}

// ── scanFolder (single folder) ──────────────────────────────────────
void LibraryScanner::scanFolder(const QString& folder)
{
    scanFolders({folder});
}

// ── setWatchEnabled ─────────────────────────────────────────────────
void LibraryScanner::setWatchEnabled(bool enabled)
{
    m_watchEnabled = enabled;

    if (!enabled) {
        // Remove all watched paths
        if (!m_watcher->directories().isEmpty())
            m_watcher->removePaths(m_watcher->directories());
        if (!m_watcher->files().isEmpty())
            m_watcher->removePaths(m_watcher->files());
    }
}

// ── onDirectoryChanged ──────────────────────────────────────────────
void LibraryScanner::onDirectoryChanged(const QString& path)
{
    // Ignore directory changes while a scan is in progress
    if (m_scanning) {
        qDebug() << "LibraryScanner: Ignoring directory change during scan:" << path;
        return;
    }

    // Cooldown after scan completes — file watchers fire as directories are indexed
    if (m_scanCooldown.isValid() && m_scanCooldown.elapsed() < 5000) {
        qDebug() << "LibraryScanner: Ignoring directory change - cooldown";
        return;
    }

    qDebug() << "LibraryScanner: Directory changed:" << path;

    // Check for new files
    QDirIterator it(path, QDir::Files);
    while (it.hasNext()) {
        it.next();
        QFileInfo fi = it.fileInfo();
        if (!s_supportedExtensions.contains(fi.suffix().toLower()))
            continue;

        QString filePath = fi.absoluteFilePath();
        if (!m_knownFiles.contains(filePath)) {
            qDebug() << "LibraryScanner: New file detected:" << filePath;
            processFile(filePath);
            emit fileAdded(filePath);
        }
    }

    // Check for removed files — only check files directly in this directory,
    // NOT in subdirectories (subdirectories get their own change notifications)
    QSet<QString> currentFiles;
    QDirIterator it2(path, QDir::Files);
    while (it2.hasNext()) {
        it2.next();
        currentFiles.insert(it2.fileInfo().absoluteFilePath());
    }

    QStringList toRemove;
    for (const QString& known : m_knownFiles) {
        // Only check files that are directly in this directory, not subdirectories
        QFileInfo fi(known);
        if (fi.absolutePath() == path && !currentFiles.contains(known)) {
            toRemove.append(known);
        }
    }
    for (const QString& removed : toRemove) {
        qDebug() << "LibraryScanner: File removed:" << removed;
        LibraryDatabase::instance()->removeTrackByPath(removed);
        m_knownFiles.remove(removed);
        emit fileRemoved(removed);
    }

    // Trigger rebuild
    QTimer::singleShot(500, this, [this]() {
        LibraryDatabase::instance()->rebuildAlbumsAndArtists();
    });
}

void LibraryScanner::onFileChanged(const QString& path)
{
    qDebug() << "LibraryScanner: File changed:" << path;

    QFileInfo fi(path);
    if (fi.exists()) {
        // File was modified — remove old entry and re-read metadata
        LibraryDatabase::instance()->removeTrackByPath(path);
        m_knownFiles.remove(path);
        processFile(path);
        LibraryDatabase::instance()->rebuildAlbumsAndArtists();
    } else {
        // File was deleted
        LibraryDatabase::instance()->removeTrackByPath(path);
        m_knownFiles.remove(path);
        emit fileRemoved(path);
        LibraryDatabase::instance()->rebuildAlbumsAndArtists();
    }
}
