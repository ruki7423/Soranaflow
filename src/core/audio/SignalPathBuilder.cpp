#include "SignalPathBuilder.h"
#include <cmath>

QString SignalPathBuilder::channelDescription(int ch) {
    switch (ch) {
    case 1:  return QStringLiteral("Mono");
    case 2:  return QStringLiteral("Stereo");
    case 3:  return QStringLiteral("3.0");
    case 4:  return QStringLiteral("4.0");
    case 6:  return QStringLiteral("5.1");
    case 8:  return QStringLiteral("7.1");
    default: return QStringLiteral("%1ch").arg(ch);
    }
}

SignalPathInfo SignalPathBuilder::build(const AudioState& s)
{
    SignalPathInfo info;

    if (s.isStopped && !s.hasFilePath) {
        return info;
    }

    double sr = s.sampleRate;
    int ch = s.channels;

    // ── 1. Source node ──────────────────────────────────────────────
    SignalPathNode sourceNode;
    sourceNode.label = QStringLiteral("Source");

    if (s.usingDSDDecoder) {
        // DSD source
        QString dsdRate;
        if (s.isDSD64)       dsdRate = QStringLiteral("DSD64");
        else if (s.isDSD128) dsdRate = QStringLiteral("DSD128");
        else if (s.isDSD256) dsdRate = QStringLiteral("DSD256");
        else if (s.isDSD512) dsdRate = QStringLiteral("DSD512");
        else dsdRate = QStringLiteral("DSD");

        sourceNode.detail = QStringLiteral("%1 \u2022 %2").arg(dsdRate, channelDescription(ch));
        sourceNode.sublabel = QStringLiteral("%.1f MHz").arg(s.dsdSampleRate / 1000000.0);
        sourceNode.quality = SignalPathNode::HighRes;
    } else if (s.decoderOpen) {
        QString codec = s.codecName.toUpper();
        const AudioStreamFormat& fmt = s.decoderFormat;

        // Detect DSD codecs decoded via FFmpeg (PCM conversion mode)
        bool isDSDCodec = codec.startsWith(QStringLiteral("DSD_"));

        if (isDSDCodec) {
            int dsdMultiplier = 64;
            double dsdNativeRate = 2822400.0;
            if (fmt.sampleRate >= 352800) {
                dsdMultiplier = 128; dsdNativeRate = 5644800.0;
            }

            QString dsdLabel = QStringLiteral("DSD%1").arg(dsdMultiplier);
            sourceNode.detail = QStringLiteral("%1 \u2022 %2").arg(dsdLabel, channelDescription(fmt.channels));
            sourceNode.sublabel = QStringLiteral("%1 MHz").arg(dsdNativeRate / 1000000.0, 0, 'f', 1);
            sourceNode.quality = SignalPathNode::HighRes;
        } else {
            // Regular PCM codec
            bool lossless = (codec == QStringLiteral("FLAC") || codec == QStringLiteral("ALAC")
                          || codec == QStringLiteral("WAV")  || codec == QStringLiteral("PCM_S16LE")
                          || codec == QStringLiteral("PCM_S24LE") || codec == QStringLiteral("PCM_S32LE")
                          || codec.startsWith(QStringLiteral("PCM_")));

            if (lossless && (fmt.sampleRate > 44100 || fmt.bitsPerSample > 16)) {
                sourceNode.quality = SignalPathNode::HighRes;
            } else if (lossless) {
                sourceNode.quality = SignalPathNode::Lossless;
            } else {
                sourceNode.quality = SignalPathNode::Lossy;
            }

            // Format codec name for display
            QString displayCodec = codec;
            if (codec.startsWith(QStringLiteral("PCM_"))) displayCodec = QStringLiteral("PCM/WAV");

            sourceNode.detail = QStringLiteral("%1 \u2022 %2-bit / %3 kHz \u2022 %4")
                .arg(displayCodec)
                .arg(fmt.bitsPerSample)
                .arg(fmt.sampleRate / 1000.0, 0, 'g', 4)
                .arg(channelDescription(fmt.channels));
        }
    }
    info.nodes.append(sourceNode);

    // ── 2. Decoder node ─────────────────────────────────────────────
    SignalPathNode decoderNode;
    decoderNode.label = QStringLiteral("Decoder");

    if (s.usingDSDDecoder && s.isDoPMode) {
        decoderNode.detail = QStringLiteral("DoP Passthrough");
        decoderNode.sublabel = QStringLiteral("DSD over PCM at %1 kHz").arg(sr / 1000.0, 0, 'g', 4);
        decoderNode.quality = SignalPathNode::HighRes;
    } else if (s.usingDSDDecoder) {
        decoderNode.detail = QStringLiteral("DSD to PCM");
        decoderNode.quality = SignalPathNode::Lossless;
    } else {
        bool lossless = false;
        bool isDSDCodec = false;
        if (s.decoderOpen) {
            QString codec = s.codecName.toUpper();
            isDSDCodec = codec.startsWith(QStringLiteral("DSD_"));
            lossless = isDSDCodec
                    || codec == QStringLiteral("FLAC") || codec == QStringLiteral("ALAC")
                    || codec == QStringLiteral("WAV") || codec.startsWith(QStringLiteral("PCM_"));
        }
        if (isDSDCodec) {
            const AudioStreamFormat& fmt = s.decoderFormat;
            if (s.dsdPlaybackMode == QStringLiteral("dop")) {
                decoderNode.detail = QStringLiteral("DSD to PCM (DoP Fallback)");
                decoderNode.sublabel = QStringLiteral("Device rate insufficient \u00b7 Output at %1 kHz")
                    .arg(fmt.sampleRate / 1000.0, 0, 'f', 1);
            } else {
                decoderNode.detail = QStringLiteral("DSD to PCM Conversion");
                decoderNode.sublabel = QStringLiteral("Output at %1 kHz")
                    .arg(fmt.sampleRate / 1000.0, 0, 'f', 1);
            }
            decoderNode.quality = SignalPathNode::Enhanced;
        } else {
            decoderNode.detail = lossless ? QStringLiteral("Lossless Decode")
                                           : QStringLiteral("Lossy Decode");
            decoderNode.quality = lossless ? SignalPathNode::Lossless : SignalPathNode::Lossy;
        }
    }
    info.nodes.append(decoderNode);

    // ── 3. Upsampler node ───────────────────────────────────────────
    if (s.upsamplerActive && !s.bitPerfect && !s.usingDSDDecoder) {
        SignalPathNode upsampleNode;
        upsampleNode.label = QStringLiteral("Upsampling");
        upsampleNode.detail = QStringLiteral("SoX Resampler (libsoxr)");
        upsampleNode.sublabel = s.upsamplerDescription;
        upsampleNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(upsampleNode);
    }

    // ── 3b. Headroom node ────────────────────────────────────────────
    if (s.headroomMode != AudioState::HROff && s.headroomGain != 1.0f) {
        SignalPathNode hrNode;
        hrNode.label = QStringLiteral("Headroom");
        double db = 20.0 * std::log10(static_cast<double>(s.headroomGain));
        QString modeStr = (s.headroomMode == AudioState::HRAuto)
            ? QStringLiteral("Auto") : QStringLiteral("Manual");
        hrNode.sublabel = modeStr + QStringLiteral(" \u00b7 ")
            + QString::number(db, 'f', 1) + QStringLiteral(" dB");
        hrNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(hrNode);
    }

    // ── 3c. Crossfeed node ────────────────────────────────────────────
    if (s.crossfeedEnabled && ch == 2) {
        SignalPathNode cfNode;
        cfNode.label = QStringLiteral("Crossfeed");
        const char* levels[] = {"Light", "Medium", "Strong"};
        int lvl = s.crossfeedLevel;
        if (lvl < 0) lvl = 0;
        if (lvl > 2) lvl = 2;
        cfNode.sublabel = QStringLiteral("Headphone \u00b7 %1").arg(QLatin1String(levels[lvl]));
        cfNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(cfNode);
    }

    // ── 3d. Convolution node ────────────────────────────────────────────
    if (s.convolutionEnabled && s.convolutionHasIR) {
        SignalPathNode convNode;
        convNode.label = QStringLiteral("Convolution");
        QString irName = s.convolutionIRPath;
        int lastSlash = irName.lastIndexOf(QLatin1Char('/'));
        if (lastSlash >= 0) irName = irName.mid(lastSlash + 1);
        convNode.sublabel = QStringLiteral("Room Correction \u00b7 ") + irName;
        convNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(convNode);
    }

    // ── 3e. HRTF node ────────────────────────────────────────────────
    if (s.hrtfEnabled && s.hrtfLoaded && ch == 2) {
        SignalPathNode hrtfNode;
        hrtfNode.label = QStringLiteral("HRTF");
        QString sofaName = s.hrtfSofaPath;
        int lastSlash = sofaName.lastIndexOf(QLatin1Char('/'));
        if (lastSlash >= 0) sofaName = sofaName.mid(lastSlash + 1);
        hrtfNode.sublabel = QStringLiteral("Binaural \u00b7 %1\u00b0 \u00b7 %2")
            .arg(static_cast<int>(s.hrtfSpeakerAngle))
            .arg(sofaName);
        hrtfNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(hrtfNode);
    }

    // ── 4. DSP nodes (per active processor) ─────────────────────────
    bool hasDSP = false;
    if (!s.bitPerfect && s.dspEnabled) {
        // Gain
        if (s.gainEnabled && std::abs(s.gainDb) > 0.01f) {
            SignalPathNode gainNode;
            gainNode.label = QStringLiteral("DSP");
            gainNode.detail = QStringLiteral("Preamp/Gain: %1%2 dB")
                .arg(s.gainDb > 0 ? QStringLiteral("+") : QString())
                .arg(s.gainDb, 0, 'f', 1);
            gainNode.quality = SignalPathNode::Enhanced;
            info.nodes.append(gainNode);
            hasDSP = true;
        }

        // EQ
        if (s.eqEnabled) {
            SignalPathNode eqNode;
            eqNode.label = QStringLiteral("DSP");
            eqNode.detail = QStringLiteral("Parametric Equalizer");
            eqNode.quality = SignalPathNode::Enhanced;
            info.nodes.append(eqNode);
            hasDSP = true;
        }

        // Plugin processors
        for (const auto& plugin : s.plugins) {
            if (plugin.enabled) {
                SignalPathNode pluginNode;
                pluginNode.label = QStringLiteral("DSP");
                pluginNode.detail = plugin.name;
                pluginNode.quality = SignalPathNode::Enhanced;
                info.nodes.append(pluginNode);
                hasDSP = true;
            }
        }
    }

    // ── 4b. Volume Leveling node ───────────────────────────────────
    if (s.volumeLevelingEnabled && s.levelingGain != 1.0f) {
        SignalPathNode levelNode;
        levelNode.label = QStringLiteral("Volume Leveling");
        double db = 20.0 * std::log10(static_cast<double>(s.levelingGain));
        QString src = s.hasReplayGain ? QStringLiteral("ReplayGain")
                    : s.hasR128 ? QStringLiteral("R128")
                    : QStringLiteral("Analyzing...");
        QString gainStr = (db >= 0 ? QStringLiteral("+") : QString())
                        + QString::number(db, 'f', 1) + QStringLiteral(" dB");
        levelNode.detail = src;
        levelNode.sublabel = gainStr;
        levelNode.quality = SignalPathNode::Enhanced;
        info.nodes.append(levelNode);
        hasDSP = true;
    }

    // ── 5. Output node ──────────────────────────────────────────────
    SignalPathNode outputNode;
    outputNode.label = QStringLiteral("Output");

    double displayRate = (s.outputNominalRate > 0) ? s.outputNominalRate : s.outputCurrentRate;

    outputNode.detail = QStringLiteral("%1 \u2022 %2 kHz")
        .arg(s.outputDeviceName)
        .arg(displayRate > 0 ? displayRate / 1000.0 : sr / 1000.0, 0, 'g', 4);

    // The rate actually fed to CoreAudio
    double rateToOutput = sr;
    if (s.upsamplerActive && !s.bitPerfect && !s.usingDSDDecoder) {
        rateToOutput = s.upsamplerOutputRate;
    }

    bool ratesMatch = (s.outputNominalRate > 0)
        ? (std::abs(s.outputNominalRate - rateToOutput) < 1.0)
        : true;

    if (!hasDSP && ratesMatch && s.bitPerfect) {
        outputNode.sublabel = QStringLiteral("Bit-Perfect");
        outputNode.quality = s.outputExclusive ? SignalPathNode::BitPerfect : decoderNode.quality;
    } else if (s.outputBuiltIn && !s.bitPerfect && !ratesMatch) {
        outputNode.sublabel = QStringLiteral("Resampled from %1 kHz")
            .arg(rateToOutput / 1000.0, 0, 'f', 1);
        outputNode.quality = SignalPathNode::Enhanced;
    } else if (hasDSP) {
        outputNode.quality = SignalPathNode::Enhanced;
    } else {
        outputNode.quality = decoderNode.quality;
    }

    if (s.outputExclusive) {
        outputNode.sublabel += (outputNode.sublabel.isEmpty() ? QString() : QStringLiteral(" \u2022 "));
        outputNode.sublabel += QStringLiteral("Exclusive Mode");
    }

    info.nodes.append(outputNode);

    return info;
}
