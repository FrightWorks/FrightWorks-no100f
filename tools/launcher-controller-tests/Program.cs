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
    static PlankButton Button(Window w, string name) => w.GetVisualDescendants().OfType<PlankButton>().First(b => b.Text == name);
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
            var adjust = Button(w, "Adjust HDR");
            Check(ControllerNavigation.Targets(w.boards["settings"]).Contains(adjust), "Adjust HDR is not a controller target.");
            adjust.Focus(NavigationMethod.Directional);
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
            Check(adjust.IsFocused, "Closing the HDR window did not return focus to Adjust HDR.");
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
        Check(w.boards["settings"].IsVisible, "Controller did not open Settings.");
        var targets = ControllerNavigation.Targets(w.boards["settings"]).ToList();
        Check(targets.Any(t => t is ThemedCombo) && targets.OfType<PlankButton>().Any(b => b.Text == "Controls"), "Display navigation surfaces missing.");
        Shot(w, "Display-focus");
        // A popup shows every option when they fit and scrolls only beyond its cap (it once showed 2 of 3, 7 of 12).
        var combos = targets.OfType<ThemedCombo>().Where(c => c.ItemCount >= 3).OrderBy(c => c.ItemCount).ToList();
        Check(combos.Count > 0, "No dropdown with three or more options on the Display page.");
        foreach (var combo in new[] { combos.First(), combos.Last() }.Distinct())
        {
            combo.Focus(NavigationMethod.Directional); Press(w, LauncherButtons.Confirm); Pump();
            var manyPopup = w.navigation.Modal!;
            var (visible, scrolls, realized) = PopupRows(manyPopup);
            Check(visible == combo.ItemCount && !scrolls, $"A popup for {combo.ItemCount} options shows {visible} rows (scrolls: {scrolls}).");
            Shot(manyPopup, $"Dropdown-popup-{combo.ItemCount}");
            Press(w, LauncherButtons.Back);
        }
        Click(w, w, "Controls");
        Check(w.layoutBox.IsEffectivelyVisible, "Controls tab failed.");
        // A picker is the themed browser only when the pad started that action, never because the pad was used earlier.
        var controlsTab = Button(w, "Controls");
        var origins = new List<bool>();
        EventHandler origin = (_, _) => origins.Add(w.navigation.Activating);
        controlsTab.Click += origin;
        Click(w, w, "Controls");
        controlsTab.PerformClick();  // what a mouse click runs
        controlsTab.Click -= origin;
        Check(origins.SequenceEqual(new[] { true, false }) && w.navigation.Used && !w.navigation.Activating,
              "Pad-started and mouse-started actions are not told apart.");
        // The bumpers switch tabs where a page has them: one per press, no wrap, focus on the new tab's button.
        Press(w, LauncherButtons.Previous);
        Check(Button(w, "Display").Selected && !w.layoutBox.IsEffectivelyVisible && Button(w, "Display").IsFocused, "LB did not switch to the Display tab.");
        Press(w, LauncherButtons.Previous);
        Check(Button(w, "Display").Selected && !Button(w, "Controls").Selected, "LB on the first tab wrapped or left it.");
        Press(w, LauncherButtons.Next);
        Check(Button(w, "Controls").Selected && w.layoutBox.IsEffectivelyVisible && Button(w, "Controls").IsFocused, "RB did not switch to the Controls tab.");
        Press(w, LauncherButtons.Next);
        Check(Button(w, "Controls").Selected && !Button(w, "Display").Selected, "RB on the last tab wrapped or left it.");
        Shot(w, "Controls-focus");

        string before = File.ReadAllText(config);
        w.layoutBox.Focus(NavigationMethod.Directional);
        Feed(w, LauncherButtons.None); Feed(w, LauncherButtons.Confirm);
        var choice = w.navigation.Modal!;
        Check(choice != null, "Controller dropdown did not create a registered dialog.");
        Check(choice!.WindowDecorations == WindowDecorations.None, "Dropdown popup kept the operating-system window frame.");
        Check(choice.GetVisualDescendants().OfType<TextBlock>().Any(t => t.Text == "Layout") && choice.Title == "Layout",
              "Dropdown popup does not name its setting.");
        Shot(choice, "Dropdown-popup");
        Check(PopupRows(choice).Visible == 2, "The two-option Layout popup does not show both options.");
        Feed(w, LauncherButtons.Confirm);
        Check(w.navigation.Modal == choice && choice!.IsVisible, "Held confirm accepted the new dialog.");
        Press(w, LauncherButtons.Down);
        Check(File.ReadAllText(config) == before, "Staged dropdown selection saved before confirmation.");
        Press(w, LauncherButtons.Back);
        Check(w.navigation.Modal == null && File.ReadAllText(config) == before, "Cancelled dropdown changed settings.");
        w.layoutBox.Focus(NavigationMethod.Directional);
        Press(w, LauncherButtons.Confirm);
        Press(w, LauncherButtons.Down);
        Press(w, LauncherButtons.Confirm);
        var c = new GameConfig(config);
        Check(c.Get("controller", "layout") == "xbox_original" && !ControllerLayout.Automatic(c), "Explicit controller choice did not persist manual scheme.");
        Check(c.Get("controller", "a") == "east@0.75,south" && c.Get("video", "hdr_paper_white_nits") == "237.5", "Choice altered unrelated settings/bindings.");
        w.autoLayout.IsChecked = true; Pump();
        w.layoutBox.Focus(NavigationMethod.Directional);
        Press(w, LauncherButtons.Confirm); Press(w, LauncherButtons.Confirm);
        Check(!ControllerLayout.Automatic(new GameConfig(config)), "Same-value explicit choice did not stop automatic matching.");
        w.autoLayout.IsChecked = true; Pump();
        w.layoutBox.IsDropDownOpen = true; Pump();
        var item = w.layoutBox.ContainerFromIndex(w.layoutBox.SelectedIndex)!;
        Check(ControlAutomationPeer.CreatePeerForElement(item) is ISelectionItemProvider, "Native layout item lost accessibility selection.");
        ((ISelectionItemProvider)ControlAutomationPeer.CreatePeerForElement(item)!).Select();
        Pump();
        Check(!ControllerLayout.Automatic(new GameConfig(config)), "Same-value native accessibility selection did not stop automatic matching.");
        w.layoutBox.IsDropDownOpen = false; Pump();
        w.autoLayout.IsChecked = true; Pump();
        w.layoutBox.IsDropDownOpen = true; Pump();
        w.layoutBox.RaiseEvent(new KeyEventArgs { RoutedEvent = InputElement.KeyDownEvent, Key = Key.Enter });
        Pump();
        Check(!ControllerLayout.Automatic(new GameConfig(config)), "Same-value native keyboard choice did not stop automatic matching.");
        w.layoutBox.IsDropDownOpen = false; Pump();
        w.autoLayout.IsChecked = true; Pump();
        w.layoutBox.IsDropDownOpen = true; Pump();
        item = w.layoutBox.ContainerFromIndex(w.layoutBox.SelectedIndex)!;
        var popup = TopLevel.GetTopLevel(item)!;
        var center = item.TranslatePoint(new Point(item.Bounds.Width / 2, item.Bounds.Height / 2), popup)!.Value;
        popup.MouseMove(center); popup.MouseDown(center, MouseButton.Left); popup.MouseUp(center, MouseButton.Left); Pump();
        Check(!ControllerLayout.Automatic(new GameConfig(config)), "Same-value native pointer choice did not stop automatic matching.");
        w.layoutBox.IsDropDownOpen = false; Pump();
        w.autoLayout.IsChecked = true; Pump();
        w.layoutBox.IsDropDownOpen = true; Pump();
        w.layoutBox.RaiseEvent(new KeyEventArgs { RoutedEvent = InputElement.KeyDownEvent, Key = Key.Escape });
        Pump();
        Check(ControllerLayout.Automatic(new GameConfig(config)), "Cancelling an unchanged native picker changed manual intent.");
        w.autoLayout.IsChecked = false; Pump();
        w.layoutBox.IsDropDownOpen = true; Pump();
        Press(w, LauncherButtons.Back);
        Check(!w.layoutBox.IsDropDownOpen && w.boards["settings"].IsVisible, "Controller Back closed the board instead of its native dropdown.");

        var scroller = w.boards["settings"].Scroller;
        w.layoutBox.Focus(NavigationMethod.Directional);
        double old = scroller.Offset.Y;
        Press(w, LauncherButtons.ScrollDown);
        Check(scroller.Offset.Y > old, "Right-stick scrolling failed.");
        double offset = scroller.Offset.Y;
        Press(w, LauncherButtons.Back); w.ShowBoard("settings"); Pump();
        Check(Math.Abs(scroller.Offset.Y - offset) < 1, "Reopening a board reset its scroll offset.");
        w.ShowBoard("mods"); Pump();
        Check(ControllerNavigation.Targets(w.boards["mods"]).OfType<PlankButton>().Any(), "Mods controls not navigable.");
        Shot(w, "Mods-focus");
        // Mods page: the pad reaches a mod's checkbox and the three buttons, ticking keeps the pad on the same row after the
        // list is rebuilt, and Add a mod goes through the themed browser (zip files and folders only) when the pad pressed it.
        foreach (var name in new[] { "ModA", "ModB", "ModC" }) Directory.CreateDirectory(Path.Combine(paths.Mods, name, "files"));
        var modsBoard = w.boards["mods"];
        Click(w, w, "Refresh");
        for (int i = 0; i < 100 && modsBoard.GetVisualDescendants().OfType<CheckBox>().Count() < 3; i++) { Pump(); Thread.Sleep(10); }
        var modTargets = ControllerNavigation.Targets(modsBoard).ToList();
        Check(modTargets.OfType<CheckBox>().Count() == 3, "Pad cannot reach the mods' checkboxes.");
        foreach (var label in new[] { "Add a mod (zip)", "Open the mods folder", "Refresh" })
            Check(modTargets.OfType<PlankButton>().Any(b => b.Text == label), "Pad cannot reach the Mods button " + label + ".");
        var second = modTargets.OfType<CheckBox>().ElementAt(1);
        second.Focus(NavigationMethod.Directional); Pump();
        Press(w, LauncherButtons.Confirm);
        for (int i = 0; i < 100 && second.IsChecked != false; i++) { Pump(); Thread.Sleep(10); }
        Check(File.Exists(Path.Combine(paths.Mods, "ModB", "disabled")), "Controller did not untick the mod.");
        for (int i = 0; i < 100; i++) { Pump(); Thread.Sleep(10); }  // the rebuild after the tick
        var focusedBox = w.FocusManager?.GetFocusedElement() as CheckBox;
        Check(focusedBox != null && modsBoard.GetVisualDescendants().Contains(focusedBox) &&
              ((focusedBox.Content as TextBlock)?.Text == "ModB"), "Ticking a mod lost the pad's focus (list rebuilt under it).");
        Check(focusedBox.Classes.Contains(":focus-visible"), "A pad tick did not keep the focus highlight on the mod.");
        Press(w, LauncherButtons.Down);
        Check((((w.FocusManager?.GetFocusedElement() as CheckBox)?.Content as TextBlock)?.Text) == "ModC", "Down did not move to the next mod after a tick.");
        Press(w, LauncherButtons.Confirm); for (int i = 0; i < 100; i++) { Pump(); Thread.Sleep(10); }
        Press(w, LauncherButtons.Up);
        Check((((w.FocusManager?.GetFocusedElement() as CheckBox)?.Content as TextBlock)?.Text) == "ModB", "Up after a tick did not go back to the previous mod.");
        // A mouse click (pointer focus) on a mod's checkbox: after the rebuild it keeps focus but shows no focus box.
        var clicked = modsBoard.GetVisualDescendants().OfType<CheckBox>().First(c => (c.Content as TextBlock)?.Text == "ModA");
        clicked.Focus(NavigationMethod.Pointer); Pump();
        Check(!clicked.Classes.Contains(":focus-visible"), "Harness: pointer focus unexpectedly shows the focus box.");
        clicked.IsChecked = !clicked.IsChecked;
        for (int i = 0; i < 100; i++) { Pump(); Thread.Sleep(10); }
        var afterClick = w.FocusManager?.GetFocusedElement() as CheckBox;
        Check(afterClick != null && (afterClick.Content as TextBlock)?.Text == "ModA", "A mouse tick lost focus on the mod.");
        Check(!afterClick.Classes.Contains(":focus-visible"), "A mouse tick put a focus box around the mod.");
        string zips = Path.Combine(paths.Root, "zips");
        Directory.CreateDirectory(Path.Combine(zips, "Sub"));
        File.WriteAllText(Path.Combine(zips, "a.zip"), "x"); File.WriteAllText(Path.Combine(zips, "b.ISO"), "x"); File.WriteAllText(Path.Combine(zips, "c.txt"), "x");
        var zipPick = ControllerDialogs.PickPath(w, "Synthetic zip browser", zips, false, viaController: true, extensions: new[] { ".zip" }, typeName: "Mod zip");
        Pump(); var zipModal = w.navigation.Modal!;
        Check(Find<ControllerList>(zipModal).ItemCount == 2, "Zip browser did not list only .zip files and folders.");
        Press(w, LauncherButtons.Back); Finish(zipPick);
        // The real button: pressed by the pad it opens the themed browser, not the OS picker.
        Click(w, w, "Add a mod (zip)");
        Check(w.navigation.Modal is { } zipDialog && zipDialog.GetVisualDescendants().OfType<ControllerList>().Any(), "Add a mod (zip) with the pad did not open the themed browser.");
        Press(w, LauncherButtons.Back); Pump();
        Check(w.navigation.Modal == null, "Backing out of the mod browser left a dialog open.");
        w.ShowBoard("extras"); Pump();
        Check(ControllerNavigation.Targets(w.boards["extras"]).OfType<PlankButton>().Count() > 3, "Extras controls not navigable.");
        Shot(w, "Extras-focus");

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
        var file = ControllerDialogs.PickPath(w, "Synthetic file browser", browser, false, viaController: true);
        modal = w.navigation.Modal!; Pump();
        var list = Find<ControllerList>(modal);
        Check(list.ItemCount == 2, "Disc filter did not include folders/uppercase ISO and exclude other files.");
        Shot(modal, "File-browser");
        // No tabs in a dialog: the bumpers still move to the next control (how its toolbar is reached).
        Press(w, LauncherButtons.Next);
        Check(modal.FocusManager?.GetFocusedElement() is PlankButton, "RB in a dialog without tabs did not move to the next control.");
        list.Focus(NavigationMethod.Directional); Pump();
        Press(w, LauncherButtons.Down); Press(w, LauncherButtons.Confirm); Finish(file);
        Check(file.Result == Path.Combine(browser, "disc.ISO"), "Managed file browser returned wrong path.");
        var folder = ControllerDialogs.PickPath(w, "Synthetic folder browser", browser, true, viaController: true);
        modal = w.navigation.Modal!; Pump();
        Press(w, LauncherButtons.Confirm);
        Click(w, modal, "Use this folder"); Finish(folder);
        Check(folder.Result == Path.Combine(browser, "Child"), "Managed folder browser did not descend/select.");
        file = ControllerDialogs.PickPath(w, "Synthetic cancelled browser", browser, false, viaController: true);
        Pump(); Press(w, LauncherButtons.Back); Finish(file);
        Check(file.Result == null && File.ReadAllText(config).Contains("unknown = 42"), "Cancelled browser changed settings.");
        folder = ControllerDialogs.PickPath(w, "Synthetic folder creation", browser, true, viaController: true); Pump();
        modal = w.navigation.Modal!;
        Click(w, modal, "New folder");
        var keyboard = w.navigation.Modal!;
        Click(w, keyboard, "1"); Click(w, keyboard, "Apply");
        Check(Directory.Exists(Path.Combine(browser, "1")), "Controller folder creation failed.");
        Press(w, LauncherButtons.Back); Finish(folder);
        Check(folder.Result == null, "Cancelling the folder picker applied its path.");
        file = ControllerDialogs.PickPath(w, "Synthetic unreadable file", browser, false, viaController: true); Pump();
        modal = w.navigation.Modal!;
        list = Find<ControllerList>(modal);
        list.SelectedIndex = list.ItemCount - 1;
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
        w.layoutBox.Focus(NavigationMethod.Directional); Press(w, LauncherButtons.Confirm); Press(w, LauncherButtons.Confirm);
        Check(File.ReadAllText(config) == before, "Explicit same-value custom scheme rewrote unrelated configuration.");
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
