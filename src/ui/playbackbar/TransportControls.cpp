#include "TransportControls.h"
#include "../../core/ThemeManager.h"
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QFont>
#include <QFile>
#include <QPainter>
#include <QSvgRenderer>

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

TransportControls::TransportControls(QWidget* parent)
    : QWidget(parent)
{
    auto c = ThemeManager::instance()->colors();

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 2);
    mainLayout->setSpacing(2);

    // Transport controls row
    auto* transportLayout = new QHBoxLayout();
    transportLayout->setContentsMargins(0, 0, 0, 0);
    transportLayout->setSpacing(20);
    transportLayout->setAlignment(Qt::AlignCenter);

    const int ctrlBtnSize = UISizes::transportButtonSize;
    const int ctrlIconSize = UISizes::buttonIconSize;

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
        .arg(QString::number(ctrlBtnSize / 2),
             QString::number(ctrlBtnSize),
             c.hover);

    m_shuffleBtn = new StyledButton("", "ghost");
    m_shuffleBtn->setObjectName(QStringLiteral("ShuffleButton"));
    m_shuffleBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/shuffle.svg"));
    m_shuffleBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_shuffleBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_shuffleBtn->setStyleSheet(transportStyle);

    m_prevBtn = new StyledButton("", "ghost");
    m_prevBtn->setObjectName(QStringLiteral("PrevButton"));
    m_prevBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/skip-back.svg"));
    m_prevBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_prevBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_prevBtn->setStyleSheet(transportStyle);

    m_playPauseBtn = new StyledButton("", "default");
    m_playPauseBtn->setObjectName(QStringLiteral("PlayPauseButton"));
    m_playPauseBtn->setIcon(tintedSvgIcon(":/icons/play.svg", QColor(c.foregroundInverse)));
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
    m_nextBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/skip-forward.svg"));
    m_nextBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_nextBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_nextBtn->setStyleSheet(transportStyle);

    m_repeatBtn = new StyledButton("", "ghost");
    m_repeatBtn->setObjectName(QStringLiteral("RepeatButton"));
    m_repeatBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/repeat.svg"));
    m_repeatBtn->setIconSize(QSize(ctrlIconSize, ctrlIconSize));
    m_repeatBtn->setFixedSize(ctrlBtnSize, ctrlBtnSize);
    m_repeatBtn->setStyleSheet(transportStyle);

    transportLayout->addWidget(m_shuffleBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_prevBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_playPauseBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_nextBtn, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_repeatBtn, 0, Qt::AlignVCenter);
    mainLayout->addLayout(transportLayout);

    // Progress bar row
    auto* progressLayout = new QHBoxLayout();
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);

    QFont monoFont("Menlo", 10);
    monoFont.setStyleHint(QFont::Monospace);

    m_currentTimeLabel = new QLabel("0:00");
    m_currentTimeLabel->setFixedWidth(UISizes::thumbnailSize);
    m_currentTimeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_currentTimeLabel->setFont(monoFont);
    m_currentTimeLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted));

    m_progressSlider = new StyledSlider();
    m_progressSlider->setObjectName(QStringLiteral("ProgressSlider"));
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setValue(0);
    m_progressSlider->setStyleSheet(ThemeManager::instance()->sliderStyle(SliderVariant::Seek));
    m_progressSlider->setMinimumWidth(120);
    m_progressSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_totalTimeLabel = new QLabel("0:00");
    m_totalTimeLabel->setFixedWidth(UISizes::thumbnailSize);
    m_totalTimeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_totalTimeLabel->setFont(monoFont);
    m_totalTimeLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted));

    progressLayout->addWidget(m_currentTimeLabel);
    progressLayout->addWidget(m_progressSlider, 1);
    progressLayout->addWidget(m_totalTimeLabel);
    mainLayout->addLayout(progressLayout);

    // Internal slider signals
    connect(m_progressSlider, &QSlider::sliderPressed, this, [this]() {
        m_sliderPressed = true;
    });
    connect(m_progressSlider, &QSlider::sliderReleased, this, [this]() {
        if (m_currentDuration > 0) {
            int seekPos = static_cast<int>(
                (static_cast<double>(m_progressSlider->value()) / 1000.0) * m_currentDuration);
            emit seekRequested(seekPos);
        }
        m_sliderPressed = false;
    });
    connect(m_progressSlider, &QSlider::sliderMoved, this, [this](int value) {
        if (m_currentDuration > 0) {
            int displayTime = static_cast<int>(
                (static_cast<double>(value) / 1000.0) * m_currentDuration);
            m_currentTimeLabel->setText(formatTime(displayTime));
        }
    });

    // Button signals → emit to coordinator
    connect(m_playPauseBtn, &QPushButton::clicked, this, &TransportControls::playPauseClicked);
    connect(m_nextBtn, &QPushButton::clicked, this, &TransportControls::nextClicked);
    connect(m_prevBtn, &QPushButton::clicked, this, &TransportControls::previousClicked);
    connect(m_shuffleBtn, &QPushButton::clicked, this, &TransportControls::shuffleClicked);
    connect(m_repeatBtn, &QPushButton::clicked, this, &TransportControls::repeatClicked);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void TransportControls::setPlaying(bool playing)
{
    m_isPlaying = playing;
    updatePlayIcon();
}

