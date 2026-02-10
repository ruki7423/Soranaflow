#include "AudioDeviceManager.h"

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

#include <QDebug>
#include <algorithm>
#include <cstring>

// ── Singleton ────────────────────────────────────────────────────────

AudioDeviceManager* AudioDeviceManager::instance()
{
    static AudioDeviceManager s_instance;
    return &s_instance;
}

AudioDeviceManager::AudioDeviceManager(QObject* parent)
    : QObject(parent)
{
    m_checkTimer = new QTimer(this);
    m_checkTimer->setInterval(kCheckIntervalMs);
    connect(m_checkTimer, &QTimer::timeout, this, &AudioDeviceManager::onDeviceCheckTimer);

    // Initial device scan
    refreshDeviceList();
}

AudioDeviceManager::~AudioDeviceManager()
{
    stopMonitoring();
    unsubscribeFromCoreAudioNotifications();
}

// ── Device Enumeration ───────────────────────────────────────────────

void AudioDeviceManager::refreshDeviceList()
{
    // Get all audio devices
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);

    if (status != noErr) {
        qWarning() << "[AudioDeviceManager] Failed to get device list size, OSStatus:" << status;
        emit deviceError(QStringLiteral("Failed to enumerate audio devices (error %1)").arg(status));
        return;
    }

    int deviceCount = dataSize / sizeof(AudioDeviceID);
    if (deviceCount == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_devices.clear();
        return;
    }

    std::vector<AudioDeviceID> deviceIds(deviceCount);
    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, deviceIds.data());

    if (status != noErr) {
        qWarning() << "[AudioDeviceManager] Failed to get device list, OSStatus:" << status;
        emit deviceError(QStringLiteral("Failed to read audio device list (error %1)").arg(status));
        return;
    }

    // Get default output device
    AudioDeviceID defaultId = 0;
    {
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 defaultSize = sizeof(AudioDeviceID);
        AudioObjectGetPropertyData(
            kAudioObjectSystemObject, &defaultProp, 0, nullptr, &defaultSize, &defaultId);
    }

    std::vector<AudioDeviceInfo> newDevices;

    for (int i = 0; i < deviceCount; ++i) {
        AudioDeviceID devId = deviceIds[i];

        // Check for output streams (skip input-only devices)
        AudioObjectPropertyAddress streamProp = {
            kAudioDevicePropertyStreams,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 streamSize = 0;
        status = AudioObjectGetPropertyDataSize(devId, &streamProp, 0, nullptr, &streamSize);
        if (status != noErr || streamSize == 0)
            continue;  // No output streams

        AudioDeviceInfo info;
        info.deviceId = devId;
        info.isDefault = (devId == defaultId);

        // Device name
        {
            AudioObjectPropertyAddress nameProp = {
                kAudioObjectPropertyName,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            CFStringRef cfName = nullptr;
            UInt32 nameSize = sizeof(CFStringRef);
            if (AudioObjectGetPropertyData(devId, &nameProp, 0, nullptr, &nameSize, &cfName) == noErr && cfName) {
                char nameBuf[256];
                if (CFStringGetCString(cfName, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8)) {
                    info.name = QString::fromUtf8(nameBuf);
                }
                CFRelease(cfName);
            }
        }

        // Manufacturer
        {
            AudioObjectPropertyAddress mfgProp = {
                kAudioObjectPropertyManufacturer,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            CFStringRef cfMfg = nullptr;
            UInt32 mfgSize = sizeof(CFStringRef);
            if (AudioObjectGetPropertyData(devId, &mfgProp, 0, nullptr, &mfgSize, &cfMfg) == noErr && cfMfg) {
                char mfgBuf[256];
                if (CFStringGetCString(cfMfg, mfgBuf, sizeof(mfgBuf), kCFStringEncodingUTF8)) {
                    info.manufacturer = QString::fromUtf8(mfgBuf);
                }
                CFRelease(cfMfg);
            }
        }

        // UID
        {
            AudioObjectPropertyAddress uidProp = {
                kAudioDevicePropertyDeviceUID,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            CFStringRef cfUid = nullptr;
            UInt32 uidSize = sizeof(CFStringRef);
            if (AudioObjectGetPropertyData(devId, &uidProp, 0, nullptr, &uidSize, &cfUid) == noErr && cfUid) {
                char uidBuf[256];
                if (CFStringGetCString(cfUid, uidBuf, sizeof(uidBuf), kCFStringEncodingUTF8)) {
                    info.uid = QString::fromUtf8(uidBuf);
                }
                CFRelease(cfUid);
            }
        }

        // Output channel count
        {
            AudioObjectPropertyAddress chanProp = {
                kAudioDevicePropertyStreamConfiguration,
                kAudioObjectPropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 chanSize = 0;
            if (AudioObjectGetPropertyDataSize(devId, &chanProp, 0, nullptr, &chanSize) == noErr && chanSize > 0) {
                std::vector<uint8_t> buf(chanSize);
                auto* bufList = reinterpret_cast<AudioBufferList*>(buf.data());
                if (AudioObjectGetPropertyData(devId, &chanProp, 0, nullptr, &chanSize, bufList) == noErr) {
                    int totalCh = 0;
                    for (UInt32 b = 0; b < bufList->mNumberBuffers; ++b) {
                        totalCh += bufList->mBuffers[b].mNumberChannels;
                    }
                    info.outputChannels = totalCh;
                }
            }
        }

        // Transport type (for diagnostics — AirPlay, Bluetooth, USB, etc.)
        {
            AudioObjectPropertyAddress transportProp = {
                kAudioDevicePropertyTransportType,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 transportType = 0;
            UInt32 transportSize = sizeof(UInt32);
            AudioObjectGetPropertyData(devId, &transportProp, 0, nullptr, &transportSize, &transportType);

            char typeStr[5] = {};
            typeStr[0] = (transportType >> 24) & 0xFF;
            typeStr[1] = (transportType >> 16) & 0xFF;
            typeStr[2] = (transportType >> 8) & 0xFF;
            typeStr[3] = transportType & 0xFF;
            qDebug() << "[AudioDevice]" << info.name
                     << "transport:" << typeStr
                     << "outputs:" << info.outputChannels;
        }

        // Device alive check
        {
            AudioObjectPropertyAddress aliveProp = {
                kAudioDevicePropertyDeviceIsAlive,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 alive = 1;
            UInt32 aliveSize = sizeof(UInt32);
            AudioObjectGetPropertyData(devId, &aliveProp, 0, nullptr, &aliveSize, &alive);
            info.isAlive = (alive != 0);
        }

        newDevices.push_back(std::move(info));
    }

    // Compare with previous list and emit signals
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Check for disconnected devices
        for (const auto& old : m_devices) {
            bool found = false;
            for (const auto& cur : newDevices) {
                if (cur.deviceId == old.deviceId) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                qDebug() << "[AudioDeviceManager] Device disconnected:"
                         << old.name << "(id:" << old.deviceId << ")";
                QMetaObject::invokeMethod(this, [this, id = old.deviceId, name = old.name]() {
                    emit deviceDisconnected(id, name);
                }, Qt::QueuedConnection);
            }
        }

        // Check for newly connected devices
        for (const auto& cur : newDevices) {
            bool found = false;
            for (const auto& old : m_devices) {
                if (old.deviceId == cur.deviceId) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                qDebug() << "[AudioDeviceManager] Device connected:"
                         << cur.name << "(id:" << cur.deviceId << ")";
                QMetaObject::invokeMethod(this, [this, id = cur.deviceId, name = cur.name]() {
                    emit deviceConnected(id, name);
                }, Qt::QueuedConnection);
            }
        }

        bool defaultChanged = (defaultId != m_defaultDeviceId);
        m_devices = std::move(newDevices);
        m_defaultDeviceId = defaultId;

        if (defaultChanged) {
            QMetaObject::invokeMethod(this, [this, defaultId]() {
                emit defaultDeviceChanged(defaultId);
            }, Qt::QueuedConnection);
        }
    }
}

std::vector<AudioDeviceInfo> AudioDeviceManager::outputDevices() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_devices;
}

AudioDeviceInfo AudioDeviceManager::defaultOutputDevice() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& d : m_devices) {
        if (d.isDefault) return d;
    }
    if (!m_devices.empty()) return m_devices.front();
    return {};
}

AudioDeviceInfo AudioDeviceManager::deviceById(uint32_t deviceId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& d : m_devices) {
        if (d.deviceId == deviceId) return d;
    }
    return {};
}

uint32_t AudioDeviceManager::deviceIdFromUID(const QString& uid) const
{
    if (uid.isEmpty()) return 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& d : m_devices) {
        if (d.uid == uid) return d.deviceId;
    }
    return 0;
}

