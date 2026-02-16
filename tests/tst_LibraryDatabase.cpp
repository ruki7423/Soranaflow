#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QDir>
#include <QFile>
#include "library/LibraryDatabase.h"

static Track makeTrack(const QString& id, const QString& title,
                       const QString& artist = QStringLiteral("Artist"),
                       const QString& album = QStringLiteral("Album"))
{
    Track t;
    t.id = id;
    t.title = title;
    t.artist = artist;
    t.album = album;
    t.filePath = QStringLiteral("/fake/") + id + QStringLiteral(".flac");
    t.duration = 180;
    t.trackNumber = 1;
    t.discNumber = 1;
    return t;
}

class tst_LibraryDatabase : public QObject {
    Q_OBJECT

private:
    LibraryDatabase* m_db = nullptr;
    QString m_dbPath;

private slots:
    void initTestCase()
    {
        m_dbPath = QDir::tempPath() + QStringLiteral("/sorana_test_db.sqlite");
        QFile::remove(m_dbPath);

        m_db = new LibraryDatabase(m_dbPath);
        QVERIFY(m_db->open());
    }

    void cleanupTestCase()
    {
        if (m_db) {
            m_db->close();
            delete m_db;
            m_db = nullptr;
        }
        QFile::remove(m_dbPath);
    }

    void init()
    {
        // Clear data between tests
        m_db->clearAllData(false);
    }

    // ── insertTrack + trackById ──────────────────────────────────
    void insertTrack_andRetrieve()
    {
        Track t = makeTrack(QStringLiteral("t1"), QStringLiteral("Song One"));
        QVERIFY(m_db->insertTrack(t));

        auto retrieved = m_db->trackById(QStringLiteral("t1"));
        QVERIFY(retrieved.has_value());
        QCOMPARE(retrieved->title, QStringLiteral("Song One"));
        QCOMPARE(retrieved->artist, QStringLiteral("Artist"));
        QCOMPARE(retrieved->album, QStringLiteral("Album"));
        QCOMPARE(retrieved->duration, 180);
    }

    // ── trackByPath ──────────────────────────────────────────────
    void trackByPath_found()
    {
        Track t = makeTrack(QStringLiteral("t1"), QStringLiteral("Song"));
        m_db->insertTrack(t);

        auto retrieved = m_db->trackByPath(QStringLiteral("/fake/t1.flac"));
        QVERIFY(retrieved.has_value());
        QCOMPARE(retrieved->id, QStringLiteral("t1"));
    }

    void trackByPath_notFound()
    {
        auto retrieved = m_db->trackByPath(QStringLiteral("/nonexistent.flac"));
        QVERIFY(!retrieved.has_value());
    }

