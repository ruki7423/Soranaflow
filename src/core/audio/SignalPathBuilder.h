#pragma once

#include <QString>
#include <QVector>
#include "SignalPathInfo.h"
#include "AudioFormat.h"

// Snapshot of AudioEngine state needed to build the signal path visualization.
// Populated by AudioEngine, consumed by SignalPathBuilder::build() (pure function).
struct AudioState {
    // Engine state
    bool isStopped = true;
    bool hasFilePath = false;

    double sampleRate = 44100.0;
    int channels = 2;

    // DSD decoder state
    bool usingDSDDecoder = false;
    bool isDSD64 = false;
    bool isDSD128 = false;
    bool isDSD256 = false;
    bool isDSD512 = false;
    double dsdSampleRate = 0.0;
    bool isDoPMode = false;

    // PCM decoder state
    bool decoderOpen = false;
    QString codecName;
    AudioStreamFormat decoderFormat{};

    // Upsampler
    bool upsamplerActive = false;
    QString upsamplerDescription;
    double upsamplerOutputRate = 0.0;

    // Modes
    bool bitPerfect = false;

    // Headroom
    float headroomGain = 1.0f;
    enum HeadroomMode { HROff, HRAuto, HRManual };
    HeadroomMode headroomMode = HROff;

    // Crossfeed
    bool crossfeedEnabled = false;
    int crossfeedLevel = 0;

    // Convolution
    bool convolutionEnabled = false;
    bool convolutionHasIR = false;
    QString convolutionIRPath;

    // HRTF
    bool hrtfEnabled = false;
    bool hrtfLoaded = false;
    QString hrtfSofaPath;
    double hrtfSpeakerAngle = 0.0;

    // DSP pipeline
    bool dspEnabled = false;
    bool gainEnabled = false;
    float gainDb = 0.0f;
    bool eqEnabled = false;
    struct PluginInfo {
        QString name;
        bool enabled = false;
    };
    QVector<PluginInfo> plugins;

    // Volume leveling
    float levelingGain = 1.0f;
    bool volumeLevelingEnabled = false;
    bool hasReplayGain = false;
    bool hasR128 = false;

    // Output device
    QString outputDeviceName;
    double outputCurrentRate = 0.0;
    double outputNominalRate = 0.0;
    bool outputBuiltIn = false;
    bool outputExclusive = false;

    // Settings
    QString dsdPlaybackMode;
};

class SignalPathBuilder {
public:
    static SignalPathInfo build(const AudioState& state);

private:
    static QString channelDescription(int ch);
};
