#include "DSDDecoder.h"

#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ═══════════════════════════════════════════════════════════════════════
//  DSF File Format Structures
// ═══════════════════════════════════════════════════════════════════════

#pragma pack(push, 1)

struct DSFHeader {
    char     magic[4];        // "DSD "
    uint64_t chunkSize;       // 28
    uint64_t totalFileSize;
    uint64_t metadataOffset;
};

struct DSFFormatChunk {
    char     magic[4];        // "fmt "
    uint64_t chunkSize;       // 52
    uint32_t formatVersion;   // 1
    uint32_t formatId;        // 0 = DSD raw
    uint32_t channelType;     // 2 = stereo
    uint32_t channelNum;
    uint32_t sampleRate;      // 2822400 .. 90316800
    uint32_t bitsPerSample;   // 1
    uint64_t sampleCount;     // per channel
    uint32_t blockSizePerCh;  // 4096
    uint32_t reserved;
};

struct DSFDataChunk {
    char     magic[4];        // "data"
    uint64_t chunkSize;
};

#pragma pack(pop)

// ═══════════════════════════════════════════════════════════════════════
//  Bit-reversal table for DSF format (LSB-first -> MSB-first)
// ═══════════════════════════════════════════════════════════════════════
//
// DSF stores DSD data with LSB first within each byte.
// Standard DSD processing expects MSB first (most significant bit = earliest
// in time). This lookup table reverses the bit order of a byte.

static const uint8_t s_bitReverse[256] = {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
    0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
    0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4,
    0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC,
    0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
    0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
    0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
    0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
    0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1,
    0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9,
    0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5,
    0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED,
    0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3,
    0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB,
    0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7,
    0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF,
    0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};

// ═══════════════════════════════════════════════════════════════════════
//  64-tap FIR lowpass filter with Blackman-Harris window
// ═══════════════════════════════════════════════════════════════════════
//
// Converts DSD 1-bit PDM to PCM by:
//   1. Expanding each DSD bit to +1/-1
//   2. Accumulating bits into popcount (density count)
//   3. Applying FIR lowpass to remove ultrasonic quantization noise
//
// The Blackman-Harris window provides ~92 dB of stopband attenuation,
// far superior to simple rectangular or Hann windows.
// ═══════════════════════════════════════════════════════════════════════

static constexpr int FIR_TAPS = 64;
static constexpr int MAX_CHANNELS = 8;

struct FIRFilter {
    float coeffs[FIR_TAPS] = {};
    float buffer[MAX_CHANNELS][FIR_TAPS] = {};
    int   pos[MAX_CHANNELS] = {};

    // Design windowed sinc lowpass filter
    // cutoffHz = cutoff frequency, outputRate = PCM output sample rate
    void design(double cutoffHz, double outputRate)
    {
        double fc = cutoffHz / outputRate; // normalized cutoff

        double sum = 0.0;
        for (int i = 0; i < FIR_TAPS; ++i) {
            double n = i - (FIR_TAPS - 1) / 2.0;

            // Sinc function
            double h;
            if (std::fabs(n) < 0.0001) {
                h = 2.0 * M_PI * fc;
            } else {
                h = std::sin(2.0 * M_PI * fc * n) / (M_PI * n);
            }

            // Blackman-Harris window (4-term, ~92 dB stopband attenuation)
            double w = 0.35875
                     - 0.48829 * std::cos(2.0 * M_PI * i / (FIR_TAPS - 1))
                     + 0.14128 * std::cos(4.0 * M_PI * i / (FIR_TAPS - 1))
                     - 0.01168 * std::cos(6.0 * M_PI * i / (FIR_TAPS - 1));

            coeffs[i] = (float)(h * w);
            sum += coeffs[i];
        }

        // Normalize for unity gain at DC
        float invSum = (float)(1.0 / sum);
        for (int i = 0; i < FIR_TAPS; ++i) {
            coeffs[i] *= invSum;
        }
    }

