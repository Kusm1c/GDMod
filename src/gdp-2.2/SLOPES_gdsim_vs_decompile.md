# Slopes: gdsim vs the real 2.2 decompile — gap analysis & fix backlog

Ground truth = this folder (camila314/gdp @ 2.2, real Ghidra decompile).
gdsim impl = [../sim/Slope.cpp](../sim/Slope.cpp) (+ Slope.hpp).

Real slope pipeline, 3 pieces:
1. `GameObject::slopeYPos(playerX)` — the **unclamped diagonal line Y** at the player's x.
2. `PlayerObject::collidedWithSlopeInternal(dt, obj, forced)` — per-slope collision:
   compute the clamped ride height `newPlayerY`, decide `collidedSlope` (ride vs
   death), set position, set `m_slopeVelocity` (the **exit launch**), set rotation.
3. `PlayerObject::collidedWithObjectInternal` — the "am I still on the slope / when
   do I exit" logic and its interplay with block snapping.

gdsim collapses (1)+(2) into `Slope::expectedY` + `Slope::collide` + `Slope::calc`.

---

## FIXED
### ✅ 2026-08-14 session: #2 exit-velocity magnitude, measured against a REAL capture (first time this gap had actual ground truth)
The 2026-08-06 pass removed the unexplained `0.9`/time-ramp but never validated the
REMAINING formula's magnitude against real data (this doc's own gap #2 still listed
"re-derive against a real slope capture" as outstanding). Got that capture 2026-08-14:
a real playtest kept reporting "the cube goes further in gdsim than in real GD" near
DeCode's (2997354) very first slope — captured via the RE scanner
(`testlevel/GDMod_physics_2997354.txt`, cube, 0.9x speed, 45° climb). Recovered the
real launch velocity from the captured POSITION deltas right after leaving the ramp
(world-Y delta ÷ raw `m_yVelocity`, averaged over the first 3 post-launch frames where
that ratio is cleanest ≈0.2183, i.e. a ≈52.4 world-units/sec-per-raw-m_yVelocity-unit
conversion factor — confirmed independently via the position/velocity data's OWN
consistent -0.216/frame linear decay), giving ≈388 units/sec. gdsim's own formula for
the identical angle/speed computes 444.3 — a clean, consistent **~14.5% overshoot**.
Since peak arc height scales with the SQUARE of launch velocity, this measurably
carries the cube higher/further than real GD every time it launches off a slope —
matching the reported symptom exactly. Fixed via a new calibration knob
(`CalibParams::slopeExitVelScale`, `src/sim/Calib.hpp`, default 0.873 = 1/1.145),
applied as a flat overall scale rather than picking one sub-term (1.12, the
player-speed term, or the angle term) to blame — one capture at one angle/speed can't
disentangle which sub-term is actually responsible; if a future capture at a DIFFERENT
angle/speed shows a different overshoot ratio, that's the signal to dig into a specific
term instead of this flat scale. `test/regress.sh` 13/13 clean; 157-macro batch:
7 levels' death frame shifted (expected — trajectory-changing fix), CLEAR count
unchanged (1/157, none lost/gained).

### ✅ 2026-08-06 session: #1 newSlopeScalar, #2 exit-velocity 0.9/time-ramp, wave multi-slope arbitration
Triggered by the user's wave level (68839068) repeatedly failing to solve. Findings:
- **#1 newSlopeScalar**: ported into `Slope::expectedY` — `isNewSlopeTransition()` detects a
  floor↔ceiling grip switch (prev slope id differs AND `isFacingUp()` differs), applies the
  `m_vehicleSize*20`(big)/`*12`(mini) shrink to BOTH the centre formula and the clamp bound
  — **only the radius-bearing clamp bound gets the scalar** (verified by hand-matching real's
  `slopeFloorTop` branch shape against gdsim's OWN already-capture-validated clamp shape:
  `slopeFloorTop == !isFacingUp()`, confirmed via the clamp bounds' structural shape, not the
  name — counterintuitive, the name suggests the opposite). regress 13/13 clean, no change
  (none of the 13 truths hit a floor↔ceiling junction as their FIRST divergence).
- **#2 exit velocity**: dropped the unexplained `0.9` factor and the `10*(elapsed)→[0.4,1.0]`
  time ramp in `Slope::calc` — real sets `m_slopeVelocity` once, directly, no ramp. Also
  discovered this whole code path (`calc()`, gated on `slopeData.slope` being set) is ONLY
  ever reached by Cube/Ball/Robot/Spider — Ship/Ufo/Wave/Swing are intercepted by their own
  branches earlier in `collide()` and never populate `slopeData.slope` — so the exit-velocity
  0.75 damping only actually matters for Ball today (Ship/Ufo checks in the old code were
  dead). regress 13/13 clean.
