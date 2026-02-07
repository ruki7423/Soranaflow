#include "MacMediaIntegration.h"

#import <MediaPlayer/MediaPlayer.h>
#import <AppKit/NSImage.h>

#include <QDebug>
#include <QMetaObject>

MacMediaIntegration& MacMediaIntegration::instance()
{
    static MacMediaIntegration s;
    return s;
}

void MacMediaIntegration::initialize()
{
    if (m_initialized) return;
    m_initialized = true;

    @autoreleasepool {
        MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];

        // Play
        [center.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            QMetaObject::invokeMethod(&MacMediaIntegration::instance(),
                "playPauseRequested", Qt::QueuedConnection);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Pause
        [center.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            QMetaObject::invokeMethod(&MacMediaIntegration::instance(),
                "playPauseRequested", Qt::QueuedConnection);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Toggle play/pause (media key)
        [center.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            QMetaObject::invokeMethod(&MacMediaIntegration::instance(),
                "playPauseRequested", Qt::QueuedConnection);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Next track
        [center.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            QMetaObject::invokeMethod(&MacMediaIntegration::instance(),
                "nextRequested", Qt::QueuedConnection);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Previous track
        [center.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            QMetaObject::invokeMethod(&MacMediaIntegration::instance(),
                "previousRequested", Qt::QueuedConnection);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Seek (Control Center scrub bar)
        [center.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
            MPChangePlaybackPositionCommandEvent* posEvent =
                static_cast<MPChangePlaybackPositionCommandEvent*>(event);
            double pos = posEvent.positionTime;
            QMetaObject::invokeMethod(&MacMediaIntegration::instance(),
                "seekRequested", Qt::QueuedConnection, Q_ARG(double, pos));
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        center.playCommand.enabled = YES;
        center.pauseCommand.enabled = YES;
        center.togglePlayPauseCommand.enabled = YES;
        center.nextTrackCommand.enabled = YES;
        center.previousTrackCommand.enabled = YES;
        center.changePlaybackPositionCommand.enabled = YES;
    }

    qDebug() << "[MacMedia] Initialized MPRemoteCommandCenter";
}

void MacMediaIntegration::updateNowPlaying(const QString& title,
    const QString& artist, const QString& album,
    double duration, double currentTime, bool isPlaying)
{
    @autoreleasepool {
        NSMutableDictionary* info = [NSMutableDictionary dictionary];
        info[MPMediaItemPropertyTitle] = title.toNSString();
        info[MPMediaItemPropertyArtist] = artist.toNSString();
        info[MPMediaItemPropertyAlbumTitle] = album.toNSString();
        info[MPMediaItemPropertyPlaybackDuration] = @(duration);
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(currentTime);
        info[MPNowPlayingInfoPropertyPlaybackRate] = @(isPlaying ? 1.0 : 0.0);

        // Preserve existing artwork if already set
        NSDictionary* existing = [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo;
        if (existing[MPMediaItemPropertyArtwork]) {
            info[MPMediaItemPropertyArtwork] = existing[MPMediaItemPropertyArtwork];
        }

        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
    }
}

void MacMediaIntegration::updateArtwork(const QImage& image)
{
    if (image.isNull()) return;

    @autoreleasepool {
        CGImageRef cgImage = image.toCGImage();
        if (!cgImage) return;

        NSImage* nsImage = [[NSImage alloc] initWithCGImage:cgImage
                                                       size:NSMakeSize(image.width(), image.height())];
        CGImageRelease(cgImage);

        MPMediaItemArtwork* artwork = [[MPMediaItemArtwork alloc]
            initWithBoundsSize:NSMakeSize(300, 300)
            requestHandler:^NSImage*(CGSize) {
                return nsImage;
            }];

        NSMutableDictionary* info =
            [[MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo mutableCopy];
        if (!info) info = [NSMutableDictionary dictionary];
        info[MPMediaItemPropertyArtwork] = artwork;
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
    }

    qDebug() << "[MacMedia] Artwork updated:" << image.size();
}

void MacMediaIntegration::clearNowPlaying()
{
    @autoreleasepool {
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
    }
    qDebug() << "[MacMedia] Now Playing cleared";
}
