#include "PlaybackBar.h"
#include <QFont>
#include <QStyle>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMouseEvent>
#include <QMenu>
#include <QActionGroup>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include "../core/ThemeManager.h"
#include "../core/Settings.h"
#include "../core/MusicData.h"
#include "../core/audio/AudioEngine.h"
#include "../core/audio/AudioDeviceManager.h"
#include "../core/audio/MetadataReader.h"
#include "../core/CoverArtLoader.h"
#include "../apple/MusicKitPlayer.h"

// ── Forward declaration ───────────────────────────────────────────
static QIcon tintedSvgIcon(const QString& resourcePath, const QColor& color);

// ── Constructor ────────────────────────────────────────────────────
PlaybackBar::PlaybackBar(QWidget* parent)
    : QWidget(parent)
{
    setupUI();

    // Connect async cover art loader
    connect(CoverArtLoader::instance(), &CoverArtLoader::coverArtReady,
            this, &PlaybackBar::onCoverArtReady);
}

// ── Setup UI ───────────────────────────────────────────────────────
void PlaybackBar::setupUI()
{
    setObjectName("PlaybackBar");
    setFixedHeight(UISizes::playbackBarHeight);
    setAttribute(Qt::WA_OpaquePaintEvent);

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(16, 0, 16, 0);
    mainLayout->setSpacing(0);

    // ═══════════════════════════════════════════════════════════════
    //  LEFT: Roon-style (Album art + Title + Artist·Album + Signal)
    // ═══════════════════════════════════════════════════════════════
    auto* leftWidget = new QWidget();
    leftWidget->setFixedWidth(280);
    auto* leftLayout = new QHBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 4, 0, 8);
    leftLayout->setSpacing(12);
    leftLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Album art 56x56
    auto c = ThemeManager::instance()->colors();

    m_coverArtLabel = new QLabel();
    m_coverArtLabel->setFixedSize(56, 56);
    m_coverArtLabel->setAlignment(Qt::AlignCenter);
    m_coverArtLabel->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 4px;"
                        " color: %2; font-size: 22px;")
            .arg(c.backgroundTertiary, c.foregroundMuted));
    m_coverArtLabel->setText(QStringLiteral("\u266B"));
    leftLayout->addWidget(m_coverArtLabel);

    // Track info column
    auto* trackInfoWidget = new QWidget();
    auto* trackInfoLayout = new QVBoxLayout(trackInfoWidget);
    trackInfoLayout->setSpacing(2);
    trackInfoLayout->setContentsMargins(0, 0, 0, 0);

    m_trackTitleLabel = new QLabel(QStringLiteral("Not Playing"));
    m_trackTitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 500;")
            .arg(c.foreground));
    m_trackTitleLabel->setMaximumWidth(170);
    trackInfoLayout->addWidget(m_trackTitleLabel);

    m_subtitleLabel = new QLabel();
    m_subtitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;")
            .arg(c.foregroundMuted));
    m_subtitleLabel->setMaximumWidth(170);
    m_subtitleLabel->setCursor(Qt::PointingHandCursor);
    m_subtitleLabel->installEventFilter(this);
    trackInfoLayout->addWidget(m_subtitleLabel);

    // Signal path row: [●] FLAC 44.1kHz/24bit
    auto* signalRow = new QWidget();
    auto* signalLayout = new QHBoxLayout(signalRow);
    signalLayout->setContentsMargins(0, 2, 0, 0);
    signalLayout->setSpacing(5);

    m_signalPathDot = new QWidget();
    m_signalPathDot->setFixedSize(8, 8);
    m_signalPathDot->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 4px;").arg(c.foregroundMuted));
    m_signalPathDot->setVisible(false);
    signalLayout->addWidget(m_signalPathDot);

    m_formatLabel = new QLabel();
    m_formatLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px;")
            .arg(c.foregroundMuted));
    m_formatLabel->setVisible(false);
    signalLayout->addWidget(m_formatLabel);

    m_autoplayLabel = new QLabel(QStringLiteral("Autoplay"));
    {
        QColor accentColor(c.accent);
        accentColor.setAlphaF(0.7);
        m_autoplayLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 10px;")
                .arg(accentColor.name(QColor::HexArgb)));
    }
    m_autoplayLabel->setVisible(false);
    signalLayout->addWidget(m_autoplayLabel);

    signalLayout->addStretch();

    trackInfoLayout->addWidget(signalRow);
    leftLayout->addWidget(trackInfoWidget);
    leftLayout->addStretch();

    mainLayout->addWidget(leftWidget);

    // ═══════════════════════════════════════════════════════════════
    //  CENTER: Spotify-style controls + Progress bar
    // ═══════════════════════════════════════════════════════════════
    auto* centerWidget = new QWidget();
    auto* centerLayout = new QVBoxLayout(centerWidget);
    centerLayout->setContentsMargins(0, 0, 0, 2);
    centerLayout->setSpacing(2);

    // Transport controls row
    auto* transportLayout = new QHBoxLayout();
    transportLayout->setContentsMargins(0, 0, 0, 0);
    transportLayout->setSpacing(20);
    transportLayout->setAlignment(Qt::AlignCenter);

    const int ctrlBtnSize = UISizes::transportButtonSize;
    const int ctrlIconSize = UISizes::buttonIconSize;

    // Inline style applied to all transport buttons (same as refreshTheme)
    QString initTransportStyle = QStringLiteral(
        "QPushButton {"
        "  background: transparent;"
        "  border: none;"
        "  border-radius: %1px;"
        "  padding: 0px;"
        "  min-width: %2px; min-height: %2px;"
        "  max-width: %2px; max-height: %2px;"
        "}"
        "QPushButton:hover {"
        "  background: %3;"
        "}")
        .arg(QString::number(UISizes::transportButtonSize / 2),
             QString::number(UISizes::transportButtonSize),
             c.hover);

    m_shuffleBtn = new StyledButton("", "ghost");
    m_shuffleBtn->setObjectName(QStringLiteral("ShuffleButton"));
    m_shuffleBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/shuffle.svg"));
    m_shuffleBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_shuffleBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_shuffleBtn->setStyleSheet(initTransportStyle);

    m_prevBtn = new StyledButton("", "ghost");
    m_prevBtn->setObjectName(QStringLiteral("PrevButton"));
    m_prevBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/skip-back.svg"));
    m_prevBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_prevBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_prevBtn->setStyleSheet(initTransportStyle);

    m_playPauseBtn = new StyledButton("", "default");
    m_playPauseBtn->setObjectName(QStringLiteral("PlayPauseButton"));
    {
        // Icon must contrast with accent button bg → always white (foregroundInverse)
        m_playPauseBtn->setIcon(tintedSvgIcon(":/icons/play.svg", QColor(c.foregroundInverse)));
    }
    m_playPauseBtn->setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    m_playPauseBtn->setFixedSize(UISizes::playButtonSize, UISizes::playButtonSize);
    m_playPauseBtn->setMinimumSize(UISizes::playButtonSize, UISizes::playButtonSize);
    m_playPauseBtn->setMaximumSize(UISizes::playButtonSize, UISizes::playButtonSize);
    m_playPauseBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton#PlayPauseButton {"
            "  background-color: %1;"
            "  border-radius: %4px;"
            "  border: none;"
            "  padding: 0px;"
            "  min-width: %5px; min-height: %5px;"
            "  max-width: %5px; max-height: %5px;"
            "}"
            "QPushButton#PlayPauseButton:hover {"
            "  background-color: %2;"
            "}"
            "QPushButton#PlayPauseButton:pressed {"
            "  background-color: %3;"
            "}")
            .arg(c.accent, c.accentHover, c.accentPressed,
                 QString::number(UISizes::playButtonSize / 2),
                 QString::number(UISizes::playButtonSize)));

    m_nextBtn = new StyledButton("", "ghost");
    m_nextBtn->setObjectName(QStringLiteral("NextButton"));
    m_nextBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/skip-forward.svg"));
    m_nextBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_nextBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_nextBtn->setStyleSheet(initTransportStyle);

    m_repeatBtn = new StyledButton("", "ghost");
    m_repeatBtn->setObjectName(QStringLiteral("RepeatButton"));
    m_repeatBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/repeat.svg"));
    m_repeatBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_repeatBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_repeatBtn->setStyleSheet(initTransportStyle);

    transportLayout->addWidget(m_shuffleBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_prevBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_playPauseBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_nextBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_repeatBtn, 0, Qt::AlignVCenter);

    centerLayout->addLayout(transportLayout);

    // Progress bar row
    auto* progressLayout = new QHBoxLayout();
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);

    QFont monoFont("monospace", 10);
    monoFont.setStyleHint(QFont::Monospace);

    m_currentTimeLabel = new QLabel("0:00");
    m_currentTimeLabel->setFixedWidth(UISizes::thumbnailSize);
    m_currentTimeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_currentTimeLabel->setFont(monoFont);
    m_currentTimeLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;")
            .arg(c.foregroundMuted));

    m_progressSlider = new StyledSlider();
    m_progressSlider->setObjectName(QStringLiteral("ProgressSlider"));
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setValue(0);
    m_progressSlider->setStyleSheet(ThemeManager::instance()->sliderStyle(SliderVariant::Seek));

    m_totalTimeLabel = new QLabel("0:00");
    m_totalTimeLabel->setFixedWidth(UISizes::thumbnailSize);
    m_totalTimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_totalTimeLabel->setFont(monoFont);
    m_totalTimeLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;")
            .arg(c.foregroundMuted));

    // Allow slider to shrink/grow with window size
    m_progressSlider->setMinimumWidth(120);
    m_progressSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // No stretches around slider — it takes ALL available space
    progressLayout->addWidget(m_currentTimeLabel);
    progressLayout->addWidget(m_progressSlider, 1);  // stretch factor 1
    progressLayout->addWidget(m_totalTimeLabel);

    centerLayout->addLayout(progressLayout);

    // Connect progress slider signals
    connect(m_progressSlider, &QSlider::sliderPressed,
            this, &PlaybackBar::onProgressSliderPressed);
    connect(m_progressSlider, &QSlider::sliderReleased,
            this, &PlaybackBar::onProgressSliderReleased);
    connect(m_progressSlider, &QSlider::sliderMoved,
            this, &PlaybackBar::onProgressSliderMoved);

    // Center section expands to fill space between left info and right controls
    centerWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mainLayout->addWidget(centerWidget, 1);  // stretch factor 1 — takes remaining space

    // ═══════════════════════════════════════════════════════════════
    //  RIGHT: Volume + Device + Queue
    // ═══════════════════════════════════════════════════════════════
    auto* rightWidget = new QWidget();
    rightWidget->setFixedWidth(260);
    auto* rightLayout = new QHBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->setAlignment(Qt::AlignVCenter);

    const int rightBtnSize = 28;
    const int rightIconSize = 16;

    // Right button inline style (same as refreshTheme applies)
    QString initRightStyle = QStringLiteral(
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
    m_muteBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/volume-2.svg"));
    m_muteBtn->setIconSize(QSize(rightIconSize, rightIconSize));
    m_muteBtn->setFixedSize(rightBtnSize, rightBtnSize);
    m_muteBtn->setStyleSheet(initRightStyle);

    m_volumeSlider = new StyledSlider();
    m_volumeSlider->setObjectName(QStringLiteral("VolumeSlider"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(Settings::instance()->volume());
    m_volumeSlider->setFixedSize(UISizes::volumeSliderWidth, 24);

    m_deviceBtn = new StyledButton("", "ghost");
    m_deviceBtn->setObjectName(QStringLiteral("DeviceButton"));
    m_deviceBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/audio-output.svg"));
    m_deviceBtn->setIconSize(QSize(rightIconSize, rightIconSize));
    m_deviceBtn->setFixedSize(rightBtnSize, rightBtnSize);
    m_deviceBtn->setToolTip(QStringLiteral("Output Device"));
    m_deviceBtn->setStyleSheet(initRightStyle);

    m_queueBtn = new StyledButton("", "ghost");
    m_queueBtn->setObjectName(QStringLiteral("QueueButton"));
    m_queueBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/list-music.svg"));
    m_queueBtn->setIconSize(QSize(rightIconSize, rightIconSize));
    m_queueBtn->setFixedSize(rightBtnSize, rightBtnSize);
    m_queueBtn->setToolTip(QStringLiteral("Queue"));
    m_queueBtn->setStyleSheet(initRightStyle);

    rightLayout->addWidget(m_muteBtn, 0, Qt::AlignVCenter);
    rightLayout->addSpacing(2);
    rightLayout->addWidget(m_volumeSlider, 0, Qt::AlignVCenter);
    rightLayout->addSpacing(16);
    rightLayout->addWidget(m_deviceBtn, 0, Qt::AlignVCenter);
    rightLayout->addSpacing(16);
    rightLayout->addWidget(m_queueBtn, 0, Qt::AlignVCenter);

    mainLayout->addWidget(rightWidget);

    // ═══════════════════════════════════════════════════════════════
    //  Connect signals
    // ═══════════════════════════════════════════════════════════════
    auto* ps = PlaybackState::instance();

    connect(ps, &PlaybackState::playStateChanged,
            this, &PlaybackBar::onPlayStateChanged);
    connect(ps, &PlaybackState::trackChanged,
            this, &PlaybackBar::onTrackChanged);
    connect(ps, &PlaybackState::timeChanged,
            this, &PlaybackBar::onTimeChanged);
    connect(ps, &PlaybackState::volumeChanged,
            this, &PlaybackBar::onVolumeChanged);
    connect(ps, &PlaybackState::shuffleChanged,
            this, &PlaybackBar::onShuffleChanged);
    connect(ps, &PlaybackState::repeatChanged,
            this, &PlaybackBar::onRepeatChanged);

    connect(ps, &PlaybackState::autoplayTrackStarted, this, [this]() {
        m_autoplayLabel->setVisible(true);
    });
    connect(ps, &PlaybackState::trackChanged, this, [this](const Track&) {
        // Hide autoplay label on manual track selection
        if (!PlaybackState::instance()->isPlaying() || !m_autoplayLabel->isVisible())
            return;
        // The label stays visible for autoplay tracks; it gets hidden
        // when playback stops or a new non-autoplay queue is set
    });
    connect(ps, &PlaybackState::playStateChanged, this, [this](bool playing) {
        if (!playing)
            m_autoplayLabel->setVisible(false);
    });
    connect(ps, &PlaybackState::queueChanged, this, [this]() {
        // Hide autoplay label when user modifies queue manually
        m_autoplayLabel->setVisible(false);
    });

    connect(m_playPauseBtn, &QPushButton::clicked,
            ps, &PlaybackState::playPause);
    connect(m_nextBtn, &QPushButton::clicked,
            ps, &PlaybackState::next);
    connect(m_prevBtn, &QPushButton::clicked,
            ps, &PlaybackState::previous);
    connect(m_shuffleBtn, &QPushButton::clicked,
            ps, &PlaybackState::toggleShuffle);
    connect(m_repeatBtn, &QPushButton::clicked,
            ps, &PlaybackState::cycleRepeat);

    connect(m_volumeSlider, &QSlider::valueChanged,
            this, &PlaybackBar::onVolumeSliderChanged);
    connect(m_muteBtn, &QPushButton::clicked,
            this, &PlaybackBar::onMuteClicked);
    connect(m_deviceBtn, &QPushButton::clicked,
            this, &PlaybackBar::onDeviceClicked);

    connect(m_queueBtn, &QPushButton::clicked, this, [this]() {
        m_queueVisible = !m_queueVisible;
        emit queueToggled(m_queueVisible);
    });

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &PlaybackBar::refreshTheme);

    // Sync initial state from PlaybackState — signals may have been emitted
    // before PlaybackBar was constructed (e.g. startup playTrack in main.cpp)
    updatePlayButton();
    updateTrackInfo();
    updateShuffleButton();
    updateRepeatButton();
    updateVolumeIcon();
    updateVolumeSliderStyle();
}

// ── Slot: Device Clicked — show popup menu ─────────────────────────
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
            // Save persistent UID and name
            auto info = AudioDeviceManager::instance()->deviceById(deviceId);
            Settings::instance()->setOutputDeviceUID(info.uid);
            Settings::instance()->setOutputDeviceName(info.name);
            // Route Apple Music WebView audio to the new device
            MusicKitPlayer::instance()->updateOutputDevice();
        });
    }

    if (menu->isEmpty()) {
        menu->addAction(QStringLiteral("No Output Devices"))->setEnabled(false);
    }

    // Show menu above the button
    QPoint pos = m_deviceBtn->mapToGlobal(QPoint(0, 0));
    pos.setY(pos.y() - menu->sizeHint().height());
    menu->popup(pos);
}

