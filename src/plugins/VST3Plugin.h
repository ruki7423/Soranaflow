#pragma once

#include "../core/dsp/IDSPProcessor.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/funknownimpl.h"

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <array>

class QWidget;

// IPlugFrame implementation for resize callbacks from the plugin editor.
class PlugFrameAdapter : public Steinberg::U::ImplementsNonDestroyable<
    Steinberg::U::Directly<Steinberg::IPlugFrame>>
{
public:
    PlugFrameAdapter(QWidget* window) : m_window(window) {}

    Steinberg::tresult PLUGIN_API resizeView(
        Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

private:
    QWidget* m_window = nullptr;
};

// IComponentHandler — receives parameter edits from the plugin controller.
// Stores pending parameter changes for the next process() call.
class ComponentHandlerAdapter : public Steinberg::U::ImplementsNonDestroyable<
    Steinberg::U::Directly<Steinberg::Vst::IComponentHandler>>
{
public:
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override
        { return Steinberg::kResultOk; }

    Steinberg::tresult PLUGIN_API performEdit(
        Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override
    {
        // Store the change for the audio thread to pick up
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_changeCount < MAX_CHANGES) {
            m_changes[m_changeCount++] = {id, value};
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override
        { return Steinberg::kResultOk; }

    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

    // Drain pending changes (called from audio thread)
    struct ParamChange { Steinberg::Vst::ParamID id; Steinberg::Vst::ParamValue value; };
    static constexpr int MAX_CHANGES = 64;

    int drainChanges(ParamChange* out, int maxCount) {
        std::lock_guard<std::mutex> lock(m_mutex);
        int n = (m_changeCount < maxCount) ? m_changeCount : maxCount;
        for (int i = 0; i < n; ++i) out[i] = m_changes[i];
        m_changeCount = 0;
        return n;
    }

    std::atomic<bool> restartRequested{false};

private:
    std::mutex m_mutex;
    std::array<ParamChange, MAX_CHANGES> m_changes;
    int m_changeCount = 0;
};

// Real VST3 plugin loaded via the Steinberg VST3 SDK.
// Wraps Module -> IComponent -> IAudioProcessor -> IEditController -> IPlugView.
class VST3Plugin : public IDSPProcessor {
public:
    VST3Plugin();
    ~VST3Plugin() override;

    // Load a .vst3 bundle from disk. Returns true on success.
    // classIndex: specific class to load (-1 = auto-select best audio class)
    bool loadFromPath(const std::string& vst3Path, int classIndex = -1);

    // Unload and release all SDK objects.
    void unload();

    bool isLoaded() const { return m_loaded; }

    // Plugin metadata (read from SDK after loading)
    const std::string& pluginName() const { return m_pluginName; }
    const std::string& pluginVendor() const { return m_pluginVendor; }
    const std::string& pluginPath() const { return m_pluginPath; }
    const std::string& pluginUID() const { return m_pluginUID; }
    bool isEffect() const { return m_isEffect; }

    // Editor support
    bool hasEditor() const;
    // Open native plugin editor in a new window. Returns the window (caller does not own).
    QWidget* openEditor(QWidget* parent = nullptr);
    void closeEditor();

    // State persistence
    QByteArray saveState() const override;
    bool restoreState(const QByteArray& data) override;

    std::string getPluginPath() const override { return m_pluginPath; }

    // --- IDSPProcessor interface ---
    void process(float* buf, int frames, int channels) override;
    std::string getName() const override { return m_pluginName; }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool enabled) override { m_enabled = enabled; }
    void prepare(double sampleRate, int channels) override;
    void reset() override;

private:
    bool initializeComponent();
    bool initializeController();
    bool connectComponents();
    void disconnectComponents();
    bool setupProcessing(double sampleRate, int maxBlockSize);
    bool activateBusses();
    void showPlaceholderEditor(QWidget* parent);

    // SDK objects
    VST3::Hosting::Module::Ptr m_module;
    Steinberg::IPtr<Steinberg::Vst::IComponent> m_component;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> m_processor;
    Steinberg::IPtr<Steinberg::Vst::IEditController> m_controller;
    Steinberg::IPtr<Steinberg::IPlugView> m_plugView;
    bool m_separateController = false;

    // Connection proxies for component <-> controller
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> m_componentCP;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> m_controllerCP;

    // Host adapters
    std::unique_ptr<PlugFrameAdapter> m_plugFrame;
    ComponentHandlerAdapter m_componentHandler;

    // Audio processing buffers (non-interleaved for VST3)
    std::vector<std::vector<float>> m_inputChannelBuffers;
    std::vector<std::vector<float>> m_outputChannelBuffers;
    std::vector<float*> m_inputPtrs;
    std::vector<float*> m_outputPtrs;

    // State
    bool m_loaded = false;
    bool m_enabled = true;
    bool m_isEffect = true;
    bool m_componentInitialized = false;
    bool m_controllerInitialized = false;
    bool m_processing = false;
    double m_sampleRate = 44100.0;
    int m_channels = 2;
    int m_maxBlockSize = 4096;
    int64_t m_transportPos = 0;

    // Metadata
    std::string m_pluginName;
    std::string m_pluginVendor;
    std::string m_pluginPath;
    std::string m_pluginUID;

    // Editor window ref (we don't own it, Qt manages lifetime)
    QWidget* m_editorWindow = nullptr;

    std::mutex m_processMutex;
};
