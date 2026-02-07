#pragma once

#include "IDSPProcessor.h"
#include <QString>
#include <QObject>
#include <soxr.h>
#include <vector>
#include <atomic>

enum class UpsamplingMode {
    None,           // No upsampling (passthrough)
    MaxRate,        // Upsample to DAC max supported rate
    Double,         // 2x source rate (44.1->88.2, 48->96)
    Quadruple,      // 4x source rate (44.1->176.4, 48->192)
    PowerOf2,       // Highest power-of-2 multiple within DAC max
    DSD256Rate,     // Always to 352.8/384 kHz
    Fixed,          // User-specified fixed rate
};

enum class UpsamplingQuality {
    Quick,          // SOXR_QQ - lowest latency
    Low,            // SOXR_LQ
    Medium,         // SOXR_MQ
    High,           // SOXR_HQ - Audirvana-level
    VeryHigh,       // SOXR_VHQ - maximum quality
};

enum class UpsamplingFilter {
    LinearPhase,    // Flat passband, symmetric pre/post ringing
    MinimumPhase,   // No pre-ringing, slight post-ringing
    SteepFilter,    // Sharp cutoff, more ringing
    SlowRolloff,    // Gentle cutoff, less ringing
};

class UpsamplerProcessor : public QObject, public IDSPProcessor {
    Q_OBJECT
public:
    explicit UpsamplerProcessor(QObject* parent = nullptr);
    ~UpsamplerProcessor() override;

    // IDSPProcessor interface
    std::string getName() const override { return "Upsampler"; }
    bool isEnabled() const override { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled) override;
    void process(float* buffer, int frames, int channels) override;

    // Upsampler-specific
    void setMode(UpsamplingMode mode);
    UpsamplingMode mode() const { return m_mode; }

    void setQuality(UpsamplingQuality quality);
    UpsamplingQuality quality() const { return m_quality; }

    void setFilter(UpsamplingFilter filter);
    UpsamplingFilter filter() const { return m_filter; }

    void setFixedRate(int rate);
    int fixedRate() const { return m_fixedRate; }

    void setMaxDacRate(int rate);
    int maxDacRate() const { return m_maxDacRate; }

    void setDeviceIsBuiltIn(bool builtIn);
    bool deviceIsBuiltIn() const { return m_deviceIsBuiltIn; }

    // Called when source format changes
    void setInputFormat(int sampleRate, int channels);

    // Get the output sample rate after upsampling
    int outputSampleRate() const { return m_outputRate; }
    int inputSampleRate() const { return m_inputRate; }
    bool isActive() const { return m_enabled.load(std::memory_order_relaxed) && m_mode != UpsamplingMode::None && m_outputRate != m_inputRate; }

    // Separate I/O buffer processing (used by AudioEngine render path)
    size_t processUpsampling(const float* input, int inputFrames, int channels,
                             float* output, int maxOutputFrames);

    // For Signal Path display
    QString getDescription() const;

signals:
    void configurationChanged();
    void outputRateChanged(int newRate);

private:
    void reconfigure();
    void destroySoxr();
    int calculateTargetRate(int sourceRate) const;
    soxr_quality_spec_t buildQualitySpec() const;

    std::atomic<bool> m_enabled{false};
    UpsamplingMode m_mode = UpsamplingMode::None;
    UpsamplingQuality m_quality = UpsamplingQuality::High;
    UpsamplingFilter m_filter = UpsamplingFilter::LinearPhase;
    int m_fixedRate = 352800;

    int m_inputRate = 44100;
    int m_outputRate = 44100;
    int m_channels = 2;

    soxr_t m_soxr = nullptr;

    int m_maxDacRate = 384000;
    bool m_deviceIsBuiltIn = false;
};
