#pragma once
#include <QObject>

class DSPPipeline;

class AudioProcessTap : public QObject {
    Q_OBJECT
public:
    static AudioProcessTap* instance();

    bool isSupported() const;
    bool start();       // warmup + activate (backward compat)
    void stop();        // full teardown
    bool isActive() const;

    // Pre-create tap + aggregate + IOProc in standby (no DSP processing).
    // Call when user navigates to Apple Music. IOProc runs but outputs silence.
    void prepareForPlayback();
    bool isPrepared() const;

    // Instant activate/deactivate — flip atomic flag, no CoreAudio calls.
    // activate(): IOProc starts running audio through DSP pipeline.
    // deactivate(): IOProc returns to silence passthrough, tap stays warm.
    void activate();
    void deactivate();

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
