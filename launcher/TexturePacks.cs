using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace FrightWorks;

// Optional HD texture packs (Extras > Textures). They hold pictures made from the game's own textures, so no
// build ships one and players opt in to each (maintainer, 2026-11-24): the launcher downloads a pack only when the
// player chooses it, checks it against the checksum below and unpacks it into UserData/texture_packs/<Folder>. The game
// indexes every folder there after the player's own texture_replacements and the mods, the lower folder name first (it
// wins); a "disabled" file in the folder switches a pack off (texture_replacement.cpp build_index). Folders the player put
// there are packs too. The launcher keeps the player's order in the names: it renumbers them 10-, 20-, ... (Reorder).
static class TexturePacks
{
    // Folder: the install folder, its number the priority (lower wins). Hash: of the downloaded file. Archive: "rar"
    // (unpacked with the system's tar/bsdtar, or 7-Zip or unrar) or "zip". The textures may sit in one folder inside
    // the archive (dogerki's are in GIHP78): a lone folder at the top is stepped into.
    public sealed record Pack(string Folder, string Name, string Credit, string About, long ApproxBytes, string Url,
                              HashAlgorithmName HashKind, string Hash, string Archive);

    // The menu pack has a lower number than Scooby-HD, so its menus and Creeper frames win over Scooby-HD's.
    public static readonly Pack[] All =
    {
        new("50-scoobyhd", "Scooby-HD", "dogerki",
            "Nearly every texture in the game redrawn at 6x with AI upscaling (1670 textures).",
            604L << 20,
            "https://archive.org/download/dolphin-scooby-doo-night-of-100-frights-hd-texture-pack/" +
            "Dolphin%20Scooby%20Doo%20Night%20of%20100%20Frights%20HD%20Texture%20Pack.rar",
            HashAlgorithmName.SHA1, "1E55E7F802BD0E5AD00538FB21D17A66417F723B", "rar"),
        new("40-menu", "Menu pack", "Loneranger419",
            "The menu screens (title, main menu, options, pause menu and maps) redrawn at 8x, and the HD Creeper window in the hub.",
            53277774,
            "https://archive.org/download/no100f-menu-pack-1.0.0/no100f-menu-pack-1.0.0.zip",
            HashAlgorithmName.SHA256, "349AD55E74C68CCCB2BE66725676D543508CD46F1F2B4D2E3AD3568F85AD88D0", "zip"),
    };

    public static string Dir(string userData) => Path.Combine(userData, "texture_packs");
    // Downloads and unpacking happen beside texture_packs (same drive, so the finished folder is moved, not copied), not
    // in it: the game would index a half-unpacked folder there.
    static string WorkDir(string userData) => Path.Combine(userData, "texture_packs.download");

    // A folder in texture_packs. Known: the catalogue pack it holds, told by the name after the number, so a pack stays
    // known after it is renumbered.
    public sealed record Installed(string Folder, string Name, Pack? Known, bool Enabled);

    // The number in front of a folder name ("50-" in "50-scoobyhd"); "~3-" is a name left half way through a Reorder.
    static readonly Regex Number = new(@"^~*(\d+)-", RegexOptions.CultureInvariant);

    public static string BaseName(string folder)
    {
        var m = Number.Match(folder);
        return m.Success && m.Length < folder.Length ? folder[m.Length..] : folder;
    }

    static int DefaultNumber(Pack p) => int.Parse(Number.Match(p.Folder).Groups[1].Value);

    // Every pack folder in the game's order: it sorts the paths, which compares the names character by character.
    public static List<Installed> List(string userData)
    {
        var dir = Dir(userData);
        if (!Directory.Exists(dir)) return new();
        return Directory.GetDirectories(dir).Select(Path.GetFileName).OfType<string>().OrderBy(n => n, StringComparer.Ordinal)
            .Select(f =>
            {
                var known = All.FirstOrDefault(p => BaseName(p.Folder).Equals(BaseName(f), StringComparison.OrdinalIgnoreCase));
                return new Installed(f, known?.Name ?? BaseName(f), known, !File.Exists(Path.Combine(dir, f, "disabled")));
            }).ToList();
    }

