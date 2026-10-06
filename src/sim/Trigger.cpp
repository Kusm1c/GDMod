#include "Trigger.hpp"
#include <cstdlib>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace gdsim {

// GD easing (field 30) with rate (field 85). Maps progress t∈[0,1] → eased [0,1].
//
// TRANSCRIBED 2026-09-12 line-for-line from GameToolbox::getEasedValue
// (0x140068b70) and GameToolbox::bounceTime (0x140068ac0). The previous version
// was a reimplementation from the standard cocos2d/Robert Penner easing set,
// which GD does NOT follow in four places — see the notes on cases 3, 4-6, 10
// and 11. Case 3 alone was up to 7% of a move trigger's whole offset.
static float bounceTime(float t) {
    if (t < 0.36363637f) return t * 7.5625f * t;
    if (t < 0.72727275f) { float u = t - 0.54545456f; return u * 7.5625f * u + 0.75f; }
    if (t < 0.90909094f) { float u = t - 0.8181818f;  return u * 7.5625f * u + 0.9375f; }
    { float u = t - 0.95454544f; return u * 7.5625f * u + 0.984375f; }
}

float easeValue(float t, int type, float rate) {
    t = std::clamp(t, 0.f, 1.f);
    if (type == 0) return t;
    // GD's own guard, and it runs BEFORE the switch, so `rate` is never <= 0
    // inside any case below (which is why case 4's own `rate == 0` fallback to
    // 0.45000002 is dead code in the binary — do not resurrect it as a default).
    if (rate <= 0.f) rate = 2.f;
    const float PI = 3.1415927f;
    switch (type) {
        case 1: {                                                    // EaseInOut
            float t2 = t + t;
            if (t2 >= 1.f) return 1.f - std::pow(2.f - t2, rate) * 0.5f;
            return std::pow(t2, rate) * 0.5f;
        }
        case 2: return std::pow(t, rate);                            // EaseIn
        // EaseOut is `t^(1/rate)`, NOT the usual `1-(1-t)^rate`. GD mirrors EaseIn
        // through the EXPONENT, not through the curve. At rate 2 the two differ by
        // up to 0.068 of full progress (t=0.75: 0.866 vs 0.938), i.e. ~7% of the
        // trigger's total offset mid-animation — 20 units on a 300-unit move.
        case 3: return std::pow(t, 1.f / rate);                      // EaseOut
        // Elastic: the period is the trigger's OWN easeRate, not a hardcoded
        // 0.45/0.3. With the default rate of 2 that is a completely different
        // oscillation from the textbook constants this used to use.
        case 4: {                                                    // ElasticInOut
            if (t == 0.f || t == 1.f) return t;
            float q = t + t - 1.f;
            float s = std::sin(((q - rate * 0.25f) * PI) * 2.f / rate);
            if (q < 0.f) return std::pow(2.f, q * 10.f) * -0.5f * s;
            return std::pow(2.f, q * -10.f) * s * 0.5f + 1.f;
        }
        case 5: {                                                    // ElasticIn
            if (t == 0.f || t == 1.f) return t;
            float s = std::sin((((t - 1.f) - rate * 0.25f) * PI) * 2.f / rate);
            return -std::pow(2.f, (t - 1.f) * 10.f) * s;
        }
        case 6: {                                                    // ElasticOut
            if (t == 0.f || t == 1.f) return t;
            float s = std::sin(((t - rate * 0.25f) * PI) * 2.f / rate);
            return std::pow(2.f, t * -10.f) * s + 1.f;
        }
        case 7:                                                      // BounceInOut
            if (t >= 0.5f) return bounceTime(t + t - 1.f) * 0.5f + 0.5f;
            return (1.f - bounceTime(1.f - (t + t))) * 0.5f;
        case 8: return 1.f - bounceTime(1.f - t);                    // BounceIn
        case 9: return bounceTime(t);                                // BounceOut
        // No t==0/t==1 special case in GD: at t=0 this really does return
        // 0.000488, not 0. The guard that used to be here clamped it to 0.
        case 10: {                                                   // ExponentialInOut
            float q = t + t - 1.f;
            if (t + t >= 1.f) return (2.f - std::pow(2.f, q * -10.f)) * 0.5f;
            return std::pow(2.f, q * 10.f) * 0.5f;
        }
        // The -0.001 is in the binary. It makes the curve start at exactly 0
        // rather than 2^-10, and shifts the whole curve down by that much.
        case 11: return t == 0.f ? 0.f : std::pow(2.f, (t - 1.f) * 10.f) - 0.001f;
        case 12: return t == 1.f ? 1.f : 1.f - std::pow(2.f, t * -10.f);
        case 13: return (std::cos(t * PI) - 1.f) * -0.5f;            // SineInOut
        case 14: return 1.f - std::cos(t * 1.5707964f);              // SineIn
        case 15: return std::sin(t * 1.5707964f);                    // SineOut
        case 16: {                                                   // BackInOut
            float t2 = t + t;
            if (t2 >= 1.f) { t2 -= 2.f; return (t2 * 3.5949094f + 2.5949094f) * t2 * t2 * 0.5f + 1.f; }
            return (t2 * 3.5949094f - 2.5949094f) * t2 * t2 * 0.5f;
        }
        case 17: return t * t * (t * 2.70158f - 1.70158f);           // BackIn
        case 18: { float u = t - 1.f; return u * u * (u * 2.70158f + 1.70158f) + 1.f; }
        default: return t;
    }
}


