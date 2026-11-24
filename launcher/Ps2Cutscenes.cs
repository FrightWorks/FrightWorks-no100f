using System.Buffers.Binary;
using System.Text;
using FrightWorks.ModPatches;

namespace FrightWorks;

// The PS2 and Xbox versions' live cutscenes, from the player's own PS2 or Xbox disc image. The GameCube shows a movie
// for each story scene; its levels kept everything around them (the cutscene managers, the cutscene_toc naming each
// cutscene by ID with a copy of its header, the soundtracks) but not the cutscenes (CSN assets) themselves. The PS2 and
// Xbox levels have them (the same files, byte for byte; only the Xbox has the ending, cin_11: the PS2 has no s006).
// This copies each CSN whose ID and header match the GameCube level's cutscene_toc into
// <UserData>/ps2/cutscenes/<name>.csn (the folder name predates the Xbox source), where the game finds them
// (wiicompiled/runtime/src/hle/no100f_cutscene.cpp); with none there it shows the movies.
// The copies are the player's disc data, like the extracted disc: never commit, share or ship them.
// No window code here, so tools/hip-patch-tests can run it.
static class Ps2Cutscenes
{
    public const int ExpectedPs2 = 13;   // the USA PS2 disc (SLUS_203.49): cin_1 to cin_10 and three more
    public const int ExpectedXbox = 14;  // the USA Xbox disc: the same 13 and the ending, cin_11
    const int HeaderCompare = 0x40;  // fixed header and soundtrack name; later bytes are per-entry tables

    public static string Dir(string userData) => Path.Combine(userData, "ps2", "cutscenes");

    public static int Count(string userData)
    {
        try { return Directory.Exists(Dir(userData)) ? Directory.GetFiles(Dir(userData), "*.csn").Length : 0; }
        catch (Exception) { return 0; }
    }

    // Expected: how many cutscenes that kind of disc has (0 when the disc wasn't read).
    public sealed record Result(int Copied, string Problem, int Expected = 0);

