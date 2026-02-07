#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPixmap>
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/FormatBadge.h"
#include "../../widgets/TrackTableView.h"
#include "../../core/MusicData.h"
#include "../../core/PlaybackState.h"

class QNetworkAccessManager;
class QNetworkReply;

class ArtistDetailView : public QWidget {
    Q_OBJECT
public:
    explicit ArtistDetailView(QWidget* parent = nullptr);
    void setArtist(const QString& artistId);

signals:
    void backRequested();
    void albumSelected(const QString& albumId);

private:
    void setupUI();
    void updateDisplay();
    void loadArtistImage();
    QPixmap findAlbumCoverArt(const Album& album);
    void refreshTheme();

    void fetchFanartImages();
    void fetchBiography();
    void onArtistThumbDownloaded(const QString& mbid, const QPixmap& pix, const QString& path);
    void onArtistBackgroundDownloaded(const QString& mbid, const QPixmap& pix, const QString& path);
    void applyCircularPixmap(const QPixmap& pix);
    void applyHeroPixmap(const QPixmap& pix);
    void applyAlbumArtFallback();
    void fetchLastFmBio(const QString& artistName);

    Artist m_artist;
    QString m_artistMbid;
    bool m_heroFromFanart = false;   // true = fanart.tv bg, false = album art fallback

    QNetworkAccessManager* m_network;
    QNetworkReply* m_pendingLastFmReply = nullptr;

    // Hero background
    QLabel* m_heroBackground;

    // Header
    StyledButton* m_backBtn;
    QLabel* m_artistImage;
    QLabel* m_nameLabel;
    QLabel* m_statsLabel;
    QWidget* m_genreBadgesContainer;

    // Action buttons
    StyledButton* m_playAllBtn;
    StyledButton* m_shuffleBtn;

    // Popular tracks
    TrackTableView* m_popularTracksTable;

    // Biography
    QLabel* m_bioHeader;
    QLabel* m_bioLabel;

    // Albums grid
    QWidget* m_albumsContainer;
    QGridLayout* m_albumsGridLayout;

    StyledScrollArea* m_scrollArea;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
};
