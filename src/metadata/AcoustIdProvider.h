#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include "MusicBrainzProvider.h"

class RateLimiter;

class AcoustIdProvider : public QObject {
    Q_OBJECT
public:
    static AcoustIdProvider* instance();

    void lookup(const QString& fingerprint, int duration, const QString& trackId = QString());

signals:
    void trackIdentified(const MusicBrainzResult& result, const QString& trackId);
    void noMatch(const QString& trackId);
    void lookupError(const QString& error, const QString& trackId);

private:
    explicit AcoustIdProvider(QObject* parent = nullptr);

    void performLookup(const QString& fingerprint, int duration, const QString& trackId);

    QNetworkAccessManager* m_nam;
    RateLimiter* m_rateLimiter;
    QString m_apiKey;
};
