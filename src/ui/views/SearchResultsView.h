#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QEvent>
#include <QPointer>
#include <QHash>
#include <QPixmap>
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/TrackTableView.h"
#include "../../core/MusicData.h"

class SearchResultsView : public QWidget {
    Q_OBJECT
public:
    explicit SearchResultsView(QWidget* parent = nullptr);

    void setResults(const QString& query,
                    const QVector<Artist>& artists,
                    const QVector<Album>& albums,
                    const QVector<Track>& tracks);
    void clearResults();

signals:
    void artistClicked(const QString& artistId);
    void albumClicked(const QString& albumId);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void refreshTheme();

private:
    void setupUI();
    void buildArtistCards(const QVector<Artist>& artists);
    void buildAlbumCards(const QVector<Album>& albums);
    QPixmap loadAlbumCover(const Album& album);
    void loadNextCoverBatch();

    StyledScrollArea* m_scrollArea;
    QWidget* m_contentWidget;
    QVBoxLayout* m_contentLayout;

    QLabel* m_queryLabel;
    QLabel* m_emptyLabel;

    // Sections
    QLabel* m_artistsHeader;
    QWidget* m_artistsContainer;
    QLabel* m_albumsHeader;
    QWidget* m_albumsContainer;
    QLabel* m_tracksHeader;
    TrackTableView* m_trackTable;

    QString m_lastQuery;
    QVector<Track> m_searchTracks;

    // Async cover loading
    QHash<QString, QPixmap> m_albumCoverCache;
    QHash<QString, QPointer<QLabel>> m_pendingCoverLabels;
    QVector<Album> m_pendingAlbums;
    int m_coverLoadIndex = 0;
};
