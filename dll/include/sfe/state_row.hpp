#pragma once

#include <cstddef>
#include <cstdint>

#include "sfe/player_state.hpp"

namespace sfe {

// One captured tick, as the capture CSV and the vs-COM agent channel both write it.
struct StateRow {
    int                row;            // the writer's own row counter (`frame`)
    int                frame_index;    // the session's capture counter (`game_frame`)
    uint32_t           battle_frame;   // BattleManager + 0x004
    uint16_t           p1_input;
    uint16_t           p2_input;
    const PlayerState* p1;
    const PlayerState* p2;
    const CameraState* camera;
};

// The capture CSV's header and rows: ONE definition for every writer. The sidecar is what every
// model in SokuBot was trained on, and the agent channel is what a policy reads live; if the two
// formatted a row differently, the policy would be acting on an observation it never trained on,
// silently. Neither output ends in a newline. Both return the length written, or -1 if `cap` is
// too small (nothing usable is left in `buf` then).
int formatStateHeader(char* buf, size_t cap);
int formatStateRow(char* buf, size_t cap, const StateRow& r);

// Big enough for either: the header is ~6 KB and a row ~3 KB at MAX_PROJECTILES = 24.
constexpr size_t STATE_ROW_CAP = 16384;

} // namespace sfe
