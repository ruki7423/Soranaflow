#include "LyricsProvider.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <climits>

// ═════════════════════════════════════════════════════════════════════
//  Constructor
// ═════════════════════════════════════════════════════════════════════

LyricsProvider::LyricsProvider(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

// ═════════════════════════════════════════════════════════════════════
//  fetchLyrics — try all sources in priority order
// ═════════════════════════════════════════════════════════════════════

void LyricsProvider::fetchLyrics(const QString& filePath,
                                  const QString& title,
                                  const QString& artist,
                                  const QString& album,
                                  int durationSec)
{
    // Cancel any in-flight network request
    // Null m_pendingReply BEFORE abort() — abort fires finished()
    // synchronously, and the callback sets m_pendingReply = nullptr,
    // so calling deleteLater() after abort() would crash on nullptr.
    if (m_pendingReply) {
        QNetworkReply* old = m_pendingReply;
        m_pendingReply = nullptr;
        old->abort();
        old->deleteLater();
        qDebug() << "[Lyrics] Cancelled pending request";
    }

    m_lyrics.clear();
    m_synced = false;

    // 1. Try embedded lyrics from file metadata
    if (!filePath.isEmpty()) {
        auto embedded = readEmbeddedLyrics(filePath);
        if (!embedded.isEmpty()) {
            m_lyrics = embedded;
            m_synced = (embedded.first().timestampMs >= 0);
            emit lyricsReady(m_lyrics, m_synced);
            qDebug() << "[Lyrics] Source: embedded,"
                     << m_lyrics.size() << "lines, synced:" << m_synced;
            return;
        }
    }

    // 2. Try sidecar .lrc file
    if (!filePath.isEmpty()) {
        auto lrc = readLrcFile(filePath);
        if (!lrc.isEmpty()) {
            m_lyrics = lrc;
            m_synced = true;
            emit lyricsReady(m_lyrics, m_synced);
            qDebug() << "[Lyrics] Source: LRC file,"
                     << m_lyrics.size() << "lines";
            return;
        }
    }

    // 3. Try LRCLIB online API
    if (!title.isEmpty()) {
        fetchFromLrclib(title, artist, album, durationSec);
    } else {
        emit lyricsNotFound();
    }
}

void LyricsProvider::clear()
{
    m_lyrics.clear();
    m_synced = false;
}

// ═════════════════════════════════════════════════════════════════════
//  readEmbeddedLyrics — extract from FFmpeg metadata
// ═════════════════════════════════════════════════════════════════════

QList<LyricLine> LyricsProvider::readEmbeddedLyrics(const QString& filePath)
{
    // Skip DSD files — they rarely have lyrics and FFmpeg metadata
    // access can crash on DSF/DFF containers
    QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext == QStringLiteral("dsf") || ext == QStringLiteral("dff")
        || ext == QStringLiteral("dsd")) {
        qDebug() << "[Lyrics] Skipping embedded read for DSD file:" << ext;
        return {};
    }

    AVFormatContext* fmtCtx = nullptr;
    QByteArray pathBytes = filePath.toUtf8();

    int ret = avformat_open_input(&fmtCtx, pathBytes.constData(), nullptr, nullptr);
    if (ret < 0 || !fmtCtx)
        return {};

    // Do NOT call avformat_find_stream_info — we only need the
    // container-level metadata dict, not stream analysis.
    // Metadata is available after avformat_open_input().

    // Check multiple tag names used by different formats
    static const char* tagNames[] = {
        "lyrics", "LYRICS", "UNSYNCEDLYRICS",
        "USLT", "SYLT", nullptr
    };

    QString lyricsText;
    if (fmtCtx->metadata) {
        for (int i = 0; tagNames[i] != nullptr; ++i) {
            AVDictionaryEntry* entry = av_dict_get(fmtCtx->metadata, tagNames[i], nullptr, 0);
            if (entry && entry->value && strlen(entry->value) > 0) {
                lyricsText = QString::fromUtf8(entry->value);
                break;
            }
        }
    }

    // Also check stream-level metadata (some MP3 files store USLT there)
    if (lyricsText.isEmpty() && fmtCtx->nb_streams > 0 && fmtCtx->streams) {
        for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
            if (!fmtCtx->streams[i]) continue;
            AVDictionary* streamMeta = fmtCtx->streams[i]->metadata;
            if (!streamMeta) continue;
            for (int j = 0; tagNames[j] != nullptr; ++j) {
                AVDictionaryEntry* entry = av_dict_get(streamMeta, tagNames[j], nullptr, 0);
                if (entry && entry->value && strlen(entry->value) > 0) {
                    lyricsText = QString::fromUtf8(entry->value);
                    break;
                }
            }
            if (!lyricsText.isEmpty()) break;
        }
    }

    avformat_close_input(&fmtCtx);

    if (lyricsText.isEmpty())
        return {};

    // Check if it contains LRC timestamps
    static QRegularExpression rxTimestamp(QStringLiteral(R"(\[\d{1,3}:\d{2})"));
    if (rxTimestamp.match(lyricsText).hasMatch()) {
        return parseLrc(lyricsText);
    }

    return parsePlainText(lyricsText);
}

