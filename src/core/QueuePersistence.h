#pragma once
#include <QObject>

class QTimer;
class QueueManager;

class QueuePersistence : public QObject {
    Q_OBJECT
public:
    explicit QueuePersistence(QueueManager* mgr, QObject* parent = nullptr);

    void scheduleSave();
    void saveImmediate();
    void restore();
    void flushPending();

    bool isRestoring() const { return m_restoring; }

private:
    void doSave();

    QueueManager* m_mgr;
    QTimer* m_saveTimer;
    bool m_restoring = false;
};
