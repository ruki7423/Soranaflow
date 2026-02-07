#include "TidalManager.h"

#include <QDebug>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDesktopServices>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>

// ═══════════════════════════════════════════════════════════════════
//  Singleton
// ═══════════════════════════════════════════════════════════════════

TidalManager* TidalManager::instance()
{
    static TidalManager s;
    return &s;
}

// ═══════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════

TidalManager::TidalManager(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    qDebug() << "[TidalManager] Initialized";
    loadTokens();
}

TidalManager::~TidalManager() = default;

// ═══════════════════════════════════════════════════════════════════
//  Authentication
// ═══════════════════════════════════════════════════════════════════

bool TidalManager::isAuthenticated() const
{
    return !m_accessToken.isEmpty() && QDateTime::currentDateTime() < m_tokenExpiry;
}

void TidalManager::authenticate()
{
    if (m_authenticating) {
        qDebug() << "[TidalManager] Authentication already in progress";
        return;
    }
    requestToken();
}

void TidalManager::requestToken()
{
    m_authenticating = true;
    qDebug() << "[TidalManager] Requesting access token...";

    QUrl url(QStringLiteral("https://auth.tidal.com/v1/oauth2/token"));
    QNetworkRequest request(url);

    // Basic auth: base64(client_id:client_secret)
    QString credentials = QStringLiteral("%1:%2").arg(CLIENT_ID, CLIENT_SECRET);
    QByteArray authHeader = "Basic " + credentials.toUtf8().toBase64();
    request.setRawHeader("Authorization", authHeader);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));

    QByteArray body = "grant_type=client_credentials";

    QNetworkReply* reply = m_networkManager->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleTokenResponse(reply);
    });
}

void TidalManager::handleTokenResponse(QNetworkReply* reply)
{
    reply->deleteLater();
    m_authenticating = false;

    if (reply->error() != QNetworkReply::NoError) {
        QString err = QStringLiteral("Token request failed: %1 (HTTP %2)")
            .arg(reply->errorString())
            .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        qWarning() << "[TidalManager]" << err;
        emit authError(err);

        // Clear pending requests
        m_pendingRequests.clear();
        return;
    }

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        emit authError(QStringLiteral("Failed to parse token response"));
        m_pendingRequests.clear();
        return;
    }

    QJsonObject root = doc.object();
    m_accessToken = root[QStringLiteral("access_token")].toString();
    int expiresIn = root[QStringLiteral("expires_in")].toInt(86400);

    // Set expiry with 5-minute buffer
    m_tokenExpiry = QDateTime::currentDateTime().addSecs(expiresIn - 300);

    qDebug() << "[TidalManager] Token acquired, expires in:" << expiresIn << "seconds";
    emit authenticated();

    // Execute pending requests
    for (const auto& callback : m_pendingRequests) {
        callback();
    }
    m_pendingRequests.clear();
}

void TidalManager::ensureAuthenticated(std::function<void()> callback)
{
    if (isAuthenticated()) {
        callback();
        return;
    }

    // Queue the request
    m_pendingRequests.append(callback);

    // Start authentication if not already in progress
    if (!m_authenticating) {
        requestToken();
    }
}

// ═══════════════════════════════════════════════════════════════════
//  API Health Check
// ═══════════════════════════════════════════════════════════════════

