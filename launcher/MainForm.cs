using System.Diagnostics;
using static FrightWorks.Theme;

namespace FrightWorks;

// The launcher window: the haunted mansion (MansionScene) whose signs are the menu. Play starts the game; Settings,
// Mods and Extras open a board over the left of the house. Everything it changes goes to Config.toml (DataPaths),
// the keys the game's F10 menu uses. No game data or character art is shipped.
sealed class MainForm : Form
{
    internal const string GameExe = DataPaths.GameExe, AppTitle = "FrightWorks: Night of 100 Frights";
    const string ProjectUrl = "https://github.com/FrightWorks/FrightWorks-no100f";

    readonly DataPaths paths;
    readonly string root, play, userData, configPath, modsDir, discDir;
    readonly string? shotDir;
    readonly MansionScene scene;
    readonly Dictionary<string, Board> boards = new();
    Label discStatus = null!, dumpHelp = null!;
    PlankButton chooseDisc = null!;
    ThemedBar progress = null!, ffmpegBar = null!;
    Label ffmpegStatus = null!, savesNote = null!, updateStatus = null!;
    PlankButton savesDefault = null!, checkNow = null!;
    readonly Updater updater;
    bool updating;  // checking, or downloading / applying an update: Play waits
    bool asking;    // a question (Xbox layout, update) is open: the other waits, so one never disables the other
    PlankButton ffmpegButton = null!;
    CancellationTokenSource? ffmpegCancel;  // an ffmpeg download is running
    Action<bool>? showControlsTab;
    readonly Gamepads gamepads;
    Label padStatus = null!;
    ThemedCombo layoutBox = null!;
    readonly List<(PadMapping.Row Row, Label Label, ThemedCombo Box)> padRows = new();
    bool askedLayout;

    public MainForm(string? shotDir = null, DataPaths? dataPaths = null)
    {
        this.shotDir = shotDir;
        K = DeviceDpi / 96f;
        AutoScaleDimensions = new SizeF(DeviceDpi, DeviceDpi);
        AutoScaleMode = AutoScaleMode.Dpi;
        SuspendLayout();
        paths = dataPaths ?? DataPaths.Find();
        (root, play, userData, configPath, modsDir, discDir) = (paths.Root, paths.Game, paths.UserData, paths.ConfigPath, paths.Mods, paths.Disc);
        gamepads = new Gamepads(play);
        updater = new Updater(paths);

        Text = AppTitle;
        try { Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!); } catch { }
        var area = Screen.PrimaryScreen?.WorkingArea.Size ?? new Size(S(1180), S(830));
        ClientSize = new Size(Math.Min(S(1180), area.Width - 40), Math.Min(S(830), area.Height - 60));
        MinimumSize = new Size(Math.Min(S(960), area.Width), Math.Min(S(680), area.Height));
        BackColor = BoardBottom;
        Font = Body;
        StartPosition = FormStartPosition.CenterScreen;
        KeyPreview = true;

        scene = new MansionScene(shotDir == null && SystemInformation.UIEffectsEnabled, shotDir == null ? Environment.TickCount : 1)
        {
            Dock = DockStyle.Fill,
        };
        scene.SignClicked += OnSign;
        scene.Resize += (_, _) => PlaceBoards();
        Controls.Add(scene);

        AddBoard("settings", "Settings", BuildSettings);
        AddBoard("mods", "Mods", BuildMods);
        AddBoard("extras", "Extras", BuildExtras);
        PlaceBoards();
        RefreshDisc();
        ResumeLayout(true);