    // ── trackExists ──────────────────────────────────────────────
    void trackExists_true()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        QVERIFY(m_db->trackExists(QStringLiteral("/fake/t1.flac")));
    }

    void trackExists_false()
    {
        QVERIFY(!m_db->trackExists(QStringLiteral("/nonexistent.flac")));
    }

    // ── allTracks ────────────────────────────────────────────────
    void allTracks_empty()
    {
        QCOMPARE(m_db->allTracks().size(), 0);
        QCOMPARE(m_db->trackCount(), 0);
    }

    void allTracks_multiple()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("One")));
        m_db->insertTrack(makeTrack(QStringLiteral("t2"), QStringLiteral("Two")));
        m_db->insertTrack(makeTrack(QStringLiteral("t3"), QStringLiteral("Three")));

        QCOMPARE(m_db->allTracks().size(), 3);
        QCOMPARE(m_db->trackCount(), 3);
    }

    // ── updateTrack ──────────────────────────────────────────────
    void updateTrack_changesFields()
    {
        Track t = makeTrack(QStringLiteral("t1"), QStringLiteral("Original"));
        m_db->insertTrack(t);

        t.title = QStringLiteral("Updated Title");
        t.artist = QStringLiteral("New Artist");
        QVERIFY(m_db->updateTrack(t));

        auto retrieved = m_db->trackById(QStringLiteral("t1"));
        QVERIFY(retrieved.has_value());
        QCOMPARE(retrieved->title, QStringLiteral("Updated Title"));
        QCOMPARE(retrieved->artist, QStringLiteral("New Artist"));
    }

    // ── removeTrack ──────────────────────────────────────────────
    void removeTrack_byId()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        QCOMPARE(m_db->trackCount(), 1);

        QVERIFY(m_db->removeTrack(QStringLiteral("t1")));
        QCOMPARE(m_db->trackCount(), 0);
    }

    void removeTrackByPath()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        QVERIFY(m_db->removeTrackByPath(QStringLiteral("/fake/t1.flac")));
        QCOMPARE(m_db->trackCount(), 0);
    }

    // ── searchTracks ─────────────────────────────────────────────
    void searchTracks_byTitle()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Bohemian Rhapsody"),
                                     QStringLiteral("Queen"), QStringLiteral("ANATO")));
        m_db->insertTrack(makeTrack(QStringLiteral("t2"), QStringLiteral("Stairway to Heaven"),
                                     QStringLiteral("Led Zeppelin"), QStringLiteral("IV")));
        m_db->rebuildFTSIndex();  // FTS5 external content needs manual sync

        auto results = m_db->searchTracks(QStringLiteral("Bohemian"));
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].title, QStringLiteral("Bohemian Rhapsody"));
    }

    void searchTracks_byArtist()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song A"),
                                     QStringLiteral("Pink Floyd")));
        m_db->insertTrack(makeTrack(QStringLiteral("t2"), QStringLiteral("Song B"),
                                     QStringLiteral("Led Zeppelin")));
        m_db->rebuildFTSIndex();

        auto results = m_db->searchTracks(QStringLiteral("Floyd"));
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].artist, QStringLiteral("Pink Floyd"));
    }

    void searchTracks_noMatch()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        auto results = m_db->searchTracks(QStringLiteral("zzznonexistent"));
        QCOMPARE(results.size(), 0);
    }

    // ── Albums ───────────────────────────────────────────────────
    void insertAlbum_andRetrieve()
    {
        Album a;
        a.id = QStringLiteral("a1");
        a.title = QStringLiteral("Dark Side of the Moon");
        a.artist = QStringLiteral("Pink Floyd");
        a.year = 1973;
        QVERIFY(m_db->insertAlbum(a));

        auto all = m_db->allAlbums();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].title, QStringLiteral("Dark Side of the Moon"));
        QCOMPARE(all[0].year, 1973);
    }

    void albumById()
    {
        Album a;
        a.id = QStringLiteral("a1");
        a.title = QStringLiteral("Test Album");
        a.artist = QStringLiteral("Test Artist");
        m_db->insertAlbum(a);

        auto retrieved = m_db->albumById(QStringLiteral("a1"));
        QCOMPARE(retrieved.title, QStringLiteral("Test Album"));
    }

    void searchAlbums()
    {
        Album a1;
        a1.id = QStringLiteral("a1");
        a1.title = QStringLiteral("Abbey Road");
        a1.artist = QStringLiteral("The Beatles");
        m_db->insertAlbum(a1);

        Album a2;
        a2.id = QStringLiteral("a2");
        a2.title = QStringLiteral("OK Computer");
        a2.artist = QStringLiteral("Radiohead");
        m_db->insertAlbum(a2);

        auto results = m_db->searchAlbums(QStringLiteral("Abbey"));
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].title, QStringLiteral("Abbey Road"));
    }

    // ── Artists ──────────────────────────────────────────────────
    void insertArtist_andRetrieve()
    {
        Artist a;
        a.id = QStringLiteral("ar1");
        a.name = QStringLiteral("Radiohead");
        QVERIFY(m_db->insertArtist(a));

        auto all = m_db->allArtists();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].name, QStringLiteral("Radiohead"));
    }

    void searchArtists()
    {
        Artist a1;
        a1.id = QStringLiteral("ar1");
        a1.name = QStringLiteral("Pink Floyd");
        m_db->insertArtist(a1);

        Artist a2;
        a2.id = QStringLiteral("ar2");
        a2.name = QStringLiteral("The Beatles");
        m_db->insertArtist(a2);

        auto results = m_db->searchArtists(QStringLiteral("Pink"));
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].name, QStringLiteral("Pink Floyd"));
    }

    // ── Playlists ────────────────────────────────────────────────
    void insertPlaylist_andRetrieve()
    {
        Playlist p;
        p.id = QStringLiteral("p1");
        p.name = QStringLiteral("My Favorites");
        QVERIFY(m_db->insertPlaylist(p));

        auto all = m_db->allPlaylists();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].name, QStringLiteral("My Favorites"));
    }

    void removePlaylist()
    {
        Playlist p;
        p.id = QStringLiteral("p1");
        p.name = QStringLiteral("To Delete");
        m_db->insertPlaylist(p);
        QCOMPARE(m_db->allPlaylists().size(), 1);

        QVERIFY(m_db->removePlaylist(QStringLiteral("p1")));
        QCOMPARE(m_db->allPlaylists().size(), 0);
    }

    void addTrackToPlaylist()
    {
        Playlist p;
        p.id = QStringLiteral("p1");
        p.name = QStringLiteral("Test");
        m_db->insertPlaylist(p);

        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song 1")));
        m_db->insertTrack(makeTrack(QStringLiteral("t2"), QStringLiteral("Song 2")));

        QVERIFY(m_db->addTrackToPlaylist(QStringLiteral("p1"), QStringLiteral("t1"), 0));
        QVERIFY(m_db->addTrackToPlaylist(QStringLiteral("p1"), QStringLiteral("t2"), 1));

        auto pl = m_db->playlistById(QStringLiteral("p1"));
        QCOMPARE(pl.tracks.size(), 2);
    }

    void removeTrackFromPlaylist()
    {
        Playlist p;
        p.id = QStringLiteral("p1");
        p.name = QStringLiteral("Test");
        m_db->insertPlaylist(p);

        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        m_db->addTrackToPlaylist(QStringLiteral("p1"), QStringLiteral("t1"), 0);

        QVERIFY(m_db->removeTrackFromPlaylist(QStringLiteral("p1"), QStringLiteral("t1")));
        auto pl = m_db->playlistById(QStringLiteral("p1"));
        QCOMPARE(pl.tracks.size(), 0);
    }

    // ── Volume Leveling (R128) ───────────────────────────────────
    void updateR128Loudness()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));

        m_db->updateR128Loudness(QStringLiteral("/fake/t1.flac"), -14.0, 0.95);

        auto retrieved = m_db->trackByPath(QStringLiteral("/fake/t1.flac"));
        QVERIFY(retrieved.has_value());
        QVERIFY(retrieved->hasR128);
        QVERIFY(std::abs(retrieved->r128Loudness - (-14.0)) < 0.01);
    }

    // ── clearAllData ─────────────────────────────────────────────
    void clearAllData_removesEverything()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        Album a;
        a.id = QStringLiteral("a1");
        a.title = QStringLiteral("Album");
        a.artist = QStringLiteral("Artist");
        m_db->insertAlbum(a);

        m_db->clearAllData(false);

        QCOMPARE(m_db->trackCount(), 0);
        QCOMPARE(m_db->allAlbums().size(), 0);
    }

    // ── Duplicate file path → INSERT OR REPLACE (upsert) ──────
    void insertTrack_duplicatePath_replaces()
    {
        Track t1 = makeTrack(QStringLiteral("t1"), QStringLiteral("Original"));
        Track t2 = makeTrack(QStringLiteral("t2"), QStringLiteral("Replacement"));
        t2.filePath = t1.filePath;  // same path

        QVERIFY(m_db->insertTrack(t1));
        QVERIFY(m_db->insertTrack(t2));  // replaces via INSERT OR REPLACE
        QCOMPARE(m_db->trackCount(), 1);  // still just one track

        auto retrieved = m_db->trackByPath(t1.filePath);
        QVERIFY(retrieved.has_value());
        QCOMPARE(retrieved->title, QStringLiteral("Replacement"));
    }

    // ── Transaction helpers ──────────────────────────────────────
    void transaction_commitWorks()
    {
        QVERIFY(m_db->beginTransaction());
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        QVERIFY(m_db->commitTransaction());
        QCOMPARE(m_db->trackCount(), 1);
    }

    // ── databaseChanged signal ───────────────────────────────────
    void databaseChanged_emittedOnClear()
    {
        m_db->insertTrack(makeTrack(QStringLiteral("t1"), QStringLiteral("Song")));
        QSignalSpy spy(m_db, &LibraryDatabase::databaseChanged);

        m_db->clearAllData(false);
        QVERIFY(spy.count() >= 1);
    }
};

QTEST_MAIN(tst_LibraryDatabase)
#include "tst_LibraryDatabase.moc"