void TidalManager::checkApiHealth()
{
    qDebug() << "[TidalManager] Checking API health...";

    // Step 1: Check if auth endpoint works (get client credentials token)
    QUrl tokenUrl(QStringLiteral("https://auth.tidal.com/v1/oauth2/token"));
    QNetworkRequest tokenRequest(tokenUrl);

    QString credentials = QStringLiteral("%1:%2").arg(CLIENT_ID, CLIENT_SECRET);
    QByteArray authHeader = "Basic " + credentials.toUtf8().toBase64();
    tokenRequest.setRawHeader("Authorization", authHeader);
    tokenRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));

    QByteArray body = "grant_type=client_credentials";

    QNetworkReply* tokenReply = m_networkManager->post(tokenRequest, body);
    connect(tokenReply, &QNetworkReply::finished, this, [this, tokenReply]() {
        tokenReply->deleteLater();

        int httpStatus = tokenReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (tokenReply->error() != QNetworkReply::NoError) {
            m_apiAvailable = false;
            m_apiStatus = QStringLiteral("Auth failed: %1 (HTTP %2)")
                .arg(tokenReply->errorString())
                .arg(httpStatus);
            qWarning() << "[TidalManager] Health check:" << m_apiStatus;
            emit apiHealthChecked(false, m_apiStatus);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(tokenReply->readAll());
        QString testToken = doc.object()[QStringLiteral("access_token")].toString();

        if (testToken.isEmpty()) {
            m_apiAvailable = false;
            m_apiStatus = QStringLiteral("Auth response missing access_token");
            qWarning() << "[TidalManager] Health check:" << m_apiStatus;
            emit apiHealthChecked(false, m_apiStatus);
            return;
        }

        qDebug() << "[TidalManager] Auth endpoint OK, testing API endpoint...";

        // Step 2: Test openapi.tidal.com with a simple search
        QUrl apiUrl(QStringLiteral("https://openapi.tidal.com/search?query=test&countryCode=US&limit=1&offset=0"));
        QNetworkRequest apiRequest(apiUrl);
        apiRequest.setRawHeader("accept", "application/vnd.tidal.v1+json");
        apiRequest.setRawHeader("Content-Type", "application/vnd.tidal.v1+json");
        apiRequest.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(testToken).toUtf8());

        QNetworkReply* apiReply = m_networkManager->get(apiRequest);
        connect(apiReply, &QNetworkReply::finished, this, [this, apiReply]() {
            apiReply->deleteLater();

            int apiHttpStatus = apiReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (apiReply->error() != QNetworkReply::NoError) {
                m_apiAvailable = false;
                m_apiStatus = QStringLiteral("API unavailable: %1 (HTTP %2)")
                    .arg(apiReply->errorString())
                    .arg(apiHttpStatus);
                qWarning() << "[TidalManager] Health check:" << m_apiStatus;
                emit apiHealthChecked(false, m_apiStatus);
                return;
            }

            // Check if we got valid JSON response
            QJsonParseError parseErr;
            QJsonDocument apiDoc = QJsonDocument::fromJson(apiReply->readAll(), &parseErr);

            if (parseErr.error != QJsonParseError::NoError) {
                m_apiAvailable = false;
                m_apiStatus = QStringLiteral("API returned invalid JSON");
                qWarning() << "[TidalManager] Health check:" << m_apiStatus;
                emit apiHealthChecked(false, m_apiStatus);
                return;
            }

            // Success!
            m_apiAvailable = true;
            m_apiStatus = QStringLiteral("API available (HTTP %1)").arg(apiHttpStatus);
            qDebug() << "[TidalManager] Health check:" << m_apiStatus;
            emit apiHealthChecked(true, m_apiStatus);
        });
    });
}

// ═══════════════════════════════════════════════════════════════════
//  WebView Search URL Helper
// ═══════════════════════════════════════════════════════════════════

QString TidalManager::getSearchUrl(const QString& query)
{
    if (query.isEmpty()) {
        return QStringLiteral("https://listen.tidal.com/");
    }
    return QStringLiteral("https://listen.tidal.com/search?q=%1")
        .arg(QString(QUrl::toPercentEncoding(query)));
}

// ═══════════════════════════════════════════════════════════════════
//  API Requests — DISABLED (openapi.tidal.com returning 404, 2025-02)
//  Uncomment when Tidal restores API endpoints
// ═══════════════════════════════════════════════════════════════════

#if 0  // === API SEARCH DISABLED ===

