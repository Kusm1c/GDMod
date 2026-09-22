#!/usr/bin/env python3
"""bromamap.py - turn the raw Ghidra decompile into a NAMED decompile.

The Ghidra export of GeometryDash.exe names every function `FUN_<va>`, which
makes it unreadable. The Geode bindings (`bindings-main/bindings/2.2081/*.bro`)
carry the real `Class::method = win 0x<rva>` table for exactly this binary.

  va = 0x140000000 + rva     (standard Windows x64 preferred image base)

Usage:
  bromamap.py map                 -> writes tools/gd_symbols.txt (va<TAB>name)
  bromamap.py annotate <in.c> <out.c>
                                  -> rewrites FUN_<va> to <Class__method__va>
  bromamap.py find <regex>        -> print symbols whose name matches
  bromamap.py at <va-or-FUN_...>  -> print the symbol at an address
  bromamap.py fields <Class>      -> print computed member byte offsets

`annotate` keeps the address inside the new identifier so the file stays
greppable both ways (by name and by original address).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINDINGS = os.path.join(ROOT, "bindings-main", "bindings", "2.2081")
SYMFILE = os.path.join(ROOT, "tools", "gd_symbols.txt")
IMAGE_BASE = 0x140000000

# ---------------------------------------------------------------- broma parse

# `... name(args) = win 0x123456, imac 0x...;`  (win may be `inline`)
RE_ADDR = re.compile(r"=\s*(?:[a-z0-9]+\s+(?:0x[0-9a-fA-F]+|inline)\s*,\s*)*"
                     r"win\s+(0x[0-9a-fA-F]+)")
RE_CLASS = re.compile(r"^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)")
# member declaration: `Type m_name;` possibly templated / namespaced
RE_MEMBER = re.compile(r"^\s*(.+?)\s+(m_[A-Za-z0-9_]+)\s*;\s*(?://.*)?$")
RE_PAD = re.compile(r"^\s*PAD\s*=\s*(.+?);")


def _fn_name(line):
    """Extract a callable name from a broma declaration line."""
    # strip the ` = win ...` tail and any trailing body
    head = line.split("=")[0].strip()
    head = head.rstrip("{").strip()
    # find `name(` - the last identifier directly before an open paren at depth 0
    depth = 0
    for i in range(len(head) - 1, -1, -1):
        c = head[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                j = i
                k = j
                while k > 0 and (head[k - 1].isalnum() or head[k - 1] in "_~"):
                    k -= 1
                nm = head[k:j]
                return nm if nm else None
    return None


def parse_symbols():
    """-> {va:int -> 'Class::method'}"""
    syms = {}
    for fname in sorted(os.listdir(BINDINGS)):
        if not fname.endswith(".bro"):
            continue
        path = os.path.join(BINDINGS, fname)
        cls = None
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                mc = RE_CLASS.match(line)
                if mc:
                    cls = mc.group(1)
                    continue
                if line.startswith("}"):
                    cls = None
                ma = RE_ADDR.search(line)
                if not ma:
                    continue
                nm = _fn_name(line)
                if not nm:
                    continue
                va = IMAGE_BASE + int(ma.group(1), 16)
                full = ("%s::%s" % (cls, nm)) if cls else nm
                # first writer wins, but prefer a class-qualified name
                if va not in syms or (syms[va].find("::") < 0 and "::" in full):
                    syms[va] = full
    return syms


# --------------------------------------------------------------- field sizes

# Windows x64 MSVC sizes for the types broma actually uses in member decls.
SIZES = {
    "bool": 1, "char": 1, "unsigned char": 1, "signed char": 1,
    "short": 2, "unsigned short": 2, "uint16_t": 2, "int16_t": 2,
    "int": 4, "unsigned int": 4, "uint32_t": 4, "int32_t": 4, "float": 4,
    "long": 4, "unsigned long": 4,
    "double": 8, "long long": 8, "unsigned long long": 8,
    "int64_t": 8, "uint64_t": 8, "size_t": 8, "intptr_t": 8, "uintptr_t": 8,
    "cocos2d::CCPoint": 8, "cocos2d::CCSize": 8, "cocos2d::CCRect": 16,
    "cocos2d::ccColor3B": 3, "cocos2d::ccColor4B": 4, "cocos2d::ccColor4F": 16,
    "cocos2d::CCAffineTransform": 24,
    "std::string": 32, "gd::string": 32,
}
ALIGN = {1: 1, 2: 2, 3: 1, 4: 4, 8: 8, 16: 4, 24: 4, 32: 8}


def _type_size(t):
    t = t.strip()
    if t.endswith("*") or t.startswith("std::function") or t.startswith("gd::function"):
        return 8, 8
    if t in SIZES:
        s = SIZES[t]
        return s, ALIGN.get(s, min(s, 8))
    m = re.match(r"std::array<\s*(.+?)\s*,\s*(\d+)\s*>$", t)
    if m:
        es, ea = _type_size(m.group(1))
        return es * int(m.group(2)), ea
    if t.startswith(("std::vector", "gd::vector")):
        return 24, 8
    if t.startswith(("std::map", "gd::map", "std::set", "gd::set")):
        return 16, 8
    if t.startswith(("std::unordered_map", "gd::unordered_map",
                     "std::unordered_set", "gd::unordered_set")):
        return 64, 8
    if t.startswith("std::pair"):
        return None, None
    return None, None  # unknown -> stop computing


def _pad_size(spec):
    """PAD = win 0x20, android 0x8;  -> the win amount, else None."""
    for part in spec.split(","):
        part = part.strip()
        m = re.match(r"win\s+(0x[0-9a-fA-F]+|\d+)", part)
        if m:
            return int(m.group(1), 0)
    return None


def parse_fields(target_cls, start_offset=0):
    """Compute member offsets for one class body (relative to start_offset)."""
    path = os.path.join(BINDINGS, "GeometryDash.bro")
    out = []
    off = start_offset
    inside = False
    stopped = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            mc = RE_CLASS.match(line)
            if mc:
                inside = (mc.group(1) == target_cls)
                continue
            if not inside:
                continue
            if line.startswith("}"):
                break
            if stopped:
                continue
            mp = RE_PAD.match(line)
            if mp:
                n = _pad_size(mp.group(1))
                if n is None:
                    stopped = "PAD with no win size: " + line.strip()
                    continue
                off += n
                continue
            mm = RE_MEMBER.match(line)
            if not mm:
                continue
            ty, nm = mm.group(1).strip(), mm.group(2)
            if ty.startswith(("class ", "struct ", "return", "//")):
                continue
            sz, al = _type_size(ty)
            if sz is None:
                stopped = "unknown type %r for %s" % (ty, nm)
                continue
            if off % al:
                off += al - (off % al)
            out.append((off, nm, ty, sz))
            off += sz
    return out, stopped


# --------------------------------------------------------------------- cli

def load_syms():
    if os.path.exists(SYMFILE):
        syms = {}
        with open(SYMFILE, "r", encoding="utf-8") as f:
            for line in f:
                a, _, n = line.rstrip("\n").partition("\t")
                if n:
                    syms[int(a, 16)] = n
        return syms
    return parse_symbols()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]

    if cmd == "map":
        syms = parse_symbols()
        os.makedirs(os.path.dirname(SYMFILE), exist_ok=True)
        with open(SYMFILE, "w", encoding="utf-8") as f:
            for va in sorted(syms):
                f.write("%X\t%s\n" % (va, syms[va]))
        print("wrote %s (%d symbols)" % (SYMFILE, len(syms)))
        return 0

    if cmd == "annotate":
        src, dst = sys.argv[2], sys.argv[3]
        syms = load_syms()
        pat = re.compile(r"\bFUN_([0-9a-fA-F]{9,16})\b")
        hits = [0]

        def sub(m):
            va = int(m.group(1), 16)
            nm = syms.get(va)
            if not nm:
                return m.group(0)
            hits[0] += 1
            return "%s__%s" % (nm.replace("::", "__"), m.group(1))

        n_in = n_out = 0
        with open(src, "r", encoding="utf-8", errors="replace") as fi, \
             open(dst, "w", encoding="utf-8") as fo:
            for line in fi:
                n_in += 1
                fo.write(pat.sub(sub, line))
        print("annotated %s -> %s (%d lines, %d FUN_ refs renamed)"
              % (src, dst, n_in, hits[0]))
        return 0

    if cmd == "find":
        rx = re.compile(sys.argv[2], re.I)
        syms = load_syms()
        for va in sorted(syms):
            if rx.search(syms[va]):
                print("%X  FUN_%x  %s" % (va, va, syms[va]))
        return 0

    if cmd == "at":
        a = sys.argv[2]
        a = a[4:] if a.upper().startswith("FUN_") else a
        va = int(a, 16)
        syms = load_syms()
        print(syms.get(va, "<no symbol>"))
        return 0

    if cmd == "fields":
        cls = sys.argv[2]
        start = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
        rows, stopped = parse_fields(cls, start)
        for off, nm, ty, sz in rows:
            print("0x%-5x %-4d %-34s %s" % (off, sz, nm, ty))
        if stopped:
            print("\n[STOPPED] %s" % stopped, file=sys.stderr)
        return 0

    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
