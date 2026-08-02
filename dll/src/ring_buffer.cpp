// =========================================================================
// SokuFrameExtractor — ring_buffer.cpp
// =========================================================================

#include "sfe/ring_buffer.hpp"
#include "sfe/logger.hpp"

#include <cassert>
#include <cstring>

namespace sfe {

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------

bool RingBuffer::init() {
    if (m_slots) {
        sfe::log("RingBuffer::init() called while already initialised — ignored");
        return true;
    }

    m_alloc_sz = static_cast<SIZE_T>(RING_CAPACITY) * sizeof(FrameSlot);

    sfe::log("RingBuffer: allocating %zu MB (%u slots × %zu B)",
             m_alloc_sz / (1024 * 1024),
             RING_CAPACITY,
             sizeof(FrameSlot));

    // MEM_COMMIT | MEM_RESERVE to get the pages immediately.
    // We do NOT use MEM_LARGE_PAGES because that requires
    // SeLockMemoryPrivilege which typical user accounts lack.
    m_slots = static_cast<FrameSlot*>(
        VirtualAlloc(nullptr, m_alloc_sz,
                     MEM_COMMIT | MEM_RESERVE,
                     PAGE_READWRITE));

    if (!m_slots) {
        sfe::log("RingBuffer: VirtualAlloc failed (GLE=%lu)", GetLastError());
        return false;
    }

    // Try to pin the pages in physical RAM.  Needs RLIMIT_MEMLOCK >=
    // m_alloc_sz, which the default 8 MB limit does not grant; the runner
    // raises it where it can (docker --ulimit memlock).  Failure is harmless:
    // the ring still works, it just may take page faults on first touch, and
    // the loop below pre-faults every page anyway.
    if (!VirtualLock(m_slots, m_alloc_sz)) {
        sfe::log("RingBuffer: VirtualLock failed (GLE=%lu) — pages not pinned, "
                 "continuing (harmless; raise RLIMIT_MEMLOCK to silence)",
                 GetLastError());
    } else {
        sfe::log("RingBuffer: %zu MB pinned in physical RAM", m_alloc_sz / (1024*1024));
    }

    // Touch every page now so the OS maps them in before extraction starts.
    // This converts ~600 page faults into one amortised cost at init time.
    {
        volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(m_slots);
        const SIZE_T page   = 4096;
        for (SIZE_T off = 0; off < m_alloc_sz; off += page) {
            p[off] = 0;
        }
    }

    if (!m_lock_ready) {
        InitializeCriticalSection(&m_lock);
        InitializeConditionVariable(&m_cv_space);
        InitializeConditionVariable(&m_cv_data);
        m_lock_ready = true;
    }

    m_write_pos.store(0, std::memory_order_relaxed);
    m_read_pos.store(0,  std::memory_order_relaxed);
    m_stop.store(false,  std::memory_order_relaxed);

    sfe::log("RingBuffer: ready (%u slots)", RING_CAPACITY);
    return true;
}

void RingBuffer::shutdown() {
    requestStop();

    if (m_slots) {
        VirtualUnlock(m_slots, m_alloc_sz);
        VirtualFree(m_slots, 0, MEM_RELEASE);
        m_slots = nullptr;
    }
    m_alloc_sz = 0;

    if (m_lock_ready) {
        DeleteCriticalSection(&m_lock);
        m_lock_ready = false;
    }
    sfe::log("RingBuffer: shut down");
}

// -------------------------------------------------------------------------
// Producer
// -------------------------------------------------------------------------

FrameSlot* RingBuffer::acquireWriteSlot() {
    while (true) {
        uint64_t w = m_write_pos.load(std::memory_order_relaxed);
        uint64_t r = m_read_pos.load(std::memory_order_acquire);

        if ((w - r) < static_cast<uint64_t>(RING_CAPACITY)) {
            // Slot available — return it.  The producer fills it and then
            // calls commitWriteSlot().
            return &m_slots[w & RING_MASK];
        }

        // Buffer full: back-pressure the game thread.
        EnterCriticalSection(&m_lock);
        while ((w - m_read_pos.load(std::memory_order_acquire))
                   >= static_cast<uint64_t>(RING_CAPACITY)
               && !m_stop.load(std::memory_order_relaxed)) {
            SleepConditionVariableCS(&m_cv_space, &m_lock, INFINITE);
        }
        LeaveCriticalSection(&m_lock);

        if (m_stop.load(std::memory_order_relaxed)) {
            return nullptr;
        }
    }
}

void RingBuffer::commitWriteSlot() {
    // The position update must happen under the same mutex the waiter's
    // predicate is evaluated against, otherwise this interleaving loses the
    // wakeup entirely:
    //
    //   consumer: evaluates predicate -> false (buffer empty)
    //   producer: fetch_add(write_pos); notify_one()   <- no waiter yet
    //   consumer: goes to sleep, having missed the notify
    //
    // The consumer then sleeps until the *next* commit. Under back-pressure
    // that is a stall, not a deadlock, which is why it showed up as latency
    // spikes rather than a hang -- and why it survived this long.
    EnterCriticalSection(&m_lock);
    m_write_pos.fetch_add(1, std::memory_order_release);
    LeaveCriticalSection(&m_lock);
    WakeConditionVariable(&m_cv_data);
}

// -------------------------------------------------------------------------
// Consumer
// -------------------------------------------------------------------------

FrameSlot* RingBuffer::acquireReadSlot() {
    while (true) {
        uint64_t w = m_write_pos.load(std::memory_order_acquire);
        uint64_t r = m_read_pos.load(std::memory_order_relaxed);

        if (w > r) {
            return &m_slots[r & RING_MASK];
        }

        if (m_stop.load(std::memory_order_relaxed)) {
            // Stopped and empty — signal end of stream.
            return nullptr;
        }

        // Buffer empty: wait for producer.
        EnterCriticalSection(&m_lock);
        while (m_write_pos.load(std::memory_order_acquire) <= r
               && !m_stop.load(std::memory_order_relaxed)) {
            SleepConditionVariableCS(&m_cv_data, &m_lock, INFINITE);
        }
        LeaveCriticalSection(&m_lock);
    }
}

void RingBuffer::releaseReadSlot() {
    // Same lost-wakeup hazard as commitWriteSlot(), mirrored: without the
    // lock, a producer blocked on a full ring can miss this notify and stall
    // the game thread until the next frame is consumed.
    EnterCriticalSection(&m_lock);
    m_read_pos.fetch_add(1, std::memory_order_release);
    LeaveCriticalSection(&m_lock);
    WakeConditionVariable(&m_cv_space);
}

// -------------------------------------------------------------------------
// Control
// -------------------------------------------------------------------------

void RingBuffer::requestStop() {
    // Same hazard as commit/release, but the consequence here is worse than a
    // stall: if a waiter evaluates its predicate (not stopped, ring empty),
    // and we then set the flag and notify before it sleeps, it sleeps forever
    // and nothing will ever wake it again. That hangs VideoEncoder::stop() on
    // thread.join(), i.e. the game never exits.
    if (!m_lock_ready) {
        m_stop.store(true, std::memory_order_relaxed);
        return;
    }
    EnterCriticalSection(&m_lock);
    m_stop.store(true, std::memory_order_relaxed);
    LeaveCriticalSection(&m_lock);
    WakeAllConditionVariable(&m_cv_space);
    WakeAllConditionVariable(&m_cv_data);
}

} // namespace sfe