uint32_t AudioDeviceManager::deviceIdFromName(const QString& name) const
{
    if (name.isEmpty()) return 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& d : m_devices) {
        if (d.name == name) return d.deviceId;
    }
    return 0;
}

// ── Device Capabilities ──────────────────────────────────────────────

uint32_t AudioDeviceManager::resolveDeviceId(uint32_t deviceId) const
{
    if (deviceId != 0) return deviceId;
    return m_defaultDeviceId;
}

std::vector<double> AudioDeviceManager::supportedSampleRates(uint32_t deviceId) const
{
    uint32_t devId = resolveDeviceId(deviceId);
    if (devId == 0) return {};

    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(devId, &prop, 0, nullptr, &dataSize);
    if (status != noErr || dataSize == 0) return {};

    int rangeCount = dataSize / sizeof(AudioValueRange);
    std::vector<AudioValueRange> ranges(rangeCount);
    status = AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &dataSize, ranges.data());
    if (status != noErr) return {};

    std::vector<double> rates;
    // Standard sample rates to check against ranges
    static const double standardRates[] = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000
    };

    for (const auto& range : ranges) {
        if (range.mMinimum == range.mMaximum) {
            // Discrete rate
            rates.push_back(range.mMinimum);
        } else {
            // Range — check which standard rates fall within
            for (double sr : standardRates) {
                if (sr >= range.mMinimum && sr <= range.mMaximum) {
                    rates.push_back(sr);
                }
            }
        }
    }

    // Sort and deduplicate
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());

    return rates;
}

