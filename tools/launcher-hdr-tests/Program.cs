using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using ScoobyReMasked;

static class LauncherHdrTests
{
    static int checks;
    static readonly WindowsHdr.Snapshot WindowsDefaults = new(true, 480, 400, "Synthetic Windows defaults");

    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new InvalidOperationException(message);
    }

    static void Pump()
    {
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        Dispatcher.UIThread.RunJobs();
    }

    static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("Usage: LauncherHdrTests <owned-scratch-output-directory>");
            return 2;
        }
        string output = Path.GetFullPath(args[0]);
        Directory.CreateDirectory(output);
        string fixture = Path.Combine(output, "fixture-" + Guid.NewGuid().ToString("N"));
        string game = Path.Combine(fixture, "Play");
        string data = Path.Combine(game, "UserData");
        Directory.CreateDirectory(data);
        File.WriteAllText(Path.Combine(game, "portable.txt"), "");
        string path = Path.Combine(data, "Config.toml");
        AppBuilder.Configure<App>().UseSkia().UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
                  .WithInterFont().SetupWithoutStarting();
        try
        {
            CheckDefaults(fixture, game, path);
            CheckMetadata(path);
            CheckTargets(path);
            CheckWindow(fixture, game, path, output);
            Console.WriteLine($"PASS {checks} HDR assertions; synthetic metadata and headless CPU rendering only.");
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e);
            return 1;
        }
        finally
        {
            Dialogs.MessageOverride = null;
            if (File.Exists(path)) File.SetAttributes(path, FileAttributes.Normal);
            Directory.Delete(fixture, true);
        }
    }

    static void CheckDefaults(string fixture, string game, string path)
    {
        string[] keys = ["hdr", "hdr_paper_white_nits", "hdr_peak_nits"];
        string[] saved = ["false", "200", "187.125"];
        string[] expected = ["true", "480", "400"];
        for (int mask = 0; mask < 8; mask++)
        {
            string before = "[future]\nvalue = \"keep\"\n[video]\n";
            for (int i = 0; i < keys.Length; i++)
                if ((mask & (1 << i)) != 0) before += keys[i] + " = " + saved[i] + "\n";
            File.WriteAllText(path, before);
            var window = new MainWindow(null, new DataPaths(fixture, game), enableServices: false);
            _ = window.RefreshGamepads();
            Check(File.ReadAllText(path) == before, "Construction/refresh saved HDR settings");
            int queries = 0;
            window.InitializeHdrDefaults(true, () => { queries++; return WindowsDefaults; }).GetAwaiter().GetResult();
            var config = new GameConfig(path);
            for (int i = 0; i < keys.Length; i++)
                Check(config.Get("video", keys[i]) == ((mask & (1 << i)) != 0 ? saved[i] : expected[i]),
                      $"Presence mask {mask}: wrong {keys[i]}");
            Check(config.Get("future", "value") == "keep", "Initialization overwrote unknown values");
            string seeded = File.ReadAllText(path);
            window.InitializeHdrDefaults(true, () => throw new Exception("Should not query on reload")).GetAwaiter().GetResult();
            Check(File.ReadAllText(path) == seeded && queries == (mask == 7 ? 0 : 1), "Defaults reapplied on reload");
            window.Close();
        }

        File.Delete(path);
        var fresh = new MainWindow(null, new DataPaths(fixture, game), enableServices: false);
        fresh.InitializeHdrDefaults(false, () => throw new Exception("Non-Windows query")).GetAwaiter().GetResult();
        Check(!File.Exists(path), "Unsupported platform created absent HDR keys");
        fresh.InitializeHdrDefaults(true, () => WindowsDefaults).GetAwaiter().GetResult();
        Check(new GameConfig(path).Get("video", "hdr_paper_white_nits") == "480", "Absent-file initialization failed");

        File.WriteAllText(path, "[video]\nhdr = false\n");
        fresh.InitializeHdrDefaults(true, () =>
        {
            File.WriteAllText(path, "[video]\nhdr = false\nhdr_paper_white_nits = 237.5\nhdr_peak_nits = 1234.5\n");
            return WindowsDefaults;
        }).GetAwaiter().GetResult();
        Check(new GameConfig(path).Get("video", "hdr_paper_white_nits") == "237.5", "Concurrent adjustment overwritten");
        string manual = File.ReadAllText(path);
        fresh.InitializeHdrDefaults(false, () => throw new Exception("Non-Windows query")).GetAwaiter().GetResult();
        Check(File.ReadAllText(path) == manual, "Unsupported platform changed saved adjustments");

        File.WriteAllText(path, "[video]\nhdr = false\n");
        File.SetAttributes(path, FileAttributes.ReadOnly);
        int errors = 0;
        Dialogs.MessageOverride = (owner, text) =>
        {
            Check(owner == fresh && text.Contains("Couldn't save initial HDR defaults"), "Startup error lost owner/diagnostic");
            errors++;
            return Task.CompletedTask;
        };
        try
        {
            fresh.InitializeHdrDefaults(true, () => WindowsDefaults).GetAwaiter().GetResult();
            Check(errors == 1 && File.ReadAllText(path) == "[video]\nhdr = false\n", "Startup failure silently saved");
        }
        finally { File.SetAttributes(path, FileAttributes.Normal); Dialogs.MessageOverride = null; }

        // A query that throws must not stop the launcher starting, and ends exactly like one that returns a failure.
        const string partial = "[future]\nvalue = \"keep\"\n[video]\nhdr_peak_nits = 1234.5\n";
        File.WriteAllText(path, partial);
        fresh.InitializeHdrDefaults(true, () => new WindowsHdr.Snapshot(null, null, null, "Detection failed")).GetAwaiter().GetResult();
        string returnedFailure = File.ReadAllText(path);
        File.WriteAllText(path, partial);
        fresh.InitializeHdrDefaults(true, () => throw new InvalidOperationException("synthetic interop failure")).GetAwaiter().GetResult();
        Check(File.ReadAllText(path) == returnedFailure, "A throwing query did not end like a returned failure");
        var failed = new GameConfig(path);
        Check(failed.Get("video", "hdr") == "false" && failed.Get("video", "hdr_paper_white_nits") == "200" &&
              failed.Get("video", "hdr_peak_nits") == "1234.5" && failed.Get("future", "value") == "keep",
              "A throwing query did not leave safe defaults with saved values kept");
        var statusWindow = fresh.CreateHdrWindow();
        statusWindow.Show();
        Pump();
        Check(statusWindow.GetVisualDescendants().OfType<TextBlock>().Any(t => t.Text?.Contains("query failed (InvalidOperationException)") == true),
              "A throwing query was not reported in the HDR window");
        statusWindow.Close();
        File.WriteAllText(path, partial);
        using (new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None))
            fresh.InitializeHdrDefaults(true, () => WindowsDefaults).GetAwaiter().GetResult();
        Check(File.ReadAllText(path) == partial, "An unreadable config was changed");
        fresh.Close();
    }

    static void CheckMetadata(string path)
    {
        foreach (var snapshot in new[] {
            new WindowsHdr.Snapshot(null, null, null, "Detection failed"),
            new WindowsHdr.Snapshot(false, null, null, "HDR off"),
            new WindowsHdr.Snapshot(true, float.NaN, float.PositiveInfinity, "Invalid metadata"),
            new WindowsHdr.Snapshot(true, 79, 10001, "Outside supported ranges") })
        {
            File.WriteAllText(path, "[video]\n");
            var config = new GameConfig(path);
            HdrSettings.InitializeMissing(config, snapshot);
            Check(config.Get("video", "hdr") == (snapshot.Enabled == true ? "true" : "false"), "Wrong enabled state");
            Check(config.Get("video", "hdr_paper_white_nits") == "200" && config.Get("video", "hdr_peak_nits") == "1000",
                  "Untrustworthy metadata used as luminance");
        }
        foreach (var snapshot in new[] {
            new WindowsHdr.Snapshot(true, 320, null, "Paper only"),
            new WindowsHdr.Snapshot(true, null, 800, "Peak only"),
            new WindowsHdr.Snapshot(true, 80, 10000, "Endpoints") })
        {
            File.WriteAllText(path, "[video]\n");
            var config = new GameConfig(path);
            HdrSettings.InitializeMissing(config, snapshot);
            Check(config.Get("video", "hdr_paper_white_nits") == HdrSettings.Raw((decimal?)snapshot.PaperWhite ?? 200),
                  "Paper white and peak semantics mixed");
            Check(config.Get("video", "hdr_peak_nits") == HdrSettings.Raw((decimal?)snapshot.Peak ?? 1000),
                  "Independent peak fallback lost");
        }
        Check(WindowsHdr.Read(null).Enabled == null, "Unknown display claimed HDR state");
        Check(WindowsHdr.HdrActive(2, 2) && !WindowsHdr.HdrActive(2, 1) && !WindowsHdr.HdrActive(4, 2),
              "Advanced-color capability confused with active HDR");
    }

    static void CheckTargets(string path)
    {
        var monitors = new List<Monitors.Monitor> {
            new("MAIN/a", "Main", @"\\.\DISPLAY1"),
            new("SIDE/b", "Side", @"\\.\DISPLAY2"),
            new("SAME/c", "Same", @"\\.\DISPLAY3"),
            new("SAME/d", "Same", @"\\.\DISPLAY4") };
        foreach (var (mode, saved, expected) in new[] {
            ("fullscreen", "SIDE/b", @"\\.\DISPLAY2"),
            ("fullscreen", "SIDE/disconnected-port", @"\\.\DISPLAY2"),
            ("fullscreen", "SAME/disconnected-port", @"\\.\DISPLAY1"),
            ("fullscreen", "gone/a", @"\\.\DISPLAY1"),
            ("fullscreen", "auto", (string?)null),
            ("windowed", "SIDE/b", null) })
        {
            File.WriteAllText(path, $"[video]\ndisplay_mode = \"{mode}\"\nfullscreen_display = \"{saved}\"\n");
            Check(HdrSettings.TargetDeviceName(new GameConfig(path), monitors, @"\\.\DISPLAY1") == expected,
                  "Target monitor matching changed");
        }
    }

    static void CheckWindow(string fixture, string game, string path, string output)
    {
        const string initial = "[launcher]\nauto_controller_layout = true\nauto_controller_layout_value = \"gamecube\"\n" +
            "[video]\nhdr = false\nhdr_paper_white_nits = 237.5125\nhdr_peak_nits = 1234.5678\n[future]\nvalue = \"keep\"\n";
        File.WriteAllText(path, initial);
        var owner = new MainWindow(null, new DataPaths(fixture, game), enableServices: false);
        owner.Show();
        var window = owner.CreateHdrWindow();
        var modal = Dialogs.ShowModal(window, owner);
        Pump();
        Check(window.Owner == owner && !modal.IsCompleted, "HDR window bypassed owned modal routing");
        Check(File.ReadAllText(path) == initial, "Opening HDR window changed explicit off/manual settings");
        Check(window.PaperSlider.Value == 237.5125 && window.PeakSlider.Value == 1234.5678,
              "Opening HDR window rounded saved high-precision adjustments");
        Check(window.PaperSlider.IsFocused && window.PaperSlider.Focusable && window.PeakSlider.Focusable,
              "HDR sliders are not keyboard accessible");
        Check(AutomationProperties.GetName(window.PaperSlider) == "Paper white in nits", "Slider lacks accessible name");
        Check(window.PaperSlider.Minimum == 80 && window.PaperSlider.Maximum == 1000 &&
              window.PeakSlider.Minimum == 80 && window.PeakSlider.Maximum == 10000, "Slider bounds differ from runtime");
        Check(window.PaperSlider is ControllerSlider { ControllerStep: 10 } &&
              window.PeakSlider is ControllerSlider { ControllerStep: 100 } &&
              window.PaperSlider.SmallChange == 0.1 && window.PeakSlider.SmallChange == 0.1,
              "HDR coarse metadata or fine keyboard/D-pad increments changed");
        window.PaperSlider.Value = 320.45;
        Pump();
        Check(window.PaperSlider.Value == 320.5 && new GameConfig(path).Get("video", "hdr_paper_white_nits") == "320.5",
              "Slider/readout/persistence precision differs");
        Check(window.GetVisualDescendants().OfType<TextBlock>().Any(x => x.Text == "320.5 nits"), "Numeric nit readout missing");
        window.PaperSlider.Focus();
        window.KeyPress(Key.Right, RawInputModifiers.None, PhysicalKey.ArrowRight, null);
        Pump();
        Check(Math.Abs(window.PaperSlider.Value - 320.6) < 0.00001, "Keyboard slider step is not 0.1 nit");
        foreach (double endpoint in new[] { 80d, 1000d })
        {
            window.PaperSlider.Value = endpoint;
            Check(new GameConfig(path).Get("video", "hdr_paper_white_nits") == endpoint.ToString(), "Paper endpoint not saved");
        }
        foreach (double endpoint in new[] { 80d, 10000d })
        {
            window.PeakSlider.Value = endpoint;
            Check(new GameConfig(path).Get("video", "hdr_peak_nits") == endpoint.ToString(), "Peak endpoint not saved");
        }
        var enabled = window.GetVisualDescendants().OfType<ThemedCombo>().Single();
        int committed = 0;
        enabled.SelectionCommitted += (_, _) => committed++;
        enabled.CommitSelection(0);
        Check(committed == 1, "Explicit same-value HDR off did not commit");
        enabled.CommitSelection(1);
        enabled.CommitSelection(0);
        var config = new GameConfig(path);
        Check(config.Get("video", "hdr") == "false" && config.Get("launcher", "auto_controller_layout") == "true" &&
              config.Get("launcher", "auto_controller_layout_value") == "gamecube" && config.Get("future", "value") == "keep",
              "HDR edits changed controller intent or unrelated preferences");
        window.Width = 500;
        window.Height = 680;
        Pump();
        using (var bitmap = window.GetLastRenderedFrame() ?? throw new Exception("Headless HDR frame unavailable"))
            bitmap.Save(Path.Combine(output, "hdr-avalonia-headless.png"), PngBitmapEncoderOptions.Default);
        File.SetAttributes(path, FileAttributes.ReadOnly);
        int errors = 0;
        Dialogs.MessageOverride = (errorOwner, text) =>
        {
            Check(errorOwner == window && text.Contains("Couldn't save HDR settings"), "HDR save error bypassed modal owner");
            errors++;
            return Task.CompletedTask;
        };
        try
        {
            window.PeakSlider.Value = 500;
            Check(errors == 1 && new GameConfig(path).Get("video", "hdr_peak_nits") == "10000", "Save failure hidden");
        }
        finally { File.SetAttributes(path, FileAttributes.Normal); Dialogs.MessageOverride = null; }
        string saved = File.ReadAllText(path);
        window.KeyPress(Key.Escape, RawInputModifiers.None, PhysicalKey.Escape, null);
        Pump();
        Check(modal.IsCompleted && !window.IsVisible, "Escape did not close canonical HDR modal");
        var reopened = owner.CreateHdrWindow();
        reopened.Show();
        Pump();
        Check(reopened.PaperSlider.Value == 1000 && reopened.PeakSlider.Value == 10000 && File.ReadAllText(path) == saved,
              "Reopening HDR window lost saved slider values");
        reopened.Close();
        owner.Close();
    }
}
