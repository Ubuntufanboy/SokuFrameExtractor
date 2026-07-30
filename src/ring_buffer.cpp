// =========================================================================
// SokuFrameExtractor — ring_buffer.cpp
// =========================================================================

#include "ring_buffer.hpp"
#include "logger.hpp"

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

    // Attempt to pin all pages in physical RAM.
    // This requires RLIMIT_MEMLOCK ≥ m_alloc_sz on the Linux side.
    // The start_sfe.sh wrapper sets  ulimit -l unlimited  before
    // launching Wine so this should succeed.  If it fails we log and
    // continue — the buffer still works, just with potential page faults
    // on first access (which happen at warmup anyway).
    if (!VirtualLock(m_slots, m_alloc_sz)) {
        sfe::log("RingBuffer: VirtualLock failed (GLE=%lu) — "
                 "pages not pinned, continuing without lock",
                 GetLastError());
        sfe::log("RingBuffer: ensure 'ulimit -l unlimited' is set in start_sfe.sh");
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
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv_space.wait(lk, [this, w]() {
                return (w - m_read_pos.load(std::memory_order_acquire))
                           < static_cast<uint64_t>(RING_CAPACITY)
                       || m_stop.load(std::memory_order_relaxed);
            });
        }

        if (m_stop.load(std::memory_order_relaxed)) {
            return nullptr;
        }
    }
}

void RingBuffer::commitWriteSlot() {
    // Advance write head — this makes the slot visible to the consumer.
    m_write_pos.fetch_add(1, std::memory_order_release);
    // Wake the consumer if it is waiting.
    m_cv_data.notify_one();
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
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv_data.wait(lk, [this, r]() {
                return m_write_pos.load(std::memory_order_acquire) > r
                       || m_stop.load(std::memory_order_relaxed);
            });
        }
    }
}

void RingBuffer::releaseReadSlot() {
    m_read_pos.fetch_add(1, std::memory_order_release);
    // Wake the producer if it is blocked on a full buffer.
    m_cv_space.notify_one();
}

// -------------------------------------------------------------------------
// Control
// -------------------------------------------------------------------------

void RingBuffer::requestStop() {
    m_stop.store(true, std::memory_order_relaxed);
    m_cv_space.notify_all();
    m_cv_data.notify_all();
}

} // namespace sfe