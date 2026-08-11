#pragma once
#include "sfe/player_state.hpp"
// =========================================================================
// SokuFrameExtractor — ring_buffer.hpp
// =========================================================================
// Single-producer / single-consumer circular ring buffer.
//
// Producer : game thread  (inside our wglSwapBuffers hook)
// Consumer : encoder thread (VideoEncoder)
//
// Memory layout
// -------------
//   512 FrameSlots are VirtualAlloc'd as one contiguous slab and then
//   VirtualLock'd so the OS cannot page them out.  Each slot is
//   cache-line-aligned (64 B) to prevent false-sharing between the
//   producer's write and the consumer's read of adjacent metadata.
//
//   Total allocation: 512 × 1,228,864 B ≈ 600 MB.
//
// Blocking semantics
// ------------------
//   acquireWriteSlot() blocks when the buffer is full (all 512 slots are
//   waiting to be consumed).  This is the backpressure mechanism — the
//   game thread waits rather than dropping a frame.
//
//   acquireReadSlot()  blocks when the buffer is empty (no ready slots).
// =========================================================================

#include "sfe/config.hpp"
#include <atomic>          // header-only, emits no msvcp140 imports
#include <windows.h>

namespace sfe {

static_assert((RING_CAPACITY & (RING_CAPACITY - 1)) == 0,
              "RING_CAPACITY must be a power of two");
constexpr uint32_t RING_MASK = static_cast<uint32_t>(RING_CAPACITY - 1);

// -------------------------------------------------------------------------
// FrameSlot — one element of the ring buffer
//
// Layout (deliberately ordered so pixels[] is the first member and fills
// an integer number of cache lines, keeping the metadata in its own line):
//
//   [0 .. FRAME_BUFFER_SIZE-1]   pixels   (1,228,800 B = 19,200 × 64)
//   [FRAME_BUFFER_SIZE .. +39]   metadata (frame_index, p1, p2, both states)
//   [+40 .. +63]                 padding  (fills the last metadata cache line)
//
// The two PlayerStates were added inside the *existing* 64-byte metadata line
// rather than by growing the struct: at 1.2 MB a slot and hundreds of slots in
// flight, an extra cache line per slot is real memory, and the static_assert
// below is what keeps the arithmetic honest.
//
// sizeof(FrameSlot) == FRAME_BUFFER_SIZE + 64 == 1,228,864 B
// -------------------------------------------------------------------------
struct alignas(64) FrameSlot {
    // Raw pixel data, BGRA-32, bottom-up (OpenGL orientation).
    // vflip is applied by FFmpeg's -vf vflip filter.
    uint8_t  pixels[FRAME_BUFFER_SIZE];

    // Per-frame metadata written by the producer alongside the pixels.
    int32_t  frame_index  = 0;
    uint16_t p1_input     = 0;
    uint16_t p2_input     = 0;
    // The engine's own battle frame counter, BattleManager + 0x004. Unlike
    // frame_index -- which is this capture's row number and therefore starts
    // at 0 whenever the capture happened to arm -- this identifies the tick in
    // the *match*, so two captures of one replay can be aligned to each other
    // exactly. They need to be: capture arms within a tick or so of the battle
    // starting, not on a fixed tick, and a run of the same replay on another
    // machine came out shifted by one.
    uint32_t battle_frame = 0;

    // Ground truth for the same tick. See sfe/player_state.hpp for why the
    // world model needs to be told these rather than asked to infer them.
    PlayerState p1_state;
    PlayerState p2_state;

