#include "AppleMusicManager.h"
#include "MusicKitPlayer.h"
#include "Settings.h"

#include <QDebug>
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QLocale>

#import <Foundation/Foundation.h>

// Import the generated Swift header (compiled by swiftc -emit-objc-header-path)
#import "SoranaFlow-Swift.h"

// ═══════════════════════════════════════════════════════════════════
//  Private implementation
// ═══════════════════════════════════════════════════════════════════

class AppleMusicManagerPrivate
{
public:
    AppleMusicManager::AuthStatus authStatus = AppleMusicManager::NotDetermined;
    bool subscription = false;
    QString developerToken;
    QString storefront;
    bool nativeSearchFailed = false;
};

// ═══════════════════════════════════════════════════════════════════
//  Singleton
// ═══════════════════════════════════════════════════════════════════

AppleMusicManager* AppleMusicManager::s_instance = nullptr;

AppleMusicManager* AppleMusicManager::instance()
{
    if (!s_instance)
        s_instance = new AppleMusicManager();
    return s_instance;
}

// ═══════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════

AppleMusicManager::AppleMusicManager(QObject* parent)
    : QObject(parent)
    , d(new AppleMusicManagerPrivate)
    , m_network(new QNetworkAccessManager(this))
{
    m_network->setTransferTimeout(15000);  // 15s timeout for all REST API requests

    // Detect storefront from locale (e.g., "us", "jp", "gb")
    QString country = QLocale::system().name().section('_', 1).toLower();
    d->storefront = country.isEmpty() ? QStringLiteral("us") : country;
    qDebug() << "AppleMusicManager: Storefront:" << d->storefront;

    // Restore persisted token from previous session
    QString savedToken = Settings::instance()->value(QStringLiteral("appleMusic/userToken")).toString();
    if (!savedToken.isEmpty()) {
        qDebug() << "[AppleMusicManager] Restored saved token, length:" << savedToken.length();
        m_musicUserToken = savedToken;
        d->authStatus = Authorized;

        // Inject into MusicKitPlayer (triggers WebView pre-warm)
        QTimer::singleShot(0, this, [this, savedToken]() {
            MusicKitPlayer::instance()->injectMusicUserToken(savedToken);
            emit authorizationStatusChanged(d->authStatus);
            emit musicUserTokenReady(savedToken);
        });
    } else {
        d->authStatus = NotDetermined;
        qDebug() << "[AppleMusicManager] No saved token — manual Connect required";
    }
}

AppleMusicManager::~AppleMusicManager()
{
    delete d;
}

// ═══════════════════════════════════════════════════════════════════
//  Getters
// ═══════════════════════════════════════════════════════════════════

AppleMusicManager::AuthStatus AppleMusicManager::authorizationStatus() const
{
    return d->authStatus;
}

bool AppleMusicManager::isAuthorized() const
{
    return d->authStatus == Authorized;
}

bool AppleMusicManager::hasSubscription() const
{
    return d->subscription;
}

bool AppleMusicManager::hasDeveloperToken() const
{
    return !d->developerToken.isEmpty();
}

QString AppleMusicManager::developerToken() const
{
    return d->developerToken;
}

QString AppleMusicManager::storefront() const
{
    return d->storefront;
}

void AppleMusicManager::setStorefront(const QString& storefront)
{
    d->storefront = storefront;
}

