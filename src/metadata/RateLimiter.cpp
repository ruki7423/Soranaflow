#include "RateLimiter.h"

RateLimiter::RateLimiter(int requestsPerSecond, QObject* parent)
    : QObject(parent)
    , m_intervalMs(1000 / qMax(requestsPerSecond, 1))
{
    m_timer.setInterval(m_intervalMs);
    connect(&m_timer, &QTimer::timeout, this, &RateLimiter::processQueue);
}

void RateLimiter::enqueue(std::function<void()> request)
{
    m_queue.enqueue(std::move(request));
    if (!m_timer.isActive()) {
        m_timer.start();
        processQueue(); // Process first immediately
    }
}

void RateLimiter::processQueue()
{
    if (m_queue.isEmpty()) {
        m_timer.stop();
        return;
    }

    auto request = m_queue.dequeue();
    request();
}

void RateLimiter::clear()
{
    m_queue.clear();
    m_timer.stop();
}
