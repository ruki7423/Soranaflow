#pragma once

#include <QObject>
#include <QStringList>
#include <QFileSystemWatcher>
#include <QSet>
#include <QMutex>
#include <QThread>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <atomic>

class LibraryScanner : public QObject {
    Q_OBJECT

public:
    static LibraryScanner* instance();
    ~LibraryScanner();

    bool isScanning() const { return m_scanning; }

    void setWatchEnabled(bool enabled);
    bool isWatchEnabled() const { return m_watchEnabled; }

public slots:
    void scanFolders(const QStringList& folders);
    void stopScan();

signals:
    void scanStarted();
    void scanProgress(int current, int total);
    void batchReady(int processed, int total);
    void scanFinished(int tracksFound);
    void scanError(const QString& message);
    void fileAdded(const QString& filePath);
    void fileRemoved(const QString& filePath);

private:
    explicit LibraryScanner(QObject* parent = nullptr);

    void scanFolder(const QString& folder);
    void processFile(const QString& filePath);
    void onDirectoryChanged(const QString& path);
    void onFileChanged(const QString& path);

    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_stopRequested{false};
    bool m_watchEnabled = false;

    QFileSystemWatcher* m_watcher = nullptr;
    QSet<QString> m_knownFiles;
    QMutex m_knownFilesMutex;
    QStringList m_watchedFolders;

    QThread* m_workerThread = nullptr;
    QElapsedTimer m_scanCooldown;

    static const QStringList s_supportedExtensions;
};