// ═════════════════════════════════════════════════════════════════════
//  readLrcFile — sidecar .lrc next to audio file
// ═════════════════════════════════════════════════════════════════════

QList<LyricLine> LyricsProvider::readLrcFile(const QString& audioFilePath)
{
    QFileInfo fi(audioFilePath);
    QString lrcPath = fi.absolutePath() + QStringLiteral("/")
                    + fi.completeBaseName() + QStringLiteral(".lrc");

    QFile file(lrcPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QTextStream stream(&file);
    QString content = stream.readAll();
    file.close();

    auto result = parseLrc(content);
    if (result.isEmpty())
        return {};

    return result;
}

// ═════════════════════════════════════════════════════════════════════
//  fetchFromLrclib — LRCLIB API (async)
// ═════════════════════════════════════════════════════════════════════

void LyricsProvider::fetchFromLrclib(const QString& title,
                                      const QString& artist,
                                      const QString& album,
                                      int durationSec)
{
    // Detect if query title has non-ASCII (Japanese/Korean/etc.)
    bool queryHasNonAscii = false;
    for (const QChar& ch : title) {
        if (ch.unicode() > 127) { queryHasNonAscii = true; break; }
    }

    // Primary: exact match via /api/get
    QUrl url(QStringLiteral("https://lrclib.net/api/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("track_name"), title);
    if (!artist.isEmpty())
        query.addQueryItem(QStringLiteral("artist_name"), artist);
    if (!album.isEmpty())
        query.addQueryItem(QStringLiteral("album_name"), album);
    query.addQueryItem(QStringLiteral("duration"),
                       QString::number(durationSec));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("SoranaFlow/1.0 (https://github.com/sorana-flow)"));

    auto* reply = m_nam->get(req);
    m_pendingReply = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, title, artist, durationSec, queryHasNonAscii]() {
        reply->deleteLater();
        if (m_pendingReply == reply)
            m_pendingReply = nullptr;

        // Aborted — new fetch in progress, ignore this callback
        if (reply->error() == QNetworkReply::OperationCanceledError)
            return;

        // If exact match succeeded, use it
        if (reply->error() == QNetworkReply::NoError) {
            QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            QString synced = obj[QStringLiteral("syncedLyrics")].toString();
            QString plain = obj[QStringLiteral("plainLyrics")].toString();

            if (!synced.isEmpty()) {
                m_lyrics = parseLrc(synced);
                m_synced = true;
                emit lyricsReady(m_lyrics, m_synced);
                qDebug() << "[Lyrics] Source: LRCLIB get synced,"
                         << m_lyrics.size() << "lines";
                return;
            }
            if (!plain.isEmpty()) {
                m_lyrics = parsePlainText(plain);
                m_synced = false;
                emit lyricsReady(m_lyrics, m_synced);
                qDebug() << "[Lyrics] Source: LRCLIB get plain,"
                         << m_lyrics.size() << "lines";
                return;
            }
        }

        // Fallback: search endpoint
        QUrl searchUrl(QStringLiteral("https://lrclib.net/api/search"));
        QUrlQuery searchQuery;
        searchQuery.addQueryItem(QStringLiteral("track_name"), title);
        if (!artist.isEmpty())
            searchQuery.addQueryItem(QStringLiteral("artist_name"), artist);
        searchUrl.setQuery(searchQuery);

        QNetworkRequest searchReq(searchUrl);
        searchReq.setHeader(QNetworkRequest::UserAgentHeader,
                            QStringLiteral("SoranaFlow/1.0 (https://github.com/sorana-flow)"));

        auto* searchReply = m_nam->get(searchReq);
        m_pendingReply = searchReply;
        connect(searchReply, &QNetworkReply::finished, this,
                [this, searchReply, durationSec, queryHasNonAscii]() {
            searchReply->deleteLater();
            if (m_pendingReply == searchReply)
                m_pendingReply = nullptr;

            // Aborted — new fetch in progress
            if (searchReply->error() == QNetworkReply::OperationCanceledError)
                return;

            if (searchReply->error() != QNetworkReply::NoError) {
                qDebug() << "[Lyrics] LRCLIB search error:"
                         << searchReply->errorString();
                emit lyricsNotFound();
                return;
            }

            QJsonArray results = QJsonDocument::fromJson(
                searchReply->readAll()).array();
            if (results.isEmpty()) {
                qDebug() << "[Lyrics] LRCLIB: no results";
                emit lyricsNotFound();
                return;
            }

            // Find best match — score by language match + duration proximity
            QString bestSynced, bestPlain;
            int bestScore = INT_MIN;

            for (const auto& val : results) {
                QJsonObject obj = val.toObject();
                int dur = static_cast<int>(
                    obj[QStringLiteral("duration")].toDouble());
                int durDiff = std::abs(dur - durationSec);

                // Check if lyrics contain non-ASCII characters
                QString synced = obj[QStringLiteral("syncedLyrics")].toString();
                QString plain = obj[QStringLiteral("plainLyrics")].toString();
                const QString& textToCheck = synced.isEmpty() ? plain : synced;

                bool lyricsHasNonAscii = false;
                for (const QChar& ch : textToCheck) {
                    if (ch.unicode() > 127) {
                        lyricsHasNonAscii = true;
                        break;
                    }
                }

                // Language match bonus (1000 points) + duration penalty
                int score = -durDiff;
                if (queryHasNonAscii == lyricsHasNonAscii)
                    score += 1000;

                if (score > bestScore) {
                    bestScore = score;
                    bestSynced = synced;
                    bestPlain = plain;
                }
            }

            if (!bestSynced.isEmpty()) {
                m_lyrics = parseLrc(bestSynced);
                m_synced = true;
                emit lyricsReady(m_lyrics, m_synced);
                qDebug() << "[Lyrics] Source: LRCLIB search synced,"
                         << m_lyrics.size() << "lines";
            } else if (!bestPlain.isEmpty()) {
                m_lyrics = parsePlainText(bestPlain);
                m_synced = false;
                emit lyricsReady(m_lyrics, m_synced);
                qDebug() << "[Lyrics] Source: LRCLIB search plain,"
                         << m_lyrics.size() << "lines";
            } else {
                qDebug() << "[Lyrics] Not found";
                emit lyricsNotFound();
            }
        });
    });
}

