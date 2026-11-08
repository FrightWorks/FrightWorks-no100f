using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace FrightWorks;

// Bringing the player's files over from an earlier copy of the game: a FrightWorks or Scooby-Doo: Refrightened zip
// folder (data\UserData, or Play\UserData in old Refrightened ones), or Documents\MarioWiiSaveData (where a FrightWorks
// game without its portable marker kept everything before the installer). Saves go to the saves folder, the disc to
// this game's disc folder, the rest (settings, controls, mods, other texture packs) to UserData. The old folder is only
// read, and files this game already has are never overwritten.
// Offered once at the first start (OfferAtStart); Extras > Folders copies from any folder (FromFolder).
static class DataImport
{
    static readonly string[] GameExes = { DataPaths.GameExe, "Scooby-Doo Refrightened.exe", "Scooby-Doo Re-Masked.exe" };
    const string Mark = "imported.txt", OldMark = "imported-from-refrightened.txt";  // in UserData: asked once
    // Rebuilt by the game or shipped with it.
    static readonly string[] Skip = { "Logs", "Cache", "button_icons", "Disc", Path.Combine("texture_replacements", "ScoobyHD") };

    sealed record Source(string Name, string UserData, string? Disc);

    public static async Task OfferAtStart(DataPaths paths, string title, Window? owner = null)
    {
        if (paths.DevTree) return;
        var userData = paths.UserData;
        if (File.Exists(Path.Combine(userData, Mark)) || File.Exists(Path.Combine(userData, OldMark)) ||
            File.Exists(paths.ConfigPath) || HasFiles(paths.Saves))
            return;
        var source = FindNewest(paths);
        if (source == null) return;
        bool yes = await Dialogs.YesNo(owner,
            $"Found your saves and settings from an earlier version in\n{source.Name}\n\nCopy its saves, settings, controls " +
            "and mods into this one?" + (CopiesDisc(source, paths) ? " Its game files come too, so you don't have to choose the disc again." : "") +
            "\n\nThe old folder is left as it is. Extras > Folders can copy from another folder later.");
        Directory.CreateDirectory(userData);
        if (!yes)
        {
            File.WriteAllText(Path.Combine(userData, Mark), $"Not copied from {source.Name}{Environment.NewLine}");
            return;
        }
        var (error, _) = await Import(source, paths, title, owner);
        if (error != null)
            await Dialogs.Message(owner, $"Copying stopped: {error.Message}\n\nCopy the old folder's files by hand if something is missing.");
        File.WriteAllText(Path.Combine(userData, Mark),
                          $"{(error == null ? "Copied" : "Partly copied")} from {source.Name}{Environment.NewLine}");
    }

    // Extras > Folders: the player picks the old game's folder (or its data, UserData or MarioWiiSaveData folder).
    public static async Task FromFolder(Window owner, DataPaths paths, string title)
    {
        var folder = await ControllerDialogs.PickPath(owner,
            "Choose the folder of the earlier FrightWorks or Refrightened (the one with its launcher), or a folder with a Config.toml", folder: true);
        if (folder == null) return;
        var source = Describe(folder);
        if (source == null || SameFolder(source.UserData, paths.UserData))
        {
            await Dialogs.Message(owner, "No saves or settings of another copy of the game are in\n" + folder);
            return;
        }
        if (!await Dialogs.YesNo(owner, $"Copy the saves, settings, controls and mods from\n{source.Name}?" +
                                        (CopiesDisc(source, paths) ? " The game files come too." : "") +
                                        "\n\nFiles this game already has are kept; the old folder is left as it is."))
            return;
        var (error, skipped) = await Import(source, paths, title, owner);
        await Dialogs.Message(owner, error != null ? $"Copying stopped: {error.Message}"
                                     : skipped > 0 ? $"Done. {skipped} file(s) this game already had were kept as they are."
                                     : "Done.");
    }

