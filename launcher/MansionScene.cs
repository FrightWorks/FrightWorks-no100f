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

// The launcher's main screen: a painted night coast (background.png) with stars, a moon, drifting clouds and fog, a
// turning lighthouse light and lightning; the mansion (launcher/art, made by tools/launcher-art/make_mansion_v2.py)
// whose windows light up, draw their curtains and show the odd silhouette; and the five signs painted on it that are
// the menu. The windows are holes in mansion.png: their insides are drawn first, then the house on top. The launcher
// writes the signs' labels on the painted planks (mansion.json holds the window boxes and the plank outlines).
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
        GhostArt = Skin.LoadBitmap("ghost.png"), BackArt = Skin.LoadBitmap("background.png");
    const int BackW = 1671, BackH = 941;
    // Open sky in background.png: stars only above this share of its height, and never on the lighthouse.
    const double StarSky = 0.55;
    static readonly Rect Lighthouse = new(80, 355, 130, 290);  // the tower and its lamp's halo

    // mansion.png's alpha (ArtW x ArtH, row by row), or null if it can't be read.
    static readonly byte[]? HouseAlpha = ReadAlpha(HouseArt);

    static byte[]? ReadAlpha(Bitmap? art)
    {
        if (art == null || art.PixelSize.Width != ArtW || art.PixelSize.Height != ArtH || art.Format is not { BitsPerPixel: 32 }) return null;
        int stride = ArtW * 4;
        var px = new byte[stride * ArtH];
        var pin = GCHandle.Alloc(px, GCHandleType.Pinned);
        try { art.CopyPixels(new PixelRect(0, 0, ArtW, ArtH), pin.AddrOfPinnedObject(), px.Length, stride); }
        catch (Exception) { return null; }
        finally { pin.Free(); }
        var alpha = new byte[ArtW * ArtH];
        for (int i = 0; i < alpha.Length; i++) alpha[i] = px[i * 4 + 3];  // alpha is the 4th byte in RGBA and BGRA
        return alpha;
    }

    // For each column of mansion.png, the first row that isn't transparent (ArtH where there is none): the roofline
    // the title is fitted above.
    static readonly int[] HouseTop = Roofline();

    static int[] Roofline()
    {
        var top = Enumerable.Repeat(ArtH, ArtW).ToArray();
        if (HouseAlpha == null) return top;
        for (int x = 0; x < ArtW; x++)
            for (int y = 0; y < ArtH; y++)
                if (HouseAlpha[y * ArtW + x] > 32) { top[x] = y; break; }
        return top;
    }

    // A window's glass, every few rows of its box: the row (art px) and the first and last see-through pixel of
    // mansion.png in it. Curtains hang inside it, so they follow arches and the slanted panes of the bay window. Without
    // the alpha it's the box.
    static (double Y, double Left, double Right, double SmoothLeft, double SmoothRight)[] MeasureGlass(Rect box)
    {
        var rows = new List<(double, double, double)>();
        int x0 = Math.Max(0, (int)box.X), x1 = Math.Min(ArtW - 1, (int)box.Right), y1 = Math.Min(ArtH - 1, (int)box.Bottom);
        for (int y = Math.Max(0, (int)box.Y); y <= y1; y += 2)
        {
            if (HouseAlpha == null) { rows.Add((y, box.X, box.Right)); continue; }
            int l = -1, r = -1;
            for (int x = x0; x <= x1; x++)
                if (HouseAlpha[y * ArtW + x] < 128) { if (l < 0) l = x; r = x + 1; }
            if (l >= 0 && r - l >= 2) rows.Add((y, l, r));
        }
        // Over a few rows: the widest edges (the outer side of a drape hides behind the frame, so it may reach past the
        // glass but never short of it) and the average ones (for the inner side, which shows: the painted frames'
        // antialiasing makes the edges step by a pixel).
        return rows.Select((row, i) =>
        {
            var near = rows.Skip(Math.Max(0, i - 2)).Take(5).ToList();
            return (row.Item1, near.Min(n => n.Item2), near.Max(n => n.Item3), near.Average(n => n.Item2), near.Average(n => n.Item3));
        }).ToArray();
    }

    readonly bool animate;
    readonly Random rnd;
    readonly Stopwatch clock = Stopwatch.StartNew();
    readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(33) };
    double shift, last;
    double Now => clock.Elapsed.TotalSeconds + shift;

    // A set of windows that light up together (the three panes of the bay window, the two lanterns) and its animation
    // state.
    sealed class Group
    {
        public double Light = 1, LightTo = 1, Curtain, CurtainTo, Figure, FigureTo, FigureUntil;
        public double Seed;
        public bool Curtains, Flame, Lit = true;
        // Where the group's one set of curtains hangs: across all its panes (see GroupGlass).
        public (double Y, double Left, double Right, double SmoothLeft, double SmoothRight)[] Glass = Array.Empty<(double, double, double, double, double)>();
    }
    sealed record Win(string Id, Group Group, Rect Box)
    {
        public readonly (double Y, double Left, double Right, double SmoothLeft, double SmoothRight)[] Glass = MeasureGlass(Box);
    }
    sealed class Sign
    {
        public string Id = "";
        public Point[] Points = Array.Empty<Point>();
        public RenderTargetBitmap? Normal, Hover, Light;
        public Point NormalAt, HoverAt, LightAt;
        public Geometry? Hit;
    }

    const int ArtW = 1447, ArtH = 1087;
    readonly List<Win> windows = new();
    readonly Dictionary<string, Group> groups = new();
    readonly List<Sign> signs = new();
    static readonly HashSet<string> CurtainGroups = new() { "parlor", "right_arch", "right_tower", "tower" };
    const string Lanterns = "lanterns";  // the porch lanterns: always lit, a flame that flickers and dips in the wind

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
            g.Flame = id == Lanterns;
        }
        // A lived-in start: most rooms lit, a couple dark, some curtains half drawn.
        foreach (var id in new[] { "attic", "right_tower" }) Set(id, light: 0);
        Set("parlor", curtain: 0.35);
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
        foreach (var g in groups.Values) g.Glass = GroupGlass(windows.Where(w => w.Group == g).Select(w => w.Glass).ToList());
    }

    // One set of curtains for a group, hung across all its panes (the bay window's three): at each row, from the left
    // edge of its leftmost pane to the right edge of its rightmost one. A pane shorter than the others counts at its
    // nearest row, so the drapes keep the outer panes' slant all the way up. The frames between the panes hide the
    // cloth behind them.
    static (double Y, double Left, double Right, double SmoothLeft, double SmoothRight)[] GroupGlass(List<(double Y, double Left, double Right, double SmoothLeft, double SmoothRight)[]> panes)
    {
        panes = panes.Where(p => p.Length > 0).ToList();
        if (panes.Count <= 1) return panes.FirstOrDefault() ?? Array.Empty<(double, double, double, double, double)>();
        double top = panes.Min(p => p[0].Y), bottom = panes.Max(p => p[^1].Y);
        var rows = new List<(double, double, double, double, double)>();
        for (double y = top; y <= bottom; y += 2)
        {
            var at = panes.Select(p => p.MinBy(r => Math.Abs(r.Y - y))).ToList();
            rows.Add((y, at.Min(r => r.Left), at.Max(r => r.Right), at.Min(r => r.SmoothLeft), at.Max(r => r.SmoothRight)));
        }
        return rows.ToArray();
    }

    // ----- Screenshots (--shots): a fixed, busy moment -----
    public void Showcase(bool lightning)
    {
        timer.Stop();
        foreach (var g in groups.Values) { g.Light = g.LightTo = 1; g.Curtain = g.CurtainTo = 0; g.Figure = g.FigureTo = 0; }
        Set("parlor", figure: 1);
        Set("tower", figure: 1);
        Set("parlor", curtain: 0.35);
        Set("right_arch", curtain: 1);
        Set("attic", light: 0);
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
        InvalidateVisual();
    }

    static double Approach(double v, double to, double step) => v < to ? Math.Min(to, v + step) : Math.Max(to, v - step);

    void WindowEvent(double t)
    {
        var ids = groups.Keys.ToList();
        var id = ids[rnd.Next(ids.Count)];
        var g = groups[id];
        double r = rnd.NextDouble();
        if (id == Lanterns)
        {
            g.Light = 0.45;  // the wind: a dip that comes straight back
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
        int lit = groups.Where(x => x.Key != Lanterns).Count(x => x.Value.LightTo > 0.5);
        if (g.LightTo > 0.5 && lit > 4) { g.LightTo = 0; g.FigureTo = 0; }
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

    // The house fills nine tenths of the height, sits at the bottom and leans right, leaving the sky on the left for
    // the title and the coast with its lighthouse.
    public Rect HouseRect => new(o.X, o.Y, ArtW * s, ArtH * s);
    public double SignsLeft => signs.Count == 0 ? W * 0.5 : signs.Min(sg => sg.Points.Min(p => p.X)) * s + o.X;

    // Picture-to-client scale and offset, for a size (the boards are placed from it before the first paint).
    public void Place(Size size)
    {
        double w = Math.Max(1, size.Width), h = Math.Max(1, size.Height);
        s = Math.Min(w * 0.99 / ArtW, (h - 6) / ArtH) * 0.9;
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

    // Draws a cached layer at its own size. The source rectangle is the layer's pixels: DrawImage(layer, rect) takes its
    // size in DIPs as the source, read as pixels, so at 150% display scale it drew only the layer's top-left two thirds,
    // stretched: magnified, cut off, with the painted signs of the house art showing beside the real ones (#193).
    static void DrawLayer(DrawingContext g, RenderTargetBitmap layer, Point at) =>
        g.DrawImage(layer, new Rect(0, 0, layer.PixelSize.Width, layer.PixelSize.Height), new Rect(at, layer.Size));

    void Rebuild()
    {
        cachedFor = Bounds.Size;
        cachedScale = Scaling;
        Place(Bounds.Size);

        house?.Dispose();
        double hw = Math.Max(1, Math.Round(ArtW * s)), hh = Math.Max(1, Math.Round(ArtH * s));
        house = Layer(hw, hh, g => { if (HouseArt != null) g.DrawImage(HouseArt, new Rect(0, 0, hw, hh)); });

        BuildSky();
        BuildDrift();
        BuildTitle();
        BuildLanternGlows();
        foreach (var sg in signs) BuildSign(sg);
    }

    // The porch lanterns' light on the house: a warm halo around each lantern, added over the house (inside its outline)
    // while they're lit. The profile is three stacked ellipses (radii 80, 48 and 26 art px, opacity 45, 75 and
    // 105 of 255) blurred by 18 px, as tuned on the art; it fades out by 120 px, before it reaches the door.
    const double GlowReach = 120;
    static readonly Color GlowColor = Color.FromRgb(255, 170, 70);
    static readonly double[] GlowProfile = { 0.561, 0.545, 0.498, 0.427, 0.349, 0.271, 0.2, 0.137, 0.086, 0.047, 0.02, 0.008, 0 };
    readonly List<(Win Window, RenderTargetBitmap Art, Point At)> lanternGlows = new();

    void BuildLanternGlows()
    {
        foreach (var lg in lanternGlows) lg.Art.Dispose();
        lanternGlows.Clear();
        var brush = new RadialGradientBrush();
        for (int i = 0; i < GlowProfile.Length; i++)
            brush.GradientStops.Add(new GradientStop(Skin.WithAlpha(GlowColor, (int)Math.Round(255 * GlowProfile[i])), i / (GlowProfile.Length - 1.0)));
        foreach (var w in windows.Where(w => w.Id.StartsWith("lantern", StringComparison.Ordinal)))
        {
            var c = w.Box.Center;
            var r = Map(new Rect(c.X - GlowReach, c.Y - GlowReach * 1.1, GlowReach * 2, GlowReach * 2.2));
            var at = new Point(Math.Floor(r.X), Math.Floor(r.Y));
            var ellipse = new Rect(r.X - at.X, r.Y - at.Y, r.Width, r.Height);
            var houseHere = new Rect(o.X - at.X, o.Y - at.Y, ArtW * s, ArtH * s);
            var art = Layer(Math.Ceiling(r.Width) + 1, Math.Ceiling(r.Height) + 1, g =>
            {
                if (HouseArt == null) { g.DrawEllipse(brush, null, ellipse); return; }
                using (g.PushOpacityMask(new ImageBrush(HouseArt) { Stretch = Stretch.Fill }, houseHere))
                    g.DrawEllipse(brush, null, ellipse);
            });
            lanternGlows.Add((w, art, at));
        }
    }

    void BuildSky()
    {
        sky?.Dispose();
        double w = Math.Max(1, W), h = Math.Max(1, H);
        // The painted coast covers the window, anchored bottom left so the lighthouse stays at the left edge.
        double k = backK = Math.Max(w / BackW, h / BackH);
        double backTop = this.backTop = h - BackH * k;
        sky = Layer(w, h, g =>
        {
            var all = new Rect(0, 0, w, h);
            if (BackArt != null) g.DrawImage(BackArt, new Rect(0, backTop, BackW * k, BackH * k));
            else
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
            // Stars in the open sky only: above the sea and the clifftop, and not on the lighthouse.
            var stars = new Random(7);
            double skyBottom = Math.Max(0, backTop + BackH * StarSky * k);
            for (int i = 0; i < 160; i++)
            {
                double x = stars.NextDouble() * w, y = Math.Pow(stars.NextDouble(), 1.6) * skyBottom;
                double d = stars.NextDouble() < 0.15 ? 2.2 : 1.3;
                int alpha = 60 + stars.Next(150);
                if (Lighthouse.Contains(new Point(x / k, (y - backTop) / k))) continue;
                g.DrawEllipse(Skin.Brush(Skin.Argb(alpha, 235, 230, 255)), null, new Rect(x, y, d, d));
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
        });
    }

    // ----- Clouds, fog and the lighthouse light -----
    // Painted sprites (launcher/art) that drift sideways and wrap round the window, slowly: clouds high in the sky, a
    // fog bank on the sea behind the house and one over the ground in front of it. Width and start x are shares of the
    // window width; y is a share of the height from the top, from the horizon or from the bottom; speed is in px/s at
    // 1180 wide (negative: to the left) and scales with the width. Fog breathes a little (opacity +-0.1 over 10 s).
    enum From { Top, Horizon, Bottom }
    sealed record Drift(Bitmap? Art, double Width, double StartX, From YFrom, double Y, double Opacity, double Speed, bool Breathes);
    sealed class DriftLayer { public Drift Spec = null!; public RenderTargetBitmap Art = null!; public double Y; }

    static readonly Bitmap? Cloud1 = Skin.LoadBitmap("cloud1.png"), Cloud2 = Skin.LoadBitmap("cloud2.png"),
        Cloud3 = Skin.LoadBitmap("cloud3.png"), Fog1 = Skin.LoadBitmap("fog1.png"), Fog2 = Skin.LoadBitmap("fog2.png"),
        Fog3 = Skin.LoadBitmap("fog3.png"), BeamArt = Skin.LoadBitmap("beam.png"), FlareArt = Skin.LoadBitmap("flare.png");
    static readonly Drift[] CloudSpecs =
    {
        new(Cloud1, 0.55, 0.28, From.Top, 0.03, 0.85, -6, false),
        new(Cloud2, 0.38, -0.05, From.Top, 0.19, 0.80, -8, false),
        new(Cloud3, 0.28, 0.62, From.Top, 0.30, 0.75, -10, false),
    };
    static readonly Drift[] BackFogSpecs =
    {
        new(Fog2, 0.75, -0.20, From.Horizon, -0.07, 0.55, 5, true),
        new(Fog3, 0.70, 0.45, From.Horizon, -0.04, 0.45, -4, true),
    };
    static readonly Drift[] FrontFogSpecs = { new(Fog1, 0.95, -0.10, From.Bottom, -0.17, 0.50, 7, true) };
    // The horizon in background.png, as a share of its height; the lamp's centre in its pixels.
    const double Horizon = 0.69;
    static readonly Point Lamp = new(146, 423);
    // The still moment drawn without animations (and in --shots): the light an eighth of a turn round, toward the
    // house, and the clouds and fog where they start.
    const double StillTime = 9.0 / 8;
    const double BeamTurn = 9;  // seconds per turn of the light

    readonly List<DriftLayer> clouds = new(), backFog = new(), frontFog = new();
    double backK = 1, backTop;
    RenderTargetBitmap? beam, flare;
    Geometry? frontFogClip;

    void BuildDrift()
    {
        double w = Math.Max(1, W), h = Math.Max(1, H), horizon = backTop + BackH * Horizon * backK;
        void Build(List<DriftLayer> list, Drift[] specs)
        {
            foreach (var l in list) l.Art.Dispose();
            list.Clear();
            foreach (var d in specs)
            {
                if (d.Art is not { } art) continue;
                double sw = Math.Max(1, Math.Round(d.Width * w)), sh = Math.Max(1, Math.Round(sw * art.PixelSize.Height / art.PixelSize.Width));
                double y = d.YFrom switch { From.Top => d.Y * h, From.Horizon => horizon + d.Y * h, _ => h + d.Y * h };
                list.Add(new DriftLayer { Spec = d, Art = Layer(sw, sh, g => g.DrawImage(art, new Rect(0, 0, sw, sh))), Y = Math.Round(y) });
            }
        }
        Build(clouds, CloudSpecs);
        Build(backFog, BackFogSpecs);
        Build(frontFog, FrontFogSpecs);

        // The light at its longest (0.8 of the width) and the flare at its biggest, scaled down as they turn.
        beam?.Dispose();
        flare?.Dispose();
        beam = flare = null;
        double bl = Math.Round(0.8 * w), bh = Math.Max(1, Math.Round(bl * 220 / 1400));
        if (BeamArt is { } beamArt) beam = Layer(bl, bh, g => g.DrawImage(beamArt, new Rect(0, 0, bl, bh)));
        double fs = Math.Max(1, Math.Ceiling(FlareSize(1, 1.06)));
        if (FlareArt is { } flareArt) flare = Layer(fs, fs, g => g.DrawImage(flareArt, new Rect(0, 0, fs, fs)));

        // The front fog never covers the signs (the painted planks): it is clipped out of their outlines.
        var signShapes = new GeometryGroup();
        foreach (var sg in signs) signShapes.Children.Add(Skin.Polygon(SignPoints(sg, 1)));
        frontFogClip = new CombinedGeometry(GeometryCombineMode.Exclude, new RectangleGeometry(new Rect(0, 0, w, h)), signShapes);
    }

    // The flare's size for how much the light faces the viewer (0..1) and its flicker.
    double FlareSize(double facing, double flicker) => (34 + 70 * Math.Pow(facing, 6)) * 1.6 * backK * flicker;

    void DrawDrift(DrawingContext g, List<DriftLayer> list, double t)
    {
        double w = Math.Max(1, W);
        for (int i = 0; i < list.Count; i++)
        {
            var l = list[i];
            double sw = l.Art.Size.Width, period = w + sw;
            double x = l.Spec.StartX * w + l.Spec.Speed * (w / 1180) * t;
            x = ((x + sw) % period + period) % period - sw;
            double opacity = l.Spec.Opacity + (l.Spec.Breathes ? 0.1 * Math.Sin(t * Math.Tau / 10 + i * 2.1) : 0);
            using (g.PushOpacity(Math.Clamp(opacity, 0, 1)))
                DrawLayer(g, l.Art, new Point(Math.Round(x), l.Y));
        }
    }

    // The lighthouse's turning light: a cone from the lamp, its length cos(angle) of the longest (mirrored to the left
    // when it points away from the house), dimmer on the far side where the tower hides part of it; and a flare on the
    // lamp that grows when the light faces the viewer. Both are behind the house and the front fog.
    void DrawLighthouse(DrawingContext g, double t)
    {
        double angle = t / BeamTurn * Math.Tau, c = Math.Cos(angle), sn = Math.Sin(angle);
        var lamp = new Point(Lamp.X * backK, backTop + Lamp.Y * backK);
        if (beam != null && Math.Abs(c) * beam.Size.Width >= 6)
        {
            double len = Math.Abs(c) * beam.Size.Width, bh = beam.Size.Height;
            var src = new Rect(0, 0, beam.PixelSize.Width, beam.PixelSize.Height);
            var dest = new Rect(lamp.X, lamp.Y - bh / 2, len, bh);
            var mirror = c < 0 ? new Matrix(-1, 0, 0, 1, 2 * lamp.X, 0) : Matrix.Identity;
            using (g.PushOpacity(sn >= 0 ? 0.75 : 0.45))
            using (g.PushTransform(mirror))
                g.DrawImage(beam, src, dest);
        }
        if (flare != null)
        {
            double facing = Math.Max(0, sn), flicker = 1 + 0.06 * Math.Sin(t * 13.1) * Math.Sin(t * 7.3);
            double size = FlareSize(facing, flicker);
            using (g.PushOpacity(0.7 + 0.3 * facing))
                g.DrawImage(flare, new Rect(0, 0, flare.PixelSize.Width, flare.PixelSize.Height),
                            new Rect(lamp.X - size / 2, lamp.Y - size / 2, size, size));
        }
    }

    // The title, top left in the sky: a small lilac FRIGHTWORKS over a big lime NIGHT OF / 100 FRIGHTS, the three lines
    // centred on one axis. em is the big lines' size: 40 at most (the small line is 0.6 of it, the big lines 1.05 apart),
    // smaller when the roofline comes down past the block.
    const double TitleX = 18, TitleY = 12, TitleMax = 40;
    static readonly Color TitleLilac = Color.Parse("#C9B8FF"), TitleLime = Color.Parse("#D8F25A"), TitleInk = Color.Parse("#1A0F2A");
    static readonly string[] TitleLines = { "FRIGHTWORKS", "NIGHT OF", "100 FRIGHTS" };
    static double TitleSize(int line, double em) => line == 0 ? em * 0.6 : em;
    static double TitleTop(int line, double em) => line == 0 ? 0 : em * 0.7 + (line - 1) * em * 1.05;
    static double TitleHeight(double em) => em * (0.7 + 1.05 + 1.15);
    static double TitleOutline(double em) => Math.Max(2, em * 0.1);  // half of it shows: about 2 px at full size
    static double TitleWidth(double em) =>
        Enumerable.Range(0, TitleLines.Length).Max(i => Skin.TextWidth(TitleLines[i], TitleSize(i, em))) + TitleOutline(em) + 8;

    void BuildTitle()
    {
        title?.Dispose();
        double em = TitleMax;
        if (TitleWidth(em) > TitleRoom(em))
        {
            // Smaller text is shorter, so the roofline lets it run further right: the largest size that fits.
            double lo = 16, hi = em;
            for (int i = 0; i < 12; i++)
            {
                double mid = (lo + hi) / 2;
                if (TitleWidth(mid) <= TitleRoom(mid)) lo = mid; else hi = mid;
            }
            em = lo;
        }
        double block = Math.Ceiling(TitleWidth(em)), width = block + 12, height = Math.Ceiling(TitleHeight(em)) + 10;
        title = Layer(width, height, g =>
        {
            for (int i = 0; i < TitleLines.Length; i++)
            {
                var box = new Rect(6, TitleTop(i, em), block, (i == 0 ? 0.7 : 1.05) * em);
                if (i == 0) Skin.Text(g, TitleLines[i], TitleSize(i, em), box, TitleLilac, default, 0);
                else Skin.Text(g, TitleLines[i], TitleSize(i, em), box, TitleLime, TitleInk, TitleOutline(em));
            }
        });
    }

    // How wide the title can be at a size: from its left edge to the first column where the house rises to within a
    // few pixels of the block's bottom.
    double TitleRoom(double em)
    {
        double bottom = TitleY + TitleHeight(em) + 6;
        for (double x = TitleX; x < W; x++)
        {
            int ax = (int)((x - o.X) / s);
            if (ax >= 0 && ax < ArtW && o.Y + HouseTop[ax] * s < bottom) return x - TitleX - 8;
        }
        return Math.Max(0, W - TitleX - 8);
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

    // The painted plank itself: mansion.json's outline is the plank grown 6 art px away from its centre, so this takes
    // each point back.
    Point[] PlankPoints(Sign sg)
    {
        double cx = sg.Points.Average(p => p.X), cy = sg.Points.Average(p => p.Y);
        return sg.Points.Select(p =>
        {
            double dx = p.X - cx, dy = p.Y - cy, d = Math.Max(7, Math.Sqrt(dx * dx + dy * dy));
            return Map(p.X - dx / d * 6, p.Y - dy / d * 6);
        }).ToArray();
    }

    void BuildSign(Sign sg)
    {
        sg.Normal?.Dispose();
        sg.Hover?.Dispose();
        sg.Light?.Dispose();
        (sg.Normal, sg.NormalAt) = RenderSign(sg, false);
        (sg.Hover, sg.HoverAt) = RenderSign(sg, true);
        (sg.Light, sg.LightAt) = RenderPlankLight(sg);
        sg.Hit = Skin.Polygon(SignPoints(sg, 1));
    }

    // The selected plank (hover or keyboard focus) is lit from the front: a warm light added over the painted wood,
    // brightest in the middle and clipped to the plank, so nothing spills onto the wall. Its label turns light with a
    // dark edge (lime on Play). Play keeps a faint, slow light when it isn't selected, so it reads as the way in.
    static readonly Color Label = Color.Parse("#3A2412"), LabelLit = Color.Parse("#FFF0C8"), PlayLit = Color.Parse("#D8F25A"),
        LabelEdge = Color.Parse("#2A1606");

    (RenderTargetBitmap, Point) RenderPlankLight(Sign sg)
    {
        var pts = PlankPoints(sg);
        double minX = pts.Min(p => p.X), minY = pts.Min(p => p.Y), maxX = pts.Max(p => p.X), maxY = pts.Max(p => p.Y);
        var at = new Point(Math.Floor(minX), Math.Floor(minY));
        var bitmap = Layer(Math.Ceiling(maxX - minX) + 1, Math.Ceiling(maxY - minY) + 1, g =>
        {
            using var moved = g.PushTransform(Matrix.CreateTranslation(-at.X, -at.Y));
            var brush = new RadialGradientBrush
            {
                GradientStops =
                {
                    new GradientStop(Skin.Argb(130, 255, 222, 160), 0), new GradientStop(Skin.Argb(90, 255, 214, 150), 0.6),
                    new GradientStop(Skin.Argb(55, 255, 205, 140), 1),
                },
            };
            g.DrawGeometry(brush, null, Skin.Polygon(pts));
        });
        return (bitmap, at);
    }

    (RenderTargetBitmap, Point) RenderSign(Sign sg, bool lit)
    {
        var pts = SignPoints(sg, 1);
        double minX = pts.Min(p => p.X), minY = pts.Min(p => p.Y), maxX = pts.Max(p => p.X), maxY = pts.Max(p => p.Y);
        var at = new Point(Math.Floor(minX), Math.Floor(minY));
        double bw = Math.Max(1, Math.Ceiling(maxX - minX) + 1), bh = Math.Max(1, Math.Ceiling(maxY - minY) + 1);
        var bitmap = Layer(bw, bh, g =>
        {
            using var moved = g.PushTransform(Matrix.CreateTranslation(-at.X, -at.Y));
            var box = new Rect(minX, minY, maxX - minX, maxY - minY);
            bool play = sg.Id == "play";
            // The outline is the plank grown by 6 picture pixels: the label goes inside the plank itself.
            var plank = box.Deflate(6 * s);
            var fill = !lit ? Label : play ? PlayLit : LabelLit;
            FitText(g, SignText.GetValueOrDefault(sg.Id, sg.Id), (play ? 84 : 44) * s, plank.Deflate(new Thickness(plank.Width * 0.12, 0)),
                    fill, LabelEdge, lit ? Math.Max(2, (play ? 7 : 5) * s) : 0);
        });
        return (bitmap, at);
    }

    static void FitText(DrawingContext g, string text, double em, Rect box, Color fill, Color outline, double outlineWidth)
    {
        double w = Skin.TextGeometry(text, em, new Rect(0, 0, 4000, 1000)).Bounds.Width;
        if (w > box.Width * 0.92) em *= box.Width * 0.92 / w;
        Skin.Text(g, text, em, box, fill, outline, outlineWidth);
    }

    // ----- Painting -----
    public override void Render(DrawingContext g)
    {
        if (W <= 0 || H <= 0) return;
        if (cachedFor != Bounds.Size || cachedScale != Scaling || house == null) Rebuild();
        double t = Now;
        double flash = Flash();
        var all = new Rect(Bounds.Size);

        DrawLayer(g, sky!, default);
        double still = animate ? t : StillTime;  // the drift and the lighthouse stand still without animations
        DrawDrift(g, clouds, still);
        DrawDrift(g, backFog, still);
        DrawLighthouse(g, still);
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
        foreach (var gr in groups.Values)
            if (gr.Curtains && gr.Curtain > 0.01) DrawCurtains(g, gr, gr.Curtain, GroupLight(gr, t));
        foreach (var w in windows)
            if (Figure(w) is { } figure && w.Group.Figure > 0.01 && w.Group.Curtains && w.Group.Curtain > 0.01)
                DrawFaded(g, figure, FigureRect(w), w.Group.Figure * w.Group.Curtain * 0.5 * GroupLight(w.Group, t));  // its shadow on the curtain

        var houseRect = new Rect(new Point((int)o.X, (int)o.Y), house!.Size);
        DrawLayer(g, house, houseRect.Position);
        // Additive, so the lanterns brighten the paint under them instead of tinting it orange.
        using (g.PushRenderOptions(new RenderOptions { BitmapBlendingMode = BitmapBlendingMode.Plus }))
            foreach (var (win, art, at) in lanternGlows)
            {
                double light = GroupLight(win.Group, t);
                if (light <= 0.01) continue;
                using (g.PushOpacity(light))
                    DrawLayer(g, art, at);
            }
        if (flash > 0.02)
        {
            // The lightning lights the house: a pale silhouette of it over it.
            using (g.PushOpacityMask(new ImageBrush(house) { Stretch = Stretch.Fill }, houseRect))
                g.FillRectangle(Skin.Brush(Skin.Argb((int)(255 * 0.3 * flash), 217, 217, 255)), houseRect);
        }
        if (frontFogClip != null)
            using (g.PushGeometryClip(frontFogClip))
                DrawDrift(g, frontFog, still);

        foreach (var sg in signs)
        {
            bool focused = sg.Id == FocusedSign && (IsFocused || signButtons.Any(b => b.IsFocused));
            bool lit = sg.Id == hover || sg.Id == active || focused;
            double breathe = (Math.Sin(t * 1.6) + 1) / 2;
            double light = lit ? 1 : sg.Id == "play" ? 0.15 + 0.15 * breathe : 0;
            if (light > 0.01)
                using (g.PushRenderOptions(new RenderOptions { BitmapBlendingMode = BitmapBlendingMode.Plus }))
                using (g.PushOpacity(light))
                    DrawLayer(g, sg.Light!, sg.LightAt);
            if (lit) DrawLayer(g, sg.Hover!, sg.HoverAt);
            else DrawLayer(g, sg.Normal!, sg.NormalAt);
        }

        DrawLayer(g, title!, new Point(TitleX, TitleY));
        DrawFooter(g);
    }

    static readonly Color Dark = Color.Parse("#0D0E1E"), WarmTop = Color.Parse("#FFE27C"), WarmBottom = Color.Parse("#F39A2B");

    // A group's light now, with its flicker (0..1): slight for a room, more for the lanterns' flames.
    static double GroupLight(Group gr, double t)
    {
        double f = gr.Flame ? 0.12 : 0.05;
        return Math.Clamp(gr.Light * (1 - f + f * Math.Sin(t * 11 + gr.Seed) * Math.Sin(t * 6.7 + gr.Seed * 2)), 0, 1);
    }

    void DrawWindow(DrawingContext g, Win w, double t)
    {
        var b = Map(w.Box).Inflate(1);
        var gr = w.Group;
        double light = GroupLight(gr, t);
        g.FillRectangle(Skin.Brush(Dark), b);
        if (light > 0.01)
            g.FillRectangle(Skin.Vertical(Skin.WithAlpha(WarmTop, (int)(255 * light)), Skin.WithAlpha(WarmBottom, (int)(255 * light))), b);
        if (Figure(w) is { } figure && gr.Figure > 0.01) DrawFaded(g, figure, FigureRect(w), gr.Figure);
    }

    // The silhouettes: the dog in the bay window's middle pane, the Mastermind in the tower.
    static Bitmap? Figure(Win w) => w.Id == "parlor" ? DogArt : w.Id == "tower" ? GhostArt : null;

    // Where a silhouette is drawn: its window's box, the dog moved left and down (art px) so it sits in the clear middle
    // of the pane and the sill hides its base.
    Rect FigureRect(Win w) => Map(w.Id == "parlor" ? w.Box.Translate(new Vector(-22, 28)) : w.Box);

    static void DrawFaded(DrawingContext g, Bitmap art, Rect dest, double alpha)
    {
        using (g.PushOpacity(Math.Clamp(alpha, 0, 1)))
            g.DrawImage(art, new Rect(Math.Round(dest.X), Math.Round(dest.Y), Math.Round(dest.Width), Math.Round(dest.Height)));
    }

    // Two drapes of muted plum fabric hung inside the window's glass, row by row, so they follow its arch and the slant
    // of a bay pane. Drawn partly open they're gathered at a tie-back a little below the middle; closed they meet. The
    // window's light shows through the cloth a little, and the folds run down with the glass.
    void DrawCurtains(DrawingContext g, Group gr, double amount, double light)
    {
        var glass = gr.Glass;
        if (glass.Length < 2) return;
        double top = glass[0].Y, height = Math.Max(1, glass[^1].Y - top);
        Color cloth = Lerp(Color.Parse("#22102A"), Color.Parse("#6A2E4E"), light);
        Color fold = Skin.WithAlpha(Lerp(Color.Parse("#12081A"), Color.Parse("#3E1730"), light), 170);
        Color sheen = Skin.WithAlpha(Lerp(Color.Parse("#3A2040"), Color.Parse("#9A5470"), light), 80);
        var folds = new Pen(Skin.Brush(fold), Math.Max(1, 1.6 * s), lineJoin: PenLineJoin.Round);
        var sheens = new Pen(Skin.Brush(sheen), Math.Max(1, 1.2 * s), lineJoin: PenLineJoin.Round);
        // How far a drape reaches into the glass at a row, as a share of half its width.
        double Reach(double y)
        {
            double v = (y - top) / height;
            return amount * (1 - (1 - amount) * 0.45 * Math.Exp(-Math.Pow((v - 0.62) / 0.16, 2)));
        }
        foreach (var left in new[] { true, false })
        {
            var outer = new List<Point>();
            var inner = new List<Point>();
            for (int i = 0; i < glass.Length; i++)
            {
                var (y, l, r, sl, sr) = glass[i];
                double half = (sr - sl) / 2 + 0.5, reach = half * Reach(y);
                double yy = i == 0 ? y - 1.5 : i == glass.Length - 1 ? y + 2.5 : y;  // rows are 2 apart: cover the ends
                outer.Add(Map(left ? l - 1 : r + 1, yy));
                inner.Add(Map(left ? sl + reach : sr - reach, yy));
            }
            if (inner.Select((p, i) => Math.Abs(p.X - outer[i].X)).Max() < 1.5) continue;
            var outline = outer.Concat(Enumerable.Reverse(inner)).ToArray();
            using (g.PushOpacity(0.9))
                g.DrawGeometry(Skin.Brush(cloth), null, Skin.Polygon(outline));
            // Folds across the drape, about one every 14 art px at its widest, each with a faint lit edge beside it.
            double widest = glass.Max(r => (r.SmoothRight - r.SmoothLeft) / 2) * amount;
            int count = Math.Max(3, (int)Math.Round(widest / 14));
            foreach (var share in Enumerable.Range(0, count).Select(i => (i + 0.6) / (count + 0.2)))
            {
                var line = outer.Select((p, i) => new Point(p.X + (inner[i].X - p.X) * share, p.Y)).ToArray();
                if (Math.Abs(inner[^1].X - outer[^1].X) * share < 2) continue;
                g.DrawGeometry(null, folds, Skin.Polyline(line));
                g.DrawGeometry(null, sheens, Skin.Polyline(line.Select(p => new Point(p.X + 1.6 * s, p.Y)).ToArray()));
            }
        }
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
