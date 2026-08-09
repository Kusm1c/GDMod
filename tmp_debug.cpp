#include "src/sim/Level.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
int main() {
    std::ifstream in("testlevel/last_level.txt", std::ios::binary);
    if (!in) { std::cerr << "open fail\n"; return 1; }
    std::stringstream ss; ss << in.rdbuf();
    std::string lvl = ss.str();
    auto normalized = gdsim::normalizeLevelString(lvl);
    std::cout << "orig size=" << lvl.size() << " norm size=" << normalized.size() << "\n";
    std::cout << "first 64 chars: " << normalized.substr(0, 64) << "\n";
    gdsim::Level sim(normalized);
    std::cout << "sections=" << sim.sections.size() << " movable=" << sim.movable.size() << " triggers=" << sim.triggers.size() << " objectCount=" << sim.objectCount << " length=" << sim.length << "\n";
    std::cout << "spawn frame=" << sim.gameStates[0].frame << " x=" << sim.gameStates[0].pos.x << " y=" << sim.gameStates[0].pos.y << " ground=" << sim.gameStates[0].grounded << " dead=" << sim.gameStates[0].dead << " vehicle=" << (int)sim.gameStates[0].vehicle.type << " speed=" << sim.gameStates[0].speed << "\n";
    sim.rollback(0);
    sim.gameStates[0] = sim.gameStates[0];
    bool foundDecision = false;
    for (int i = 1; i <= 2000; ++i) {
        auto &s = sim.runFrame(false, 1.f/240.f);
        std::cout << "step=" << i << " x=" << s.pos.x << " y=" << s.pos.y << " ground=" << s.grounded << " dead=" << s.dead << " vehicle=" << (int)s.vehicle.type << " speed=" << s.speed << "\n";
        if (s.dead) break;
        if (s.pos.x >= sim.length) { std::cout << "reached end\n"; break; }
        if (s.grounded || s.vehicle.type != gdsim::VehicleType::Cube) {
            std::cout << "decision at step=" << i << "\n";
            foundDecision = true;
            break;
        }
    }
    if (!foundDecision) std::cout << "no decision in 2000 steps\n";
    return 0;
}
