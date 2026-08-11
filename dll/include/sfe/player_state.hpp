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
// THE ADDED FIELDS ARE VERIFIED TOO, 2026-08-11
// ---------------------------------------------
// pipeline/verify_extended.py, on a 10 076-frame capture. Each check is a
// relationship the field could not satisfy by accident, not a plausibility
// test:
//
//   world dx tracks vx * direction at err 1.59 against 6.20 for the world
//     frame -- which is how the facing convention above was found
//   action_frame advances on 92-94% of same-action ticks and RESETS on 100%
//     of 506 action changes
//   the opponent's hp fell on 97.3% of the 112 ticks where combo_damage rose
//     -- this one couples three offsets across two players
//   hitstop follows an active hitbox 68 times and precedes it once
//   0 of 20 152 frames violate 0 <= spirit <= max_spirit
//   hp spans exactly [0, 10000] from a 10000 start
#pragma once

#include <cstdint>

namespace sfe {

// ---- offsets into the character object -----------------------------------
constexpr int CHAR_POSITION_X_OFFSET = 0x0EC;  // float
constexpr int CHAR_POSITION_Y_OFFSET = 0x0F0;  // float
// FACING-RELATIVE, not world space. Measured on a real capture: world dx
// against vx has a mean error of 6.20 where mean |dx| is 4.51, and against
// vx * direction it is 1.59 -- four times better and unambiguous. The training
// side MUST multiply by `direction`; a velocity whose sign flips with facing
// makes "moving away" unlearnable, which is the exact failure this redesign
// exists to escape.
constexpr int CHAR_SPEED_X_OFFSET    = 0x0F4;  // float, per tick, facing-frame
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

// ---- projectiles ---------------------------------------------------------
// `hitboxes` above does NOT cover these. It is the character's own attack-box
// count -- it fires for a melee jab and reads zero for a fireball already in
// flight, which the hitbox_precedes check confirms by tying it to the
// opponent's hitstop. Bullets are separate objects, and in a game where half
// the cast fights at range they are most of what a defender is reacting to.
//
// WHAT IS ACTUALLY IN THE LIST
// ----------------------------
// Not just bullets. Measured over three replays (41 000 player-frames), the
// list holds up to 213 objects at once and the median frame has no dangerous
// object on it at all: most members are visual effects. They are separable, and
// by exactly the field that matters -- a live `hitboxes` -- which is why the
// slots below are filled danger-first rather than by raw distance. An earlier
// version ordered purely by distance and spent all eight slots on decorative
// sparks while a laser went unlogged.
//
// The action ids confirm the two populations are distinct objects rather than
// a misread: they run 800-998, disjoint from the character actions above, and
// they separate cleanly. Action 801 carries a hitbox on 62.8% of its frames
// and moves at |v| 9.98; action 805, the single commonest member, carries one
// on 0.0% and moves at 0.386.
//
// THE WALK
// --------
// Every bullet a character owns hangs off its own list:
//
//   charObj + 0x6F8   -> ObjListManager&      (a reference: stored as a ptr)
//   objListMgr + 0x58 -> LinkedList<ObjectManager>
//                          +0x00 alloc, +0x04 head, +0x08 size
//   node              -> +0x00 next, +0x04 prev, +0x08 val (ObjectManager*)
//
// The head node is a sentinel, so iteration starts at head->next and the size
// field bounds it. This is the one part of the capture that dereferences
// pointers it did not get from a fixed address, so it is bounded, null-checked
// at every hop, and wrapped in SEH -- see readProjectiles.
//
// WHY THE FIELD OFFSETS ARE THE SAME AS A CHARACTER'S
// ---------------------------------------------------
// ProjectileManager begins with `ObjectManager objectBase` at 0x000, exactly
// as CharacterManager does. A bullet's position, speed, action and hitbox
// count are therefore at the offsets already verified above, and they are
// verified *again* for bullets by pipeline/verify_extended.py rather than
// assumed from the shared base.
constexpr int CHAR_OBJLIST_OFFSET    = 0x6F8;  // ObjListManager&
constexpr int OBJLIST_LIST_OFFSET    = 0x58;   // LinkedList<ObjectManager>
constexpr int LIST_HEAD_OFFSET       = 0x04;   // Node*, a sentinel
constexpr int LIST_SIZE_OFFSET       = 0x08;   // uint32
constexpr int NODE_NEXT_OFFSET       = 0x00;
constexpr int NODE_VAL_OFFSET        = 0x08;   // ObjectManager*
// Nothing past the shared ObjectManager base (which ends at 0x348) is read.
// ProjectileManager continues with characterIndex at 0x34A and isActive at
// 0x34C, and an earlier version filtered on isActive -- but it removed nothing
// whatsoever: live count equalled list length on 12 122 of 12 122
// player-frames. The list is maintained, so a dead object is unlinked rather
// than flagged. Reading it was therefore a dereference past the only layout
// every member is guaranteed to share, in exchange for no information, and it
// is gone.

// A list longer than this is not a list -- it is a garbage pointer that
// happened to be readable, and the walk refuses it rather than following it.
// Measured maximum over three replays is 213, so this rejects nonsense without
// rejecting a real frame; `p*_proj_raw` records the length the game reported
// even when the walk refuses, so a refusal is visible in the sidecar rather
// than silent.
constexpr uint32_t PROJ_LIST_SANITY = 512;
// How many objects are written per player per frame, danger-first.
//
// SIZED FROM THE EXACT COUNT, AFTER THE FIRST ATTEMPT MEASURED ITSELF
// -------------------------------------------------------------------
// The first sizing read "objects carrying a live hitbox never exceed 8" and
// was an artefact: it counted hitboxes among the eight LOGGED slots, so it
// could not have returned more than eight whatever the game did. `proj_hb`,
// which counts over the whole list, says the real distribution is
//
//     p50 0    p90 4-5    p99 10-25    max 29 / 96 / 31
//
// -- median nothing, and rare curtain patterns two orders of magnitude above
// it. No slot count covers the tail; 24 covers the 99th percentile on two of
// three replays and most of it on the third, and past that the marginal bullet
// is one of ninety-six identical ones in a spellcard wall, where "there are
// ninety-six" is the information and the individual is not.
//
// Chosen high rather than tight on purpose. Three replays do not sample twenty
// characters, the count columns make any truncation visible after the fact,
// and a bullet that did not fit cannot be recovered without paying the
// 29-hour capture again.
constexpr int MAX_PROJECTILES = 24;

// One bullet. Deliberately 20 bytes: sixteen of position and velocity plus the
// two things that say what it is and whether it can hurt you right now.
struct ProjectileState {
    float    x = 0.0f;
    float    y = 0.0f;
    // FACING-RELATIVE, exactly as the character's is -- a separate question
    // about a separate object, so it was measured separately rather than
    // inherited. On matched pairs of a moving object with a live hitbox, the
    // world frame misses by 19.3 and the facing frame by 0.220 against a mean
    // |dx| of 16.4. As with the character, the training side MUST multiply by
    // `direction`.
    //
    // Not every object moves by this field: scripted and attached objects have
    // their position written directly, so `vx` is near zero while they travel.
    // That is why verify_extended.py measures it on objects that are actually
    // moving instead of pooling them with the decorations.
    float    vx = 0.0f;
    float    vy = 0.0f;
    uint16_t action = 0;         // which bullet: an amulet is not a laser
    int8_t   direction = 0;
    uint8_t  hitboxes = 0;       // >0 means it is live and can connect
};

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