void TransportControls::setTime(int seconds, int duration)
{
    m_currentDuration = duration;

    QString newTime = formatTime(seconds);
    if (m_currentTimeLabel->text() != newTime)
        m_currentTimeLabel->setText(newTime);

    if (!m_sliderPressed && duration > 0) {
        int sliderValue = static_cast<int>(
            (static_cast<double>(seconds) / duration) * 1000);
        m_progressSlider->blockSignals(true);
        m_progressSlider->setValue(sliderValue);
        m_progressSlider->blockSignals(false);
    }
}

void TransportControls::setShuffleEnabled(bool enabled)
{
    m_shuffleActive = enabled;
    updateShuffleIcon();
}

void TransportControls::setRepeatMode(PlaybackState::RepeatMode mode)
{
    m_repeatMode = mode;
    updateRepeatIcon();
}

void TransportControls::resetProgress(int duration)
{
    m_currentDuration = duration;
    m_totalTimeLabel->setText(formatTime(duration));
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setValue(0);
    m_currentTimeLabel->setText("0:00");
}

void TransportControls::showTemporaryMessage(const QString& msg)
{
    // Briefly show message in the total time label (3 seconds), then restore
    QString saved = m_totalTimeLabel->text();
    m_totalTimeLabel->setText(msg);
    QTimer::singleShot(3000, this, [this, saved]() {
        m_totalTimeLabel->setText(saved);
    });
}

void TransportControls::refreshTheme()
{
    auto* tm = ThemeManager::instance();
    auto c = tm->colors();
    QString mutedStyle = QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted);

    m_currentTimeLabel->setStyleSheet(mutedStyle);
    m_totalTimeLabel->setStyleSheet(mutedStyle);

    m_prevBtn->setIcon(tm->cachedIcon(":/icons/skip-back.svg"));
    m_nextBtn->setIcon(tm->cachedIcon(":/icons/skip-forward.svg"));

    updatePlayIcon();
    updateShuffleIcon();
    updateRepeatIcon();

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
        .arg(QString::number(tSize / 2),
             QString::number(tSize),
             c.hover);
    m_shuffleBtn->setFixedSize(tSize, tSize);
    m_shuffleBtn->setStyleSheet(transportStyle);
    m_prevBtn->setFixedSize(tSize, tSize);
    m_prevBtn->setStyleSheet(transportStyle);
    m_nextBtn->setFixedSize(tSize, tSize);
    m_nextBtn->setStyleSheet(transportStyle);
    m_repeatBtn->setFixedSize(tSize, tSize);
    m_repeatBtn->setStyleSheet(transportStyle);

    // Play/pause button
    m_playPauseBtn->setFixedSize(UISizes::playButtonSize, UISizes::playButtonSize);
    m_playPauseBtn->setMinimumSize(UISizes::playButtonSize, UISizes::playButtonSize);
    m_playPauseBtn->setMaximumSize(UISizes::playButtonSize, UISizes::playButtonSize);
    m_playPauseBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton#PlayPauseButton {"
            "  background-color: %1;"
            "  border-radius: %4px;"
            "  border: none; padding: 0px;"
            "  min-width: %5px; min-height: %5px;"
            "  max-width: %5px; max-height: %5px;"
            "}"
            "QPushButton#PlayPauseButton:hover { background-color: %2; }"
            "QPushButton#PlayPauseButton:pressed { background-color: %3; }")
            .arg(c.accent, c.accentHover, c.accentPressed,
                 QString::number(UISizes::playButtonSize / 2),
                 QString::number(UISizes::playButtonSize)));

    m_progressSlider->setStyleSheet(tm->sliderStyle(SliderVariant::Seek));
}

void TransportControls::updatePlayIcon()
{
    auto c = ThemeManager::instance()->colors();
    QColor iconColor(c.foregroundInverse);
    if (m_isPlaying) {
        m_playPauseBtn->setIcon(tintedSvgIcon(":/icons/pause.svg", iconColor));
    } else {
        m_playPauseBtn->setIcon(tintedSvgIcon(":/icons/play.svg", iconColor));
    }
}

void TransportControls::updateShuffleIcon()
{
    if (m_shuffleActive) {
        m_shuffleBtn->setIcon(tintedSvgIcon(":/icons/shuffle.svg",
            QColor(ThemeManager::instance()->colors().accent)));
    } else {
        m_shuffleBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/shuffle.svg"));
    }
}

void TransportControls::updateRepeatIcon()
{
    QColor green(ThemeManager::instance()->colors().accent);
    switch (m_repeatMode) {
    case PlaybackState::Off:
        m_repeatBtn->setIcon(ThemeManager::instance()->cachedIcon(":/icons/repeat.svg"));
        break;
    case PlaybackState::All:
        m_repeatBtn->setIcon(tintedSvgIcon(":/icons/repeat.svg", green));
        break;
    case PlaybackState::One:
        m_repeatBtn->setIcon(tintedSvgIcon(":/icons/repeat-1.svg", green));
        break;
    }
}

QString TransportControls::formatTime(int seconds)
{
    int mins = seconds / 60;
    int secs = seconds % 60;
    return QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
}