// GD trigger object IDs we simulate (physics-affecting only).
//   901  = Move        1346 = Rotate      1347 = Follow
//   1049 = Toggle      1007 = Alpha       1268 = Spawn
//   3022 = Teleport
bool isTriggerId(int id) {
    return id == 901 || id == 1346 || id == 1347 ||
           id == 1049 || id == 1007 || id == 1268 || id == 3022;
}

static float ff(const std::unordered_map<int, std::string>& m, int k, float def = 0.f) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return def;
    return stod_def(it->second, def);
}
static int fi(const std::unordered_map<int, std::string>& m, int k, int def = 0) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return def;
    return numFromString<int>(it->second, def);
}

std::vector<int> parseGroups(const std::string& field57) {
    std::vector<int> out;
    if (field57.empty()) return out;
    std::stringstream ss(field57);
    std::string g;
    while (std::getline(ss, g, '.'))
        if (!g.empty()) out.push_back(numFromString<int>(g));
    return out;
}

std::optional<Trigger> parseTrigger(int id, const std::unordered_map<int, std::string>& f) {
    Trigger t;
    t.x              = ff(f, 2);
    t.y              = ff(f, 3);
    t.targetGroup    = fi(f, 51);
    t.duration       = ff(f, 10);
    t.easing         = fi(f, 30);
    t.easeRate       = ff(f, 85, 2.f);
    t.touchTriggered = fi(f, 11) != 0;
    t.spawnTriggered = fi(f, 62) != 0;
    { auto it = f.find(57); if (it != f.end()) t.ownGroups = parseGroups(it->second); }

    switch (id) {
        case 901: // Move
            t.kind  = TriggerKind::Move;
            t.moveX = ff(f, 28);
            t.moveY = ff(f, 29);
            t.lockToPlayerX = fi(f, 58)  != 0;
            t.lockToPlayerY = fi(f, 59)  != 0;
            t.lockToCameraX = fi(f, 141) != 0;
            t.lockToCameraY = fi(f, 142) != 0;
            t.moveModX      = ff(f, 143, 1.f);
            t.moveModY      = ff(f, 144, 1.f);
            t.silent        = fi(f, 544) != 0;
            // GD's own guard: createMoveCommand stores 1.0 when the field is 0.
            if (t.moveModX == 0.f) t.moveModX = 1.f;
            if (t.moveModY == 0.f) t.moveModY = 1.f;
            break;
        case 1346: // Rotate
            t.kind        = TriggerKind::Rotate;
            t.degrees     = ff(f, 68);
            t.centerGroup = fi(f, 71);
            break;
        case 1347: // Follow (target group copies followGroup's motion, scaled)
            t.kind        = TriggerKind::Follow;
            t.followGroup = fi(f, 71);
            t.followXMod  = ff(f, 72, 1.f);
            t.followYMod  = ff(f, 73, 1.f);
            break;
        case 1049: // Toggle (show/enable a group)
            t.kind     = TriggerKind::Toggle;
            t.toggleOn = fi(f, 56) != 0;
            break;
        case 1007: // Alpha (fade a group; collision usually tied to alpha>0)
            t.kind  = TriggerKind::Alpha;
            t.alpha = ff(f, 35, 1.f);
            break;
        case 1268: // Spawn (fires targetGroup's triggers after spawnDelay)
            t.kind       = TriggerKind::Spawn;
            t.spawnDelay = ff(f, 63);
            break;
        case 3022: // Teleport (player -> an object of targetGroup)
            t.kind         = TriggerKind::Teleport;
            t.tpSaveOffset = fi(f, 351) != 0;
            t.tpIgnoreX    = fi(f, 352) != 0;
            t.tpIgnoreY    = fi(f, 353) != 0;
            t.tpGravity    = fi(f, 354);
            if (t.targetGroup == 0 && t.tpGravity == 0) return std::nullopt;
            return t;
        default:
            return std::nullopt;
    }
    if (t.targetGroup == 0) return std::nullopt; // nothing to affect/spawn
    return t;
}

} // namespace gdsim
