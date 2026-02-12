#pragma once

#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QSize>
#include <QPoint>

class Settings : public QObject {
    Q_OBJECT

public:
    static Settings* instance();

    // ── Library ──────────────────────────────────────────────────────
    QStringList libraryFolders() const;
    void setLibraryFolders(const QStringList& folders);
    void addLibraryFolder(const QString& folder);
    void removeLibraryFolder(const QString& folder);

    bool autoScanOnStartup() const;
    void setAutoScanOnStartup(bool enabled);

    bool watchForChanges() const;
    void setWatchForChanges(bool enabled);

    QStringList ignoreExtensions() const;
    void setIgnoreExtensions(const QStringList& exts);

    // ── Audio ────────────────────────────────────────────────────────
    int volume() const;
    void setVolume(int vol);

    uint32_t outputDeviceId() const;
    void setOutputDeviceId(uint32_t deviceId);

    // Persistent device identification (UID survives reboots, numeric ID does not)
    QString outputDeviceUID() const;
    void setOutputDeviceUID(const QString& uid);

    QString outputDeviceName() const;
    void setOutputDeviceName(const QString& name);

    bool exclusiveMode() const;
    void setExclusiveMode(bool enabled);

    // ── Processing ──────────────────────────────────────────────────
    QString bufferSize() const;
    void setBufferSize(const QString& size);

    QString sampleRateConversion() const;
    void setSampleRateConversion(const QString& mode);

    // ── DSD ─────────────────────────────────────────────────────────
    // DSD playback mode: "pcm" (default, works everywhere) or "dop" (external DAC only)
    QString dsdPlaybackMode() const;
    void setDsdPlaybackMode(const QString& mode);

    // ── Quality ─────────────────────────────────────────────────────
    // Bit-perfect mode: skip all DSP processing (gain, EQ, plugins)
    bool bitPerfectMode() const;
    void setBitPerfectMode(bool enabled);

    // Auto sample rate: match output rate to source file rate
    bool autoSampleRate() const;
    void setAutoSampleRate(bool enabled);

    // DSD output quality: target PCM rate for DSD conversion
    // "44100", "88200", "176400", "352800", "auto"
    QString dsdOutputQuality() const;
    void setDsdOutputQuality(const QString& quality);

    // ── Resampling ──────────────────────────────────────────────────
    bool resamplingEnabled() const;
    void setResamplingEnabled(bool enabled);

    int targetSampleRate() const;
    void setTargetSampleRate(int rate);

    // ── Upsampling ──────────────────────────────────────────────────
    bool upsamplingEnabled() const;
    void setUpsamplingEnabled(bool enabled);

    int upsamplingMode() const;
    void setUpsamplingMode(int mode);

    int upsamplingQuality() const;
    void setUpsamplingQuality(int quality);

    int upsamplingFilter() const;
    void setUpsamplingFilter(int filter);

    int upsamplingFixedRate() const;
    void setUpsamplingFixedRate(int rate);

    // ── DSP ──────────────────────────────────────────────────────────
    bool dspEnabled() const;
    void setDspEnabled(bool enabled);

    float preampGain() const;
    void setPreampGain(float db);

    float eqLow() const;
    void setEqLow(float db);

    float eqMid() const;
    void setEqMid(float db);

    float eqHigh() const;
    void setEqHigh(float db);

    // 10-band EQ (legacy)
    float eqBand(int band) const;
    void setEqBand(int band, float db);

    QString eqPreset() const;
    void setEqPreset(const QString& preset);

    // 20-band parametric EQ
    int eqActiveBands() const;
    void setEqActiveBands(int count);

    float eqBandFreq(int band) const;
    void setEqBandFreq(int band, float hz);

    float eqBandGain(int band) const;
    void setEqBandGain(int band, float db);

    float eqBandQ(int band) const;
    void setEqBandQ(int band, float q);

    int eqBandType(int band) const;
    void setEqBandType(int band, int type);

    bool eqBandEnabled(int band) const;
    void setEqBandEnabled(int band, bool enabled);

