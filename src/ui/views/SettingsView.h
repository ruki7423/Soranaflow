#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QScrollArea>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledSlider.h"
#include "../../widgets/StyledSwitch.h"
#include "../../widgets/StyledComboBox.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../core/ThemeManager.h"
#include "../../core/dsp/EqualizerProcessor.h"

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

class SettingsView : public QWidget {
    Q_OBJECT
public:
    explicit SettingsView(QWidget* parent = nullptr);

private slots:
    void onAddFolderClicked();
    void onRemoveFolderClicked(const QString& folder);
    void onScanNowClicked();
    void onFullRescanClicked();
    void onScanProgress(int current, int total);
    void onScanFinished(int tracksFound);

private:
    void setupUI();
    void refreshTheme();
    QWidget* createAudioTab();
    QWidget* createLibraryTab();
    QWidget* createAppleMusicTab();
    // QWidget* createTidalTab();  // TODO: restore when Tidal API available
    QWidget* createAppearanceTab();
    QWidget* createAboutTab();
    QWidget* createSettingRow(const QString& label, const QString& description, QWidget* control);
    QWidget* createSectionHeader(const QString& title);
    QWidget* createDSPCard(QVBoxLayout* parentLayout);
    QWidget* createVSTCard(QVBoxLayout* parentLayout);
    void applyEQPreset(const QString& presetName);
    void rebuildFolderList();
    void saveVstPlugins();
    void loadVstPlugins();
    void updateEQGraph();
    void syncBandToProcessor(int bandIndex);
    void rebuildBandRows();

    QTabWidget* m_tabWidget;

    // Library tab controls
    QVBoxLayout* m_foldersLayout = nullptr;
    QWidget* m_foldersContainer = nullptr;
    QLabel* m_scanStatusLabel = nullptr;
    StyledButton* m_scanNowBtn = nullptr;
    StyledButton* m_fullRescanBtn = nullptr;
    StyledButton* m_restoreButton = nullptr;

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

    // Apple Music
    StyledButton* m_appleMusicConnectBtn = nullptr;
    QLabel* m_appleMusicStatusLabel = nullptr;
    QLabel* m_appleMusicSubLabel = nullptr;

    /* TODO: restore when Tidal API available
    // Tidal
    StyledButton* m_tidalConnectBtn = nullptr;
    StyledButton* m_tidalLogoutBtn = nullptr;
    QLabel* m_tidalStatusLabel = nullptr;
    QLabel* m_tidalSubLabel = nullptr;
    */
};