// ═══════════════════════════════════════════════════════════════════
//  loadDeveloperCredentials — read .p8 key and generate JWT
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::loadDeveloperCredentials(const QString& teamId,
                                                  const QString& keyId,
                                                  const QString& privateKeyPath)
{
#ifdef MUSICKIT_DEVELOPER_TOKEN
    // Tier 0: Compile-time token from cmake / deploy.sh
    d->developerToken = QStringLiteral(MUSICKIT_DEVELOPER_TOKEN);
    qDebug() << "[AppleMusicManager] Using compile-time developer token ("
             << d->developerToken.length() << "chars)";
    Q_UNUSED(teamId);
    Q_UNUSED(keyId);
    Q_UNUSED(privateKeyPath);
#else
    // Tier 1: Try runtime generation from .p8 file (dev builds)
    if (!privateKeyPath.isEmpty()) {
        QFile keyFile(privateKeyPath);
        if (keyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString pemContents = QString::fromUtf8(keyFile.readAll());
            keyFile.close();

            if (!pemContents.isEmpty()) {
                qDebug() << "[AppleMusicManager] Loaded private key from" << privateKeyPath;

                @autoreleasepool {
                    NSString* nsTeamId = teamId.toNSString();
                    NSString* nsKeyId  = keyId.toNSString();
                    NSString* nsPem    = pemContents.toNSString();

                    NSString* token = [MusicKitSwiftBridge generateDeveloperTokenWithTeamId:nsTeamId
                                                                                      keyId:nsKeyId
                                                                              privateKeyPEM:nsPem];
                    if (token) {
                        d->developerToken = QString::fromNSString(token);
                        qDebug() << "[AppleMusicManager] Developer token generated from .p8 ("
                                 << d->developerToken.length() << "chars)";
                    } else {
                        qWarning() << "[AppleMusicManager] Failed to generate token from .p8";
                    }
                }
            }
        } else {
            qDebug() << "[AppleMusicManager] .p8 not found at:" << privateKeyPath;
        }
    }

    // Tier 2: Embedded pre-generated token (distributed builds without .p8)
    if (d->developerToken.isEmpty()) {
        qDebug() << "[AppleMusicManager] .p8 not available — using embedded token";
        d->developerToken = QStringLiteral(
            "eyJhbGciOiJFUzI1NiIsImtpZCI6IjRHVzY2ODZDSDQiLCJ0eXAiOiJKV1QifQ."
            "eyJpc3MiOiJXNUpNUEpYQjVIIiwiaWF0IjoxNzcwNzI5MjUzLCJleHAiOjE3ODYyODEyNTN9."
            "OskqwBYwLw_wbBSwz0fqZI9VjZSWW1w6rZaLzqMZJFMvhxdbQccR2KZMqHRhv6muuZ_Mcj5M8PKx2qbvYczcDg");
    }
#endif

    // Token expiry check (applies to all tiers)
    if (!d->developerToken.isEmpty()) {
        QStringList parts = d->developerToken.split(QLatin1Char('.'));
        if (parts.size() >= 2) {
            QByteArray payload = QByteArray::fromBase64(
                parts[1].toUtf8(), QByteArray::Base64UrlEncoding);
            QJsonObject obj = QJsonDocument::fromJson(payload).object();
            qint64 exp = obj[QStringLiteral("exp")].toInteger();
            qint64 now = QDateTime::currentSecsSinceEpoch();
            qint64 daysLeft = (exp - now) / 86400;
            if (daysLeft < 30) {
                qWarning() << "[AppleMusicManager] WARNING: Developer token expires in"
                           << daysLeft << "days — rebuild with fresh token needed soon";
            }
            qDebug() << "[AppleMusicManager] Developer token valid for" << daysLeft << "days";
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  disconnectAppleMusic
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::disconnectAppleMusic()
{
    qDebug() << "[AppleMusicManager] Disconnecting Apple Music";
    m_musicUserToken.clear();
    Settings::instance()->remove(QStringLiteral("appleMusic/userToken"));
    d->authStatus = NotDetermined;
    emit authorizationStatusChanged(d->authStatus);

    // Clear MusicKitPlayer token + JS state
    auto* player = MusicKitPlayer::instance();
    player->clearMusicUserToken();
}

// ═══════════════════════════════════════════════════════════════════
//  requestAuthorization
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::requestAuthorization()
{
    @autoreleasepool {
        [MusicKitSwiftBridge requestAuthorizationWithCompletion:^(NSInteger rawStatus) {
            d->authStatus = static_cast<AuthStatus>(static_cast<int>(rawStatus));
            qDebug() << "AppleMusicManager: Auth status changed to" << static_cast<int>(d->authStatus);

            QMetaObject::invokeMethod(this, [this]() {
                emit authorizationStatusChanged(d->authStatus);
            }, Qt::QueuedConnection);

            // Automatically check subscription and re-request token after authorization
            if (d->authStatus == Authorized) {
                checkSubscriptionStatus();
                // Re-request Music User Token so MusicKit JS gets full playback access
                QMetaObject::invokeMethod(this, [this]() {
                    requestMusicUserToken();
                }, Qt::QueuedConnection);
            }
        }];
    }
}

// ═══════════════════════════════════════════════════════════════════
//  checkSubscriptionStatus
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::checkSubscriptionStatus()
{
    @autoreleasepool {
        [MusicKitSwiftBridge checkSubscriptionWithCompletion:^(BOOL canPlay) {
            d->subscription = canPlay;
            qDebug() << "AppleMusicManager: Subscription canPlay:" << canPlay;

            QMetaObject::invokeMethod(this, [this]() {
                emit subscriptionStatusChanged(d->subscription);
            }, Qt::QueuedConnection);
        }];
    }
}

// ═══════════════════════════════════════════════════════════════════
//  requestMusicUserToken — get user token from native MusicKit
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::requestMusicUserToken()
{
    qDebug() << "[AppleMusicManager] Requesting Music User Token...";

    QString devToken = d->developerToken;
    if (devToken.isEmpty()) {
        qDebug() << "[AppleMusicManager] ERROR: No developer token available";
        emit musicUserTokenFailed(QStringLiteral("No developer token available"));
        return;
    }

    qDebug() << "[AppleMusicManager] Developer token length:" << devToken.length();

    @autoreleasepool {
        NSString* devTokenNS = devToken.toNSString();

        [MusicKitSwiftBridge getUserTokenWithDeveloperToken:devTokenNS
            completion:^(NSString* token, NSError* error) {
                if (error) {
                    QString errorMsg = QString::fromNSString(error.localizedDescription);
                    qDebug() << "[AppleMusicManager] Token request failed:" << errorMsg;
                    QMetaObject::invokeMethod(this, [this, errorMsg]() {
                        emit musicUserTokenFailed(errorMsg);
                    }, Qt::QueuedConnection);
                    return;
                }

                if (!token || token.length == 0) {
                    qDebug() << "[AppleMusicManager] Token is empty";
                    QMetaObject::invokeMethod(this, [this]() {
                        emit musicUserTokenFailed(QStringLiteral("Received empty token"));
                    }, Qt::QueuedConnection);
                    return;
                }

                QString tokenStr = QString::fromNSString(token);
                qDebug() << "[AppleMusicManager] Music User Token obtained, length:" << tokenStr.length();

                // All state mutations must happen on the main thread —
                // this Swift callback may arrive on an arbitrary queue.
                QMetaObject::invokeMethod(this, [this, tokenStr]() {
                    m_musicUserToken = tokenStr;

                    // Persist token so reconnect is instant on next launch
                    Settings::instance()->setValue(QStringLiteral("appleMusic/userToken"), tokenStr);
                    qDebug() << "[AppleMusicManager] Token persisted to Settings";

                    // Token success implies authorized — update status so UI shows "Connected"
                    if (d->authStatus != Authorized) {
                        d->authStatus = Authorized;
                        emit authorizationStatusChanged(d->authStatus);
                    }

                    emit musicUserTokenReady(tokenStr);
                }, Qt::QueuedConnection);
            }];
    }
}

QString AppleMusicManager::musicUserToken() const
{
    return m_musicUserToken;
}

// ═══════════════════════════════════════════════════════════════════
//  searchCatalog — tries native MusicKit, falls back to REST API
// ═══════════════════════════════════════════════════════════════════

static QJsonArray parseJsonString(NSString* nsStr)
{
    if (!nsStr) return {};
    QString str = QString::fromNSString(nsStr);
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(str.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "AppleMusicManager: JSON parse error:" << err.errorString();
        return {};
    }
    return doc.array();
}

void AppleMusicManager::searchCatalog(const QString& term, int limit)
{
    if (!isAuthorized()) {
        emit errorOccurred(QStringLiteral("Not authorized for Apple Music"));
        return;
    }

    // If native search previously failed and we have a developer token, go straight to REST API
    if (d->nativeSearchFailed && !d->developerToken.isEmpty()) {
        searchViaRestApi(term, limit);
        return;
    }

    // If we have no developer token and native hasn't been tried, try native first
    if (!d->nativeSearchFailed) {
        @autoreleasepool {
            NSString* nsTerm = term.toNSString();

            // Capture term and limit for fallback
            QString capturedTerm = term;
            int capturedLimit = limit;

            [MusicKitSwiftBridge searchCatalogWithTerm:nsTerm
                                                 limit:limit
                                            completion:^(NSString* songsJson,
                                                         NSString* albumsJson,
                                                         NSString* artistsJson,
                                                         NSString* errorStr) {
                if (errorStr) {
                    QString err = QString::fromNSString(errorStr);
                    qDebug() << "AppleMusicManager: Native search unavailable:" << err
                             << "(using REST API)";

                    // Mark native search as failed for future calls
                    d->nativeSearchFailed = true;

                    // Try REST API fallback if we have a developer token
                    if (!d->developerToken.isEmpty()) {
                        qDebug() << "AppleMusicManager: Falling back to REST API search";
                        QMetaObject::invokeMethod(this, [this, capturedTerm, capturedLimit]() {
                            searchViaRestApi(capturedTerm, capturedLimit);
                        }, Qt::QueuedConnection);
                    } else {
                        QMetaObject::invokeMethod(this, [this, err]() {
                            emit errorOccurred(err + QStringLiteral("\n\nTo fix: place your AuthKey .p8 file in the project directory "
                                "and restart the app, or install a Developer ID certificate."));
                        }, Qt::QueuedConnection);
                    }
                    return;
                }

                QJsonArray songs   = parseJsonString(songsJson);
                QJsonArray albums  = parseJsonString(albumsJson);
                QJsonArray artists = parseJsonString(artistsJson);

                qDebug() << "AppleMusicManager: Native search results —"
                         << songs.size() << "songs,"
                         << albums.size() << "albums,"
                         << artists.size() << "artists";

                QMetaObject::invokeMethod(this, [this, songs, albums, artists]() {
                    emit searchResultsReady(songs, albums, artists);
                }, Qt::QueuedConnection);
            }];
        }
        return;
    }

    // No developer token and native failed
    emit errorOccurred(QStringLiteral("Apple Music search unavailable. "
        "Place your AuthKey .p8 file in the project directory and restart the app."));
}

// ═══════════════════════════════════════════════════════════════════
//  REST API search — uses developer token + Apple Music API
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::searchViaRestApi(const QString& term, int limit)
{
    if (d->developerToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("No developer token available for REST API search"));
        return;
    }

    // Cancel any in-progress pagination from a previous search
    ++m_paginationId;
    if (m_paginationReply) {
        m_paginationReply->abort();
        m_paginationReply = nullptr;
    }
    m_collectedSongs = QJsonArray();
    m_collectedAlbums = QJsonArray();
    m_collectedArtists = QJsonArray();

    QUrl url(QStringLiteral("https://api.music.apple.com/v1/catalog/%1/search").arg(d->storefront));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("term"), term);
    query.addQueryItem(QStringLiteral("types"), QStringLiteral("songs,albums,artists"));
    query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    query.addQueryItem(QStringLiteral("include[songs]"), QStringLiteral("artists,albums"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(d->developerToken).toUtf8());

    qDebug() << "AppleMusicManager: REST API search for" << term
             << "storefront:" << d->storefront;

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleRestApiReply(reply);
    });
}

