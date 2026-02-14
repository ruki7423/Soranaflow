#pragma once

#include <QWidget>
#include <QLabel>

class StyledButton;

class AppleMusicSettingsTab : public QWidget
{
    Q_OBJECT

public:
    explicit AppleMusicSettingsTab(QWidget* parent = nullptr);

private:
    StyledButton* m_appleMusicConnectBtn = nullptr;
    QLabel* m_appleMusicStatusLabel = nullptr;
    QLabel* m_appleMusicSubLabel = nullptr;
};