    // Copies the cutscenes; on any problem the folder is left as it was. progress: (levels done, levels to do, name).
    public static Result Copy(string isoPath, string gcDisc, string userData, Action<int, int, string>? progress = null,
                              CancellationToken cancel = default)
    {
        IDiscImage iso;
        try { iso = OpenImage(isoPath); }
        catch (InvalidDataException e) { return new Result(0, e.Message); }
        using (iso)
        {
            int expected = iso is Xdvdfs ? ExpectedXbox : ExpectedPs2;
            var ps2Levels = new Dictionary<string, DiscEntry>(StringComparer.OrdinalIgnoreCase);
            foreach (var e in iso.Files())
                if (e.Name.EndsWith(".HIP", StringComparison.OrdinalIgnoreCase)) ps2Levels[e.Name] = e;
            if (ps2Levels.Count == 0) return new Result(0, "That disc image has no Night of 100 Frights levels on it.");

            var gcFiles = Path.Combine(gcDisc, "files");
            if (!Directory.Exists(gcFiles)) return new Result(0, "Set up the GameCube disc first: the cutscenes are checked against its levels.");
            var gcLevels = Directory.EnumerateFiles(gcFiles, "*.hip", new EnumerationOptions { RecurseSubdirectories = true, MatchCasing = MatchCasing.CaseInsensitive })
                                    .Where(p => ps2Levels.ContainsKey(Path.GetFileName(p))).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).ToList();

            var dir = Dir(userData);
            var staging = dir + ".new";
            TryDelete(staging);
            Directory.CreateDirectory(staging);
            int copied = 0, done = 0;
            try
            {
                foreach (var gcPath in gcLevels)
                {
                    cancel.ThrowIfCancellationRequested();
                    var name = Path.GetFileName(gcPath);
                    progress?.Invoke(done++, gcLevels.Count, name);
                    var gcBytes = File.ReadAllBytes(gcPath);
                    if (gcBytes.AsSpan().IndexOf("CTOC"u8) < 0) continue;  // no cutscene_toc: nothing to look for
                    var tocs = TocHeaders(new HipFile(gcBytes));
                    if (tocs.Count == 0) continue;
                    var ps2Bytes = iso.Read(ps2Levels[name]);
                    if (ps2Bytes.AsSpan().IndexOf("CSN "u8) < 0) continue;
                    HipFile ps2;
                    try { ps2 = new HipFile(ps2Bytes); }
                    catch (InvalidDataException) { continue; }
                    foreach (var a in ps2.Assets)
                    {
                        if (a.TypeName != "CSN " || !tocs.TryGetValue(a.Id, out var toc) || !HeaderMatches(toc, a.Data.Span)) continue;
                        File.WriteAllBytes(Path.Combine(staging, FileName(a)), a.Data.ToArray());
                        copied++;
                    }
                }
                progress?.Invoke(done, gcLevels.Count, "");
                if (copied == 0)
                {
                    TryDelete(staging);
                    return new Result(0, "No cutscenes on that disc match this game's levels. It needs Scooby-Doo! Night of 100 Frights for the PS2 or Xbox.");
                }
                TryDelete(dir);
                Directory.CreateDirectory(Path.GetDirectoryName(dir)!);
                Directory.Move(staging, dir);
                return new Result(copied, "", expected);
            }
            catch (Exception) { TryDelete(staging); throw; }
        }
    }

    public static void Remove(string userData) => TryDelete(Dir(userData));

    // CSN ID -> the copy of its header that the GameCube cutscene_toc keeps (little-endian, starting with 'CTSN').
    static Dictionary<uint, ReadOnlyMemory<byte>> TocHeaders(HipFile level)
    {
        var tocs = new Dictionary<uint, ReadOnlyMemory<byte>>();
        foreach (var a in level.Assets)
        {
            if (a.TypeName != "CTOC") continue;
            var d = a.Data;
            int i = d.Span.IndexOf("CTSN"u8);
            while (i >= 0 && i + 8 <= d.Length)
            {
                int next = d.Span[(i + 4)..].IndexOf("CTSN"u8);
                int end = next < 0 ? d.Length : i + 4 + next;
                tocs[BinaryPrimitives.ReadUInt32LittleEndian(d.Span.Slice(i + 4, 4))] = d[i..end];
                i = next < 0 ? -1 : i + 4 + next;
            }
        }
        return tocs;
    }

    static bool HeaderMatches(ReadOnlyMemory<byte> toc, ReadOnlySpan<byte> csn)
    {
        int n = Math.Min(Math.Min(toc.Length, csn.Length), HeaderCompare);
        return n >= 8 && toc.Span[..n].SequenceEqual(csn[..n]);
    }

    // The asset's name (cin_1.csn), or its ID when the name isn't a plain file name.
    static string FileName(HipFile.Asset a)
    {
        var name = a.Name;
        bool plain = name.Length is > 0 and <= 64 && name.All(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-');
        return (plain ? name : a.Id.ToString("X8")) + ".csn";
    }

    static void TryDelete(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
        catch (Exception) { }
    }

    // A file on a disc image: where its bytes start (a sector for ISO 9660, a byte offset for XDVDFS) and its size.
    internal sealed record DiscEntry(string Name, long Lba, long Size);

    internal interface IDiscImage : IDisposable
    {
        List<DiscEntry> Files();
        byte[] Read(DiscEntry e);
    }

    // A PS2 image is ISO 9660; an Xbox image is XDVDFS (a trimmed .iso, or a full dump of the disc). XDVDFS is tried first:
    // Xbox images can also carry an empty ISO 9660 volume (seen on the USA disc's image).
    internal static IDiscImage OpenImage(string path)
    {
        if (Xdvdfs.Open(path) is { } xbox) return xbox;
        try { return new Iso9660(path); }
        catch (InvalidDataException) { }
        throw new InvalidDataException("That file is not a PS2 or Xbox disc image (use a plain .iso dump of the disc).");
    }

    // Just enough ISO 9660 to read files by name: 2048-byte sectors (.iso), or raw 2352-byte sectors (.bin).
    sealed class Iso9660 : IDiscImage
    {
        readonly FileStream file;
        readonly int sectorSize, dataOffset;

        public Iso9660(string path)
        {
            file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.RandomAccess);
            foreach (var (size, offset) in new[] { (2048, 0), (2352, 24), (2352, 16) })
            {
                var pvd = ReadRaw(16L * size + offset, 6);
                if (pvd.Length == 6 && pvd[0] == 1 && Encoding.ASCII.GetString(pvd, 1, 5) == "CD001")
                {
                    (sectorSize, dataOffset) = (size, offset);
                    return;
                }
            }
            file.Dispose();
            throw new InvalidDataException("That file is not an ISO 9660 disc image.");
        }

        byte[] ReadRaw(long off, int count)
        {
            var b = new byte[count];
            int n = off < file.Length ? RandomAccess.Read(file.SafeFileHandle, b, off) : 0;
            return n == count ? b : b[..n];
        }

        // count bytes from sector lba on, skipping each raw sector's header and error correction.
        byte[] ReadSectors(long lba, long count)
        {
            if (count < 0 || count > int.MaxValue) throw new InvalidDataException("a file on the disc image is too big");
            var dst = new byte[count];
            if (sectorSize == 2048)
            {
                int n = RandomAccess.Read(file.SafeFileHandle, dst, lba * 2048);
                if (n < count) throw new InvalidDataException("The disc image is cut short.");
                return dst;
            }
            for (long done = 0; done < count; lba++)
            {
                int n = (int)Math.Min(2048, count - done);
                if (RandomAccess.Read(file.SafeFileHandle, dst.AsSpan((int)done, n), lba * sectorSize + dataOffset) < n)
                    throw new InvalidDataException("The disc image is cut short.");
                done += n;
            }
            return dst;
        }

        public byte[] Read(DiscEntry e) => ReadSectors(e.Lba, e.Size);

        public List<DiscEntry> Files()
        {
            var files = new List<DiscEntry>();
            var pvd = ReadSectors(16, 2048);
            var root = pvd.AsSpan(156, 34);
            var pending = new Stack<(long Lba, long Size, int Depth)>();
            pending.Push((BinaryPrimitives.ReadUInt32LittleEndian(root[2..]), BinaryPrimitives.ReadUInt32LittleEndian(root[10..]), 0));
            var seen = new HashSet<long>();
            while (pending.Count > 0)
            {
                var (lba, size, depth) = pending.Pop();
                if (depth > 8 || size > (1 << 20) || !seen.Add(lba)) continue;
                var d = ReadSectors(lba, size);
                for (int i = 0; i < d.Length;)
                {
                    int n = d[i];
                    if (n == 0) { i = (i / 2048 + 1) * 2048; continue; }  // records don't cross sectors
                    if (n < 34 || i + n > d.Length) break;
                    var r = d.AsSpan(i, n);
                    long extent = BinaryPrimitives.ReadUInt32LittleEndian(r[2..]), length = BinaryPrimitives.ReadUInt32LittleEndian(r[10..]);
                    int nameLength = r[32];
                    if (33 + nameLength <= n)
                    {
                        var name = Encoding.Latin1.GetString(r.Slice(33, nameLength)).Split(';')[0];
                        bool directory = (r[25] & 2) != 0;
                        if (directory && nameLength == 1 && r[33] <= 1) { }  // . and ..
                        else if (directory) pending.Push((extent, length, depth + 1));
                        else files.Add(new DiscEntry(name, extent, length));
                    }
                    i += n;
                }
            }
            return files;
        }

        public void Dispose() => file.Dispose();
    }
    // Just enough XDVDFS (the Xbox disc file system) to read files by name. The volume descriptor is at sector 32 of the
    // game partition, which starts at 0 in a trimmed image and further in on a full dump of the disc. Each directory is
    // a binary tree of 4-byte-aligned entries: left and right child (in 4-byte units from the directory's start, 0 =
    // none), first sector, size, attributes (0x10 = directory), name length, name. 0xFF pads the rest of a sector.
    internal sealed class Xdvdfs : IDiscImage
    {
        static readonly long[] PartitionStarts = { 0, 0x18300000, 0xFD90000, 0x2080000 };  // trimmed, XGD1, XGD2, XGD3
        const int Sector = 2048;

        readonly FileStream file;
        readonly long start, rootSector, rootSize;

        Xdvdfs(FileStream file, long start, long rootSector, long rootSize)
        {
            (this.file, this.start, this.rootSector, this.rootSize) = (file, start, rootSector, rootSize);
        }

        public static Xdvdfs? Open(string path)
        {
            var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.RandomAccess);
            var head = new byte[28];
            foreach (var at in PartitionStarts)
            {
                long off = at + 32L * Sector;
                if (off + head.Length > file.Length || RandomAccess.Read(file.SafeFileHandle, head, off) < head.Length) continue;
                if (!head.AsSpan(0, 20).SequenceEqual("MICROSOFT*XBOX*MEDIA"u8)) continue;
                return new Xdvdfs(file, at, BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(20)),
                                  BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(24)));
            }
            file.Dispose();
            return null;
        }

        public byte[] Read(DiscEntry e)
        {
            if (e.Size < 0 || e.Size > int.MaxValue) throw new InvalidDataException("a file on the disc image is too big");
            var dst = new byte[e.Size];
            if (e.Size > 0 && RandomAccess.Read(file.SafeFileHandle, dst, e.Lba) < e.Size)
                throw new InvalidDataException("The disc image is cut short.");
            return dst;
        }

        public List<DiscEntry> Files()
        {
            var files = new List<DiscEntry>();
            var pending = new Stack<(long Sector, long Size, int Depth)>();
            pending.Push((rootSector, rootSize, 0));
            var seen = new HashSet<long>();
            while (pending.Count > 0)
            {
                var (sector, size, depth) = pending.Pop();
                if (depth > 8 || size <= 0 || size > (1 << 20) || !seen.Add(sector)) continue;
                var d = Read(new DiscEntry("", start + sector * Sector, size));
                var nodes = new Stack<int>();
                var visited = new HashSet<int>();
                nodes.Push(0);
                while (nodes.Count > 0)
                {
                    int at = nodes.Pop() * 4;
                    if (at + 14 > d.Length || !visited.Add(at)) continue;
                    ushort left = BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(at));
                    ushort right = BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(at + 2));
                    if (left == 0xFFFF && right == 0xFFFF) continue;  // padding: an empty directory
                    long first = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at + 4));
                    long length = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at + 8));
                    byte attributes = d[at + 12], nameLength = d[at + 13];
                    if (at + 14 + nameLength > d.Length) continue;
                    var name = Encoding.Latin1.GetString(d, at + 14, nameLength);
                    if ((attributes & 0x10) != 0) pending.Push((first, length, depth + 1));
                    else files.Add(new DiscEntry(name, start + first * Sector, length));
                    if (left != 0) nodes.Push(left);
                    if (right != 0) nodes.Push(right);
                }
            }
            return files;
        }

        public void Dispose() => file.Dispose();
    }
}
