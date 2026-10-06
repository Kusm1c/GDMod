#!/usr/bin/env python3
"""Build the gdsim fidelity corpus: real verified-clear replays + the level strings they play.

    py -3 tools/corpus/build_corpus.py [--no-showcase] [--max-showcase N]

Sources (all real runs of the REAL engine — whatever gdsim reports on them is gdsim's fault):
  A. Paul's Macro Demonlist (testlevel/macrolist/source_links.tsv), seed-flagged entries SKIPPED
     (RNG-dependent levels cannot be checked by a deterministic sim). Level ids come from
     results_2026-08-09.tsv. Macros are re-downloaded from Google Drive.
  B. The Showcase v4 archive (showcase-api.flafy.dev), keyed by sha256(decompressed level string),
     for every level in the corpus. Only verified clears are useful; gdrcheck reports deaths=0.
  C. The user's own bot replays (Eclipse replays folder).

Output: testlevel/corpus/levels/<id>.txt, testlevel/corpus/replays/<id>/<src>_<name>, and
testlevel/corpus/manifest.tsv (levelId \t replayPath \t source). Idempotent: anything already on
disk is skipped, so it can be re-run after an interruption.

GD SERVER ETIQUETTE: boomlings.com sits behind Cloudflare and bans IPs that hammer it. Every
request goes through gd_post(), which enforces a minimum gap between requests (GD_GAP seconds)
and backs off hard on 'error code:' / 429 / 5xx answers. Never call boomlings directly.
"""
import base64, hashlib, json, os, re, shutil, subprocess, sys, time, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "testlevel" / "corpus"
LEVELS = CORPUS / "levels"
REPLAYS = CORPUS / "replays"
LOG = CORPUS / "build.log"
ECLIPSE = Path(os.path.expandvars(r"%LOCALAPPDATA%")) / "GeometryDash/geode/mods/eclipse.eclipse-menu/replays"
SHOWCASE = "https://showcase-api.flafy.dev"
GD_GAP = 15.0          # seconds between two boomlings requests, minimum
_last_gd = [0.0]


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def curl(args, out=None, timeout=60):
    cmd = ["curl", "-s", "-L", "-m", str(timeout), "-w", "\n%{http_code}"] + args
    if out:
        cmd[cmd.index("-w") + 1] = "%{http_code}"
        cmd += ["-o", str(out)]
    p = subprocess.run(cmd, capture_output=True)
    txt = p.stdout.decode("utf-8", "replace")
    if out:
        return int(txt.strip() or 0), None
    body, _, code = txt.rpartition("\n")
    return int(code or 0), body


def gd_post(path, data):
    """Rate-limited POST to boomlings. Returns body or None."""
    backoff = 30
    for attempt in range(5):
        wait = _last_gd[0] + GD_GAP - time.time()
        if wait > 0:
            time.sleep(wait)
        _last_gd[0] = time.time()
        code, body = curl(["-X", "POST", "-H", "User-Agent:", "-d", data,
                           "http://www.boomlings.com/database/" + path])
        if code == 200 and body and not body.startswith("error code"):
            return body
        if code == 200 and body == "-1":
            return "-1"
        if code == 404:
            log(f"  GD {path} -> HTTP 404 (level unavailable), giving up")
            return None
        log(f"  GD {path} -> HTTP {code} {body[:40]!r}; backing off {backoff}s")
        if body and body.startswith("error code: 1015"):
            backoff = max(backoff, 900)          # Cloudflare rate-limit ban: wait it out
        time.sleep(backoff)
        backoff = min(backoff * 2, 1800)
    return None


def decode_level_data(field):
    if field.startswith("kS") or field.startswith("kA") or ";" in field[:200]:
        return field
    raw = base64.urlsafe_b64decode(field + "=" * (-len(field) % 4))
    return zlib.decompress(raw, 47).decode("utf-8", "replace")


def fetch_level(lid):
    out = LEVELS / f"{lid}.txt"
    if out.exists() and out.stat().st_size > 100:
        return out
    cache = ROOT / "app" / "cache" / f"{lid}.txt"
    if cache.exists():
        lines = cache.read_text(encoding="utf-8", errors="replace").split("\n", 1)
        if len(lines) == 2 and ";" in lines[1]:
            out.write_text(lines[1].strip(), encoding="utf-8")
            return out
    body = gd_post("downloadGJLevel22.php",
                   f"gameVersion=22&binaryVersion=45&levelID={lid}&secret=Wmfd2893gb7")
    if not body or body == "-1":
        log(f"  level {lid}: download failed ({body})")
        return None
    toks = body.split("#")[0].split(":")
    fields = dict(zip(toks[0::2], toks[1::2]))
    try:
        lvl = decode_level_data(fields.get("4", ""))
    except Exception as e:
        log(f"  level {lid}: decode failed {e}")
        return None
    if ";" not in lvl:
        log(f"  level {lid}: no objects in decoded string")
        return None
    out.write_text(lvl, encoding="utf-8")
    (LEVELS / f"{lid}.name").write_text(fields.get("2", ""), encoding="utf-8")
    log(f"  level {lid} '{fields.get('2','')}' {len(lvl)} chars")
    return out


def add(manifest, lid, path, src):
    manifest.add((str(lid), str(path.relative_to(ROOT)).replace("\\", "/"), src))


