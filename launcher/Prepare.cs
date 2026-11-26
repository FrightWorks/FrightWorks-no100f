using System.Globalization;

namespace FrightWorks;

// RecompLauncher's setup step (recomplauncher.json "prepare"; https://recomplauncher.com). It runs this exe with no window
// after it has checked the player's disc image, again whenever the player picks another copy, and after every update
// (the disc is unpacked again only for another image: <disc folder>.source records which one it came from):
//   FrightWorks.exe --prepare --game=<image>
// It does what this launcher's first start and disc choice do, so the game can start straight away without it:
// - copies the saves, settings and controls of an installed FrightWorks into a fresh portable copy (DataImport.Installed),
// - extracts the disc into this game's disc folder (DataPaths.Disc) and points [paths] dvd_root at it,
// - removes old shipped copies and fills the HDR keys from the display (Windows), as the launcher does at its start.
// Texture packs stay this launcher's (Extras > Textures, maintainer 2026-11-25): the step never downloads one.
// Prints "progress <0..1> <text>" lines, which RecompLauncher shows under the step. Exit codes: 0 done, 1 error, 2 the
// image is incomplete, 3 not the right disc. Nothing is written to the old copy or the image.
static class Prepare
{
    public const string Flag = "--prepare";

    public static int Run(string[] args)
    {
        string? image = null;
        bool removeInstalled = false;
        for (int i = 1; i < args.Length; i++)
        {
            string a = args[i];
            if (a.StartsWith("--game=", StringComparison.Ordinal)) image = a["--game=".Length..];
            else if (a == "--game" && i + 1 < args.Length) image = args[++i];
            else if (a == InstalledRemoval.Flag) removeInstalled = true;
            else return Fail(1, $"Unknown argument: {a}");
        }
        if (string.IsNullOrEmpty(image)) return Fail(1, "No disc image given (--game=<image>).");

        var paths = DataPaths.Find();
        // A source checkout's disc is the shared Disc\ that builds read: never replace it from here.
        if (paths.DevTree) return Fail(1, "This is a source checkout's Play folder: set the disc up with tools/ instead.");
        if (!File.Exists(Path.Combine(paths.Game, DataPaths.GameExe))) return Fail(1, $"The game files are missing: there is no {DataPaths.GameExe} in {paths.Game}.");

        const double discShare = 0.95;  // of the bar: the disc is nearly all the work
        try
        {
            using var disc = GcDisc.Open(image);
            var problem = disc.Problem();
            if (problem.Length > 0) return Fail(3, problem);

            ImportInstalled(paths);

            // RecompLauncher runs this step again after every update: the image already unpacked here isn't unpacked
            // again (1.4 GB). Another image, or a disc folder missing its files, is unpacked as before.
            string stamp = SourceStamp(image);
            string stampFile = paths.Disc + ".source";
            if (File.Exists(stampFile) && File.ReadAllText(stampFile) == stamp &&
                File.Exists(Path.Combine(paths.Disc, "sys", "main.dol")) &&
                File.Exists(Path.Combine(paths.Disc, "sys", "boot.bin")))
            {
                Progress(discShare, "Your disc is already unpacked");
                var current = new GameConfig(paths.ConfigPath);
                current.Set("paths", "dvd_root", paths.DvdRootValue);
                current.Save();
                return Finish(paths, removeInstalled);
            }

            Progress(0.02, "Unpacking your disc");
            var staging = paths.Disc + ".new";
            if (Directory.Exists(staging)) Directory.Delete(staging, true);
            int lastShown = -1;
            disc.Extract(staging, (done, total, name) =>
            {
                int percent = total > 0 ? done * 100 / total : 100;
                if (percent == lastShown) return;
                lastShown = percent;
                Progress(0.02 + (discShare - 0.02) * percent / 100.0, $"Unpacking your disc: {done} of {total} files");
            });
            if (disc.Holes > 0)
            {
                try { Directory.Delete(staging, true); } catch (Exception) { }
                return Fail(2, "The disc image is incomplete (it ends early): dump the disc again.");
            }
            File.Delete(stampFile);  // until the new folder is in place
            if (Directory.Exists(paths.Disc)) Directory.Delete(paths.Disc, true);
            Directory.CreateDirectory(Path.GetDirectoryName(paths.Disc)!);
            Directory.Move(staging, paths.Disc);
            File.WriteAllText(stampFile, stamp);
            var cfg = new GameConfig(paths.ConfigPath);
            cfg.Set("paths", "dvd_root", paths.DvdRootValue);  // relative to UserData, so the folder can move
            cfg.Save();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return Fail(1, "Setting up the disc failed: " + e.Message);
        }

        return Finish(paths, removeInstalled);
    }