// ── Slot: Play State Changed ───────────────────────────────────────
void PlaybackBar::onPlayStateChanged(bool playing)
{
    Q_UNUSED(playing);
    updatePlayButton();
}

// ── Slot: Track Changed ────────────────────────────────────────────
void PlaybackBar::onTrackChanged(const Track& track)
{
    Q_UNUSED(track);
    updateTrackInfo();
}

// ── Slot: Time Changed ─────────────────────────────────────────────
void PlaybackBar::onTimeChanged(int seconds)
{
    QString newTime = formatTime(seconds);
    if (m_currentTimeLabel->text() != newTime)
        m_currentTimeLabel->setText(newTime);

    if (!m_sliderPressed) {
        auto* ps = PlaybackState::instance();
        int duration = ps->currentTrack().duration;
        if (duration > 0) {
            int sliderValue = static_cast<int>(
                (static_cast<double>(seconds) / duration) * 1000);
            m_progressSlider->blockSignals(true);
            m_progressSlider->setValue(sliderValue);
            m_progressSlider->blockSignals(false);
        }
    }
}

// ── Slot: Volume Changed ───────────────────────────────────────────
void PlaybackBar::onVolumeChanged(int volume)
{
    if (!m_isMuted) {
        m_volumeSlider->blockSignals(true);
        m_volumeSlider->setValue(volume);
        m_volumeSlider->blockSignals(false);
    }
    updateVolumeIcon();
}

