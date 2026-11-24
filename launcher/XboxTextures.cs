using System.Buffers.Binary;
using System.IO.Compression;
using FrightWorks.ModPatches;

namespace FrightWorks;

// The Xbox version's textures, from the player's own Xbox disc image. They are the GameCube's textures at the same size
// (three are bigger), but stored without loss: 8-bit palettes or 32-bit colour with full alpha, where the GameCube
// mostly has CMPR (DXT1: 4x4 blocks of two colours, on/off alpha) and RGB5A3 (3-bit alpha). Each level's texture
// dictionaries (RWTX) carry the same asset IDs on both discs, so this pairs them up, decodes the Xbox texture and writes
// it as a Dolphin-style replacement named after the GameCube texture (tex1_WxH_hash[_tlut]_fmt.png, the name the
// game's texture replacement looks for) into <UserData>/xbox/textures. The game indexes that folder after the HD pack
// (wiicompiled/runtime/src/main.cpp), so the HD pack wins where it has a texture.
// The copies are the player's disc data, like the extracted disc: never commit, share or ship them.
// No window code here, so tools/hip-patch-tests can run it.
static class XboxTextures
{
    public static string Dir(string userData) => Path.Combine(userData, "xbox", "textures");

    public static int Count(string userData)
    {
        try { return Directory.Exists(Dir(userData)) ? Directory.GetFiles(Dir(userData), "*.png").Length : 0; }
        catch (Exception) { return 0; }
    }

    public static void Remove(string userData) => TryDelete(Dir(userData));

