using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using FrightWorks.ModPatches;

namespace HipPatchTests;

// Test data made in code: a small level file with the disc's layout and a plain box model. No game data.
static class Fixtures
{
    public const uint SimpleSize = 0x5C;

    public sealed record A(uint Layer, string Type, string Name, byte[] Data, uint Flags);

    static void Tag(Stream s, string tag, byte[] body)
    {
        s.Write(Encoding.ASCII.GetBytes(tag));
        var size = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(size, (uint)body.Length);
        s.Write(size);
        s.Write(body);
    }

    static byte[] U32s(params uint[] values)
    {
        var b = new byte[values.Length * 4];
        for (int i = 0; i < values.Length; i++) BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(i * 4), values[i]);
        return b;
    }

    static uint Align(uint x, uint a) => (x + a - 1) & ~(a - 1);

    // A level with layers 0, 1 and 3 written as the disc writes them. seed changes a byte, so two levels can differ.
    public static byte[] Level(byte seed = 1, A[]? extra = null, bool withSimple = true, uint[]? layerTypes = null)
    {
        var simple = new byte[SimpleSize];
        for (int i = 0; i < simple.Length; i++) simple[i] = (byte)(i * 3 + seed);
        var modelBytes = Model("tree");
        var textureBytes = Texture("tree.RW3");
        var list = new List<A>();
        if (withSimple) list.Add(new A(0, "SIMP", "TREE1", simple, 2));
        list.Add(new A(0, "PLYR", "PLAYER", new byte[] { seed, 2, 3, 4, 5, 6, 7, 8 }, 2));
        list.Add(new A(1, "RWTX", "tree.RW3", textureBytes, 6));
        list.Add(new A(3, "MODL", "tree", modelBytes, 5));
        if (extra != null) list.AddRange(extra);
        var types = layerTypes ?? new uint[] { 0, 1, 3 };

        // Lay out the stream: layers on 0x20, each asset followed by its plus.
        var relative = new Dictionary<A, uint>();
        var plus = new Dictionary<A, uint>();
        uint rel = 0;
        var layerLists = types.Select(t => list.Where(a => a.Layer == t).ToList()).ToList();
        foreach (var layer in layerLists)
        {
            if (layer.Count == 0) continue;
            rel = Align(rel, 0x20);
            foreach (var a in layer)
            {
                relative[a] = rel;
                rel += (uint)a.Data.Length;
                plus[a] = Align(rel, 0x10) - rel;
                rel += plus[a];
            }
        }
        uint total = Align(rel, 0x20);
        var sorted = list.OrderBy(a => HipFile.HashName(a.Name)).ToList();

        var pack = new MemoryStream();
        Tag(pack, "PVER", U32s(2, 0x00040000, 0x00040000));
        Tag(pack, "PFLG", U32s(0x2E));
        uint largestAsset = list.Max(a => (uint)a.Data.Length);
        uint largestLayer = layerLists.Max(l => l.Aggregate(0u, (sum, a) => sum + (uint)a.Data.Length + plus[a]));
        Tag(pack, "PCNT", U32s((uint)list.Count, (uint)types.Length, largestAsset, largestLayer, 0));
        Tag(pack, "PCRT", new byte[8]);
        Tag(pack, "PMOD", U32s(0));
        var head = new MemoryStream();
        head.Write("HIPA"u8);
        head.Write(new byte[4]);
        Tag(head, "PACK", pack.ToArray());

        byte[] Dict(uint streamBase)
        {
            var toc = new MemoryStream();
            Tag(toc, "AINF", U32s(0));
            foreach (var a in sorted)
            {
                var body = new MemoryStream();
                body.Write(U32s(HipFile.HashName(a.Name)));
                body.Write(Encoding.ASCII.GetBytes(a.Type));
                body.Write(U32s(streamBase + relative[a], (uint)a.Data.Length, plus[a], a.Flags));
                var name = new MemoryStream();
                name.Write(new byte[4]);
                name.Write(Encoding.ASCII.GetBytes(a.Name));
                name.WriteByte(0);
                if ((a.Name.Length + 1) % 2 != 0) name.WriteByte(0);
                name.Write(new byte[6]);
                Tag(body, "ADBG", name.ToArray());
                Tag(toc, "AHDR", body.ToArray());
            }
            var ltoc = new MemoryStream();
            Tag(ltoc, "LINF", U32s(0));
            for (int i = 0; i < types.Length; i++)
            {
                var body = new MemoryStream();
                body.Write(U32s(types[i], (uint)layerLists[i].Count));
                foreach (var a in layerLists[i]) body.Write(U32s(HipFile.HashName(a.Name)));
                body.Write(U32s(0));
                Tag(ltoc, "LHDR", body.ToArray());
            }
            var inner = new MemoryStream();
            Tag(inner, "ATOC", toc.ToArray());
            Tag(inner, "LTOC", ltoc.ToArray());
            var whole = new MemoryStream();
            Tag(whole, "DICT", inner.ToArray());
            return whole.ToArray();
        }

        var streamHeader = U32s(0xFFFFFFFF);
        uint pre = (uint)head.Length + (uint)Dict(0).Length + 8 + 8 + (uint)streamHeader.Length + 8 + 4;
        uint streamBaseValue = Align(pre, 0x40);
        var stream = new byte[total];
        Array.Fill(stream, (byte)0x33);
        foreach (var a in list) a.Data.CopyTo(stream, relative[a]);
        var dpak = new byte[4 + (streamBaseValue - pre) + total];
        BinaryPrimitives.WriteUInt32BigEndian(dpak, streamBaseValue - pre);
        Array.Fill(dpak, (byte)0x33, 4, (int)(streamBaseValue - pre));
        stream.CopyTo(dpak, (int)(4 + streamBaseValue - pre));
        var strm = new MemoryStream();
        Tag(strm, "DHDR", streamHeader);
        Tag(strm, "DPAK", dpak);
        head.Write(Dict(streamBaseValue));
        Tag(head, "STRM", strm.ToArray());
        return head.ToArray();
    }

    static byte[] Sec(uint type, byte[] body, uint version = 0x310)
    {
        var b = new byte[12 + body.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(b, type);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(4), (uint)body.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8), version);
        body.CopyTo(b, 12);
        return b;
    }

    static byte[] Join(params byte[][] parts) => parts.SelectMany(p => p).ToArray();

    // Enough of a clump for the structure check: real sections, a geometry header with this triangle count and a material
    // mesh list. tag makes the bytes differ. meshIndices = each material's index count (default: one material drawing all
    // the triangles), so a test can make the two disagree. strip = a triangle strip list instead of a triangle list.
    public static byte[] Model(string tag, uint triangles = 12, uint[]? meshIndices = null, bool strip = false, bool noMeshList = false)
    {
        var header = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(header, 0x1003E);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4), triangles);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(8), 24);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12), 1);
        var extension = noMeshList ? Array.Empty<byte>() : Sec(0x50E, MeshList(meshIndices ?? new[] { triangles * 3 }, strip));
        var geometry = Sec(0x0F, Join(Sec(1, header), Sec(2, Encoding.ASCII.GetBytes(tag + "\0\0\0\0")), Sec(3, extension)));
        var list = Sec(0x1A, Join(Sec(1, new byte[4]), geometry));
        return Join(Sec(0x10, Join(Sec(1, new byte[4]), list)), Sec(3, Array.Empty<byte>()));
    }

    public static byte[] MeshList(uint[] meshIndices, bool strip = false)
    {
        long size = 12 + meshIndices.Sum(n => 8 + 4L * n);
        var b = new byte[size];
        BinaryPrimitives.WriteUInt32LittleEndian(b, strip ? 1u : 0u);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(4), (uint)meshIndices.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8), (uint)meshIndices.Sum(n => (long)n));
        int o = 12;
        for (int i = 0; i < meshIndices.Length; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(o), meshIndices[i]);
            BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(o + 4), (uint)i);
            o += 8 + (int)meshIndices[i] * 4;
        }
        return b;
    }

    // A dictionary with one GameCube texture, 8x8.
    public static byte[] Texture(string tag, int width = 8, int height = 8, int padTo = 0)
    {
        const uint V = 0x0C02FFFF;
        var bytes = Math.Max(width * height * 2, padTo);
        var head = new byte[0x6C + bytes];
        BinaryPrimitives.WriteUInt32BigEndian(head, 6);
        Encoding.ASCII.GetBytes(tag).CopyTo(head, 0x18);
        BinaryPrimitives.WriteUInt16BigEndian(head.AsSpan(0x5C), (ushort)width);
        BinaryPrimitives.WriteUInt16BigEndian(head.AsSpan(0x5E), (ushort)height);
        BinaryPrimitives.WriteUInt32BigEndian(head.AsSpan(0x68), (uint)bytes);
        var native = Sec(0x15, Join(Sec(1, head, V), Sec(3, Array.Empty<byte>(), V)), V);
        return Sec(0x16, Join(Sec(1, new byte[4], V), native, Sec(3, Array.Empty<byte>(), V)), V);
    }

    public static string Sha(byte[] d) => Convert.ToHexString(SHA256.HashData(d));
}