// Helper: parse song items from a "songs" object in API response
static QJsonArray parseSongsFromObject(const QJsonObject& songsObj)
{
    QJsonArray songs;
    for (const QJsonValue& val : songsObj[QStringLiteral("data")].toArray()) {
        QJsonObject item = val.toObject();
        QJsonObject attrs = item[QStringLiteral("attributes")].toObject();
        QJsonObject song;
        song[QStringLiteral("id")] = item[QStringLiteral("id")].toString();
        song[QStringLiteral("title")] = attrs[QStringLiteral("name")].toString();
        song[QStringLiteral("artist")] = attrs[QStringLiteral("artistName")].toString();
        song[QStringLiteral("album")] = attrs[QStringLiteral("albumName")].toString();
        song[QStringLiteral("isAppleMusic")] = true;

        int durationMs = attrs[QStringLiteral("durationInMillis")].toInt();
        if (durationMs > 0)
            song[QStringLiteral("duration")] = durationMs / 1000.0;

        QString artworkUrl = attrs[QStringLiteral("artwork")].toObject()
                                  [QStringLiteral("url")].toString();
        if (!artworkUrl.isEmpty()) {
            artworkUrl.replace(QStringLiteral("{w}"), QStringLiteral("300"));
            artworkUrl.replace(QStringLiteral("{h}"), QStringLiteral("300"));
            song[QStringLiteral("artworkUrl")] = artworkUrl;
        }

        // Extract artist/album IDs from relationships (available when include[songs]=artists,albums)
        QJsonObject rels = item[QStringLiteral("relationships")].toObject();
        QJsonArray artistsData = rels[QStringLiteral("artists")].toObject()
                                     [QStringLiteral("data")].toArray();
        if (!artistsData.isEmpty())
            song[QStringLiteral("artistId")] = artistsData[0].toObject()[QStringLiteral("id")].toString();

        QJsonArray albumsData = rels[QStringLiteral("albums")].toObject()
                                    [QStringLiteral("data")].toArray();
        if (!albumsData.isEmpty())
            song[QStringLiteral("albumId")] = albumsData[0].toObject()[QStringLiteral("id")].toString();

        songs.append(song);
    }
    return songs;
}

