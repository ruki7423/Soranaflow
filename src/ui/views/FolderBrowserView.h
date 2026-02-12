#pragma once

#include <QWidget>
#include <QSplitter>
#include <QTreeWidget>
#include <QLabel>
#include <QHash>
#include <QVector>
#include "../../core/MusicData.h"

class TrackTableView;

class FolderBrowserView : public QWidget
{
    Q_OBJECT
public:
    explicit FolderBrowserView(QWidget* parent = nullptr);

signals:
    void albumSelected(const QString& albumId);
    void artistSelected(const QString& artistId);

private slots:
    void reloadFolders();
    void onFolderSelected(QTreeWidgetItem* item, int column);
    void onTrackDoubleClicked(const Track& track);
    void applyTheme();

private:
    void buildTree();
    QTreeWidgetItem* findOrCreatePath(const QString& rootPath,
                                       const QString& fullPath,
                                       QTreeWidgetItem* rootItem);

    QSplitter*      m_splitter    = nullptr;
    QTreeWidget*    m_folderTree  = nullptr;
    TrackTableView* m_trackTable  = nullptr;
    QLabel*         m_pathLabel   = nullptr;
    QLabel*         m_countLabel  = nullptr;

    QHash<QString, QVector<Track>> m_folderTracks;  // dir path -> tracks
    QString m_currentFolder;
    QVector<Track> m_currentTracks;  // tracks shown in table (for queue)
    bool m_libraryDirty = false;

protected:
    void showEvent(QShowEvent* event) override;
};