void TidalManager::makeApiRequest(const QString& endpoint,
                                   const QString& countryCode,
                                   std::function<void(const QJsonObject&)> callback,
                                   bool useV1Headers)
{
    ensureAuthenticated([this, endpoint, countryCode, callback, useV1Headers]() {
        QString urlStr = QStringLiteral("https://openapi.tidal.com") + endpoint;

        // Add countryCode if not already in URL
        if (!endpoint.contains(QStringLiteral("countryCode"))) {
            urlStr += (endpoint.contains('?') ? QStringLiteral("&") : QStringLiteral("?"));
            urlStr += QStringLiteral("countryCode=") + countryCode;
        }

        QUrl url(urlStr);
        QNetworkRequest request(url);

        // Use v1 headers for search, v2 headers for other endpoints
        if (useV1Headers) {
            request.setRawHeader("accept", "application/vnd.tidal.v1+json");
            request.setRawHeader("Content-Type", "application/vnd.tidal.v1+json");
        } else {
            request.setRawHeader("accept", "application/vnd.api+json");
            request.setRawHeader("Content-Type", "application/vnd.api+json");
        }
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_accessToken).toUtf8());

        qDebug() << "[TidalManager] API request:" << url.toString();

        QNetworkReply* reply = m_networkManager->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
            reply->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                QString err = QStringLiteral("API error: %1 (HTTP %2)")
                    .arg(reply->errorString())
                    .arg(httpStatus);

                // Check for token expiry
                if (httpStatus == 401) {
                    qDebug() << "[TidalManager] Token expired, clearing for re-auth";
                    m_accessToken.clear();
                }

                qWarning() << "[TidalManager]" << err;
                emit networkError(err);
                return;
            }

            QByteArray data = reply->readAll();
            QJsonParseError parseErr;
            QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
            if (parseErr.error != QJsonParseError::NoError) {
                qWarning() << "[TidalManager] JSON parse error:" << parseErr.errorString();
                emit networkError(QStringLiteral("Failed to parse API response"));
                return;
            }

            callback(doc.object());
        });
    });
}

// ═══════════════════════════════════════════════════════════════════
//  Search
// ═══════════════════════════════════════════════════════════════════

void TidalManager::search(const QString& query, const QString& countryCode)
{
    if (query.isEmpty()) {
        emit searchResultsReady(QJsonObject());
        return;
    }

    qDebug() << "[TidalManager] Search query:" << query;

    // URL-encode the query (v1 uses query parameter, not path)
    QString encodedQuery = QUrl::toPercentEncoding(query);
    QString endpoint = QStringLiteral("/search?query=%1&countryCode=%2&limit=25&offset=0")
        .arg(encodedQuery, countryCode);

    // Use v1 headers for search endpoint
    makeApiRequest(endpoint, countryCode, [this](const QJsonObject& response) {
        // v1 response format:
        // { "tracks": { "items": [...], "totalNumberOfItems": N },
        //   "albums": { "items": [...], "totalNumberOfItems": N },
        //   "artists": { "items": [...], "totalNumberOfItems": N } }

        QJsonArray tracks = response[QStringLiteral("tracks")].toObject()
                                [QStringLiteral("items")].toArray();
        QJsonArray albums = response[QStringLiteral("albums")].toObject()
                                [QStringLiteral("items")].toArray();
        QJsonArray artists = response[QStringLiteral("artists")].toObject()
                                 [QStringLiteral("items")].toArray();

        qDebug() << "[TidalManager] Search results — tracks:" << tracks.size()
                 << "albums:" << albums.size()
                 << "artists:" << artists.size();

        // Convert to unified format for TidalView
        // v1 format: direct objects with id, title, artists array, album object, cover string
        QJsonObject result;
        result[QStringLiteral("tracks")] = tracks;
        result[QStringLiteral("albums")] = albums;
        result[QStringLiteral("artists")] = artists;

        emit searchResultsReady(result);
    }, true);  // useV1Headers = true
}

// ═══════════════════════════════════════════════════════════════════
//  Resource Fetching
// ═══════════════════════════════════════════════════════════════════

void TidalManager::getAlbum(const QString& albumId, const QString& countryCode)
{
    // shareCode=xyz is required for the v2 API
    QString endpoint = QStringLiteral("/v2/albums/%1?countryCode=%2&shareCode=xyz")
        .arg(albumId, countryCode);

    makeApiRequest(endpoint, countryCode, [this](const QJsonObject& response) {
        QJsonObject data = response[QStringLiteral("data")].toObject();
        QJsonObject included = response[QStringLiteral("included")].toObject();
        QJsonObject album = parseAlbumFromJsonApi(data, included);
        qDebug() << "[TidalManager] Album:" << album[QStringLiteral("title")].toString()
                 << "by" << album[QStringLiteral("artist")].toString();
        emit albumReady(album);
    });
}

