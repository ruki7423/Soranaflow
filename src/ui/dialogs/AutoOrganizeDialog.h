#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include "../../core/library/AutoOrganizer.h"

class AutoOrganizeDialog : public QDialog {
    Q_OBJECT
public:
    explicit AutoOrganizeDialog(const QStringList& filePaths, QWidget* parent = nullptr);

private slots:
    void onPatternChanged();
    void onBrowseDestination();
    void onExecute();
    void onUndo();

private:
    void setupUi();
    void refreshPreview();

    AutoOrganizer m_organizer;
    QStringList m_filePaths;
    QVector<OrganizeAction> m_previewActions;

    QLineEdit* m_patternEdit = nullptr;
    QLineEdit* m_destEdit = nullptr;
    QTreeWidget* m_previewTree = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_executeBtn = nullptr;
    QPushButton* m_undoBtn = nullptr;
};
