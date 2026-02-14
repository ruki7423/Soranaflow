#pragma once
#include <QWidget>
#include <QTabWidget>

class SettingsView : public QWidget {
    Q_OBJECT
public:
    explicit SettingsView(QWidget* parent = nullptr);

private:
    void setupUI();
    void refreshTheme();

    QTabWidget* m_tabWidget = nullptr;
};
