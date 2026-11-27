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
// Mods and Extras open a full-window menu laid out like the game's F10 menu (GameMenu.cs). Everything it changes goes
// to Config.toml (DataPaths), the keys the game's F10 menu uses. No game data or character art is shipped.
sealed class MainWindow : Window
{
    internal const string AppTitle = "FrightWorks: Night of 100 Frights";
    static readonly string GameExe = DataPaths.GameExe;
    const string ProjectUrl = "https://github.com/FrightWorks/FrightWorks-no100f";

    internal readonly DataPaths paths;
    readonly string root, play, userData, configPath, modsDir, discDir;
    readonly string? shotDir;
    readonly bool enableServices;
    // A test fixture (services off) that starts a stand-in game: only Play's own check sets it.
    internal bool launchInFixture;
    internal bool GameOwnsControllers { get; private set; }
    internal readonly MansionScene scene;
    internal readonly Dictionary<string, MenuScreen> boards = new();
    readonly Stage stage;
    readonly PromptBar mainPrompts;  // the main screen's button prompts (hidden under an open menu)
    TextBlock discStatus = null!, dumpHelp = null!;
    ActionRow chooseDisc = null!;
    ThemedBar progress = null!, ffmpegBar = null!;
    TextBlock ps2Status = null!;
    ActionRow choosePs2 = null!, removePs2 = null!;
    ThemedBar ps2Bar = null!;
    TextBlock ffmpegStatus = null!, savesNote = null!, updateStatus = null!;
    ActionRow savesDefault = null!, checkNow = null!;
    readonly Updater updater;
    bool updating;  // checking, or downloading / applying an update: Play waits
    bool asking;    // a question (Xbox layout, update) is open: the other waits, so one never disables the other
    ActionRow ffmpegButton = null!;
    CancellationTokenSource? ffmpegCancel;  // an ffmpeg download is running
    internal Action<bool>? showControlsTab;
    readonly Gamepads gamepads;
    internal readonly ControllerNavigation navigation;
    internal TextBlock padStatus = null!;
    internal ChoiceRow layoutBox = null!;
    readonly List<(PadMapping.Row Row, ChoiceRow Box)> padRows = new();
    bool askedLayout;
    string hdrStatus = "Saved HDR settings are kept. Windows defaults are used only for settings not saved yet.";
    bool fillingLayout;
    bool startupReady;
    internal ToggleRow autoLayout = null!;
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
        Prompts.SetFolder(play);
        mainPrompts = new PromptBar
        {
            Centred = true,
            Items = new[]
            {
                new PromptBar.Item(new[] { Prompts.Button.Move }, "Move", null),
                new PromptBar.Item(new[] { Prompts.Button.Confirm }, "Select", () => scene.Navigate(LauncherButtons.Confirm)),
            },
        };
        stage = new Stage(scene, mainPrompts);
        Content = stage;

        AddBoard("settings", "Settings", BuildSettings);
        AddBoard("mods", "Mods", BuildMods);
        AddBoard("extras", "Extras", BuildExtras);
        RefreshDisc();

