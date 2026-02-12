#include "ThemeManager.h"

#include <QStyleHints>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>

// ── Singleton ───────────────────────────────────────────────────────
ThemeManager* ThemeManager::instance()
{
    static ThemeManager s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent)
{
}

// ── setTheme ────────────────────────────────────────────────────────
void ThemeManager::setTheme(Theme theme)
{
    m_theme = theme;

    // Resolve the effective theme when System is selected
    Theme effective = theme;
    if (effective == System) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        auto scheme = QGuiApplication::styleHints()->colorScheme();
        effective = (scheme == Qt::ColorScheme::Dark) ? Dark : Light;
        qDebug() << "[ThemeManager] System color scheme detected:"
                 << ((effective == Dark) ? "Dark" : "Light");
#else
        effective = Dark;
        qDebug() << "[ThemeManager] System theme detection not available, defaulting to Dark";
#endif
    }

    // Choose the stylesheet resource path
    QString qssPath = (effective == Dark)
        ? QStringLiteral(":/styles/dark-theme.qss")
        : QStringLiteral(":/styles/light-theme.qss");

    qDebug() << "[ThemeManager] Loading stylesheet:" << qssPath;

    QString stylesheet = loadStyleSheet(qssPath);

    if (stylesheet.isEmpty()) {
        qWarning() << "[ThemeManager] Stylesheet loaded but is empty or file not found:" << qssPath;
    } else {
        qDebug() << "[ThemeManager] Stylesheet loaded successfully — size:"
                 << stylesheet.size() << "bytes";
    }

    qApp->setStyleSheet(stylesheet);

    m_iconCache.clear();  // invalidate cached icons — colors changed
    emit themeChanged(m_theme);
}

// ── isDark ──────────────────────────────────────────────────────────
bool ThemeManager::isDark() const
{
    if (m_theme == System) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        auto scheme = QGuiApplication::styleHints()->colorScheme();
        return scheme == Qt::ColorScheme::Dark;
#else
        return true;
#endif
    }
    return m_theme == Dark;
}

// ── loadStyleSheet ──────────────────────────────────────────────────
QString ThemeManager::loadStyleSheet(const QString& path)
{
    QFile file(path);

    if (!file.exists()) {
        qWarning() << "[ThemeManager] Stylesheet file not found:" << path;
        return {};
    }

    qDebug() << "[ThemeManager] File found:" << path
             << "| size:" << file.size() << "bytes";

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[ThemeManager] Failed to open stylesheet:" << path;
        return {};
    }

    QString content = QString::fromUtf8(file.readAll());
    file.close();

    qDebug() << "[ThemeManager] File read successfully:" << path;
    return content;
}

// ═════════════════════════════════════════════════════════════════════
//  Centralized Color Definitions
// ═════════════════════════════════════════════════════════════════════

ThemeColors ThemeManager::colors() const
{
    return isDark() ? darkColors() : lightColors();
}

