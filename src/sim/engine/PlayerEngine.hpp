// PlayerEngine — PlayerObject gameplay functions TRANSCRIBED from the GD 2.2081 binary.
//
// This is the core of the engine-decoding program: not an approximation calibrated
// against captures, but the engine's own code, function by function, operating on the
// engine's own fields (EngineSnap, generated from the binary's compiler-exact layout).
//
// Method, for every function here:
//   * structure from the named Ghidra decompile (decompGD/GeometryDash.named.c);
//   * every arithmetic expression checked against the DISASSEMBLY for its precision and
//     operand order — Ghidra's C writes `(double)(8.0 / f)` where the binary does a FLOAT
//     `divss`, and single-precision multiplication is not associative, so the C alone is
//     not enough to be bit-exact;
//   * every call whose arguments Ghidra dropped (`setYVelocity()`, `round()`, the dt passed
//     to updateJump) recovered from the disassembly;
//   * every constant read from .rdata, cited by address.
//
// Scope: classic (non-platformer) mode. Platformer-only branches are omitted and marked.
// Not modelled, by design: presentation (particles, animations, streaks, sounds, game
// events) and state that is not deterministic (the anti-cheat rand() fields, wall-clock
// timestamps). Those fields are listed in kNonPhysicsFields so a comparison can skip them.
#pragma once
#include "EngineSnap.hpp"

namespace gdsim::engine {

// Engine functions called from the ones ported here but not ported yet. Injected so each
// function can be ported and tested on its own; a null entry means that path is not
// available yet (the caller reports it rather than guessing).
struct Callees {
    void (*spiderTestJump)(EngineSnap&) = nullptr;   // PlayerObject::spiderTestJump 0x140394340
};

// PlayerObject::setYVelocity 0x140388d10. EVERY write through it is quantised to 3 decimals:
// v = trunc(v) + round((v - trunc(v)) * 1000) / 1000. This is where the engine's
// "3-decimal velocities" come from — not a separate rounding pass.
void setYVelocity(EngineSnap& s, double v);

// The same quantisation, for the places that store m_yVelocity directly with it inlined.
double quantizeYVelocity(double v);

// PlayerObject::playerIsFallingBugged 0x14039a430 (pure query).
bool playerIsFallingBugged(const EngineSnap& s);

// PlayerObject::flipGravity 0x14039a1d0.
void flipGravity(EngineSnap& s, bool flip, bool noEffects);

// PlayerObject::updateJump 0x14038b900. `dt` is the value the engine passes, which is NOT
// the step's dt: PlayerObject::update calls it with dt * 0.9f (0x140388df3, constant
// 0.9f @0x140622bd8) — so 0.225 for a 0.25 sub-step. updateJump changes velocity only;
// the position is integrated afterwards by PlayerObject::update with that same dt * 0.9.
void updateJump(EngineSnap& s, float dt, const Callees& callees = {});

// PlayerObject::update 0x140388d80 — one 240 Hz step of the player BEFORE collisions: clamp,
// updateJump(dt * 0.9f), force blocks, then the position: dy = (dt*0.9) * m_yVelocity (the
// NEW velocity), dx = playerSpeed * speedMultiplier * dt (the FULL dt); the wave instead
// moves |dx| diagonally (x2 mini). Ends by decrementing the m_state* frame counters and
// clearing this step's touch flags. `dt` is the step's dt as GJBaseGameLayer passes it.
void update(EngineSnap& s, float dt, const Callees& callees = {});

// Fields a comparison must ignore: presentation, or not deterministic.
extern const char* const kNonPhysicsFields[];

} // namespace gdsim::engine
