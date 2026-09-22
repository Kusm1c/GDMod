#!/usr/bin/env python3
"""fieldname.py - replace raw byte offsets in a Ghidra decompile snippet with
the real member names, using the compiler-verified table in tools/gd_fields.txt.

Ghidra writes member accesses on a `CCNode *param_1` as `param_1 + 0x9b9`
(CCNode is a 1-byte placeholder struct, so the arithmetic is in bytes).

  fieldname.py <file.c> [class] [--range A B]      # rewrite in place-ish (stdout)
  fieldname.py extract <src.c> <startline> <endline> [class] > out.c
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, "tools", "gd_fields.txt")


def load(cls_filter=None):
    off = {}
    with open(TABLE, "r", encoding="utf-8-sig") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 3:
                continue
            cls, o, name = parts
            if cls_filter and cls != cls_filter:
                continue
            off[int(o, 16)] = name
    return off


def annotate(text, off):
    # `param_1 + 0x9b9`  /  `param_1 + 0x9a0`  /  `(longlong)param_1 + 0x7c8`
    def sub_plus(m):
        v = int(m.group(2), 16)
        n = off.get(v)
        return m.group(0) if n is None else "%s + /*%s*/ %s" % (
            m.group(1), n, m.group(2))

    text = re.sub(r"(param_1|this|puVar\d+|pCVar\d+|lVar\d+)\s*\+\s*(0x[0-9a-f]{2,4})\b",
                  sub_plus, text)

    # `param_1[0x9b9]`
    def sub_idx(m):
        v = int(m.group(2), 16)
        n = off.get(v)
        return m.group(0) if n is None else "%s[/*%s*/ %s]" % (
            m.group(1), n, m.group(2))

    text = re.sub(r"(param_1|this)\[(0x[0-9a-f]{2,4})\]", sub_idx, text)
    return text


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "extract":
        src, a, b = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
        cls = sys.argv[5] if len(sys.argv) > 5 else "PlayerObject"
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()[a - 1:b]
        sys.stdout.write(annotate("".join(lines), load(cls)))
        return 0
    src = sys.argv[1]
    cls = sys.argv[2] if len(sys.argv) > 2 else "PlayerObject"
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        sys.stdout.write(annotate(f.read(), load(cls)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
