#pragma once

#include <QWidget>

class QVBoxLayout;
class QListWidget;

class VSTSettingsWidget : public QWidget {
    Q_OBJECT
public:
    explicit VSTSettingsWidget(QWidget* parent = nullptr);

private:
    QWidget* createVSTCard(QVBoxLayout* parentLayout);
    void saveVstPlugins();
    void loadVstPlugins();

    QListWidget* m_vst3AvailableList = nullptr;
    QListWidget* m_vst2AvailableList = nullptr;
    QListWidget* m_vst3ActiveList = nullptr;
};