    // Writes the textures; on any problem the folder is left as it was. progress: (levels done, levels to do, name).
    // Returns how many were written.
    public static int Copy(Ps2Cutscenes.IDiscImage xbox, string gcDisc, string userData, Action<int, int, string>? progress = null,
                           CancellationToken cancel = default)
    {
        var xboxLevels = new Dictionary<string, Ps2Cutscenes.DiscEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in xbox.Files())
            if (e.Name.EndsWith(".HIP", StringComparison.OrdinalIgnoreCase)) xboxLevels[e.Name] = e;
        var gcFiles = Path.Combine(gcDisc, "files");
        var gcLevels = Directory.EnumerateFiles(gcFiles, "*.hip", new EnumerationOptions { RecurseSubdirectories = true, MatchCasing = MatchCasing.CaseInsensitive })
                                .Where(p => xboxLevels.ContainsKey(Path.GetFileName(p))).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToList();
        var dir = Dir(userData);
        var staging = dir + ".new";
        TryDelete(staging);
        Directory.CreateDirectory(staging);
        var written = new HashSet<string>(StringComparer.Ordinal);
        int done = 0;
        try
        {
            foreach (var gcPath in gcLevels)
            {
                cancel.ThrowIfCancellationRequested();
                var name = Path.GetFileName(gcPath);
                progress?.Invoke(done++, gcLevels.Count, name);
                HipFile gc, xb;
                try { gc = new HipFile(File.ReadAllBytes(gcPath)); xb = new HipFile(xbox.Read(xboxLevels[name])); }
                catch (InvalidDataException) { continue; }
                var xboxDicts = new Dictionary<uint, ReadOnlyMemory<byte>>();
                foreach (var a in xb.Assets) if (a.TypeName == "RWTX") xboxDicts[a.Id] = a.Data;
                foreach (var a in gc.Assets)
                {
                    if (a.TypeName != "RWTX" || !xboxDicts.TryGetValue(a.Id, out var xd)) continue;
                    var gcTextures = GcTexture.Read(a.Data.Span);
                    var xboxTextures = XboxTexture.Read(xd.Span);
                    for (int i = 0; i < Math.Min(gcTextures.Count, xboxTextures.Count); i++)
                    {
                        var (g, x) = (gcTextures[i], xboxTextures[i]);
                        if (x.Rgba == null) continue;                              // a format this doesn't decode (DXT)
                        if (x.Width < g.Width || x.Height < g.Height) continue;    // smaller on the Xbox: keep the GameCube's
                        if ((long)x.Width * g.Height != (long)x.Height * g.Width) continue;
                        if (!x.HasAlpha && g.MaySeeThrough) continue;           // the Xbox one would cover what shows through
                        var key = g.Key;
                        if (!written.Add(key)) continue;
                        File.WriteAllBytes(Path.Combine(staging, key + ".png"), Png(x.Width, x.Height, x.Rgba));
                    }
                }
            }
            progress?.Invoke(done, gcLevels.Count, "");
            if (written.Count == 0) { TryDelete(staging); return 0; }
            TryDelete(dir);
            Directory.CreateDirectory(Path.GetDirectoryName(dir)!);
            Directory.Move(staging, dir);
            return written.Count;
        }
        catch (Exception) { TryDelete(staging); throw; }
    }

    // RenderWare chunks: type, size, version, all little-endian on every platform.
    static IEnumerable<(uint Type, int Start, int Size)> Chunks(ReadOnlySpan<byte> b, int start, int end)
    {
        var list = new List<(uint, int, int)>();
        while (start + 12 <= end)
        {
            uint type = BinaryPrimitives.ReadUInt32LittleEndian(b[start..]);
            int size = (int)Math.Min(BinaryPrimitives.ReadUInt32LittleEndian(b[(start + 4)..]), (uint)(end - start - 12));
            list.Add((type, start + 12, size));
            start += 12 + size;
        }
        return list;
    }

    // The struct chunk of each texture native (0x15) in a texture dictionary (0x16).
    static List<byte[]> TextureStructs(ReadOnlySpan<byte> b)
    {
        var structs = new List<byte[]>();
        foreach (var (t, o, s) in Chunks(b, 0, b.Length))
        {
            if (t != 0x16) continue;
            foreach (var (t2, o2, s2) in Chunks(b, o, o + s))
            {
                if (t2 != 0x15) continue;
                foreach (var (t3, o3, s3) in Chunks(b, o2, o2 + s2))
                {
                    if (t3 == 1) structs.Add(b.Slice(o3, s3).ToArray());
                    break;
                }
            }
        }
        return structs;
    }

    // A GameCube texture: its replacement name (the hash of its base level and, for palette textures, of the palette
    // entries the indices use) and whether it can have see-through pixels.
    internal sealed record GcTexture(int Width, int Height, int Format, string Key, bool MaySeeThrough)
    {
        // GX formats: block width, height, bits per pixel.
        static readonly Dictionary<int, (int W, int H, int Bpp)> Blocks = new()
        {
            [0] = (8, 8, 4), [1] = (8, 4, 8), [2] = (8, 8, 4), [3] = (4, 4, 16), [4] = (4, 4, 16), [5] = (4, 4, 16),
            [6] = (4, 4, 32), [8] = (8, 8, 4), [9] = (8, 4, 8), [10] = (4, 4, 16), [14] = (8, 8, 4),
        };

        public static List<GcTexture> Read(ReadOnlySpan<byte> rwtx)
        {
            var list = new List<GcTexture>();
            foreach (var h in TextureStructs(rwtx))
            {
                if (h.Length < 0x6C) continue;
                int w = BinaryPrimitives.ReadUInt16BigEndian(h.AsSpan(0x5C)), hh = BinaryPrimitives.ReadUInt16BigEndian(h.AsSpan(0x5E));
                int fmt = h[0x62], tlutFormat = h[0x63];
                if (!Blocks.TryGetValue(fmt, out var blk) || w == 0 || hh == 0) continue;
                int size = (w + blk.W - 1) / blk.W * ((hh + blk.H - 1) / blk.H) * blk.W * blk.H * blk.Bpp / 8;
                string key;
                bool maySeeThrough;
                if (fmt is 8 or 9)
                {
                    int n = fmt == 8 ? 16 : 256;
                    int data = 0x68 + 2 * n + 4;
                    if (h.Length < data + size) continue;
                    var pixels = h.AsSpan(data, size);
                    int lo = 255, hi = 0;
                    foreach (byte p in pixels)
                    {
                        int a = fmt == 8 ? p >> 4 : p, b = fmt == 8 ? p & 15 : p;
                        lo = Math.Min(lo, Math.Min(a, b));
                        hi = Math.Max(hi, Math.Max(a, b));
                    }
                    ulong th = XxHash64(pixels), tl = XxHash64(h.AsSpan(0x68 + 2 * lo, 2 * (hi - lo + 1)));
                    key = $"tex1_{w}x{hh}_{th:x16}_{tl:x16}_{fmt}";
                    maySeeThrough = tlutFormat is 0 or 2;  // IA8 or RGB5A3 palette
                }
                else
                {
                    if (h.Length < 0x6C + size) continue;
                    var pixels = h.AsSpan(0x6C, size);
                    key = $"tex1_{w}x{hh}_{XxHash64(pixels):x16}_{fmt}";
                    maySeeThrough = fmt is 0 or 1 or 2 or 3 or 5 or 6 || (fmt == 14 && CmprHasClearTexels(pixels));
                }
                list.Add(new GcTexture(w, hh, fmt, key, maySeeThrough));
            }
            return list;
        }

        // CMPR (DXT1, big-endian colours): a 4x4 block whose first colour isn't above the second has a clear 4th entry.
        static bool CmprHasClearTexels(ReadOnlySpan<byte> d)
        {
            for (int i = 0; i + 8 <= d.Length; i += 8)
            {
                if (BinaryPrimitives.ReadUInt16BigEndian(d[i..]) > BinaryPrimitives.ReadUInt16BigEndian(d[(i + 2)..])) continue;
                for (int k = 4; k < 8; k++)
                {
                    byte s = d[i + k];
                    if ((s & 3) == 3 || (s >> 2 & 3) == 3 || (s >> 4 & 3) == 3 || (s >> 6) == 3) return true;
                }
            }
            return false;
        }
    }

    // An Xbox texture, decoded to RGBA (null for formats this doesn't decode). The raster format's colour part: 5 = 8888,
    // 6 = 888; 0x2000 = 8-bit palette of that format. Pixels (and palette indices) are swizzled (Morton order).
    internal sealed record XboxTexture(int Width, int Height, byte[]? Rgba, bool HasAlpha)
    {
        public static List<XboxTexture> Read(ReadOnlySpan<byte> rwtx)
        {
            var list = new List<XboxTexture>();
            foreach (var h in TextureStructs(rwtx))
            {
                if (h.Length < 0x5C) { list.Add(new XboxTexture(0, 0, null, false)); continue; }
                uint raster = BinaryPrimitives.ReadUInt32LittleEndian(h.AsSpan(0x48));
                int w = BinaryPrimitives.ReadUInt16LittleEndian(h.AsSpan(0x50)), hh = BinaryPrimitives.ReadUInt16LittleEndian(h.AsSpan(0x52));
                int compression = h[0x57];
                int colour = (int)(raster >> 8 & 0xF);
                bool palette = (raster & 0x2000) != 0, alpha = colour == 5;
                long pixels = (long)w * hh;
                byte[]? rgba = null;
                if (compression == 0 && (colour is 5 or 6) && w > 0 && hh > 0 && pixels <= 4096 * 4096)
                {
                    int need = palette ? 0x5C + 1024 + (int)pixels : 0x5C + 4 * (int)pixels;
                    if (h.Length >= need)
                    {
                        rgba = new byte[pixels * 4];
                        var order = Swizzle(w, hh);
                        for (int i = 0; i < pixels; i++)
                        {
                            int src = palette ? 0x5C + 4 * h[0x5C + 1024 + order[i]] : 0x5C + 4 * order[i];
                            rgba[4 * i] = h[src + 2];
                            rgba[4 * i + 1] = h[src + 1];
                            rgba[4 * i + 2] = h[src];
                            rgba[4 * i + 3] = alpha ? h[src + 3] : (byte)255;
                        }
                    }
                }
                list.Add(new XboxTexture(w, hh, rgba, alpha));
            }
            return list;
        }

        // For each pixel in row order, its place in the swizzled data: x and y bits interleaved (x first) while both have
        // bits left, then the longer side's remaining bits.
        static int[] Swizzle(int w, int h)
        {
            uint mx = 0, my = 0, bit = 1;
            for (int i = 1; i < w || i < h; i <<= 1)
            {
                if (i < w) { mx |= bit; bit <<= 1; }
                if (i < h) { my |= bit; bit <<= 1; }
            }
            static uint Spread(uint v, uint mask)
            {
                uint r = 0;
                for (uint m = mask, b = 1; m != 0; m &= m - 1, b <<= 1)
                    if ((v & b) != 0) r |= m & ~(m - 1);
                return r;
            }
            var order = new int[w * h];
            for (int y = 0; y < h; y++)
            {
                uint sy = Spread((uint)y, my);
                for (int x = 0; x < w; x++) order[y * w + x] = (int)(Spread((uint)x, mx) | sy);
            }
            return order;
        }
    }

    // XXH64 (seed 0), as the game's texture replacement hashes texture data.
    internal static ulong XxHash64(ReadOnlySpan<byte> b)
    {
        const ulong P1 = 11400714785074694791, P2 = 14029467366897019727, P3 = 1609587929392839161,
                    P4 = 9650029242287828579, P5 = 2870177450012600261;
        static ulong Round(ulong acc, ulong lane) => ulong.RotateLeft(acc + lane * P2, 31) * P1;
        static ulong Merge(ulong acc, ulong v) => (acc ^ Round(0, v)) * P1 + P4;
        int p = 0, n = b.Length;
        ulong h;
        if (n >= 32)
        {
            ulong v1 = unchecked(P1 + P2), v2 = P2, v3 = 0, v4 = unchecked(0 - P1);
            for (; p + 32 <= n; p += 32)
            {
                v1 = Round(v1, BinaryPrimitives.ReadUInt64LittleEndian(b[p..]));
                v2 = Round(v2, BinaryPrimitives.ReadUInt64LittleEndian(b[(p + 8)..]));
                v3 = Round(v3, BinaryPrimitives.ReadUInt64LittleEndian(b[(p + 16)..]));
                v4 = Round(v4, BinaryPrimitives.ReadUInt64LittleEndian(b[(p + 24)..]));
            }
            h = ulong.RotateLeft(v1, 1) + ulong.RotateLeft(v2, 7) + ulong.RotateLeft(v3, 12) + ulong.RotateLeft(v4, 18);
            h = Merge(Merge(Merge(Merge(h, v1), v2), v3), v4);
        }
        else h = P5;
        h += (ulong)n;
        for (; p + 8 <= n; p += 8) h = ulong.RotateLeft(h ^ Round(0, BinaryPrimitives.ReadUInt64LittleEndian(b[p..])), 27) * P1 + P4;
        if (p + 4 <= n) { h = ulong.RotateLeft(h ^ BinaryPrimitives.ReadUInt32LittleEndian(b[p..]) * P1, 23) * P2 + P3; p += 4; }
        for (; p < n; p++) h = ulong.RotateLeft(h ^ b[p] * P5, 11) * P1;
        h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
        return h;
    }

    // A plain 8-bit RGBA PNG, rows unfiltered.
    internal static byte[] Png(int w, int h, byte[] rgba)
    {
        using var ms = new MemoryStream();
        ms.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
        var header = new byte[13];
        BinaryPrimitives.WriteInt32BigEndian(header, w);
        BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(4), h);
        header[8] = 8;
        header[9] = 6;
        Chunk(ms, "IHDR", header);
        using (var z = new MemoryStream())
        {
            using (var zs = new ZLibStream(z, CompressionLevel.Optimal, true))
                for (int y = 0; y < h; y++)
                {
                    zs.WriteByte(0);
                    zs.Write(rgba, y * w * 4, w * 4);
                }
            Chunk(ms, "IDAT", z.ToArray());
        }
        Chunk(ms, "IEND", Array.Empty<byte>());
        return ms.ToArray();
    }

    static void Chunk(Stream s, string type, byte[] data)
    {
        var head = new byte[8];
        BinaryPrimitives.WriteInt32BigEndian(head, data.Length);
        System.Text.Encoding.ASCII.GetBytes(type).CopyTo(head, 4);
        s.Write(head);
        s.Write(data);
        uint crc = Crc32(head.AsSpan(4), 0xFFFFFFFF);
        crc = Crc32(data, crc) ^ 0xFFFFFFFF;
        var tail = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(tail, crc);
        s.Write(tail);
    }

    static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n =>
    {
        uint c = (uint)n;
        for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();

    static uint Crc32(ReadOnlySpan<byte> d, uint crc)
    {
        foreach (byte b in d) crc = CrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    static void TryDelete(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
        catch (Exception) { }
    }
}
