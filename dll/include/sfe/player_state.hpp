// Per-tick ground truth about both players, read from game memory.
//
// WHY THIS EXISTS
// ---------------
// The world model is trained from pixels and inputs, and it does not learn
// where the characters are. Measured in SokuBot with `scripts/spatial_probe.py`:
// mirroring the play area horizontally while holding the HUD fixed is
// undetectable from the encoder's latent (AUC 0.540 against a 0.956 ceiling),
// and the predictor's damage forecast is the same whether the defender holds
// LEFT or RIGHT. Guarding in Hisoutensoku is holding *away from the opponent*,
// so a model that cannot see which side the opponent is on cannot represent
// blocking -- which is the mechanic the whole matchup runs through.
//
// Two objectives have now failed to recover it from pixels alone at this scale:
// plain JEPA prediction, and JEPA plus an inverse-dynamics term. Both improved
// the representation; neither made guarding predict less damage.
//
// So this stops asking the model to infer what the game can simply be asked.
// The constraint the project runs under is unchanged and is about *inference*:
// the policy at play time consumes pixels and its own inputs, nothing else.
// These labels never reach it. They train a world model, which is the same
// asymmetric arrangement the HUD head already uses -- the difference is only
// that the HUD is legible in pixels and position is not.
//
// NO NEW GAMEPLAY IS NEEDED
// -------------------------
// Captures are driven by .rep files, so the existing corpus can be re-run to
// produce these labels for frames we already have. The pixels do not change;
// only the sidecar gains columns.
//
// OFFSETS
// -------
// All from third_party/SokuLib/src/CharacterManager.hpp, which documents them
// inline against the game's own ADDR_* names. They are offsets into the
// character object that `BattleManager + 0x0C / 0x10` points at -- the same
// object `readPlayerInput` already reads at 0x754. Absolute addresses are only
// valid for the build `CheckVersion()` accepts; these are *relative* and so
// depend only on the struct layout, which is why they are safer than the
// scene-pointer constants above them.
//
// VERIFIED AGAINST THE RUNNING GAME, 2026-08-08
// ---------------------------------------------
// A documented offset that is wrong reads a neighbouring field and produces
// numbers of exactly the right shape, which nothing downstream would reject.
// So they were checked against the input columns, which have been trusted
// since the first capture -- `pipeline/verify_state.py`, on a 10 073-frame
// capture of replay 5262777:
//
//   holding RIGHT moves x by +3.20/frame, LEFT by -4.49       (x is horizontal)
//   93.3% of 20 136 frames have `direction` pointing at the
//     other player                                            (direction is facing)
//   87.9% of 1 118 guarding frames hold away from the opponent (the guard range)
//   guard occurs on 18.9% of frames holding away against 1.9%
//     holding toward -- 10.1x                                 (not a stuck field)
//   a grounded UP press peaks y +92.7 within 12 frames        (y is height, up)
//
// Positions run about 40..1240, so the stage is ~1280 wide with the origin at
// one edge, not centred. Separation reached +-1200.
#pragma once

#include <cstdint>

namespace sfe {

// Offsets into the character object. Cited above; do not re-derive by guessing.
constexpr int CHAR_POSITION_X_OFFSET = 0x0EC;  // float
constexpr int CHAR_POSITION_Y_OFFSET = 0x0F0;  // float
constexpr int CHAR_DIRECTION_OFFSET  = 0x104;  // int8: which way the char faces
constexpr int CHAR_ACTION_OFFSET     = 0x13C;  // uint16: SokuLib::Action

// Action ranges that matter, from third_party/SokuLib/src/Action.hpp. These are
// the mechanic itself, not a proxy for it: the attacker's whole game is to make
// the defender drop guard (a hit) or guard the wrong way (WRONGBLOCK, or a
// crush when the guard finally breaks).
constexpr uint16_t ACT_RIGHTBLOCK_FIRST = 150;  // ..157, correct guard
constexpr uint16_t ACT_RIGHTBLOCK_LAST  = 157;
constexpr uint16_t ACT_AIR_GUARD        = 158;
// WRONGBLOCK is the wrong *height*, not the wrong direction: the full names are
// ACTION_WRONGBLOCK_{HIGH,LOW}_*_BLOCKSTUN, beside RIGHTBLOCK's. Measured on a
// real capture, 96.4% of these frames hold away exactly as a right block does.
constexpr uint16_t ACT_WRONGBLOCK_FIRST = 159;  // ..166
constexpr uint16_t ACT_WRONGBLOCK_LAST  = 166;
constexpr uint16_t ACT_GROUND_CRUSHED   = 143;  // guard broke
constexpr uint16_t ACT_AIR_CRUSHED      = 145;
constexpr uint16_t ACT_KNOCKED_DOWN     = 97;   // ..98
constexpr uint16_t ACT_KNOCKED_DOWN_STATIC = 98;
constexpr uint16_t ACT_GRABBED          = 100;

// One player's state for one tick. Deliberately flat and small: it is written
// to the sidecar CSV once per frame per player, and the encoder's ring buffer
// copies it, so anything that allocates here would be paid 43 million times.
struct PlayerState {
    float    x = 0.0f;
    float    y = 0.0f;
    int8_t   direction = 0;
    uint16_t action = 0;
    // Derived, so the training side does not have to carry the action table.
    // `guarding` is *correct* guard only; `wrongblock` and `crushed` are the
    // two ways guarding fails, and telling them apart is the point -- one is a
    // false positive in gap detection and the other a false negative.
    bool guarding   = false;
    bool wrongblock = false;
    bool crushed    = false;
    bool knockdown  = false;
};

// Reads one character object. Null-safe: the battle manager can be null between
// scenes, and `readPlayerInput` already returns 0 in that case rather than
// faulting, so this matches.
PlayerState readPlayerState(void* char_obj);

}  // namespace sfe