ThemeColors ThemeManager::darkColors() const
{
    return ThemeColors{
        // Base colors — deep blacks for OLED-friendly, audiophile aesthetic
        QStringLiteral("#0A0A0A"),                      // background
        QStringLiteral("#141414"),                      // backgroundSecondary
        QStringLiteral("#1E1E1E"),                      // backgroundTertiary
        QStringLiteral("#252525"),                      // backgroundElevated

        // Text colors
        QStringLiteral("rgba(255, 255, 255, 0.87)"),   // foreground
        QStringLiteral("rgba(255, 255, 255, 0.60)"),   // foregroundSecondary
        QStringLiteral("rgba(255, 255, 255, 0.38)"),   // foregroundMuted
        QStringLiteral("#FFFFFF"),                      // foregroundInverse

        // Border colors
        QStringLiteral("rgba(255, 255, 255, 0.12)"),   // border
        QStringLiteral("rgba(255, 255, 255, 0.06)"),   // borderSubtle
        QStringLiteral("#0A84FF"),                      // borderFocus

        // Accent colors — Apple-style blue
        QStringLiteral("#0A84FF"),                      // accent
        QStringLiteral("#409CFF"),                      // accentHover
        QStringLiteral("#0066CC"),                      // accentPressed
        QStringLiteral("rgba(10, 132, 255, 0.15)"),    // accentMuted

        // Semantic colors
        QStringLiteral("#30D158"),                      // success
        QStringLiteral("#FF9F0A"),                      // warning
        QStringLiteral("#FF453A"),                      // error
        QStringLiteral("#FF6961"),                      // errorHover

        // Audio player specific
        QStringLiteral("#30D158"),                      // playing
        QStringLiteral("#0A84FF"),                      // waveform
        QStringLiteral("#0A84FF"),                      // progressFill
        QStringLiteral("rgba(255, 255, 255, 0.12)"),   // progressTrack
        QStringLiteral("#FFFFFF"),                      // volumeFill
        QStringLiteral("rgba(255, 255, 255, 0.24)"),   // volumeTrack

        // Format badges
        QStringLiteral("#30D158"),                      // badgeFlac — green
        QStringLiteral("#BF5AF2"),                      // badgeDsd — purple
        QStringLiteral("#FF375F"),                      // badgeMqa — pink
        QStringLiteral("#0A84FF"),                      // badgeHires — blue
        QStringLiteral("#FFFFFF"),                      // badgeText

        // Interactive states
        QStringLiteral("rgba(255, 255, 255, 0.08)"),   // hover
        QStringLiteral("rgba(255, 255, 255, 0.12)"),   // pressed
        QStringLiteral("rgba(10, 132, 255, 0.20)"),    // selected
        QStringLiteral("#0A84FF"),                      // selectedBorder

        // Shadows
        QStringLiteral("rgba(0, 0, 0, 0.2)"),          // shadowLight
        QStringLiteral("rgba(0, 0, 0, 0.4)"),          // shadowMedium
        QStringLiteral("rgba(0, 0, 0, 0.6)")           // shadowHeavy
    };
}

ThemeColors ThemeManager::lightColors() const
{
    return ThemeColors{
        // Base colors — clean whites and light grays
        QStringLiteral("#FFFFFF"),                      // background
        QStringLiteral("#F5F5F7"),                      // backgroundSecondary
        QStringLiteral("#E8E8ED"),                      // backgroundTertiary
        QStringLiteral("#FFFFFF"),                      // backgroundElevated

        // Text colors
        QStringLiteral("rgba(0, 0, 0, 0.87)"),         // foreground
        QStringLiteral("rgba(0, 0, 0, 0.60)"),         // foregroundSecondary
        QStringLiteral("rgba(0, 0, 0, 0.38)"),         // foregroundMuted
        QStringLiteral("#FFFFFF"),                      // foregroundInverse

        // Border colors
        QStringLiteral("rgba(0, 0, 0, 0.12)"),         // border
        QStringLiteral("rgba(0, 0, 0, 0.06)"),         // borderSubtle
        QStringLiteral("#007AFF"),                      // borderFocus

        // Accent colors — Apple-style blue
        QStringLiteral("#007AFF"),                      // accent
        QStringLiteral("#0066CC"),                      // accentHover
        QStringLiteral("#004999"),                      // accentPressed
        QStringLiteral("rgba(0, 122, 255, 0.10)"),     // accentMuted

        // Semantic colors
        QStringLiteral("#34C759"),                      // success
        QStringLiteral("#FF9500"),                      // warning
        QStringLiteral("#FF3B30"),                      // error
        QStringLiteral("#FF6B6B"),                      // errorHover

        // Audio player specific
        QStringLiteral("#34C759"),                      // playing
        QStringLiteral("#007AFF"),                      // waveform
        QStringLiteral("#007AFF"),                      // progressFill
        QStringLiteral("rgba(0, 0, 0, 0.12)"),         // progressTrack
        QStringLiteral("#000000"),                      // volumeFill
        QStringLiteral("rgba(0, 0, 0, 0.16)"),         // volumeTrack

        // Format badges
        QStringLiteral("#34C759"),                      // badgeFlac
        QStringLiteral("#AF52DE"),                      // badgeDsd
        QStringLiteral("#FF2D55"),                      // badgeMqa
        QStringLiteral("#007AFF"),                      // badgeHires
        QStringLiteral("#FFFFFF"),                      // badgeText

        // Interactive states
        QStringLiteral("rgba(0, 0, 0, 0.04)"),         // hover
        QStringLiteral("rgba(0, 0, 0, 0.08)"),         // pressed
        QStringLiteral("rgba(0, 122, 255, 0.12)"),     // selected
        QStringLiteral("#007AFF"),                      // selectedBorder

        // Shadows
        QStringLiteral("rgba(0, 0, 0, 0.08)"),         // shadowLight
        QStringLiteral("rgba(0, 0, 0, 0.16)"),         // shadowMedium
        QStringLiteral("rgba(0, 0, 0, 0.24)")          // shadowHeavy
    };
}