void AppleMusicManager::handleRestApiReply(QNetworkReply* reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = QStringLiteral("REST API error: %1 (HTTP %2)")
            .arg(reply->errorString())
            .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        qWarning() << "AppleMusicManager:" << err;

        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus == 401) {
            err += QStringLiteral("\nDeveloper token may be expired or invalid.");
        }

        emit errorOccurred(err);
        return;
    }

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        emit errorOccurred(QStringLiteral("Failed to parse API response: ") + parseErr.errorString());
        return;
    }

    QJsonObject root = doc.object();
    if (!root.contains(QStringLiteral("results"))) {
        emit errorOccurred(QStringLiteral("API response missing 'results' key"));
        return;
    }
    QJsonObject results = root[QStringLiteral("results")].toObject();

    // Parse songs (page 1)
    QJsonObject songsObj = results[QStringLiteral("songs")].toObject();
    QJsonArray songs = parseSongsFromObject(songsObj);

    qDebug() << "AppleMusicManager: Page 1 returned" << songs.size() << "songs";

    // Parse albums (no pagination)
    QJsonArray albums;
    QJsonObject albumsObj = results[QStringLiteral("albums")].toObject();
    for (const QJsonValue& val : albumsObj[QStringLiteral("data")].toArray()) {
        QJsonObject item = val.toObject();
        QJsonObject attrs = item[QStringLiteral("attributes")].toObject();
        QJsonObject album;
        album[QStringLiteral("id")] = item[QStringLiteral("id")].toString();
        album[QStringLiteral("title")] = attrs[QStringLiteral("name")].toString();
        album[QStringLiteral("artist")] = attrs[QStringLiteral("artistName")].toString();
        album[QStringLiteral("trackCount")] = attrs[QStringLiteral("trackCount")].toInt();
        album[QStringLiteral("isAppleMusic")] = true;

        QString artworkUrl = attrs[QStringLiteral("artwork")].toObject()
                                  [QStringLiteral("url")].toString();
        if (!artworkUrl.isEmpty()) {
            artworkUrl.replace(QStringLiteral("{w}"), QStringLiteral("300"));
            artworkUrl.replace(QStringLiteral("{h}"), QStringLiteral("300"));
            album[QStringLiteral("artworkUrl")] = artworkUrl;
        }

        albums.append(album);
    }

    // Parse artists (no pagination)
    QJsonArray artists;
    QJsonObject artistsObj = results[QStringLiteral("artists")].toObject();
    for (const QJsonValue& val : artistsObj[QStringLiteral("data")].toArray()) {
        QJsonObject item = val.toObject();
        QJsonObject attrs = item[QStringLiteral("attributes")].toObject();
        QJsonObject artist;
        artist[QStringLiteral("id")] = item[QStringLiteral("id")].toString();
        artist[QStringLiteral("name")] = attrs[QStringLiteral("name")].toString();

        QString artworkUrl = attrs[QStringLiteral("artwork")].toObject()
                                  [QStringLiteral("url")].toString();
        if (!artworkUrl.isEmpty()) {
            artworkUrl.replace(QStringLiteral("{w}"), QStringLiteral("300"));
            artworkUrl.replace(QStringLiteral("{h}"), QStringLiteral("300"));
            artist[QStringLiteral("artworkUrl")] = artworkUrl;
        }

        artists.append(artist);
    }

    // Store collected results
    m_collectedSongs = songs;
    m_collectedAlbums = albums;
    m_collectedArtists = artists;

    // Check if more song pages are available (max 4 pages = 100 songs)
    QString nextPath = songsObj[QStringLiteral("next")].toString();
    if (!nextPath.isEmpty() && songs.size() >= 25) {
        qDebug() << "AppleMusicManager: Fetching songs page 2 offset: 25";
        fetchNextSongsPage(nextPath, 2);
    } else {
        emitCollectedResults();
    }
}

