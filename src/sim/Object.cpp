#include "Object.hpp"
#include "Player.hpp"
#include "Level.hpp"
#include "Calib.hpp"
#include "Block.hpp"
#include "Hazard.hpp"
#include "Portals.hpp"
#include "Pad.hpp"
#include "Orb.hpp"
#include "Slope.hpp"
#include <string>
#include <climits>
#include <cmath>

namespace gdsim {

struct range : public std::pair<int, int> {
    range(int i)       : std::pair<int,int>({i, i}) {}
    range(int i, int j): std::pair<int,int>({i, j}) {}
};

static std::vector<int> unroll(std::vector<range> ranges) {
    std::vector<int> res;
    for (auto r : ranges)
        for (int i = r.first; i <= r.second; i++)
            res.push_back(i);
    return res;
}

#define objs(x, type, w, h) \
    for (auto& i : unroll x) \
        if (id == i) \
            return ObjectContainer(type({(float)(w), (float)(h)}, std::move(ob)));

std::optional<ObjectContainer> Object::create(std::unordered_map<int, std::string>&& ob) {
    // Robust id parse: skip objects with a missing / non-numeric id field instead
    // of throwing (some levels carry malformed/edge objects; throwing would abort
    // the whole parse). numFromString never throws — a fully non-numeric field
    // yields the sentinel, which we treat as "skip" (preserving the old catch path).
    auto it = ob.find(1);
    if (it == ob.end() || it->second.empty()) return {};
    constexpr int kBadId = INT_MIN;
    int id = numFromString<int>(it->second, kBadId);
    if (id == kBadId) return {};

    objs(({ {1,4},{6,7},63,{69,72},{74,78},{81,83},{90,96},{116,119},{121,122},146,
             {160,163},{165,169},173,175,{207,210},{212,213},{247,250},{252,258},
             {260,261},{263,265},{267,272},{274,275},467,{469,471},{1203,1204},
             {1209,1210},{1221,1222},1226 }), Block, 30, 30)

    objs(({ 64,195,206,220,661,{1155,1157},1208,1910 }), Block, 15, 15)
    objs(({ 40,147,215,{369,370},{1903,1905} }), Block, 30, 14)
    objs(({ {170,172},174,192 }), Block, 30, 21)
    objs(({ 468,475,1260 }), Block, 30, 1.5)
    objs(({ 62,65,66,68 }), Block, 30, 16)
    objs(({ 1202,1262 }), Block, 30, 3)
    objs(({ 1220,1264 }), Block, 30, 6)
    objs(({ 196,219,1911 }), Block, 15, 8)
    objs(({ 204 }), Block, 8, 15)
    objs(({ 662,663,664 }), Block, 30, 15)
    objs(({ 1561 }), Block, 30, 10)
    objs(({ 1567 }), Block, 15, 10)
    objs(({ 1566 }), Block, 12, 12)
    objs(({ 1565 }), Block, 17, 17)
    objs(({ 1227 }), Block, 30, 7)
    objs(({ 328 }), Block, 22, 22)
    objs(({ 197 }), Block, 22, 21)
    objs(({ 194 }), Block, 21, 21)
    objs(({ 176 }), Block, 14, 21)
    objs(({ 1562 }), Block, 30, 2)
    objs(({ 1343 }), Block, 25, 3)
    objs(({ 1340 }), Block, 27, 2)
    objs(({ 34 }), Block, 37, 23)
    objs(({ 329 }), Block, 43, 22)
    objs(({ 1154 }), Block, 15, 1.5)
    objs(({ 1563 }), Block, 15, 2)
    objs(({ 1564 }), Block, 12, 12)
    objs(({ 1568 }), Block, 62, 32)
    objs(({ 1569 }), Block, 32, 32)

    // D block (id 1755): a wave-safe solid. Grounded modes land on it like a normal
    // 30×30 block; the wave never dies inside it (handled in Block::collide). Hitbox
    // size assumed 30×30 (default block) pending validation from GDMod_hitbox capture.
    objs(({ 1755 }), Block, 30, 30)

    objs(({ 720,991,1731,1733 }), Hazard, 2.40039063, 3.20001221)
    objs(({ 61,446,1719,1728 }), Hazard, 9, 7.2)
    objs(({ 365,667,1716,1730 }), Hazard, 9, 6)
    objs(({ 392,{458,459} }), Hazard, 2.6, 4.8)
    objs(({ 8,144,177,216 }), Hazard, 6, 12)
    objs(({ 103,145,218 }), Hazard, 4, 7.6)
    objs(({ 39,205,217 }), Hazard, 6, 5.6)
    objs(({ 768,1727 }), Hazard, 4.5, 5.2)
    objs(({ 447,1729 }), Hazard, 5.2, 7.2)
    objs(({ 135,1711 }), Hazard, 14.1, 20)
    objs(({ 422,1726 }), Hazard, 6, 4.4)
    objs(({ 244,1721 }), Hazard, 6, 6.8)
    objs(({ 243,1720 }), Hazard, 6, 7.2)
    objs(({ 421,1725 }), Hazard, 9, 5.2)
    objs(({ 9,1715 }), Hazard, 9, 10.8)
    objs(({ 989,1732 }), Hazard, 9, 12)
    objs(({ 1714 }), Hazard, 11.4, 16.4)
    objs(({ 1712 }), Hazard, 13.5, 22.4)
    objs(({ 368,1722 }), Hazard, 9, 4)
    objs(({ 1713 }), Hazard, 11.7, 20)
    objs(({ 178 }), Hazard, 6, 6.4)
    objs(({ 919 }), Hazard, 25, 6)
    objs(({ 179 }), Hazard, 4, 8)
    objs(({ 1327,1584,2012 }), Hazard, 8, 8)
    objs(({ 1328 }), Hazard, 8, 15)

    objs(({ 88,186,740,1705 }), Sawblade, 32.3, 32.3)
    objs(({ 89,1706 }), Sawblade, 21.6, 21.6)
    objs(({ 98 }), Sawblade, 12, 12)
    objs(({ 183 }), Sawblade, 15.660001, 15.660001)
    objs(({ 184 }), Sawblade, 20.4, 20.4)
    objs(({ 185 }), Sawblade, 2.8500001, 2.8500001)
    objs(({ 187,741 }), Sawblade, 21.960001, 21.960001)
    objs(({ 188,742 }), Sawblade, 12.6000004, 12.6000004)
    objs(({ 397,1708 }), Sawblade, 28.9, 28.9)
    objs(({ 398,1709 }), Sawblade, 17.44, 17.44)
    objs(({ 399,1710 }), Sawblade, 12.900001, 12.900001)
    objs(({ 675,1734 }), Sawblade, 32, 32)
    objs(({ 676,1735 }), Sawblade, 17.5100002, 17.5100002)
    objs(({ 677,1736 }), Sawblade, 12.479999, 12.479999)
    objs(({ 678 }), Sawblade, 30.4, 30.4)
    objs(({ 679 }), Sawblade, 18.54, 18.54)
    objs(({ 680 }), Sawblade, 10.8, 10.8)
    objs(({ 918 }), Sawblade, 24, 24)
    objs(({ 1582,1583 }), Sawblade, 4, 4)
    objs(({ 1619 }), Sawblade, 25, 25)
    objs(({ 1620 }), Sawblade, 15, 15)
    objs(({ 1707 }), Sawblade, 12, 12)
    objs(({ {1701,1703} }), Sawblade, 6, 6)

    objs(({ 35 }), Pad, 25, 4)
    objs(({ 140 }), Pad, 25, 5)
    objs(({ 67 }), Pad, 25, 6)
    objs(({ 1332 }), Pad, 25, 6)   // red pad — Pad.cpp handled it but the factory never built it (red pads were invisible to the sim). Hitbox approx; verify.

    objs(({ 36,84,141,1022,1330,1333,1704 }), Orb, 36, 36)

    objs(({ 12,13,47,111,660,745,1331,1933,2751 }), VehiclePortal, 34, 86)
    objs(({ 747 }), TeleportPortal, 34, 86)
    objs(({ 10,11 }), GravityPortal, 25, 75)
    objs(({ 99,101 }), SizePortal, 31, 90)
    objs(({ 286,287 }), DualPortal, 34, 86)
    objs(({ 143 }), BreakableBlock, 30, 30)

    objs(({ 200 }), SpeedPortal, 35, 44)
    objs(({ 201 }), SpeedPortal, 33, 56)
    objs(({ 202 }), SpeedPortal, 51, 56)
    objs(({ 203 }), SpeedPortal, 65, 56)
    objs(({ 1334 }), SpeedPortal, 69, 56)

    objs(({ 289,294,299,305,309,315,321,326,331,337,343,349,353,363,371,483,492,651,665,
             673,709,711,726,728,886,1338,1341,1344,1723,1743,1745,1747,1749,1906 }), Slope, 30, 30)
    objs(({ 1717 }), SlopeHazard, 30, 30);

    objs(({ 291,295,301,307,311,317,323,327,333,339,345,351,355,364,366,367,372,484,493,652,666,
             674,710,712,727,729,887,1339,1342,1345,1724,1744,1746,1748,1750,1907 }), Slope, 60, 30)
    objs(({ 1718 }), SlopeHazard, 60, 30);

    return {};
}

Object::Object(Vec2D s, std::unordered_map<int, std::string>&& fields) {
    // Save the GD object type ID before fields is potentially moved by a subclass.
    // 'id' is later overwritten by Level with a sequential instance index needed
    // by EffectObject for usedEffects uniqueness tracking.
    typeId = numFromString<int>(fields[1]);
    id     = 0;
    size = s;
    pos.x    = stod_def(fields[2]);
    pos.y    = stod_def(fields[3]);
    rotation = -stod_def(fields[6]);
    prio = 0;

    float scale  = stod_def(fields[32],  1);
    float scalex = stod_def(fields[128], 1);
    float scaley = stod_def(fields[129], 1);
    size.x *= scale * scalex;
    size.y *= scale * scaley;
}

bool Object::touching(Player const& player) const {
    // GD's player collision hitbox is ALWAYS axis-aligned — the cube's visual spin
    // is cosmetic and never rotates the hitbox. The old code rotated the player box
    // by its spin angle when the OBJECT was rotated (e.g. a 45° block), so gdsim
    // missed collisions the real game registers (player threads a tilted block in
    // sim but dies on replay). Always use the unrotated player box; the SAT test
    // (intersects) still accounts for the object's own rotation.
    Entity playerHb = player.unrotatedHitbox();

    // Calibratable portal trigger reach: widen the portal's hitbox so it fires
    // earlier (+) / later (-) to match GD's exact trigger frame. 0 = unchanged.
    if (triggerCat != PCAT_NONE) {
        float reach = g_calib.portalReach[triggerCat];
        if (reach != 0.f) {
            Entity self = *this;
            self.size.x += 2.f * reach;
            self.size.y += 2.f * reach;
            return self.intersects(playerHb);
        }
    }
    return intersects(playerHb);
}

void Object::collide(Player&) const { abort(); }

} // namespace gdsim
