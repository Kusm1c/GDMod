#include "Level.hpp"
#include "Calib.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_set>
#include <cmath>

namespace gdsim {

std::string normalizeLevelString(std::string const& lvlString) {
    if (lvlString.empty()) return {};
    if (lvlString.find(';') != std::string::npos) {
        // Already in the standard ';'-delimited form. The first ';'-segment is the
        // level-settings header (kS38 colour blob + kA* keys). Return it INTACT:
        // stripping the leading "kS38," used to leave the comma-free colour blob as
        // the first token, which desynced the key/value pairing in initLevelSettings
        // and silently dropped every header setting after it (kA4 start-speed,
        // kA2 gamemode, kA3 mini …) → e.g. a 2x level ran at 1x and false-died.
        return lvlString;
    }

    std::string result = lvlString;
    if (lvlString.rfind("kS", 0) == 0) {
        auto comma = lvlString.find(',');
        if (comma == std::string::npos) return lvlString;
        result = lvlString.substr(comma + 1);
    }

    // Raw exports use |/_ separators. Compressed base64url can contain '_' but
    // never '|', so only rewrite when the raw export delimiter is present.
    if (result.find('|') != std::string::npos) {
        std::replace(result.begin(), result.end(), '|', ';');
        std::replace(result.begin(), result.end(), '_', ',');
    }
    return result;
}

void Level::initLevelSettings(std::string const& lvlSettings, Player& player) {
    std::unordered_map<std::string, std::string> obj;
    std::stringstream ss2(lvlSettings);
    std::string k, v;
    while (std::getline(ss2, k, ',')) {
        std::getline(ss2, v, ',');
        obj[k] = v;
    }

    auto get_or = [&obj](std::string const& key, std::string const& def) {
        if (auto it = obj.find(key); it != obj.end()) return it->second.c_str();
        return def.c_str();
    };

    // Level header start speed (kA4) uses GD's Speed enum: 0=Normal(1x) 1=Slow(0.5x)
    // 2=Fast(2x) 3=Faster(3x) 4=Fastest(4x). gdsim's speed index (matching the
    // SpeedPortal ids 200/201/202/203/1334) is 0=0.5x 1=1x 2=2x 3=3x 4=4x, so only
    // Normal/Slow (0/1) swap; 2/3/4 map straight through. (Empirically verified:
    // truth 77236587 kA4=0→1x, truth 21227933 kA4=2→2x.)
    player.speed = numFromString<int>(get_or("kA4", "0"));
    if (player.speed == 0) player.speed = 1;       // Normal → 1x
    else if (player.speed == 1) player.speed = 0;  // Slow   → 0.5x
    player.xSpeed = player.pendingXSpeed = player.speed; // X-lag starts in sync

    if ((player.small = (bool)numFromString<int>(get_or("kA3", "0"))))
        player.size = player.size * 0.6f;

    player.upsideDown = (bool)numFromString<int>(get_or("kA11", "0"));
    player.vehicle = Vehicle::from(static_cast<VehicleType>(numFromString<int>(get_or("kA2", "0"))));
    // A level that STARTS in a vehicle never fires that vehicle's `enter` transition
    // (enter only runs when a portal switches INTO it mid-level). Most enters just
    // adjust velocity for the transition — which must NOT apply at spawn — but the
    // WAVE's enter also sets the player's true small hitbox size (6/10, not the cube's
    // 30/18). Without it a level starting as a wave keeps the cube size, and the wave
    // `clamp` then floor-snaps it to a wrong Y — killing the run at frame 1 (e.g. the
    // mini-wave on level 130084245 died at spawn, so the solver called it impossible).
    if (player.vehicle.type == VehicleType::Wave)
        player.size = player.small ? Vec2D{6, 6} : Vec2D{10, 10};
    player.floor   = 0;
    player.ceiling = player.vehicle.bounds;
}

Level::Level(std::string const& lvlString) {
    std::string normalized = normalizeLevelString(lvlString);
    std::stringstream ss(normalized);
    std::string objstr;
    bool first = true;
    auto player = Player();

    // First pass: parse triggers into `triggers`, and collision objects into a
    // temp list with their group memberships (so movable ones can be split out
    // once we know which groups any trigger targets).
    std::vector<std::pair<ObjectContainer, std::vector<int>>> created;

    while (std::getline(ss, objstr, ';')) {
        if (first) {
            initLevelSettings(objstr, player);
            first = false;
            continue;
        }

        std::unordered_map<int, std::string> obj;
        std::stringstream ss2(objstr);
        std::string k, v;
        while (std::getline(ss2, k, ',')) {
            std::getline(ss2, v, ',');
            if (numFromString<int>(k) > 0)
                obj[numFromString<int>(k)] = v;
        }

        const int id = obj.count(1) ? numFromString<int>(obj[1]) : 0;

        if (isTriggerId(id)) {                       // a trigger, not a collision object
            if (auto tr = parseTrigger(id, obj)) triggers.push_back(*tr);
            continue;
        }

        // id 31 = Start Pos. Deliberately IGNORED: it only relocates the spawn for the
        // creator's own in-editor "practice from here" convenience — a level can only be
        // verified/recorded completing in NORMAL mode, which always spawns at the level's
        // true x=0 start regardless of how many Start Pos objects are scattered through
        // it. Honoring the LAST one parsed (the old behaviour) silently teleported the
        // simulated spawn to wherever a creator's editor-navigation marker happened to
        // sit — often deep into the level — so the solver "solved" the level in 0 clicks
        // by spawning past every real obstacle. Real levels routinely carry several Start
        // Pos objects (one per section, for editor jump-to-here convenience), so this
        // isn't a rare edge case.
        if (id == 31) continue;

        std::vector<int> groups = parseGroups(obj.count(57) ? obj[57] : std::string());

        if (auto ob_o = Object::create(std::move(obj))) {
            auto ob = ob_o.value();
            ob->id = (int)objectCount++;
            if (ob->pos.x > length) length = ob->pos.x + 100;
            created.emplace_back(ob, std::move(groups));
        }
    }

    // Which groups are animated by a trigger → their collision objects are movable.
    // Only Move/Rotate/Toggle/Follow actually change an object's collision (position,
    // rotation, or on/off). Alpha changes ONLY visibility (invisible objects still
    // collide in GD), and Spawn's target is a group of TRIGGERS, not collision
    // objects — so neither should drag objects into the expensive per-frame posing
    // path. Excluding them is what keeps a heavily-decorated-but-lightly-moved level
    // (thousands of Alpha/fade triggers) solvable instead of posing the whole map.
    std::unordered_set<int> movableGroups;
    for (auto& t : triggers)
        if (t.kind == TriggerKind::Move || t.kind == TriggerKind::Rotate ||
            t.kind == TriggerKind::Toggle || t.kind == TriggerKind::Follow)
            movableGroups.insert(t.targetGroup);

    for (auto& [oc, groups] : created) {
        bool mov = false;
        for (int g : groups) if (movableGroups.count(g)) { mov = true; break; }
        if (mov) {
            int idx = (int)movable.size();
            movable.push_back(oc);
            movableMeta.push_back({idx, oc->pos, oc->rotation});
            for (int g : groups) if (movableGroups.count(g)) groupToMovable[g].push_back(idx);
        } else {
            size_t sectionPos = (size_t)std::max(.0f, oc->pos.x / (float)sectionSize);
            if (sectionPos >= sections.size()) sections.resize(sectionPos + 1);
            sections[sectionPos].push_back(oc);
        }
    }

    player.level = this;
    gameStates.push_back(player);
    // gameStates2 stays empty until the first dual portal (lazy, zero overhead).

    hasTriggers = !movable.empty();
    if (hasTriggers) {
        std::sort(triggers.begin(), triggers.end(),
                  [](const Trigger& a, const Trigger& b) { return a.x < b.x; });
        buildTriggerTimeline();   // reads gameStates[0]; must run after push_back

        // Invert group→trigger into movable-object→triggers (X-sorted) and measure
        // each object's max X travel, so per step we pose only the objects near the
        // player from their own triggers (rollback-safe, no level-wide rescan).
        movableToTriggers.assign(movable.size(), {});
        std::vector<float> objMoveExtent(movable.size(), 0.f);
        for (int ti = 0; ti < (int)triggers.size(); ++ti) {
            const Trigger& t = triggers[ti];
            auto it = groupToMovable.find(t.targetGroup);
            if (it == groupToMovable.end()) continue;
            for (int idx : it->second) {
                movableToTriggers[idx].push_back(ti);
                if (t.kind == TriggerKind::Move)
                    objMoveExtent[idx] += std::abs(t.moveX) * g_calib.moveScale;
            }
        }
        // Second pass: a follower's X reach is bounded by the follow group's own
        // move extent (times |xMod|). Needs pass 1's move extents, hence separate.
        for (const Trigger& t : triggers) {
            if (t.kind != TriggerKind::Follow) continue;
            auto tgt = groupToMovable.find(t.targetGroup);
            auto src = groupToMovable.find(t.followGroup);
            if (tgt == groupToMovable.end() || src == groupToMovable.end() ||
                src->second.empty()) continue;
            const float srcExtent = objMoveExtent[src->second.front()] * std::abs(t.followXMod);
            for (int idx : tgt->second) objMoveExtent[idx] += srcExtent;
        }
        movableExtent = objMoveExtent;   // keep per-object extent for the scan gate
        movableStartSortedX.reserve(movable.size());
        for (int idx = 0; idx < (int)movable.size(); ++idx) {
            movableStartSortedX.push_back({movableMeta[idx].startPos.x, idx});
            maxMoveExtent = std::max(maxMoveExtent, objMoveExtent[idx]);
        }
        std::sort(movableStartSortedX.begin(), movableStartSortedX.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
    }
}

// ── Phase 2: trigger fire-frames from a deterministic X(frame) timeline ──────
// The player's X advances at an input-independent rate (speed changes only at
// speed portals, which are X-positioned), so the frame each X-activated trigger
// fires is fixed. We approximate by assuming the player passes every speed portal
// (true for almost all paths) and walking X forward frame by frame.
void Level::buildTriggerTimeline() {
    // Speed changes (x, speedIndex) from speed-portal objects in the static set.
    std::vector<std::pair<float,int>> speedChanges;
    for (auto& sec : sections)
        for (auto& oc : sec) {
            int tid = oc->typeId, sp = -1;
            switch (tid) { case 200: sp=0; break; case 201: sp=1; break;
                           case 202: sp=2; break; case 203: sp=3; break; case 1334: sp=4; break; }
            if (sp >= 0) speedChanges.push_back({oc->pos.x, sp});
        }
    std::sort(speedChanges.begin(), speedChanges.end(),
              [](auto& a, auto& b){ return a.first < b.first; });

    const float dt = 1.f / 240.f;
    int   speed = gameStates[0].speed;
    float x     = gameStates[0].pos.x;
    size_t si = 0, ti = 0;

    // Triggers BEHIND the spawn: the player never crosses them, so GD has already
    // applied them when the level starts (same fast-forward as a Start Pos). Marking
    // them pre-applied (end value held from frame 0) instead of letting the walk
    // below fire them at frame 1 — which animated them across the spawn and, e.g.,
    // swept a -150u move-triggered platform straight through the falling player.
    // `triggers` is X-sorted, so these are exactly the leading entries.
    while (ti < triggers.size() && triggers[ti].x < x) {
        triggers[ti].fireFrame  = 0;
        triggers[ti].preApplied = true;
        ++ti;
    }
    // Bound: enough frames to cross the whole level at the slowest speed.
    const uint64_t maxF = (uint64_t)((double)(length + 600.f) / player_speeds[0] / dt) + 480;
    for (uint64_t f = 1; f <= maxF && ti < triggers.size(); ++f) {
        x += (float)(player_speeds[speed] * dt);
        while (si < speedChanges.size() && x >= speedChanges[si].first) speed = speedChanges[si++].second;
        while (ti < triggers.size() && triggers[ti].x <= x) triggers[ti++].fireFrame = (int)f;
    }

    // Spawn/touch triggers do NOT fire by X — drop the X fire-frame the walk gave
    // them. Touch stays unfired (path-dependent, not modelled). Spawn-triggered ones
    // get their fire-frame from the spawn propagation below.
    // (Also clears preApplied: a touch/spawn trigger behind the spawn is still gated
    // on its touch/spawn event — being before the start does not auto-apply it.)
    for (auto& t : triggers)
        if (t.spawnTriggered || t.touchTriggered) { t.fireFrame = -1; t.preApplied = false; }

    // ── Spawn propagation ───────────────────────────────────────────────────
    // A Spawn trigger fires its targetGroup's members at fireFrame + spawnDelay.
    // Roots are X-activated spawns (fireFrame already set); iterate so spawn->spawn
    // chains resolve. Fully deterministic (all rooted at X), so the per-frame pose
    // cache stays valid. Earliest activation wins (a group is usually spawned once).
    std::unordered_map<int, std::vector<int>> groupTriggers;
    for (int i = 0; i < (int)triggers.size(); ++i)
        for (int g : triggers[i].ownGroups)
            groupTriggers[g].push_back(i);

    for (int iter = 0; iter < 64; ++iter) {
        bool changed = false;
        for (int si2 = 0; si2 < (int)triggers.size(); ++si2) {
            const Trigger& s = triggers[si2];
            if (s.kind != TriggerKind::Spawn || s.fireFrame < 0) continue;
            int fire = s.fireFrame + (int)std::lround((double)s.spawnDelay * 240.0);
            auto it = groupTriggers.find(s.targetGroup);
            if (it == groupTriggers.end()) continue;
            for (int tg : it->second) {
                if (tg == si2 || !triggers[tg].spawnTriggered) continue;
                if (triggers[tg].fireFrame < 0 || fire < triggers[tg].fireFrame) {
                    triggers[tg].fireFrame = fire;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
}

// ── Phase 3: pose one movable object for frame f from its own triggers ────────
// Start at the object's recorded pose, then add the eased contribution of each of
// ITS triggers that has fired by frame f. Moves/rotations compose additively;
// toggles are applied in X order so the last-fired one wins. Depends only on f
// (and the object's fixed start pose), so it is identical across every search
// branch and exact after the solver rolls a player back to an earlier frame.
void Level::poseMovable(int idx, int f, Vec2D& pos, float& rot, bool& active) const {
    pos    = movableMeta[idx].startPos;
    rot    = movableMeta[idx].startRot;
    active = true;

    const float dt = 1.f / 240.f;
    for (int ti : movableToTriggers[idx]) {
        const Trigger& t = triggers[ti];
        if (t.fireFrame < 0 || t.fireFrame > f) continue;   // not fired (X or spawn)
        if (t.touchTriggered) continue;                      // touch = path-dependent, not modelled

        // preApplied (behind spawn) → already at its end value on frame 0.
        float prog = t.preApplied ? 1.f
                   : (t.duration > 0.f) ? (float)(f - t.fireFrame) * dt / t.duration
                                        : 1.f;
        // Completed triggers hold their end value (e == 1); skip the transcendental
        // easing for them — most triggers near the player have long since finished.
        float e = (prog >= 1.f) ? 1.f : easeValue(prog, t.easing, t.easeRate);

        if (t.kind == TriggerKind::Move) {
            pos.x += t.moveX * g_calib.moveScale * e;
            pos.y += t.moveY * g_calib.moveScale * e;
        } else if (t.kind == TriggerKind::Rotate) {
            rot += t.degrees * e;
        } else if (t.kind == TriggerKind::Toggle) {
            active = t.toggleOn;   // appearing/disappearing collision (last wins)
        } else if (t.kind == TriggerKind::Follow) {
            // Copy the follow group's MOVE displacement accrued over the follow
            // window [fireFrame, fireFrame+duration], scaled by the follow mods.
            int fEnd = f;
            if (t.duration > 0.f) {
                int end = t.fireFrame + (int)std::lround((double)t.duration * 240.0);
                if (fEnd > end) fEnd = end;
            }
            Vec2D now  = moveOffsetOfGroupAt(t.followGroup, fEnd);
            Vec2D base = moveOffsetOfGroupAt(t.followGroup, t.fireFrame);
            pos.x += (now.x - base.x) * t.followXMod;
            pos.y += (now.y - base.y) * t.followYMod;
        }
        // Alpha doesn't change collision in GD (invisible objects still collide).
    }
}

// Move-only offset of a representative object in `group` at frame f (Follow uses
// this to copy the group's motion). All members of a group share the same move
// triggers, so the first movable index is representative.
Vec2D Level::moveOffsetOfGroupAt(int group, int f) const {
    Vec2D off{0.f, 0.f};
    auto git = groupToMovable.find(group);
    if (git == groupToMovable.end() || git->second.empty()) return off;
    const int idx = git->second.front();
    const float dt = 1.f / 240.f;
    for (int ti : movableToTriggers[idx]) {
        const Trigger& t = triggers[ti];
        if (t.kind != TriggerKind::Move) continue;          // move-only → no recursion
        if (t.fireFrame < 0 || t.fireFrame > f) continue;
        if (t.touchTriggered) continue;
        float prog = (t.duration > 0.f)
            ? (float)(f - t.fireFrame) * dt / t.duration : 1.f;
        float e = (prog >= 1.f) ? 1.f : easeValue(prog, t.easing, t.easeRate);
        off.x += t.moveX * g_calib.moveScale * e;
        off.y += t.moveY * g_calib.moveScale * e;
    }
    return off;
}

// Reflect the primary into the dual mirror at the moment dual turns on: opposite
// gravity, position mirrored across the play-area centre, inverted velocity.
static Player spawnMirror(const Player& p1) {
    Player m = p1;
    // Play-area centre: floor + half the flight column. Cube/ball have no finite
    // ceiling, so fall back to the default ~300-unit screen column (centre 150).
    float half = (p1.vehicle.bounds < 100000.f) ? p1.vehicle.bounds * 0.5f : 150.f;
    float axis = p1.floor + half;
    m.pos.y      = 2.f * axis - p1.pos.y;
    m.upsideDown = !p1.upsideDown;
    m.velocity   = -p1.velocity;
    m.dead       = false;
    m.deathCause = nullptr;
    m.dual       = true;
    m.isMirror   = true;
    return m;
}

Player Level::stepPlayer(Player p, bool pressed, float dt) {
    p.dt = dt;
    p.preCollision(pressed);
    size_t sectionIdx = (size_t)std::min(
        std::max(0, (int)(p.pos.x / sectionSize)),
        (int)sections.size() - 1);

    auto prevSection = &sections[sectionIdx == 0 ? 0 : sectionIdx - 1];
    auto currSection = &sections[sectionIdx];
    auto nextSection = &sections[sectionIdx + 1 >= sections.size() - 1 ? sections.size() - 1 : sectionIdx + 1];

    std::vector<ObjectContainer>* secs[3] = {prevSection, nullptr, nullptr};
    if (&currSection != &prevSection) secs[1] = currSection;
    if (&nextSection != &currSection) secs[2] = nextSection;

    std::vector<ObjectContainer> blocks, hazards, effects;
    blocks.reserve(100);
    hazards.reserve(100);

    for (auto section : secs) {
        if (section == nullptr) continue;
        for (auto& o : *section) {
            if (p.dead) break;
            if (o->prio == 1)      blocks.push_back(o);
            else if (o->prio == 2) hazards.push_back(o);
            else if (o->touching(p)) effects.push_back(o);  // collide in X order below
        }
    }

    // Trigger-animated collision objects: pose ONLY the ones whose start X (widened
    // by their max travel) is near the player, recomputed for this exact frame from
    // each object's own triggers, then fold into the same block/hazard/effect path.
    if (hasTriggers) {
        const float reach = 220.f + maxMoveExtent;
        const float lo = p.pos.x - reach, hi = p.pos.x + reach;
        auto first = std::lower_bound(movableStartSortedX.begin(), movableStartSortedX.end(), lo,
                         [](const std::pair<float,int>& e, float v){ return e.first < v; });
        for (auto it = first; it != movableStartSortedX.end() && it->first <= hi; ++it) {
            if (p.dead) break;
            int idx = it->second;
            // Cheap per-object gate: this object can only reach the player if its OWN
            // travel extent brings it within posing range. Skips the expensive
            // poseMovable for the many objects the wide (global-extent) window admits
            // but that can't actually get near.
            if (std::abs(it->first - p.pos.x) > 220.f + movableExtent[idx]) continue;
            Vec2D pos; float rot; bool active;
            poseMovable(idx, p.frame, pos, rot, active);
            if (!active) continue;                              // toggled off → no collision
            if (std::abs(pos.x - p.pos.x) > 220.f) continue;    // not actually in reach now
            auto& oc = movable[idx];
            Object* o = oc.operator->();
            o->pos = pos;                                       // commit pose for touching/collide
            o->rotation = rot;
            if (o->prio == 1)      blocks.push_back(oc);
            else if (o->prio == 2) hazards.push_back(oc);
            else if (o->touching(p)) effects.push_back(oc);
        }
    }

    // Apply effects (portals/orbs/pads) in the order the player ENCOUNTERS them —
    // left to right by X — not arbitrary parse order. Matters when two effects
    // overlap in one step: e.g. a gravity portal immediately before a teleport
    // portal must flip gravity BEFORE the teleport moves the player out of its
    // range (otherwise the flip is missed and the player dies in wrong gravity).
    std::sort(effects.begin(), effects.end(),
              [](const ObjectContainer& a, const ObjectContainer& b) {
                  return a.operator->()->pos.x < b.operator->()->pos.x;
              });
    const bool upBeforeEffects = p.upsideDown;
    for (auto& e : effects) {
        if (p.dead) break;
        if (e->touching(p)) {
#ifdef DEBUG_CUBE_START
            std::fprintf(stderr, "DEBUG effect touch frame=%d x=%f y=%f type=%d prio=%d used=%d\n",
                         p.frame, p.pos.x, p.pos.y, e->typeId, e->prio,
                         p.usedEffects.contains(e->id) ? 1 : 0);
#endif
            e->collide(p);   // re-check: a prior effect may have moved us
        }
    }

    // Gravity flipped under a WAVE this frame (gravity portal, blue/green orb, or
    // gravity pad). GD flips gravity BEFORE integrating Y on the flip frame, so the
    // wave reverses that very frame. Our pipeline integrates Y in preCollision (with
    // the pre-flip gravity) and only resolves effects here, afterward — leaving one
    // frame moving the wrong way (~one wave step). Because the wave's velocity is a
    // pure function of input recomputed every frame, that single frame froze a ~6.5u
    // offset for the rest of the segment (proven on level 38564711, f1385: a blue orb
    // clicked on the press frame). Re-integrate this frame's Y delta with the
    // now-flipped gravity. grav_old == -grav_new on a flip, so the correction is
    // +2*grav_new(v0)*dt. Excludes spider orb (grounds + repositions Y explicitly).
    if (p.upsideDown != upBeforeEffects && !p.dead && !p.grounded
        && p.vehicle.type == VehicleType::Wave) {
        const double v0 = (p.input * 2 - 1) * player_speeds[(int)p.speed]
                          * (p.small ? 2.0 : 1.0);
        p.pos.y += (float)(2.0 * p.grav(v0) * p.dt);
    }

    for (int i = (int)blocks.size() - 1; i >= 0; --i) {
        if (p.dead) break;
        auto& b = blocks[i];
        if (b->touching(p)) b->collide(p);
    }

    // Total hazard-hitbox margin = calibrated accuracy value (match GD's hitboxes)
    // + the solver's optional extra safety. Restore size before postCollision so
    // nothing else sees the inflated box.
    float hzInflate = g_calib.hazardHitboxInflate + hazardInflate;
    // Flight modes (ship/ufo/wave/swing) are the least reliable to simulate — fast,
    // hitbox-sensitive, and steep mini-waves amplify any sub-frame timing residual
    // into several units. So the SOLVER keeps extra clearance from hazards there;
    // flightHazardInflate is 0 during playback/divergence (those stay accurate).
    if (flightHazardInflate > 0.f) {
        const VehicleType vt = p.vehicle.type;
        if (vt == VehicleType::Ship || vt == VehicleType::Ufo ||
            vt == VehicleType::Wave || vt == VehicleType::Swing)
            hzInflate += flightHazardInflate;
    }
    if (hzInflate > 0.f) {
        const Vec2D realSize = p.size;
        p.size = realSize + Vec2D(2.f * hzInflate, 2.f * hzInflate);
        for (auto& h : hazards) {
            if (p.dead) break;
            if (h->touching(p)) h->collide(p);
        }
        p.size = realSize;
    } else {
        for (auto& h : hazards) {
            if (p.dead) break;
            if (h->touching(p)) h->collide(p);
        }
    }

    if (!p.dead) p.postCollision();

    // Cache the stair-snap reference x on the frame the cube lands (landingFrame was
    // set to this exact frame during collision), but ONLY while still grounded. On
    // an orb/jump the frame after touchdown, Block::collide re-stamps landingFrame
    // but the effect has already launched the cube (grounded=false); caching there
    // overwrites refX with the post-launch X — one X-step (~1.3u) too far forward,
    // over-driving the next stair snap into its ±threshold clamp (proven on truth
    // 308 f1778→1779: real locks refX at the 2308.29 landing; gdsim was overwriting
    // it with the 2309.59 launch frame). The `grounded` guard keeps refX correct
    // through multi-frame grounded walks (needed by 174's pre-ship cube phase) while
    // rejecting the launched frame. On non-landing frames refX carries forward via
    // the state copy; works under solver state injection without a gameStates lookup.
    if (p.snapData.landingFrame == p.frame && p.grounded)
        p.snapData.refX = p.pos.x;

    return p;
}

Player& Level::runFrame(bool pressed, float dt) {
    if (gameStates.back().dead) return gameStates.back();

    const bool wasDual = gameStates.back().dual;
    Player p = stepPlayer(gameStates.back(), pressed, dt); // may toggle p.dual via DualPortal

    // Mirror only runs during a validation pass (simulateMirror). During the
    // search it's off, so this is single-player speed and the search is never
    // dead-ended by an approximate dual mirror. Non-dual levels never enter here.
    if (simulateMirror && (p.dual || dualEverActive)) {
        if (p.dual && !dualEverActive) {
            // First dual portal of the whole level: backfill the mirror history
            // to match (one-time) so indices stay in lockstep from here on.
            dualEverActive = true;
            gameStates2 = gameStates;
        }
        Player p2 = gameStates2.back();
        if (p.dual) {
            // Spawn the mirror the frame dual turns on (reflect the pre-step
            // primary), then advance it alongside the primary. We deliberately do
            // NOT kill the primary when the mirror dies: this level's dual is not
            // a clean mirror (no single symmetry axis), so a wrong/approximate
            // mirror would dead-end the search at the dual portal. The mirror is
            // simulated for detection/validation only; mirrorDead records it.
            if (!wasDual) p2 = spawnMirror(gameStates.back());
            if (!p2.dead) p2 = stepPlayer(std::move(p2), pressed, dt);
            if (p2.dead) mirrorDead = true;
        } else {
            p2 = p; // dual off: mirror tracks the primary
        }
        gameStates2.push_back(std::move(p2));
    }

    gameStates.push_back(std::move(p));
    return gameStates.back();
}

void Level::rollback(int frame) {
    int n = frame > 0 ? frame : 1;
    gameStates.resize(n);
    if (!gameStates2.empty()) gameStates2.resize(n); // lockstep only once latched
}

// Clearance to the nearest hazard: binary-search the largest inflation `m` of
// the player's hitbox that still touches nothing. touching() for hazards reads
// only pos+size (rect spikes via unrotatedHitbox, sawblades via getLeft/getTop),
// so growing `size` by 2m on a throwaway copy probes the gap on all sides.
float Level::hazardClearance(const Player& p, float maxProbe) const {
    if (sections.empty() || maxProbe <= 0.f) return maxProbe;

    const int si = std::min(std::max(0, (int)(p.pos.x / sectionSize)),
                            (int)sections.size() - 1);
    const int lo = std::max(0, si - 1);
    const int hi = std::min((int)sections.size() - 1, si + 1);

    Player probe = p;                        // one copy; only `size` is mutated below
    auto touchesAny = [&](float m) -> bool {
        probe.size = p.size + Vec2D(2.f * m, 2.f * m);
        for (int s = lo; s <= hi; ++s) {
            for (auto& oc : sections[s]) {
                const Object* o = oc.operator->();
                if (o->prio != 2) continue;  // hazards only (spikes, sawblades)
                if (o->touching(probe)) return true;
            }
        }
        return false;
    };

    if (touchesAny(0.f)) return 0.f;         // a live (non-dead) state shouldn't, but be safe
    float a = 0.f, b = maxProbe;
    for (int it = 0; it < 10; ++it) {        // ~maxProbe/1024 resolution
        float mid = 0.5f * (a + b);
        if (touchesAny(mid)) b = mid; else a = mid;
    }
    return a;
}

// Cheap one-shot safety test (no binary search): would the player's hitbox,
// inflated by `margin` on every side, still avoid every nearby hazard? The beam
// uses this to prefer flight states that keep clearance over ones that graze.
bool Level::clearsHazards(const Player& p, float margin) const {
    if (sections.empty() || margin <= 0.f) return true;

    const int si = std::min(std::max(0, (int)(p.pos.x / sectionSize)),
                            (int)sections.size() - 1);
    const int lo = std::max(0, si - 1);
    const int hi = std::min((int)sections.size() - 1, si + 1);

    Player probe = p;
    probe.size = p.size + Vec2D(2.f * margin, 2.f * margin);
    for (int s = lo; s <= hi; ++s) {
        for (auto& oc : sections[s]) {
            const Object* o = oc.operator->();
            if (o->prio != 2) continue;      // hazards only
            if (o->touching(probe)) return false;
        }
    }
    return true;
}

int Level::currentFrame() const { return (int)gameStates.size(); }

Player const& Level::getState(int frame) const {
    if (frame == 0) return gameStates[0];
    if ((int)gameStates.size() < frame) return gameStates.back();
    return gameStates[frame - 1];
}

Player& Level::latestState() { return gameStates.back(); }

} // namespace gdsim
