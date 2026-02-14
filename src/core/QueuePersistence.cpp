#include "QueuePersistence.h"
#include "QueueManager.h"
#include "Settings.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent>

QueuePersistence::QueuePersistence(QueueManager* mgr, QObject* parent)
    : QObject(parent)
    , m_mgr(mgr)
{
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500);
    connect(m_saveTimer, &QTimer::timeout, this, &QueuePersistence::doSave);
}

void QueuePersistence::scheduleSave()
{
    if (m_restoring) return;
    m_saveTimer->start();  // restarts the 500ms timer
}

void QueuePersistence::doSave()
{
    // Snapshot queue data (fast — just copies QVector of value types)
    QVector<Track> snapshot = m_mgr->queue();
    int idx = m_mgr->currentIndex();
    bool shuffle = m_mgr->shuffleEnabled();
    int repeat = m_mgr->repeatMode();

    (void)QtConcurrent::run([snapshot, idx, shuffle, repeat]() {
        static QMutex mutex;
        QMutexLocker lock(&mutex);

        QElapsedTimer timer;
        timer.start();

        QSettings settings(Settings::settingsPath(), QSettings::IniFormat);

        settings.beginWriteArray(QStringLiteral("queue/tracks"), snapshot.size());
        for (int i = 0; i < snapshot.size(); ++i) {
            settings.setArrayIndex(i);
            const Track& t = snapshot[i];
            settings.setValue(QStringLiteral("id"), t.id);
            settings.setValue(QStringLiteral("title"), t.title);
            settings.setValue(QStringLiteral("artist"), t.artist);
            settings.setValue(QStringLiteral("album"), t.album);
            settings.setValue(QStringLiteral("albumId"), t.albumId);
            settings.setValue(QStringLiteral("duration"), t.duration);
            settings.setValue(QStringLiteral("filePath"), t.filePath);
            settings.setValue(QStringLiteral("trackNumber"), t.trackNumber);
            settings.setValue(QStringLiteral("format"), static_cast<int>(t.format));
            settings.setValue(QStringLiteral("sampleRate"), t.sampleRate);
            settings.setValue(QStringLiteral("bitDepth"), t.bitDepth);
            settings.setValue(QStringLiteral("bitrate"), t.bitrate);
            settings.setValue(QStringLiteral("coverUrl"), t.coverUrl);
        }
        settings.endArray();

        settings.setValue(QStringLiteral("queue/currentIndex"), idx);
        settings.setValue(QStringLiteral("queue/shuffle"), shuffle);
        settings.setValue(QStringLiteral("queue/repeat"), repeat);

        qDebug() << "[Queue] Saved" << snapshot.size() << "tracks in"
                 << timer.elapsed() << "ms (async, index:" << idx << ")";
    });
}

void QueuePersistence::saveImmediate()
{
    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);

    const auto queue = m_mgr->queue();

    settings.beginWriteArray(QStringLiteral("queue/tracks"), queue.size());
    for (int i = 0; i < queue.size(); ++i) {
        settings.setArrayIndex(i);
        const Track& t = queue[i];
        settings.setValue(QStringLiteral("id"), t.id);
        settings.setValue(QStringLiteral("title"), t.title);
        settings.setValue(QStringLiteral("artist"), t.artist);
        settings.setValue(QStringLiteral("album"), t.album);
        settings.setValue(QStringLiteral("albumId"), t.albumId);
        settings.setValue(QStringLiteral("duration"), t.duration);
        settings.setValue(QStringLiteral("filePath"), t.filePath);
        settings.setValue(QStringLiteral("trackNumber"), t.trackNumber);
        settings.setValue(QStringLiteral("format"), static_cast<int>(t.format));
        settings.setValue(QStringLiteral("sampleRate"), t.sampleRate);
        settings.setValue(QStringLiteral("bitDepth"), t.bitDepth);
        settings.setValue(QStringLiteral("bitrate"), t.bitrate);
        settings.setValue(QStringLiteral("coverUrl"), t.coverUrl);
    }
    settings.endArray();

    settings.setValue(QStringLiteral("queue/currentIndex"), m_mgr->currentIndex());
    settings.setValue(QStringLiteral("queue/shuffle"), m_mgr->shuffleEnabled());
    settings.setValue(QStringLiteral("queue/repeat"), m_mgr->repeatMode());

    qDebug() << "[Queue] Saved" << queue.size() << "tracks, index:" << m_mgr->currentIndex();
}

void QueuePersistence::restore()
{
    m_restoring = true;

    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);

    int count = settings.beginReadArray(QStringLiteral("queue/tracks"));
    QVector<Track> tracks;
    tracks.reserve(count);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        Track t;
        t.id = settings.value(QStringLiteral("id")).toString();
        t.title = settings.value(QStringLiteral("title")).toString();
        t.artist = settings.value(QStringLiteral("artist")).toString();
        t.album = settings.value(QStringLiteral("album")).toString();
        t.albumId = settings.value(QStringLiteral("albumId")).toString();
        t.duration = settings.value(QStringLiteral("duration")).toInt();
        t.filePath = settings.value(QStringLiteral("filePath")).toString();
        t.trackNumber = settings.value(QStringLiteral("trackNumber")).toInt();
        t.format = static_cast<AudioFormat>(settings.value(QStringLiteral("format")).toInt());
        t.sampleRate = settings.value(QStringLiteral("sampleRate")).toString();
        t.bitDepth = settings.value(QStringLiteral("bitDepth")).toString();
        t.bitrate = settings.value(QStringLiteral("bitrate")).toString();
        t.coverUrl = settings.value(QStringLiteral("coverUrl")).toString();
        tracks.append(t);
    }
    settings.endArray();

    int idx = settings.value(QStringLiteral("queue/currentIndex"), -1).toInt();
    bool shuffle = settings.value(QStringLiteral("queue/shuffle"), false).toBool();
    int repeat = settings.value(QStringLiteral("queue/repeat"), 0).toInt();

    m_mgr->restoreState(tracks, idx, shuffle, repeat);

    qDebug() << "[Queue] Restored" << tracks.size() << "tracks, index:" << idx;

    m_restoring = false;
}

void QueuePersistence::flushPending()
{
    if (m_saveTimer->isActive()) {
        m_saveTimer->stop();
        doSave();
        qDebug() << "[Shutdown] Flushed pending queue save";
    }
}
