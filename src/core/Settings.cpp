#include "Settings.h"

// ── Singleton ───────────────────────────────────────────────────────
Settings* Settings::instance()
{
    static Settings s;
    return &s;
}

// ── Constructor ─────────────────────────────────────────────────────
Settings::Settings(QObject* parent)
    : QObject(parent)
    , m_settings(QStringLiteral("SoranaFlow"), QStringLiteral("SoranaFlow"))
{
}

// ── Library Folders ─────────────────────────────────────────────────
QStringList Settings::libraryFolders() const
{
    return m_settings.value(QStringLiteral("library/folders")).toStringList();
}

void Settings::setLibraryFolders(const QStringList& folders)
{
    m_settings.setValue(QStringLiteral("library/folders"), folders);
    emit libraryFoldersChanged(folders);
}

void Settings::addLibraryFolder(const QString& folder)
{
    QStringList folders = libraryFolders();
    if (!folders.contains(folder)) {
        folders.append(folder);
        setLibraryFolders(folders);
    }
}

void Settings::removeLibraryFolder(const QString& folder)
{
    QStringList folders = libraryFolders();
    folders.removeAll(folder);
    setLibraryFolders(folders);
}

// ── Auto-scan ───────────────────────────────────────────────────────
bool Settings::autoScanOnStartup() const
{
    return m_settings.value(QStringLiteral("library/autoScan"), true).toBool();
}

void Settings::setAutoScanOnStartup(bool enabled)
{
    m_settings.setValue(QStringLiteral("library/autoScan"), enabled);
    emit autoScanOnStartupChanged(enabled);
}

// ── Watch for changes ───────────────────────────────────────────────
bool Settings::watchForChanges() const
{
    return m_settings.value(QStringLiteral("library/watchChanges"), true).toBool();
}

void Settings::setWatchForChanges(bool enabled)
{
    m_settings.setValue(QStringLiteral("library/watchChanges"), enabled);
    emit watchForChangesChanged(enabled);
}

// ── Volume ──────────────────────────────────────────────────────────
int Settings::volume() const
{
    return m_settings.value(QStringLiteral("audio/volume"), 75).toInt();
}

void Settings::setVolume(int vol)
{
    m_settings.setValue(QStringLiteral("audio/volume"), vol);
}

// ── Output Device ───────────────────────────────────────────────────
uint32_t Settings::outputDeviceId() const
{
    return m_settings.value(QStringLiteral("audio/outputDeviceId"), 0).toUInt();
}

void Settings::setOutputDeviceId(uint32_t deviceId)
{
    m_settings.setValue(QStringLiteral("audio/outputDeviceId"), deviceId);
}

QString Settings::outputDeviceUID() const
{
    return m_settings.value(QStringLiteral("audio/outputDeviceUID")).toString();
}

void Settings::setOutputDeviceUID(const QString& uid)
{
    m_settings.setValue(QStringLiteral("audio/outputDeviceUID"), uid);
}

QString Settings::outputDeviceName() const
{
    return m_settings.value(QStringLiteral("audio/outputDeviceName")).toString();
}

void Settings::setOutputDeviceName(const QString& name)
{
    m_settings.setValue(QStringLiteral("audio/outputDeviceName"), name);
}

// ── Exclusive Mode ──────────────────────────────────────────────────
bool Settings::exclusiveMode() const
{
    return m_settings.value(QStringLiteral("audio/exclusiveMode"), false).toBool();
}

void Settings::setExclusiveMode(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/exclusiveMode"), enabled);
}

// ── Buffer Size ─────────────────────────────────────────────────────
QString Settings::bufferSize() const
{
    return m_settings.value(QStringLiteral("audio/bufferSize"),
                            QStringLiteral("Medium (512)")).toString();
}

void Settings::setBufferSize(const QString& size)
{
    m_settings.setValue(QStringLiteral("audio/bufferSize"), size);
}

// ── Sample Rate Conversion ─────────────────────────────────────────
QString Settings::sampleRateConversion() const
{
    return m_settings.value(QStringLiteral("audio/sampleRateConversion"),
                            QStringLiteral("SoX High Quality")).toString();
}

void Settings::setSampleRateConversion(const QString& mode)
{
    m_settings.setValue(QStringLiteral("audio/sampleRateConversion"), mode);
}

