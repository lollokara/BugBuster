#pragma once
// =============================================================================
// state_lock.h — RAII scoped lock for g_stateMutex
// Single canonical timeout (50 ms) instead of scattered 5/10/20/50/100 ms.
// =============================================================================
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tasks.h"

#define STATE_LOCK_TIMEOUT_MS  50

class ScopedStateLock {
public:
    explicit ScopedStateLock()
        : m_held(xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(STATE_LOCK_TIMEOUT_MS)) == pdTRUE) {}
    ~ScopedStateLock() { if (m_held) xSemaphoreGive(g_stateMutex); }
    bool held() const { return m_held; }
private:
    bool m_held;
    ScopedStateLock(const ScopedStateLock&) = delete;
    ScopedStateLock& operator=(const ScopedStateLock&) = delete;
};