// A throwaway game folder: a disc with levels, and an empty mods folder.
sealed class Sandbox : IDisposable
{
    public readonly string Root, Mods, Disc, Cache;

    public Sandbox()
    {
        Root = Path.Combine(Path.GetTempPath(), "hip-patch-tests", Guid.NewGuid().ToString("N"));
        Mods = Path.Combine(Root, "UserData", "mods");
        Disc = Path.Combine(Root, "Disc");
        Cache = Path.Combine(Root, "UserData", "Cache");
        Directory.CreateDirectory(Mods);
        Directory.CreateDirectory(Path.Combine(Disc, "files", "hb"));
        Directory.CreateDirectory(Path.Combine(Disc, "files", "lb"));
        File.WriteAllBytes(Path.Combine(Disc, "files", "hb", "h002.HIP"), Fixtures.Level(1));
        var source = new Fixtures.A(3, "MODL", "barrel", Fixtures.Model("barrel"), 5);
        var sourceTexture = new Fixtures.A(1, "RWTX", "keg.RW3", Fixtures.Texture("keg.RW3"), 6);
        File.WriteAllBytes(Path.Combine(Disc, "files", "lb", "l011.HIP"), Fixtures.Level(2, new[] { source, sourceTexture }));
    }

    public string LevelSha(string relative) => Fixtures.Sha(File.ReadAllBytes(Path.Combine(Disc, "files", relative)));
    public string H002Sha => LevelSha(Path.Combine("hb", "h002.HIP"));
    public string L011Sha => LevelSha(Path.Combine("lb", "l011.HIP"));
    public string Output => Path.Combine(Mods, PatchBuilder.OutputFolder);
    public string OutputH002 => Path.Combine(Output, "files", "hb", "h002.HIP");

