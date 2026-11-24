using System.Buffers.Binary;
using System.Text;
using FrightWorks;
using FrightWorks.ModPatches;
using HipPatchTests;

// The launcher's cutscene copy (launcher/Ps2Cutscenes.cs) on PS2 and Xbox disc images and a GameCube disc made in code.
static partial class Tests
{
    static void Ps2CutsceneCopy()
    {
        var root = Path.Combine(Path.GetTempPath(), "hip-patch-tests", Guid.NewGuid().ToString("N"));
        try
        {
            var gcDisc = Path.Combine(root, "Disc");
            var userData = Path.Combine(root, "UserData");
            Directory.CreateDirectory(Path.Combine(gcDisc, "files", "hb"));
            Directory.CreateDirectory(userData);

            var cin1 = Csn("cin_1", 0x11);
            var cin2 = Csn("cin_2", 0x22);
            var cin3 = Csn("cin_3", 0x33);
            // GameCube level: a cutscene_toc that names cin_1 with its header and cin_2 with a different one, not cin_3.
            var differing = (byte[])cin2.Clone();
            differing[0x30] ^= 0xFF;
            var toc = Join(new byte[] { 2, 0, 0, 0, 0, 0, 0, 0 }, cin1[..0x50], differing[..0x50]);
            File.WriteAllBytes(Path.Combine(gcDisc, "files", "hb", "h001.HIP"),
                               Fixtures.Level(1, new[] { new Fixtures.A(0, "CTOC", "cutscene_toc", toc, 2) }));
            // PS2 level: the three cutscenes in layer 8.
            var ps2Level = Fixtures.Level(2, new[]
            {
                new Fixtures.A(8, "CSN ", "cin_1", cin1, 0), new Fixtures.A(8, "CSN ", "cin_2", cin2, 0), new Fixtures.A(8, "CSN ", "cin_3", cin3, 0),
            }, layerTypes: new uint[] { 0, 1, 3, 8 });
            var other = Fixtures.Level(3);
            var iso = Path.Combine(root, "ps2.iso");
            var image = Iso(("H0", "H001.HIP", ps2Level), ("H0", "H002.HIP", other));
            File.WriteAllBytes(iso, image);

            var r = Ps2Cutscenes.Copy(iso, gcDisc, userData);
            var copy = Path.Combine(Ps2Cutscenes.Dir(userData), "cin_1.csn");
            Check("PS2: only the cutscene whose ID and header match is copied", r.Copied == 1 && r.Problem == "", $"{r.Copied} '{r.Problem}'");
            Check("PS2: expects the PS2 disc's 13", r.Expected == Ps2Cutscenes.ExpectedPs2, $"{r.Expected}");
            Check("PS2: the copy is byte for byte", File.Exists(copy) && File.ReadAllBytes(copy).AsSpan().SequenceEqual(cin1));
            Check("PS2: count", Ps2Cutscenes.Count(userData) == 1);
            Check("PS2: no staging folder left", !Directory.Exists(Ps2Cutscenes.Dir(userData) + ".new"));

            var raw = Path.Combine(root, "ps2.bin");
            File.WriteAllBytes(raw, Raw2352(image));
            r = Ps2Cutscenes.Copy(raw, gcDisc, userData);
            Check("PS2: a raw 2352-byte sector image works too", r.Copied == 1 && File.Exists(copy), $"{r.Copied} '{r.Problem}'");

            var junk = Path.Combine(root, "junk.iso");
            File.WriteAllBytes(junk, new byte[100_000]);
            r = Ps2Cutscenes.Copy(junk, gcDisc, userData);
            Check("PS2: a file that isn't a disc image is refused", r.Copied == 0 && r.Problem.Length > 0);
            Check("PS2: a refusal keeps the earlier copy", File.Exists(copy));

            var gameCubeOnly = Path.Combine(root, "nolevels.iso");
            File.WriteAllBytes(gameCubeOnly, Iso(("H0", "H002.HIP", other)));
            r = Ps2Cutscenes.Copy(gameCubeOnly, gcDisc, userData);
            Check("PS2: a disc with no matching cutscenes is refused", r.Copied == 0 && r.Problem.Length > 0 && File.Exists(copy));

            r = Ps2Cutscenes.Copy(iso, Path.Combine(root, "NoDisc"), userData);
            Check("PS2: no GameCube disc yet is refused", r.Copied == 0 && r.Problem.Length > 0);

            var cut = Path.Combine(root, "cut.iso");
            File.WriteAllBytes(cut, image[..(image.Length - 4096)]);
            bool threw = false;
            try { Ps2Cutscenes.Copy(cut, gcDisc, userData); }
            catch (InvalidDataException) { threw = true; }
            Check("PS2: a cut-short image fails without touching the copy", threw && File.Exists(copy) && !Directory.Exists(Ps2Cutscenes.Dir(userData) + ".new"));

            // Xbox: the same level in an XDVDFS image, trimmed (partition at 0) and as a full dump (partition further in).
            foreach (var (what, start) in new[] { ("trimmed", 0L), ("full dump", 0x18300000L) })
            {
                Ps2Cutscenes.Remove(userData);
                var xbox = Path.Combine(root, $"xbox-{start:X}.iso");
                var xboxImage = Xiso(start, ("fmv", "cin1.xmv", new byte[3000]), ("h0", "h002.HIP", other), ("h0", "h001.HIP", ps2Level));  // h001 last: the cut-short image cuts it
                File.WriteAllBytes(xbox, xboxImage);
                r = Ps2Cutscenes.Copy(xbox, gcDisc, userData);
                Check($"Xbox ({what}): the matching cutscene is copied byte for byte",
                      r.Copied == 1 && r.Problem == "" && File.Exists(copy) && File.ReadAllBytes(copy).AsSpan().SequenceEqual(cin1), $"{r.Copied} '{r.Problem}'");
                Check($"Xbox ({what}): expects the Xbox disc's 14", r.Expected == Ps2Cutscenes.ExpectedXbox, $"{r.Expected}");
                if (start != 0) continue;
                var xboxCut = Path.Combine(root, "xbox-cut.iso");
                File.WriteAllBytes(xboxCut, xboxImage[..(xboxImage.Length - 4096)]);
                threw = false;
                try { Ps2Cutscenes.Copy(xboxCut, gcDisc, userData); }
                catch (InvalidDataException) { threw = true; }
                Check("Xbox: a cut-short image fails without touching the copy", threw && File.Exists(copy) && !Directory.Exists(Ps2Cutscenes.Dir(userData) + ".new"));
            }

            Ps2Cutscenes.Remove(userData);
            Check("PS2: remove", Ps2Cutscenes.Count(userData) == 0);
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    // A cutscene asset: 'CTSN', its ID (the name hash, as Fixtures gives assets), then bytes from the seed.
    static byte[] Csn(string name, byte seed)
    {
        var d = new byte[0x90];
        "CTSN"u8.CopyTo(d);
        BinaryPrimitives.WriteUInt32LittleEndian(d.AsSpan(4), HipFile.HashName(name));
        for (int i = 8; i < d.Length; i++) d[i] = (byte)(seed + i);
        return d;
    }

    static byte[] Join(params byte[][] parts) => parts.SelectMany(p => p).ToArray();

    // An ISO 9660 image: volume descriptor at sector 16, root at 18, one directory per distinct folder, then the files.
    static byte[] Iso(params (string Folder, string Name, byte[] Data)[] files)
    {
        const int S = 2048;
        var folders = files.Select(f => f.Folder).Distinct().ToList();
        int next = 19 + folders.Count;
        var placed = files.Select(f => { int at = next; next += (f.Data.Length + S - 1) / S; return (f.Folder, f.Name, f.Data, At: at); }).ToList();
        var image = new byte[next * S];
        static byte[] Record(string name, int lba, int size, bool directory)
        {
            var n = name == "\0" || name == "\u0001" ? new[] { (byte)name[0] } : Encoding.ASCII.GetBytes(name);
            var r = new byte[33 + n.Length + (n.Length % 2 == 0 ? 1 : 0)];
            r[0] = (byte)r.Length;
            BinaryPrimitives.WriteUInt32LittleEndian(r.AsSpan(2), (uint)lba);
            BinaryPrimitives.WriteUInt32BigEndian(r.AsSpan(6), (uint)lba);
            BinaryPrimitives.WriteUInt32LittleEndian(r.AsSpan(10), (uint)size);
            BinaryPrimitives.WriteUInt32BigEndian(r.AsSpan(14), (uint)size);
            r[25] = (byte)(directory ? 2 : 0);
            r[32] = (byte)n.Length;
            n.CopyTo(r, 33);
            return r;
        }
        void Directory(int lba, int parent, IEnumerable<byte[]> entries)
        {
            var all = new[] { Record("\0", lba, S, true), Record("\u0001", parent, S, true) }.Concat(entries).SelectMany(e => e).ToArray();
            all.CopyTo(image, lba * S);
        }
        image[16 * S] = 1;
        "CD001"u8.CopyTo(image.AsSpan(16 * S + 1));
        Record("\0", 18, S, true).CopyTo(image, 16 * S + 156);
        Directory(18, 18, folders.Select((f, i) => Record(f, 19 + i, S, true)));
        for (int i = 0; i < folders.Count; i++)
            Directory(19 + i, 18, placed.Where(p => p.Folder == folders[i]).Select(p => Record(p.Name + ";1", p.At, p.Data.Length, false)));
        foreach (var p in placed) p.Data.CopyTo(image, p.At * S);
        return image;
    }

    // An XDVDFS (Xbox) image with its game partition at start: volume descriptor at the partition's sector 32, root
    // directory at 33, one directory per folder after it, then the files (and a decoy ISO 9660 descriptor at sector 16). Each directory is a tree whose entries all
    // hang off the right child of the one before (a valid, if unbalanced, tree), padded with 0xFF to whole sectors.
    static byte[] Xiso(long start, params (string Folder, string Name, byte[] Data)[] files)
    {
        const int S = 2048;
        var folders = files.Select(f => f.Folder).Distinct().ToList();
        int next = 34 + folders.Count;
        var placed = files.Select(f => { int at = next; next += (f.Data.Length + S - 1) / S; return (f.Folder, f.Name, f.Data, At: at); }).ToList();
        var image = new byte[start + (long)next * S];
        byte[] Directory(IEnumerable<(string Name, int Sector, int Size, bool Dir)> entries)
        {
            var list = entries.ToList();
            var d = new List<byte>();
            for (int i = 0; i < list.Count; i++)
            {
                var n = Encoding.ASCII.GetBytes(list[i].Name);
                int length = (14 + n.Length + 3) / 4 * 4;
                var e = Enumerable.Repeat((byte)0xFF, length).ToArray();
                BinaryPrimitives.WriteUInt16LittleEndian(e.AsSpan(0), 0);
                BinaryPrimitives.WriteUInt16LittleEndian(e.AsSpan(2), (ushort)(i + 1 < list.Count ? (d.Count + length) / 4 : 0));
                BinaryPrimitives.WriteUInt32LittleEndian(e.AsSpan(4), (uint)list[i].Sector);
                BinaryPrimitives.WriteUInt32LittleEndian(e.AsSpan(8), (uint)list[i].Size);
                e[12] = (byte)(list[i].Dir ? 0x10 : 0x20);
                e[13] = (byte)n.Length;
                n.CopyTo(e, 14);
                d.AddRange(e);
            }
            while (d.Count % S != 0) d.Add(0xFF);
            return d.ToArray();
        }
        long At(int sector) => start + (long)sector * S;
        image[16 * S] = 1;  // an empty ISO 9660 volume at the start of the image, as real Xbox images can have
        "CD001"u8.CopyTo(image.AsSpan(16 * S + 1));
        "MICROSOFT*XBOX*MEDIA"u8.CopyTo(image.AsSpan((int)At(32)));
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan((int)At(32) + 20), 33);
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan((int)At(32) + 24), S);
        Directory(folders.Select((f, i) => (f, 34 + i, S, true))).CopyTo(image, At(33));
        for (int i = 0; i < folders.Count; i++)
            Directory(placed.Where(p => p.Folder == folders[i]).Select(p => (p.Name, p.At, p.Data.Length, false))).CopyTo(image, At(34 + i));
        foreach (var p in placed) p.Data.CopyTo(image, At(p.At));
        return image;
    }

