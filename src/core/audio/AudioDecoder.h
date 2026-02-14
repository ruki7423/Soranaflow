#pragma once
#include <string>
#include <memory>
#include "IDecoder.h"

class AudioDecoder : public IDecoder {
public:
    AudioDecoder();
    ~AudioDecoder() override;

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool open(const std::string& filePath) override;
    void close() override;
    bool isOpen() const override;

    int read(float* buf, int maxFrames) override;
    bool seek(double secs) override;

    AudioStreamFormat format() const override;
    double currentPositionSecs() const override;

    QString codecName() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
