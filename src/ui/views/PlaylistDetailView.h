#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/FormatBadge.h"
#include "../../widgets/TrackTableView.h"
#include "../../core/MusicData.h"
#include "../../core/PlaybackState.h"

class PlaylistDetailView : public QWidget {
    Q_OBJECT
public:
    explicit PlaylistDetailView(QWidget* parent = nullptr);
    void setPlaylist(const QString& playlistId);

signals:
    void backRequested();

private:
    void setupUI();
    void updateDisplay();
    void refreshTheme();

    Playlist m_playlist;

    StyledButton* m_backBtn;
    QLabel* m_coverLabel;
    QLabel* m_typeLabel;
    QLabel* m_nameLabel;
    QLabel* m_descLabel;
    QLabel* m_statsLabel;

    StyledButton* m_playAllBtn;
    StyledButton* m_shuffleBtn;

    TrackTableView* m_trackTable;

    StyledScrollArea* m_scrollArea;
};
