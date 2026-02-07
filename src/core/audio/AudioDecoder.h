#pragma once
#include <string>
#include <memory>
#include <QString>
#include "AudioFormat.h"

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool open(const std::string& filePath);
    void close();
    bool isOpen() const;

    // Read interleaved float32 samples into buf. Returns frames actually read.
    int read(float* buf, int maxFrames);

    // Seek to a position in seconds. Returns true on success.
    bool seek(double secs);

    AudioStreamFormat format() const;
    double currentPositionSecs() const;

    // Returns the FFmpeg codec name (e.g. "flac", "alac", "mp3") or empty if not loaded
    QString codecName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
