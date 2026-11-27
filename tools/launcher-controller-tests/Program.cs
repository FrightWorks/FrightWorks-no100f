using System.Reflection;
using Avalonia;
using Avalonia.Automation.Peers;
using Avalonia.Automation.Provider;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using FrightWorks;

static class LauncherControllerTests
{
    static int checks;
    static long time;
    static string output = "";
    static void Check(bool value, string message) { checks++; if (!value) throw new InvalidOperationException(message); }
    static void Pump()
    {
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        Dispatcher.UIThread.RunJobs();
    }
    static void Finish(Task task)
    {
        for (int i = 0; !task.IsCompleted && i < 200; i++) { Pump(); Thread.Sleep(5); }
        Check(task.IsCompleted, "An asynchronous controller operation did not finish.");
        task.GetAwaiter().GetResult();
    }
    static void Feed(MainWindow w, LauncherButtons buttons, Window? target = null, bool active = true, bool game = false, uint id = 1)
    {
        w.navigation.Feed(active ? target ?? w.navigation.Modal ?? w : null, new[] { new ControllerSample(id, buttons) }, time += 20, game);
        Pump();
    }
    static void Press(MainWindow w, LauncherButtons buttons)
    {
        Feed(w, LauncherButtons.None);
        Feed(w, buttons);
        Feed(w, LauncherButtons.None);
    }
    // Rows of a popup list that are fully inside its viewport, and whether it scrolls.
    static (int Visible, bool Scrolls, int Rows) PopupRows(Window popup)
    {
        var list = popup.GetVisualDescendants().OfType<ControllerList>().First();
        var scroll = list.GetVisualDescendants().OfType<ScrollViewer>().First();
        var rows = list.GetVisualDescendants().OfType<ListBoxItem>().ToList();
        int visible = rows.Count(r => r.TranslatePoint(default, list) is Point top && top.Y >= -1 && top.Y + r.Bounds.Height <= scroll.Viewport.Height + 1);
        return (visible, scroll.Extent.Height > scroll.Viewport.Height + 1, rows.Count);
    }
    static T Find<T>(Window w) where T : Control => w.GetVisualDescendants().OfType<T>().First();
    static bool InView(MenuScreen menu, Control c) => c.TranslatePoint(default, menu.Scroller) is Point top &&
        top.Y >= -1 && top.Y + c.Bounds.Height <= menu.Scroller.Bounds.Height + 1;
    static void Wait(Func<bool> done) { for (int i = 0; i < 200 && !done(); i++) { Pump(); Thread.Sleep(10); } Pump(); }
    static PlankButton Button(Window w, string name) => w.GetVisualDescendants().OfType<PlankButton>().First(b => b.Text == name);
    // The file browser: its menu, its folder and file rows (they carry their path), and picking a row with the pad.
    static MenuScreen Screen(Window w) => w.GetVisualDescendants().OfType<MenuScreen>().First();
    static List<MenuRow> Entries(Window w) => Screen(w).Rows.Where(r => r.Tag is string).ToList();
    static void Choose(MainWindow owner, Window window, string label)
    {
        var screen = Screen(window);
        screen.Select(screen.Rows.First(r => r.Label == label)); Pump();
        Press(owner, LauncherButtons.Confirm);
    }
    static void Click(MainWindow owner, Window window, string name)
    {
        Button(window, name).Focus(NavigationMethod.Directional);
        Press(owner, LauncherButtons.Confirm);
    }
    static void Shot(Window w, string name)
    {
        Pump();
        using var bitmap = new RenderTargetBitmap(new PixelSize((int)w.Bounds.Width, (int)w.Bounds.Height));
        bitmap.Render(w);
        bitmap.Save(Path.Combine(output, name + ".png"), PngBitmapEncoderOptions.Default);
    }

