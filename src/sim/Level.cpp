#include "Level.hpp"
#include "Calib.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_set>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

    // Spawn group (kA36, LevelSettingsObject+0x1c4) and platformer flag (kA22, +0x155):
    // GJBaseGameLayer::resetPlayer (0x1402120b0) puts the player on an object of the
    // spawn group instead of (0,105) — Y only, unless platformer. Applied once the
    // objects are parsed (Level ctor). ALLOY 123617195 spawns at real Y 975 this way.
    spawnGroup     = numFromString<int>(get_or("kA36", "0"));
    platformerMode = numFromString<int>(get_or("kA22", "0")) != 0;

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
    // Seed the solver block margins from the Physics Lab's live globals so editing them
    // there still affects a newly built Level (see Level.hpp). The solver overwrites the
    // per-Level copy for its own run; replay/divergence Levels keep whatever the lab set,
    // which is the same behaviour the globals had.
    solveShipBlockClearance  = g_solveShipBlockClearance;
    solveRobotBlockClearance = g_solveRobotBlockClearance;

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

        if (obj.count(57)) {                         // teleport/spawn targets (Level.hpp)
            const Vec2D at{(float)stod_def(obj[2]), (float)stod_def(obj[3])};
            for (int g : parseGroups(obj[57])) groupAnchor.emplace(g, at);
            // Field 274 = the groups this object is the PARENT of
            // (GJBaseGameLayer::loadGroupParentsFromString; setGroupParent: last wins).
            if (obj.count(274))
                for (int g : parseGroups(obj[274])) groupParentPos[g] = at;
        }

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

    int dbgPortalCreated = 0, dbgPortalSections = 0, dbgPortalMovable = 0;
    for (auto& [oc, groups] : created) {
        bool isPortal = oc->typeId==12||oc->typeId==13||oc->typeId==47||oc->typeId==111||
                        oc->typeId==660||oc->typeId==745||oc->typeId==1331||oc->typeId==1933||oc->typeId==2751||
                        oc->typeId==747;
        if (isPortal) dbgPortalCreated++;
        bool mov = false;
        for (int g : groups) if (movableGroups.count(g)) { mov = true; break; }
        if (mov) {
            int idx = (int)movable.size();
            movable.push_back(oc);
            movableMeta.push_back({idx, oc->pos, oc->rotation});
            for (int g : groups) if (movableGroups.count(g)) groupToMovable[g].push_back(idx);
            if (isPortal) dbgPortalMovable++;
        } else {
            size_t sectionPos = (size_t)std::max(.0f, oc->pos.x / (float)sectionSize);
            if (sectionPos >= sections.size()) sections.resize(sectionPos + 1);
            sections[sectionPos].push_back(oc);
            if (isPortal) dbgPortalSections++;
        }
    }
    if (getenv("GDSIM_PORTAL_DEBUG"))
        std::fprintf(stderr, "LEVEL-LOAD: portals created=%d insections=%d inmovable=%d totalsections=%zu totalcreated=%zu\n",
                     dbgPortalCreated, dbgPortalSections, dbgPortalMovable, sections.size(), created.size());
    if (getenv("GDSIM_PORTAL_DEBUG")) {
        for (size_t idx = 0; idx < movable.size(); ++idx) {
            const Object* o = movable[idx].operator->();
            bool isPortal = o->typeId==12||o->typeId==13||o->typeId==47||o->typeId==111||
                            o->typeId==660||o->typeId==745||o->typeId==1331||o->typeId==1933||o->typeId==2751||
                            o->typeId==747;
            if (isPortal)
                std::fprintf(stderr, "  movable portal idx=%zu typeId=%d startPos=(%.2f,%.2f) numTriggers=(filled later)\n",
                             idx, o->typeId, movableMeta[idx].startPos.x, movableMeta[idx].startPos.y);
        }
    }

    if (spawnGroup > 0) {
        Vec2D at;
        if (groupObjectPos(spawnGroup, at)) {
            player.pos.y = at.y;
            if (platformerMode) player.pos.x = at.x;
            player.grounded = false;
        }
    }

    player.rotLastPos = player.pos;
    player.level = this;
    gameStates.push_back(player);
    // gameStates2 stays empty until the first dual portal (lazy, zero overhead).

    spawnPosRaw        = gameStates[0].pos;
    spawnUpsideDownRaw = gameStates[0].upsideDown;
    bool anyTeleport = false;
    for (const Trigger& t : triggers) anyTeleport |= t.kind == TriggerKind::Teleport;
    hasTriggers = !movable.empty() || anyTeleport;
    if (hasTriggers) {
        std::sort(triggers.begin(), triggers.end(),
                  [](const Trigger& a, const Trigger& b) { return a.x < b.x; });
        for (int ti = 0; ti < (int)triggers.size(); ++ti)
            if (triggers[ti].kind == TriggerKind::Teleport) teleportTriggers.push_back(ti);
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

        // `triggers` is already X-sorted above, so filtering preserves order — no
        // separate sort needed.
        for (int ti = 0; ti < (int)triggers.size(); ++ti)
            if (triggers[ti].touchTriggered)
                touchTriggerSortedX.push_back({triggers[ti].x, ti});
    }
}

