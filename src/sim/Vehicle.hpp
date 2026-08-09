#pragma once
#include <functional>

namespace gdsim {

enum class VehicleType {
    Cube,
    Ship,
    Ball,
    Ufo,
    Wave,
    Robot,   // variable-height jump (hold to charge)
    Spider,  // instant teleport to opposite surface
    Swing,   // ship-like with gravity flip
};

// Orb/pad velocity tables only cover the four classic vehicles, so the gravity
// vehicles collapse to a covered one. Mappings calibrated against real-engine capture
// (test/compare_mechanics.cpp):
//   Spider -> BALL  (exact: spider yellow pad 9.6 = Ball, pink 6.72, red 12, yellow orb 7.861).
//   Swing  -> BALL  (interim, net-best: ship was +67% on yellow; ball makes yellow/red
//                    pads exact, most orbs within ~12%; pink orb still +25%. Swing really
//                    has its own table — needs full-speed+mini capture to fill it).
//   Robot  -> CUBE  (pads exact; yellow/red orbs ~6-11% high — robot orbs slightly weaker).
// Collapsing also guarantees the lookup hits a valid key (a missing key threw
// std::out_of_range and closed the game).
inline VehicleType orbPadVehicle(VehicleType v) {
    switch (v) {
        case VehicleType::Robot:  return VehicleType::Cube;
        case VehicleType::Spider: return VehicleType::Ball;
        case VehicleType::Swing:  return VehicleType::Ball;
        default:                  return v;
    }
}

struct Player;
struct Object;

struct Vehicle {
    VehicleType type;
    std::function<void(Player&)> enter;
    std::function<void(Player&)> clamp;
    std::function<void(Player&)> update;
    float bounds;
    static Vehicle from(VehicleType v);
};

// Nearest opposite surface for a spider teleport (used by the Spider vehicle and
// the Spider orb). Defined in Vehicle.cpp.
float findSpiderTarget(const Player& p);

} // namespace gdsim