def gdr_header(path):
    """(levelId, bot, deaths) from a replay header via gdrcheck's parser."""
    exe = ROOT / "gdrcheck.exe"
    p = subprocess.run([str(exe), str(path), "nonexistent"], capture_output=True, timeout=60)
    out = p.stdout.decode("utf-8", "replace")
    m = re.search(r"\(id=(\d+)\)\s+bot=(\S*)", out)
    d = re.search(r"deaths=(\d+)", out)
    return (int(m.group(1)), m.group(2)) if m else (0, ""), (int(d.group(1)) if d else -1)


def source_paul(manifest):
    res = {}
    for line in (ROOT / "testlevel/macrolist/results_2026-08-09.tsv").read_text(encoding="utf-8").splitlines():
        c = line.split("\t")
        if len(c) >= 3 and c[2].isdigit():
            res[c[1]] = int(c[2])
    for line in (ROOT / "testlevel/macrolist/source_links.tsv").read_text(encoding="utf-8").splitlines():
        c = line.split("\t")
        if len(c) < 3 or c[2] == "1":
            continue                             # seed-flagged: RNG level, skipped
        drive, lid = c[1], res.get(c[1])
        if not lid:
            continue
        d = REPLAYS / str(lid)
        d.mkdir(parents=True, exist_ok=True)
        dst = d / f"paul_{drive}.macro"
        if not (dst.exists() and dst.stat().st_size > 16):
            code, _ = curl([f"https://drive.usercontent.google.com/download?id={drive}&export=download&confirm=t"],
                           out=dst, timeout=180)
            head = dst.read_bytes()[:64] if dst.exists() else b""
            if code != 200 or head.lstrip().startswith(b"<"):
                log(f"  drive {drive} (level {lid}): HTTP {code}, not a replay")
                dst.unlink(missing_ok=True)
                time.sleep(3)
                continue
            log(f"  drive {drive} -> level {lid} ({dst.stat().st_size} bytes)")
            time.sleep(2)
        if fetch_level(lid):
            add(manifest, lid, dst, "paul")


def source_eclipse(manifest):
    if not ECLIPSE.exists():
        return
    for f in sorted(ECLIPSE.glob("*.gdr*")):
        # Only real recorded runs: EclipseBot, deaths == 0, a real level id. The
        # Pathfinder / rhythm-game exports are this project's own output, not ground truth.
        (lid, bot), deaths = gdr_header(f)
        if not lid or bot != "EclipseBot" or deaths != 0:
            log(f"  eclipse {f.name}: id={lid} bot={bot} deaths={deaths}, skipped")
            continue
        d = REPLAYS / str(lid)
        d.mkdir(parents=True, exist_ok=True)
        dst = d / ("user_" + re.sub(r"[^A-Za-z0-9_.-]", "_", f.name))
        if not dst.exists():
            shutil.copy2(f, dst)
        if fetch_level(lid):
            add(manifest, lid, dst, "user")


def source_showcase(manifest, max_per_level):
    for lvl in sorted(LEVELS.glob("*.txt")):
        lid = lvl.stem
        h = hashlib.sha256(lvl.read_bytes()).hexdigest()
        code, body = curl([f"{SHOWCASE}/v4/levels/{h}/replays"], timeout=30)
        time.sleep(1.0)
        if code != 200:
            continue
        try:
            j = json.loads(body)
        except Exception:
            continue
        seen, ids = set(), []
        for k in ("top", "recommended", "different", "new"):
            for e in j.get(k, []) or []:
                if e["id"] not in seen:
                    seen.add(e["id"]); ids.append(e["id"])
        if not ids:
            continue
        d = REPLAYS / lid
        d.mkdir(parents=True, exist_ok=True)
        for rid in ids[:max_per_level]:
            dst = d / f"showcase_{rid}.gdr2"
            if not dst.exists():
                c2, _ = curl([f"{SHOWCASE}/v4/levels/{h}/replays/{rid}/data"], out=dst, timeout=60)
                time.sleep(1.0)
                if c2 != 200:
                    dst.unlink(missing_ok=True)
                    continue
            add(manifest, lid, dst, "showcase")
        log(f"  showcase {lid}: {len(ids)} replays listed, kept {min(len(ids), max_per_level)}")


def main():
    LEVELS.mkdir(parents=True, exist_ok=True)
    REPLAYS.mkdir(parents=True, exist_ok=True)
    manifest = set()
    mf = CORPUS / "manifest.tsv"
    if mf.exists():
        for line in mf.read_text(encoding="utf-8").splitlines():
            c = line.split("\t")
            if len(c) == 3 and (ROOT / c[1]).exists():
                manifest.add(tuple(c))
    def save():
        mf.write_text("".join("\t".join(r) + "\n" for r in sorted(manifest)), encoding="utf-8")
    log("== source C: user replays")
    source_eclipse(manifest); save()
    log("== source A: Paul's macro demonlist (no seed levels)")
    source_paul(manifest); save()
    if "--no-showcase" not in sys.argv:
        n = 6
        if "--max-showcase" in sys.argv:
            n = int(sys.argv[sys.argv.index("--max-showcase") + 1])
        log("== source B: showcase archive")
        source_showcase(manifest, n)
    mf.write_text("".join("\t".join(r) + "\n" for r in sorted(manifest)), encoding="utf-8")
    log(f"== done: {len(manifest)} replays over {len({r[0] for r in manifest})} levels")


if __name__ == "__main__":
    main()
