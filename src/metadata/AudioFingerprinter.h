#pragma once

#include <QObject>
#include <QString>

class AudioFingerprinter : public QObject {
    Q_OBJECT
public:
    static AudioFingerprinter* instance();

    void generateFingerprint(const QString& filePath);

signals:
    void fingerprintReady(const QString& filePath, const QString& fingerprint, int duration);
    void fingerprintError(const QString& filePath, const QString& error);

private:
    explicit AudioFingerprinter(QObject* parent = nullptr);
};