    void reset()
    {
        for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
            std::memset(buffer[ch], 0, sizeof(buffer[ch]));
            pos[ch] = 0;
        }
    }

    float process(int channel, float input)
    {
        buffer[channel][pos[channel]] = input;

        float output = 0.0f;
        int p = pos[channel];
        for (int i = 0; i < FIR_TAPS; ++i) {
            output += buffer[channel][p] * coeffs[i];
            if (--p < 0) p = FIR_TAPS - 1;
        }

        if (++pos[channel] >= FIR_TAPS) pos[channel] = 0;

        return output;
    }
};

// ═══════════════════════════════════════════════════════════════════════
//  Implementation
// ═══════════════════════════════════════════════════════════════════════

struct DSDDecoder::Impl {
    bool opened = false;
    std::ifstream file;

    // File info
    bool     isDSF = false;
    int      channels = 2;
    uint32_t dsdRate = 2822400;
    uint64_t totalDSDSamples = 0;
    uint32_t blockSize = 4096;

    // Mode: DoP (true) vs PCM conversion (false)
    bool     dopMode = false;
    bool     dopMarker = false; // alternates between false (0x05) and true (0xFA)

    // PCM/DoP output format
    double   pcmSampleRate = 44100.0;
    int      decimationRatio = 64;
    int      bytesPerPCMSample = 8;
    int64_t  totalPCMFrames = 0;

    // File positions
    uint64_t dataOffset = 0;
    uint64_t dataSize = 0;

    // Read state
    int64_t  pcmFramesRead = 0;

    // DSF block reading state
    std::vector<std::vector<uint8_t>> dsfBlockBuf;
    int dsfBlockPos = 0;
    bool dsfBlockValid = false;

    // 64-tap FIR lowpass filter (PCM mode only)
    FIRFilter fir;

    // ── DSF parsing ─────────────────────────────────────────────────
    bool openDSF(const std::string& path)
    {
        file.open(path, std::ios::binary);
        if (!file.is_open()) return false;

        DSFHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (std::memcmp(header.magic, "DSD ", 4) != 0) return false;

        DSFFormatChunk fmt;
        file.read(reinterpret_cast<char*>(&fmt), sizeof(fmt));
        if (std::memcmp(fmt.magic, "fmt ", 4) != 0) return false;

        channels = fmt.channelNum;
        dsdRate = fmt.sampleRate;
        totalDSDSamples = fmt.sampleCount;
        blockSize = fmt.blockSizePerCh;

        file.seekg(28 + fmt.chunkSize, std::ios::beg);

        DSFDataChunk dataChunk;
        file.read(reinterpret_cast<char*>(&dataChunk), sizeof(dataChunk));
        if (std::memcmp(dataChunk.magic, "data", 4) != 0) return false;

        dataOffset = file.tellg();
        dataSize = dataChunk.chunkSize - 12;

        isDSF = true;
        return true;
    }