// ── Slot: Shuffle Changed ──────────────────────────────────────────
void PlaybackBar::onShuffleChanged(bool enabled)
{
    Q_UNUSED(enabled);
    updateShuffleButton();
}

// ── Slot: Repeat Changed ───────────────────────────────────────────
void PlaybackBar::onRepeatChanged(PlaybackState::RepeatMode mode)
{
    Q_UNUSED(mode);
    updateRepeatButton();
}

// ── Slot: Progress Slider Pressed ──────────────────────────────────
void PlaybackBar::onProgressSliderPressed()
{
    m_sliderPressed = true;
}

// ── Slot: Progress Slider Released ─────────────────────────────────
void PlaybackBar::onProgressSliderReleased()
{
    auto* ps = PlaybackState::instance();
    int duration = ps->currentTrack().duration;
    if (duration > 0) {
        int seekPos = static_cast<int>(
            (static_cast<double>(m_progressSlider->value()) / 1000.0) * duration);
        ps->seek(seekPos);
    }
    m_sliderPressed = false;
}

// ── Slot: Progress Slider Moved ────────────────────────────────────
void PlaybackBar::onProgressSliderMoved(int value)
{
    auto* ps = PlaybackState::instance();
    int duration = ps->currentTrack().duration;
    if (duration > 0) {
        int displayTime = static_cast<int>(
            (static_cast<double>(value) / 1000.0) * duration);
        m_currentTimeLabel->setText(formatTime(displayTime));
    }
}

