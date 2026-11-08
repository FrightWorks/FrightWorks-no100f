namespace FrightWorks;

// Where the game and the player's files are. Keep in step with the game (wiicompiled/runtime/include/runtime_config.h:
// PortableRootDirectory, ApplicationDataDirectory, SavesDirectory, CaptureDirectory):
// - Portable (the zip, and Play\ in the dev tree): data\portable.txt (Play\portable.txt) marks the folder; everything
//   is in its UserData, the disc in data\Disc (the dev tree's shared Disc\), F2 / F3 beside the launcher.
// - Installed (Velopack, %LOCALAPPDATA%\FrightWorks, replaced on update and deleted on uninstall): settings, logs,
//   caches, the disc and mods in %LOCALAPPDATA%\FrightWorks-Data, saves in Saved Games\FrightWorks, F2 / F3 in
//   Pictures\FrightWorks and Videos\FrightWorks.
// - On Linux (source builds and portable folders so far) the game without the marker keeps everything in
//   $XDG_DATA_HOME/WiiCompiled (~/.local/share/WiiCompiled), saves in its Saves, F2 / F3 beside the game.
// In both, [paths] saves_dir in Config.toml moves the saves.
sealed class DataPaths
{
    public static readonly string GameExe = OperatingSystem.IsWindows() ? "FrightWorks Game.exe" : "FrightWorks Game";
    const string Marker = "portable.txt";
    const int MarkerSearchDepth = 4;  // the game's kPortableSearchDepth
    public const string InstalledDataName = "FrightWorks-Data", UserFolderName = "FrightWorks";

    public readonly string Root;      // the launcher's folder (release) or the repo root (dev tree)
    public readonly string Game;      // the folder with the game exe: data\ (release) or Play\ (dev tree)
    public readonly bool Portable;    // the portable marker is there
    public readonly bool DevTree;     // a source checkout's Play\
    public readonly string UserData;  // Config.toml, Logs, Cache, mods, texture_replacements, ...

    public DataPaths(string root, string game)
    {
        Root = root;
        Game = game;
        DevTree = Path.GetFileName(game) == "Play";
        Portable = FindMarker(game);
        UserData = Portable ? Path.Combine(game, "UserData") : InstalledDataDir;
    }

    // The game folder: data\ beside the launcher (release), or Play\ above launcher\publish (dev tree).
    public static DataPaths Find()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
            foreach (var sub in new[] { "data", "Play" })
                if (File.Exists(Path.Combine(d.FullName, sub, GameExe))) return new DataPaths(d.FullName, Path.Combine(d.FullName, sub));
        return new DataPaths(AppContext.BaseDirectory, Path.Combine(AppContext.BaseDirectory, "data"));
    }

    public static string InstalledDataDir
    {
        get
        {
            if (OperatingSystem.IsWindows())
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), InstalledDataName);
            // runtime_config.h ApplicationDataDirectory: the XDG data folder, under the runtime's own name.
            var xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
            var data = !string.IsNullOrEmpty(xdg) ? xdg : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share");
            return Path.Combine(data, "WiiCompiled");
        }
    }

    // From the game's folder upwards, like the game looks from its exe.
    static bool FindMarker(string game)
    {
        var d = new DirectoryInfo(game);
        for (int level = 0; d != null && level <= MarkerSearchDepth; level++, d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, Marker))) return true;
        return false;
    }

    public string ConfigPath => Path.Combine(UserData, "Config.toml");
    public string Mods => Path.Combine(UserData, "mods");

    // The extracted disc and the [paths] dvd_root that points at it (relative to UserData, so the folder can move).
    public string Disc => !Portable ? Path.Combine(UserData, "Disc", "DATA")
                        : DevTree ? Path.Combine(Root, "Disc", "DATA") : Path.Combine(Game, "Disc", "DATA");
    public string DvdRootValue => !Portable ? "'Disc/DATA'" : DevTree ? "'../../Disc/DATA'" : "'../Disc/DATA'";

    // The disc the game will use: [paths] dvd_root (relative to UserData) when it holds one, else Disc.
    public string PlayableDisc
    {
        get
        {
            try
            {
                var configured = new GameConfig(ConfigPath).Get("paths", "dvd_root");
                if (!string.IsNullOrWhiteSpace(configured))
                {
                    var full = Path.GetFullPath(Path.Combine(UserData, configured));
                    if (File.Exists(Path.Combine(full, "sys", "boot.bin"))) return full;
                }
            }
            catch (Exception) { }
            return Disc;
        }
    }

    public string DefaultSaves => Portable || !OperatingSystem.IsWindows() ? Path.Combine(UserData, "Saves") : Path.Combine(KnownFolders.SavedGames, UserFolderName);

    // [paths] saves_dir when set (absolute, or relative to UserData), else the default.
    public string Saves
    {
        get
        {
            var configured = new GameConfig(ConfigPath).Get("paths", "saves_dir");
            if (string.IsNullOrWhiteSpace(configured)) return DefaultSaves;
            try { return Path.GetFullPath(Path.Combine(UserData, configured)); }
            catch (Exception) { return DefaultSaves; }
        }
    }

    // Before 2026-11 the zip had the HD pack and the button icons in UserData, where the old copy would win over the new
    // one beside the game: remove them once the game's own are there. A link is only unlinked.
    public void RemoveOldShippedCopies()
    {
        foreach (var rel in new[] { Path.Combine("texture_replacements", "ScoobyHD"), "button_icons" })
        {
            var old = Path.Combine(UserData, rel);
            try
            {
                if (!Directory.Exists(old) || !Directory.Exists(Path.Combine(Game, rel))) continue;
                if (new DirectoryInfo(old).Attributes.HasFlag(FileAttributes.ReparsePoint)) Directory.Delete(old, false);
                else Directory.Delete(old, true);
            }
            catch (Exception) { }  // in use or read only: the game still runs, with the old copy
        }
    }

    public string Screenshots => Capture(false);
    public string Videos => Capture(true);

    string Capture(bool videos)
    {
        if (!Portable && OperatingSystem.IsWindows())
            return Path.Combine(Environment.GetFolderPath(videos ? Environment.SpecialFolder.MyVideos : Environment.SpecialFolder.MyPictures), UserFolderName);
        // Beside the launcher: the folder above data/ in a release, Play/ itself in the dev tree (on Linux the game puts
        // them there with or without the marker).
        return Path.Combine(DevTree ? Game : Root, videos ? "Videos" : "Screenshots");
    }
}

[System.Runtime.Versioning.SupportedOSPlatform("windows")]
static class KnownFolders
{
    // Environment.SpecialFolder has no Saved Games.
    static readonly Guid SavedGamesId = new("4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4");

    [System.Runtime.InteropServices.DllImport("shell32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern int SHGetKnownFolderPath([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPStruct)] Guid id,
                                           uint flags, IntPtr token, out string path);

    public static string SavedGames
    {
        get
        {
            const uint KF_FLAG_CREATE = 0x8000;
            if (SHGetKnownFolderPath(SavedGamesId, KF_FLAG_CREATE, IntPtr.Zero, out var path) == 0) return path;
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Saved Games");
        }
    }
}
