#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <vector>

#include "../../../core/ThemeManager.h"
#include "../../../core/dsp/EqualizerProcessor.h"
#include "../../../widgets/StyledSwitch.h"
#include "../../../widgets/StyledSlider.h"
#include "../../../widgets/StyledComboBox.h"

class QListWidget;

// ── EQ Frequency Response Graph Widget ──────────────────────────────
class EQGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit EQGraphWidget(QWidget* parent = nullptr);
    void setResponse(const std::vector<double>& dBValues);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<double> m_response;
};

// ── Band Row UI ─────────────────────────────────────────────────────
struct EQBandRow {
    QWidget*        widget      = nullptr;
    QCheckBox*      enableCheck = nullptr;
    QLabel*         bandLabel   = nullptr;
    QComboBox*      typeCombo   = nullptr;
    QDoubleSpinBox* freqSpin    = nullptr;
    QDoubleSpinBox* gainSpin    = nullptr;
    QDoubleSpinBox* qSpin       = nullptr;
};

// ── Audio Settings Tab ──────────────────────────────────────────────
class AudioSettingsTab : public QWidget {
    Q_OBJECT
public:
    explicit AudioSettingsTab(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QWidget* createDSPCard(QVBoxLayout* parentLayout);
    QWidget* createVSTCard(QVBoxLayout* parentLayout);
    void applyEQPreset(const QString& presetName);
    void saveVstPlugins();
    void loadVstPlugins();
    void updateEQGraph();
    void syncBandToProcessor(int bandIndex);
    void rebuildBandRows();

    // DSP controls
    StyledSwitch* m_dspEnabledSwitch = nullptr;
    StyledSlider* m_gainSlider = nullptr;
    QLabel* m_gainValueLabel = nullptr;

    // 20-band parametric EQ
    EQGraphWidget* m_eqGraph = nullptr;
    QVBoxLayout* m_bandRowsLayout = nullptr;
    QWidget* m_bandRowsContainer = nullptr;
    EQBandRow m_bandRows[20] = {};
    QSpinBox* m_bandCountSpin = nullptr;
    StyledComboBox* m_eqPresetCombo = nullptr;
    int m_activeBandCount = 1;

    // VST
    QListWidget* m_vst3AvailableList = nullptr;
    QListWidget* m_vst2AvailableList = nullptr;
    QListWidget* m_vst3ActiveList = nullptr;  // shared active list for VST2 + VST3
};
