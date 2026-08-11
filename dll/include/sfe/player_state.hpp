// Per-tick ground truth about both players, read from game memory.
//
// WHY THIS FILE GREW
// ------------------
// The first version logged position, facing and the guard flags, to fix a world
// model that could not represent blocking. It did not fix it. Five objectives
// failed against the same representation -- JEPA, inverse dynamics,
// class-balanced IDM, direct dx supervision over 2003 replays, and a
// play-area-mirror augmentation built specifically to forbid the HUD shortcut.
// The spatial probe never moved past 0.62 against a 0.958 ceiling, and the
// reward stayed flat in the one dimension the mechanic lives in: forcing the
// defender to hold AWAY rather than TOWARD was worth -0.00025 +- 0.00761.
//
// The conclusion drawn from that is architectural. The model no longer predicts
// latents of pixels at all: the state below IS the representation, the rollout
// happens in it, and the encoder's whole job is to read it off the screen. An
// encoder cannot drop the fighters when the fighters are the target, and
// "away" is expressible because it is a subtraction of two coordinates.
//
// So this logs everything the game will tell us, in ONE pass. A capture of the
// 2003-replay corpus costs about 29 hours; leaving a field out to be tidy buys
// nothing and risks paying that again. Anything plausibly useful is here.
//
// OFFSETS
// -------
// Every one is documented inline in third_party/SokuLib/src/CharacterManager.hpp
// against the game's own ADDR_* names, and they are offsets into the character
// object that `BattleManager + 0x0C / 0x10` points at -- the same object
// `readPlayerInput` reads at 0x754. They are *relative*, so they depend only on
// the struct layout rather than on the build CheckVersion() accepts.
//
// VERIFIED AGAINST THE RUNNING GAME, 2026-08-08 (the original four)
// -----------------------------------------------------------------
// pipeline/verify_state.py, over six replays and 66 380 frames:
//
//   holding RIGHT moves x by +1.9..+3.7/frame, LEFT by -1.0..-4.5
//   90.6-95.3% of frames have `direction` pointing at the opponent
//   89-98% of guarding frames hold away, 97-100% of wrong-block frames
//   guard is 4.9x-13.1x commoner while holding away than toward
//   a grounded UP press peaks y +92.7 within twelve frames
//
// The fields added since are NOT yet verified against the game. Every one of
// them needs a prediction in verify_state.py before it is trained on -- a wrong
// offset reads a neighbouring field and produces numbers of exactly the right
// shape, which is how this file's own history reads.
#pragma once

#include <cstdint>

namespace sfe {

// ---- offsets into the character object -----------------------------------
constexpr int CHAR_POSITION_X_OFFSET = 0x0EC;  // float
constexpr int CHAR_POSITION_Y_OFFSET = 0x0F0;  // float
constexpr int CHAR_SPEED_X_OFFSET    = 0x0F4;  // float, per tick
constexpr int CHAR_SPEED_Y_OFFSET    = 0x0F8;  // float
constexpr int CHAR_GRAVITY_X_OFFSET  = 0x0FC;  // float; the acceleration term
constexpr int CHAR_GRAVITY_Y_OFFSET  = 0x100;  // float
constexpr int CHAR_DIRECTION_OFFSET  = 0x104;  // int8: which way the char faces
constexpr int CHAR_ACTION_OFFSET     = 0x13C;  // uint16: SokuLib::Action
constexpr int CHAR_FRAME_COUNT_OFFSET = 0x144; // uint32: frames into the action
constexpr int CHAR_HP_OFFSET         = 0x184;  // int16, exact -- not the HUD
constexpr int CHAR_HIT_COUNT_OFFSET  = 0x194;  // int8
constexpr int CHAR_HITSTOP_OFFSET    = 0x196;  // uint16
constexpr int CHAR_HITBOX_COUNT_OFFSET  = 0x1CB;  // uint8: >0 means ATTACKING
constexpr int CHAR_HURTBOX_COUNT_OFFSET = 0x1CC;  // uint8
constexpr int CHAR_GROUND_DASH_OFFSET = 0x49A; // uint8
constexpr int CHAR_AIR_DASH_OFFSET    = 0x49B; // uint8
constexpr int CHAR_SPIRIT_OFFSET      = 0x49E; // uint16, exact -- probe R2 0.26
constexpr int CHAR_MAX_SPIRIT_OFFSET  = 0x4A0; // uint16
constexpr int CHAR_SPIRIT_DELAY_OFFSET = 0x4A2; // uint16
constexpr int CHAR_TIMESTOP_OFFSET    = 0x4A8; // uint16
constexpr int CHAR_CORRECTION_OFFSET  = 0x4AD; // int8: damage correction
constexpr int CHAR_COMBO_RATE_OFFSET  = 0x4B0; // float
constexpr int CHAR_COMBO_HITS_OFFSET  = 0x4B4; // uint16
constexpr int CHAR_COMBO_DAMAGE_OFFSET = 0x4B6; // uint16
constexpr int CHAR_COMBO_LIMIT_OFFSET = 0x4B8; // uint16
constexpr int CHAR_UNTECH_OFFSET      = 0x4BA; // uint16: frames until recovery

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

// One player's state for one tick. Deliberately flat and POD: it is written to
// the sidecar once per frame per player and copied through the ring buffer, so
// anything that allocates here would be paid 43 million times.
struct PlayerState {
    // --- kinematics: the whole reason for the redesign --------------------
    float    x = 0.0f;
    float    y = 0.0f;
    float    vx = 0.0f;          // the game's own speed, not a difference
    float    vy = 0.0f;
    float    ax = 0.0f;          // gravity/acceleration term
    float    ay = 0.0f;
    int8_t   direction = 0;

    // --- what the character is doing --------------------------------------
    uint16_t action = 0;
    uint32_t action_frame = 0;   // frames into the current action
    uint16_t hitstop = 0;
    uint16_t untech = 0;         // frames before they can act again
    uint8_t  hitboxes = 0;       // >0 means an active attack exists RIGHT NOW
    uint8_t  hurtboxes = 0;
    int8_t   hit_count = 0;

    // --- resources --------------------------------------------------------
    int16_t  hp = 0;             // exact; the HUD probe managed R2 0.9
    uint16_t spirit = 0;         // exact; the HUD probe managed R2 0.26
    uint16_t max_spirit = 0;
    uint16_t spirit_delay = 0;
    uint16_t timestop = 0;
    uint8_t  ground_dashes = 0;
    uint8_t  air_dashes = 0;
    int8_t   correction = 0;

    // --- combo in flight --------------------------------------------------
    float    combo_rate = 0.0f;
    uint16_t combo_hits = 0;
    uint16_t combo_damage = 0;
    uint16_t combo_limit = 0;

    // --- derived, so the training side does not carry the action table ----
    bool guarding   = false;     // correct guard, ground or air
    bool wrongblock = false;     // right direction, wrong height
    bool crushed    = false;
    bool knockdown  = false;
};

// Reads one character object. Null-safe: the battle manager can be null between
// scenes, and `readPlayerInput` already returns 0 in that case rather than
// faulting, so this matches.
PlayerState readPlayerState(void* char_obj);

}  // namespace sfe