void TidalManager::getTrack(const QString& trackId, const QString& countryCode)
{
    QString endpoint = QStringLiteral("/v2/tracks/%1?countryCode=%2&shareCode=xyz")
        .arg(trackId, countryCode);

    makeApiRequest(endpoint, countryCode, [this](const QJsonObject& response) {
        QJsonObject data = response[QStringLiteral("data")].toObject();
        QJsonObject included = response[QStringLiteral("included")].toObject();
        QJsonObject track = parseTrackFromJsonApi(data, included);
        qDebug() << "[TidalManager] Track:" << track[QStringLiteral("title")].toString();
        emit trackReady(track);
    });
}

void TidalManager::getArtist(const QString& artistId, const QString& countryCode)
{
    QString endpoint = QStringLiteral("/v2/artists/%1?countryCode=%2&shareCode=xyz")
        .arg(artistId, countryCode);

    makeApiRequest(endpoint, countryCode, [this](const QJsonObject& response) {
        QJsonObject data = response[QStringLiteral("data")].toObject();
        QJsonObject artist = parseArtistFromJsonApi(data);
        qDebug() << "[TidalManager] Artist:" << artist[QStringLiteral("name")].toString();
        emit artistReady(artist);
    });
}

void TidalManager::getAlbumTracks(const QString& albumId, const QString& countryCode)
{
    QString endpoint = QStringLiteral("/v2/albums/%1/relationships/items?countryCode=%2")
        .arg(albumId, countryCode);

    makeApiRequest(endpoint, countryCode, [this, albumId](const QJsonObject& response) {
        QJsonArray dataArr = response[QStringLiteral("data")].toArray();
        QJsonObject included;  // May need separate fetch for full track details

        QJsonArray tracks;
        for (const QJsonValue& val : dataArr) {
            QJsonObject item = val.toObject();
            QJsonObject track = parseTrackFromJsonApi(item, included);
            tracks.append(track);
        }

        qDebug() << "[TidalManager] Album" << albumId << "has" << tracks.size() << "tracks";
        emit albumTracksReady(albumId, tracks);
    });
}

void TidalManager::getArtistAlbums(const QString& artistId, const QString& countryCode)
{
    QString endpoint = QStringLiteral("/v2/artists/%1/relationships/albums?countryCode=%2")
        .arg(artistId, countryCode);

    makeApiRequest(endpoint, countryCode, [this, artistId](const QJsonObject& response) {
        QJsonArray dataArr = response[QStringLiteral("data")].toArray();

        QJsonArray albums;
        for (const QJsonValue& val : dataArr) {
            QJsonObject item = val.toObject();
            QJsonObject album = parseAlbumFromJsonApi(item, QJsonObject());
            albums.append(album);
        }

        qDebug() << "[TidalManager] Artist" << artistId << "has" << albums.size() << "albums";
        emit artistAlbumsReady(artistId, albums);
    });
}

void TidalManager::getArtistTopTracks(const QString& artistId, const QString& countryCode)
{
    QString endpoint = QStringLiteral("/v2/artists/%1/relationships/tracks?countryCode=%2")
        .arg(artistId, countryCode);

    makeApiRequest(endpoint, countryCode, [this, artistId](const QJsonObject& response) {
        QJsonArray dataArr = response[QStringLiteral("data")].toArray();

        QJsonArray tracks;
        for (const QJsonValue& val : dataArr) {
            QJsonObject item = val.toObject();
            QJsonObject track = parseTrackFromJsonApi(item, QJsonObject());
            tracks.append(track);
        }

        qDebug() << "[TidalManager] Artist" << artistId << "top tracks:" << tracks.size();
        emit artistTopTracksReady(artistId, tracks);
    });
}

#endif  // === END API SEARCH DISABLED ===

// ═══════════════════════════════════════════════════════════════════
//  Cover Art URL (kept active — used by WebView browse UI)
// ═══════════════════════════════════════════════════════════════════

QString TidalManager::coverArtUrl(const QString& imageId, int size)
{
    if (imageId.isEmpty()) return QString();

    // Convert dashes to slashes: "ab-cd-ef-12" -> "ab/cd/ef/12"
    QString path = imageId;
    path.replace('-', '/');

    return QStringLiteral("https://resources.tidal.com/images/%1/%2x%2.jpg")
        .arg(path)
        .arg(size);
}

// ═══════════════════════════════════════════════════════════════════
//  JSON:API Parsing Helpers — DISABLED (API is down)
// ═══════════════════════════════════════════════════════════════════

