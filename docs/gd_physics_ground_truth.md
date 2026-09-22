# GD 2.2081 physics — ground truth from the binary

Everything here was read out of `GeometryDash.exe` (Windows 2.2081) itself, either
as disassembly or as raw `.rdata` bytes. Nothing is fitted, measured, or inferred
from captures. Where a capture and this file disagree, **this file wins** — a
capture sees GD's 3-decimal velocity quantisation and cannot separate the true
rate from the rounded one (that mistake is documented below, it cost real
accuracy).

## Tooling that produced this

| tool | what it does |
| --- | --- |
| `tools/bromamap.py map` | parses `bindings-main/bindings/2.2081/*.bro` into `tools/gd_symbols.txt` (5151 `va -> Class::method`) |
| `tools/bromamap.py annotate <in.c> <out.c>` | rewrites every `FUN_<va>` in a Ghidra export to `Class__method__<va>` (27047 renamed) |
| `tools/bromamap.py find <regex>` / `at <va>` | look a symbol up by name or address |
| `tools/fieldname.py extract <f.c> <a> <b>` | annotates raw byte offsets in a snippet with real member names from `tools/gd_fields.txt` |
| `tools/gd_fields.txt` | 70 **compiler-exact** `PlayerObject`/`GJBaseGameLayer` field offsets |

`va = 0x140000000 + <win rva from the .bro>`.

The field offsets are not guessed from declaration order. They come from MSVC
itself: compile `template<size_t N> struct OFFSET_IS;` plus
`static OFFSET_IS<offsetof(PlayerObject, m_x)> probe;` against the vendored Geode
header using the project's own toolchain, and read N out of the resulting
`error C2079`. Must be MSVC — struct layout is ABI-dependent and GD is
MSVC-built. To regenerate, append such a block to any file in the mod build, run
`cmake --build build --config RelWithDebInfo --target Pathfinder`, scrape the
errors, then delete the block.

## Unit conversions

| quantity | real -> gdsim |
| --- | --- |
| velocity | `x 54` |
| acceleration | `x 12960` on the per-step dv, i.e. `x 2916` on the rate (`0.225 dt * 54 * 240`) |
| X position rate | `m_playerSpeed * m_speedMultiplier * 60` |
| `updateJump`'s `dt` param | `0.225` |

## Per-tier table (`PlayerObject::updateTimeMod`, 0x1403a0d50)

Stored as raw 8-byte immediates; transcribed bit-for-bit.

| tier | `m_playerSpeed` | `m_yStart` | `m_gravity` | `m_speedMultiplier` |
| --- | --- | --- | --- | --- |
| 0.5x | 0.7 | 10.6200320000 | 0.9401990000 | 5.9800020000 |
| 1x | 0.9 | 11.1800317764 | 0.9581990242 | 5.7700018883 |
| 2x | 1.1 | 11.4200320000 | 0.9571990000 | 5.8700020000 |
| 3x | 1.3 | 11.2300320000 | 0.9611990000 | 6.0000020000 |
| 4x | 1.6 | *(shares the 1.3 row)* | | |

3x and 4x genuinely share one row — the branch is `if (speed == 1.3 || speed == 1.6)`.

All five gdsim X speeds reproduce `float(m_playerSpeed) * m_speedMultiplier * 60`
to **0.000e+00** relative error. `cubeJumpHeight = m_yStart * 54` and
`velocityThreshold = m_gravity * 108` likewise.

## Gravity selection (`PlayerObject::updateJump`, 0x14038b900)

```
usedGravity = m_gravity                       // the per-tier table above
if (m_isBall||m_isShip||m_isBird||m_isDart||m_isSwing||m_isSpider)
    usedGravity = 0.958199024f                // .rdata 0x140622bf8, bytes 88 4c 75 3f
if (m_gravityMod != 1.0) usedGravity *= m_gravityMod
```

**Six modes ignore the speed portal for gravity.** Only the cube and the robot
scale with it — they are the two missing from that OR. `0.958199024` equals the
1x tier exactly, which is why 1x-only captures never revealed the difference.
gdsim calls this `flightGravity = 2794.108353984`.

Size divisor and clamp asymmetry:

```
sizeDiv = 1.0 ; clampAsym = 0.8
if (m_vehicleSize != 1.0)               sizeDiv = 0.8       // 0x140622ba4
if (isFlying() && m_vehicleSize != 1.0) sizeDiv = 0.85
if (m_isSwing)                        { sizeDiv = 1.0 ; clampAsym = 1.0 }
```

