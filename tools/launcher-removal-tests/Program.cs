// The setup step's "Remove the installed FrightWorks" (launcher/InstalledRemoval.cs) against a made-up installed game
// and portable copy in a scratch folder, with a stand-in uninstaller: the real one is never run.
// dotnet run --project tools\launcher-removal-tests\LauncherRemovalTests.csproj -c Release -- <scratch folder>
using FrightWorks;

static class LauncherRemovalTests
{
    static int checks, failures;

    static void Check(bool condition, string message)
    {
        checks++;
        if (condition) return;
        failures++;
        Console.Error.WriteLine("FAIL " + message);
    }

    sealed record Fixture(DataPaths Paths, DataImport.Source Source, string DefaultSaves, InstalledRemoval.Target Target);

    // An installed game (data folder with settings, controls, a mod, a texture pack, logs and caches; saves in its own
    // folder) and a portable copy that the setup step has imported it into, as Prepare.ImportInstalled does.
    static Fixture Make(string root, bool customSaves = false)
    {
        string installed = Path.Combine(root, "installed");
        string data = Path.Combine(installed, "FrightWorks-Data");
        string saves = Path.Combine(installed, customSaves ? "MySaves" : "SavedGames");
        string defaultSaves = Path.Combine(installed, "SavedGames");
        Write(Path.Combine(data, "Config.toml"), "[video]\nfps60 = true\n[paths]\ndvd_root = 'Disc/DATA'\n");
        Write(Path.Combine(data, "controls.toml"), "a = 1\n");
        Write(Path.Combine(data, "mods", "spooky", "files", "x.bin"), "mod");
        Write(Path.Combine(data, "texture_packs", "01-hd", "t.png"), "pack");
        Write(Path.Combine(data, "Logs", "old.log"), "log");
        Write(Path.Combine(data, "Cache", "pipeline_cache.db"), "cache");
        Write(Path.Combine(saves, "card.raw"), "save");

        string copy = Path.Combine(root, "copy");
        Write(Path.Combine(copy, "data", "portable.txt"), "");
        Write(Path.Combine(copy, "data", DataPaths.GameExe), "stand-in");
        var paths = new DataPaths(copy, Path.Combine(copy, "data"));
        var source = new DataImport.Source(data, data, null, saves);
        foreach (var (from, to) in DataImport.Files(source, paths))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            File.Copy(from, to);
        }
        // The setup step then rewrites the copy's [paths].
        File.WriteAllText(paths.ConfigPath, "[video]\nfps60 = true\n[paths]\ndvd_root = '../Disc/DATA'\n");
        var target = new InstalledRemoval.Target(source, defaultSaves, Path.Combine(installed, "program"), "Update.exe", "--uninstall --silent");
        return new Fixture(paths, source, defaultSaves, target);
    }

    static void Write(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }

    static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("Usage: LauncherRemovalTests <scratch folder>");
            return 2;
        }
        string scratch = Path.Combine(Path.GetFullPath(args[0]), "removal-" + Guid.NewGuid().ToString("N"));
        int n = 0;
        string Case() => Path.Combine(scratch, (++n).ToString());

        // Everything copied: the uninstaller runs, then the data folder and the default saves folder go.
        {
            var f = Make(Case());
            Check(InstalledRemoval.Verify(f.Source, f.Paths) == null, "verify: a full copy (with rewritten settings) matches");
            int calls = 0;
            string result = InstalledRemoval.Remove(f.Target, f.Paths, _ => { calls++; return 0; }, _ => false);
            Check(calls == 1, "remove: the uninstaller runs once");
            Check(result.StartsWith("Removed the installed"), "remove: says it removed it: " + result);
            Check(!Directory.Exists(f.Source.UserData), "remove: the data folder is gone");
            Check(!Directory.Exists(f.DefaultSaves), "remove: the default saves folder is gone");
            Check(File.Exists(Path.Combine(f.Paths.Saves, "card.raw")), "remove: the copy keeps the save");
            Check(File.Exists(Path.Combine(f.Paths.UserData, "texture_packs", "01-hd", "t.png")), "remove: the copy keeps the texture pack");
        }
        // A save the installed game wrote since: nothing is removed and the uninstaller doesn't run.
        {
            var f = Make(Case());
            File.WriteAllText(Path.Combine(f.Source.Saves!, "card.raw"), "newer");
            int calls = 0;
            string result = InstalledRemoval.Remove(f.Target, f.Paths, _ => { calls++; return 0; }, _ => false);
            Check(calls == 0 && result.StartsWith("Kept") && result.Contains("card.raw"), "changed save: kept: " + result);
            Check(File.Exists(Path.Combine(f.Source.Saves!, "card.raw")), "changed save: its save is still there");
        }
        // A file that wasn't copied.
        {
            var f = Make(Case());
            File.Delete(Path.Combine(f.Paths.UserData, "mods", "spooky", "files", "x.bin"));
            Check(InstalledRemoval.Verify(f.Source, f.Paths)?.Contains("x.bin") == true, "missing copy: verify names it");
        }
        // Same length, other bytes.
        {
            var f = Make(Case());
            File.WriteAllText(Path.Combine(f.Paths.UserData, "controls.toml"), "b = 1\n");
            Check(InstalledRemoval.Verify(f.Source, f.Paths) != null, "same size, other contents: not verified");
        }
        // The installed game is open.
        {
            var f = Make(Case());
            int calls = 0;
            string result = InstalledRemoval.Remove(f.Target, f.Paths, _ => { calls++; return 0; }, _ => true);
            Check(calls == 0 && result.Contains("running") && Directory.Exists(f.Source.UserData), "running: kept: " + result);
        }
        // The uninstaller fails: its files stay.
        {
            var f = Make(Case());
            string result = InstalledRemoval.Remove(f.Target, f.Paths, _ => 1, _ => false);
            Check(result.Contains("code 1") && Directory.Exists(f.Source.UserData) && Directory.Exists(f.Source.Saves!),
                  "uninstaller fails: files kept: " + result);
        }
        // Saves in a folder the player chose: never deleted.
        {
            var f = Make(Case(), customSaves: true);
            string result = InstalledRemoval.Remove(f.Target, f.Paths, _ => 0, _ => false);
            Check(result.StartsWith("Removed") && !Directory.Exists(f.Source.UserData) && Directory.Exists(f.Source.Saves!),
                  "custom saves folder: data gone, saves kept: " + result);
        }
        // The registered command.
        {
            var (exe, rest) = InstalledRemoval.SplitCommand("\"C:\\Users\\A B\\AppData\\Local\\FrightWorks\\Update.exe\" --uninstall --silent");
            Check(exe == "C:\\Users\\A B\\AppData\\Local\\FrightWorks\\Update.exe" && rest == "--uninstall --silent", "split: quoted exe");
            (exe, rest) = InstalledRemoval.SplitCommand("C:\\FW\\Update.exe --uninstall");
            Check(exe == "C:\\FW\\Update.exe" && rest == "--uninstall", "split: plain exe");
        }

        try { Directory.Delete(scratch, true); } catch (Exception) { }
        Console.WriteLine(failures == 0 ? $"launcher removal tests: all {checks} checks passed" : $"{failures} of {checks} checks failed");
        return failures == 0 ? 0 : 1;
    }
}
