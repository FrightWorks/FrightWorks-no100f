using System.Buffers.Binary;
using System.Text;

namespace FrightWorks.ModPatches;

// A Heavy Iron level file (HIP) that can be rebuilt with extra assets: the launcher's version of
// tools/debug/game/hipedit.py, which it must match byte for byte (tools/hip-patch-tests checks that).
//
// Layout: HIPA | PACK(PVER PFLG PCNT PCRT PMOD) | DICT(ATOC(AINF AHDR...) LTOC(LINF LHDR...)) | STRM(DHDR DPAK).
// An AHDR holds the ID, type, absolute file offset, size, plus (padding counted after the asset) and flags, then the
// ADBG (name). The ATOC is sorted by ID. Assets sit in the DPAK in LTOC order, each followed by its plus; layers start
// on 0x20. Adding an asset takes an AHDR in sorted place, its ID at the end of its layer's list, its bytes at that
// layer's end, PCNT's asset count and every later offset shifted. Build does all of that.
sealed class HipFile
{
    public const uint LayerDefault = 0, LayerTextures = 1, LayerModels = 3;
    const byte Fill = 0x33;  // what the disc pads with

    public sealed class Asset
    {
        public uint Id;
        public byte[] Type = Array.Empty<byte>();
        public ReadOnlyMemory<byte> Data;
        public uint Plus, Flags, Offset, StartAlign;
        public byte[] Debug = Array.Empty<byte>();
        public uint Relative;

        public string TypeName => Encoding.ASCII.GetString(Type);

        public string Name
        {
            get
            {
                if (Debug.Length <= 4) return "";
                var end = Array.IndexOf(Debug, (byte)0, 4);
                return Encoding.Latin1.GetString(Debug, 4, (end < 0 ? Debug.Length : end) - 4);
            }
        }
    }

    sealed class Layer
    {
        public uint Type;
        public List<uint> Ids = new();
        public byte[] Rest = Array.Empty<byte>();
    }

    readonly List<(byte[] Tag, byte[] Value)> pack = new();
    readonly byte[] assetInfo, layerInfo, streamHeader;
    readonly List<Layer> layers = new();
    readonly uint baseAlign;
    List<Asset> assets = new();
    readonly Dictionary<uint, Asset> byId = new();

    public IReadOnlyList<Asset> Assets => assets;
    public Asset? Find(uint id) => byId.GetValueOrDefault(id);
    public bool HasLayer(uint type) => layers.Any(l => l.Type == type);

    public HipFile(byte[] file)
    {
        try
        {
            Expect(file, 0, "HIPA");
            int off = 8;
            Expect(file, off, "PACK");
            int packEnd = off + 8 + Size(file, off);
            int p = off + 8;
            while (p < packEnd)
            {
                int z = Size(file, p);
                pack.Add((Slice(file, p, 4), Slice(file, p + 8, z)));
                p += 8 + z;
            }
            off = packEnd;
            Expect(file, off, "DICT");
            p = off + 8;
            Expect(file, p, "ATOC");
            int q = p + 8;
            int assetsEnd = q + Size(file, p);
            Expect(file, q, "AINF");
            assetInfo = Slice(file, q + 8, Size(file, q));
            q += 8 + assetInfo.Length;
            while (q < assetsEnd)
            {
                Expect(file, q, "AHDR");
                int z = Size(file, q);
                int b = q + 8;
                Expect(file, b + 24, "ADBG");
                var asset = new Asset
                {
                    Id = U32(file, b), Type = Slice(file, b + 4, 4), Offset = U32(file, b + 8), Plus = U32(file, b + 16), Flags = U32(file, b + 20),
                    Debug = Slice(file, b + 32, Size(file, b + 24)),
                };
                uint size = U32(file, b + 12);
                if ((ulong)asset.Offset + size > (ulong)file.Length) throw new InvalidDataException("an asset runs past the end of the file");
                asset.Data = file.AsMemory((int)asset.Offset, (int)size);
                assets.Add(asset);
                byId[asset.Id] = asset;
                q = b + z;
            }
            p = assetsEnd;
            Expect(file, p, "LTOC");
            q = p + 8;
            int layersEnd = q + Size(file, p);
            Expect(file, q, "LINF");
            layerInfo = Slice(file, q + 8, Size(file, q));
            q += 8 + layerInfo.Length;
            while (q < layersEnd)
            {
                Expect(file, q, "LHDR");
                int z = Size(file, q);
                int b = q + 8;
                var layer = new Layer { Type = U32(file, b) };
                int count = checked((int)U32(file, b + 4));
                for (int i = 0; i < count; i++) layer.Ids.Add(U32(file, b + 8 + 4 * i));
                layer.Rest = Slice(file, b + 8 + 4 * count, z - 8 - 4 * count);
                layers.Add(layer);
                q = b + z;
            }
            p = layersEnd;
            Expect(file, p, "STRM");
            q = p + 8;
            Expect(file, q, "DHDR");
            streamHeader = Slice(file, q + 8, Size(file, q));
            q += 8 + streamHeader.Length;
            Expect(file, q, "DPAK");
            if (assets.Count == 0) throw new InvalidDataException("no assets");
            baseAlign = assets.Min(a => a.Offset) % 0x40 == 0 ? 0x40u : 0x20u;  // the disc has both; kept so a rebuild comes out the same
        }
        catch (Exception e) when (e is ArgumentException or IndexOutOfRangeException or OverflowException)
        {
            throw new InvalidDataException("not a readable level file", e);
        }
    }

