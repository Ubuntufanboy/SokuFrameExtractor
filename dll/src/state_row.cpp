#include "sfe/state_row.hpp"

#include <cstdarg>
#include <cstdio>

#include "sfe/config.hpp"     // INPUT_* bits

namespace sfe {
namespace {

// snprintf that appends, and fails sticky: one short buffer makes the whole
// row unusable rather than truncated somewhere in the middle.
struct Out {
    char*  p;
    size_t cap;
    size_t n  = 0;
    bool   ok = true;
};

void app(Out& o, const char* fmt, ...) {
    if (!o.ok) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(o.p + o.n, o.cap - o.n, fmt, ap);
    va_end(ap);
    if (k < 0 || static_cast<size_t>(k) >= o.cap - o.n) { o.ok = false; return; }
    o.n += static_cast<size_t>(k);
}

int done(const Out& o) { return o.ok ? static_cast<int>(o.n) : -1; }

} // namespace

// Moved verbatim from VideoEncoder (the format strings and their order ARE the
// sidecar format). The state columns are appended, never inserted, so every
// reader written against the old header keeps working -- `data/soku.py`
// selects columns by name and a corpus mixing both layouts must stay loadable.
int formatStateHeader(char* buf, size_t cap) {
    Out o{ buf, cap };
    app(o, "%s",
        "frame,game_frame,"
        "p1_input,p2_input,"
        "p1_up,p1_down,p1_left,p1_right,"
        "p1_a,p1_b,p1_c,p1_d,p1_change,p1_spell,"
        "p2_up,p2_down,p2_left,p2_right,"
        "p2_a,p2_b,p2_c,p2_d,p2_change,p2_spell,"
        "p1_x,p1_y,p1_vx,p1_vy,p1_ax,p1_ay,p1_dir,p1_action,p1_action_frame,p1_hitstop,p1_untech,p1_hitboxes,p1_hurtboxes,p1_hit_count,p1_hp,p1_spirit,p1_max_spirit,p1_spirit_delay,p1_timestop,p1_ground_dashes,p1_air_dashes,p1_correction,p1_combo_rate,p1_combo_hits,p1_combo_damage,p1_combo_limit,p1_guarding,p1_wrongblock,p1_crushed,p1_knockdown,"
        "p2_x,p2_y,p2_vx,p2_vy,p2_ax,p2_ay,p2_dir,p2_action,p2_action_frame,p2_hitstop,p2_untech,p2_hitboxes,p2_hurtboxes,p2_hit_count,p2_hp,p2_spirit,p2_max_spirit,p2_spirit_delay,p2_timestop,p2_ground_dashes,p2_air_dashes,p2_correction,p2_combo_rate,p2_combo_hits,p2_combo_damage,p2_combo_limit,p2_guarding,p2_wrongblock,p2_crushed,p2_knockdown,"
        "battle_frame");
    // Projectile columns, appended after everything that already existed so no
    // reader written against the previous header has to change. Emitted in a
    // loop rather than spelled out because MAX_PROJECTILES is the only place
    // the width is decided -- a literal header here would silently disagree
    // with the row writer the first time that constant moves.
    for (int p = 1; p <= 2; ++p) {
        app(o, ",p%d_proj_n,p%d_proj_hb,p%d_proj_raw", p, p, p);
        for (int k = 0; k < MAX_PROJECTILES; ++k) {
            app(o,
                ",p%d_pr%d_x,p%d_pr%d_y,p%d_pr%d_vx,p%d_pr%d_vy,"
                "p%d_pr%d_dir,p%d_pr%d_act,p%d_pr%d_hb",
                p, k, p, k, p, k, p, k, p, k, p, k, p, k);
        }
    }
    // Camera last, appended after everything that already existed. Seven
    // floats that turn every world-space label in this row into something a
    // screen-space model can be supervised against.
    app(o, "%s", ",cam_x,cam_y,cam_scale,cam_left,cam_top,cam_right,cam_bottom");
    return done(o);
}

int formatStateRow(char* buf, size_t cap, const StateRow& r) {
    Out o{ buf, cap };
    const auto  p1 = r.p1_input;
    const auto  p2 = r.p2_input;
    const auto& s1 = *r.p1;
    const auto& s2 = *r.p2;

    app(o,
        "%d,%d,%u,%u,"
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
        "%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%d,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%d,%.4f,%u,%u,%u,%d,%d,%d,%d,"
        "%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%d,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%d,%.4f,%u,%u,%u,%d,%d,%d,%d,"
        "%u",   // no separator: the projectile columns follow below
        r.row, r.frame_index,
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
        static_cast<unsigned>(r.battle_frame));

    // Bullets. Slots past the live count are written as zeros rather than
    // left empty so every row has the same width -- a ragged CSV is the
    // kind of thing that parses fine for 10 000 rows and then does not.
    for (int p = 0; p < 2; ++p) {
        const auto& s = p ? s2 : s1;
        app(o, ",%u,%u,%u",
            static_cast<unsigned>(s.projectiles),
            static_cast<unsigned>(s.proj_hb),
            static_cast<unsigned>(s.proj_raw));
        for (int k = 0; k < MAX_PROJECTILES; ++k) {
            const auto& pr = s.proj[k];
            app(o, ",%.3f,%.3f,%.4f,%.4f,%d,%u,%u",
                pr.x, pr.y, pr.vx, pr.vy,
                static_cast<int>(pr.direction),
                static_cast<unsigned>(pr.action),
                static_cast<unsigned>(pr.hitboxes));
        }
    }
    const auto& cam = *r.camera;
    app(o, ",%.3f,%.3f,%.5f,%.3f,%.3f,%.3f,%.3f",
        cam.x, cam.y, cam.scale,
        cam.left, cam.top, cam.right, cam.bottom);
    return done(o);
}

} // namespace sfe