// ── Slot: Volume Slider Changed ────────────────────────────────────
void PlaybackBar::onVolumeSliderChanged(int value)
{
    if (m_isMuted) {
        m_isMuted = false;
        updateVolumeIcon();
    }
    updateVolumeSliderStyle();
    PlaybackState::instance()->setVolume(value);
}

// ── Slot: Mute Clicked ────────────────────────────────────────────
void PlaybackBar::onMuteClicked()
{
    m_isMuted = !m_isMuted;

    if (m_isMuted) {
        // Mute: silence audio but keep slider where it is
        AudioEngine::instance()->setVolume(0.0f);
    } else {
        // Unmute: restore volume from current slider position
        float vol = m_volumeSlider->value() / 100.0f;
        AudioEngine::instance()->setVolume(vol);
    }
    // Only change the icon — slider appearance stays as-is
    updateVolumeIcon();
}

// ── Helper: Format Time ────────────────────────────────────────────
// Forward declaration of helper used in updatePlayButton
static QIcon tintedSvgIcon(const QString& resourcePath, const QColor& color);

QString PlaybackBar::formatTime(int seconds)
{
    int mins = seconds / 60;
    int secs = seconds % 60;
    return QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
}

// ── Helper: Update Play Button ─────────────────────────────────────
void PlaybackBar::updatePlayButton()
{
    auto* ps = PlaybackState::instance();
    auto c = ThemeManager::instance()->colors();
    // Icon uses foregroundInverse to contrast with accent-colored button bg
    QColor iconColor(c.foregroundInverse);
    if (ps->isPlaying()) {
        m_playPauseBtn->setIcon(tintedSvgIcon(":/icons/pause.svg", iconColor));
    } else {
        m_playPauseBtn->setIcon(tintedSvgIcon(":/icons/play.svg", iconColor));
    }
}

