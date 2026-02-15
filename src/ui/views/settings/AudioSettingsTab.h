#pragma once

#include <QWidget>

// ── Audio Settings Tab ──────────────────────────────────────────────
// Thin shell composing OutputSettingsWidget, DSPSettingsWidget, and
// VSTSettingsWidget inside a single scrollable area.
class AudioSettingsTab : public QWidget {
    Q_OBJECT
public:
    explicit AudioSettingsTab(QWidget* parent = nullptr);
};