// ═════════════════════════════════════════════════════════════════════
//  parseLrc — parse LRC format text
// ═════════════════════════════════════════════════════════════════════

QList<LyricLine> LyricsProvider::parseLrc(const QString& lrcText)
{
    QList<LyricLine> result;
    int offsetMs = 0;

    // Check for offset tag
    static QRegularExpression rxOffset(
        QStringLiteral(R"(\[offset:([+-]?\d+)\])"));
    auto offsetMatch = rxOffset.match(lrcText);
    if (offsetMatch.hasMatch())
        offsetMs = offsetMatch.captured(1).toInt();

    // Parse timestamped lines
    static QRegularExpression rxLine(
        QStringLiteral(R"(\[(\d{1,3}):(\d{2})(?:[\.:](\d{1,3}))?\]\s*(.*))"));

    const QStringList lines = lrcText.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        // Skip metadata tags like [ti:], [ar:], [al:], [by:]
        if (line.contains(QRegularExpression(
                QStringLiteral(R"(^\[(ti|ar|al|by|offset|re|ve):)"))))
            continue;

        auto m = rxLine.match(line);
        if (m.hasMatch()) {
            int min = m.captured(1).toInt();
            int sec = m.captured(2).toInt();
            int ms = 0;
            if (!m.captured(3).isEmpty()) {
                QString msStr = m.captured(3);
                if (msStr.length() == 2)
                    ms = msStr.toInt() * 10;  // [00:12.50] → 500ms
                else
                    ms = msStr.toInt();        // [00:12.500] → 500ms
            }
            qint64 timestamp = static_cast<qint64>(min) * 60000
                             + sec * 1000 + ms + offsetMs;
            QString text = m.captured(4).trimmed();
            if (!text.isEmpty())
                result.append({timestamp, text});
        }
    }

    std::sort(result.begin(), result.end(),
              [](const LyricLine& a, const LyricLine& b) {
                  return a.timestampMs < b.timestampMs;
              });

    return result;
}

// ═════════════════════════════════════════════════════════════════════
//  parsePlainText — split into unsynced lines
// ═════════════════════════════════════════════════════════════════════

QList<LyricLine> LyricsProvider::parsePlainText(const QString& text)
{
    QList<LyricLine> result;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            result.append({-1, trimmed});
    }
    return result;
}