    static int Main(string[] args)
    {
        if (args.Length != 1) { Console.Error.WriteLine("Usage: LauncherControllerTests <owned-scratch-directory>"); return 2; }
        output = Path.GetFullPath(args[0]);
        Directory.CreateDirectory(output);
        string fixture = Path.Combine(output, "fixture-" + Guid.NewGuid().ToString("N"));
        string game = Path.Combine(fixture, "Play"), data = Path.Combine(game, "UserData");
        Directory.CreateDirectory(data);
        File.WriteAllText(Path.Combine(game, "portable.txt"), "");
        string config = Path.Combine(data, "Config.toml");
        File.WriteAllText(config, "[controller]\nlayout = \"gamecube\"\na = \"east@0.75,south\"\n[launcher]\nauto_controller_layout = true\n[video]\nhdr_paper_white_nits = 237.5\nunknown = 42\n");
        AppBuilder.Configure<App>().UseSkia().UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false }).WithInterFont().SetupWithoutStarting();
        AvaloniaSynchronizationContext.InstallIfNeeded();
        var errors = new List<Exception>();
        Fault.Override = e => { errors.Add(e); return Task.CompletedTask; };
        try
        {
            Pure(config);
            Run(new DataPaths(fixture, game), config);
            Hdr(new DataPaths(fixture, game), config);
            Check(errors.Count == 0, "Unexpected routed error: " + string.Join("; ", errors.Select(e => e.ToString())));
            Console.WriteLine($"PASS {checks} controller assertions (headless, services disabled; no SDL/game/native UI).");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e); return 1; }
        finally { Fault.Override = null; Directory.Delete(fixture, true); }
    }

    // The HDR window (#116) with the pad (#115): the Display page's button opens it as a registered modal, the left stick
    // steps each slider by its coarse step and the D-pad by 0.1 nit, ends clamp, the HDR dropdown uses the themed popup,
    // Back closes it. The fixture file is put back afterwards.
    static void Hdr(DataPaths paths, string config)
    {
        string saved = File.ReadAllText(config);
        var w = new MainWindow(null, paths, false);
        Fault.Owner = w;
        w.Show(); Pump();
        void Stick(HdrWindow hdr, LauncherButtons buttons, bool coarse)
        {
            w.navigation.Feed(hdr, new[] { new ControllerSample(1, LauncherButtons.None) }, time += 20, false);
            w.navigation.Feed(hdr, new[] { new ControllerSample(1, buttons, coarse) }, time += 20, false);
            Pump();
            w.navigation.Feed(hdr, new[] { new ControllerSample(1, LauncherButtons.None) }, time += 20, false);
        }
        try
        {
            Press(w, LauncherButtons.Next);
            Press(w, LauncherButtons.Confirm);
            Check(w.boards["settings"].IsVisible, "Controller did not open Settings for the HDR checks.");
            var settings = w.boards["settings"];
            var adjust = settings.Rows.OfType<ActionRow>().Single(r => r.Label == "HDR");
            while (settings.Selected != adjust && settings.Selected != settings.Rows.Last()) Press(w, LauncherButtons.Down);
            Check(settings.Selected == adjust, "Down did not reach the HDR row.");
            Press(w, LauncherButtons.Confirm);
            var hdr = w.navigation.Modal as HdrWindow;
            Check(hdr != null && hdr.IsVisible && hdr.Owner == w, "Adjust HDR did not open the HDR window as the registered modal.");
            Check(hdr!.PaperSlider.IsFocused, "The HDR window did not start on the paper white slider.");
            double paper = hdr.PaperSlider.Value, peak = hdr.PeakSlider.Value;
            Stick(hdr, LauncherButtons.Right, true);
            Check(Math.Abs(hdr.PaperSlider.Value - (paper + 10)) < 1e-6 && Math.Abs(hdr.PeakSlider.Value - peak) < 1e-6,
                  "Left stick did not step paper white by 10 nits.");
            Stick(hdr, LauncherButtons.Right, false);
            Check(Math.Abs(hdr.PaperSlider.Value - (paper + 10.1)) < 1e-6, "D-pad did not step paper white by 0.1 nit.");
            Check(new GameConfig(config).Get("video", "hdr_paper_white_nits") == HdrSettings.Raw((decimal)Math.Round(paper + 10.1, 1)),
                  "A pad step on paper white was not saved with its precision.");
            hdr.PaperSlider.Value = 1000;
            Stick(hdr, LauncherButtons.Right, true);
            Check(hdr.PaperSlider.Value == 1000, "Left stick stepped paper white past its maximum.");
            hdr.PaperSlider.Value = 85;
            Stick(hdr, LauncherButtons.Left, true);
            Check(hdr.PaperSlider.Value == 80, "Left stick stepped paper white below its minimum.");
            Press(w, LauncherButtons.Down);
            Check(hdr.PeakSlider.IsFocused, "Down did not move from paper white to peak highlights.");
            Stick(hdr, LauncherButtons.Right, true);
            Check(Math.Abs(hdr.PeakSlider.Value - (peak + 100)) < 1e-6, "Left stick did not step peak highlights by 100 nits.");
            Stick(hdr, LauncherButtons.Left, false);
            Check(Math.Abs(hdr.PeakSlider.Value - (peak + 99.9)) < 1e-6, "D-pad did not step peak highlights by 0.1 nit.");
            Check(new GameConfig(config).Get("video", "hdr_paper_white_nits") == "80" &&
                  new GameConfig(config).Get("video", "unknown") == "42", "HDR pad steps lost the paper white value or an unknown key.");
            // The HDR dropdown is the themed popup, and a pad choice (including the same value) is committed.
            Press(w, LauncherButtons.Up); Press(w, LauncherButtons.Up);
            var enabled = hdr.GetVisualDescendants().OfType<ThemedCombo>().Single();
            Check(enabled.IsFocused, "Up did not reach the HDR dropdown.");
            Press(w, LauncherButtons.Confirm);
            var popup = w.navigation.Modal!;
            Check(popup != hdr && popup.Title == "HDR", "The HDR dropdown did not open the controller popup.");
            Press(w, LauncherButtons.Down);
            Press(w, LauncherButtons.Confirm);
            Check(w.navigation.Modal == hdr && new GameConfig(config).Get("video", "hdr") == "true", "Choosing HDR On with the pad did not save it.");
            Press(w, LauncherButtons.Confirm);
            Press(w, LauncherButtons.Confirm);
            Check(w.navigation.Modal == hdr && new GameConfig(config).Get("video", "hdr") == "true", "Choosing the same HDR value with the pad changed it.");
            Press(w, LauncherButtons.Back);
            Check(w.navigation.Modal == null && !hdr.IsVisible, "Back did not close the HDR window.");
            Check(settings.Selected == adjust && adjust.IsFocused, "Closing the HDR window did not return to the HDR row.");
        }
        finally
        {
            w.Close(); Pump(); Fault.Owner = null;
            File.WriteAllText(config, saved);
        }
    }

    static void Pure(string path)
    {
        var input = new ControllerInput();
        Check(input.Read(LauncherButtons.Confirm, 0) == 0, "Held hotplug confirm was not suppressed.");
        Check(input.Read(0, 1) == 0 && input.Read(LauncherButtons.Down, 2) == LauncherButtons.Down, "Neutral did not arm movement.");
        Check(input.Read(LauncherButtons.Down, 401) == 0, "Movement repeated before 400ms.");
        Check(input.Read(LauncherButtons.Down, 402) == LauncherButtons.Down, "Movement did not repeat at 400ms.");
        Check(input.Read(LauncherButtons.Down, 501) == 0 && input.Read(LauncherButtons.Down, 502) == LauncherButtons.Down, "100ms repeat threshold.");
        input.Reset(); input.Read(0, 0);
        Check(input.Read(LauncherButtons.Next, 1) == LauncherButtons.Next && input.Read(LauncherButtons.Next, 500) == 0 &&
              input.Read(LauncherButtons.Next, 3000) == 0, "A held bumper repeated (tabs would race).");
        input.Reset();
        input.Read(0, 0);
        Check(input.Read(LauncherButtons.Confirm, 1) == LauncherButtons.Confirm && input.Read(LauncherButtons.Confirm, 1000) == 0, "Confirm repeated.");
        input.Reset(); input.Read(0, 0);
        Check(input.Read(LauncherButtons.Up | LauncherButtons.Down, 1) == 0, "Opposing movement did not cancel.");
        Check(ControllerInput.FromSdl(0, 16000, -16000, 0) == 0, "Deadzone boundary.");
        Check(ControllerInput.FromSdl(3, 16001, -16001, 16001) == (LauncherButtons.Confirm | LauncherButtons.Back | LauncherButtons.Right | LauncherButtons.Up | LauncherButtons.ScrollDown), "SDL positional controls/axes.");
        var stick = ControllerInput.SampleFromSdl(7, 0, 16001, 0, 0);
        Check(stick.Id == 7 && stick.Buttons == LauncherButtons.Right && stick.CoarseHorizontal, "Left-stick source was not retained.");
        Check(ControllerInput.SampleFromSdl(7, 0, short.MinValue, 0, 0).CoarseHorizontal, "Negative full stick lost coarse adjustment.");
        foreach (short axis in new short[] { -16000, 0, 16000 })
            Check(!ControllerInput.SampleFromSdl(7, 0, axis, 0, 0).CoarseHorizontal, "Neutral/deadzone axis enabled coarse adjustment.");
        Check(!ControllerInput.SampleFromSdl(7, 1u << 14, 32767, 0, 0).CoarseHorizontal, "D-pad precision lost priority over the stick.");
        foreach (var kind in Enum.GetValues<Gamepads.Kind>())
        {
            string? expected = kind == Gamepads.Kind.Xbox ? "xbox_original" : kind == Gamepads.Kind.GameCube ? "gamecube" : null;
            Check(ControllerLayout.Match(new[] { new Gamepads.Pad("synthetic", kind) }) == expected, "Reliable scheme matching: " + kind);
        }
        Check(ControllerLayout.Match(Array.Empty<Gamepads.Pad>()) == null, "No-device matching.");
        Check(Gamepads.HintAccepted(true, null, "0") && Gamepads.HintAccepted(false, "0", "0") &&
              !Gamepads.HintAccepted(false, "1", "0") && !Gamepads.HintAccepted(false, null, "1"),
              "An SDL hint already set to the wanted value was treated as a failure, or a conflicting one as fine.");
        Check(ControllerLayout.Match(new[] { new Gamepads.Pad("Xbox", Gamepads.Kind.Xbox), new Gamepads.Pad("GC", Gamepads.Kind.GameCube) }) == null, "Conflicting scheme matching.");
        var c = new GameConfig(path);
        Check(ControllerLayout.ApplyDetected(c, new[] { new Gamepads.Pad("Xbox One Controller", Gamepads.Kind.Xbox) }), "Xbox One did not match existing Original Xbox scheme.");
        Check(c.Get("controller", "a") == "east@0.75,south", "Automatic scheme rewrote custom bindings.");
        c.SetString("controller", "layout", "gamecube");
        Check(ControllerLayout.ApplyDetected(c, new[] { new Gamepads.Pad("Xbox", Gamepads.Kind.Xbox) }) && !ControllerLayout.Automatic(c), "Older F10 changed value was overwritten.");
        c = new GameConfig(path);
        ControllerLayout.SetManual(c, "gamecube");
        Check(!ControllerLayout.Automatic(c) && !ControllerLayout.ApplyDetected(c, new[] { new Gamepads.Pad("Xbox", Gamepads.Kind.Xbox) }), "Manual choice was overridden.");
        Check(ControllerLayout.NeedsConsent(new GameConfig(path)) == false, "Explicit automatic marker required migration.");
        c.Remove("launcher", "auto_controller_layout");
        Check(ControllerLayout.NeedsConsent(c), "Existing layout did not require opt-in.");
        ControllerLayout.SetAutomatic(c, true);
        string original = c.Get("controller", "layout")!;
        Check(!ControllerLayout.ApplyDetected(c, new[] { new Gamepads.Pad("PS5", Gamepads.Kind.PlayStation) }) && c.Get("controller", "layout") == original, "Unsupported type changed an automatic layout.");
    }

    static void Run(DataPaths paths, string config)
    {
        var w = new MainWindow(null, paths, false);
        Fault.Owner = w;
        w.Show(); Pump();
        Finish(w.RefreshGamepads());
        var pads = (Gamepads)typeof(MainWindow).GetField("gamepads", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(w)!;
        Check(!pads.Polling && pads.Samples.Count == 0, "Service-disabled fixture started SDL.");
        var peer = ControlAutomationPeer.CreatePeerForElement(w.scene)!;
        Check(peer.GetAutomationControlType() == AutomationControlType.Menu, "Painted menu lacks accessible menu role.");
        Check(peer.GetChildren()!.Count == 5 && peer.GetChildren()!.All(p => p is IInvokeProvider), "Painted signs lack accessible invoke peers.");
        string first = w.scene.FocusedSign;
        Press(w, LauncherButtons.ScrollDown);
        Check(w.scene.FocusedSign == first, "Right-stick scrolling unexpectedly navigated the main menu.");
        // Use a fresh router's latch in the foreground test below.
        w.Close(); Pump();
        w = new MainWindow(null, paths, false); w.Show(); Pump();
        Fault.Owner = w;
        first = w.scene.FocusedSign;
        Feed(w, LauncherButtons.Next, active: false);
        Check(w.scene.FocusedSign == first && !w.navigation.Used, "Background controller activated launcher.");
        Feed(w, LauncherButtons.Next);
        Check(w.scene.FocusedSign == first, "Foreground return did not require neutral.");
        Press(w, LauncherButtons.Next);
        Check(w.scene.FocusedSign != first, "Menu movement failed.");
        Shot(w, "Main-focus");
        Press(w, LauncherButtons.Confirm);
        var settings = w.boards["settings"];
        Check(settings.IsVisible && w.OpenMenu == settings, "Controller did not open Settings.");
        var rows = settings.Rows.ToList();
        Check(settings.SelectedTab == 0 && settings.Selected == rows[0] && rows[0].IsFocused, "Settings did not open on Display's first row.");
        Shot(w, "Display-focus");
        // Up / down pick a row and stop at the ends (as F10's).
        Press(w, LauncherButtons.Up);
        Check(settings.Selected == rows[0], "Up on the first row left it.");
        Press(w, LauncherButtons.Down);
        Check(settings.Selected == rows[1] && rows[1].IsFocused, "Down did not select the next row.");
        for (int i = 0; i < rows.Count + 2; i++) Press(w, LauncherButtons.Down);
        Check(settings.Selected == rows[^1] && InView(settings, rows[^1]), "Down did not stop on the last row, in view.");
        // A short choice: left / right step through its values and wrap; A steps it too. Each change is saved.
        var shortChoice = rows.OfType<ChoiceRow>().First(r => r.ItemCount is >= 2 and <= 6 && r.IsEffectivelyEnabled);
        settings.Select(shortChoice); Pump();
        int start = shortChoice.SelectedIndex, count = shortChoice.ItemCount;
        Press(w, LauncherButtons.Left);
        Check(shortChoice.SelectedIndex == (start - 1 + count) % count, $"Left did not step {shortChoice.Label} back (wrapping).");
        Press(w, LauncherButtons.Right);
        Check(shortChoice.SelectedIndex == start, $"Right did not step {shortChoice.Label} forward.");
        Press(w, LauncherButtons.Confirm);
        Check(shortChoice.SelectedIndex == (start + 1) % count, $"A did not step {shortChoice.Label}.");
        Press(w, LauncherButtons.Left);
        Check(shortChoice.SelectedIndex == start, $"{shortChoice.Label} did not go back to its value.");
        // A long choice: A opens the whole list, which shows every option when they fit (it once showed 2 of 3, 7 of 12);
        // Back leaves it as it was, and picking the value it has counts as a choice.
        var longChoices = rows.OfType<ChoiceRow>().Where(r => r.ItemCount > 6).ToList();
        Check(longChoices.Count > 0, "No choice with more than six values on the Display page.");
        foreach (var longChoice in longChoices)
        {
            settings.Select(longChoice); Pump();
            int was = longChoice.SelectedIndex, commits = 0;
            EventHandler committed = (_, _) => commits++;
            longChoice.SelectionCommitted += committed;
            Press(w, LauncherButtons.Confirm);
            var listPopup = w.navigation.Modal!;
            Check(listPopup != null && listPopup.Title == longChoice.Label, $"A on {longChoice.Label} did not open its list.");
            var (visible, scrolls, _) = PopupRows(listPopup!);
            Check(visible == longChoice.ItemCount && !scrolls, $"A list of {longChoice.ItemCount} options shows {visible} rows (scrolls: {scrolls}).");
            Shot(listPopup!, $"Choice-list-{longChoice.ItemCount}");
            Press(w, LauncherButtons.Down);
            Press(w, LauncherButtons.Back);
            Check(w.navigation.Modal == null && longChoice.SelectedIndex == was && commits == 0, $"Backing out of {longChoice.Label}'s list changed it.");
            Press(w, LauncherButtons.Confirm);
            Press(w, LauncherButtons.Confirm);
            Check(w.navigation.Modal == null && longChoice.SelectedIndex == was && commits == 1, $"Picking {longChoice.Label}'s own value was not a choice.");
            Check(settings.Selected == longChoice && longChoice.IsFocused, $"Closing {longChoice.Label}'s list lost the selection.");
            longChoice.SelectionCommitted -= committed;
        }
        // A row is told apart by who pressed it: the pad (a picker is the themed browser) or the mouse.
        var origins = new List<bool>();
        var probe = new ActionRow("Synthetic origin", () => origins.Add(w.navigation.Activating));
        settings.Page.Children.Add(probe); Pump();
        settings.Select(probe); Pump();
        Press(w, LauncherButtons.Confirm);
        probe.PerformClick();  // what a mouse click runs
        settings.Page.Children.Remove(probe); settings.SelectFirst(); Pump();
        Check(origins.SequenceEqual(new[] { true, false }) && w.navigation.Used && !w.navigation.Activating,
              "Pad-started and mouse-started actions are not told apart.");
        // The bumpers switch pages: one per press, no wrap, the new page's first row selected.
        Press(w, LauncherButtons.Next);
        Check(settings.SelectedTab == 1 && settings.Selected == settings.Rows.First() && !w.layoutBox.IsAttachedToVisualTree(), "RB did not switch to Graphics.");
        Press(w, LauncherButtons.Next);
        Check(settings.SelectedTab == 2 && w.layoutBox.IsAttachedToVisualTree() && settings.Selected == settings.Rows.First(), "RB did not switch to Controls.");
        Press(w, LauncherButtons.Next);
        Check(settings.SelectedTab == 2, "RB on the last page wrapped or left it.");
        Press(w, LauncherButtons.Previous); Press(w, LauncherButtons.Previous);
        Check(settings.SelectedTab == 0, "LB did not go back to Display.");
        Press(w, LauncherButtons.Previous);
        Check(settings.SelectedTab == 0, "LB on the first page wrapped or left it.");
        // Extras has pages too: Discs, Textures, Game, Folders, About.
        Press(w, LauncherButtons.Back);
        Check(!settings.IsVisible && w.OpenMenu == null, "Back did not close Settings.");
        w.ShowBoard("extras"); Pump();
        var extras = w.boards["extras"];
        Check(extras.SelectedTab == 0 && extras.Selected != null, "Extras did not open on Discs with a row selected.");
        foreach (int tab in new[] { 1, 2, 3, 4, 4 })
        {
            Press(w, LauncherButtons.Next);
            Check(extras.SelectedTab == tab && extras.Selected != null && extras.Selected.IsFocused, $"RB in Extras did not reach page {tab} (or wrapped).");
        }
        Press(w, LauncherButtons.Previous);
        Check(extras.SelectedTab == 3, "LB in Extras did not go back to Folders.");
        Shot(w, "Extras-focus");
        extras.SelectTab(0);
        Press(w, LauncherButtons.Back);
        Check(!extras.IsVisible, "Back did not close Extras.");

        // ----- Controls: the layout is a choice, Match my controller a switch -----
        w.ShowBoard("settings"); w.showControlsTab!(true); Pump();
        Shot(w, "Controls-focus");
        Check(settings.Selected == w.layoutBox, "Controls did not open on Layout.");
        string before = File.ReadAllText(config);
        Press(w, LauncherButtons.Down); Press(w, LauncherButtons.Up);
        Check(File.ReadAllText(config) == before, "Moving over the Layout row saved something.");
        Press(w, LauncherButtons.Right);
        var c = new GameConfig(config);
        Check(c.Get("controller", "layout") == "xbox_original" && !ControllerLayout.Automatic(c), "Explicit controller choice did not persist manual scheme.");
        Check(c.Get("controller", "a") == "east@0.75,south" && c.Get("video", "hdr_paper_white_nits") == "237.5", "Choice altered unrelated settings/bindings.");
        Check(w.autoLayout.IsChecked == false, "Choosing a layout left Match my controller on.");
        Press(w, LauncherButtons.Down);
        Check(settings.Selected == w.autoLayout, "Down did not reach Match my controller.");
        Press(w, LauncherButtons.Confirm);
        Check(w.autoLayout.IsChecked == true && ControllerLayout.Automatic(new GameConfig(config)), "A did not switch Match my controller on.");
        Press(w, LauncherButtons.Right);
        Check(w.autoLayout.IsChecked == true, "Right switched Match my controller off.");
        Press(w, LauncherButtons.Left);
        Check(w.autoLayout.IsChecked == false && !ControllerLayout.Automatic(new GameConfig(config)), "Left did not switch Match my controller off.");
        Press(w, LauncherButtons.Left);
        Check(w.autoLayout.IsChecked == false, "Left switched Match my controller back on.");
        w.autoLayout.IsChecked = true; Pump();
        Press(w, LauncherButtons.Up); Press(w, LauncherButtons.Left);
        c = new GameConfig(config);
        Check(c.Get("controller", "layout") == "gamecube" && !ControllerLayout.Automatic(c), "Left on Layout did not choose GameCube and stop automatic matching.");

        // The right stick scrolls the page; the selection stays on a row in view.
        var scroller = settings.Scroller;
        double old = scroller.Offset.Y;
        Press(w, LauncherButtons.ScrollDown);
        Check(scroller.Offset.Y > old, "Right-stick scrolling failed.");
        Check(settings.Selected != null && InView(settings, settings.Selected), "Scrolling left the selection out of view.");
        double offset = scroller.Offset.Y;
        Press(w, LauncherButtons.Back); w.ShowBoard("settings"); Pump();
        Check(Math.Abs(scroller.Offset.Y - offset) < 1, "Reopening a menu reset its scroll offset.");
        Check(settings.Selected != null && InView(settings, settings.Selected), "Reopening a scrolled menu selected a row out of view.");

        // ----- Mods: the pad reaches each mod's switch and the three actions; switching keeps the selection on the same
        // mod after the list is rebuilt; Add a mod goes through the themed browser (zip files and folders only) -----
        w.ShowBoard("mods"); Pump();
        var modsBoard = w.boards["mods"];
        Shot(w, "Mods-focus");
        foreach (var name in new[] { "ModA", "ModB", "ModC" }) Directory.CreateDirectory(Path.Combine(paths.Mods, name, "files"));
        var refresh = modsBoard.Rows.OfType<ActionRow>().Single(r => r.Label == "Refresh");
        modsBoard.Select(refresh); Pump();
        Press(w, LauncherButtons.Confirm);
        Wait(() => modsBoard.Rows.OfType<ToggleRow>().Count() >= 3);
        Check(modsBoard.Rows.OfType<ToggleRow>().Select(r => r.Label).SequenceEqual(new[] { "ModA", "ModB", "ModC" }), "The mods' switches are not listed in order.");
        foreach (var label in new[] { "Add a mod (zip)", "Open the mods folder", "Refresh" })
            Check(modsBoard.Rows.OfType<ActionRow>().Any(b => b.Label == label), "Pad cannot reach the Mods action " + label + ".");
        ToggleRow Mod(string name) => modsBoard.Rows.OfType<ToggleRow>().Single(r => r.Label == name);
        modsBoard.Select(Mod("ModB")); Pump();
        Press(w, LauncherButtons.Confirm);
        Wait(() => File.Exists(Path.Combine(paths.Mods, "ModB", "disabled")));
        Check(File.Exists(Path.Combine(paths.Mods, "ModB", "disabled")), "A did not switch the mod off.");
        Wait(() => modsBoard.Selected is { } s && s.IsAttachedToVisualTree() && s.Label == "ModB");  // the rebuild after the switch
        Check(modsBoard.Selected is ToggleRow { Label: "ModB", IsChecked: false } sel && sel.IsAttachedToVisualTree() && sel.IsFocused,
              "Switching a mod lost the selection (list rebuilt under it).");
        Press(w, LauncherButtons.Down);
        Check(modsBoard.Selected?.Label == "ModC", "Down did not move to the next mod after a switch.");
        Press(w, LauncherButtons.Left);
        Wait(() => File.Exists(Path.Combine(paths.Mods, "ModC", "disabled")) && modsBoard.Selected?.IsAttachedToVisualTree() == true);
        Check(File.Exists(Path.Combine(paths.Mods, "ModC", "disabled")) && modsBoard.Selected?.Label == "ModC", "Left did not switch a mod off in place.");
        Press(w, LauncherButtons.Up);
        Check(modsBoard.Selected?.Label == "ModB", "Up after a switch did not go back to the previous mod.");
        // A mouse click on a mod's switch: it switches, and the selection follows to that mod.
        var clicked = Mod("ModA");
        // The middle of its value (On / Off between the arrows) flips it.
        double valueMiddle = clicked.Bounds.Width - 14 - Math.Clamp(clicked.Bounds.Width * 0.46, 170, 360) / 2;
        var at = clicked.TranslatePoint(new Point(valueMiddle, clicked.Bounds.Height / 2), w)!.Value;
        w.MouseMove(at); w.MouseDown(at, MouseButton.Left); w.MouseUp(at, MouseButton.Left);
        Wait(() => File.Exists(Path.Combine(paths.Mods, "ModA", "disabled")) && modsBoard.Selected?.IsAttachedToVisualTree() == true);
        Check(File.Exists(Path.Combine(paths.Mods, "ModA", "disabled")) && modsBoard.Selected?.Label == "ModA", "A mouse click on a mod didn't switch it and select it.");
        string zips = Path.Combine(paths.Root, "zips");
        Directory.CreateDirectory(Path.Combine(zips, "Sub"));
        File.WriteAllText(Path.Combine(zips, "a.zip"), "x"); File.WriteAllText(Path.Combine(zips, "b.ISO"), "x"); File.WriteAllText(Path.Combine(zips, "c.txt"), "x");
        var zipPick = ControllerDialogs.PickPath(w, "Synthetic zip browser", zips, false, viaController: true, extensions: new[] { ".zip" }, typeName: "Mod zip");
        Pump(); var zipModal = w.navigation.Modal!;
        Check(Entries(zipModal).Count == 2, "Zip browser did not list only .zip files and folders.");
        Press(w, LauncherButtons.Back); Finish(zipPick);
        // The real row: pressed by the pad it opens the themed browser, not the OS picker.
        modsBoard.Select(modsBoard.Rows.OfType<ActionRow>().Single(r => r.Label == "Add a mod (zip)")); Pump();
        Press(w, LauncherButtons.Confirm);
        Check(w.navigation.Modal is { } zipDialog && zipDialog.GetVisualDescendants().OfType<MenuScreen>().Any(), "Add a mod (zip) with the pad did not open the themed browser.");
        Press(w, LauncherButtons.Back); Pump();
        Check(w.navigation.Modal == null && modsBoard.IsVisible, "Backing out of the mod browser left a dialog open or closed Mods.");
        Press(w, LauncherButtons.Back);
        Check(w.OpenMenu == null, "Back did not close Mods.");

        // ----- Extras > Textures: every folder in texture_packs in the game's order with a switch each; A picks a pack up
        // and moves it (the folders are renumbered), B puts it back; X removes a downloaded pack, never the player's own -----
        string packsDir = TexturePacks.Dir(paths.UserData);
        foreach (var f in new[] { "40-menu", "50-scoobyhd", "MyPack" })
        {
            Directory.CreateDirectory(Path.Combine(packsDir, f));
            File.WriteAllText(Path.Combine(packsDir, f, "t.png"), "synthetic");
        }
        string Folders() => string.Join(",", TexturePacks.List(paths.UserData).Select(i => i.Folder));
        var tex = w.boards["extras"];
        tex.SelectTab(1);
        w.ShowBoard("extras"); Pump();
        List<OrderRow> PackRows() => tex.Rows.OfType<OrderRow>().ToList();
        string Labels() => string.Join(",", PackRows().Select(r => r.Label));
        OrderRow? Picked() => tex.Selected as OrderRow;
        Check(Labels() == "Menu pack,Scooby-HD,MyPack", "The packs are not listed in the game's order: " + Labels());
        Check(!tex.Rows.OfType<ActionRow>().Any(r => r.Label is "Menu pack" or "Scooby-HD"), "A downloaded pack is still offered for download.");
        Check(Picked() == PackRows()[0], "Textures did not open on the first pack.");
        Shot(w, "Textures-focus");
        Press(w, LauncherButtons.Left);
        Wait(() => Picked()?.IsAttachedToVisualTree() == true);
        Check(File.Exists(Path.Combine(packsDir, "40-menu", "disabled")) && Picked() is { Label: "Menu pack", IsChecked: false },
              "Left did not switch the pack off (or lost the selection).");
        Press(w, LauncherButtons.Right);
        Wait(() => Picked()?.IsAttachedToVisualTree() == true);
        Check(!File.Exists(Path.Combine(packsDir, "40-menu", "disabled")) && Picked() is { IsChecked: true }, "Right did not switch the pack on.");
        // Picked up, carried down, put back with B: nothing renamed, and nothing else reacts while it is carried.
        Press(w, LauncherButtons.Confirm);
        Check(Picked() is { Carrying: true }, "A did not pick the pack up.");
        Press(w, LauncherButtons.Down);
        Check(Labels() == "Scooby-HD,Menu pack,MyPack" && Picked()?.Label == "Menu pack", "Down did not carry the pack: " + Labels());
        Shot(w, "Textures-carrying");
        Press(w, LauncherButtons.Next);
        Press(w, LauncherButtons.Right);
        Check(tex.SelectedTab == 1 && Picked() is { Carrying: true, IsChecked: true }, "RB or Right acted on a carried pack.");
        Press(w, LauncherButtons.Back);
        Check(Picked() is { Carrying: false } && Labels() == "Menu pack,Scooby-HD,MyPack" && Folders() == "40-menu,50-scoobyhd,MyPack" && w.OpenMenu == tex,
              "B did not put the pack back where it was (or closed the menu): " + Labels() + " / " + Folders());
        // Carried to the bottom and put down: the folders get the new order's numbers, the selection stays on the pack.
        Press(w, LauncherButtons.Confirm); Press(w, LauncherButtons.Down); Press(w, LauncherButtons.Down); Press(w, LauncherButtons.Down);
        Press(w, LauncherButtons.Confirm);
        Wait(() => Folders() == "10-scoobyhd,20-MyPack,30-menu" && Picked()?.IsAttachedToVisualTree() == true);
        Check(Folders() == "10-scoobyhd,20-MyPack,30-menu", "Putting the pack down didn't renumber the folders: " + Folders());
        Check(Labels() == "Scooby-HD,MyPack,Menu pack" && Picked()?.Label == "Menu pack" && Picked()!.IsFocused, "The list or selection after a move is wrong: " + Labels());
        // X: only a downloaded pack offers Remove; No keeps it, Yes deletes it and offers the download again.
        tex.Select(PackRows().Single(r => r.Label == "MyPack")); Pump();
        Check(Picked()!.ExtraLabel == null, "A pack the player added offers Remove.");
        Press(w, LauncherButtons.Extra);
        Check(w.navigation.Modal == null && Directory.Exists(Path.Combine(packsDir, "20-MyPack")), "X on the player's own pack did something.");
        tex.Select(PackRows().Single(r => r.Label == "Scooby-HD")); Pump();
        Press(w, LauncherButtons.Extra);
        Check(w.navigation.Modal != null, "X on a downloaded pack did not ask first.");
        Press(w, LauncherButtons.Back);
        Wait(() => w.navigation.Modal == null);
        Check(Directory.Exists(Path.Combine(packsDir, "10-scoobyhd")), "Answering No removed the pack.");
        Press(w, LauncherButtons.Extra);
        Click(w, w.navigation.Modal!, "Yes");
        Wait(() => !Directory.Exists(Path.Combine(packsDir, "10-scoobyhd")) && tex.Rows.OfType<ActionRow>().Any(r => r.Label == "Scooby-HD"));
        Check(!Directory.Exists(Path.Combine(packsDir, "10-scoobyhd")) && Labels() == "MyPack,Menu pack", "Yes did not remove the pack: " + Labels());
        Check(tex.Rows.OfType<ActionRow>().Any(r => r.Label == "Scooby-HD" && r.Value == "604 MB"), "A removed pack isn't offered for download again.");
        // A folder put there by hand shows up when Extras opens again.
        Press(w, LauncherButtons.Back);
        Directory.CreateDirectory(Path.Combine(packsDir, "Another"));
        tex.SelectTab(1);
        w.ShowBoard("extras"); Pump();
        Check(Labels() == "MyPack,Menu pack,Another", "A pack folder added by hand didn't appear: " + Labels());
        tex.SelectTab(0);
        Press(w, LauncherButtons.Back);
        Directory.Delete(packsDir, true);

        var question = Dialogs.YesNo(w, "Synthetic safe-focus question.");
        var modal = w.navigation.Modal!;
        Check(((PlankButton?)modal.FocusManager?.GetFocusedElement())?.Text == "No", "Question did not initially focus the safe answer.");
        w.navigation.Feed(w, new[] { new ControllerSample(1, LauncherButtons.Back) }, time += 20, false);
        Pump();
        Check(!question.IsCompleted && modal.IsVisible, "Input activated the underlying owner behind its modal.");
        Shot(modal, "Confirmation");
        Feed(w, LauncherButtons.Confirm, game: true);
        Check(!question.IsCompleted, "Game ownership allowed modal activation.");
        Feed(w, LauncherButtons.Confirm);
        Check(!question.IsCompleted, "Game return did not require neutral.");
        Press(w, LauncherButtons.Back); Finish(question);
        Check(!question.Result, "Back accepted a Yes/No question.");

        var generic = Dialogs.Make("Synthetic settings dialog", w);
        var slider = new Slider { Minimum = 0, Maximum = 1, Value = 0.5, SmallChange = 0.25 };
        var number = new NumericUpDown { Minimum = 1, Maximum = 10, Value = 5, Increment = 2 };
        var toggle = new CheckBox { Content = "Synthetic checkbox" };
        var textField = new TextBox { Text = "unchanged", MinWidth = 400 };
        int activated = 0;
        var narrow = new PlankButton("Synthetic action", (_, _) => activated++);
        var left = new PlankButton("Left action", (_, _) => { });
        var right = new PlankButton("Longer right action", (_, _) => { });
        var row = new StackPanel { Orientation = Avalonia.Layout.Orientation.Horizontal, Children = { left, right } };
        var stepped = new ControllerSlider(5) { Minimum = 0, Maximum = 100, Value = 50, SmallChange = 0.25 };
        generic.Content = new StackPanel { Children = { textField, narrow, slider, number, toggle, row, stepped } };
        var shown = Dialogs.ShowModal(generic, w); Pump();
        textField.Focus(NavigationMethod.Directional); Press(w, LauncherButtons.Down);
        Check(narrow.IsFocused, "Down skipped a narrow button below a stretched text field.");
        Press(w, LauncherButtons.Confirm);
        Check(activated == 1, "Directional button selection did not activate exactly once.");
        Press(w, LauncherButtons.Up);
        Check(textField.IsFocused, "Up did not return from a narrow button to its text field.");
        Press(w, LauncherButtons.Confirm);
        Check(w.navigation.Modal != generic && Find<ControllerTextBox>(w.navigation.Modal!).Text == "unchanged",
              "Directionally selected text field did not open controller text entry.");
        Press(w, LauncherButtons.Back);
        Check(w.navigation.Modal == generic && textField.IsFocused && textField.Text == "unchanged",
              "Cancelling text entry did not restore the unchanged field.");
        Press(w, LauncherButtons.Down); Press(w, LauncherButtons.Down);
        Check(slider.IsFocused, "Down did not reach the slider after a narrow button.");
        slider.Focus(NavigationMethod.Directional); Press(w, LauncherButtons.Right);
        Check(slider.Value == 0.75, "Slider adjustment failed.");
        w.navigation.Feed(generic, new[] { new ControllerSample(1, LauncherButtons.None) }, time += 20, false);
        w.navigation.Feed(generic, new[] { new ControllerSample(1, LauncherButtons.Left, true) }, time += 20, false);
        Pump();
        Check(slider.Value == 0.5, "Coarse stick steps changed an ordinary slider.");
        number.Focus(NavigationMethod.Directional); Press(w, LauncherButtons.Left);
        Check(number.Value == 3, "Numeric adjustment failed.");
        number.Minimum = decimal.MinValue; number.Maximum = decimal.MaxValue; number.Value = decimal.MaxValue; number.Increment = 2;
        Press(w, LauncherButtons.Right);
        Check(number.Value == decimal.MaxValue, "Numeric maximum overflowed instead of clamping.");
        number.Value = decimal.MinValue; Press(w, LauncherButtons.Left);
        Check(number.Value == decimal.MinValue, "Numeric minimum overflowed instead of clamping.");
        toggle.Focus(NavigationMethod.Directional); Press(w, LauncherButtons.Confirm);
        Check(toggle.IsChecked == true, "Checkbox activation failed.");
        Feed(w, LauncherButtons.Confirm, id: 2);
        Check(toggle.IsChecked == true, "Held hotplug button activated a control.");
        Feed(w, LauncherButtons.None, id: 2); Feed(w, LauncherButtons.Confirm, id: 2);
        Check(toggle.IsChecked == false, "Reconnected neutral device did not arm.");
        left.Focus(NavigationMethod.Directional); Press(w, LauncherButtons.Right);
        Check(right.IsFocused, $"Horizontal movement did not reach the adjacent button; focused {generic.FocusManager?.GetFocusedElement()}.");
        Press(w, LauncherButtons.Left);
        Check(left.IsFocused, "Horizontal movement did not return to the adjacent button.");
        void Samples(int elapsed, params ControllerSample[] samples)
        {
            w.navigation.Feed(generic, samples, time += elapsed, false);
            Pump();
        }
        void Neutral() => Samples(20, new(1, LauncherButtons.None), new(2, LauncherButtons.None));
        stepped.Focus(NavigationMethod.Directional); Neutral();
        Samples(20, new ControllerSample(1, LauncherButtons.Right, true));
        Check(stepped.Value == 55, "Opt-in slider did not use its coarse step.");
        Samples(399, new ControllerSample(1, LauncherButtons.Right, true));
        Check(stepped.Value == 55, "Coarse slider repeated before 400ms.");
        Samples(1, new ControllerSample(1, LauncherButtons.Right, true));
        Check(stepped.Value == 60, "Coarse slider did not repeat at 400ms.");
        Samples(99, new ControllerSample(1, LauncherButtons.Right, true));
        Check(stepped.Value == 60, "Coarse slider repeated before the next 100ms.");
        Samples(1, new ControllerSample(1, LauncherButtons.Right, true));
        Check(stepped.Value == 65, "Coarse slider did not retain its step during repeat.");
        Neutral(); Samples(20, new ControllerSample(1, LauncherButtons.Left));
        Check(stepped.Value == 64.75, "D-pad did not retain the slider's precise increment.");
        Neutral(); Samples(20, new ControllerSample(1, LauncherButtons.Right), new ControllerSample(2, LauncherButtons.Right, true));
        Check(stepped.Value == 65, "Another controller's coarse metadata leaked into the winning precise action.");
        Neutral(); Samples(20, new ControllerSample(1, LauncherButtons.None, true), new ControllerSample(2, LauncherButtons.Left));
        Check(stepped.Value == 64.75, "An idle controller's coarse metadata leaked into another controller's action.");
        stepped.Value = 99; Neutral(); Samples(20, new ControllerSample(1, LauncherButtons.Right, true));
        Check(stepped.Value == 100, "Coarse slider exceeded its upper bound.");
        stepped.Value = 1; Neutral(); Samples(20, new ControllerSample(1, LauncherButtons.Left, true));
        Check(stepped.Value == 0, "Coarse slider exceeded its lower bound.");
        w.navigation.Feed(generic, new[] { new ControllerSample(1, LauncherButtons.Right, true) }, time += 20, true); Pump();
        Check(stepped.Value == 0, "Coarse metadata bypassed game ownership.");
        Samples(20, new ControllerSample(1, LauncherButtons.Right, true));
        Check(stepped.Value == 0, "Coarse input did not require neutral after game ownership.");
        Press(w, LauncherButtons.Back); Finish(shown);

        var text = ControllerDialogs.Text(w, "Synthetic text entry", "A");
        modal = w.navigation.Modal!; Pump();
        Click(w, modal, "1");
        Check(Find<ControllerTextBox>(modal).Text == "A1", "On-screen text did not insert at the caret.");
        Shot(modal, "Text-entry");
        Click(w, modal, "Apply"); Finish(text);
        Check(text.Result == "A1", "Text entry did not apply.");
        text = ControllerDialogs.Text(w, "Synthetic cancelled text", "keep"); Pump();
        Press(w, LauncherButtons.Back); Finish(text);
        Check(text.Result == null, "Cancelled text returned a changed value.");
        text = ControllerDialogs.Text(w, "Synthetic Unicode preservation", "A\U0001f600"); Pump();
        modal = w.navigation.Modal!;
        Click(w, modal, "Backspace"); Click(w, modal, "Apply"); Finish(text);
        Check(text.Result == "A", "Backspace split an existing Unicode character.");

        string browser = Path.Combine(paths.Root, "browser");
        Directory.CreateDirectory(Path.Combine(browser, "Child"));
        File.WriteAllText(Path.Combine(browser, "disc.ISO"), "synthetic non-game bytes");
        File.WriteAllText(Path.Combine(browser, "notes.txt"), "synthetic");
        // The browser is a full-window menu: a row per folder and file, A opens or chooses, X goes up a folder, B cancels.
        var file = ControllerDialogs.PickPath(w, "Synthetic file browser", browser, false, viaController: true);
        modal = w.navigation.Modal!; Pump();
        Check(string.Join(",", Entries(modal).Select(r => r.Label)) == "Child,disc.ISO", "Disc filter did not include folders/uppercase ISO and exclude other files.");
        Check(Screen(modal).Selected?.Label == "Child", "The browser did not open on the first folder.");
        Check(Math.Abs(modal.Bounds.Width - w.Bounds.Width) < 1 && Math.Abs(modal.Bounds.Height - w.Bounds.Height) < 1, "The browser does not fill the launcher's window.");
        Shot(modal, "File-browser");
        Press(w, LauncherButtons.Extra);
        Check(Screen(modal).Selected?.Label == "browser" && Entries(modal).Any(r => r.Label == "browser"), "X did not go up a folder onto the one it came from.");
        Press(w, LauncherButtons.Confirm);
        Check(Screen(modal).Selected?.Label == "Child", "A on a folder did not open it.");
        Choose(w, modal, "Show all files");
        Check(string.Join(",", Entries(modal).Select(r => r.Label)) == "Child,disc.ISO,notes.txt" && Screen(modal).Selected?.Label == "Show all files",
              "Show all files did not list every file (or lost the selection).");
        Press(w, LauncherButtons.Left);
        Check(Entries(modal).Count == 2, "Switching Show all files off did not filter again.");
        Choose(w, modal, "Drives and folders");
        Check(Entries(modal).Any(r => r.Label == "Home") && Entries(modal).Any(r => Path.GetPathRoot((string)r.Tag!) == (string)r.Tag!),
              "Drives and folders did not list the home folder and a drive.");
        Shot(modal, "File-browser-places");
        Press(w, LauncherButtons.Extra);
        Check(w.navigation.Modal == modal && Screen(modal).Selected?.Label == "Home", "X on the drives list did something.");
        Choose(w, modal, "Type a path");
        var typing = w.navigation.Modal!;
        Check(typing != modal, "Type a path did not open the keyboard.");
        typing.GetVisualDescendants().OfType<ControllerTextBox>().First().Text = browser;
        Click(w, typing, "Apply");
        Wait(() => w.navigation.Modal == modal);
        Check(Screen(modal).Selected?.Label == "Child", "A typed path did not open that folder.");
        Choose(w, modal, "disc.ISO"); Finish(file);
        Check(file.Result == Path.Combine(browser, "disc.ISO"), "Managed file browser returned wrong path.");
        var folder = ControllerDialogs.PickPath(w, "Synthetic folder browser", browser, true, viaController: true);
        modal = w.navigation.Modal!; Pump();
        Press(w, LauncherButtons.Confirm);
        Choose(w, modal, "Use this folder"); Finish(folder);
        Check(folder.Result == Path.Combine(browser, "Child"), "Managed folder browser did not descend/select.");
        file = ControllerDialogs.PickPath(w, "Synthetic cancelled browser", browser, false, viaController: true);
        Pump(); Press(w, LauncherButtons.Back); Finish(file);
        Check(file.Result == null && File.ReadAllText(config).Contains("unknown = 42"), "Cancelled browser changed settings.");
        folder = ControllerDialogs.PickPath(w, "Synthetic folder creation", browser, true, viaController: true); Pump();
        modal = w.navigation.Modal!;
        Choose(w, modal, "New folder");
        var keyboard = w.navigation.Modal!;
        Click(w, keyboard, "1"); Click(w, keyboard, "Apply");
        Wait(() => w.navigation.Modal == modal && Screen(modal).Selected?.Label == "1");
        Check(Directory.Exists(Path.Combine(browser, "1")) && Screen(modal).Selected?.Label == "1", "Controller folder creation failed (or didn't select it).");
        Press(w, LauncherButtons.Back); Finish(folder);
        Check(folder.Result == null, "Cancelling the folder picker applied its path.");
        file = ControllerDialogs.PickPath(w, "Synthetic unreadable file", browser, false, viaController: true); Pump();
        modal = w.navigation.Modal!;
        Screen(modal).Select(Entries(modal).Last()); Pump();
        File.Delete(Path.Combine(browser, "disc.ISO"));
        Press(w, LauncherButtons.Confirm);
        Check(w.navigation.Modal != modal && w.navigation.Modal != null, "Deleted selected file was not reported explicitly.");
        Press(w, LauncherButtons.Back); Press(w, LauncherButtons.Back); Finish(file);
        Check(file.Result == null, "An unreadable file was returned as success.");

        c = new GameConfig(config);
        c.Remove("launcher", "auto_controller_layout"); c.Remove("launcher", "auto_controller_layout_value"); c.Save();
        before = File.ReadAllText(config);
        var migration = w.ApplyControllerLayout(new[] { new Gamepads.Pad("Xbox One Controller", Gamepads.Kind.Xbox) });
        modal = w.navigation.Modal!; Pump();
        Check(((PlankButton?)modal.FocusManager?.GetFocusedElement())?.Text == "Keep my layout", "Migration prompt did not default safely.");
        Press(w, LauncherButtons.Back); Finish(migration);
        Check(File.ReadAllText(config) == before, "Cancelled migration changed persistence.");
        migration = w.ApplyControllerLayout(new[] { new Gamepads.Pad("Xbox", Gamepads.Kind.Xbox) }); Finish(migration);
        Check(w.navigation.Modal == null && File.ReadAllText(config) == before, "Cancelled migration was immediately re-asked.");
        w.Close(); Pump();
        w = new MainWindow(null, paths, false); w.Show(); Pump();
        migration = w.ApplyControllerLayout(new[] { new Gamepads.Pad("Xbox", Gamepads.Kind.Xbox) }); Pump();
        Check(w.navigation.Modal != null, "Legacy migration was not deferred until next startup.");
        Click(w, w.navigation.Modal!, "Keep my layout"); Finish(migration);
        Check(!ControllerLayout.Automatic(new GameConfig(config)), "Keep-layout intent was not persisted.");
        w.Close(); Pump();
        c = new GameConfig(config); c.Remove("launcher", "auto_controller_layout"); c.SetString("controller", "layout", "gamecube"); c.Save();
        w = new MainWindow(null, paths, false); w.Show(); Pump(); Fault.Owner = w;
        migration = w.ApplyControllerLayout(new[] { new Gamepads.Pad("Xbox One Controller", Gamepads.Kind.Xbox) }); Pump();
        Click(w, w.navigation.Modal!, "Enable automatic matching"); Finish(migration);
        c = new GameConfig(config);
        Check(ControllerLayout.Automatic(c) && c.Get("controller", "layout") == "xbox_original" &&
              c.Get("launcher", "auto_controller_layout_value") == "xbox_original", "Legacy opt-in did not persist automatic matching.");
        Finish(w.ApplyControllerLayout(new[] { new Gamepads.Pad("Unknown", Gamepads.Kind.Other) }));
        Check(new GameConfig(config).Get("controller", "layout") == "xbox_original", "Unsupported hotplug overwrote an automatic scheme.");
        Finish(w.ApplyControllerLayout(new[] { new Gamepads.Pad("Adapter", Gamepads.Kind.GameCube) }));
        Check(new GameConfig(config).Get("controller", "layout") == "gamecube" &&
              new GameConfig(config).Get("controller", "a") == "east@0.75,south", "Supported hotplug failed or remapped bindings.");
        w.Close(); Pump();
        c = new GameConfig(config); ControllerLayout.SetManual(c, "unrecognized-layout"); c.Save();
        before = File.ReadAllText(config);
        w = new MainWindow(null, paths, false); w.Show(); Pump(); Fault.Owner = w;
        Check(w.layoutBox.SelectedIndex == 2 && w.layoutBox.SelectedItem?.ToString() == "(custom) unrecognized-layout", "Unknown layout was displayed as a different supported scheme.");
        Finish(w.ApplyControllerLayout(new[] { new Gamepads.Pad("Xbox", Gamepads.Kind.Xbox) }));
        Check(File.ReadAllText(config) == before, "Unknown deliberate scheme was overwritten.");
        w.ShowBoard("settings"); w.showControlsTab!(true); Pump();
        Check(w.boards["settings"].Selected == w.layoutBox, "Controls did not open on Layout.");
        Press(w, LauncherButtons.Down); Press(w, LauncherButtons.Up);
        Check(File.ReadAllText(config) == before, "Moving over a custom layout rewrote the configuration.");
        w.CloseBoards(); Pump();
        w.Close(); Pump(); Fault.Owner = null;

        // The launcher comes back after the game exits, a clean quit (exit code 0) included.
        w = new MainWindow(null, paths, false); w.Show(); Pump(); Fault.Owner = w;
        w.WindowState = WindowState.Minimized; Pump();
        Check(w.WindowState == WindowState.Minimized, "Headless window could not be minimized for the exit check.");
        var standIn = OperatingSystem.IsWindows() ? new System.Diagnostics.ProcessStartInfo("cmd.exe", "/c exit 0")
                                                  : new System.Diagnostics.ProcessStartInfo("/bin/sh", "-c \"exit 0\"");
        standIn.UseShellExecute = false;
        standIn.CreateNoWindow = true;
        using (var quick = System.Diagnostics.Process.Start(standIn)!)
        {
            quick.WaitForExit();
            Check(quick.ExitCode == 0, "Stand-in game process did not exit with 0.");
            var exited = typeof(MainWindow).GetMethod("GameExited", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!;
            Finish((Task)exited.Invoke(w, new object[] { quick })!);
        }
        Check(w.WindowState == WindowState.Normal, "Launcher stayed minimized after a clean game exit.");
        w.Close(); Pump(); Fault.Owner = null;
    }
}
