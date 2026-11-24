using System.Buffers.Binary;
using System.IO.Compression;
using FrightWorks;
using FrightWorks.ModPatches;
using HipPatchTests;

// The launcher's Xbox texture copy (launcher/XboxTextures.cs) on textures, an Xbox disc image and a GameCube disc made
// in code.
static partial class Tests
{
    static void XboxTextureCopy()
    {
        // XXH64: the published values for "" and "abc", and longer inputs (the 32-byte loop) from tools/debug/texhunt/gxtex.py.
        Check("XXH64 empty", XboxTextures.XxHash64(ReadOnlySpan<byte>.Empty) == 0xEF46DB3751D8E999);
        Check("XXH64 abc", XboxTextures.XxHash64("abc"u8) == 0x44BC2CF5AD770999);
        Check("XXH64 37 bytes", XboxTextures.XxHash64(Enumerable.Range(0, 37).Select(i => (byte)i).ToArray()) == 0xD93FA2DFEE5C24C9);
        Check("XXH64 100 bytes", XboxTextures.XxHash64(Enumerable.Range(0, 100).Select(i => (byte)i).ToArray()) == 0x6AC1E58032166597);

        // Swizzle: in a 4x4 texture whose n-th stored pixel has red n, row 0 reads 0 1 4 5 and column 0 reads 0 2 8 10.
        var square = XboxTexture(4, 4, 0x0500, Enumerable.Range(0, 16).SelectMany(n => new byte[] { 0, 0, (byte)n, 0x80 }).ToArray());
        var px = XboxTextures.XboxTexture.Read(square).Single();
        Check("Xbox: 8888 decodes", px.Rgba != null && px.HasAlpha && px.Rgba[3] == 0x80);
        Check("Xbox: swizzled rows", px.Rgba != null && new[] { 0, 1, 2, 3 }.Select(x => px.Rgba[4 * x]).SequenceEqual(new byte[] { 0, 1, 4, 5 }));
        Check("Xbox: swizzled columns", px.Rgba != null && new[] { 0, 1, 2, 3 }.Select(y => px.Rgba[4 * 4 * y]).SequenceEqual(new byte[] { 0, 2, 8, 10 }));
        // 4x2: the wider side's last bit goes on top, so row 0 reads 0 1 4 5 and row 1 reads 2 3 6 7.
        var wide = XboxTextures.XboxTexture.Read(XboxTexture(4, 2, 0x0500, Enumerable.Range(0, 8).SelectMany(n => new byte[] { 0, 0, (byte)n, 0xFF }).ToArray())).Single();
        Check("Xbox: swizzled non-square", wide.Rgba != null && Enumerable.Range(0, 8).Select(i => wide.Rgba[4 * i]).SequenceEqual(new byte[] { 0, 1, 4, 5, 2, 3, 6, 7 }));
        // An 8-bit palette of 888 colours: BGRA entries, alpha forced opaque; the indices are swizzled too.
        var palette = new byte[1024];
        for (int i = 0; i < 256; i++) { palette[4 * i] = (byte)i; palette[4 * i + 1] = 7; palette[4 * i + 2] = (byte)(255 - i); palette[4 * i + 3] = 0; }
        var pal = XboxTextures.XboxTexture.Read(XboxTexture(4, 4, 0x2600, palette.Concat(Enumerable.Range(0, 16).Select(n => (byte)(n * 3))).ToArray())).Single();
        Check("Xbox: palette decodes", pal.Rgba != null && !pal.HasAlpha && pal.Rgba[4 * 2] == 255 - 12 && pal.Rgba[4 * 2 + 1] == 7 && pal.Rgba[4 * 2 + 2] == 12 && pal.Rgba[4 * 2 + 3] == 255,
              pal.Rgba == null ? "null" : string.Join(",", pal.Rgba.Take(12)));
        Check("Xbox: DXT is left alone", XboxTextures.XboxTexture.Read(XboxTexture(4, 4, 0x0100, new byte[8], dxt: 1)).Single().Rgba == null);

        // GameCube names: RGBA8 hashes its data; a palette texture also hashes the palette entries its indices use.
        var gcRgba = GcTexture(8, 8, 6, Enumerable.Range(0, 256).Select(i => (byte)(i * 7)).ToArray());
        var gt = XboxTextures.GcTexture.Read(gcRgba).Single();
        Check("GC: RGBA8 name", gt.Key == $"tex1_8x8_{XboxTextures.XxHash64(Enumerable.Range(0, 256).Select(i => (byte)(i * 7)).ToArray()):x16}_6", gt.Key);
        var tlut = Enumerable.Range(0, 512).Select(i => (byte)(i * 5)).ToArray();
        var indices = Enumerable.Range(0, 64).Select(i => (byte)(3 + i % 4)).ToArray();  // entries 3 to 6
        var gcPal = XboxTextures.GcTexture.Read(GcTexture(8, 8, 9, tlut.Concat(new byte[4]).Concat(indices).ToArray(), tlutAt: true, tlutFormat: 1)).Single();
        Check("GC: palette name", gcPal.Key == $"tex1_8x8_{XboxTextures.XxHash64(indices):x16}_{XboxTextures.XxHash64(tlut.AsSpan(6, 8)):x16}_9", gcPal.Key);
        Check("GC: RGB565 palette is opaque", !gcPal.MaySeeThrough);
        var opaqueCmpr = new byte[32];  // four 4x4 blocks, first colour above the second: no clear texels
        for (int b = 0; b < 4; b++) { opaqueCmpr[8 * b] = 0xFF; opaqueCmpr[8 * b + 4] = 0xFF; }
        Check("GC: CMPR without clear texels", !XboxTextures.GcTexture.Read(GcTexture(8, 8, 14, opaqueCmpr)).Single().MaySeeThrough);
        var clearCmpr = (byte[])opaqueCmpr.Clone();
        clearCmpr[8] = 0; clearCmpr[10] = 0xFF;  // second block: first colour below the second, and its texels pick entry 3
        Check("GC: CMPR with clear texels", XboxTextures.GcTexture.Read(GcTexture(8, 8, 14, clearCmpr)).Single().MaySeeThrough);

        var root = Path.Combine(Path.GetTempPath(), "hip-patch-tests", Guid.NewGuid().ToString("N"));
        try
        {
            var gcDisc = Path.Combine(root, "Disc");
            var userData = Path.Combine(root, "UserData");
            Directory.CreateDirectory(Path.Combine(gcDisc, "files", "hb"));
            Directory.CreateDirectory(userData);
            // GC level: wall (RGBA8, bigger on the Xbox), glass (RGB5A3, see-through; opaque on the Xbox: kept), and
            // floor (RGB565; DXT on the Xbox: kept). The fixture level's own tree texture has no Xbox counterpart.
            var wallData = Enumerable.Range(0, 256).Select(i => (byte)(i ^ 0x5A)).ToArray();
            var gcLevel = Fixtures.Level(1, new[]
            {
                new Fixtures.A(1, "RWTX", "wall.RW3", GcTexture(8, 8, 6, wallData), 6),
                new Fixtures.A(1, "RWTX", "glass.RW3", GcTexture(8, 8, 5, new byte[128]), 6),
                new Fixtures.A(1, "RWTX", "floor.RW3", GcTexture(8, 8, 4, new byte[128]), 6),
            });
            File.WriteAllBytes(Path.Combine(gcDisc, "files", "hb", "h001.HIP"), gcLevel);
            var wallXbox = Enumerable.Range(0, 256).SelectMany(n => new byte[] { (byte)n, 1, 2, 0xFF }).ToArray();
            var xboxLevel = Fixtures.Level(2, new[]
            {
                new Fixtures.A(1, "RWTX", "wall.RW3", XboxTexture(16, 16, 0x0500, wallXbox), 6),
                new Fixtures.A(1, "RWTX", "glass.RW3", XboxTexture(8, 8, 0x0600, new byte[256]), 6),
                new Fixtures.A(1, "RWTX", "floor.RW3", XboxTexture(8, 8, 0x0100, new byte[32], dxt: 1), 6),
            });
            var iso = Path.Combine(root, "xbox.iso");
            File.WriteAllBytes(iso, Xiso(0, ("h0", "h001.HIP", xboxLevel)));

            int n;
            using (var disc = Ps2Cutscenes.OpenImage(iso)) n = XboxTextures.Copy(disc, gcDisc, userData);
            var wallKey = $"tex1_8x8_{XboxTextures.XxHash64(wallData):x16}_6";
            var files = Directory.Exists(XboxTextures.Dir(userData)) ? Directory.GetFiles(XboxTextures.Dir(userData)).Select(Path.GetFileName).ToList() : new List<string?>();
            Check("Xbox textures: only the wall is written, under its GameCube name", n == 1 && files.SequenceEqual(new[] { wallKey + ".png" }), $"{n}: {string.Join(" ", files)}");
            var png = File.ReadAllBytes(Path.Combine(XboxTextures.Dir(userData), wallKey + ".png"));
            var (w, h, rgba) = ReadPng(png);
            Check("Xbox textures: the PNG keeps the Xbox size", w == 16 && h == 16, $"{w}x{h}");
            // Stored pixel n has blue n; pixel (x=2, y=0) is stored 4th.
            Check("Xbox textures: the PNG holds the unswizzled pixels", rgba.Length == 16 * 16 * 4 && rgba[4 * 2 + 2] == 4 && rgba[4 * 2 + 3] == 0xFF);
            Check("Xbox textures: count", XboxTextures.Count(userData) == 1);
            Check("Xbox textures: no staging folder left", !Directory.Exists(XboxTextures.Dir(userData) + ".new"));
            XboxTextures.Remove(userData);
            Check("Xbox textures: remove", XboxTextures.Count(userData) == 0);
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    const uint RwVersion = 0x1003FFFF;

    static byte[] RwChunk(uint type, byte[] body)
    {
        var c = new byte[12 + body.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(c, type);
        BinaryPrimitives.WriteUInt32LittleEndian(c.AsSpan(4), (uint)body.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(c.AsSpan(8), RwVersion);
        body.CopyTo(c, 12);
        return c;
    }

    static byte[] Dictionary(byte[] textureStruct) =>
        RwChunk(0x16, Join(RwChunk(1, new byte[4]), RwChunk(0x15, Join(RwChunk(1, textureStruct), RwChunk(3, Array.Empty<byte>()))), RwChunk(3, Array.Empty<byte>())));

    // An Xbox texture dictionary: raster format at +0x48, size at +0x50, compression at +0x57, pixels from +0x5C.
    static byte[] XboxTexture(int w, int h, uint raster, byte[] data, byte dxt = 0)
    {
        var s = new byte[0x5C + data.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(s, 5);
        BinaryPrimitives.WriteUInt32LittleEndian(s.AsSpan(0x48), raster);
        BinaryPrimitives.WriteUInt16LittleEndian(s.AsSpan(0x50), (ushort)w);
        BinaryPrimitives.WriteUInt16LittleEndian(s.AsSpan(0x52), (ushort)h);
        s[0x54] = 32;
        s[0x55] = 1;
        s[0x57] = dxt;
        data.CopyTo(s, 0x5C);
        return Dictionary(s);
    }

    // A GameCube texture dictionary: size at +0x5C (big-endian), format at +0x62; data from +0x6C, or for palette
    // formats the palette from +0x68 (then 4 bytes, then the indices).
    static byte[] GcTexture(int w, int h, byte fmt, byte[] data, bool tlutAt = false, byte tlutFormat = 0)
    {
        int at = tlutAt ? 0x68 : 0x6C;
        var s = new byte[at + data.Length];
        BinaryPrimitives.WriteUInt32BigEndian(s, 6);
        BinaryPrimitives.WriteUInt16BigEndian(s.AsSpan(0x5C), (ushort)w);
        BinaryPrimitives.WriteUInt16BigEndian(s.AsSpan(0x5E), (ushort)h);
        s[0x61] = 1;
        s[0x62] = fmt;
        s[0x63] = tlutFormat;
        data.CopyTo(s, at);
        return Dictionary(s);
    }

    // Reads back an RGBA PNG made by XboxTextures.Png (one IDAT, rows unfiltered).
    static (int W, int H, byte[] Rgba) ReadPng(byte[] png)
    {
        int w = BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(16)), h = BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(20));
        int at = 8 + 25, idat = BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(at));
        using var z = new ZLibStream(new MemoryStream(png, at + 8, idat), CompressionMode.Decompress);
        using var raw = new MemoryStream();
        z.CopyTo(raw);
        var rows = raw.ToArray();
        var rgba = new byte[w * h * 4];
        for (int y = 0; y < h; y++) Array.Copy(rows, y * (w * 4 + 1) + 1, rgba, y * w * 4, w * 4);
        return (w, h, rgba);
    }
}

static class XboxTexturesMode
{
    // --xbox-textures ISO DISC USERDATA: writes a real Xbox disc's textures into USERDATA/xbox/textures (the player's data:
    // keep it local).
    public static int Run(string[] args)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        int n;
        using (var disc = Ps2Cutscenes.OpenImage(args[1]))
            n = XboxTextures.Copy(disc, args[2], args[3], (done, total, name) => { if (name.Length > 0) Console.Write($"\r{done}/{total} {name}    "); });
        Console.WriteLine();
        Console.WriteLine($"wrote {n} textures in {sw.Elapsed.TotalSeconds:F1} s");
        return n > 0 ? 0 : 1;
    }
}
