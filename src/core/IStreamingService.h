#pragma once

#include <QString>
#include <QJsonArray>

// Common interface for streaming music services (Apple Music, Tidal, etc.).
//
// Concrete implementations inherit both QObject and IStreamingService.
// Signals remain on the concrete class (Qt doesn't support interface signals
// without QObject diamond inheritance).
//
// For polymorphic signal connections, callers can qobject_cast the
// QObject* returned by asQObject().
class IStreamingService {
public:
    virtual ~IStreamingService() = default;

    // ── Identity ─────────────────────────────────────────────────
    virtual QString serviceName() const = 0;   // e.g. "Apple Music", "Tidal"
    virtual QString serviceId() const = 0;     // e.g. "apple-music", "tidal"

    // ── Authentication ───────────────────────────────────────────
    virtual bool isServiceAuthorized() const = 0;
    virtual void authorize() = 0;
    virtual void deauthorize() = 0;

    // ── Region ───────────────────────────────────────────────────
    virtual QString region() const = 0;
    virtual void setRegion(const QString& region) = 0;

    // ── Search & Browse ──────────────────────────────────────────
    // All methods are async — results arrive via signals on the
    // concrete QObject.  Methods may be no-ops if the service's
    // API is unavailable (check isServiceAuthorized() / isApiAvailable()).
    virtual void searchCatalog(const QString& term, int limit = 25) = 0;
    virtual void fetchAlbumTracks(const QString& albumId) = 0;
    virtual void fetchArtistAlbums(const QString& artistId) = 0;
    virtual void fetchArtistSongs(const QString& artistId) = 0;

    // ── QObject bridge ───────────────────────────────────────────
    // Returns the concrete QObject* for signal/slot connections.
    virtual QObject* asQObject() = 0;
};
