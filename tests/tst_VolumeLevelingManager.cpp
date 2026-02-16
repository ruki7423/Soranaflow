#include <QtTest/QtTest>
#include <QSignalSpy>
#include <cmath>
#include "core/audio/VolumeLevelingManager.h"
#include "core/Settings.h"

// Helper: create a Track with ReplayGain data
static Track makeRGTrack(double trackGain, double trackPeak,
                         double albumGain = 0.0, double albumPeak = 1.0)
{
    Track t;
    t.filePath = QStringLiteral("/fake/rg_track.flac");
    t.title = QStringLiteral("RG Track");
    t.hasReplayGain = true;
    t.replayGainTrack = trackGain;
    t.replayGainTrackPeak = trackPeak;
    t.replayGainAlbum = albumGain;
    t.replayGainAlbumPeak = albumPeak;
    return t;
}

// Helper: create a Track with R128 data
static Track makeR128Track(double loudness, double peak = 0.0)
{
    Track t;
    t.filePath = QStringLiteral("/fake/r128_track.flac");
    t.title = QStringLiteral("R128 Track");
    t.hasReplayGain = false;
    t.hasR128 = true;
    t.r128Loudness = loudness;
    t.r128Peak = peak;
    return t;
}

class tst_VolumeLevelingManager : public QObject {
    Q_OBJECT

private:
    void enableLeveling(double targetLUFS = -14.0, int mode = 0)
    {
        Settings::instance()->setVolumeLeveling(true);
        Settings::instance()->setTargetLoudness(targetLUFS);
        Settings::instance()->setLevelingMode(mode);
    }

    void disableLeveling()
    {
        Settings::instance()->setVolumeLeveling(false);
    }

private slots:
    void init()
    {
        // Reset settings before each test
        disableLeveling();
    }

    // ── Initial state ────────────────────────────────────────────
    void initialState_unityGain()
    {
        VolumeLevelingManager vlm;
        QCOMPARE(vlm.gainLinear(), 1.0f);
        QCOMPARE(vlm.gainDb(), 0.0f);
    }

