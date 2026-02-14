#pragma once

#include <string>
#include <QString>
#include "AudioFormat.h"

// Unified decoder interface for PCM and DSD playback.
// AudioDecoder and DSDDecoder both implement this.
class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual bool open(const std::string& filePath) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Read interleaved float32 samples. Returns frames actually read.
    virtual int read(float* buf, int maxFrames) = 0;

    // Seek to position in seconds. Returns true on success.
    virtual bool seek(double secs) = 0;

    virtual AudioStreamFormat format() const = 0;
    virtual double currentPositionSecs() const = 0;

    // PCM-specific (default: empty)
    virtual QString codecName() const { return QString(); }

    // DSD-specific (default: not DSD)
    virtual bool isDSD64() const { return false; }
    virtual bool isDSD128() const { return false; }
    virtual bool isDSD256() const { return false; }
    virtual bool isDSD512() const { return false; }
    virtual bool isDSD1024() const { return false; }
    virtual bool isDSD2048() const { return false; }
    virtual double dsdSampleRate() const { return 0.0; }
    virtual bool isDoPMode() const { return false; }
    virtual bool dopMarkerState() const { return false; }
    virtual void setDoPMarkerState(bool) {}
};