// ── Helper: tint an SVG icon with a specific color ─────────────────
static QIcon tintedSvgIcon(const QString& resourcePath, const QColor& color)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon(resourcePath);

    QString svgContent = QString::fromUtf8(file.readAll());
    file.close();

    svgContent.replace(QStringLiteral("currentColor"), color.name());

    QSvgRenderer renderer(svgContent.toUtf8());
    if (!renderer.isValid())
        return QIcon(resourcePath);

    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    painter.end();

    return QIcon(pixmap);
}

// ── Helper: Update Shuffle Button ──────────────────────────────────
void PlaybackBar::updateShuffleButton()
{
    auto* ps = PlaybackState::instance();
    bool active = ps->shuffleEnabled();

    if (active) {
        m_shuffleBtn->setIcon(tintedSvgIcon(":/icons/shuffle.svg", QColor(ThemeManager::instance()->colors().accent)));
    } else {
        m_shuffleBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/shuffle.svg"));
    }
}

// ── Helper: Update Repeat Button ───────────────────────────────────
void PlaybackBar::updateRepeatButton()
{
    auto* ps = PlaybackState::instance();
    PlaybackState::RepeatMode mode = ps->repeatMode();
    QColor green(ThemeManager::instance()->colors().accent);

    switch (mode) {
    case PlaybackState::Off:
        m_repeatBtn->setIcon(ThemeManager::instance()->themedIcon(":/icons/repeat.svg"));
        break;
    case PlaybackState::All:
        m_repeatBtn->setIcon(tintedSvgIcon(":/icons/repeat.svg", green));
        break;
    case PlaybackState::One:
        m_repeatBtn->setIcon(tintedSvgIcon(":/icons/repeat-1.svg", green));
        break;
    }
}

