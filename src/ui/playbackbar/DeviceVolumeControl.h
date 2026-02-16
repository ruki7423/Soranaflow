#pragma once
#include <QWidget>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledSlider.h"

class DeviceVolumeControl : public QWidget {
    Q_OBJECT
public:
    explicit DeviceVolumeControl(QWidget* parent = nullptr);

    void setVolume(int volume);

public slots:
    void refreshTheme();

signals:
    void volumeChanged(int value);
    void muteClicked();
    void deviceClicked();
    void queueToggled(bool visible);

private:
    void updateVolumeIcon();
    void updateVolumeSliderStyle();

    StyledButton* m_muteBtn = nullptr;
    StyledSlider* m_volumeSlider = nullptr;
    StyledButton* m_deviceBtn = nullptr;
    StyledButton* m_queueBtn = nullptr;

    bool m_isMuted = false;
    bool m_queueVisible = false;
    int m_volumeHideFill = -1;
    int m_volumeIconTier = -1;
};
