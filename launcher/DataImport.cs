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

    public static void OfferAtStart(DataPaths paths, string title)
    {
        if (paths.DevTree) return;
        var userData = paths.UserData;
        if (File.Exists(Path.Combine(userData, Mark)) || File.Exists(Path.Combine(userData, OldMark)) ||
            File.Exists(paths.ConfigPath) || HasFiles(paths.Saves))
            return;
        var source = FindNewest(paths);
        if (source == null) return;
        var answer = MessageBox.Show(
            $"Found your saves and settings from an earlier version in\n{source.Name}\n\nCopy its saves, settings, controls " +
            "and mods into this one?" + (CopiesDisc(source, paths) ? " Its game files come too, so you don't have to choose the disc again." : "") +
            "\n\nThe old folder is left as it is. Extras > Folders can copy from another folder later.",
            title, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
        Directory.CreateDirectory(userData);
        if (answer != DialogResult.Yes)
        {
            File.WriteAllText(Path.Combine(userData, Mark), $"Not copied from {source.Name}{Environment.NewLine}");
            return;
        }
        var (error, _) = Import(source, paths, title);
        if (error != null)
            MessageBox.Show($"Copying stopped: {error.Message}\n\nCopy the old folder's files by hand if something is missing.",
                            title, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        File.WriteAllText(Path.Combine(userData, Mark),
                          $"{(error == null ? "Copied" : "Partly copied")} from {source.Name}{Environment.NewLine}");
    }

    // Extras > Folders: the player picks the old game's folder (or its data, UserData or MarioWiiSaveData folder).
    public static void FromFolder(IWin32Window owner, DataPaths paths, string title)
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Choose the folder of the earlier FrightWorks or Refrightened (the one with its launcher), or a folder with a Config.toml",
            UseDescriptionForTitle = true,
        };
        if (dialog.ShowDialog(owner) != DialogResult.OK) return;
        var source = Describe(dialog.SelectedPath);
        if (source == null || SameFolder(source.UserData, paths.UserData))
        {
            MessageBox.Show(owner, "No saves or settings of another copy of the game are in\n" + dialog.SelectedPath, title,
                            MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        if (MessageBox.Show(owner, $"Copy the saves, settings, controls and mods from\n{source.Name}?" +
                                   (CopiesDisc(source, paths) ? " The game files come too." : "") +
                                   "\n\nFiles this game already has are kept; the old folder is left as it is.",
                            title, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;
        var (error, skipped) = Import(source, paths, title);
        MessageBox.Show(owner, error != null ? $"Copying stopped: {error.Message}"
                               : skipped > 0 ? $"Done. {skipped} file(s) this game already had were kept as they are."
                               : "Done.", title, MessageBoxButtons.OK, error != null ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
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
        if (!paths.Portable)
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
            return string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(a)),
                                 Path.TrimEndingDirectorySeparator(Path.GetFullPath(b)), StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception) { return false; }
    }

    // Copies everything (the disc into a staging folder first) and points the settings at this game's disc.
    // Returns the error, if any, and how many files were already there.
    static (Exception? error, int skipped) Import(Source source, DataPaths paths, string title)
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
        var (error, skipped) = Copy(files, title);
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
    static (Exception? error, int skipped) Copy(List<(string from, string to)> files, string title)
    {
        Exception? error = null;
        int skipped = 0;
        using var form = new Form
        {
            Text = title, FormBorderStyle = FormBorderStyle.FixedDialog, ControlBox = false, StartPosition = FormStartPosition.CenterScreen,
            ClientSize = new Size(480, 96), Font = new Font("Segoe UI", 10f),
        };
        var label = new Label { Text = "Copying...", Location = new Point(16, 14), AutoSize = true };
        var bar = new ProgressBar { Location = new Point(16, 48), Size = new Size(448, 20), Maximum = Math.Max(1, files.Count) };
        form.Controls.Add(label);
        form.Controls.Add(bar);
        form.Shown += async (_, _) =>
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
            form.Close();
        };
        form.ShowDialog();
        return (error, skipped);
    }
}
