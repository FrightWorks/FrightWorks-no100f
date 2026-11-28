using System;
using System.Buffers.Binary;
using System.IO;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace FrightWorks;

// Discord Rich Presence for the launcher: "In the launcher" with the game's art and buttons, through the Discord app's
// local IPC pipe (Windows) or socket (Linux), no SDK. The game shows its own status the same way
// (wiicompiled/runtime/src/discord_presence.cpp; app, art and buttons in no100f_discord_status.h): while it runs, the
// launcher lets go of Discord so the game's status shows.
sealed class DiscordPresence : IDisposable
{
    // The "FrightWorks - No100F" Discord application; the game's default too (runtime_config.h).
    public const string ClientId = "1575956576467165184";
    const string GamePage = "https://frightworks.dev/games/no100f/";  // also where clicking the pictures goes
    static readonly (string Label, string Url)[] Buttons =
    {
        ("Get the Recomp", GamePage),
        ("Get it on RecompLauncher", "https://recomplauncher.com/game/frightworks-no100f"),
    };
    static readonly TimeSpan RetryAfter = TimeSpan.FromSeconds(10);  // Discord not running: look again this often

    readonly long startedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    CancellationTokenSource? running;
    bool enabled, paused;

    // [discord] enabled in Config.toml: on unless set to false (the game reads the same key).
    public static bool EnabledIn(GameConfig config) => config.Get("discord", "enabled")?.Trim() != "false";

    public void SetEnabled(bool on) { enabled = on; Update(); }

    // While the game runs it shows its own status.
    public void SetPaused(bool on) { paused = on; Update(); }

    public void Dispose() { enabled = false; Update(); }

    void Update()
    {
        bool want = enabled && !paused;
        if (want == (running != null)) return;
        if (!want)
        {
            running!.Cancel();  // closing the connection clears the launcher's status
            running.Dispose();
            running = null;
            return;
        }
        running = new CancellationTokenSource();
        var token = running.Token;
        Task.Run(() => Run(token));
    }

