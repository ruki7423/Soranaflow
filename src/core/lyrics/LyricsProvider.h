#pragma once
#include <QObject>
#include <QString>
#include <QList>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

struct LyricLine {
    qint64 timestampMs = -1;  // -1 = unsynced
    QString text;
};

class LyricsProvider : public QObject
{
    Q_OBJECT
public:
    explicit LyricsProvider(QObject* parent = nullptr);

    void fetchLyrics(const QString& filePath,
                     const QString& title,
                     const QString& artist,
                     const QString& album,
                     int durationSec);

    void clear();

    QList<LyricLine> lyrics() const { return m_lyrics; }
    bool isSynced() const { return m_synced; }

signals:
    void lyricsReady(const QList<LyricLine>& lyrics, bool synced);
    void lyricsNotFound();

private:
    QList<LyricLine> readEmbeddedLyrics(const QString& filePath);
    QList<LyricLine> readLrcFile(const QString& audioFilePath);
    void fetchFromLrclib(const QString& title,
                         const QString& artist,
                         const QString& album,
                         int durationSec);

    QList<LyricLine> parseLrc(const QString& lrcText);
    QList<LyricLine> parsePlainText(const QString& text);

    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_pendingReply = nullptr;
    QList<LyricLine> m_lyrics;
    bool m_synced = false;
};
