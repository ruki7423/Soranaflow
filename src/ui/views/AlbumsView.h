#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QEvent>
#include <QPixmap>
#include <QPushButton>
#include <QButtonGroup>
#include <QHash>
#include <QPointer>

#include "../../core/MusicData.h"
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledInput.h"

class AlbumsView : public QWidget {
    Q_OBJECT
public:
    explicit AlbumsView(QWidget* parent = nullptr);

    enum ViewMode { LargeIcons, SmallIcons, ListView };

signals:
    void albumSelected(const QString& albumId);

public slots:
    void reloadAlbums();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUI();
    void clearGrid();
    QWidget* createAlbumCard(const Album& album, int cardWidth);
    QWidget* createAlbumListRow(const Album& album);
    QPixmap findCoverArt(const Album& album);
    void relayoutGrid();
    void setViewMode(ViewMode mode);
    void loadNextCoverBatch();
    void onFilterChanged(const QString& text);
    static QPixmap renderRoundedCover(const QPixmap& src, int size, int radius);

    ViewMode m_viewMode = LargeIcons;
    QLabel* m_headerLabel = nullptr;
    QLabel* m_countLabel = nullptr;
    QWidget* m_gridContainer = nullptr;
    QGridLayout* m_gridLayout = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QVector<QWidget*> m_cards;
    QVector<Album> m_albums;
    StyledButton* m_largeIconBtn = nullptr;
    StyledButton* m_smallIconBtn = nullptr;
    StyledButton* m_listBtn = nullptr;
    QPushButton* m_navBackBtn = nullptr;
    QPushButton* m_navForwardBtn = nullptr;
    StyledInput* m_filterInput = nullptr;
    QString m_filterText;

    // Cover art cache + async loading
    QHash<QString, QPixmap> m_coverCache;
    QHash<QString, QPointer<QLabel>> m_coverLabels;
    int m_coverLoadIndex = 0;
};
