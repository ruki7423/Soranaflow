#pragma once

#include <QObject>
#include <atomic>
#include "../MusicData.h"

class VolumeLevelingManager : public QObject {
    Q_OBJECT

public:
    explicit VolumeLevelingManager(QObject* parent = nullptr);

    void setCurrentTrack(const Track& track);
    void updateGain();

    // Lock-free — safe to call from render thread
    float gainLinear() const { return m_gain.load(std::memory_order_relaxed); }
    float gainDb() const;

    const Track& currentTrack() const { return m_currentTrack; }

signals:
    void gainChanged();

private:
    Track m_currentTrack;
    std::atomic<float> m_gain{1.0f};
};
