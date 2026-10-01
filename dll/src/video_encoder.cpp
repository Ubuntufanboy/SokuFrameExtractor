// =========================================================================
// SokuFrameExtractor — video_encoder.cpp
// =========================================================================
// See video_encoder.hpp for the threading contract.  The short version: this
// file's outputs (the FIFO and the CSV) are touched by the encoder thread and
// nothing else, which is what guarantees CSV row i describes video frame i.
// =========================================================================

#include "sfe/video_encoder.hpp"
#include "sfe/logger.hpp"
#include "sfe/state_row.hpp"

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

    // One definition of the header and the rows (sfe/state_row.hpp): the vs-COM
    // agent channel writes them too, and the two must never disagree.
    {
        static char hdr[STATE_ROW_CAP];
        if (formatStateHeader(hdr, sizeof(hdr)) < 0) {
            sfe::log("VideoEncoder: CSV header does not fit in %u bytes", unsigned(sizeof(hdr)));
            fclose(m_csv);
            m_csv = nullptr;
            return false;
        }
        fputs(hdr, m_csv);
        fputc('\n', m_csv);
    }

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
        // by construction and is what video frame N maps to.  Deriving it here,
        // on the thread that writes the row, is what stops it drifting -- the
        // old code computed it as (global_frame - m_replay_start_frame) with
        // the subtrahend owned by another thread.
        //
        // `game_frame` was documented as "the engine tick the pixels came
        // from, which can legitimately repeat if the game presents twice for
        // one tick".  IT IS NOT.  It is the session's own capture counter and
        // is equal to `frame` on every row of every capture ever taken -- 6055
        // of 6055 on a corpus capture, 6058 of 6058 on a fresh one.  The claim
        // went unchallenged because comparing it between two captures compares
        // 0,1,2,... with 0,1,2,... and always agrees, which is how two
        // captures of one replay sat a frame apart with nothing noticing.
        //
        // `battle_frame` is the real thing, read from BattleManager + 0x004.
        // It does repeat and skip as described above (about 50 repeats and 5
        // skips per match), and it restarts at every battle sub-state, so it
        // identifies a moment within a phase rather than within the match.
        // Both columns are kept: `game_frame` because captures in the wild
        // have it, `battle_frame` because it is what two captures can be
        // aligned on.
        static char line[STATE_ROW_CAP];
        const StateRow r{ m_total_written.load(), slot->frame_index, slot->battle_frame,
                          slot->p1_input, slot->p2_input,
                          &slot->p1_state, &slot->p2_state, &slot->camera };
        const int n = formatStateRow(line, sizeof(line), r);
        if (n < 0) {
            sfe::log("VideoEncoder: row %d does not fit in %u bytes; written empty",
                     r.row, unsigned(sizeof(line)));
        } else {
            fwrite(line, 1, static_cast<size_t>(n), m_csv);
        }
        fputc('\n', m_csv);

        m_ring.releaseReadSlot();
        m_total_written.fetch_add(1, std::memory_order_relaxed);
    }

    sfe::log("VideoEncoder: draining complete (%d frames)", m_total_written.load());
    closeOutputs();
}

} // namespace sfe
