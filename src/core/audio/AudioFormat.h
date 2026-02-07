#pragma once
#include <cstdint>

struct AudioStreamFormat {
    double   sampleRate    = 44100.0;
    int      channels      = 2;
    int      bitsPerSample = 16;
    int64_t  totalFrames   = 0;
    double   durationSecs  = 0.0;
};