QJsonObject TidalManager::parseTrackFromJsonApi(const QJsonObject& data, const QJsonObject& included)
{
    Q_UNUSED(included)

    QJsonObject track;
    QJsonObject attrs = data[QStringLiteral("attributes")].toObject();
    QJsonObject resource = data[QStringLiteral("resource")].toObject();

    // Use resource if available (search results), otherwise use attrs (direct fetch)
    if (!resource.isEmpty()) {
        attrs = resource;
    }

    track[QStringLiteral("id")] = data[QStringLiteral("id")].toString();
    track[QStringLiteral("title")] = attrs[QStringLiteral("title")].toString();
    track[QStringLiteral("isTidal")] = true;

    // Duration in seconds
    int durationSecs = attrs[QStringLiteral("duration")].toInt();
    if (durationSecs > 0) {
        track[QStringLiteral("duration")] = durationSecs;
    }

    // Artist name (may be nested)
    QJsonArray artistNames = attrs[QStringLiteral("artistNames")].toArray();
    if (!artistNames.isEmpty()) {
        track[QStringLiteral("artist")] = artistNames[0].toString();
    } else {
        // Try relationships
        QJsonObject rels = data[QStringLiteral("relationships")].toObject();
        QJsonArray artistsData = rels[QStringLiteral("artists")].toObject()
                                     [QStringLiteral("data")].toArray();
        if (!artistsData.isEmpty()) {
            track[QStringLiteral("artistId")] = artistsData[0].toObject()[QStringLiteral("id")].toString();
        }
    }

    // Album info
    QString albumTitle = attrs[QStringLiteral("albumTitle")].toString();
    if (!albumTitle.isEmpty()) {
        track[QStringLiteral("album")] = albumTitle;
    }

    // Cover art
    QJsonObject imageLinks = attrs[QStringLiteral("imageLinks")].toArray().first().toObject();
    QString imageHref = imageLinks[QStringLiteral("href")].toString();
    if (!imageHref.isEmpty()) {
        track[QStringLiteral("artworkUrl")] = imageHref;
    } else {
        // Try imageCover array
        QJsonArray imageCover = attrs[QStringLiteral("imageCover")].toArray();
        if (!imageCover.isEmpty()) {
            QString imgId = imageCover[0].toString();
            track[QStringLiteral("artworkUrl")] = coverArtUrl(imgId, 320);
        }
    }

    // Quality indicators
    QString mediaMetaTags = attrs[QStringLiteral("mediaMetadata")].toObject()
                               [QStringLiteral("tags")].toString();
    track[QStringLiteral("isHiRes")] = mediaMetaTags.contains(QStringLiteral("HIRES"));
    track[QStringLiteral("isMQA")] = mediaMetaTags.contains(QStringLiteral("MQA"));
    track[QStringLiteral("isDolbyAtmos")] = mediaMetaTags.contains(QStringLiteral("DOLBY_ATMOS"));

    return track;
}

QJsonObject TidalManager::parseAlbumFromJsonApi(const QJsonObject& data, const QJsonObject& included)
{
    Q_UNUSED(included)

    QJsonObject album;
    QJsonObject attrs = data[QStringLiteral("attributes")].toObject();
    QJsonObject resource = data[QStringLiteral("resource")].toObject();

    if (!resource.isEmpty()) {
        attrs = resource;
    }

    album[QStringLiteral("id")] = data[QStringLiteral("id")].toString();
    album[QStringLiteral("title")] = attrs[QStringLiteral("title")].toString();
    album[QStringLiteral("isTidal")] = true;

    // Artist
    QJsonArray artistNames = attrs[QStringLiteral("artistNames")].toArray();
    if (!artistNames.isEmpty()) {
        album[QStringLiteral("artist")] = artistNames[0].toString();
    }

    // Track count
    album[QStringLiteral("trackCount")] = attrs[QStringLiteral("numberOfItems")].toInt();

    // Release date
    album[QStringLiteral("releaseDate")] = attrs[QStringLiteral("releaseDate")].toString();

    // Cover art
    QJsonObject imageLinks = attrs[QStringLiteral("imageLinks")].toArray().first().toObject();
    QString imageHref = imageLinks[QStringLiteral("href")].toString();
    if (!imageHref.isEmpty()) {
        album[QStringLiteral("artworkUrl")] = imageHref;
    } else {
        QJsonArray imageCover = attrs[QStringLiteral("imageCover")].toArray();
        if (!imageCover.isEmpty()) {
            album[QStringLiteral("artworkUrl")] = coverArtUrl(imageCover[0].toString(), 640);
        }
    }

    return album;
}

