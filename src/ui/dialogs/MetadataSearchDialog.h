#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include "../../core/MusicData.h"
#include "../../metadata/MusicBrainzProvider.h"

class MetadataSearchDialog : public QDialog {
    Q_OBJECT
public:
    explicit MetadataSearchDialog(const Track& track, QWidget* parent = nullptr);

    MusicBrainzResult selectedResult() const { return m_selectedResult; }

private slots:
    void onSearch();
    void onResultsReceived(const QVector<MusicBrainzResult>& results);
    void onApply();

private:
    void setupUI();
    void setupPreviewPanel();
    void updatePreview(int row);
    void applyConfirmed();
    QString confidenceText(double score) const;
    QString confidenceColor(double score) const;

    Track m_track;
    MusicBrainzResult m_selectedResult;
    QVector<MusicBrainzResult> m_results;

    // Search fields
    QLineEdit* m_titleEdit = nullptr;
    QLineEdit* m_artistEdit = nullptr;
    QLineEdit* m_albumEdit = nullptr;
    QPushButton* m_searchBtn = nullptr;
    QTableWidget* m_resultsTable = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QLabel* m_statusLabel = nullptr;

    // Preview panel
    QWidget* m_previewPanel = nullptr;
    QLabel* m_previewConfidence = nullptr;

    // Per-field checkboxes and labels
    QCheckBox* m_chkTitle = nullptr;
    QCheckBox* m_chkArtist = nullptr;
    QCheckBox* m_chkAlbum = nullptr;
    QCheckBox* m_chkYear = nullptr;

    QLabel* m_lblCurTitle = nullptr;
    QLabel* m_lblNewTitle = nullptr;
    QLabel* m_lblCurArtist = nullptr;
    QLabel* m_lblNewArtist = nullptr;
    QLabel* m_lblCurAlbum = nullptr;
    QLabel* m_lblNewAlbum = nullptr;
    QLabel* m_lblCurYear = nullptr;
    QLabel* m_lblNewYear = nullptr;
};
