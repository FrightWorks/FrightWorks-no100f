"""Rebuild a Heavy Iron HIP with extra assets (local mods and experiments: never commit game data).

    python tools/debug/game/hipedit.py FILE.HIP [...]     no-op rebuild of each file: must come out byte identical

As a module:

    from hipedit import Hip, xstrhash
    h = Hip(open(path, "rb").read())
    h.add(0, b"SIMP", xstrhash("MY OBJECT"), data, "MY OBJECT", 0x2)   # layer type, asset type, ID, bytes, name, flags
    open(out, "wb").write(h.build())

Layout: HIPA | PACK(PVER PFLG PCNT PCRT PMOD) | DICT(ATOC(AINF AHDR...) LTOC(LINF LHDR...)) | STRM(DHDR DPAK)
AHDR = ID, type, absolute file offset, size, plus (padding counted after the asset), flags, then ADBG (name).
The ATOC is sorted by ID. Assets sit in the DPAK in LTOC order, each followed by its plus; layers start on 0x20.
Layer types in h002: 0 default, 1 textures, 2 BSP, 3 models, 4 and 8 empty, 6 sounds, 7 sound info.

Adding an asset takes: an AHDR in sorted place, its ID at the end of its layer's list, its bytes at that layer's end,
PCNT's asset count, and every later offset shifted. build() does all of that. A SIMP in layer 0 is created by the scene
from its type; no other table lists it (tried in h002: it draws, collides, and survives level changes, saves and loads).

Disc facts this relies on: every RWTX on the disc has the ID xstrhash(its name), and one with another ID drew untextured
with no error, so materials seem to find textures by that hash. Flags seen: RWTX 0x6, MODL 0x5, SIMP 0x2.
"""
import struct
import sys
import zlib

FILL = b"\x33"  # what the disc pads with


def u32(d, o):
    return struct.unpack(">I", d[o:o+4])[0]


def blk(tag, body):
    return tag + struct.pack(">I", len(body)) + body


def align(x, a):
    return (x + a - 1) & ~(a - 1)


def xstrhash(s):
    """The game's asset ID hash of a name."""
    h = 0
    for c in s.encode("latin1"):
        if 97 <= c <= 122:
            c -= 32
        h = (h * 131 + c) & 0xFFFFFFFF
    return h


def make_adbg(name, data=b""):
    n = name.encode("latin1") + b"\0"
    if len(n) & 1:
        n += b"\0"
    return struct.pack(">I", 0) + n + b"\0\0" + struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)  # empty file name


class Asset:
    def __init__(self, aid, typ, data, plus, flags, adbg, off=None):
        self.id, self.typ, self.data, self.plus, self.flags, self.adbg, self.off = aid, typ, data, plus, flags, adbg, off
        self.start_align = 0  # only set on added assets
        self.rel = 0

    @property
    def name(self):
        return self.adbg[4:].split(b"\0")[0].decode("latin1")


