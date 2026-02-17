#include "VST3Plugin.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/base/ibstream.h"

#include <QWidget>
#include <QWindow>
#include <QVBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QApplication>
#include <QDebug>
#include <QDataStream>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

// ═══════════════════════════════════════════════════════════════════════
//  Lightweight IBStream for state sync (stack-allocated, not ref-counted)
// ═══════════════════════════════════════════════════════════════════════

class MemoryStream : public IBStream
{
public:
    MemoryStream() = default;
    MemoryStream(const void* data, size_t size)
        : m_data(static_cast<const uint8_t*>(data),
                 static_cast<const uint8_t*>(data) + size) {}

    const uint8_t* getData() const { return m_data.data(); }
    size_t getSize() const { return m_data.size(); }

    // FUnknown — stack-allocated, no real ref-counting needed
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override
    { if (obj) *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
    {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        int32 avail = static_cast<int32>(m_data.size()) - static_cast<int32>(m_pos);
        if (avail < 0) avail = 0;
        int32 toRead = (numBytes < avail) ? numBytes : avail;
        if (toRead > 0) {
            std::memcpy(buffer, m_data.data() + m_pos, static_cast<size_t>(toRead));
            m_pos += static_cast<size_t>(toRead);
        }
        if (numBytesRead) *numBytesRead = toRead;
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override
    {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        size_t end = m_pos + static_cast<size_t>(numBytes);
        if (end > m_data.size()) m_data.resize(end);
        std::memcpy(m_data.data() + m_pos, buffer, static_cast<size_t>(numBytes));
        m_pos += static_cast<size_t>(numBytes);
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override
    {
        int64 newPos = 0;
        switch (mode) {
        case IBStream::kIBSeekSet: newPos = pos; break;
        case IBStream::kIBSeekCur: newPos = static_cast<int64>(m_pos) + pos; break;
        case IBStream::kIBSeekEnd: newPos = static_cast<int64>(m_data.size()) + pos; break;
        default: return kInvalidArgument;
        }
        if (newPos < 0) newPos = 0;
        m_pos = static_cast<size_t>(newPos);
        if (result) *result = static_cast<int64>(m_pos);
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) override
    {
        if (pos) *pos = static_cast<int64>(m_pos);
        return kResultOk;
    }

private:
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

// Single host context for all VST3 plugin instances
static Steinberg::Vst::HostApplication g_hostApp;

// ═══════════════════════════════════════════════════════════════════════
//  ComponentHandlerAdapter::restartComponent
// ═══════════════════════════════════════════════════════════════════════

tresult PLUGIN_API ComponentHandlerAdapter::restartComponent(int32 flags)
{
    qDebug() << "VST3: restartComponent requested, flags:" << flags;
    restartRequested.store(true, std::memory_order_release);
    return kResultOk;
}

// ═══════════════════════════════════════════════════════════════════════
//  Lightweight IParameterChanges for feeding GUI edits into process()
//  Stack-allocated, no ref-counting — lives only during one process() call
// ═══════════════════════════════════════════════════════════════════════

class SingleParamValueQueue : public IParamValueQueue
{
public:
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override
    { if (obj) *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    ParamID PLUGIN_API getParameterId() override { return m_id; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override
    {
        if (index != 0) return kResultFalse;
        sampleOffset = 0;
        value = m_value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override
    { return kResultFalse; }

    ParamID m_id = 0;
    ParamValue m_value = 0.0;
};

class HostParameterChanges : public IParameterChanges
{
public:
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override
    { if (obj) *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    int32 PLUGIN_API getParameterCount() override { return m_count; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        if (index < 0 || index >= m_count) return nullptr;
        return &m_queues[index];
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override
    { return nullptr; }

    static constexpr int MAX_PARAMS = 64;
    SingleParamValueQueue m_queues[MAX_PARAMS];
    int32 m_count = 0;

    void reset() { m_count = 0; }
    void add(ParamID id, ParamValue value) {
        if (m_count < MAX_PARAMS) {
            m_queues[m_count].m_id = id;
            m_queues[m_count].m_value = value;
            m_count++;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════
//  PlugFrameAdapter — resize callback for plugin editor
// ═══════════════════════════════════════════════════════════════════════

tresult PLUGIN_API PlugFrameAdapter::resizeView(IPlugView* view, ViewRect* newSize)
{
    if (!view || !newSize || !m_window) return kResultFalse;

    int w = newSize->getWidth();
    int h = newSize->getHeight();
    if (w <= 0 || h <= 0) return kResultFalse;

    qDebug() << "VST3: Plugin requests resize to" << w << "x" << h;
    m_window->setFixedSize(w, h);

    // Notify the plugin that resize happened
    view->onSize(newSize);
    return kResultTrue;
}

// ═══════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════

VST3Plugin::VST3Plugin() = default;

VST3Plugin::~VST3Plugin()
{
    unload();
}

// ═══════════════════════════════════════════════════════════════════════
//  Load a .vst3 bundle
// ═══════════════════════════════════════════════════════════════════════

bool VST3Plugin::loadFromPath(const std::string& vst3Path, int classIndex)
{
    qDebug() << "=== VST3Plugin::loadFromPath ===" << QString::fromStdString(vst3Path)
             << "classIndex:" << classIndex;

    if (m_loaded) unload();

    m_pluginPath = vst3Path;

    // 1. Load the module (bundle)
    std::string errorDesc;
    m_module = VST3::Hosting::Module::create(vst3Path, errorDesc);
    if (!m_module) {
        qWarning() << "VST3: Failed to load module:" << QString::fromStdString(errorDesc);
        return false;
    }
    qDebug() << "VST3: Module loaded OK";

    // 2. Get plugin factory and find an audio effect class
    auto& factory = m_module->getFactory();
    auto classInfos = factory.classInfos();

    qDebug() << "VST3: Found" << classInfos.size() << "classes in module";
    for (int i = 0; i < (int)classInfos.size(); ++i) {
        qDebug() << "  Class" << i << ":" << QString::fromStdString(classInfos[i].name())
                 << "category:" << QString::fromStdString(classInfos[i].category())
                 << "subcats:" << QString::fromStdString(classInfos[i].subCategoriesString());
    }

    if (classInfos.empty()) {
        qWarning() << "VST3: No classes found in module";
        return false;
    }

    // Select the audio class to load.
    // If classIndex was specified (>= 0), use it directly.
    // Otherwise, auto-select: prefer Fx over Instrument.
    int selectedClass = classIndex;
    if (selectedClass < 0 || selectedClass >= (int)classInfos.size()) {
        selectedClass = -1;
        int fallbackIndex = -1;
        for (int i = 0; i < (int)classInfos.size(); ++i) {
            if (classInfos[i].category() == kVstAudioEffectClass) {
                auto sub = classInfos[i].subCategoriesString();
                if (sub.find("Fx") != std::string::npos) {
                    selectedClass = i;  // Fx class — preferred for audio processing
                    break;
                }
                if (fallbackIndex < 0)
                    fallbackIndex = i;  // first non-Fx audio class
            }
        }
        if (selectedClass < 0) selectedClass = fallbackIndex;
        if (selectedClass < 0) {
            selectedClass = 0;
            qDebug() << "VST3: No audio effect class found, using first class";
        }
    } else {
        qDebug() << "VST3: Using specified classIndex:" << selectedClass;
    }

    auto& info = classInfos[selectedClass];
    m_pluginName = info.name();
    m_pluginVendor = info.vendor();
    m_pluginUID = info.ID().toString();

    auto subCats = info.subCategoriesString();
    m_isEffect = (subCats.find("Instrument") == std::string::npos);

    qDebug() << "VST3: Using plugin:" << QString::fromStdString(m_pluginName)
             << "vendor:" << QString::fromStdString(m_pluginVendor)
             << "uid:" << QString::fromStdString(m_pluginUID);

    // 3. Create IComponent
    m_component = factory.createInstance<IComponent>(info.ID());
    if (!m_component) {
        qWarning() << "VST3: Failed to create IComponent";
        return false;
    }
    qDebug() << "VST3: IComponent created";

    // 4. Initialize the component
    if (!initializeComponent()) {
        qWarning() << "VST3: Failed to initialize component";
        m_component = nullptr;
        return false;
    }
    qDebug() << "VST3: Component initialized";

    // 5. Query IAudioProcessor from the component
    m_processor = FUnknownPtr<IAudioProcessor>(m_component);
    if (!m_processor) {
        qWarning() << "VST3: Component does not implement IAudioProcessor";
        m_component->terminate();
        m_component = nullptr;
        return false;
    }
    qDebug() << "VST3: IAudioProcessor acquired";

    // 6. Get the edit controller
    // First try: query it from the component itself (single-component design)
    m_controller = FUnknownPtr<IEditController>(m_component);
    m_separateController = false;

    if (m_controller) {
        qDebug() << "VST3: Controller is same object as component (single-component)";
    } else {
        // Separate controller: get its class ID and create it
        TUID controllerCID;
        tresult getCIDResult = m_component->getControllerClassId(controllerCID);
        qDebug() << "VST3: getControllerClassId result:" << getCIDResult;

        if (getCIDResult == kResultTrue) {
            VST3::UID controllerUID(controllerCID);
            qDebug() << "VST3: Controller class UID:" << QString::fromStdString(controllerUID.toString());

            m_controller = factory.createInstance<IEditController>(controllerUID);
            if (m_controller) {
                m_separateController = true;
                qDebug() << "VST3: Separate controller created";
            } else {
                qDebug() << "VST3: Failed to create separate controller from UID";
            }
        }
    }

    // 7. Initialize the controller (if separate)
    if (m_controller && m_separateController) {
        if (!initializeController()) {
            qWarning() << "VST3: Failed to initialize controller";
            m_controller = nullptr;
            m_separateController = false;
        }
    }

    // 8. Set component handler on controller
    if (m_controller) {
        tresult handlerResult = m_controller->setComponentHandler(&m_componentHandler);
        qDebug() << "VST3: setComponentHandler result:" << handlerResult;
    }

    // 9. Connect component <-> controller
    if (m_separateController) {
        connectComponents();
    }

    // 10. Sync component state to controller (REQUIRED by VST3 spec)
    // Many plugins refuse to process or show GUI without this.
    if (m_controller) {
        MemoryStream stream;
        tresult getStateResult = m_component->getState(&stream);
        if (getStateResult == kResultOk) {
            stream.seek(0, IBStream::kIBSeekSet, nullptr);
            tresult syncResult = m_controller->setComponentState(&stream);
            qDebug() << "VST3: State sync component→controller:" << syncResult;
        } else {
            qDebug() << "VST3: getState returned" << getStateResult << "(no state to sync)";
        }
    }

    if (m_controller) {
        qDebug() << "VST3: Edit controller ready";
    } else {
        qDebug() << "VST3: No edit controller available (no GUI)";
    }

    // 11. Configure and activate busses (BEFORE setupProcessing per VST3 spec)
    activateBusses();

    // 12. Setup processing with default parameters
    if (!setupProcessing(m_sampleRate, m_maxBlockSize)) {
        qWarning() << "VST3: setupProcessing failed";
    }

    // 13. Activate the component
    m_component->setActive(true);

    // 14. Start processing
    m_processor->setProcessing(true);
    m_processing = true;

    m_loaded = true;
    qDebug() << "=== VST3Plugin::loadFromPath OK ===" << QString::fromStdString(m_pluginName);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Unload
// ═══════════════════════════════════════════════════════════════════════

void VST3Plugin::unload()
{
    closeEditor();

    if (m_processing && m_processor) {
        m_processor->setProcessing(false);
        m_processing = false;
    }

    if (m_componentInitialized && m_component) {
        m_component->setActive(false);
    }

    disconnectComponents();

    if (m_controller && m_separateController && m_controllerInitialized) {
        m_controller->terminate();
        m_controllerInitialized = false;
    }
    m_controller = nullptr;

    if (m_componentInitialized && m_component) {
        m_component->terminate();
        m_componentInitialized = false;
    }

    m_processor = nullptr;
    m_component = nullptr;
    m_module = nullptr;
    m_loaded = false;
    m_separateController = false;

    m_inputChannelBuffers.clear();
    m_outputChannelBuffers.clear();
    m_inputPtrs.clear();
    m_outputPtrs.clear();
}

// ═══════════════════════════════════════════════════════════════════════
//  Initialize Component
// ═══════════════════════════════════════════════════════════════════════

bool VST3Plugin::initializeComponent()
{
    if (!m_component) return false;

    tresult result = m_component->initialize(&g_hostApp);
    qDebug() << "VST3: Component initialize result:" << result;

    if (result != kResultOk && result != kNotImplemented) {
        return false;
    }
    m_componentInitialized = true;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Initialize Controller (separate controller only)
// ═══════════════════════════════════════════════════════════════════════

bool VST3Plugin::initializeController()
{
    if (!m_controller) return false;

    tresult result = m_controller->initialize(&g_hostApp);
    qDebug() << "VST3: Controller initialize result:" << result;

    if (result != kResultOk && result != kNotImplemented) {
        return false;
    }
    m_controllerInitialized = true;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Connect Component <-> Controller via IConnectionPoint
// ═══════════════════════════════════════════════════════════════════════

bool VST3Plugin::connectComponents()
{
    if (!m_component || !m_controller) return false;

    m_componentCP = FUnknownPtr<IConnectionPoint>(m_component);
    m_controllerCP = FUnknownPtr<IConnectionPoint>(m_controller);

    if (m_componentCP && m_controllerCP) {
        m_componentCP->connect(m_controllerCP);
        m_controllerCP->connect(m_componentCP);
        qDebug() << "VST3: Component <-> Controller connected via IConnectionPoint";
        return true;
    } else {
        qDebug() << "VST3: IConnectionPoint not supported"
                 << "(component:" << (m_componentCP != nullptr)
                 << "controller:" << (m_controllerCP != nullptr) << ")";
    }
    return false;
}

void VST3Plugin::disconnectComponents()
{
    if (m_componentCP && m_controllerCP) {
        m_componentCP->disconnect(m_controllerCP);
        m_controllerCP->disconnect(m_componentCP);
    }
    m_componentCP = nullptr;
    m_controllerCP = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
//  Setup Processing
// ═══════════════════════════════════════════════════════════════════════

bool VST3Plugin::setupProcessing(double sampleRate, int maxBlockSize)
{
    if (!m_processor) return false;

    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;

    tresult result = m_processor->setupProcessing(setup);
    if (result != kResultOk && result != kNotImplemented) {
        qWarning() << "VST3: setupProcessing returned" << result;
        return false;
    }

    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;

    // Allocate non-interleaved buffers
    m_inputChannelBuffers.resize(m_channels);
    m_outputChannelBuffers.resize(m_channels);
    m_inputPtrs.resize(m_channels);
    m_outputPtrs.resize(m_channels);

    for (int ch = 0; ch < m_channels; ++ch) {
        m_inputChannelBuffers[ch].resize(maxBlockSize, 0.0f);
        m_outputChannelBuffers[ch].resize(maxBlockSize, 0.0f);
        m_inputPtrs[ch] = m_inputChannelBuffers[ch].data();
        m_outputPtrs[ch] = m_outputChannelBuffers[ch].data();
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Activate Busses
// ═══════════════════════════════════════════════════════════════════════

bool VST3Plugin::activateBusses()
{
    if (!m_component) return false;

    int32 numAudioIn  = m_component->getBusCount(kAudio, kInput);
    int32 numAudioOut = m_component->getBusCount(kAudio, kOutput);
    int32 numEventIn  = m_component->getBusCount(kEvent, kInput);
    int32 numEventOut = m_component->getBusCount(kEvent, kOutput);
    qDebug() << "VST3: Bus counts — audioIn:" << numAudioIn
             << "audioOut:" << numAudioOut
             << "eventIn:" << numEventIn
             << "eventOut:" << numEventOut;

    // ── 1. Negotiate speaker arrangement ─────────────────────────────
    if (m_processor) {
        // Build arrangement arrays matching the plugin's actual bus count
        std::vector<SpeakerArrangement> inArr(numAudioIn, SpeakerArr::kStereo);
        std::vector<SpeakerArrangement> outArr(numAudioOut, SpeakerArr::kStereo);

        // Plugins with 0 audio inputs (instruments) — just set output arrangement
        tresult arrResult = m_processor->setBusArrangements(
            inArr.empty() ? nullptr : inArr.data(), numAudioIn,
            outArr.empty() ? nullptr : outArr.data(), numAudioOut);
        qDebug() << "VST3: setBusArrangements(stereo) result:" << arrResult;

        if (arrResult != kResultOk && arrResult != kNotImplemented) {
            // Plugin rejected our arrangement — query what it actually wants
            qDebug() << "VST3: Plugin rejected stereo, querying preferred arrangement";

            for (int32 i = 0; i < numAudioIn; ++i) {
                m_processor->getBusArrangement(kInput, i, inArr[i]);
                qDebug() << "  Input bus" << i << "arrangement:" << inArr[i];
            }
            for (int32 i = 0; i < numAudioOut; ++i) {
                m_processor->getBusArrangement(kOutput, i, outArr[i]);
                qDebug() << "  Output bus" << i << "arrangement:" << outArr[i];
            }

            // Try again with the plugin's preferred arrangement
            tresult retryResult = m_processor->setBusArrangements(
                inArr.empty() ? nullptr : inArr.data(), numAudioIn,
                outArr.empty() ? nullptr : outArr.data(), numAudioOut);
            qDebug() << "VST3: setBusArrangements(plugin-preferred) result:" << retryResult;

            // Update channel count to match what the plugin actually uses
            if (retryResult == kResultOk && numAudioOut > 0) {
                SpeakerArrangement finalOut = 0;
                m_processor->getBusArrangement(kOutput, 0, finalOut);
                int pluginChannels = SpeakerArr::getChannelCount(finalOut);
                if (pluginChannels > 0 && pluginChannels != m_channels) {
                    qDebug() << "VST3: Adapting channel count to" << pluginChannels;
                    m_channels = pluginChannels;
                }
            }
        }
    }

    // ── 2. Activate audio busses ─────────────────────────────────────
    for (int32 i = 0; i < numAudioIn; ++i) {
        m_component->activateBus(kAudio, kInput, i, (i == 0));  // only main bus active
    }
    for (int32 i = 0; i < numAudioOut; ++i) {
        m_component->activateBus(kAudio, kOutput, i, (i == 0)); // only main bus active
    }

    // ── 3. Activate event busses (some Fx plugins accept MIDI) ───────
    for (int32 i = 0; i < numEventIn; ++i) {
        m_component->activateBus(kEvent, kInput, i, true);
    }
    for (int32 i = 0; i < numEventOut; ++i) {
        m_component->activateBus(kEvent, kOutput, i, true);
    }

    qDebug() << "VST3: All busses activated";
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Audio Processing (IDSPProcessor)
// ═══════════════════════════════════════════════════════════════════════

void VST3Plugin::process(float* buf, int frames, int channels)
{
    if (!m_enabled || !m_loaded || !m_processor || !m_processing)
        return;

    std::unique_lock<std::mutex> lock(m_processMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;  // Skip this cycle, don't block audio thread

    // Verify pre-allocated buffers are sufficient (never allocate on audio thread)
    if (channels > (int)m_inputPtrs.size() || frames > m_maxBlockSize) {
        return;  // Buffer mismatch — prepare() must be called first
    }

    // Deinterleave: interleaved buf -> per-channel input buffers
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < channels; ++ch) {
            m_inputChannelBuffers[ch][f] = buf[f * channels + ch];
        }
    }

    // Clear output buffers
    for (int ch = 0; ch < channels; ++ch) {
        std::fill(m_outputChannelBuffers[ch].begin(),
                  m_outputChannelBuffers[ch].begin() + frames, 0.0f);
    }

    // Setup ProcessData
    AudioBusBuffers inputBus;
    inputBus.numChannels = channels;
    inputBus.silenceFlags = 0;
    inputBus.channelBuffers32 = m_inputPtrs.data();

    AudioBusBuffers outputBus;
    outputBus.numChannels = channels;
    outputBus.silenceFlags = 0;
    outputBus.channelBuffers32 = m_outputPtrs.data();

    // Provide ProcessContext — many plugins (e.g. iZotope Ozone, Crave EQ) require it
    ProcessContext processContext{};
    processContext.state = ProcessContext::kPlaying
                         | ProcessContext::kTempoValid
                         | ProcessContext::kTimeSigValid;
    processContext.sampleRate = m_sampleRate;
    processContext.projectTimeSamples = m_transportPos;
    processContext.tempo = 120.0;
    processContext.timeSigNumerator = 4;
    processContext.timeSigDenominator = 4;
    m_transportPos += frames;

    // Drain pending parameter changes from the component handler (GUI edits)
    HostParameterChanges inputParamChanges;
    ComponentHandlerAdapter::ParamChange changes[ComponentHandlerAdapter::MAX_CHANGES];
    int nChanges = m_componentHandler.drainChanges(changes, ComponentHandlerAdapter::MAX_CHANGES);
    for (int i = 0; i < nChanges; ++i) {
        inputParamChanges.add(changes[i].id, changes[i].value);
    }

    ProcessData data;
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inputBus;
    data.outputs = &outputBus;
    data.inputParameterChanges = (inputParamChanges.m_count > 0) ? &inputParamChanges : nullptr;
    data.outputParameterChanges = nullptr;
    data.inputEvents = nullptr;
    data.outputEvents = nullptr;
    data.processContext = &processContext;

    tresult result = m_processor->process(data);
    if (result != kResultOk) {
        return;
    }

    // Re-interleave: per-channel output buffers -> interleaved buf
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < channels; ++ch) {
            buf[f * channels + ch] = m_outputChannelBuffers[ch][f];
        }
    }
}

void VST3Plugin::prepare(double sampleRate, int channels)
{
    // Skip redundant deactivate/reactivate cycle when settings match.
    // loadFromPath() already activates the plugin; calling prepare() again
    // with the same rate/channels triggers a rapid activate→deactivate→activate
    // transition that some plugins (e.g. hardware-interfacing ones) can't handle,
    // causing the main thread to hang in setActive(true).
    if (m_loaded && m_processor &&
        m_sampleRate == sampleRate && m_channels == channels) {
        return;
    }

    m_sampleRate = sampleRate;
    m_channels = channels;

    if (m_loaded && m_processor) {
        // Deactivate for reconfiguration (VST3 spec requires this order)
        if (m_processing) {
            m_processor->setProcessing(false);
        }
        if (m_component) {
            m_component->setActive(false);
        }

        // Re-negotiate bus arrangements then setup processing
        activateBusses();
        setupProcessing(sampleRate, m_maxBlockSize);

        if (m_component) {
            m_component->setActive(true);
        }
        if (m_processing) {
            m_processor->setProcessing(true);
        }
    }
}

void VST3Plugin::reset()
{
    if (!m_loaded || !m_processor) return;
    m_transportPos = 0;

    if (m_processing) {
        m_processor->setProcessing(false);
        m_processor->setProcessing(true);
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Editor Support
// ═══════════════════════════════════════════════════════════════════════

bool VST3Plugin::hasEditor() const
{
    if (!m_controller) {
        qDebug() << "VST3: hasEditor() - no controller";
        return false;
    }

    auto* view = m_controller->createView(ViewType::kEditor);
    if (view) {
        // Also check platform support
        tresult supported = view->isPlatformTypeSupported(kPlatformTypeNSView);
        view->release();
        bool has = (supported == kResultTrue);
        qDebug() << "VST3: hasEditor() -" << QString::fromStdString(m_pluginName)
                 << "createView OK, NSView supported:" << has;
        return has;
    }

    qDebug() << "VST3: hasEditor() -" << QString::fromStdString(m_pluginName)
             << "createView returned null";
    return false;
}

QWidget* VST3Plugin::openEditor(QWidget* parent)
{
    qDebug() << "=== VST3Plugin::openEditor START ===" << QString::fromStdString(m_pluginName);

    // If editor is already open, just raise it
    if (m_editorWindow) {
        qDebug() << "VST3: Editor already open, raising window";
        m_editorWindow->raise();
        m_editorWindow->activateWindow();
        return m_editorWindow;
    }

    if (!m_controller) {
        qDebug() << "VST3: No controller, showing placeholder";
        showPlaceholderEditor(parent);
        return m_editorWindow;
    }

    // Create the IPlugView
    qDebug() << "VST3: Calling createView(kEditor)...";
    IPlugView* rawView = m_controller->createView(ViewType::kEditor);
    qDebug() << "VST3: createView result:" << (rawView != nullptr);

    if (!rawView) {
        qDebug() << "VST3: createView returned null, showing placeholder";
        showPlaceholderEditor(parent);
        return m_editorWindow;
    }

    m_plugView = owned(rawView);

    // Check platform support (NSView on macOS)
    tresult nsViewSupport = m_plugView->isPlatformTypeSupported(kPlatformTypeNSView);
    qDebug() << "VST3: NSView supported:" << nsViewSupport
             << "(kResultTrue =" << kResultTrue << ")";

    if (nsViewSupport != kResultTrue) {
        qWarning() << "VST3: Plugin does not support NSView platform type";
        m_plugView = nullptr;
        showPlaceholderEditor(parent);
        return m_editorWindow;
    }

    // Get the editor size
    ViewRect rect;
    tresult sizeResult = m_plugView->getSize(&rect);
    qDebug() << "VST3: getSize result:" << sizeResult;

    int editorWidth = 800;
    int editorHeight = 600;

    if (sizeResult == kResultTrue) {
        editorWidth = rect.getWidth();
        editorHeight = rect.getHeight();
        qDebug() << "VST3: Plugin editor size:" << editorWidth << "x" << editorHeight;
    } else {
        qDebug() << "VST3: Using default editor size";
    }

    if (editorWidth <= 0) editorWidth = 800;
    if (editorHeight <= 0) editorHeight = 600;

    // Create a Qt window to host the native view
    auto* window = new QWidget(parent, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setAttribute(Qt::WA_NativeWindow);
    window->setWindowTitle(QString::fromStdString(m_pluginName));
    window->setFixedSize(editorWidth, editorHeight);

    // Set IPlugFrame for resize callbacks
    m_plugFrame = std::make_unique<PlugFrameAdapter>(window);
    m_plugView->setFrame(m_plugFrame.get());

    // Show and process events to ensure native window handle is created
    window->show();
    QApplication::processEvents();

    // QWidget::winId() on macOS returns an NSView* cast to WId
    WId wid = window->winId();
    void* nativeView = reinterpret_cast<void*>(wid);
    qDebug() << "VST3: Native view handle:" << nativeView;

    if (!nativeView) {
        qWarning() << "VST3: Failed to get native view from QWidget";
        m_plugView->setFrame(nullptr);
        m_plugFrame.reset();
        m_plugView = nullptr;
        delete window;
        showPlaceholderEditor(parent);
        return m_editorWindow;
    }

    // Attach the plugin view to our native view
    qDebug() << "VST3: Calling attached(nativeView, kPlatformTypeNSView)...";
    tresult attachResult = m_plugView->attached(nativeView, kPlatformTypeNSView);
    qDebug() << "VST3: attached() result:" << attachResult;

    if (attachResult != kResultTrue) {
        qWarning() << "VST3: IPlugView::attached() failed:" << attachResult;
        m_plugView->setFrame(nullptr);
        m_plugFrame.reset();
        m_plugView = nullptr;
        delete window;
        showPlaceholderEditor(parent);
        return m_editorWindow;
    }

    m_editorWindow = window;

    // When window closes, detach the plug view
    QObject::connect(window, &QObject::destroyed, [this]() {
        qDebug() << "VST3: Editor window destroyed, cleaning up";
        if (m_plugView) {
            m_plugView->setFrame(nullptr);
            m_plugView->removed();
            m_plugView = nullptr;
        }
        m_plugFrame.reset();
        m_editorWindow = nullptr;
    });

    qDebug() << "=== VST3Plugin::openEditor SUCCESS ==="
             << QString::fromStdString(m_pluginName)
             << editorWidth << "x" << editorHeight;

    window->raise();
    window->activateWindow();
    return window;
}

void VST3Plugin::closeEditor()
{
    if (m_plugView) {
        m_plugView->setFrame(nullptr);
        m_plugView->removed();
        m_plugView = nullptr;
    }
    m_plugFrame.reset();
    // Don't delete m_editorWindow — Qt manages it via WA_DeleteOnClose
    m_editorWindow = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
//  Placeholder Editor (when native GUI not available)
// ═══════════════════════════════════════════════════════════════════════

void VST3Plugin::showPlaceholderEditor(QWidget* parent)
{
    qDebug() << "VST3: Showing placeholder editor for:" << QString::fromStdString(m_pluginName);

    auto* window = new QWidget(parent, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QString::fromStdString(m_pluginName) + " (No GUI)");
    window->resize(400, 220);

    auto* layout = new QVBoxLayout(window);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(12);
    layout->setContentsMargins(24, 24, 24, 24);

    auto* title = new QLabel(QString::fromStdString(m_pluginName));
    title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    if (!m_pluginVendor.empty()) {
        auto* vendor = new QLabel(QString::fromStdString(m_pluginVendor));
        vendor->setStyleSheet(QStringLiteral("font-size: 13px; color: #999;"));
        vendor->setAlignment(Qt::AlignCenter);
        layout->addWidget(vendor);
    }

    auto* info = new QLabel(QStringLiteral(
        "Native VST3 GUI not available.\nPlugin is active in audio chain."));
    info->setAlignment(Qt::AlignCenter);
    info->setStyleSheet(QStringLiteral("color: gray; font-size: 12px;"));
    layout->addWidget(info);

    auto* bypass = new QCheckBox(QStringLiteral("Bypass"));
    bypass->setChecked(!m_enabled);
    QObject::connect(bypass, &QCheckBox::toggled, [this](bool b) { setEnabled(!b); });
    layout->addWidget(bypass, 0, Qt::AlignCenter);

    layout->addStretch();

    m_editorWindow = window;
    QObject::connect(window, &QObject::destroyed, [this]() { m_editorWindow = nullptr; });

    window->show();
    window->raise();
    window->activateWindow();
}

// ═══════════════════════════════════════════════════════════════════════
//  State Persistence
// ═══════════════════════════════════════════════════════════════════════

QByteArray VST3Plugin::saveState() const
{
    if (!m_loaded || !m_component) return {};

    // Save component state
    MemoryStream componentStream;
    tresult compResult = m_component->getState(&componentStream);
    if (compResult != kResultOk) {
        qWarning() << "[VST3] Failed to get component state for"
                   << QString::fromStdString(m_pluginName) << "result:" << compResult;
        return {};
    }

    // Save controller state (if available)
    MemoryStream controllerStream;
    if (m_controller) {
        m_controller->getState(&controllerStream);
        // Failure OK — some plugins don't support separate controller state
    }

    // Pack both into single QByteArray via QDataStream
    QByteArray data;
    QDataStream ds(&data, QIODevice::WriteOnly);
    ds << QByteArray(reinterpret_cast<const char*>(componentStream.getData()),
                     static_cast<int>(componentStream.getSize()));
    ds << QByteArray(reinterpret_cast<const char*>(controllerStream.getData()),
                     static_cast<int>(controllerStream.getSize()));

    qDebug() << "[VST3] Saved state for" << QString::fromStdString(m_pluginName)
             << "(" << data.size() << "bytes, component:"
             << componentStream.getSize() << "controller:"
             << controllerStream.getSize() << ")";
    return data;
}

bool VST3Plugin::restoreState(const QByteArray& data)
{
    if (!m_loaded || !m_component || data.isEmpty()) return false;

    QDataStream ds(data);
    QByteArray componentData, controllerData;
    ds >> componentData >> controllerData;

    if (componentData.isEmpty()) {
        qWarning() << "[VST3] Empty component state for"
                   << QString::fromStdString(m_pluginName);
        return false;
    }

    // Restore component state
    MemoryStream componentStream(componentData.constData(),
                                  static_cast<size_t>(componentData.size()));
    tresult compResult = m_component->setState(&componentStream);
    if (compResult != kResultOk) {
        qWarning() << "[VST3] Failed to restore component state for"
                   << QString::fromStdString(m_pluginName) << "result:" << compResult;
        return false;
    }

    // Restore controller state (if available)
    if (m_controller && !controllerData.isEmpty()) {
        MemoryStream controllerStream(controllerData.constData(),
                                       static_cast<size_t>(controllerData.size()));
        m_controller->setState(&controllerStream);
    }

    // Sync controller with restored component state
    if (m_controller) {
        MemoryStream syncStream;
        if (m_component->getState(&syncStream) == kResultOk) {
            syncStream.seek(0, IBStream::kIBSeekSet, nullptr);
            m_controller->setComponentState(&syncStream);
        }
    }

    qDebug() << "[VST3] Restored state for" << QString::fromStdString(m_pluginName)
             << "(" << data.size() << "bytes)";
    return true;
}