// ═════════════════════════════════════════════════════════════════════
//  Reusable Style-Sheet Helpers (sizes from UISizes, colors from theme)
// ═════════════════════════════════════════════════════════════════════

QString ThemeManager::buttonStyle(ButtonVariant variant) const
{
    auto c = colors();

    switch (variant) {
    case ButtonVariant::Primary:
        return QString(
            "QPushButton {"
            "  background-color: %1;"
            "  border: none;"
            "  border-radius: %2px;"
            "  color: %3;"
            "  padding: %4px %5px;"
            "  font-size: %6px;"
            "  font-weight: 500;"
            "  min-height: %7px;"
            "}"
            "QPushButton:hover { background-color: %8; }"
            "QPushButton:pressed { background-color: %9; }"
            "QPushButton:disabled { background-color: %10; color: %11; }")
            .arg(c.accent)
            .arg(UISizes::buttonRadius)
            .arg(c.foregroundInverse)
            .arg(UISizes::buttonPaddingV)
            .arg(UISizes::buttonPaddingH)
            .arg(UISizes::fontSizeMD)
            .arg(UISizes::buttonHeight)
            .arg(c.accentHover)
            .arg(c.accentPressed)
            .arg(c.backgroundTertiary)
            .arg(c.foregroundMuted);

    case ButtonVariant::Secondary:
        return QString(
            "QPushButton {"
            "  background-color: %1;"
            "  border: none;"
            "  border-radius: %2px;"
            "  color: %3;"
            "  padding: %4px %5px;"
            "  font-size: %6px;"
            "  font-weight: 500;"
            "  min-height: %7px;"
            "}"
            "QPushButton:hover { background-color: %8; }"
            "QPushButton:pressed { background-color: %9; }")
            .arg(c.backgroundTertiary)
            .arg(UISizes::buttonRadius)
            .arg(c.foreground)
            .arg(UISizes::buttonPaddingV)
            .arg(UISizes::buttonPaddingH)
            .arg(UISizes::fontSizeMD)
            .arg(UISizes::buttonHeight)
            .arg(c.hover)
            .arg(c.pressed);

    case ButtonVariant::Ghost:
        return QString(
            "QPushButton {"
            "  background-color: transparent;"
            "  border: none;"
            "  border-radius: %1px;"
            "  color: %2;"
            "  padding: %3px %4px;"
            "  font-size: %5px;"
            "  font-weight: 500;"
            "  min-height: %6px;"
            "}"
            "QPushButton:hover { background-color: %7; }"
            "QPushButton:pressed { background-color: %8; }")
            .arg(UISizes::buttonRadius)
            .arg(c.foreground)
            .arg(UISizes::buttonPaddingV)
            .arg(UISizes::buttonPaddingH)
            .arg(UISizes::fontSizeMD)
            .arg(UISizes::buttonHeight)
            .arg(c.hover)
            .arg(c.pressed);

    case ButtonVariant::Destructive:
        return QString(
            "QPushButton {"
            "  background-color: %1;"
            "  border: none;"
            "  border-radius: %2px;"
            "  color: %3;"
            "  padding: %4px %5px;"
            "  font-size: %6px;"
            "  font-weight: 500;"
            "  min-height: %7px;"
            "}"
            "QPushButton:hover { background-color: %8; }"
            "QPushButton:pressed { background-color: %9; }")
            .arg(c.error)
            .arg(UISizes::buttonRadius)
            .arg(c.foregroundInverse)
            .arg(UISizes::buttonPaddingV)
            .arg(UISizes::buttonPaddingH)
            .arg(UISizes::fontSizeMD)
            .arg(UISizes::buttonHeight)
            .arg(c.errorHover)
            .arg(c.error);
    }
    return {};
}

