#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QProgressBar;

class ProcessingSettingsWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProcessingSettingsWidget(QWidget* parent = nullptr);

private:
    QPushButton* m_scanButton = nullptr;
    QProgressBar* m_scanProgress = nullptr;
    QLabel* m_scanStatusLabel = nullptr;
};
