#include "Calib.hpp"
#include <fstream>
#include <sstream>
#include <string>

namespace gdsim {

// Baked-in defaults = current behaviour. The calibrator overwrites this at
// runtime; once values converge they are pasted back into Calib.hpp's defaults.
CalibParams g_calib{};

bool loadCalibFromFile(const char* path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string key; double v;
    while (in >> key >> v) {
        if      (key == "reach_speed")   g_calib.portalReach[PCAT_SPEED]   = (float)v;
        else if (key == "reach_size")    g_calib.portalReach[PCAT_SIZE]    = (float)v;
        else if (key == "reach_vehicle") g_calib.portalReach[PCAT_VEHICLE] = (float)v;
        else if (key == "reach_gravity") g_calib.portalReach[PCAT_GRAVITY] = (float)v;
        else if (key == "reach_dual")    g_calib.portalReach[PCAT_DUAL]    = (float)v;
        else if (key == "speedXLag")     g_calib.speedXLagFrames           = (int)v;
        else if (key == "hazardInflate") g_calib.hazardHitboxInflate       = (float)v;
        else if (key == "moveScale")     g_calib.moveScale                 = (float)v;
    }
    return true;
}

void saveCalibToFile(const char* path) {
    std::ofstream o(path, std::ios::trunc);
    if (!o) return;
    o << "reach_speed "   << g_calib.portalReach[PCAT_SPEED]   << "\n";
    o << "reach_size "    << g_calib.portalReach[PCAT_SIZE]    << "\n";
    o << "reach_vehicle " << g_calib.portalReach[PCAT_VEHICLE] << "\n";
    o << "reach_gravity " << g_calib.portalReach[PCAT_GRAVITY] << "\n";
    o << "reach_dual "    << g_calib.portalReach[PCAT_DUAL]    << "\n";
    o << "speedXLag "     << g_calib.speedXLagFrames           << "\n";
    o << "hazardInflate " << g_calib.hazardHitboxInflate       << "\n";
    o << "moveScale "     << g_calib.moveScale                 << "\n";
}

} // namespace gdsim