    public static Installed? Find(string userData, Pack p) => List(userData).FirstOrDefault(i => i.Known == p);
    public static bool IsInstalled(string userData, Pack p) => Find(userData, p) != null;

    public static void SetEnabled(string userData, string folder, bool on)
    {
        var flag = Path.Combine(Dir(userData), folder, "disabled");
        if (!Directory.Exists(Path.GetDirectoryName(flag))) return;
        if (on) File.Delete(flag);
        else File.WriteAllText(flag, "");
    }

    // Only a catalogue pack is ever deleted by the launcher; a folder the player put there is theirs.
    public static void Remove(string userData, Pack p)
    {
        if (Find(userData, p) is { } found) Directory.Delete(Path.Combine(Dir(userData), found.Folder), true);
    }

    // Gives the folders (current names, all of texture_packs) the numbers for this order: 10-, 20-, ... (three digits past
    // nine packs, so the names still sort as the numbers do). Goes through temporary names so a new name never meets an
    // old one, and puts back what it renamed when a rename fails (a file in use, say).
    public static void Reorder(string userData, IReadOnlyList<string> folders)
    {
        var dir = Dir(userData);
        string width = folders.Count * 10 < 100 ? "D2" : "D3";
        var renames = folders.Select((f, i) => (From: f, Temp: $"~{i}-{BaseName(f)}", To: $"{((i + 1) * 10).ToString(width)}-{BaseName(f)}"))
                             .Where(r => r.From != r.To).ToList();
        // A temporary name is never another folder's (a folder may already have one, left by a Reorder cut short).
        for (int i = 0; i < renames.Count; i++)
        {
            var r = renames[i];
            while (r.Temp != r.From && (Directory.Exists(Path.Combine(dir, r.Temp)) || renames.Take(i).Any(o => o.Temp == r.Temp))) r.Temp = "~" + r.Temp;
            renames[i] = r;
        }
        var done = new List<(string From, string To)>();
        void Move(string from, string to)
        {
            Directory.Move(Path.Combine(dir, from), Path.Combine(dir, to));
            done.Add((from, to));
        }
        try
        {
            foreach (var r in renames.Where(r => r.From != r.Temp)) Move(r.From, r.Temp);
            foreach (var r in renames) Move(r.Temp, r.To);
        }
        catch
        {
            for (int i = done.Count - 1; i >= 0; i--)
                try { Directory.Move(Path.Combine(dir, done[i].To), Path.Combine(dir, done[i].From)); } catch { }
            throw;
        }
    }

    // Where a new download goes: above the first catalogue pack that comes after it in the catalogue's own order (the
    // menu pack above Scooby-HD), else at the bottom; folders the player ordered keep their places.
    static void Place(string userData, Pack p)
    {
        var all = List(userData);
        var placed = all.FirstOrDefault(i => i.Known == p);
        if (placed == null) return;
        var order = all.Where(i => i != placed).ToList();
        int at = order.FindIndex(i => i.Known != null && DefaultNumber(i.Known) > DefaultNumber(p));
        order.Insert(at < 0 ? order.Count : at, placed);
        Reorder(userData, order.Select(i => i.Folder).ToList());
    }