BufferSizeRange AudioDeviceManager::supportedBufferSizes(uint32_t deviceId) const
{
    uint32_t devId = resolveDeviceId(deviceId);
    if (devId == 0) return {256, 4096};

    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    AudioValueRange range = {};
    UInt32 dataSize = sizeof(AudioValueRange);
    OSStatus status = AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &dataSize, &range);

    if (status != noErr) {
        qWarning() << "[AudioDeviceManager] Failed to get buffer size range for device"
                    << devId << ", OSStatus:" << status;
        return {256, 4096};
    }

    return {
        static_cast<uint32_t>(range.mMinimum),
        static_cast<uint32_t>(range.mMaximum)
    };
}

uint32_t AudioDeviceManager::currentBufferSize(uint32_t deviceId) const
{
    uint32_t devId = resolveDeviceId(deviceId);
    if (devId == 0) return 512;

    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 bufferSize = 512;
    UInt32 dataSize = sizeof(UInt32);
    AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &dataSize, &bufferSize);
    return bufferSize;
}

double AudioDeviceManager::currentSampleRate(uint32_t deviceId) const
{
    uint32_t devId = resolveDeviceId(deviceId);
    if (devId == 0) return 44100.0;

    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    Float64 rate = 44100.0;
    UInt32 dataSize = sizeof(Float64);
    AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &dataSize, &rate);
    return rate;
}

bool AudioDeviceManager::setBufferSize(uint32_t frames)
{
    // Query fresh default output device from CoreAudio (m_defaultDeviceId
    // may lag if refreshDeviceList hasn't run since a device switch).
    AudioObjectPropertyAddress defaultProp = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID devId = 0;
    UInt32 devSize = sizeof(devId);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                               0, nullptr, &devSize, &devId);
    if (devId == 0) {
        qDebug() << "[AudioDeviceManager] setBufferSize: no active device";
        return false;
    }

    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 bufSize = frames;
    OSStatus status = AudioObjectSetPropertyData(devId, &prop, 0, nullptr, sizeof(UInt32), &bufSize);
    if (status != noErr) {
        qWarning() << "[AudioDeviceManager] Failed to set buffer size to" << frames
                    << "for device" << devId << ", OSStatus:" << status;
        emit deviceError(QStringLiteral("Failed to set buffer size to %1 frames (error %2)")
                             .arg(frames).arg(status));
        return false;
    }

    // Read back actual value (device may choose closest supported size)
    UInt32 actualSize = 0;
    UInt32 propSize = sizeof(actualSize);
    AudioObjectGetPropertyData(devId, &prop, 0, nullptr, &propSize, &actualSize);
    qDebug() << "[AudioDeviceManager] Buffer size requested:" << frames
             << "actual:" << actualSize << "for device" << devId;

    emit bufferSizeChanged(actualSize);
    return true;
}

// ── CoreAudio Property Listeners ─────────────────────────────────────

