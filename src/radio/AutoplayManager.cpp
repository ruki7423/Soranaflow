#include "AutoplayManager.h"
#include "LastFmProvider.h"
#include "MusicData.h"
#include "Settings.h"

#include <QDebug>
#include <QRandomGenerator>

// ── Singleton ───────────────────────────────────────────────────────
AutoplayManager* AutoplayManager::instance()
{
    static AutoplayManager s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
AutoplayManager::AutoplayManager(QObject* parent)
    : QObject(parent)
    , m_lastFm(LastFmProvider::instance())
{
    connect(m_lastFm, &LastFmProvider::similarTracksFetched,
            this, &AutoplayManager::onSimilarTracksFetched);
    connect(m_lastFm, &LastFmProvider::similarArtistsFetched,
            this, &AutoplayManager::onSimilarArtistsFetched);
    connect(m_lastFm, &LastFmProvider::fetchError,
            this, &AutoplayManager::onFetchError);
}

// ── setEnabled ──────────────────────────────────────────────────────
void AutoplayManager::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

// ── requestNextTrack ────────────────────────────────────────────────
void AutoplayManager::requestNextTrack(const QString& artist, const QString& title)
{
    if (!m_enabled) {
        emit noRecommendation();
        return;
    }

    m_currentArtist = artist;
    m_currentTitle = title;
    m_fallbackStage = 0;

    qDebug() << "[Autoplay] Requesting next track for:" << artist << "-" << title;

    // Stage 0: Try similar tracks via Last.fm
    m_lastFm->fetchSimilarTracks(artist, title);
}

// ── Stage 0: Similar tracks from Last.fm ────────────────────────────
void AutoplayManager::onSimilarTracksFetched(const QList<QPair<QString, QString>>& tracks)
{
    if (m_fallbackStage != 0) return;

    const auto allTracks = MusicDataProvider::instance()->allTracks();

    for (const auto& [simArtist, simTitle] : tracks) {
        for (const Track& t : allTracks) {
            if (t.artist.compare(simArtist, Qt::CaseInsensitive) == 0 &&
                t.title.compare(simTitle, Qt::CaseInsensitive) == 0 &&
                !isRecentlyPlayed(t.id)) {
                qDebug() << "[Autoplay] Stage 0 match:" << t.artist << "-" << t.title;
                addToRecentlyPlayed(t.id);
                emit trackRecommended(t);
                return;
            }
        }
    }

    qDebug() << "[Autoplay] Stage 0: no library match, trying similar artists";
    m_fallbackStage = 1;
    m_lastFm->fetchSimilarArtists(m_currentArtist);
}

// ── Stage 1: Similar artists from Last.fm ───────────────────────────
void AutoplayManager::onSimilarArtistsFetched(const QStringList& artists)
{
    if (m_fallbackStage != 1) return;

    for (const QString& simArtist : artists) {
        Track pick = pickFromArtist(simArtist);
        if (!pick.id.isEmpty()) {
            qDebug() << "[Autoplay] Stage 1 match:" << pick.artist << "-" << pick.title;
            addToRecentlyPlayed(pick.id);
            emit trackRecommended(pick);
            return;
        }
    }

    qDebug() << "[Autoplay] Stage 1: no artist match, trying local fallback";
    m_fallbackStage = 2;
    tryLocalFallback();
}

// ── Stage 2: Local library fallback ─────────────────────────────────
void AutoplayManager::tryLocalFallback()
{
    // First: try same artist
    Track pick = pickFromArtist(m_currentArtist);
    if (!pick.id.isEmpty()) {
        qDebug() << "[Autoplay] Stage 2 same-artist match:" << pick.artist << "-" << pick.title;
        addToRecentlyPlayed(pick.id);
        emit trackRecommended(pick);
        return;
    }

    // Second: random from full library
    const auto allTracks = MusicDataProvider::instance()->allTracks();
    if (allTracks.isEmpty()) {
        qDebug() << "[Autoplay] No tracks in library";
        emit noRecommendation();
        return;
    }

    // Collect non-recently-played candidates
    QVector<int> candidates;
    for (int i = 0; i < allTracks.size(); ++i) {
        if (!isRecentlyPlayed(allTracks[i].id))
            candidates.append(i);
    }

    if (candidates.isEmpty()) {
        // All tracks recently played — clear history and pick random
        m_recentlyPlayed.clear();
        m_recentOrder.clear();
        candidates.resize(allTracks.size());
        for (int i = 0; i < allTracks.size(); ++i)
            candidates[i] = i;
    }

    int idx = candidates[QRandomGenerator::global()->bounded(candidates.size())];
    const Track& t = allTracks[idx];
    qDebug() << "[Autoplay] Stage 2 random:" << t.artist << "-" << t.title;
    addToRecentlyPlayed(t.id);
    emit trackRecommended(t);
}

// ── Fetch error → skip to next fallback stage ───────────────────────
void AutoplayManager::onFetchError(const QString& error)
{
    qDebug() << "[Autoplay] Fetch error at stage" << m_fallbackStage << ":" << error;

    if (m_fallbackStage == 0) {
        m_fallbackStage = 1;
        m_lastFm->fetchSimilarArtists(m_currentArtist);
    } else if (m_fallbackStage == 1) {
        m_fallbackStage = 2;
        tryLocalFallback();
    }
}

// ── pickFromArtist ──────────────────────────────────────────────────
Track AutoplayManager::pickFromArtist(const QString& artist)
{
    const auto allTracks = MusicDataProvider::instance()->allTracks();
    QVector<int> candidates;

    for (int i = 0; i < allTracks.size(); ++i) {
        if (allTracks[i].artist.compare(artist, Qt::CaseInsensitive) == 0 &&
            !isRecentlyPlayed(allTracks[i].id)) {
            candidates.append(i);
        }
    }

    if (candidates.isEmpty())
        return Track();

    int idx = candidates[QRandomGenerator::global()->bounded(candidates.size())];
    return allTracks[idx];
}

// ── Recently played tracking ────────────────────────────────────────
bool AutoplayManager::isRecentlyPlayed(const QString& trackId)
{
    return m_recentlyPlayed.contains(trackId);
}

void AutoplayManager::addToRecentlyPlayed(const QString& trackId)
{
    if (m_recentlyPlayed.contains(trackId))
        return;

    m_recentlyPlayed.insert(trackId);
    m_recentOrder.enqueue(trackId);

    while (m_recentOrder.size() > MAX_RECENT) {
        QString oldest = m_recentOrder.dequeue();
        m_recentlyPlayed.remove(oldest);
    }
}