- **Wave multi-slope conflict (NEW finding, not in the original #1-#7 list)**: a wave
  corridor is typically built from TWO opposing slopes (floor rising to meet a descending
  ceiling — confirmed in 68839068's object dump around both x=563 and x=2078 pinches: dense
  `id=1338` slope clusters). The wave branch in `collide()` used to unconditionally
  `pos.y = expectedY(p)` — if both slopes' broad-phase boxes touch the same frame (common at
  a tight pinch), whichever was LATER in section/parse order silently won, an arbitrary,
  level-string-dependent outcome. Fixed to an order-independent CLAMP: floor slopes
  (`orientation<2`) only ever push pos.y UP (`max`), ceiling slopes only ever push it DOWN
  (`min`) — applying both in any order converges to the same, correctly-bounded result.
  Mathematically identical to the old code in the single-slope case (proven: the entry gate
  already guarantees pos.y is on the correct side of expectedY before this runs), so this is
  a pure win with zero regression risk. regress 13/13 clean, tested; 68839068 end-to-end
  re-solve in progress at time of writing.
- **ATTEMPTED AND REVERTED: gap #4 grace-zone entry test.** Ported real's `collidedSlope`
  formula (float_g/bool_h) to replace the raw `expectedY<=pos.y` entry cutoff for grounded
  vehicles, targeting the slopelab f204 divergence (real starts rising 3-4 frames /
  ~2u before gdsim). Empirically **made ZERO measurable difference** (identical severity,
  not even partial credit) — traced to `playerUphill`'s derivation depending on real's
  per-object `m_slopeUphill` flag, which isn't recoverable from the decompile alone; my
  assumption (`m_slopeUphill = angle()>0`) may well be INVERTED (worked through the XOR
  chain: `playerUphill = !m_slopeUphill` when moving right, so if the guess is backwards,
  the whole grace branch silently never applies). Reverted rather than ship an unverified
  guess. **f204 sev=1.14 divergence on slopelab remains OPEN** — needs either a live GD
  debug session to read the real flag, or a differently-shaped capture that isolates
  `m_slopeUphill`'s effect from everything else.