    // No hand-computed padding any more. `alignas(64)` on the struct already
    // rounds the total up to a multiple of 64, and the metadata grew from 44
    // bytes to a couple of hundred when PlayerState went from four fields to
    // twenty-six. A hand-maintained _pad[] would have to be recomputed on
    // every such change and is exactly the kind of arithmetic that is wrong
    // silently -- the previous assert pinned sizeof(PlayerState) == 16 for
    // that reason, and it did its job by failing here.
};

// The metadata must stay small relative to the pixels, which is the property
// the old hand-padding was really protecting. A slot is 1.2 MB of pixels and
// hundreds are in flight, so metadata under one kilobyte is free; anything
// approaching the pixel buffer would mean a field was added by mistake.
static_assert(sizeof(FrameSlot) - FRAME_BUFFER_SIZE < 1024,
              "FrameSlot metadata is over 1 KB per slot -- did a field grow "
              "unintentionally? Hundreds of slots are in flight.");
static_assert(sizeof(FrameSlot) % 64 == 0,
              "FrameSlot must stay a multiple of 64 bytes");

// =========================================================================
class RingBuffer {
public:
    RingBuffer()  = default;
    ~RingBuffer() { shutdown(); }

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    // Allocate one contiguous slab of memory for all 512 FrameSlots, then
    // attempt to VirtualLock it.  Returns false on allocation failure.
    // VirtualLock failure is non-fatal: logged but execution continues.
    bool init();

    // Release VirtualLock and free the slab.
    void shutdown();

    // ------------------------------------------------------------------
    // Producer API  (call from game thread)
    // ------------------------------------------------------------------

    // Block until a free slot is available (or stop is requested).
    // Returns a pointer to the slot — the caller MUST fill slot->pixels
    // and the metadata fields, then call commitWriteSlot().
    // Returns nullptr if the buffer has been stopped.
    FrameSlot* acquireWriteSlot();

    // Mark the slot acquired via acquireWriteSlot() as ready for the
    // consumer and advance the write head.
    void commitWriteSlot();

    // ------------------------------------------------------------------
    // Consumer API  (call from encoder thread)
    // ------------------------------------------------------------------

    // Block until a slot is ready (or stop is requested and the buffer
    // is drained).  Returns nullptr only when stopped *and* empty.
    FrameSlot* acquireReadSlot();

    // Mark the slot acquired via acquireReadSlot() as free and advance
    // the read head.
    void releaseReadSlot();

    // ------------------------------------------------------------------
    // Control
    // ------------------------------------------------------------------

    // Signal both the producer and consumer to unblock and return nullptr.
    void requestStop();

    bool isStopped() const {
        return m_stop.load(std::memory_order_relaxed);
    }

    // Number of slots that are filled and waiting to be consumed.
    int pendingCount() const {
        uint64_t w = m_write_pos.load(std::memory_order_acquire);
        uint64_t r = m_read_pos.load(std::memory_order_acquire);
        return static_cast<int>(w - r);
    }

private:
    // Pointer to the VirtualAlloc'd slab; individual slots are indexed
    // with m_slots[pos & RING_MASK].
    FrameSlot* m_slots    = nullptr;
    SIZE_T     m_alloc_sz = 0;

    // SPSC positions: write_pos is exclusively written by the producer,
    // read_pos exclusively by the consumer — no CAS needed.
    // Placed in separate cache lines to prevent false-sharing.
    alignas(64) std::atomic<uint64_t> m_write_pos{0};
    alignas(64) std::atomic<uint64_t> m_read_pos{0};

    // Win32 rather than <mutex>/<condition_variable>: those live in msvcp140,
    // whose Wine builtin aborts the process (see config.hpp). CRITICAL_SECTION
    // and CONDITION_VARIABLE are kernel32 and fully implemented by Wine, and
    // they are a closer match to what this needs anyway -- no exceptions, no
    // allocation, no locale machinery dragged in behind them.
    CRITICAL_SECTION   m_lock{};
    CONDITION_VARIABLE m_cv_space{};   // producer waits: buffer full
    CONDITION_VARIABLE m_cv_data{};    // consumer waits: buffer empty
    bool               m_lock_ready = false;
    std::atomic<bool>  m_stop{false};
};

} // namespace sfe