QJsonObject TidalManager::parseArtistFromJsonApi(const QJsonObject& data)
{
    QJsonObject artist;
    QJsonObject attrs = data[QStringLiteral("attributes")].toObject();
    QJsonObject resource = data[QStringLiteral("resource")].toObject();

    if (!resource.isEmpty()) {
        attrs = resource;
    }

    artist[QStringLiteral("id")] = data[QStringLiteral("id")].toString();
    artist[QStringLiteral("name")] = attrs[QStringLiteral("name")].toString();

    // Artist image
    QJsonObject imageLinks = attrs[QStringLiteral("imageLinks")].toArray().first().toObject();
    QString imageHref = imageLinks[QStringLiteral("href")].toString();
    if (!imageHref.isEmpty()) {
        artist[QStringLiteral("artworkUrl")] = imageHref;
    } else {
        QJsonArray picture = attrs[QStringLiteral("picture")].toArray();
        if (!picture.isEmpty()) {
            artist[QStringLiteral("artworkUrl")] = coverArtUrl(picture[0].toString(), 480);
        }
    }

    return artist;
}

// ═══════════════════════════════════════════════════════════════════
//  User OAuth Login (PKCE)
// ═══════════════════════════════════════════════════════════════════

QString TidalManager::generateCodeVerifier()
{
    // Generate random 64-character string (alphanumeric + -._~)
    const QString chars = QStringLiteral(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~");
    QString verifier;
    verifier.reserve(64);
    for (int i = 0; i < 64; ++i) {
        verifier.append(chars.at(QRandomGenerator::global()->bounded(chars.length())));
    }
    return verifier;
}

QString TidalManager::generateCodeChallenge(const QString& verifier)
{
    // S256: base64url(sha256(verifier))
    QByteArray hash = QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256);
    QString challenge = QString::fromLatin1(hash.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    return challenge;
}

void TidalManager::loginWithBrowser()
{
    if (m_userLoggedIn) {
        qDebug() << "[TidalManager] Already logged in as:" << m_username;
        return;
    }

    // Clean up any existing server
    if (m_oauthServer) {
        m_oauthServer->close();
        m_oauthServer->deleteLater();
        m_oauthServer = nullptr;
    }

    // Generate PKCE code verifier and challenge
    m_codeVerifier = generateCodeVerifier();
    QString codeChallenge = generateCodeChallenge(m_codeVerifier);

    // Start local TCP server for OAuth callback
    m_oauthServer = new QTcpServer(this);
    if (!m_oauthServer->listen(QHostAddress::LocalHost, 0)) {
        qDebug() << "[TidalManager] Failed to start OAuth server:" << m_oauthServer->errorString();
        emit loginError(QStringLiteral("Failed to start local server"));
        return;
    }

    quint16 port = m_oauthServer->serverPort();
    m_redirectUri = QStringLiteral("http://localhost:%1/callback").arg(port);

    qDebug() << "[TidalManager] Starting OAuth, listening on port:" << port;

    // Handle incoming connections
    connect(m_oauthServer, &QTcpServer::newConnection, this, [this]() {
        QTcpSocket* socket = m_oauthServer->nextPendingConnection();
        if (!socket) return;

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            QByteArray data = socket->readAll();
            QString request = QString::fromUtf8(data);

            // Parse the callback URL for the authorization code
            if (request.startsWith(QStringLiteral("GET /callback?"))) {
                int start = request.indexOf('?') + 1;
                int end = request.indexOf(' ', start);
                QString queryString = request.mid(start, end - start);

                QUrlQuery query(queryString);
                QString code = query.queryItemValue(QStringLiteral("code"));
                QString error = query.queryItemValue(QStringLiteral("error"));

                // Send response to browser
                QString response;
                if (!code.isEmpty()) {
                    response = QStringLiteral(
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html\r\n"
                        "Connection: close\r\n\r\n"
                        "<html><body style='font-family:sans-serif;text-align:center;padding:50px'>"
                        "<h1>✓ Logged into Tidal</h1>"
                        "<p>You can close this window and return to Sorana Flow.</p>"
                        "</body></html>");
                } else {
                    response = QStringLiteral(
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html\r\n"
                        "Connection: close\r\n\r\n"
                        "<html><body style='font-family:sans-serif;text-align:center;padding:50px'>"
                        "<h1>Login Failed</h1>"
                        "<p>Error: %1</p>"
                        "</body></html>").arg(error.isEmpty() ? QStringLiteral("Unknown error") : error);
                }

                socket->write(response.toUtf8());
                socket->flush();
                socket->disconnectFromHost();

                // Close the server
                m_oauthServer->close();

                if (!code.isEmpty()) {
                    qDebug() << "[TidalManager] OAuth callback received, code length:" << code.length();
                    handleOAuthCallback(code);
                } else {
                    qDebug() << "[TidalManager] OAuth error:" << error;
                    emit loginError(error.isEmpty() ? QStringLiteral("Login cancelled") : error);
                }
            }

            socket->deleteLater();
        });
    });

    // Set timeout for OAuth server (2 minutes)
    QTimer::singleShot(120000, this, [this]() {
        if (m_oauthServer && m_oauthServer->isListening()) {
            qDebug() << "[TidalManager] OAuth timeout, closing server";
            m_oauthServer->close();
            emit loginError(QStringLiteral("Login timed out"));
        }
    });

    // Build authorization URL
    QUrl authUrl(QStringLiteral("https://login.tidal.com/authorize"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(CLIENT_ID));
    query.addQueryItem(QStringLiteral("redirect_uri"), m_redirectUri);
    query.addQueryItem(QStringLiteral("scope"), QString()); // Empty for now
    query.addQueryItem(QStringLiteral("code_challenge"), codeChallenge);
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    authUrl.setQuery(query);

    // Open in external browser
    qDebug() << "[TidalManager] Opening browser for login";
    QDesktopServices::openUrl(authUrl);
}

