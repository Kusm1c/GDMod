#include "Trigger.hpp"
#include <cstdlib>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace gdsim {

// GD easing (field 30) with rate (field 85). Maps progress t∈[0,1] → eased [0,1].
// Types: 0 none, 1 InOut, 2 In, 3 Out (rate-based); 4-6 Elastic; 7-9 Bounce;
// 10-12 Exponential; 13-15 Sine; 16-18 Back (In/Out/InOut groupings per GD).
static float bounceOut(float t) {
    if (t < 1.f/2.75f)      return 7.5625f*t*t;
    else if (t < 2.f/2.75f) { t -= 1.5f/2.75f;  return 7.5625f*t*t + 0.75f; }
    else if (t < 2.5f/2.75f){ t -= 2.25f/2.75f; return 7.5625f*t*t + 0.9375f; }
    else                    { t -= 2.625f/2.75f;return 7.5625f*t*t + 0.984375f; }
}
float easeValue(float t, int type, float rate) {
    t = std::clamp(t, 0.f, 1.f);
    if (rate <= 0.f) rate = 2.f;
    const float PI = 3.14159265358979f;
    switch (type) {
        case 0:  return t;                                                   // none/linear
        case 1:  return t < 0.5f ? 0.5f*std::pow(2*t, rate)                  // EaseInOut
                                 : 1.f - 0.5f*std::pow(2*(1-t), rate);
        case 2:  return std::pow(t, rate);                                   // EaseIn
        case 3:  return 1.f - std::pow(1-t, rate);                           // EaseOut
        case 4:  // ElasticInOut
            if (t==0||t==1) return t;
            { float p=0.45f; t=t*2-1;
              if (t<0) return -0.5f*std::pow(2,10*t)*std::sin((t-p/4)*(2*PI/p));
              return std::pow(2,-10*t)*std::sin((t-p/4)*(2*PI/p))*0.5f+1; }
        case 5:  // ElasticIn
            if (t==0||t==1) return t;
            { float p=0.3f; return -std::pow(2,10*(t-1))*std::sin((t-1-p/4)*(2*PI/p)); }
        case 6:  // ElasticOut
            if (t==0||t==1) return t;
            { float p=0.3f; return std::pow(2,-10*t)*std::sin((t-p/4)*(2*PI/p))+1; }
        case 7:  return t<0.5f ? (1-bounceOut(1-2*t))*0.5f : bounceOut(2*t-1)*0.5f+0.5f; // BounceInOut
        case 8:  return 1.f - bounceOut(1-t);                                // BounceIn
        case 9:  return bounceOut(t);                                        // BounceOut
        case 10: // ExponentialInOut
            if (t==0||t==1) return t;
            return t<0.5f ? 0.5f*std::pow(2,20*t-10) : 1-0.5f*std::pow(2,-20*t+10);
        case 11: return t==0?0:std::pow(2,10*(t-1));                          // ExponentialIn
        case 12: return t==1?1:1-std::pow(2,-10*t);                           // ExponentialOut
        case 13: return -0.5f*(std::cos(PI*t)-1);                             // SineInOut
        case 14: return 1-std::cos(t*PI/2);                                   // SineIn
        case 15: return std::sin(t*PI/2);                                     // SineOut
        case 16: { float s=1.70158f*1.525f;                                  // BackInOut
                   t*=2; if (t<1) return 0.5f*(t*t*((s+1)*t-s));
                   t-=2; return 0.5f*(t*t*((s+1)*t+s)+2); }
        case 17: { float s=1.70158f; return t*t*((s+1)*t-s); }               // BackIn
        case 18: { float s=1.70158f; t-=1; return t*t*((s+1)*t+s)+1; }       // BackOut
        default: return t;
    }
}


// GD trigger object IDs we simulate (physics-affecting only).
//   901  = Move        1346 = Rotate      1347 = Follow
//   1049 = Toggle      1007 = Alpha       1268 = Spawn
bool isTriggerId(int id) {
    return id == 901 || id == 1346 || id == 1347 ||
           id == 1049 || id == 1007 || id == 1268;
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
        default:
            return std::nullopt;
    }
    if (t.targetGroup == 0) return std::nullopt; // nothing to affect/spawn
    return t;
}

} // namespace gdsim
