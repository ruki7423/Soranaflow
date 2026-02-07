#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/FormatBadge.h"
#include "../../core/PlaybackState.h"
#include "../../core/MusicData.h"
#include <QPixmap>

class QueueView : public QWidget {
    Q_OBJECT
public:
    explicit QueueView(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onTrackChanged(const Track& track);
    void onQueueChanged();

private:
    void setupUI();
    void updateCurrentTrack();
    void updateQueueList();
    void refreshTheme();
    QWidget* createQueueItem(const Track& track, int index, bool isCurrent, bool isHistory);

    // Current track section
    QWidget* m_currentSection;
    QLabel* m_currentCover;
    QLabel* m_currentTitle;
    QLabel* m_currentArtist;
    FormatBadge* m_currentFormat;
    QWidget* m_currentFormatContainer;

    // Queue list
    QLabel* m_queueHeader;
    QWidget* m_queueListContainer;
    QVBoxLayout* m_queueListLayout;

    // History section
    QLabel* m_historyHeader;
    QWidget* m_historyListContainer;
    QVBoxLayout* m_historyListLayout;

    QLabel* m_titleLabel;
    StyledButton* m_clearBtn;
    StyledButton* m_shuffleBtn;
    StyledScrollArea* m_scrollArea;
    QLabel* m_emptyLabel;
    QLabel* m_nowPlayingLabel;

    QVector<Track> m_cachedDisplayQueue;

    // Drag reorder state
    int m_dragSourceIndex = -1;
    QPoint m_dragStartPos;
    bool m_blockRebuild = false;
    QWidget* m_dragSourceWidget = nullptr;
    int m_dropIndicatorIndex = -1;
};