QString ThemeManager::toggleButtonStyle(bool checked) const
{
    auto c = colors();
    QString bg = checked ? c.accent : QStringLiteral("transparent");
    QString hoverBg = checked ? c.accentHover : c.hover;

    return QString(
        "QPushButton {"
        "  background-color: %1;"
        "  border: none;"
        "  border-radius: %2px;"
        "  padding: 0px;"
        "  min-width: %3px;"
        "  max-width: %3px;"
        "  min-height: %3px;"
        "  max-height: %3px;"
        "}"
        "QPushButton:hover { background-color: %4; }")
        .arg(bg)
        .arg(UISizes::toggleRadius)
        .arg(UISizes::toggleButtonSize)
        .arg(hoverBg);
}

QString ThemeManager::inputStyle() const
{
    auto c = colors();

    return QString(
        "QLineEdit {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: %3px;"
        "  padding: %4px %5px;"
        "  color: %6;"
        "  font-size: %7px;"
        "  min-height: %8px;"
        "  selection-background-color: %9;"
        "}"
        "QLineEdit:focus {"
        "  border: 1px solid %10;"
        "}"
        "QLineEdit::placeholder {"
        "  color: %11;"
        "}"
        "QLineEdit:disabled {"
        "  background-color: %12;"
        "  color: %13;"
        "}")
        .arg(c.backgroundTertiary)
        .arg(c.border)
        .arg(UISizes::inputRadius)
        .arg(UISizes::inputPaddingV)
        .arg(UISizes::inputPaddingH)
        .arg(c.foreground)
        .arg(UISizes::fontSizeMD)
        .arg(UISizes::inputHeight)
        .arg(c.accent)
        .arg(c.borderFocus)
        .arg(c.foregroundMuted)
        .arg(c.backgroundSecondary)
        .arg(c.foregroundMuted);
}

QString ThemeManager::sliderStyle(SliderVariant variant) const
{
    auto c = colors();

    QString trackColor = (variant == SliderVariant::Volume) ? c.volumeTrack : c.progressTrack;
    QString fillColor  = (variant == SliderVariant::Volume) ? c.volumeFill  : c.progressFill;
    QString handleColor = (variant == SliderVariant::Volume) ? c.volumeFill : c.foreground;
    int grooveH = (variant == SliderVariant::Volume) ? UISizes::volumeSliderHeight : UISizes::seekSliderHeight;

    return QString(
        "QSlider::groove:horizontal {"
        "  background: %1;"
        "  height: %2px;"
        "  border-radius: %3px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background: %4;"
        "  border-radius: %3px;"
        "}"
        "QSlider::handle:horizontal {"
        "  background: %5;"
        "  width: 12px;"
        "  height: 12px;"
        "  margin: -4px 0;"
        "  border-radius: 6px;"
        "}"
        "QSlider::handle:horizontal:hover {"
        "  background: %6;"
        "}")
        .arg(trackColor)
        .arg(grooveH)
        .arg(grooveH / 2)
        .arg(fillColor, handleColor, c.accent);
}

