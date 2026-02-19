#include "HRTFProcessor.h"

#ifdef HAVE_LIBMYSOFA

#include <QDebug>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>

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

    {
        std::lock_guard<std::mutex> lock(m_metaMutex);
        m_irLength = filterLength;
        m_loaded = true;
        m_sofaPath = filePath;
    }

    qDebug() << "[HRTF] Loaded SOFA:" << filePath
             << "IR length:" << m_irLength
             << "sample rate:" << m_sampleRate;

    buildStagedFilters(m_speakerAngle);
    return true;
}

void HRTFProcessor::unloadSOFA()
{
    if (m_sofa) {
        mysofa_close(m_sofa);
        m_sofa = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_metaMutex);
        m_loaded = false;
        m_irLength = 0;
        m_sofaPath.clear();
    }
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
    if (m_loaded && m_sofa)
        buildStagedFilters(degrees);
}

void HRTFProcessor::setSampleRate(int rate)
{
    if (m_sampleRate != rate) {
        m_sampleRate = rate;
        // Reload SOFA at new sample rate (rebuilds staged filters)
        if (m_loaded && !m_sofaPath.isEmpty()) {
            loadSOFA(m_sofaPath);
        }
    }
}

void HRTFProcessor::buildStagedFilters(float angle)
{
    if (!m_sofa || m_irLength <= 0) return;

    // Wait for RT thread to consume previous staged data before overwriting
    int timeout = 100;
    while (m_stagedReady.load(std::memory_order_acquire) && --timeout > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (timeout <= 0) return;  // RT thread not consuming — skip this update

    m_staged.irLength = m_irLength;
    m_staged.angle = angle;
    m_staged.irLL.resize(m_irLength);
    m_staged.irLR.resize(m_irLength);
    m_staged.irRL.resize(m_irLength);
    m_staged.irRR.resize(m_irLength);

    // Convert speaker angle to Cartesian coordinates
    // SOFA convention: x=front, y=left, z=up
    float angRad = angle * static_cast<float>(M_PI) / 180.0f;

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
        m_staged.irLL.data(), m_staged.irLR.data(),
        &delayL, &delayR);

    qDebug() << "[HRTF] Left speaker IR: delayL=" << delayL << "delayR=" << delayR;

    // Get HRTF for right speaker position → both ears
    mysofa_getfilter_float(m_sofa,
        rightX, rightY, rightZ,
        m_staged.irRL.data(), m_staged.irRR.data(),
        &delayL, &delayR);

    qDebug() << "[HRTF] Right speaker IR: delayL=" << delayL << "delayR=" << delayR;

#ifdef __APPLE__
    // Compute reversed IRs for vDSP_conv block FIR
    auto reverseIR = [](const std::vector<float>& ir) {
        return std::vector<float>(ir.rbegin(), ir.rend());
    };
    m_staged.revIrLL = reverseIR(m_staged.irLL);
    m_staged.revIrLR = reverseIR(m_staged.irLR);
    m_staged.revIrRL = reverseIR(m_staged.irRL);
    m_staged.revIrRR = reverseIR(m_staged.irRR);

    // Pre-allocate extended signal and output buffers
    int histLen = m_irLength - 1;
    int maxFrames = 4096;
    m_staged.extL.assign(histLen + maxFrames, 0.0f);
    m_staged.extR.assign(histLen + maxFrames, 0.0f);
    m_staged.outL.resize(maxFrames, 0.0f);
    m_staged.outR.resize(maxFrames, 0.0f);
    m_staged.tempFir.resize(maxFrames, 0.0f);
#else
    // Pre-allocate history buffers for per-sample FIR
    int histLen = m_irLength - 1;
    m_staged.historyL.assign(histLen, 0.0f);
    m_staged.historyR.assign(histLen, 0.0f);
#endif

    // Signal render thread to swap
    m_stagedReady.store(true, std::memory_order_release);

    qDebug() << "[HRTF] Staged filters: angle=" << angle
             << "IR length=" << m_irLength;
}

void HRTFProcessor::reset()
{
#ifdef __APPLE__
    // Clear extended signal history region
    int histLen = m_irLength > 0 ? m_irLength - 1 : 0;
    if (!m_extL.empty() && histLen > 0) {
        std::memset(m_extL.data(), 0, histLen * sizeof(float));
        std::memset(m_extR.data(), 0, histLen * sizeof(float));
    }
#else
    std::fill(m_historyL.begin(), m_historyL.end(), 0.0f);
    std::fill(m_historyR.begin(), m_historyR.end(), 0.0f);
#endif
    m_wetMix = 0.0f;
}

