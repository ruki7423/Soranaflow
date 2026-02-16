#include "DeviceVolumeControl.h"
#include "../../core/ThemeManager.h"
#include "../../core/Settings.h"
#include <QHBoxLayout>

DeviceVolumeControl::DeviceVolumeControl(QWidget* parent)
    : QWidget(parent)
{
    auto c = ThemeManager::instance()->colors();

    setMinimumWidth(180);
    setMaximumWidth(260);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->setAlignment(Qt::AlignVCenter);

    const int rightBtnSize = 28;
    const int rightIconSize = 16;

    QString rightStyle = QStringLiteral(
        "QPushButton {"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: 14px;"
        "  padding: 0px;"
        "  min-width: 28px; min-height: 28px;"
        "  max-width: 28px; max-height: 28px;"
        "}"
        "QPushButton:hover {"
        "  background: %1;"
        "}"
        "QPushButton::menu-indicator { image: none; width: 0; }")
        .arg(c.hover);

    m_muteBtn = new StyledButton("", "ghost");
    m_muteBtn->setObjectName(QStringLiteral("MuteButton"));
    m_muteBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/volume-2.svg"));
    m_muteBtn->setIconSize(QSize(rightIconSize, rightIconSize));
    m_muteBtn->setFixedSize(rightBtnSize, rightBtnSize);
    m_muteBtn->setStyleSheet(rightStyle);

    m_volumeSlider = new StyledSlider();
    m_volumeSlider->setObjectName(QStringLiteral("VolumeSlider"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(Settings::instance()->volume());
    m_volumeSlider->setFixedSize(UISizes::volumeSliderWidth, 24);

    m_deviceBtn = new StyledButton("", "ghost");
    m_deviceBtn->setObjectName(QStringLiteral("DeviceButton"));
    m_deviceBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/audio-output.svg"));
    m_deviceBtn->setIconSize(QSize(rightIconSize, rightIconSize));
    m_deviceBtn->setFixedSize(rightBtnSize, rightBtnSize);
    m_deviceBtn->setToolTip(QStringLiteral("Output Device"));
    m_deviceBtn->setStyleSheet(rightStyle);

    m_queueBtn = new StyledButton("", "ghost");
    m_queueBtn->setObjectName(QStringLiteral("QueueButton"));
    m_queueBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/list-music.svg"));
    m_queueBtn->setIconSize(QSize(rightIconSize, rightIconSize));
    m_queueBtn->setFixedSize(rightBtnSize, rightBtnSize);
    m_queueBtn->setToolTip(QStringLiteral("Queue"));
    m_queueBtn->setStyleSheet(rightStyle);

    layout->addWidget(m_muteBtn, 0, Qt::AlignVCenter);
    layout->addSpacing(2);
    layout->addWidget(m_volumeSlider, 0, Qt::AlignVCenter);
    layout->addSpacing(16);
    layout->addWidget(m_deviceBtn, 0, Qt::AlignVCenter);
    layout->addSpacing(16);
    layout->addWidget(m_queueBtn, 0, Qt::AlignVCenter);

    // Volume slider → emit signal
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_isMuted) {
            m_isMuted = false;
            updateVolumeIcon();
        }
        updateVolumeSliderStyle();
        emit volumeChanged(value);
    });

    // Mute button
    connect(m_muteBtn, &QPushButton::clicked, this, [this]() {
        m_isMuted = !m_isMuted;
        updateVolumeIcon();
        emit muteClicked();
    });

    // Device + Queue → emit signals
    connect(m_deviceBtn, &QPushButton::clicked, this, &DeviceVolumeControl::deviceClicked);
    connect(m_queueBtn, &QPushButton::clicked, this, [this]() {
        m_queueVisible = !m_queueVisible;
        emit queueToggled(m_queueVisible);
    });

    updateVolumeIcon();
    updateVolumeSliderStyle();
}

void DeviceVolumeControl::setVolume(int volume)
{
    if (!m_isMuted) {
        m_volumeSlider->blockSignals(true);
        m_volumeSlider->setValue(volume);
        m_volumeSlider->blockSignals(false);
    }
    updateVolumeIcon();
}

void DeviceVolumeControl::refreshTheme()
{
    auto c = ThemeManager::instance()->colors();
    auto* tm = ThemeManager::instance();

    const int rSize = 28;
    QString rightBtnStyle = QStringLiteral(
        "QPushButton {"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: 14px;"
        "  padding: 0px;"
        "  min-width: 28px; min-height: 28px;"
        "  max-width: 28px; max-height: 28px;"
        "}"
        "QPushButton:hover {"
        "  background: %1;"
        "}"
        "QPushButton::menu-indicator { image: none; width: 0; }")
        .arg(c.hover);

    m_muteBtn->setFixedSize(rSize, rSize);
    m_muteBtn->setStyleSheet(rightBtnStyle);
    m_deviceBtn->setFixedSize(rSize, rSize);
    m_deviceBtn->setStyleSheet(rightBtnStyle);
    m_queueBtn->setFixedSize(rSize, rSize);
    m_queueBtn->setStyleSheet(rightBtnStyle);

    m_queueBtn->setIcon(tm->cachedIcon(":/icons/list-music.svg"));
    m_deviceBtn->setIcon(tm->cachedIcon(":/icons/audio-output.svg"));

    m_volumeHideFill = -1;
    m_volumeIconTier = -1;
    updateVolumeSliderStyle();
    updateVolumeIcon();
}

void DeviceVolumeControl::updateVolumeIcon()
{
    int tier;
    if (m_isMuted) {
        tier = 0;
    } else {
        int vol = m_volumeSlider->value();
        tier = (vol == 0) ? 0 : (vol < 50) ? 1 : 2;
    }

    if (tier == m_volumeIconTier)
        return;
    m_volumeIconTier = tier;

    auto* tm = ThemeManager::instance();
    switch (tier) {
    case 0:  m_muteBtn->setIcon(tm->cachedIcon(":/icons/volume-x.svg")); break;
    case 1:  m_muteBtn->setIcon(tm->cachedIcon(":/icons/volume-1.svg")); break;
    default: m_muteBtn->setIcon(tm->cachedIcon(":/icons/volume-2.svg")); break;
    }
}

void DeviceVolumeControl::updateVolumeSliderStyle()
{
    int hideFill = m_volumeSlider->value() == 0 ? 1 : 0;
    if (hideFill == m_volumeHideFill)
        return;
    m_volumeHideFill = hideFill;

    auto c = ThemeManager::instance()->colors();
    QString subPageColor = hideFill ? c.volumeTrack : c.volumeFill;
    QString subPageHoverColor = hideFill ? c.volumeTrack : c.accent;

    m_volumeSlider->setStyleSheet(
        QStringLiteral(
            "QSlider::groove:horizontal {"
            "  background: %1;"
            "  height: 4px;"
            "  border-radius: 2px;"
            "  margin: 0px;"
            "}"
            "QSlider::sub-page:horizontal {"
            "  background: %2;"
            "  height: 4px;"
            "  border-radius: 2px;"
            "}"
            "QSlider::sub-page:horizontal:hover {"
            "  background: %3;"
            "}"
            "QSlider::handle:horizontal {"
            "  background: %4;"
            "  width: 10px;"
            "  height: 10px;"
            "  margin: -3px 0;"
            "  border-radius: 5px;"
            "}"
            "QSlider::handle:horizontal:!hover {"
            "  background: transparent;"
            "  width: 0px;"
            "  margin: 0px;"
            "}"
        ).arg(c.volumeTrack, subPageColor, subPageHoverColor, c.volumeFill));
}
