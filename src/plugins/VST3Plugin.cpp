#include "VST3Plugin.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <QWidget>
#include <QWindow>
#include <QVBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QApplication>
#include <QDebug>

using namespace Steinberg;
using namespace Steinberg::Vst;

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

bool VST3Plugin::loadFromPath(const std::string& vst3Path)
{
    qDebug() << "=== VST3Plugin::loadFromPath ===" << QString::fromStdString(vst3Path);

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

    // Find the best Audio Module Class — prefer Fx over Instrument
    int classIndex = -1;
    int fallbackIndex = -1;
    for (int i = 0; i < (int)classInfos.size(); ++i) {
        if (classInfos[i].category() == kVstAudioEffectClass) {
            auto sub = classInfos[i].subCategoriesString();
            if (sub.find("Fx") != std::string::npos) {
                classIndex = i;  // Fx class — preferred for audio processing
                break;
            }
            if (fallbackIndex < 0)
                fallbackIndex = i;  // first non-Fx audio class
        }
    }
    if (classIndex < 0) classIndex = fallbackIndex;
    if (classIndex < 0) {
        classIndex = 0;
        qDebug() << "VST3: No audio effect class found, using first class";
    }

    auto& info = classInfos[classIndex];
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

    // 10. Sync component state to controller
    if (m_controller && m_separateController) {
        // Get component state and pass to controller
        // This is important for the controller to know about the component's state
        // Many plugins require this before createView() works
        qDebug() << "VST3: Syncing component state to controller";
        // Note: Full state sync requires IBStream implementation which is complex.
        // The PlugProvider class handles this, but for now we rely on the connection.
    }

    if (m_controller) {
        qDebug() << "VST3: Edit controller ready";
    } else {
        qDebug() << "VST3: No edit controller available (no GUI)";
    }

    // 11. Setup processing with default parameters
    if (!setupProcessing(m_sampleRate, m_maxBlockSize)) {
        qWarning() << "VST3: setupProcessing failed";
    }

    // 12. Activate busses
    activateBusses();

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

    static Steinberg::Vst::HostApplication hostApp;
    tresult result = m_component->initialize(&hostApp);
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

    static Steinberg::Vst::HostApplication hostApp;
    tresult result = m_controller->initialize(&hostApp);
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

    // Set stereo speaker arrangement BEFORE activating busses.
    // Without this, some plugins default to mono and only process the left channel.
    if (m_processor) {
        SpeakerArrangement stereo = SpeakerArr::kStereo;
        tresult arrResult = m_processor->setBusArrangements(&stereo, 1, &stereo, 1);
        qDebug() << "VST3: setBusArrangements(stereo) result:" << arrResult;
        if (arrResult != kResultOk && arrResult != kNotImplemented) {
            qDebug() << "VST3: Plugin did not accept stereo arrangement, querying current";
            SpeakerArrangement currentIn = 0, currentOut = 0;
            m_processor->getBusArrangement(kOutput, 0, currentOut);
            m_processor->getBusArrangement(kInput, 0, currentIn);
            qDebug() << "VST3: Current arrangement — in:" << currentIn << "out:" << currentOut;
        }
    }

    int32 numInputBusses = m_component->getBusCount(kAudio, kInput);
    if (numInputBusses > 0) {
        m_component->activateBus(kAudio, kInput, 0, true);
    }

    int32 numOutputBusses = m_component->getBusCount(kAudio, kOutput);
    if (numOutputBusses > 0) {
        m_component->activateBus(kAudio, kOutput, 0, true);
    }

    qDebug() << "VST3: Busses activated (in:" << numInputBusses << "out:" << numOutputBusses << ")";
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

    // Ensure buffers are large enough
    if (channels != m_channels || frames > m_maxBlockSize) {
        m_channels = channels;
        if (frames > m_maxBlockSize) m_maxBlockSize = frames;

        m_inputChannelBuffers.resize(channels);
        m_outputChannelBuffers.resize(channels);
        m_inputPtrs.resize(channels);
        m_outputPtrs.resize(channels);

        for (int ch = 0; ch < channels; ++ch) {
            m_inputChannelBuffers[ch].resize(m_maxBlockSize, 0.0f);
            m_outputChannelBuffers[ch].resize(m_maxBlockSize, 0.0f);
            m_inputPtrs[ch] = m_inputChannelBuffers[ch].data();
            m_outputPtrs[ch] = m_outputChannelBuffers[ch].data();
        }
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

    // Provide ProcessContext — many plugins (e.g. iZotope Ozone) require it
    ProcessContext processContext{};
    processContext.state = ProcessContext::kPlaying;
    processContext.sampleRate = m_sampleRate;
    processContext.projectTimeSamples = m_transportPos;
    m_transportPos += frames;

    ProcessData data;
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inputBus;
    data.outputs = &outputBus;
    data.inputParameterChanges = nullptr;
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
    m_sampleRate = sampleRate;
    m_channels = channels;

    if (m_loaded && m_processor) {
        if (m_processing) {
            m_processor->setProcessing(false);
        }
        if (m_component) {
            m_component->setActive(false);
        }

        setupProcessing(sampleRate, m_maxBlockSize);

        // Re-apply stereo arrangement after re-setup
        SpeakerArrangement stereo = SpeakerArr::kStereo;
        m_processor->setBusArrangements(&stereo, 1, &stereo, 1);

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
