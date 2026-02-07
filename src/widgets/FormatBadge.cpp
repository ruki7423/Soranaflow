#include "FormatBadge.h"
#include "../core/ThemeManager.h"

FormatBadge::FormatBadge(AudioFormat format,
                         const QString& sampleRate,
                         const QString& bitDepth,
                         const QString& bitrate,
                         QWidget* parent)
    : QWidget(parent)
    , m_formatLabel(nullptr)
    , m_specsLabel(nullptr)
{
    setObjectName(QStringLiteral("FormatBadge"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // Format name label with colored background
    m_formatLabel = new QLabel(getFormatLabel(format), this);
    auto c = ThemeManager::instance()->colors();
    const QColor badgeColor = resolveFormatColor(format, sampleRate, bitDepth);
    m_formatLabel->setStyleSheet(
        QStringLiteral("QLabel {"
                       "  color: %1;"
                       "  background-color: %2;"
                       "  padding: 2px 6px;"
                       "  border-radius: 3px;"
                       "  font-weight: bold;"
                       "  font-size: 11px;"
                       "}").arg(c.badgeText,
                                badgeColor.name()));
    layout->addWidget(m_formatLabel);

    // Optional specs label
    QString specsText;
    if (!sampleRate.isEmpty())
        specsText += sampleRate;
    if (!bitDepth.isEmpty()) {
        if (!specsText.isEmpty()) specsText += QStringLiteral(" / ");
        specsText += bitDepth;
    }
    if (!bitrate.isEmpty()) {
        if (!specsText.isEmpty()) specsText += QStringLiteral(" / ");
        specsText += bitrate;
    }

    if (!specsText.isEmpty()) {
        m_specsLabel = new QLabel(specsText, this);
        m_specsLabel->setStyleSheet(
            QStringLiteral("QLabel {"
                           "  color: %1;"
                           "  font-family: monospace;"
                           "  font-size: 11px;"
                           "}").arg(c.foregroundMuted));
        layout->addWidget(m_specsLabel);
    }

    layout->addStretch();
    setLayout(layout);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QColor FormatBadge::resolveFormatColor(AudioFormat format,
                                       const QString& sampleRate,
                                       const QString& bitDepth) const
{
    auto c = ThemeManager::instance()->colors();

    Q_UNUSED(sampleRate);
    Q_UNUSED(bitDepth);

    switch (format) {
    case AudioFormat::DSD64:
    case AudioFormat::DSD128:
    case AudioFormat::DSD256:
    case AudioFormat::DSD512:
    case AudioFormat::DSD1024:
    case AudioFormat::DSD2048:
        return QColor(c.badgeDsd);

    case AudioFormat::WAV:
        return QColor(c.badgeHires);

    case AudioFormat::FLAC:
    case AudioFormat::ALAC:
        return QColor(c.badgeFlac);

    case AudioFormat::MP3:
    case AudioFormat::AAC:
        return QColor(0x95, 0xA5, 0xA6);

    default:
        return QColor(0x95, 0xA5, 0xA6);
    }
}
