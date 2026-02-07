#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../core/MusicData.h"

class PlaylistsView : public QWidget {
    Q_OBJECT
public:
    explicit PlaylistsView(QWidget* parent = nullptr);

    enum ViewMode { LargeIcons = 0, SmallIcons = 1, ListView = 2 };

signals:
    void playlistSelected(const QString& playlistId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onCreatePlaylistClicked();
    void onPlaylistsChanged();
    void onDeletePlaylistClicked(const QString& playlistId);

private:
    void setupUI();
    void populatePlaylists();
    void refreshTheme();
    void clearPlaylistCards();
    void setViewMode(ViewMode mode);
    QWidget* createPlaylistCard(const Playlist& playlist, int coverSize = 164);
    QWidget* createPlaylistListRow(const Playlist& playlist);

    QLabel* m_headerLabel;
    QLabel* m_smartHeader;
    QLabel* m_userHeader;
    QWidget* m_smartGrid;
    QGridLayout* m_smartGridLayout;
    QWidget* m_userGrid;
    QGridLayout* m_userGridLayout;
    StyledScrollArea* m_scrollArea;
    StyledButton* m_createBtn;

    // View toggle buttons
    StyledButton* m_largeIconBtn;
    StyledButton* m_smallIconBtn;
    StyledButton* m_listBtn;
    ViewMode m_viewMode = LargeIcons;

    QPushButton* m_navBackBtn = nullptr;
    QPushButton* m_navForwardBtn = nullptr;
};
