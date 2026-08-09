#!/usr/bin/env bash
# One-command build + install + freshness verification for the GDMod mod.
#
#   ./deploy.sh
#
# Why this exists: MSBuild's incremental build has twice silently SKIPPED recompiling
# changed src/sim/*.cpp — it still prints "Successfully packaged" and "Installed" while
# the .geode wraps a STALE .dll (old .obj files), so in-game nothing changes even though
# the build "succeeded". This script force-touches the sim sources, builds, then HARD
# VERIFIES the .dll is newer than every source file and the installed .geode matches the
# build. It exits non-zero (loud) if the DLL is stale — so a stale deploy can't slip by.
#
# GD must be CLOSED (a running GD locks the .geode / .dll).
set -u
cd "$(dirname "$0")"

INSTALL="/c/Users/Kusmic/Documents/GeodeModing/geode/mods/kusmic.pathfinder.geode"
DLL="build/RelWithDebInfo/kusmic.pathfinder.dll"
GEODE="build/kusmic.pathfinder.geode"

# 0. Refuse to build while GD holds the file locks.
if tasklist 2>/dev/null | grep -qi "GeometryDash"; then
    echo "✗ Geometry Dash is RUNNING — close it first (it locks the .geode/.dll)."; exit 2
fi

# 1. Force every sim source to look newer than its .obj so MSBuild MUST recompile it.
touch src/sim/*.cpp src/sim/*.hpp src/*.cpp 2>/dev/null

# 2. Build (auto-installs to the GeodeModing mods dir on success).
echo "→ building (RelWithDebInfo)…"
if ! cmake --build build --config RelWithDebInfo 2>&1 | grep -iE "error|\.dll|packaged|Installed" | grep -viE "warning|MSB8027"; then :; fi

# 3. HARD freshness check: the .dll must be newer than every source file.
if [ ! -f "$DLL" ]; then echo "✗ no DLL produced ($DLL)"; exit 1; fi
newest_src=$(ls -t src/sim/*.cpp src/sim/*.hpp src/*.cpp 2>/dev/null | head -1)
if [ "$newest_src" -nt "$DLL" ]; then
    echo "✗ STALE DLL: $newest_src is newer than $DLL — MSBuild skipped a recompile."
    echo "  DLL:  $(stat -c '%y' "$DLL" | cut -d. -f1)"
    echo "  src:  $(stat -c '%y' "$newest_src" | cut -d. -f1)  ($newest_src)"
    exit 1
fi

# 4. The installed .geode must match the freshly-built one.
if [ ! -f "$INSTALL" ]; then echo "✗ not installed at $INSTALL"; exit 1; fi
if [ "$GEODE" -nt "$INSTALL" ]; then
    echo "→ installed copy is older than the build — copying…"
    cp "$GEODE" "$INSTALL" || { echo "✗ copy failed (GD running?)"; exit 1; }
fi

echo "✓ DEPLOYED — DLL $(stat -c '%y' "$DLL" | cut -d. -f1)"
echo "  installed: $(stat -c '%y' "$INSTALL" | cut -d. -f1)  $INSTALL"