    // ── DFF parsing ─────────────────────────────────────────────────
    bool openDFF(const std::string& path)
    {
        file.open(path, std::ios::binary);
        if (!file.is_open()) return false;

        char magic[4];
        file.read(magic, 4);
        if (std::memcmp(magic, "FRM8", 4) != 0) return false;

        file.seekg(8, std::ios::cur);

        char formType[4];
        file.read(formType, 4);
        if (std::memcmp(formType, "DSD ", 4) != 0) return false;

        while (file.good()) {
            char chunkId[4];
            file.read(chunkId, 4);
            if (!file.good()) break;

            uint8_t sizeBytes[8];
            file.read(reinterpret_cast<char*>(sizeBytes), 8);
            uint64_t chunkSize = 0;
            for (int i = 0; i < 8; ++i)
                chunkSize = (chunkSize << 8) | sizeBytes[i];

            if (std::memcmp(chunkId, "PROP", 4) == 0) {
                char propType[4];
                file.read(propType, 4);
                uint64_t remaining = chunkSize - 4;

                while (remaining > 0 && file.good()) {
                    char subId[4];
                    file.read(subId, 4);
                    uint8_t subSizeBytes[8];
                    file.read(reinterpret_cast<char*>(subSizeBytes), 8);
                    uint64_t subSize = 0;
                    for (int i = 0; i < 8; ++i)
                        subSize = (subSize << 8) | subSizeBytes[i];

                    if (std::memcmp(subId, "FS  ", 4) == 0) {
                        uint8_t rateBytes[4];
                        file.read(reinterpret_cast<char*>(rateBytes), 4);
                        dsdRate = ((uint32_t)rateBytes[0] << 24) |
                                  ((uint32_t)rateBytes[1] << 16) |
                                  ((uint32_t)rateBytes[2] << 8)  |
                                  (uint32_t)rateBytes[3];
                        if (subSize > 4) file.seekg(subSize - 4, std::ios::cur);
                    } else if (std::memcmp(subId, "CHNL", 4) == 0) {
                        uint8_t chBytes[2];
                        file.read(reinterpret_cast<char*>(chBytes), 2);
                        channels = ((int)chBytes[0] << 8) | chBytes[1];
                        if (subSize > 2) file.seekg(subSize - 2, std::ios::cur);
                    } else {
                        file.seekg(subSize, std::ios::cur);
                    }
                    remaining -= (12 + subSize);
                }
            } else if (std::memcmp(chunkId, "DSD ", 4) == 0) {
                dataOffset = file.tellg();
                dataSize = chunkSize;
                break;
            } else {
                file.seekg(chunkSize, std::ios::cur);
            }
        }

        if (dataOffset == 0) return false;
        isDSF = false;
        totalDSDSamples = dataSize * 8 / channels;
        return true;
    }

    // ── Init output format ────────────────────────────────────────────
    void initFormat()
    {
        if (dopMode) {
            // DoP mode: 16 DSD bits per PCM frame → DSD_rate / 16
            // Each DoP sample = 2 DSD bytes packed into 24-bit PCM with marker
            pcmSampleRate = (double)dsdRate / 16.0;
            decimationRatio = 16;
            bytesPerPCMSample = 2;  // 2 bytes = 16 DSD bits per DoP frame
            totalPCMFrames = (int64_t)(totalDSDSamples / 16);
            dopMarker = false; // start with 0x05 marker
        } else {
            // PCM conversion mode: FIR decimation to 44.1 kHz
            decimationRatio = dsdRate / 44100;
            bytesPerPCMSample = decimationRatio / 8;
            pcmSampleRate = 44100.0;
            totalPCMFrames = (int64_t)(totalDSDSamples / decimationRatio);
        }

        // DSF block buffer (needed for both modes)
        dsfBlockBuf.resize(channels);
        for (int ch = 0; ch < channels; ++ch) {
            dsfBlockBuf[ch].resize(blockSize, 0);
        }
        dsfBlockPos = blockSize; // force first read
        dsfBlockValid = false;

        if (!dopMode) {
            // Design FIR lowpass at 20 kHz for 44.1 kHz output (PCM mode only)
            fir.design(20000.0, pcmSampleRate);
            fir.reset();
        }
    }

    // ── Read next DSF block set ─────────────────────────────────────
    bool readNextDSFBlocks()
    {
        for (int ch = 0; ch < channels; ++ch) {
            file.read(reinterpret_cast<char*>(dsfBlockBuf[ch].data()), blockSize);
            if (!file.good() && !file.eof()) return false;
            auto bytesRead = file.gcount();
            if (bytesRead == 0) return false;
            if (bytesRead < (std::streamsize)blockSize) {
                std::memset(dsfBlockBuf[ch].data() + bytesRead, 0,
                            blockSize - bytesRead);
            }
        }
        dsfBlockPos = 0;
        dsfBlockValid = true;
        return true;
    }

