using System.Diagnostics;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using static FrightWorks.Skin;

namespace FrightWorks;

// The launcher window: the haunted mansion (MansionScene) whose signs are the menu. Play starts the game; Settings,
// Mods and Extras open a board over the left of the house. Everything it changes goes to Config.toml (DataPaths),
// the keys the game's F10 menu uses. No game data or character art is shipped.
sealed class MainWindow : Window
{
    internal const string AppTitle = "FrightWorks: Night of 100 Frights";
    static readonly string GameExe = DataPaths.GameExe;
    const string ProjectUrl = "https://github.com/FrightWorks/FrightWorks-no100f";

    internal readonly DataPaths paths;
    readonly string root, play, userData, configPath, modsDir, discDir;
    readonly string? shotDir;
    readonly bool enableServices;
    internal bool GameOwnsControllers { get; private set; }
    internal readonly MansionScene scene;
    internal readonly Dictionary<string, Board> boards = new();
    readonly Stage stage;
    TextBlock discStatus = null!, dumpHelp = null!;
    PlankButton chooseDisc = null!;
    ThemedBar progress = null!, ffmpegBar = null!;
    TextBlock ffmpegStatus = null!, savesNote = null!, updateStatus = null!;
    PlankButton savesDefault = null!, checkNow = null!;
    readonly Updater updater;
    bool updating;  // checking, or downloading / applying an update: Play waits
    bool asking;    // a question (Xbox layout, update) is open: the other waits, so one never disables the other
    PlankButton ffmpegButton = null!;
    CancellationTokenSource? ffmpegCancel;  // an ffmpeg download is running
    internal Action<bool>? showControlsTab;
    readonly Gamepads gamepads;
    internal readonly ControllerNavigation navigation;
    internal TextBlock padStatus = null!;
    internal ThemedCombo layoutBox = null!;
    readonly List<(PadMapping.Row Row, TextBlock Label, ThemedCombo Box)> padRows = new();
    bool askedLayout;
    bool fillingLayout;
    bool startupReady;
    internal CheckBox autoLayout = null!;
    readonly DispatcherTimer padTimer = new() { Interval = TimeSpan.FromSeconds(1) };

