#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QEvent>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledInput.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/TrackTableView.h"
#include "../../core/MusicData.h"
#include "../../core/PlaybackState.h"

class LibraryView : public QWidget {
    Q_OBJECT
public:
    explicit LibraryView(QWidget* parent = nullptr);
    void filterByFolder(const QString& folderPath);
    void filterByArtist(const QString& artistName);
    void filterByAlbum(const QString& albumName);
    void showAllTracks();
    void filterTracks(const QString& query);

signals:
    void albumClicked(const QString& albumId);
    void artistClicked(const QString& artistId);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onSearchChanged(const QString& text);
    void onTrackChanged(const Track& track);
    void onOpenFilesClicked();
    void onScanClicked();
    void onPlayAllClicked();
    void onFetchMetadataClicked();
    void onIdentifyAudioClicked();
    void onLibraryUpdated();

private:
    void setupUI();
    void populateTracks();
    void addTracksFromFiles(const QStringList& files);
    void refreshTheme();

    StyledInput* m_searchInput;
    StyledButton* m_openFilesBtn;
    StyledButton* m_showAllBtn = nullptr;
    QPushButton* m_playAllBtn = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QPushButton* m_fetchMetadataBtn = nullptr;
    QPushButton* m_identifyAudioBtn = nullptr;
    TrackTableView* m_trackTable;
    QLabel* m_headerLabel;
    QLabel* m_countLabel;
    QLabel* m_statusLabel = nullptr;
    QString m_activeFolder;
    QString m_activeArtist;
    QString m_activeAlbum;
    QPropertyAnimation* m_scanSpinAnim = nullptr;
    QPushButton* m_navBackBtn = nullptr;
    QPushButton* m_navForwardBtn = nullptr;
    QTimer* m_searchDebounceTimer = nullptr;
    bool m_libraryDirty = false;

protected:
    void showEvent(QShowEvent* event) override;
};