// Trampoline for CoreAudio C callback
static OSStatus deviceListListenerProc(
    AudioObjectID /*objectId*/,
    UInt32 /*numberAddresses*/,
    const AudioObjectPropertyAddress* /*addresses*/,
    void* clientData)
{
    auto* self = static_cast<AudioDeviceManager*>(clientData);
    qDebug() << "[AudioDeviceManager] CoreAudio: device list changed";
    QMetaObject::invokeMethod(self, [self]() {
        self->refreshDeviceList();
        emit self->deviceListChanged();
    }, Qt::QueuedConnection);
    return noErr;
}

static OSStatus defaultDeviceListenerProc(
    AudioObjectID /*objectId*/,
    UInt32 /*numberAddresses*/,
    const AudioObjectPropertyAddress* /*addresses*/,
    void* clientData)
{
    auto* self = static_cast<AudioDeviceManager*>(clientData);
    qDebug() << "[AudioDeviceManager] CoreAudio: default output device changed";
    QMetaObject::invokeMethod(self, [self]() {
        self->refreshDeviceList();
    }, Qt::QueuedConnection);
    return noErr;
}

void AudioDeviceManager::subscribeToCoreAudioNotifications()
{
    if (m_subscribed) return;

    // Listen for device list changes (add/remove)
    AudioObjectPropertyAddress devicesProp = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectAddPropertyListener(
        kAudioObjectSystemObject, &devicesProp, deviceListListenerProc, this);
    if (status != noErr) {
        qWarning() << "[AudioDeviceManager] Failed to add device list listener, OSStatus:" << status;
    }

    // Listen for default output device changes
    AudioObjectPropertyAddress defaultProp = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    status = AudioObjectAddPropertyListener(
        kAudioObjectSystemObject, &defaultProp, defaultDeviceListenerProc, this);
    if (status != noErr) {
        qWarning() << "[AudioDeviceManager] Failed to add default device listener, OSStatus:" << status;
    }

    m_subscribed = true;
    qDebug() << "[AudioDeviceManager] Subscribed to CoreAudio device notifications";
}

void AudioDeviceManager::unsubscribeFromCoreAudioNotifications()
{
    if (!m_subscribed) return;

    AudioObjectPropertyAddress devicesProp = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListener(
        kAudioObjectSystemObject, &devicesProp, deviceListListenerProc, this);

    AudioObjectPropertyAddress defaultProp = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListener(
        kAudioObjectSystemObject, &defaultProp, defaultDeviceListenerProc, this);

    m_subscribed = false;
    qDebug() << "[AudioDeviceManager] Unsubscribed from CoreAudio device notifications";
}

// ── Monitoring ───────────────────────────────────────────────────────

void AudioDeviceManager::startMonitoring()
{
    if (m_monitoring) return;

    subscribeToCoreAudioNotifications();
    m_checkTimer->start();
    m_monitoring = true;

    qDebug() << "[AudioDeviceManager] Monitoring started (interval:" << kCheckIntervalMs << "ms)";
}

void AudioDeviceManager::stopMonitoring()
{
    if (!m_monitoring) return;

    m_checkTimer->stop();
    m_monitoring = false;

    qDebug() << "[AudioDeviceManager] Monitoring stopped";
}

void AudioDeviceManager::onDeviceCheckTimer()
{
    // TOPPING-style polling: check if devices are still alive
    std::vector<std::pair<uint32_t, QString>> deadDevices;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& dev : m_devices) {
            AudioObjectPropertyAddress aliveProp = {
                kAudioDevicePropertyDeviceIsAlive,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 alive = 1;
            UInt32 aliveSize = sizeof(UInt32);
            OSStatus status = AudioObjectGetPropertyData(
                dev.deviceId, &aliveProp, 0, nullptr, &aliveSize, &alive);

            bool wasAlive = dev.isAlive;
            dev.isAlive = (status == noErr && alive != 0);

            if (wasAlive && !dev.isAlive) {
                qWarning() << "[AudioDeviceManager] Device became unresponsive:"
                           << dev.name << "(id:" << dev.deviceId << ")";
                deadDevices.push_back({dev.deviceId, dev.name});
            }
        }
    }

    for (const auto& [id, name] : deadDevices) {
        emit deviceDisconnected(id, name);
        emit deviceError(QStringLiteral("Audio device \"%1\" is no longer responding").arg(name));
    }

    if (!deadDevices.empty()) {
        refreshDeviceList();
        emit deviceListChanged();
    }
}
