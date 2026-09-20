#include "sfe/player_state.hpp"

#include <windows.h>   // SEH around the projectile walk

namespace sfe {

namespace {
template <typename T>
inline T rd(const char* base, int off) {
    return *reinterpret_cast<const T*>(base + off);
}
}  // namespace

PlayerState readPlayerState(void* char_obj) {
    PlayerState s;
    if (!char_obj) return s;

    auto* base = reinterpret_cast<const char*>(char_obj);

    // Kinematics. `speed` and `gravity` are the game's own per-tick values, so
    // velocity and acceleration are read rather than differenced -- a
    // difference across presented frames would be wrong whenever the renderer
    // presents twice for one tick, which it does about fifty times a match.
    s.x  = rd<float>(base, CHAR_POSITION_X_OFFSET);
    s.y  = rd<float>(base, CHAR_POSITION_Y_OFFSET);
    s.vx = rd<float>(base, CHAR_SPEED_X_OFFSET);
    s.vy = rd<float>(base, CHAR_SPEED_Y_OFFSET);
    s.ax = rd<float>(base, CHAR_GRAVITY_X_OFFSET);
    s.ay = rd<float>(base, CHAR_GRAVITY_Y_OFFSET);
    s.direction = rd<int8_t>(base, CHAR_DIRECTION_OFFSET);

    s.action       = rd<uint16_t>(base, CHAR_ACTION_OFFSET);
    s.action_frame = rd<uint32_t>(base, CHAR_FRAME_COUNT_OFFSET);
    s.hitstop      = rd<uint16_t>(base, CHAR_HITSTOP_OFFSET);
    s.untech       = rd<uint16_t>(base, CHAR_UNTECH_OFFSET);
    // An active hitbox is the cleanest "is attacking me right now" the game
    // has, and it is what a blocking decision is actually a response to --
    // far tighter than inferring an attack from the action id.
    s.hitboxes   = rd<uint8_t>(base, CHAR_HITBOX_COUNT_OFFSET);
    s.hurtboxes  = rd<uint8_t>(base, CHAR_HURTBOX_COUNT_OFFSET);
    s.hit_count  = rd<int8_t>(base, CHAR_HIT_COUNT_OFFSET);

    s.hp            = rd<int16_t>(base, CHAR_HP_OFFSET);
    s.spirit        = rd<uint16_t>(base, CHAR_SPIRIT_OFFSET);
    s.max_spirit    = rd<uint16_t>(base, CHAR_MAX_SPIRIT_OFFSET);
    s.spirit_delay  = rd<uint16_t>(base, CHAR_SPIRIT_DELAY_OFFSET);
    s.timestop      = rd<uint16_t>(base, CHAR_TIMESTOP_OFFSET);
    s.ground_dashes = rd<uint8_t>(base, CHAR_GROUND_DASH_OFFSET);
    s.air_dashes    = rd<uint8_t>(base, CHAR_AIR_DASH_OFFSET);
    s.correction    = rd<int8_t>(base, CHAR_CORRECTION_OFFSET);

    s.combo_rate   = rd<float>(base, CHAR_COMBO_RATE_OFFSET);
    s.combo_hits   = rd<uint16_t>(base, CHAR_COMBO_HITS_OFFSET);
    s.combo_damage = rd<uint16_t>(base, CHAR_COMBO_DAMAGE_OFFSET);
    s.combo_limit  = rd<uint16_t>(base, CHAR_COMBO_LIMIT_OFFSET);

    const uint16_t a = s.action;
    // Correct guard, in the air or on the ground. AIR_GUARD sits between the
    // two blockstun ranges, which is why this is not one contiguous test.
    s.guarding   = (a >= ACT_RIGHTBLOCK_FIRST && a <= ACT_RIGHTBLOCK_LAST)
                   || a == ACT_AIR_GUARD;
    // Guarded in the right direction at the wrong HEIGHT -- the range is
    // ACTION_WRONGBLOCK_{HIGH,LOW}_*_BLOCKSTUN, beside RIGHTBLOCK's. Verified
    // on a real capture: 96.4% of these frames hold away from the opponent,
    // the same as a right block, so it is not a directional mistake. Kept
    // separate from `guarding` because it is still a *failure* of gap
    // detection -- it costs spirit and is the road to a crush.
    s.wrongblock = (a >= ACT_WRONGBLOCK_FIRST && a <= ACT_WRONGBLOCK_LAST);
    s.crushed    = (a == ACT_GROUND_CRUSHED || a == ACT_AIR_CRUSHED);
    s.knockdown  = (a == ACT_KNOCKED_DOWN || a == ACT_KNOCKED_DOWN_STATIC
                    || a == ACT_GRABBED);
    return s;
}

// -------------------------------------------------------------------------
// Projectiles
// -------------------------------------------------------------------------
namespace {

// The raw pointer walk, kept in its own leaf function for two reasons. It is
// the only code in the capture that follows pointers it did not get from a
// fixed address, so it is the only code that can fault on a stale object -- and
// __try/__except cannot live in a function that has anything needing unwinding,
// which is why everything here is a POD and the output is a caller-supplied
// array.
//
// Selection happens INSIDE the walk rather than over a scratch copy, so the
// counts are exact over the whole list however long it is. A scratch buffer
// would have had to be as large as the longest list to keep `hb` honest, and
// the longest list measured is 213 -- at which point the buffer is the thing
// being sized by guesswork instead of the output.
//
// Returns the total number of objects on the list (which may far exceed `cap`),
// writing the best `cap` of them into `out` ordered danger-first. Returns -1 if
// any dereference faulted.
int walkProjectiles(const char* base, ProjectileState* out, int cap,
                    float tx, float ty, uint32_t* raw_size, int* hb_count) {
    int total = 0, kept = 0;
    *raw_size = 0;
    *hb_count = 0;

    // Sort keys, parallel to `out`. Tier 0 is "has a live hitbox", so the
    // comparison is lexicographic on (tier, distance) and one memory-cheap
    // insertion keeps the array ordered.
    float   keyd[MAX_PROJECTILES];
    uint8_t keyt[MAX_PROJECTILES];

    __try {
        const char* mgr = rd<const char*>(base, CHAR_OBJLIST_OFFSET);
        if (!mgr) return 0;

        const char* lst  = mgr + OBJLIST_LIST_OFFSET;
        const char* head = rd<const char*>(lst, LIST_HEAD_OFFSET);
        const uint32_t size = rd<uint32_t>(lst, LIST_SIZE_OFFSET);
        if (!head || size == 0) return 0;
        *raw_size = size;
        // Refuse rather than follow. A plausible-looking but wrong pointer
        // gives a huge size here, and walking it is how a capture wedges.
        if (size > PROJ_LIST_SANITY) return 0;

        // head is a sentinel node; the first real element is head->next, and
        // the list is circular, so arriving back at head ends it.
        const char* node = rd<const char*>(head, NODE_NEXT_OFFSET);
        for (uint32_t k = 0; k < size && node && node != head; ++k) {
            const char* obj = rd<const char*>(node, NODE_VAL_OFFSET);
            node = rd<const char*>(node, NODE_NEXT_OFFSET);
            if (!obj) continue;
            ++total;

            const float px = rd<float>(obj, CHAR_POSITION_X_OFFSET);
            const float py = rd<float>(obj, CHAR_POSITION_Y_OFFSET);
            const uint8_t hb = rd<uint8_t>(obj, CHAR_HITBOX_COUNT_OFFSET);
            if (hb) ++*hb_count;

            const uint8_t tier = hb ? 0 : 1;
            const float ddx = px - tx, ddy = py - ty;
            const float dist = ddx * ddx + ddy * ddy;   // squared: rank only

            // Worse than everything already kept, and the array is full.
            if (kept == cap
                && (keyt[cap - 1] < tier
                    || (keyt[cap - 1] == tier && keyd[cap - 1] <= dist)))
                continue;

            int pos = (kept < cap) ? kept : cap - 1;   // overwrite = discard
            while (pos > 0
                   && (keyt[pos - 1] > tier
                       || (keyt[pos - 1] == tier && keyd[pos - 1] > dist))) {
                out[pos]  = out[pos - 1];
                keyd[pos] = keyd[pos - 1];
                keyt[pos] = keyt[pos - 1];
                --pos;
            }
            out[pos].x  = px;
            out[pos].y  = py;
            out[pos].vx = rd<float>(obj, CHAR_SPEED_X_OFFSET);
            out[pos].vy = rd<float>(obj, CHAR_SPEED_Y_OFFSET);
            out[pos].action    = rd<uint16_t>(obj, CHAR_ACTION_OFFSET);
            out[pos].direction = rd<int8_t>(obj, CHAR_DIRECTION_OFFSET);
            out[pos].hitboxes  = hb;
            keyd[pos] = dist;
            keyt[pos] = tier;
            if (kept < cap) ++kept;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return total;
}

inline uint8_t clamp255(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

void readProjectiles(void* char_obj, float target_x, float target_y,
                     PlayerState* out) {
    if (!out) return;
    out->projectiles = 0;
    out->proj_hb     = 0;
    out->proj_raw    = 0;
    for (int k = 0; k < MAX_PROJECTILES; ++k) out->proj[k] = ProjectileState{};
    if (!char_obj) return;

    uint32_t raw = 0;
    int hb = 0;
    const int found = walkProjectiles(reinterpret_cast<const char*>(char_obj),
                                      out->proj, MAX_PROJECTILES,
                                      target_x, target_y, &raw, &hb);
    // Record the reported length even on a refusal or a fault, so the sidecar
    // distinguishes "no objects" from "the walk declined to follow this".
    out->proj_raw = clamp255(static_cast<int>(raw > 255u ? 255u : raw));
    if (found <= 0) return;   // -1 is a faulted walk: report no objects
    out->projectiles = clamp255(found);
    out->proj_hb     = clamp255(hb);
}

}  // namespace sfe

namespace {
// SokuLib: ADDR_CAMERA_OBJ. A plain global, not a pointer to be chased.
constexpr uintptr_t ADDR_CAMERA_OBJ = 0x00898600;
constexpr uintptr_t CAM_TRANSLATE   = 0x0C;
constexpr uintptr_t CAM_SCALE       = 0x14;
constexpr uintptr_t CAM_LEFT_EDGE   = 0x5C;
}  // namespace

sfe::CameraState sfe::readCamera() {
    CameraState c;
    const auto base = reinterpret_cast<const unsigned char*>(ADDR_CAMERA_OBJ);
    if (!base) return c;
    const auto f = [&](uintptr_t off) {
        return *reinterpret_cast<const float*>(base + off);
    };
    c.x      = f(CAM_TRANSLATE);
    c.y      = f(CAM_TRANSLATE + 4);
    c.scale  = f(CAM_SCALE);
    c.left   = f(CAM_LEFT_EDGE);
    c.top    = f(CAM_LEFT_EDGE + 4);
    c.right  = f(CAM_LEFT_EDGE + 8);
    c.bottom = f(CAM_LEFT_EDGE + 12);
    // Validate the HORIZONTAL extent only, and do not assume a vertical
    // ordering. The first version of this also required `bottom > top` and
    // zeroed every row of a 12 788-frame capture: Soku's world y increases
    // UPWARD, so `topEdge` holds the larger value and a perfectly good camera
    // failed the check. A guard that silently blanks the field it is
    // protecting is worse than no guard, so this one only rejects what it can
    // actually justify -- a zero-width rectangle, which nothing can divide by.
    if (!(c.right > c.left)) {
        return CameraState{};
    }
    return c;
}
