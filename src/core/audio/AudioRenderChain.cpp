#include "AudioRenderChain.h"
#include "../dsp/DSPPipeline.h"
#include "VolumeLevelingManager.h"
#include "../Settings.h"

#include <cmath>
#include <QDebug>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

// Padé [3,3] tanh approximation: tanh(x) ≈ x(15+x²)/(15+6x²)
// Accurate to <0.04% for |x|<1, <1.1% for |x|<2. Clamped for |x|>2.5.
static inline float fastTanhPade(float x) {
    if (__builtin_expect(x >  2.5f, 0)) return  1.0f;
    if (__builtin_expect(x < -2.5f, 0)) return -1.0f;
    float x2 = x * x;
    return x * (15.0f + x2) / (15.0f + 6.0f * x2);
}

AudioRenderChain::AudioRenderChain() = default;

void AudioRenderChain::setSampleRate(int rate)
{
    m_crossfeed.setSampleRate(rate);
    m_convolution.setSampleRate(rate);
    m_hrtf.setSampleRate(rate);
}

void AudioRenderChain::updateHeadroomGain()
{
    auto mode = Settings::instance()->headroomMode();
    double dB = 0.0;

    switch (mode) {
    case Settings::HeadroomMode::Off:
        dB = 0.0;
        break;
    case Settings::HeadroomMode::Auto: {
        bool anyDspActive = Settings::instance()->volumeLeveling()
                         || Settings::instance()->crossfeedEnabled()
                         || (Settings::instance()->convolutionEnabled() && m_convolution.hasIR())
                         || Settings::instance()->upsamplingEnabled();
        dB = anyDspActive ? -3.0 : 0.0;
        break;
    }
    case Settings::HeadroomMode::Manual:
        dB = Settings::instance()->manualHeadroom();
        break;
    }

    dB = qBound(-12.0, dB, 0.0);
    float linear = static_cast<float>(std::pow(10.0, dB / 20.0));
    m_headroomGain.store(linear, std::memory_order_relaxed);

    qDebug() << "[Headroom] Mode:" << static_cast<int>(mode)
             << "gain:" << dB << "dB linear:" << linear;
}

void AudioRenderChain::process(float* buf, int frames, int channels,
                                DSPPipeline* dsp, VolumeLevelingManager* leveling,
                                bool dopPassthrough, bool bitPerfect)
{
    if (frames <= 0 || dopPassthrough) return;

    // Chunk large buffers to stay within DSP processor internal limits
    while (frames > 4096) {
        process(buf, 4096, channels, dsp, leveling, dopPassthrough, bitPerfect);
        buf += 4096 * channels;
        frames -= 4096;
    }

    // Headroom (smooth ramp to prevent clicks on gain change)
    {
        float hr = m_headroomGain.load(std::memory_order_relaxed);
        float prevHr = m_prevHeadroomGain;
        if (hr != 1.0f || prevHr != 1.0f) {
#ifdef __APPLE__
            if (prevHr != hr && frames > 1) {
                float step = (hr - prevHr) / static_cast<float>(frames - 1);
                for (int c = 0; c < channels; ++c) {
                    float start = prevHr;
                    vDSP_vrampmul(buf + c, channels, &start, &step, buf + c, channels, frames);
                }
            } else {
                vDSP_vsmul(buf, 1, &hr, buf, 1, frames * channels);
            }
#else
            for (int f = 0; f < frames; ++f) {
                float t = (frames > 1) ? static_cast<float>(f) / static_cast<float>(frames - 1) : 1.0f;
                float gain = prevHr + (hr - prevHr) * t;
                for (int c = 0; c < channels; ++c)
                    buf[f * channels + c] *= gain;
            }
#endif
        }
        m_prevHeadroomGain = hr;
    }

    // Crossfeed (stereo only, mutually exclusive with HRTF — HRTF wins)
    if (channels == 2 && !(m_hrtf.isEnabled() && m_crossfeed.isEnabled())) {
        m_crossfeed.process(buf, frames);
    }

    // Convolution (room correction / IR)
    m_convolution.process(buf, frames, channels);

    // HRTF (binaural spatial audio)
    if (channels == 2) {
        m_hrtf.process(buf, frames);
    }

    // DSP pipeline (EQ, gain, plugins) — skip in bit-perfect mode
    if (!bitPerfect && dsp) {
        dsp->process(buf, frames, channels);
    }

    // Volume leveling (with smooth ramp to prevent clicks)
    if (leveling) {
        float targetGain = leveling->gainLinear();
        float prevGain = m_prevLevelingGain;
        if (targetGain != 1.0f || prevGain != 1.0f) {
#ifdef __APPLE__
            if (prevGain != targetGain && frames > 1) {
                float step = (targetGain - prevGain) / static_cast<float>(frames - 1);
                for (int c = 0; c < channels; ++c) {
                    float start = prevGain;
                    vDSP_vrampmul(buf + c, channels, &start, &step, buf + c, channels, frames);
                }
            } else {
                vDSP_vsmul(buf, 1, &targetGain, buf, 1, frames * channels);
            }
#else
            for (int f = 0; f < frames; ++f) {
                float t = (frames > 1) ? (float)f / (float)(frames - 1) : 1.0f;
                float gain = prevGain + (targetGain - prevGain) * t;
                for (int c = 0; c < channels; ++c)
                    buf[f * channels + c] *= gain;
            }
#endif
        }
        m_prevLevelingGain = targetGain;
    }

    // Peak limiter (safety net after all DSP)
    {
        int n = frames * channels;
        for (int i = 0; i < n; ++i) {
            float s = buf[i];
            // Sanitize NaN/Inf before limiter math
            if (__builtin_expect(!std::isfinite(s), 0)) { buf[i] = 0.0f; continue; }
            if (__builtin_expect(s > 0.95f, 0))
                buf[i] = 0.95f + 0.05f * fastTanhPade((s - 0.95f) * 20.0f);
            else if (__builtin_expect(s < -0.95f, 0))
                buf[i] = -0.95f - 0.05f * fastTanhPade((-s - 0.95f) * 20.0f);
        }
    }
}