    static int Finish(DataPaths paths, bool removeInstalled)
    {
        Progress(0.95, "Finishing the settings");
        paths.RemoveOldShippedCopies();
        if (OperatingSystem.IsWindows()) FillHdrDefaults(paths);
        // Last, with this copy ready: the player's tick on RecompLauncher's setup screen (InstalledRemoval). What it did
        // is printed; it never fails the step.
        if (removeInstalled && OperatingSystem.IsWindows())
        {
            Progress(0.97, "Removing the installed FrightWorks");
            var (target, why) = InstalledRemoval.Find(paths);
            string result = target == null ? why
                : InstalledRemoval.Remove(target, paths, InstalledRemoval.RunUninstaller, InstalledRemoval.AnyRunningFrom);
            Console.Out.WriteLine(result);
            Console.Out.Flush();
        }
        Progress(1, "Ready to play");
        return 0;
    }

    // Which image the disc folder was unpacked from: its full path, size and last change. Any difference unpacks again,
    // which is only slower, never wrong.
    static string SourceStamp(string image)
    {
        var info = new FileInfo(image);
        return $"{info.FullName}\n{info.Length}\n{info.LastWriteTimeUtc.Ticks}\n";
    }

    // A fresh portable copy (RecompLauncher's install) of a player who has the installed game too: the same copy the
    // launcher's first start offers from an earlier zip, without asking, since the copy only adds files.
    static void ImportInstalled(DataPaths paths)
    {
        if (!DataImport.Fresh(paths) || DataImport.Installed(paths) is not { } source) return;
        Progress(0.01, "Copying your saves and settings from the installed FrightWorks");
        string result;
        try
        {
            int copied = 0;
            foreach (var (from, to) in DataImport.Files(source, paths))
            {
                if (File.Exists(to)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                File.Copy(from, to);
                copied++;
            }
            // The installed game's disc and saves folders are its own: this copy uses its own (the disc is set below).
            if (File.Exists(paths.ConfigPath))
            {
                var cfg = new GameConfig(paths.ConfigPath);
                cfg.Remove("paths", "saves_dir");
                cfg.Remove("paths", "dvd_root");
                cfg.Save();
            }
            result = $"Copied {copied} files from {source.Name}";
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            result = $"Partly copied from {source.Name}: {e.Message}";
            Console.Error.WriteLine(result);
        }
        Directory.CreateDirectory(paths.UserData);
        File.WriteAllText(Path.Combine(paths.UserData, DataImport.Mark), result + Environment.NewLine);
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    static void FillHdrDefaults(DataPaths paths)
    {
        try
        {
            var config = new GameConfig(paths.ConfigPath);
            if (!HdrSettings.NeedsDefaults(config)) return;
            WindowsHdr.Snapshot display;
            try { display = WindowsHdr.Read(HdrSettings.TargetDisplay(config)); }
            catch (Exception e) { display = new(null, null, null, e.Message); }
            HdrSettings.InitializeMissing(config, display);
            config.Save();
        }
        catch (Exception e) { Console.Error.WriteLine("The HDR settings weren't filled in: " + e.Message); }  // the game's own defaults
    }

    static void Progress(double fraction, string text)
    {
        Console.Out.WriteLine("progress " + Math.Clamp(fraction, 0, 1).ToString("0.###", CultureInfo.InvariantCulture) + " " + text);
        Console.Out.Flush();
    }

    static int Fail(int code, string message)
    {
        Console.Out.WriteLine(message);
        Console.Error.WriteLine(message);
        return code;
    }
}
