#pragma once
// =========================================================================
// SokuFrameExtractor — video_encoder.hpp
// =========================================================================
// Consumer side of the ring buffer pipeline.
//
// Responsibilities
// ----------------
//   1. Owns the RingBuffer that the OGL hook (producer) writes into.
//   2. Runs one background thread that drains the ring and writes raw BGRA
//      frames to a Unix FIFO.
//   3. Writes the per-frame input CSV that pairs with those frames.
//
// Video pipeline (outside this DLL)
// ----------------------------------
//   runner/encode.py creates the FIFO and attaches a native Linux FFmpeg to
//   it before the game launches:
//
//     ffmpeg -f rawvideo -pix_fmt bgra -s 640x480 -r 60 -i /tmp/sfe_video.fifo
//            -vf vflip -c:v libx264 -preset veryfast -crf 23  video.mp4
//
//   The vflip corrects OpenGL's bottom-up pixel order.
//
// FIFO access from Wine
// ---------------------
//   Wine maps Z:\ to the Linux filesystem root, so the default FIFO path
//   "Z:\\tmp\\sfe_video.fifo" opens /tmp/sfe_video.fifo on the host.
//
// THREADING: THE CSV IS OWNED BY THE ENCODER THREAD
// -------------------------------------------------
//   Both the frames and the CSV rows are produced here, on this one thread,
//   from the same ring slots.  That is what makes "CSV row i describes video
//   frame i" true by construction rather than by convention.
//
//   The previous design had the game thread call openReplay()/closeReplay()
//   to swap m_csv while this thread was writing to it.  Besides being a plain
//   data race on a std::ofstream, it meant up to RING_CAPACITY frames from the
//   outgoing replay were still queued when the stream was replaced, so they
//   landed in the incoming replay's file with local_frame recomputed against
//   the new offset.  The pre-refactor output shows the result: each CSV begins
//   at exactly the frame index its predecessor was stuck on (9718 -> 8622 ->
//   7696, three consecutive files).
//
//   There is now no per-replay swap at all.  One process captures one replay,
//   so the CSV is opened once at the top of encoderLoop() and closed at the
//   bottom, and no other thread can reach it.
//
// FIFO OPEN HAPPENS ON THIS THREAD, NOT THE GAME THREAD
// -----------------------------------------------------
//   Opening a FIFO for writing blocks until a reader attaches.  Doing that on
//   the game thread froze the game whenever FFmpeg was not already running.
//   start() spawns the thread and returns; the thread opens the FIFO on its
//   own stack and sets m_fifo_ready when it succeeds.
// =========================================================================

#include "sfe/config.hpp"
#include "sfe/ring_buffer.hpp"

#include <atomic>   // header-only; no msvcp140
#include <cstdio>   // FILE* -- UCRT, which Wine implements fully

namespace sfe {

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&)            = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    // Initialise the ring and start the consumer thread.  `csv_path` is the
    // full path of the per-frame input CSV for this run.  Safe to call from
    // the game thread: does NOT block on the FIFO open.
    bool start(const Config& cfg, const char* csv_path);

    // Drain remaining slots, close outputs, join the thread.
    void stop();

    // ------------------------------------------------------------------
    // Access for the producer (OGL hook)
    // ------------------------------------------------------------------
    RingBuffer& ring() { return m_ring; }

    bool isRunning() const { return m_running.load(); }

    // True once the encoder thread has opened its output.  The session waits
    // for this before arming capture, so the ring cannot fill while the
    // encoder is still connecting.
    bool isFifoReady() const { return m_fifo_ready.load(); }

    // True if the output could not be opened at all -- capture is pointless.
    bool failed() const { return m_failed.load(); }

    // ------------------------------------------------------------------
    // Diagnostics
    // ------------------------------------------------------------------
    int totalFramesWritten() const { return m_total_written.load(); }

private:
    void encoderLoop();
    bool openOutputs();     // encoder thread only
    void closeOutputs();    // encoder thread only

    // Win32 thread entry trampoline -- see the note on m_thread below.
    static DWORD WINAPI threadMain(LPVOID self);

    Config     m_cfg;
    char       m_csv_path[SFE_PATH_MAX] = {};
    RingBuffer m_ring;

    // CreateThread rather than std::thread: <thread> lives in msvcp140, and
    // std::thread's failure path calls ?_Throw_Cpp_error, which Wine's builtin
    // does not implement (see config.hpp).
    HANDLE m_thread = nullptr;

    std::atomic<bool> m_running     {false};
    std::atomic<bool> m_initialized {false};
    std::atomic<bool> m_fifo_ready  {false};
    std::atomic<bool> m_failed      {false};
    std::atomic<int>  m_total_written{0};

    // --- Owned exclusively by the encoder thread once start() returns. -----
    // FILE* for the CSV too: <fstream> is what dragged in basic_ostream,
    // _Fiopen and the locale/codecvt machinery, all of it msvcp140.
    FILE* m_fifo         = nullptr;
    bool  m_fifo_is_pipe = false;   // false = fallback regular file
    FILE* m_csv          = nullptr;
};

} // namespace sfe
