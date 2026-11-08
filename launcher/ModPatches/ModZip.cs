using System.IO.Compression;

namespace FrightWorks.ModPatches;

// Installs a mod from a zip. Zips are untrusted: nothing outside the mod folder, no links, size limits. Everything is
// unpacked and checked in the cache first, and the mods folder only changes in the final move, so a bad zip leaves it as it was.
static class ModZip
{
    public const long MaxTotalBytes = 2L << 30, MaxFileBytes = 1L << 30;
    public const int MaxEntries = 20000;
    const int MaxRatio = 1000;     // a file that expands more than this is treated as a zip bomb
    const long RatioFloor = 1 << 20;  // small files can compress well without meaning anything
    const string StagingName = "mod-install";

    public sealed record Prepared(string Name, string Dir, string Staging);

    // Unpacks and checks. Throws PatchException with the reason, leaving nothing behind.
    public static Prepared Prepare(string zipPath, string cacheDir)
    {
        RemoveLeftovers(Path.Combine(cacheDir, StagingName));
        var staging = Path.Combine(cacheDir, StagingName, Guid.NewGuid().ToString("N"));
        try
        {
            using var zip = OpenZip(zipPath);
            if (zip.Entries.Count > MaxEntries) throw new PatchException($"This zip has too many files ({zip.Entries.Count}).");
            var entries = zip.Entries.Select(e => (Entry: e, Path: CheckEntry(e))).Where(x => x.Path.Length > 0).ToList();

            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var (_, path) in entries)
                if (!seen.Add(path)) throw new PatchException($"This zip has \"{path}\" twice.");

            // Either the mod's files are at the top of the zip, or everything is inside one folder, which is the mod.
            var tops = entries.Select(x => x.Path.Split('/')[0]).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            bool wrapped = tops.Count == 1 && entries.All(x => x.Path.Contains('/') || x.Entry.FullName.EndsWith('/')) && !IsModMarker(tops[0]);
            var name = wrapped ? tops[0] : Path.GetFileNameWithoutExtension(zipPath);
            CheckModName(name);

            var dir = Path.Combine(staging, name);
            Directory.CreateDirectory(dir);
            long total = 0;
            foreach (var (entry, path) in entries)
            {
                var relative = wrapped ? path.Contains('/') ? path[(path.IndexOf('/') + 1)..] : "" : path;
                if (relative.Length == 0 || entry.FullName.EndsWith('/')) continue;
                var target = Path.Combine(dir, relative.Replace('/', Path.DirectorySeparatorChar));
                if (!Path.GetFullPath(target).StartsWith(Path.GetFullPath(dir) + Path.DirectorySeparatorChar, StringComparison.Ordinal)) throw new PatchException($"\"{path}\" isn't a path inside the mod folder.");
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                total += Extract(entry, target, MaxTotalBytes - total);
            }
            CheckIsMod(dir);
            return new Prepared(name, dir, staging);
        }
        catch (Exception e)
        {
            TryDelete(staging);
            if (e is PatchException) throw;
            throw new PatchException("This zip couldn't be read: " + e.Message);
        }
    }

    // True when a mod with this name is already there, so the caller can ask before replacing it.
    public static bool Exists(Prepared mod, string modsDir) => Directory.Exists(Path.Combine(modsDir, mod.Name));

    // The only step that changes the mods folder. Replacing moves the old one aside first and puts it back if the new one won't go in.
    public static void Commit(Prepared mod, string modsDir)
    {
        string? stranded = null;
        try
        {
            Directory.CreateDirectory(modsDir);
            var target = Path.Combine(modsDir, mod.Name);
            string? aside = null;
            if (Directory.Exists(target))
            {
                aside = Path.Combine(mod.Staging, "old");
                Directory.Move(target, aside);
            }
            try { Directory.Move(mod.Dir, target); }
            catch
            {
                stranded = aside;
                if (aside != null) { Directory.Move(aside, target); stranded = null; }
                throw;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw new PatchException("The mod couldn't be put in the mods folder: " + e.Message + (stranded != null ? $"\n\nYour old copy of it is in {stranded}." : ""));
        }
        finally { if (stranded == null) Discard(mod); }
    }

    // A launcher killed part way leaves its unpacked zip. One holding an "old" folder has a mod that was being replaced: never delete that.
    static void RemoveLeftovers(string root)
    {
        if (!Directory.Exists(root)) return;
        foreach (var dir in Directory.GetDirectories(root))
            if (!Directory.Exists(Path.Combine(dir, "old"))) TryDelete(dir);
    }

    public static void Discard(Prepared mod) => TryDelete(mod.Staging);

    static ZipArchive OpenZip(string zipPath)
    {
        try { return ZipFile.OpenRead(zipPath); }
        catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException) { throw new PatchException("This isn't a zip that can be read: " + e.Message); }
    }

    // The entry's path with forward slashes and no trailing slash, after the checks that make it safe to unpack.
    static string CheckEntry(ZipArchiveEntry entry)
    {
        int unixMode = (entry.ExternalAttributes >> 16) & 0xF000;
        if (unixMode == 0xA000) throw new PatchException($"\"{entry.FullName}\" is a link, which mods can't use.");
        if (unixMode != 0 && unixMode != 0x8000 && unixMode != 0x4000) throw new PatchException($"\"{entry.FullName}\" isn't a plain file.");
        if ((entry.ExternalAttributes & (int)FileAttributes.ReparsePoint) != 0) throw new PatchException($"\"{entry.FullName}\" is a link, which mods can't use.");
        var path = entry.FullName.Replace('\\', '/');
        if (path.EndsWith('/')) path = path[..^1];
        if (path.Length == 0) return "";
        ModFiles.CheckRelativePath(path);
        if (entry.Length > MaxFileBytes) throw new PatchException($"\"{path}\" is too large.");
        if (entry.Length > RatioFloor && entry.CompressedLength > 0 && entry.Length / entry.CompressedLength > MaxRatio) throw new PatchException($"\"{path}\" expands too much to be a real file.");
        return path;
    }

    // Copies with the limit counted on what really comes out: a zip's headers can lie about the size.
    static long Extract(ZipArchiveEntry entry, string target, long allowed)
    {
        using var input = entry.Open();
        using var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write);
        var buffer = new byte[81920];
        long written = 0;
        int n;
        while ((n = input.Read(buffer, 0, buffer.Length)) > 0)
        {
            written += n;
            if (written > allowed || written > MaxFileBytes) throw new PatchException("This zip is too large to be a mod.");
            output.Write(buffer, 0, n);
        }
        return written;
    }

    static bool IsModMarker(string top) => top.Equals(PatchRecipe.FileName, StringComparison.OrdinalIgnoreCase) || top.Equals("files", StringComparison.OrdinalIgnoreCase) || top.Equals("textures", StringComparison.OrdinalIgnoreCase);

    static void CheckModName(string name)
    {
        ModFiles.CheckName(name, name);
        if (name.StartsWith('!') || name.Length > 64 || name.Contains('/')) throw new PatchException($"\"{name}\" can't be the name of a mod.");
    }

    static void CheckIsMod(string dir)
    {
        bool recipe = File.Exists(Path.Combine(dir, PatchRecipe.FileName));
        if (!recipe && !Directory.Exists(Path.Combine(dir, "files")) && !Directory.Exists(Path.Combine(dir, "textures")))
            throw new PatchException($"This doesn't look like a mod: it has no {PatchRecipe.FileName}, files folder or textures folder.");
        if (recipe) ModFiles.Load(dir);
    }

    static void TryDelete(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
