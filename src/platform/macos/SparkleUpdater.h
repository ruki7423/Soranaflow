#pragma once

#include <QObject>

class SparkleUpdater : public QObject {
    Q_OBJECT
public:
    static SparkleUpdater* instance();

    void checkForUpdates();
    void checkForUpdatesInBackground();

private:
    explicit SparkleUpdater(QObject* parent = nullptr);
    ~SparkleUpdater();

    void* m_updaterController;  // SPUStandardUpdaterController*
};
