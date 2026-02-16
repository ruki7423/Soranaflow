#pragma once
#include <QWidget>
#include <QLabel>
#include <QPixmap>
#include "../../core/MusicData.h"

class NowPlayingInfo : public QWidget {
    Q_OBJECT
public:
    explicit NowPlayingInfo(QWidget* parent = nullptr);

    void setTrack(const Track& track);
    void setAutoplayVisible(bool visible);

public slots:
    void onCoverArtReady(const QString& trackPath, const QPixmap& pixmap);
    void refreshTheme();

signals:
    void subtitleClicked();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void updateSignalPath(const Track& track);

    QLabel* m_coverArtLabel = nullptr;
    QLabel* m_trackTitleLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QWidget* m_signalPathDot = nullptr;
    QLabel* m_formatLabel = nullptr;
    QLabel* m_autoplayLabel = nullptr;
    QString m_currentTrackPath;
};