// ── Phase 2: trigger fire-frames from a deterministic X(frame) timeline ──────
// The player's X advances at an input-independent rate (speed changes only at
// speed portals, which are X-positioned), so the frame each X-activated trigger
// fires is fixed. We approximate by assuming the player passes every speed portal
// (true for almost all paths) and walking X forward frame by frame.
// The player X on each frame, index 0..maxFrames. Factored out of the trigger
// timeline below so external tooling (test/moveanalyze.cpp) can map a real
// capture back onto a sim frame using the ENGINE'S OWN walk instead of a copy of
// it — a private copy would risk measuring the copy's bug rather than gdsim's.
void Level::buildXTable(std::vector<float>& out, int maxFrames) const {
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
    size_t si = 0;
    out.assign((size_t)maxFrames + 1, x);
    for (int f = 1; f <= maxFrames; ++f) {
        x += (float)(player_speeds[speed] * dt);
        while (si < speedChanges.size() && x >= speedChanges[si].first) speed = speedChanges[si++].second;
        out[(size_t)f] = x;
    }
}

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
    // Reset-time teleports below rewrite gameStates[0]; start from the parsed spawn so
    // a rebuild (rollback / tunable edit) is idempotent.
    gameStates[0].pos        = spawnPosRaw;
    gameStates[0].upsideDown = spawnUpsideDownRaw;
    int   speed = gameStates[0].speed;
    float x     = gameStates[0].pos.x;
    size_t si = 0, ti = 0;

    // Triggers BEHIND the spawn: the player never crosses them, so GD has already
    // applied them when the level starts (same fast-forward as a Start Pos). Marking
    // them pre-applied (end value held from frame 0) instead of letting the walk
    // below fire them at frame 1 — which animated them across the spawn and, e.g.,
    // swept a -150u move-triggered platform straight through the falling player.
    // `triggers` is X-sorted, so these are exactly the leading entries.
    // PlayLayer::resetLevel runs checkSpawnObjects with the player at spawn, which
    // fires every trigger with x <= spawn X (`playerX < triggerX` breaks): <=, not <.
    while (ti < triggers.size() && triggers[ti].x <= x) {
        triggers[ti].fireFrame  = 0;
        triggers[ti].preApplied = true;
        ++ti;
    }
    // Reset-time Teleport triggers (ALLOY 123617195 spawns at real Y 975 this way) move
    // the starting player itself. checkSpawnObjects reads the player X once, so every
    // trigger behind spawn fires even if a teleport moves the player mid-loop.
    for (int tk : teleportTriggers) {
        const Trigger& t = triggers[tk];
        if (t.preApplied && !t.touchTriggered && !t.spawnTriggered)
            applyTeleport(gameStates[0], t);
    }
    x = gameStates[0].pos.x;
    gameStates[0].rotLastPos = gameStates[0].pos;
    // Bound: enough frames to cross the whole level at the slowest speed.
    const uint64_t maxF = (uint64_t)((double)(length + 600.f) / player_speeds[0] / dt) + 480;
    // Record X per frame while we walk, for poseMovable's Lock-to-Player-X moves.
    // The walk must now run to maxF even once every trigger has a fire-frame,
    // since the table is consumed at arbitrary later frames.
    playerXAtFrame.assign((size_t)maxF + 1, x);
    for (uint64_t f = 1; f <= maxF; ++f) {
        x += (float)(player_speeds[speed] * dt);
        while (si < speedChanges.size() && x >= speedChanges[si].first) speed = speedChanges[si++].second;
        float teleportX = x; bool teleported = false;
        while (ti < triggers.size() && triggers[ti].x <= x) {
            Trigger& t = triggers[ti++];
            t.fireFrame = (int)f;
            // A teleport fired at the end of step f moves the X every later trigger is
            // measured against (checkSpawnObjects of the following steps).
            Vec2D to;
            if (t.kind == TriggerKind::Teleport && !t.touchTriggered && !t.spawnTriggered
                && teleportTarget(t, {x, gameStates[0].pos.y}, (int)f + 1, to)) {
                teleportX = to.x; teleported = true;
            }
        }
        playerXAtFrame[(size_t)f] = x;
        if (teleported) x = teleportX;
    }

    // Spawn/touch triggers do NOT fire by X — drop the X fire-frame the walk gave
    // them. Touch stays unfired (path-dependent, not modelled). Spawn-triggered ones
    // get their fire-frame from the spawn propagation below.
    //
    // EXCEPTION found 2026-08-10 (level 127323087 "Society"): a touch-triggered
    // trigger that is ALSO preApplied (positioned behind spawn, x < the player's
    // starting X) can NEVER fire under the blanket "wipe" this used to do — the
    // player only ever moves forward from spawn, so a touch-triggered object
    // behind it is not just "not yet touched", it is PERMANENTLY unreachable by
    // any mechanism this sim (or real GD's replay) has. Society's very first
    // teleport portal (id=747, right at spawn) is toggled OFF by default and
    // re-enabled via a spawn-chain rooted at a touch-triggered Spawn trigger at
    // x=-105 (with an outer root at x=-1065) — both were being wiped here,
    // permanently killing the chain and leaving the portal off for the entire
    // level, so the player floated through empty space exactly like Cobwebs did
    // before the PCAT_TELEPORT fix (see that fix's comment). A trigger placed
    // this far behind spawn, touch-triggered or not, is a standard GD technique
    // for "fire once, guaranteed, at attempt start" — not a real request to wait
    // for a physical touch that can't happen. Only wipe touch-triggered ones that
    // the X-walk did NOT already preApply (i.e. still ahead of spawn — those stay
    // unfired, matching the existing "touch = path-dependent" architecture).
    // Spawn-triggered ones are unconditionally wiped regardless of preApplied,
    // since their real fire-frame must come from their OWN parent chain below,
    // not their (often irrelevant) placement X — if that parent is itself
    // preApplied, the propagation loop resolves this correctly on its own.
    for (auto& t : triggers) {
        if (t.spawnTriggered) { t.fireFrame = -1; t.preApplied = false; }
        else if (t.touchTriggered && !t.preApplied) { t.fireFrame = -1; }
    }

    // ── Spawn propagation ───────────────────────────────────────────────────
    // A Spawn trigger fires its targetGroup's members at fireFrame + spawnDelay.
    // Roots are X-activated spawns (fireFrame already set); walk each root's chain
    // so spawn->spawn chains resolve — INCLUDING chains that loop back on
    // themselves. A cyclic spawn chain (e.g. two Spawn triggers that spawn each
    // other, a standard GD "pendulum" construction) is common and real GD keeps
    // firing it forever; found 2026-09-10 via level 123617195 "ALLOY" and
    // test/moveanalyze.exe measuring a Move trigger's group moving 4-5x further
    // in the real capture than gdsim's single-fire model produced (accumulated
    // relative Move offsets from repeated firings). See Trigger.hpp's
    // repeatPeriodFrames for the full derivation.
    //
    // Detection: a plain DFS over the graph of Spawn triggers (edges = "my
    // targetGroup contains this other spawn-triggered Spawn trigger", weighted by
    // spawnDelay). Re-entering a trigger that is still on the current DFS path
    // closes a cycle; the elapsed time since that trigger was first entered is
    // the period, and EVERY trigger between there and here (inclusive) shares it.
    // Still fully deterministic (a pure function of the level's own graph, not of
    // the search) so the solver's per-frame pose cache remains rollback-safe.
    std::unordered_map<int, std::vector<int>> groupTriggers;
    for (int i = 0; i < (int)triggers.size(); ++i)
        for (int g : triggers[i].ownGroups)
            groupTriggers[g].push_back(i);

    // Fires every NON-Spawn member of `group` (Spawn members are walked, not
    // fired here). `period`, if >0, marks the leaf as repeating at that rate —
    // never CLEARS an already-known period (a 0 here just means "this particular
    // path to the leaf doesn't happen to be the cyclic one"; another path already
    // marked it, and a plain wipe would lose that).
    auto fireGroupLeaves = [&](int group, int fire, int period) {
        auto it = groupTriggers.find(group);
        if (it == groupTriggers.end()) return;
        for (int tg : it->second) {
            Trigger& t = triggers[tg];
            if (t.kind == TriggerKind::Spawn || !t.spawnTriggered) continue;
            if (t.fireFrame < 0 || fire < t.fireFrame) t.fireFrame = fire;
            if (period > 0 && (t.repeatPeriodFrames == 0 || period < t.repeatPeriodFrames))
                t.repeatPeriodFrames = period;
        }
    };

    std::vector<char> onPath(triggers.size(), 0), visited(triggers.size(), 0);
    std::vector<int>  pathStack, pathFireTime;   // parallel: DFS ancestors + their fire time

    std::function<void(int, int)> walk = [&](int si2, int fire) {
        Trigger& s = triggers[si2];
        if (s.fireFrame < 0 || fire < s.fireFrame) s.fireFrame = fire;

        if (onPath[si2]) {
            // Cycle closes here: si2 is its own ancestor on the current path.
            // Mark every trigger from si2's first entry through here — the whole
            // loop — with the elapsed time as their shared period.
            auto pos = std::find(pathStack.begin(), pathStack.end(), si2);
            const int entryTime = pathFireTime[(size_t)std::distance(pathStack.begin(), pos)];
            const int period = fire - entryTime;
            if (period > 0)
                for (auto it2 = pos; it2 != pathStack.end(); ++it2) {
                    Trigger& c = triggers[*it2];
                    if (c.repeatPeriodFrames == 0 || period < c.repeatPeriodFrames)
                        c.repeatPeriodFrames = period;
                }
            return;
        }
        if (visited[si2]) return;   // already fully explored elsewhere

        onPath[si2] = 1;
        pathStack.push_back(si2);
        pathFireTime.push_back(fire);

        const int childFire = fire + (int)std::lround((double)s.spawnDelay * 240.0);
        auto it = groupTriggers.find(s.targetGroup);
        if (it != groupTriggers.end())
            for (int tg : it->second)
                if (tg != si2 && triggers[tg].kind == TriggerKind::Spawn && triggers[tg].spawnTriggered)
                    walk(tg, childFire);

        // Fire this trigger's own non-spawn leaves LAST, once any cycle a
        // descendant closed back through `s` has already set s.repeatPeriodFrames.
        fireGroupLeaves(s.targetGroup, childFire, s.repeatPeriodFrames);

        pathStack.pop_back();
        pathFireTime.pop_back();
        onPath[si2] = 0;
        visited[si2] = 1;
    };

    for (int si2 = 0; si2 < (int)triggers.size(); ++si2) {
        Trigger& s = triggers[si2];
        if (s.kind == TriggerKind::Spawn && !s.spawnTriggered && s.fireFrame >= 0 && !visited[si2])
            walk(si2, s.fireFrame);
    }
}