    // --- objects this player owns -----------------------------------------
    // Three counts because they answer three different questions, all of them
    // exact even when the slot array truncates.
    //
    //   projectiles  everything on the list -- reaches 213, mostly effects
    //   proj_hb      how many can hurt the opponent RIGHT NOW. This is the
    //                one a defender cares about, and the one that stayed
    //                under eight on every frame ever measured.
    //   proj_raw     the length the game itself reported, recorded even when
    //                the sanity cap makes the walk refuse -- so a refusal
    //                reads as (projectiles 0, proj_raw large) rather than as
    //                an empty list.
    uint8_t projectiles = 0;
    uint8_t proj_hb     = 0;
    uint8_t proj_raw    = 0;
    // Ordered danger-first: everything with a live hitbox, nearest to the
    // opponent first, then everything else nearest first.
    ProjectileState proj[MAX_PROJECTILES];
};

// Reads one character object. Null-safe: the battle manager can be null between
// scenes, and `readPlayerInput` already returns 0 in that case rather than
// faulting, so this matches.
PlayerState readPlayerState(void* char_obj);

// Fills `out`'s projectile fields by walking `char_obj`'s object list, keeping
// the MAX_PROJECTILES bullets nearest to (target_x, target_y) -- the player
// they are flying at.
//
// Separate from readPlayerState because it needs the *other* player's
// position, which is only known once both have been read. Never throws and
// never faults: the walk runs under SEH and yields a zero-filled result if any
// dereference is bad, so a garbage pointer between scenes costs one frame's
// bullets rather than the whole capture.
void readProjectiles(void* char_obj, float target_x, float target_y,
                     PlayerState* out);

}  // namespace sfe
