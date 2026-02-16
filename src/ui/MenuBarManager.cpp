#include "MenuBarManager.h"
#include "../core/PlaybackState.h"
#include "../apple/MusicKitPlayer.h"
#include "../core/CoverArtLoader.h"

#include <QMainWindow>
#include <QMenuBar>
#include <QShortcut>
#include <QApplication>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>

#ifdef Q_OS_MAC
#include "../platform/macos/MacMediaIntegration.h"
#endif

static bool isTextInputFocused()
{
    QWidget* w = QApplication::focusWidget();
    if (w) {
        QString className = QString::fromLatin1(w->metaObject()->className());
        if (className.contains(QStringLiteral("WebEngine")) ||
            className.contains(QStringLiteral("RenderWidget")) ||
            className.contains(QStringLiteral("QtWebEngine"))) {
            return true;
        }
        if (qobject_cast<QLineEdit*>(w) ||
            qobject_cast<QTextEdit*>(w) ||
            qobject_cast<QPlainTextEdit*>(w)) {
            return true;
        }
    }
    return false;
}

MenuBarManager::MenuBarManager(QMainWindow* window)
    : QObject(window)
{
    // ── File menu ────────────────────────────────────────────────────
    QMenu* fileMenu = window->menuBar()->addMenu(tr("File"));
    QAction* quitAction = fileMenu->addAction(tr("Quit Sorana Flow"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered,
            this, &MenuBarManager::quitRequested);

    // ── Global keyboard shortcuts ────────────────────────────────────
    // Space — play/pause (skip if text input is focused)
    auto* spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), window);
    spaceShortcut->setContext(Qt::ApplicationShortcut);
    connect(spaceShortcut, &QShortcut::activated, this, []() {
        if (isTextInputFocused()) return;
        auto* ps = PlaybackState::instance();
        if (ps->currentSource() == PlaybackState::AppleMusic) {
            MusicKitPlayer::instance()->togglePlayPause();
        } else {
            ps->playPause();
        }
    });

    // Ctrl+Left / Ctrl+Right — prev / next
    auto* ctrlLeft = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), window);
    ctrlLeft->setContext(Qt::ApplicationShortcut);
    connect(ctrlLeft, &QShortcut::activated, this, []() {
        PlaybackState::instance()->previous();
    });

    auto* ctrlRight = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), window);
    ctrlRight->setContext(Qt::ApplicationShortcut);
    connect(ctrlRight, &QShortcut::activated, this, []() {
        PlaybackState::instance()->next();
    });

    // Media keys (unconditional)
    auto* mediaPlay = new QShortcut(Qt::Key_MediaPlay, window);
    mediaPlay->setContext(Qt::ApplicationShortcut);
    connect(mediaPlay, &QShortcut::activated, this, []() {
        PlaybackState::instance()->playPause();
    });

    auto* mediaNext = new QShortcut(Qt::Key_MediaNext, window);
    mediaNext->setContext(Qt::ApplicationShortcut);
    connect(mediaNext, &QShortcut::activated, this, []() {
        PlaybackState::instance()->next();
    });

    auto* mediaPrev = new QShortcut(Qt::Key_MediaPrevious, window);
    mediaPrev->setContext(Qt::ApplicationShortcut);
    connect(mediaPrev, &QShortcut::activated, this, []() {
        PlaybackState::instance()->previous();
    });

    // Cmd+F / Ctrl+F → focus search
    auto* searchShortcut = new QShortcut(QKeySequence::Find, window);
    searchShortcut->setContext(Qt::ApplicationShortcut);
    connect(searchShortcut, &QShortcut::activated,
            this, &MenuBarManager::focusSearchRequested);

    // ── macOS Now Playing + Media Keys ───────────────────────────────
#ifdef Q_OS_MAC
    auto& macMedia = MacMediaIntegration::instance();
    macMedia.initialize();

    connect(&macMedia, &MacMediaIntegration::playPauseRequested, this, []() {
        PlaybackState::instance()->playPause();
    });
    connect(&macMedia, &MacMediaIntegration::nextRequested, this, []() {
        PlaybackState::instance()->next();
    });
    connect(&macMedia, &MacMediaIntegration::previousRequested, this, []() {
        PlaybackState::instance()->previous();
    });
    connect(&macMedia, &MacMediaIntegration::seekRequested, this, [](double pos) {
        PlaybackState::instance()->seek(static_cast<int>(pos));
    });

    auto* ps = PlaybackState::instance();

    // Track changed → update Now Playing metadata
    connect(ps, &PlaybackState::trackChanged, this, [](const Track& track) {
        MacMediaIntegration::instance().updateNowPlaying(
            track.title, track.artist, track.album,
            static_cast<double>(track.duration), 0.0, true);
    });

    // Play/pause state changed → update playback rate
    connect(ps, &PlaybackState::playStateChanged, this, [ps](bool playing) {
        const Track& t = ps->currentTrack();
        if (t.title.isEmpty()) return;
        MacMediaIntegration::instance().updateNowPlaying(
            t.title, t.artist, t.album,
            static_cast<double>(t.duration),
            static_cast<double>(ps->currentTime()), playing);
    });

    // Cover art loaded → update Now Playing artwork
    connect(CoverArtLoader::instance(), &CoverArtLoader::coverArtReady,
            this, [ps](const QString& trackPath, const QPixmap& pixmap) {
        if (ps->currentTrack().filePath == trackPath && !pixmap.isNull()) {
            MacMediaIntegration::instance().updateArtwork(pixmap.toImage());
        }
    });
#endif
}
