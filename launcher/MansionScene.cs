using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Automation.Peers;
using Avalonia.Automation.Provider;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace FrightWorks;

// The launcher's main screen: a stormy night sky with lightning, the mansion (launcher/art, made by
// tools/launcher-art/make_launcher_art.py) whose windows light up, draw their curtains and show the odd silhouette,
// and the five signs that are the menu. The windows are holes in mansion.png: their insides are drawn first, then the
// house on top. The signs are drawn over the ones painted in the picture (mansion.json holds both outlines).
sealed class MansionScene : Control
{
    public event Action<string>? SignClicked;
    string? active;
    public string? ActiveSign { get => active; set { active = value; InvalidateVisual(); } }
    string status = "";
    bool statusOk;
    public void SetStatus(string text, bool ok) { status = text; statusOk = ok; InvalidateVisual(); }
    public string Status => status;

    static readonly Bitmap? HouseArt = Skin.LoadBitmap("mansion.png"), DogArt = Skin.LoadBitmap("dog.png"),
        GhostArt = Skin.LoadBitmap("ghost.png");

    readonly bool animate;
    readonly Random rnd;
    readonly Stopwatch clock = Stopwatch.StartNew();
    readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(33) };
    double shift, last;
    double Now => clock.Elapsed.TotalSeconds + shift;

    // A set of windows that light up together (the entrance's three, the tower's two) and its animation state.
    sealed class Group
    {
        public double Light = 1, LightTo = 1, Curtain, CurtainTo, Figure, FigureTo, FigureUntil;
        public double Seed;
        public bool Curtains, Lit = true;
    }
    sealed record Win(string Id, Group Group, Rect Box);
    sealed class Sign
    {
        public string Id = "";
        public Point[] Points = Array.Empty<Point>();
        public RenderTargetBitmap? Normal, Hover;
        public Point NormalAt, HoverAt;
        public Geometry? Hit;
    }
    sealed class Cloud { public RenderTargetBitmap Art = null!; public double X, Y, Speed; }

    const int ArtW = 1447, ArtH = 1087;
    readonly List<Win> windows = new();
    readonly Dictionary<string, Group> groups = new();
    readonly List<Sign> signs = new();
    readonly List<Cloud> clouds = new();
    static readonly HashSet<string> CurtainGroups = new() { "parlor", "parlor_side", "dormer", "right_arch", "right_tower", "tower" };

    double s = 1;
    Point o;
    Size cachedFor;
    double cachedScale;
    RenderTargetBitmap? house, sky, title;
    double nextStrike, strikeAt = -100, nextEvent;
    List<Point[]>? bolt;
    string? hover;
    int focusedSign;
    readonly List<SignButton> signButtons = new();
    internal string FocusedSign => signs[focusedSign].Id;

    double W => Bounds.Width;
    double H => Bounds.Height;

    public MansionScene(bool animate, int seed)
    {
        this.animate = animate;
        rnd = new Random(seed);
        ClipToBounds = true;
        Focusable = true;
        FocusAdorner = null;  // the focused sign is drawn in OnRender; the theme's focus rectangle would box it
        LoadLayout();
        foreach (var sign in signs)
        {
            var button = new SignButton(this, sign.Id);
            signButtons.Add(button);
            LogicalChildren.Add(button);
            VisualChildren.Add(button);
        }
        foreach (var (id, g) in groups)
        {
            g.Seed = rnd.NextDouble() * 100;
            g.Curtains = CurtainGroups.Contains(id);
        }
        // A lived-in start: most rooms lit, a couple dark, some curtains half drawn.
        foreach (var id in new[] { "gable", "right_tower" }) Set(id, light: 0);
        Set("dormer", curtain: 0.55);
        Set("right_arch", curtain: 1);
        nextStrike = 3 + rnd.NextDouble() * 5;
        nextEvent = 1.5;
        timer.Tick += (_, _) => Step();
    }

    // Windows' "Animate controls and elements inside windows" (off = a still scene); always on elsewhere.
    public static bool AnimationsEnabled()
    {
        if (!OperatingSystem.IsWindows()) return true;
        const uint SPI_GETUIEFFECTS = 0x103E;
        return !SystemParametersInfo(SPI_GETUIEFFECTS, 0, out int on, 0) || on != 0;
    }

    [DllImport("user32.dll")]
    static extern bool SystemParametersInfo(uint action, uint param, out int value, uint winIni);

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        if (animate) timer.Start();
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        timer.Stop();
        base.OnDetachedFromVisualTree(e);
    }

    void Set(string id, double? light = null, double? curtain = null, double? figure = null)
    {
        if (!groups.TryGetValue(id, out var g)) return;
        if (light is double l) { g.Light = g.LightTo = l; g.Lit = l > 0.5; }
        if (curtain is double c) g.Curtain = g.CurtainTo = c;
        if (figure is double f) { g.Figure = g.FigureTo = f; g.FigureUntil = f > 0 ? double.MaxValue : 0; }
    }

    void LoadLayout()
    {
        using var stream = Skin.Resource("mansion.json");
        if (stream == null) return;
        using var doc = JsonDocument.Parse(stream);
        var root = doc.RootElement;
        foreach (var w in root.GetProperty("windows").EnumerateArray())
        {
            var gid = w.GetProperty("group").GetString()!;
            if (!groups.TryGetValue(gid, out var g)) groups[gid] = g = new Group();
            windows.Add(new Win(w.GetProperty("id").GetString()!, g,
                new Rect(w.GetProperty("x").GetDouble(), w.GetProperty("y").GetDouble(), w.GetProperty("w").GetDouble(), w.GetProperty("h").GetDouble())));
        }
        foreach (var sg in root.GetProperty("signs").EnumerateArray())
            signs.Add(new Sign
            {
                Id = sg.GetProperty("id").GetString()!,
                Points = sg.GetProperty("points").EnumerateArray().Select(p => new Point(p[0].GetDouble(), p[1].GetDouble())).ToArray(),
            });
    }

    // ----- Screenshots (--shots): a fixed, busy moment -----
    public void Showcase(bool lightning)
    {
        timer.Stop();
        foreach (var g in groups.Values) { g.Light = g.LightTo = 1; g.Curtain = g.CurtainTo = 0; g.Figure = g.FigureTo = 0; }
        Set("parlor", figure: 1);
        Set("tower", figure: 1);
        Set("dormer", curtain: 0.55);
        Set("right_arch", curtain: 1);
        Set("gable", light: 0);
        shift = 40 - clock.Elapsed.TotalSeconds;
        if (lightning)
        {
            strikeAt = Now - 0.05;
            bolt = MakeBolt(new Random(3));
        }
        else strikeAt = -100;
        InvalidateVisual();
    }

    // ----- Animation -----
    void Step()
    {
        if (TopLevel.GetTopLevel(this) is Window { WindowState: WindowState.Minimized } || !IsEffectivelyVisible) return;
        double t = Now, dt = Math.Clamp(t - last, 0, 0.1);
        last = t;
        foreach (var g in groups.Values)
        {
            g.Light = Approach(g.Light, g.LightTo, dt * 2.5);
            g.Curtain = Approach(g.Curtain, g.CurtainTo, dt * 0.7);
            if (g.FigureTo > 0 && t > g.FigureUntil) g.FigureTo = 0;
            g.Figure = Approach(g.Figure, g.FigureTo, dt * 1.3);
        }
        if (t >= nextEvent) { WindowEvent(t); nextEvent = t + 1.2 + rnd.NextDouble() * 2.8; }
        if (t >= nextStrike)
        {
            strikeAt = t;
            bolt = MakeBolt(rnd);
            nextStrike = t + 9 + rnd.NextDouble() * 14;
        }
        foreach (var c in clouds)
        {
            c.X += c.Speed * dt;
            if (c.X > W) c.X = -c.Art.Size.Width;
        }
        InvalidateVisual();
    }

    static double Approach(double v, double to, double step) => v < to ? Math.Min(to, v + step) : Math.Max(to, v - step);

    void WindowEvent(double t)
    {
        var ids = groups.Keys.ToList();
        var id = ids[rnd.Next(ids.Count)];
        var g = groups[id];
        double r = rnd.NextDouble();
        if (id == "entrance")
        {
            g.Light = 0.3;  // a flicker; it comes straight back
            return;
        }
        if ((id == "parlor" || id == "tower") && g.LightTo > 0.5 && g.FigureTo == 0 && r < 0.45)
        {
            g.FigureTo = 1;
            g.FigureUntil = t + 3 + rnd.NextDouble() * 4;
            return;
        }
        if (g.Curtains && g.LightTo > 0.5 && r < 0.75)
        {
            g.CurtainTo = g.CurtainTo > 0.3 ? 0 : rnd.NextDouble() < 0.5 ? 1 : 0.55;
            return;
        }
        int lit = groups.Values.Count(x => x.LightTo > 0.5);
        if (g.LightTo > 0.5 && lit > 5) { g.LightTo = 0; g.FigureTo = 0; }
        else g.LightTo = 1;
    }

    double Flash()
    {
        double e = Now - strikeAt;
        if (e < 0 || e > 1) return 0;
        static double Pulse(double x, double at, double width) => Math.Exp(-Math.Pow((x - at) / width, 2));
        return Math.Min(1, Pulse(e, 0.05, 0.04) + 0.7 * Pulse(e, 0.22, 0.05) + 0.35 * Pulse(e, 0.42, 0.09));
    }

    List<Point[]> MakeBolt(Random r)
    {
        var paths = new List<Point[]>();
        double w = Math.Max(1, W), h = Math.Max(1, H), step = Math.Max(6, 18 * s);
        double x = w * (0.08 + r.NextDouble() * 0.84), y = 0;
        var main = new List<Point> { new(x, y) };
        while (y < h * 0.6)
        {
            y += step * (0.6 + r.NextDouble());
            x += (r.NextDouble() * 2 - 1) * step * 1.1;
            main.Add(new Point(x, y));
        }
        paths.Add(main.ToArray());
        for (int b = 0; b < 2; b++)
        {
            var from = main[2 + r.Next(Math.Max(1, main.Count - 4))];
            double bx = from.X, by = from.Y, dir = r.NextDouble() < 0.5 ? -1 : 1;
            var branch = new List<Point> { from };
            for (int i = 0; i < 5 + r.Next(4); i++)
            {
                by += step * (0.5 + r.NextDouble() * 0.7);
                bx += dir * step * (0.4 + r.NextDouble());
                branch.Add(new Point(bx, by));
            }
            paths.Add(branch.ToArray());
        }
        return paths;
    }

    // ----- Layout and cached layers -----
    Point Map(double x, double y) => new(o.X + x * s, o.Y + y * s);
    Rect Map(Rect r) => new(o.X + r.X * s, o.Y + r.Y * s, r.Width * s, r.Height * s);

    // The house fills the height, sits at the bottom and leans right, leaving the sky on the left for the title.
    public Rect HouseRect => new(o.X, o.Y, ArtW * s, ArtH * s);
    public double SignsLeft => signs.Count == 0 ? W * 0.5 : signs.Min(sg => sg.Points.Min(p => p.X)) * s + o.X;

    // Picture-to-client scale and offset, for a size (the boards are placed from it before the first paint).
    public void Place(Size size)
    {
        double w = Math.Max(1, size.Width), h = Math.Max(1, size.Height);
        s = Math.Min(w * 0.99 / ArtW, (h - 6) / ArtH);
        o = new Point(Math.Round((w - ArtW * s) * 0.82), Math.Round(h - ArtH * s));
    }

    double Scaling => TopLevel.GetTopLevel(this)?.RenderScaling ?? 1;

    // A cached layer drawn at the screen's pixel density, so it stays sharp with display scaling.
    RenderTargetBitmap Layer(double w, double h, Action<DrawingContext> draw)
    {
        double scale = cachedScale;
        var size = new PixelSize(Math.Max(1, (int)Math.Ceiling(w * scale)), Math.Max(1, (int)Math.Ceiling(h * scale)));
        var bitmap = new RenderTargetBitmap(size, new Vector(96 * scale, 96 * scale));
        using (var g = bitmap.CreateDrawingContext())
        using (g.PushRenderOptions(new RenderOptions { BitmapInterpolationMode = BitmapInterpolationMode.HighQuality, EdgeMode = EdgeMode.Antialias }))
            draw(g);
        return bitmap;
    }

    void Rebuild()
    {
        cachedFor = Bounds.Size;
        cachedScale = Scaling;
        Place(Bounds.Size);

        house?.Dispose();
        double hw = Math.Max(1, Math.Round(ArtW * s)), hh = Math.Max(1, Math.Round(ArtH * s));
        house = Layer(hw, hh, g => { if (HouseArt != null) g.DrawImage(HouseArt, new Rect(0, 0, hw, hh)); });

        BuildSky();
        BuildClouds();
        BuildTitle();
        foreach (var sg in signs) BuildSign(sg);
    }

    void BuildSky()
    {
        sky?.Dispose();
        double w = Math.Max(1, W), h = Math.Max(1, H);
        sky = Layer(w, h, g =>
        {
            var all = new Rect(0, 0, w, h);
            g.FillRectangle(new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
                EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
                GradientStops =
                {
                    new GradientStop(Color.Parse("#05030B"), 0), new GradientStop(Color.Parse("#140C28"), 0.45),
                    new GradientStop(Color.Parse("#2A1A45"), 0.8), new GradientStop(Color.Parse("#331F4E"), 1),
                },
            }, all);
            var stars = new Random(7);
            for (int i = 0; i < 160; i++)
            {
                double x = stars.NextDouble() * w, y = Math.Pow(stars.NextDouble(), 1.6) * h * 0.75;
                double d = stars.NextDouble() < 0.15 ? 2.2 : 1.3;
                g.DrawEllipse(Skin.Brush(Skin.Argb(60 + stars.Next(150), 235, 230, 255)), null, new Rect(x, y, d, d));
            }
            // The moon, above the right tower.
            var m = Map(1300, 105);
            double mr = Math.Max(18, 52 * s);
            g.DrawEllipse(new RadialGradientBrush
            {
                GradientStops = { new GradientStop(Skin.Argb(70, 230, 225, 255), 0), new GradientStop(Skin.Argb(0, 230, 225, 255), 1) },
            }, null, new Rect(m.X - mr * 3.2, m.Y - mr * 3.2, mr * 6.4, mr * 6.4));
            g.DrawEllipse(Skin.Brush(Color.Parse("#EEEAD3")), null, new Rect(m.X - mr, m.Y - mr, mr * 2, mr * 2));
            var crater = Skin.Brush(Color.Parse("#D7D2B7"));
            g.DrawEllipse(crater, null, new Rect(m.X - mr * 0.45, m.Y - mr * 0.35, mr * 0.42, mr * 0.42));
            g.DrawEllipse(crater, null, new Rect(m.X + mr * 0.2, m.Y + mr * 0.15, mr * 0.3, mr * 0.3));
            g.DrawEllipse(crater, null, new Rect(m.X - mr * 0.1, m.Y + mr * 0.45, mr * 0.22, mr * 0.22));
            // Rolling hills along the bottom, behind the house.
            var pts = new List<Point> { new(0, h) };
            for (int i = 0; i <= 12; i++)
                pts.Add(new Point(w * i / 12, h * (0.86 + 0.04 * Math.Sin(i * 1.7 + 0.5))));
            pts.Add(new Point(w, h));
            g.DrawGeometry(Skin.Brush(Color.Parse("#0C0816")), null, Skin.Curve(pts, 0.4, filled: true));
        });
    }

    void BuildClouds()
    {
        foreach (var c in clouds) c.Art.Dispose();
        clouds.Clear();
        var r = new Random(11);
        for (int i = 0; i < 6; i++)
        {
            double w = (int)((240 + r.Next(220)) * s), h = (int)((60 + r.Next(50)) * s);
            if (w < 8 || h < 8) continue;
            var art = Layer(w, h, g =>
            {
                for (int k = 0; k < 9; k++)
                {
                    double pw = w * (0.3 + r.NextDouble() * 0.35), ph = h * (0.5 + r.NextDouble() * 0.5);
                    double px = r.NextDouble() * (w - pw), py = h - ph - r.NextDouble() * (h - ph) * 0.6;
                    g.DrawEllipse(new RadialGradientBrush
                    {
                        GradientStops = { new GradientStop(Skin.Argb(120, 58, 46, 88), 0), new GradientStop(Skin.Argb(0, 58, 46, 88), 1) },
                    }, null, new Rect(px, py, pw, ph));
                }
            });
            clouds.Add(new Cloud { Art = art, X = r.NextDouble() * W, Y = H * (0.02 + r.NextDouble() * 0.38), Speed = (4 + r.NextDouble() * 9) * Math.Max(0.5, s) });
        }
    }

    // The title, top left in the sky: fitted to the space left of the chimney.
    void BuildTitle()
    {
        title?.Dispose();
        double room = Math.Max(160, o.X + 318 * s - 22);
        double em = 46;
        double w = Skin.TextGeometry("FRIGHTWORKS", em, new Rect(0, 0, 4000, 400), Skin.Align.Near).Bounds.Width;
        if (w > room) em *= room / w;
        double width = (int)room + 30, height = (int)(em * 2.3) + 10;
        title = Layer(width, height, g =>
        {
            // Layout boxes far wider than the text, as the old GDI+ drawing used.
            Skin.Text(g, "FRIGHTWORKS", em, new Rect(6, 0, 4000, em * 1.25), Skin.Lime, Skin.Ink, Math.Max(3, em * 0.16), Skin.Align.Near);
            Skin.Text(g, "Night of 100 Frights", em * 0.52, new Rect(8, em * 1.12, 4000, em * 0.8), Skin.Cream, Skin.Ink, Math.Max(2, em * 0.11), Skin.Align.Near);
        });
    }

    // ----- Signs -----
    static readonly Dictionary<string, string> SignText = new()
    {
        ["play"] = "Play", ["settings"] = "Settings", ["mods"] = "Mods", ["extras"] = "Extras", ["exit"] = "Exit",
    };

    Point[] SignPoints(Sign sg, double grow)
    {
        double cx = sg.Points.Average(p => p.X), cy = sg.Points.Average(p => p.Y);
        return sg.Points.Select(p => Map(cx + (p.X - cx) * grow, cy + (p.Y - cy) * grow)).ToArray();
    }

    void BuildSign(Sign sg)
    {
        sg.Normal?.Dispose();
        sg.Hover?.Dispose();
        (sg.Normal, sg.NormalAt) = RenderSign(sg, false);
        (sg.Hover, sg.HoverAt) = RenderSign(sg, true);
        sg.Hit = Skin.Polygon(SignPoints(sg, 1));
    }

    (RenderTargetBitmap, Point) RenderSign(Sign sg, bool lit)
    {
        var pts = SignPoints(sg, lit ? 1.045 : 1);
        double minX = pts.Min(p => p.X), minY = pts.Min(p => p.Y), maxX = pts.Max(p => p.X), maxY = pts.Max(p => p.Y);
        int pad = (int)Math.Ceiling(6 * s) + 2;
        var at = new Point(Math.Floor(minX) - pad, Math.Floor(minY) - pad);
        double bw = Math.Max(1, Math.Ceiling(maxX - minX) + pad * 2), bh = Math.Max(1, Math.Ceiling(maxY - minY) + pad * 2);
        var bitmap = Layer(bw, bh, g =>
        {
            using var moved = g.PushTransform(Matrix.CreateTranslation(-at.X, -at.Y));
            var box = new Rect(minX, minY, maxX - minX, maxY - minY);
            bool play = sg.Id == "play";
            double u = s * (lit ? 1.045 : 1);  // picture pixels to client pixels

            var shape = Skin.Polygon(pts);
            Color top = play ? Color.Parse("#DDF063") : Color.Parse("#7B4CBA");
            Color bottom = play ? Color.Parse("#A3BE33") : Color.Parse("#4B2884");
            if (lit) { top = Skin.Light(top, play ? 0.35f : 0.3f); bottom = Skin.Light(bottom, 0.3f); }
            g.DrawGeometry(Skin.Vertical(top, bottom), null, shape);

            // Wood grain and a couple of knots, the same every time for a sign.
            var r = new Random(sg.Id.Length * 31 + sg.Id[0]);
            using (g.PushGeometryClip(shape))
            {
                var grain = new Pen(Skin.Brush(Skin.Argb(play ? 55 : 70, 20, 8, 30)), Math.Max(1, 2.2 * u));
                for (int i = 0; i < 6; i++)
                {
                    double y = box.Top + box.Height * (i + 0.5) / 6 + (r.NextDouble() * 6 - 3) * u;
                    var line = new Point[8];
                    for (int k = 0; k < line.Length; k++)
                        line[k] = new Point(box.Left + box.Width * k / (line.Length - 1), y + (r.NextDouble() * 5 - 2.5) * u);
                    g.DrawGeometry(null, grain, Skin.Curve(line, 0.5, filled: false));
                }
                for (int k = 0; k < 2; k++)
                {
                    double kx = box.Left + box.Width * (0.25 + r.NextDouble() * 0.5), ky = box.Top + box.Height * (0.25 + r.NextDouble() * 0.5);
                    g.DrawEllipse(null, grain, new Rect(kx, ky, 18 * u, 7 * u));
                }
                g.DrawLine(new Pen(Skin.Brush(Skin.Argb(80, 255, 255, 255)), 3 * u),
                           new Point(box.Left + 14 * u, box.Top + 7 * u), new Point(box.Right - 14 * u, box.Top + 6 * u));
            }
            g.DrawGeometry(null, new Pen(Skin.Brush(Skin.Ink), 6.5 * u, lineJoin: PenLineJoin.Round), shape);
            double n = 4.5 * u;
            foreach (var (nx, ny) in new[] { (box.Left + 17 * u, box.Top + 18 * u), (box.Left + 17 * u, box.Bottom - 18 * u), (box.Right - 17 * u, box.Top + 18 * u), (box.Right - 17 * u, box.Bottom - 18 * u) })
                Skin.Nail(g, nx, ny, n);

            double cy = box.Top + box.Height / 2;
            if (play)
            {
                DrawPaw(g, box.Left + box.Width * 0.13, cy, 30 * u, Skin.Ink);
                DrawPaw(g, box.Right - box.Width * 0.13, cy, 30 * u, Skin.Ink);
                FitText(g, SignText[sg.Id], 92 * u, new Rect(box.Left + box.Width * 0.24, box.Top, box.Width * 0.52, box.Height), Skin.Ink, default, 0);
            }
            else
            {
                DrawIcon(g, sg.Id, box.Left + box.Width * 0.2, cy, u);
                FitText(g, SignText.GetValueOrDefault(sg.Id, sg.Id), 54 * u, new Rect(box.Left + box.Width * 0.3, box.Top, box.Width * 0.62, box.Height),
                        Skin.Cream, Skin.Ink, 6 * u);
            }
        });
        return (bitmap, at);
    }

    static void FitText(DrawingContext g, string text, double em, Rect box, Color fill, Color outline, double outlineWidth)
    {
        double w = Skin.TextGeometry(text, em, new Rect(0, 0, 4000, 1000)).Bounds.Width;
        if (w > box.Width * 0.92) em *= box.Width * 0.92 / w;
        Skin.Text(g, text, em, box, fill, outline, outlineWidth);
    }

    static void DrawPaw(DrawingContext g, double cx, double cy, double size, Color color)
    {
        var b = Skin.Brush(color);
        double u = size / 30;
        g.DrawEllipse(b, null, new Rect(cx - 15 * u, cy - 2 * u, 30 * u, 24 * u));
        foreach (var (dx, dy, w, h) in new[] { (-21.0, -14.0, 11.0, 14.0), (-9.0, -26.0, 12.0, 15.0), (5.0, -26.0, 12.0, 15.0), (17.0, -14.0, 11.0, 14.0) })
            g.DrawEllipse(b, null, new Rect(cx + dx * u - 1, cy + dy * u, w * u, h * u));
    }

    // The outline goes first and the cream over it, so half of the outline shows, as on the painted signs.
    static void Outlined(DrawingContext g, Geometry shape, Pen ink, IBrush cream)
    {
        g.DrawGeometry(null, ink, shape);
        g.DrawGeometry(cream, null, shape);
    }

    static void DrawIcon(DrawingContext g, string id, double cx, double cy, double u)
    {
        var cream = Skin.Brush(Skin.Cream);
        var ink = new Pen(Skin.Brush(Skin.Ink), 4 * u, lineJoin: PenLineJoin.Round);
        switch (id)
        {
            case "settings":
            {
                const int teeth = 8;
                var pts = new List<Point>();
                for (int i = 0; i < teeth * 2; i++)
                {
                    double a0 = Math.PI * 2 * i / (teeth * 2) - 0.12, a1 = a0 + 0.24;
                    double rad = (i % 2 == 0 ? 25 : 18) * u;
                    pts.Add(new Point(cx + Math.Cos(a0) * rad, cy + Math.Sin(a0) * rad));
                    pts.Add(new Point(cx + Math.Cos(a1) * rad, cy + Math.Sin(a1) * rad));
                }
                Outlined(g, new GeometryGroup
                {
                    FillRule = FillRule.EvenOdd,
                    Children = { Skin.Polygon(pts), new EllipseGeometry(new Rect(cx - 8 * u, cy - 8 * u, 16 * u, 16 * u)) },
                }, ink, cream);
                break;
            }
            case "mods":
            {
                // A jigsaw piece: a square with a knob on top and on the right.
                Outlined(g, new GeometryGroup
                {
                    FillRule = FillRule.NonZero,
                    Children =
                    {
                        new RectangleGeometry(new Rect(cx - 20 * u, cy - 12 * u, 34 * u, 34 * u)),
                        new EllipseGeometry(new Rect(cx - 11 * u, cy - 27 * u, 17 * u, 18 * u)),
                        new EllipseGeometry(new Rect(cx + 9 * u, cy - 4 * u, 18 * u, 17 * u)),
                    },
                }, ink, cream);
                break;
            }
            case "extras":
            {
                var thick = new Pen(Skin.Brush(Skin.Ink), 13 * u, lineCap: PenLineCap.Round);
                var handle = new Pen(cream, 7 * u, lineCap: PenLineCap.Round);
                g.DrawLine(thick, new Point(cx + 9 * u, cy + 9 * u), new Point(cx + 22 * u, cy + 22 * u));
                g.DrawLine(handle, new Point(cx + 9 * u, cy + 9 * u), new Point(cx + 22 * u, cy + 22 * u));
                Outlined(g, new GeometryGroup
                {
                    FillRule = FillRule.EvenOdd,
                    Children =
                    {
                        new EllipseGeometry(new Rect(cx - 21 * u, cy - 21 * u, 34 * u, 34 * u)),
                        new EllipseGeometry(new Rect(cx - 13 * u, cy - 13 * u, 18 * u, 18 * u)),
                    },
                }, ink, cream);
                break;
            }
            case "exit":
            {
                // A door frame with the door swung open.
                Outlined(g, new GeometryGroup
                {
                    FillRule = FillRule.EvenOdd,
                    Children =
                    {
                        new RectangleGeometry(new Rect(cx - 15 * u, cy - 24 * u, 30 * u, 48 * u)),
                        new RectangleGeometry(new Rect(cx - 10 * u, cy - 19 * u, 20 * u, 43 * u)),
                    },
                }, ink, cream);
                Outlined(g, Skin.Polygon(new[] { new Point(cx - 10 * u, cy - 19 * u), new Point(cx + 4 * u, cy - 14 * u), new Point(cx + 4 * u, cy + 27 * u), new Point(cx - 10 * u, cy + 24 * u) }),
                         ink, cream);
                g.DrawEllipse(Skin.Brush(Skin.Ink), null, new Rect(cx - 1 * u, cy + 1 * u, 4 * u, 4 * u));
                break;
            }
        }
    }

    // ----- Painting -----
    public override void Render(DrawingContext g)
    {
        if (W <= 0 || H <= 0) return;
        if (cachedFor != Bounds.Size || cachedScale != Scaling || house == null) Rebuild();
        double t = Now;
        double flash = Flash();
        var all = new Rect(Bounds.Size);

        g.DrawImage(sky!, new Rect(sky!.Size));
        foreach (var c in clouds) g.DrawImage(c.Art, new Rect(new Point((int)c.X, (int)c.Y), c.Art.Size));
        if (flash > 0)
        {
            g.FillRectangle(Skin.Brush(Skin.Argb((int)(120 * flash), 196, 186, 255)), all);
            if (bolt != null && t - strikeAt < 0.35)
                foreach (var line in bolt)
                {
                    var glow = new Pen(Skin.Brush(Skin.Argb((int)(90 * flash), 210, 200, 255)), Math.Max(4, 11 * s), lineJoin: PenLineJoin.Round);
                    var core = new Pen(Skin.Brush(Skin.Argb((int)(255 * Math.Min(1, flash * 1.4)), 255, 255, 255)), Math.Max(1.5, 3 * s), lineJoin: PenLineJoin.Round);
                    var geometry = Skin.Polyline(line);
                    g.DrawGeometry(null, glow, geometry);
                    g.DrawGeometry(null, core, geometry);
                }
        }

        foreach (var w in windows) DrawWindow(g, w, t);

        var houseRect = new Rect(new Point((int)o.X, (int)o.Y), house!.Size);
        g.DrawImage(house, houseRect);
        if (flash > 0.02)
        {
            // The lightning lights the house: a pale silhouette of it over it.
            using (g.PushOpacityMask(new ImageBrush(house) { Stretch = Stretch.Fill }, houseRect))
                g.FillRectangle(Skin.Brush(Skin.Argb((int)(255 * 0.3 * flash), 217, 217, 255)), houseRect);
        }

        foreach (var sg in signs)
        {
            bool focused = sg.Id == FocusedSign && (IsFocused || signButtons.Any(b => b.IsFocused));
            bool lit = sg.Id == hover || sg.Id == active || focused;
            double breathe = (Math.Sin(t * 2.6) + 1) / 2;
            if (sg.Id == "play" || lit)
            {
                int a = lit ? 150 : (int)(40 + 70 * breathe);
                Color glowColor = sg.Id == "play" ? Skin.Lime : Skin.Gold;
                g.DrawGeometry(null, new Pen(Skin.Brush(Skin.WithAlpha(glowColor, a)), Math.Max(6, 16 * s), lineJoin: PenLineJoin.Round),
                               Skin.Polygon(SignPoints(sg, lit ? 1.045 : 1)));
            }
            if (lit) g.DrawImage(sg.Hover!, new Rect(sg.HoverAt, sg.Hover!.Size));
            else g.DrawImage(sg.Normal!, new Rect(sg.NormalAt, sg.Normal!.Size));
            if (focused)
                g.DrawGeometry(null, new Pen(Skin.Brush(Skin.Cream), Math.Max(1, 2 * s), new DashStyle(new double[] { 2, 2 }, 0)),
                               Skin.Polygon(SignPoints(sg, 1.045)));
        }

        g.DrawImage(title!, new Rect(new Point(18, 12), title!.Size));
        DrawFooter(g);
    }

    static readonly Color Dark = Color.Parse("#0D0E1E"), WarmTop = Color.Parse("#FFE27C"), WarmBottom = Color.Parse("#F39A2B");

    void DrawWindow(DrawingContext g, Win w, double t)
    {
        var b = Map(w.Box).Inflate(1);
        var gr = w.Group;
        double light = gr.Light * (0.95 + 0.05 * Math.Sin(t * 11 + gr.Seed) * Math.Sin(t * 6.7 + gr.Seed * 2));
        light = Math.Clamp(light, 0, 1);
        g.FillRectangle(Skin.Brush(Dark), b);
        if (light > 0.01)
            g.FillRectangle(Skin.Vertical(Skin.WithAlpha(WarmTop, (int)(255 * light)), Skin.WithAlpha(WarmBottom, (int)(255 * light))), b);
        var figure = w.Id == "parlor" ? DogArt : w.Id == "tower" ? GhostArt : null;
        double fig = gr.Figure;
        if (figure != null && fig > 0.01) DrawFaded(g, figure, Map(w.Box), fig);
        double curtain = gr.Curtain;
        if (gr.Curtains && curtain > 0.01)
        {
            DrawCurtains(g, b, curtain, light);
            if (figure != null && fig > 0.01) DrawFaded(g, figure, Map(w.Box), fig * curtain * 0.5 * light);  // its shadow on the curtain
        }
    }

    static void DrawFaded(DrawingContext g, Bitmap art, Rect dest, double alpha)
    {
        using (g.PushOpacity(Math.Clamp(alpha, 0, 1)))
            g.DrawImage(art, new Rect(Math.Round(dest.X), Math.Round(dest.Y), Math.Round(dest.Width), Math.Round(dest.Height)));
    }

    static void DrawCurtains(DrawingContext g, Rect b, double amount, double light)
    {
        Color shade = Lerp(Color.Parse("#2E0B22"), Color.Parse("#B4453E"), light);
        Color fold = Lerp(Color.Parse("#1C0616"), Color.Parse("#7E2830"), light);
        double half = b.Width / 2 + 1, cw = half * amount;
        foreach (var left in new[] { true, false })
        {
            var r = left ? new Rect(b.Left, b.Top, cw, b.Height) : new Rect(b.Right - cw, b.Top, cw, b.Height);
            if (r.Width < 0.5) continue;
            g.FillRectangle(Skin.Brush(shade), r);
            var pen = new Pen(Skin.Brush(fold), Math.Max(1, r.Width / 9));
            for (int i = 1; i < 4; i++)
            {
                double x = r.Left + r.Width * i / 4;
                g.DrawLine(pen, new Point(x, r.Top), new Point(x, r.Bottom));
            }
        }
        g.FillRectangle(Skin.Brush(fold), new Rect(b.Left, b.Top, b.Width, Math.Max(2, b.Height * 0.06 * Math.Min(1, amount * 3))));
    }

    static Color Lerp(Color a, Color b, double f) => Color.FromRgb(
        (byte)(a.R + (b.R - a.R) * f), (byte)(a.G + (b.G - a.G) * f), (byte)(a.B + (b.B - a.B) * f));

    void DrawFooter(DrawingContext g)
    {
        var face = new Typeface(Skin.BodyFamily, FontStyle.Normal, FontWeight.Bold);
        const double pad = 8, side = 14, gap = 10, extra = 6;  // extra: TextRenderer's own side padding
        double y = H - 30;
        FormattedText Text(string text, Color color) =>
            new(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, face, Skin.SmallSize, Skin.Brush(color));
        void Pill(FormattedText text, double x)
        {
            var r = new Rect(x, y, Math.Ceiling(text.Width) + extra + pad * 2, 24);
            g.DrawGeometry(Skin.Brush(Skin.Argb(185, 10, 6, 18)), null, Skin.Rounded(r, 11));
            g.DrawText(text, new Point(r.X + (r.Width - text.Width) / 2, r.Y + (r.Height - text.Height) / 2));
        }
        // Bottom right: the status. Bottom left: the build ID, shortened with an ellipsis when the window is too narrow
        // for both.
        double statusLeft = W - side;
        if (status.Length > 0)
        {
            var text = Text(status, statusOk ? Skin.Good : Skin.Warn);
            statusLeft = W - side - (Math.Ceiling(text.Width) + extra + pad * 2);
            Pill(text, statusLeft);
        }
        if (BuildId.Length > 0)
        {
            var text = Text(BuildId, Skin.CreamDim);
            double room = statusLeft - gap - side - extra - pad * 2;
            if (room < 40) return;
            if (text.Width > room)
            {
                text.MaxTextWidth = room;
                text.MaxLineCount = 1;
                text.Trimming = TextTrimming.CharacterEllipsis;
            }
            Pill(text, side);
        }
    }

    // The build ID, the same as the game's for one build: just the version for a release, else
    // "<version> branch@commit[+diff hash]" (set at build time: InformationalVersion in the csproj).
    public static readonly string BuildId = typeof(MansionScene).Assembly
        .GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
        .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion ?? "";

    // ----- Input -----
    // The middle of a sign (tests click it).
    internal Point? SignCenter(string id) =>
        signs.FirstOrDefault(sg => sg.Id == id) is { } sg ? Map(sg.Points.Average(p => p.X), sg.Points.Average(p => p.Y)) : null;

    string? SignAt(Point p) => signs.LastOrDefault(sg => sg.Hit != null && sg.Hit.FillContains(p))?.Id;

    protected override Size ArrangeOverride(Size finalSize)
    {
        Place(finalSize);
        for (int i = 0; i < signs.Count; i++)
        {
            var points = signs[i].Points.Select(p => Map(p.X, p.Y)).ToArray();
            var rect = new Rect(new Point(points.Min(p => p.X), points.Min(p => p.Y)),
                                new Point(points.Max(p => p.X), points.Max(p => p.Y)));
            signButtons[i].Measure(rect.Size);
            signButtons[i].Arrange(rect);
        }
        return finalSize;
    }

    internal void Navigate(LauncherButtons action)
    {
        if (action == LauncherButtons.Confirm) { SignClicked?.Invoke(FocusedSign); return; }
        if (action is not (LauncherButtons.Up or LauncherButtons.Down or LauncherButtons.Left or LauncherButtons.Right or LauncherButtons.Previous or LauncherButtons.Next)) return;
        int direction = action is LauncherButtons.Up or LauncherButtons.Left or LauncherButtons.Previous ? -1 : 1;
        focusedSign = (focusedSign + direction + signs.Count) % signs.Count;
        signButtons[focusedSign].Focus(NavigationMethod.Directional);
        InvalidateVisual();
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        var action = e.Key switch
        {
            Key.Up or Key.Left => LauncherButtons.Previous,
            Key.Down or Key.Right => LauncherButtons.Next,
            Key.Enter or Key.Space => LauncherButtons.Confirm,
            _ => LauncherButtons.None,
        };
        if (action != LauncherButtons.None) { Navigate(action); e.Handled = true; }
    }

    protected override void OnGotFocus(FocusChangedEventArgs e) { base.OnGotFocus(e); InvalidateVisual(); }
    protected override void OnLostFocus(FocusChangedEventArgs e) { base.OnLostFocus(e); InvalidateVisual(); }

    protected override AutomationPeer OnCreateAutomationPeer() => new MenuPeer(this);

    sealed class MenuPeer : ControlAutomationPeer
    {
        public MenuPeer(MansionScene owner) : base(owner) { }
        protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Menu;
        protected override string? GetNameCore() => "Launcher menu";
    }

    sealed class SignButton : Control
    {
        readonly MansionScene scene;
        readonly string id;
        public SignButton(MansionScene scene, string id)
        {
            this.scene = scene;
            this.id = id;
            Focusable = true;
            FocusAdorner = null;  // OnRender outlines the sign's own shape; the default box would cover its bounding rectangle
            IsHitTestVisible = false;
            AutomationProperties.SetName(this, char.ToUpperInvariant(id[0]) + id[1..]);
        }
        protected override void OnGotFocus(FocusChangedEventArgs e)
        {
            base.OnGotFocus(e);
            scene.focusedSign = scene.signs.FindIndex(s => s.Id == id);
            scene.InvalidateVisual();
        }
        protected override void OnLostFocus(FocusChangedEventArgs e) { base.OnLostFocus(e); scene.InvalidateVisual(); }
        protected override AutomationPeer OnCreateAutomationPeer() => new Peer(this);
        sealed class Peer : ControlAutomationPeer, IInvokeProvider
        {
            public Peer(SignButton owner) : base(owner) { }
            protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Button;
            protected override bool IsControlElementCore() => true;
            protected override bool IsContentElementCore() => true;
            public void Invoke()
            {
                var button = (SignButton)Owner;
                if (button.IsEffectivelyEnabled) button.scene.SignClicked?.Invoke(button.id);
            }
        }
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        var now = SignAt(e.GetPosition(this));
        if (now == hover) return;
        hover = now;
        Cursor = hover != null ? new Cursor(StandardCursorType.Hand) : Cursor.Default;
        InvalidateVisual();
    }

    protected override void OnPointerExited(PointerEventArgs e)
    {
        base.OnPointerExited(e);
        if (hover == null) return;
        hover = null;
        InvalidateVisual();
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        Focus();
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        if (e.InitialPressMouseButton == MouseButton.Left && SignAt(e.GetPosition(this)) is string id)
        {
            focusedSign = signs.FindIndex(s => s.Id == id);
            SignClicked?.Invoke(id);
        }
    }
}