void AppleMusicManager::fetchNextSongsPage(const QString& nextPath, int pageNum)
{
    int currentPaginationId = m_paginationId;

    QUrl url(QStringLiteral("https://api.music.apple.com") + nextPath);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(d->developerToken).toUtf8());

    m_paginationReply = m_network->get(request);
    connect(m_paginationReply, &QNetworkReply::finished, this,
            [this, pageNum, currentPaginationId]() {
        QNetworkReply* reply = m_paginationReply;
        m_paginationReply = nullptr;
        reply->deleteLater();

        // Check if this pagination was cancelled by a new search
        if (currentPaginationId != m_paginationId) {
            qDebug() << "AppleMusicManager: Pagination cancelled (new search started)";
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "AppleMusicManager: Pagination page" << pageNum
                       << "failed:" << reply->errorString();
            // Emit what we have so far
            emitCollectedResults();
            return;
        }

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            qWarning() << "AppleMusicManager: Pagination page" << pageNum
                       << "parse error:" << parseErr.errorString();
            emitCollectedResults();
            return;
        }

        QJsonObject root = doc.object();
        QJsonObject results = root[QStringLiteral("results")].toObject();
        QJsonObject songsObj = results[QStringLiteral("songs")].toObject();
        QJsonArray newSongs = parseSongsFromObject(songsObj);

        qDebug() << "AppleMusicManager: Page" << pageNum << "returned"
                 << newSongs.size() << "songs";

        // Append to collected songs
        for (const QJsonValue& s : newSongs)
            m_collectedSongs.append(s);

        // Check for more pages (max 4 pages total)
        QString nextPath = songsObj[QStringLiteral("next")].toString();
        if (!nextPath.isEmpty() && pageNum < 4 && newSongs.size() >= 25) {
            int nextOffset = pageNum * 25;
            qDebug() << "AppleMusicManager: Fetching songs page" << (pageNum + 1)
                     << "offset:" << nextOffset;
            fetchNextSongsPage(nextPath, pageNum + 1);
        } else {
            emitCollectedResults();
        }
    });
}