// ── DSD Playback Mode ───────────────────────────────────────────
QString Settings::dsdPlaybackMode() const
{
    return m_settings.value(QStringLiteral("audio/dsdPlaybackMode"),
                            QStringLiteral("pcm")).toString();
}

void Settings::setDsdPlaybackMode(const QString& mode)
{
    m_settings.setValue(QStringLiteral("audio/dsdPlaybackMode"), mode);
}

// ── Bit-Perfect Mode ───────────────────────────────────────────────
bool Settings::bitPerfectMode() const
{
    return m_settings.value(QStringLiteral("audio/bitPerfect"), false).toBool();
}

void Settings::setBitPerfectMode(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/bitPerfect"), enabled);
}

// ── Auto Sample Rate ───────────────────────────────────────────────
bool Settings::autoSampleRate() const
{
    return m_settings.value(QStringLiteral("audio/autoSampleRate"), false).toBool();
}

void Settings::setAutoSampleRate(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/autoSampleRate"), enabled);
}

// ── DSD Output Quality ─────────────────────────────────────────────
QString Settings::dsdOutputQuality() const
{
    return m_settings.value(QStringLiteral("audio/dsdOutputQuality"),
                            QStringLiteral("44100")).toString();
}

void Settings::setDsdOutputQuality(const QString& quality)
{
    m_settings.setValue(QStringLiteral("audio/dsdOutputQuality"), quality);
}

// ── Resampling ──────────────────────────────────────────────────────
bool Settings::resamplingEnabled() const
{
    return m_settings.value(QStringLiteral("audio/resamplingEnabled"), false).toBool();
}

void Settings::setResamplingEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/resamplingEnabled"), enabled);
}

int Settings::targetSampleRate() const
{
    return m_settings.value(QStringLiteral("audio/targetSampleRate"), 44100).toInt();
}

void Settings::setTargetSampleRate(int rate)
{
    m_settings.setValue(QStringLiteral("audio/targetSampleRate"), rate);
}

// ── Upsampling ──────────────────────────────────────────────────────
bool Settings::upsamplingEnabled() const
{
    return m_settings.value(QStringLiteral("audio/upsampling/enabled"), false).toBool();
}

void Settings::setUpsamplingEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/upsampling/enabled"), enabled);
}

int Settings::upsamplingMode() const
{
    return m_settings.value(QStringLiteral("audio/upsampling/mode"), 0).toInt();
}

void Settings::setUpsamplingMode(int mode)
{
    m_settings.setValue(QStringLiteral("audio/upsampling/mode"), mode);
}

int Settings::upsamplingQuality() const
{
    return m_settings.value(QStringLiteral("audio/upsampling/quality"), 3).toInt();
}

void Settings::setUpsamplingQuality(int quality)
{
    m_settings.setValue(QStringLiteral("audio/upsampling/quality"), quality);
}

int Settings::upsamplingFilter() const
{
    return m_settings.value(QStringLiteral("audio/upsampling/filter"), 0).toInt();
}

void Settings::setUpsamplingFilter(int filter)
{
    m_settings.setValue(QStringLiteral("audio/upsampling/filter"), filter);
}

int Settings::upsamplingFixedRate() const
{
    return m_settings.value(QStringLiteral("audio/upsampling/fixedRate"), 352800).toInt();
}

void Settings::setUpsamplingFixedRate(int rate)
{
    m_settings.setValue(QStringLiteral("audio/upsampling/fixedRate"), rate);
}

// ── DSP ─────────────────────────────────────────────────────────────
bool Settings::dspEnabled() const
{
    return m_settings.value(QStringLiteral("dsp/enabled"), true).toBool();
}

void Settings::setDspEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("dsp/enabled"), enabled);
}

float Settings::preampGain() const
{
    return m_settings.value(QStringLiteral("dsp/preampGain"), 0.0f).toFloat();
}

void Settings::setPreampGain(float db)
{
    m_settings.setValue(QStringLiteral("dsp/preampGain"), static_cast<double>(db));
}

float Settings::eqLow() const
{
    return m_settings.value(QStringLiteral("dsp/eqLow"), 0.0f).toFloat();
}

void Settings::setEqLow(float db)
{
    m_settings.setValue(QStringLiteral("dsp/eqLow"), static_cast<double>(db));
}

