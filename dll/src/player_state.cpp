#include "sfe/player_state.hpp"

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

}  // namespace sfe
