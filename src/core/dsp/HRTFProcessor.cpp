#include "HRTFProcessor.h"

#ifdef HAVE_LIBMYSOFA

#include <QDebug>
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

HRTFProcessor::HRTFProcessor()
{
}

HRTFProcessor::~HRTFProcessor()
{
    unloadSOFA();
}

bool HRTFProcessor::loadSOFA(const QString& filePath)
{
    unloadSOFA();

    int filterLength = 0;
    int err = MYSOFA_OK;

    m_sofa = mysofa_open(filePath.toUtf8().constData(),
                         static_cast<float>(m_sampleRate),
                         &filterLength, &err);

    if (!m_sofa || err != MYSOFA_OK) {
        qDebug() << "[HRTF] Failed to load SOFA:" << filePath << "error:" << err;
        m_sofa = nullptr;
        return false;
    }

    m_irLength = filterLength;
    m_loaded = true;
    m_sofaPath = filePath;

    qDebug() << "[HRTF] Loaded SOFA:" << filePath
             << "IR length:" << m_irLength
             << "sample rate:" << m_sampleRate;

    updateFilters();
    return true;
}

void HRTFProcessor::unloadSOFA()
{
    if (m_sofa) {
        mysofa_close(m_sofa);
        m_sofa = nullptr;
    }
    m_loaded = false;
    m_irLength = 0;
    m_sofaPath.clear();
    m_irLL.clear();
    m_irLR.clear();
    m_irRL.clear();
    m_irRR.clear();
}

void HRTFProcessor::setEnabled(bool enabled)
{
    if (enabled && !m_enabled.load(std::memory_order_relaxed)) {
        m_needsStateReset.store(true, std::memory_order_relaxed);
    }
    m_enabled.store(enabled, std::memory_order_relaxed);
    qDebug() << "[HRTF] Enabled:" << enabled;
}

void HRTFProcessor::setSpeakerAngle(float degrees)
{
    degrees = std::clamp(degrees, 10.0f, 90.0f);
    m_pendingAngle.store(degrees, std::memory_order_relaxed);
}

void HRTFProcessor::setSampleRate(int rate)
{
    if (m_sampleRate != rate) {
        m_sampleRate = rate;
        m_needsFilterUpdate.store(true, std::memory_order_relaxed);
    }
}

void HRTFProcessor::updateFilters()
{
    if (!m_sofa || m_irLength <= 0) return;

    m_irLL.resize(m_irLength);
    m_irLR.resize(m_irLength);
    m_irRL.resize(m_irLength);
    m_irRR.resize(m_irLength);

    // Convert speaker angle to Cartesian coordinates
    // SOFA convention: x=front, y=left, z=up
    float angRad = m_speakerAngle * static_cast<float>(M_PI) / 180.0f;

    // Left speaker at -speakerAngle (positive Y = left in SOFA)
    float leftX = cosf(angRad);
    float leftY = sinf(angRad);
    float leftZ = 0.0f;

    // Right speaker at +speakerAngle (negative Y = right)
    float rightX = cosf(angRad);
    float rightY = -sinf(angRad);
    float rightZ = 0.0f;

    float delayL = 0.0f, delayR = 0.0f;

    // Get HRTF for left speaker position → both ears
    mysofa_getfilter_float(m_sofa,
        leftX, leftY, leftZ,
        m_irLL.data(), m_irLR.data(),
        &delayL, &delayR);

    qDebug() << "[HRTF] Left speaker IR: delayL=" << delayL << "delayR=" << delayR;

    // Get HRTF for right speaker position → both ears
    mysofa_getfilter_float(m_sofa,
        rightX, rightY, rightZ,
        m_irRL.data(), m_irRR.data(),
        &delayL, &delayR);

    qDebug() << "[HRTF] Right speaker IR: delayL=" << delayL << "delayR=" << delayR;

    // Allocate history buffers
    int histLen = m_irLength - 1;
    m_historyL.assign(histLen, 0.0f);
    m_historyR.assign(histLen, 0.0f);

    qDebug() << "[HRTF] Filters updated: angle=" << m_speakerAngle
             << "IR length=" << m_irLength;
}

void HRTFProcessor::reset()
{
    std::fill(m_historyL.begin(), m_historyL.end(), 0.0f);
    std::fill(m_historyR.begin(), m_historyR.end(), 0.0f);
    m_wetMix = 0.0f;
}

void HRTFProcessor::process(float* buffer, int frameCount)
{
    bool wantEnabled = m_enabled.load(std::memory_order_relaxed);

    // Skip if disabled and fully faded out
    if (!wantEnabled && m_wetMix <= 0.0f) return;

    // Skip if no HRTF loaded
    if (!m_loaded || m_irLength <= 0) return;

    // Reset state on enable
    if (m_needsStateReset.exchange(false, std::memory_order_relaxed)) {
        reset();
    }

    // Apply pending angle change
    float pendingAngle = m_pendingAngle.exchange(-1.0f, std::memory_order_relaxed);
    if (pendingAngle >= 0.0f) {
        m_speakerAngle = pendingAngle;
        updateFilters();
    }

    // Apply pending sample rate change (requires SOFA reload)
    if (m_needsFilterUpdate.exchange(false, std::memory_order_relaxed)) {
        if (m_loaded && !m_sofaPath.isEmpty()) {
            QString path = m_sofaPath;
            loadSOFA(path);
        }
    }

    int histLen = m_irLength - 1;

    // Process each frame
    for (int n = 0; n < frameCount; n++) {
        float inL = buffer[n * 2];
        float inR = buffer[n * 2 + 1];

        // Smooth fade in/out
        if (wantEnabled && m_wetMix < 1.0f) {
            m_wetMix = std::min(1.0f, m_wetMix + FADE_STEP);
        } else if (!wantEnabled && m_wetMix > 0.0f) {
            m_wetMix = std::max(0.0f, m_wetMix - FADE_STEP);
        }

        // FIR convolution: direct-form time-domain
        // outL = inL * irLL + inR * irRL
        // outR = inL * irLR + inR * irRR
        float sumL = 0.0f, sumR = 0.0f;

        // k=0 term (current sample)
        sumL += inL * m_irLL[0] + inR * m_irRL[0];
        sumR += inL * m_irLR[0] + inR * m_irRR[0];

        // k=1..irLength-1 terms (history)
        for (int k = 1; k < m_irLength; k++) {
            int hIdx = histLen - k;
            if (hIdx >= 0) {
                sumL += m_historyL[hIdx] * m_irLL[k] + m_historyR[hIdx] * m_irRL[k];
                sumR += m_historyL[hIdx] * m_irLR[k] + m_historyR[hIdx] * m_irRR[k];
            }
        }

        // Shift history (FIFO)
        if (histLen > 0) {
            std::memmove(m_historyL.data(), m_historyL.data() + 1, (histLen - 1) * sizeof(float));
            std::memmove(m_historyR.data(), m_historyR.data() + 1, (histLen - 1) * sizeof(float));
            m_historyL[histLen - 1] = inL;
            m_historyR[histLen - 1] = inR;
        }

        // Blend dry/wet
        buffer[n * 2]     = inL * (1.0f - m_wetMix) + sumL * m_wetMix;
        buffer[n * 2 + 1] = inR * (1.0f - m_wetMix) + sumR * m_wetMix;
    }

    // Clear state if fully faded out
    if (m_wetMix <= 0.0f) {
        reset();
    }
}

#endif // HAVE_LIBMYSOFA
