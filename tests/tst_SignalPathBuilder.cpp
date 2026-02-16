#include <QtTest/QtTest>
#include "core/audio/SignalPathBuilder.h"

// Helper: default AudioState for "PCM FLAC 44.1/16 stereo" playing
static AudioState makePCMState()
{
    AudioState s;
    s.isStopped = false;
    s.hasFilePath = true;
    s.sampleRate = 44100.0;
    s.channels = 2;
    s.decoderOpen = true;
    s.codecName = QStringLiteral("FLAC");
    s.decoderFormat.sampleRate = 44100.0;
    s.decoderFormat.channels = 2;
    s.decoderFormat.bitsPerSample = 16;
    s.outputDeviceName = QStringLiteral("TestDAC");
    s.outputNominalRate = 44100.0;
    return s;
}

class tst_SignalPathBuilder : public QObject {
    Q_OBJECT

private slots:

    // ── Stopped/empty → empty path ──────────────────────────────
    void stopped_noFile_emptyPath()
    {
        AudioState s;
        s.isStopped = true;
        s.hasFilePath = false;
        auto info = SignalPathBuilder::build(s);
        QVERIFY(info.nodes.isEmpty());
    }

    // ── Minimal PCM path: Source + Decoder + Output ─────────────
    void pcmFlac_minimalPath()
    {
        auto s = makePCMState();
        auto info = SignalPathBuilder::build(s);

        QCOMPARE(info.nodes.size(), 3);  // Source, Decoder, Output
        QCOMPARE(info.nodes[0].label, QStringLiteral("Source"));
        QCOMPARE(info.nodes[1].label, QStringLiteral("Decoder"));
        QCOMPARE(info.nodes[2].label, QStringLiteral("Output"));
    }

