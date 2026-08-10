#include "Portals.hpp"
#include "Player.hpp"
#include "Calib.hpp"

namespace gdsim {

// COUNTERINTUITIVE but PROVEN: in real GD, id 101 = MINI, id 99 = BIG — the
// OPPOSITE of the commonly-assumed convention. See sizeportal_mini_big_inverted
// memory: a 2026-07-18 real-engine capture on level 98414841 recorded the actual
// PlayerObject::m_isMini flag frame-by-frame and found it flips ON exactly at the
// id=101 portal and OFF exactly at the id=99 portal — direct ground truth, not
// inference. A 2026-07-07 attempt to "fix" this to ==99 (reasoning from expected
// GD-wiki convention / solver behavior, not a capture) was itself the bug.
//
// Re-discovered the hard way 2026-08-09 while chasing an unrelated "bounds"
// false-death on level 61079355 ("Acu"): flipped this to ==99 again (it LOOKED
// obviously right), then ran the same fix through a whole-batch A/B against the
// 2026-08-09 macro-demonlist (157 real, human-verified-clearing macros,
// testlevel/macrolist/) rather than trusting the single level being chased — it
// REGRESSED the 13-level truth bank (98414841: first-divergence frame 1988→1537,
// the SAME level the original capture was taken from) and dropped the macro
// batch's average simulated-progress from 7.85% to 5.92% (51 levels measurably
// worse vs only 13 better). Reverted back to ==101, which matches the capture.
// Lesson for next time: a mapping that "looks backwards" relative to general GD
// knowledge can still be the correct one here if it's capture-proven — check this
// project's own memory before re-deriving from first principles, and always
// re-validate ANY size/portal change with the same whole-batch A/B, not one level.
SizePortal::SizePortal(Vec2D s, std::unordered_map<int, std::string>&& fields)
    : EffectObject(s, std::move(fields)), small(numFromString<int>(fields[1]) == 101) { triggerCat = PCAT_SIZE; }

void SizePortal::collide(Player& p) const {
    EffectObject::collide(p);
    p.small = small;
}

} // namespace gdsim
