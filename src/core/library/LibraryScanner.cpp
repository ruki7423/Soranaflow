#include "LibraryScanner.h"
#include "LibraryDatabase.h"
#include "../audio/MetadataReader.h"
#include "../Settings.h"

#include <QCoreApplication>
#include <QtConcurrent>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
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
                    { QMutexLocker lock(&m_knownFilesMutex); m_knownFiles.insert(filePath); }
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

                // Serial DB insert — mini-batches of 20 to reduce mutex hold time
                // (lets read queries through between commits)
                const int MINI_BATCH = 20;
                for (int mb = 0; mb < batchTracks.size(); mb += MINI_BATCH) {
                    int mbEnd = qMin(mb + MINI_BATCH, (int)batchTracks.size());
                    db->beginTransaction();
                    for (int j = mb; j < mbEnd; ++j) {
                        const Track& track = batchTracks[j];
                        if (!track.filePath.isEmpty()) {
                            db->insertTrack(track);
                            { QMutexLocker lock(&m_knownFilesMutex); m_knownFiles.insert(track.filePath); }
                        }
                    }
                    db->commitTransaction();
                    if (mbEnd < batchTracks.size())
                        QThread::usleep(100);  // 0.1ms yield for read queries
                }

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
            m_scanning = false;
            m_scanCooldown.start();  // Start 5-second cooldown for directory change events
            qDebug() << "LibraryScanner: Scan complete." << finalCount << "tracks found";
            emit scanFinished(finalCount);

            // Set up file watching AFTER scanFinished (async, non-blocking)
            if (m_watchEnabled) {
                (void)QtConcurrent::run([this, folders]() {
                    QStringList dirs;
                    for (const QString& folder : folders) {
                        dirs << folder;
                        QDirIterator dirIt(folder, QDir::Dirs | QDir::NoDotAndDotDot,
                                           QDirIterator::Subdirectories);
                        while (dirIt.hasNext()) {
                            dirIt.next();
                            dirs << dirIt.filePath();
                        }
                    }
                    qDebug() << "[Scanner] Collected" << dirs.size() << "dirs for file watcher";
                    QMetaObject::invokeMethod(this, [this, dirs]() {
                        m_watcher->addPaths(dirs);
                        qDebug() << "[Scanner] File watchers set up:" << dirs.size() << "dirs";
                    }, Qt::QueuedConnection);
                });
            }
        }, Qt::QueuedConnection);
    });

    connect(m_workerThread, &QThread::finished, m_workerThread, &QObject::deleteLater);
    m_workerThread->start();
}

void LibraryScanner::stopScan()
{
    m_stopRequested = true;
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->wait(5000);
    }
}

LibraryScanner::~LibraryScanner()
{
    stopScan();
}

// ── processFile ─────────────────────────────────────────────────────
void LibraryScanner::processFile(const QString& filePath)
{
    // Skip if already in database
    if (LibraryDatabase::instance()->trackExists(filePath)) {
        { QMutexLocker lock(&m_knownFilesMutex); m_knownFiles.insert(filePath); }
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
    { QMutexLocker lock(&m_knownFilesMutex); m_knownFiles.insert(filePath); }
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

    // Collect new files (pre-register in m_knownFiles to prevent re-dispatch)
    QStringList newFiles;
    QDirIterator it(path, QDir::Files);
    while (it.hasNext()) {
        it.next();
        QFileInfo fi = it.fileInfo();
        QString suffix = fi.suffix().toLower();
        if (ignoreExts.contains(suffix)) continue;
        if (!s_supportedExtensions.contains(suffix))
            continue;

        QString filePath = fi.absoluteFilePath();
        {
            QMutexLocker lock(&m_knownFilesMutex);
            if (!m_knownFiles.contains(filePath)) {
                qDebug() << "LibraryScanner: New file detected:" << filePath;
                m_knownFiles.insert(filePath);
                newFiles.append(filePath);
            }
        }
    }

    // Process new files on worker thread (MetadataReader I/O off main thread)
    if (!newFiles.isEmpty()) {
        (void)QtConcurrent::run([this, newFiles]() {
            for (const QString& filePath : newFiles) {
                if (LibraryDatabase::instance()->trackExists(filePath))
                    continue;
                auto trackOpt = MetadataReader::readTrack(filePath);
                if (!trackOpt.has_value()) continue;
                Track track = trackOpt.value();
                QFileInfo fi(filePath);
                track.fileSize = fi.size();
                track.fileMtime = fi.lastModified().toSecsSinceEpoch();
                LibraryDatabase::instance()->insertTrack(track);
            }
            QMetaObject::invokeMethod(this, [this, newFiles]() {
                for (const QString& fp : newFiles)
                    emit fileAdded(fp);
                LibraryDatabase::instance()->cleanOrphanedAlbumsAndArtists();
                emit LibraryDatabase::instance()->databaseChanged();
            }, Qt::QueuedConnection);
        });
    }

    // Check for removed files — only check files directly in this directory
    QSet<QString> currentFiles;
    QDirIterator it2(path, QDir::Files);
    while (it2.hasNext()) {
        it2.next();
        currentFiles.insert(it2.fileInfo().absoluteFilePath());
    }

    QStringList toRemove;
    {
        QMutexLocker lock(&m_knownFilesMutex);
        for (const QString& known : m_knownFiles) {
            QFileInfo fi(known);
            if (fi.absolutePath() == path && !currentFiles.contains(known)) {
                toRemove.append(known);
            }
        }
        if (!toRemove.isEmpty()) {
            for (const QString& removed : toRemove) {
                qDebug() << "LibraryScanner: File removed:" << removed;
                LibraryDatabase::instance()->removeTrackByPath(removed);
                m_knownFiles.remove(removed);
                emit fileRemoved(removed);
            }
        }
    }
    if (!toRemove.isEmpty()) {
        QTimer::singleShot(500, this, []() {
            LibraryDatabase::instance()->cleanOrphanedAlbumsAndArtists();
            emit LibraryDatabase::instance()->databaseChanged();
        });
    }
}

void LibraryScanner::onFileChanged(const QString& path)
{
    qDebug() << "LibraryScanner: File changed:" << path;

    QFileInfo fi(path);
    if (fi.exists()) {
        // File was modified — remove old entry and re-read metadata on worker thread
        LibraryDatabase::instance()->removeTrackByPath(path);
        { QMutexLocker lock(&m_knownFilesMutex); m_knownFiles.remove(path); m_knownFiles.insert(path); }  // pre-register
        (void)QtConcurrent::run([this, path]() {
            auto trackOpt = MetadataReader::readTrack(path);
            if (!trackOpt.has_value()) return;
            Track track = trackOpt.value();
            QFileInfo tfi(path);
            track.fileSize = tfi.size();
            track.fileMtime = tfi.lastModified().toSecsSinceEpoch();
            LibraryDatabase::instance()->insertTrack(track);
            QMetaObject::invokeMethod(this, [this, path]() {
                emit fileAdded(path);
                emit LibraryDatabase::instance()->databaseChanged();
            }, Qt::QueuedConnection);
        });
    } else {
        // File was deleted — removeTrackByPath handles cleanup incrementally
        LibraryDatabase::instance()->removeTrackByPath(path);
        { QMutexLocker lock(&m_knownFilesMutex); m_knownFiles.remove(path); }
        emit fileRemoved(path);
    }
}
