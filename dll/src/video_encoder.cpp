// =========================================================================
// SokuFrameExtractor — video_encoder.cpp
// =========================================================================
// See video_encoder.hpp for the threading contract.  The short version: this
// file's outputs (the FIFO and the CSV) are touched by the encoder thread and
// nothing else, which is what guarantees CSV row i describes video frame i.
// =========================================================================

#include "sfe/video_encoder.hpp"
#include "sfe/logger.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <io.h>      // _commit, _fileno

namespace sfe {

// Create a directory and any missing parents, using Win32 only.
// Replaces std::filesystem::create_directories: <filesystem> is one of the
// heaviest msvcp140 dependencies in the STL (see config.hpp).
static void makeDirs(const char* path) {
    char buf[SFE_PATH_MAX];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (char* p = buf; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            const char saved = *p;
            *p = '\0';
            // Skip "Z:" and UNC-ish empty leading components.
            if (*buf && !(p == buf + 2 && buf[1] == ':'))
                CreateDirectoryA(buf, nullptr);
            *p = saved;
        }
    }
    CreateDirectoryA(buf, nullptr);
}

// Strip the last path component, in place.
static void dirName(char* path) {
    char* last = nullptr;
    for (char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') last = p;
    if (last) *last = '\0';
}

VideoEncoder::VideoEncoder()  = default;
VideoEncoder::~VideoEncoder() { stop(); }

// -------------------------------------------------------------------------
// Lifecycle  (game thread)
// -------------------------------------------------------------------------

bool VideoEncoder::start(const Config& cfg, const char* csv_path) {
    if (m_running.load()) return true;

    m_cfg = cfg;
    strncpy(m_csv_path, csv_path, sizeof(m_csv_path) - 1);
    m_csv_path[sizeof(m_csv_path) - 1] = '\0';

    if (!m_initialized.load()) {
        if (!m_ring.init()) {
            sfe::log("VideoEncoder: ring buffer init failed");
            return false;
        }
        m_initialized.store(true);
    }

    m_running.store(true);
    m_fifo_ready.store(false);
    m_failed.store(false);
    m_total_written.store(0);

    // NOTE: outputs are opened inside encoderLoop(), never here -- opening a
    // FIFO for writing blocks until FFmpeg attaches, and this is the game
    // thread.
    m_thread = CreateThread(nullptr, 0, &VideoEncoder::threadMain, this, 0, nullptr);
    if (!m_thread) {
        sfe::log("VideoEncoder: CreateThread failed (GLE=%lu)", GetLastError());
        m_running.store(false);
        return false;
    }
    sfe::log("VideoEncoder: encoder thread started (outputs open in-thread)");
    return true;
}

DWORD WINAPI VideoEncoder::threadMain(LPVOID self) {
    static_cast<VideoEncoder*>(self)->encoderLoop();
    return 0;
}

void VideoEncoder::stop() {
    if (m_running.load()) {
        m_running.store(false);
        m_ring.requestStop();
        if (m_thread) {
            sfe::log("VideoEncoder: joining encoder thread...");
            WaitForSingleObject(m_thread, INFINITE);
            CloseHandle(m_thread);
            m_thread = nullptr;
            sfe::log("VideoEncoder: thread joined (%d frames written)",
                     m_total_written.load());
        }
    }

    if (m_initialized.load()) {
        m_ring.shutdown();
        m_initialized.store(false);
    }
}

// -------------------------------------------------------------------------
// Output management  (encoder thread only)
// -------------------------------------------------------------------------

bool VideoEncoder::openOutputs() {
    // --- video sink -------------------------------------------------------
    sfe::log("VideoEncoder: opening video sink '%s'...", m_cfg.fifo_path);

    m_fifo = fopen(m_cfg.fifo_path, "wb");
    if (m_fifo) {
        m_fifo_is_pipe = true;
        sfe::log("VideoEncoder: FIFO opened (FFmpeg attached)");
    } else {
        // No FIFO: fall back to a raw file so a capture run started without
        // the runner still yields usable data.
        char fallback[SFE_PATH_MAX];
        snprintf(fallback, sizeof(fallback), "%s/stream.bgra", m_cfg.output_dir);
        sfe::log("VideoEncoder: FIFO open failed — falling back to %s", fallback);
        m_fifo = fopen(fallback, "wb");
        m_fifo_is_pipe = false;
        if (!m_fifo) {
            sfe::log("VideoEncoder: fallback fopen also failed (GLE=%lu)",
                     GetLastError());
            return false;
        }
        sfe::log("VideoEncoder: writing raw BGRA. Encode with: ffmpeg -f "
                 "rawvideo -pix_fmt bgra -s %dx%d -r 60 -vf vflip -i "
                 "stream.bgra video.mp4", GAME_WIDTH, GAME_HEIGHT);
    }

    // --- input CSV --------------------------------------------------------
    {
        char dir[SFE_PATH_MAX];
        strncpy(dir, m_csv_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        dirName(dir);
        if (*dir) makeDirs(dir);
    }

    m_csv = fopen(m_csv_path, "w");
    if (!m_csv) {
        sfe::log("VideoEncoder: failed to open CSV %s", m_csv_path);
        return false;
    }

    fputs("frame,game_frame,"
          "p1_input,p2_input,"
          "p1_up,p1_down,p1_left,p1_right,"
          "p1_a,p1_b,p1_c,p1_d,p1_change,p1_spell,"
          "p2_up,p2_down,p2_left,p2_right,"
          "p2_a,p2_b,p2_c,p2_d,p2_change,p2_spell\n", m_csv);

    sfe::log("VideoEncoder: CSV opened %s", m_csv_path);
    return true;
}

void VideoEncoder::closeOutputs() {
    if (m_csv) {
        fflush(m_csv);
        fclose(m_csv);
        m_csv = nullptr;
    }
    if (m_fifo) {
        fflush(m_fifo);
        fclose(m_fifo);   // closing the pipe is FFmpeg's EOF -- it exits here
        m_fifo = nullptr;
    }
}

// -------------------------------------------------------------------------
// Consumer loop  (encoder thread)
// -------------------------------------------------------------------------

void VideoEncoder::encoderLoop() {
    if (!openOutputs()) {
        m_failed.store(true);
        m_fifo_ready.store(true);   // unblock the waiter so it can see failed()
        closeOutputs();
        return;
    }

    // Signal the session that it is now safe to arm capture.
    m_fifo_ready.store(true);
    sfe::log("VideoEncoder: encoder loop running");

    // For the fallback regular file, periodically force writeback.  At ~70
    // MB/s a slow disk accumulates dirty pages without bound; _commit maps to
    // fsync under Wine.  Pipes have no page cache, so this is skipped there.
    constexpr int FALLBACK_SYNC_INTERVAL = 300;   // ~5 s @ 60 fps

    while (true) {
        FrameSlot* slot = m_ring.acquireReadSlot();
        if (!slot) break;   // stopped and drained — end of stream

        // --- 1. pixels ----------------------------------------------------
        const size_t written = fwrite(slot->pixels, 1, FRAME_BUFFER_SIZE, m_fifo);
        if (written != FRAME_BUFFER_SIZE) {
            sfe::log("VideoEncoder: short write (%zu/%d) — FFmpeg may have exited",
                     written, FRAME_BUFFER_SIZE);
        }
        if (!m_fifo_is_pipe &&
            ((m_total_written.load() + 1) % FALLBACK_SYNC_INTERVAL) == 0) {
            _commit(_fileno(m_fifo));
        }

        // --- 2. the CSV row describing exactly those pixels ---------------
        // `frame` is this file's own row counter, so it is dense and gap-free
        // by construction and is what video frame N maps to.  `game_frame` is
        // the engine tick the pixels came from; it can legitimately repeat if
        // the game presents twice for one tick, which is why the two are
        // recorded separately instead of assuming they agree.  Deriving the
        // row number here, on the thread that writes the row, is also what
        // stops it drifting -- the old code computed it as
        // (global_frame - m_replay_start_frame) with the subtrahend owned by
        // another thread.
        const int   row = m_total_written.load();
        const auto  p1  = slot->p1_input;
        const auto  p2  = slot->p2_input;

        fprintf(m_csv,
                "%d,%d,%u,%u,"
                "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                row, slot->frame_index,
                static_cast<unsigned>(p1), static_cast<unsigned>(p2),
                (p1 & INPUT_UP)     ? 1 : 0, (p1 & INPUT_DOWN)  ? 1 : 0,
                (p1 & INPUT_LEFT)   ? 1 : 0, (p1 & INPUT_RIGHT) ? 1 : 0,
                (p1 & INPUT_A)      ? 1 : 0, (p1 & INPUT_B)     ? 1 : 0,
                (p1 & INPUT_C)      ? 1 : 0, (p1 & INPUT_D)     ? 1 : 0,
                (p1 & INPUT_CHANGE) ? 1 : 0, (p1 & INPUT_SPELL) ? 1 : 0,
                (p2 & INPUT_UP)     ? 1 : 0, (p2 & INPUT_DOWN)  ? 1 : 0,
                (p2 & INPUT_LEFT)   ? 1 : 0, (p2 & INPUT_RIGHT) ? 1 : 0,
                (p2 & INPUT_A)      ? 1 : 0, (p2 & INPUT_B)     ? 1 : 0,
                (p2 & INPUT_C)      ? 1 : 0, (p2 & INPUT_D)     ? 1 : 0,
                (p2 & INPUT_CHANGE) ? 1 : 0, (p2 & INPUT_SPELL) ? 1 : 0);

        m_ring.releaseReadSlot();
        m_total_written.fetch_add(1, std::memory_order_relaxed);
    }

    sfe::log("VideoEncoder: draining complete (%d frames)", m_total_written.load());
    closeOutputs();
}

} // namespace sfe
