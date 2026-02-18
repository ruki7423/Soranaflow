#pragma once
#include <QWidget>
#include <QLabel>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledSlider.h"
#include "../../core/PlaybackState.h"

class TransportControls : public QWidget {
    Q_OBJECT
public:
    explicit TransportControls(QWidget* parent = nullptr);

    void setPlaying(bool playing);
    void setTime(int seconds, int duration);
    void setShuffleEnabled(bool enabled);
    void setRepeatMode(PlaybackState::RepeatMode mode);
    void resetProgress(int duration);
    void showTemporaryMessage(const QString& msg);

public slots:
    void refreshTheme();

signals:
    void playPauseClicked();
    void nextClicked();
    void previousClicked();
    void shuffleClicked();
    void repeatClicked();
    void seekRequested(int seconds);

private:
    void updatePlayIcon();
    void updateShuffleIcon();
    void updateRepeatIcon();
    QString formatTime(int seconds);

    StyledButton* m_shuffleBtn = nullptr;
    StyledButton* m_prevBtn = nullptr;
    StyledButton* m_playPauseBtn = nullptr;
    StyledButton* m_nextBtn = nullptr;
    StyledButton* m_repeatBtn = nullptr;
    QLabel* m_currentTimeLabel = nullptr;
    StyledSlider* m_progressSlider = nullptr;
    QLabel* m_totalTimeLabel = nullptr;

    bool m_sliderPressed = false;
    bool m_isPlaying = false;
    bool m_shuffleActive = false;
    PlaybackState::RepeatMode m_repeatMode = PlaybackState::Off;
    int m_currentDuration = 0;
};
