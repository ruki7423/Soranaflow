#pragma once

#include <QObject>
#include <QThread>
#include <atomic>

class ReplayGainScanner : public QObject {
    Q_OBJECT

public:
    static ReplayGainScanner* instance();
    ~ReplayGainScanner();

    bool isScanning() const { return m_scanning.load(std::memory_order_relaxed); }

public slots:
    void startScan();
    void stopScan();

signals:
    void scanStarted();
    void scanProgress(int current, int total);
    void scanFinished(int scannedCount, int albumCount);

private:
    explicit ReplayGainScanner(QObject* parent = nullptr);

    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_stopRequested{false};
    QThread* m_workerThread = nullptr;
};