    // ── VST ──────────────────────────────────────────────────────────
    QStringList activeVstPlugins() const;
    void setActiveVstPlugins(const QStringList& paths);

    // ── Volume Leveling ──────────────────────────────────────────────
    bool volumeLeveling() const;
    void setVolumeLeveling(bool enabled);
    int levelingMode() const;          // 0=Track, 1=Album
    void setLevelingMode(int mode);
    double targetLoudness() const;     // LUFS, default -14.0
    void setTargetLoudness(double lufs);

    // ── Headroom Management ─────────────────────────────────────────
    enum class HeadroomMode { Off, Auto, Manual };
    Q_ENUM(HeadroomMode)

    HeadroomMode headroomMode() const;
    void setHeadroomMode(HeadroomMode mode);
    double manualHeadroom() const;
    void setManualHeadroom(double dB);

    // ── Crossfeed ─────────────────────────────────────────────────────
    bool crossfeedEnabled() const;
    void setCrossfeedEnabled(bool enabled);
    int crossfeedLevel() const;
    void setCrossfeedLevel(int level);

    // ── Convolution (Room Correction / IR Loading) ────────────────────
    bool convolutionEnabled() const;
    void setConvolutionEnabled(bool enabled);
    QString convolutionIRPath() const;
    void setConvolutionIRPath(const QString& path);

    // ── HRTF (Binaural Spatial Audio) ────────────────────────────────
    bool hrtfEnabled() const;
    void setHrtfEnabled(bool enabled);
    QString hrtfSofaPath() const;
    void setHrtfSofaPath(const QString& path);
    float hrtfSpeakerAngle() const;
    void setHrtfSpeakerAngle(float degrees);

    // ── Autoplay / Radio ──────────────────────────────────────────────
    bool autoplayEnabled() const;
    void setAutoplayEnabled(bool enabled);

    // ── Playback ─────────────────────────────────────────────────────
    bool gaplessPlayback() const;
    void setGaplessPlayback(bool enabled);

    int crossfadeDurationMs() const;
    void setCrossfadeDurationMs(int ms);

    bool shuffleEnabled() const;
    void setShuffleEnabled(bool enabled);

    int repeatMode() const;
    void setRepeatMode(int mode);

    QString lastTrackId() const;
    void setLastTrackId(const QString& id);

    int lastTrackPosition() const;
    void setLastTrackPosition(int secs);

    // ── Auto-Organize ─────────────────────────────────────────────────
    bool autoOrganizeOnImport() const;
    void setAutoOrganizeOnImport(bool enabled);

    QString organizePattern() const;
    void setOrganizePattern(const QString& pattern);

    // ── Appearance ───────────────────────────────────────────────────
    int themeIndex() const;
    void setThemeIndex(int index);

    // ── Language ─────────────────────────────────────────────────────
    // "auto" = follow system locale, or explicit: "en", "ko", "ja", "zh"
    QString language() const;
    void setLanguage(const QString& lang);

    // ── Window ───────────────────────────────────────────────────────
    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray& geometry);

    QSize windowSize() const;
    void setWindowSize(const QSize& size);

    QPoint windowPosition() const;
    void setWindowPosition(const QPoint& pos);

    // ── Generic access (for external callers) ────────────────────────
    QVariant value(const QString& key, const QVariant& defaultValue = QVariant()) const;
    void setValue(const QString& key, const QVariant& value);
    void remove(const QString& key);
    void sync() { m_settings.sync(); }

    // INI file path — use to create local QSettings(IniFormat) in other files
    static QString settingsPath();

signals:
    void libraryFoldersChanged(const QStringList& folders);
    void autoScanOnStartupChanged(bool enabled);
    void watchForChangesChanged(bool enabled);
    void volumeLevelingChanged(bool enabled);
    void levelingModeChanged(int mode);
    void targetLoudnessChanged(double lufs);
    void headroomChanged();
    void crossfeedChanged();
    void convolutionChanged();
    void hrtfChanged();
    void autoplayEnabledChanged(bool enabled);
    void languageChanged();

private:
    explicit Settings(QObject* parent = nullptr);
    QSettings m_settings;
};