void TidalManager::handleOAuthCallback(const QString& code)
{
    exchangeCodeForTokens(code);
}

void TidalManager::exchangeCodeForTokens(const QString& code)
{
    qDebug() << "[TidalManager] Exchanging code for tokens...";

    QUrl url(QStringLiteral("https://auth.tidal.com/v1/oauth2/token"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));

    // Basic auth
    QString credentials = QStringLiteral("%1:%2").arg(QString::fromLatin1(CLIENT_ID), QString::fromLatin1(CLIENT_SECRET));
    QByteArray authHeader = "Basic " + credentials.toUtf8().toBase64();
    request.setRawHeader("Authorization", authHeader);

    // Form data
    QUrlQuery postData;
    postData.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    postData.addQueryItem(QStringLiteral("code"), code);
    postData.addQueryItem(QStringLiteral("redirect_uri"), m_redirectUri);
    postData.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(CLIENT_ID));
    postData.addQueryItem(QStringLiteral("code_verifier"), m_codeVerifier);

    QNetworkReply* reply = m_networkManager->post(request, postData.toString(QUrl::FullyEncoded).toUtf8());

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "[TidalManager] Token exchange failed:" << reply->errorString();
            emit loginError(reply->errorString());
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject json = doc.object();

        m_userAccessToken = json[QStringLiteral("access_token")].toString();
        m_userRefreshToken = json[QStringLiteral("refresh_token")].toString();
        int expiresIn = json[QStringLiteral("expires_in")].toInt(3600);
        m_userTokenExpiry = QDateTime::currentDateTime().addSecs(expiresIn);

        if (m_userAccessToken.isEmpty()) {
            qDebug() << "[TidalManager] No access token in response";
            emit loginError(QStringLiteral("No access token received"));
            return;
        }

        qDebug() << "[TidalManager] Token exchange successful";

        // Fetch user info
        fetchUserInfo();
    });
}

