#include "SparkleUpdater.h"

#import <Sparkle/Sparkle.h>

// ── Singleton ───────────────────────────────────────────────────────
SparkleUpdater* SparkleUpdater::instance()
{
    static SparkleUpdater s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
SparkleUpdater::SparkleUpdater(QObject* parent)
    : QObject(parent)
    , m_updaterController(nullptr)
{
    @autoreleasepool {
        SPUStandardUpdaterController* controller =
            [[SPUStandardUpdaterController alloc]
                initWithStartingUpdater:YES
                updaterDelegate:nil
                userDriverDelegate:nil];
        m_updaterController = (__bridge_retained void*)controller;
    }
}

// ── Destructor ──────────────────────────────────────────────────────
SparkleUpdater::~SparkleUpdater()
{
    if (m_updaterController) {
        CFRelease(m_updaterController);
        m_updaterController = nullptr;
    }
}

// ── Check for updates (user-initiated, shows UI) ────────────────────
void SparkleUpdater::checkForUpdates()
{
    @autoreleasepool {
        SPUStandardUpdaterController* controller =
            (__bridge SPUStandardUpdaterController*)m_updaterController;
        [controller checkForUpdates:nil];
    }
}

// ── Check for updates silently (on startup) ─────────────────────────
void SparkleUpdater::checkForUpdatesInBackground()
{
    // Sparkle handles automatic background checks via SUScheduledCheckInterval
    // in Info.plist. SPUStandardUpdaterController starts checking automatically
    // when initialized with startingUpdater:YES.
}
