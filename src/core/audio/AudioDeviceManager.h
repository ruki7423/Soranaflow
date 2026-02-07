#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <vector>
#include <cstdint>
#include <mutex>

struct AudioDeviceInfo {
    uint32_t    deviceId = 0;
    QString     name;
    bool        isDefault = false;
    bool        isAlive = true;
    int         inputChannels = 0;
    int         outputChannels = 0;
    QString     manufacturer;
    QString     uid;
};

struct SampleRateRange {
    double minimum;
    double maximum;
};

struct BufferSizeRange {
    uint32_t minimum;
    uint32_t maximum;
};

class AudioDeviceManager : public QObject {
    Q_OBJECT

public:
    static AudioDeviceManager* instance();
    ~AudioDeviceManager() override;

    // Device enumeration
    std::vector<AudioDeviceInfo> outputDevices() const;
    AudioDeviceInfo defaultOutputDevice() const;
    AudioDeviceInfo deviceById(uint32_t deviceId) const;

    // Resolve a persistent device UID to a current numeric AudioDeviceID.
    // Returns 0 if the device is not found (disconnected).
    uint32_t deviceIdFromUID(const QString& uid) const;

    // Find a device by name (fallback when UID doesn't match).
    // Returns 0 if not found.
    uint32_t deviceIdFromName(const QString& name) const;

    // Device capabilities
    std::vector<double> supportedSampleRates(uint32_t deviceId = 0) const;
    BufferSizeRange supportedBufferSizes(uint32_t deviceId = 0) const;
    uint32_t currentBufferSize(uint32_t deviceId = 0) const;
    double currentSampleRate(uint32_t deviceId = 0) const;

    // Buffer size control (always targets current active output device)
    bool setBufferSize(uint32_t frames);

    // Monitoring
    void startMonitoring();
    void stopMonitoring();
    bool isMonitoring() const { return m_monitoring; }

    // Called from CoreAudio property listener trampolines
    void refreshDeviceList();

signals:
    void deviceListChanged();
    void defaultDeviceChanged(uint32_t newDeviceId);
    void deviceDisconnected(uint32_t deviceId, const QString& name);
    void deviceConnected(uint32_t deviceId, const QString& name);
    void sampleRateChanged(uint32_t deviceId, double newRate);
    void bufferSizeChanged(uint32_t newSize);
    void deviceError(const QString& message);

private:
    explicit AudioDeviceManager(QObject* parent = nullptr);

    void subscribeToCoreAudioNotifications();
    void unsubscribeFromCoreAudioNotifications();
    void onDeviceCheckTimer();
    uint32_t resolveDeviceId(uint32_t deviceId) const;

    mutable std::mutex m_mutex;
    std::vector<AudioDeviceInfo> m_devices;
    uint32_t m_defaultDeviceId = 0;
    bool m_monitoring = false;
    bool m_subscribed = false;

    QTimer* m_checkTimer = nullptr;
    static constexpr int kCheckIntervalMs = 2000;  // TOPPING-style polling
};
