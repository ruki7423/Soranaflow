#pragma once

#include <QWidget>

// ── DSP Settings Widget — thin coordinator ──────────────────────────
// Composes ProcessingSettingsWidget, SpatialSettingsWidget, and
// EQSettingsWidget into a single vertical layout.
class DSPSettingsWidget : public QWidget {
    Q_OBJECT
public:
    explicit DSPSettingsWidget(QWidget* parent = nullptr);
};
