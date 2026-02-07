#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QImage>
#include "../../core/audio/TagWriter.h"

class TagEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit TagEditorDialog(const QString& filePath, QWidget* parent = nullptr);
    explicit TagEditorDialog(const QStringList& filePaths, QWidget* parent = nullptr);

signals:
    void tagsUpdated();

private slots:
    void onSave();
    void onChangeArt();
    void onRemoveArt();
    void onUndo();

private:
    void setupUi();
    void loadTags();
    void saveTags();
    void updateArtDisplay();

    QStringList m_filePaths;
    bool m_batchMode = false;

    // Original state for undo
    QVector<TrackMetadata> m_originalMeta;

    QLineEdit* m_titleEdit = nullptr;
    QLineEdit* m_artistEdit = nullptr;
    QLineEdit* m_albumEdit = nullptr;
    QLineEdit* m_albumArtistEdit = nullptr;
    QSpinBox* m_trackSpin = nullptr;
    QSpinBox* m_discSpin = nullptr;
    QSpinBox* m_yearSpin = nullptr;
    QLineEdit* m_genreEdit = nullptr;
    QLineEdit* m_composerEdit = nullptr;
    QLineEdit* m_commentEdit = nullptr;
    QLabel* m_artLabel = nullptr;
    QPushButton* m_changeArtBtn = nullptr;
    QPushButton* m_removeArtBtn = nullptr;
    QImage m_albumArt;
    bool m_artChanged = false;
    bool m_artRemoved = false;
};