float Settings::eqMid() const
{
    return m_settings.value(QStringLiteral("dsp/eqMid"), 0.0f).toFloat();
}

void Settings::setEqMid(float db)
{
    m_settings.setValue(QStringLiteral("dsp/eqMid"), static_cast<double>(db));
}

float Settings::eqHigh() const
{
    return m_settings.value(QStringLiteral("dsp/eqHigh"), 0.0f).toFloat();
}

void Settings::setEqHigh(float db)
{
    m_settings.setValue(QStringLiteral("dsp/eqHigh"), static_cast<double>(db));
}

// ── 10-band EQ ──────────────────────────────────────────────────────
float Settings::eqBand(int band) const
{
    return m_settings.value(
        QStringLiteral("dsp/eqBand%1").arg(band), 0.0f).toFloat();
}

void Settings::setEqBand(int band, float db)
{
    m_settings.setValue(
        QStringLiteral("dsp/eqBand%1").arg(band), static_cast<double>(db));
}

QString Settings::eqPreset() const
{
    return m_settings.value(QStringLiteral("dsp/eqPreset"), QStringLiteral("Flat")).toString();
}

void Settings::setEqPreset(const QString& preset)
{
    m_settings.setValue(QStringLiteral("dsp/eqPreset"), preset);
}

// ── 20-band parametric EQ ────────────────────────────────────────────
int Settings::eqActiveBands() const
{
    return m_settings.value(QStringLiteral("dsp/eqActiveBands"), 10).toInt();
}

void Settings::setEqActiveBands(int count)
{
    m_settings.setValue(QStringLiteral("dsp/eqActiveBands"), count);
}

float Settings::eqBandFreq(int band) const
{
    return m_settings.value(
        QStringLiteral("dsp/eqBand%1Freq").arg(band), 0.0f).toFloat();
}

void Settings::setEqBandFreq(int band, float hz)
{
    m_settings.setValue(
        QStringLiteral("dsp/eqBand%1Freq").arg(band), static_cast<double>(hz));
}

float Settings::eqBandGain(int band) const
{
    return m_settings.value(
        QStringLiteral("dsp/eqBand%1Gain").arg(band), 0.0f).toFloat();
}

void Settings::setEqBandGain(int band, float db)
{
    m_settings.setValue(
        QStringLiteral("dsp/eqBand%1Gain").arg(band), static_cast<double>(db));
}

float Settings::eqBandQ(int band) const
{
    return m_settings.value(
        QStringLiteral("dsp/eqBand%1Q").arg(band), 1.0f).toFloat();
}

void Settings::setEqBandQ(int band, float q)
{
    m_settings.setValue(
        QStringLiteral("dsp/eqBand%1Q").arg(band), static_cast<double>(q));
}

int Settings::eqBandType(int band) const
{
    return m_settings.value(
        QStringLiteral("dsp/eqBand%1Type").arg(band), 0).toInt();
}

void Settings::setEqBandType(int band, int type)
{
    m_settings.setValue(
        QStringLiteral("dsp/eqBand%1Type").arg(band), type);
}

bool Settings::eqBandEnabled(int band) const
{
    return m_settings.value(
        QStringLiteral("dsp/eqBand%1Enabled").arg(band), true).toBool();
}

void Settings::setEqBandEnabled(int band, bool enabled)
{
    m_settings.setValue(
        QStringLiteral("dsp/eqBand%1Enabled").arg(band), enabled);
}

// ── VST ─────────────────────────────────────────────────────────────
QStringList Settings::activeVstPlugins() const
{
    return m_settings.value(QStringLiteral("vst/activePlugins")).toStringList();
}

void Settings::setActiveVstPlugins(const QStringList& paths)
{
    m_settings.setValue(QStringLiteral("vst/activePlugins"), paths);
}

// ── Volume Leveling ─────────────────────────────────────────────────
bool Settings::volumeLeveling() const
{
    return m_settings.value(QStringLiteral("audio/volumeLeveling"), false).toBool();
}

void Settings::setVolumeLeveling(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/volumeLeveling"), enabled);
    emit volumeLevelingChanged(enabled);
}

int Settings::levelingMode() const
{
    return m_settings.value(QStringLiteral("audio/levelingMode"), 0).toInt();
}

