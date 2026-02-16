#include "PlaybackBar.h"
#include "playbackbar/NowPlayingInfo.h"
#include "playbackbar/TransportControls.h"
#include "playbackbar/DeviceVolumeControl.h"
#include "../core/ThemeManager.h"
#include "../core/Settings.h"
#include "../core/MusicData.h"
#include "../core/audio/AudioEngine.h"
#include "../core/audio/AudioDeviceManager.h"
#include "../core/CoverArtLoader.h"
#include "../apple/MusicKitPlayer.h"
#include <QHBoxLayout>
#include <QMenu>
#include <QActionGroup>

// ═════════════════════════════════════════════════════════════════════
//  PlaybackBar — thin coordinator composing 3 sub-widgets
// ═════════════════════════════════════════════════════════════════════

PlaybackBar::PlaybackBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("PlaybackBar");
    setFixedHeight(UISizes::playbackBarHeight);
    setAttribute(Qt::WA_OpaquePaintEvent);

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(16, 0, 16, 0);
    mainLayout->setSpacing(0);

    m_nowPlaying = new NowPlayingInfo(this);
    m_transport = new TransportControls(this);
    m_deviceVolume = new DeviceVolumeControl(this);

    mainLayout->addWidget(m_nowPlaying);
    mainLayout->addWidget(m_transport, 1);  // stretch — takes remaining space
    mainLayout->addWidget(m_deviceVolume);

    wirePlaybackStateSignals();
    wireTransportSignals();
    wireNowPlayingSignals();
    wireDeviceVolumeSignals();

    // Theme
    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        m_nowPlaying->refreshTheme();
        m_transport->refreshTheme();
        m_deviceVolume->refreshTheme();
    });

    // Sync initial state
    auto* ps = PlaybackState::instance();
    m_transport->setPlaying(ps->isPlaying());
    m_transport->setShuffleEnabled(ps->shuffleEnabled());
    m_transport->setRepeatMode(ps->repeatMode());

    Track current = ps->currentTrack();
    m_nowPlaying->setTrack(current);
    m_transport->resetProgress(current.duration);
    if (!current.filePath.isEmpty() || !current.coverUrl.isEmpty())
        CoverArtLoader::instance()->requestCoverArt(current.filePath, current.coverUrl, 56);
}

// ═════════════════════════════════════════════════════════════════════
//  Wire: PlaybackState → sub-widgets
// ═════════════════════════════════════════════════════════════════════

void PlaybackBar::wirePlaybackStateSignals()
{
    auto* ps = PlaybackState::instance();

    // Transport state updates
    connect(ps, &PlaybackState::playStateChanged, this, [this](bool playing) {
        m_transport->setPlaying(playing);
        if (!playing)
            m_nowPlaying->setAutoplayVisible(false);
    });

    connect(ps, &PlaybackState::timeChanged, this, [this](int seconds) {
        int duration = PlaybackState::instance()->currentTrack().duration;
        m_transport->setTime(seconds, duration);
    });

    connect(ps, &PlaybackState::shuffleChanged, this, [this](bool enabled) {
        m_transport->setShuffleEnabled(enabled);
    });

    connect(ps, &PlaybackState::repeatChanged, this, [this](PlaybackState::RepeatMode mode) {
        m_transport->setRepeatMode(mode);
    });

    // Track changes → NowPlaying + reset progress
    connect(ps, &PlaybackState::trackChanged, this, [this](const Track& track) {
        m_nowPlaying->setTrack(track);
        m_transport->resetProgress(track.duration);
        if (!track.filePath.isEmpty() || !track.coverUrl.isEmpty())
            CoverArtLoader::instance()->requestCoverArt(track.filePath, track.coverUrl, 56);
    });

    // Volume
    connect(ps, &PlaybackState::volumeChanged, this, [this](int volume) {
        m_deviceVolume->setVolume(volume);
    });

    // Autoplay
    connect(ps, &PlaybackState::autoplayTrackStarted, this, [this]() {
        m_nowPlaying->setAutoplayVisible(true);
    });
    connect(ps, &PlaybackState::queueChanged, this, [this]() {
        m_nowPlaying->setAutoplayVisible(false);
    });
}

// ═════════════════════════════════════════════════════════════════════
//  Wire: TransportControls → PlaybackState
// ═════════════════════════════════════════════════════════════════════

void PlaybackBar::wireTransportSignals()
{
    auto* ps = PlaybackState::instance();

    connect(m_transport, &TransportControls::playPauseClicked,
            ps, &PlaybackState::playPause);
    connect(m_transport, &TransportControls::nextClicked,
            ps, &PlaybackState::next);
    connect(m_transport, &TransportControls::previousClicked,
            ps, &PlaybackState::previous);
    connect(m_transport, &TransportControls::shuffleClicked,
            ps, &PlaybackState::toggleShuffle);
    connect(m_transport, &TransportControls::repeatClicked,
            ps, &PlaybackState::cycleRepeat);
    connect(m_transport, &TransportControls::seekRequested, this, [](int seconds) {
        PlaybackState::instance()->seek(seconds);
    });
}