void AppleMusicManager::emitCollectedResults()
{
    qDebug() << "AppleMusicManager: REST API total results —"
             << m_collectedSongs.size() << "songs,"
             << m_collectedAlbums.size() << "albums,"
             << m_collectedArtists.size() << "artists";

    emit searchResultsReady(m_collectedSongs, m_collectedAlbums, m_collectedArtists);
}

// ═══════════════════════════════════════════════════════════════════
//  Artist discography — fetch all songs by artist ID
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::fetchArtistSongs(const QString& artistId)
{
    if (d->developerToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("No developer token for artist lookup"));
        return;
    }

    ++m_artistFetchId;
    m_artistSongs = QJsonArray();

    QString path = QStringLiteral("/v1/catalog/%1/artists/%2/view/top-songs?limit=100")
        .arg(d->storefront, artistId);

    qDebug() << "AppleMusicManager: Fetching all songs for artist:" << artistId;
    qDebug() << "AppleMusicManager: Fetching artist songs from:" << path;
    fetchArtistSongsPage(artistId, path, 1);
}

void AppleMusicManager::fetchArtistSongsPage(const QString& artistId,
                                               const QString& urlPath, int pageNum)
{
    int currentFetchId = m_artistFetchId;

    QUrl url(QStringLiteral("https://api.music.apple.com") + urlPath);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(d->developerToken).toUtf8());

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, artistId, pageNum, currentFetchId]() {
        reply->deleteLater();

        if (currentFetchId != m_artistFetchId) {
            qDebug() << "AppleMusicManager: Artist songs fetch cancelled";
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "AppleMusicManager: Artist songs page" << pageNum
                       << "failed:" << reply->errorString();
            if (!m_artistSongs.isEmpty())
                emit artistSongsReady(artistId, m_artistSongs);
            else
                emit errorOccurred(QStringLiteral("Failed to fetch artist songs: ") + reply->errorString());
            return;
        }

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            if (!m_artistSongs.isEmpty())
                emit artistSongsReady(artistId, m_artistSongs);
            return;
        }

        QJsonObject root = doc.object();
        QJsonArray data = root[QStringLiteral("data")].toArray();

        int count = 0;
        for (const QJsonValue& val : data) {
            QJsonObject item = val.toObject();
            QJsonObject attrs = item[QStringLiteral("attributes")].toObject();
            QJsonObject song;
            song[QStringLiteral("id")] = item[QStringLiteral("id")].toString();
            song[QStringLiteral("title")] = attrs[QStringLiteral("name")].toString();
            song[QStringLiteral("artist")] = attrs[QStringLiteral("artistName")].toString();
            song[QStringLiteral("album")] = attrs[QStringLiteral("albumName")].toString();
            song[QStringLiteral("isAppleMusic")] = true;

            int durationMs = attrs[QStringLiteral("durationInMillis")].toInt();
            if (durationMs > 0)
                song[QStringLiteral("duration")] = durationMs / 1000.0;

            QString artworkUrl = attrs[QStringLiteral("artwork")].toObject()
                                      [QStringLiteral("url")].toString();
            if (!artworkUrl.isEmpty()) {
                artworkUrl.replace(QStringLiteral("{w}"), QStringLiteral("300"));
                artworkUrl.replace(QStringLiteral("{h}"), QStringLiteral("300"));
                song[QStringLiteral("artworkUrl")] = artworkUrl;
            }

            m_artistSongs.append(song);
            ++count;
        }

        qDebug() << "AppleMusicManager: Artist songs page" << pageNum
                 << "returned" << count << "songs";

        QString nextPath = root[QStringLiteral("next")].toString();
        if (!nextPath.isEmpty() && pageNum < 5 && count >= 100) {
            fetchArtistSongsPage(artistId, nextPath, pageNum + 1);
        } else {
            qDebug() << "AppleMusicManager: Total artist songs:" << m_artistSongs.size();
            emit artistSongsReady(artistId, m_artistSongs);
        }
    });
}