void Settings::setLevelingMode(int mode)
{
    m_settings.setValue(QStringLiteral("audio/levelingMode"), mode);
    emit levelingModeChanged(mode);
}

double Settings::targetLoudness() const
{
    return m_settings.value(QStringLiteral("audio/targetLoudness"), -14.0).toDouble();
}

void Settings::setTargetLoudness(double lufs)
{
    m_settings.setValue(QStringLiteral("audio/targetLoudness"), lufs);
    emit targetLoudnessChanged(lufs);
}

// ── Headroom Management ─────────────────────────────────────────────
Settings::HeadroomMode Settings::headroomMode() const
{
    return static_cast<HeadroomMode>(m_settings.value(QStringLiteral("audio/headroomMode"), 0).toInt());
}

void Settings::setHeadroomMode(HeadroomMode mode)
{
    m_settings.setValue(QStringLiteral("audio/headroomMode"), static_cast<int>(mode));
    emit headroomChanged();
}

double Settings::manualHeadroom() const
{
    return m_settings.value(QStringLiteral("audio/manualHeadroom"), -3.0).toDouble();
}

void Settings::setManualHeadroom(double dB)
{
    m_settings.setValue(QStringLiteral("audio/manualHeadroom"), dB);
    emit headroomChanged();
}

// ── Crossfeed ────────────────────────────────────────────────────────
bool Settings::crossfeedEnabled() const
{
    return m_settings.value(QStringLiteral("audio/crossfeedEnabled"), false).toBool();
}

void Settings::setCrossfeedEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/crossfeedEnabled"), enabled);
    // Mutually exclusive with HRTF — both simulate speaker listening
    if (enabled && hrtfEnabled()) {
        m_settings.setValue(QStringLiteral("audio/hrtfEnabled"), false);
        emit hrtfChanged();
        qDebug() << "[Settings] Crossfeed enabled → HRTF auto-disabled";
    }
    emit crossfeedChanged();
}

int Settings::crossfeedLevel() const
{
    return m_settings.value(QStringLiteral("audio/crossfeedLevel"), 1).toInt();
}

void Settings::setCrossfeedLevel(int level)
{
    m_settings.setValue(QStringLiteral("audio/crossfeedLevel"), level);
    emit crossfeedChanged();
}

// ── Convolution ─────────────────────────────────────────────────────
bool Settings::convolutionEnabled() const
{
    return m_settings.value(QStringLiteral("audio/convolutionEnabled"), false).toBool();
}

void Settings::setConvolutionEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/convolutionEnabled"), enabled);
    emit convolutionChanged();
}

QString Settings::convolutionIRPath() const
{
    return m_settings.value(QStringLiteral("audio/convolutionIRPath")).toString();
}

void Settings::setConvolutionIRPath(const QString& path)
{
    m_settings.setValue(QStringLiteral("audio/convolutionIRPath"), path);
    emit convolutionChanged();
}

// ── HRTF (Binaural Spatial Audio) ───────────────────────────────────
bool Settings::hrtfEnabled() const
{
    return m_settings.value(QStringLiteral("audio/hrtfEnabled"), false).toBool();
}

void Settings::setHrtfEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/hrtfEnabled"), enabled);
    // Mutually exclusive with Crossfeed — both simulate speaker listening
    if (enabled && crossfeedEnabled()) {
        m_settings.setValue(QStringLiteral("audio/crossfeedEnabled"), false);
        emit crossfeedChanged();
        qDebug() << "[Settings] HRTF enabled → Crossfeed auto-disabled";
    }
    emit hrtfChanged();
}

QString Settings::hrtfSofaPath() const
{
    return m_settings.value(QStringLiteral("audio/hrtfSofaPath")).toString();
}

void Settings::setHrtfSofaPath(const QString& path)
{
    m_settings.setValue(QStringLiteral("audio/hrtfSofaPath"), path);
    emit hrtfChanged();
}

float Settings::hrtfSpeakerAngle() const
{
    return m_settings.value(QStringLiteral("audio/hrtfSpeakerAngle"), 30.0f).toFloat();
}

void Settings::setHrtfSpeakerAngle(float degrees)
{
    m_settings.setValue(QStringLiteral("audio/hrtfSpeakerAngle"), static_cast<double>(degrees));
    emit hrtfChanged();
}

