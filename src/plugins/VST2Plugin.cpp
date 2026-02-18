#include "VST2Plugin.h"
#include <vst.h>

#include <QDebug>
#include <QWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QApplication>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════

VST2Plugin::VST2Plugin() = default;

VST2Plugin::~VST2Plugin()
{
    unload();
}

// ═══════════════════════════════════════════════════════════════════════
//  Host Callback — the plugin calls us here
// ═══════════════════════════════════════════════════════════════════════

// Match the Xaymar vst_host_callback_t signature exactly:
//   intptr_t (plugin, opcode, p_int1, p_int2, p_str, p_float)
intptr_t VST2Plugin::hostCallbackStatic(
    vst_effect_t* /*effect*/, int32_t opcode,
    int32_t /*p_int1*/, intptr_t /*p_int2*/,
    void* /*p_ptr*/, float /*p_float*/)
{
    switch (opcode) {
    case VST_HOST_OPCODE_VST_VERSION:
        return VST_VERSION_2_4_0_0;
    case VST_HOST_OPCODE_CURRENT_EFFECT_ID:
        return 0;
    default:
        return 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Dispatcher helper
// ═══════════════════════════════════════════════════════════════════════

intptr_t VST2Plugin::dispatcher(int32_t opcode, int32_t index,
                                 intptr_t value, void* ptr, float opt)
{
    if (!m_effect || !m_effect->control)
        return 0;
    return m_effect->control(m_effect, opcode, index, value, ptr, opt);
}

// ═══════════════════════════════════════════════════════════════════════
//  Load a .vst bundle via CFBundle
// ═══════════════════════════════════════════════════════════════════════

bool VST2Plugin::loadFromPath(const std::string& vstPath)
{
    qDebug() << "=== VST2Plugin::loadFromPath ===" << QString::fromStdString(vstPath);

    if (m_effect) unload();

    m_path = vstPath;

    // 1. Create CFBundle from path
    CFStringRef pathStr = CFStringCreateWithCString(
        kCFAllocatorDefault, vstPath.c_str(), kCFStringEncodingUTF8);
    if (!pathStr) {
        qWarning() << "VST2: Failed to create CFString for path";
        return false;
    }

    CFURLRef bundleURL = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault, pathStr, kCFURLPOSIXPathStyle, true);
    CFRelease(pathStr);

    if (!bundleURL) {
        qWarning() << "VST2: Failed to create URL for" << QString::fromStdString(vstPath);
        return false;
    }

    m_bundle = CFBundleCreate(kCFAllocatorDefault, bundleURL);
    CFRelease(bundleURL);

    if (!m_bundle) {
        qWarning() << "VST2: Failed to create bundle";
        return false;
    }

    if (!CFBundleLoadExecutable(m_bundle)) {
        qWarning() << "VST2: Failed to load bundle executable";
        CFRelease(m_bundle);
        m_bundle = nullptr;
        return false;
    }

    // 2. Find entry point — try multiple known names
    typedef vst_effect_t* (*EntryFunc)(vst_host_callback_t);
    EntryFunc entryPoint = nullptr;

    const char* entryNames[] = { "VSTPluginMain", "main_macho", "main" };
    for (const char* name : entryNames) {
        CFStringRef nameStr = CFStringCreateWithCString(
            kCFAllocatorDefault, name, kCFStringEncodingASCII);
        void* sym = CFBundleGetFunctionPointerForName(m_bundle, nameStr);
        CFRelease(nameStr);

        if (sym) {
            entryPoint = reinterpret_cast<EntryFunc>(sym);
            qDebug() << "VST2: Found entry point:" << name;
            break;
        }
    }

    if (!entryPoint) {
        qWarning() << "VST2: No entry point found (tried VSTPluginMain, main_macho, main)";
        CFBundleUnloadExecutable(m_bundle);
        CFRelease(m_bundle);
        m_bundle = nullptr;
        return false;
    }

    // 3. Instantiate the plugin
    m_effect = entryPoint(reinterpret_cast<vst_host_callback_t>(hostCallbackStatic));

    if (!m_effect) {
        qWarning() << "VST2: Entry point returned null";
        CFBundleUnloadExecutable(m_bundle);
        CFRelease(m_bundle);
        m_bundle = nullptr;
        return false;
    }

    // 4. Verify magic number
    if (m_effect->magic_number != VST_MAGICNUMBER) {
        qWarning() << "VST2: Bad magic number:" << m_effect->magic_number;
        m_effect = nullptr;
        CFBundleUnloadExecutable(m_bundle);
        CFRelease(m_bundle);
        m_bundle = nullptr;
        return false;
    }

    // 5. Initialize (effOpen)
    dispatcher(VST_EFFECT_OPCODE_CREATE);

    // 6. Read plugin name
    char nameBuf[VST_BUFFER_SIZE_EFFECT_NAME + 1] = {};
    dispatcher(VST_EFFECT_OPCODE_EFFECT_NAME, 0, 0, nameBuf);
    m_name = nameBuf;

    // Fallback: try product name
    if (m_name.empty()) {
        char prodBuf[VST_BUFFER_SIZE_PRODUCT_NAME + 1] = {};
        dispatcher(VST_EFFECT_OPCODE_PRODUCT_NAME, 0, 0, prodBuf);
        m_name = prodBuf;
    }

    // Fallback: use filename
    if (m_name.empty()) {
        auto pos = vstPath.rfind('/');
        auto dotPos = vstPath.rfind('.');
        if (pos != std::string::npos && dotPos != std::string::npos && dotPos > pos)
            m_name = vstPath.substr(pos + 1, dotPos - pos - 1);
        else if (pos != std::string::npos)
            m_name = vstPath.substr(pos + 1);
        else
            m_name = vstPath;
    }

    // 7. Read vendor name
    char vendorBuf[VST_BUFFER_SIZE_VENDOR_NAME + 1] = {};
    dispatcher(VST_EFFECT_OPCODE_VENDOR_NAME, 0, 0, vendorBuf);
    m_vendor = vendorBuf;

    // 8. Configure audio
    dispatcher(VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr,
               static_cast<float>(m_sampleRate));
    dispatcher(VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0,
               static_cast<intptr_t>(m_blockSize));

    // 9. Resume (start processing)
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1);

    // 10. Pre-allocate planar buffers
    int ch = std::max(m_effect->num_inputs, m_effect->num_outputs);
    if (ch < 2) ch = 2;
    m_channels = ch;
    m_inBuffers.resize(ch);
    m_outBuffers.resize(ch);
    m_inPtrs.resize(ch);
    m_outPtrs.resize(ch);
    for (int c = 0; c < ch; ++c) {
        m_inBuffers[c].resize(m_blockSize, 0.0f);
        m_outBuffers[c].resize(m_blockSize, 0.0f);
        m_inPtrs[c] = m_inBuffers[c].data();
        m_outPtrs[c] = m_outBuffers[c].data();
    }

    qDebug() << "VST2: Loaded OK —" << QString::fromStdString(m_name)
             << "vendor:" << QString::fromStdString(m_vendor)
             << "inputs:" << m_effect->num_inputs
             << "outputs:" << m_effect->num_outputs
             << "params:" << m_effect->num_params
             << "hasEditor:" << hasEditor();

    if (m_effect->num_inputs == 0) {
        qWarning() << "VST2: Rejecting instrument plugin (0 audio inputs):"
                    << QString::fromStdString(m_name);
        unload();
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Unload
// ═══════════════════════════════════════════════════════════════════════

void VST2Plugin::unload()
{
    closeEditor();

    if (!m_effect) return;

    qDebug() << "VST2: Unloading" << QString::fromStdString(m_name);

    // Suspend
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0);

    // Destroy
    dispatcher(VST_EFFECT_OPCODE_DESTROY);
    m_effect = nullptr;

    if (m_bundle) {
        CFBundleUnloadExecutable(m_bundle);
        CFRelease(m_bundle);
        m_bundle = nullptr;
    }

    m_name.clear();
    m_vendor.clear();
    m_inBuffers.clear();
    m_outBuffers.clear();
    m_inPtrs.clear();
    m_outPtrs.clear();
}

// ═══════════════════════════════════════════════════════════════════════
//  Audio Processing (IDSPProcessor)
// ═══════════════════════════════════════════════════════════════════════

void VST2Plugin::process(float* buf, int frames, int channels)
{
    if (!m_enabled || !m_effect || !m_effect->process_float)
        return;

    // Instruments (synths) have 0 audio inputs — they generate audio from MIDI.
    // Without a MIDI source, their output is silence and would overwrite the
    // audio buffer.  Bypass to preserve the signal.
    if (m_effect->num_inputs == 0) return;

    std::unique_lock<std::mutex> lock(m_processMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;  // Skip this cycle, don't block audio thread

    // One-time diagnostic — capture input peak before processing
    static bool s_vst2Diag = false;
    bool doLog = !s_vst2Diag;
    float inputPeak = 0;
    if (doLog) {
        int n = std::min(frames * channels, 1024);
        for (int i = 0; i < n; ++i)
            inputPeak = std::max(inputPeak, std::abs(buf[i]));
    }

    // Ensure buffers are large enough
    if (channels != m_channels || frames > m_blockSize) {
        m_channels = channels;
        if (frames > m_blockSize) m_blockSize = frames;

        m_inBuffers.resize(channels);
        m_outBuffers.resize(channels);
        m_inPtrs.resize(channels);
        m_outPtrs.resize(channels);

        for (int ch = 0; ch < channels; ++ch) {
            m_inBuffers[ch].resize(m_blockSize, 0.0f);
            m_outBuffers[ch].resize(m_blockSize, 0.0f);
            m_inPtrs[ch] = m_inBuffers[ch].data();
            m_outPtrs[ch] = m_outBuffers[ch].data();
        }
    }

    // Deinterleave: interleaved buf -> per-channel input buffers
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < channels; ++ch) {
            m_inBuffers[ch][f] = buf[f * channels + ch];
        }
    }

    // Clear output buffers
    for (int ch = 0; ch < channels; ++ch) {
        std::fill(m_outBuffers[ch].begin(),
                  m_outBuffers[ch].begin() + frames, 0.0f);
    }

    // Process — VST2 processReplacing with separate in/out planar buffers
    m_effect->process_float(m_effect,
                            m_inPtrs.data(),
                            m_outPtrs.data(),
                            frames);

    // Re-interleave: per-channel output buffers -> interleaved buf
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < channels; ++ch) {
            buf[f * channels + ch] = m_outBuffers[ch][f];
        }
    }

    // One-time diagnostic — capture output peak and log comparison
    if (doLog) {
        float outputPeak = 0;
        int n = std::min(frames * channels, 1024);
        for (int i = 0; i < n; ++i)
            outputPeak = std::max(outputPeak, std::abs(buf[i]));
        qDebug() << "[VST2 DIAG]" << QString::fromStdString(m_name)
                 << "in:" << inputPeak << "out:" << outputPeak
                 << "frames:" << frames << "ch:" << channels
                 << "enabled:" << m_enabled;
        s_vst2Diag = true;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Prepare / Reset
// ═══════════════════════════════════════════════════════════════════════

void VST2Plugin::prepare(double sampleRate, int channels)
{
    // Skip redundant suspend/resume when settings match (same fix as VST3Plugin)
    if (m_effect && m_sampleRate == sampleRate && m_channels == channels)
        return;

    m_sampleRate = sampleRate;
    m_channels = channels;

    if (!m_effect) return;

    // Suspend → reconfigure → resume
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0);
    dispatcher(VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr,
               static_cast<float>(sampleRate));
    dispatcher(VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0,
               static_cast<intptr_t>(m_blockSize));
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1);
}

