#include "sfe/player_state.hpp"

namespace sfe {

PlayerState readPlayerState(void* char_obj) {
    PlayerState s;
    if (!char_obj) return s;

    auto* base = reinterpret_cast<const char*>(char_obj);
    s.x         = *reinterpret_cast<const float*>(base + CHAR_POSITION_X_OFFSET);
    s.y         = *reinterpret_cast<const float*>(base + CHAR_POSITION_Y_OFFSET);
    s.direction = *reinterpret_cast<const int8_t*>(base + CHAR_DIRECTION_OFFSET);
    s.action    = *reinterpret_cast<const uint16_t*>(base + CHAR_ACTION_OFFSET);

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
    // The guard broke.
    s.crushed    = (a == ACT_GROUND_CRUSHED || a == ACT_AIR_CRUSHED);
    s.knockdown  = (a == ACT_KNOCKED_DOWN || a == ACT_KNOCKED_DOWN_STATIC
                    || a == ACT_GRABBED);
    return s;
}

}  // namespace sfe