    // The same image as raw CD sectors (mode 2 form 1: 24 bytes of sync and headers, 2048 of data, 280 of error codes).
    static byte[] Raw2352(byte[] image)
    {
        var raw = new byte[image.Length / 2048 * 2352];
        for (int s = 0; s < image.Length / 2048; s++)
        {
            raw[s * 2352] = 0;
            for (int i = 1; i < 11; i++) raw[s * 2352 + i] = 0xFF;
            Array.Copy(image, s * 2048, raw, s * 2352 + 24, 2048);
        }
        return raw;
    }
}

static class Ps2Mode
{
    // --ps2 ISO DISC USERDATA: copies a real PS2 or Xbox disc's cutscenes into USERDATA/ps2/cutscenes (the player's data:
    // keep it local).
    public static int Run(string[] args)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var r = Ps2Cutscenes.Copy(args[1], args[2], args[3], (done, total, name) => { if (name.Length > 0) Console.Write($"\r{done}/{total} {name}    "); });
        Console.WriteLine();
        Console.WriteLine(r.Problem.Length > 0 ? "PROBLEM: " + r.Problem : $"copied {r.Copied} cutscenes in {sw.Elapsed.TotalSeconds:F1} s");
        foreach (var f in Directory.Exists(Ps2Cutscenes.Dir(args[3])) ? Directory.GetFiles(Ps2Cutscenes.Dir(args[3])) : Array.Empty<string>())
            Console.WriteLine($"  {Path.GetFileName(f)} {new FileInfo(f).Length / 1024} KB");
        Console.WriteLine($"expected {r.Expected}");
        return r.Problem.Length == 0 && r.Copied == r.Expected ? 0 : 1;
    }
}