    async Task Run(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                await using var stream = await Connect(token);
                if (stream != null)
                {
                    await WriteFrame(stream, 0, "{\"v\":1,\"client_id\":\"" + ClientId + "\"}", token);
                    using var ready = new CancellationTokenSource(TimeSpan.FromSeconds(2));
                    using var readyOrStop = CancellationTokenSource.CreateLinkedTokenSource(token, ready.Token);
                    var answer = await ReadFrame(stream, readyOrStop.Token);
                    if (answer != null && answer.Contains("\"evt\":\"READY\"", StringComparison.Ordinal))
                    {
                        await WriteFrame(stream, 1, ActivityPayload(Environment.ProcessId, startedAt), token);
                        while (await ReadFrame(stream, token) != null) { }  // Discord's answers, until it quits
                    }
                }
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { return; }
            catch (Exception) { }  // Discord quit or refused: try again later
            try { await Task.Delay(RetryAfter, token); }
            catch (OperationCanceledException) { return; }
        }
    }

    internal static async Task<Stream?> Connect(CancellationToken token)
    {
        for (int i = 0; i < 10; i++)
        {
            if (OperatingSystem.IsWindows())
            {
                var pipe = new NamedPipeClientStream(".", "discord-ipc-" + i, PipeDirection.InOut, PipeOptions.Asynchronous);
                try { await pipe.ConnectAsync(50, token); return pipe; }
                catch (Exception e) when (e is TimeoutException or IOException) { await pipe.DisposeAsync(); }
                continue;
            }
            foreach (var dir in SocketFolders())
            {
                if (string.IsNullOrEmpty(dir)) continue;
                var path = Path.Combine(dir, "discord-ipc-" + i);
                if (!File.Exists(path)) continue;
                var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
                try { await socket.ConnectAsync(new UnixDomainSocketEndPoint(path), token); return new NetworkStream(socket, ownsSocket: true); }
                catch (SocketException) { socket.Dispose(); }
            }
        }
        return null;
    }

    // Discord's socket sits in the runtime or temp folder, or under the runtime folder in a sandbox's own subfolder for
    // Flatpak and Snap builds of Discord and Vesktop's Flatpak (the game looks in the same places: discord_presence.cpp).
    static string?[] SocketFolders()
    {
        var runtime = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
        bool hasRuntime = !string.IsNullOrEmpty(runtime);
        return new[]
        {
            runtime,
            hasRuntime ? Path.Combine(runtime!, "app", "com.discordapp.Discord") : null,
            hasRuntime ? Path.Combine(runtime!, "snap.discord") : null,
            hasRuntime ? Path.Combine(runtime!, ".flatpak", "dev.vencord.Vesktop", "xdg-run") : null,
            Environment.GetEnvironmentVariable("TMPDIR"), Environment.GetEnvironmentVariable("TMP"),
            Environment.GetEnvironmentVariable("TEMP"), "/tmp",
        };
    }

    // Frame: opcode and length (little-endian uint32 each), then the JSON.
    internal static async Task WriteFrame(Stream stream, int opcode, string json, CancellationToken token)
    {
        var body = Encoding.UTF8.GetBytes(json);
        var frame = new byte[8 + body.Length];
        BinaryPrimitives.WriteInt32LittleEndian(frame, opcode);
        BinaryPrimitives.WriteInt32LittleEndian(frame.AsSpan(4), body.Length);
        body.CopyTo(frame, 8);
        await stream.WriteAsync(frame, token);
        await stream.FlushAsync(token);
    }

    // The next frame's JSON; null when Discord closed the connection.
    internal static async Task<string?> ReadFrame(Stream stream, CancellationToken token)
    {
        var header = new byte[8];
        if (!await ReadExactly(stream, header, token)) return null;
        int length = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(4));
        if (length < 0 || length > 1 << 20) return null;
        var body = new byte[length];
        return await ReadExactly(stream, body, token) ? Encoding.UTF8.GetString(body) : null;
    }

    static async Task<bool> ReadExactly(Stream stream, byte[] buffer, CancellationToken token)
    {
        int done = 0;
        while (done < buffer.Length)
        {
            int n = await stream.ReadAsync(buffer.AsMemory(done), token);
            if (n == 0) return false;
            done += n;
        }
        return true;
    }

    internal static string ActivityPayload(int pid, long startedAt)
    {
        using var buffer = new MemoryStream();
        using (var json = new Utf8JsonWriter(buffer))
        {
            json.WriteStartObject();
            json.WriteString("cmd", "SET_ACTIVITY");
            json.WriteString("nonce", "frightworks-launcher");
            json.WriteStartObject("args");
            json.WriteNumber("pid", pid);
            json.WriteStartObject("activity");
            json.WriteString("name", "FrightWorks: Night of 100 Frights");  // the game's kActivityName
            json.WriteString("details", "In the launcher");
            json.WriteStartObject("assets");
            json.WriteString("large_image", "frightworks-no100f-icon");
            json.WriteString("large_text", "Night of 100 Frights");
            json.WriteString("large_url", GamePage);
            json.WriteString("small_image", "frightworks-pfp");
            json.WriteString("small_text", "FrightWorks");
            json.WriteString("small_url", GamePage);
            json.WriteEndObject();
            json.WriteStartObject("timestamps");
            json.WriteNumber("start", startedAt);
            json.WriteEndObject();
            json.WriteStartArray("buttons");
            foreach (var (label, url) in Buttons)
            {
                json.WriteStartObject();
                json.WriteString("label", label);
                json.WriteString("url", url);
                json.WriteEndObject();
            }
            json.WriteEndArray();
            json.WriteBoolean("instance", false);
            json.WriteEndObject();
            json.WriteEndObject();
            json.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.ToArray());
    }
}
