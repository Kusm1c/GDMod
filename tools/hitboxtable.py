#!/usr/bin/env python3
"""hitboxtable.py - extract GD's per-object-id hitbox table from the binary.

GameObject::setupSpriteSize (0x1401a36a0) assigns m_width (0x2fc) and m_height
(0x300) per object id. It is a chain of ranges:

  * ids 1..0x1d7 go through a BYTE jump table in .rdata (`switch (bVar1)` where
    `bVar1 = table[id - 1]`), so the case labels there are case INDICES.
  * every later range switches on the object id directly, so its case labels ARE
    object ids.

This reads the byte table straight out of GeometryDash.exe and parses the switch
bodies out of the Ghidra export, resolving `goto switchD_..._caseD_XX` aliases,
and prints `id width height`.

  hitboxtable.py <sliced-setupSpriteSize.c> <byteTableVA> <count>
"""
import re
import struct
import sys

EXE = r"C:\Users\Kusmic\Documents\GeodeModing\GeometryDash.exe"


def pe_reader(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    secs = []
    for i in range(nsec):
        b = pe + 24 + optsz + i * 40
        vsz, va, rsz, rp = struct.unpack_from("<IIII", d, b + 8)
        secs.append((va, vsz, rp, rsz))

    def rd(vaddr, n):
        rva = vaddr - base
        for va, vsz, rp, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                off = rp + (rva - va)
                return d[off:off + n]
        return None
    return rd


def f32(hexstr):
    return struct.unpack("<f", struct.pack("<I", int(hexstr, 16)))[0]


def main():
    src, tableVA, count = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
    lines = open(src, encoding="utf-8", errors="replace").readlines()

    rd = pe_reader(EXE)
    idxTable = rd(tableVA, count)          # id-1 -> case index (first range only)

    RE_CASE = re.compile(r"^\s*case (-?\w+):")
    RE_LABEL = re.compile(r"^\s*(switchD_\w+_caseD_[0-9a-f]+):")
    RE_W = re.compile(r"\(param_1 \+ 0x2fc\) = (0x[0-9a-f]+);")
    RE_H = re.compile(r"\(param_1 \+ 0x300\) = (0x[0-9a-f]+);")
    RE_GOTO = re.compile(r"goto (switchD_\w+_caseD_[0-9a-f]+);")
    RE_SWITCH = re.compile(r"switch\((\w+)\)")

    # label/case -> (w,h) or an alias to another label
    size_of = {}        # key -> (w,h)
    alias_of = {}       # key -> label
    pending = []        # keys awaiting a body
    w = h = None
    curswitch = None
    # keys are ("idx", n) for the byte-table switch and ("id", n) elsewhere
    mode = "idx"

    for ln in lines:
        ms = RE_SWITCH.search(ln)
        if ms:
            curswitch = ms.group(1)
            mode = "idx" if curswitch == "bVar1" else "id"
        ml = RE_LABEL.match(ln)
        if ml:
            pending.append(("lbl", ml.group(1)))
        mc = RE_CASE.match(ln)
        if mc:
            tok = mc.group(1)
            try:
                n = int(tok, 0)
            except ValueError:
                continue
            pending.append((mode, n))
        mg = RE_GOTO.search(ln)
        if mg and pending:
            for k in pending:
                alias_of[k] = ("lbl", mg.group(1))
            pending = []
            w = h = None
            continue
        mw = RE_W.search(ln)
        if mw:
            w = f32(mw.group(1))
        mh = RE_H.search(ln)
        if mh:
            h = f32(mh.group(1))
        if ("return;" in ln or "break;" in ln) and pending:
            # A case that ends WITHOUT assigning anything keeps whatever
            # commonSetup left in m_width/m_height — it is not "the next case's
            # values". Dropping its labels here instead of letting them ride is
            # the difference between id 1333/1704/1755 reading as 25x20 (the next
            # case's numbers) and correctly reading as "not set here".
            if w is not None or h is not None:
                for k in pending:
                    size_of[k] = (w, h)
            pending = []
            w = h = None

    def resolve(k, depth=0):
        if depth > 8:
            return None
        if k in size_of:
            return size_of[k]
        if k in alias_of:
            return resolve(alias_of[k], depth + 1)
        return None

    out = {}
    for i in range(count):
        oid = i + 1
        r = resolve(("idx", idxTable[i]))
        if r:
            out[oid] = r
    for k in list(size_of) + list(alias_of):
        if k[0] == "id":
            r = resolve(k)
            if r:
                out[k[1]] = r

    print("# id width height   (GameObject::setupSpriteSize, from the binary)")
    for oid in sorted(out):
        wv, hv = out[oid]
        print("%d %s %s" % (oid,
                            "?" if wv is None else ("%g" % wv),
                            "?" if hv is None else ("%g" % hv)))


if __name__ == "__main__":
    main()