    public PatchResult Ensure(Func<bool>? running = null) => PatchBuilder.Ensure(Mods, Disc, Cache, running);

    // A mod with a crate model and texture in assets/ and the given recipe (or one that adds a crate to h002).
    public string Mod(string name, string? recipe = null, byte[]? model = null, byte[]? texture = null)
    {
        var dir = Path.Combine(Mods, name);
        Directory.CreateDirectory(Path.Combine(dir, "assets"));
        File.WriteAllBytes(Path.Combine(dir, "assets", "m.modl"), model ?? Fixtures.Model(name));
        File.WriteAllBytes(Path.Combine(dir, "assets", "t.rwtx"), texture ?? Fixtures.Texture(name));
        File.WriteAllText(Path.Combine(dir, PatchRecipe.FileName), recipe ?? Crate(name));
        return dir;
    }

    public string Crate(string name, string level = "h002", string? sha = null) => $$"""
        { "format": 1, "name": "{{name}}", "levels": { "{{level}}": { "base_sha256": "{{sha ?? H002Sha}}",
          "add": [ { "type": "RWTX", "name": "{{name}}.RW3", "file": "assets/t.rwtx" }, { "type": "MODL", "name": "{{name}}", "file": "assets/m.modl" } ],
          "objects": [ { "name": "{{name}} OBJ", "model": "{{name}}", "pos": [1, 2, 3], "yaw": 90 } ] } } }
        """;

    // Every file under the mods folder with its hash: "is the folder unchanged?"
    public string Snapshot()
    {
        var lines = Directory.EnumerateFileSystemEntries(Mods, "*", SearchOption.AllDirectories).OrderBy(f => f, StringComparer.Ordinal)
            .Select(f => Path.GetRelativePath(Mods, f) + " " + (File.Exists(f) ? Fixtures.Sha(File.ReadAllBytes(f)) : "dir"));
        return string.Join("\n", lines);
    }

    public void Dispose()
    {
        try { Directory.Delete(Root, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
