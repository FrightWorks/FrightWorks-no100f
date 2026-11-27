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

// The launcher's main screen: a painted night coast (background.png) with stars, a moon, clouds and lightning, the
// mansion (launcher/art, made by tools/launcher-art/make_mansion_v2.py) whose windows light up, draw their curtains
// and show the odd silhouette, and the five signs that are the menu. The windows are holes in mansion.png: their
// insides are drawn first, then the house on top. The signs are drawn over the ones painted in the picture
// (mansion.json holds both outlines).
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
    static readonly Rect Lighthouse = new(95, 370, 100, 275);

    // For each column of mansion.png, the first row that isn't transparent (ArtH where there is none): the roofline
    // the title is fitted above.
    static readonly int[] HouseTop = Roofline(HouseArt);

    static int[] Roofline(Bitmap? art)
    {
        var top = Enumerable.Repeat(ArtH, ArtW).ToArray();
        if (art == null || art.PixelSize.Width != top.Length || art.Format is not { BitsPerPixel: 32 }) return top;
        int w = art.PixelSize.Width, h = art.PixelSize.Height, stride = w * 4;
        var px = new byte[stride * h];
        var pin = GCHandle.Alloc(px, GCHandleType.Pinned);
        try { art.CopyPixels(new PixelRect(0, 0, w, h), pin.AddrOfPinnedObject(), px.Length, stride); }
        catch (Exception) { return top; }
        finally { pin.Free(); }
        for (int x = 0; x < w; x++)
            for (int y = 0; y < h; y++)
                if (px[y * stride + x * 4 + 3] > 32) { top[x] = y; break; }  // alpha is the 4th byte in RGBA and BGRA
        return top;
    }

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
    static readonly HashSet<string> CurtainGroups = new() { "parlor", "parlor_side", "right_arch", "right_tower", "tower" };

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
        foreach (var id in new[] { "attic", "right_tower" }) Set(id, light: 0);
        Set("parlor_side", curtain: 0.55);
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
        Set("parlor_side", curtain: 0.55);
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
        BuildClouds();
        BuildTitle();
        foreach (var sg in signs) BuildSign(sg);
    }

    void BuildSky()
    {
        sky?.Dispose();
        double w = Math.Max(1, W), h = Math.Max(1, H);
        // The painted coast covers the window, anchored bottom left so the lighthouse stays at the left edge.
        double k = Math.Max(w / BackW, h / BackH), backTop = h - BackH * k;
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

    // The title, top left in the sky: as big as fits (46 at most) left of where the roofline comes down past it.
    const double TitleX = 18, TitleY = 12;

    void BuildTitle()
    {
        title?.Dispose();
        double TextWidth(double size) =>
            Skin.TextGeometry("FRIGHTWORKS", size, new Rect(0, 0, 4000, 400), Skin.Align.Near).Bounds.Width + 8;
        double em = 46, room = TitleRoom(em);
        if (TextWidth(em) > room)
        {
            // Smaller text is shorter, so the roofline lets it run further right: the largest size that fits.
            double lo = 14, hi = em;
            for (int i = 0; i < 12; i++)
            {
                double mid = (lo + hi) / 2;
                if (TextWidth(mid) <= TitleRoom(mid)) lo = mid; else hi = mid;
            }
            em = lo;
            room = TitleRoom(em);
        }
        double sub = Skin.TextGeometry("Night of 100 Frights", em * 0.52, new Rect(0, 0, 4000, 400), Skin.Align.Near).Bounds.Width;
        double width = (int)Math.Max(sub, TextWidth(em)) + 30, height = (int)(em * 2.3) + 10;
        title = Layer(width, height, g =>
        {
            // Layout boxes far wider than the text, as the old GDI+ drawing used.
            Skin.Text(g, "FRIGHTWORKS", em, new Rect(6, 0, 4000, em * 1.25), Skin.Lime, Skin.Ink, Math.Max(3, em * 0.16), Skin.Align.Near);
            Skin.Text(g, "Night of 100 Frights", em * 0.52, new Rect(8, em * 1.12, 4000, em * 0.8), Skin.Cream, Skin.Ink, Math.Max(2, em * 0.11), Skin.Align.Near);
        });
    }

    // How wide the title can be at a size: from its left edge to the first column where the house rises to within a
    // few pixels of the title's bottom (the subtitle ends at about twice the size below the top).
    double TitleRoom(double em)
    {
        double bottom = TitleY + em * 2 + 6;
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

    void BuildSign(Sign sg)
    {
        sg.Normal?.Dispose();
        sg.Hover?.Dispose();
        (sg.Normal, sg.NormalAt) = RenderSign(sg, false);
        (sg.Hover, sg.HoverAt) = RenderSign(sg, true);
        sg.Hit = Skin.Polygon(SignPoints(sg, 1));
    }

    // The signs are the planks painted in mansion.png: only the label is drawn, centred on the plank's outline, in a
    // dark brown that reads on the weathered wood. Hover and focus add the glow around the outline (OnRender) and a
    // slightly warmer label.
    static readonly Color Label = Color.Parse("#3A2412"), LabelLit = Color.Parse("#4E2C10");

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
            FitText(g, SignText.GetValueOrDefault(sg.Id, sg.Id), (play ? 84 : 44) * s, plank.Deflate(new Thickness(plank.Width * 0.12, 0)),
                    lit ? LabelLit : Label, default, 0);
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
        foreach (var c in clouds) DrawLayer(g, c.Art, new Point((int)c.X, (int)c.Y));
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
        DrawLayer(g, house, houseRect.Position);
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
            if (lit) DrawLayer(g, sg.Hover!, sg.HoverAt);
            else DrawLayer(g, sg.Normal!, sg.NormalAt);
            if (focused)
                g.DrawGeometry(null, new Pen(Skin.Brush(Skin.Cream), Math.Max(1, 2 * s), new DashStyle(new double[] { 2, 2 }, 0)),
                               Skin.Polygon(SignPoints(sg, 1.045)));
        }

        DrawLayer(g, title!, new Point(TitleX, TitleY));
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
