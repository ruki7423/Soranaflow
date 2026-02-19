#include "UpsamplerProcessor.h"
#include <QDebug>
#include <cmath>
#include <cstring>

UpsamplerProcessor::UpsamplerProcessor(QObject* parent)
    : QObject(parent)
{
}

UpsamplerProcessor::~UpsamplerProcessor()
{
    destroySoxr();
}

// THREAD SAFETY: Null the pointer before deleting so the RT thread
// (which checks !m_soxr at processUpsampling entry) sees passthrough
// immediately. All callers that change upsampling config ultimately go
// through applyUpsamplingChange() → load() → stop() which stops the
// render callback before reconfigure().
void UpsamplerProcessor::destroySoxr()
{
    soxr_t old = m_soxr;
    m_soxr = nullptr;
    if (old) soxr_delete(old);
}

void UpsamplerProcessor::setEnabled(bool enabled)
{
    m_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        reconfigure();
    } else {
        destroySoxr();
        m_outputRate = m_inputRate;
    }
    emit configurationChanged();
}

void UpsamplerProcessor::setMode(UpsamplingMode mode)
{
    m_mode = mode;
    reconfigure();
    emit configurationChanged();
}

void UpsamplerProcessor::setQuality(UpsamplingQuality quality)
{
    m_quality = quality;
    reconfigure();
    emit configurationChanged();
}

void UpsamplerProcessor::setFilter(UpsamplingFilter filter)
{
    m_filter = filter;
    reconfigure();
    emit configurationChanged();
}

void UpsamplerProcessor::setFixedRate(int rate)
{
    m_fixedRate = rate;
    if (m_mode == UpsamplingMode::Fixed) {
        reconfigure();
        emit configurationChanged();
    }
}

void UpsamplerProcessor::setMaxDacRate(int rate)
{
    if (m_maxDacRate == rate) return;
    m_maxDacRate = rate;
    reconfigure();
}

void UpsamplerProcessor::setDeviceIsBuiltIn(bool builtIn)
{
    if (m_deviceIsBuiltIn == builtIn) return;
    m_deviceIsBuiltIn = builtIn;
    reconfigure();
}

void UpsamplerProcessor::setInputFormat(int sampleRate, int channels)
{
    m_inputRate = sampleRate;
    m_channels = channels;
    reconfigure();
}

int UpsamplerProcessor::calculateTargetRate(int sourceRate) const
{
    // Determine the base family (44.1k or 48k)
    bool is44family = (sourceRate % 44100 == 0) || (sourceRate == 88200) ||
                      (sourceRate == 176400) || (sourceRate == 352800);

    // For built-in devices (e.g., MacBook speakers fixed at 96kHz):
    // - Stay in the same sample rate family to avoid cross-family resampling
    // - Find highest same-family rate <= device rate
    // - If source exceeds device rate, downsample to highest same-family rate <= device rate
    if (m_deviceIsBuiltIn && m_maxDacRate > 0 && m_mode != UpsamplingMode::None) {
        // Find the highest in-family rate that fits within the device's capability
        int bestRate = sourceRate;
        if (is44family) {
            // 44.1k family: 44100, 88200, 176400, 352800
            static const int rates44[] = { 352800, 176400, 88200, 44100 };
            for (int r : rates44) {
                if (r <= m_maxDacRate) { bestRate = r; break; }
            }
        } else {
            // 48k family: 48000, 96000, 192000, 384000
            static const int rates48[] = { 384000, 192000, 96000, 48000 };
            for (int r : rates48) {
                if (r <= m_maxDacRate) { bestRate = r; break; }
            }
        }
        return bestRate;
    }

    switch (m_mode) {
    case UpsamplingMode::None:
        return sourceRate;

    case UpsamplingMode::Double: {
        int target = sourceRate * 2;
        return (target <= m_maxDacRate) ? target : sourceRate;
    }

    case UpsamplingMode::Quadruple: {
        int target = sourceRate * 4;
        return (target <= m_maxDacRate) ? target : sourceRate;
    }

    case UpsamplingMode::PowerOf2:
    case UpsamplingMode::MaxRate:
        if (is44family) {
            if (m_maxDacRate >= 352800) return 352800;
            if (m_maxDacRate >= 176400) return 176400;
            if (m_maxDacRate >= 88200)  return 88200;
            return sourceRate;
        } else {
            if (m_maxDacRate >= 384000) return 384000;
            if (m_maxDacRate >= 192000) return 192000;
            if (m_maxDacRate >= 96000)  return 96000;
            return sourceRate;
        }

    case UpsamplingMode::DSD256Rate:
        return is44family ? 352800 : 384000;

    case UpsamplingMode::Fixed:
        return (m_fixedRate <= m_maxDacRate) ? m_fixedRate : m_maxDacRate;
    }
    return sourceRate;
}