// ── Phase 3: pose one movable object for frame f from its own triggers ────────
// Start at the object's recorded pose, then add the eased contribution of each of
// ITS triggers that has fired by frame f. Moves/rotations compose additively;
// toggles are applied in X order so the last-fired one wins. Depends only on f
// (and the object's fixed start pose), so it is identical across every search
// branch and exact after the solver rolls a player back to an earlier frame.
// ENGINE TIMELINE (GJBaseGameLayer::update 0x140237850, per 240 Hz step): processCommands ->
// PlayerObject::update -> prepareMoveActions/processMoveActionsStep -> checkCollisions ->
// checkSpawnObjects. So a trigger the player passes in loop step F (Trigger::fireFrame, set by
// buildTriggerTimeline's X walk) is activated at the END of step F and first affects step F+1;
// a touch trigger is activated inside step F's collision pass, same result. A Move/Rotate
// command's elapsed time does NOT advance on its first step: GroupCommandObject2::reset sets the
// skip flag +0x1b0 and GroupCommandObject2::step consumes it instead of adding dt. Its eased
// progress in step g is therefore (g - F - 1) * dt / duration — measured exactly on truth
// 21227933 (group 9 rising floor, x=1 trigger: real motion ends at step 122, gdsim ended at 120).
// `f` here is Player::frame, which runs one ahead of the loop step (g = f - 1).
static inline bool engineActive(const Trigger& t, int g) {
    return t.preApplied || (t.fireFrame >= 0 && g >= t.fireFrame + 1);
}

