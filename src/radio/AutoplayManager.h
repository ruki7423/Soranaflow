#pragma once

#include <QObject>
#include <QSet>
#include <QQueue>
#include <QPair>
#include "MusicData.h"

class LastFmProvider;

class AutoplayManager : public QObject {
    Q_OBJECT
public:
    static AutoplayManager* instance();

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void requestNextTrack(const QString& artist, const QString& title);

signals:
    void trackRecommended(const Track& track);
    void noRecommendation();

private:
    explicit AutoplayManager(QObject* parent = nullptr);

    void onSimilarTracksFetched(const QList<QPair<QString, QString>>& tracks);
    void onSimilarArtistsFetched(const QStringList& artists);
    void onFetchError(const QString& error);
    void tryLocalFallback();
    Track pickFromArtist(const QString& artist);
    bool isRecentlyPlayed(const QString& trackId);
    void addToRecentlyPlayed(const QString& trackId);

    QSet<QString> m_recentlyPlayed;
    QQueue<QString> m_recentOrder;
    int m_fallbackStage = 0;
    QString m_currentArtist;
    QString m_currentTitle;
    LastFmProvider* m_lastFm;
    bool m_enabled = false;

    static const int MAX_RECENT = 50;
};