    public MainWindow(string? shotDir = null, DataPaths? dataPaths = null, bool enableServices = true)
    {
        this.shotDir = shotDir;
        this.enableServices = enableServices;
        paths = dataPaths ?? DataPaths.Find();
        (root, play, userData, configPath, modsDir, discDir) = (paths.Root, paths.Game, paths.UserData, paths.ConfigPath, paths.Mods, paths.Disc);
        gamepads = new Gamepads(play);
        navigation = new ControllerNavigation(this, gamepads);
        startupReady = !enableServices;
        updater = new Updater(paths);

        Title = AppTitle;
        Icon = App.Icon;
        var screen = Screens.Primary;
        var area = screen == null ? new Size(1220, 890) : screen.WorkingArea.Size.ToSize(screen.Scaling);
        Width = Math.Min(1180, area.Width - 40);
        Height = Math.Min(830, area.Height - 60);
        MinWidth = Math.Min(960, area.Width);
        MinHeight = Math.Min(680, area.Height);
        Background = Brush(BoardBottom);
        WindowStartupLocation = WindowStartupLocation.CenterScreen;

        scene = new MansionScene(enableServices && shotDir == null && MansionScene.AnimationsEnabled(), shotDir == null ? Environment.TickCount : 1);
        scene.SignClicked += id => OnSign(id).Watch();
        stage = new Stage(scene);
        Content = stage;

        AddBoard("settings", "Settings", BuildSettings);
        AddBoard("mods", "Mods", BuildMods);
        AddBoard("extras", "Extras", BuildExtras);
        RefreshDisc();

        // Escape closes an open board (an open drop-down list takes it first).
        KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape && boards.Values.Any(b => b.IsVisible)) { CloseBoards(); e.Handled = true; }
        };
        // Connected controllers: shown on Settings > Controls; an Xbox pad asks for the layout once.
        padTimer.Tick += (_, _) => RefreshGamepads().Watch();
        Closed += (_, _) => { padTimer.Stop(); navigation.Dispose(); gamepads.Dispose(); ffmpegCancel?.Cancel(); };
        Opened += (_, _) =>
        {
            if (shotDir != null) { Dispatcher.UIThread.Post(() => TakeShots(shotDir), DispatcherPriority.Background); return; }
            if (!enableServices) return;
            UpdateControllerOwnership();
            navigation.Start();
            padTimer.Start();
            scene.Focus(NavigationMethod.Directional);
        };
    }

    // The scene fills the window; the open board sits over the house's left part, clear of the signs.
    sealed class Stage : Panel
    {
        readonly MansionScene scene;

        public Stage(MansionScene scene)
        {
            this.scene = scene;
            Children.Add(scene);
        }

        Rect BoardRect(Size size)
        {
            scene.Place(size);
            double left = 14, top = 14, right = scene.SignsLeft - 18, bottom = size.Height - 44;
            return new Rect(left, top, Math.Max(300, right - left), Math.Max(300, bottom - top));
        }

        protected override Size MeasureOverride(Size availableSize)
        {
            scene.Measure(availableSize);
            var board = BoardRect(availableSize).Size;
            foreach (var child in Children) if (child != scene) child.Measure(board);
            return availableSize;
        }

        protected override Size ArrangeOverride(Size finalSize)
        {
            scene.Arrange(new Rect(finalSize));
            var board = BoardRect(finalSize);
            foreach (var child in Children) if (child != scene) child.Arrange(board);
            return finalSize;
        }
    }

    // Headless visual check (--shots <dir>): the main screen, a lightning flash and every board, then exit.
    void TakeShots(string dir)
    {
        Directory.CreateDirectory(dir);
        void Shot(string name) => SaveShot(Path.Combine(dir, name + ".png"));
        scene.Showcase(false);
        Shot("Main");
        scene.Showcase(true);
        Shot("Lightning");
        scene.Showcase(false);
        foreach (var id in new[] { "settings", "mods", "extras" })
        {
            ShowBoard(id);
            Shot(char.ToUpperInvariant(id[0]) + id[1..]);
        }
        ShowBoard("extras");
        UpdateLayout();
        boards["extras"].Scroller.ScrollToEnd();  // the bottom: folders and saves
        Shot("Extras-Folders");
        boards["extras"].Scroller.ScrollToHome();
        ShowBoard("settings");
        showControlsTab?.Invoke(true);
        Shot("Settings-Controls");
        Close();
    }

    // The window as it looks now, at the screen's pixel density.
    internal void SaveShot(string file)
    {
        UpdateLayout();
        double scale = RenderScaling;
        var size = new PixelSize(Math.Max(1, (int)Math.Round(Bounds.Width * scale)), Math.Max(1, (int)Math.Round(Bounds.Height * scale)));
        using var bitmap = new RenderTargetBitmap(size, new Vector(96 * scale, 96 * scale));
        bitmap.Render(this);
        bitmap.Save(file, PngBitmapEncoderOptions.Default);
    }

    // ----- Signs and boards -----
    async Task OnSign(string id)
    {
        switch (id)
        {
            case "play":
                if (updating && updater.Installed)
                    await Dialogs.Message(this, "The launcher is updating the game. It starts again by itself when it's done.");
                else if (CanPlay()) await Launch();
                else ShowBoard("extras");
                break;
            case "exit":
                Close();
                break;
            default:
                if (boards.TryGetValue(id, out var b) && b.IsVisible) CloseBoards();
                else ShowBoard(id);
                break;
        }
    }

    void AddBoard(string id, string title, Action<StackPanel> build)
    {
        var board = new Board(title, CloseBoards);
        build(board.Body);
        boards[id] = board;
        stage.Children.Add(board);
    }

    internal void ShowBoard(string id)
    {
        RefreshDisc();
        foreach (var (key, b) in boards) b.IsVisible = key == id;
        scene.ActiveSign = id;
        UpdateLayout();
        ControllerNavigation.FocusFirst(boards[id]);
        navigation.Reset();
    }

    internal void CloseBoards()
    {
        foreach (var b in boards.Values) b.IsVisible = false;
        scene.ActiveSign = null;
        scene.Focus(NavigationMethod.Directional);
        navigation.Reset();
    }

    static WrapPanel Row() => new() { Orientation = Orientation.Horizontal };

    static StackPanel Column() => new() { Orientation = Orientation.Vertical };

    async Task OpenFolder(string path)
    {
        try
        {
            var dir = Directory.CreateDirectory(path);
            if (!await Launcher.LaunchDirectoryInfoAsync(dir)) throw new IOException("no file manager answered");
        }
        catch (Exception e) { await Dialogs.Message(this, "Couldn't open " + path + ": " + e.Message); }
    }

    // ----- Settings: Display and Controls -----
    void BuildSettings(StackPanel body)
    {
        var tabs = new TabBar();
        var display = Column();
        var controls = Column();
        controls.IsVisible = false;
        PlankButton displayTab = null!, controlsTab = null!;
        void Select(bool showControls)
        {
            display.IsVisible = !showControls;
            controls.IsVisible = showControls;
            displayTab.Selected = !showControls;
            controlsTab.Selected = showControls;
        }
        displayTab = new PlankButton("Display", (_, _) => Select(false)) { Selected = true };
        controlsTab = new PlankButton("Controls", (_, _) => Select(true));
        showControlsTab = Select;
        tabs.Children.Add(displayTab);
        tabs.Children.Add(controlsTab);
        body.Children.Add(tabs);
        BuildDisplay(display);
        BuildControls(controls);
        body.Children.Add(display);
        body.Children.Add(controls);
    }

    // Config values as the game reads them: numbers by value (F10 writes 8 or 0.75, a hand-edited file may say 8.0 or
    // 2.250), the rest as text without the quotes.
    static bool SameValue(string a, string b)
    {
        a = a.Trim('"');
        b = b.Trim('"');
        return a == b || double.TryParse(a, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var x) &&
                         double.TryParse(b, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var y) && x == y;
    }

    // F10's resolutions (settings_overlay.cpp kResolutions), written the way F10 writes them; the game takes no others.
    static readonly (string text, string raw)[] Resolutions =
    {
        ("Auto (screen size)", "0"), ("0.5x (240p)", "0.5"), ("0.75x (360p)", "0.75"), ("1x (480p)", "1"),
        ("1.5x (720p)", "1.5"), ("2x (960p)", "2"), ("2.25x (1080p)", "2.25"), ("3x (1440p)", "3"),
        ("4x (1920p)", "4"), ("4.5x (4K)", "4.5"), ("6x (2880p)", "6"), ("8x (3840p)", "8"),
    };

    // F10's rule (LimitResolutionForFrameRate): frame interpolation above 60 fps allows up to 4x; the game turns 6x and
    // 8x into 4x at start.
    static bool AboveCap(string resolution) => SameValue(resolution, "6") || SameValue(resolution, "8");

    // [video] frame_interpolation_fps as the game reads it: 0, 120 or 180 (240 from older versions = 180), else off.
    static int InterpolationFps(GameConfig c) => c.Get("video", "frame_interpolation_fps")?.Trim() switch
    {
        "120" => 120, "180" or "240" => 180, _ => 0,
    };

    // The game picks the fullscreen monitor itself on Windows only ([video] fullscreen_display); elsewhere the row is left out.
    internal static bool MonitorChoice => OperatingSystem.IsWindows();

    void BuildDisplay(StackPanel body)
    {
        var cfg = new GameConfig(configPath);
        var table = new FormTable();
        void Row(string label, string key, (string text, string raw)[] options, string fallback, Action<GameConfig>? changed = null)
        {
            var box = new ThemedCombo();
            foreach (var o in options) box.Items.Add(o.text);
            var current = cfg.Get("video", key) ?? fallback;
            int index = Array.FindIndex(options, o => SameValue(o.raw, current));
            box.SelectedIndex = index >= 0 ? index : Array.FindIndex(options, o => SameValue(o.raw, fallback));
            box.SelectedIndexChanged += (_, _) =>
            {
                if (box.SelectedIndex < 0) return;
                var c = new GameConfig(configPath);
                c.Set("video", key, options[box.SelectedIndex].raw);
                changed?.Invoke(c);
                c.Save();
            };
            table.Add(FieldLabel(label), box);
        }
        Row("Window", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Window", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }, "borderless");

        // A saved monitor that isn't connected stays selected and saved, so it works again when plugged back in. One found
        // by model on another port keeps its saved id: both mean the same monitor to the game.
        if (MonitorChoice)
        {
            var monitors = enableServices ? Monitors.List() : new List<Monitors.Monitor>();
            string savedMonitor = cfg.Get("video", "fullscreen_display") ?? "";
            if (savedMonitor.Length == 0) savedMonitor = "auto";
            var savedMatch = Monitors.Find(monitors, savedMonitor);
            var monitorOptions = new List<(string text, string raw)> { ("Automatic (where the game window is)", "\"auto\"") };
            foreach (var m in monitors) monitorOptions.Add((m.Label, "\"" + (m == savedMatch ? savedMonitor : m.Id) + "\""));
            if (savedMonitor != "auto" && savedMatch == null) monitorOptions.Add(("Saved monitor (not connected)", "\"" + savedMonitor + "\""));
            Row("Fullscreen monitor", "fullscreen_display", monitorOptions.ToArray(), "auto");
        }

        // Resolution: refilled when frame interpolation crosses 60 fps. A missing or unknown value is the game's 1080p.
        var resolutionBox = new ThemedCombo();
        var shownResolutions = Resolutions;
        bool filling = false;
        void FillResolutions(GameConfig c)
        {
            bool capped = InterpolationFps(c) > 60;
            shownResolutions = capped ? Resolutions.Where(r => !AboveCap(r.raw)).ToArray() : Resolutions;
            var current = c.Get("video", "resolution_multiplier") ?? "2.25";
            if (!Resolutions.Any(r => SameValue(r.raw, current))) current = "2.25";
            if (capped && AboveCap(current)) current = "4";
            filling = true;
            resolutionBox.Items.Clear();
            foreach (var r in shownResolutions) resolutionBox.Items.Add(r.text);
            resolutionBox.SelectedIndex = Array.FindIndex(shownResolutions, r => SameValue(r.raw, current));
            filling = false;
        }
        FillResolutions(cfg);
        resolutionBox.SelectedIndexChanged += (_, _) =>
        {
            if (filling || resolutionBox.SelectedIndex < 0) return;
            var c = new GameConfig(configPath);
            c.Set("video", "resolution_multiplier", shownResolutions[resolutionBox.SelectedIndex].raw);
            c.Save();
        };
        table.Add(FieldLabel("Resolution"), resolutionBox);

        // Without aspect_mode the game goes by the older keys (main.cpp): force_16_9 = 16:9, widescreen = false = 4:3,
        // else fill the screen.
        string aspectFallback = cfg.Get("video", "force_16_9") == "true" ? "1" : cfg.Get("video", "widescreen") == "false" ? "0" : "2";
        Row("Picture shape", "aspect_mode", new[] { ("Original 4:3", "0"), ("Widescreen 16:9", "1"), ("Fill the screen", "2"), ("Cinematic 2.39:1", "3") }, aspectFallback);
        Row("Frame rate", "fps60", new[] { ("30 fps (original)", "false"), ("60 fps", "true") }, "false");
        Row("HD textures", "texture_replacements", new[] { ("On", "true"), ("Off (original textures)", "false") }, "true");
        Row("Anti-aliasing", "msaa", new[] { ("Off", "1"), ("2x MSAA", "2"), ("4x MSAA", "4"), ("8x MSAA", "8") }, "1");
        Row("Intro movies", "skip_intro", new[] { ("Play them", "false"), ("Skip them", "true") }, "false");
        Row("HDR", "hdr", new[] { ("Off", "false"), (OperatingSystem.IsWindows() ? "On (needs HDR on in Windows)" : "On (needs HDR on in your desktop)", "true") }, "false");
        Row("HDR brightness", "hdr_paper_white_nits", new[] { ("Dim (120 nits)", "120.0"), ("Normal (200 nits)", "200.0"), ("Bright (300 nits)", "300.0"), ("Brightest (400 nits)", "400.0") }, "200.0");
        Row("HDR highlights", "hdr_peak_nits", new[] { ("Subtle (400 nits)", "400.0"), ("Standard (1000 nits)", "1000.0"), ("Strong (1500 nits)", "1500.0") }, "1000.0");
        body.Children.Add(table);
        body.Children.Add(Note("Changes are saved at once and apply the next time the game starts. In the game, F10 has the " +
                               (MonitorChoice ? "resolution, picture shape, window, fullscreen monitor and frame interpolation settings too, and more (sound, controls); "
                                              : "resolution, picture shape, window and frame interpolation settings too, and more (sound, controls); ") +
                               "F11 switches between fullscreen and a window, and F2 saves a screenshot." +
                               (MonitorChoice ? " Fullscreen monitor also applies when F11 goes fullscreen from a window." : "")));

        // Experimental: F10's Smoothness > Frame interpolation. Going above 60 fps at 6x or 8x drops the resolution to
        // 4x, as F10 does.
        body.Children.Add(Title("Experimental"));
        table = new FormTable();  // Row adds to this one now
        Row("Frame interpolation", "frame_interpolation_fps", new[] { ("Off", "0"), ("120 FPS", "120"), ("180 FPS", "180") },
            InterpolationFps(cfg).ToString(), c =>
            {
                if (InterpolationFps(c) > 60 && AboveCap(c.Get("video", "resolution_multiplier") ?? "")) c.Set("video", "resolution_multiplier", "4");
                FillResolutions(c);
            });
        body.Children.Add(table);
        body.Children.Add(Note("Draws extra frames between the game's own for smoother motion. Experimental: you might see visual " +
                               "glitches. At 120 or 180 FPS the resolution goes up to 4x (1920p)."));
    }

    void BuildControls(StackPanel body)
    {
        body.Children.Add(Note("Navigate the launcher with the D-pad or left stick; South confirms, East goes back, bumpers move focus, " +
                               "and the right stick scrolls. Launcher navigation is independent of the game's scheme and bindings. " +
                               "Controller-operated file pickers and text entry use launcher dialogs; mouse-opened native pickers use the OS keyboard/mouse."));
        body.Children.Add(Title("Gamepad"));
        padStatus = FieldLabel("Looking for controllers...");
        padStatus.Margin = new Thickness(0, 4, 0, 6);
        body.Children.Add(padStatus);
        // [controller] layout: "gamecube" or "xbox_original" (the game's original Xbox controls, hle/no100f_xbox_layout.h).
        var table = new FormTable();
        layoutBox = new ThemedCombo();
        layoutBox.Items.Add("GameCube");
        layoutBox.Items.Add("Original Xbox");
        var config = new GameConfig(configPath);
        ShowLayout(config);
        void ManualLayout()
        {
            if (fillingLayout || layoutBox.SelectedIndex < 0) return;
            if (layoutBox.SelectedIndex <= 1) SaveLayout(layoutBox.SelectedIndex == 1);
            else if (new GameConfig(configPath).Get("controller", "layout") is string saved) SaveLayout(saved);
            ShowXboxRows();
        }
        layoutBox.SelectedIndexChanged += (_, _) => ManualLayout();
        layoutBox.SelectionCommitted += (_, _) => ManualLayout();
        table.Add(FieldLabel("Layout"), layoutBox);
        body.Children.Add(table);
        autoLayout = new CheckBox { Content = "Automatically match the connected controller until I choose a layout",
                                   IsChecked = ControllerLayout.Automatic(config), Margin = new Thickness(0, 6, 0, 6) };
        autoLayout.IsCheckedChanged += (_, _) =>
        {
            if (fillingLayout) return;
            var c = new GameConfig(configPath);
            ControllerLayout.SetAutomatic(c, autoLayout.IsChecked == true);
            c.Save();
            ApplyControllerLayout(gamepads.Connected).Watch();
        };
        body.Children.Add(autoLayout);
        body.Children.Add(Note("Xbox (including Xbox One) matches Original Xbox; GameCube matches GameCube. Other or conflicting " +
                               "controller types leave the layout unchanged. Explicit layout choices, including the current value, stop automatic selection; bindings are kept."));
        body.Children.Add(Note("Original Xbox: A jump, X head smash, B dig / enter / talk and back in menus, Y on-screen display, " +
                               "LT gum, RT run, LB soap bubble (Xbox White), RB sneak (Xbox Black); the bumpers work together with the " +
                               "triggers, and the hints in the game name these buttons. Black and White can go on other buttons below. " +
                               "GameCube: B head smash and back in menus, X dig / enter / talk, Z + L soap bubble, Z + R sneak (Z = RB). " +
                               "Also in F10 > Controls."));
        BuildPadMapping(body);
        body.Children.Add(Title("Keyboard"));
        table = new FormTable();
        foreach (var (what, key) in new[] { ("Move", "W A S D"), ("Camera (C-stick)", "Arrow keys"), ("A (jump)", "Space"), ("B", "Left Shift"),
                     ("X / Y", "R / F"), ("Z", "C"), ("L / R", "Q / E"), ("Start (pause)", "Enter"), ("D-pad", "I J K L") })
        {
            var name = FieldLabel(what);
            name.FontWeight = FontWeight.Normal;
            name.Foreground = Brush(CreamDim);
            table.Add(name, FieldLabel(key));
        }
        body.Children.Add(table);
        body.Children.Add(Title("In the game"));
        body.Children.Add(Note("F10 settings   ·   F11 fullscreen / window   ·   F2 screenshot   ·   F1 project page"));
    }

    // ----- Mods -----
    void BuildMods(StackPanel body)
    {
        var sep = Path.DirectorySeparatorChar;
        var patches = Patches;
        body.Children.Add(Note($"A mod is a folder in the mods folder (the button below opens it). Its files{sep} folder mirrors the disc's files{sep} (replacing game files), " +
                               $"its textures{sep} folder holds texture replacements, and a patch.json adds to levels using your own disc. Untick a mod to switch it off; when two mods replace the same file, the later name wins."));
        var list = new StackPanel { Orientation = Orientation.Vertical };
        var frame = new Border
        {
            Background = Brush(Field), Padding = new Thickness(10, 8, 10, 8), Margin = new Thickness(0, 8, 0, 8),
            Height = 240,
            Child = new ScrollViewer { Content = list, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled, AllowAutoHide = false },
        };
        void Fill()
        {
            // The list is rebuilt after every tick: put the pad back on the same mod, or it would be left on nothing.
            // Only a pad/keyboard focus shows the focus box; a mouse click must not get one, so restore it the way it came.
            var focused = TopLevel.GetTopLevel(list)?.FocusManager?.GetFocusedElement() as CheckBox;
            var keep = focused is { Parent: { } p } && ReferenceEquals(p, list) ? (focused.Content as TextBlock)?.Text : null;
            var how = focused?.Classes.Contains(":focus-visible") == true ? NavigationMethod.Directional : NavigationMethod.Pointer;
            CheckBox? refocus = null;
            list.Children.Clear();
            var dirs = Directory.Exists(modsDir) ? Directory.GetDirectories(modsDir).Where(ModPatches.ModPatchPanel.IsListed).OrderBy(d => d, StringComparer.OrdinalIgnoreCase).ToArray() : Array.Empty<string>();
            if (dirs.Length == 0) { list.Children.Add(Note("No mods yet.")); return; }
            foreach (var d in dirs)
            {
                var box = new CheckBox
                {
                    Content = new TextBlock { Text = Path.GetFileName(d), TextWrapping = TextWrapping.NoWrap, TextTrimming = TextTrimming.CharacterEllipsis },
                    IsChecked = !File.Exists(Path.Combine(d, "disabled")), Foreground = Brush(Cream),
                    Cursor = new Cursor(StandardCursorType.Hand), Margin = new Thickness(0, 3, 0, 3),
                };
                var dir = d;
                box.IsCheckedChanged += (_, _) =>
                {
                    var flag = Path.Combine(dir, "disabled");
                    if (box.IsChecked == true) File.Delete(flag); else File.WriteAllText(flag, "");
                    patches.Update(Fill).Watch();
                };
                list.Children.Add(box);
                if (keep != null && Path.GetFileName(d) == keep) refocus = box;
                if (patches.StatusText(d) is { } status) list.Children.Add(Note(status));
            }
            if (refocus != null) Dispatcher.UIThread.Post(() => { refocus.Focus(how); refocus.BringIntoView(); }, DispatcherPriority.Loaded);
        }
        body.Children.Add(new LeftCap(480, frame));
        var row = Row();
        row.Children.Add(new PlankButton("Add a mod (zip)", (_, _) => patches.AddFromZip(Fill).Watch()));
        row.Children.Add(new PlankButton("Open the mods folder", (_, _) => OpenFolder(modsDir).Watch()));
        row.Children.Add(new PlankButton("Refresh", (_, _) => { Fill(); patches.Update(Fill).Watch(); }));
        body.Children.Add(row);
        Fill();
        patches.Update(Fill).Watch();
    }

    ModPatches.ModPatchPanel? patchPanel;
    ModPatches.ModPatchPanel Patches => patchPanel ??= new ModPatches.ModPatchPanel(this, paths, GameRunning);

    // ----- Extras: the disc, holidays, folders, about -----
    void BuildExtras(StackPanel body)
    {
        body.Children.Add(Title("Game disc"));
        discStatus = FieldLabel("");
        discStatus.MaxWidth = 520;
        body.Children.Add(discStatus);
        chooseDisc = new PlankButton("Choose disc image", (_, _) => ChooseDisc().Watch(), primary: true);
        body.Children.Add(chooseDisc);
        progress = new ThemedBar { IsVisible = false };
        body.Children.Add(new LeftCap(480, progress));
        body.Children.Add(dumpHelp = Note(
            "FrightWorks runs your own copy of Scooby-Doo! Night of 100 Frights for the GameCube (USA, GIHE78, version 1.01). " +
            "Dump the disc you own (for example with CleanRip on a Wii) as a plain .iso or .ciso, then choose it here. " +
            "The launcher copies the game files once; the image file isn't needed afterwards."));

        body.Children.Add(Title("Holidays"));
        body.Children.Add(HolidayBox());
        body.Children.Add(Note("Outside Mystic Manor and in the playground, the game celebrates holidays on their real dates. " +
                               "Pick one to see it any day, or turn them off."));

        body.Children.Add(Title("Developer tools"));
        body.Children.Add(DevToolsBox());
        body.Children.Add(Note("For testing and bug hunting: F8 in the game opens the dev menu (go to any level, saved spots, " +
                               "free camera, god mode, texture finder, recording options), F3 records a video of the game into " +
                               "the Videos folder with a .txt for bug reports, and the game shows DEV and its build in the " +
                               "corner. Applies the next time the game starts."));
        ffmpegStatus = Note("");
        body.Children.Add(ffmpegStatus);
        ffmpegButton = new PlankButton("Download ffmpeg", (_, _) => DownloadFfmpeg().Watch());
        body.Children.Add(ffmpegButton);
        ffmpegBar = new ThemedBar { IsVisible = false };
        body.Children.Add(new LeftCap(480, ffmpegBar));
        RefreshFfmpeg(offerDownload: false);

        body.Children.Add(Title("Folders"));
        var folders = Row();
        folders.MaxWidth = 520;  // wrap instead of widening the page
        folders.HorizontalAlignment = HorizontalAlignment.Left;
        folders.Children.Add(new PlankButton("Saves", (_, _) => OpenFolder(paths.Saves).Watch()));
        folders.Children.Add(new PlankButton("Settings and mods", (_, _) => OpenFolder(userData).Watch()));
        folders.Children.Add(new PlankButton("Screenshots", (_, _) => OpenFolder(paths.Screenshots).Watch()));
        folders.Children.Add(new PlankButton("Videos", (_, _) => OpenFolder(paths.Videos).Watch()));
        folders.Children.Add(new PlankButton("Game", (_, _) => OpenFolder(play).Watch()));
        body.Children.Add(folders);
        savesNote = Note("");
        body.Children.Add(savesNote);
        var saveRow = Row();
        saveRow.MaxWidth = 520;
        saveRow.HorizontalAlignment = HorizontalAlignment.Left;
        saveRow.Children.Add(new PlankButton("Change the saves folder", (_, _) => ChooseSavesFolder().Watch()));
        savesDefault = new PlankButton("Back to the usual folder", (_, _) => MoveSaves(null).Watch());
        saveRow.Children.Add(savesDefault);
        saveRow.Children.Add(new PlankButton("Copy from an earlier version", async (_, _) =>
        {
            await DataImport.FromFolder(this, paths, AppTitle);
            RefreshDisc();
            RefreshSaves();
        }));
        body.Children.Add(saveRow);
        RefreshSaves();

        body.Children.Add(Title("Updates"));
        updateStatus = Note("");
        if (updater.CanCheck)
        {
            body.Children.Add(UpdateBox());
            body.Children.Add(updateStatus);
            checkNow = new PlankButton("Check now", (_, _) => CheckForUpdates(manual: true).Watch());
            body.Children.Add(checkNow);
            ShowUpdateState(null);
        }
        else
        {
            updateStatus.Text = paths.DevTree ? "A source build doesn't look for updates."
                                              : "This copy wasn't installed with the FrightWorks installer, so it can't look for updates.";
            body.Children.Add(updateStatus);
        }

        body.Children.Add(Title("About"));
        var version = typeof(MainWindow).Assembly.GetName().Version;
        body.Children.Add(Note("FrightWorks " + (version == null ? "" : "v" + version.ToString(3) + " ") + "by Loneranger419, based on Scooby-Doo: Refrightened by JosiahG. Built on WiiCompiled (GPLv3) " +
                               "and Aurora (MIT). HD texture pack by dogerki. Button prompts by Kenney (CC0). Lettering in " +
                               "Luckiest Guy by Astigmatic and in-game menu text in Roboto by Christian Robertson (both Apache 2.0)" +
                               "; the launcher bundles Inter by Rasmus Andersson (SIL Open Font License), which it uses for text on Linux. " +
                               "Scooby-Doo is a trademark of Hanna-Barbera / Warner Bros.; this is an unofficial fan project."));
        body.Children.Add(new PlankButton("Project page", async (_, _) => await Launcher.LaunchUriAsync(new Uri(ProjectUrl))));
    }

    // ----- The saves folder ([paths] saves_dir) -----
    void RefreshSaves()
    {
        bool custom = !string.IsNullOrWhiteSpace(new GameConfig(configPath).Get("paths", "saves_dir"));
        savesNote.Text = "Saves are in " + paths.Saves + (custom ? " (your choice)." : ".") +
                         " Settings and mods are in " + userData + ".";
        savesDefault.IsVisible = custom;
    }

    async Task ChooseSavesFolder()
    {
        var path = await ControllerDialogs.PickPath(this, "Choose the folder for your saves", paths.Saves, folder: true);
        if (path != null) await MoveSaves(path);
    }

    static bool SamePath(string a, string b) => string.Equals(Path.TrimEndingDirectorySeparator(a), Path.TrimEndingDirectorySeparator(b),
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    // Points [paths] saves_dir at target (null = the usual folder) and offers to move the saves already there. Files the
    // new folder already has are left in both places.
    async Task MoveSaves(string? target)
    {
        var from = paths.Saves;
        var to = Path.GetFullPath(target ?? paths.DefaultSaves);
        if (SamePath(from, to))
        {
            if (target == null) { var c = new GameConfig(configPath); c.Remove("paths", "saves_dir"); c.Save(); RefreshSaves(); }
            return;
        }
        // An installed game's folder (%LOCALAPPDATA%\FrightWorks) is replaced on update and deleted on uninstall.
        var program = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(root)) ?? root;
        var sep = Path.DirectorySeparatorChar;
        if (!paths.Portable && (to + sep).StartsWith(program + sep, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
        {
            await Dialogs.Message(this, "That folder is part of the installed game, which an update replaces. Choose another one.");
            return;
        }
        if (GameRunning())
        {
            await Dialogs.Message(this, "Close the game first: it is using the saves.");
            return;
        }
        var files = Directory.Exists(from) ? Directory.GetFiles(from, "*", SearchOption.AllDirectories) : Array.Empty<string>();
        var answer = files.Length == 0 ? Dialogs.Answer.No
            : await Dialogs.YesNoCancel(this, $"Move your saves from\n{from}\nto\n{to}?\n\nNo uses the new folder without moving them.");
        if (answer == Dialogs.Answer.Cancel) return;
        int kept = 0;
        try
        {
            Directory.CreateDirectory(to);
            if (answer == Dialogs.Answer.Yes)
                foreach (var f in files)
                {
                    var dest = Path.Combine(to, Path.GetRelativePath(from, f));
                    if (File.Exists(dest)) { kept++; continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                    File.Copy(f, dest);
                    File.Delete(f);
                }
        }
        catch (Exception e)
        {
            await Dialogs.Message(this, "Moving the saves stopped: " + e.Message + "\n\nThe saves folder is unchanged.");
            return;
        }
        var cfg = new GameConfig(configPath);
        if (target == null) cfg.Remove("paths", "saves_dir"); else cfg.SetString("paths", "saves_dir", to);
        cfg.Save();
        RefreshSaves();
        if (kept > 0)
            await Dialogs.Message(this, $"{kept} save file(s) already in {to} were kept; the old copies are still in {from}.");
    }

    // This checkout's or release's game is running (another copy of the game elsewhere doesn't count).
    bool GameRunning()
    {
        var exe = Path.GetFullPath(Path.Combine(play, GameExe));
        if (OperatingSystem.IsLinux())
        {
            foreach (var dir in Directory.EnumerateDirectories("/proc"))
            {
                if (!int.TryParse(Path.GetFileName(dir), out _)) continue;
                try { if (File.ResolveLinkTarget(Path.Combine(dir, "exe"), false)?.FullName == exe) return true; }
                catch (Exception) { }  // another user's process, or gone
            }
            return false;
        }
        foreach (var p in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(GameExe)))
        {
            try { if (string.Equals(p.MainModule?.FileName, exe, StringComparison.OrdinalIgnoreCase)) return true; }
            catch (Exception) { return true; }  // can't tell: assume it's ours
            finally { p.Dispose(); }
        }
        return false;
    }

    // ----- Updates (Updater) -----
    Control UpdateBox()
    {
        var box = new ThemedCombo();
        box.Items.Add("Look for a new version at start");
        box.Items.Add("Don't look");
        box.SelectedIndex = updater.CheckAtStart ? 0 : 1;
        box.SelectedIndexChanged += (_, _) => updater.SetCheckAtStart(box.SelectedIndex == 0);
        return new LeftCap(320, box);
    }

    void ShowUpdateState(string? text)
    {
        if (updateStatus == null) return;
        updateStatus.Text = text ?? ("You have version " + updater.CurrentVersion + (updater.Installed
            ? ". New versions download and install from the launcher; your saves and settings stay."
            : ". This is the zip: a new version is a new download (the launcher tells you when one is out)."));
    }

    // At start (manual = false: quiet when there's nothing new, the check fails or the player skipped that version) or
    // from Extras > Check now.
    async Task CheckForUpdates(bool manual)
    {
        if (updating) return;
        updating = true;
        checkNow.IsEnabled = false;
        ShowUpdateState("Looking for a new version...");
        Updater.Found? found = null;
        string? failure = null;
        try { found = await updater.CheckAsync(CancellationToken.None); }
        catch (Exception e)
        {
            failure = "Couldn't look for a new version: " + e.Message;
            ShowUpdateState(failure);
        }
        updating = false;
        if (!IsVisible) return;
        checkNow.IsEnabled = true;
        if (failure != null && manual) await Dialogs.Message(this, failure);
        if (found == null)
        {
            if (updateStatus.Text?.StartsWith("Looking") == true) ShowUpdateState(manual ? "You have the newest version, " + updater.CurrentVersion + "." : null);
            return;
        }
        ShowUpdateState($"Version {found.Version} is out (you have {updater.CurrentVersion}).");
        if (!manual && (updater.Skipped == found.Version || GameRunning())) return;  // the game has the screen: Extras says it
        while (asking) await Task.Delay(300);
        if (!IsVisible) return;
        UpdateAnswer answer;
        asking = true;
        try { answer = await AskUpdate(found); }
        finally { asking = false; }
        switch (answer)
        {
            case UpdateAnswer.Update when updater.Installed: await InstallUpdate(found); break;
            case UpdateAnswer.Update: await Launcher.LaunchUriAsync(new Uri(found.PageUrl!)); break;
            case UpdateAnswer.Skip: updater.Skip(found.Version); break;
        }
    }

    async Task InstallUpdate(Updater.Found found)
    {
        while (GameRunning())
            if (!await Dialogs.OkCancel(this, "Close the game first, then press OK to update.")) return;
        updating = true;
        checkNow.IsEnabled = false;
        void Show(string text) { if (IsVisible) { scene.SetStatus(text, false); ShowUpdateState(text); } }
        Show($"Downloading version {found.Version}...");
        try
        {
            await updater.DownloadAsync(found, p => Dispatcher.UIThread.Post(() => Show($"Downloading version {found.Version}: {p}%")),
                                        CancellationToken.None);
            Show($"Installing version {found.Version}...");
            updater.ApplyAndRestart(found);  // exits the launcher
        }
        catch (Exception e)
        {
            updating = false;
            checkNow.IsEnabled = true;
            RefreshDisc();
            ShowUpdateState("The update failed: " + e.Message);
            await Dialogs.Message(this, "The update failed: " + e.Message + "\n\nThe game is unchanged; try again with Extras > Check now.");
        }
    }

    enum UpdateAnswer { Update, Later, Skip }

    async Task<UpdateAnswer> AskUpdate(Updater.Found found)
    {
        int pick = await Dialogs.Choose(this, "A new version of FrightWorks", $"FrightWorks {found.Version} is out", new[]
        {
            $"You have version {updater.CurrentVersion}. " + (updater.Installed
                ? "Updating downloads the new version and restarts the launcher; your saves, settings and mods stay."
                : "This is the zip, which can't update itself: download the new version from its release page and unzip it over this folder."),
            "Extras > Updates turns this question off.",
        }, (updater.Installed ? "Update now" : "Open the download page", true), ("Not now", false), ("Skip this version", false));
        return pick switch { 0 => UpdateAnswer.Update, 2 => UpdateAnswer.Skip, _ => UpdateAnswer.Later };
    }

    // The hubs' holiday events ([game] holiday): the game checks the date when a hub loads. "auto" uses today's date,
    // "off" none, "MM-DD" pretends it is that day.
    static readonly (string text, string value)[] Holidays =
    {
        ("By today's date", "auto"), ("Off", "off"), ("New Year's Day", "01-01"), ("St. Patrick's Day", "03-17"),
        ("Fourth of July", "07-04"), ("Halloween", "10-31"), ("Christmas", "12-25"),
    };

    Control HolidayBox()
    {
        var box = new ThemedCombo();
        foreach (var h in Holidays) box.Items.Add(h.text);
        var current = new GameConfig(configPath).Get("game", "holiday") ?? "auto";
        int index = Array.FindIndex(Holidays, h => h.value == current);
        if (index < 0) { box.Items.Add("Custom date (" + current + ")"); index = box.Items.Count - 1; }
        box.SelectedIndex = index;
        box.SelectedIndexChanged += (_, _) =>
        {
            if (box.SelectedIndex < 0 || box.SelectedIndex >= Holidays.Length) return;
            var c = new GameConfig(configPath);
            c.SetString("game", "holiday", Holidays[box.SelectedIndex].value);
            c.Save();
        };
        return new LeftCap(320, box);
    }

    // [game] dev_tools: the F8 dev menu and the DEV build mark (the game reads it at startup).
    Control DevToolsBox()
    {
        var box = new ThemedCombo();
        box.Items.Add("Off");
        box.Items.Add("On (F8 dev menu, DEV mark)");
        box.SelectedIndex = new GameConfig(configPath).Get("game", "dev_tools") == "true" ? 1 : 0;
        box.SelectedIndexChanged += (_, _) =>
        {
            var c = new GameConfig(configPath);
            c.Set("game", "dev_tools", box.SelectedIndex == 1 ? "true" : "false");
            c.Save();
            RefreshFfmpeg(offerDownload: box.SelectedIndex == 1);
        };
        return new LeftCap(320, box);
    }

    // F3 videos need ffmpeg. Looked for only while the developer tools are on; turning them on offers the download.
    // The game records only on Windows so far (no100f_recorder.cpp): elsewhere the page just says so.
    void RefreshFfmpeg(bool offerDownload)
    {
        bool dev = new GameConfig(configPath).Get("game", "dev_tools") == "true";
        bool busy = ffmpegCancel != null;
        if (!FfmpegSetup.Supported)
        {
            ffmpegStatus.IsVisible = dev;
            ffmpegButton.IsVisible = false;
            ffmpegStatus.Text = "F3 video recording is Windows-only for now.";
            ffmpegStatus.Foreground = Brush(CreamDim);
            return;
        }
        var found = dev && !busy ? FfmpegSetup.Find(paths) : null;
        ffmpegStatus.IsVisible = dev || busy;
        ffmpegButton.IsVisible = dev && !busy && found == null;
        if (busy || !dev) return;
        ffmpegStatus.Text = found != null
            ? "F3 videos use ffmpeg from " + found + "."
            : "F3 videos need ffmpeg, which isn't installed. The launcher can download it (about 200 MB, the free " +
              "win64 build from github.com/BtbN/FFmpeg-Builds) into " + FfmpegSetup.InstallDir(paths) + ".";
        ffmpegStatus.Foreground = Brush(found != null ? Good : Warn);
        if (found == null && offerDownload) OfferFfmpeg().Watch();
    }

    async Task OfferFfmpeg()
    {
        if (await Dialogs.YesNo(this, "F3 video recording needs ffmpeg, which isn't installed.\n\nDownload it now? (about 200 MB, " +
                                      "the free win64 build from github.com/BtbN/FFmpeg-Builds, checked against its published SHA-256)"))
            await DownloadFfmpeg();
    }

    async Task DownloadFfmpeg()
    {
        if (ffmpegCancel != null) return;
        ffmpegCancel = new CancellationTokenSource();
        ffmpegButton.IsVisible = false;
        ffmpegBar.IsVisible = true;
        ffmpegBar.Maximum = 1000;
        ffmpegBar.Value = 0;
        ffmpegStatus.Foreground = Brush(CreamDim);
        ffmpegStatus.Text = "Downloading ffmpeg...";
        var report = new Progress<(long done, long total)>(p =>
        {
            if (ffmpegCancel == null) return;
            ffmpegBar.Value = p.total > 0 ? (int)(p.done * 1000 / p.total) : 0;
            ffmpegStatus.Text = p.total > 0 ? $"Downloading ffmpeg: {p.done >> 20} of {p.total >> 20} MB" : $"Downloading ffmpeg: {p.done >> 20} MB";
        });
        string? error = null;
        try { await FfmpegSetup.Download(paths, report, ffmpegCancel.Token); }
        catch (OperationCanceledException) when (ffmpegCancel.IsCancellationRequested) { return; }  // the launcher closed
        catch (Exception e) { error = e.Message; }
        finally
        {
            ffmpegCancel.Dispose();
            ffmpegCancel = null;
            ffmpegBar.IsVisible = false;
        }
        RefreshFfmpeg(offerDownload: false);
        if (error != null) await Dialogs.Message(this, "Downloading ffmpeg failed: " + error);
    }

    // ----- The disc and starting the game -----
    (bool ok, string text) DiscState()
    {
        var boot = Path.Combine(paths.PlayableDisc, "sys", "boot.bin");
        if (!File.Exists(boot)) return (false, "No game disc set up yet. Choose your Scooby-Doo! Night of 100 Frights disc image.");
        var id = System.Text.Encoding.ASCII.GetString(File.ReadAllBytes(boot), 0, 6);
        return id == GcDisc.WantedId ? (true, "Scooby-Doo! Night of 100 Frights (GIHE78) is set up.") : (false, $"The Disc folder holds {id}, not Scooby-Doo! Night of 100 Frights USA (GIHE78).");
    }

    bool CanPlay() => DiscState().ok && File.Exists(Path.Combine(play, GameExe));

    void RefreshDisc()
    {
        var (ok, text) = DiscState();
        bool exe = File.Exists(Path.Combine(play, GameExe));
        scene.SetStatus(!exe ? "Game files missing: re-extract the download" : ok ? "Ready to play" : "No game disc yet: press Play to set it up", ok && exe);
        if (discStatus == null) return;
        discStatus.Text = text;
        discStatus.Foreground = Brush(ok ? Good : Warn);
        chooseDisc.Text = ok ? "Change disc image" : "Choose disc image";
        dumpHelp.IsVisible = !ok;
    }

    async Task ChooseDisc()
    {
        var file = await ControllerDialogs.PickPath(this, "Choose your Scooby-Doo! Night of 100 Frights disc image");
        if (file == null) return;
        GcDisc disc;
        try { disc = GcDisc.Open(file); }
        catch (Exception e) { await Dialogs.Message(this, "That file can't be read: " + e.Message); return; }
        using (disc)
        {
            var problem = disc.Problem();
            if (problem.Length > 0) { await Dialogs.Message(this, problem); return; }
            var staging = discDir + ".new";
            if (Directory.Exists(staging)) Directory.Delete(staging, true);
            chooseDisc.IsEnabled = false;
            progress.IsVisible = true;
            progress.Value = 0;
            var report = new Progress<(int done, int total, string name)>(p =>
            {
                progress.Maximum = Math.Max(1, p.total);
                progress.Value = Math.Min(p.done, progress.Maximum);
                discStatus.Text = $"Extracting {p.done}/{p.total}: {p.name}";
            });
            string? error = null;
            try
            {
                await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                Directory.Move(staging, discDir);
                var cfg = new GameConfig(configPath);
                cfg.Set("paths", "dvd_root", paths.DvdRootValue);  // relative to UserData, so the folder can move
                cfg.Save();
            }
            catch (Exception e) { error = e.Message; }
            finally { progress.IsVisible = false; chooseDisc.IsEnabled = true; RefreshDisc(); }
            if (error != null) await Dialogs.Message(this, "Extraction failed: " + error);
        }
    }

    async Task Launch()
    {
        if (!enableServices) throw new InvalidOperationException("Game launch is disabled in this test fixture.");
        var exe = Path.Combine(play, GameExe);
        if (!File.Exists(exe)) { await Dialogs.Message(this, "The game files (the data folder) are missing. Re-extract the download next to this launcher."); return; }
        await Patches.BeforePlay();  // can't stop the game starting; the check below comes after it so a second press can't start two
        // One game at a time: two would share the saves and settings.
        if (game is { HasExited: false } || GameRunning()) { await Dialogs.Message(this, "The game is already running."); return; }
        gamepads.Pause();  // the game has the controllers to itself
        navigation.Reset();
        GameOwnsControllers = true;
        try { game = Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = play, UseShellExecute = false }); }
        catch (Exception e)
        {
            UpdateControllerOwnership();
            await Dialogs.Message(this, "The game couldn't start: " + e.Message);
            return;
        }
        if (game == null) { UpdateControllerOwnership(); return; }
        // The status says so too: some window managers (tiling ones) don't minimise, and the launcher stays in view.
        scene.SetStatus("The game is running", true);
        var started = game;
        started.EnableRaisingEvents = true;
        started.Exited += (_, _) => Dispatcher.UIThread.Post(() => GameExited(started).Watch());
        WindowState = WindowState.Minimized;
    }

    Process? game;  // the game this launcher started, while it runs

    // Back from the game: controllers to the launcher again, and when the game ended with an error (it writes 0 when the
    // player quits), the launcher comes back to say so instead of looking as if Play did nothing.
    async Task GameExited(Process exited)
    {
        int code;
        try { code = exited.ExitCode; } catch (InvalidOperationException) { code = 0; }
        exited.Dispose();
        if (game == exited) game = null;
        UpdateControllerOwnership();
        RefreshDisc();
        // Always back, after a clean quit too: the pad can't reach a minimized launcher (navigation ignores it).
        if (IsVisible)
        {
            if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
            Activate();
        }
        if (code == 0) return;
        var logs = Path.Combine(userData, "Logs");
        int pick = await Dialogs.Choose(this, "The game stopped", new[]
        {
            $"The game closed because of an error (exit code {code}). Its log and crash report are in the Logs folder; " +
            "they help when you report the problem.",
        }, ("Open the Logs folder", false), ("OK", true));
        if (pick == 0) await OpenFolder(logs);
    }

    // ----- Controllers -----
    // Gamepad buttons: the [controller] keys F10 > Controls edits too (PadMapping). The game reads them at start.
    void BuildPadMapping(StackPanel body)
    {
        body.Children.Add(Title("Gamepad buttons"));
        body.Children.Add(Note("The controller button that presses each GameCube button. Buttons go by position: South is A " +
                               "on Xbox, Cross on PlayStation, B on Nintendo. Used from the next game start; F10 > Controls " +
                               "in the game edits the same mapping."));
        var table = new FormTable();
        var config = new GameConfig(configPath);
        foreach (var row in PadMapping.Rows)
        {
            var label = FieldLabel(row.Label);
            label.FontWeight = FontWeight.Normal;
            label.Foreground = Brush(CreamDim);
            var box = new ThemedCombo();
            foreach (var choice in PadMapping.ChoicesFor(row)) box.Items.Add(choice);
            table.Add(label, box);
            padRows.Add((row, label, box));
            ShowPadValue(row, box, config.Get("controller", row.Key));
            box.SelectedIndexChanged += (_, _) =>
            {
                if (box.SelectedItem is not PadMapping.Choice choice) return;  // the "(custom)" entry
                var c = new GameConfig(configPath);
                string? old = c.Get("controller", row.Key);
                if (old != null && PadMapping.Split(old).Name == choice.Name) return;
                if (old == null && choice.Name == row.Default) return;  // shown as the default already
                c.SetString("controller", row.Key, PadMapping.Compose(choice.Name, old));
                c.Save();
            };
        }
        body.Children.Add(table);
        ShowXboxRows();
        var restore = new PlankButton("Restore defaults", (_, _) => RestorePadDefaults()) { Margin = new Thickness(0, 4, 0, 8) };
        body.Children.Add(restore);
    }

    // Selects the value's pad button; one the list doesn't know stays as it is, shown as "(custom)".
    static void ShowPadValue(PadMapping.Row row, ThemedCombo box, string? value)
    {
        string name = value == null ? row.Default : PadMapping.Split(value).Name;
        for (int i = box.Items.Count - 1; i >= 0; i--)
            if (box.Items[i] is string) box.Items.RemoveAt(i);
        int index = -1;
        for (int i = 0; i < box.Items.Count; i++)
            if (box.Items[i] is PadMapping.Choice c && c.Name == name) { index = i; break; }
        if (index < 0) { box.Items.Add("(custom) " + value); index = box.Items.Count - 1; }
        box.SelectedIndex = index;
    }

    // Black / White exist only with the original Xbox layout.
    void ShowXboxRows()
    {
        bool xbox = layoutBox.SelectedIndex == 1;
        foreach (var (row, label, box) in padRows)
            if (row.Xbox) { label.IsVisible = xbox; box.IsVisible = xbox; }
    }

    // Writes every GameCube button's default (so a mapping saved for one controller in F10 doesn't stay in force) and
    // drops the Black / White choices.
    void RestorePadDefaults()
    {
        var c = new GameConfig(configPath);
        foreach (var row in PadMapping.Rows)
        {
            if (row.Xbox) c.Remove("controller", row.Key);
            else c.SetString("controller", row.Key, row.Default);
        }
        c.Save();
        foreach (var (row, _, box) in padRows) ShowPadValue(row, box, c.Get("controller", row.Key));
    }

    void SaveLayout(bool original) => SaveLayout(original ? "xbox_original" : "gamecube");

    void SaveLayout(string layout)
    {
        var c = new GameConfig(configPath);
        if (ControllerLayout.SetManual(c, layout)) c.Save();
        ShowLayout(c);
    }

    internal async Task RefreshGamepads()
    {
        if (!enableServices)
        {
            padStatus.Text = "Controller services are disabled in this test fixture.";
            return;
        }
        UpdateControllerOwnership();
        if (GameOwnsControllers)
        {
            padStatus.Text = "The game is running; controller detection is paused.";
            return;
        }
        var pads = gamepads.Connected;
        padStatus.Text = !gamepads.Available ? "Controller input is not available: " + gamepads.Error
            : pads.Count == 0 ? "No controller connected."
            : "Connected: " + string.Join(", ", pads.Select(p => $"{p.Name} ({Gamepads.Describe(p.Kind)})"));
        if (startupReady && IsActive && navigation.Modal == null && !asking) await ApplyControllerLayout(pads);
    }

    void UpdateControllerOwnership()
    {
        GameOwnsControllers = game is { HasExited: false } || GameRunning();
        if (GameOwnsControllers) { gamepads.Pause(); navigation.Reset(); }
        else if (enableServices) gamepads.Start();
    }

    internal void CompleteStartup()
    {
        foreach (var board in boards.Values) stage.Children.Remove(board);
        boards.Clear();
        padRows.Clear();
        AddBoard("settings", "Settings", BuildSettings);
        AddBoard("mods", "Mods", BuildMods);
        AddBoard("extras", "Extras", BuildExtras);
        RefreshDisc();
        startupReady = true;
        if (enableServices && updater.CanCheck && updater.CheckAtStart) CheckForUpdates(manual: false).Watch();
    }

    void ShowLayout(GameConfig config)
    {
        fillingLayout = true;
        try
        {
            while (layoutBox.Items.Count > 2) layoutBox.Items.RemoveAt(2);
            var layout = config.Get("controller", "layout");
            int index = layout switch { "xbox_original" => 1, null or "gamecube" => 0, _ => 2 };
            if (index == 2) layoutBox.Items.Add(ControllerLayout.Describe(layout));
            layoutBox.SelectedIndex = index;
            if (autoLayout != null) autoLayout.IsChecked = ControllerLayout.Automatic(config);
            ShowXboxRows();
        }
        finally { fillingLayout = false; }
    }

    internal async Task ApplyControllerLayout(IReadOnlyList<Gamepads.Pad> pads)
    {
        if (GameOwnsControllers) return;
        var config = new GameConfig(configPath);
        if (ControllerLayout.NeedsConsent(config))
        {
            if (askedLayout || ControllerLayout.Match(pads) == null) return;
            askedLayout = true;
            asking = true;
            int pick;
            try
            {
                pick = await Dialogs.Choose(this, "Controller layout", new[]
                {
                    "Your saved layout predates automatic controller matching. Enable matching until you choose a layout manually, or keep your deliberate preference?",
                    "Xbox controllers match the existing Original Xbox layout; GameCube adapters match GameCube. Unsupported or conflicting types do not change it. Custom bindings are never rewritten.",
                }, ("Enable automatic matching", false), ("Keep my layout", true));
            }
            finally { asking = false; }
            if (pick < 0) return;
            config = new GameConfig(configPath);
            ControllerLayout.SetAutomatic(config, pick == 0);
            config.Save();
        }
        if (GameOwnsControllers) return;
        if (ControllerLayout.ApplyDetected(config, pads)) config.Save();
        ShowLayout(config);
    }
}