// ═══════════════════════════════════════════════════════════════════
//  Artist discography — fetch all albums by artist ID
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::fetchArtistAlbums(const QString& artistId)
{
    if (d->developerToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("No developer token for artist lookup"));
        return;
    }

    ++m_artistFetchId;
    m_artistAlbums = QJsonArray();

    QString path = QStringLiteral("/v1/catalog/%1/artists/%2/albums?limit=100")
        .arg(d->storefront, artistId);

    qDebug() << "AppleMusicManager: Fetching all albums for artist:" << artistId;
    fetchArtistAlbumsPage(artistId, path, 1);
}

void AppleMusicManager::fetchArtistAlbumsPage(const QString& artistId,
                                                const QString& urlPath, int pageNum)
{
    QUrl url(QStringLiteral("https://api.music.apple.com") + urlPath);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(d->developerToken).toUtf8());

    int currentFetchId = m_artistFetchId;

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, artistId, pageNum, currentFetchId]() {
        reply->deleteLater();

        if (currentFetchId != m_artistFetchId) {
            qDebug() << "AppleMusicManager: Artist albums fetch cancelled (new request started)";
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "AppleMusicManager: Artist albums page" << pageNum
                       << "failed:" << reply->errorString();
            if (!m_artistAlbums.isEmpty())
                emit artistAlbumsReady(artistId, m_artistAlbums);
            return;
        }

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            if (!m_artistAlbums.isEmpty())
                emit artistAlbumsReady(artistId, m_artistAlbums);
            return;
        }

        QJsonObject root = doc.object();
        QJsonArray data = root[QStringLiteral("data")].toArray();

        int count = 0;
        for (const QJsonValue& val : data) {
            QJsonObject item = val.toObject();
            QJsonObject attrs = item[QStringLiteral("attributes")].toObject();
            QJsonObject album;
            album[QStringLiteral("id")] = item[QStringLiteral("id")].toString();
            album[QStringLiteral("title")] = attrs[QStringLiteral("name")].toString();
            album[QStringLiteral("artist")] = attrs[QStringLiteral("artistName")].toString();
            album[QStringLiteral("trackCount")] = attrs[QStringLiteral("trackCount")].toInt();
            album[QStringLiteral("isAppleMusic")] = true;

            QString artworkUrl = attrs[QStringLiteral("artwork")].toObject()
                                      [QStringLiteral("url")].toString();
            if (!artworkUrl.isEmpty()) {
                artworkUrl.replace(QStringLiteral("{w}"), QStringLiteral("300"));
                artworkUrl.replace(QStringLiteral("{h}"), QStringLiteral("300"));
                album[QStringLiteral("artworkUrl")] = artworkUrl;
            }

            m_artistAlbums.append(album);
            ++count;
        }

        qDebug() << "AppleMusicManager: Artist albums page" << pageNum
                 << "returned" << count << "albums";

        QString nextPath = root[QStringLiteral("next")].toString();
        if (!nextPath.isEmpty() && pageNum < 5 && count >= 100) {
            fetchArtistAlbumsPage(artistId, nextPath, pageNum + 1);
        } else {
            qDebug() << "AppleMusicManager: Total artist albums:" << m_artistAlbums.size();
            emit artistAlbumsReady(artistId, m_artistAlbums);
        }
    });
}

