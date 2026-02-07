#pragma once
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include "../../widgets/FormatBadge.h"
#include "../../widgets/StyledButton.h"
#include "../../widgets/StyledScrollArea.h"
#include "../../widgets/SignalPathWidget.h"
#include "../../widgets/LyricsWidget.h"
#include "../../core/PlaybackState.h"
#include "../../core/MusicData.h"
#include "../../core/lyrics/LyricsProvider.h"

class NowPlayingView : public QWidget {
    Q_OBJECT
public:
    explicit NowPlayingView(QWidget* parent = nullptr);

signals:
    void artistClicked(const QString& artistId);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onTrackChanged(const Track& track);
    void onQueueChanged();
    void onSignalPathChanged();
    void onPositionChanged(double secs);
    void onLyricsReady(const QList<LyricLine>& lyrics, bool synced);
    void onLyricsNotFound();

private:
    void setupUI();
    void updateTrackDisplay();
    void updateQueueList();
    void refreshTheme();

    // Left - Album art
    QLabel* m_albumArt;

    // Center - Track info
    QLabel* m_titleLabel;
    QLabel* m_artistLabel;
    QLabel* m_albumLabel;
    QWidget* m_formatContainer;
    QWidget* m_metadataContainer;
    SignalPathWidget* m_signalPathWidget;

    // Lyrics (no resize handle - fixed layout)
    QWidget* m_leftColumn;
    QLabel* m_lyricsHeader;
    LyricsWidget* m_lyricsWidget;
    LyricsProvider* m_lyricsProvider;

    // Right - Queue
    QWidget* m_queueContainer;
    QVBoxLayout* m_queueLayout;
    QLabel* m_queueTitle;

    QVector<Track> m_cachedDisplayQueue;
};
