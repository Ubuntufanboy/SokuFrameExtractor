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

    // The state columns are appended, never inserted, so every reader written
    // against the old header keeps working -- `data/soku.py` selects columns by
    // name and a corpus mixing both layouts must stay loadable.
    fputs("frame,game_frame,"
          "p1_input,p2_input,"
          "p1_up,p1_down,p1_left,p1_right,"
          "p1_a,p1_b,p1_c,p1_d,p1_change,p1_spell,"
          "p2_up,p2_down,p2_left,p2_right,"
          "p2_a,p2_b,p2_c,p2_d,p2_change,p2_spell,"
          "p1_x,p1_y,p1_vx,p1_vy,p1_ax,p1_ay,p1_dir,p1_action,p1_action_frame,p1_hitstop,p1_untech,p1_hitboxes,p1_hurtboxes,p1_hit_count,p1_hp,p1_spirit,p1_max_spirit,p1_spirit_delay,p1_timestop,p1_ground_dashes,p1_air_dashes,p1_correction,p1_combo_rate,p1_combo_hits,p1_combo_damage,p1_combo_limit,p1_guarding,p1_wrongblock,p1_crushed,p1_knockdown,"
          "p2_x,p2_y,p2_vx,p2_vy,p2_ax,p2_ay,p2_dir,p2_action,p2_action_frame,p2_hitstop,p2_untech,p2_hitboxes,p2_hurtboxes,p2_hit_count,p2_hp,p2_spirit,p2_max_spirit,p2_spirit_delay,p2_timestop,p2_ground_dashes,p2_air_dashes,p2_correction,p2_combo_rate,p2_combo_hits,p2_combo_damage,p2_combo_limit,p2_guarding,p2_wrongblock,p2_crushed,p2_knockdown,"
          "battle_frame", m_csv);

    // Projectile columns, appended after everything that already existed so no
    // reader written against the previous header has to change. Emitted in a
    // loop rather than spelled out because MAX_PROJECTILES is the only place
    // the width is decided -- a literal header here would silently disagree
    // with the row writer the first time that constant moves.
    for (int p = 1; p <= 2; ++p) {
        fprintf(m_csv, ",p%d_proj_n,p%d_proj_hb,p%d_proj_raw", p, p, p);
        for (int k = 0; k < MAX_PROJECTILES; ++k) {
            fprintf(m_csv,
                    ",p%d_pr%d_x,p%d_pr%d_y,p%d_pr%d_vx,p%d_pr%d_vy,"
                    "p%d_pr%d_dir,p%d_pr%d_act,p%d_pr%d_hb",
                    p, k, p, k, p, k, p, k, p, k, p, k, p, k);
        }
    }
    fputc('\n', m_csv);

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
        const int   row = m_total_written.load();
        const auto  p1  = slot->p1_input;
        const auto  p2  = slot->p2_input;
        const auto& s1  = slot->p1_state;
        const auto& s2  = slot->p2_state;

        fprintf(m_csv,
                "%d,%d,%u,%u,"
                "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                "%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%d,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%d,%.4f,%u,%u,%u,%d,%d,%d,%d,"
                "%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%d,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%d,%.4f,%u,%u,%u,%d,%d,%d,%d,"
                "%u",   // no newline: the projectile columns follow below
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
                (p2 & INPUT_CHANGE) ? 1 : 0, (p2 & INPUT_SPELL) ? 1 : 0,
                // %.3f: positions are in game units of a few hundred across the
                // stage, so a millipixel is far below anything that matters and
                // full float precision would only inflate the sidecar.
                s1.x, s1.y, s1.vx, s1.vy, s1.ax, s1.ay,
                static_cast<int>(s1.direction),
                static_cast<unsigned>(s1.action),
                static_cast<unsigned>(s1.action_frame),
                static_cast<unsigned>(s1.hitstop),
                static_cast<unsigned>(s1.untech),
                static_cast<unsigned>(s1.hitboxes),
                static_cast<unsigned>(s1.hurtboxes),
                static_cast<int>(s1.hit_count),
                static_cast<int>(s1.hp),
                static_cast<unsigned>(s1.spirit),
                static_cast<unsigned>(s1.max_spirit),
                static_cast<unsigned>(s1.spirit_delay),
                static_cast<unsigned>(s1.timestop),
                static_cast<unsigned>(s1.ground_dashes),
                static_cast<unsigned>(s1.air_dashes),
                static_cast<int>(s1.correction),
                s1.combo_rate,
                static_cast<unsigned>(s1.combo_hits),
                static_cast<unsigned>(s1.combo_damage),
                static_cast<unsigned>(s1.combo_limit),
                s1.guarding ? 1 : 0, s1.wrongblock ? 1 : 0,
                s1.crushed  ? 1 : 0, s1.knockdown  ? 1 : 0,
                s2.x, s2.y, s2.vx, s2.vy, s2.ax, s2.ay,
                static_cast<int>(s2.direction),
                static_cast<unsigned>(s2.action),
                static_cast<unsigned>(s2.action_frame),
                static_cast<unsigned>(s2.hitstop),
                static_cast<unsigned>(s2.untech),
                static_cast<unsigned>(s2.hitboxes),
                static_cast<unsigned>(s2.hurtboxes),
                static_cast<int>(s2.hit_count),
                static_cast<int>(s2.hp),
                static_cast<unsigned>(s2.spirit),
                static_cast<unsigned>(s2.max_spirit),
                static_cast<unsigned>(s2.spirit_delay),
                static_cast<unsigned>(s2.timestop),
                static_cast<unsigned>(s2.ground_dashes),
                static_cast<unsigned>(s2.air_dashes),
                static_cast<int>(s2.correction),
                s2.combo_rate,
                static_cast<unsigned>(s2.combo_hits),
                static_cast<unsigned>(s2.combo_damage),
                static_cast<unsigned>(s2.combo_limit),
                s2.guarding ? 1 : 0, s2.wrongblock ? 1 : 0,
                s2.crushed  ? 1 : 0, s2.knockdown  ? 1 : 0,
                static_cast<unsigned>(slot->battle_frame));

        // Bullets. Slots past the live count are written as zeros rather than
        // left empty so every row has the same width -- a ragged CSV is the
        // kind of thing that parses fine for 10 000 rows and then does not.
        for (int p = 0; p < 2; ++p) {
            const auto& s = p ? s2 : s1;
            fprintf(m_csv, ",%u,%u,%u",
                    static_cast<unsigned>(s.projectiles),
                    static_cast<unsigned>(s.proj_hb),
                    static_cast<unsigned>(s.proj_raw));
            for (int k = 0; k < MAX_PROJECTILES; ++k) {
                const auto& pr = s.proj[k];
                fprintf(m_csv, ",%.3f,%.3f,%.4f,%.4f,%d,%u,%u",
                        pr.x, pr.y, pr.vx, pr.vy,
                        static_cast<int>(pr.direction),
                        static_cast<unsigned>(pr.action),
                        static_cast<unsigned>(pr.hitboxes));
            }
        }
        fputc('\n', m_csv);

        m_ring.releaseReadSlot();
        m_total_written.fetch_add(1, std::memory_order_relaxed);
    }

    sfe::log("VideoEncoder: draining complete (%d frames)", m_total_written.load());
    closeOutputs();
}

} // namespace sfe
