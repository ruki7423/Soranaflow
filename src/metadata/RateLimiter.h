#pragma once

#include <QObject>
#include <QQueue>
#include <QTimer>
#include <functional>

class RateLimiter : public QObject {
    Q_OBJECT
public:
    explicit RateLimiter(int requestsPerSecond, QObject* parent = nullptr);

    void enqueue(std::function<void()> request);
    void clear();

private slots:
    void processQueue();

private:
    QQueue<std::function<void()>> m_queue;
    QTimer m_timer;
    int m_intervalMs;
};
