#pragma once
#include <QObject>
#include <QString>
#include <QColor>
#include <QFile>
#include <QApplication>
#include <QDebug>
#include <QIcon>

// ── Centralized color system ────────────────────────────────────────
struct ThemeColors {
    // Base colors
    QString background;           // Main app background
    QString backgroundSecondary;  // Cards, panels, sidebars
    QString backgroundTertiary;   // Inputs, hover states
    QString backgroundElevated;   // Dialogs, popovers, dropdowns

    // Text colors
    QString foreground;           // Primary text (87% opacity)
    QString foregroundSecondary;  // Secondary text (60% opacity)
    QString foregroundMuted;      // Disabled, hints (38% opacity)
    QString foregroundInverse;    // Text on accent color

    // Border colors
    QString border;               // Default borders
    QString borderSubtle;         // Subtle dividers
    QString borderFocus;          // Focus rings (usually accent)

    // Accent colors
    QString accent;               // Primary accent (buttons, links)
    QString accentHover;          // Accent hover state
    QString accentPressed;        // Accent pressed state
    QString accentMuted;          // Accent at low opacity for backgrounds

    // Semantic colors
    QString success;              // Green - success states
    QString warning;              // Orange - warnings
    QString error;                // Red - errors, destructive
    QString errorHover;           // Red hover

    // Audio player specific
    QString playing;              // Now playing indicator
    QString waveform;             // Waveform visualization color
    QString progressFill;         // Progress bar filled portion
    QString progressTrack;        // Progress bar background track
    QString volumeFill;           // Volume slider filled portion
    QString volumeTrack;          // Volume slider background track

    // Format badges
    QString badgeFlac;            // FLAC / lossless badge
    QString badgeDsd;             // DSD badge
    QString badgeMqa;             // MQA badge
    QString badgeHires;           // Hi-Res badge
    QString badgeText;            // Badge text color

    // Interactive states
    QString hover;                // Generic hover overlay
    QString pressed;              // Generic pressed overlay
    QString selected;             // Selected item background
    QString selectedBorder;       // Selected item border

    // Shadows (for elevated surfaces)
    QString shadowLight;
    QString shadowMedium;
    QString shadowHeavy;

    // Convert a CSS color string (hex or rgba) to QColor for QPainter use
    static QColor toQColor(const QString& css) {
        if (css.startsWith('#'))
            return QColor(css);
        if (css.startsWith(QStringLiteral("rgba("))) {
            // Parse "rgba(r, g, b, alpha)" where alpha is 0.0-1.0
            QString inner = css.mid(5, css.length() - 6);
            QStringList parts = inner.split(',');
            if (parts.size() == 4) {
                return QColor(parts[0].trimmed().toInt(),
                              parts[1].trimmed().toInt(),
                              parts[2].trimmed().toInt(),
                              qRound(parts[3].trimmed().toDouble() * 255));
            }
        }
        if (css.startsWith(QStringLiteral("rgb("))) {
            QString inner = css.mid(4, css.length() - 5);
            QStringList parts = inner.split(',');
            if (parts.size() == 3) {
                return QColor(parts[0].trimmed().toInt(),
                              parts[1].trimmed().toInt(),
                              parts[2].trimmed().toInt());
            }
        }
        return QColor(css);
    }
};

// ── Theme-independent sizing constants ──────────────────────────────
// These values NEVER change between themes
namespace UISizes {
    // Buttons - standard
    constexpr int buttonHeight    = 32;
    constexpr int buttonPaddingV  = 8;
    constexpr int buttonPaddingH  = 16;
    constexpr int buttonRadius    = 6;
    constexpr int buttonIconSize  = 16;
    constexpr int buttonSpacing   = 12;

    // Buttons - small (icon only)
    constexpr int smallButtonSize   = 28;
    constexpr int smallButtonRadius = 6;
    constexpr int smallIconSize     = 16;

    // View toggle buttons
    constexpr int toggleButtonSize = 24;
    constexpr int toggleIconSize   = 14;
    constexpr int toggleRadius     = 6;
    constexpr int toggleSpacing    = 4;

