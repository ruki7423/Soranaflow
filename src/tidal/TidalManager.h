#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <functional>

class QNetworkReply;
class QTcpServer;

class TidalManager : public QObject
{
    Q_OBJECT

public:
    static TidalManager* instance();

    // Client credentials authentication (for search/browse)
    void authenticate();
    bool isAuthenticated() const;
    QString accessToken() const { return m_accessToken; }

    // User OAuth login (for full playback - future)
    void loginWithBrowser();
    void logout();
    bool isUserLoggedIn() const { return m_userLoggedIn; }
    QString username() const { return m_username; }
    QString countryCode() const { return m_countryCode; }
    void setCountryCode(const QString& code) { m_countryCode = code; }

    // API health check (call on app startup to verify API availability)
    void checkApiHealth();
    bool isApiAvailable() const { return m_apiAvailable; }
    QString apiStatus() const { return m_apiStatus; }

    // WebView search URL helper (API is down, use web player)
    static QString getSearchUrl(const QString& query);

    // Cover art URL helper (converts image ID to URL)
    static QString coverArtUrl(const QString& imageId, int size = 750);

    // === API SEARCH DISABLED — openapi.tidal.com returning 404 (2025-02) ===
    // Uncomment when Tidal restores API endpoints
    // void search(const QString& query, const QString& countryCode = QStringLiteral("US"));
    // void getAlbum(const QString& albumId, const QString& countryCode = QStringLiteral("US"));
    // void getTrack(const QString& trackId, const QString& countryCode = QStringLiteral("US"));
    // void getArtist(const QString& artistId, const QString& countryCode = QStringLiteral("US"));
    // void getAlbumTracks(const QString& albumId, const QString& countryCode = QStringLiteral("US"));
    // void getArtistAlbums(const QString& artistId, const QString& countryCode = QStringLiteral("US"));
    // void getArtistTopTracks(const QString& artistId, const QString& countryCode = QStringLiteral("US"));

signals:
    void apiHealthChecked(bool available, const QString& status);
    void authenticated();
    void authError(const QString& error);
    void userLoggedIn(const QString& username);
    void userLoggedOut();
    void loginError(const QString& error);
    void searchResultsReady(const QJsonObject& results);
    void albumReady(const QJsonObject& album);
    void trackReady(const QJsonObject& track);
    void artistReady(const QJsonObject& artist);
    void albumTracksReady(const QString& albumId, const QJsonArray& tracks);
    void artistAlbumsReady(const QString& artistId, const QJsonArray& albums);
    void artistTopTracksReady(const QString& artistId, const QJsonArray& tracks);
    void networkError(const QString& error);

private:
    explicit TidalManager(QObject* parent = nullptr);
    ~TidalManager();

    void requestToken();
    void handleTokenResponse(QNetworkReply* reply);
    void ensureAuthenticated(std::function<void()> callback);
    void makeApiRequest(const QString& endpoint,
                        const QString& countryCode,
                        std::function<void(const QJsonObject&)> callback,
                        bool useV1Headers = false);

    // Parse JSON:API format responses
    static QJsonObject parseTrackFromJsonApi(const QJsonObject& data, const QJsonObject& included);
    static QJsonObject parseAlbumFromJsonApi(const QJsonObject& data, const QJsonObject& included);
    static QJsonObject parseArtistFromJsonApi(const QJsonObject& data);

    QNetworkAccessManager* m_networkManager = nullptr;
    QString m_accessToken;
    QDateTime m_tokenExpiry;

    // Pending requests while authenticating
    QList<std::function<void()>> m_pendingRequests;
    bool m_authenticating = false;

    // User OAuth login
    void handleOAuthCallback(const QString& code);
    void exchangeCodeForTokens(const QString& code);
    void fetchUserInfo();
    void saveTokens();
    void loadTokens();
    static QString generateCodeVerifier();
    static QString generateCodeChallenge(const QString& verifier);

    QTcpServer* m_oauthServer = nullptr;
    QString m_codeVerifier;
    QString m_redirectUri;
    QString m_userAccessToken;
    QString m_userRefreshToken;
    QDateTime m_userTokenExpiry;
    QString m_username;
    QString m_userId;
    QString m_countryCode = QStringLiteral("US");
    bool m_userLoggedIn = false;

    // API credentials
    static constexpr const char* CLIENT_ID = "5w6Lrp0d9NS4MWgo";
    static constexpr const char* CLIENT_SECRET = "vede5Lg2g0d1FogHBlEoHpOC1pLfHUAhMAxb0M4dGmw=";

    // API health status
    bool m_apiAvailable = false;
    QString m_apiStatus = QStringLiteral("Not checked");
};
