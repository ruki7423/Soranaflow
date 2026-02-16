#include <QApplication>
#include <QFontDatabase>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QLockFile>
#include <QSqlQuery>
#include <QSqlError>
#include <QTimer>
#include <QTranslator>
#include <QLocale>
#include <QStandardPaths>
#include <QDateTime>
#include <QMutex>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

// Signal handler: if we crash during shutdown, just exit immediately
static void shutdownCrashHandler(int sig)
{
    (void)sig;
#ifdef Q_OS_WIN
    ExitProcess(0);
#else
    _exit(0);
#endif
}
#include "core/CrashHandler.h"
#include "core/MusicData.h"
#include "core/ThemeManager.h"
#include "core/PlaybackState.h"
#include "core/Settings.h"
#include "core/ServiceLocator.h"
#include "core/audio/AudioEngine.h"
#include "core/audio/AudioDeviceManager.h"
#include "core/dsp/DSPPipeline.h"
#include "core/library/LibraryDatabase.h"
#include "core/library/LibraryScanner.h"
#include "core/library/PlaylistManager.h"
#include "ui/MainWindow.h"
#ifdef Q_OS_MACOS
#include "platform/macos/BookmarkManager.h"
#include "platform/macos/SparkleUpdater.h"
#include "platform/macos/AudioProcessTap.h"
#include "apple/AppleMusicManager.h"
#endif

// ── i18n: Translation loading ────────────────────────────────────────
static QTranslator* s_appTranslator = nullptr;

static void loadLanguage(QApplication& app, const QString& lang)
{
    if (s_appTranslator) {
        app.removeTranslator(s_appTranslator);
        delete s_appTranslator;
        s_appTranslator = nullptr;
    }

    QString locale = lang;
    if (locale == QStringLiteral("auto")) {
        locale = QLocale::system().name().left(2); // "ko", "ja", "zh", "en"
    }

    if (locale == QStringLiteral("en")) return; // English is source, no translator needed

    s_appTranslator = new QTranslator();

    // Try loading from Qt resource system (embedded .qm files)
    QString qrcPath = QStringLiteral(":/translations/soranaflow_%1.qm").arg(locale);
    if (s_appTranslator->load(qrcPath)) {
        app.installTranslator(s_appTranslator);
        qDebug() << "[i18n] Loaded translation:" << locale << "(from qrc)";
        return;
    }

    // Fallback: try app bundle Resources/translations/
    QString bundlePath = QCoreApplication::applicationDirPath()
                       + QStringLiteral("/../Resources/translations/soranaflow_%1.qm").arg(locale);
    if (s_appTranslator->load(bundlePath)) {
        app.installTranslator(s_appTranslator);
        qDebug() << "[i18n] Loaded translation:" << locale << bundlePath;
        return;
    }

    qDebug() << "[i18n] No translation found for" << locale;
    delete s_appTranslator;
    s_appTranslator = nullptr;
}

int main(int argc, char* argv[]) {
    CrashHandler::install();  // must be first — captures crash backtraces
    std::cout << "[STARTUP] Sorana Flow initializing..." << std::endl;

    // Disable macOS window restoration + reduce native widget overhead
    QCoreApplication::setAttribute(Qt::AA_DisableSessionManager);
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);

    QApplication app(argc, argv);

    // ── File logging for Finder-launch diagnostics ──────────────────
    static QFile s_logFile;
    {
        QString logPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                          + QStringLiteral("/soranaflow-debug.log");
        s_logFile.setFileName(logPath);
        s_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate);

        qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& msg) {
            static QMutex mtx;
            QMutexLocker lock(&mtx);
            QString line = QStringLiteral("[%1] %2\n")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), msg);
            QByteArray utf8 = line.toUtf8();
            s_logFile.write(utf8);
            s_logFile.flush();
            fprintf(stderr, "%s", utf8.constData());
        });

        qDebug() << "=== SoranaFlow launched ==="
                 << "Log:" << logPath
                 << "PID:" << QCoreApplication::applicationPid();
    }

    app.setOrganizationDomain("soranaflow.com");
    app.setOrganizationName("SoranaFlow");
    app.setApplicationName("Sorana Flow");
    app.setApplicationVersion(APP_VERSION);

    // Track app activation state (crucial for Finder launch diagnosis)
    QObject::connect(&app, &QApplication::applicationStateChanged,
        [](Qt::ApplicationState state) {
        qDebug() << "[App] State changed:" << static_cast<int>(state)
                 << (state == Qt::ApplicationActive ? "ACTIVE" :
                     state == Qt::ApplicationInactive ? "INACTIVE" : "HIDDEN");
    });

    // Single-instance guard — prevent duplicate launches
    QLockFile lockFile(QDir::tempPath() + QStringLiteral("/soranaflow.lock"));
    if (!lockFile.tryLock(100)) {
        qWarning() << "SoranaFlow is already running — exiting duplicate instance.";
        return 1;
    }

    // Check for previous crash
    {
        QString crashLog = CrashHandler::crashLogPath();
        if (QFile::exists(crashLog)) {
            qWarning() << "[STARTUP] Previous crash detected:" << crashLog;
            QString prevPath = crashLog;
            prevPath.replace(QStringLiteral(".log"), QStringLiteral("_prev.log"));
            QFile::remove(prevPath);
            QFile::rename(crashLog, prevPath);
        }
    }

    // Set default font
    QFont defaultFont = app.font();
    defaultFont.setPointSize(13);
    app.setFont(defaultFont);

    // Initialize Sparkle auto-updater (must be early, before window)