// ═════════════════════════════════════════════════════════════════════
//  Wire: NowPlayingInfo → artist navigation + cover art
// ═════════════════════════════════════════════════════════════════════

void PlaybackBar::wireNowPlayingSignals()
{
    // Async cover art loader → NowPlaying display
    connect(CoverArtLoader::instance(), &CoverArtLoader::coverArtReady,
            m_nowPlaying, &NowPlayingInfo::onCoverArtReady);

    // Subtitle click → look up artist and navigate
    connect(m_nowPlaying, &NowPlayingInfo::subtitleClicked, this, [this]() {
        const Track current = PlaybackState::instance()->currentTrack();
        if (current.artist.isEmpty()) return;

        QString targetId = current.artistId;

        // Fallback: look up by name
        if (targetId.isEmpty()) {
            const auto artists = MusicDataProvider::instance()->allArtists();
            for (const auto& a : artists) {
                if (a.name == current.artist) {
                    targetId = a.id;
                    break;
                }
            }
        }

        if (!targetId.isEmpty()) {
            qDebug() << "[PlaybackBar] Artist clicked:" << current.artist
                     << "id:" << targetId;
            emit artistClicked(targetId);
        } else {
            qDebug() << "[PlaybackBar] Artist not found:" << current.artist;
        }
    });
}

// ═════════════════════════════════════════════════════════════════════
//  Wire: DeviceVolumeControl → AudioEngine + queue toggle
// ═════════════════════════════════════════════════════════════════════

void PlaybackBar::wireDeviceVolumeSignals()
{
    connect(m_deviceVolume, &DeviceVolumeControl::volumeChanged, this, [](int value) {
        PlaybackState::instance()->setVolume(value);
    });

    connect(m_deviceVolume, &DeviceVolumeControl::muteClicked, this, [this]() {
        // DeviceVolumeControl toggles m_isMuted internally and updates icon.
        // We just need to set the audio engine volume.
        // Check if currently muted by reading volume icon state:
        // volume-x icon means muted → set volume to 0
        // Otherwise → restore from slider
        // Simpler: DeviceVolumeControl tracks mute state, we query slider value
        // On mute: silence. On unmute: restore slider volume.
        // The widget handles the icon; we handle the audio.
        static bool muted = false;
        muted = !muted;
        if (muted) {
            AudioEngine::instance()->setVolume(0.0f);
        } else {
            // Restore volume from PlaybackState (which tracks the real volume)
            float vol = PlaybackState::instance()->volume() / 100.0f;
            AudioEngine::instance()->setVolume(vol);
        }
    });

    connect(m_deviceVolume, &DeviceVolumeControl::deviceClicked,
            this, &PlaybackBar::onDeviceClicked);

    connect(m_deviceVolume, &DeviceVolumeControl::queueToggled,
            this, &PlaybackBar::queueToggled);
}

// ═════════════════════════════════════════════════════════════════════
//  Device popup menu (singleton access stays in coordinator)
// ═════════════════════════════════════════════════════════════════════

void PlaybackBar::onDeviceClicked()
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setStyleSheet(ThemeManager::instance()->menuStyle());

    auto* group = new QActionGroup(menu);
    group->setExclusive(true);

    auto devices = AudioDeviceManager::instance()->outputDevices();
    uint32_t savedId = Settings::instance()->outputDeviceId();

    for (const auto& dev : devices) {
        if (dev.outputChannels <= 0) continue;

        auto* action = menu->addAction(dev.name);
        action->setCheckable(true);
        action->setChecked(dev.deviceId == savedId || (savedId == 0 && dev.isDefault));
        group->addAction(action);

        connect(action, &QAction::triggered, this, [deviceId = dev.deviceId]() {
            AudioEngine::instance()->setOutputDevice(deviceId);
            Settings::instance()->setOutputDeviceId(deviceId);
            auto info = AudioDeviceManager::instance()->deviceById(deviceId);
            Settings::instance()->setOutputDeviceUID(info.uid);
            Settings::instance()->setOutputDeviceName(info.name);
            MusicKitPlayer::instance()->updateOutputDevice();
        });
    }

    if (menu->isEmpty()) {
        menu->addAction(QStringLiteral("No Output Devices"))->setEnabled(false);
    }

    QPoint pos = mapToGlobal(QPoint(width() - menu->sizeHint().width(), 0));
    pos.setY(pos.y() - menu->sizeHint().height());
    menu->popup(pos);
}
