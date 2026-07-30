// =========================================================================
// SokuFrameExtractor — video_encoder.cpp
// =========================================================================
//
// BUG FIX 1 (critical) — FIFO open moved to encoder thread
// ---------------------------------------------------------
//   Previously: start() called fopen(fifo_path) on the GAME THREAD.
//   On Linux, opening a named FIFO for writing blocks until FFmpeg (the
//   reader) attaches.  This froze the game thread indefinitely whenever
//   start_sfe.sh was not running.
//
//   Fix: start() launches the encoder thread and returns immediately.
//   encoderLoop() opens the FIFO (or falls back to a local file) on its
//   own stack.  m_fifo_ready is set true once the file is open, so
//   FrameExtractor can gate m_capturing on isFifoReady().
//
// BUG FIX 2 (high) — dirty page cache bounded for fallback file
// -------------------------------------------------------------
//   When writing the fallback stream.bgra at 70 MB/s, dirty kernel pages
//   accumulate if the disk cannot keep up (HDD scenario).  The fix is
//   to periodically call _commit() (Wine → fsync) on the fallback fd to
//   force writeback and keep resident dirty pages bounded.  This is
//   skipped for the FIFO path because pipes have no page cache.
// =========================================================================

#include "video_encoder.hpp"
#include "logger.hpp"

#include <cstdio>
#include <filesystem>
#include <io.h>      // _commit, _fileno

namespace fs = std::filesystem;