void HRTFProcessor::process(float* buf, int frames, int channels)
{
    if (channels != 2) return;  // stereo-only processor
    process(buf, frames);
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

    // Swap staged filters if ready (RT-safe: std::swap is O(1), no allocation)
    if (m_stagedReady.load(std::memory_order_acquire)) {
        std::swap(m_irLL, m_staged.irLL);
        std::swap(m_irLR, m_staged.irLR);
        std::swap(m_irRL, m_staged.irRL);
        std::swap(m_irRR, m_staged.irRR);
#ifdef __APPLE__
        std::swap(m_revIrLL, m_staged.revIrLL);
        std::swap(m_revIrLR, m_staged.revIrLR);
        std::swap(m_revIrRL, m_staged.revIrRL);
        std::swap(m_revIrRR, m_staged.revIrRR);
        std::swap(m_extL, m_staged.extL);
        std::swap(m_extR, m_staged.extR);
        std::swap(m_outL, m_staged.outL);
        std::swap(m_outR, m_staged.outR);
        std::swap(m_tempFir, m_staged.tempFir);
#else
        std::swap(m_historyL, m_staged.historyL);
        std::swap(m_historyR, m_staged.historyR);
#endif
        m_irLength = m_staged.irLength;
        m_speakerAngle = m_staged.angle;
        m_stagedReady.store(false, std::memory_order_release);
    }

    // Safety: if no filters loaded yet, skip processing
    if (m_irLL.empty() || m_irLength <= 0) return;

    int histLen = m_irLength - 1;

#ifdef __APPLE__
    // Block-based FIR via vDSP_conv
    if (m_revIrLL.empty() || frameCount <= 0) return;
    if (frameCount > (int)m_outL.size()) return;  // safety: max 4096

    // Append current block to extended signal buffer [history | current]
    for (int n = 0; n < frameCount; n++) {
        m_extL[histLen + n] = buffer[n * 2];
        m_extR[histLen + n] = buffer[n * 2 + 1];
    }

    // 4 convolutions: outL = extL*irLL + extR*irRL, outR = extL*irLR + extR*irRR
    vDSP_Length resultLen = static_cast<vDSP_Length>(frameCount);
    vDSP_Length filterLen = static_cast<vDSP_Length>(m_irLength);

    vDSP_conv(m_extL.data(), 1, m_revIrLL.data(), 1, m_outL.data(), 1, resultLen, filterLen);
    vDSP_conv(m_extR.data(), 1, m_revIrRL.data(), 1, m_tempFir.data(), 1, resultLen, filterLen);
    vDSP_vadd(m_outL.data(), 1, m_tempFir.data(), 1, m_outL.data(), 1, resultLen);

    vDSP_conv(m_extL.data(), 1, m_revIrLR.data(), 1, m_outR.data(), 1, resultLen, filterLen);
    vDSP_conv(m_extR.data(), 1, m_revIrRR.data(), 1, m_tempFir.data(), 1, resultLen, filterLen);
    vDSP_vadd(m_outR.data(), 1, m_tempFir.data(), 1, m_outR.data(), 1, resultLen);

    // Blend dry/wet with per-sample fade
    for (int n = 0; n < frameCount; n++) {
        float inL = buffer[n * 2];
        float inR = buffer[n * 2 + 1];

        if (wantEnabled && m_wetMix < 1.0f)
            m_wetMix = std::min(1.0f, m_wetMix + FADE_STEP);
        else if (!wantEnabled && m_wetMix > 0.0f)
            m_wetMix = std::max(0.0f, m_wetMix - FADE_STEP);

        buffer[n * 2]     = inL * (1.0f - m_wetMix) + m_outL[n] * m_wetMix;
        buffer[n * 2 + 1] = inR * (1.0f - m_wetMix) + m_outR[n] * m_wetMix;
    }

    // Shift history: move last histLen samples to start of ext buffer
    if (histLen > 0) {
        std::memmove(m_extL.data(), m_extL.data() + frameCount, histLen * sizeof(float));
        std::memmove(m_extR.data(), m_extR.data() + frameCount, histLen * sizeof(float));
    }

#else
    // Per-sample FIR (non-Apple fallback)
    for (int n = 0; n < frameCount; n++) {
        float inL = buffer[n * 2];
        float inR = buffer[n * 2 + 1];

        if (wantEnabled && m_wetMix < 1.0f)
            m_wetMix = std::min(1.0f, m_wetMix + FADE_STEP);
        else if (!wantEnabled && m_wetMix > 0.0f)
            m_wetMix = std::max(0.0f, m_wetMix - FADE_STEP);

        float sumL = inL * m_irLL[0] + inR * m_irRL[0];
        float sumR = inL * m_irLR[0] + inR * m_irRR[0];

        for (int k = 1; k < m_irLength; k++) {
            int hIdx = histLen - k;
            if (hIdx >= 0) {
                sumL += m_historyL[hIdx] * m_irLL[k] + m_historyR[hIdx] * m_irRL[k];
                sumR += m_historyL[hIdx] * m_irLR[k] + m_historyR[hIdx] * m_irRR[k];
            }
        }

        if (histLen > 0) {
            std::memmove(m_historyL.data(), m_historyL.data() + 1, (histLen - 1) * sizeof(float));
            std::memmove(m_historyR.data(), m_historyR.data() + 1, (histLen - 1) * sizeof(float));
            m_historyL[histLen - 1] = inL;
            m_historyR[histLen - 1] = inR;
        }

        buffer[n * 2]     = inL * (1.0f - m_wetMix) + sumL * m_wetMix;
        buffer[n * 2 + 1] = inR * (1.0f - m_wetMix) + sumR * m_wetMix;
    }
#endif

    // Clear state if fully faded out
    if (m_wetMix <= 0.0f) {
        reset();
    }
}

#endif // HAVE_LIBMYSOFA