    public static async Task Download(string userData, Pack p, IProgress<(long done, long total)> progress, CancellationToken cancel)
    {
        var work = WorkDir(userData);
        if (Directory.Exists(work)) Directory.Delete(work, true);
        Directory.CreateDirectory(work);
        try
        {
            var archive = Path.Combine(work, "pack." + p.Archive);
            using (var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan })
            {
                http.DefaultRequestHeaders.UserAgent.ParseAdd("FrightWorks-Launcher");
                using var response = await http.GetAsync(p.Url, HttpCompletionOption.ResponseHeadersRead, cancel);
                response.EnsureSuccessStatusCode();
                long total = response.Content.Headers.ContentLength ?? 0, done = 0;
                await using var input = await response.Content.ReadAsStreamAsync(cancel);
                await using var output = File.Create(archive);
                var buffer = new byte[1 << 16];
                for (int n; (n = await input.ReadAsync(buffer, cancel)) > 0;)
                {
                    await output.WriteAsync(buffer.AsMemory(0, n), cancel);
                    done += n;
                    progress.Report((done, total));
                }
            }
            string got;
            await using (var stream = File.OpenRead(archive))
                got = Convert.ToHexString(p.HashKind == HashAlgorithmName.SHA1 ? await SHA1.HashDataAsync(stream, cancel)
                                                                               : await SHA256.HashDataAsync(stream, cancel));
            if (!got.Equals(p.Hash, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"the download is damaged ({p.HashKind.Name} {got}, expected {p.Hash})");

            var unpacked = Path.Combine(work, "unpacked");
            Directory.CreateDirectory(unpacked);
            await Task.Run(() =>
            {
                if (p.Archive == "zip") ZipFile.ExtractToDirectory(archive, unpacked);
                else UnpackWithTool(archive, unpacked, cancel);
            }, cancel);
            File.Delete(archive);
            var root = unpacked;
            while (Directory.GetFiles(root).Length == 0 && Directory.GetDirectories(root) is [var only]) root = only;
            if (!Directory.EnumerateFiles(root, "*.png", SearchOption.AllDirectories).Any() &&
                !Directory.EnumerateFiles(root, "*.dds", SearchOption.AllDirectories).Any())
                throw new InvalidDataException("there were no textures in the download");

            Directory.CreateDirectory(Dir(userData));
            if (Find(userData, p) is { } old) Directory.Delete(Path.Combine(Dir(userData), old.Folder), true);
            var target = Path.Combine(Dir(userData), p.Folder);
            if (Directory.Exists(target)) Directory.Delete(target, true);
            Directory.Move(root, target);
            Place(userData, p);
        }
        finally
        {
            try { Directory.Delete(work, true); } catch { }
        }
    }

    // RAR5 (dogerki's pack): libarchive's tar reads it (Windows 10 and later ship it as System32\tar.exe; on Linux it is
    // bsdtar), else 7-Zip or unrar. Like the build's tools/stage-assets.py did while builds shipped the pack.
    static void UnpackWithTool(string archive, string into, CancellationToken cancel)
    {
        var tries = new List<(string exe, string[] args)>();
        if (OperatingSystem.IsWindows())
        {
            var sys = Environment.GetFolderPath(Environment.SpecialFolder.System);
            tries.Add((Path.Combine(sys, "tar.exe"), new[] { "-xf", archive, "-C", into }));
            var pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            tries.Add((Path.Combine(pf, "7-Zip", "7z.exe"), new[] { "x", "-y", "-o" + into, archive }));
        }
        else
        {
            tries.Add(("bsdtar", new[] { "-xf", archive, "-C", into }));
            foreach (var seven in new[] { "7z", "7zz", "7za" }) tries.Add((seven, new[] { "x", "-y", "-o" + into, archive }));
            tries.Add(("unrar", new[] { "x", "-y", archive, into + Path.DirectorySeparatorChar }));
        }
        foreach (var (exe, args) in tries)
        {
            if (Path.IsPathRooted(exe) && !File.Exists(exe)) continue;
            var start = new ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var a in args) start.ArgumentList.Add(a);
            Process? proc;
            try { proc = Process.Start(start); }
            catch (Exception) { continue; }  // not installed
            if (proc == null) continue;
            using (proc)
            {
                using var stop = cancel.Register(() => { try { proc.Kill(true); } catch { } });
                _ = proc.StandardOutput.ReadToEndAsync();
                _ = proc.StandardError.ReadToEndAsync();
                proc.WaitForExit();
                cancel.ThrowIfCancellationRequested();
                if (proc.ExitCode == 0) return;
            }
            foreach (var entry in Directory.GetFileSystemEntries(into))  // a failed try may leave part of the files
                if (Directory.Exists(entry)) Directory.Delete(entry, true); else File.Delete(entry);
        }
        throw new InvalidOperationException(OperatingSystem.IsWindows()
            ? "the pack is a RAR file and Windows' tar couldn't unpack it (installing 7-Zip would help)"
            : "the pack is a RAR file: install bsdtar (libarchive-tools), 7-Zip or unrar to unpack it");
    }
}
