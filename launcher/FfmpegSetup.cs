using System.IO.Compression;
using System.Security.Cryptography;

namespace FrightWorks;

// ffmpeg for the game's F3 video recording (developer tools only; never shipped). Find looks where the game looks
// (no100f_recorder.cpp FindFfmpeg); Download fetches the BtbN win64 GPL build from GitHub, checks its SHA-256 against
// the release's checksums.sha256 (like tools\debug\capture\get-ffmpeg.ps1) and unpacks only what's needed.
static class FfmpegSetup
{
    const string ZipName = "ffmpeg-master-latest-win64-gpl.zip";
    const string BaseUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/";

    // PATH, then beside the game, then beside the launcher (portable release) / the debug kit's copy (dev tree), then
    // the user data folder (installed).
    public static string? Find(DataPaths paths)
    {
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
        {
            try
            {
                var p = Path.Combine(dir.Trim().Trim('"'), "ffmpeg.exe");
                if (dir.Trim().Length > 0 && File.Exists(p)) return p;
            }
            catch (ArgumentException) { }  // a PATH entry with bad characters
        }
        foreach (var b in new[] { paths.Game, paths.Root, paths.UserData })
            foreach (var rel in new[] { "ffmpeg.exe", @"ffmpeg\bin\ffmpeg.exe", @"tools\debug\local\ffmpeg\bin\ffmpeg.exe" })
                if (File.Exists(Path.Combine(b, rel))) return Path.Combine(b, rel);
        return null;
    }

    // Where Download puts it: tools\debug\local\ffmpeg in the dev tree (with ffprobe, which the debug kit's scripts use
    // too), <root>\ffmpeg in a portable release, UserData\ffmpeg when installed (the program folder is replaced on update).
    public static string InstallDir(DataPaths paths) =>
        paths.DevTree ? Path.Combine(paths.Root, "tools", "debug", "local", "ffmpeg")
        : paths.Portable ? Path.Combine(paths.Root, "ffmpeg") : Path.Combine(paths.UserData, "ffmpeg");

    public static async Task<string> Download(DataPaths paths, IProgress<(long done, long total)> progress, CancellationToken cancel)
    {
        var target = InstallDir(paths);
        bool release = !paths.DevTree;
        var tmp = Path.Combine(Path.GetTempPath(), "frightworks-ffmpeg-" + Environment.ProcessId);
        Directory.CreateDirectory(tmp);
        try
        {
            using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("FrightWorks-Launcher");
            var sums = await http.GetStringAsync(BaseUrl + "checksums.sha256", cancel);
            var want = sums.Split('\n').Select(l => l.Trim().Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
                           .Where(f => f.Length == 2 && f[1].TrimStart('*') == ZipName).Select(f => f[0]).FirstOrDefault()
                       ?? throw new InvalidDataException("the download's checksum list has no entry for " + ZipName);

            var zipPath = Path.Combine(tmp, ZipName);
            using (var response = await http.GetAsync(BaseUrl + ZipName, HttpCompletionOption.ResponseHeadersRead, cancel))
            {
                response.EnsureSuccessStatusCode();
                long total = response.Content.Headers.ContentLength ?? 0, done = 0;
                await using var input = await response.Content.ReadAsStreamAsync(cancel);
                await using var output = File.Create(zipPath);
                var buffer = new byte[1 << 16];
                for (int n; (n = await input.ReadAsync(buffer, cancel)) > 0;)
                {
                    await output.WriteAsync(buffer.AsMemory(0, n), cancel);
                    done += n;
                    progress.Report((done, total));
                }
            }
            string got;
            await using (var zipStream = File.OpenRead(zipPath)) got = Convert.ToHexString(await SHA256.HashDataAsync(zipStream, cancel));
            if (!got.Equals(want, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"the download is damaged (SHA-256 {got}, expected {want})");

            // The zip holds one ffmpeg-<version>\ folder with bin\ and LICENSE.txt.
            var keep = release ? new[] { "bin/ffmpeg.exe", "LICENSE.txt" } : new[] { "bin/ffmpeg.exe", "bin/ffprobe.exe", "LICENSE.txt" };
            var staging = target + ".new";
            if (Directory.Exists(staging)) Directory.Delete(staging, true);
            await Task.Run(() =>
            {
                using var zip = ZipFile.OpenRead(zipPath);
                foreach (var entry in zip.Entries)
                {
                    var slash = entry.FullName.IndexOf('/');
                    var rel = slash < 0 ? "" : entry.FullName[(slash + 1)..];
                    if (!keep.Contains(rel)) continue;
                    var dest = Path.Combine(staging, rel.Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                    entry.ExtractToFile(dest, true);
                }
            }, cancel);
            var exe = Path.Combine(staging, "bin", "ffmpeg.exe");
            if (!File.Exists(exe)) throw new InvalidDataException("ffmpeg.exe wasn't in the download");
            if (Directory.Exists(target)) Directory.Delete(target, true);
            Directory.Move(staging, target);
            return Path.Combine(target, "bin", "ffmpeg.exe");
        }
        finally
        {
            try { Directory.Delete(tmp, true); } catch { }
        }
    }
}
