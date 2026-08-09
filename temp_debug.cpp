nclude "src/sim/Level.hpp"
#include "src/sim/Solver.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

int main() {
    std::string path = "C:/Users/Kusmic/Documents/last_level.txt";
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::cerr << "cannot open " << path << "\n"; return 1; }
    std::stringstream ss; ss << in.rdbuf();
    std::string lvl = ss.str();
    gdsim::Level sim(lvl);
    auto& p = sim.gameStates[0];
    std::cout << "spawn frame="<<p.frame<<" x="<<p.pos.x<<" y="<<p.pos.y<<" button="<<p.button<<" input="<<p.input<<" buffer="<<p.buffer<<" grounded="<<p.grounded<<" dead="<<p.dead<<" vel="<<p.velocity<<"\n";
    std::cout << "sections=" << sim.sections.size() << "\n";
    std::cout << "First sections object positions:\n";
    int count = 0;
    for (int si = 0; si < (int)sim.sections.size() && si < 5; ++si) {
        std::cout << " section " << si << " size=" << sim.sections[si].size() << "\n";
        for (int oi = 0; oi < (int)sim.sections[si].size() && oi < 10; ++oi) {
            auto& oc = sim.sections[si][oi];
            std::cout << "   obj " << oi << " id=" << oc->typeId << " x=" << oc->pos.x << " y=" << oc->pos.y << " prio=" << oc->prio << " trig=" << oc->triggerCat << "\n";
            ++count;
        }
    }
    std::cout << "All static objects within x<=200:\n";
    count = 0;
    for (auto& sec : sim.sections) {
        for (auto& oc : sec) {
            if (oc->pos.x > 200) continue;
            std::cout << "  obj id="<<oc->typeId<<" x="<<oc->pos.x<<" y="<<oc->pos.y<<" prio="<<oc->prio<<" trig="<²<oc->triggerCat<<"\n";
            if (++count >= 80) break;
        }
        if (count >= 80) break;
    }

    std::cout << "Movable objects count=" << sim.movable.size() << "\n";
    for (int i = 0; i < (int)sim.movable.size() && i < 20; ++i) {
        gdsim::Vec2D pos; float rot; bool active;
        sim.poseMovable(i, 2, pos, rot, active);
        std::cout << " movable "<<i<<" startX="<<sim.movableMeta[i].startPos.x<<" pos="<<pos.x<<","<<pos.y<<" active="<<active<<" type="<<sim.movable[i]->typeId<<" prio="<<sim.movable[i]->prio<<"\n";
    }
    try {
        sim.rollback(0);
        sim.gameStates[0] = p;
        bool pressed = true;
        std::cout << "STEP TRACE:\n";
        for (int i = 0; i < 20; ++i) {
            auto& s = sim.runFrame(i == 0 ? pressed : false, 1.f/240.f);
            std::cout << "f="<<s.frame<<" x="<<s.pos.x<<" y="<<s.pos.y<<" grounded="<<s.grounded
                      <<" dead="<<s.dead<<" vel="<<s.velocity<<" button="<<s.button
                      <<" input="<<s.input<<" buffer="<<s.buffer<<" usedEffects="<<s.usedEffects.size()<<"\n";
            if (s.dead) break;
        }
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
    }
    return 0;
}
