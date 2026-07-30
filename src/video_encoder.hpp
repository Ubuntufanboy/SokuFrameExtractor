#pragma once
// =========================================================================
// SokuFrameExtractor — video_encoder.hpp
// =========================================================================
// Consumer side of the ring buffer pipeline.
//
// Responsibilities
// ----------------
//   1. Owns the RingBuffer that the OGL hook (producer) writes into.
//   2. Runs a single background thread that drains the ring buffer and
//      writes raw BGRA frames to a Unix FIFO (see start_sfe.sh).
//   3. Maintains a per-replay CSV file with frame-accurate input data.
//
// Video pipeline (outside this DLL)
// ----------------------------------
//   start_sfe.sh creates the FIFO and launches a native Linux FFmpeg
//   instance reading from it:
//
//     ffmpeg -f rawvideo -pix_fmt bgra -s 640x480 -r 60 -i /tmp/sfe_video.fifo
//            -vf "vflip,format=nv12,hwupload"
//            -vaapi_device /dev/dri/renderD128
//            -c:v h264_vaapi -qp 23  output.mp4
//
//   The vflip filter corrects the OpenGL bottom-up pixel orientation.
//
// FIFO access from Wine
// ---------------------
//   Wine maps Z:\ to the Linux filesystem root, so the default FIFO path
//   "Z:\\tmp\\sfe_video.fifo" opens /tmp/sfe_video.fifo on the host.
//
// BUG FIX — FIFO open moved to encoder thread
// -------------------------------------------
//   Previously fopen(fifo_path) was called inside start() on the GAME
//   THREAD. On Linux, opening a named FIFO for writing BLOCKS until a
//   reader attaches. This froze the game thread indefinitely when
//   start_sfe.sh was not running. The fix: start() launches the encoder
//   thread immediately and returns. The encoder thread opens the FIFO
//   (or fallback file) on its own stack. FrameExtractor polls
//   isFifoReady() before setting m_capturing = true so the ring buffer
//   never fills while the encoder thread is still connecting.
// =========================================================================

#include "config.hpp"
#include "ring_buffer.hpp"

#include <atomic>
#include <fstream>
#include <string>
#include <thread>

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

    // Initialise the ring buffer and start the consumer thread.
    // The consumer thread opens the FIFO (possibly blocking) internally.
    // Safe to call from the game thread — does NOT block on FIFO open.
    bool start(const Config& cfg);

    // Drain remaining ring slots, close the FIFO, and join the thread.
    void stop();

    // ------------------------------------------------------------------
    // Per-replay management
    // ------------------------------------------------------------------
    bool openReplay(const std::string& replay_name);
    void closeReplay();

    // ------------------------------------------------------------------
    // Access for the producer (OGL hook)
    // ------------------------------------------------------------------
    RingBuffer& ring() { return m_ring; }

    bool isRunning()   const { return m_running.load(); }

    // True once the encoder thread has successfully opened the FIFO or
    // the fallback file.  FrameExtractor must wait for this before
    // setting m_capturing = true to prevent the ring buffer from filling
    // while the encoder thread is still connecting.
    bool isFifoReady() const { return m_fifo_ready.load(); }

    // ------------------------------------------------------------------
    // Diagnostics
    // ------------------------------------------------------------------
    int totalFramesWritten() const { return m_total_written.load(); }

private:
    void encoderLoop();

    Config        m_cfg;
    RingBuffer    m_ring;
    std::thread   m_thread;
    std::atomic<bool> m_running    {false};
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_fifo_ready {false};   // set true inside encoderLoop()
    std::atomic<int>  m_total_written{0};

    FILE*         m_fifo    = nullptr;
    bool          m_fifo_ok = false;   // true  = real FIFO (pipe), false = fallback file

    std::ofstream m_csv;
    std::string   m_current_replay_name;
    int           m_replay_start_frame = 0;
};

} // namespace sfe