soxr_quality_spec_t UpsamplerProcessor::buildQualitySpec() const
{
    unsigned long recipe = SOXR_HQ;
    switch (m_quality) {
    case UpsamplingQuality::Quick:    recipe = SOXR_QQ; break;
    case UpsamplingQuality::Low:      recipe = SOXR_LQ; break;
    case UpsamplingQuality::Medium:   recipe = SOXR_MQ; break;
    case UpsamplingQuality::High:     recipe = SOXR_HQ; break;
    case UpsamplingQuality::VeryHigh: recipe = SOXR_VHQ; break;
    }

    unsigned long flags = 0;
    switch (m_filter) {
    case UpsamplingFilter::LinearPhase:
        flags = SOXR_LINEAR_PHASE;
        break;
    case UpsamplingFilter::MinimumPhase:
        flags = SOXR_MINIMUM_PHASE;
        break;
    case UpsamplingFilter::SteepFilter:
        flags = SOXR_LINEAR_PHASE | SOXR_STEEP_FILTER;
        break;
    case UpsamplingFilter::SlowRolloff:
        flags = SOXR_LINEAR_PHASE;
        break;
    }

    return soxr_quality_spec(recipe | flags, 0);
}

void UpsamplerProcessor::reconfigure()
{
    destroySoxr();

    if (!m_enabled.load(std::memory_order_relaxed) || m_mode == UpsamplingMode::None) {
        m_outputRate = m_inputRate;
        return;
    }

    int targetRate = calculateTargetRate(m_inputRate);

    // No conversion needed if target matches source
    if (targetRate == m_inputRate) {
        m_outputRate = m_inputRate;
        qDebug() << "[Upsampler] Target rate == source" << m_inputRate << ", passthrough";
        return;
    }

    // For external DACs, never downsample (only built-in devices may need it)
    if (targetRate < m_inputRate && !m_deviceIsBuiltIn) {
        m_outputRate = m_inputRate;
        qDebug() << "[Upsampler] Target rate" << targetRate
                 << "< source" << m_inputRate << ", passthrough (external DAC)";
        return;
    }

    m_outputRate = targetRate;

    soxr_error_t error;
    soxr_io_spec_t ioSpec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t qualitySpec = buildQualitySpec();

    m_soxr = soxr_create(
        (double)m_inputRate,
        (double)m_outputRate,
        (unsigned)m_channels,
        &error,
        &ioSpec,
        &qualitySpec,
        nullptr
    );

    if (error || !m_soxr) {
        qWarning() << "[Upsampler] Failed to create soxr:" << (error ? error : "null");
        m_soxr = nullptr;
        m_outputRate = m_inputRate;
        return;
    }

    qDebug() << "[Upsampler] Configured:" << m_inputRate << "Hz ->"
             << m_outputRate << "Hz"
             << "quality:" << (int)m_quality
             << "filter:" << (int)m_filter
             << "ratio:" << ((double)m_outputRate / m_inputRate)
             << "builtIn:" << m_deviceIsBuiltIn;

    emit outputRateChanged(m_outputRate);
}

void UpsamplerProcessor::process(float* /*buffer*/, int /*frames*/, int /*channels*/)
{
    // No-op: upsampling uses processUpsampling() with separate I/O buffers.
    // This satisfies the IDSPProcessor interface but is not used for upsampling.
}

size_t UpsamplerProcessor::processUpsampling(
    const float* input, int inputFrames, int channels,
    float* output, int maxOutputFrames)
{
    if (!m_soxr || m_outputRate == m_inputRate) {
        // Passthrough: copy input to output
        int frames = std::min(inputFrames, maxOutputFrames);
        std::memcpy(output, input, frames * channels * sizeof(float));
        return frames;
    }

    size_t inputUsed = 0;
    size_t outputGenerated = 0;

    soxr_error_t error = soxr_process(
        m_soxr,
        input, (size_t)inputFrames, &inputUsed,
        output, (size_t)maxOutputFrames, &outputGenerated
    );

    if (error) {
        qWarning() << "[Upsampler] processUpsampling error:" << error;
        std::memset(output, 0, maxOutputFrames * channels * sizeof(float));
        return maxOutputFrames;
    }

    return outputGenerated;
}

QString UpsamplerProcessor::getDescription() const
{
    if (!m_enabled.load(std::memory_order_relaxed) || m_mode == UpsamplingMode::None || m_outputRate == m_inputRate) {
        return QString();
    }

    QString qualityStr;
    switch (m_quality) {
    case UpsamplingQuality::Quick:    qualityStr = QStringLiteral("Quick"); break;
    case UpsamplingQuality::Low:      qualityStr = QStringLiteral("Low"); break;
    case UpsamplingQuality::Medium:   qualityStr = QStringLiteral("Medium"); break;
    case UpsamplingQuality::High:     qualityStr = QStringLiteral("High"); break;
    case UpsamplingQuality::VeryHigh: qualityStr = QStringLiteral("Very High"); break;
    }

    QString filterStr;
    switch (m_filter) {
    case UpsamplingFilter::LinearPhase:  filterStr = QStringLiteral("Linear Phase"); break;
    case UpsamplingFilter::MinimumPhase: filterStr = QStringLiteral("Minimum Phase"); break;
    case UpsamplingFilter::SteepFilter:  filterStr = QStringLiteral("Steep"); break;
    case UpsamplingFilter::SlowRolloff:  filterStr = QStringLiteral("Slow Rolloff"); break;
    }

    return QStringLiteral("%1 kHz \u2192 %2 kHz (%3, %4)")
        .arg(m_inputRate / 1000.0, 0, 'f', 1)
        .arg(m_outputRate / 1000.0, 0, 'f', 1)
        .arg(qualityStr, filterStr);
}
