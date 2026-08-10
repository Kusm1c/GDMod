#include "Portals.hpp"
#include "Player.hpp"
#include "Calib.hpp"
#include <cstdio>
#include <cstdlib>

namespace gdsim {

TeleportPortal::TeleportPortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields))
{
    // FOUND 2026-08-10: TeleportPortal never set triggerCat at all (every other
    // portal type does — VehiclePortal/SizePortal/GravityPortal/SpeedPortal/
    // DualPortal), so it was permanently excluded from the reach-calibration
    // system every other portal type gets. See Calib.hpp's PCAT_TELEPORT comment
    // for the real, concrete case (level 82172844) this was found from.
    triggerCat = PCAT_TELEPORT;
    // Field 54 = vertical teleport distance. (fields survives the base ctor move,
    // same pattern as VehiclePortal reading fields[1] below its EffectObject init.)
    auto it = fields.find(54);
    distance = (it != fields.end() && !it->second.empty()) ? stod_def(it->second) : 0.f;
}

bool TeleportPortal::touching(Player const& p) const {
    // Hitbox overlap (and not-yet-used) AND the player's LEADING edge has reached
    // the portal's centre X. Measured from recorded real teleports: the player's
    // centre fires the portal at portalX − halfWidth (≈ −14.5 for a cube), i.e. its
    // front edge touches the portal centre. Firing on first hitbox touch (centre
    // ~15u earlier) or on centre-cross (~15u later) both desync the landing.
    bool base = EffectObject::touching(p);
    bool xok = p.getRight() >= pos.x;
    if (getenv("GDSIM_TP_DEBUG2") && pos.x < 0 && p.frame < 10)
        std::fprintf(stderr, "TP2 f=%llu id=%d playerXY=(%.3f,%.3f) playerR=%.3f portalXY=(%.3f,%.3f) portalSize=(%.2f,%.2f) base=%d xok=%d used=%d\n",
                     (unsigned long long)p.frame, id, p.pos.x, p.pos.y, p.getRight(), pos.x, pos.y, size.x, size.y,
                     base, xok, p.usedEffects.contains(id));
    return base && xok;
}

void TeleportPortal::collide(Player& p) const {
    EffectObject::collide(p);                 // one-shot via usedEffects
    // Real game: player exits at portalY + distance (e.g. portal y=195, dist=100 →
    // y=295). Velocity / vehicle / gravity are unchanged; only Y jumps.
    //
    // OPEN — biggest remaining regression-bank divergence (2026-08-06): truth
    // 85701165 has a single id=747 portal at (1311,-17) with distance(f54)=852.
    // This formula gives target Y = 835, but the real capture lands the player at
    // Y=925 (a 90u gap) — regress.sh's worst entry, sev=812.44 @f827. The simpler
    // case this formula was written against (195+100=295) may not have been
    // verified against a real capture the way the slope work was (no matching
    // truth-bank evidence found for it). Root-causing the real formula needs
    // either ground truth this repo doesn't have (GD Creators School's teleport
    // page 403s even with a browser UA; camila314/gdp's decompile has no
    // teleport-portal coverage), or a live read of the real PlayerObject/portal
    // target-Y computation during a real playthrough of 85701165 (the mod already
    // has the Geode bindings/addresses to do this — see the bindings inventory).
    // Don't guess a replacement formula from this single data point alone.
    p.pos.y = pos.y + distance;
}

} // namespace gdsim
