using System.Reflection;

static class LauncherLayoutTests
{
    const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    static int checks;

    static object? Call(object target, string name, params object[] args) =>
        target.GetType().GetMethod(name, Private)!.Invoke(target, args);

    static IEnumerable<Control> Descendants(Control c) =>
        c.Controls.Cast<Control>().SelectMany(child => new[] { child }.Concat(Descendants(child)));

    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new InvalidOperationException(message);
    }

    // Simulated monitor layouts: the same cases and expected text as the game's wiicompiled/runtime/tests/monitor_labels_tests.cpp.
    static void CheckMonitorLabels(Assembly assembly)
    {
        var labels = assembly.GetType("FrightWorks.Monitors")!.GetMethod("Labels", BindingFlags.Static | BindingFlags.Public)!;
        (string, Rectangle, bool) M(string name, int x, int y, int w, int h, bool primary = false) => (name, new Rectangle(x, y, w, h), primary);
        void Case(string name, (string, Rectangle, bool)[] layout, params string[] expected)
        {
            var got = (List<string>)labels.Invoke(null, new object[] { layout.ToList() })!;
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

    [STAThread]
    static int Main(string[] args)
    {
        if (args.Length is < 1 or > 2)
        {
            Console.Error.WriteLine("Usage: LauncherLayoutTests <scratch-output-directory> [monitor-index]");
            return 2;
        }
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        string output = Path.GetFullPath(args[0]);
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

        var assembly = Assembly.Load("FrightWorks");
        var type = assembly.GetType("FrightWorks.MainForm")!;
        var paths = Activator.CreateInstance(assembly.GetType("FrightWorks.DataPaths")!, fixture, game);
        using var form = (Form)Activator.CreateInstance(type, new object?[] { null, paths })!;
        form.Text = "Launcher layout regression checks";
        Console.WriteLine($"Available monitors: {Screen.AllScreens.Length}");
        if (args.Length == 2)
        {
            if (!int.TryParse(args[1], out int monitor) || monitor < 0 || monitor >= Screen.AllScreens.Length)
            {
                Console.Error.WriteLine("Monitor index is out of range.");
                return 2;
            }
            form.StartPosition = FormStartPosition.Manual;
            var area = Screen.AllScreens[monitor].WorkingArea;
            form.Location = new Point(area.Left + 32, area.Top + 32);
        }
        form.DpiChanged += (_, e) => Console.WriteLine($"Actual monitor DPI transition: {e.DeviceDpiOld} -> {e.DeviceDpiNew}");
        int result = 0;
        form.Shown += (_, _) => form.BeginInvoke(() =>
        {
            try
            {
                var scene = form.Controls[0];
                // Controller detection runs on its own thread and rewrites the Controls status line when it finishes
                // ("Looking for controllers..." -> found / none / SDL3 missing): wait for it, or the layout can change
                // between two snapshots of the same page.
                var padStatus = (Label)type.GetField("padStatus", Private)!.GetValue(form)!;
                for (var waited = System.Diagnostics.Stopwatch.StartNew(); padStatus.Text.StartsWith("Looking") && waited.Elapsed.TotalSeconds < 10;)
                {
                    Thread.Sleep(25);
                    Application.DoEvents();
                }
                Check(!padStatus.Text.StartsWith("Looking"), "Controller detection did not finish within 10 seconds");
                scene.GetType().GetMethod("Showcase")!.Invoke(scene, new object[] { false });
                var selectTab = (Action<bool>)type.GetField("showControlsTab", Private)!.GetValue(form)!;
                var initialDpi = form.DeviceDpi;
                float nativeScale = initialDpi / 96f;
                int regionChanges = 0;
                foreach (Control board in scene.Controls) board.RegionChanged += (_, _) => regionChanges++;

                Call(form, "ShowBoard", "settings");
                selectTab(false);
                var buttonBoard = scene.Controls.Cast<Control>().Single(c => c.Visible);
                var tabButtons = Descendants(buttonBoard).OfType<Button>().Where(b => b.Text is "Display" or "Controls").ToArray();
                var tabStrip = tabButtons[0].Parent!;
                using (var strip = new Bitmap(tabStrip.Width, tabStrip.Height))
                {
                    tabStrip.DrawToBitmap(strip, new Rectangle(Point.Empty, tabStrip.Size));
                    strip.Save(Path.Combine(output, "button-borders.png"));
                }
                foreach (var button in Descendants(buttonBoard).OfType<Button>().Where(b => b.Visible).ToArray())
                {
                    Check(!(bool)Call(button, "GetStyle", ControlStyles.Opaque)!, $"{button.Text}: background painting is suppressed");
                    using var parentBackground = new Bitmap(button.Parent!.Width, button.Parent.Height);
                    using (var graphics = Graphics.FromImage(parentBackground))
                    {
                        graphics.Clear(Color.Magenta);
                        using var paint = new PaintEventArgs(graphics, button.Parent.ClientRectangle);
                        Call(button.Parent, "OnPaintBackground", paint);
                        Call(button.Parent, "OnPaint", paint);
                    }
                    void CheckCorners(string state)
                    {
                        Check(!(bool)Call(button, "GetStyle", ControlStyles.Opaque)!, $"{button.Text} ({state}): background painting is suppressed");
                        using var bitmap = new Bitmap(button.Width, button.Height);
                        using (var graphics = Graphics.FromImage(bitmap))
                        {
                            graphics.Clear(Color.Magenta);
                            using var paint = new PaintEventArgs(graphics, button.ClientRectangle);
                            Call(button, "OnPaintBackground", paint);
                            Call(button, "OnPaint", paint);
                        }
                        bitmap.Save(Path.Combine(output, $"button-{button.Text}-{state}.png"));
                        foreach (var corner in new[] { Point.Empty, new Point(button.Width - 1, 0),
                                     new Point(0, button.Height - 1), new Point(button.Width - 1, button.Height - 1) })
                        {
                            var expected = parentBackground.GetPixel(button.Left + corner.X, button.Top + corner.Y);
                            Check(bitmap.GetPixel(corner.X, corner.Y).ToArgb() == expected.ToArgb(),
                                $"{button.Text} ({state}): rectangular border at {corner}; got {bitmap.GetPixel(corner.X, corner.Y)}, expected {expected}");
                        }
                    }
                    CheckCorners("normal");
                    Call(button, "OnMouseEnter", EventArgs.Empty);
                    CheckCorners("hover");
                    Call(button, "OnMouseLeave", EventArgs.Empty);
                    Call(button, "OnMouseDown", new MouseEventArgs(MouseButtons.Left, 1, button.Width / 2, button.Height / 2, 0));
                    CheckCorners("pressed");
                    Call(button, "OnMouseUp", new MouseEventArgs(MouseButtons.Left, 1, -1, -1, 0));
                    button.Enabled = false;
                    CheckCorners("disabled");
                    button.Enabled = true;
                }

                void Validate(string name, bool controls = false)
                {
                    Call(form, "ShowBoard", name);
                    if (name == "settings") selectTab(controls);
                    Application.DoEvents();
                    var board = scene.Controls.Cast<Control>().Single(c => c.Visible);
                    var body = (Panel)board.GetType().GetField("Body")!.GetValue(board)!;
                    Check(scene.ClientRectangle.Contains(board.Bounds), $"{name}: board escapes the window after scaling");
                    Check(!body.HorizontalScroll.Visible, $"{name}: horizontal scrollbar at {form.ClientSize}");
                    foreach (var button in Descendants(board).OfType<Button>().Where(b => b.Visible))
                        Check(!(bool)Call(button, "GetStyle", ControlStyles.Opaque)!, $"{name}: button background painting was suppressed after layout/scaling");
                    foreach (var panel in Descendants(body).OfType<ScrollableControl>().Where(p => p.Visible && p.AutoScroll))
                        Check(!panel.HorizontalScroll.Visible, $"{name}: nested horizontal scrollbar");
                    foreach (var child in Descendants(body).Where(c => c.Visible && c.Parent is FlowLayoutPanel))
                        Check(child.Right + child.Margin.Right <= child.Parent!.ClientSize.Width + 1,
                            $"{name}: child exceeds its flow viewport");
                    foreach (var table in Descendants(body).OfType<TableLayoutPanel>().Where(t => t.Visible))
                    {
                        foreach (Control child in table.Controls)
                        {
                            if (!child.Visible) continue;
                            Check(child.Right + child.Margin.Right <= table.Width + 1, $"{name}: clipped {child.Text}");
                            int row = table.GetPositionFromControl(child).Row;
                            Check(table.GetRowHeights()[row] >= child.Height + child.Margin.Vertical,
                                $"{name}: row clips a control or its vertical spacing");
                            if (child is ComboBox box)
                            {
                                Check(box.Width >= 80, $"{name}: unusably narrow field");
                                Call(box, "OnDropDown", EventArgs.Empty);
                                int textWidth = box.Items.Cast<object>().Select(item =>
                                    TextRenderer.MeasureText(box.GetItemText(item), box.Font).Width).Max();
                                Check(box.DropDownWidth >= Math.Min(textWidth, Screen.FromControl(box).WorkingArea.Width),
                                    $"{name}: dropdown truncates options");
                            }
                        }
                    }
                    var firstBox = Descendants(board).OfType<ComboBox>().FirstOrDefault(c => c.Visible);
                    if (firstBox != null)
                    {
                        int selected = firstBox.SelectedIndex;
                        var wheel = Message.Create(firstBox.Handle, 0x020A, new IntPtr(-120 << 16), IntPtr.Zero);
                        Call(firstBox, "WndProc", wheel);
                        Check(firstBox.SelectedIndex == selected, "Closed dropdown changed a setting on mouse wheel");
                    }
                    string signature = string.Join("\n", Descendants(board).Where(c => c.Visible).Select(c => $"{c.GetType().Name} {c.Text} {c.Bounds}"));
                    Call(form, "CloseBoards");
                    Call(form, "ShowBoard", name);
                    Application.DoEvents();
                    string reopened = string.Join("\n", Descendants(board).Where(c => c.Visible).Select(c => $"{c.GetType().Name} {c.Text} {c.Bounds}"));
                    if (signature != reopened)
                    {
                        File.WriteAllText(Path.Combine(output, "unstable-before.txt"), signature);
                        File.WriteAllText(Path.Combine(output, "unstable-after.txt"), reopened);
                    }
                    Check(signature == reopened, $"{name}: reopening changes layout");
                    int before = regionChanges;
                    board.PerformLayout();
                    board.PerformLayout();
                    Check(before == regionChanges, $"{name}: unchanged layout replaces the native window region");
                    body.AutoScrollPosition = Point.Empty;
                    using var bitmap = new Bitmap(form.Width, form.Height);
                    form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                    string page = controls ? "controls" : name;
                    bitmap.Save(Path.Combine(output, $"{form.ClientSize.Width}-{page}.png"));
                    Console.WriteLine($"PASS {form.ClientSize.Width}x{form.ClientSize.Height} {page}: viewport {body.ClientSize}; content {body.DisplayRectangle.Size}");
                }

                foreach (var size in new[] { new Size(1180, 830), new Size(960, 680), new Size(820, 600), new Size(1180, 830) })
                {
                    // The 820 case deliberately exercises less than the normal minimum.
                    form.MinimumSize = Size.Empty;
                    form.ClientSize = new Size((int)Math.Round(size.Width * nativeScale), (int)Math.Round(size.Height * nativeScale));
                    Validate("settings");
                    Validate("settings", controls: true);
                    Validate("mods");
                    Validate("extras");
                }
                Check(File.ReadAllText(config) == initialConfig, "Opening, resizing, or navigation changed configuration");

                Call(form, "ShowBoard", "settings");
                var settings = scene.Controls.Cast<Control>().Single(c => c.Visible);
                var displayTab = Descendants(settings).OfType<Button>().Single(b => b.Text == "Display");
                Check(displayTab.TabStop && displayTab.CanSelect, "Display tab is not keyboard focusable");
                Check(displayTab.AccessibilityObject.Role == AccessibleRole.PushButton, "Themed button lacks a native button role");
                Check(Descendants(settings).OfType<Button>().Any(b => b.AccessibleName == "Close Settings"), "Close button lacks an accessible name");
                displayTab.Focus();
                Call(displayTab, "OnKeyDown", new KeyEventArgs(Keys.Space));
                Call(displayTab, "OnKeyUp", new KeyEventArgs(Keys.Space));
                Application.DoEvents();
                var display = displayTab.Parent!.Parent!.Controls.Cast<Control>().Single(c => c.Visible && Descendants(c).OfType<ComboBox>().Count() == 12);
                Check(display.Visible, "Space did not select the Display tab");
                var window = Descendants(display).OfType<ComboBox>().First();
                window.SelectedIndex = 1;
                string saved = File.ReadAllText(config);
                Check(saved.Contains("display_mode = \"windowed\""), "Changed display setting was not saved");
                Check(saved.Contains("hdr = true") && saved.Contains("hdr_paper_white_nits = 237.5") &&
                    saved.Contains("hdr_peak_nits = 1234.0") && saved.Contains("unknown_setting = 42"),
                    "Changing another setting overwrote custom HDR/unknown values");
                var monitorBox = Descendants(display).OfType<ComboBox>().ElementAt(1);
                Check((string)monitorBox.SelectedItem! == "Saved monitor (not connected)", "A missing monitor is not shown as saved");
                Check(saved.Contains("fullscreen_display = \"NOPE0000/gone\""), "Another setting dropped the saved monitor");
                monitorBox.SelectedIndex = 0;
                Check(File.ReadAllText(config).Contains("fullscreen_display = \"auto\""), "Automatic monitor was not saved");
                CheckMonitorLabels(assembly);
                var layout = (ComboBox)type.GetField("layoutBox", Private)!.GetValue(form)!;
                layout.SelectedIndex = 0;
                selectTab(true);
                var xboxLabel = Descendants(settings).OfType<Label>().Single(l => l.Text == "Black (sneak)");
                Check(!xboxLabel.Visible, "GameCube layout did not hide Xbox-only mapping");
                layout.SelectedIndex = 1;
                Check(xboxLabel.Visible, "Xbox layout did not restore Xbox-only mapping");

                var home = form.Location;
                foreach (var screen in Screen.AllScreens)
                {
                    form.Location = new Point(screen.WorkingArea.Left + 32, screen.WorkingArea.Top + 32);
                    Application.DoEvents();
                    Console.WriteLine($"Native monitor check: DPI={form.DeviceDpi}, client={form.ClientSize}");
                    Validate("settings");
                    Validate("settings", controls: true);
                    Validate("mods");
                    Validate("extras");
                }
                form.Location = home;
                Application.DoEvents();
                Check(form.DeviceDpi == initialDpi, "Returning to the original monitor did not restore DPI");

                // This is a layout stress simulation, not a physical monitor-DPI transition.
                form.AutoScaleMode = AutoScaleMode.None;
                float syntheticScale = 1;
                var fonts = Descendants(form).Prepend(form).Select(c => (Control: c, Font: c.Font)).ToArray();
                foreach (float factor in new[] { 1.25f, 1.2f, 4f / 3f, 0.5f })
                {
                    form.SuspendLayout();
                    syntheticScale *= factor;
                    assembly.GetType("FrightWorks.Theme")!.GetField("K")!.SetValue(null, nativeScale * syntheticScale);
                    form.Scale(new SizeF(factor, factor));
                    foreach (var entry in fonts)
                        entry.Control.Font = new Font(entry.Font.FontFamily, entry.Font.Size * syntheticScale, entry.Font.Style);
                    foreach (var box in Descendants(form).OfType<ComboBox>())
                        Call(box, "OnDpiChangedAfterParent", EventArgs.Empty);
                    form.ResumeLayout(true);
                    foreach (var board in scene.Controls.Cast<Control>()) board.PerformLayout();
                    Validate("settings");
                    Validate("settings", controls: true);
                    Validate("mods");
                    Validate("extras");
                    var normalSize = form.ClientSize;
                    form.ClientSize = new Size((int)Math.Round(960 * nativeScale * syntheticScale), (int)Math.Round(680 * nativeScale * syntheticScale));
                    Validate("settings");
                    Validate("settings", controls: true);
                    Validate("mods");
                    Validate("extras");
                    form.ClientSize = normalSize;
                }
                Check(form.DeviceDpi == initialDpi, "Stress test must not change operating-system DPI");
                Console.WriteLine($"PASS {checks} assertions. Actual DPI: {initialDpi}; font/size stress: 125%, 150%, 200%, back to 100% (simulated).");
            }
            catch (Exception e) { Console.Error.WriteLine(e); result = 1; }
            finally { form.Close(); }
        });
        Application.Run(form);
        return result;
    }
}
