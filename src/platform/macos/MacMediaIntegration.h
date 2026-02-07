#pragma once
#include <QObject>
#include <QImage>

class MacMediaIntegration : public QObject {
    Q_OBJECT
public:
    static MacMediaIntegration& instance();

    void initialize();

    void updateNowPlaying(const QString& title, const QString& artist,
                          const QString& album, double duration,
                          double currentTime, bool isPlaying);
    void updateArtwork(const QImage& image);
    void clearNowPlaying();

signals:
    void playPauseRequested();
    void nextRequested();
    void previousRequested();
    void seekRequested(double position);

private:
    MacMediaIntegration() = default;
    bool m_initialized = false;
};