#ifdef Q_OS_MACOS
    SparkleUpdater::instance();
#endif

    // ── Phase 0: Lightweight init needed for UI ──────────────────────
    Settings::instance();

    // Load translations before any UI is created
    loadLanguage(app, Settings::instance()->language());

    int themeIdx = Settings::instance()->themeIndex();
    ThemeManager::instance()->setTheme(static_cast<ThemeManager::Theme>(themeIdx));

    // Restore security-scoped bookmarks before any file I/O
#ifdef Q_OS_MACOS
    BookmarkManager::instance()->restoreAllBookmarks();
    // Save bookmarks for any folders added before BookmarkManager existed
    BookmarkManager::instance()->ensureBookmarks(Settings::instance()->libraryFolders());

    // Load Apple Music developer credentials for REST API fallback
    {
        auto* am = AppleMusicManager::instance();
        // Search for .p8 key file (dev builds); falls back to embedded token
        QString keyPath;
        QStringList keySearchPaths = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/AuthKey_4GW6686CH4.p8"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/AuthKey_4GW6686CH4.p8"),
        };
#ifdef QT_DEBUG
        // Debug: also search path from SORANA_KEY_DIR env var
        QByteArray envKeyDir = qgetenv("SORANA_KEY_DIR");
        if (!envKeyDir.isEmpty())
            keySearchPaths.append(QString::fromUtf8(envKeyDir) + QStringLiteral("/AuthKey_4GW6686CH4.p8"));
#endif
        for (const QString& path : keySearchPaths) {
            if (QFile::exists(path)) {
                keyPath = path;
                break;
            }
        }
        am->loadDeveloperCredentials(
            QStringLiteral("W5JMPJXB5H"),
            QStringLiteral("4GW6686CH4"),
            keyPath);
        if (!am->hasDeveloperToken()) {
            qWarning() << "[AppleMusicManager] ERROR: No developer token available";
        }
    }
#endif

    // Open database (just the connection, no heavy queries)
    LibraryDatabase::instance()->open();

    // Create singletons (lightweight construction)
    MusicDataProvider::instance();
    PlaybackState::instance();
    PlaylistManager::instance();

    // Register services for dependency injection
    ServiceLocator::provide<Settings>(Settings::instance());
    ServiceLocator::provide<ThemeManager>(ThemeManager::instance());
    ServiceLocator::provide<LibraryDatabase>(LibraryDatabase::instance());
    ServiceLocator::provide<MusicDataProvider>(MusicDataProvider::instance());
    ServiceLocator::provide<PlaybackState>(PlaybackState::instance());
    ServiceLocator::provide<PlaylistManager>(PlaylistManager::instance());
#ifdef Q_OS_MACOS
    ServiceLocator::provide<AppleMusicManager>(AppleMusicManager::instance());