// ── Autoplay / Radio ────────────────────────────────────────────────
bool Settings::autoplayEnabled() const
{
    return m_settings.value(QStringLiteral("audio/autoplay_enabled"), false).toBool();
}

void Settings::setAutoplayEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("audio/autoplay_enabled"), enabled);
    emit autoplayEnabledChanged(enabled);
}

// ── Gapless Playback ────────────────────────────────────────────────
bool Settings::gaplessPlayback() const
{
    return m_settings.value(QStringLiteral("playback/gapless"), true).toBool();
}

void Settings::setGaplessPlayback(bool enabled)
{
    m_settings.setValue(QStringLiteral("playback/gapless"), enabled);
}

// ── Crossfade Duration ──────────────────────────────────────────────
int Settings::crossfadeDurationMs() const
{
    return m_settings.value(QStringLiteral("playback/crossfadeDurationMs"), 0).toInt();
}

void Settings::setCrossfadeDurationMs(int ms)
{
    m_settings.setValue(QStringLiteral("playback/crossfadeDurationMs"), ms);
}

// ── Shuffle / Repeat ────────────────────────────────────────────────
bool Settings::shuffleEnabled() const
{
    return m_settings.value(QStringLiteral("playback/shuffle"), false).toBool();
}

void Settings::setShuffleEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("playback/shuffle"), enabled);
}

int Settings::repeatMode() const
{
    return m_settings.value(QStringLiteral("playback/repeat"), 0).toInt();
}

void Settings::setRepeatMode(int mode)
{
    m_settings.setValue(QStringLiteral("playback/repeat"), mode);
}

// ── Window Geometry ────────────────────────────────────────────────
QByteArray Settings::windowGeometry() const
{
    return m_settings.value(QStringLiteral("window/geometry")).toByteArray();
}

void Settings::setWindowGeometry(const QByteArray& geometry)
{
    m_settings.setValue(QStringLiteral("window/geometry"), geometry);
}

// ── Auto-Organize ───────────────────────────────────────────────────
bool Settings::autoOrganizeOnImport() const
{
    return m_settings.value(QStringLiteral("library/autoOrganize"), false).toBool();
}

void Settings::setAutoOrganizeOnImport(bool enabled)
{
    m_settings.setValue(QStringLiteral("library/autoOrganize"), enabled);
}

QString Settings::organizePattern() const
{
    return m_settings.value(QStringLiteral("library/organizePattern"),
                            QStringLiteral("%artist%/%album%/%track% - %title%")).toString();
}

void Settings::setOrganizePattern(const QString& pattern)
{
    m_settings.setValue(QStringLiteral("library/organizePattern"), pattern);
}

// ── Theme ───────────────────────────────────────────────────────────
int Settings::themeIndex() const
{
    return m_settings.value(QStringLiteral("appearance/theme"), 1).toInt(); // Dark by default
}

void Settings::setThemeIndex(int index)
{
    m_settings.setValue(QStringLiteral("appearance/theme"), index);
}

// ── Last Track ──────────────────────────────────────────────────────
QString Settings::lastTrackId() const
{
    return m_settings.value(QStringLiteral("playback/lastTrackId")).toString();
}

void Settings::setLastTrackId(const QString& id)
{
    m_settings.setValue(QStringLiteral("playback/lastTrackId"), id);
}

int Settings::lastTrackPosition() const
{
    return m_settings.value(QStringLiteral("playback/lastPosition"), 0).toInt();
}

void Settings::setLastTrackPosition(int secs)
{
    m_settings.setValue(QStringLiteral("playback/lastPosition"), secs);
}

// ── Window ──────────────────────────────────────────────────────────
QSize Settings::windowSize() const
{
    return m_settings.value(QStringLiteral("window/size"), QSize(1400, 900)).toSize();
}

void Settings::setWindowSize(const QSize& size)
{
    m_settings.setValue(QStringLiteral("window/size"), size);
}

QPoint Settings::windowPosition() const
{
    return m_settings.value(QStringLiteral("window/position"), QPoint(-1, -1)).toPoint();
}

void Settings::setWindowPosition(const QPoint& pos)
{
    m_settings.setValue(QStringLiteral("window/position"), pos);
}
