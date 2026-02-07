#pragma once

#include <QObject>
#include <QString>
#include <QJsonArray>

class QNetworkAccessManager;
class QNetworkReply;
class AppleMusicManagerPrivate;

class AppleMusicManager : public QObject
{
    Q_OBJECT

public:
    enum AuthStatus {
        NotDetermined = 0,
        Denied        = 1,
        Restricted    = 2,
        Authorized    = 3
    };
    Q_ENUM(AuthStatus)

    static AppleMusicManager* instance();

    AuthStatus authorizationStatus() const;
    bool isAuthorized() const;
    bool hasSubscription() const;
    bool hasDeveloperToken() const;
    QString developerToken() const;
    QString storefront() const;

    Q_INVOKABLE void requestAuthorization();
    Q_INVOKABLE void disconnectAppleMusic();
    Q_INVOKABLE void checkSubscriptionStatus();
    Q_INVOKABLE void requestMusicUserToken();
    QString musicUserToken() const;
    Q_INVOKABLE void searchCatalog(const QString& term, int limit = 25);
    Q_INVOKABLE void fetchArtistSongs(const QString& artistId);
    Q_INVOKABLE void fetchArtistAlbums(const QString& artistId);
    Q_INVOKABLE void fetchAlbumTracks(const QString& albumId);

    // Developer credentials for REST API fallback
    void loadDeveloperCredentials(const QString& teamId,
                                  const QString& keyId,
                                  const QString& privateKeyPath);
    void setStorefront(const QString& storefront);

signals:
    void authorizationStatusChanged(AuthStatus status);
    void subscriptionStatusChanged(bool hasSubscription);
    void searchResultsReady(const QJsonArray& songs,
                            const QJsonArray& albums,
                            const QJsonArray& artists);
    void artistSongsReady(const QString& artistId, const QJsonArray& songs);
    void artistAlbumsReady(const QString& artistId, const QJsonArray& albums);
    void albumTracksReady(const QString& albumId, const QJsonArray& tracks);
    void errorOccurred(const QString& error);
    void musicUserTokenReady(const QString& token);
    void musicUserTokenFailed(const QString& error);

private:
    explicit AppleMusicManager(QObject* parent = nullptr);
    ~AppleMusicManager();

    void searchViaRestApi(const QString& term, int limit);
    void handleRestApiReply(QNetworkReply* reply);
    void fetchNextSongsPage(const QString& nextPath, int pageNum);
    void emitCollectedResults();
    void fetchArtistSongsPage(const QString& artistId, const QString& urlPath, int pageNum);
    void fetchArtistAlbumsPage(const QString& artistId, const QString& urlPath, int pageNum);

    static AppleMusicManager* s_instance;
    AppleMusicManagerPrivate* d;
    QString m_musicUserToken;
    QNetworkAccessManager* m_network = nullptr;

    // Pagination state for song results
    QJsonArray m_collectedSongs;
    QJsonArray m_collectedAlbums;
    QJsonArray m_collectedArtists;
    int m_paginationId = 0;           // Incremented on new search to cancel old pagination
    QNetworkReply* m_paginationReply = nullptr;

    // Artist discography pagination state
    QJsonArray m_artistSongs;
    QJsonArray m_artistAlbums;
    int m_artistFetchId = 0;          // Incremented on new artist fetch to cancel old one
};
