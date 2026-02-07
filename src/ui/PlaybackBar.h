#pragma once
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QPixmap>
#include "../widgets/StyledButton.h"
#include "../widgets/StyledSlider.h"
#include "../core/PlaybackState.h"

class PlaybackBar : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackBar(QWidget* parent = nullptr);

signals:
    void queueToggled(bool visible);
    void artistClicked(const QString& artistId);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onPlayStateChanged(bool playing);
    void onTrackChanged(const Track& track);
    void onTimeChanged(int seconds);
    void onVolumeChanged(int volume);
    void onShuffleChanged(bool enabled);
    void onRepeatChanged(PlaybackState::RepeatMode mode);
    void onProgressSliderPressed();
    void onProgressSliderReleased();
    void onProgressSliderMoved(int value);
    void onVolumeSliderChanged(int value);
    void onMuteClicked();
    void onDeviceClicked();
    void onCoverArtReady(const QString& trackPath, const QPixmap& pixmap);
    void refreshTheme();

private:
    void setupUI();
    void updateTrackInfo();
    void updateSignalPath();
    void updatePlayButton();
    void updateShuffleButton();
    void updateRepeatButton();
    void updateVolumeIcon();
    void updateVolumeSliderStyle();
    QString formatTime(int seconds);

    // Left section
    QLabel* m_coverArtLabel;
    QLabel* m_trackTitleLabel;
    QLabel* m_subtitleLabel;       // "Artist · Album"
    QWidget* m_signalPathDot;
    QLabel* m_formatLabel;         // "FLAC 44.1kHz/24bit"
    QLabel* m_autoplayLabel;       // "Autoplay" indicator

    // Center controls
    StyledButton* m_shuffleBtn;
    StyledButton* m_prevBtn;
    StyledButton* m_playPauseBtn;
    StyledButton* m_nextBtn;
    StyledButton* m_repeatBtn;

    // Progress section
    QLabel* m_currentTimeLabel;
    StyledSlider* m_progressSlider;
    QLabel* m_totalTimeLabel;
    bool m_sliderPressed = false;

    // Right section
    StyledButton* m_muteBtn;
    StyledSlider* m_volumeSlider;
    StyledButton* m_deviceBtn;
    StyledButton* m_queueBtn;

    bool m_isMuted = false;
    bool m_queueVisible = false;
    QString m_currentTrackPath;
};
