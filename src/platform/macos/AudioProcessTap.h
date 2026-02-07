#pragma once
#include <QObject>

class DSPPipeline;

class AudioProcessTap : public QObject {
    Q_OBJECT
public:
    static AudioProcessTap* instance();

    bool isSupported() const;
    bool start();
    void stop();
    bool isActive() const;

    // Pre-create tap resources for faster start
    // Call when user navigates to Apple Music to avoid stall on play
    void prepareForPlayback();
    bool isPrepared() const;

    void setDSPPipeline(DSPPipeline* pipeline);

    void onPlaybackStall();
    void onPlaybackResumed();

signals:
    void tapStarted();
    void tapStopped();
    void tapError(const QString& error);
    void tapPrepared();

private:
    explicit AudioProcessTap(QObject* parent = nullptr);
    ~AudioProcessTap();

    class Impl;
    Impl* d;
};