// ── Helper: Update Volume Icon ─────────────────────────────────────
void PlaybackBar::updateVolumeIcon()
{
    auto* tm = ThemeManager::instance();

    if (m_isMuted) {
        m_muteBtn->setIcon(tm->themedIcon(":/icons/volume-x.svg"));
        return;
    }

    int vol = m_volumeSlider->value();
    if (vol == 0) {
        m_muteBtn->setIcon(tm->themedIcon(":/icons/volume-x.svg"));
    } else if (vol < 50) {
        m_muteBtn->setIcon(tm->themedIcon(":/icons/volume-1.svg"));
    } else {
        m_muteBtn->setIcon(tm->themedIcon(":/icons/volume-2.svg"));
    }
}

// ── Helper: Update Volume Slider Style (0 = no fill) ────────────────
void PlaybackBar::updateVolumeSliderStyle()
{
    auto c = ThemeManager::instance()->colors();
    bool hideFill = m_volumeSlider->value() == 0;
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

// ── Helper: Update Signal Path indicator ───────────────────────────
void PlaybackBar::updateSignalPath()
{
    auto* ps = PlaybackState::instance();
    Track track = ps->currentTrack();

    if (track.id.isEmpty()) {
        m_signalPathDot->setVisible(false);
        m_formatLabel->setVisible(false);
        return;
    }

    // Apple Music streaming track: empty filePath with a valid id
    if (track.filePath.isEmpty() && !track.id.isEmpty()) {
        m_signalPathDot->setStyleSheet(QStringLiteral(
            "background: #FC3C44; border-radius: 4px;"));
        m_signalPathDot->setVisible(true);
        m_formatLabel->setText(QStringLiteral("Apple Music"));
        m_formatLabel->setVisible(true);
        return;
    }

    // Color from existing utility (purple=DSD, gold=hi-res, green=lossless, gray=lossy)
    QColor dotColor = getFormatColor(track.format);

    m_signalPathDot->setStyleSheet(QString(
        "background: %1; border-radius: 4px;"
    ).arg(dotColor.name()));
    m_signalPathDot->setVisible(true);

    // Build format text: "FLAC 44.1kHz/24bit"
    QString formatText = getFormatLabel(track.format);

    if (!track.sampleRate.isEmpty())
        formatText += QStringLiteral(" ") + track.sampleRate;
    if (!track.bitDepth.isEmpty())
        formatText += QStringLiteral("/") + track.bitDepth;

    m_formatLabel->setText(formatText);
    m_formatLabel->setVisible(true);
}

// ── Helper: find cover art for a track ─────────────────────────────
static QPixmap findPlaybackCoverArt(const Track& track, int size)
{
    QPixmap pix;

    if (!track.coverUrl.isEmpty()) {
        QString loadPath = track.coverUrl;
        if (loadPath.startsWith(QStringLiteral("qrc:")))
            loadPath = loadPath.mid(3);
        if (QFile::exists(loadPath)) {
            pix.load(loadPath);
            if (!pix.isNull()) return pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        }
    }

    if (!track.filePath.isEmpty()) {
        QString folder = QFileInfo(track.filePath).absolutePath();
        static const QStringList names = {
            QStringLiteral("cover.jpg"),   QStringLiteral("cover.png"),
            QStringLiteral("Cover.jpg"),   QStringLiteral("Cover.png"),
            QStringLiteral("folder.jpg"),  QStringLiteral("folder.png"),
            QStringLiteral("Folder.jpg"),  QStringLiteral("Folder.png"),
            QStringLiteral("front.jpg"),   QStringLiteral("front.png"),
            QStringLiteral("Front.jpg"),   QStringLiteral("Front.png"),
            QStringLiteral("album.jpg"),   QStringLiteral("album.png"),
            QStringLiteral("Album.jpg"),   QStringLiteral("Album.png"),
            QStringLiteral("artwork.jpg"), QStringLiteral("artwork.png"),
            QStringLiteral("Artwork.jpg"), QStringLiteral("Artwork.png"),
        };
        for (const QString& n : names) {
            QString path = folder + QStringLiteral("/") + n;
            if (QFile::exists(path)) {
                pix.load(path);
                if (!pix.isNull()) return pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            }
        }
    }

    if (!track.filePath.isEmpty()) {
        pix = MetadataReader::extractCoverArt(track.filePath);
        if (!pix.isNull()) return pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }

    if (!track.filePath.isEmpty()) {
        QDir dir(QFileInfo(track.filePath).absolutePath());
        QStringList images = dir.entryList(
            {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"), QStringLiteral("*.webp")},
            QDir::Files, QDir::Name);
        if (!images.isEmpty()) {
            pix.load(dir.filePath(images.first()));
            if (!pix.isNull()) return pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        }
    }

    return pix;
}

// ── Helper: Update Track Info ──────────────────────────────────────
void PlaybackBar::updateTrackInfo()
{
    auto* ps = PlaybackState::instance();
    Track track = ps->currentTrack();

    auto c = ThemeManager::instance()->colors();
    if (track.id.isEmpty()) {
        m_trackTitleLabel->setText(QStringLiteral("Not Playing"));
        m_subtitleLabel->setText(QString());
        m_coverArtLabel->setPixmap(QPixmap());
        m_coverArtLabel->setText(QStringLiteral("\u266B"));
        m_coverArtLabel->setStyleSheet(
                QStringLiteral("background: %1; border-radius: 4px;"
                    " color: %2; font-size: 22px;")
                    .arg(c.backgroundTertiary, c.foregroundMuted));
    } else {
        m_trackTitleLabel->setText(track.title);

        // "Artist · Album"
        QString subtitle;
        if (!track.artist.isEmpty()) {
            subtitle = track.artist;
            if (!track.album.isEmpty())
                subtitle += QStringLiteral(" \u00B7 ") + track.album;
        } else if (!track.album.isEmpty()) {
            subtitle = track.album;
        }
        m_subtitleLabel->setText(subtitle);

        // Cover art — show placeholder immediately, load async
        m_currentTrackPath = track.filePath;
        {
            QString fallback = track.album.isEmpty()
                ? QStringLiteral("\u266B")
                : track.album.left(1).toUpper();
            m_coverArtLabel->setPixmap(QPixmap());
            m_coverArtLabel->setText(fallback);
            m_coverArtLabel->setStyleSheet(
                QStringLiteral("background: %1; border-radius: 4px;"
                    " color: %2; font-size: 22px;")
                    .arg(c.backgroundTertiary, c.foregroundMuted));
        }
        CoverArtLoader::instance()->requestCoverArt(track.filePath, track.coverUrl, 56);
    }

    updateSignalPath();

    m_totalTimeLabel->setText(formatTime(track.duration));
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setValue(0);
    m_currentTimeLabel->setText("0:00");
}

// ── Slot: Async cover art arrived ──────────────────────────────────
void PlaybackBar::onCoverArtReady(const QString& trackPath, const QPixmap& pixmap)
{
    // Only apply if this is still the current track
    if (trackPath != m_currentTrackPath)
        return;

    const int artSize = 56;
    if (!pixmap.isNull()) {
        QPixmap coverPix = pixmap;
        if (coverPix.width() > artSize || coverPix.height() > artSize) {
            int x = (coverPix.width() - artSize) / 2;
            int y = (coverPix.height() - artSize) / 2;
            coverPix = coverPix.copy(x, y, artSize, artSize);
        }
        QPixmap rounded(artSize, artSize);
        rounded.fill(Qt::transparent);
        QPainter painter(&rounded);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(0, 0, artSize, artSize, 4, 4);
        painter.setClipPath(path);
        painter.drawPixmap(0, 0, coverPix);
        painter.end();
        m_coverArtLabel->setPixmap(rounded);
        m_coverArtLabel->setStyleSheet(QStringLiteral("border: none;"));
    }
}

// ── Slot: Refresh Theme ────────────────────────────────────────────
void PlaybackBar::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();
    QString mutedStyle = QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted);

    m_currentTimeLabel->setStyleSheet(mutedStyle);
    m_totalTimeLabel->setStyleSheet(mutedStyle);
    m_trackTitleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 500;")
            .arg(c.foreground));
    m_subtitleLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 11px; }"
                       "QLabel:hover { color: %2; }")
            .arg(c.foregroundMuted, c.foreground));

    m_prevBtn->setIcon(tm->themedIcon(":/icons/skip-back.svg"));
    m_nextBtn->setIcon(tm->themedIcon(":/icons/skip-forward.svg"));
    m_queueBtn->setIcon(tm->themedIcon(":/icons/list-music.svg"));
    m_deviceBtn->setIcon(tm->themedIcon(":/icons/audio-output.svg"));

    updatePlayButton();
    updateShuffleButton();
    updateRepeatButton();
    updateVolumeIcon();
    updateTrackInfo();

    // Transport buttons — transparent background, subtle hover
    {
        const int tSize = UISizes::transportButtonSize;
        QString transportStyle = QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  border-radius: %1px;"
            "  padding: 0px;"
            "  min-width: %2px; min-height: %2px;"
            "  max-width: %2px; max-height: %2px;"
            "}"
            "QPushButton:hover {"
            "  background: %3;"
            "}")
            .arg(QString::number(UISizes::transportButtonSize / 2),
                 QString::number(UISizes::transportButtonSize),
                 c.hover);
        m_shuffleBtn->setFixedSize(tSize, tSize);
        m_shuffleBtn->setStyleSheet(transportStyle);
        m_prevBtn->setFixedSize(tSize, tSize);
        m_prevBtn->setStyleSheet(transportStyle);
        m_nextBtn->setFixedSize(tSize, tSize);
        m_nextBtn->setStyleSheet(transportStyle);
        m_repeatBtn->setFixedSize(tSize, tSize);
        m_repeatBtn->setStyleSheet(transportStyle);

        // Right section buttons (volume, device, queue) — circular hover
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
    }

    // Play/pause button — always circular (40x40 → radius 20px)
    {
        m_playPauseBtn->setFixedSize(UISizes::playButtonSize, UISizes::playButtonSize);
        m_playPauseBtn->setMinimumSize(UISizes::playButtonSize, UISizes::playButtonSize);
        m_playPauseBtn->setMaximumSize(UISizes::playButtonSize, UISizes::playButtonSize);
        m_playPauseBtn->setStyleSheet(
            QStringLiteral(
                "QPushButton#PlayPauseButton {"
                "  background-color: %1;"
                "  border-radius: %4px;"
                "  border: none;"
                "  padding: 0px;"
                "  min-width: %5px;"
                "  min-height: %5px;"
                "  max-width: %5px;"
                "  max-height: %5px;"
                "}"
                "QPushButton#PlayPauseButton:hover {"
                "  background-color: %2;"
                "}"
                "QPushButton#PlayPauseButton:pressed {"
                "  background-color: %3;"
                "}")
                .arg(c.accent, c.accentHover, c.accentPressed,
                     QString::number(UISizes::playButtonSize / 2),
                     QString::number(UISizes::playButtonSize)));
    }

    // Update progress slider
    m_progressSlider->setStyleSheet(tm->sliderStyle(SliderVariant::Seek));

    // Update format label
    m_formatLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px;").arg(c.foregroundMuted));

    // Update autoplay label
    {
        QColor accentColor(c.accent);
        accentColor.setAlphaF(0.7);
        m_autoplayLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 10px;")
                .arg(accentColor.name(QColor::HexArgb)));
    }

    updateVolumeSliderStyle();
}

// ── eventFilter — artist click on subtitle label ────────────────────
bool PlaybackBar::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_subtitleLabel && event->type() == QEvent::MouseButtonRelease) {
        const Track current = PlaybackState::instance()->currentTrack();
        if (current.artist.isEmpty()) return true;

        // Try artistId from track first
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
        return true;
    }
    return QWidget::eventFilter(obj, event);
}
