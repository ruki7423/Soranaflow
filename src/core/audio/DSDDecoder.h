#pragma once

#include <string>
#include <memory>
#include "AudioFormat.h"

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
class DSDDecoder {
public:
    DSDDecoder();
    ~DSDDecoder();

    DSDDecoder(const DSDDecoder&) = delete;
    DSDDecoder& operator=(const DSDDecoder&) = delete;

    // Open a DSD file. If dopMode is true, output DoP-encoded data
    // at DSD_rate/16 instead of FIR-filtered PCM at 44.1 kHz.
    bool open(const std::string& filePath, bool dopMode = false);
    void close();
    bool isOpen() const;

    // Read interleaved float32 samples. Returns frames actually read.
    // In PCM mode: filtered PCM at 44.1 kHz
    // In DoP mode: DoP-encoded data at DSD_rate/16
    int read(float* buf, int maxFrames);

    // Seek to position in seconds.
    bool seek(double secs);

    AudioStreamFormat format() const;
    double currentPositionSecs() const;

    // DSD-specific info (range-based detection)
    bool isDSD64() const;
    bool isDSD128() const;
    bool isDSD256() const;
    bool isDSD512() const;
    bool isDSD1024() const;
    bool isDSD2048() const;
    double dsdSampleRate() const;
    bool isDoPMode() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