    // ── Leveling disabled → always unity ─────────────────────────
    void disabled_alwaysUnity()
    {
        disableLeveling();
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeRGTrack(-6.0, 0.8));
        QCOMPARE(vlm.gainLinear(), 1.0f);
    }

    // ── Empty filePath → unity ───────────────────────────────────
    void emptyFilePath_unity()
    {
        enableLeveling();
        VolumeLevelingManager vlm;
        Track t;
        t.hasReplayGain = true;
        t.replayGainTrack = -5.0;
        // filePath is empty
        vlm.setCurrentTrack(t);
        QCOMPARE(vlm.gainLinear(), 1.0f);
    }

    // ── No gain data → unity ─────────────────────────────────────
    void noGainData_unity()
    {
        enableLeveling();
        VolumeLevelingManager vlm;
        Track t;
        t.filePath = QStringLiteral("/fake/no_data.flac");
        t.hasReplayGain = false;
        t.hasR128 = false;
        vlm.setCurrentTrack(t);
        QCOMPARE(vlm.gainLinear(), 1.0f);
    }

    // ── ReplayGain: track mode, target = RG reference ────────────
    void replayGain_trackMode_atReference()
    {
        // RG reference = -18 LUFS. Target = -18. RG gain = 0.0
        // gainDB = 0.0 + (-18.0 - (-18.0)) = 0.0
        enableLeveling(-18.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeRGTrack(0.0, 1.0));
        QVERIFY(std::abs(vlm.gainLinear() - 1.0f) < 0.001f);
    }

    // ── ReplayGain: track mode, louder target ────────────────────
    void replayGain_trackMode_louderTarget()
    {
        // RG gain = -6.0, target = -14.0, ref = -18.0
        // gainDB = -6.0 + (-14.0 - (-18.0)) = -6.0 + 4.0 = -2.0
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeRGTrack(-6.0, 1.0));
        double expectedDb = -2.0;
        double expectedLinear = std::pow(10.0, expectedDb / 20.0);
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    // ── ReplayGain: album mode uses album gain ───────────────────
    void replayGain_albumMode_usesAlbumGain()
    {
        // mode=1 (Album), albumGain = -3.0, target = -14.0
        // gainDB = -3.0 + (-14.0 - (-18.0)) = -3.0 + 4.0 = 1.0
        // albumPeak = 0.8 (!= 1.0, so album peak used); 0.8 * 1.122 = 0.898 < 1.0 → no limiting
        enableLeveling(-14.0, 1);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeRGTrack(-6.0, 0.8, -3.0, 0.8));
        double expectedDb = 1.0;
        double expectedLinear = std::pow(10.0, expectedDb / 20.0);
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    // ── ReplayGain: album mode falls back to track if album = 0 ──
    void replayGain_albumMode_fallbackToTrack()
    {
        // mode=1, albumGain = 0.0, trackGain = -4.0
        // Falls back to trackGain: gainDB = -4.0 + (-14.0 - (-18.0)) = 0.0
        enableLeveling(-14.0, 1);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeRGTrack(-4.0, 1.0, 0.0, 1.0));
        double expectedDb = 0.0;
        QVERIFY(std::abs(vlm.gainLinear() - 1.0f) < 0.001f);
    }

    // ── ReplayGain: peak limiting ────────────────────────────────
    void replayGain_peakLimiting()
    {
        // Gain would push above peak limit
        // trackGain = 6.0, target = -14.0, ref = -18.0
        // gainDB = 6.0 + 4.0 = 10.0 → linear = 3.16
        // trackPeak = 0.5 → 0.5 * 3.16 = 1.58 > 1.0 → clamp
        // linearGain = 1.0 / 0.5 = 2.0
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeRGTrack(6.0, 0.5));
        double expectedLinear = 1.0 / 0.5;  // 2.0
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    // ── ReplayGain: no peak limiting when below 1.0 ─────────────
    void replayGain_noPeakLimitingWhenSafe()
    {
        // trackGain = -2.0, target = -18.0
        // gainDB = -2.0 + 0.0 = -2.0 → linear ≈ 0.794
        // trackPeak = 0.9 → 0.9 * 0.794 = 0.715 < 1.0 → no limiting
        enableLeveling(-18.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeRGTrack(-2.0, 0.9));
        double expectedDb = -2.0;
        double expectedLinear = std::pow(10.0, expectedDb / 20.0);
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    // ── R128: simple gain calculation ────────────────────────────
    void r128_simpleGain()
    {
        // loudness = -20.0, target = -14.0
        // gainDB = -14.0 - (-20.0) = 6.0
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeR128Track(-20.0));
        double expectedDb = 6.0;
        double expectedLinear = std::pow(10.0, expectedDb / 20.0);
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    // ── R128: loud track → negative gain ─────────────────────────
    void r128_loudTrack_negativeGain()
    {
        // loudness = -10.0, target = -14.0
        // gainDB = -14.0 - (-10.0) = -4.0
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeR128Track(-10.0));
        double expectedDb = -4.0;
        double expectedLinear = std::pow(10.0, expectedDb / 20.0);
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    // ── Clamp: gain clamped to ±12 dB ────────────────────────────
    void clamp_maxGain()
    {
        // R128: loudness = -30.0, target = -14.0
        // gainDB = -14.0 - (-30.0) = 16.0 → clamped to 12.0
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeR128Track(-30.0));
        double expectedLinear = std::pow(10.0, 12.0 / 20.0);
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    void clamp_minGain()
    {
        // R128: loudness = -1.0, target = -14.0
        // gainDB = -14.0 - (-1.0) = -13.0 → clamped to -12.0
        // (r128Loudness == 0.0 is treated as "no data" by the code)
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeR128Track(-1.0));
        double expectedLinear = std::pow(10.0, -12.0 / 20.0);
        QVERIFY(std::abs(vlm.gainLinear() - static_cast<float>(expectedLinear)) < 0.001f);
    }

    // ── gainDb() conversion ──────────────────────────────────────
    void gainDb_conversion()
    {
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeR128Track(-20.0));
        // gainDB should be 6.0
        QVERIFY(std::abs(vlm.gainDb() - 6.0f) < 0.1f);
    }

    void gainDb_unityReturnsZero()
    {
        VolumeLevelingManager vlm;
        QCOMPARE(vlm.gainDb(), 0.0f);
    }

    // ── gainChanged signal ───────────────────────────────────────
    void gainChanged_emitted()
    {
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        QSignalSpy spy(&vlm, &VolumeLevelingManager::gainChanged);

        vlm.setCurrentTrack(makeRGTrack(-6.0, 1.0));
        QVERIFY(spy.count() >= 1);
    }

    void gainChanged_emittedWhenDisabled()
    {
        disableLeveling();
        VolumeLevelingManager vlm;
        QSignalSpy spy(&vlm, &VolumeLevelingManager::gainChanged);

        vlm.setCurrentTrack(makeRGTrack(-6.0, 1.0));
        QVERIFY(spy.count() >= 1);  // still emits, just with unity gain
    }

    // ── currentTrack() accessor ──────────────────────────────────
    void currentTrack_updated()
    {
        enableLeveling();
        VolumeLevelingManager vlm;
        auto t = makeRGTrack(-3.0, 1.0);
        vlm.setCurrentTrack(t);
        QCOMPARE(vlm.currentTrack().filePath, t.filePath);
        QVERIFY(vlm.currentTrack().hasReplayGain);
    }

    // ── Target loudness affects gain ─────────────────────────────
    void targetLoudness_affectsGain()
    {
        enableLeveling(-14.0, 0);
        VolumeLevelingManager vlm;
        vlm.setCurrentTrack(makeR128Track(-20.0));
        float gain14 = vlm.gainLinear();

        enableLeveling(-18.0, 0);
        vlm.setCurrentTrack(makeR128Track(-20.0));
        float gain18 = vlm.gainLinear();

        // -14 target needs more gain boost than -18 for same track
        QVERIFY(gain14 > gain18);
    }
};

QTEST_MAIN(tst_VolumeLevelingManager)
#include "tst_VolumeLevelingManager.moc"