void TidalManager::fetchUserInfo()
{
    qDebug() << "[TidalManager] Fetching user info...";

    QUrl url(QStringLiteral("https://api.tidal.com/v1/sessions"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_userAccessToken).toUtf8());

    QNetworkReply* reply = m_networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            // Try alternative endpoint
            qDebug() << "[TidalManager] Sessions endpoint failed, trying user endpoint";
            QUrl url2(QStringLiteral("https://api.tidal.com/v1/users/me"));
            QNetworkRequest request2(url2);
            request2.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_userAccessToken).toUtf8());
            request2.setRawHeader("x-tidal-token", CLIENT_ID);

            QNetworkReply* reply2 = m_networkManager->get(request2);
            connect(reply2, &QNetworkReply::finished, this, [this, reply2]() {
                reply2->deleteLater();

                m_userLoggedIn = true;
                m_username = QStringLiteral("Tidal User"); // Default if can't get name

                if (reply2->error() == QNetworkReply::NoError) {
                    QJsonDocument doc = QJsonDocument::fromJson(reply2->readAll());
                    QJsonObject user = doc.object();
                    QString firstName = user[QStringLiteral("firstName")].toString();
                    QString lastName = user[QStringLiteral("lastName")].toString();
                    m_userId = QString::number(user[QStringLiteral("id")].toInteger());
                    if (!firstName.isEmpty()) {
                        m_username = firstName;
                        if (!lastName.isEmpty()) {
                            m_username += QStringLiteral(" ") + lastName;
                        }
                    }
                }

                saveTokens();
                qDebug() << "[TidalManager] Logged in as:" << m_username;
                emit userLoggedIn(m_username);
            });
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject session = doc.object();

        m_userId = QString::number(session[QStringLiteral("userId")].toInteger());
        m_countryCode = session[QStringLiteral("countryCode")].toString(QStringLiteral("US"));

        m_userLoggedIn = true;
        m_username = QStringLiteral("Tidal User"); // Will be updated from user endpoint

        saveTokens();
        qDebug() << "[TidalManager] Logged in, user ID:" << m_userId << "country:" << m_countryCode;
        emit userLoggedIn(m_username);
    });
}

void TidalManager::logout()
{
    qDebug() << "[TidalManager] Logging out";

    m_userAccessToken.clear();
    m_userRefreshToken.clear();
    m_userTokenExpiry = QDateTime();
    m_username.clear();
    m_userId.clear();
    m_userLoggedIn = false;

    // Clear saved tokens
    QSettings settings;
    settings.remove(QStringLiteral("Tidal/accessToken"));
    settings.remove(QStringLiteral("Tidal/refreshToken"));
    settings.remove(QStringLiteral("Tidal/tokenExpiry"));
    settings.remove(QStringLiteral("Tidal/username"));
    settings.remove(QStringLiteral("Tidal/userId"));

    emit userLoggedOut();
}

void TidalManager::saveTokens()
{
    QSettings settings;
    settings.setValue(QStringLiteral("Tidal/accessToken"), m_userAccessToken);
    settings.setValue(QStringLiteral("Tidal/refreshToken"), m_userRefreshToken);
    settings.setValue(QStringLiteral("Tidal/tokenExpiry"), m_userTokenExpiry);
    settings.setValue(QStringLiteral("Tidal/username"), m_username);
    settings.setValue(QStringLiteral("Tidal/userId"), m_userId);
    settings.setValue(QStringLiteral("Tidal/countryCode"), m_countryCode);
    qDebug() << "[TidalManager] Tokens saved";
}

void TidalManager::loadTokens()
{
    QSettings settings;
    m_userAccessToken = settings.value(QStringLiteral("Tidal/accessToken")).toString();
    m_userRefreshToken = settings.value(QStringLiteral("Tidal/refreshToken")).toString();
    m_userTokenExpiry = settings.value(QStringLiteral("Tidal/tokenExpiry")).toDateTime();
    m_username = settings.value(QStringLiteral("Tidal/username")).toString();
    m_userId = settings.value(QStringLiteral("Tidal/userId")).toString();
    m_countryCode = settings.value(QStringLiteral("Tidal/countryCode"), QStringLiteral("US")).toString();

    if (!m_userAccessToken.isEmpty() && QDateTime::currentDateTime() < m_userTokenExpiry) {
        m_userLoggedIn = true;
        qDebug() << "[TidalManager] Restored login for:" << m_username;
        emit userLoggedIn(m_username);
    } else if (!m_userAccessToken.isEmpty()) {
        qDebug() << "[TidalManager] Saved tokens expired";
        // Could implement refresh token flow here
        m_userLoggedIn = false;
    }
}
