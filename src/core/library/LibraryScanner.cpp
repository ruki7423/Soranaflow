#include "LibraryScanner.h"
#include "LibraryDatabase.h"
#include "../audio/MetadataReader.h"
#include "../Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>

#ifdef Q_OS_MACOS
#include "../../platform/macos/BookmarkManager.h"
#endif

static const QStringList kDefaultIgnoreExtensions = {
    QStringLiteral("cue"), QStringLiteral("log"), QStringLiteral("txt"),
    QStringLiteral("nfo"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
    QStringLiteral("png"), QStringLiteral("gif"), QStringLiteral("bmp"),
    QStringLiteral("pdf"), QStringLiteral("md"), QStringLiteral("m3u"),
    QStringLiteral("m3u8"), QStringLiteral("pls"), QStringLiteral("accurip"),
    QStringLiteral("sfv"), QStringLiteral("ffp"), QStringLiteral("db"),
    QStringLiteral("ini"), QStringLiteral("ds_store")
};

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
        QElapsedTimer pipelineTimer; pipelineTimer.start();
        QElapsedTimer stepTimer;
        qDebug() << "[TIMING] === SCAN PIPELINE START ===" << QDateTime::currentDateTime().toString();

        // Build ignore extensions set
        QSet<QString> ignoreExts;
        for (const QString& ext : Settings::instance()->ignoreExtensions())
            ignoreExts.insert(ext.toLower());

        // Collect all audio files — parallel walk across folders
        stepTimer.start();
        QList<QStringList> perFolderFiles = QtConcurrent::blockingMapped(
            folders, [this, &ignoreExts](const QString& folder) -> QStringList {
                QElapsedTimer folderTimer; folderTimer.start();
                QStringList files;
                QFileInfo folderInfo(folder);
                if (!folderInfo.exists() || !folderInfo.isReadable()) {
                    qDebug() << "[SCAN] Folder not accessible, skipping:" << folder;
                    return files;
                }
                QDirIterator it(folder, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    if (m_stopRequested) break;
                    it.next();
                    QFileInfo fi = it.fileInfo();
                    if (!fi.isFile()) continue;
                    QString suffix = fi.suffix().toLower();
                    if (ignoreExts.contains(suffix)) continue;
                    if (s_supportedExtensions.contains(suffix)) {
                        files.append(fi.absoluteFilePath());
                    }
                }
                qDebug() << "[SCAN] Walked" << folder << ":" << files.size()
                         << "files in" << folderTimer.elapsed() << "ms";
                return files;
            });

        QStringList allFiles;
        for (const auto& list : perFolderFiles) {
            allFiles.append(list);
        }
        qDebug() << "[TIMING] Directory walk:" << stepTimer.elapsed() << "ms —"
                 << allFiles.size() << "files from" << folders.size() << "folders";

        if (m_stopRequested) {
            QMetaObject::invokeMethod(this, [this]() {
                m_scanning = false;
                emit scanFinished(0);
            }, Qt::QueuedConnection);
            return;
        }

        int total = allFiles.size();
        int newCount = 0;
        int updatedCount = 0;
        int skippedCount = 0;

        qDebug() << "LibraryScanner: Found" << total << "audio files to process";

        auto* db = LibraryDatabase::instance();

        // ── Phase 1: Classify files (batch hash lookup) ──────────────
        stepTimer.start();

        // Single DB query → in-memory hash for O(1) skip checks
        auto knownTracks = db->allTrackFileMeta();  // path → (size, mtime)

        QStringList filesToProcess;
        filesToProcess.reserve(total);

        for (const QString& filePath : allFiles) {
            if (m_stopRequested) break;

            auto it = knownTracks.find(filePath);
            if (it != knownTracks.end()) {
                QFileInfo fi(filePath);
                qint64 currentSize = fi.size();
                qint64 currentMtime = fi.lastModified().toSecsSinceEpoch();

                if (it.value().first == currentSize
                    && it.value().second == currentMtime
                    && it.value().first > 0) {
                    // Unchanged — skip
                    m_knownFiles.insert(filePath);
                    skippedCount++;
                } else {
                    // Changed — remove old entry, queue for re-parse
                    db->removeTrackByPath(filePath);
                    filesToProcess.append(filePath);
                    updatedCount++;
                }
            } else {
                filesToProcess.append(filePath);
                newCount++;
            }
        }

        qDebug() << "[TIMING] Phase 1 (classify):" << filesToProcess.size()
                 << "to process," << skippedCount << "skipped in"
                 << stepTimer.elapsed() << "ms";

        // ── Phase 2: Parallel metadata read + serial DB insert ───────
        const int BATCH_SIZE = 100;
        int totalNew = filesToProcess.size();
        int processedCount = 0;

        // Split files by storage type — external USB/HDD can't handle
        // concurrent random reads, so reduce parallelism for /Volumes/
        QStringList localFiles, externalFiles;
        for (const QString& f : filesToProcess) {
            if (f.startsWith(QLatin1String("/Volumes/")))
                externalFiles.append(f);
            else
                localFiles.append(f);
        }

        QThreadPool pool;
        int localThreads = qMin(QThread::idealThreadCount(), 8);
        int extThreads = qMin(2, localThreads);

        qDebug() << "[SCAN] Split:" << localFiles.size() << "local (threads:"
                 << localThreads << ")," << externalFiles.size()
                 << "external (threads:" << extThreads << ")";

        auto processOneFile = [](const QString& path) -> Track {
            auto opt = MetadataReader::readTrack(path);
            if (opt.has_value()) {
                Track t = std::move(opt.value());
                QFileInfo fi(path);
                t.fileSize = fi.size();
                t.fileMtime = fi.lastModified().toSecsSinceEpoch();
                return t;
            }
            return Track{};  // empty filePath = failed
        };

        stepTimer.restart();
        qint64 tagLibReadMs = 0;

        // Batch-process a file list with the given thread count
        auto scanGroup = [&](const QStringList& files, int threads, const char* label) {
            if (files.isEmpty()) return;
            pool.setMaxThreadCount(threads);
            QElapsedTimer groupTimer; groupTimer.start();

            for (int i = 0; i < files.size(); i += BATCH_SIZE) {
                if (m_stopRequested) break;

                QStringList chunk = files.mid(i, BATCH_SIZE);

                QElapsedTimer tagLibTimer;
                tagLibTimer.start();
                QList<Track> batchTracks = QtConcurrent::blockingMapped(
                    &pool, chunk, processOneFile);
                tagLibReadMs += tagLibTimer.elapsed();

                // Serial DB insert (batch transaction)
                db->beginTransaction();
                for (const Track& track : batchTracks) {
                    if (!track.filePath.isEmpty()) {
                        db->insertTrack(track);
                        m_knownFiles.insert(track.filePath);
                    }
                }
                db->commitTransaction();

                processedCount += chunk.size();

                // Progressive reload on main thread
                int p = processedCount, t = total;
                QMetaObject::invokeMethod(this, [this, p, t]() {
                    emit batchReady(p, t);
                }, Qt::QueuedConnection);

                // Progress update
                QMetaObject::invokeMethod(this, [this, p, t]() {
                    emit scanProgress(p, t);
                }, Qt::QueuedConnection);

                qDebug() << "[SCAN]" << processedCount << "/" << totalNew
                         << "files (" << stepTimer.elapsed() << "ms)";
            }

            int perFile = (files.size() > 0) ? (int)(groupTimer.elapsed() / files.size()) : 0;
            qDebug() << "[TIMING] Phase 2" << label << ":" << files.size()
                     << "files in" << groupTimer.elapsed() << "ms"
                     << "(" << perFile << "ms/file," << threads << "threads)";
        };

        // Local SSD first (full parallelism), then external (reduced)
        scanGroup(localFiles, localThreads, "local");
        scanGroup(externalFiles, extThreads, "external");

        int perFileTotal = (totalNew > 0) ? (int)(tagLibReadMs / totalNew) : 0;
        qDebug() << "[TIMING] Phase 2 TagLib-only:" << totalNew
                 << "files in" << tagLibReadMs << "ms"
                 << "(" << perFileTotal << "ms/file)";

        int perFile = (totalNew > 0) ? (int)(stepTimer.elapsed() / totalNew) : 0;
        qDebug() << "[TIMING] Phase 2 (parallel scan):" << totalNew
                 << "files in" << stepTimer.elapsed() << "ms"
                 << "(" << perFile << "ms/file)";

        qDebug() << "[TIMING] File scan total:" << pipelineTimer.elapsed() << "ms —"
                 << "scanned:" << (newCount + updatedCount)
                 << "skipped:" << skippedCount
                 << "new:" << newCount << "updated:" << updatedCount;

        qDebug() << "[LibraryScanner] Scan complete —"
                 << "scanned:" << (newCount + updatedCount)
                 << "skipped (unchanged):" << skippedCount
                 << "new:" << newCount
                 << "updated:" << updatedCount;

        // Rebuild album/artist tables only if tracks changed
        if ((newCount + updatedCount) > 0) {
            stepTimer.start();
            db->rebuildAlbumsAndArtists();
            qDebug() << "[TIMING] rebuildAlbumsAndArtists:" << stepTimer.elapsed() << "ms";

            stepTimer.restart();
            db->rebuildFTSIndex();
            qDebug() << "[TIMING] rebuildFTSIndex:" << stepTimer.elapsed() << "ms";
        } else {
            qDebug() << "[SCAN] No changes — skipping rebuild";
        }

        qDebug() << "[TIMING] === SCAN WORKER THREAD DONE ===" << pipelineTimer.elapsed() << "ms total";

        int finalCount = skippedCount + processedCount;
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

    // Populate file size/mtime for future scan skip
    QFileInfo fi(filePath);
    track.fileSize = fi.size();
    track.fileMtime = fi.lastModified().toSecsSinceEpoch();

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

    // Build ignore extensions set
    QSet<QString> ignoreExts;
    for (const QString& ext : Settings::instance()->ignoreExtensions())
        ignoreExts.insert(ext.toLower());

    // Check for new files
    QDirIterator it(path, QDir::Files);
    while (it.hasNext()) {
        it.next();
        QFileInfo fi = it.fileInfo();
        QString suffix = fi.suffix().toLower();
        if (ignoreExts.contains(suffix)) continue;
        if (!s_supportedExtensions.contains(suffix))
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

    // Incremental: insertTrack/removeTrackByPath already handle album/artist updates.
    // Just emit databaseChanged after a short debounce.
    QTimer::singleShot(500, this, []() {
        LibraryDatabase::instance()->cleanOrphanedAlbumsAndArtists();
        emit LibraryDatabase::instance()->databaseChanged();
    });
}

void LibraryScanner::onFileChanged(const QString& path)
{
    qDebug() << "LibraryScanner: File changed:" << path;

    QFileInfo fi(path);
    if (fi.exists()) {
        // File was modified — remove old entry and re-read metadata
        // removeTrackByPath now handles album stats + orphan cleanup incrementally
        LibraryDatabase::instance()->removeTrackByPath(path);
        m_knownFiles.remove(path);
        // processFile → insertTrack now handles findOrCreate + stats incrementally
        processFile(path);
    } else {
        // File was deleted — removeTrackByPath handles cleanup incrementally
        LibraryDatabase::instance()->removeTrackByPath(path);
        m_knownFiles.remove(path);
        emit fileRemoved(path);
    }
}
