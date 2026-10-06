#!/usr/bin/env python3
"""Which object ids (triggers above all) does the corpus actually use, and which does gdsim know?

    py -3 tools/corpus/census.py [results.tsv]

For every corpus level: count object ids. Triggers are reported with their GD name, the number
of levels using them, and — given a batch results.tsv — how many of those levels gdsim fails
on BEFORE the replay's end. A trigger that appears in many failing levels and that gdsim does
not model is a candidate for the next port.
"""
import re, sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "testlevel" / "corpus"

TRIG = {
    901: "Move", 1616: "Stop", 1006: "Pulse", 1007: "Alpha", 1049: "Toggle", 1268: "Spawn",
    1346: "Rotate", 1347: "Follow", 1585: "Animate", 1595: "Touch", 1611: "Count",
    1811: "InstantCount", 1812: "OnDeath", 1814: "FollowPlayerY", 1815: "Collision",
    1817: "Pickup", 1818: "BGEffectOn", 1819: "BGEffectOff", 1912: "Random", 1913: "Zoom",
    1914: "StaticCamera", 1916: "OffsetCamera", 1917: "Reverse", 1931: "End",
    1932: "PlayerControl", 1934: "Song", 1935: "TimeWarp", 2015: "RotateCamera",
    2016: "CameraGuide", 2062: "CameraEdge", 2066: "Gravity", 2067: "Scale",
    2068: "AdvRandom", 2899: "Options", 2900: "RotateGameplay", 2901: "GameplayOffset",
    2903: "Gradient", 2999: "MGConfig", 3006: "AreaMove", 3007: "AreaRotate", 3008: "AreaScale",
    3009: "AreaFade", 3010: "AreaTint", 3011: "EditAreaMove", 3016: "AdvFollow",
    3017: "EnterArea", 3600: "End2", 3602: "SFX", 3603: "EditSFX", 3604: "Event",
    3605: "EditSong", 3606: "BGSpeed", 3607: "Sequence", 3608: "SpawnParticle",
    3609: "InstantCollision", 3612: "MGSpeed", 3613: "UI", 3614: "Time", 3615: "TimeEvent",
    3617: "TimeControl", 3618: "ResetGroup", 3619: "ItemEdit", 3620: "ItemCompare",
    3640: "CollisionState", 3641: "PersistentItem", 3642: "BPM", 3660: "EditAdvFollow",
    3661: "RetargetAdvFollow", 1520: "Shake", 1612: "HidePlayer", 1613: "ShowPlayer",
    1915: "DontFade?", 2925: "CameraMode", 899: "Color", 29: "BGColor(old)", 30: "GColor(old)",
    105: "ObjColor(old)", 744: "3DLColor(old)", 915: "LineColor(old)", 31: "StartPos",
    22: "Enter1", 24: "Enter2", 23: "Enter3", 25: "Enter4", 26: "Enter5", 27: "Enter6",
    28: "Enter7", 55: "Enter8", 56: "Enter9", 57: "Enter10", 58: "Enter11", 59: "Enter12",
    1915: "EnterStop",
}
# Triggers gdsim models (src/sim/Trigger.hpp TriggerKind + Level.cpp handling).
MODELLED = {901, 1346, 1049, 1007, 1268, 1347, 31}


def ids_of(level_text):
    c = Counter()
    for obj in level_text.split(";")[1:]:
        m = re.match(r"1,(\d+)(,|$)", obj)
        if m:
            c[int(m.group(1))] += 1
    return c


def main():
    # A level "fails" when no replay of it clears in gdsim.
    best = {}
    if len(sys.argv) > 1:
        for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()[1:]:
            c = line.split("	")
            if c[3] in ("CLEAR", "DIES", "OOF"):
                best[c[0]] = best.get(c[0], False) or c[3] == "CLEAR"
    fail = {l: not ok for l, ok in best.items()}
    uses, levels = defaultdict(set), 0
    for f in sorted((CORPUS / "levels").glob("*.txt")):
        levels += 1
        for i, n in ids_of(f.read_text(encoding="utf-8", errors="replace")).items():
            if i in TRIG:
                uses[i].add(f.stem)
    print(f"{levels} levels")
    print(f"{'id':>5} {'trigger':18} {'levels':>6} {'failing':>7}  modelled")
    for i, ls in sorted(uses.items(), key=lambda kv: -len(kv[1])):
        nf = sum(1 for l in ls if fail.get(l))
        print(f"{i:>5} {TRIG[i]:18} {len(ls):>6} {nf:>7}  {'yes' if i in MODELLED else '-'}")


if __name__ == "__main__":
    main()
