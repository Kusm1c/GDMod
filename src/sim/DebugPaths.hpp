// DebugPaths.hpp — the ONE place that decides WHERE GDMod writes its debug output.
//
// Everything the mod emits for offline inspection used to litter the user's
// Documents folder. It now lives under the repo's testlevel/ in organized
// subfolders, so a debugging session has one tidy tree instead of a pile of
// GDMod_*.txt in Documents:
//
//     testlevel/debug/     solver + replay diagnostics
//                          (solver_debug, solver_profile, stuck, cube_first/add,
//                           inject_debug, death_debug, divergence, repair_log,
//                           last_level)
//     testlevel/captures/  runtime captures fed back into the sim
//                          (GDMod_truth_<id>, GDMod_hitbox_capture)
//     testlevel/config/    tuning inputs the mod reads back at solve/replay time
//                          (GDMod_calib, GDMod_waypoints_<id>)
//
// The re-scanner's structured captures (GDMod_physics_<id>, GDMod_levelparams_<id>
// at the testlevel/ root; GDMod_mechanics + GDMod_movetriggers + GDMod_hitbox_<id>
// under testlevel/movetest/) are unchanged — they were already inside testlevel/.
//
// If the repo path isn't writable (running on a machine without the checkout),
// every path transparently falls back to Documents so nothing is lost.
//
// Standalone: this header pulls in only std, so BOTH the Geode hooks and the
// no-Geode sim (Solver*, offline test harnesses) can include it.
#pragma once
#include <filesystem>
#include <fstream>
#include <string>

namespace gdsim {

// Resolve the writable base directory once (repo testlevel/, else Documents/).
inline const std::string& gdmodBaseDir() {
    static const std::string base = [] {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* repo = "C:/Users/Kusmic/Documents/GitHub/GDMod/testlevel/";
        fs::create_directories(repo, ec);
        const std::string probe = std::string(repo) + ".wtest";
        {
            std::ofstream t(probe);
            if (t.is_open()) { t.close(); fs::remove(probe, ec); return std::string(repo); }
        }
        const char* fallback = "C:/Users/Kusmic/Documents/";
        fs::create_directories(fallback, ec);
        return std::string(fallback);
    }();
    return base;
}

// A subfolder under the base (created on demand), returned WITH a trailing slash.
inline std::string gdmodSubDir(const char* sub) {
    std::string d = gdmodBaseDir() + sub + "/";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

inline std::string debugPath(const std::string& name)   { return gdmodSubDir("debug")    + name; }
inline std::string capturePath(const std::string& name) { return gdmodSubDir("captures") + name; }
inline std::string configPath(const std::string& name)  { return gdmodSubDir("config")   + name; }

} // namespace gdsim