// ═══════════════════════════════════════════════════════════════════
//  Album tracks — fetch all tracks from a specific album
// ═══════════════════════════════════════════════════════════════════

void AppleMusicManager::fetchAlbumTracks(const QString& albumId)
{
    if (d->developerToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("No developer token for album lookup"));
        return;
    }

    QUrl url(QStringLiteral("https://api.music.apple.com/v1/catalog/%1/albums/%2?include=tracks")
        .arg(d->storefront, albumId));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(d->developerToken).toUtf8());

    qDebug() << "AppleMusicManager: Fetching tracks for album:" << albumId;

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, albumId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(QStringLiteral("Failed to fetch album tracks: ") + reply->errorString());
            return;
        }

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            emit errorOccurred(QStringLiteral("Failed to parse album response"));
            return;
        }

        QJsonObject root = doc.object();
        QJsonArray dataArr = root[QStringLiteral("data")].toArray();
        if (dataArr.isEmpty()) {
            emit albumTracksReady(albumId, QJsonArray());
            return;
        }

        QJsonObject albumObj = dataArr[0].toObject();
        QJsonObject albumAttrs = albumObj[QStringLiteral("attributes")].toObject();
        QString albumArtworkUrl = albumAttrs[QStringLiteral("artwork")].toObject()
                                      [QStringLiteral("url")].toString();
        if (!albumArtworkUrl.isEmpty()) {
            albumArtworkUrl.replace(QStringLiteral("{w}"), QStringLiteral("300"));
            albumArtworkUrl.replace(QStringLiteral("{h}"), QStringLiteral("300"));
        }

        QJsonArray tracksData = albumObj[QStringLiteral("relationships")].toObject()
                                    [QStringLiteral("tracks")].toObject()
                                    [QStringLiteral("data")].toArray();

        QJsonArray tracks;
        for (const QJsonValue& val : tracksData) {
            QJsonObject item = val.toObject();
            QJsonObject attrs = item[QStringLiteral("attributes")].toObject();
            QJsonObject song;
            song[QStringLiteral("id")] = item[QStringLiteral("id")].toString();
            song[QStringLiteral("title")] = attrs[QStringLiteral("name")].toString();
            song[QStringLiteral("artist")] = attrs[QStringLiteral("artistName")].toString();
            song[QStringLiteral("album")] = attrs[QStringLiteral("albumName")].toString();
            song[QStringLiteral("isAppleMusic")] = true;

            int durationMs = attrs[QStringLiteral("durationInMillis")].toInt();
            if (durationMs > 0)
                song[QStringLiteral("duration")] = durationMs / 1000.0;

            // Use album artwork for all tracks
            if (!albumArtworkUrl.isEmpty())
                song[QStringLiteral("artworkUrl")] = albumArtworkUrl;

            tracks.append(song);
        }

        qDebug() << "AppleMusicManager: Album" << albumId << "has" << tracks.size() << "tracks";
        emit albumTracksReady(albumId, tracks);
    });
}