namespace sfe {

// -------------------------------------------------------------------------
// Construction / Destruction
// -------------------------------------------------------------------------

VideoEncoder::VideoEncoder()  = default;
VideoEncoder::~VideoEncoder() { stop(); }

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------

bool VideoEncoder::start(const Config& cfg) {
    if (m_running.load()) return true;
    m_cfg = cfg;

    if (!m_initialized.load()) {
        if (!m_ring.init()) {
            sfe::log("VideoEncoder: ring buffer init failed");
            return false;
        }
        // NOTE: FIFO / fallback file is opened inside encoderLoop(), NOT
        // here, to avoid blocking the game thread on FIFO open.
        m_initialized.store(true);
    }

    m_running.store(true);
    m_fifo_ready.store(false);
    m_total_written.store(0);
    m_thread = std::thread(&VideoEncoder::encoderLoop, this);
    sfe::log("VideoEncoder: encoder thread started (FIFO open pending in thread)");
    return true;
}

void VideoEncoder::stop() {
    if (m_running.load()) {
        m_running.store(false);
        m_ring.requestStop();
        if (m_thread.joinable()) {
            sfe::log("VideoEncoder: joining encoder thread...");
            m_thread.join();
            sfe::log("VideoEncoder: thread joined");
        }
    }

    closeReplay();

    if (m_fifo) {
        fflush(m_fifo);
        fclose(m_fifo);
        m_fifo = nullptr;
    }

    if (m_initialized.load()) {
        m_ring.shutdown();
        m_initialized.store(false);
    }
}

// -------------------------------------------------------------------------
// Per-replay management
// -------------------------------------------------------------------------

bool VideoEncoder::openReplay(const std::string& replay_name) {
    closeReplay();
    m_current_replay_name = replay_name;
    m_replay_start_frame  = m_total_written.load();

    std::string dir = m_cfg.output_dir + "/" + replay_name;
    try {
        fs::create_directories(dir);
    } catch (const fs::filesystem_error& e) {
        sfe::log("VideoEncoder: failed to create dir %s: %s", dir.c_str(), e.what());
        return false;
    }

    std::string csv_path = dir + "/inputs.csv";
    m_csv.open(csv_path, std::ios::out | std::ios::trunc);
    if (!m_csv.is_open()) {
        sfe::log("VideoEncoder: failed to open CSV %s", csv_path.c_str());
        return false;
    }

    m_csv << "global_frame,local_frame,"
          << "p1_input,p2_input,"
          << "p1_up,p1_down,p1_left,p1_right,"
          << "p1_a,p1_b,p1_c,p1_d,p1_change,p1_spell,"
          << "p2_up,p2_down,p2_left,p2_right,"
          << "p2_a,p2_b,p2_c,p2_d,p2_change,p2_spell\n";

    sfe::log("VideoEncoder: opened replay '%s' (global frame offset %d)",
             replay_name.c_str(), m_replay_start_frame);
    return true;
}

void VideoEncoder::closeReplay() {
    if (m_csv.is_open()) {
        m_csv.flush();
        m_csv.close();
        sfe::log("VideoEncoder: closed replay '%s' CSV", m_current_replay_name.c_str());
    }
}

// -------------------------------------------------------------------------
// Consumer loop — runs on the encoder thread
// -------------------------------------------------------------------------

void VideoEncoder::encoderLoop() {
    // -----------------------------------------------------------------
    // Open output destination HERE (background thread).
    // If the FIFO exists but FFmpeg is not yet reading, fopen blocks
    // on this thread only — the game thread continues unaffected.
    // -----------------------------------------------------------------
    sfe::log("VideoEncoder: encoder thread — opening output '%s'...",
             m_cfg.fifo_path.c_str());

    m_fifo = fopen(m_cfg.fifo_path.c_str(), "wb");
    if (!m_fifo) {
        std::string fallback = m_cfg.output_dir + "/stream.bgra";
        sfe::log("VideoEncoder: FIFO open failed (not running start_sfe.sh?). "
                 "Falling back to %s", fallback.c_str());
        m_fifo = fopen(fallback.c_str(), "wb");
        m_fifo_ok = false;
        if (!m_fifo) {
            sfe::log("VideoEncoder: fallback fopen also failed (GLE=%lu)", GetLastError());
        } else {
            sfe::log("VideoEncoder: fallback file opened; raw BGRA will be written. "
                     "Encode with: ffmpeg -f rawvideo -pix_fmt bgra -s 640x480 "
                     "-r 60 -vf vflip -i stream.bgra output.mp4");
        }
    } else {
        m_fifo_ok = true;
        sfe::log("VideoEncoder: FIFO opened successfully (FFmpeg connected)");
    }

    // Signal FrameExtractor that it is now safe to start capture.
    m_fifo_ready.store(true);
    sfe::log("VideoEncoder: encoder loop running");

    // -----------------------------------------------------------------
    // Main drain loop
    // -----------------------------------------------------------------
    // How often to flush dirty pages when using the fallback file.
    // Every 300 frames ≈ 5 s @ 60 fps ≈ ~350 MB between syncs.
    constexpr int FALLBACK_SYNC_INTERVAL = 300;

    while (true) {
        FrameSlot* slot = m_ring.acquireReadSlot();
        if (!slot) break;   // stopped and empty — end of stream

        // 1. Write pixels to FIFO / fallback.
        if (m_fifo) {
            size_t written = fwrite(slot->pixels, 1, FRAME_BUFFER_SIZE, m_fifo);
            if (written != FRAME_BUFFER_SIZE) {
                sfe::log("VideoEncoder: short write (%zu/%d) — "
                         "FFmpeg may have exited", written, FRAME_BUFFER_SIZE);
            }

            // For the fallback regular file: periodically call _commit()
            // (Wine maps this to fsync()) to drain dirty pages and prevent
            // multi-GB memory growth on slow disks.  Skip for the FIFO
            // path (pipes have no kernel page cache).
            if (!m_fifo_ok) {
                int frames_now = m_total_written.load() + 1;
                if ((frames_now % FALLBACK_SYNC_INTERVAL) == 0) {
                    _commit(_fileno(m_fifo));
                }
            }
        }

        // 2. Append to per-replay CSV.
        if (m_csv.is_open()) {
            const int   gf = slot->frame_index;
            const int   lf = gf - m_replay_start_frame;
            const auto  p1 = slot->p1_input;
            const auto  p2 = slot->p2_input;

            m_csv << gf << "," << lf << ","
                  << p1 << "," << p2 << ","
                  << ((p1 & INPUT_UP)     ? 1 : 0) << ","
                  << ((p1 & INPUT_DOWN)   ? 1 : 0) << ","
                  << ((p1 & INPUT_LEFT)   ? 1 : 0) << ","
                  << ((p1 & INPUT_RIGHT)  ? 1 : 0) << ","
                  << ((p1 & INPUT_A)      ? 1 : 0) << ","
                  << ((p1 & INPUT_B)      ? 1 : 0) << ","
                  << ((p1 & INPUT_C)      ? 1 : 0) << ","
                  << ((p1 & INPUT_D)      ? 1 : 0) << ","
                  << ((p1 & INPUT_CHANGE) ? 1 : 0) << ","
                  << ((p1 & INPUT_SPELL)  ? 1 : 0) << ","
                  << ((p2 & INPUT_UP)     ? 1 : 0) << ","
                  << ((p2 & INPUT_DOWN)   ? 1 : 0) << ","
                  << ((p2 & INPUT_LEFT)   ? 1 : 0) << ","
                  << ((p2 & INPUT_RIGHT)  ? 1 : 0) << ","
                  << ((p2 & INPUT_A)      ? 1 : 0) << ","
                  << ((p2 & INPUT_B)      ? 1 : 0) << ","
                  << ((p2 & INPUT_C)      ? 1 : 0) << ","
                  << ((p2 & INPUT_D)      ? 1 : 0) << ","
                  << ((p2 & INPUT_CHANGE) ? 1 : 0) << ","
                  << ((p2 & INPUT_SPELL)  ? 1 : 0) << "\n";
        }

        m_ring.releaseReadSlot();
        m_total_written.fetch_add(1, std::memory_order_relaxed);
    }

    if (m_csv.is_open()) m_csv.flush();
    sfe::log("VideoEncoder: encoder loop exited (%d frames written)",
             m_total_written.load());
}

} // namespace sfe