    // Appends a new asset at the end of a layer. Disc alignment by type: RWTX 0x40, MODL 0x10, SIMP 0x20.
    public Asset Add(uint layerType, string type, uint id, ReadOnlyMemory<byte> data, string name, uint flags, uint startAlign = 0x20)
    {
        if (byId.ContainsKey(id)) throw new InvalidOperationException($"ID already used: {id:X8}");
        var layer = layers.First(l => l.Type == layerType);
        var asset = new Asset { Id = id, Type = Encoding.ASCII.GetBytes(type), Data = data, Flags = flags, Debug = MakeDebug(name, data.Span), StartAlign = startAlign };
        assets.Add(asset);
        byId[id] = asset;
        layer.Ids.Add(id);
        assets = assets.OrderBy(a => a.Id).ToList();  // OrderBy is stable, like Python's sort
        return asset;
    }

    public byte[] Build()
    {
        // Lay the stream out relative to its start. Existing assets keep their plus unless an added asset follows them.
        uint rel = 0;
        foreach (var layer in layers)
        {
            if (layer.Ids.Count == 0) continue;
            rel = Align(rel, 0x20);
            for (int j = 0; j < layer.Ids.Count; j++)
            {
                var a = byId[layer.Ids[j]];
                a.Relative = rel;
                uint end = rel + (uint)a.Data.Length + a.Plus;
                uint want = j + 1 < layer.Ids.Count ? byId[layer.Ids[j + 1]].StartAlign : 0;
                if (want != 0 && Align(end, want) != end)
                {
                    a.Plus += Align(end, want) - end;
                    end = Align(end, want);
                }
                rel = end;
            }
        }
        uint total = Align(rel, 0x20);

        var packBytes = new MemoryStream();
        foreach (var (tag, value) in pack)
        {
            var v = value;
            if (Encoding.ASCII.GetString(tag) == "PCNT")  // assets, layers, largest asset, largest layer, largest virtual asset; only ever grown
            {
                v = (byte[])value.Clone();
                Put(v, 0, (uint)assets.Count);
                Put(v, 4, (uint)layers.Count);
                Put(v, 8, Math.Max(U32(value, 8), assets.Max(a => (uint)a.Data.Length)));
                uint largestLayer = U32(value, 12);
                foreach (var l in layers)
                {
                    uint sum = 0;
                    foreach (var i in l.Ids) sum += (uint)byId[i].Data.Length + byId[i].Plus;
                    largestLayer = Math.Max(largestLayer, sum);
                }
                Put(v, 12, largestLayer);
            }
            Block(packBytes, tag, v);
        }
        var head = new MemoryStream();
        head.Write("HIPA"u8);
        head.Write(new byte[4]);
        Block(head, "PACK"u8, packBytes.ToArray());

        // STRM tag + size, DHDR + its bytes, DPAK tag + size, padding size, padding, stream
        uint pre = (uint)head.Length + (uint)DictBlock(0).Length + 8 + 8 + (uint)streamHeader.Length + 8 + 4;
        uint streamBase = Align(pre, Math.Max(baseAlign, assets.Max(a => a.StartAlign)));
        var stream = new byte[total];
        Array.Fill(stream, Fill);
        foreach (var a in assets) a.Data.Span.CopyTo(stream.AsSpan((int)a.Relative));

        var dpak = new byte[4 + (streamBase - pre) + total];
        Put(dpak, 0, streamBase - pre);
        Array.Fill(dpak, Fill, 4, (int)(streamBase - pre));
        stream.CopyTo(dpak, (int)(4 + streamBase - pre));

        var strm = new MemoryStream();
        Block(strm, "DHDR"u8, streamHeader);
        Block(strm, "DPAK"u8, dpak);
        head.Write(DictBlock(streamBase));
        Block(head, "STRM"u8, strm.ToArray());
        return head.ToArray();
    }