### ✅ Ride-clamp inversion (found via slopelab capture 2026-07-17) — the big one
`Slope::expectedY`'s two clamp branches were SWAPPED vs the decompile. A floor slope
ridden on top (`isFacingUp`, centre = surface + radOnSlope) must clamp the centre to
`[getBottom(), getTop()+radius]` so it climbs the diagonal to top+radius; gdsim clamped
it to `[getBottom()-radius, getTop()]`, pinning the ride at the slope base. On the
slopelab 291 (26.57°) the cube stuck at y=30 (bouncing) while real rode to y=45, then
missed the launch entirely. Fixed by swapping the two clamps to match
`collidedWithSlopeInternal` (floor → [bottom, top+radius]; ceiling → [bottom-radius, top]).
Result: ride now tracks real to ~1.7u, launch fires (~+3.4 vs real +3.57), peak 52.9 vs
52.8. **regress 11/11 clean (62687326 wave-slope still clean); divergehunt first-divergence
moved f229(on-slope, growing) → f290(post-slope landing).** RESIDUALS now exposed:
- launch fires ~3 frames LATE (gdsim f255 vs real f252) → the launch-trigger condition
  (gap #2/#4, the `p.gravBottom==p.gravTop(slope)` gate in Slope::calc).
- ride still ~1.7u low mid-slope (candidate: #1 newSlopeScalar, or a radOnSlope refinement).

## Ranked discrepancies (highest impact first)

### #1 — `newSlopeScalar` transition term — **MISSING in gdsim**  (likely root of floor-slope 6508283 + "slope-launch fragile")
Real (`collidedWithSlopeInternal`):
```
bool isNewSlope = m_wasOnSlope && m_collidingWithSlopeId != obj->m_uniqueID
                                && m_isCurrentSlopeTop != slopeFloorTop;
float newSlopeScalar = (isNewSlope && !m_isPlatformer) ? m_vehicleSize * 20 : 0;
float newPlayerY = slopeYPos + (playerRadOnSlope - newSlopeScalar)*(slopeFloorTop?-1:1);
// and the clamp lower/upper bound is ALSO offset by +newSlopeScalar:
//   floor: max(newPlayerY, minY - playerRadius + newSlopeScalar)
//   ceil : min(newPlayerY, maxY + playerRadius - newSlopeScalar)
```
`m_vehicleSize` = 1.0 (big) / 0.6 (mini) → scalar = **20 (big) / 12 (mini)**.
It fires only when transitioning onto a NEW slope object whose top/bottom orientation
differs from the one you were just on (floor→ceiling junction). gdsim's `expectedY`
([Slope.cpp:47-58]) has NO such term, so at those junctions the ride height is off by
up to 20u → the player pops/sinks, which is exactly the "fragile slope-launch" and the
6508283 floor-slope fall-through class. **Fix needs gdsim to know the previous slope's
id + orientation (it tracks `slopeData.slope` already) and player size.**

### #2 — exit launch velocity: gdsim's formula diverges from `m_slopeVelocity` — PARTIALLY ADDRESSED 2026-08-14 (see FIXED section above: magnitude now calibrated via `slopeExitVelScale`, but the sub-term breakdown below is still unverified against real data and worth revisiting if a different angle/speed capture shows a different overshoot ratio)
Real:
```
slopeYVelocity   = (height * m_playerSpeed * m_speedMultiplier) / width;
m_slopeVelocity  = min(1.12/slopeAngle, 1.54) * slopeYVelocity * flipMod * sign(uphill…);
if (isFlying() || m_isBall) m_slopeVelocity *= 0.75;   // ship/ufo/wave/swing/ball
```
gdsim ([Slope.cpp:78-83]):
```
vel = 0.9 * min(1.12/grav(angle), 1.54) * (size.y * player_speeds[speed] / size.x);
vel *= clamp(10*(timeElapsed - slopeData.elapsed), 0.4, 1.0);   // <-- not in real
if (Ball||Ship) vel *= 0.75;  if (Ufo) vel *= 0.7499;           // <-- 0.7499 vs 0.75
```
Divergences: (a) extra **`0.9`** factor with no counterpart in real; (b) a **`time`
ramp 0.4→1.0** that the real exit velocity does not apply (real sets `m_slopeVelocity`
once at contact, applied on exit); (c) **UFO 0.7499** vs real **0.75** (real = 0.75 for
ALL flying+ball, incl. Wave/Swing); (d) `grav(angle)` vs real `slopeAngle` — confirm the
angle units/gravity-scaling match. This is the boost memory flags as fragile — the
`0.9` and `time` ramp were empirical; re-derive against a real slope capture.

### #3 — wave (dart) slope death is gated by `m_stateDartSlide`, not "never dies"
Real death condition:
```
collidedSlope && !ignoreDamage && (slopeIsHazard
   || (!platformer && stateHitHead<=0 && (isNewSlope || (m_isDart && m_stateDartSlide<=0))))
   → destroyPlayer
```
So a WAVE (dart) **does die** on a slope when `m_stateDartSlide <= 0` (i.e. it hit the
diagonal head-on before entering the slide state). Once sliding (`dartSlide>0`) it's
safe. gdsim ([Slope.cpp:144-158]) makes the wave ALWAYS ride, never die — a safe
over-approximation that lets the solver believe impossible wave-slope entries are OK.
Model `m_stateDartSlide` (set when the wave establishes contact along the surface).
CORROBORATED by GDCS using-gamemodes: "Wave dies when hitting **everything except the
camera-border grounds**" — a slope is an object, so a wave must be able to die on it.

### #4 — "still on slope" tolerance `onSlopeThreshold` (float_g)
Real:
```
float_g = playerUphill ? (m_wasOnSlope ? 4.0 : 1.0) : 0.0;
if (slopeMoveDown||forced) float_g += min(|slopeMoveSpeed.y|, dt*(platformer?10:5));
onSlopeThreshold = posY - upsideMod*(playerRadOnPrevSlope + float_g);
// wasOnSlope exit: upsideDown ? threshold<minY : threshold>maxY  → leave slope
```
Defines exactly how far above the diagonal you can be and still count as on the slope
(and thus when the ride ends → airborne). gdsim's exit uses simpler `touching`/`getTop`
checks → slope-exit timing drift.

### #5 — `slopeYPos` edge convention (relates to slope_outside_xspan)
Real branches on `slopeLeft < playerX` → uses distance-from-RIGHT inside the span,
distance-from-LEFT when left of it (the line is extrapolated both ways). gdsim's
`expectedY` always measures `ratio*(x - getLeft())` from the left edge. Inside the span
they agree; at approach/exit edges they differ — the class of bug in
`slope_outside_xspan` (slopes acting outside their x-span).

### #6 — moving slopes: `slopeMoveSpeed` → `m_groundYVelocity` adjust  (niche)
Real reads `obj.realPos - obj.lastPos` and, for a moving slope, sets
`m_groundYVelocity = slopeMoveSpeed.y/dt`. gdsim poses movable objects but doesn't feed
slope motion into the ride velocity. Rare; defer.

### #7 — ball rotation (`runBallRotation`, cosmetic) + Ball coyote-time (leeway). Defer.

---

## Validation plan (do NOT blind-port — naive slope flips broke 62687326 before)
1. Capture source = **[testlevel/slopelab.spwn](../../testlevel/slopelab.spwn)** — a dedicated
   NO-INPUT slope course (build `spwn build testlevel/slopelab.spwn -c`, play zero-click,
   re-scanner ON → `testlevel/GDMod_physics_<id>.txt`). It walks a slope through cube +
   ball/robot/spider/ufo, both angles (289/291), mini/big, 2x speed, a /\ peak (UP + DOWN
   ride = the `falls[]` table), and a final wave segment (#3). Covers #2/#4/#5 directly;
   #1 (floor↔ceiling junction) + ceiling slopes need a SECOND inverted-gravity lab (TODO).
2. `divergehunt` on it → find which of #1–#5 fires FIRST on a real slope.
3. Port that item faithfully from the decompile; gate on regression (`test/regress.sh`,
   11/11, esp. 62687326 wave-slope + 85701165) AND the new slope capture.
4. Repeat. Keep this file as the running checklist.