void VST2Plugin::reset()
{
    if (!m_effect) return;

    // Suspend and resume to reset internal state
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0);
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1);
}

// ═══════════════════════════════════════════════════════════════════════
//  Editor
// ═══════════════════════════════════════════════════════════════════════

bool VST2Plugin::hasEditor() const
{
    if (!m_effect) return false;
    return (m_effect->flags & VST_EFFECT_FLAG_EDITOR) != 0;
}

QWidget* VST2Plugin::openEditor(QWidget* parent)
{
    qDebug() << "=== VST2Plugin::openEditor ===" << QString::fromStdString(m_name);

    // If editor is already open, raise it
    if (m_editorWindow) {
        m_editorWindow->raise();
        m_editorWindow->activateWindow();
        return m_editorWindow;
    }

    if (!m_effect || !hasEditor()) {
        qWarning() << "VST2: Plugin has no editor";
        return nullptr;
    }

    // Get editor rect (before open)
    vst_rect_t* rect = nullptr;
    dispatcher(VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0, &rect);

    int width  = (rect && rect->right > rect->left) ? (rect->right - rect->left) : 400;
    int height = (rect && rect->bottom > rect->top) ? (rect->bottom - rect->top) : 300;

    qDebug() << "VST2: Editor rect:" << width << "x" << height;

    // Create Qt window to host the native view
    auto* window = new QWidget(parent, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setAttribute(Qt::WA_NativeWindow);
    window->setWindowTitle(QString::fromStdString(m_name));
    window->setFixedSize(width, height);

    // Show and process events to ensure native window handle is created
    window->show();
    QApplication::processEvents();

    // QWidget::winId() on macOS returns an NSView* cast to WId
    WId wid = window->winId();
    void* nativeView = reinterpret_cast<void*>(wid);

    if (!nativeView) {
        qWarning() << "VST2: Failed to get native view from QWidget";
        delete window;
        return nullptr;
    }

    qDebug() << "VST2: Native view handle:" << nativeView;

    // Open the VST2 editor into our NSView
    dispatcher(VST_EFFECT_OPCODE_EDITOR_OPEN, 0, 0, nativeView);

    // Re-query rect (some plugins update size after open)
    rect = nullptr;
    dispatcher(VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0, &rect);
    if (rect && rect->right > rect->left && rect->bottom > rect->top) {
        int newW = rect->right - rect->left;
        int newH = rect->bottom - rect->top;
        if (newW != width || newH != height) {
            qDebug() << "VST2: Updated editor rect:" << newW << "x" << newH;
            window->setFixedSize(newW, newH);
        }
    }

    m_editorWindow = window;

    // Start idle timer — many VST2 plugins require periodic effEditIdle
    m_idleTimer = new QTimer(window);
    QObject::connect(m_idleTimer, &QTimer::timeout, [this]() {
        if (m_effect)
            dispatcher(VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE);
    });
    m_idleTimer->start(50);  // 20 Hz

    // When window closes externally (X button), null our pointers
    // Do NOT call EDITOR_CLOSE here — closeEditor() handles that
    QObject::connect(window, &QObject::destroyed, [this]() {
        qDebug() << "VST2: Editor window destroyed";
        m_idleTimer = nullptr;   // owned by window, already deleted
        m_editorWindow = nullptr;
    });

    qDebug() << "=== VST2Plugin::openEditor SUCCESS ===" << QString::fromStdString(m_name);

    window->raise();
    window->activateWindow();
    return window;
}

void VST2Plugin::closeEditor()
{
    if (!m_editorWindow) return;

    if (m_idleTimer) {
        m_idleTimer->stop();
        m_idleTimer = nullptr;
    }

    // Disconnect destroyed signal FIRST to prevent re-entry
    QObject::disconnect(m_editorWindow, nullptr, nullptr, nullptr);

    // Tell VST2 plugin to close its editor
    if (m_effect)
        dispatcher(VST_EFFECT_OPCODE_EDITOR_CLOSE);

    auto* win = m_editorWindow;
    m_editorWindow = nullptr;
    win->close();
    win->deleteLater();
    qDebug() << "VST2: Editor closed cleanly";
}

// ═══════════════════════════════════════════════════════════════════════
//  Parameters
// ═══════════════════════════════════════════════════════════════════════

std::vector<DSPParameter> VST2Plugin::getParameters() const
{
    std::vector<DSPParameter> params;
    if (!m_effect) return params;

    for (int i = 0; i < m_effect->num_params; ++i) {
        DSPParameter p;

        char nameBuf[VST_BUFFER_SIZE_PARAM_LONG_NAME + 1] = {};
        const_cast<VST2Plugin*>(this)->dispatcher(
            VST_EFFECT_OPCODE_EFFECT_NAME, i, 0, nameBuf);
        p.name = nameBuf;
        if (p.name.empty()) p.name = "Param " + std::to_string(i);

        p.value = m_effect->get_parameter(m_effect, i);
        p.minValue = 0.0f;
        p.maxValue = 1.0f;
        p.defaultValue = 0.0f;

        params.push_back(std::move(p));
    }
    return params;
}

void VST2Plugin::setParameter(int index, float value)
{
    if (!m_effect || index < 0 || index >= m_effect->num_params) return;
    m_effect->set_parameter(m_effect, index, value);
}

float VST2Plugin::getParameter(int index) const
{
    if (!m_effect || index < 0 || index >= m_effect->num_params) return 0.0f;
    return m_effect->get_parameter(m_effect, index);
}

// ═══════════════════════════════════════════════════════════════════════
//  State Persistence (chunk-based)
// ═══════════════════════════════════════════════════════════════════════

QByteArray VST2Plugin::saveState() const
{
    if (!m_effect) return {};

    // Check if plugin supports chunks
    if (!(m_effect->flags & VST_EFFECT_FLAG_CHUNKS)) {
        qDebug() << "[VST2] Plugin does not support chunks:"
                 << QString::fromStdString(m_name);
        return {};
    }

    void* chunk = nullptr;
    // isPreset=0 → full bank (all programs + params)
    intptr_t size = const_cast<VST2Plugin*>(this)->dispatcher(
        VST_EFFECT_OPCODE_GET_CHUNK_DATA, 0, 0, &chunk);

    if (size > 0 && chunk) {
        qDebug() << "[VST2] Saved state for" << QString::fromStdString(m_name)
                 << "(" << size << "bytes)";
        return QByteArray(static_cast<const char*>(chunk), static_cast<int>(size));
    }

    qWarning() << "[VST2] getChunk returned 0 for" << QString::fromStdString(m_name);
    return {};
}

bool VST2Plugin::restoreState(const QByteArray& data)
{
    if (!m_effect || data.isEmpty()) return false;

    if (!(m_effect->flags & VST_EFFECT_FLAG_CHUNKS)) {
        qWarning() << "[VST2] Plugin does not support chunks:"
                   << QString::fromStdString(m_name);
        return false;
    }

    // Suspend before state restore — some plugins reset internal buffers
    // or enter a broken state if state is set while running
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0);

    // isPreset=0 → full bank
    dispatcher(VST_EFFECT_OPCODE_SET_CHUNK_DATA, 0,
               static_cast<intptr_t>(data.size()),
               const_cast<char*>(data.constData()));

    // Resume after state restore
    dispatcher(VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1);

    qDebug() << "[VST2] Restored state for" << QString::fromStdString(m_name)
             << "(" << data.size() << "bytes, suspended/resumed)";
    return true;
}