    // Playback bar
    constexpr int playbackBarHeight    = 72;
    constexpr int playButtonSize       = 40;
    constexpr int playButtonRadius     = 20;
    constexpr int transportButtonSize  = 32;
    constexpr int controlButtonSize    = 28;
    constexpr int volumeSliderWidth    = 100;
    constexpr int volumeSliderHeight   = 4;
    constexpr int seekSliderHeight     = 4;

    // Inputs
    constexpr int inputHeight    = 36;
    constexpr int inputPaddingV  = 8;
    constexpr int inputPaddingH  = 12;
    constexpr int inputRadius    = 6;

    // Cards
    constexpr int cardRadius         = 8;
    constexpr int albumCardWidth     = 180;
    constexpr int albumCardHeight    = 220;
    constexpr int albumCoverSize     = 180;
    constexpr int playlistCardWidth  = 200;
    constexpr int playlistCardHeight = 240;
    constexpr int playlistCoverSize  = 200;

    // Spacing
    constexpr int spacingXS  = 4;
    constexpr int spacingSM  = 8;
    constexpr int spacingMD  = 12;
    constexpr int spacingLG  = 16;
    constexpr int spacingXL  = 24;
    constexpr int spacingXXL = 32;

    // Content margins
    constexpr int contentMargin = 24;
    constexpr int sidebarWidth  = 240;

    // Typography
    constexpr int fontSizeXS  = 10;
    constexpr int fontSizeSM  = 12;
    constexpr int fontSizeMD  = 14;
    constexpr int fontSizeLG  = 16;
    constexpr int fontSizeXL  = 20;
    constexpr int fontSizeXXL = 28;

    // Dialogs
    constexpr int dialogWidth   = 360;
    constexpr int dialogPadding = 24;
    constexpr int dialogRadius  = 12;

    // Tables / Lists
    constexpr int rowHeight     = 48;
    constexpr int thumbnailSize = 40;
    constexpr int headerHeight  = 32;

    // Format badge
    constexpr int badgePaddingV = 2;
    constexpr int badgePaddingH = 6;
    constexpr int badgeRadius   = 3;
    constexpr int badgeFontSize = 10;

    // Switch
    constexpr int switchWidth  = 44;
    constexpr int switchHeight = 24;
}

// ── Button / Slider variant enums ───────────────────────────────────
enum class ButtonVariant { Primary, Secondary, Ghost, Destructive };
enum class SliderVariant { Volume, Seek };

class ThemeManager : public QObject {
    Q_OBJECT
public:
    enum Theme { Light, Dark, System };
    Q_ENUM(Theme)

    static ThemeManager* instance();
    Theme currentTheme() const { return m_theme; }
    void setTheme(Theme theme);
    bool isDark() const;

    // ── Centralized color access ────────────────────────────────────
    ThemeColors colors() const;

    // ── Convenience methods (delegate to colors()) ──────────────────
    QString foregroundColor() const  { return colors().foreground; }
    QString mutedColor() const       { return colors().foregroundMuted; }
    QString iconColor() const        { return isDark() ? QStringLiteral("#FFFFFF") : QStringLiteral("#333333"); }
    QString surfaceColor() const     { return colors().backgroundSecondary; }
    QString backgroundColor() const  { return colors().background; }
    QString accentColor() const      { return colors().accent; }

    // ── Reusable style-sheet helpers ────────────────────────────────
    QString buttonStyle(ButtonVariant variant) const;
    QString toggleButtonStyle(bool checked) const;
    QString inputStyle() const;
    QString sliderStyle(SliderVariant variant) const;
    QString menuStyle() const;
    QString scrollbarStyle() const;
    QString dialogStyle() const;
    QString formatBadgeStyle(const QString& format) const;

    // ── Icon helper ─────────────────────────────────────────────────
    QIcon themedIcon(const QString& resourcePath) const;

signals:
    void themeChanged(Theme theme);

private:
    explicit ThemeManager(QObject* parent = nullptr);
    Theme m_theme = Dark;
    QString loadStyleSheet(const QString& path);
    ThemeColors darkColors() const;
    ThemeColors lightColors() const;
};
