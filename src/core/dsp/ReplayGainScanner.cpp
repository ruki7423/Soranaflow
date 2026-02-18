#include "ReplayGainScanner.h"
#include "LoudnessAnalyzer.h"
#include "../library/LibraryDatabase.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <QThread>
#include <cmath>

// ── Singleton ────────────────────────────────────────────────────────
ReplayGainScanner* ReplayGainScanner::instance()
{
    static ReplayGainScanner s;
    return &s;
}

ReplayGainScanner::ReplayGainScanner(QObject* parent)
    : QObject(parent)
{
}

ReplayGainScanner::~ReplayGainScanner()
{
    stopScan();
}

// ── stopScan ─────────────────────────────────────────────────────────
void ReplayGainScanner::stopScan()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->wait(10000);
    }
}

// ── startScan ────────────────────────────────────────────────────────
void ReplayGainScanner::startScan()
{
    if (m_scanning.load(std::memory_order_relaxed))
        return;

    m_scanning.store(true, std::memory_order_relaxed);
    m_stopRequested.store(false, std::memory_order_relaxed);
    emit scanStarted();

    m_workerThread = QThread::create([this]() {
        QElapsedTimer timer;
        timer.start();

        auto* db = LibraryDatabase::instance();
        QVector<Track> tracks = db->allTracks();

        // Filter to tracks that need scanning (no R128 data)
        QVector<Track> toScan;
        toScan.reserve(tracks.size());
        for (const Track& t : tracks) {
            if (t.filePath.isEmpty()) continue;
            if (t.hasR128 && t.r128Loudness != 0.0) continue;
            toScan.append(t);
        }

        int total = toScan.size();
        int scanned = 0;

        qDebug() << "[ReplayGainScanner] Starting scan:"
                 << total << "tracks to analyze," << tracks.size() << "total in library";

        QMetaObject::invokeMethod(this, [this, total]() {
            emit scanProgress(0, total);
        }, Qt::QueuedConnection);

        // ── Phase 1: Per-track R128 analysis ─────────────────────────
        QThreadPool pool;
        int threads = qMin(QThread::idealThreadCount(), 4);
        pool.setMaxThreadCount(threads);

        const int BATCH_SIZE = 20;

        struct ScanResult {
            QString filePath;
            double loudness = 0.0;
            double peak = 0.0;
            bool valid = false;
        };

        for (int i = 0; i < total; i += BATCH_SIZE) {
            if (m_stopRequested.load(std::memory_order_relaxed))
                break;

            int batchEnd = qMin(i + BATCH_SIZE, total);
            QVector<Track> batch(toScan.begin() + i, toScan.begin() + batchEnd);

            QList<ScanResult> results = QtConcurrent::blockingMapped(
                &pool, batch, [this](const Track& t) -> ScanResult {
                    ScanResult r;
                    r.filePath = t.filePath;
                    if (m_stopRequested.load(std::memory_order_relaxed))
                        return r;
                    LoudnessResult lr = LoudnessAnalyzer::analyze(t.filePath);
                    r.loudness = lr.integratedLoudness;
                    r.peak = lr.truePeak;
                    r.valid = lr.valid;
                    return r;
                });

            // Store results in DB (serial)
            db->beginTransaction();
            for (const ScanResult& r : results) {
                if (r.valid) {
                    db->updateR128Loudness(r.filePath, r.loudness, r.peak);
                }
            }
            db->commitTransaction();

            scanned += batchEnd - i;

            int s = scanned, t2 = total;
            QMetaObject::invokeMethod(this, [this, s, t2]() {
                emit scanProgress(s, t2);
            }, Qt::QueuedConnection);

            qDebug() << "[ReplayGainScanner]" << scanned << "/" << total
                     << "tracks analyzed (" << timer.elapsed() << "ms)";
        }

        if (m_stopRequested.load(std::memory_order_relaxed)) {
            qDebug() << "[ReplayGainScanner] Scan stopped by user at" << scanned << "/" << total;
            QMetaObject::invokeMethod(this, [this]() {
                m_scanning.store(false, std::memory_order_relaxed);
                emit scanFinished(0, 0);
            }, Qt::QueuedConnection);
            return;
        }

        // ── Phase 2: Album gain calculation ──────────────────────────
        // Re-read all tracks to get updated R128 data
        tracks = db->allTracks();

        // Group tracks by albumId
        QHash<QString, QVector<int>> albumGroups;  // albumId → indices into tracks
        for (int idx = 0; idx < tracks.size(); ++idx) {
            const Track& t = tracks[idx];
            if (t.albumId.isEmpty()) continue;
            if (t.r128Loudness == 0.0) continue;
            albumGroups[t.albumId].append(idx);
        }

        int albumCount = 0;

        db->beginTransaction();
        for (auto it = albumGroups.constBegin(); it != albumGroups.constEnd(); ++it) {
            const QVector<int>& indices = it.value();
            if (indices.size() < 2) continue;  // single-track: no album gain needed

            // Power-average loudness: 10 * log10( mean(10^(L_i/10)) )
            double sumLinear = 0.0;
            double maxAdjustedPeak = 0.0;
            for (int idx : indices) {
                const Track& t = tracks[idx];
                sumLinear += std::pow(10.0, t.r128Loudness / 10.0);
                // Adjusted peak = raw peak * track gain (accounts for gain applied at playback)
                double trackGainDb = -18.0 - t.r128Loudness;
                double trackPeakLinear = std::pow(10.0, t.r128Peak / 20.0);
                double trackGainLinear = std::pow(10.0, trackGainDb / 20.0);
                double adjustedPeak = trackPeakLinear * trackGainLinear;
                if (adjustedPeak > maxAdjustedPeak)
                    maxAdjustedPeak = adjustedPeak;
            }

            double albumLoudness = 10.0 * std::log10(sumLinear / indices.size());
            double albumGainDb = -18.0 - albumLoudness;  // ReplayGain 2.0 reference
            double albumPeakLinear = maxAdjustedPeak;

            for (int idx : indices) {
                const Track& t = tracks[idx];
                double trackGainDb = -18.0 - t.r128Loudness;
                double trackPeakLinear = std::pow(10.0, t.r128Peak / 20.0);
                db->updateReplayGain(t.filePath, trackGainDb, albumGainDb,
                                     trackPeakLinear, albumPeakLinear);
            }
            albumCount++;
        }
        db->commitTransaction();

        qDebug() << "[ReplayGainScanner] Scan complete:" << scanned << "tracks,"
                 << albumCount << "albums in" << timer.elapsed() << "ms";

        int finalScanned = scanned;
        int finalAlbums = albumCount;
        QMetaObject::invokeMethod(this, [this, finalScanned, finalAlbums]() {
            m_scanning.store(false, std::memory_order_relaxed);
            emit scanFinished(finalScanned, finalAlbums);
        }, Qt::QueuedConnection);
    });

    connect(m_workerThread, &QThread::finished, m_workerThread, &QObject::deleteLater);
    m_workerThread->start();
}