        // The prompts show the keys once the keyboard is used (the menus mark their own keys too).
        AddHandler(KeyDownEvent, (_, _) => Prompts.UsedKeyboard(), RoutingStrategies.Tunnel);
        // Escape closes an open menu (the menu takes it first; this is for a key it doesn't see).
        KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape && boards.Values.Any(b => b.IsVisible)) { CloseBoards(); e.Handled = true; }
        };
        // Connected controllers: shown on Settings > Controls; an Xbox pad asks for the layout once.
        padTimer.Tick += (_, _) => RefreshGamepads().Watch();
        Closed += (_, _) => { padTimer.Stop(); navigation.Dispose(); gamepads.Dispose(); ffmpegCancel?.Cancel(); packCancel?.Cancel(); };
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

    // The scene fills the window, with the main screen's button prompts at the bottom in the middle; an open menu covers
    // the whole window.
    sealed class Stage : Panel
    {
        readonly MansionScene scene;
        readonly PromptBar prompts;

        public Stage(MansionScene scene, PromptBar prompts)
        {
            this.scene = scene;
            this.prompts = prompts;
            Children.Add(scene);
            Children.Add(prompts);
        }

        protected override Size MeasureOverride(Size availableSize)
        {
            foreach (var child in Children) child.Measure(availableSize);
            return availableSize;
        }

        protected override Size ArrangeOverride(Size finalSize)
        {
            foreach (var child in Children)
            {
                if (child == prompts)
                {
                    // On the line of the build ID and status pills (MansionScene draws those 30 px from the bottom).
                    double w = Math.Min(prompts.DesiredSize.Width, finalSize.Width);
                    child.Arrange(new Rect(Math.Round((finalSize.Width - w) / 2), finalSize.Height - 34, w, prompts.DesiredSize.Height));
                }
                else child.Arrange(new Rect(finalSize));
            }
            return finalSize;
        }
    }

    // The open menu, if any.
    internal MenuScreen? OpenMenu => boards.Values.FirstOrDefault(b => b.IsVisible);

    // The connected controller's family, for the button prompts (the first one when there are several).
    internal Gamepads.Kind? PadKind => gamepads.Connected.FirstOrDefault()?.Kind;

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
        foreach (var (id, tab, name) in new[] { ("settings", 0, "Settings"), ("settings", 1, "Settings-Graphics"), ("settings", 2, "Settings-Controls"),
                     ("mods", 0, "Mods"), ("extras", 0, "Extras"), ("extras", 1, "Extras-Textures"), ("extras", 2, "Extras-Game"),
                     ("extras", 3, "Extras-Folders"), ("extras", 4, "Extras-About") })
        {
            boards[id].SelectTab(tab);
            ShowBoard(id);
            Shot(name);
        }
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
                else
                {
                    boards["extras"].SelectTab(0);  // the disc
                    ShowBoard("extras");
                }
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

    void AddBoard(string id, string title, Action<MenuScreen> build)
    {
        var board = new MenuScreen(title, CloseBoards);
        build(board);
        boards[id] = board;
        stage.Children.Add(board);
    }

    internal void ShowBoard(string id)
    {
        RefreshDisc();
        if (id == "extras") refreshPacks?.Invoke();  // packs added or removed by hand in the folder meanwhile
        foreach (var (key, b) in boards) b.IsVisible = key == id;
        mainPrompts.IsVisible = false;
        scene.ActiveSign = id;
        UpdateLayout();
        boards[id].SelectFirst();
        navigation.Reset();
    }

    internal void CloseBoards()
    {
        foreach (var b in boards.Values) b.IsVisible = false;
        mainPrompts.IsVisible = true;
        scene.ActiveSign = null;
        scene.Focus(NavigationMethod.Directional);
        navigation.Reset();
    }

    static WrapPanel Row() => new() { Orientation = Orientation.Horizontal };


    async Task OpenFolder(string path)
    {
        try
        {
            var dir = Directory.CreateDirectory(path);
            if (!await Launcher.LaunchDirectoryInfoAsync(dir)) throw new IOException("no file manager answered");
        }
        catch (Exception e) { await Dialogs.Message(this, "Couldn't open " + path + ": " + e.Message); }
    }

    // ----- Settings: Display, Graphics and Controls -----
    void BuildSettings(MenuScreen board)
    {
        var display = MenuScreen.NewPage();
        var graphics = MenuScreen.NewPage();
        var controls = MenuScreen.NewPage();
        BuildDisplay(display, graphics);
        BuildControls(controls);
        board.AddTab("Display", display);
        board.AddTab("Graphics", graphics);
        board.AddTab("Controls", controls);
        showControlsTab = showControls => board.SelectTab(showControls ? 2 : 0);
    }

    // A heading at the end of the page; the rows after it go on the page too (returned, to add them to).
    static StackPanel Section(StackPanel page, string title, string? hint = null)
    {
        var heading = new MenuHeading(title, hint);
        if (page.Children.Count == 0) heading.Margin = new Thickness(0, 0, 0, 2);
        page.Children.Add(heading);
        return page;
    }

    // Text in a menu page (a status, a note), lined up with the rows' names.
    static T Indent<T>(T text) where T : Control
    {
        text.Margin = new Thickness(22, 2, 14, 6);
        return text;
    }

    static ThemedBar Bar() => new() { IsVisible = false, MaxWidth = 480, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(22, 4, 14, 8), Width = 420 };

    const string SavedNote = "Changes save at once and apply the next time the game starts. In the game, F10 has these settings and more.";

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

    static bool Fps60(GameConfig c) => c.Get("video", "fps60")?.Trim() == "true";

    // [video] frame_interpolation_fps as the game reads it (RuntimeConfigFile::FrameInterpolationFps): 0, 120 or 180 (240
    // from older versions = 180), else off, and always off at 30 fps.
    static int InterpolationFps(GameConfig c) => !Fps60(c) ? 0 : c.Get("video", "frame_interpolation_fps")?.Trim() switch
    {
        "120" => 120, "180" or "240" => 180, _ => 0,
    };

    // The game picks the fullscreen monitor itself on Windows only ([video] fullscreen_display); elsewhere the row is left out.
    internal static bool MonitorChoice => OperatingSystem.IsWindows();

    // Display: the screen and picture shape. Graphics: textures, anti-aliasing and the experimental effects, in the same
    // order as F10 > Video > Experimental.
    void BuildDisplay(StackPanel display, StackPanel graphics)
    {
        var cfg = new GameConfig(configPath);
        var card = Section(display, "Screen");  // Row adds to this one
        bool filling = false;  // a box is being set from the config, not by the player
        ChoiceRow Row(string label, string? hint, string key, (string text, string raw)[] options, string fallback, Action<GameConfig>? changed = null)
        {
            var box = new ChoiceRow(label, hint);
            foreach (var o in options) box.Items.Add(o.text);
            var current = cfg.Get("video", key) ?? fallback;
            int index = Array.FindIndex(options, o => SameValue(o.raw, current));
            box.SelectedIndex = index >= 0 ? index : Array.FindIndex(options, o => SameValue(o.raw, fallback));
            box.SelectedIndexChanged += (_, _) =>
            {
                if (filling || box.SelectedIndex < 0) return;
                var c = new GameConfig(configPath);
                c.Set("video", key, options[box.SelectedIndex].raw);
                changed?.Invoke(c);
                c.Save();
            };
            card.Children.Add(box);
            return box;
        }
        Row("Window", "How the game opens: borderless fullscreen, in a window, or exclusive fullscreen. F11 switches in the game.",
            "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Window", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }, "borderless");

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
            Row("Fullscreen monitor", "The monitor the game goes fullscreen on. Also used when F11 goes fullscreen.", "fullscreen_display", monitorOptions.ToArray(), "auto");
        }

        // Resolution: refilled when frame interpolation crosses 60 fps. A missing or unknown value is the game's 1080p.
        var resolutionBox = new ChoiceRow("Resolution", "How sharply the game is drawn: higher is sharper, and harder on the graphics card. " +
                                                        "Auto matches the window or screen.");
        var shownResolutions = Resolutions;
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
            resolutionBox.Refresh();
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
        card.Children.Add(resolutionBox);

        // Without aspect_mode the game goes by the older keys (main.cpp): force_16_9 = 16:9, widescreen = false = 4:3,
        // else fill the screen.
        string aspectFallback = cfg.Get("video", "force_16_9") == "true" ? "1" : cfg.Get("video", "widescreen") == "false" ? "0" : "2";
        Row("Picture shape", "The shape of the picture: the original 4:3, widescreen 16:9, the whole screen, or cinematic 2.39:1.",
            "aspect_mode", new[] { ("Original 4:3", "0"), ("Widescreen 16:9", "1"), ("Fill the screen", "2"), ("Cinematic 2.39:1", "3") }, aspectFallback);
        // The Frame interpolation row (Graphics): Off and locked at 30 fps. Changing the frame rate saves it as off in both
        // directions, so what comes back at 60 fps is the Off the player was just shown, not an old choice that would
        // also drop the resolution to 4x.
        (string text, string raw)[] interpolationModes = { ("Off", "0"), ("120 FPS", "120"), ("180 FPS", "180") };
        ChoiceRow? interpolationBox = null;
        void ShowInterpolation(GameConfig c)
        {
            filling = true;
            interpolationBox!.SelectedIndex = Array.FindIndex(interpolationModes, m => m.raw == InterpolationFps(c).ToString());
            filling = false;
            interpolationBox.IsEnabled = Fps60(c);
        }
        Row("Frame rate", "30 fps is the game's original speed; 60 fps is smoother. Frame interpolation (Graphics) needs 60 fps.",
            "fps60", new[] { ("30 fps (original)", "false"), ("60 fps", "true") }, "false", c =>
        {
            c.Set("video", "frame_interpolation_fps", "0");
            FillResolutions(c);
            ShowInterpolation(c);
        });
        card.Children.Add(new ActionRow("HDR", () => OpenHdr().Watch(), "Adjust", "Brightness on an HDR screen: opens the HDR adjustment."));

        card = Section(display, "Startup");
        Row("Intro movies", "Whether the intro movies play when the game starts.", "skip_intro", new[] { ("Play them", "false"), ("Skip them", "true") }, "false");
        display.Children.Add(Indent(Footnote(SavedNote)));

        card = Section(graphics, "Picture");
        Row("HD textures", "Sharper textures from a texture pack (Extras > Textures).", "texture_replacements", new[] { ("On", "true"), ("Off (original textures)", "false") }, "true");
        Row("Anti-aliasing", "Smooths jagged edges.", "msaa", new[] { ("Off", "1"), ("2x MSAA", "2"), ("4x MSAA", "4"), ("8x MSAA", "8") }, "1");

        // Going above 60 fps at 6x or 8x drops the resolution to 4x, as F10 does.
        card = Section(graphics, "Experimental", "These can show visual glitches.");
        interpolationBox = Row("Frame interpolation", "Extra in-between frames. Needs 60 fps; caps the resolution at 4x.",
            "frame_interpolation_fps", interpolationModes, InterpolationFps(cfg).ToString(), c =>
            {
                if (InterpolationFps(c) > 60 && AboveCap(c.Get("video", "resolution_multiplier") ?? "")) c.Set("video", "resolution_multiplier", "4");
                FillResolutions(c);
            });
        ShowInterpolation(cfg);
        // Strengths in percent, as in F10 (runtime_config.h kMotionBlurLevels, kAmbientOcclusionLevels, kBloomLevels); a
        // strength set by hand in Config.toml that is none of the levels is kept as its own entry.
        (string text, string raw)[] Levels(string key, params (string text, string raw)[] levels)
        {
            var list = levels.ToList();
            if (int.TryParse(cfg.Get("video", key)?.Trim(), out int saved) && saved > 0 && saved <= 100 && !list.Exists(l => l.raw == saved.ToString()))
            {
                list.Add(($"{saved}%", saved.ToString()));
                list.Sort((a, b) => int.Parse(a.raw).CompareTo(int.Parse(b.raw)));
            }
            return list.ToArray();
        }
        Row("Motion blur", "Blurs fast movement.", "motion_blur",
            Levels("motion_blur", ("Off", "0"), ("Extra low", "10"), ("Low", "20"), ("Medium", "30"), ("High", "40"), ("Extra high", "50")), "0");
        Row("Ink outlines", "Cartoon outlines around characters and things.", "ink_outlines", new[] { ("Off", "0"), ("Thin", "1"), ("Bold", "2") }, "0");
        Row("Ambient occlusion", "Soft shade in corners and creases.", "ambient_occlusion",
            Levels("ambient_occlusion", ("Off", "0"), ("Low", "35"), ("Medium", "65")), "0");
        Row("Bloom", "A glow around lamps, fire and sparkles.", "bloom",
            Levels("bloom", ("Off", "0"), ("Low", "40"), ("Medium", "70"), ("High", "100")), "0");
        graphics.Children.Add(Indent(Footnote(SavedNote)));
    }

    internal HdrWindow CreateHdrWindow() => new(configPath, hdrStatus, this);

    async Task OpenHdr()
    {
        var window = CreateHdrWindow();
        await Dialogs.ShowModal(window, this);
    }

    // Import must finish before this can create Config.toml; construction stays read-only.
    internal Task InitializeHdrDefaults() => InitializeHdrDefaults(OperatingSystem.IsWindows(),
        () => WindowsHdr.Read(HdrSettings.TargetDisplay(new GameConfig(configPath))));

    // Headless fixtures supply platform state and metadata without querying a real display.
    internal async Task InitializeHdrDefaults(bool windows, Func<WindowsHdr.Snapshot> readDefaults)
    {
        if (!windows)
        {
            hdrStatus = "Windows HDR defaults are unavailable on this platform. Saved targets are kept.";
            return;
        }
        // Nothing here may stop the launcher from starting: a query that throws counts as a failed query (the
        // same safe defaults and status as one that returns a failure), and a config that can't be read stays as it is.
        try
        {
            var config = new GameConfig(configPath);
            if (!HdrSettings.NeedsDefaults(config)) return;
            WindowsHdr.Snapshot defaults;
            try { defaults = readDefaults(); }
            catch (Exception e)
            {
                defaults = new(null, null, null, $"Windows HDR query failed ({e.GetType().Name}); using safe HDR defaults.");
            }
            config = new GameConfig(configPath);
            HdrSettings.InitializeMissing(config, defaults);
            hdrStatus = defaults.Status + " Saved adjustments are kept.";
            try { config.Save(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                hdrStatus = "Couldn't save initial HDR defaults: " + e.Message;
                await Dialogs.Message(this, hdrStatus);
            }
        }
        catch (Exception e)
        {
            hdrStatus = $"Couldn't set up the initial HDR defaults ({e.GetType().Name}); saved settings are unchanged.";
        }
    }

    void BuildControls(StackPanel page)
    {
        var card = Section(page, "Controller");
        padStatus = Indent(Hint("Looking for controllers..."));
        card.Children.Add(padStatus);
        // [controller] layout: "gamecube" or "xbox_original" (the game's original Xbox controls, hle/no100f_xbox_layout.h).
        layoutBox = new ChoiceRow("Layout", "Which version's controls the game uses. Original Xbox uses the buttons of the game's Xbox version.");
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
        card.Children.Add(layoutBox);
        // Xbox (including Xbox One) matches Original Xbox, GameCube matches GameCube; other or conflicting controller types
        // leave the layout as it is. Choosing a layout by hand, even the current one, stops it; bindings are kept.
        autoLayout = new ToggleRow("Match my controller", "Picks the layout for an Xbox or GameCube controller until you choose one yourself.")
        {
            IsChecked = ControllerLayout.Automatic(config),
        };
        autoLayout.IsCheckedChanged += (_, _) =>
        {
            if (fillingLayout) return;
            var c = new GameConfig(configPath);
            ControllerLayout.SetAutomatic(c, autoLayout.IsChecked == true);
            c.Save();
            ApplyControllerLayout(gamepads.Connected).Watch();
        };
        card.Children.Add(autoLayout);
        BuildPadMapping(page);
        card = Section(page, "Keyboard");
        foreach (var (what, key) in new[] { ("Move", "W A S D"), ("Camera (C-stick)", "Arrow keys"), ("A (jump)", "Space"), ("B", "Left Shift"),
                     ("X / Y", "R / F"), ("Z", "C"), ("L / R", "Q / E"), ("Start (pause)", "Enter"), ("D-pad", "I J K L") })
            card.Children.Add(new InfoRow(what, key));
        page.Children.Add(Indent(Footnote("In the game: F10 settings  ·  F11 fullscreen  ·  F2 screenshot  ·  F1 project page")));
    }

    // ----- Mods -----
    void BuildMods(MenuScreen board)
    {
        var body = board.Body;
        var sep = Path.DirectorySeparatorChar;
        var patches = Patches;
        var list = new StackPanel { Orientation = Orientation.Vertical };
        string modHelp = $"A mod is a folder with files{sep} (replaces game files), textures{sep} or a patch.json (adds to levels from your disc). " +
                         "When two mods change the same file, the later name wins.";
        void Fill()
        {
            // The list is rebuilt after every switch: keep the selection on the same mod, or it would be left on nothing.
            var keep = board.Selected is ToggleRow { Parent: { } p } kept && ReferenceEquals(p, list) ? kept.Label : null;
            ToggleRow? reselect = null;
            list.Children.Clear();
            var dirs = Directory.Exists(modsDir) ? Directory.GetDirectories(modsDir).Where(ModPatches.ModPatchPanel.IsListed).OrderBy(d => d, StringComparer.OrdinalIgnoreCase).ToArray() : Array.Empty<string>();
            if (dirs.Length == 0) { list.Children.Add(Indent(Hint("No mods yet. Add one from a zip, or put its folder in the mods folder."))); return; }
            foreach (var d in dirs)
            {
                var status = patches.StatusText(d);
                var box = new ToggleRow(Path.GetFileName(d), (status != null ? status + "\n\n" : "") + "Switch a mod off to leave it out without removing it.")
                {
                    IsChecked = !File.Exists(Path.Combine(d, "disabled")),
                };
                var dir = d;
                box.IsCheckedChanged += (_, _) =>
                {
                    var flag = Path.Combine(dir, "disabled");
                    if (box.IsChecked == true) File.Delete(flag); else File.WriteAllText(flag, "");
                    patches.Update(Fill).Watch();
                };
                list.Children.Add(box);
                if (keep != null && Path.GetFileName(d) == keep) reselect = box;
                if (status != null) list.Children.Add(Indent(Note(status)));
            }
            if (reselect != null && board.IsVisible) Dispatcher.UIThread.Post(() => board.Select(reselect), DispatcherPriority.Loaded);
        }
        Section(body, "Add mods");
        body.Children.Add(new ActionRow("Add a mod (zip)", () => patches.AddFromZip(Fill).Watch(), "Choose", "Choose a mod's zip file to add it."));
        body.Children.Add(new ActionRow("Open the mods folder", () => OpenFolder(modsDir).Watch(), "Open", modsDir));
        body.Children.Add(new ActionRow("Refresh", () => { Fill(); patches.Update(Fill).Watch(); }, "Refresh", "Looks at the mods folder again."));
        Section(body, "Installed mods");
        body.Children.Add(list);
        body.Children.Add(Indent(Footnote(modHelp)));
        Fill();
        patches.Update(Fill).Watch();
    }

    ModPatches.ModPatchPanel? patchPanel;
    ModPatches.ModPatchPanel Patches => patchPanel ??= new ModPatches.ModPatchPanel(this, paths, GameRunning);

    // ----- Extras: discs, game options, folders, about -----
    void BuildExtras(MenuScreen board)
    {
        var discs = MenuScreen.NewPage();
        var textures = MenuScreen.NewPage();
        var game = MenuScreen.NewPage();
        var folders = MenuScreen.NewPage();
        var about = MenuScreen.NewPage();
        board.AddTab("Discs", discs);
        board.AddTab("Textures", textures);
        board.AddTab("Game", game);
        board.AddTab("Folders", folders);
        board.AddTab("About", about);

        // Discs
        const string discHelp = "Your own USA disc (GIHE78, version 1.01), dumped as .iso or .ciso, for example with CleanRip on a Wii. " +
                                "The game files are copied once; the image isn't needed afterwards.";
        var card = Section(discs, "GameCube disc");
        discStatus = Indent(Status());
        chooseDisc = new ActionRow("Choose disc image", () => ChooseDisc().Watch(), "Choose", discHelp);
        progress = Bar();
        dumpHelp = Indent(Hint(discHelp));
        card.Children.Add(discStatus);
        card.Children.Add(chooseDisc);
        card.Children.Add(progress);
        card.Children.Add(dumpHelp);

        card = Section(discs, "PS2 and Xbox extras", "Optional.");
        const string ps2Help = "Plays the story scenes in the game itself instead of as movies. An Xbox disc also brings the ending scene, its " +
                               "textures (used where a texture pack has none) and its sound without the GameCube's hiss. About 240 MB from a PS2 " +
                               "disc or 800 MB from an Xbox disc; the image isn't needed afterwards.";
        ps2Status = Indent(Status());
        choosePs2 = new ActionRow("Choose PS2 or Xbox disc image", () => ChoosePs2Disc().Watch(), "Choose", ps2Help);
        removePs2 = new ActionRow("Remove the extras", () => RemovePs2Cutscenes().Watch(), "Remove",
            "Removes the cutscenes, textures and sound taken from the PS2 or Xbox disc. The game goes back to the GameCube's movies.");
        ps2Bar = Bar();
        card.Children.Add(ps2Status);
        card.Children.Add(choosePs2);
        card.Children.Add(removePs2);
        card.Children.Add(ps2Bar);
        RefreshPs2();

        // Textures
        BuildTexturePacks(board, textures);

        // Game
        card = Section(game, "Holidays");
        card.Children.Add(HolidayBox());
        card = Section(game, "Developer tools");
        card.Children.Add(DevToolsBox());
        ffmpegStatus = Indent(Hint(""));
        ffmpegButton = new ActionRow("Download ffmpeg", () => DownloadFfmpeg().Watch(), "Download",
            "F3 videos need ffmpeg: about 200 MB, the free win64 build from github.com/BtbN/FFmpeg-Builds, checked against its published SHA-256.");
        ffmpegBar = Bar();
        card.Children.Add(ffmpegStatus);
        card.Children.Add(ffmpegButton);
        card.Children.Add(ffmpegBar);
        RefreshFfmpeg(offerDownload: false);

        // Folders
        card = Section(folders, "Open a folder");
        foreach (var (label, path) in new[] { ("Saves", paths.Saves), ("Settings and mods", userData), ("Screenshots", paths.Screenshots),
                     ("Videos", paths.Videos), ("Game", play) })
            card.Children.Add(new ActionRow(label, () => OpenFolder(path).Watch(), "Open", path));
        card = Section(folders, "Saves");
        savesNote = Indent(Hint(""));
        card.Children.Add(savesNote);
        card.Children.Add(new ActionRow("Change the saves folder", () => ChooseSavesFolder().Watch(), "Choose",
            "Keep your saves in another folder. The launcher offers to move the saves already there."));
        savesDefault = new ActionRow("Back to the usual folder", () => MoveSaves(null).Watch(), "Move", "Puts the saves folder back where the game keeps it.");
        card.Children.Add(savesDefault);
        async Task CopyEarlier()
        {
            await DataImport.FromFolder(this, paths, AppTitle);
            RefreshDisc();
            RefreshSaves();
        }
        card.Children.Add(new ActionRow("Copy from an earlier version", () => CopyEarlier().Watch(), "Choose",
            "Copies the saves, settings and disc from the folder of an earlier FrightWorks copy."));
        RefreshSaves();

        // About
        card = Section(about, "Updates");
        updateStatus = Indent(Hint(""));
        if (updater.CanCheck)
        {
            card.Children.Add(UpdateBox());
            checkNow = new ActionRow("Check now", () => CheckForUpdates(manual: true).Watch(), "Check", "Looks for a new version now.");
            card.Children.Add(checkNow);
            card.Children.Add(updateStatus);
            ShowUpdateState(null);
        }
        else
        {
            updateStatus.Text = paths.DevTree ? "A source build doesn't look for updates."
                                              : "This copy wasn't installed with the FrightWorks installer, so it can't look for updates.";
            card.Children.Add(updateStatus);
        }

        card = Section(about, "About");
        var version = typeof(MainWindow).Assembly.GetName().Version;
        var name = Indent(Status());
        name.Text = "FrightWorks" + (version == null ? "" : " v" + version.ToString(3));
        name.Foreground = Brush(Cream);
        card.Children.Add(name);
        card.Children.Add(Indent(Hint(
            "By Loneranger419 with the help of BitsofBitsofBits and Junkaholic55, based on Scooby-Doo: Refrightened by JosiahG. Built on WiiCompiled (GPLv3) " +
            "and Aurora (MIT). Scooby-HD texture pack (optional download) by dogerki. Button prompts by Kenney (CC0). Lettering in " +
            "Luckiest Guy by Astigmatic and in-game menu text in Roboto by Christian Robertson (both Apache 2.0)" +
            "; the launcher bundles Inter by Rasmus Andersson (SIL Open Font License), which it uses for text on Linux. " +
            "Scooby-Doo is a trademark of Hanna-Barbera / Warner Bros.; this is an unofficial fan project.")));
        card.Children.Add(new ActionRow("Project page", () => Launcher.LaunchUriAsync(new Uri(ProjectUrl)).Watch(), "Open", ProjectUrl));
    }

    // ----- The saves folder ([paths] saves_dir) -----
    void RefreshSaves()
    {
        bool custom = !string.IsNullOrWhiteSpace(new GameConfig(configPath).Get("paths", "saves_dir"));
        savesNote.Text = "Saves: " + paths.Saves + (custom ? " (your choice)" : "") + "\nSettings and mods: " + userData;
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
        var box = new ChoiceRow("Look for updates", "Whether the launcher looks for a new version each time it starts.");
        box.Items.Add("When the launcher starts");
        box.Items.Add("Never");
        box.SelectedIndex = updater.CheckAtStart ? 0 : 1;
        box.SelectedIndexChanged += (_, _) => updater.SetCheckAtStart(box.SelectedIndex == 0);
        return box;
    }

    void ShowUpdateState(string? text)
    {
        if (updateStatus == null) return;
        updateStatus.Text = text ?? ("You have version " + updater.CurrentVersion + (updater.Installed
            ? ". Updates install from here and keep your saves and settings."
            : ", the zip: a new version is a new download (the launcher tells you when one is out)."));
    }

    // At start (manual = false: quiet when there's nothing new, the check fails or the player skipped that version) or
    // from Extras > About > Check now.
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
            await Dialogs.Message(this, "The update failed: " + e.Message + "\n\nThe game is unchanged; try again with Extras > About > Check now.");
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
            "Extras > About turns this question off.",
        }, (updater.Installed ? "Update now" : "Open the download page", true), ("Not now", false), ("Skip this version", false));
        return pick switch { 0 => UpdateAnswer.Update, 2 => UpdateAnswer.Skip, _ => UpdateAnswer.Later };
    }

    // The hubs' holiday events ([game] holiday): the game checks the date when a hub loads. "auto" uses today's date,
    // "off" none, "confetti" fireworks with the colours mixed through each burst, "MM-DD" pretends it is that day.
    static readonly (string text, string value)[] Holidays =
    {
        ("By today's date", "auto"), ("Off", "off"), ("New Year's Day", "01-01"), ("St. Patrick's Day", "03-17"),
        ("Fourth of July", "07-04"), ("Halloween", "10-31"), ("Christmas", "12-25"), ("Confetti fireworks", "confetti"),
    };

    Control HolidayBox()
    {
        var box = new ChoiceRow("Holiday events", "Fireworks and decorations outside Mystic Manor and in the playground, on their real dates, " +
                                                  "or on the day you pick here.");
        foreach (var h in Holidays) box.Items.Add(h.text);
        var current = new GameConfig(configPath).Get("game", "holiday") ?? "auto";
        int index = Array.FindIndex(Holidays, h => h.value == current);
        if (index < 0) { box.Items.Add("Custom date (" + current + ")"); index = box.Items.Count - 1; }
        box.SelectedIndex = index;
        box.Refresh();
        box.SelectedIndexChanged += (_, _) =>
        {
            if (box.SelectedIndex < 0 || box.SelectedIndex >= Holidays.Length) return;
            var c = new GameConfig(configPath);
            c.SetString("game", "holiday", Holidays[box.SelectedIndex].value);
            c.Save();
        };
        return box;
    }

    // [game] dev_tools: the F8 dev menu and the DEV build mark (the game reads it at startup).
    Control DevToolsBox()
    {
        var box = new ToggleRow("Developer tools", "The F8 dev menu, F3 video recording and a DEV mark on the screen. From the next game start.")
        {
            IsChecked = new GameConfig(configPath).Get("game", "dev_tools") == "true",
        };
        box.IsCheckedChanged += (_, _) =>
        {
            var c = new GameConfig(configPath);
            c.Set("game", "dev_tools", box.IsChecked == true ? "true" : "false");
            c.Save();
            RefreshFfmpeg(offerDownload: box.IsChecked == true);
        };
        return box;
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
            : "F3 videos need ffmpeg (about 200 MB, the free win64 build from github.com/BtbN/FFmpeg-Builds).";
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

    // ----- Texture packs (TexturePacks.cs): every folder in texture_packs in the game's order (the first wins), each with a
    // switch, movable to change the order; below them the catalogue's packs not downloaded yet. One download at a time. -----
    CancellationTokenSource? packCancel;
    TexturePacks.Pack? packDownloading;
    string packProgress = "";
    ActionRow? packDownloadRow;
    readonly ThemedBar packBar = Bar();
    Action? refreshPacks;

    void BuildTexturePacks(MenuScreen board, StackPanel page)
    {
        var list = new StackPanel();
        var downloads = new StackPanel();
        var packsDir = TexturePacks.Dir(userData);
        packBar.Maximum = 1000;
        Section(page, "Texture packs", "Higher in the list wins.");
        page.Children.Add(list);
        Section(page, "Downloads");
        page.Children.Add(downloads);
        Section(page, "Your own packs");
        page.Children.Add(new ActionRow("Open the texture packs folder", () => OpenFolder(packsDir).Watch(), "Open",
            "Put a pack's folder in here (its PNG or DDS textures anywhere inside it) and it joins the list above. " + packsDir));
        page.Children.Add(new ActionRow("Refresh", () => Fill(), "Refresh", "Looks at the texture packs folder again."));
        page.Children.Add(Indent(Footnote("Your texture_replacements folder and mods win over every pack; the Xbox textures (Extras > Discs) " +
                                          "come after them. Settings > HD textures turns them all off. To keep this order the launcher numbers " +
                                          "the pack folders (10-, 20-, ...). Changes apply at the next game start.")));

        string Describe(TexturePacks.Installed pack, bool on) =>
            (pack.Known != null ? $"{pack.Known.About} By {pack.Known.Credit}." : $"Added by you: the folder {pack.Folder} in the texture packs folder. To remove it, delete that folder.") +
            "\n\n" + (on ? "On: the game uses it." : "Off: the game leaves it out.") +
            "\n\nA (or drag it) to move it up or down the list.";

        // Rebuilt after every change; the selection stays on the same pack (by name: a moved pack gets a new folder name, a
        // downloaded one moves from the downloads to the list).
        void Fill()
        {
            string? keep = board.Selected is { Parent: { } parent } kept && (parent == list || parent == downloads) ? kept.Label : null;
            list.Children.Clear();
            downloads.Children.Clear();
            packDownloadRow = null;
            var installed = TexturePacks.List(userData);
            if (installed.Count == 0)
                list.Children.Add(Indent(Hint("No texture packs yet. Download one below, or put a pack's folder in the texture packs folder.")));
            foreach (var pack in installed)
            {
                var row = new OrderRow(pack.Folder, pack.Name, Describe(pack, pack.Enabled)) { IsChecked = pack.Enabled };
                row.IsCheckedChanged += (_, _) =>
                {
                    try { TexturePacks.SetEnabled(userData, pack.Folder, row.IsChecked); }
                    catch (Exception e) { Dialogs.Message(this, $"Switching {pack.Name} failed: {e.Message}").Watch(); }
                    Fill();
                };
                row.Moved += (_, _) => SaveOrder().Watch();
                if (pack.Known is { } known) row.SetExtra("Remove", () => RemovePack(known).Watch());
                list.Children.Add(row);
            }
            var missing = TexturePacks.All.Where(p => installed.All(i => i.Known != p)).ToList();
            if (missing.Count == 0) downloads.Children.Add(Indent(Hint("Every pack listed here is downloaded.")));
            foreach (var pack in missing)
            {
                bool mine = packDownloading == pack;
                var row = new ActionRow(pack.Name, () => { if (packDownloading == pack) packCancel?.Cancel(); else Download(pack).Watch(); },
                                        mine ? "Cancel" : "Download", $"{pack.About} By {pack.Credit}.")
                {
                    Value = mine ? packProgress : $"{pack.ApproxBytes >> 20} MB",
                    IsEnabled = mine || packCancel == null,
                };
                downloads.Children.Add(row);
                if (mine)
                {
                    packDownloadRow = row;
                    downloads.Children.Add(packBar);
                }
            }
            if (keep == null || !board.IsVisible) return;
            var again = list.Children.Concat(downloads.Children).OfType<MenuRow>().FirstOrDefault(r => r.Label == keep);
            Dispatcher.UIThread.Post(() => { if (again != null) board.Select(again); else board.SelectFirst(); }, DispatcherPriority.Loaded);
        }

        async Task SaveOrder()
        {
            var order = list.Children.OfType<OrderRow>().Select(r => r.Key).ToList();
            if (GameRunning()) await Dialogs.Message(this, "Close the game first: it reads the texture packs while it runs.");
            else
                try { TexturePacks.Reorder(userData, order); }
                catch (Exception e) { await Dialogs.Message(this, $"The packs couldn't be put in that order: {e.Message}"); }
            Fill();
        }

        async Task RemovePack(TexturePacks.Pack pack)
        {
            if (GameRunning()) { await Dialogs.Message(this, "Close the game first: it reads the texture packs while it runs."); return; }
            if (!await Dialogs.YesNo(this, $"Remove {pack.Name}? It can be downloaded again any time.")) return;
            try { TexturePacks.Remove(userData, pack); }
            catch (Exception e) { await Dialogs.Message(this, $"Removing {pack.Name} failed: {e.Message}"); }
            Fill();
        }

        async Task Download(TexturePacks.Pack pack)
        {
            if (packCancel != null) return;
            packCancel = new CancellationTokenSource();
            packDownloading = pack;
            packProgress = "Starting";
            packBar.Value = 0;
            packBar.IsVisible = true;
            Fill();
            var token = packCancel.Token;
            var report = new Progress<(long done, long total)>(p =>
            {
                if (token.IsCancellationRequested) return;
                packBar.Value = p.total > 0 ? (int)(p.done * 1000 / p.total) : 0;
                packProgress = p.total > 0 && p.done >= p.total ? "Unpacking" : p.total > 0 ? $"{p.done >> 20} of {p.total >> 20} MB" : $"{p.done >> 20} MB";
                if (packDownloadRow != null) packDownloadRow.Value = packProgress;
            });
            string? error = null;
            try
            {
                await TexturePacks.Download(userData, pack, report, token);
                // A pack the player just chose should show: HD textures on (it may have been off for the Xbox textures).
                var c = new GameConfig(configPath);
                if (c.Get("video", "texture_replacements") == "false") { c.Set("video", "texture_replacements", "true"); c.Save(); }
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { }
            catch (Exception e) { error = e.Message; }
            finally
            {
                packCancel.Dispose();
                packCancel = null;
                packDownloading = null;
                packBar.IsVisible = false;
            }
            if (!IsVisible) return;  // the launcher closed
            Fill();
            if (error != null) await Dialogs.Message(this, $"Downloading {pack.Name} failed: {error}");
        }

        refreshPacks = Fill;
        Fill();
    }

    // ----- The disc and starting the game -----
    (bool ok, string text) DiscState()
    {
        var boot = Path.Combine(paths.PlayableDisc, "sys", "boot.bin");
        if (!File.Exists(boot)) return (false, "No disc set up yet.");
        var id = System.Text.Encoding.ASCII.GetString(File.ReadAllBytes(boot), 0, 6);
        return id == GcDisc.WantedId ? (true, "Set up: Scooby-Doo! Night of 100 Frights (GIHE78).") : (false, $"The Disc folder holds {id}, not Scooby-Doo! Night of 100 Frights USA (GIHE78).");
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
        chooseDisc.Label = ok ? "Change disc image" : "Choose disc image";
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

    void RefreshPs2()
    {
        if (ps2Status == null) return;
        int count = Ps2Cutscenes.Count(userData), textures = XboxTextures.Count(userData), sounds = XboxAudio.Count(userData);
        var parts = new List<string>();
        if (count > 0) parts.Add($"{count} cutscenes");
        if (textures > 0) parts.Add($"{textures} Xbox textures");
        if (sounds > 0) parts.Add($"{sounds} Xbox sounds");
        bool any = parts.Count > 0;
        ps2Status.Text = any ? "Set up: " + string.Join(", ", parts) + "." : "Not set up: the game shows the GameCube's movies.";
        ps2Status.Foreground = Brush(any ? Good : CreamDim);  // optional: nothing is missing
        choosePs2.Label = any ? "Choose another PS2 or Xbox disc image" : "Choose PS2 or Xbox disc image";
        removePs2.IsVisible = any;
    }

    async Task ChoosePs2Disc()
    {
        if (GameRunning()) { await Dialogs.Message(this, "Close the game first: it reads the cutscenes while it runs."); return; }
        if (!DiscState().ok) { await Dialogs.Message(this, "Set up the GameCube disc first: the cutscenes are checked against its levels."); return; }
        var file = await ControllerDialogs.PickPath(this, "Choose your Scooby-Doo! Night of 100 Frights PS2 or Xbox disc image");
        if (file == null) return;
        choosePs2.IsEnabled = removePs2.IsEnabled = false;
        ps2Bar.IsVisible = true;
        ps2Bar.Value = 0;
        var report = new Progress<(int done, int total, string name)>(p =>
        {
            ps2Bar.Maximum = Math.Max(1, p.total);
            ps2Bar.Value = Math.Min(p.done, ps2Bar.Maximum);
            ps2Status.Text = p.name.Length > 0 ? $"Looking for cutscenes {p.done + 1}/{p.total}: {p.name}" : "Copying";
        });
        Ps2Cutscenes.Result? result = null;
        string? error = null;
        try
        {
            var disc = paths.PlayableDisc;
            result = await Task.Run(() => Ps2Cutscenes.Copy(file, disc, userData, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
            if (result is { Problem.Length: 0, Expected: Ps2Cutscenes.ExpectedXbox })
            {
                var textureReport = new Progress<(int done, int total, string name)>(p =>
                {
                    ps2Bar.Maximum = Math.Max(1, p.total);
                    ps2Bar.Value = Math.Min(p.done, ps2Bar.Maximum);
                    ps2Status.Text = p.name.Length > 0 ? $"Taking the Xbox textures {p.done + 1}/{p.total}: {p.name}" : "Copying";
                });
                await Task.Run(() =>
                {
                    using var image = Ps2Cutscenes.OpenImage(file);
                    XboxTextures.Copy(image, disc, userData, (d, t, n) => ((IProgress<(int, int, string)>)textureReport).Report((d, t, n)));
                });
                var soundReport = new Progress<(int done, int total, string name)>(p =>
                {
                    ps2Bar.Maximum = Math.Max(1, p.total);
                    ps2Bar.Value = Math.Min(p.done, ps2Bar.Maximum);
                    ps2Status.Text = p.name.Length > 0 ? $"Taking the Xbox sound {p.done + 1}/{p.total}: {p.name}" : "Copying";
                });
                await Task.Run(() =>
                {
                    using var image = Ps2Cutscenes.OpenImage(file);
                    XboxAudio.Copy(image, disc, userData, (d, t, n) => ((IProgress<(int, int, string)>)soundReport).Report((d, t, n)));
                });
            }
        }
        catch (Exception e) { error = e.Message; }
        finally { ps2Bar.IsVisible = false; choosePs2.IsEnabled = removePs2.IsEnabled = true; RefreshPs2(); }
        if (error != null) await Dialogs.Message(this, "Copying the cutscenes failed: " + error);
        else if (result is { Problem.Length: > 0 }) await Dialogs.Message(this, result.Problem);
        else if (result != null && result.Copied < result.Expected)
            await Dialogs.Message(this, $"Copied {result.Copied} of the {result.Expected} cutscenes on that disc: the others play as movies.");
    }

    async Task RemovePs2Cutscenes()
    {
        if (GameRunning()) { await Dialogs.Message(this, "Close the game first: it reads the cutscenes while it runs."); return; }
        Ps2Cutscenes.Remove(userData);
        XboxTextures.Remove(userData);
        XboxAudio.Remove(userData);
        RefreshPs2();
    }

    async Task Launch()
    {
        if (!enableServices && !launchInFixture) throw new InvalidOperationException("Game launch is disabled in this test fixture.");
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
    void BuildPadMapping(StackPanel page)
    {
        var card = Section(page, "Gamepad buttons", "F10 > Controller edits the same mapping.");
        var config = new GameConfig(configPath);
        foreach (var row in PadMapping.Rows)
        {
            var box = new ChoiceRow(row.Label, $"The controller button that works as {row.Label}. By position: South is A on Xbox, " +
                                               "Cross on PlayStation, B on Nintendo.");
            foreach (var choice in PadMapping.ChoicesFor(row)) box.Items.Add(choice);
            card.Children.Add(box);
            padRows.Add((row, box));
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
        ShowXboxRows();
        card.Children.Add(new ActionRow("Restore defaults", RestorePadDefaults, "Restore", "Puts every button back to the GameCube default."));
    }

    // Selects the value's pad button; one the list doesn't know stays as it is, shown as "(custom)".
    static void ShowPadValue(PadMapping.Row row, ChoiceRow box, string? value)
    {
        string name = value == null ? row.Default : PadMapping.Split(value).Name;
        for (int i = box.Items.Count - 1; i >= 0; i--)
            if (box.Items[i] is string) box.Items.RemoveAt(i);
        int index = -1;
        for (int i = 0; i < box.Items.Count; i++)
            if (box.Items[i] is PadMapping.Choice c && c.Name == name) { index = i; break; }
        if (index < 0) { box.Items.Add("(custom) " + value); index = box.Items.Count - 1; }
        box.SelectedIndex = index;
        box.Refresh();
    }

    // Black / White exist only with the original Xbox layout.
    void ShowXboxRows()
    {
        bool xbox = layoutBox.SelectedIndex == 1;
        foreach (var (row, box) in padRows)
            if (row.Xbox) box.IsVisible = xbox;
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
        foreach (var (row, box) in padRows) ShowPadValue(row, box, c.Get("controller", row.Key));
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
            layoutBox.Refresh();
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