void Level::poseMovable(int idx, int f, Vec2D& pos, float& rot, bool& active) const {
    pos    = movableMeta[idx].startPos;
    rot    = movableMeta[idx].startRot;
    active = true;

    const float dt = 1.f / 240.f;
    const int g = f - 1;   // loop step (see the timeline above)
    for (int ti : movableToTriggers[idx]) {
        const Trigger& t = triggers[ti];
        if (!engineActive(t, g)) continue;                   // not fired yet (X, spawn or touch)
        // Touch triggers act only once the replay-only detector fired them (the engine never
        // activates one without a touch, including the "behind spawn" ones).
        if (t.touchTriggered && (!modelTouchTriggers || t.preApplied)) continue;

        if (t.kind == TriggerKind::Move || t.kind == TriggerKind::Rotate) {
            // A trigger on a cyclic spawn chain (Trigger.hpp's repeatPeriodFrames)
            // fires again every period forever, and each firing's own offset is
            // RELATIVE — real GD accumulates them, it doesn't re-home to start —
            // so replay every firing up to f and sum. repeatPeriodFrames is 0 for
            // the overwhelming majority of triggers, where this is exactly the
            // single-firing loop that was here before (k=0, thisFire=t.fireFrame).
            int fireCount = 1;
            if (t.repeatPeriodFrames > 0 && g > t.fireFrame + 1)
                fireCount = 1 + (g - t.fireFrame - 1) / t.repeatPeriodFrames;
            // Safety rail, not a real limit: only a degenerate near-zero period
            // over a long level could reach this, and no real level needs it.
            if (fireCount > 20000) fireCount = 20000;

            for (int k = 0; k < fireCount; k++) {
                const int thisFire = t.fireFrame + k * t.repeatPeriodFrames;
                // A Silent move has no action at all in GD — the offset is added
                // to the objects the instant the trigger fires — so it is already
                // fully applied on its own fire frame regardless of duration.
                // Reset-activated triggers (preApplied: x <= spawn) are NOT held at their
                // end value: PlayLayer::resetLevel activates them through the ordinary
                // checkSpawnObjects path (fireFrame 0), so they animate from step 1 like any
                // other. (The audio-only flag +0x88a is the only reset special case.) Holding
                // them put 113443235's -1500u/100s blade wall on the spawn at frame 1.
                float prog = t.silent ? 1.f
                           : (t.duration > 0.f) ? (float)(g - thisFire - 1) * dt / t.duration
                                                 : 1.f;
                // Completed firings hold their end value (e == 1); skip the
                // transcendental easing for them — every firing but at most the
                // latest one or two has long since finished.
                float e = (prog >= 1.f) ? 1.f : easeValue(std::clamp(prog, 0.f, 1.f), t.easing, t.easeRate);
                if (t.kind == TriggerKind::Move) {
                    if (t.lockToPlayerX && !playerXAtFrame.empty()) {
                        // Lock to Player X: prepareMoveActions REPLACES this axis's
                        // eased delta with the player's own per-step X delta times
                        // moveModX, for as long as the command lives. Summed over
                        // the window that is just (X(end) - X(start)) * mod, and
                        // the player's X walk is exactly playerXAtFrame.
                        // duration <= 0 means the command never finishes (GD's
                        // `m_duration != -1.0` infinite case and the 0 case both
                        // land here), so it tracks the player to the current frame.
                        // The per-step player delta is applied from the command's
                        // first step (F+1) on, so the window is X(g) - X(F).
                        int endF = g;
                        if (t.duration > 0.f) {
                            int lim = thisFire + (int)std::lround((double)t.duration * 240.0);
                            if (endF > lim) endF = lim;
                        }
                        auto X = [&](int fr) -> float {
                            if (fr < 0) fr = 0;
                            if ((size_t)fr >= playerXAtFrame.size()) fr = (int)playerXAtFrame.size() - 1;
                            return playerXAtFrame[(size_t)fr];
                        };
                        pos.x += (X(endF) - X(thisFire)) * t.moveModX;
                    } else {
                        pos.x += t.moveX * g_calib.moveScale * e;
                    }
                    pos.y += t.moveY * g_calib.moveScale * e;
                } else {
                    rot += t.degrees * e;
                }
            }
        } else if (t.kind == TriggerKind::Toggle) {
            active = t.toggleOn;   // appearing/disappearing collision (last wins)
        } else if (t.kind == TriggerKind::Follow) {
            // Copy the follow group's MOVE displacement accrued over the follow
            // window [fireFrame, fireFrame+duration], scaled by the follow mods.
            // moveOffsetOfGroupAt takes a Player::frame like this function.
            int fEnd = f;
            if (t.duration > 0.f) {
                int end = t.fireFrame + 1 + (int)std::lround((double)t.duration * 240.0);
                if (fEnd > end) fEnd = end;
            }
            Vec2D now  = moveOffsetOfGroupAt(t.followGroup, fEnd);
            Vec2D base = moveOffsetOfGroupAt(t.followGroup, t.fireFrame + 1);
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
    const int g = f - 1;   // loop step, see poseMovable's timeline comment
    for (int ti : movableToTriggers[idx]) {
        const Trigger& t = triggers[ti];
        if (t.kind != TriggerKind::Move) continue;          // move-only → no recursion
        if (!engineActive(t, g)) continue;
        if (t.touchTriggered && (!modelTouchTriggers || t.preApplied)) continue;
        float prog = t.silent ? 1.f
                   : (t.duration > 0.f) ? (float)(g - t.fireFrame - 1) * dt / t.duration : 1.f;
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
    const auto snapBefore = p.snapData;   // see the stair-snap undo at the end
    const bool groundedBefore = p.grounded;
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

    // Reused per-Level buffers, not locals — see Level.hpp. clear() keeps the capacity,
    // so after the first few frames this allocates nothing at all.
    std::vector<ObjectContainer>& blocks    = scratchBlocks;
    std::vector<ObjectContainer>& hazards   = scratchHazards;
    std::vector<ObjectContainer>& effects   = scratchEffects;
    std::vector<ObjectContainer>& modifiers = scratchModifiers;
    blocks.clear();
    hazards.clear();
    effects.clear();
    modifiers.clear();
    blocks.reserve(100);
    hazards.reserve(100);

    for (auto section : secs) {
        if (section == nullptr) continue;
        for (auto& o : *section) {
            if (p.dead) break;
            if (o->prio == 1)      blocks.push_back(o);
            else if (o->prio == 2) hazards.push_back(o);
            else if (o->prio == 3) { if (o->touching(p)) modifiers.push_back(o); }
            // Every nearby effect is a CANDIDATE, tested at processing time (see the
            // effect loop below): a teleport earlier in the pass moves the player.
            else if (std::abs(o->pos.x - p.pos.x) < kEffectScanX) effects.push_back(o);
        }
    }

    // Fire any not-yet-fired touch trigger the player's hitbox now overlaps (see
    // Level.hpp's modelTouchTriggers comment: forward-replay-only, never during
    // solving). Must run BEFORE this frame's movable posing below so a trigger
    // fired this exact frame already affects this frame's pose (same-frame effect,
    // matching the rest of this codebase's philosophy for portals/orbs/gravity).
    if (modelTouchTriggers && !touchTriggerSortedX.empty()) {
        const float reach = 220.f;
        const float lo = p.pos.x - reach, hi = p.pos.x + reach;
        auto first = std::lower_bound(touchTriggerSortedX.begin(), touchTriggerSortedX.end(), lo,
                         [](const std::pair<float,int>& e, float v){ return e.first < v; });
        for (auto it = first; it != touchTriggerSortedX.end() && it->first <= hi; ++it) {
            Trigger& t = triggers[it->second];
            if (t.fireFrame >= 0) continue;   // already fired
            if (std::abs(p.pos.x - t.x) > kTouchTriggerHalfSize + p.size.x * 0.5f) continue;
            if (std::abs(p.pos.y - t.y) > kTouchTriggerHalfSize + p.size.y * 0.5f) continue;
            t.fireFrame = (int)p.frame - 1;   // activated in this loop step (p.frame - 1)
            if (getenv("GDSIM_TOUCHTRIG_DEBUG"))
                std::fprintf(stderr, "TOUCHTRIG fired ti=%d f=%d group=%d x=%.2f y=%.2f playerXY=(%.2f,%.2f)\n",
                             it->second, (int)p.frame, t.targetGroup, t.x, t.y, p.pos.x, p.pos.y);
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
            if (getenv("GDSIM_BLOCKTRIG_DEBUG") && p.frame >= 130 && p.frame <= 146
                && (movable[idx].operator->()->typeId == 1 || movable[idx].operator->()->typeId == 84)
                && std::abs(pos.x - p.pos.x) < 60.f) {
                const Object* o = movable[idx].operator->();
                std::fprintf(stderr, "BLOCKTRIG f=%llu idx=%d typeId=%d startPos=(%.2f,%.2f) posedPos=(%.2f,%.2f) active=%d numTrig=%zu\n",
                             (unsigned long long)p.frame, idx, o->typeId,
                             movableMeta[idx].startPos.x, movableMeta[idx].startPos.y, pos.x, pos.y, active, movableToTriggers[idx].size());
                for (int ti : movableToTriggers[idx]) {
                    const Trigger& t = triggers[ti];
                    std::fprintf(stderr, "    trig ti=%d kind=%d x=%.2f fireFrame=%d dur=%.3f moveXY=(%.2f,%.2f) easing=%d toggleOn=%d touchTrig=%d preApplied=%d\n",
                                 ti, (int)t.kind, t.x, t.fireFrame, t.duration, t.moveX, t.moveY, t.easing, t.toggleOn, t.touchTriggered, t.preApplied);
                }
            }
            if (getenv("GDSIM_MOVABLE_DEBUG") && movable[idx].operator->()->typeId == 747 && p.frame == 2) {
                std::fprintf(stderr, "MOVABLE-747 f=%llu idx=%d pos=(%.2f,%.2f) active=%d playerX=%.2f numTrig=%zu\n",
                             (unsigned long long)p.frame, idx, pos.x, pos.y, active, p.pos.x, movableToTriggers[idx].size());
                for (int ti : movableToTriggers[idx]) {
                    const Trigger& t = triggers[ti];
                    std::string og; for (int g : t.ownGroups) og += std::to_string(g) + ",";
                    std::fprintf(stderr, "    trig ti=%d kind=%d x=%.2f fireFrame=%d touchTrig=%d spawnTrig=%d toggleOn=%d targetGroup=%d preApplied=%d ownGroups=%s\n",
                                 ti, (int)t.kind, t.x, t.fireFrame, t.touchTriggered, t.spawnTriggered, t.toggleOn, t.targetGroup, t.preApplied, og.c_str());
                }
                for (size_t ti = 0; ti < triggers.size(); ++ti) {
                    const Trigger& t = triggers[ti];
                    if (t.kind == TriggerKind::Spawn && t.x < 2000.f) {
                        std::string og; for (int g : t.ownGroups) og += std::to_string(g) + ",";
                        std::fprintf(stderr, "  SPAWN-TRIG ti=%zu x=%.2f fireFrame=%d targetGroup=%d spawnDelay=%.2f preApplied=%d spawnTrig=%d touchTrig=%d ownGroups=%s\n",
                                     ti, t.x, t.fireFrame, t.targetGroup, t.spawnDelay, t.preApplied, t.spawnTriggered, t.touchTriggered, og.c_str());
                    }
                }
            }
            if (!active) continue;                              // toggled off → no collision
            if (std::abs(pos.x - p.pos.x) > 220.f) continue;    // not actually in reach now
            auto& oc = movable[idx];
            Object* o = oc.operator->();
            o->pos = pos;                                       // commit pose for touching/collide
            o->rotation = rot;
            if (o->prio == 1)      blocks.push_back(oc);
            else if (o->prio == 2) hazards.push_back(oc);
            else if (o->prio == 3) { if (o->touching(p)) modifiers.push_back(oc); }
            else if (std::abs(o->pos.x - p.pos.x) < kEffectScanX) effects.push_back(oc);
        }
    }

    // Special letter blocks (J/S/H/F/Force — see SpecialBlock.hpp): resolve BEFORE
    // anything else this frame. Real GD reads/writes these as flags/state that the
    // jump-buffer, dash, block-death, and gravity logic below all consult
    // synchronously, so they must never depend on where a modifier happens to fall
    // in the effects bucket's left-to-right X sort (a modifier co-located with the
    // thing it modifies isn't guaranteed to sort before it).
    for (auto& m : modifiers) {
        if (p.dead) break;
        if (m->touching(p)) m->collide(p);
    }

    // Engine order (GJBaseGameLayer::collisionCheckObjects): sections left to right,
    // each section's objects in m_uniqueID order (level-string order = Object::id), and
    // every non-solid is tested against the player's CURRENT rect when its turn comes —
    // the rect is refreshed after a teleport (LAB_140215e78). So a portal that only
    // overlaps the teleport's exit fires in the same step if it comes later in that
    // order (truth 85701165 f827: wave teleported to y 835 and turned cube there on the
    // same frame via the cube portal at (1321.6,825.7)). The old left-to-right X sort,
    // with touching() decided before any effect ran, missed it.
    std::sort(effects.begin(), effects.end(),
              [](const ObjectContainer& a, const ObjectContainer& b) {
                  const Object* oa = a.operator->(); const Object* ob = b.operator->();
                  const int sa = (int)std::floor(oa->pos.x / (float)sectionSize);
                  const int sb = (int)std::floor(ob->pos.x / (float)sectionSize);
                  return sa != sb ? sa < sb : oa->id < ob->id;
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
    if (p.upsideDown != upBeforeEffects && getenv("GDSIM_GRAVFLIP_DEBUG")) {
        std::fprintf(stderr, "GRAVFLIP-RAW f=%d veh=%d dead=%d grounded=%d upBefore=%d upAfter=%d gravPortal=%d preFrameVel=%.3f\n",
                     p.frame, (int)p.vehicle.type, p.dead, p.grounded, upBeforeEffects, p.upsideDown,
                     p.gravityPortal, p.preFrameVelocity);
    }
    // Only for a flip the engine makes BEFORE update() (pushButton on an already-touched
    // ring). A flip from the collision pass (gravity portal, pad, a ring touched this
    // step) comes after the move: real Y keeps the old direction this step (truth
    // 108166595 f318: 45-deg gravity portal, wave continues down 1.95 then turns).
    if (p.upsideDown != upBeforeEffects && !p.dead && !p.grounded && p.flipBeforeUpdate
        && p.vehicle.type == VehicleType::Wave) {
        const double v0 = (p.input * 2 - 1) * player_speeds[(int)p.speed]
                          * (p.small ? 2.0 : 1.0);
        p.pos.y += (float)(2.0 * p.grav(v0) * p.dt);
    }

    // Same fix, generalized to Cube/Robot (2026-08-10): these modes carry a TRUE
    // accumulated `velocity` (not wave's per-frame-recomputed synthetic one), but
    // preCollision integrates Y with it BEFORE this frame's gravity-orb/portal
    // effect can flip upsideDown — same one-frame-stale-gravity bug as the wave
    // case above, just never generalized past wave. FOUND via the macro-demonlist
    // batch (level 82172844 "Cobwebs"): a cube clicks a gravity orb mid-fall, and
    // the very next frame reports a DEEP (15.7×13.8u) "block-side" overlap that a
    // real, human-verified-clearing run does not hit.
    //
    // TWICE-CORRECTED same day. v1 used `2*grav_new(velocity)` (wave's shortcut,
    // but velocity there is a fresh per-frame value; here it's the POST-effect
    // p.velocity) — wrong because a gravity-flip ORB (Blue/Green/1751, Orb.cpp)
    // and a gravity PAD both set p.velocity AND p.velocityOverride=true in the same
    // collide() call: that override means the boost is explicitly DEFERRED to next
    // frame (see orb_touch_timing_sameframe / Orb.cpp's own comment — "the orb
    // frame still moves with the OLD velocity"), so using the already-boosted
    // p.velocity here un-defers it a frame early (measured: INCREASED the Cobwebs
    // overlap, 13.8u -> 15.4u). v2 tried "undo old, redo new" using preFrameVelocity
    // for the old term but p.velocity (still post-effect) for the new term — same
    // bug, just reshaped; it improved penX but left penY worse (17.56u) since it's
    // still reading the boosted velocity.
    //
    // The correct form drops p.velocity entirely and mirrors the wave formula
    // exactly: 2*grav_new(preFrameVelocity). Proof it's right for BOTH cases below
    // (grav_old(v) == -grav_new(v) on any flip, so undo+redo of the SAME v collapses
    // to 2x — this is just that shortcut applied to the value preCollision actually
    // used, preFrameVelocity, instead of the post-effect velocity):
    //  - DEFERRED (orb/pad, velocityOverride=true): postCollision's OWN semi-implicit
    //    resync is gated off by `!velocityOverride`, so this is the ONLY correction
    //    that runs this frame. Net Y this frame = grav_old(preFrameVelocity)*dt
    //    [preCollision] + 2*grav_new(preFrameVelocity)*dt [this] =
    //    grav_new(preFrameVelocity)*dt — old (pre-boost) velocity, new gravity sign.
    //    Exactly the deferred model: gravity flips immediately, the velocity SET
    //    lands next frame.
    //  - NOT DEFERRED (GravityPortal: sets velocity=-v/2 with no override):
    //    postCollision's semi-implicit resync DOES still fire afterward — its delta
    //    is `grav_new(v_final)*dt - grav_new(preFrameVelocity)*dt` (v_final = velocity
    //    after the portal AND this frame's gravity accel). Added to this correction's
    //    net (grav_new(preFrameVelocity)*dt), the preFrameVelocity terms cancel
    //    exactly, leaving grav_new(v_final)*dt — the correct single semi-implicit
    //    step using the frame's true final velocity. Using post-effect p.velocity
    //    here (v1/v2) instead double-counted against that same postCollision resync.
    // Excludes Spider (its own orb grounds + repositions Y explicitly, so this
    // would double-correct) and Ball/Ufo/Ship/Swing (their flip handling is
    // already separate — Ball flips via its own clamp() on ceiling contact, not a
    // portal touch mid-frame the way this generic effects-order gap applies to).
    //
    // TERMINAL-VELOCITY EXCEPTION (2026-08-19, real DeCode.gdr2 capture, level
    // 2997354): the formula above is proven correct when preFrameVelocity is
    // NOT at the Cube/Robot fall cap (Vehicle.cpp's -810 clamp) — a Blue Orb
    // touch entering upside-down mid-fall at preFrameVelocity=-621 landed at
    // posY 164.113, matching real (162.631 after Y-offset, diff 1.48 — inside
    // this level's general ~1u noise floor). But the SAME formula at a second
    // Blue Orb touch 52 frames later, exiting back to normal orientation with
    // preFrameVelocity pinned exactly at -810 (the cap), gave posY 276.314 vs
    // real 283.902 (diff 7.59 — a real, precise divergence, not noise) — while
    // simply SKIPPING the correction that frame (leaving preCollision's own
    // uncorrected 283.064) landed within 0.838 of real, six times closer than
    // the "corrected" answer moved it. Frame-matched directly against the
    // re-scanner capture (testlevel/GDMod_physics_2997354.txt, same block for
    // both touches, X positions matched exactly to the sim's own trace) — this
    // isn't a guess. Root cause not fully understood (possibly: real GD's own
    // per-frame clamp ordering behaves differently once velocity has already
    // saturated), so gated off empirically rather than re-derived; revisit if
    // a real capture ever contradicts this gate.
    else if (p.upsideDown != upBeforeEffects && !p.dead && !p.grounded
             // TRIED 2026-08-21 (DeCode, x~2500): found a Ship-entry happening WHILE
             // upside-down (mid-flip, from an earlier gravity portal at f1909, flipping
             // BACK to normal at f1932 while already in Ship mode) gets ZERO resync here
             // — this whole mechanism was generalized from Wave to Cube/Robot only,
             // Ship (also a true-accumulated-velocity, gravity/thrust-driven vehicle,
             // same category the original bug report was about) was never covered.
             // Extending to Ship as the most direct, structurally-motivated fix for this
             // exact gap — validate against the full regression bank before trusting.
             && (p.vehicle.type == VehicleType::Cube || p.vehicle.type == VehicleType::Robot
                 || p.vehicle.type == VehicleType::Ship)
             // TRIED 2026-08-21 (DeCode): a gravity flip landing within several
             // frames of a fresh vehicle switch uses preFrameVelocity from the OLD
             // vehicle's own (already-transformed-by-the-switch) scale, not a normal
             // accumulated velocity this formula assumes — real capture shows a
             // SMOOTH position continuation through BOTH such clusters found this
             // session (x~12304 Ball->Cube+flip 1 frame later, delta would be
             // -5.529 vs real's smooth +continuation; x~2500 Cube->Ship+flip 4
             // frames later, real also shows a smooth +0.371 continuation, not a
             // jump). Widened from an initial 2-frame window (which only caught the
             // first case) to 5 to also cover the second.
             && (p.frame - p.lastVehicleSwitchFrame) > 5
             // FOUND 2026-08-21 (user-flagged, x~6135, same level): the terminal-
             // velocity exception above was validated on a Blue Orb touch (deferred
             // boost). A GravityPortal touch at the SAME cap (-810) behaves
             // differently — the standard formula's own delta (-6.75) lands almost
             // exactly on real's needed correction here (target -6.31), while
             // skipping it (as the orb case wants) leaves a real +6.3 jump. So the
             // cap exception only applies to orb-caused flips; p.gravityPortal
             // (set by GravityPortal::collide(), reset every frame in preCollision)
             // distinguishes the two without needing a second position-keyed hack.
             && !getenv("GDSIM_NOCUBEFLIPCORR")) {
        // TRIED 2026-08-20 (DeCode): a least-squares fit across 8 real gravity-flip
        // touches suggested scaling this "2.0" down to ~0.82 (pointwise errors at
        // those 8 touches looked smaller on average). REVERTED — it regressed the
        // level's actual reachable distance (sim died back at f2411/x3281, the exact
        // wall the gravityPortalFlipScale fix upstream had already pushed past to
        // f3595/x5193). The 8-touch fit was likely chasing noise rather than a real
        // shared constant — each touch's individual "needed" delta varied too much
        // (see the git history of this comment / conversation for the raw numbers)
        // to trust a single scale confidently. Formula kept at the original,
        // theory-derived 2.0 pending a better per-touch understanding.
        double delta = 2.0 * p.grav(p.preFrameVelocity) * p.dt;
        // ENGINE RULE (2026-10-05, GJBaseGameLayer::playerTouchedRing 0x140217e40): a ring
        // or pad touched in the collision pass flips gravity AFTER this step's update()
        // already moved the player with the old WORLD velocity — which is exactly what
        // preCollision did. So a DEFERRED flip (velocityOverride: newly touched orb/pad)
        // needs no Y correction at all. Only the same-step case (pushButton -> ringJump
        // BEFORE update, i.e. a fresh press on an already-touched orb, velocityOverride
        // false) needs this term, to undo postCollision's resync running with the new
        // sign. Truth 2997354 f616: blue orb grazed with the button held — real Y keeps
        // falling 1.664, the old correction mirrored it to +1.567. The per-velocity
        // patches that used to sit below (-422.712 / -313.200 -> 0) and the terminal-
        // velocity gate were all this one rule, measured touch by touch.
        if (p.velocityOverride) delta = 0.0;
        // TRIED 2026-08-21: an independent reference implementation
        // (seanlnge/gd-simulate) suggested a delta=0 model for EVERY GravityPortal
        // touch (position integration happens before portal application there, so
        // the flip has zero effect on the touch's own frame). REGRESSED hard
        // (reachable distance f5216->f3595) — the terminal-velocity-specific gate
        // below still matters in gdsim's actual pipeline even with the new
        // decompiled-exact scale/velocityOverride, so this project's own model
        // isn't a full match for that reference's simplified one. Reverted.
        // DeCode (level 2997354)-scoped override: this exact touch (preFrameVelocity
        // -422.712, the level's 3rd gravity-flip, a Blue Orb at x~1171) measurably
        // needs ~0 correction against real (formula gives +3.523, real needs -0.049) —
        // unlike every other touch in this corridor, which need the standard formula
        // to survive downstream (tried a general threshold/scale fix twice; both
        // pointwise-improved this touch but broke the ship section's reachable
        // distance, f3595->f2411 — the corridor's later touches are apparently
        // sensitive to any change in the shared formula). Keyed narrowly by this
        // touch's own near-unique preFrameVelocity so nothing else is affected.

        // Same fix, 4th gravity-flip touch (preFrameVelocity -313.200, Blue Orb at
        // x~2373, user-flagged): locally correct (formula +2.610, real needs ~0) but
        // regressed the reachable distance when applied ALONE — this touch feeds
        // directly into the corridor's already-tuned downstream batch (the
        // GravityPortal.cpp corrections below), whose constants were derived
        // assuming this touch's OLD (uncorrected) behavior. Re-enabled together with
        // a re-derivation of that whole downstream batch against the new state.

        // TRIED extending this same per-touch approach to the corridor's other 5
        // gravity-flip touches (f1476/1837/1857/1883/1909), each keyed by its own
        // preFrameVelocity with a "needed" delta measured the same way as the
        // -422.712 case above. REVERTED — those 5 measurements were computed from
        // data with the same frame-indexing bug this conversation already caught and
        // fixed once for touch1 (see git history), and applying them made those exact
        // touches measurably WORSE, not better. Only the -422.712 touch above was
        // independently re-verified with the corrected indexing before being kept.
        // SUPERSEDED 2026-08-22 (DeCode, gravity portal at x=1905, measured exactly
        // against the raw capture): this whole `2 * grav(preFrameVelocity) * dt`
        // correction is wrong for a GRAVITY-PORTAL flip. Real GD's order is
        // updateJump [v += a*dt; y += v*dt, BOTH in the PRE-flip orientation] ->
        // checkCollisions -> flipGravity. So the flip frame's Y motion is just the
        // ordinary semi-implicit step in the OLD orientation — there is no extra
        // flip term at all. Measured: real dY = +2.2473 = (527.688 + 11.664)/240,
        // while this correction drove gdsim to -2.199 (it subtracts 2x the pre-frame
        // velocity term, inverting the frame's motion) — a 4.45u error on a single
        // frame. The genuinely missing piece is only the a*dt^2 gravity step that
        // preCollision's explicit integration omits, which Player::postCollision now
        // applies for every override frame (see its own comment there), gravity
        // portals included. So: zero this out for portal-caused flips and let that
        // one shared, evidence-backed path handle it.
        // NOT touched for ORB/PAD-caused flips (p.gravityPortal false): those keep
        // their existing, separately-calibrated behaviour including the two
        // preFrameVelocity-keyed delta=0 overrides above.
        if (p.gravityPortal) delta = 0.0;
        if (getenv("GDSIM_GRAVFLIP_DEBUG"))
            std::fprintf(stderr, "GRAVFLIP-CORR f=%d preFrameVel=%.3f velocity=%.3f upBefore=%d upAfter=%d delta=%.3f posY_before=%.3f posY_after=%.3f\n",
                         p.frame, p.preFrameVelocity, p.velocity, upBeforeEffects, p.upsideDown, delta, p.pos.y, p.pos.y + (float)delta);
        p.pos.y += (float)delta;
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

    // Undo a stair-snap record made on the frame the cube LAUNCHED (jump/orb). The
    // engine runs updateJump before the collision pass, so a launching cube has
    // already left the floor and collidedWithObjectInternal never places it (never
    // calls checkSnapJumpToObject); gdsim collides first and jumps in postCollision,
    // so Block::collide still recorded this frame's floor. Keeping it paired the
    // wrong object with the next landing — truth 274283 f1907: the jump recorded
    // block (2475,15) where the engine still held (2445,15), and the landing at f1970
    // on (2565,75) matched the +90/+60 stair: gdsim snapped X +1.0, real did not,
    // and the cube then took an edge one frame early and died on the spike at 2835.
    // (Also truth 308 f1778->1779: the record stays the landing's.) Only a launch
    // from an established floor contact: a landing that launches on the same frame
    // (held into the landing) was a real placement and keeps its record.
    if (p.snapData.landingFrame == p.frame && !p.grounded && groundedBefore) {
        p.snapData.object      = snapBefore.object;
        p.snapData.objectId    = snapBefore.objectId;
        p.snapData.objectSolid = snapBefore.objectSolid;
        p.snapData.snapDX      = snapBefore.snapDX;
    }

    // Teleport triggers activated in this step (X-passed, spawned or touched): the engine
    // runs them from checkSpawnObjects / the collision pass, i.e. at the END of the step.
    if (!p.dead)
        for (int tk : teleportTriggers) {
            const Trigger& t = triggers[tk];
            if (!t.preApplied && t.fireFrame >= 0 && t.fireFrame == (int)p.frame - 1
                && (!t.touchTriggered || modelTouchTriggers))
                applyTeleport(p, t);
        }
    updateVisualRotation(p);

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
    // CRASH FIX (2026-08-18, real user crash: EXCEPTION_ACCESS_VIOLATION reading
    // 0x8 in Level::currentFrame(), called from Block::collide's Cube-landing
    // branch during centerClicks). Root cause: PathSeeker's backtracking
    // (Solver_Path.cpp, solveLevelPath) calls rollback(trueBest - numAway), where
    // trueBest is the farthest frame EVER reached across the whole randomized
    // search (any branch), not necessarily one still represented in `sim`'s
    // CURRENT lineage after backtracking away from it. When that target exceeds
    // gameStates.size(), resize() GROWS the vector, filling the new slots with
    // default-constructed Player()s — which explicitly sets level=nullptr (see
    // Player::Player()) — instead of real simulated states. The very next
    // runFrame() derives from that garbage tail (default Player also happens to
    // be vehicle=Cube, grounded=true, so it reliably lands in the Cube branch)
    // and crashes dereferencing the null `level`. rollback()'s only real contract
    // is "return to an earlier already-simulated frame" — growth is never a
    // valid outcome of "rolling BACK" for any caller — so clamp here once,
    // fixing every current and future call site instead of hardening each one.
    if (n > (int)gameStates.size()) n = (int)gameStates.size();
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

void Level::resetToStart() {
    // gameStates[0] is the spawn state built by the constructor and is never
    // mutated by stepping, so truncating back to it restores the exact starting
    // condition. rollback() already clamps and keeps gameStates2 in lockstep.
    rollback(1);
    // The dual latch is NOT part of gameStates: once a dual portal fires,
    // dualEverActive stays true and gameStates2 keeps being stepped. Leaving it
    // set would make a "fresh" run start already-dual on a level whose dual
    // section is far past the frame being studied.
    gameStates2.clear();
    dualEverActive = false;
    mirrorDead = false;
    // Trigger fire-frames are precomputed from a predicted X(frame) walk that
    // reads player_speeds[] — a Speed-group tunable edit invalidates them, and
    // that is exactly the kind of edit the fitter probes. Cheap next to a parse.
    if (hasTriggers) buildTriggerTimeline();
}

// FUN_140071ef0: slerp between two angles (radians) through unit quaternions of the half
// angles, every operation in float like the binary (cosf/sinf/acosf, atan2 in double).
static float slerpAngle(float a, float b, float t) {
    const float ha = a * 0.5f, hb = b * 0.5f;
    const float c1 = std::cos(ha), s1 = std::sin(ha);
    float c2 = std::cos(hb), s2 = std::sin(hb);
    float dot = c2 * c1 + s2 * s1;
    if (dot < 0.f) { dot = -dot; s2 = -s2; c2 = -c2; }
    float w1 = 1.f - t, w2 = t;
    if (1.f - dot > 0.0001f) {
        const float om = std::acos(dot), so = std::sin(om);
        w1 = std::sin((1.f - t) * om) / so;
        w2 = std::sin(om * t) / so;
    }
    const float S = s1 * w1 + s2 * w2;
    const float C = c1 * w1 + c2 * w2;
    return (float)(std::atan2((double)S, (double)C) * 2.0);
}

// PlayerObject::updateShipRotation (0x140390c40), classic mode (no reverse, not sideways):
// target = atan2f(-dy, dx) of this step's motion, slerped from the current rotation by
// t = min(delta, factor*delta), delta = 0.25 (the step's 60 Hz-unit dt). Factor: 0.15
// ship/swing, 0.25 wave (0.4 mini), 0.07 UFO (whose target is first scaled by -0.4 and
// clamped to +-0.1). x0.25 while colliding with a slope. Skipped on a slope / dashing, and
// when the step moved less than sqrt(1.2*delta).
void Level::updateVisualRotation(Player& p) {
    const VehicleType v = p.vehicle.type;
    const bool flying = v == VehicleType::Ship || v == VehicleType::Ufo
                     || v == VehicleType::Wave || v == VehicleType::Swing;
    if (flying && !p.dashing && !p.slopeData.slope) {
        const float delta = 0.25f;
        const float dx = p.pos.x - p.rotLastPos.x;
        const float dy = -(p.pos.y - p.rotLastPos.y);
        if (dx * dx + dy * dy >= delta * 1.2f) {
            float target = std::atan2(dy, dx);
            float factor = 0.15f;
            if (v == VehicleType::Ufo) {
                target *= -0.4f;
                factor = 0.07f;
                target = (p.upsideDown == false) ? std::max(-0.1f, target) : std::min(0.1f, target);
            } else if (v == VehicleType::Wave) {
                factor = p.small ? 0.4f : 0.25f;
            }
            const float t = std::min(delta, factor * delta);
            const float cur = p.visRot * 0.017453292f;
            p.visRot = slerpAngle(cur, target, t) * 57.29578f;
        }
    }
    p.rotLastPos = p.pos;
}

// GJBaseGameLayer::tryGetObject (0x140224590): the group's parent object (field 274), else
// the group's only object, else a RANDOM one (rand-driven in the engine; the first in level
// order stands in for it here).
bool Level::groupObjectPos(int group, Vec2D& out) const {
    if (auto it = groupParentPos.find(group); it != groupParentPos.end()) { out = it->second; return true; }
    if (auto it = groupAnchor.find(group); it != groupAnchor.end()) { out = it->second; return true; }
    return false;
}

// GJBaseGameLayer::teleportPlayer (0x14020fdb0), positional part. Target = an object of
// the trigger's group (the engine picks one at random when the group holds several; the
// first in level order is used here). Save Offset (351) keeps the player's offset from the
// trigger; Ignore X / Ignore Y (352/353) keep that axis.
bool Level::teleportTarget(const Trigger& t, Vec2D pos, int f, Vec2D& out) const {
    Vec2D target;
    auto mv = groupToMovable.find(t.targetGroup);
    if (mv != groupToMovable.end() && !mv->second.empty()) {
        const int idx = mv->second.front();
        target = movableMeta[idx].startPos;
        if ((size_t)idx < movableToTriggers.size()) {
            float rot; bool active;
            poseMovable(idx, f, target, rot, active);
        }
    } else if (!groupObjectPos(t.targetGroup, target)) {
        return false;
    }
    if (t.tpSaveOffset) target = target + (pos - Vec2D{t.x, t.y});
    if (t.tpIgnoreX) target.x = pos.x;
    if (t.tpIgnoreY) target.y = pos.y;
    out = target;
    return true;
}

void Level::applyTeleport(Player& p, const Trigger& t) const {
    Vec2D to;
    if (teleportTarget(t, p.pos, (int)p.frame, to)) {
        p.pos = to;
        p.grounded = false;                       // m_isOnGround2 = 0
    }
    // Gravity option 354: flipGravity(player, flip, true) — halves the WORLD velocity;
    // gdsim's velocity is gravity-relative, so the sign turns as well.
    bool flip = p.upsideDown;
    if (t.tpGravity == 1) flip = false;
    else if (t.tpGravity == 2) flip = true;
    else if (t.tpGravity == 3) flip = !p.upsideDown;
    if (flip != p.upsideDown) {
        p.upsideDown = flip;
        p.velocity   = -p.velocity * 0.5;
        p.grounded   = false;
    }
}

int Level::currentFrame() const { return (int)gameStates.size(); }

Player const& Level::getState(int frame) const {
    if (frame == 0) return gameStates[0];
    if ((int)gameStates.size() < frame) return gameStates.back();
    return gameStates[frame - 1];
}

Player& Level::latestState() { return gameStates.back(); }

} // namespace gdsim