#endif

    // Hide-on-close: don't quit when last window closes (background playback)
    QApplication::setQuitOnLastWindowClosed(false);

    // ── Show window IMMEDIATELY ──────────────────────────────────────
    MainWindow window;
    QByteArray savedGeo = Settings::instance()->windowGeometry();
    if (!savedGeo.isEmpty()) {
        window.restoreGeometry(savedGeo);
    }
    window.show();
    app.processEvents();   // Force first paint

    qDebug() << "[STARTUP] MainWindow shown, isActiveWindow=" << window.isActiveWindow()
             << "appState=" << static_cast<int>(app.applicationState());

    // ── Phase 1: Audio engine (deferred, after window visible) ───────
    QTimer::singleShot(0, [&window]() {
        AudioEngine::instance();
        AudioDeviceManager::instance()->startMonitoring();
        ServiceLocator::provide<AudioEngine>(AudioEngine::instance());
        ServiceLocator::provide<AudioDeviceManager>(AudioDeviceManager::instance());

        // Restore playback settings
        auto* settings = Settings::instance();
        PlaybackState::instance()->setVolume(settings->volume());
        if (settings->shuffleEnabled()) {
            PlaybackState::instance()->toggleShuffle();
        }
        for (int i = 0; i < settings->repeatMode(); ++i) {
            PlaybackState::instance()->cycleRepeat();
        }

        // Restore DSP settings
        auto* pipeline = AudioEngine::instance()->dspPipeline();
        if (pipeline) {
            bool dspOn = settings->dspEnabled();
            pipeline->setEnabled(dspOn);
            qDebug() << "[STARTUP] DSP pipeline enabled:" << dspOn;
            pipeline->gainProcessor()->setGainDb(settings->preampGain());
            pipeline->equalizerProcessor()->setBand(0, 250.0f, settings->eqLow(), 1.0f);
            pipeline->equalizerProcessor()->setBand(1, 1000.0f, settings->eqMid(), 1.0f);
            pipeline->equalizerProcessor()->setBand(2, 4000.0f, settings->eqHigh(), 1.0f);
        }

        // Restore output device — prefer UID (persistent), fall back to name, then numeric ID
        {
            uint32_t resolvedId = 0;
            QString savedUID = settings->outputDeviceUID();
            QString savedName = settings->outputDeviceName();
            uint32_t savedNumericId = settings->outputDeviceId();

            auto* adm = AudioDeviceManager::instance();

            if (!savedUID.isEmpty()) {
                resolvedId = adm->deviceIdFromUID(savedUID);
                if (resolvedId != 0) {
                    auto info = adm->deviceById(resolvedId);
                    qDebug() << "[STARTUP] Device restored by UID:" << savedUID
                             << "-> ID:" << resolvedId << info.name;
                } else {
                    qDebug() << "[STARTUP] Saved device" << savedUID << "not available";
                }
            }

            if (resolvedId == 0 && !savedName.isEmpty()) {
                resolvedId = adm->deviceIdFromName(savedName);
                if (resolvedId != 0) {
                    qDebug() << "[STARTUP] Device restored by name:" << savedName
                             << "-> ID:" << resolvedId;
                }
            }

            if (resolvedId == 0 && savedNumericId != 0) {
                auto info = adm->deviceById(savedNumericId);
                if (info.deviceId != 0) {
                    resolvedId = savedNumericId;
                    qDebug() << "[STARTUP] Device restored by numeric ID:" << savedNumericId
                             << info.name;
                }
            }

            if (resolvedId != 0) {
                AudioEngine::instance()->setOutputDevice(resolvedId);
                // Re-save UID/name in case they were missing
                auto info = adm->deviceById(resolvedId);
                if (!info.uid.isEmpty()) settings->setOutputDeviceUID(info.uid);
                if (!info.name.isEmpty()) settings->setOutputDeviceName(info.name);
                settings->setOutputDeviceId(resolvedId);
            } else if (savedNumericId != 0 || !savedUID.isEmpty()) {
                qDebug() << "[STARTUP] Saved device not available, using default";
            }
        }

        std::cout << "[STARTUP] Audio engine initialized" << std::endl;

        // ── Phase 2: Library data (after audio is ready) ─────────
        QTimer::singleShot(0, [&window]() {
            // Connect database changes to MusicDataProvider
            QObject::connect(LibraryDatabase::instance(), &LibraryDatabase::databaseChanged,
                             MusicDataProvider::instance(), &MusicDataProvider::reloadFromDatabase);

            // Reload again after scan finishes — m_scanning is now false,
            // so albums/artists will be loaded (databaseChanged fires while
            // scan is still flagged as running, causing albums to be skipped)
            QObject::connect(LibraryScanner::instance(), &LibraryScanner::scanFinished,
                             MusicDataProvider::instance(), &MusicDataProvider::reloadFromDatabase);

            MusicDataProvider::instance()->reloadFromDatabase();

            // Set queue from loaded tracks only if no queue was restored
            // from the previous session.  restoreQueueFromSettings() already
            // populated the queue with the saved index and current track —
            // calling setQueue() again would reset the index to 0 and lose
            // the restored Now Playing state.
            auto* ps = PlaybackState::instance();
            if (ps->queue().isEmpty()) {
                auto tracks = MusicDataProvider::instance()->allTracks();
                if (!tracks.isEmpty()) {
                    ps->setQueue(tracks);
                }
            }

            std::cout << "[STARTUP] Library loaded (" << MusicDataProvider::instance()->allTracks().size()
                      << " tracks)" << std::endl;

            // Pre-load restored track into AudioEngine (without playing)
            // so Signal Path populates with Source/Decoder/Output data.
            {
                const Track current = ps->currentTrack();
                if (!current.id.isEmpty() && !current.filePath.isEmpty()) {
                    auto* engine = AudioEngine::instance();
                    engine->setCurrentTrack(current);
                    if (engine->load(current.filePath)) {
                        qDebug() << "[STARTUP] Pre-loaded track for signal path:" << current.title;
                    }
                }
            }

            // Signal deferred init complete
            window.initializeDeferred();

            // ── Phase 3: Background tasks (low priority) ─────
            QTimer::singleShot(500, []() {
                if (Settings::instance()->autoScanOnStartup()) {
                    QStringList folders = Settings::instance()->libraryFolders();
                    if (!folders.isEmpty()) {
                        qDebug() << "[STARTUP] Auto-scan starting for" << folders.size() << "folders";
                        LibraryScanner::instance()->setWatchEnabled(Settings::instance()->watchForChanges());
                        LibraryScanner::instance()->scanFolders(folders);
                    }
                }
                std::cout << "[STARTUP] Background tasks started" << std::endl;
            });
        });
    });

