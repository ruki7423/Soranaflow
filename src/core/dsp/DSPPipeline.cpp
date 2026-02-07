#include "DSPPipeline.h"

DSPPipeline::DSPPipeline(QObject* parent)
    : QObject(parent)
    , m_gain(std::make_unique<GainProcessor>())
    , m_eq(std::make_unique<EqualizerProcessor>())
{
}

void DSPPipeline::setEnabled(bool enabled)
{
    m_enabled.store(enabled, std::memory_order_release);
    emit configurationChanged();
}

void DSPPipeline::notifyConfigurationChanged()
{
    emit configurationChanged();
}

void DSPPipeline::process(float* buf, int frames, int channels)
{
    if (!m_enabled.load(std::memory_order_acquire)) return;

    // Signal chain: Gain -> EQ -> Plugins
    m_gain->process(buf, frames, channels);
    m_eq->process(buf, frames, channels);

    // Process plugin chain — use try_lock to avoid priority inversion
    // on the real-time audio thread. If the main thread holds the lock
    // (e.g., adding/removing plugins), we skip plugin processing for
    // this buffer rather than blocking the audio thread.
    std::unique_lock<std::mutex> lock(m_pluginMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        for (auto& proc : m_plugins) {
            if (proc && proc->isEnabled()) {
                proc->process(buf, frames, channels);
            }
        }
    }
}

void DSPPipeline::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;

    m_gain->prepare(sampleRate, channels);
    m_eq->prepare(sampleRate, channels);

    std::lock_guard<std::mutex> lock(m_pluginMutex);
    for (auto& proc : m_plugins) {
        if (proc) proc->prepare(sampleRate, channels);
    }
}

void DSPPipeline::reset()
{
    m_gain->reset();
    m_eq->reset();

    std::lock_guard<std::mutex> lock(m_pluginMutex);
    for (auto& proc : m_plugins) {
        if (proc) proc->reset();
    }
}

void DSPPipeline::addProcessor(std::shared_ptr<IDSPProcessor> proc)
{
    if (!proc) return;
    proc->prepare(m_sampleRate, m_channels);
    std::lock_guard<std::mutex> lock(m_pluginMutex);
    m_plugins.push_back(std::move(proc));
    emit configurationChanged();
}

void DSPPipeline::removeProcessor(int index)
{
    std::lock_guard<std::mutex> lock(m_pluginMutex);
    if (index >= 0 && index < (int)m_plugins.size()) {
        m_plugins.erase(m_plugins.begin() + index);
    }
    emit configurationChanged();
}

int DSPPipeline::processorCount() const
{
    std::lock_guard<std::mutex> lock(m_pluginMutex);
    return (int)m_plugins.size();
}

IDSPProcessor* DSPPipeline::processor(int index) const
{
    std::lock_guard<std::mutex> lock(m_pluginMutex);
    if (index >= 0 && index < (int)m_plugins.size()) {
        return m_plugins[index].get();
    }
    return nullptr;
}