    // A folder the player names: the game's own folder, its data\ or Play\, a UserData, or MarioWiiSaveData.
    static Source? Describe(string dir)
    {
        foreach (var sub in new[] { "data", "Play" })
        {
            var game = Path.Combine(dir, sub);
            if (GameExes.Any(e => File.Exists(Path.Combine(game, e))) && File.Exists(Path.Combine(game, "UserData", "Config.toml")))
                return Make(dir, Path.Combine(game, "UserData"));
        }
        if (File.Exists(Path.Combine(dir, "UserData", "Config.toml"))) return Make(dir, Path.Combine(dir, "UserData"));
        if (File.Exists(Path.Combine(dir, "Config.toml"))) return Make(dir, dir);
        return null;
    }

    // The old game's disc: its [paths] dvd_root (relative to its UserData), if that holds this game's disc.
    static Source Make(string name, string userData)
    {
        string? disc = null;
        try
        {
            var dvdRoot = new GameConfig(Path.Combine(userData, "Config.toml")).Get("paths", "dvd_root");
            if (!string.IsNullOrWhiteSpace(dvdRoot))
            {
                var full = Path.GetFullPath(Path.Combine(userData, dvdRoot));
                if (File.Exists(Path.Combine(full, "sys", "boot.bin"))) disc = full;
            }
        }
        catch (Exception) { }
        return new Source(name, userData, disc);
    }

