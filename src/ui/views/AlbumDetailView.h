#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QEvent>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/FormatBadge.h"
#include "../../widgets/TrackTableView.h"
#include "../../core/MusicData.h"
#include "../../core/PlaybackState.h"

class MetadataFixService;

class AlbumDetailView : public QWidget {
    Q_OBJECT
public:
    explicit AlbumDetailView(QWidget* parent = nullptr);
    void setAlbum(const QString& albumId);

signals:
    void backRequested();
    void artistClicked(const QString& artistId);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUI();
    void updateDisplay();
    void loadCoverArt();
    void applyHeroBackground(const QPixmap& pix);
    void refreshTheme();
    void resizeEvent(QResizeEvent* event) override;

    Album m_album;
    QPixmap m_heroSourcePixmap;   // original album art for hero background

    // Hero section
    QLabel* m_heroBackground;     // full-width blurred background
    QWidget* m_heroSection;
    QLabel* m_coverLabel;
    QLabel* m_titleLabel;
    QLabel* m_artistLabel;
    QLabel* m_yearLabel;
    QLabel* m_statsLabel;
    FormatBadge* m_formatBadge;
    QWidget* m_formatContainer;

    // Action buttons
    StyledButton* m_playAllBtn;
    StyledButton* m_shuffleBtn;
    StyledButton* m_addQueueBtn;

    // Track table
    TrackTableView* m_trackTable;

    // Back button
    StyledButton* m_backBtn;

    // Scroll
    StyledScrollArea* m_scrollArea;
    QVBoxLayout* m_mainLayout;

    MetadataFixService* m_metadataFixService;

    // Async cover loading
    int m_coverLoadId = 0;
};
