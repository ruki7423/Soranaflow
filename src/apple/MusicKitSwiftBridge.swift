import Foundation
import MusicKit
import CryptoKit

@objc public class MusicKitSwiftBridge: NSObject {

    // MARK: - Authorization

    @objc public static func requestAuthorization(completion: @escaping (Int) -> Void) {
        Task {
            let status = await MusicAuthorization.request()
            DispatchQueue.main.async {
                switch status {
                case .notDetermined: completion(0)
                case .denied:        completion(1)
                case .restricted:    completion(2)
                case .authorized:    completion(3)
                @unknown default:    completion(0)
                }
            }
        }
    }

    @objc public static func currentAuthorizationStatus() -> Int {
        let status = MusicAuthorization.currentStatus
        switch status {
        case .notDetermined: return 0
        case .denied:        return 1
        case .restricted:    return 2
        case .authorized:    return 3
        @unknown default:    return 0
        }
    }

    // MARK: - Subscription

    @objc public static func checkSubscription(completion: @escaping (Bool) -> Void) {
        Task {
            var canPlay = false
            for await subscription in MusicSubscription.subscriptionUpdates {
                canPlay = subscription.canPlayCatalogContent
                break
            }
            DispatchQueue.main.async {
                completion(canPlay)
            }
        }
    }

    // MARK: - Developer Token (JWT via CryptoKit)

    @objc public static func generateDeveloperToken(
        teamId: String,
        keyId: String,
        privateKeyPEM: String
    ) -> String? {
        // Strip PEM headers and whitespace
        let cleanKey = privateKeyPEM
            .replacingOccurrences(of: "-----BEGIN PRIVATE KEY-----", with: "")
            .replacingOccurrences(of: "-----END PRIVATE KEY-----", with: "")
            .replacingOccurrences(of: "\n", with: "")
            .replacingOccurrences(of: "\r", with: "")
            .trimmingCharacters(in: .whitespaces)

        guard let keyData = Data(base64Encoded: cleanKey) else {
            NSLog("MusicKitSwiftBridge: Failed to decode base64 key data")
            return nil
        }

        // Create P256 signing key from DER representation
        let signingKey: P256.Signing.PrivateKey
        do {
            signingKey = try P256.Signing.PrivateKey(derRepresentation: keyData)
        } catch {
            NSLog("MusicKitSwiftBridge: Failed to create signing key: %@", error.localizedDescription)
            return nil
        }

        // JWT Header
        let header: [String: String] = ["alg": "ES256", "kid": keyId]
        guard let headerData = try? JSONSerialization.data(withJSONObject: header) else { return nil }

        // JWT Payload — token valid for ~6 months
        let now = Int(Date().timeIntervalSince1970)
        let payload: [String: Any] = [
            "iss": teamId,
            "iat": now,
            "exp": now + 15777000
        ]
        guard let payloadData = try? JSONSerialization.data(withJSONObject: payload) else { return nil }

        let headerB64  = base64UrlEncode(headerData)
        let payloadB64 = base64UrlEncode(payloadData)
        let message = "\(headerB64).\(payloadB64)"

        // Sign with ES256
        guard let messageData = message.data(using: .utf8) else { return nil }
        do {
            let signature = try signingKey.signature(for: messageData)
            let sigB64 = base64UrlEncode(signature.rawRepresentation)
            let token = "\(message).\(sigB64)"
            NSLog("MusicKitSwiftBridge: Generated developer token (%d chars)", token.count)
            return token
        } catch {
            NSLog("MusicKitSwiftBridge: Failed to sign JWT: %@", error.localizedDescription)
            return nil
        }
    }

    // MARK: - Catalog Search (native MusicKit — requires proper code signing)

    @objc public static func searchCatalog(
        term: String,
        limit: Int,
        completion: @escaping (String?, String?, String?, String?) -> Void
    ) {
        Task {
            let status = MusicAuthorization.currentStatus
            guard status == .authorized else {
                DispatchQueue.main.async {
                    completion(nil, nil, nil, "Not authorized. Status: \(status)")
                }
                return
            }

            do {
                var request = MusicCatalogSearchRequest(
                    term: term,
                    types: [Song.self, Album.self, Artist.self]
                )
                request.limit = limit

                let response = try await request.response()

                let songs: [[String: Any]] = response.songs.map { song in
                    var dict: [String: Any] = [
                        "id": song.id.rawValue,
                        "title": song.title,
                        "artist": song.artistName,
                        "album": song.albumTitle ?? "",
                        "isAppleMusic": true
                    ]
                    if let dur = song.duration {
                        dict["duration"] = dur
                    }
                    if let art = song.artwork,
                       let url = art.url(width: 300, height: 300) {
                        dict["artworkUrl"] = url.absoluteString
                    }
                    return dict
                }

                let albums: [[String: Any]] = response.albums.map { album in
                    var dict: [String: Any] = [
                        "id": album.id.rawValue,
                        "title": album.title,
                        "artist": album.artistName,
                        "trackCount": album.trackCount,
                        "isAppleMusic": true
                    ]
                    if let art = album.artwork,
                       let url = art.url(width: 300, height: 300) {
                        dict["artworkUrl"] = url.absoluteString
                    }
                    return dict
                }

                let artists: [[String: Any]] = response.artists.map { artist in
                    var dict: [String: Any] = [
                        "id": artist.id.rawValue,
                        "name": artist.name
                    ]
                    if let art = artist.artwork,
                       let url = art.url(width: 300, height: 300) {
                        dict["artworkUrl"] = url.absoluteString
                    }
                    return dict
                }

                let songsData = try JSONSerialization.data(withJSONObject: songs)
                let albumsData = try JSONSerialization.data(withJSONObject: albums)
                let artistsData = try JSONSerialization.data(withJSONObject: artists)

                DispatchQueue.main.async {
                    completion(
                        String(data: songsData, encoding: .utf8),
                        String(data: albumsData, encoding: .utf8),
                        String(data: artistsData, encoding: .utf8),
                        nil
                    )
                }
            } catch {
                DispatchQueue.main.async {
                    let nsError = error as NSError
                    let detail = "[\(nsError.domain) \(nsError.code)] \(error.localizedDescription)"
                    NSLog("MusicKitSwiftBridge: Search error: %@", detail)
                    completion(nil, nil, nil, detail)
                }
            }
        }
    }

    // MARK: - Music User Token

    @objc public static func getUserToken(
        developerToken: String,
        completion: @escaping (String?, Error?) -> Void
    ) {
        Task {
            do {
                // Step 1: Ensure authorization (shows system consent dialog on first run)
                let status = await MusicAuthorization.request()
                guard status == .authorized else {
                    DispatchQueue.main.async {
                        completion(nil, NSError(
                            domain: "MusicKit",
                            code: -1,
                            userInfo: [NSLocalizedDescriptionKey:
                                "MusicKit authorization denied. Status: \(status)"]
                        ))
                    }
                    return
                }

                // Step 2: Get user token from Apple via DefaultMusicTokenProvider
                let provider = DefaultMusicTokenProvider()
                let userToken = try await provider.userToken(
                    for: developerToken,
                    options: [.ignoreCache]
                )

                NSLog("MusicKitSwiftBridge: Music User Token obtained (%d chars)", userToken.count)
                DispatchQueue.main.async {
                    completion(userToken, nil)
                }
            } catch {
                NSLog("MusicKitSwiftBridge: getUserToken error: %@", error.localizedDescription)
                DispatchQueue.main.async {
                    completion(nil, error)
                }
            }
        }
    }

    // MARK: - Helpers

    private static func base64UrlEncode(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}
