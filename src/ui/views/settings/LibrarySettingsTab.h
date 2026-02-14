#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>

class StyledButton;

class LibrarySettingsTab : public QWidget {
    Q_OBJECT
public:
    explicit LibrarySettingsTab(QWidget* parent = nullptr);

private slots:
    void onAddFolderClicked();
    void onRemoveFolderClicked(const QString& folder);
    void onScanNowClicked();
    void onFullRescanClicked();
    void onScanProgress(int current, int total);
    void onScanFinished(int tracksFound);

private:
    void rebuildFolderList();

    QVBoxLayout* m_foldersLayout = nullptr;
    QWidget* m_foldersContainer = nullptr;
    QLabel* m_scanStatusLabel = nullptr;
    StyledButton* m_scanNowBtn = nullptr;
    StyledButton* m_fullRescanBtn = nullptr;
    StyledButton* m_restoreButton = nullptr;
};
