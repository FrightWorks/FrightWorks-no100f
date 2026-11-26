using System.Diagnostics;
using System.Security.Cryptography;

namespace FrightWorks;

// RecompLauncher's setup step, when the player ticks "Remove the installed FrightWorks" (recomplauncher.json
// prepare.options, Windows only): once this portable copy has the installed game's saves and settings, the installed
// game goes. In order, stopping at the first thing that isn't right, and never failing the setup step itself:
// 1. every file the import brings over (DataImport.Files) is here with the same contents: a save the installed game
//    wrote since, or a copy that failed, keeps it;
// 2. the installed game isn't running;
// 3. Velopack's own uninstall (the QuietUninstallString it registered: Update.exe --uninstall --silent), which also
//    takes its shortcut and its Apps & features entry;
// 4. only then its data folder (FrightWorks-Data: settings, mods, texture packs, its disc, logs, caches) and its saves
//    in the default Saved Games\FrightWorks. A saves_dir the player chose is never deleted.
// Linux has no such option: ~/.local/share/WiiCompiled is shared with other WiiCompiled games.
static class InstalledRemoval
{
    public const string Flag = "--remove-installed";
    const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\FrightWorks";

    // What would go. DefaultSaves: the installed game's default saves folder, deleted only when Source.Saves is it.
    internal sealed record Target(DataImport.Source Source, string DefaultSaves, string InstallFolder, string UninstallExe,
                                  string UninstallArgs);

    // The installed game beside this copy, from its uninstall entry; null (and why) when there is none to remove.
    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    internal static (Target? target, string why) Find(DataPaths paths)
    {
        if (DataImport.Installed(paths) is not { } source) return (null, "No installed FrightWorks was found.");
        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(UninstallKey);
        var command = key?.GetValue("QuietUninstallString") as string;
        var folder = key?.GetValue("InstallLocation") as string;
        if (string.IsNullOrWhiteSpace(command) || string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder))
            return (null, "The installed FrightWorks's data is there, but not its program (no uninstall entry): left as it is.");
        var (exe, args) = SplitCommand(command);
        if (!File.Exists(exe)) return (null, $"The installed FrightWorks's uninstaller isn't there ({exe}): left as it is.");
        string defaultSaves = Path.Combine(KnownFolders.SavedGames, DataPaths.UserFolderName);
        return (new Target(source, defaultSaves, folder, exe, args), "");
    }

    // "C:\path\Update.exe" --uninstall --silent -> the exe and the rest.
    internal static (string exe, string args) SplitCommand(string command)
    {
        command = command.Trim();
        if (command.StartsWith('"'))
        {
            int close = command.IndexOf('"', 1);
            if (close > 0) return (command[1..close], command[(close + 1)..].Trim());
        }
        int space = command.IndexOf(' ');
        return space < 0 ? (command, "") : (command[..space], command[(space + 1)..].Trim());
    }

    // Null when every imported file is here with the same contents, else the first one that isn't.
    internal static string? Verify(DataImport.Source source, DataPaths paths)
    {
        try
        {
            foreach (var (from, to) in DataImport.Files(source, paths))
            {
                if (!File.Exists(to)) return $"{Path.GetFileName(from)} wasn't copied";
                // The setup step rewrites the copied settings' [paths] (this copy's disc and saves): only that it's here.
                if (string.Equals(Path.GetFullPath(to), Path.GetFullPath(paths.ConfigPath), StringComparison.OrdinalIgnoreCase)) continue;
                var a = new FileInfo(from);
                var b = new FileInfo(to);
                if (a.Length != b.Length || !SameContents(from, to)) return $"{Path.GetFileName(from)} differs from this copy's";
            }
            return null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return "its files couldn't be read: " + e.Message;
        }
    }

    static bool SameContents(string a, string b)
    {
        using var fa = File.OpenRead(a);
        using var fb = File.OpenRead(b);
        return SHA256.HashData(fa).AsSpan().SequenceEqual(SHA256.HashData(fb));
    }

    // Steps 1 to 4. uninstall runs the uninstaller and returns its exit code; running says whether a program from the
    // install folder is still open. Returns what happened, for the setup step's output.
    internal static string Remove(Target target, DataPaths paths, Func<Target, int> uninstall, Func<string, bool> running)
    {
        if (Verify(target.Source, paths) is { } problem)
            return $"Kept the installed FrightWorks: {problem}. Copy your saves from it by hand, or untick the option.";
        if (running(target.InstallFolder)) return "Kept the installed FrightWorks: it's running. Close it and run the setup again.";

        int code;
        try { code = uninstall(target); }
        catch (Exception e) { return "Kept the installed FrightWorks: its uninstaller didn't start: " + e.Message; }
        if (code != 0) return $"Kept the installed FrightWorks's files: its uninstaller stopped with code {code}.";

        var left = new List<string>();
        Delete(target.Source.UserData, left);
        if (target.Source.Saves is { } saves && SameFolder(saves, target.DefaultSaves)) Delete(saves, left);
        return left.Count == 0
            ? "Removed the installed FrightWorks; its saves and settings are in this copy."
            : "Removed the installed FrightWorks, but couldn't delete " + string.Join(" and ", left) + ".";
    }

    static void Delete(string folder, List<string> left)
    {
        try { if (Directory.Exists(folder)) Directory.Delete(folder, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { left.Add(folder); }
    }

    static bool SameFolder(string a, string b) =>
        string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(a)), Path.TrimEndingDirectorySeparator(Path.GetFullPath(b)),
                      StringComparison.OrdinalIgnoreCase);

    // The real uninstaller: Velopack's Update.exe, waited for (up to five minutes).
    internal static int RunUninstaller(Target target)
    {
        using var process = Process.Start(new ProcessStartInfo(target.UninstallExe, target.UninstallArgs) { UseShellExecute = false })
                            ?? throw new InvalidOperationException("no process");
        if (!process.WaitForExit(TimeSpan.FromMinutes(5))) return -1;
        return process.ExitCode;
    }

    // A program of the installed game (its launcher or the game) is open.
    internal static bool AnyRunningFrom(string folder)
    {
        string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder)) + Path.DirectorySeparatorChar;
        foreach (var name in new[] { "FrightWorks", "FrightWorks Game" })
            foreach (var p in Process.GetProcessesByName(name))
                using (p)
                {
                    try
                    {
                        if (p.MainModule?.FileName is { } file && file.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return true;
                    }
                    catch (Exception) { }  // another user's process, or gone
                }
        return false;
    }
}