class Hip:
    def __init__(self, d):
        assert d[:4] == b"HIPA", "not a HIP"
        off = 8
        assert d[off:off+4] == b"PACK"
        psz = u32(d, off+4)
        p = off + 8
        self.pack = []
        while p < off + 8 + psz:
            z = u32(d, p+4)
            self.pack.append((d[p:p+4], d[p+8:p+8+z]))
            p += 8 + z
        off = off + 8 + psz
        assert d[off:off+4] == b"DICT"
        p = off + 8
        assert d[p:p+4] == b"ATOC"
        q = p + 8
        aend = q + u32(d, p+4)
        assert d[q:q+4] == b"AINF"
        self.ainf = d[q+8:q+8+u32(d, q+4)]
        q += 8 + u32(d, q+4)
        self.assets = []
        self.by = {}
        while q < aend:
            assert d[q:q+4] == b"AHDR"
            z = u32(d, q+4)
            b = q + 8
            aid, typ, aoff, asize, plus, flags = struct.unpack(">I4sIIII", d[b:b+24])
            assert d[b+24:b+28] == b"ADBG"
            a = Asset(aid, typ, d[aoff:aoff+asize], plus, flags, d[b+32:b+32+u32(d, b+28)], aoff)
            self.assets.append(a)
            self.by[aid] = a
            q = b + z
        p = aend
        assert d[p:p+4] == b"LTOC"
        q = p + 8
        lend = q + u32(d, p+4)
        assert d[q:q+4] == b"LINF"
        self.linf = d[q+8:q+8+u32(d, q+4)]
        q += 8 + u32(d, q+4)
        self.layers = []  # [type, [ids], rest of the LHDR]
        while q < lend:
            assert d[q:q+4] == b"LHDR"
            z = u32(d, q+4)
            b = q + 8
            t, c = struct.unpack(">II", d[b:b+8])
            self.layers.append([t, list(struct.unpack(">%dI" % c, d[b+8:b+8+4*c])), d[b+8+4*c:b+z]])
            q = b + z
        p = lend
        assert d[p:p+4] == b"STRM"
        q = p + 8
        assert d[q:q+4] == b"DHDR"
        self.dhdr = d[q+8:q+8+u32(d, q+4)]
        q += 8 + u32(d, q+4)
        assert d[q:q+4] == b"DPAK"
        first = min(a.off for a in self.assets)
        self.base_align = 0x40 if first % 0x40 == 0 else 0x20  # the disc has both; kept so a rebuild comes out the same

    def add(self, layer_type, typ, aid, data, name, flags, start_align=0x20):
        """Append a new asset at the end of a layer. Disc alignment by type: RWTX 0x40, MODL 0x10, SIMP 0x20."""
        assert aid not in self.by, "ID already used: %08X" % aid
        layer = next(l for l in self.layers if l[0] == layer_type)
        a = Asset(aid, typ, data, 0, flags, make_adbg(name, data))
        a.start_align = start_align
        self.assets.append(a)
        self.by[aid] = a
        layer[1].append(aid)
        self.assets.sort(key=lambda x: x.id)
        return a

    def replace(self, aid, data):
        """Swap the bytes of an existing asset (local experiments; not in launcher/ModPatches/HipFile.cs, which only adds).
        Its ID, name, flags and ADBG stay as they are, so materials still find it. A size change keeps the asset's end on the
        alignment it had (the largest power of two up to 0x40 that its size plus padding divided by); every later offset
        moves in build()."""
        a = self.by[aid]
        total = len(a.data) + a.plus
        if len(data) != len(a.data):
            unit = 1
            while unit < 0x40 and total % (unit * 2) == 0:
                unit *= 2
            a.plus = align(len(data), unit) - len(data)
        a.data = bytes(data)
        return a

    def build(self):
        # Lay the stream out relative to its start. Existing assets keep their plus unless an added asset follows them.
        rel = 0
        for _, ids, _ in self.layers:
            if not ids:
                continue
            rel = align(rel, 0x20)
            for j, i in enumerate(ids):
                a = self.by[i]
                a.rel = rel
                end = rel + len(a.data) + a.plus
                want = self.by[ids[j+1]].start_align if j + 1 < len(ids) else 0
                if want and align(end, want) != end:
                    a.plus += align(end, want) - end
                    end = align(end, want)
                rel = end
        total = align(rel, 0x20)

        def dict_block(base):
            ahdrs = b"".join(blk(b"AHDR", struct.pack(">I4sIIII", a.id, a.typ, base + a.rel, len(a.data), a.plus, a.flags)
                                 + blk(b"ADBG", a.adbg)) for a in self.assets)
            lt = blk(b"LINF", self.linf)
            for t, ids, rest in self.layers:
                lt += blk(b"LHDR", struct.pack(">II", t, len(ids)) + struct.pack(">%dI" % len(ids), *ids) + rest)
            return blk(b"DICT", blk(b"ATOC", blk(b"AINF", self.ainf) + ahdrs) + blk(b"LTOC", lt))

        pack = []
        for t, v in self.pack:
            if t == b"PCNT":  # assets, layers, largest asset, largest layer, largest virtual asset; only ever grown
                c = list(struct.unpack(">5I", v))
                c[0], c[1] = len(self.assets), len(self.layers)
                c[2] = max(c[2], max(len(a.data) for a in self.assets))
                c[3] = max([c[3]] + [sum(len(self.by[i].data) + self.by[i].plus for i in ids) for _, ids, _ in self.layers])
                v = struct.pack(">5I", *c)
            pack.append(blk(t, v))
        head = b"HIPA" + struct.pack(">I", 0) + blk(b"PACK", b"".join(pack))
        # STRM tag + size, DHDR + its bytes, DPAK tag + size, padding size, padding, stream
        pre = len(head) + len(dict_block(0)) + 8 + 8 + len(self.dhdr) + 8 + 4
        base = align(pre, max([self.base_align] + [a.start_align for a in self.assets]))
        stream = bytearray(FILL * total)
        for a in self.assets:
            stream[a.rel:a.rel+len(a.data)] = a.data
        dpak = struct.pack(">I", base - pre) + FILL * (base - pre) + bytes(stream)
        return head + dict_block(base) + blk(b"STRM", blk(b"DHDR", self.dhdr) + blk(b"DPAK", dpak))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bad = 0
    for path in sys.argv[1:]:
        d = open(path, "rb").read()
        o = Hip(d).build()
        if o == d:
            print("identical", len(o), path)
        else:
            bad += 1
            first = next((i for i in range(min(len(o), len(d))) if o[i] != d[i]), min(len(o), len(d)))
            print("DIFFERENT", len(o), len(d), "first at %#x" % first, path)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