`isFlying() == m_isShip || m_isBird || m_isDart || m_isSwing` (from the
bindings' own reconstructed inline source).

## Per-mode acceleration

`flipMod = m_isUpsideDown ? -1 : 1`. Every arm ends in
`setYVelocity(-(...) + m_yVelocity)`.

| mode | formula |
| --- | --- |
| cube | `flipMod * dt * usedGravity * 1.0` |
| ball | `... * 0.6` |
| spider | `... * 0.6` |
| robot | `... * 0.9` |
| ship | `(flipMod * dt * usedGravity * m20 * m24) / sizeDiv` |
| UFO | `(flipMod * dt * usedGravity * m3 * 0.5) / sizeDiv` |
| **swing** | `flipMod * dt * usedGravity * (mini ? 0.6f : 0.4f)` — **no divide** |
| wave | not an acceleration: `v = flipMod * (held?1:-1) * m_playerSpeed * m_speedMultiplier` |

Ship's two multipliers:

```
m20 = 0.8
if (held) { if (!m_isAccelerating) m20 = -1; else if (moving down) m20 = -1; }
else      { if (m_isAccelerating && moving down) m20 = -1; }
if (!held && !playerIsFallingBugged()) m20 = 1.2
m24 = 0.4 ; if (held && playerIsFallingBugged()) m24 = 0.5
```

giving the four familiar bands `{-0.5, -0.4, +0.48, +0.32}`.

UFO: `m3 = playerIsFallingBugged() ? 0.8 : 1.2`, so `{0.4, 0.6}`.

Swing's block, verbatim, at `0x14038c959` — this is what settles it, because
Ghidra's C output loses the arguments of the swing/UFO `setYVelocity` call and
makes the swing look like it shares the ship's formula:

```
movaps %xmm8,%xmm9          ; sizeDiv   = 1.0
movaps %xmm8,%xmm7          ; clampAsym = 1.0
movss  0x140622ad0,%xmm1    ; m = 0.4f
movss  0x9f0(%rdi),%xmm0    ; m_vehicleSize
ucomiss %xmm8,%xmm0
je     .keep
movss  0x140622b38,%xmm1    ; m = 0.6f   (mini)
.keep: sign * dt * usedGravity * m , negate, add m_yVelocity
```

No input term, no threshold, no `playerIsFallingBugged` call, no divide.

## `.rdata` constants used above

| address | value |
| --- | --- |
| 0x140622bf8 | 0.958199024f (flight-group gravity) |
| 0x140622ad0 | 0.4f |
| 0x140622b08 | 0.5f |
| 0x140622b38 | 0.6f |
| 0x140622c84 | 1.2f |
| 0x140622ba4 | 0.8f |
| 0x140622c24 | 1.0f |
| 0x1406236f0 | -1.0f |
| 0x1406237d4 | -6.4f |
| 0x1406237fc | -8.0f |

## `PlayerObject::playerIsFallingBugged` (0x14039a430)

```c
if (m_isSideways || m_isPlatformer || m_isSwing || m_fixGravityBug /*0x561*/) {
    float g2 = (float)(m_gravity + m_gravity);
    return m_isUpsideDown ? (-g2 < m_yVelocity) : (m_yVelocity < g2);
}
double g  = m_gravity;
double g2 = m_unkA99 /*0xa99*/ ? -g : g;
return m_isUpsideDown ? (g + g < m_yVelocity)              // <-- +2g, not -2g
                      : (m_yVelocity < (float)g2 + (float)g2);
```

The name is literal. In the second path the upside-down branch compares against
**+2*m_gravity**, ignoring the sign flip the first path applies. gdsim
reproduces it by wrapping `velocityThreshold[]` in `Player::grav()` at the use
site — verified: with gdsim's player-relative velocity, `v > grav(T)` is
algebraically identical to GD's world-frame test in both orientations.

Swing, platformer and sideways take the *correct* symmetric path.

## Velocity clamps

Flight clamp, one shared basic block at `0x14038ca9f`, skipped for the wave and
skipped entirely while `m_isAccelerating`:

```
normal      : clamp(v, (clampAsym * -8.0)/sizeDiv,  8.0/sizeDiv)
upside-down : clamp(v,             -8.0 /sizeDiv, (clampAsym * 8.0)/sizeDiv)
```

| mode/size | gdsim min | gdsim max |
| --- | --- | --- |
| ship & UFO, big | -345.6 | 432.0 |
| ship & UFO, mini | -406.5882352941 | 508.2352941176 |
| swing, either size | -432.0 | 432.0 |

Non-flight terminal velocity is a hard `+-15.0` -> **810** gdsim units.

Every write to `m_yVelocity` is then quantised to 3 decimals:
`if (v != (int)v) v = round(frac*1000)/1000 + (int)v`.

## Jump impulse

```
imp = m_yStart ; if (m_isRobot) imp *= 0.5
v = flipMod * imp * sizeDiv
if ((m_isOnSlope || m_wasOnSlope) && slopeVelocity points up) {
    ramp = 1.0
    if (m_totalTime - m_slopeStartTime < 0.1)
        ramp = max(0.4, (m_totalTime - m_slopeStartTime) * 10)
    sv   = m_isBall ? m_slopeVelocity : m_slopeVelocity * ramp
    vOld = v
    v    = vOld + sv * 0.25
    v    = m_isUpsideDown ? max(v, vOld*1.4) : min(v, vOld*1.4)   // 1.4x cap
}
if (m_isBall) { flipGravity(); v *= 0.6 }
```

Note the bindings' hand-written `getModifiedSlopeYVel()` inline reconstruction
has the ramp condition **inverted** (`if (diff > 0.1)`); the binary says
`< 0.1`. Trust the binary.

`PlayerObject::flipGravity` (0x14039a1d0) halves velocity: `m_yVelocity *= 0.5`.
So a ball jump is `m_yStart * 0.5 * 0.6 = m_yStart * 0.3`, which is exactly
gdsim's `ballJumpHeight`.

## Robot hold-charge

While `m_maybeIsBoosted`:

```
if (m_isRobot && held && !m_touchedPad && m_accelerationOrSpeed < 1.5) {
    m_accelerationOrSpeed += dt * 0.1;          // dt = 0.225 -> +0.0225/step
    setYVelocity(+flipMod*dt*grav*mult + v);    // cancels the gravity below
}
setYVelocity(v - flipMod*dt*grav*mult);
```

`m_accelerationOrSpeed` is zeroed on the jump, so the cancel runs while
`(n-1)*0.0225 < 1.5`, i.e. **67 steps**. gdsim uses 66/67 — the 67 figure for
mini is the one this supports; the big-size 66 was measured and is worth
re-deriving.

## Orbs (`PlayerObject::ringJump`, 0x140398c00)

```
size = (m_vehicleSize != 1.0) ? 0.8 : 1.0
v    = m_yStart
switch (objectType) {
  GravityRing  (13, blue) : v *= 0.8
  GreenRing    (29)       : if (m_isShip) v *= 0.7        // + flipGravity BEFORE the set
  PinkJumpRing (12)       : ship 0.37, UFO 0.42, ball 0.77, else 0.72
  RedJumpRing  (35)       : ship 1.0 big / 1.4 mini,  UFO 1.02 big / 1.36 mini,
                            ball 1.34, robot 1.28, spider 1.34, else 1.38
  default (incl. yellow)  : if (m_isRobot) v *= 0.9
}
setYVelocity(flipMod * v * size)
```

So **yellow == the plain jump impulse**, and the whole table is
`cubeJumpHeight[tier] * typeMult * sizeMult`.

Ordering matters and explains an apparent inconsistency: the green ring's
`flipGravity` runs *before* `setYVelocity`, so its halving is overwritten and
green keeps the full value; the blue ring's flip runs *after*, so blue ends up at
`0.8 * 0.5 = 0.4` of the jump. gdsim's blue row is exactly `-0.4 *
cubeJumpHeight`, which independently confirms both the 0.8 multiplier and the
ordering.

There is one more post-scale, applied after `setYVelocity` and before the blue
ring's flip:

```
if (m_isBall || m_isSpider) m_yVelocity *= 0.699999988079071f
else if (m_isSwing)         m_yVelocity *= 0.6000000238418579f
```

That 0.7 is the origin of the long-unexplained "ball orbs are 0.7x cube" pattern
in gdsim's table — it is this one line, not a per-orb constant. The swing takes
**0.6**, so collapsing swing onto the ball row (as `orbPadVehicle` does) makes
every orb 16.7% too strong in swing mode; Orb.cpp rescales for it.

Pads are unaffected by that distinction: `propellPlayer` scales ball, spider AND
swing by the same 0.6, so the swing->ball collapse is right there.

## Pads (`GJBaseGameLayer::bumpPlayer` + `PlayerObject::propellPlayer`)

```
sizeMult = (m_vehicleSize != 1.0) ? 0.8 : 1.0
setYVelocity(flipMod * bumpMod * 16.0 * sizeMult)
if (ball || spider || swing) m_yVelocity *= 0.6000000238418579
```

**Pads carry no m_yStart and therefore no speed tier at all** — 16.0 is a literal.
`16 * 54 = 864` is the yellow pad's whole value.

| pad | bumpMod |
| --- | --- |
| yellow (8) | 1.0 |
| pink (9) | ship 0.35, UFO 0.4, ball 0.7, spider 0.7, else 0.65 |
| red (34) | ship 0.63 big / 0.95 mini, UFO 0.6 big / 0.98 mini, else 1.25 |
| gravity (10) | 0.8, via `propellPlayer` from case 10 — then `flipGravity` halves it |
| spider (44) | no propel at all: `spiderTestJumpInternal`, a teleport |

`bumpPlayer` clears `m_isAccelerating` and then sets it again only for the red
pad. The gravity pad never reaches `bumpPlayer`, so it leaves the flag alone.

## `m_isAccelerating` (0x952)

Set by: red ring, dash ring, red pad, force block, dash start, `boostPlayer`,
`rotateGameplay`, and updateJump's own not-on-ground anti-cheat path.
Cleared by: every non-red pad, `handlePlayerCommand(543)`, and updateJump itself
whenever the velocity sits back inside the flight band.

While set it disables the flight clamp entirely, which is the only way a boosted
ship or UFO can exceed 432. gdsim models this on `Player::isAccelerating`; before
it existed, `ufoClampMaxBig` had been raised 432 -> 520 purely to let a red pad's
518.4 survive.

It also suppresses ship thrust while holding and already moving up (`m20` stays
0.8 instead of -1) — that half is **not** yet modelled in gdsim.

## Object dispatch (`GJBaseGameLayer::collisionCheckObjects`)

Every gameplay interaction in the game funnels through one switch on
`m_objectType`. Decoration (7) is the only type skipped outright, which is why
the runtime capture now uses exactly that rule.

| type | handler | in gdsim |
| --- | --- | --- |
| 0 Solid / 21 Breakable | deferred solid list | yes |
| 2 Hazard / 47 AnimatedHazard | deferred hazard list | yes |
| 3/4 GravityPortal | `flipGravity` | yes |
| 5/6/16/19/26/27/33/41 mode portals | `toggle*Mode` | yes |
| 8/9/34/44 pads | `bumpPlayer` | yes except **44 spider pad** |
| 10 GravityPad | `propellPlayer` + flip + optional `reversePlayer` | partial (no reverse) |
| 11/12/13/29/32/35/36/37/38/43/46 rings | `playerTouchedRing` | yes except **36 CustomRing** |
| 14/15 MirrorPortal | `toggleFlipped` | **visual only — correctly ignored** |
| 17/18 SizePortal | `togglePlayerScale(enable)` | yes (17=big, 18=mini) |
| 20/45 triggers | `playerTouchedTrigger` | partial |
| 23/24 DualPortal | `toggleDualMode` | yes |
| 25 Slope | `collidedWithSlopeInternal` | yes |
| 28 TeleportPortal | `teleportPlayer` | yes |
| 40 Special | J/S/H/F/Force blocks by id | yes |
| 42 GravityTogglePortal | `flipGravity(!upsideDown)` | **missing** |

Special-block ids read straight out of case 0x28, settling a disagreement with
the bindings' hand-written inline source (which says 3845): **1755** dart-slide,
**1813** no-auto-jump, **1829** cancel dash, **1859** hit-head, **2069** and
**3645** force block, **2866** flip gravity. gdsim already had 3645 and is right.

The mirror portal only sets `m_isFlipping` and an animation value; nothing in the
physics path reads them. The real direction reversal is `m_isGoingLeft`, set by
`doReversePlayer` — a different mechanic (see gaps below).

## Force block multipliers (`PlayerObject::update`, `m_stateForce > 0`)

| mode | big | mini |
| --- | --- | --- |
| ship | 0.47 | 0.5875 |
| UFO | 0.58 | 0.72499996 |
| swing | 0.4 | 0.61538464 |
| ball / spider | 0.6 | — |
| robot | 0.9 | — |
| cube | 1.0 | — |

`dv = dt * m_forceMagnitude * mult`; sets `m_isAccelerating` when non-zero.

## `touchedObject` special IDs (bindings inline source, Windows)

| id | effect |
| --- | --- |
| 1755 | `m_stateDartSlide = 2` (the D block — wave rides it) |
| 1813 | `m_stateNoAutoJump = 2` |
| 1829 | cancels a dash |
| 1859 | `m_stateHitHead = 2` |
| 2069 / 3845 | force block, magnitude capped at 9999 |
| 2866 | `m_stateFlipGravity = 2` |

All `m_stateXxx` counters decrement once per frame, so "set to 2" means active
for two frames.

---

## Move triggers

Chain: `GJBaseGameLayer::triggerMoveCommand` (0x14021ea40) →
`GJEffectManager::createMoveCommand` (0x14025c700) → per substep
`GJEffectManager::prepareMoveActions` (0x14025f3f0) →
`GroupCommandObject2::step`/`updateAction` (0x140257900 / 0x1402579d0) →
`GJBaseGameLayer::processMoveActions` (0x14022d5b0) → `moveObjects`.

### Level-string field ids

Read out of `EffectGameObject::customObjectSetup` (0x1404a8ad0). Its parameter
table is indexed by field id, so `param_3 offset / 8` and
`param_2 offset / 0x20` both give the id directly.

| field | member |
| --- | --- |
| 10 / 28 / 29 | duration, moveX, moveY |
| 30 / 85 | easing type, easing rate |
| 51 / 71 | target group, center group |
| 58 / 59 | lock to player X / Y |
| 141 / 142 | lock to camera X / Y |
| 143 / 144 | move mod X / Y |
| 100 / 101 | use move target, target mode (0 both, 1 X only, 2 Y only) |
| 138 / 200 | target is player 1 / player 2 |
| 393 / 394 | small step, direction mode |
| 395 / 396 | target-mode center group, direction distance |
| 397 / 544 | dynamic mode, **silent** |

Silent is **544**, not 36 — every one of ALLOY's 258 move triggers carries 36
while their motion is plainly interpolated.

### Per-step integration

`updateAction` keeps the ABSOLUTE eased amount and emits the difference:

```
p      = clamp(elapsed / max(duration, FLT_EPSILON), 0, 1)
amount = total * getEasedValue(p, easingType, easingRate)
delta  = amount - lastAmount ; lastAmount = amount ; accum += delta
```

`step` does not advance `elapsed` on its very first call (a one-frame arm), and
`duration == -1` means the command never finishes.

Channels are assigned in `createMoveCommand`: X becomes channel 1 and Y channel 2,
but if X is locked or zero, Y takes channel 1 instead. An axis that is locked gets
NO channel — `prepareMoveActions` overwrites that axis's delta with

```
lockToPlayerX : delta.x = effectManager.playerDeltaX * moveModX
lockToCameraX : delta.x = -cameraDeltaX * moveModX      (stored pre-negated)
```

and the same for Y. `playerDeltaX/Y` are written once per substep in
`GJBaseGameLayer::update` as the player's position minus its `m_lastPosition`.

Silent takes none of this path: `triggerMoveCommand` adds the whole offset to the
group's objects immediately and then writes their `m_lastPosition` to match, so
duration and easing are ignored **and the move does not register as motion** (a
player riding the object is not carried).

### Easing (`GameToolbox::getEasedValue`, 0x140068b70)

Four places where GD does not match the textbook set, all of which gdsim had
wrong until 2026-09-12:

* **EaseOut (3) is `t^(1/rate)`**, not `1-(1-t)^rate`. GD mirrors EaseIn through
  the exponent, not the curve. At rate 2 they differ by up to 0.068 of progress.
* **Elastic (4/5/6) use the trigger's own `easeRate` as the period**, not a
  hardcoded 0.45/0.3.
* **ExponentialIn (11) subtracts 0.001.**
* **ExponentialInOut (10) has no endpoint special-case** — at t=0 it really
  returns 0.000488.

`bounceTime` matched already.

### What gdsim models

Eased X/Y offsets, rotate, toggle, follow, spawn chains with cycle detection, and
now lock-to-player-X (deterministic: the player's X advance is input-independent,
so `poseMovable` sums it from `Level::playerXAtFrame`). Lock-to-player-**Y** and
the camera locks are parsed but never applied — Y depends on the path being
searched, which would make an object's position a function of the search branch
and break the engine's central property that a pose is a pure function of frame.
Target/direction/dynamic mode are parsed as ids only, not implemented.

## Hitboxes — the exact collision check

`GJBaseGameLayer::collisionCheckObjects` runs, per nearby object:

```
rect = (type == Slope) ? obj->getObjectRect(2, 2)   // slopes get a DOUBLE broad rect
                       : obj->getObjectRect()
if (obj->m_objectRadius > 0)  broad = playerCircleCollision(player, obj)
else                          broad = AABB(rect, player->getObjectRect())   // inclusive
if (!broad) skip
if (obj->m_shouldUseOuterOb)                      // i.e. the object needs an OBB
    narrow = overlaps1Way(objOBB, playerOBB) && overlaps1Way(playerOBB, objOBB)
else
    narrow = true
if (type == Slope) rect = obj->getObjectRect()    // restore before dispatch
```

### The boxes

`GameObject::getObjectRect2` — the cached `getObjectRect()` — returns
**`m_shouldUseOuterOb ? m_orientedBox->getBoundingRect() : rawAABB`**, so a
rotated object's broad rect IS rotation-inflated. The raw box is

```
w = m_width  * m_scaleX ,  h = m_height * m_scaleY      (swapped if m_isRotationAligned)
centre = getRealPosition() + getBoxOffset()
```

while the OBB additionally applies the per-object hitbox scales:

```
OBB2D::create(centre, m_spriteWidthScale  * m_width  * m_scaleX,
                      m_spriteHeightScale * m_height * m_scaleY, -rotation)
```

`m_spriteWidthScale`/`m_spriteHeightScale` are set per object id in
`GameObject::customSetup` and are where the small spike hitboxes come from —
hazards get width 0.2 or 0.3 and height 0.4, i.e. 6x12 and 9x12 off a 30x30
base, matching gdsim's engine-captured hazard table.

**Correction to an earlier reading in this file:** the no-argument
`GameObject::getObjectRect()` (0x1401976a0, vtable 0x490) is a one-line tail call
into the two-float overload passing **`m_spriteWidthScale, m_spriteHeightScale`**.
So the sprite scales ARE in the raw rect — the full expression is

```
hitbox_w = m_width  * m_scaleX * m_spriteWidthScale
hitbox_h = m_height * m_scaleY * m_spriteHeightScale
```

and the OBB path is not needed for it. `m_shouldUseOuterOb` is set by
`GameObject::updateIsOriented` (0x1401a1730) only when the object's rotation is
**not a multiple of 90 degrees** and it has no radius — it is a "rotated at an odd
angle" flag, nothing more.

### The player's boxes

`m_width`/`m_height` are 30 x 30 from `PlayerObject`'s constructor and
`resetPlayerIcon`. Only two modes override them, both writing the same value to
width AND height:

| mode | m_width x m_height | big | mini (x0.6) |
| --- | --- | --- | --- |
| cube, ship, ball, UFO, robot, swing | 30 x 30 | 30 x 30 | 18 x 18 |
| wave (`toggleDartMode`) | 10 x 10 | 10 x 10 | 6 x 6 |
| spider (`toggleSpiderMode`) | 27 x 27 | 27 x 27 | 16.2 x 16.2 |

The mini scale reaches the rect through `m_spriteWidthScale`, not `m_scaleX`:
`togglePlayerScale` ends with `m_spriteWidthScale = m_spriteHeightScale =
m_vehicleSize` (1.0 or 0.6). `m_scaleX`/`m_scaleY` stay 1.0 on the player — they
are the per-instance scale a LEVEL applies to an object.

gdsim matches on every mode except the spider, which it left on the generic 30
that `VehiclePortal::collide` installs; now 27/16.2 from `g_phys.spiderSize*`.

### There is no small "inner" player box in classic mode

`collidedWithObjectInternal` does build a narrowed player rect —
`CCRect(x + inset*0.5, y, w - inset, h)`, width only, height untouched — and
tests it with `intersectsRect` before resolving a vertical collision. But `inset`
is 5.0 **and is zeroed immediately unless `m_isPlatformer`**:

```c
if (m_isPlatformer == 0) local_188 = 0.0;
```

So in classic mode the block/ceiling resolution runs against the FULL player
rect. gdsim's `innerHitbox()` (9x9) and `blockDeathHitbox()` (7x7) have no
counterpart in this function — they are calibration constants fitted against real
runs, not engine geometry, and their comments say as much. Left alone: they are
load-bearing for a lot of validated behaviour, and replacing them is a separate,
evidence-led job rather than a transcription.

**The player's rect is never rotation-inflated.** `GameObject::getOrientedBox`
sets `m_shouldUseOuterOb = 1`, but `PlayerObject` OVERRIDES that method and its
version does not — so the player's `getObjectRect()` always takes the raw
axis-aligned path. That independently confirms gdsim's `unrotatedHitbox()`,
which had only been justified empirically.

### `OBB2D::overlaps1Way` (0x14006e130)

Morgan McGuire's OBB2D SAT. `calculateWithCenter` normalises each axis by its
SQUARED length, so the box spans exactly `[origin, origin+1]` on its own axis —
which is what the `+ 1.0` in the interval test is. Not a tolerance.

```
for a in 0,1:
    project other's 4 corners onto axis[a]
    if (origin[a] + 1 < min || max < origin[a]) return false
return true
```

gdsim's `Entity::intersects` / `intersectOneWay` is a different formulation
(rotate B into A's frame, then "a corner inside A's slab OR consecutive corners
straddle A's centre") but is **mathematically equivalent**: a corner inside the
slab or a straddle both imply interval overlap, and a closed quad whose
projection contains A's centre must cross it an even number of times, so at
least one of the three checked adjacent pairs catches it.

### `playerCircleCollision` (0x140211df0) — blades

```
r = (m_scaleX == 1 && m_scaleY == 1) ? m_objectRadius
                                     : max(m_scaleX, m_scaleY) * m_objectRadius
if (playerRect.containsPoint(centre)) return true
for each of the player rect's FOUR CORNERS:
    if (distance(corner, centre) < r) return true
return false
```

**There is no "closest point on the rect" term.** A blade approaching a FACE
head-on — centre outside the rect, nearest point in the middle of an edge, all
four corners further away than r — does **not** collide. With the 30x30 player
and a radius-16 blade centred 8 above the top edge: nearest edge point 8 away
(textbook test says hit), both top corners sqrt(15²+8²) = 17.0 away, so GD says
miss. gdsim used the textbook closest-point test and was therefore strictly more
lethal than the engine on the most common way to meet a blade; now transcribed.

## The hitbox VALUES, and where they live

Three functions, all reachable through a jump table keyed on the object id. The
tables are in `.rdata`, so the ids come out of the binary, not out of guesswork.

| what | function | how to read it |
| --- | --- | --- |
| `m_width` / `m_height` | `GameObject::setupSpriteSize` 0x1401a36a0 | ids 1..471 index a BYTE table at **0x1401a3fec** into a case switch; later ids switch on the id directly |
| `m_objectType`, `m_spriteWidthScale`, `m_spriteHeightScale`, `m_objectRadius` | `GameObject::customSetup` 0x140190e20 | ids 5..4539 index a BYTE table at **0x140194b44** (154 distinct cases) |
| blade `m_objectRadius` | `EnhancedGameObject::customSetup` 0x1401a4f70 | per-id constants and `m_width * k` |

`tools/hitboxtable.py` does the first one; `tools/gd_spritesize.txt` is its
output (349 ids).

**The effective hitbox is the product:**

```
hitbox_w = m_width  * m_spriteWidthScale  * m_scaleX
hitbox_h = m_height * m_spriteHeightScale * m_scaleY
```

and the sprite scales apply **only through the OBB path**, never through the raw
rect. That is why hazards read small: id 9 is a 30 x 27 sprite, and 0.3 x 0.4
gives **9 x 10.8** — exactly gdsim's captured value. Orbs are the other
direction: 30 x 30 with a scale of **1.2** gives **36 x 36**, again exactly what
gdsim has.

### Checked against gdsim

Of the 60 comparable block/slope/portal/pad entries (sprite scale 1, so the
sprite size IS the hitbox), **60 match exactly**. Pads: 35 = 25x4, 140 = 25x5,
67 = 25x6 all exact; **1332 was 25x6 and is really 29x7** — it had been copied
from the pink pad with a "Hitbox approx; verify" note, now verified and fixed.

Spot confirmations that fell out of the same tables: id 10/11 = 25 x 75 and
12/13 = 34 x 86 match gdsim's portal constants; customSetup case 20 gives id
**67** type 10 (GravityPad), and case 26/27 give **99 = RegularSize (big)** and
**101 = Mini** — a third independent confirmation of both mappings.

### Blades

Radii come from `EnhancedGameObject::customSetup`:

```
88, 186, 740, 1705 -> 32.3     397 -> 28.9     675 -> 32.0     678 -> 30.4
1619 -> 25.0       1620 -> 15.0      1582 -> 4.0
89, 183, 187, 679, 741 -> m_width * 0.36
184, 676               -> m_width * 0.34
398, 677               -> m_width * 0.32
everything else        -> m_width * 0.30
```

Twenty ids in gdsim's blade table reproduce their engine radius EXACTLY from
`size.x` (89 = 60*0.36 = 21.6, 677 = 39*0.32 = 12.48, 98 = 40*0.30 = 12.0, …)
and **not one** reproduces it from `size.x/2`. The old
`isBigRadiusSawblade(typeId) ? size.x : size.x/2` was therefore halving an
already-correct radius for roughly thirty ids; removed.

Still off, and needing their `m_width` re-verified before touching: 918
(engine 14.4 vs 24.0), 1583 (9.3 vs 4.0), 1702 (4.5 vs 6.0), 1703 (3.9 vs 6.0),
185 (3.0 vs 2.85). Ids 1706..1736 go through a further computed jump table that
is not decoded; gdsim pairs them with their pre-2.1 twins, which the decoded
pairs support.

### Two ids gdsim still lacks

`customSetup` case 150 covers **3610 and 3611**: both `m_objectType = 2`
(Hazard), and 3611 additionally gets `m_objectRadius = 15.0` — so 3610 is a
rectangular hazard and 3611 a circular one. 614 sightings across four levels in
the capture set; the player currently passes straight through both.

**1816 is correctly absent**: it is type 39 (CollisionObject), and
`collisionCheckObjects` skips that type outright before any test.

## Slopes never kill

`collisionCheckObjects` case 0x19 (GameObjectType 25, Slope) dispatches to
`PlayerObject::collidedWithSlopeInternal` (0x14038f810), and that function has
**no death path at all** — no `destroyPlayer`, no `playerDestroyed`, no death
gameEvent anywhere in it. It positions and rides, nothing else. The wave
(`m_isDart`) appears in it only inside the `ship|bird|dart|swing` group, i.e. the
flight family that rides the diagonal.

A wave dies on solid geometry in a *different* function,
`collidedWithObjectInternal`, through

```c
if (m_isDart && m_stateDartSlide < 1) goto <skip all resolution>;
```

Lethal slopes are a separate object type, not this one: ids **1718** and **364**
are GameObjectType 2 (Hazard), while **315 / 289 / 294 / 299 / 1339** are type 25.

gdsim's `Slope::collide` used to kill a wave on any slope touch. On DeCode
(2997354) that killed the wave halfway up its 45-degree ramp at x~6477 y~393 —
where the recorded deviation from the real player is **0.19 units**, i.e. the
trajectory was right and only the rule was wrong. `test/waveprobe.exe` reproduces
the old verdict in one command.

## Known gaps in gdsim after this pass

Ordered by how much they cost the pathfinder on real levels.

1. **Reverse (`m_isGoingLeft`).** `doReversePlayer` flips the sign of the X
   advance, and `reversePlayer` is reachable from a gravity pad carrying the
   flag at object+0x704, plus the 2.2 Reverse trigger. gdsim advances X
   unconditionally (`pos.x += player_speeds[xs] * dt`), so a reversed section is
   simulated running the wrong way through the level. Not a constant fix:
   level parsing, `buildXTable`, the trigger timeline and the beam search are
   all built on X being monotonically increasing. Needs its own design pass.
2. **Sideways gravity (`m_isSideways`, `rotateGameplay`).** Same class of
   problem — swaps the gravity axis wholesale.
3. **Ship thrust suppression while `m_isAccelerating`.** The clamp half of that
   flag is now modelled; the `m20` half (holding + already moving up keeps 0.8
   instead of -1) is not.
4. **`m_gravityMod`** (the 2.2 Gravity trigger) is unparsed. Note the ship arm
   applies it only when `m20 >= 0`.
5. **GravityTogglePortal (type 42)** and the **spider pad (type 44)** have no
   entry in `Object::create`. Both need their object ids, which the widened
   runtime registry will supply.
6. The swing's gravity flip on input is modelled as a press-edge toggle with
   velocity carried through unchanged. `flipGravity` halves velocity, so if the
   swing routes through it the carried velocity is wrong by 2x. The call site
   for the swing's own flip has not been located yet.
7. `m_unkA99` is read by `playerIsFallingBugged` but nothing found yet writes
   it; assumed false throughout.

## Getting the object table from the game instead of guessing

`m_objectType` is not assigned anywhere in code — it is data-driven, so the
id -> type table cannot be extracted from the decompile at all. The runtime
capture is the only authoritative source, and until 2026-09-11 it was filtered to
"solid OR hazard OR one of 20 hardcoded portal ids", which dropped every orb,
pad, special block, trigger and the remaining portal types.

It now mirrors the engine's own rule (everything whose `m_objectType` is not
Decoration) and accumulates into
`testlevel/movetest/GDMod_objtypes.txt` — `id type width height sightings
firstLevel`, merged across every level and every session, keeping the dominant
(unscaled) footprint per id. That file is what `Object::create` should
eventually be generated from.
