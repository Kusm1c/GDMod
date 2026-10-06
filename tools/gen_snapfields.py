"""gen_snapfields.py — derive the engine-state snapshot FROM THE ENGINE'S OWN CODE.

WHY: the lockstep bench (test/lockstep.cpp) validates one ported engine function at a time:
load the real state captured just before the call, run our port, compare to the real state
captured just after. That only works if the snapshot contains every field those functions
read or write — and guessing that list is exactly the kind of approximation this program
exists to remove. So the list is derived: annotate each in-scope function of the named
decompile with the compiler-exact offset table (tools/gd_fields.txt, see gen_offsets.py),
collect every PlayerObject member it touches, take the union.

OUTPUT: src/sim/engine/SnapFields.inc — an X-macro list, the SINGLE source of truth shared by
  * the mod's capture hook (src/engine_trace.cpp), which expands it to `s.f = player->f;`
  * gdsim's EngineSnap struct + field-by-field comparison (Geode-free, so plain types only).
Because both sides expand the same list, "what is captured" and "what is compared" cannot
drift apart.

    SNAP_SCALAR(type, name, offset)   bool / int / float / double / unsigned char
    SNAP_POINT(name, offset)          cocos2d::CCPoint -> two floats  name_x, name_y
    SNAP_NODEPOS(name)                the CCNode position (getPosition), defaults to SNAP_POINT
    SNAP_OBJ(name, offset)            GameObject*      -> identity (m_uniqueID, 0 = null)
    SNAP_SEED(name, offset)           geode::SeedValueRSV (GD anti-cheat obfuscated int)

LIMITS (stated, not hidden): fieldname.py only resolves accesses written `param_1 + 0x..`
on the function's own `this`. Fields reached through another pointer (a local copy of the
player, the game layer) are not seen, and on functions whose param_1 is typed `longlong *`
Ghidra's index is x8 — see the named_decompile_toolchain memory. The union is therefore a
strong starting set, not a proof of completeness; the lockstep bench reveals anything
missing as an unexplained mismatch, and the fix is to extend SCOPE / EXTRA below.

    py -3 tools/gen_snapfields.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fieldname  # noqa: E402  (reuses its annotate() and the gd_fields.txt loader)

DECOMP = r"C:\Users\Kusmic\decompGD\GeometryDash.named.c"
HEADER = os.path.join(ROOT, "build", "bindings", "bindings", "Geode", "binding", "PlayerObject.hpp")
OUT = os.path.join(ROOT, "src", "sim", "engine", "SnapFields.inc")

# The engine functions this program ports (plan, phase 2) that operate on `this` = PlayerObject.
SCOPE = [
    "updateTimeMod", "updateJump", "playerIsFallingBugged", "flipGravity",
    "hitGround", "hitGroundNoJump", "didHitHead", "checkSnapJumpToObject",
    "collidedWithObjectInternal", "updateCollide", "updateCollideLeft", "updateCollideRight",
    "postCollision", "preSlopeCollision", "collidedWithSlopeInternal",
    "ringJump", "bumpPlayer", "propellPlayer", "boostPlayer",
    "toggleFlyMode", "toggleRollMode", "toggleBirdMode", "toggleDartMode",
    "toggleRobotMode", "toggleSpiderMode", "toggleSwingMode", "togglePlayerScale",
    "spiderTestJumpInternal", "update",
]

SCALARS = {"bool", "int", "float", "double", "unsigned char", "unsigned int", "short"}

# Members the engine reaches WITHOUT a raw `param_1 + off` access, so the offset scan above
# cannot see them. Each entry needs a reason.
EXTRA = {
    # NOT the live position (an earlier version of this entry assumed it was). The binary
    # writes 0xa90/0xa94 only in the constructor and in spiderTestJumpInternal
    # (0x1403950c0 / 0x1403950d2, one axis at a time), through float stores the offset scan
    # does not attribute to a CCPoint member.
    "m_position": "spiderTestJumpInternal stores 0xa90/0xa94",
}

# The live player position is NOT a PlayerObject member: it is cocos2d::CCNode's own
# position, read and written through the virtual getPosition() (vtable +0xc8) and
# setPosition() (+0xb8; PlayerObject::setPosition 0x14039c640 -> GameObject::setPosition
# 0x140197b60 -> CCSprite::setPosition). Emitted as SNAP_NODEPOS so the capture side can use
# getPosition() while every other expander treats it as a plain point.
NODE = {
    "nodePosition": "CCNode position via virtual getPosition/setPosition (update, updateJump, ...)",
}


def function_ranges(src_lines):
    """Map PlayerObject method name -> (start, end) line numbers in the named decompile.
    A function body runs from its signature to the next top-level signature."""
    # The return type is usually on the same line (`void PlayerObject__f__14..(`) but some
    # functions put it on the line above, so the name starts at column 0
    # (`PlayerObject__collidedWithObjectInternal__140391a70(` under `undefined8`) — the old
    # pattern required a character before the name and silently missed exactly the
    # biggest function in scope.
    sig = re.compile(r"^(?:\S.*\s)?(\w+?)__(\w+)__(14[0-9a-f]{7})\s*\(")
    starts = []
    for i, ln in enumerate(src_lines):
        m = sig.match(ln)
        if m and not ln.rstrip().endswith(";"):
            starts.append((i, m.group(1), m.group(2)))
    ranges = {}
    for k, (i, cls, fn) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(src_lines)
        if cls == "PlayerObject" and fn not in ranges:
            ranges[fn] = (i, end)
    return ranges


def member_types():
    types = {}
    with open(HEADER, encoding="utf-8") as fh:
        for ln in fh:
            s = ln.strip()
            m = re.match(r"^(?P<t>[^(;]+?)\s+\*?(?P<n>m_\w+)\s*;$", s)
            if m and "(" not in s:
                t = m.group("t").strip()
                if s.split()[-1].startswith("*"):
                    t += "*"
                types[m.group("n")] = t
    return types


def main():
    with open(DECOMP, encoding="utf-8", errors="replace") as fh:
        src = fh.readlines()
    ranges = function_ranges(src)
    off = fieldname.load("PlayerObject")   # {offset: name}

    used, per_fn, not_found = {}, {}, []
    for fn in SCOPE:
        if fn not in ranges:
            not_found.append(fn)
            continue
        a, b = ranges[fn]
        text = fieldname.annotate("".join(src[a:b]), off)
        found = sorted(set(re.findall(r"/\*(m_\w+)\*/", text)))
        per_fn[fn] = len(found)
        for n in found:
            used.setdefault(n, []).append(fn)
    for n, why in EXTRA.items():
        used.setdefault(n, []).append("EXTRA: " + why)

    types = member_types()
    inv = {name: o for o, name in off.items()}   # name -> offset, for the annotation

    lines = ["// GENERATED by tools/gen_snapfields.py — do not edit by hand, re-run the tool.",
             "// Every PlayerObject member read or written by the in-scope engine functions",
             "// (tools/gen_snapfields.py SCOPE), with its compiler-exact offset.",
             "// Expand with SNAP_SCALAR / SNAP_POINT / SNAP_OBJ / SNAP_SEED (see the tool's doc).",
             "// SNAP_NODEPOS defaults to SNAP_POINT(n, 0); only the capture side needs to tell it apart.",
             "",
             "#ifndef SNAP_NODEPOS",
             "#define SNAP_NODEPOS(n) SNAP_POINT(n, 0)",
             "#define SNAP_NODEPOS_DEFAULTED",
             "#endif",
             ""]
    for n, why in NODE.items():
        lines.append("SNAP_NODEPOS(%s)  // %s" % (n, why))
    skipped = []
    for n in sorted(used):
        t = types.get(n, "?")
        o = inv.get(n)
        o_s = "0x%x" % o if isinstance(o, int) else "?"
        users = ", ".join(used[n][:4]) + (" ..." if len(used[n]) > 4 else "")
        if t in SCALARS:
            lines.append("SNAP_SCALAR(%s, %s, %s)  // %s" % (t.replace(" ", "_"), n, o_s, users))
        elif t == "cocos2d::CCPoint":
            lines.append("SNAP_POINT(%s, %s)  // %s" % (n, o_s, users))
        elif t == "GameObject*":
            lines.append("SNAP_OBJ(%s, %s)  // %s" % (n, o_s, users))
        elif t == "geode::SeedValueRSV":
            lines.append("SNAP_SEED(%s, %s)  // %s" % (n, o_s, users))
        else:
            skipped.append("%s (%s)" % (n, t))
    lines.append("")
    lines.append("#ifdef SNAP_NODEPOS_DEFAULTED")
    lines.append("#undef SNAP_NODEPOS")
    lines.append("#undef SNAP_NODEPOS_DEFAULTED")
    lines.append("#endif")
    lines.append("")
    lines.append("// Not snapshotted (presentation / container state, not physics):")
    for s in skipped:
        lines.append("//   " + s)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")

    print("functions resolved: %d/%d" % (len(per_fn), len(SCOPE)))
    for fn in SCOPE:
        if fn in per_fn:
            print("  %-28s %3d fields" % (fn, per_fn[fn]))
    if not_found:
        print("  NOT FOUND in decompile (inline or renamed): " + ", ".join(not_found))
    snap = sum(1 for l in lines if l.startswith("SNAP_"))
    print("snapshot entries: %d   skipped non-physics: %d   -> %s" % (snap, len(skipped), OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