    // The most recently used earlier copy: zip folders beside this one (portable) or in Downloads, Desktop and
    // Documents (one folder deep), and Documents\MarioWiiSaveData if it holds this game's settings.
    static Source? FindNewest(DataPaths paths)
    {
        var places = new List<string>();
        if (paths.Portable)
        {
            try { if (Directory.GetParent(Path.TrimEndingDirectorySeparator(paths.Root)) is { } parent) places.Add(parent.FullName); }
            catch (Exception) { }
        }
        else
        {
            var downloads = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
            foreach (var p in new[] { downloads, Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                                      Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments) })
            {
                if (string.IsNullOrEmpty(p) || !Directory.Exists(p)) continue;
                places.Add(p);
                try { places.AddRange(Directory.EnumerateDirectories(p)); } catch (Exception) { }
            }
        }
        var candidates = new List<Source>();
        foreach (var place in places)
        {
            IEnumerable<string> dirs;
            try { dirs = Directory.EnumerateDirectories(place).ToList(); } catch (Exception) { continue; }
            foreach (var d in dirs)
            {
                if (SameFolder(d, paths.Root)) continue;
                foreach (var sub in new[] { "data", "Play" })
                {
                    var game = Path.Combine(d, sub);
                    try
                    {
                        if (GameExes.Any(e => File.Exists(Path.Combine(game, e))) && File.Exists(Path.Combine(game, "UserData", "Config.toml")))
                            candidates.Add(Make(d, Path.Combine(game, "UserData")));
                    }
                    catch (Exception) { }
                }
            }
        }
        if (!paths.Portable && OperatingSystem.IsWindows())
        {
            var oldData = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "MarioWiiSaveData");
            if (IsThisGamesConfig(Path.Combine(oldData, "Config.toml"))) candidates.Add(Make(oldData, oldData));
        }
        return candidates.Where(c => !SameFolder(c.UserData, paths.UserData))
                         .OrderByDescending(c => LastUsed(c.UserData)).FirstOrDefault();
    }

    static DateTime LastUsed(string userData)
    {
        try { return File.GetLastWriteTimeUtc(Path.Combine(userData, "Config.toml")); } catch (Exception) { return DateTime.MinValue; }
    }

    // MarioWiiSaveData is shared with other WiiCompiled games: only take it when the settings are this game's.
    static bool IsThisGamesConfig(string config)
    {
        try
        {
            if (!File.Exists(config)) return false;
            var text = File.ReadAllText(config);
            return text.Contains("Night of 100 Frights", StringComparison.OrdinalIgnoreCase) ||
                   text.Contains("Scooby", StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception) { return false; }
    }

    static bool CopiesDisc(Source source, DataPaths paths) =>
        source.Disc != null && !File.Exists(Path.Combine(paths.Disc, "sys", "boot.bin")) && !SameFolder(source.Disc, paths.Disc);

    static bool HasFiles(string dir)
    {
        try { return Directory.Exists(dir) && Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Any(); }
        catch (Exception) { return false; }
    }

    static bool SameFolder(string a, string b)
    {
        try
        {
            return string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(a)), Path.TrimEndingDirectorySeparator(Path.GetFullPath(b)),
                                 OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
        }
        catch (Exception) { return false; }
    }

    // Copies everything (the disc into a staging folder first) and points the settings at this game's disc.
    // Returns the error, if any, and how many files were already there.
    static async Task<(Exception? error, int skipped)> Import(Source source, DataPaths paths, string title, Window? owner)
    {
        var files = new List<(string from, string to)>();
        bool configCopied = !File.Exists(paths.ConfigPath);
        var saves = paths.Saves;
        foreach (var f in Directory.EnumerateFiles(source.UserData, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(source.UserData, f);
            if (Skip.Any(s => rel.StartsWith(s + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))) continue;
            if (rel is Mark or OldMark) continue;
            const string savesPrefix = "Saves";
            files.Add(rel.StartsWith(savesPrefix + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
                ? (f, Path.Combine(saves, rel[(savesPrefix.Length + 1)..]))
                : (f, Path.Combine(paths.UserData, rel)));
        }
        bool copyDisc = CopiesDisc(source, paths);
        var staging = paths.Disc + ".new";
        if (copyDisc)
        {
            if (Directory.Exists(staging)) Directory.Delete(staging, true);
            foreach (var f in Directory.EnumerateFiles(source.Disc!, "*", SearchOption.AllDirectories))
                files.Add((f, Path.Combine(staging, Path.GetRelativePath(source.Disc!, f))));
        }
        var (error, skipped) = await Copy(files, title, owner);
        if (error == null && copyDisc)
        {
            try
            {
                if (Directory.Exists(paths.Disc)) Directory.Delete(paths.Disc, true);
                Directory.Move(staging, paths.Disc);
            }
            catch (Exception e) { error = e; }
        }
        if (copyDisc && error != null && Directory.Exists(staging))
        {
            try { Directory.Delete(staging, true); } catch (Exception) { }
        }
        bool discHere = File.Exists(Path.Combine(paths.Disc, "sys", "boot.bin"));
        if ((configCopied && File.Exists(paths.ConfigPath)) || (copyDisc && discHere))
        {
            var cfg = new GameConfig(paths.ConfigPath);
            if (discHere) cfg.Set("paths", "dvd_root", paths.DvdRootValue);
            else if (source.Disc != null) cfg.SetString("paths", "dvd_root", source.Disc);  // the old one, where it is
            if (configCopied) cfg.Remove("paths", "saves_dir");  // the saves were copied to this game's saves folder
            cfg.Save();
        }
        return (error, skipped);
    }

    // Copies with a small progress window; never overwrites a file the game already has.
    static async Task<(Exception? error, int skipped)> Copy(List<(string from, string to)> files, string title, Window? owner)
    {
        Exception? error = null;
        int skipped = 0;
        var dialog = Dialogs.Make(title, owner);
        dialog.WindowDecorations = WindowDecorations.BorderOnly;
        var label = Skin.Note("Copying...");
        label.Foreground = Skin.Brush(Skin.Cream);
        var bar = new ThemedBar { Maximum = Math.Max(1, files.Count), Width = 448 };
        dialog.Content = new Border
        {
            Padding = new Thickness(16, 14, 16, 14),
            Child = new StackPanel { Orientation = Orientation.Vertical, Children = { label, bar } },
        };
        bool done = false;
        dialog.Closing += (_, e) => e.Cancel = !done;  // it closes itself when the copy ends
        dialog.Opened += async (_, _) =>
        {
            var progress = new Progress<int>(i =>
            {
                bar.Value = Math.Min(i, bar.Maximum);
                label.Text = $"Copying: {i}/{files.Count} files";
            });
            try
            {
                await Task.Run(() =>
                {
                    for (int i = 0; i < files.Count; i++)
                    {
                        var (from, to) = files[i];
                        Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                        if (!File.Exists(to)) File.Copy(from, to);
                        else skipped++;
                        if (i % 20 == 0) ((IProgress<int>)progress).Report(i);
                    }
                });
            }
            catch (Exception e) { error = e; }
            done = true;
            Dispatcher.UIThread.Post(dialog.Close);
        };
        await Dialogs.ShowModal(dialog, owner);
        return (error, skipped);
    }
}