    // ── Source quality: FLAC 44.1/16 → Lossless ─────────────────
    void source_flac44_lossless()
    {
        auto s = makePCMState();
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::Lossless);
    }

    // ── Source quality: FLAC 96/24 → HighRes ────────────────────
    void source_flac96_highRes()
    {
        auto s = makePCMState();
        s.sampleRate = 96000.0;
        s.decoderFormat.sampleRate = 96000.0;
        s.decoderFormat.bitsPerSample = 24;
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::HighRes);
    }

    // ── Source quality: FLAC 44.1/24 → HighRes (bits > 16) ─────
    void source_flac44_24bit_highRes()
    {
        auto s = makePCMState();
        s.decoderFormat.bitsPerSample = 24;
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::HighRes);
    }

    // ── Source quality: MP3 → Lossy ─────────────────────────────
    void source_mp3_lossy()
    {
        auto s = makePCMState();
        s.codecName = QStringLiteral("MP3");
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::Lossy);
    }

    // ── Source quality: AAC → Lossy ─────────────────────────────
    void source_aac_lossy()
    {
        auto s = makePCMState();
        s.codecName = QStringLiteral("AAC");
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::Lossy);
    }

    // ── Source quality: ALAC → Lossless ─────────────────────────
    void source_alac_lossless()
    {
        auto s = makePCMState();
        s.codecName = QStringLiteral("ALAC");
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::Lossless);
    }

    // ── Source quality: WAV → Lossless ──────────────────────────
    void source_wav_lossless()
    {
        auto s = makePCMState();
        s.codecName = QStringLiteral("WAV");
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::Lossless);
    }

    // ── Source quality: PCM_S24LE → HighRes (24-bit) ────────────
    void source_pcmS24_highRes()
    {
        auto s = makePCMState();
        s.codecName = QStringLiteral("PCM_S24LE");
        s.decoderFormat.bitsPerSample = 24;
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::HighRes);
        // PCM_ codecs show as "PCM/WAV"
        QVERIFY(info.nodes[0].detail.contains(QStringLiteral("PCM/WAV")));
    }

    // ── Decoder: FLAC → Lossless Decode ─────────────────────────
    void decoder_flac_lossless()
    {
        auto s = makePCMState();
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[1].detail, QStringLiteral("Lossless Decode"));
        QCOMPARE(info.nodes[1].quality, SignalPathNode::Lossless);
    }

    // ── Decoder: MP3 → Lossy Decode ─────────────────────────────
    void decoder_mp3_lossy()
    {
        auto s = makePCMState();
        s.codecName = QStringLiteral("MP3");
        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[1].detail, QStringLiteral("Lossy Decode"));
        QCOMPARE(info.nodes[1].quality, SignalPathNode::Lossy);
    }

    // ── DSD source → HighRes ────────────────────────────────────
    void source_dsd64_highRes()
    {
        AudioState s;
        s.isStopped = false;
        s.hasFilePath = true;
        s.usingDSDDecoder = true;
        s.isDSD64 = true;
        s.dsdSampleRate = 2822400.0;
        s.channels = 2;
        s.sampleRate = 176400.0;
        s.outputDeviceName = QStringLiteral("TestDAC");
        s.outputNominalRate = 176400.0;

        auto info = SignalPathBuilder::build(s);
        QVERIFY(info.nodes.size() >= 2);
        QCOMPARE(info.nodes[0].quality, SignalPathNode::HighRes);
        QVERIFY(info.nodes[0].detail.contains(QStringLiteral("DSD64")));
    }

    // ── DSD DoP decoder → HighRes passthrough ───────────────────
    void decoder_dsd_dop_highRes()
    {
        AudioState s;
        s.isStopped = false;
        s.hasFilePath = true;
        s.usingDSDDecoder = true;
        s.isDSD64 = true;
        s.isDoPMode = true;
        s.dsdSampleRate = 2822400.0;
        s.channels = 2;
        s.sampleRate = 176400.0;
        s.outputDeviceName = QStringLiteral("TestDAC");
        s.outputNominalRate = 176400.0;

        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[1].detail, QStringLiteral("DoP Passthrough"));
        QCOMPARE(info.nodes[1].quality, SignalPathNode::HighRes);
    }

    // ── DSD non-DoP decoder → Lossless (DSD to PCM) ────────────
    void decoder_dsd_noDop_lossless()
    {
        AudioState s;
        s.isStopped = false;
        s.hasFilePath = true;
        s.usingDSDDecoder = true;
        s.isDSD64 = true;
        s.isDoPMode = false;
        s.dsdSampleRate = 2822400.0;
        s.channels = 2;
        s.sampleRate = 176400.0;
        s.outputDeviceName = QStringLiteral("TestDAC");
        s.outputNominalRate = 176400.0;

        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes[1].detail, QStringLiteral("DSD to PCM"));
        QCOMPARE(info.nodes[1].quality, SignalPathNode::Lossless);
    }

    // ── DSD via FFmpeg (DSD_ codec) → Enhanced decoder ──────────
    void decoder_dsdCodecViaFFmpeg_enhanced()
    {
        auto s = makePCMState();
        s.codecName = QStringLiteral("DSD_LSBF");
        s.decoderFormat.sampleRate = 176400.0;
        s.decoderFormat.channels = 2;

        auto info = SignalPathBuilder::build(s);
        // Source should be HighRes (DSD detected)
        QCOMPARE(info.nodes[0].quality, SignalPathNode::HighRes);
        // Decoder should be Enhanced (DSD to PCM Conversion)
        QCOMPARE(info.nodes[1].quality, SignalPathNode::Enhanced);
        QVERIFY(info.nodes[1].detail.contains(QStringLiteral("DSD to PCM")));
    }

    // ── Upsampler adds node ─────────────────────────────────────
    void upsampler_addsNode()
    {
        auto s = makePCMState();
        s.upsamplerActive = true;
        s.upsamplerDescription = QStringLiteral("44.1 → 96 kHz");
        s.upsamplerOutputRate = 96000.0;

        auto info = SignalPathBuilder::build(s);
        // Should have 4 nodes: Source, Decoder, Upsampling, Output
        QCOMPARE(info.nodes.size(), 4);
        QCOMPARE(info.nodes[2].label, QStringLiteral("Upsampling"));
        QCOMPARE(info.nodes[2].quality, SignalPathNode::Enhanced);
    }

    // ── Upsampler skipped in bit-perfect mode ───────────────────
    void upsampler_skippedInBitPerfect()
    {
        auto s = makePCMState();
        s.upsamplerActive = true;
        s.bitPerfect = true;

        auto info = SignalPathBuilder::build(s);
        for (const auto& node : info.nodes)
            QVERIFY(node.label != QStringLiteral("Upsampling"));
    }

    // ── Upsampler skipped for DSD ───────────────────────────────
    void upsampler_skippedForDSD()
    {
        AudioState s;
        s.isStopped = false;
        s.hasFilePath = true;
        s.usingDSDDecoder = true;
        s.isDSD64 = true;
        s.dsdSampleRate = 2822400.0;
        s.channels = 2;
        s.sampleRate = 176400.0;
        s.upsamplerActive = true;
        s.outputDeviceName = QStringLiteral("TestDAC");

        auto info = SignalPathBuilder::build(s);
        for (const auto& node : info.nodes)
            QVERIFY(node.label != QStringLiteral("Upsampling"));
    }

    // ── Headroom adds node when active ──────────────────────────
    void headroom_addsNode()
    {
        auto s = makePCMState();
        s.headroomMode = AudioState::HRAuto;
        s.headroomGain = 0.5f;  // -6 dB

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("Headroom")) {
                found = true;
                QVERIFY(n.sublabel.contains(QStringLiteral("Auto")));
            }
        }
        QVERIFY(found);
    }

    // ── Headroom skipped when gain = 1.0 ────────────────────────
    void headroom_skippedWhenUnity()
    {
        auto s = makePCMState();
        s.headroomMode = AudioState::HRAuto;
        s.headroomGain = 1.0f;

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes)
            QVERIFY(n.label != QStringLiteral("Headroom"));
    }

    // ── Crossfeed adds node for stereo ──────────────────────────
    void crossfeed_addsNode_stereo()
    {
        auto s = makePCMState();
        s.crossfeedEnabled = true;
        s.crossfeedLevel = 1;  // Medium

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("Crossfeed")) {
                found = true;
                QVERIFY(n.sublabel.contains(QStringLiteral("Medium")));
            }
        }
        QVERIFY(found);
    }

    // ── Crossfeed skipped for mono ──────────────────────────────
    void crossfeed_skippedForMono()
    {
        auto s = makePCMState();
        s.channels = 1;
        s.crossfeedEnabled = true;

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes)
            QVERIFY(n.label != QStringLiteral("Crossfeed"));
    }

    // ── Convolution adds node ───────────────────────────────────
    void convolution_addsNode()
    {
        auto s = makePCMState();
        s.convolutionEnabled = true;
        s.convolutionHasIR = true;
        s.convolutionIRPath = QStringLiteral("/path/to/room.wav");

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("Convolution")) {
                found = true;
                QVERIFY(n.sublabel.contains(QStringLiteral("room.wav")));
            }
        }
        QVERIFY(found);
    }

    // ── Convolution skipped when no IR ──────────────────────────
    void convolution_skippedWithoutIR()
    {
        auto s = makePCMState();
        s.convolutionEnabled = true;
        s.convolutionHasIR = false;

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes)
            QVERIFY(n.label != QStringLiteral("Convolution"));
    }

    // ── HRTF adds node for stereo ───────────────────────────────
    void hrtf_addsNode_stereo()
    {
        auto s = makePCMState();
        s.hrtfEnabled = true;
        s.hrtfLoaded = true;
        s.hrtfSofaPath = QStringLiteral("/path/to/hrtf.sofa");
        s.hrtfSpeakerAngle = 30.0;

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("HRTF")) {
                found = true;
                QVERIFY(n.sublabel.contains(QStringLiteral("hrtf.sofa")));
                QVERIFY(n.sublabel.contains(QStringLiteral("30")));
            }
        }
        QVERIFY(found);
    }

    // ── HRTF skipped when not loaded ────────────────────────────
    void hrtf_skippedWhenNotLoaded()
    {
        auto s = makePCMState();
        s.hrtfEnabled = true;
        s.hrtfLoaded = false;

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes)
            QVERIFY(n.label != QStringLiteral("HRTF"));
    }

    // ── DSP: EQ enabled adds node ───────────────────────────────
    void dsp_eq_addsNode()
    {
        auto s = makePCMState();
        s.dspEnabled = true;
        s.eqEnabled = true;

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("DSP") &&
                n.detail == QStringLiteral("Parametric Equalizer")) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    // ── DSP: EQ disabled → no EQ node ───────────────────────────
    void dsp_eqDisabled_noNode()
    {
        auto s = makePCMState();
        s.dspEnabled = true;
        s.eqEnabled = false;

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("DSP"))
                QVERIFY(n.detail != QStringLiteral("Parametric Equalizer"));
        }
    }

    // ── DSP: Gain adds node ─────────────────────────────────────
    void dsp_gain_addsNode()
    {
        auto s = makePCMState();
        s.dspEnabled = true;
        s.gainEnabled = true;
        s.gainDb = 3.0f;

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("DSP") &&
                n.detail.contains(QStringLiteral("Preamp/Gain"))) {
                found = true;
                QVERIFY(n.detail.contains(QStringLiteral("+3.0")));
            }
        }
        QVERIFY(found);
    }

    // ── DSP: Gain near-zero → no node ───────────────────────────
    void dsp_gainNearZero_noNode()
    {
        auto s = makePCMState();
        s.dspEnabled = true;
        s.gainEnabled = true;
        s.gainDb = 0.005f;  // below threshold

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("DSP"))
                QVERIFY(!n.detail.contains(QStringLiteral("Preamp")));
        }
    }

    // ── DSP: Plugin adds node ───────────────────────────────────
    void dsp_plugin_addsNode()
    {
        auto s = makePCMState();
        s.dspEnabled = true;
        s.plugins = {{QStringLiteral("FabFilter Pro-Q 3"), true}};

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("DSP") &&
                n.detail == QStringLiteral("FabFilter Pro-Q 3")) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    // ── DSP: Disabled plugin → no node ──────────────────────────
    void dsp_disabledPlugin_noNode()
    {
        auto s = makePCMState();
        s.dspEnabled = true;
        s.plugins = {{QStringLiteral("MyPlugin"), false}};

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes)
            QVERIFY(n.detail != QStringLiteral("MyPlugin"));
    }

    // ── DSP: bitPerfect skips all DSP ───────────────────────────
    void dsp_bitPerfect_skipsAll()
    {
        auto s = makePCMState();
        s.bitPerfect = true;
        s.dspEnabled = true;
        s.eqEnabled = true;
        s.gainEnabled = true;
        s.gainDb = 6.0f;

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes)
            QVERIFY(n.label != QStringLiteral("DSP"));
    }

    // ── Volume leveling adds node ───────────────────────────────
    void volumeLeveling_addsNode()
    {
        auto s = makePCMState();
        s.volumeLevelingEnabled = true;
        s.levelingGain = 0.7f;
        s.hasReplayGain = true;

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("Volume Leveling")) {
                found = true;
                QCOMPARE(n.detail, QStringLiteral("ReplayGain"));
            }
        }
        QVERIFY(found);
    }

    // ── Volume leveling: R128 source ────────────────────────────
    void volumeLeveling_r128()
    {
        auto s = makePCMState();
        s.volumeLevelingEnabled = true;
        s.levelingGain = 0.8f;
        s.hasR128 = true;

        auto info = SignalPathBuilder::build(s);
        bool found = false;
        for (const auto& n : info.nodes) {
            if (n.label == QStringLiteral("Volume Leveling")) {
                found = true;
                QCOMPARE(n.detail, QStringLiteral("R128"));
            }
        }
        QVERIFY(found);
    }

    // ── Volume leveling: unity gain → no node ───────────────────
    void volumeLeveling_unityGain_noNode()
    {
        auto s = makePCMState();
        s.volumeLevelingEnabled = true;
        s.levelingGain = 1.0f;

        auto info = SignalPathBuilder::build(s);
        for (const auto& n : info.nodes)
            QVERIFY(n.label != QStringLiteral("Volume Leveling"));
    }

    // ── Output node: bit-perfect + exclusive → BitPerfect ───────
    void output_bitPerfect_exclusive()
    {
        auto s = makePCMState();
        s.bitPerfect = true;
        s.outputExclusive = true;
        s.outputNominalRate = 44100.0;

        auto info = SignalPathBuilder::build(s);
        auto& out = info.nodes.last();
        QCOMPARE(out.label, QStringLiteral("Output"));
        QCOMPARE(out.quality, SignalPathNode::BitPerfect);
        QVERIFY(out.sublabel.contains(QStringLiteral("Bit-Perfect")));
        QVERIFY(out.sublabel.contains(QStringLiteral("Exclusive")));
    }

    // ── Output node: bit-perfect without exclusive → decoder quality ─
    void output_bitPerfect_noExclusive()
    {
        auto s = makePCMState();
        s.bitPerfect = true;
        s.outputExclusive = false;
        s.outputNominalRate = 44100.0;

        auto info = SignalPathBuilder::build(s);
        auto& out = info.nodes.last();
        QVERIFY(out.sublabel.contains(QStringLiteral("Bit-Perfect")));
        // Quality inherits from decoder (Lossless for FLAC)
        QCOMPARE(out.quality, SignalPathNode::Lossless);
    }

    // ── Output node: DSP active → Enhanced ──────────────────────
    void output_withDSP_enhanced()
    {
        auto s = makePCMState();
        s.dspEnabled = true;
        s.eqEnabled = true;

        auto info = SignalPathBuilder::build(s);
        QCOMPARE(info.nodes.last().quality, SignalPathNode::Enhanced);
    }

    // ── Output: exclusive mode label ────────────────────────────
    void output_exclusiveMode_label()
    {
        auto s = makePCMState();
        s.outputExclusive = true;

        auto info = SignalPathBuilder::build(s);
        QVERIFY(info.nodes.last().sublabel.contains(QStringLiteral("Exclusive Mode")));
    }

    // ── Output: rate mismatch on built-in speaker → resampled ───
    void output_rateMismatch_builtIn_resampled()
    {
        auto s = makePCMState();
        s.upsamplerActive = true;
        s.upsamplerOutputRate = 96000.0;
        s.outputBuiltIn = true;
        s.outputNominalRate = 48000.0;

        auto info = SignalPathBuilder::build(s);
        auto& out = info.nodes.last();
        QVERIFY(out.sublabel.contains(QStringLiteral("Resampled")));
        QCOMPARE(out.quality, SignalPathNode::Enhanced);
    }

    // ── Channel descriptions ────────────────────────────────────
    void channelDescriptions()
    {
        // Test via source node detail string
        auto s = makePCMState();

        s.decoderFormat.channels = 1;
        s.channels = 1;
        auto info1 = SignalPathBuilder::build(s);
        QVERIFY(info1.nodes[0].detail.contains(QStringLiteral("Mono")));

        s.decoderFormat.channels = 2;
        s.channels = 2;
        auto info2 = SignalPathBuilder::build(s);
        QVERIFY(info2.nodes[0].detail.contains(QStringLiteral("Stereo")));

        s.decoderFormat.channels = 6;
        s.channels = 6;
        auto info6 = SignalPathBuilder::build(s);
        QVERIFY(info6.nodes[0].detail.contains(QStringLiteral("5.1")));

        s.decoderFormat.channels = 8;
        s.channels = 8;
        auto info8 = SignalPathBuilder::build(s);
        QVERIFY(info8.nodes[0].detail.contains(QStringLiteral("7.1")));
    }

    // ── Full chain: all DSP stages present ──────────────────────
    void fullChain_allStages()
    {
        auto s = makePCMState();
        s.upsamplerActive = true;
        s.upsamplerOutputRate = 96000.0;
        s.upsamplerDescription = QStringLiteral("44.1 → 96 kHz");
        s.headroomMode = AudioState::HRManual;
        s.headroomGain = 0.5f;
        s.crossfeedEnabled = true;
        s.crossfeedLevel = 2;
        s.dspEnabled = true;
        s.eqEnabled = true;
        s.gainEnabled = true;
        s.gainDb = -3.0f;
        s.volumeLevelingEnabled = true;
        s.levelingGain = 0.8f;
        s.hasReplayGain = true;
        s.outputDeviceName = QStringLiteral("TestDAC");
        s.outputNominalRate = 96000.0;

        auto info = SignalPathBuilder::build(s);

        QStringList labels;
        for (const auto& n : info.nodes)
            labels.append(n.label);

        QVERIFY(labels.contains(QStringLiteral("Source")));
        QVERIFY(labels.contains(QStringLiteral("Decoder")));
        QVERIFY(labels.contains(QStringLiteral("Upsampling")));
        QVERIFY(labels.contains(QStringLiteral("Headroom")));
        QVERIFY(labels.contains(QStringLiteral("Crossfeed")));
        QVERIFY(labels.contains(QStringLiteral("DSP")));
        QVERIFY(labels.contains(QStringLiteral("Volume Leveling")));
        QVERIFY(labels.contains(QStringLiteral("Output")));
    }
};

QTEST_MAIN(tst_SignalPathBuilder)
#include "tst_SignalPathBuilder.moc"
