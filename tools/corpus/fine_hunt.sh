#!/usr/bin/env bash
# Fine-grained truth check: first divergence at 0.05u for every recorded real run (bank +
# captures), one line per level. Diff two outputs to see small wins/losses that the 1u
# regression gate cannot see.   ./tools/corpus/fine_hunt.sh [exe] > out.txt
cd "$(dirname "$0")/../.."
EXE="${1:-./divergehunt.exe}"
F="$(ls test/truths/GDMod_truth_*.txt) $(ls testlevel/captures/GDMod_truth_*.txt | grep -v '116174063\|120012581\|76962930')"
for f in $F; do "$EXE" --thresh 0.05 "$f" 2>/dev/null | tail -1 | sed "s|^|$(basename $(dirname $f))/ |"; done