    // ── Read DSD bytes for one PCM frame, all channels ──────────────
    // outBuf layout: [ch0_byte0, ch0_byte1, ..., ch1_byte0, ...]
    bool readDSDFrame(uint8_t* outBuf, int nChannels, int bytesPerCh)
    {
        if (isDSF) {
            for (int i = 0; i < bytesPerCh; ++i) {
                if (dsfBlockPos >= (int)blockSize) {
                    if (!readNextDSFBlocks()) return false;
                }
                for (int ch = 0; ch < nChannels; ++ch) {
                    // DSF is LSB-first; reverse bits to MSB-first
                    outBuf[ch * bytesPerCh + i] =
                        s_bitReverse[dsfBlockBuf[ch][dsfBlockPos]];
                }
                dsfBlockPos++;
            }
            return true;
        } else {
            // DFF is MSB-first, byte-interleaved
            for (int i = 0; i < bytesPerCh; ++i) {
                for (int ch = 0; ch < nChannels; ++ch) {
                    uint8_t b;
                    file.read(reinterpret_cast<char*>(&b), 1);
                    if (file.gcount() == 0) return false;
                    outBuf[ch * bytesPerCh + i] = b;
                }
            }
            return true;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════════

DSDDecoder::DSDDecoder() : m_impl(std::make_unique<Impl>()) {}
DSDDecoder::~DSDDecoder() { close(); }

bool DSDDecoder::open(const std::string& filePath, bool dopMode)
{
    close();

    m_impl->dopMode = dopMode;

    std::string ext;
    auto dot = filePath.rfind('.');
    if (dot != std::string::npos)
        ext = filePath.substr(dot + 1);
    for (auto& c : ext) c = std::tolower(c);

    bool ok = false;
    if (ext == "dsf") {
        ok = m_impl->openDSF(filePath);
    } else if (ext == "dff") {
        ok = m_impl->openDFF(filePath);
    }

    if (!ok) {
        close();
        return false;
    }

    m_impl->initFormat();
    m_impl->pcmFramesRead = 0;
    m_impl->file.seekg(m_impl->dataOffset, std::ios::beg);
    m_impl->opened = true;

    // Debug logging
    const char* typeName =
        isDSD2048() ? "DSD2048" : isDSD1024() ? "DSD1024" :
        isDSD512()  ? "DSD512"  : isDSD256()  ? "DSD256"  :
        isDSD128()  ? "DSD128"  : "DSD64";

    fprintf(stderr, "=== DSD FILE OPENED ===\n");
    fprintf(stderr, "  File: %s\n", filePath.c_str());
    fprintf(stderr, "  Mode: %s\n", dopMode ? "DoP (Native DSD over PCM)" : "PCM conversion");
    fprintf(stderr, "  Format: %s (bit order: %s)\n",
            m_impl->isDSF ? "DSF" : "DFF",
            m_impl->isDSF ? "LSB->MSB reversed" : "MSB native");
    fprintf(stderr, "  DSD Rate: %u Hz\n", m_impl->dsdRate);
    fprintf(stderr, "  Channels: %d\n", m_impl->channels);
    fprintf(stderr, "  Type: %s\n", typeName);
    fprintf(stderr, "  Output rate: %.0f Hz\n", m_impl->pcmSampleRate);
    fprintf(stderr, "  Total output frames: %lld\n", (long long)m_impl->totalPCMFrames);
    fprintf(stderr, "  Duration: %.1f sec\n",
            m_impl->totalPCMFrames / m_impl->pcmSampleRate);
    fprintf(stderr, "  Block size: %u\n", m_impl->blockSize);
    fprintf(stderr, "  Data offset: %llu\n", (unsigned long long)m_impl->dataOffset);
    fprintf(stderr, "  Data size: %llu bytes\n", (unsigned long long)m_impl->dataSize);
    if (dopMode) {
        fprintf(stderr, "  DoP markers: 0x05/0xFA alternating\n");
    } else {
        fprintf(stderr, "  Decimation: %d:1\n", m_impl->decimationRatio);
        fprintf(stderr, "  Filter: 64-tap FIR, Blackman-Harris, 20 kHz cutoff\n");
    }
    fprintf(stderr, "========================\n");

    return true;
}

void DSDDecoder::close()
{
    if (m_impl->file.is_open())
        m_impl->file.close();
    m_impl->opened = false;
    m_impl->pcmFramesRead = 0;
    m_impl->dsfBlockBuf.clear();
    m_impl->dsfBlockPos = 0;
    m_impl->dsfBlockValid = false;
}

bool DSDDecoder::isOpen() const { return m_impl->opened; }

int DSDDecoder::read(float* buf, int maxFrames)
{
    if (!m_impl->opened) return 0;

    const int ch = m_impl->channels;
    const int bytesPerCh = m_impl->bytesPerPCMSample;
    int framesWritten = 0;

    if (m_impl->dopMode) {
        // ── DoP encoding mode ──────────────────────────────────────
        // Pack 16 DSD bits (2 bytes) into 24-bit PCM with DoP marker.
        // The 24-bit word: [marker_byte][dsd_byte_high][dsd_byte_low]
        // Marker alternates 0x05 / 0xFA each sample.
        // Convert to float32 for CoreAudio: int24_signed / 2^23
        // The DAC hardware recognizes the markers and recovers DSD.

        uint8_t frameBuf[MAX_CHANNELS * 2]; // 2 bytes per channel per DoP frame

        while (framesWritten < maxFrames) {
            if (!m_impl->readDSDFrame(frameBuf, ch, 2)) {
                break;
            }

            uint8_t marker = m_impl->dopMarker ? 0xFA : 0x05;
            m_impl->dopMarker = !m_impl->dopMarker;

            for (int c = 0; c < ch; ++c) {
                const uint8_t* chBytes = &frameBuf[c * 2];

                // Build 24-bit DoP word: [marker][dsd_high][dsd_low]
                uint32_t dopWord = ((uint32_t)marker << 16)
                                 | ((uint32_t)chBytes[0] << 8)
                                 | (uint32_t)chBytes[1];

                // Convert to signed 24-bit integer
                int32_t signed24 = (int32_t)dopWord;
                if (signed24 & 0x800000) {
                    signed24 |= (int32_t)0xFF000000; // sign-extend
                }

                // Convert to float32: exact for 24-bit values (float32 has 24-bit mantissa)
                // CoreAudio will convert float32 → device integer format, preserving bits
                buf[framesWritten * ch + c] = (float)signed24 / 8388608.0f;
            }

            framesWritten++;
        }
    } else {
        // ── PCM conversion mode (FIR decimation) ───────────────────
        const int totalBits = bytesPerCh * 8;
        const float scale = 2.0f / (float)totalBits;

        // Temp buffer: max 8 channels * 256 bytes (DSD2048) = 2048 bytes
        uint8_t frameBuf[MAX_CHANNELS * 256];

        while (framesWritten < maxFrames) {
            if (!m_impl->readDSDFrame(frameBuf, ch, bytesPerCh)) {
                break;
            }

            for (int c = 0; c < ch; ++c) {
                const uint8_t* chBytes = &frameBuf[c * bytesPerCh];

                // Count set bits across all bytes (popcount decimation)
                int ones = 0;
                for (int b = 0; b < bytesPerCh; ++b) {
                    uint8_t byte = chBytes[b];
                    ones += (byte & 1) + ((byte >> 1) & 1) + ((byte >> 2) & 1)
                          + ((byte >> 3) & 1) + ((byte >> 4) & 1) + ((byte >> 5) & 1)
                          + ((byte >> 6) & 1) + ((byte >> 7) & 1);
                }

                // Popcount -> [-1.0 .. +1.0]
                float raw = (float)ones * scale - 1.0f;

                // 64-tap FIR lowpass to remove DSD quantization noise
                float filtered = m_impl->fir.process(c, raw);

                buf[framesWritten * ch + c] = filtered;
            }

            framesWritten++;
        }
    }

    m_impl->pcmFramesRead += framesWritten;
    return framesWritten;
}

bool DSDDecoder::seek(double secs)
{
    if (!m_impl->opened) return false;

    int64_t pcmFrame = (int64_t)(secs * m_impl->pcmSampleRate);
    pcmFrame = std::max((int64_t)0, std::min(pcmFrame, m_impl->totalPCMFrames));

    int64_t dsdBytePerCh = pcmFrame * m_impl->bytesPerPCMSample;

    if (m_impl->isDSF) {
        int64_t blockIndex = dsdBytePerCh / m_impl->blockSize;
        int64_t posInBlock = dsdBytePerCh % m_impl->blockSize;
        int64_t fileOffset = blockIndex * m_impl->blockSize * m_impl->channels;

        m_impl->file.clear();
        m_impl->file.seekg(m_impl->dataOffset + fileOffset, std::ios::beg);
        m_impl->dsfBlockPos = m_impl->blockSize;
        if (m_impl->readNextDSFBlocks()) {
            m_impl->dsfBlockPos = (int)posInBlock;
        }
    } else {
        int64_t byteOffset = dsdBytePerCh * m_impl->channels;
        m_impl->file.clear();
        m_impl->file.seekg(m_impl->dataOffset + byteOffset, std::ios::beg);
    }

    m_impl->pcmFramesRead = pcmFrame;

    if (m_impl->dopMode) {
        // Reset DoP marker to maintain proper alternation from seek point
        // Even frame positions start with 0x05, odd with 0xFA
        m_impl->dopMarker = (pcmFrame % 2) != 0;
    } else {
        // Reset FIR filter state after seek to avoid transients
        m_impl->fir.reset();
    }

    return true;
}

AudioStreamFormat DSDDecoder::format() const
{
    AudioStreamFormat fmt;
    fmt.sampleRate = m_impl->pcmSampleRate;
    fmt.channels = m_impl->channels;
    fmt.bitsPerSample = 32;
    fmt.totalFrames = m_impl->totalPCMFrames;
    fmt.durationSecs = (m_impl->totalPCMFrames > 0 && m_impl->pcmSampleRate > 0)
        ? (double)m_impl->totalPCMFrames / m_impl->pcmSampleRate
        : 0.0;
    return fmt;
}

double DSDDecoder::currentPositionSecs() const
{
    if (m_impl->pcmSampleRate <= 0) return 0.0;
    return (double)m_impl->pcmFramesRead / m_impl->pcmSampleRate;
}

// Range-based detection to handle slight sample rate variations
bool DSDDecoder::isDSD64() const
{
    return m_impl->dsdRate >= 2800000 && m_impl->dsdRate < 5600000;
}

bool DSDDecoder::isDSD128() const
{
    return m_impl->dsdRate >= 5600000 && m_impl->dsdRate < 11200000;
}

bool DSDDecoder::isDSD256() const
{
    return m_impl->dsdRate >= 11200000 && m_impl->dsdRate < 22400000;
}

bool DSDDecoder::isDSD512() const
{
    return m_impl->dsdRate >= 22400000 && m_impl->dsdRate < 45000000;
}

bool DSDDecoder::isDSD1024() const
{
    return m_impl->dsdRate >= 45000000 && m_impl->dsdRate < 90000000;
}

bool DSDDecoder::isDSD2048() const
{
    return m_impl->dsdRate >= 90000000;
}

double DSDDecoder::dsdSampleRate() const
{
    return (double)m_impl->dsdRate;
}

bool DSDDecoder::isDoPMode() const
{
    return m_impl->dopMode;
}
