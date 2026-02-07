#pragma once

#include "../core/dsp/IDSPProcessor.h"

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

#include <CoreFoundation/CoreFoundation.h>

// Forward-declare VST2 SDK types to avoid including vst.h in the header
// (vst.h defines global variables that cause duplicate symbol errors)
struct vst_effect_t;

class QWidget;
class QTimer;

class VST2Plugin : public IDSPProcessor {
public:
    VST2Plugin();
    ~VST2Plugin() override;

    bool loadFromPath(const std::string& vstPath);
    void unload();

    bool isLoaded() const { return m_effect != nullptr; }

    const std::string& pluginName() const { return m_name; }
    const std::string& pluginVendor() const { return m_vendor; }
    const std::string& pluginPath() const { return m_path; }

    bool hasEditor() const;
    QWidget* openEditor(QWidget* parent = nullptr);
    void closeEditor();

    // --- IDSPProcessor interface ---
    void process(float* buf, int frames, int channels) override;
    std::string getName() const override { return m_name; }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool enabled) override { m_enabled = enabled; }
    void prepare(double sampleRate, int channels) override;
    void reset() override;

    std::vector<DSPParameter> getParameters() const override;
    void setParameter(int index, float value) override;
    float getParameter(int index) const override;

private:
    static intptr_t hostCallbackStatic(
        vst_effect_t* effect, int32_t opcode,
        int32_t p_int1, intptr_t p_int2,
        void* p_ptr, float p_float);

    intptr_t dispatcher(int32_t opcode, int32_t index = 0,
                        intptr_t value = 0, void* ptr = nullptr,
                        float opt = 0.0f);

    vst_effect_t* m_effect = nullptr;
    CFBundleRef m_bundle = nullptr;

    std::string m_name;
    std::string m_vendor;
    std::string m_path;

    bool m_enabled = true;
    double m_sampleRate = 44100.0;
    int m_blockSize = 4096;
    int m_channels = 2;

    std::mutex m_processMutex;

    // Editor
    QWidget* m_editorWindow = nullptr;
    QTimer* m_idleTimer = nullptr;

    // Planar buffers for deinterleave/re-interleave
    std::vector<std::vector<float>> m_inBuffers;
    std::vector<std::vector<float>> m_outBuffers;
    std::vector<float*> m_inPtrs;
    std::vector<float*> m_outPtrs;
};
