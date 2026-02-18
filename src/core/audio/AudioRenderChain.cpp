#include "AudioRenderChain.h"
#include "../dsp/DSPPipeline.h"
#include "VolumeLevelingManager.h"
#include "../Settings.h"

#include <cmath>
#include <QDebug>

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

    // Headroom
    {
        float hr = m_headroomGain.load(std::memory_order_relaxed);
        if (hr != 1.0f) {
            int n = frames * channels;
            for (int i = 0; i < n; ++i) buf[i] *= hr;
        }
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
            for (int f = 0; f < frames; ++f) {
                float t = (frames > 1) ? (float)f / (float)(frames - 1) : 1.0f;
                float gain = prevGain + (targetGain - prevGain) * t;
                for (int c = 0; c < channels; ++c) {
                    buf[f * channels + c] *= gain;
                }
            }
        }
        m_prevLevelingGain = targetGain;
    }

    // Peak limiter (safety net after all DSP)
    {
        int n = frames * channels;
        for (int i = 0; i < n; ++i) {
            float s = buf[i];
            if (s > 0.95f) buf[i] = 0.95f + 0.05f * std::tanh((s - 0.95f) / 0.05f);
            else if (s < -0.95f) buf[i] = -0.95f - 0.05f * std::tanh((-s - 0.95f) / 0.05f);
        }
    }
}
