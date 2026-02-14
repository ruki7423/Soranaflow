#include "VolumeLevelingManager.h"
#include "AudioEngine.h"
#include "../dsp/LoudnessAnalyzer.h"
#include "../Settings.h"
#include "../library/LibraryDatabase.h"

#include <QDebug>
#include <QtConcurrent>
#include <cmath>

VolumeLevelingManager::VolumeLevelingManager(QObject* parent)
    : QObject(parent)
{
}

void VolumeLevelingManager::setCurrentTrack(const Track& track)
{
    m_currentTrack = track;

    // Enrich with stored ReplayGain data from DB if the track doesn't have it
    // (e.g. when converted from TrackIndex which doesn't carry RG fields)
    if (!m_currentTrack.hasReplayGain && !m_currentTrack.filePath.isEmpty()) {
        auto dbTrack = LibraryDatabase::instance()->trackByPath(m_currentTrack.filePath);
        if (dbTrack && dbTrack->hasReplayGain) {
            m_currentTrack.replayGainTrack     = dbTrack->replayGainTrack;
            m_currentTrack.replayGainAlbum     = dbTrack->replayGainAlbum;
            m_currentTrack.replayGainTrackPeak = dbTrack->replayGainTrackPeak;
            m_currentTrack.replayGainAlbumPeak = dbTrack->replayGainAlbumPeak;
            m_currentTrack.hasReplayGain       = true;
        }
    }

    updateGain();

    // Background R128 analysis if no gain data and leveling is enabled
    if (Settings::instance()->volumeLeveling()
        && !m_currentTrack.hasReplayGain
        && !m_currentTrack.hasR128
        && !m_currentTrack.filePath.isEmpty()) {

        QString path = m_currentTrack.filePath;
        (void)QtConcurrent::run([this, path]() {
            LoudnessResult r = LoudnessAnalyzer::analyze(path);
            if (r.valid) {
                QMetaObject::invokeMethod(this, [this, r, path]() {
                    // Only update if still the same track
                    if (m_currentTrack.filePath == path) {
                        m_currentTrack.r128Loudness = r.integratedLoudness;
                        m_currentTrack.r128Peak = r.truePeak;
                        m_currentTrack.hasR128 = true;
                        updateGain();
                    }
                    // Cache in DB
                    LibraryDatabase::instance()->updateR128Loudness(
                        path, r.integratedLoudness, r.truePeak);
                }, Qt::QueuedConnection);
            }
        });
    }
}

void VolumeLevelingManager::updateGain()
{
    if (!Settings::instance()->volumeLeveling() || m_currentTrack.filePath.isEmpty()) {
        m_gain.store(1.0f, std::memory_order_relaxed);
        emit gainChanged();
        return;
    }

    double targetLUFS = Settings::instance()->targetLoudness();
    int mode = Settings::instance()->levelingMode();
    double gainDB = 0.0;

    if (m_currentTrack.hasReplayGain) {
        // ReplayGain: value is already a gain adjustment
        // RG reference = -18 LUFS, adjust to our target
        double rgGain = (mode == 1 && m_currentTrack.replayGainAlbum != 0.0)
            ? m_currentTrack.replayGainAlbum
            : m_currentTrack.replayGainTrack;
        double rgRef = -18.0;
        gainDB = rgGain + (targetLUFS - rgRef);

        // Peak limiting
        double peak = (mode == 1 && m_currentTrack.replayGainAlbumPeak != 1.0)
            ? m_currentTrack.replayGainAlbumPeak
            : m_currentTrack.replayGainTrackPeak;
        double linearGain = std::pow(10.0, gainDB / 20.0);
        if (peak > 0.0 && peak * linearGain > 1.0) {
            linearGain = 1.0 / peak;
            gainDB = 20.0 * std::log10(linearGain);
        }
    } else if (m_currentTrack.hasR128 && m_currentTrack.r128Loudness != 0.0) {
        gainDB = targetLUFS - m_currentTrack.r128Loudness;
    } else {
        // No gain data available
        m_gain.store(1.0f, std::memory_order_relaxed);
        emit gainChanged();
        return;
    }

    // Clamp to safe range
    gainDB = std::max(-12.0, std::min(12.0, gainDB));
    float linear = static_cast<float>(std::pow(10.0, gainDB / 20.0));
    m_gain.store(linear, std::memory_order_relaxed);

    qDebug() << "[Volume Leveling]" << m_currentTrack.title
             << "gain:" << gainDB << "dB linear:" << linear
             << (m_currentTrack.hasReplayGain ? "ReplayGain" :
                 m_currentTrack.hasR128 ? "R128" : "None");

    emit gainChanged();
}

float VolumeLevelingManager::gainDb() const
{
    float linear = m_gain.load(std::memory_order_relaxed);
    if (linear <= 0.0f || linear == 1.0f) return 0.0f;
    return 20.0f * std::log10(linear);
}
