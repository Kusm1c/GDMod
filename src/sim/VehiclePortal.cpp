#include "Portals.hpp"
#include "Player.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace gdsim {

VehiclePortal::VehiclePortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields))
{
    triggerCat = PCAT_VEHICLE;
    switch (numFromString<int>(fields[1])) {
        case 12:   type = VehicleType::Cube;   break;
        case 13:   type = VehicleType::Ship;   break;
        case 47:   type = VehicleType::Ball;   break;
        case 111:  type = VehicleType::Ufo;    break;
        case 660:  type = VehicleType::Wave;   break;
        case 745:  type = VehicleType::Robot;  break;
        case 1331: type = VehicleType::Spider; break;   // real spider portal (747 is teleport)
        case 1933: type = VehicleType::Swing;  break;   // real swing portal
        case 2751: type = VehicleType::Swing;  break;
        default:   type = VehicleType::Cube;   break;
    }
}

bool VehiclePortal::touching(Player const& player) const {
    if (type != VehicleType::Wave) return EffectObject::touching(player);
    float extra = g_calib.waveEntryReachAdjust;
    // INVESTIGATING 2026-08-21 (DeCode): the shared waveEntryReachAdjust fixes
    // this level's FIRST wave-entry (x~6315) but leaves a real, confirmed-flat
    // residual (~-7.99u, re-derived via the same "sweep the frame offset until
    // the bimodal alignment artifact collapses to flat" method that found the
    // first fix) at the SECOND one (x~19545) — testing a position-keyed extra
    // delay for just this instance rather than retuning the shared constant
    // (which already regresses if pushed further, per memory notes).
    if (getenv("GDSIM_WAVE2_EXTRA") && std::fabs(pos.x - 19545.f) < 5.f)
        extra += (float)atof(getenv("GDSIM_WAVE2_EXTRA"));
    if (extra == 0.f) return EffectObject::touching(player);
    if (player.usedEffects.contains(id)) return false;
    Entity self = *this;
    float reach = g_calib.portalReach[triggerCat] + extra;
    self.size.x += 2.f * reach;
    self.size.y += 2.f * reach;
    return self.intersects(player.unrotatedHitbox());
}

void VehiclePortal::collide(Player& player) const {
    if (getenv("GDSIM_PORTAL_DEBUG"))
        std::fprintf(stderr, "VEHPORTAL collide f=%llu playerXY=(%.2f,%.2f) portalXY=(%.2f,%.2f) fromVeh=%d toVeh=%d\n",
                     (unsigned long long)player.frame, player.pos.x, player.pos.y, pos.x, pos.y,
                     (int)player.vehicle.type, (int)type);
    EffectObject::collide(player);
    if (player.vehicle.type != type) {
        player.lastVehicleSwitchFrame = player.frame;
        player.size = Vec2D(30, 30) * (player.small ? 0.6f : 1.0f);
        if (player.vehicle.type == VehicleType::Wave)
            player.velocity *= 0.9;
        // FOUND 2026-08-21 (DeCode, real capture at the Cube->Wave vehicle-portal
        // touch, x~6291): real shows ONE transitional frame at HALF the carried-over
        // velocity (-7.5, exactly half of the cube terminal-fall cap -15) before
        // settling to wave's own steady Y-velocity next frame. -810(gdsim cube cap)
        // *0.5/54 = -7.5, an exact match. Mirrors the same flat *0.5 pattern already
        // decompiled-confirmed for gravity flips (see GravityPortal.cpp) — carried-
        // over velocity is halved on ANY major state transition, not just gravity.
        // velocityOverride makes this stick for exactly this one frame; Wave's own
        // v.clamp (Vehicle.cpp) now respects it instead of unconditionally
        // overwriting to the full formula on the same frame as entry.
        const bool enteringWave = (type == VehicleType::Wave && player.vehicle.type != VehicleType::Wave);
        player.vehicle = Vehicle::from(type);
        player.vehicle.enter(player);
        if (enteringWave) {
            player.velocity *= 0.5;
            player.velocityOverride = true;
        }
    }
    player.floor   = std::max(0., 30. * std::ceil((pos.y - (player.vehicle.bounds / 2. + 30)) / 30.));
    player.ceiling = player.floor + player.vehicle.bounds;
}

} // namespace gdsim
