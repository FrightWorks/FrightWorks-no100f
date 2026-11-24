"""Copy the PS2 version's live cutscenes (CSN assets) into the GameCube levels, as a local mod.

    python tools/debug/game/ps2_cutscenes.py PS2.iso GC_FILES_DIR OUT_FILES_DIR [--only p004]

PS2.iso        the player's own PS2 disc image (USA, SLUS_203.49)
GC_FILES_DIR   the GameCube disc's files folder (Disc\\DATA\\files)
OUT_FILES_DIR  where the rebuilt levels go, mirroring files\\ (e.g. <UserData>\\mods\\ps2-cutscenes\\files)

Writes only levels that gain a cutscene. Experiment tool: its output is the player's disc data, so never commit or
share it (same rule as the launcher's mods\\!patched).

Why this works: the GameCube levels kept everything around each cutscene (the CSNM manager, the cutscene_toc that
lists each CSN by ID with a copy of its header, the soundtrack SNDS) and an empty layer 8, which is where the PS2 keeps
its CSN assets. Only the CSN assets themselves were dropped. Each one goes back into layer 8 with the PS2's flags, and
only when the GameCube level's cutscene_toc names its ID with the same header. The PS2 starts each on a 2048-byte
sector; the GameCube streams its SNDS music from 32-byte-aligned offsets, so this keeps hipedit's 32-byte layer start.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hipedit import Hip  # noqa: E402

SECTOR = 2048
CUTSCENE_LAYER = 8


class Iso9660:
    """Just enough ISO 9660 to read files by path (the PS2 disc has no Joliet names that matter)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read(16 * SECTOR, SECTOR)
        assert pvd[0] == 1 and pvd[1:6] == b"CD001", "not an ISO 9660 image"
        self.root = self._record(pvd[156:156+34])

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    @staticmethod
    def _record(r):
        lba, size = struct.unpack("<I", r[2:6])[0], struct.unpack("<I", r[10:14])[0]
        flags, nlen = r[25], r[32]
        name = r[33:33+nlen].decode("latin1").split(";")[0]
        return name, lba, size, bool(flags & 2)

    def listdir(self, rec):
        _, lba, size, _ = rec
        d = self.read(lba * SECTOR, size)
        out, i = [], 0
        while i < len(d):
            n = d[i]
            if n == 0:  # records don't cross sectors
                i = (i // SECTOR + 1) * SECTOR
                continue
            r = self._record(d[i:i+n])
            if r[0] not in ("\0", "\1"):
                out.append(r)
            i += n
        return out

    def walk(self, rec=None, prefix=""):
        for r in self.listdir(rec or self.root):
            p = prefix + r[0]
            if r[3]:
                yield from self.walk(r, p + "/")
            else:
                yield p, r

    def readfile(self, rec):
        return self.read(rec[1] * SECTOR, rec[2])


def ctoc_headers(hip):
    """CSN ID -> the header copy the GameCube cutscene_toc keeps for it (little-endian, 'CTSN' magic)."""
    out = {}
    for a in hip.assets:
        if a.typ != b"CTOC":
            continue
        d = a.data
        i = d.find(b"CTSN")
        while i >= 0:
            out[struct.unpack("<I", d[i+4:i+8])[0]] = (d, i)
            i = d.find(b"CTSN", i + 4)
    return out


def header_matches(toc, csn):
    """The CTOC entry is a copy of the CSN's first bytes; compare up to the next entry or the end."""
    d, i = toc
    j = d.find(b"CTSN", i + 4)
    n = (j if j >= 0 else len(d)) - i
    n = min(n, len(csn), 0x40)  # fixed header + soundtrack name; later bytes are per-entry tables
    return d[i:i+n] == csn[:n]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("iso")
    ap.add_argument("gc_files")
    ap.add_argument("out_files")
    ap.add_argument("--only", action="append", help="level name, e.g. p004 (repeatable)")
    args = ap.parse_args()

    iso = Iso9660(args.iso)
    gc_index = {}
    for dp, _, fn in os.walk(args.gc_files):
        for f in fn:
            if f.upper().endswith(".HIP"):
                gc_index[f.upper()] = os.path.join(dp, f)

    wrote = 0
    for path, rec in sorted(iso.walk()):
        name = path.split("/")[-1].upper()
        if not name.endswith(".HIP") or name not in gc_index:
            continue
        level = name[:-4].lower()
        if args.only and level not in args.only:
            continue
        raw = iso.readfile(rec)
        if b"CSN " not in raw:  # cheap skip before parsing
            continue
        ps2 = Hip(raw)
        csns = [a for a in ps2.assets if a.typ == b"CSN "]
        if not csns:
            continue
        gc_path = gc_index[name]
        gc = Hip(open(gc_path, "rb").read())
        tocs = ctoc_headers(gc)
        if not any(l[0] == CUTSCENE_LAYER for l in gc.layers):
            print("skip %s: no layer %d" % (level, CUTSCENE_LAYER))
            continue
        added = []
        for a in csns:
            if a.id in gc.by:
                print("skip %s %s: already there" % (level, a.name))
                continue
            if a.id not in tocs:
                print("skip %s %s: the GameCube cutscene_toc doesn't list %08X" % (level, a.name, a.id))
                continue
            if not header_matches(tocs[a.id], a.data):
                print("skip %s %s: header differs from the GameCube cutscene_toc" % (level, a.name))
                continue
            gc.add(CUTSCENE_LAYER, b"CSN ", a.id, a.data, a.name, a.flags)
            added.append("%s (%d KB)" % (a.name, len(a.data) // 1024))
        if not added:
            continue
        rel = os.path.relpath(gc_path, args.gc_files)
        out = os.path.join(args.out_files, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        data = gc.build()
        # Round trip: the rebuilt level must parse and hold every added asset at its claimed offset.
        check = Hip(data)
        for a in csns:
            if a.id in check.by:
                assert check.by[a.id].data == a.data and check.by[a.id].off % 0x20 == 0
        with open(out, "wb") as f:
            f.write(data)
        wrote += 1
        print("%s: %s -> %s" % (level, ", ".join(added), out))
    print("levels written: %d" % wrote)


if __name__ == "__main__":
    main()
