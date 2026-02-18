#include "DSPPipeline.h"
#include <QDebug>

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
    if (m_gain) m_gain->process(buf, frames, channels);
    if (m_eq)   m_eq->process(buf, frames, channels);

    // Process plugin chain — use try_lock to avoid priority inversion
    // on the real-time audio thread. If the main thread holds the lock
    // (e.g., adding/removing plugins), we skip plugin processing for
    // this buffer rather than blocking the audio thread.
    std::shared_lock<std::shared_mutex> lock(m_pluginMutex, std::try_to_lock);
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

    std::unique_lock<std::shared_mutex> lock(m_pluginMutex);
    for (auto& proc : m_plugins) {
        if (proc) proc->prepare(sampleRate, channels);
    }
}

void DSPPipeline::reset()
{
    m_gain->reset();
    m_eq->reset();

    std::unique_lock<std::shared_mutex> lock(m_pluginMutex);
    for (auto& proc : m_plugins) {
        if (proc) proc->reset();
    }
}

void DSPPipeline::addProcessor(std::shared_ptr<IDSPProcessor> proc)
{
    if (!proc) return;
    proc->prepare(m_sampleRate, m_channels);

    // Scoped lock — emit signal AFTER releasing to avoid deadlock.
    // getSignalPath() calls processorCount()/processor() which also lock m_pluginMutex.
    {
        std::unique_lock<std::shared_mutex> lock(m_pluginMutex);
        m_plugins.push_back(std::move(proc));
        qDebug() << "[DSPPipeline] Added processor — external processors:" << (int)m_plugins.size();
    }

    // Auto-enable pipeline when plugins are added — user expects VSTs to process
    if (!m_enabled.load(std::memory_order_acquire)) {
        m_enabled.store(true, std::memory_order_release);
        qDebug() << "[DSPPipeline] Auto-enabled — processor added while pipeline was disabled";
    }

    // Signal OUTSIDE lock scope — slots may call processorCount()/processor()
    emit configurationChanged();
}

void DSPPipeline::removeProcessor(int index)
{
    // Move the shared_ptr out of the vector under the lock, then destroy
    // it AFTER releasing the lock. Plugin destructors (closeEditor,
    // win->close()) can trigger Qt event processing that re-enters
    // processorCount()/processor() — which also lock m_pluginMutex.
    // Destroying inside the lock would deadlock on the non-recursive mutex.
    std::shared_ptr<IDSPProcessor> removed;
    {
        std::unique_lock<std::shared_mutex> lock(m_pluginMutex);
        if (index >= 0 && index < (int)m_plugins.size()) {
            qDebug() << "[DSPPipeline] Removing processor at index" << index
                     << "— remaining:" << (int)m_plugins.size() - 1;
            removed = std::move(m_plugins[index]);
            m_plugins.erase(m_plugins.begin() + index);
        }
    }
    // Signal OUTSIDE lock scope
    emit configurationChanged();
    // `removed` destroyed here — plugin cleanup runs without the lock held
    if (removed) {
        qDebug() << "[DSPPipeline] Plugin"
                 << QString::fromStdString(removed->getName()) << "destroyed safely";
    }
}

int DSPPipeline::processorCount() const
{
    std::shared_lock<std::shared_mutex> lock(m_pluginMutex);
    return (int)m_plugins.size();
}

IDSPProcessor* DSPPipeline::processor(int index) const
{
    std::shared_lock<std::shared_mutex> lock(m_pluginMutex);
    if (index >= 0 && index < (int)m_plugins.size()) {
        return m_plugins[index].get();
    }
    return nullptr;
}
