#pragma once
#include <QWidget>
#include "../core/PlaybackState.h"

class NowPlayingInfo;
class TransportControls;
class DeviceVolumeControl;

class PlaybackBar : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackBar(QWidget* parent = nullptr);

signals:
    void queueToggled(bool visible);
    void artistClicked(const QString& artistId);

private:
    void wirePlaybackStateSignals();
    void wireTransportSignals();
    void wireNowPlayingSignals();
    void wireDeviceVolumeSignals();
    void onDeviceClicked();

    NowPlayingInfo* m_nowPlaying = nullptr;
    TransportControls* m_transport = nullptr;
    DeviceVolumeControl* m_deviceVolume = nullptr;
};
