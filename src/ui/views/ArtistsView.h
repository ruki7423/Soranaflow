#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QShowEvent>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include "../../widgets/StyledInput.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/StyledButton.h"
#include "../../core/MusicData.h"

class ArtistsView : public QWidget {
    Q_OBJECT
public:
    explicit ArtistsView(QWidget* parent = nullptr);

    enum ViewMode { LargeIcons, SmallIcons, ListView };

signals:
    void artistSelected(const QString& artistId);

private slots:
    void onSearchChanged(const QString& text);
    void onLibraryUpdated();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUI();
    void populateArtists();
    QWidget* createArtistCard(const Artist& artist, int cardWidth);
    QWidget* createArtistListRow(const Artist& artist);
    void refreshTheme();
    QPixmap findArtistCoverArt(const Artist& artist);
    void relayoutGrid();
    void setViewMode(ViewMode mode);
    void loadNextCoverBatch();
    static QPixmap renderCircularCover(const QPixmap& src, int size);

    ViewMode m_viewMode = LargeIcons;
    StyledInput* m_searchInput;
    QWidget* m_gridContainer;
    QGridLayout* m_gridLayout;
    StyledScrollArea* m_scrollArea;
    QVector<QWidget*> m_artistCards;
    QVector<Artist> m_artists;
    QLabel* m_headerLabel;
    QLabel* m_countLabel;
    bool m_firstShow = true;
    StyledButton* m_largeIconBtn = nullptr;
    StyledButton* m_smallIconBtn = nullptr;
    StyledButton* m_listBtn = nullptr;
    QPushButton* m_navBackBtn = nullptr;
    QPushButton* m_navForwardBtn = nullptr;

    // Cover art cache + async loading
    QHash<QString, QPixmap> m_coverCache;
    QHash<QString, QPointer<QLabel>> m_coverLabels;
    int m_coverLoadIndex = 0;
    QTimer* m_resizeDebounceTimer = nullptr;
    QTimer* m_searchDebounceTimer = nullptr;
    bool m_libraryDirty = false;
};
