#include "Portals.hpp"
#include "Player.hpp"

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

void VehiclePortal::collide(Player& player) const {
    EffectObject::collide(player);
    if (player.vehicle.type != type) {
        player.size = Vec2D(30, 30) * (player.small ? 0.6f : 1.0f);
        if (player.vehicle.type == VehicleType::Wave)
            player.velocity *= 0.9;
        player.vehicle = Vehicle::from(type);
        player.vehicle.enter(player);
    }
    player.floor   = std::max(0., 30. * std::ceil((pos.y - (player.vehicle.bounds / 2. + 30)) / 30.));
    player.ceiling = player.floor + player.vehicle.bounds;
}

} // namespace gdsim