    byte[] DictBlock(uint streamBase)
    {
        var toc = new MemoryStream();
        Block(toc, "AINF"u8, assetInfo);
        foreach (var a in assets)
        {
            var body = new MemoryStream();
            var fixedPart = new byte[24];
            Put(fixedPart, 0, a.Id);
            a.Type.CopyTo(fixedPart, 4);
            Put(fixedPart, 8, streamBase + a.Relative);
            Put(fixedPart, 12, (uint)a.Data.Length);
            Put(fixedPart, 16, a.Plus);
            Put(fixedPart, 20, a.Flags);
            body.Write(fixedPart);
            Block(body, "ADBG"u8, a.Debug);
            Block(toc, "AHDR"u8, body.ToArray());
        }
        var layerToc = new MemoryStream();
        Block(layerToc, "LINF"u8, layerInfo);
        foreach (var l in layers)
        {
            var body = new byte[8 + 4 * l.Ids.Count + l.Rest.Length];
            Put(body, 0, l.Type);
            Put(body, 4, (uint)l.Ids.Count);
            for (int i = 0; i < l.Ids.Count; i++) Put(body, 8 + 4 * i, l.Ids[i]);
            l.Rest.CopyTo(body, 8 + 4 * l.Ids.Count);
            Block(layerToc, "LHDR"u8, body);
        }
        var inner = new MemoryStream();
        Block(inner, "ATOC"u8, toc.ToArray());
        Block(inner, "LTOC"u8, layerToc.ToArray());
        var whole = new MemoryStream();
        Block(whole, "DICT"u8, inner.ToArray());
        return whole.ToArray();
    }

    // The game's asset ID hash of a name. Every RWTX ID on the disc equals this of its name, and materials find their
    // textures by it. Names are checked to be Latin-1 before they get here.
    public static uint HashName(string name)
    {
        uint h = 0;
        foreach (var raw in Encoding.Latin1.GetBytes(name))
        {
            uint c = raw;
            if (c is >= 97 and <= 122) c -= 32;
            h = unchecked(h * 131 + c);
        }
        return h;
    }

    // Empty file name, the asset's name, and a checksum of its bytes.
    static byte[] MakeDebug(string name, ReadOnlySpan<byte> data)
    {
        var n = Encoding.Latin1.GetBytes(name);
        int nameLength = n.Length + 1;
        if ((nameLength & 1) != 0) nameLength++;
        var debug = new byte[4 + nameLength + 2 + 4];
        n.CopyTo(debug, 4);
        Put(debug, 4 + nameLength + 2, Crc32(data));
        return debug;
    }

    static readonly uint[] CrcTable = MakeCrcTable();

    static uint[] MakeCrcTable()
    {
        var table = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return table;
    }

    static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint c = 0xFFFFFFFFu;
        foreach (var b in data) c = CrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
        return ~c;
    }

    static uint Align(uint x, uint a) => (x + a - 1) & ~(a - 1);
    static uint U32(byte[] d, int o) => BinaryPrimitives.ReadUInt32BigEndian(d.AsSpan(o, 4));
    static void Put(byte[] d, int o, uint v) => BinaryPrimitives.WriteUInt32BigEndian(d.AsSpan(o, 4), v);
    static int Size(byte[] d, int o) => checked((int)U32(d, o + 4));
    static byte[] Slice(byte[] d, int o, int n) => d.AsSpan(o, n).ToArray();

    static void Expect(byte[] d, int o, string tag)
    {
        if (o < 0 || o + 4 > d.Length || Encoding.ASCII.GetString(d, o, 4) != tag) throw new InvalidDataException($"not a readable level file (no {tag})");
    }

    static void Block(Stream s, ReadOnlySpan<byte> tag, byte[] body)
    {
        s.Write(tag);
        Span<byte> size = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(size, (uint)body.Length);
        s.Write(size);
        s.Write(body);
    }
}
