#pragma once

#include <string>
#include <memory>
#include "IDecoder.h"

// DSD decoder for DSF and DFF file formats.
//
// Two modes:
//   PCM mode (default): Converts DSD bitstream to PCM float32 via 64-tap
//     FIR decimation with Blackman-Harris windowed sinc lowpass filtering.
//     All DSD rates → 44.1 kHz PCM.
//
//   DoP mode: Packs 16 DSD bits into 24-bit PCM frames with DoP markers
//     (0x05/0xFA alternating). Output at DSD_rate/16:
//     DSD64 → 176.4 kHz, DSD128 → 352.8 kHz, DSD256 → 705.6 kHz
//     The DAC recognizes the markers and reconstructs the DSD stream.
class DSDDecoder : public IDecoder {
public:
    DSDDecoder();
    ~DSDDecoder() override;

    DSDDecoder(const DSDDecoder&) = delete;
    DSDDecoder& operator=(const DSDDecoder&) = delete;

    // IDecoder interface — open() ignores dopMode, use openDSD() for DoP
    bool open(const std::string& filePath) override;
    void close() override;
    bool isOpen() const override;

    int read(float* buf, int maxFrames) override;
    bool seek(double secs) override;

    AudioStreamFormat format() const override;
    double currentPositionSecs() const override;

    // DSD-specific open — if dopMode is true, output DoP-encoded data
    bool openDSD(const std::string& filePath, bool dopMode);

    // DSD-specific info (overrides from IDecoder)
    bool isDSD64() const override;
    bool isDSD128() const override;
    bool isDSD256() const override;
    bool isDSD512() const override;
    bool isDSD1024() const override;
    bool isDSD2048() const override;
    double dsdSampleRate() const override;
    bool isDoPMode() const override;

    bool dopMarkerState() const override;
    void setDoPMarkerState(bool marker) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
