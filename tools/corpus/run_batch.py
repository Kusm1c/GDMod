#!/usr/bin/env python3
"""Replay every corpus replay through gdsim (gdrcheck.exe) and score gdsim's fidelity.

    py -3 tools/corpus/run_batch.py [--exe gdrcheck.exe] [--out results.tsv] [--compare old.tsv] [-j N]

A replay is a real run of the real engine. If it is a verified clear (GDR2 deaths == 0, or a
list macro), gdsim must clear it too; anything else is a gdsim bug (or a level-version mismatch).
Replays whose own header says deaths > 0 are attempts and are skipped.

Columns: levelId, replay, source, verdict (CLEAR/DIES/OOF/ERR/SKIP), reach% (x / level length),
frame, cause, killObjType, killX, killY.
Summary: clears, mean reach, death-cause histogram, and — with --compare — every replay whose
verdict or reach moved, which is the A/B gate for a physics change.
"""
import concurrent.futures as cf, os, re, subprocess, sys, time
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "testlevel" / "corpus"


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def run_one(exe, row):
    lid, rpath, src = row
    lvl = CORPUS / "levels" / f"{lid}.txt"
    try:
        p = subprocess.run([exe, str(ROOT / rpath), str(lvl)], capture_output=True, timeout=600)
        out = p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return [lid, rpath, src, "ERR", "0", "0", "timeout", "0", "0", "0"]
    m = re.search(r"deaths=(\d+)", out)
    if m and int(m.group(1)) > 0:
        return [lid, rpath, src, "SKIP", "0", "0", "attempt", "0", "0", "0"]
    m = re.search(r"CLEARS at frame=(\d+)", out)
    if m:
        return [lid, rpath, src, "CLEAR", "100.00", m.group(1), "-", "0", "0", "0"]
    m = re.search(r"DIES at frame=(\d+) x=(\S+) y=\S+ cause=(\S+) killObjType=(-?\d+) "
                  r"killObjPos=\((\S+),(\S+)\)\s+\(level length=(\S+), (\S+)%\)", out)
    if m:
        return [lid, rpath, src, "DIES", f"{float(m.group(8)):.2f}", m.group(1), m.group(3),
                m.group(4), m.group(5), m.group(6)]
    m = re.search(r"ran out of frames, maxX=(\S+) / (\S+)", out)
    if m:
        return [lid, rpath, src, "OOF", f"{100*float(m.group(1))/max(1.0,float(m.group(2))):.2f}",
                "0", "oof", "0", "0", "0"]
    return [lid, rpath, src, "ERR", "0", "0", (out.strip().splitlines() or ["?"])[-1][:60].replace("\t", " "),
            "0", "0", "0"]


def load(path):
    rows = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        c = line.split("\t")
        if len(c) >= 10 and c[0] != "levelId":
            rows[c[1]] = c
    return rows


def summary(rows, title):
    valid = [r for r in rows.values() if r[3] in ("CLEAR", "DIES", "OOF")]
    clears = sum(r[3] == "CLEAR" for r in valid)
    reach = sum(float(r[4]) for r in valid) / max(1, len(valid))
    lv = {}
    for r in valid:
        lv[r[0]] = max(lv.get(r[0], 0.0), float(r[4]))
    lclear = sum(v >= 100.0 for v in lv.values())
    causes = Counter(r[6] for r in valid if r[3] == "DIES")
    print(f"== {title}: {clears}/{len(valid)} replays clear ({lclear}/{len(lv)} levels), "
          f"mean reach {reach:.2f}%  (skipped {sum(r[3]=='SKIP' for r in rows.values())}, "
          f"err {sum(r[3]=='ERR' for r in rows.values())})")
    print("   causes: " + ", ".join(f"{k} {v}" for k, v in causes.most_common()))
    objs = Counter(f"{r[6]}/id{r[7]}" for r in valid if r[3] == "DIES")
    print("   killers: " + ", ".join(f"{k} {v}" for k, v in objs.most_common(12)))
    return clears, reach


def main():
    exe = str((ROOT / arg("--exe", "gdrcheck.exe")).resolve())
    out = Path(arg("--out", str(CORPUS / "results.tsv")))
    jobs = int(arg("-j", str(max(1, (os.cpu_count() or 4) - 2))))
    # Every replay on disk whose level is on disk (the builder's manifest is only rewritten
    # at the end of each phase, so scanning lets a batch run while the corpus still grows).
    manifest = []
    for f in sorted((CORPUS / "replays").glob("*/*")):
        lid = f.parent.name
        if (CORPUS / "levels" / f"{lid}.txt").exists() and f.stat().st_size > 16:
            manifest.append((lid, f.relative_to(ROOT).as_posix(), f.name.split("_")[0]))
    only = arg("--only", None)
    if only:
        manifest = [m for m in manifest if re.search(only, m[1])]
    t0 = time.time()
    with cf.ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(lambda r: run_one(exe, r), manifest))
    hdr = "levelId\treplay\tsource\tverdict\treach\tframe\tcause\tkillObjType\tkillX\tkillY\n"
    out.write_text(hdr + "".join("\t".join(r) + "\n" for r in sorted(res)), encoding="utf-8")
    rows = {r[1]: r for r in res}
    print(f"[{len(res)} replays in {time.time()-t0:.0f}s -> {out}]")
    summary(rows, "now")
    cmp = arg("--compare", None)
    if cmp and Path(cmp).exists():
        old = load(cmp)
        summary(old, "before")
        better = worse = 0
        for k, r in sorted(rows.items()):
            o = old.get(k)
            if not o or o[3] == "SKIP" or r[3] == "SKIP":
                continue
            d = float(r[4]) - float(o[4])
            if abs(d) > 0.005 or o[3] != r[3]:
                better += d > 0; worse += d < 0
                print(f"   {'+' if d > 0 else '-'} {r[0]:>10} {Path(k).name[:34]:34} {o[3]} {o[4]:>6}% -> "
                      f"{r[3]} {r[4]:>6}%  ({r[6]} id{r[7]} @{r[8]},{r[9]})")
        print(f"   A/B: {better} better, {worse} worse")


if __name__ == "__main__":
    main()