        KeyDown += (_, e) =>
        {
            if (e.KeyCode == Keys.Escape && boards.Values.Any(b => b.Visible)) { CloseBoards(); e.Handled = true; }
        };
        // Connected controllers: shown on Settings > Controls; an Xbox pad asks for the layout once.
        var padTimer = new System.Windows.Forms.Timer { Interval = 1000 };
        padTimer.Tick += (_, _) => RefreshGamepads();
        FormClosed += (_, _) => { padTimer.Stop(); gamepads.Dispose(); ffmpegCancel?.Cancel(); };
        Shown += (_, _) =>
        {
            if (shotDir != null) { TakeShots(shotDir); return; }
            gamepads.Start();
            padTimer.Start();
            if (updater.CanCheck && updater.CheckAtStart) _ = CheckForUpdates(manual: false);
        };
    }

    protected override void OnDpiChanged(DpiChangedEventArgs e)
    {
        K = e.DeviceDpiNew / 96f;
        base.OnDpiChanged(e);
        PlaceBoards();
        Invalidate(true);
    }

    protected override void OnLayout(LayoutEventArgs levent)
    {
        base.OnLayout(levent);
        // A DPI transition scales children after their Resize events; place the boards at the final size.
        if (scene != null) PlaceBoards();
    }

    // Headless visual check (--shots <dir>): the main screen, a lightning flash and every board, then exit.
    void TakeShots(string dir)
    {
        Directory.CreateDirectory(dir);
        void Shot(string name)
        {
            Application.DoEvents();
            using var bmp = new Bitmap(Width, Height);
            DrawToBitmap(bmp, new Rectangle(Point.Empty, Size));
            bmp.Save(Path.Combine(dir, name + ".png"));
        }
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
        var extras = boards["extras"].Body;
        extras.AutoScrollPosition = new Point(0, extras.DisplayRectangle.Height);  // the bottom: folders and saves
        Shot("Extras-Folders");
        extras.AutoScrollPosition = Point.Empty;
        ShowBoard("settings");
        showControlsTab?.Invoke(true);
        Shot("Settings-Controls");
        Close();
    }

    // ----- Signs and boards -----
    void OnSign(string id)
    {
        switch (id)
        {
            case "play":
                if (updating && updater.Installed)
                    MessageBox.Show(this, "The launcher is updating the game. It starts again by itself when it's done.", AppTitle);
                else if (CanPlay()) Launch();
                else ShowBoard("extras");
                break;
            case "exit":
                Close();
                break;
            default:
                if (boards.TryGetValue(id, out var b) && b.Visible) CloseBoards();
                else ShowBoard(id);
                break;
        }
    }

    void AddBoard(string id, string title, Action<FlowLayoutPanel> build)
    {
        var board = new Board(title, CloseBoards);
        board.SuspendLayout();
        var flow = new FlowLayoutPanel
        {
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.TopDown,
            WrapContents = false, BackColor = Color.Transparent, Margin = Padding.Empty,
        };
        flow.SuspendLayout();
        build(flow);
        board.Body.Controls.Add(flow);
        boards[id] = board;
        scene.Controls.Add(board);
        flow.ResumeLayout(true);
        board.ResumeLayout(true);
    }

    void PlaceBoards()
    {
        int left = S(14), top = S(14);
        int right = (int)scene.SignsLeft - S(18), bottom = scene.Height - S(44);
        var r = new Rectangle(left, top, Math.Max(S(300), right - left), Math.Max(S(300), bottom - top));
        foreach (var b in boards.Values) b.Bounds = r;
    }

    void ShowBoard(string id)
    {
        RefreshDisc();
        scene.SuspendLayout();
        try
        {
            foreach (var (key, b) in boards) b.Visible = key == id;
            boards[id].BringToFront();
            boards[id].PerformLayout();
            boards[id].Body.PerformLayout();
            scene.ActiveSign = id;
        }
        finally { scene.ResumeLayout(true); }
    }

    void CloseBoards()
    {
        foreach (var b in boards.Values) b.Visible = false;
        scene.ActiveSign = null;
        scene.Focus();
    }

    static FlowLayoutPanel Row() => new()
    {
        AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
        Margin = Padding.Empty, BackColor = Color.Transparent, WrapContents = true,
    };

    static TableLayoutPanel Table() => new()
    {
        AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
        ColumnCount = 2, BackColor = Color.Transparent, Margin = S(0, 2, 0, 6),
    };

    void OpenFolder(string path)
    {
        try { Process.Start("explorer.exe", Directory.CreateDirectory(path).FullName); }
        catch (Exception e) { MessageBox.Show(this, "Couldn't open " + path + ": " + e.Message, AppTitle); }
    }

    // ----- Settings: Display and Controls -----
    void BuildSettings(FlowLayoutPanel body)
    {
        var tabs = Row();
        var display = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Color.Transparent, Margin = Padding.Empty };
        var controls = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Color.Transparent, Margin = Padding.Empty, Visible = false };
        PlankButton displayTab = null!, controlsTab = null!;
        void Select(bool showControls)
        {
            body.SuspendLayout();
            try
            {
                display.Visible = !showControls;
                controls.Visible = showControls;
                displayTab.Selected = !showControls;
                controlsTab.Selected = showControls;
            }
            finally { body.ResumeLayout(true); }
        }
        displayTab = new PlankButton("Display", (_, _) => Select(false)) { Selected = true };
        controlsTab = new PlankButton("Controls", (_, _) => Select(true));
        showControlsTab = Select;
        tabs.Controls.Add(displayTab);
        tabs.Controls.Add(controlsTab);
        body.Controls.Add(tabs);
        BuildDisplay(display);
        BuildControls(controls);
        body.Controls.Add(display);
        body.Controls.Add(controls);
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

    void BuildDisplay(FlowLayoutPanel body)
    {
        var cfg = new GameConfig(configPath);
        var table = Table();
        void Row(string label, string key, (string text, string raw)[] options, string fallback, Action<GameConfig>? changed = null)
        {
            var box = new ThemedCombo();
            foreach (var o in options) box.Items.Add(o.text);
            var current = cfg.Get("video", key) ?? fallback;
            int index = Array.FindIndex(options, o => SameValue(o.raw, current));
            box.SelectedIndex = index >= 0 ? index : Array.FindIndex(options, o => SameValue(o.raw, fallback));
            box.SelectedIndexChanged += (_, _) =>
            {
                var c = new GameConfig(configPath);
                c.Set("video", key, options[box.SelectedIndex].raw);
                changed?.Invoke(c);
                c.Save();
            };
            table.Controls.Add(FieldLabel(label));
            table.Controls.Add(box);
        }
        Row("Window", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Window", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }, "borderless");

        // A saved monitor that isn't connected stays selected and saved, so it works again when plugged back in. One found
        // by model on another port keeps its saved id: both mean the same monitor to the game.
        var monitors = Monitors.List();
        string savedMonitor = cfg.Get("video", "fullscreen_display") ?? "";
        if (savedMonitor.Length == 0) savedMonitor = "auto";
        var savedMatch = Monitors.Find(monitors, savedMonitor);
        var monitorOptions = new List<(string text, string raw)> { ("Automatic (where the game window is)", "\"auto\"") };
        foreach (var m in monitors) monitorOptions.Add((m.Label, "\"" + (m == savedMatch ? savedMonitor : m.Id) + "\""));
        if (savedMonitor != "auto" && savedMatch == null) monitorOptions.Add(("Saved monitor (not connected)", "\"" + savedMonitor + "\""));
        Row("Fullscreen monitor", "fullscreen_display", monitorOptions.ToArray(), "auto");

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
            if (filling) return;
            var c = new GameConfig(configPath);
            c.Set("video", "resolution_multiplier", shownResolutions[resolutionBox.SelectedIndex].raw);
            c.Save();
        };
        table.Controls.Add(FieldLabel("Resolution"));
        table.Controls.Add(resolutionBox);

        // Without aspect_mode the game goes by the older keys (main.cpp): force_16_9 = 16:9, widescreen = false = 4:3,
        // else fill the screen.
        string aspectFallback = cfg.Get("video", "force_16_9") == "true" ? "1" : cfg.Get("video", "widescreen") == "false" ? "0" : "2";
        Row("Picture shape", "aspect_mode", new[] { ("Original 4:3", "0"), ("Widescreen 16:9", "1"), ("Fill the screen", "2"), ("Cinematic 2.39:1", "3") }, aspectFallback);
        Row("Frame rate", "fps60", new[] { ("30 fps (original)", "false"), ("60 fps", "true") }, "false");
        Row("HD textures", "texture_replacements", new[] { ("On", "true"), ("Off (original textures)", "false") }, "true");
        Row("Anti-aliasing", "msaa", new[] { ("Off", "1"), ("2x MSAA", "2"), ("4x MSAA", "4"), ("8x MSAA", "8") }, "1");
        Row("Intro movies", "skip_intro", new[] { ("Play them", "false"), ("Skip them", "true") }, "false");
        Row("HDR", "hdr", new[] { ("Off", "false"), ("On (needs HDR on in Windows)", "true") }, "false");
        Row("HDR brightness", "hdr_paper_white_nits", new[] { ("Dim (120 nits)", "120.0"), ("Normal (200 nits)", "200.0"), ("Bright (300 nits)", "300.0"), ("Brightest (400 nits)", "400.0") }, "200.0");
        Row("HDR highlights", "hdr_peak_nits", new[] { ("Subtle (400 nits)", "400.0"), ("Standard (1000 nits)", "1000.0"), ("Strong (1500 nits)", "1500.0") }, "1000.0");
        body.Controls.Add(table);
        body.Controls.Add(Note("Changes are saved at once and apply the next time the game starts. In the game, F10 has the " +
                               "resolution, picture shape, window, fullscreen monitor and frame interpolation settings too, and more (sound, controls); " +
                               "F11 switches between fullscreen and a window, and F2 saves a screenshot. " +
                               "Fullscreen monitor also applies when F11 goes fullscreen from a window."));

        // Experimental: F10's Smoothness > Frame interpolation. Going above 60 fps at 6x or 8x drops the resolution to
        // 4x, as F10 does.
        body.Controls.Add(Title("Experimental"));
        table = Table();  // Row adds to this one now
        Row("Frame interpolation", "frame_interpolation_fps", new[] { ("Off", "0"), ("120 FPS", "120"), ("180 FPS", "180") },
            InterpolationFps(cfg).ToString(), c =>
            {
                if (InterpolationFps(c) > 60 && AboveCap(c.Get("video", "resolution_multiplier") ?? "")) c.Set("video", "resolution_multiplier", "4");
                FillResolutions(c);
            });
        body.Controls.Add(table);
        body.Controls.Add(Note("Draws extra frames between the game's own for smoother motion. Experimental: you might see visual " +
                               "glitches. At 120 or 180 FPS the resolution goes up to 4x (1920p)."));
    }

    void BuildControls(FlowLayoutPanel body)
    {
        body.Controls.Add(Note("Gamepads (Xbox, PlayStation, Switch Pro, GameCube adapter...) work right away as a GameCube controller. " +
                               "Change the buttons below, or in the game with F10 > Controls."));
        body.Controls.Add(Title("Gamepad"));
        padStatus = FieldLabel("Looking for controllers...");
        padStatus.Margin = S(0, 4, 0, 6);
        body.Controls.Add(padStatus);
        // [controller] layout: "gamecube" or "xbox_original" (the game's original Xbox controls, hle/no100f_xbox_layout.h).
        var table = Table();
        layoutBox = new ThemedCombo();
        layoutBox.Items.Add("GameCube");
        layoutBox.Items.Add("Original Xbox");
        layoutBox.SelectedIndex = new GameConfig(configPath).Get("controller", "layout") == "xbox_original" ? 1 : 0;
        layoutBox.SelectedIndexChanged += (_, _) => { SaveLayout(layoutBox.SelectedIndex == 1); ShowXboxRows(); };
        table.Controls.Add(FieldLabel("Layout"));
        table.Controls.Add(layoutBox);
        body.Controls.Add(table);
        body.Controls.Add(Note("Original Xbox: A jump, X head smash, B dig / enter / talk and back in menus, Y on-screen display, " +
                               "LT gum, RT run, LB soap bubble (Xbox White), RB sneak (Xbox Black); the bumpers work together with the " +
                               "triggers, and the hints in the game name these buttons. Black and White can go on other buttons below. " +
                               "GameCube: B head smash and back in menus, X dig / enter / talk, Z + L soap bubble, Z + R sneak (Z = RB). " +
                               "Also in F10 > Controls."));
        BuildPadMapping(body);
        body.Controls.Add(Title("Keyboard"));
        table = Table();
        foreach (var (what, key) in new[] { ("Move", "W A S D"), ("Camera (C-stick)", "Arrow keys"), ("A (jump)", "Space"), ("B", "Left Shift"),
                     ("X / Y", "R / F"), ("Z", "C"), ("L / R", "Q / E"), ("Start (pause)", "Enter"), ("D-pad", "I J K L") })
        {
            var name = FieldLabel(what);
            name.Font = Body;
            name.ForeColor = CreamDim;
            table.Controls.Add(name);
            table.Controls.Add(FieldLabel(key));
        }
        body.Controls.Add(table);
        body.Controls.Add(Title("In the game"));
        body.Controls.Add(Note("F10 settings   ·   F11 fullscreen / window   ·   F2 screenshot   ·   F1 project page"));
    }

    // ----- Mods -----
    void BuildMods(FlowLayoutPanel body)
    {
        body.Controls.Add(Note("A mod is a folder in the mods folder (the button below opens it). Its files\\ folder mirrors the disc's files\\ (replacing game files), " +
                               "and its textures\\ folder holds texture replacements. Untick a mod to switch it off; when two mods change the same file, the later name wins."));
        var list = new FlowLayoutPanel
        {
            Size = S(480, 240), FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true,
            BackColor = Field, Padding = S(10, 8, 10, 8), Margin = S(0, 8, 0, 8),
        };
        var empty = Note("No mods yet.");
        void Fill()
        {
            list.Controls.Clear();
            var dirs = Directory.Exists(modsDir) ? Directory.GetDirectories(modsDir).OrderBy(d => d, StringComparer.OrdinalIgnoreCase).ToArray() : Array.Empty<string>();
            if (dirs.Length == 0) { list.Controls.Add(empty); return; }
            foreach (var d in dirs)
            {
                var box = new CheckBox
                {
                    Text = Path.GetFileName(d), Checked = !File.Exists(Path.Combine(d, "disabled")), AutoSize = true, AutoEllipsis = true,
                    ForeColor = Cream, BackColor = Field, Font = Body, FlatStyle = FlatStyle.Standard, Cursor = Cursors.Hand, Margin = S(0, 3, 0, 3),
                };
                var dir = d;
                box.CheckedChanged += (_, _) =>
                {
                    var flag = Path.Combine(dir, "disabled");
                    if (box.Checked) File.Delete(flag); else File.WriteAllText(flag, "");
                };
                list.Controls.Add(box);
            }
        }
        body.Controls.Add(list);
        var row = Row();
        row.Controls.Add(new PlankButton("Open the mods folder", (_, _) => OpenFolder(modsDir)));
        row.Controls.Add(new PlankButton("Refresh", (_, _) => Fill()));
        body.Controls.Add(row);
        Fill();
    }

    // ----- Extras: the disc, holidays, folders, about -----
    void BuildExtras(FlowLayoutPanel body)
    {
        body.Controls.Add(Title("Game disc"));
        discStatus = FieldLabel("");
        discStatus.MaximumSize = new Size(S(520), 0);
        body.Controls.Add(discStatus);
        chooseDisc = new PlankButton("Choose disc image", async (_, _) => await ChooseDisc(), primary: true);
        body.Controls.Add(chooseDisc);
        progress = new ThemedBar { Visible = false };
        body.Controls.Add(progress);
        body.Controls.Add(dumpHelp = Note(
            "FrightWorks runs your own copy of Scooby-Doo! Night of 100 Frights for the GameCube (USA, GIHE78, version 1.01). " +
            "Dump the disc you own (for example with CleanRip on a Wii) as a plain .iso or .ciso, then choose it here. " +
            "The launcher copies the game files once; the image file isn't needed afterwards."));

        body.Controls.Add(Title("Holidays"));
        body.Controls.Add(HolidayBox());
        body.Controls.Add(Note("Outside Mystic Manor and in the playground, the game celebrates holidays on their real dates. " +
                               "Pick one to see it any day, or turn them off."));

        body.Controls.Add(Title("Developer tools"));
        body.Controls.Add(DevToolsBox());
        body.Controls.Add(Note("For testing and bug hunting: F8 in the game opens the dev menu (go to any level, saved spots, " +
                               "free camera, god mode, texture finder, recording options), F3 records a video of the game into " +
                               "the Videos folder with a .txt for bug reports, and the game shows DEV and its build in the " +
                               "corner. Applies the next time the game starts."));
        ffmpegStatus = Note("");
        body.Controls.Add(ffmpegStatus);
        ffmpegButton = new PlankButton("Download ffmpeg", async (_, _) => await DownloadFfmpeg());
        body.Controls.Add(ffmpegButton);
        ffmpegBar = new ThemedBar { Visible = false };
        body.Controls.Add(ffmpegBar);
        RefreshFfmpeg(offerDownload: false);

        body.Controls.Add(Title("Folders"));
        var folders = Row();
        folders.MaximumSize = new Size(S(520), 0);  // wrap instead of widening the page
        folders.Controls.Add(new PlankButton("Saves", (_, _) => OpenFolder(paths.Saves)));
        folders.Controls.Add(new PlankButton("Settings and mods", (_, _) => OpenFolder(userData)));
        folders.Controls.Add(new PlankButton("Screenshots", (_, _) => OpenFolder(paths.Screenshots)));
        folders.Controls.Add(new PlankButton("Videos", (_, _) => OpenFolder(paths.Videos)));
        folders.Controls.Add(new PlankButton("Game", (_, _) => OpenFolder(play)));
        body.Controls.Add(folders);
        savesNote = Note("");
        body.Controls.Add(savesNote);
        var saveRow = Row();
        saveRow.MaximumSize = new Size(S(520), 0);
        saveRow.Controls.Add(new PlankButton("Change the saves folder", (_, _) => ChooseSavesFolder()));
        savesDefault = new PlankButton("Back to the usual folder", (_, _) => MoveSaves(null));
        saveRow.Controls.Add(savesDefault);
        saveRow.Controls.Add(new PlankButton("Copy from an earlier version", (_, _) => { DataImport.FromFolder(this, paths, AppTitle); RefreshDisc(); RefreshSaves(); }));
        body.Controls.Add(saveRow);
        RefreshSaves();

        body.Controls.Add(Title("Updates"));
        updateStatus = Note("");
        if (updater.CanCheck)
        {
            body.Controls.Add(UpdateBox());
            body.Controls.Add(updateStatus);
            checkNow = new PlankButton("Check now", async (_, _) => await CheckForUpdates(manual: true));
            body.Controls.Add(checkNow);
            ShowUpdateState(null);
        }
        else
        {
            updateStatus.Text = paths.DevTree ? "A source build doesn't look for updates."
                                              : "This copy wasn't installed with the FrightWorks installer, so it can't look for updates.";
            body.Controls.Add(updateStatus);
        }

        body.Controls.Add(Title("About"));
        var version = typeof(MainForm).Assembly.GetName().Version;
        body.Controls.Add(Note("FrightWorks " + (version == null ? "" : "v" + version.ToString(3) + " ") + "by Loneranger419, based on Scooby-Doo: Refrightened by JosiahG. Built on WiiCompiled (GPLv3) " +
                               "and Aurora (MIT). HD texture pack by dogerki. Button prompts by Kenney (CC0). Lettering in " +
                               "Luckiest Guy by Astigmatic and in-game menu text in Roboto by Christian Robertson (both Apache 2.0). " +
                               "Scooby-Doo is a trademark of Hanna-Barbera / Warner Bros.; this is an unofficial fan project."));
        body.Controls.Add(new PlankButton("Project page", (_, _) => Process.Start(new ProcessStartInfo(ProjectUrl) { UseShellExecute = true })));
    }

    // ----- The saves folder ([paths] saves_dir) -----
    void RefreshSaves()
    {
        bool custom = !string.IsNullOrWhiteSpace(new GameConfig(configPath).Get("paths", "saves_dir"));
        savesNote.Text = "Saves are in " + paths.Saves + (custom ? " (your choice)." : ".") +
                         " Settings and mods are in " + userData + ".";
        savesDefault.Visible = custom;
    }

    void ChooseSavesFolder()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Choose the folder for your saves", UseDescriptionForTitle = true, SelectedPath = paths.Saves,
        };
        if (dialog.ShowDialog(this) == DialogResult.OK) MoveSaves(dialog.SelectedPath);
    }

    // Points [paths] saves_dir at target (null = the usual folder) and offers to move the saves already there. Files the
    // new folder already has are left in both places.
    void MoveSaves(string? target)
    {
        var from = paths.Saves;
        var to = Path.GetFullPath(target ?? paths.DefaultSaves);
        if (string.Equals(Path.TrimEndingDirectorySeparator(from), Path.TrimEndingDirectorySeparator(to), StringComparison.OrdinalIgnoreCase))
        {
            if (target == null) { var c = new GameConfig(configPath); c.Remove("paths", "saves_dir"); c.Save(); RefreshSaves(); }
            return;
        }
        // An installed game's folder (%LOCALAPPDATA%\FrightWorks) is replaced on update and deleted on uninstall.
        var program = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(root)) ?? root;
        if (!paths.Portable && (to + "\\").StartsWith(program + "\\", StringComparison.OrdinalIgnoreCase))
        {
            MessageBox.Show(this, "That folder is part of the installed game, which an update replaces. Choose another one.", AppTitle);
            return;
        }
        if (GameRunning())
        {
            MessageBox.Show(this, "Close the game first: it is using the saves.", AppTitle);
            return;
        }
        var files = Directory.Exists(from) ? Directory.GetFiles(from, "*", SearchOption.AllDirectories) : Array.Empty<string>();
        var answer = files.Length == 0 ? DialogResult.No
            : MessageBox.Show(this, $"Move your saves from\n{from}\nto\n{to}?\n\nNo uses the new folder without moving them.",
                              AppTitle, MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
        if (answer == DialogResult.Cancel) return;
        int kept = 0;
        try
        {
            Directory.CreateDirectory(to);
            if (answer == DialogResult.Yes)
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
            MessageBox.Show(this, "Moving the saves stopped: " + e.Message + "\n\nThe saves folder is unchanged.", AppTitle,
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        var cfg = new GameConfig(configPath);
        if (target == null) cfg.Remove("paths", "saves_dir"); else cfg.SetString("paths", "saves_dir", to);
        cfg.Save();
        RefreshSaves();
        if (kept > 0)
            MessageBox.Show(this, $"{kept} save file(s) already in {to} were kept; the old copies are still in {from}.", AppTitle);
    }

    bool GameRunning()
    {
        var exe = Path.Combine(play, GameExe);
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
        return box;
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
        checkNow.Enabled = false;
        ShowUpdateState("Looking for a new version...");
        Updater.Found? found = null;
        try { found = await updater.CheckAsync(CancellationToken.None); }
        catch (Exception e)
        {
            ShowUpdateState("Couldn't look for a new version: " + e.Message);
            if (manual) MessageBox.Show(this, "Couldn't look for a new version: " + e.Message, AppTitle, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        updating = false;
        if (IsDisposed) return;
        checkNow.Enabled = true;
        if (found == null)
        {
            if (updateStatus.Text.StartsWith("Looking")) ShowUpdateState(manual ? "You have the newest version, " + updater.CurrentVersion + "." : null);
            return;
        }
        ShowUpdateState($"Version {found.Version} is out (you have {updater.CurrentVersion}).");
        if (!manual && (updater.Skipped == found.Version || GameRunning())) return;  // the game has the screen: Extras says it
        while (asking) await Task.Delay(300);
        if (IsDisposed) return;
        UpdateAnswer answer;
        asking = true;
        try { answer = AskUpdate(found); }
        finally { asking = false; }
        switch (answer)
        {
            case UpdateAnswer.Update when updater.Installed: await InstallUpdate(found); break;
            case UpdateAnswer.Update: Process.Start(new ProcessStartInfo(found.PageUrl!) { UseShellExecute = true }); break;
            case UpdateAnswer.Skip: updater.Skip(found.Version); break;
        }
    }

    async Task InstallUpdate(Updater.Found found)
    {
        while (GameRunning())
            if (MessageBox.Show(this, "Close the game first, then press OK to update.", AppTitle, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        updating = true;
        checkNow.Enabled = false;
        void Show(string text) { if (!IsDisposed) { scene.SetStatus(text, false); ShowUpdateState(text); } }
        Show($"Downloading version {found.Version}...");
        try
        {
            await updater.DownloadAsync(found, p => { try { BeginInvoke(() => Show($"Downloading version {found.Version}: {p}%")); } catch (InvalidOperationException) { } },
                                        CancellationToken.None);
            Show($"Installing version {found.Version}...");
            updater.ApplyAndRestart(found);  // exits the launcher
        }
        catch (Exception e)
        {
            updating = false;
            checkNow.Enabled = true;
            RefreshDisc();
            ShowUpdateState("The update failed: " + e.Message);
            MessageBox.Show(this, "The update failed: " + e.Message + "\n\nThe game is unchanged; try again with Extras > Check now.", AppTitle,
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    enum UpdateAnswer { Update, Later, Skip }

    UpdateAnswer AskUpdate(Updater.Found found)
    {
        if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
        using var dialog = new Form
        {
            Text = "A new version of FrightWorks", FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false,
            StartPosition = FormStartPosition.CenterParent, ShowInTaskbar = false, BackColor = BoardBottom, Font = Body,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = S(20, 14, 20, 14), Icon = Icon,
        };
        var body = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = BoardBottom };
        var title = Title($"FrightWorks {found.Version} is out");
        title.Margin = S(0, 0, 0, 6);
        body.Controls.Add(title);
        body.Controls.Add(Note($"You have version {updater.CurrentVersion}. " + (updater.Installed
            ? "Updating downloads the new version and restarts the launcher; your saves, settings and mods stay."
            : "This is the zip, which can't update itself: download the new version from its release page and unzip it over this folder.")));
        body.Controls.Add(Note("Extras > Updates turns this question off."));
        var row = Row();
        row.Margin = S(0, 8, 0, 0);
        var answer = UpdateAnswer.Later;
        row.Controls.Add(new PlankButton(updater.Installed ? "Update now" : "Open the download page", (_, _) => { answer = UpdateAnswer.Update; dialog.Close(); }, primary: true));
        row.Controls.Add(new PlankButton("Not now", (_, _) => { answer = UpdateAnswer.Later; dialog.Close(); }));
        row.Controls.Add(new PlankButton("Skip this version", (_, _) => { answer = UpdateAnswer.Skip; dialog.Close(); }));
        body.Controls.Add(row);
        dialog.Controls.Add(body);
        dialog.ShowDialog(this);
        return answer;
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
            if (box.SelectedIndex >= Holidays.Length) return;
            var c = new GameConfig(configPath);
            c.SetString("game", "holiday", Holidays[box.SelectedIndex].value);
            c.Save();
        };
        return box;
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
        return box;
    }

    // F3 videos need ffmpeg. Looked for only while the developer tools are on; turning them on offers the download.
    void RefreshFfmpeg(bool offerDownload)
    {
        bool dev = new GameConfig(configPath).Get("game", "dev_tools") == "true";
        bool busy = ffmpegCancel != null;
        var found = dev && !busy ? FfmpegSetup.Find(paths) : null;
        ffmpegStatus.Visible = dev || busy;
        ffmpegButton.Visible = dev && !busy && found == null;
        if (busy || !dev) return;
        ffmpegStatus.Text = found != null
            ? "F3 videos use ffmpeg from " + found + "."
            : "F3 videos need ffmpeg, which isn't installed. The launcher can download it (about 200 MB, the free " +
              "win64 build from github.com/BtbN/FFmpeg-Builds) into " + FfmpegSetup.InstallDir(paths) + ".";
        ffmpegStatus.ForeColor = found != null ? Good : Warn;
        if (found == null && offerDownload &&
            MessageBox.Show(this, "F3 video recording needs ffmpeg, which isn't installed.\n\nDownload it now? (about 200 MB, " +
                                  "the free win64 build from github.com/BtbN/FFmpeg-Builds, checked against its published SHA-256)",
                            AppTitle, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
            _ = DownloadFfmpeg();
    }

    async Task DownloadFfmpeg()
    {
        if (ffmpegCancel != null) return;
        ffmpegCancel = new CancellationTokenSource();
        ffmpegButton.Visible = false;
        ffmpegBar.Visible = true;
        ffmpegBar.Maximum = 1000;
        ffmpegBar.Value = 0;
        ffmpegStatus.ForeColor = CreamDim;
        ffmpegStatus.Text = "Downloading ffmpeg...";
        var report = new Progress<(long done, long total)>(p =>
        {
            if (IsDisposed || ffmpegCancel == null) return;
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
            if (!IsDisposed) ffmpegBar.Visible = false;
        }
        RefreshFfmpeg(offerDownload: false);
        if (error != null) MessageBox.Show(this, "Downloading ffmpeg failed: " + error, AppTitle, MessageBoxButtons.OK, MessageBoxIcon.Warning);
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
        discStatus.ForeColor = ok ? Good : Warn;
        chooseDisc.Text = ok ? "Change disc image" : "Choose disc image";
        dumpHelp.Visible = !ok;
    }

    async Task ChooseDisc()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Choose your Scooby-Doo! Night of 100 Frights disc image",
            Filter = "GameCube disc images (*.iso;*.gcm;*.ciso)|*.iso;*.gcm;*.ciso|All files (*.*)|*.*",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        GcDisc disc;
        try { disc = GcDisc.Open(dialog.FileName); }
        catch (Exception e) { MessageBox.Show(this, "That file can't be read: " + e.Message, AppTitle); return; }
        using (disc)
        {
            var problem = disc.Problem();
            if (problem.Length > 0) { MessageBox.Show(this, problem, AppTitle, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            var staging = discDir + ".new";
            if (Directory.Exists(staging)) Directory.Delete(staging, true);
            chooseDisc.Enabled = false;
            progress.Visible = true;
            progress.Value = 0;
            var report = new Progress<(int done, int total, string name)>(p =>
            {
                progress.Maximum = Math.Max(1, p.total);
                progress.Value = Math.Min(p.done, progress.Maximum);
                discStatus.Text = $"Extracting {p.done}/{p.total}: {p.name}";
            });
            try
            {
                await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                Directory.Move(staging, discDir);
                var cfg = new GameConfig(configPath);
                cfg.Set("paths", "dvd_root", paths.DvdRootValue);  // relative to UserData, so the folder can move
                cfg.Save();
            }
            catch (Exception e) { MessageBox.Show(this, "Extraction failed: " + e.Message, AppTitle, MessageBoxButtons.OK, MessageBoxIcon.Error); }
            finally { progress.Visible = false; chooseDisc.Enabled = true; RefreshDisc(); }
        }
    }

    void Launch()
    {
        var exe = Path.Combine(play, GameExe);
        if (!File.Exists(exe)) { MessageBox.Show(this, "The game files (the data folder) are missing. Re-extract the download next to this launcher.", AppTitle); return; }
        gamepads.Pause();  // the game has the controllers to itself
        var game = Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = play, UseShellExecute = true });
        if (game != null)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) => { try { BeginInvoke(() => gamepads.Start()); } catch (InvalidOperationException) { } };
        }
        WindowState = FormWindowState.Minimized;
    }

    // ----- Controllers -----
    // Gamepad buttons: the [controller] keys F10 > Controls edits too (PadMapping). The game reads them at start.
    void BuildPadMapping(FlowLayoutPanel body)
    {
        body.Controls.Add(Title("Gamepad buttons"));
        body.Controls.Add(Note("The controller button that presses each GameCube button. Buttons go by position: South is A " +
                               "on Xbox, Cross on PlayStation, B on Nintendo. Used from the next game start; F10 > Controls " +
                               "in the game edits the same mapping."));
        var table = Table();
        var config = new GameConfig(configPath);
        foreach (var row in PadMapping.Rows)
        {
            var label = FieldLabel(row.Label);
            label.Font = Body;
            label.ForeColor = CreamDim;
            var box = new ThemedCombo();
            foreach (var choice in PadMapping.ChoicesFor(row)) box.Items.Add(choice);
            table.Controls.Add(label);
            table.Controls.Add(box);
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
        body.Controls.Add(table);
        ShowXboxRows();
        var restore = new PlankButton("Restore defaults", (_, _) => RestorePadDefaults());
        restore.Margin = S(0, 4, 0, 8);
        body.Controls.Add(restore);
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
        if (index < 0) index = box.Items.Add("(custom) " + value);
        box.SelectedIndex = index;
    }

    // Black / White exist only with the original Xbox layout.
    void ShowXboxRows()
    {
        bool xbox = layoutBox.SelectedIndex == 1;
        foreach (var (row, label, box) in padRows)
            if (row.Xbox) { label.Visible = xbox; box.Visible = xbox; }
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

    void SaveLayout(bool original)
    {
        var c = new GameConfig(configPath);
        c.SetString("controller", "layout", original ? "xbox_original" : "gamecube");
        c.Save();
    }

    void RefreshGamepads()
    {
        var pads = gamepads.Connected;
        padStatus.Text = !gamepads.Available ? "Controller detection is not available (SDL3.dll missing from the game folder)."
            : pads.Count == 0 ? "No controller connected."
            : "Connected: " + string.Join(", ", pads.Select(p => $"{p.Name} ({Gamepads.Describe(p.Kind)})"));
        // An Xbox pad and no layout chosen yet (key absent): ask once per launcher start.
        var xbox = pads.FirstOrDefault(p => p.Kind == Gamepads.Kind.Xbox);
        if (xbox == null || askedLayout || asking || new GameConfig(configPath).Get("controller", "layout") != null) return;
        askedLayout = true;
        bool? original;
        asking = true;
        try { original = AskXboxLayout(xbox.Name); }
        finally { asking = false; }
        if (original == null) return;
        SaveLayout(original.Value);
        layoutBox.SelectedIndex = original.Value ? 1 : 0;
    }

    // true = original Xbox layout, false = GameCube layout, null = closed without choosing (asked again next start).
    bool? AskXboxLayout(string padName)
    {
        if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
        using var dialog = new Form
        {
            Text = "Xbox controller detected", FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false,
            StartPosition = FormStartPosition.CenterParent, ShowInTaskbar = false, BackColor = BoardBottom, Font = Body,
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = S(20, 14, 20, 14), Icon = Icon,
        };
        var body = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = BoardBottom };
        var title = Title("Play with the original Xbox controls?");
        title.Margin = S(0, 0, 0, 6);
        body.Controls.Add(title);
        body.Controls.Add(Note($"{padName} is connected. Night of 100 Frights came out on the Xbox too, with its own button layout."));
        body.Controls.Add(Note("Original Xbox: A jump, X head smash, B dig / enter / talk, LT gum, RT run, LB soap bubble, RB sneak. " +
                               "The bumpers work together with the triggers, like on the Xbox."));
        body.Controls.Add(Note("GameCube: B head smash, X dig / enter / talk, RB (Z) + a trigger for soap bubble and sneak."));
        body.Controls.Add(Note("You can change this any time in Settings > Controls or in the game (F10 > Controls)."));
        var row = Row();
        row.Margin = S(0, 8, 0, 0);
        bool? answer = null;
        row.Controls.Add(new PlankButton("Original Xbox layout", (_, _) => { answer = true; dialog.Close(); }, primary: true));
        row.Controls.Add(new PlankButton("GameCube layout", (_, _) => { answer = false; dialog.Close(); }));
        body.Controls.Add(row);
        dialog.Controls.Add(body);
        dialog.ShowDialog(this);
        return answer;
    }
}
