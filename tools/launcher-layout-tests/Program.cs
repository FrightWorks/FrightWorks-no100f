using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Automation.Peers;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using FrightWorks;
using Rectangle = System.Drawing.Rectangle;

// Layout regression checks for the launcher, on its real window and pages, without a screen (Avalonia's headless
// platform drawn with Skia, the same renderer as on screen). Windows and Linux. Writes page screenshots to the output
// directory; never starts the game or touches the player's files (a disposable portable fixture).
static class LauncherLayoutTests
{
    static int checks;

    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new InvalidOperationException(message);
    }

    static IEnumerable<Visual> Descendants(Visual v) => v.GetVisualDescendants();

    static void Pump()
    {
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        Dispatcher.UIThread.RunJobs();
    }

    // Simulated monitor layouts: the same cases and expected text as the game's wiicompiled/runtime/tests/monitor_labels_tests.cpp.
    static void CheckMonitorLabels()
    {
        (string, Rectangle, bool) M(string name, int x, int y, int w, int h, bool primary = false) => (name, new Rectangle(x, y, w, h), primary);
        void Case(string name, (string, Rectangle, bool)[] layout, params string[] expected)
        {
            var got = Monitors.Labels(layout.ToList());
            Check(got.SequenceEqual(expected), $"Monitor labels, {name}: {string.Join(" | ", got)}");
        }
        Case("no clash: names and places differ",
            new[] { M("XB271HU", 0, 0, 2560, 1440, true), M("G215HV", 640, -1080, 1920, 1080), M("VS248", 2560, 0, 1920, 1080), M("VS248", 2560, -1080, 1920, 1080) },
            "XB271HU (2560x1440, main)", "G215HV (1920x1080, above)", "VS248 (1920x1080, right)", "VS248 (1920x1080, above right)");
        Case("two matching to the right: numbered left to right, whatever the list order",
            new[] { M("Main", 0, 0, 2560, 1440, true), M("VS248", 4480, 0, 1920, 1080), M("VS248", 2560, 0, 1920, 1080) },
            "Main (2560x1440, main)", "VS248 (1920x1080, right 2)", "VS248 (1920x1080, right 1)");
        Case("same left edge: numbered top to bottom",
            new[] { M("Main", 0, 0, 2560, 1440, true), M("VS248", 2560, 980, 1920, 1080), M("VS248", 2560, -100, 1920, 1080) },
            "Main (2560x1440, main)", "VS248 (1920x1080, right 2)", "VS248 (1920x1080, right 1)");
        Case("each clash counts on its own",
            new[] { M("Main", 0, 0, 2560, 1440, true), M("VS248", -1920, 0, 1920, 1080), M("VS248", -3840, 0, 1920, 1080), M("VS248", 2560, 0, 1920, 1080), M("VS248", 4480, 0, 1920, 1080) },
            "Main (2560x1440, main)", "VS248 (1920x1080, left 2)", "VS248 (1920x1080, left 1)", "VS248 (1920x1080, right 1)", "VS248 (1920x1080, right 2)");
        Case("same place but another size or name: no number",
            new[] { M("Main", 0, 0, 2560, 1440, true), M("VS248", 2560, 0, 1920, 1080), M("VS248", 4480, 0, 1280, 1024), M("G215HV", 5760, 0, 1920, 1080) },
            "Main (2560x1440, main)", "VS248 (1920x1080, right)", "VS248 (1280x1024, right)", "G215HV (1920x1080, right)");
        Case("below and above left",
            new[] { M("Main", 0, 0, 2560, 1440, true), M("VS248", 0, 1440, 1920, 1080), M("VS248", -1920, -1080, 1920, 1080) },
            "Main (2560x1440, main)", "VS248 (1920x1080, below)", "VS248 (1920x1080, above left)");
        Case("overlapping the main monitor: no place word, number only on a clash",
            new[] { M("Main", 0, 0, 2560, 1440, true), M("VS248", 100, 100, 1920, 1080), M("VS248", 200, 100, 1920, 1080), M("G215HV", 300, 100, 1920, 1080) },
            "Main (2560x1440, main)", "VS248 (1920x1080, 1)", "VS248 (1920x1080, 2)", "G215HV (1920x1080)");
    }

    // The control drawn on its own over a transparent bitmap: its four corners must stay see-through (a plank, not a box).
    static void CheckCorners(PlankButton button, string state, string output)
    {
        var size = new PixelSize(Math.Max(1, (int)Math.Ceiling(button.Bounds.Width)), Math.Max(1, (int)Math.Ceiling(button.Bounds.Height)));
        using var bitmap = new RenderTargetBitmap(size);
        bitmap.Render(button);
        bitmap.Save(Path.Combine(output, $"button-{button.Text}-{state}.png"), PngBitmapEncoderOptions.Default);
        var pixels = new byte[size.Width * size.Height * 4];
        var handle = GCHandle.Alloc(pixels, GCHandleType.Pinned);
        try { bitmap.CopyPixels(new PixelRect(size), handle.AddrOfPinnedObject(), pixels.Length, size.Width * 4); }
        finally { handle.Free(); }
        foreach (var (x, y) in new[] { (0, 0), (size.Width - 1, 0), (0, size.Height - 1), (size.Width - 1, size.Height - 1) })
        {
            byte alpha = pixels[(y * size.Width + x) * 4 + 3];
            Check(alpha == 0, $"{button.Text} ({state}): corner ({x},{y}) is painted (alpha {alpha}), not see-through");
        }
    }

    static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("Usage: LauncherLayoutTests <scratch-output-directory>");
            return 2;
        }
        string output = Path.GetFullPath(args[0]);
        Directory.CreateDirectory(output);
        string fixture = Path.Combine(output, "fixture-" + Guid.NewGuid().ToString("N"));
        string game = Path.Combine(fixture, "Play");
        string userData = Path.Combine(game, "UserData");
        Directory.CreateDirectory(userData);
        File.WriteAllText(Path.Combine(game, "portable.txt"), "");
        string config = Path.Combine(userData, "Config.toml");
        const string initialConfig = "# Preserve custom calibration and unknown values.\n[video]\nhdr = true\nhdr_paper_white_nits = 237.5\nhdr_peak_nits = 1234.0\nunknown_setting = 42\nfullscreen_display = \"NOPE0000/gone\"\n[controller]\nlayout = \"xbox_original\"\n";
        File.WriteAllText(config, initialConfig);
        for (int i = 0; i < 20; i++)
            Directory.CreateDirectory(Path.Combine(userData, "mods", $"{i:D2}-" + new string('M', 100)));
        // A patch mod with a problem (its status line must fit under it), and the generated folder (never listed).
        Directory.CreateDirectory(Path.Combine(userData, "mods", "patch-mod"));
        File.WriteAllText(Path.Combine(userData, "mods", "patch-mod", "patch.json"), "{ \"format\": 9 }");
        Directory.CreateDirectory(Path.Combine(userData, "mods", "!patched"));

        AppBuilder.Configure<App>().UseSkia().UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
                  .WithInterFont().SetupWithoutStarting();
        try
        {
            Run(fixture, game, config, initialConfig, output);
            Console.WriteLine($"PASS {checks} assertions.");
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e);
            return 1;
        }
        finally
        {
            try { Directory.Delete(fixture, true); } catch (Exception) { }
        }
    }

    static void Run(string fixture, string game, string config, string initialConfig, string output)
    {
        CheckMonitorLabels();

        var window = new MainWindow(null, new DataPaths(fixture, game), enableServices: false) { Title = "Launcher layout regression checks" };
        window.Show();
        Pump();
        // The fixture never starts SDL; refresh only fills the managed status line.
        _ = window.RefreshGamepads();
        Pump();
        var pads = typeof(MainWindow).GetField("gamepads", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.GetValue(window)!;
        Check(typeof(Gamepads).GetField("thread", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.GetValue(pads) == null,
              "Service-disabled fixture started the SDL controller thread");
        Check(!window.GameOwnsControllers && File.ReadAllText(config) == initialConfig,
              "Service-disabled construction/refresh changed ownership or saved preferences");
        Check(window.padStatus.Text?.StartsWith("Looking") == false, "Controller detection status was not filled in");
        window.scene.Showcase(false);

        // ----- Mods page: the status of a patch mod appears once its background check is done; the generated folder stays out of the list -----
        window.ShowBoard("mods");
        var modTexts = () => Descendants(window.boards["mods"]).OfType<TextBlock>().Select(t => t.Text ?? "").ToList();
        for (int i = 0; i < 100 && !modTexts().Any(t => t.Contains("patch format 9")); i++)
        {
            Thread.Sleep(50);
            Pump();
        }
        Check(modTexts().Any(t => t.Contains("patch format 9")), "A patch mod's problem isn't shown on the Mods page");
        Check(modTexts().Contains("patch-mod"), "A patch mod isn't listed");
        Check(!modTexts().Contains("!patched"), "The generated folder is listed as a mod");
        Check(Descendants(window.boards["mods"]).OfType<PlankButton>().Any(b => b.Text == "Add a mod (zip)"), "No button to add a mod");
        window.CloseBoards();
        Pump();

        // ----- Planks: see-through corners in every state -----
        window.ShowBoard("settings");
        window.showControlsTab!(false);
        Pump();
        var settings = window.boards["settings"];
        foreach (var button in Descendants(settings).OfType<PlankButton>().Where(b => b.IsEffectivelyVisible).ToArray())
        {
            var center = button.TranslatePoint(new Point(button.Bounds.Width / 2, button.Bounds.Height / 2), window)!.Value;
            CheckCorners(button, "normal", output);
            window.MouseMove(center);
            Pump();
            Check(button.IsPointerOver, $"{button.Text}: the pointer over it doesn't count as hover");
            CheckCorners(button, "hover", output);
            window.MouseDown(center, MouseButton.Left);
            Pump();
            CheckCorners(button, "pressed", output);
            window.MouseMove(new Point(1, 1));
            window.MouseUp(new Point(1, 1), MouseButton.Left);  // released elsewhere: no click
            button.IsEnabled = false;
            Pump();
            CheckCorners(button, "disabled", output);
            button.IsEnabled = true;
        }
        window.showControlsTab!(false);

        void Validate(string name, bool controls = false)
        {
            window.ShowBoard(name);
            if (name == "settings") window.showControlsTab!(controls);
            Pump();
            var board = window.boards[name];
            var stage = (Control)window.Content!;
            string page = controls ? "controls" : name;
            window.SaveShot(Path.Combine(output, $"{window.ClientSize.Width:0}x{window.ClientSize.Height:0}-{page}.png"));
            Check(new Rect(stage.Bounds.Size).Contains(board.Bounds), $"{page}: board {board.Bounds} escapes the window {stage.Bounds.Size}");
            var scroller = board.Scroller;
            Check(scroller.Extent.Width <= scroller.Viewport.Width + 1,
                $"{page}: content {scroller.Extent.Width} is wider than the page {scroller.Viewport.Width} at {window.ClientSize}");
            Check(board.Body.Bounds.Right <= scroller.Viewport.Width + 1, $"{page}: page body wider than its viewport");
            string Describe(Control c) => $"{c.GetType().Name} '{(c as TextBlock)?.Text ?? (c as PlankButton)?.Text ?? (c as ThemedCombo)?.SelectedItem}'" +
                (c is TextBlock or PlankButton or ThemedCombo ? "" : $" in {string.Join(" < ", c.GetVisualAncestors().Take(3).Select(a => a.GetType().Name + (a is Control { Name: { } n } ? "#" + n : "")))}");
            double Right(Control c) => c.TranslatePoint(new Point(c.Bounds.Width, 0), board.Body)!.Value.X;
            var visible = Descendants(board.Body).OfType<Control>().Where(c => c.IsEffectivelyVisible && c.Bounds.Width > 0).ToList();
            // Report the innermost control that sticks out (its panels stick out with it).
            var outside = visible.Where(c => Right(c) > board.Body.Bounds.Width + 1).ToList();
            var innermost = outside.Where(c => !outside.Any(o => o != c && o.GetVisualAncestors().Contains(c))).ToList();
            Check(innermost.Count == 0, $"{page}: sticks out of the {board.Body.Bounds.Width:0} wide page: " +
                  string.Join("; ", innermost.Take(5).Select(c => $"{Describe(c)} to {Right(c):0} (width {c.Bounds.Width:0})")));
            foreach (var box in visible.OfType<ThemedCombo>())
                Check(box.Bounds.Width >= 80, $"{page}: unusably narrow drop-down ({box.Bounds.Width:0})");
            // The wheel over a closed drop-down scrolls the page and never changes (and saves) a setting.
            var firstBox = Descendants(board.Body).OfType<ThemedCombo>().FirstOrDefault(c => c.IsEffectivelyVisible);
            if (firstBox != null)
            {
                int selected = firstBox.SelectedIndex;
                var over = firstBox.TranslatePoint(new Point(firstBox.Bounds.Width / 2, firstBox.Bounds.Height / 2), window)!.Value;
                window.MouseWheel(over, new Vector(0, -1));
                Pump();
                Check(firstBox.SelectedIndex == selected, $"{page}: the wheel over a closed drop-down changed a setting");
                scroller.ScrollToHome();
                Pump();
            }
            string Signature() => string.Join("\n", Descendants(board).OfType<Control>().Where(c => c.IsEffectivelyVisible)
                .Select(c => $"{c.GetType().Name} {(c as TextBlock)?.Text ?? (c as PlankButton)?.Text} {c.Bounds}"));
            string before = Signature();
            window.CloseBoards();
            Pump();
            window.ShowBoard(name);
            Pump();
            string after = Signature();
            if (before != after)
            {
                File.WriteAllText(Path.Combine(output, "unstable-before.txt"), before);
                File.WriteAllText(Path.Combine(output, "unstable-after.txt"), after);
            }
            Check(before == after, $"{page}: reopening changes layout");
            Console.WriteLine($"PASS {window.ClientSize.Width:0}x{window.ClientSize.Height:0} {page}: viewport {scroller.Viewport}, content {scroller.Extent}");
        }

        // Normal, minimum, below the minimum (a tiling window manager ignores it), very narrow, and back.
        window.MinWidth = window.MinHeight = 0;
        foreach (var size in new[] { new Size(1180, 830), new Size(960, 680), new Size(820, 600), new Size(500, 547), new Size(1180, 830) })
        {
            window.Width = size.Width;
            window.Height = size.Height;
            Pump();
            Check(Math.Abs(window.ClientSize.Width - size.Width) < 1, $"window did not take {size}: {window.ClientSize}");
            Validate("settings");
            Validate("settings", controls: true);
            Validate("mods");
            Validate("extras");
        }
        Check(File.ReadAllText(config) == initialConfig, "Opening, resizing, or navigation changed configuration");

        // ----- Keyboard and accessibility -----
        window.ShowBoard("settings");
        window.showControlsTab!(true);
        Pump();
        var displayTab = Descendants(settings).OfType<PlankButton>().Single(b => b.Text == "Display");
        Check(displayTab.Focusable && displayTab.IsEffectivelyEnabled, "Display tab is not keyboard focusable");
        Check(ControlAutomationPeer.CreatePeerForElement(displayTab).GetAutomationControlType() == AutomationControlType.Button,
              "Themed button lacks a button role");
        Check(Descendants(settings).OfType<PlankButton>().Any(b => AutomationProperties.GetName(b) == "Close Settings"), "Close button lacks an accessible name");
        displayTab.Focus(NavigationMethod.Tab);
        window.KeyPress(Key.Space, RawInputModifiers.None, PhysicalKey.Space, " ");
        window.KeyRelease(Key.Space, RawInputModifiers.None, PhysicalKey.Space, " ");
        Pump();
        var display = (StackPanel)settings.Body.Children[1];
        Check(display.IsVisible, "Space did not select the Display tab");

        // ----- Settings are saved, and nothing else in the file changes -----
        var boxes = Descendants(display).OfType<ThemedCombo>().ToList();
        boxes[0].SelectedIndex = 1;
        string saved = File.ReadAllText(config);
        Check(saved.Contains("display_mode = \"windowed\""), "Changed display setting was not saved");
        Check(saved.Contains("hdr = true") && saved.Contains("hdr_paper_white_nits = 237.5") &&
              saved.Contains("hdr_peak_nits = 1234.0") && saved.Contains("unknown_setting = 42"),
              "Changing another setting overwrote custom HDR/unknown values");
        bool hasMonitorRow = Descendants(display).OfType<TextBlock>().Any(t => t.Text == "Fullscreen monitor");
        if (MainWindow.MonitorChoice)
        {
            Check(hasMonitorRow, "Fullscreen monitor row is missing");
            var monitorBox = boxes[1];
            Check((string)monitorBox.SelectedItem! == "Saved monitor (not connected)", "A missing monitor is not shown as saved");
            Check(saved.Contains("fullscreen_display = \"NOPE0000/gone\""), "Another setting dropped the saved monitor");
            monitorBox.SelectedIndex = 0;
            Check(File.ReadAllText(config).Contains("fullscreen_display = \"auto\""), "Automatic monitor was not saved");
        }
        else
        {
            Check(!hasMonitorRow, "Fullscreen monitor row shown where the game can't pick a monitor");
            Check(saved.Contains("fullscreen_display = \"NOPE0000/gone\""), "Another setting dropped the saved monitor");
        }

        // ----- A Config.toml that can't be written: the launcher asks Continue or Quit, it doesn't close -----
        string beforeReadOnly = File.ReadAllText(config);
        File.SetAttributes(config, FileAttributes.ReadOnly);
        Fault.Owner = window;
        try
        {
            Dispatcher.UIThread.Post(() => boxes[0].SelectedIndex = 0);  // a changed setting, run as a posted job
            Window? shown = null;
            for (var waited = System.Diagnostics.Stopwatch.StartNew(); shown == null && waited.Elapsed.TotalSeconds < 5;)
            {
                Thread.Sleep(10);
                Pump();
                shown = window.OwnedWindows.FirstOrDefault();
            }
            Check(shown != null, "A setting that can't be saved showed no dialog");
            Window fault = shown!;
            var faultText = string.Join(" | ", Descendants(fault).OfType<TextBlock>().Select(t => t.Text));
            Check(faultText.Contains("Config.toml") && faultText.Contains("UnauthorizedAccessException"), "The dialog doesn't name the problem: " + faultText);
            var faultButtons = Descendants(fault).OfType<PlankButton>().Select(b => b.Text).ToList();
            Check(faultButtons.SequenceEqual(new[] { "Continue", "Quit" }), "The dialog's buttons are " + string.Join(",", faultButtons));
            var @continue = Descendants(fault).OfType<PlankButton>().Single(b => b.Text == "Continue");
            var press = @continue.TranslatePoint(new Point(@continue.Bounds.Width / 2, @continue.Bounds.Height / 2), fault)!.Value;
            fault.MouseMove(press);
            fault.MouseDown(press, MouseButton.Left);
            fault.MouseUp(press, MouseButton.Left);
            Pump();
            Check(!fault.IsVisible && window.IsVisible, "Continue did not close the dialog and leave the launcher open");
            Check(File.ReadAllText(config) == beforeReadOnly, "A failed save changed the configuration");

            // An unawaited task that fails reports too (the `_ = Task()` handlers), and a second fault doesn't stack dialogs.
            var faults = new List<Exception>();
            Fault.Override = e => { faults.Add(e); return Task.CompletedTask; };
            Task.Run(() => throw new IOException("background task")).Watch();
            for (var waited = System.Diagnostics.Stopwatch.StartNew(); faults.Count == 0 && waited.Elapsed.TotalSeconds < 5;) { Thread.Sleep(10); Pump(); }
            Check(faults.Count == 1 && faults[0].Message == "background task", "A failed unawaited task was not reported");
            Dispatcher.UIThread.Post(() => throw new InvalidOperationException("posted job"));
            Pump();
            Check(faults.Count == 2 && faults[1].Message == "posted job", "An exception in a posted job was not reported");
            Check(window.IsVisible && !window.OwnedWindows.Any(), "A reported fault left the launcher closed or a dialog behind");
        }
        finally
        {
            Fault.Override = null;
            Fault.Owner = null;
            File.SetAttributes(config, FileAttributes.Normal);
        }

        // ----- The Xbox-only button rows follow the layout -----
        window.layoutBox.SelectedIndex = 0;
        window.showControlsTab!(true);
        Pump();
        var xboxLabel = Descendants(settings).OfType<TextBlock>().Single(l => l.Text == "Black (sneak)");
        Check(!xboxLabel.IsVisible, "GameCube layout did not hide Xbox-only mapping");
        window.layoutBox.SelectedIndex = 1;
        Pump();
        Check(xboxLabel.IsVisible, "Xbox layout did not restore Xbox-only mapping");

        // ----- Play starts the game (Linux: a stand-in script for the game) and the launcher steps aside -----
        if (!OperatingSystem.IsWindows())
        {
            var sys = Path.Combine(fixture, "Disc", "DATA", "sys");
            Directory.CreateDirectory(sys);
            File.WriteAllBytes(Path.Combine(sys, "boot.bin"), System.Text.Encoding.ASCII.GetBytes("GIHE78\0\u0001"));
            string stub = Path.Combine(game, DataPaths.GameExe), marker = Path.Combine(game, "started.txt");
            File.WriteAllText(stub, "#!/bin/sh\npwd > started.txt\n");
            File.SetUnixFileMode(stub, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            window.CloseBoards();
            Pump();
            var play = window.scene.SignCenter("play")!.Value;
            var at = window.scene.TranslatePoint(play, window)!.Value;
            window.MouseMove(at);
            window.MouseDown(at, MouseButton.Left);
            window.MouseUp(at, MouseButton.Left);
            for (var waited = System.Diagnostics.Stopwatch.StartNew(); !File.Exists(marker) && waited.Elapsed.TotalSeconds < 10;)
            {
                Thread.Sleep(25);
                Pump();
            }
            Check(File.Exists(marker), "Play did not start the game");
            Check(File.ReadAllText(marker).Trim() == game, "The game did not start in its own folder");
            Check(window.WindowState == WindowState.Minimized, "The launcher did not minimise when the game started");

            // A game that ends with an error brings the launcher back with a message (it used to look like Play did nothing).
            File.WriteAllText(stub, "#!/bin/sh\nexit 3\n");
            window.WindowState = WindowState.Normal;
            Thread.Sleep(300);
            Pump();
            window.MouseMove(at);
            window.MouseDown(at, MouseButton.Left);
            window.MouseUp(at, MouseButton.Left);
            Window? dialog = null;
            for (var waited = System.Diagnostics.Stopwatch.StartNew(); dialog == null && waited.Elapsed.TotalSeconds < 10;)
            {
                Thread.Sleep(25);
                Pump();
                dialog = window.OwnedWindows.FirstOrDefault();
            }
            Check(dialog != null, "A game that failed at start left no message");
            Check(Descendants(dialog!).OfType<TextBlock>().Any(t => t.Text?.Contains("exit code 3") == true), "The message doesn't give the exit code");
            Check(window.WindowState == WindowState.Normal, "The launcher stayed minimised after the game failed");
            dialog!.Close();
            Pump();
        }

        window.Close();
        Pump();
    }
}
