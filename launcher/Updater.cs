using System.Net.Http.Json;
using System.Text.Json.Serialization;
using Velopack;
using Velopack.Sources;

namespace FrightWorks;

// New versions. The installed game (Velopack, from the installer) updates itself from the project's GitHub Releases:
// the launcher asks at start (MainForm), downloads the full or delta package and restarts into the new version. The
// zip can't update itself: it only says a newer release is out and opens its page. A dev tree never checks.
// Config.toml [launcher]: check_updates = false turns the check at start off (Extras > Updates); skip_update = "<ver>"
// is the version the player chose to skip. FRIGHTWORKS_UPDATE_SOURCE (a folder or URL holding a vpk pack output)
// replaces GitHub for testing an update (docs/testing.md).
sealed class Updater
{
    public const string RepoUrl = "https://github.com/FrightWorks/FrightWorks-no100f";
    public const string SourceVariable = "FRIGHTWORKS_UPDATE_SOURCE";

    // A newer version: Info for the installed game, PageUrl (the release page) for the zip.
    public sealed record Found(string Version, UpdateInfo? Info, string? PageUrl);

    readonly DataPaths paths;
    readonly UpdateManager? manager;  // the installed game only

    public Updater(DataPaths paths)
    {
        this.paths = paths;
        if (paths.DevTree || paths.Portable) return;
        try
        {
            var custom = Environment.GetEnvironmentVariable(SourceVariable);
            var m = string.IsNullOrWhiteSpace(custom)
                ? new UpdateManager(new GithubSource(RepoUrl, null, false))
                : new UpdateManager(custom.Trim());
            if (m.IsInstalled) manager = m;
        }
        catch (Exception) { }  // not a Velopack install (a data\ copy without the marker): no updates
    }

    public bool CanCheck => !paths.DevTree && (manager != null || paths.Portable);
    public bool Installed => manager != null;

    public static string AppVersion => typeof(Updater).Assembly.GetName().Version?.ToString(3) ?? "?";
    public string CurrentVersion => manager?.CurrentVersion?.ToString() ?? AppVersion;

    public bool CheckAtStart => new GameConfig(paths.ConfigPath).Get("launcher", "check_updates") != "false";

    public void SetCheckAtStart(bool on)
    {
        var c = new GameConfig(paths.ConfigPath);
        c.Set("launcher", "check_updates", on ? "true" : "false");
        c.Save();
    }

    public string? Skipped => new GameConfig(paths.ConfigPath).Get("launcher", "skip_update");

    public void Skip(string version)
    {
        var c = new GameConfig(paths.ConfigPath);
        c.SetString("launcher", "skip_update", version);
        c.Save();
    }

    // null: this is the newest version.
    public async Task<Found?> CheckAsync(CancellationToken cancel)
    {
        if (manager != null)
        {
            var info = await manager.CheckForUpdatesAsync();
            return info == null ? null : new Found(info.TargetFullRelease.Version.ToString(), info, null);
        }
        // The zip: the latest (non-pre-release) GitHub release, by its tag "v<version>".
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("FrightWorks-Launcher");
        http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        var api = RepoUrl.Replace("https://github.com/", "https://api.github.com/repos/") + "/releases/latest";
        var latest = await http.GetFromJsonAsync<Release>(api, cancel);
        if (latest?.Tag == null || !TryVersion(latest.Tag, out var theirs) || !TryVersion(AppVersion, out var ours) || theirs <= ours)
            return null;
        return new Found(latest.Tag.TrimStart('v', 'V'), null, latest.Page ?? RepoUrl + "/releases/latest");
    }

    // "v1.0.6" / "1.0.6-beta.1" -> 1.0.6 (the zip only compares release numbers).
    static bool TryVersion(string text, out Version version)
    {
        var core = text.TrimStart('v', 'V').Split('-', '+')[0];
        return Version.TryParse(core, out version!);
    }

    public Task DownloadAsync(Found found, Action<int> progress, CancellationToken cancel) =>
        manager!.DownloadUpdatesAsync(found.Info!, progress, cancel);

    // Exits this process; Velopack's updater swaps the program folder and starts the new launcher.
    public void ApplyAndRestart(Found found) => manager!.ApplyUpdatesAndRestart(found.Info!.TargetFullRelease);

    sealed class Release
    {
        [JsonPropertyName("tag_name")] public string? Tag { get; set; }
        [JsonPropertyName("html_url")] public string? Page { get; set; }
    }
}