QString ThemeManager::menuStyle() const
{
    auto c = colors();

    return QString(
        "QMenu {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: %3px;"
        "  padding: %4px 0px;"
        "}"
        "QMenu::item {"
        "  padding: %5px %6px;"
        "  color: %7;"
        "  font-size: %8px;"
        "  border-radius: %9px;"
        "}"
        "QMenu::item:selected {"
        "  background-color: %10;"
        "}"
        "QMenu::separator {"
        "  height: 1px;"
        "  background-color: %11;"
        "  margin: %4px %5px;"
        "}")
        .arg(c.backgroundElevated)
        .arg(c.border)
        .arg(UISizes::cardRadius)
        .arg(UISizes::spacingXS)
        .arg(UISizes::spacingSM)
        .arg(UISizes::spacingLG)
        .arg(c.foreground)
        .arg(UISizes::fontSizeMD)
        .arg(UISizes::spacingXS)
        .arg(c.hover)
        .arg(c.borderSubtle);
}

QString ThemeManager::scrollbarStyle() const
{
    auto c = colors();

    return QString(
        "QScrollBar:vertical {"
        "  background: transparent;"
        "  width: %1px;"
        "  margin: 0;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: %2;"
        "  border-radius: %3px;"
        "  min-height: 40px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background: %4;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}")
        .arg(UISizes::spacingSM)
        .arg(c.border)
        .arg(UISizes::spacingXS)
        .arg(c.foregroundMuted);
}

QString ThemeManager::dialogStyle() const
{
    auto c = colors();

    return QString(
        "QDialog {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: %3px;"
        "}")
        .arg(c.backgroundElevated)
        .arg(c.border)
        .arg(UISizes::dialogRadius);
}

QString ThemeManager::formatBadgeStyle(const QString& format) const
{
    auto c = colors();
    QString bgColor;

    if (format == QStringLiteral("FLAC") || format == QStringLiteral("ALAC")) {
        bgColor = c.badgeFlac;
    } else if (format == QStringLiteral("DSD") || format == QStringLiteral("DSF")
               || format == QStringLiteral("DFF")) {
        bgColor = c.badgeDsd;
    } else if (format == QStringLiteral("MQA")) {
        bgColor = c.badgeMqa;
    } else if (format.contains(QStringLiteral("24")) || format.contains(QStringLiteral("32"))
               || format.contains(QStringLiteral("Hi-Res"))) {
        bgColor = c.badgeHires;
    } else {
        bgColor = c.foregroundMuted;
    }

    return QString(
        "QLabel {"
        "  background-color: %1;"
        "  color: %2;"
        "  border-radius: %3px;"
        "  padding: %4px %5px;"
        "  font-size: %6px;"
        "  font-weight: 600;"
        "}")
        .arg(bgColor)
        .arg(c.badgeText)
        .arg(UISizes::badgeRadius)
        .arg(UISizes::badgePaddingV)
        .arg(UISizes::badgePaddingH)
        .arg(UISizes::badgeFontSize);
}

// ── cachedIcon ─────────────────────────────────────────────────────
QIcon ThemeManager::cachedIcon(const QString& resourcePath)
{
    auto it = m_iconCache.find(resourcePath);
    if (it != m_iconCache.end())
        return it.value();

    QIcon icon = themedIcon(resourcePath);
    m_iconCache.insert(resourcePath, icon);
    return icon;
}

// ── themedIcon ──────────────────────────────────────────────────────
QIcon ThemeManager::themedIcon(const QString& resourcePath) const
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon(resourcePath);

    QString svgContent = QString::fromUtf8(file.readAll());
    file.close();

    QString color = iconColor();
    svgContent.replace(QStringLiteral("currentColor"), color);

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
