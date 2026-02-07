#pragma once

#include <QString>

struct LoudnessResult {
    double integratedLoudness = 0.0;  // LUFS
    double truePeak = 0.0;            // dBTP
    bool valid = false;
};

class LoudnessAnalyzer {
public:
    static LoudnessResult analyze(const QString& filePath);
};