#ifdef QT_DEBUG
    // === DIAGNOSTIC: Verify database state ===
    QTimer::singleShot(1000, []() {
        qDebug() << "=== DATABASE DIAGNOSTIC ===";
        QSqlDatabase db = QSqlDatabase::database(QStringLiteral("library_read"));
        QSqlQuery q(db);

        q.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
        if (q.next()) qDebug() << "  Tracks in DB:" << q.value(0).toInt();

        q.exec(QStringLiteral("SELECT COUNT(*) FROM albums"));
        if (q.next()) qDebug() << "  Albums in DB:" << q.value(0).toInt();

        q.exec(QStringLiteral("SELECT COUNT(*) FROM artists"));
        if (q.next()) qDebug() << "  Artists in DB:" << q.value(0).toInt();

        qDebug() << "  MusicDataProvider allAlbums():" << MusicDataProvider::instance()->allAlbums().size();
        qDebug() << "  MusicDataProvider allArtists():" << MusicDataProvider::instance()->allArtists().size();
        qDebug() << "  MusicDataProvider allTracks():" << MusicDataProvider::instance()->allTracks().size();
        qDebug() << "=== END DIAGNOSTIC ===";
    });
#endif

    // Safe shutdown: stop audio engine before Qt event loop ends
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&window]() {
        qDebug() << "=== aboutToQuit: safe shutdown ===";

        // Safety net: force exit if shutdown hangs longer than 5 seconds
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            qWarning() << "[SHUTDOWN] Timeout after 5s — forcing exit";
            _exit(0);
        }).detach();

        std::signal(SIGSEGV, shutdownCrashHandler);
        std::signal(SIGABRT, shutdownCrashHandler);
        // Flush debounced saves before anything shuts down
        PlaybackState::instance()->flushPendingSaves();
        Settings::instance()->sync();
        qDebug() << "[SHUTDOWN] Settings flushed and synced";

        window.performQuit();
        AudioDeviceManager::instance()->stopMonitoring();
#ifdef Q_OS_MACOS
        AudioProcessTap::instance()->stop();
        qDebug() << "[SHUTDOWN] ProcessTap stopped";
#endif
        // Close DB explicitly before QCoreApplication destructs
        // (prevents "QSqlDatabase requires a QCoreApplication" warning)
        LibraryDatabase::instance()->close();
        qDebug() << "[SHUTDOWN] Database closed";

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        qDebug() << "=== aboutToQuit: shutdown complete ===";
    });

    int ret = app.exec();
    return ret;
}
