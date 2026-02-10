#pragma once

#include "IDSPProcessor.h"
#include "GainProcessor.h"
#include "EqualizerProcessor.h"

#include <QObject>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

// Ordered DSP processing chain.
// Thread-safe: prepare/process may be called from the audio thread,
// add/remove/reorder from the main thread.
class DSPPipeline : public QObject {
    Q_OBJECT
public:
    explicit DSPPipeline(QObject* parent = nullptr);

    // Process audio buffer through all enabled processors in order.
    // Called from the audio thread.
    void process(float* buf, int frames, int channels);

    // Prepare all processors for the given format.
    void prepare(double sampleRate, int channels);

    // Reset all processor states (e.g., after seek).
    void reset();

    // Enable / disable entire pipeline (thread-safe)
    bool isEnabled() const { return m_enabled.load(std::memory_order_acquire); }
    void setEnabled(bool enabled);

    // Access built-in processors
    GainProcessor*       gainProcessor()       { return m_gain.get(); }
    EqualizerProcessor*  equalizerProcessor()   { return m_eq.get(); }

    // Plugin slot management
    void addProcessor(std::shared_ptr<IDSPProcessor> proc);
    void removeProcessor(int index);
    int  processorCount() const;
    IDSPProcessor* processor(int index) const;

    // Call after modifying individual processor state (gain dB, EQ bands,
    // processor enable/disable) to notify the signal path widget.
    void notifyConfigurationChanged();

signals:
    void configurationChanged();

private:
    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_processLogOnce{false};
    std::atomic<bool> m_pluginLogOnce{false};
    std::atomic<int>  m_processCallCount{0};  // periodic diagnostic counter
    double m_sampleRate = 44100.0;
    int m_channels = 2;

    // Built-in processors (always present)
    std::unique_ptr<GainProcessor>      m_gain;
    std::unique_ptr<EqualizerProcessor> m_eq;

    // Plugin processors (VST3, etc.)
    std::vector<std::shared_ptr<IDSPProcessor>> m_plugins;
    mutable std::mutex m_pluginMutex;
};
