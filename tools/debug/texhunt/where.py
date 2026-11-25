# where.py [IDs...] [--all]: where a suspect's texture is used, to help find a spot for it.
# Lists the assets in each of the suspect's scenes that name the texture. For level geometry (BSP) it also prints the
# world position of the triangles, grouped (centre, size, height), so you can `tp X Y Z` there with the dev menu.
# Models (MODL) only get their asset name: where they're placed isn't read here. Default: every suspect still open.
import csv, glob, os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import gxtex

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")  # repo root
DATA = os.path.join(ROOT, "autotest", "texhunt")  # git-ignored work data and output
HIPS = {os.path.splitext(os.path.basename(p))[0].lower(): p
        for p in glob.glob(os.path.join(ROOT, "Disc", "DATA", "files", "**", "*.HIP"), recursive=True)}
_hip_cache = {}

def hip(scene):
    if scene not in _hip_cache:
        data = open(HIPS[scene], "rb").read()
        _hip_cache[scene] = (data, gxtex.hip_assets(data))
    return _hip_cache[scene]

def world_triangles(b, tex):
    """Triangles (3 vertices each) of an RW world (BSP asset) whose material uses texture `tex`."""
    mats, tris = [], []
    def walk(off, end):
        for t, o, s in gxtex.rw_chunks(b, off, end):
            if t == 0x07:  # material: texture name = first string under its texture chunk
                names = [b[o2:o2+s2].split(b"\0")[0].decode("latin1")
                         for t1, o1, s1 in gxtex.rw_chunks(b, o, o+s) if t1 == 0x06
                         for t2, o2, s2 in gxtex.rw_chunks(b, o1, o1+s1) if t2 == 0x02]
                mats.append(names[0] if names else "")
            elif t == 0x09:  # atomic sector: 44-byte header, vertices/normals/prelight/UVs (28 bytes each), triangles
                o1, s1 = next((o1, s1) for t1, o1, s1 in gxtex.rw_chunks(b, o, o+s) if t1 == 0x01)
                base, nt, nv = struct.unpack_from("<III", b, o1)
                if nv == 0 or (s1 - 44 - nt * 8) // nv != 28: continue
                to = o1 + 44 + nv * 28
                for i in range(nt):
                    m, *vi = struct.unpack_from("<4H", b, to + 8 * i)  # matId first, then the 3 vertex indices
                    if mats[m + base].lower() == tex:
                        tris.append([struct.unpack_from("<3f", b, o1 + 44 + 12 * j) for j in vi])
            elif t in (0x0B, 0x08, 0x0A):
                walk(o, o + s)
    walk(0, len(b))
    return tris

def groups(tris, radius=3.0):
    out = []
    for tri in tris:
        c = [sum(v[k] for v in tri) / 3 for k in range(3)]
        for g in out:
            if sum((g["c"][k] - c[k]) ** 2 for k in range(3)) < radius ** 2: g["t"].append(tri); break
        else: out.append({"c": c, "t": [tri]})
    for g in out:
        vs = [v for t in g["t"] for v in t]
        g["lo"] = [min(v[k] for v in vs) for k in range(3)]
        g["hi"] = [max(v[k] for v in vs) for k in range(3)]
    return sorted(out, key=lambda g: -len(g["t"]))

def where(row, max_groups=8):
    tex = re.sub(r"\.rw3$", "", row["asset"], flags=re.I).lower()
    pat = re.compile(rb"(?<![A-Za-z0-9_])" + re.escape(tex.encode()) + rb"\x00", re.I)
    lines = []
    for scene in row["scenes"].split():
        if scene not in HIPS: lines.append(f"  {scene}: no HIP"); continue
        data, assets = hip(scene)
        for aid, typ, aoff, asz, name in assets:
            if typ == "RWTX" or not pat.search(data[aoff:aoff+asz]): continue
            if typ == "BSP ":
                gs = groups(world_triangles(data[aoff:aoff+asz], tex))
                lines.append(f"  {scene} BSP {name}: {sum(len(g['t']) for g in gs)} tris in {len(gs)} places")
                for g in gs[:max_groups]:
                    c = [(g["lo"][k] + g["hi"][k]) / 2 for k in range(3)]
                    size = [g["hi"][k] - g["lo"][k] for k in range(3)]
                    lines.append(f"    tp {c[0]:.1f} {g['lo'][1] + 0.5:.1f} {c[2]:.1f}   ({len(g['t'])} tris, "
                                 f"{size[0]:.1f} x {size[2]:.1f} wide, y {g['lo'][1]:.1f}..{g['hi'][1]:.1f})")
                if len(gs) > max_groups: lines.append(f"    ... {len(gs) - max_groups} more")
            else:
                lines.append(f"  {scene} {typ.strip()} {name}")
    return tex, lines

if __name__ == "__main__":
    rows = list(csv.DictReader(open(os.path.join(DATA, "suspects.csv"), encoding="utf-8")))
    ids = [a for a in sys.argv[1:] if not a.startswith("--")]
    pick = [r for r in rows if r["id"] in ids] if ids else \
           rows if "--all" in sys.argv else [r for r in rows if r["status"] == "suspect"]
    for r in pick:
        tex, lines = where(r)
        print(f"{r['id']} {tex} ({r['level']}; {r['status']})")
        print("\n".join(lines) if lines else "  not named by any asset in its scenes")
