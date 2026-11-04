using System.Diagnostics;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Text.Json;

namespace FrightWorks;

// The launcher's main screen: a stormy night sky with lightning, the mansion (launcher\art, made by
// tools\launcher-art\make_launcher_art.py) whose windows light up, draw their curtains and show the odd silhouette,
// and the five signs that are the menu. The windows are holes in mansion.png: their insides are drawn first, then the
// house on top. The signs are drawn over the ones painted in the picture (mansion.json holds both outlines).
sealed class MansionScene : Control
{
    public event Action<string>? SignClicked;
    string? active;
    public string? ActiveSign { get => active; set { active = value; Invalidate(); } }
    string status = "";
    bool statusOk;
    public void SetStatus(string text, bool ok) { status = text; statusOk = ok; Invalidate(); }

    static readonly Bitmap? HouseArt = Theme.LoadBitmap("mansion.png"), DogArt = Theme.LoadBitmap("dog.png"),
        GhostArt = Theme.LoadBitmap("ghost.png");

    readonly bool animate;
    readonly Random rnd;
    readonly Stopwatch clock = Stopwatch.StartNew();
    readonly System.Windows.Forms.Timer timer = new() { Interval = 33 };
    double shift, last;
    double Now => clock.Elapsed.TotalSeconds + shift;

    // A set of windows that light up together (the entrance's three, the tower's two) and its animation state.
    sealed class Group
    {
        public double Light = 1, LightTo = 1, Curtain, CurtainTo, Figure, FigureTo, FigureUntil;
        public double Seed;
        public bool Curtains, Lit = true;
    }
    sealed record Win(string Id, Group Group, RectangleF Box);
    sealed class Sign
    {
        public string Id = "";
        public PointF[] Points = Array.Empty<PointF>();
        public Bitmap? Normal, Hover;
        public Point NormalAt, HoverAt;
        public GraphicsPath? Hit;
    }
    sealed class Cloud { public Bitmap Art = null!; public float X, Y, Speed; }

    readonly int artW = 1447, artH = 1087;
    readonly List<Win> windows = new();
    readonly Dictionary<string, Group> groups = new();
    readonly List<Sign> signs = new();
    readonly List<Cloud> clouds = new();
    static readonly HashSet<string> CurtainGroups = new() { "parlor", "parlor_side", "dormer", "right_arch", "right_tower", "tower" };

    float s = 1;
    PointF o;
    Size cachedFor;
    Bitmap? house, sky, title;
    double nextStrike, strikeAt = -100, nextEvent;
    List<PointF[]>? bolt;
    string? hover;

    public MansionScene(bool animate, int seed)
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.ResizeRedraw | ControlStyles.Opaque, true);
        this.animate = animate;
        rnd = new Random(seed);
        LoadLayout();
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
        if (animate) timer.Start();
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
        using var stream = Theme.Resource("mansion.json");
        if (stream == null) return;
        using var doc = JsonDocument.Parse(stream);
        var root = doc.RootElement;
        foreach (var w in root.GetProperty("windows").EnumerateArray())
        {
            var gid = w.GetProperty("group").GetString()!;
            if (!groups.TryGetValue(gid, out var g)) groups[gid] = g = new Group();
            windows.Add(new Win(w.GetProperty("id").GetString()!, g,
                new RectangleF(w.GetProperty("x").GetSingle(), w.GetProperty("y").GetSingle(), w.GetProperty("w").GetSingle(), w.GetProperty("h").GetSingle())));
        }
        foreach (var sg in root.GetProperty("signs").EnumerateArray())
            signs.Add(new Sign
            {
                Id = sg.GetProperty("id").GetString()!,
                Points = sg.GetProperty("points").EnumerateArray().Select(p => new PointF(p[0].GetSingle(), p[1].GetSingle())).ToArray(),
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
        Invalidate();
    }

    // ----- Animation -----
    void Step()
    {
        var form = FindForm();
        if (form == null || form.WindowState == FormWindowState.Minimized || !Visible) return;
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
            c.X += c.Speed * (float)dt;
            if (c.X > Width) c.X = -c.Art.Width;
        }
        Invalidate();
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

    List<PointF[]> MakeBolt(Random r)
    {
        var paths = new List<PointF[]>();
        float W = Math.Max(1, Width), H = Math.Max(1, Height), step = Math.Max(6, 18 * s);
        float x = W * (0.08f + (float)r.NextDouble() * 0.84f), y = 0;
        var main = new List<PointF> { new(x, y) };
        while (y < H * 0.6f)
        {
            y += step * (0.6f + (float)r.NextDouble());
            x += (float)(r.NextDouble() * 2 - 1) * step * 1.1f;
            main.Add(new PointF(x, y));
        }
        paths.Add(main.ToArray());
        for (int b = 0; b < 2; b++)
        {
            var from = main[2 + r.Next(Math.Max(1, main.Count - 4))];
            float bx = from.X, by = from.Y, dir = r.NextDouble() < 0.5 ? -1 : 1;
            var branch = new List<PointF> { from };
            for (int i = 0; i < 5 + r.Next(4); i++)
            {
                by += step * (0.5f + (float)r.NextDouble() * 0.7f);
                bx += dir * step * (0.4f + (float)r.NextDouble());
                branch.Add(new PointF(bx, by));
            }
            paths.Add(branch.ToArray());
        }
        return paths;
    }

    // ----- Layout and cached layers -----
    PointF Map(float x, float y) => new(o.X + x * s, o.Y + y * s);
    RectangleF Map(RectangleF r) => new(o.X + r.X * s, o.Y + r.Y * s, r.Width * s, r.Height * s);

    // The house fills the height, sits at the bottom and leans right, leaving the sky on the left for the title.
    public RectangleF HouseRect => new(o.X, o.Y, artW * s, artH * s);
    public float SignsLeft => signs.Count == 0 ? Width * 0.5f : signs.Min(sg => sg.Points.Min(p => p.X)) * s + o.X;

    // Picture-to-client scale and offset, on every resize (the boards are placed from it before the first paint).
    void Place()
    {
        float W = Math.Max(1, Width), H = Math.Max(1, Height);
        s = Math.Min(W * 0.99f / artW, (H - Theme.S(6)) / artH);
        o = new PointF((float)Math.Round((W - artW * s) * 0.82f), (float)Math.Round(H - artH * s));
    }

    protected override void OnResize(EventArgs e)
    {
        Place();
        base.OnResize(e);
    }

    void Rebuild()
    {
        cachedFor = Size;
        Place();

        house?.Dispose();
        house = new Bitmap(Math.Max(1, (int)Math.Round(artW * s)), Math.Max(1, (int)Math.Round(artH * s)), PixelFormat.Format32bppPArgb);
        if (HouseArt != null)
            using (var g = Graphics.FromImage(house))
            {
                Theme.HighQuality(g);
                g.DrawImage(HouseArt, new Rectangle(0, 0, house.Width, house.Height));
            }

        BuildSky();
        BuildClouds();
        BuildTitle();
        foreach (var sg in signs) BuildSign(sg);
    }

    void BuildSky()
    {
        sky?.Dispose();
        sky = new Bitmap(Math.Max(1, Width), Math.Max(1, Height), PixelFormat.Format32bppPArgb);
        using var g = Graphics.FromImage(sky);
        Theme.HighQuality(g);
        var all = new Rectangle(0, 0, Width, Height);
        using (var grad = new LinearGradientBrush(all, Color.Black, Color.Black, LinearGradientMode.Vertical))
        {
            grad.InterpolationColors = new ColorBlend
            {
                Colors = new[] { ColorTranslator.FromHtml("#05030B"), ColorTranslator.FromHtml("#140C28"), ColorTranslator.FromHtml("#2A1A45"), ColorTranslator.FromHtml("#331F4E") },
                Positions = new[] { 0f, 0.45f, 0.8f, 1f },
            };
            g.FillRectangle(grad, all);
        }
        var stars = new Random(7);
        for (int i = 0; i < 160; i++)
        {
            float x = (float)stars.NextDouble() * Width, y = (float)Math.Pow(stars.NextDouble(), 1.6) * Height * 0.75f;
            float d = stars.NextDouble() < 0.15 ? 2.2f : 1.3f;
            using var b = new SolidBrush(Color.FromArgb(60 + stars.Next(150), 235, 230, 255));
            g.FillEllipse(b, x, y, d, d);
        }
        // The moon, above the right tower.
        var m = Map(1300, 105);
        float mr = Math.Max(18, 52 * s);
        using (var glowPath = new GraphicsPath())
        {
            glowPath.AddEllipse(m.X - mr * 3.2f, m.Y - mr * 3.2f, mr * 6.4f, mr * 6.4f);
            using var glow = new PathGradientBrush(glowPath) { CenterColor = Color.FromArgb(70, 230, 225, 255), SurroundColors = new[] { Color.FromArgb(0, 230, 225, 255) } };
            g.FillPath(glow, glowPath);
        }
        using (var moon = new SolidBrush(ColorTranslator.FromHtml("#EEEAD3"))) g.FillEllipse(moon, m.X - mr, m.Y - mr, mr * 2, mr * 2);
        using (var crater = new SolidBrush(ColorTranslator.FromHtml("#D7D2B7")))
        {
            g.FillEllipse(crater, m.X - mr * 0.45f, m.Y - mr * 0.35f, mr * 0.42f, mr * 0.42f);
            g.FillEllipse(crater, m.X + mr * 0.2f, m.Y + mr * 0.15f, mr * 0.3f, mr * 0.3f);
            g.FillEllipse(crater, m.X - mr * 0.1f, m.Y + mr * 0.45f, mr * 0.22f, mr * 0.22f);
        }
        // Rolling hills along the bottom, behind the house.
        using var hills = new GraphicsPath();
        var pts = new List<PointF> { new(0, Height) };
        for (int i = 0; i <= 12; i++)
        {
            float x = Width * i / 12f;
            pts.Add(new PointF(x, Height * (0.86f + 0.04f * (float)Math.Sin(i * 1.7 + 0.5))));
        }
        pts.Add(new PointF(Width, Height));
        hills.AddCurve(pts.ToArray(), 0.4f);
        using (var ground = new SolidBrush(ColorTranslator.FromHtml("#0C0816"))) g.FillPath(ground, hills);
    }

    void BuildClouds()
    {
        foreach (var c in clouds) c.Art.Dispose();
        clouds.Clear();
        var r = new Random(11);
        for (int i = 0; i < 6; i++)
        {
            int w = (int)((240 + r.Next(220)) * s), h = (int)((60 + r.Next(50)) * s);
            if (w < 8 || h < 8) continue;
            var art = new Bitmap(w, h, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(art))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                for (int k = 0; k < 9; k++)
                {
                    float pw = w * (0.3f + (float)r.NextDouble() * 0.35f), ph = h * (0.5f + (float)r.NextDouble() * 0.5f);
                    float px = (float)r.NextDouble() * (w - pw), py = h - ph - (float)r.NextDouble() * (h - ph) * 0.6f;
                    using var path = new GraphicsPath();
                    path.AddEllipse(px, py, pw, ph);
                    using var brush = new PathGradientBrush(path) { CenterColor = Color.FromArgb(120, 58, 46, 88), SurroundColors = new[] { Color.FromArgb(0, 58, 46, 88) } };
                    g.FillPath(brush, path);
                }
            }
            clouds.Add(new Cloud { Art = art, X = (float)r.NextDouble() * Width, Y = Height * (0.02f + (float)r.NextDouble() * 0.38f), Speed = (float)(4 + r.NextDouble() * 9) * Math.Max(0.5f, s) });
        }
    }

    // The title, top left in the sky: fitted to the space left of the chimney.
    void BuildTitle()
    {
        title?.Dispose();
        float room = Math.Max(Theme.S(160), o.X + 318 * s - Theme.S(22));
        float em = Theme.S(46);
        using (var probe = Theme.TextPath("FRIGHTWORKS", em, new RectangleF(0, 0, 4000, 400), StringAlignment.Near))
        {
            float w = probe.GetBounds().Width;
            if (w > room) em *= room / w;
        }
        int width = (int)room + Theme.S(30), height = (int)(em * 2.3f) + Theme.S(10);
        title = new Bitmap(Math.Max(1, width), Math.Max(1, height), PixelFormat.Format32bppPArgb);
        using var g = Graphics.FromImage(title);
        Theme.HighQuality(g);
        // Layout boxes far wider than the text: a box too narrow for a word cuts its last letters off.
        Theme.Text(g, "FRIGHTWORKS", em, new RectangleF(Theme.S(6), 0, 4000, em * 1.25f), Theme.Lime, Theme.Ink, Math.Max(3, em * 0.16f), StringAlignment.Near);
        Theme.Text(g, "Night of 100 Frights", em * 0.52f, new RectangleF(Theme.S(8), em * 1.12f, 4000, em * 0.8f), Theme.Cream, Theme.Ink, Math.Max(2, em * 0.11f), StringAlignment.Near);
    }

    // ----- Signs -----
    static readonly Dictionary<string, string> SignText = new()
    {
        ["play"] = "Play", ["settings"] = "Settings", ["mods"] = "Mods", ["extras"] = "Extras", ["exit"] = "Exit",
    };

    PointF[] SignPoints(Sign sg, float grow)
    {
        float cx = sg.Points.Average(p => p.X), cy = sg.Points.Average(p => p.Y);
        return sg.Points.Select(p => Map(cx + (p.X - cx) * grow, cy + (p.Y - cy) * grow)).ToArray();
    }

    void BuildSign(Sign sg)
    {
        sg.Normal?.Dispose();
        sg.Hover?.Dispose();
        (sg.Normal, sg.NormalAt) = RenderSign(sg, false);
        (sg.Hover, sg.HoverAt) = RenderSign(sg, true);
        sg.Hit?.Dispose();
        sg.Hit = new GraphicsPath();
        sg.Hit.AddPolygon(SignPoints(sg, 1f));
    }

    (Bitmap, Point) RenderSign(Sign sg, bool lit)
    {
        var pts = SignPoints(sg, lit ? 1.045f : 1f);
        float minX = pts.Min(p => p.X), minY = pts.Min(p => p.Y), maxX = pts.Max(p => p.X), maxY = pts.Max(p => p.Y);
        int pad = (int)Math.Ceiling(6 * s) + 2;
        var at = new Point((int)Math.Floor(minX) - pad, (int)Math.Floor(minY) - pad);
        var bmp = new Bitmap(Math.Max(1, (int)Math.Ceiling(maxX - minX) + pad * 2), Math.Max(1, (int)Math.Ceiling(maxY - minY) + pad * 2), PixelFormat.Format32bppPArgb);
        using var g = Graphics.FromImage(bmp);
        Theme.HighQuality(g);
        g.TranslateTransform(-at.X, -at.Y);
        var box = new RectangleF(minX, minY, maxX - minX, maxY - minY);
        bool play = sg.Id == "play";
        float u = s * (lit ? 1.045f : 1f);  // picture pixels to client pixels

        using var shape = new GraphicsPath();
        shape.AddPolygon(pts);
        Color top = play ? ColorTranslator.FromHtml("#DDF063") : ColorTranslator.FromHtml("#7B4CBA");
        Color bottom = play ? ColorTranslator.FromHtml("#A3BE33") : ColorTranslator.FromHtml("#4B2884");
        if (lit) { top = ControlPaint.Light(top, play ? 0.35f : 0.3f); bottom = ControlPaint.Light(bottom, 0.3f); }
        using (var fill = new LinearGradientBrush(box, top, bottom, LinearGradientMode.Vertical)) g.FillPath(fill, shape);

        // Wood grain and a couple of knots, the same every time for a sign.
        var r = new Random(sg.Id.Length * 31 + sg.Id[0]);
        var clip = g.Clip;
        g.SetClip(shape);
        using (var grain = new Pen(Color.FromArgb(play ? 55 : 70, 20, 8, 30), Math.Max(1f, 2.2f * u)))
        {
            for (int i = 0; i < 6; i++)
            {
                float y = box.Top + box.Height * (i + 0.5f) / 6 + (float)(r.NextDouble() * 6 - 3) * u;
                var line = new PointF[8];
                for (int k = 0; k < line.Length; k++)
                    line[k] = new PointF(box.Left + box.Width * k / (line.Length - 1), y + (float)(r.NextDouble() * 5 - 2.5) * u);
                g.DrawCurve(grain, line, 0.5f);
            }
            for (int k = 0; k < 2; k++)
            {
                float kx = box.Left + box.Width * (0.25f + (float)r.NextDouble() * 0.5f), ky = box.Top + box.Height * (0.25f + (float)r.NextDouble() * 0.5f);
                g.DrawEllipse(grain, kx, ky, 18 * u, 7 * u);
            }
        }
        using (var shine = new Pen(Color.FromArgb(80, 255, 255, 255), 3 * u))
            g.DrawLine(shine, box.Left + 14 * u, box.Top + 7 * u, box.Right - 14 * u, box.Top + 6 * u);
        g.Clip = clip;
        using (var edge = new Pen(Theme.Ink, 6.5f * u) { LineJoin = LineJoin.Round }) g.DrawPath(edge, shape);
        float n = 4.5f * u;
        foreach (var (nx, ny) in new[] { (box.Left + 17 * u, box.Top + 18 * u), (box.Left + 17 * u, box.Bottom - 18 * u), (box.Right - 17 * u, box.Top + 18 * u), (box.Right - 17 * u, box.Bottom - 18 * u) })
            Theme.Nail(g, nx, ny, n);

        float cy = box.Top + box.Height / 2;
        if (play)
        {
            DrawPaw(g, box.Left + box.Width * 0.13f, cy, 30 * u, Theme.Ink);
            DrawPaw(g, box.Right - box.Width * 0.13f, cy, 30 * u, Theme.Ink);
            FitText(g, SignText[sg.Id], 92 * u, new RectangleF(box.Left + box.Width * 0.24f, box.Top, box.Width * 0.52f, box.Height), Theme.Ink, Color.Empty, 0);
        }
        else
        {
            float ix = box.Left + box.Width * 0.2f;
            DrawIcon(g, sg.Id, ix, cy, u);
            FitText(g, SignText.GetValueOrDefault(sg.Id, sg.Id), 54 * u, new RectangleF(box.Left + box.Width * 0.3f, box.Top, box.Width * 0.62f, box.Height),
                    Theme.Cream, Theme.Ink, 6 * u);
        }
        return (bmp, at);
    }

    static void FitText(Graphics g, string text, float em, RectangleF box, Color fill, Color outline, float outlineWidth)
    {
        using (var probe = Theme.TextPath(text, em, new RectangleF(0, 0, 4000, 1000)))
        {
            float w = probe.GetBounds().Width;
            if (w > box.Width * 0.92f) em *= box.Width * 0.92f / w;
        }
        Theme.Text(g, text, em, box, fill, outline, outlineWidth);
    }

    static void DrawPaw(Graphics g, float cx, float cy, float size, Color color)
    {
        using var b = new SolidBrush(color);
        float u = size / 30f;
        g.FillEllipse(b, cx - 15 * u, cy - 2 * u, 30 * u, 24 * u);
        foreach (var (dx, dy, w, h) in new[] { (-21f, -14f, 11f, 14f), (-9f, -26f, 12f, 15f), (5f, -26f, 12f, 15f), (17f, -14f, 11f, 14f) })
            g.FillEllipse(b, cx + dx * u - 1, cy + dy * u, w * u, h * u);
    }

    static void DrawIcon(Graphics g, string id, float cx, float cy, float u)
    {
        using var cream = new SolidBrush(Theme.Cream);
        using var ink = new Pen(Theme.Ink, 4f * u) { LineJoin = LineJoin.Round };
        using var path = new GraphicsPath { FillMode = FillMode.Alternate };
        switch (id)
        {
            case "settings":
            {
                const int teeth = 8;
                var pts = new List<PointF>();
                for (int i = 0; i < teeth * 2; i++)
                {
                    double a0 = Math.PI * 2 * i / (teeth * 2) - 0.12, a1 = a0 + 0.24;
                    float rad = (i % 2 == 0 ? 25 : 18) * u;
                    pts.Add(new PointF(cx + (float)Math.Cos(a0) * rad, cy + (float)Math.Sin(a0) * rad));
                    pts.Add(new PointF(cx + (float)Math.Cos(a1) * rad, cy + (float)Math.Sin(a1) * rad));
                }
                path.AddPolygon(pts.ToArray());
                path.AddEllipse(cx - 8 * u, cy - 8 * u, 16 * u, 16 * u);
                break;
            }
            case "mods":
            {
                // A jigsaw piece: a square with a knob on top and on the right.
                path.FillMode = FillMode.Winding;
                path.AddRectangle(new RectangleF(cx - 20 * u, cy - 12 * u, 34 * u, 34 * u));
                path.AddEllipse(cx - 11 * u, cy - 27 * u, 17 * u, 18 * u);
                path.AddEllipse(cx + 9 * u, cy - 4 * u, 18 * u, 17 * u);
                break;
            }
            case "extras":
            {
                using var thick = new Pen(Theme.Ink, 13f * u) { StartCap = LineCap.Round, EndCap = LineCap.Round };
                using var handle = new Pen(Theme.Cream, 7f * u) { StartCap = LineCap.Round, EndCap = LineCap.Round };
                g.DrawLine(thick, cx + 9 * u, cy + 9 * u, cx + 22 * u, cy + 22 * u);
                g.DrawLine(handle, cx + 9 * u, cy + 9 * u, cx + 22 * u, cy + 22 * u);
                path.AddEllipse(cx - 21 * u, cy - 21 * u, 34 * u, 34 * u);
                path.AddEllipse(cx - 13 * u, cy - 13 * u, 18 * u, 18 * u);
                break;
            }
            case "exit":
            {
                // A door frame with the door swung open.
                using var frame = new GraphicsPath { FillMode = FillMode.Alternate };
                frame.AddRectangle(new RectangleF(cx - 15 * u, cy - 24 * u, 30 * u, 48 * u));
                frame.AddRectangle(new RectangleF(cx - 10 * u, cy - 19 * u, 20 * u, 43 * u));
                g.DrawPath(ink, frame);
                g.FillPath(cream, frame);
                path.AddPolygon(new[] { new PointF(cx - 10 * u, cy - 19 * u), new PointF(cx + 4 * u, cy - 14 * u), new PointF(cx + 4 * u, cy + 27 * u), new PointF(cx - 10 * u, cy + 24 * u) });
                g.DrawPath(ink, path);
                g.FillPath(cream, path);
                using var knob = new SolidBrush(Theme.Ink);
                g.FillEllipse(knob, cx - 1 * u, cy + 1 * u, 4 * u, 4 * u);
                return;
            }
        }
        g.DrawPath(ink, path);
        g.FillPath(cream, path);
    }

    // ----- Painting -----
    protected override void OnPaint(PaintEventArgs e)
    {
        if (Width <= 0 || Height <= 0) return;
        if (cachedFor != Size || house == null) Rebuild();
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.CompositingQuality = CompositingQuality.HighSpeed;
        g.InterpolationMode = InterpolationMode.Bilinear;
        double t = Now;
        float flash = (float)Flash();

        g.DrawImageUnscaled(sky!, 0, 0);
        foreach (var c in clouds) g.DrawImageUnscaled(c.Art, (int)c.X, (int)c.Y);
        if (flash > 0)
        {
            using var lit = new SolidBrush(Color.FromArgb((int)(120 * flash), 196, 186, 255));
            g.FillRectangle(lit, ClientRectangle);
            if (bolt != null && t - strikeAt < 0.35)
                foreach (var line in bolt)
                {
                    using var glow = new Pen(Color.FromArgb((int)(90 * flash), 210, 200, 255), Math.Max(4, 11 * s)) { LineJoin = LineJoin.Round };
                    using var core = new Pen(Color.FromArgb((int)(255 * Math.Min(1, flash * 1.4)), 255, 255, 255), Math.Max(1.5f, 3 * s)) { LineJoin = LineJoin.Round };
                    g.DrawLines(glow, line);
                    g.DrawLines(core, line);
                }
        }

        foreach (var w in windows) DrawWindow(g, w, t);

        if (flash > 0.02)
        {
            g.DrawImageUnscaled(house!, (int)o.X, (int)o.Y);
            var m = new ColorMatrix(new[]
            {
                new float[] { 0, 0, 0, 0, 0 }, new float[] { 0, 0, 0, 0, 0 }, new float[] { 0, 0, 0, 0, 0 },
                new float[] { 0, 0, 0, 0.3f * flash, 0 }, new float[] { 0.85f, 0.85f, 1f, 0, 1 },
            });
            using var attrs = new ImageAttributes();
            attrs.SetColorMatrix(m);
            g.DrawImage(house!, new Rectangle((int)o.X, (int)o.Y, house!.Width, house.Height), 0, 0, house.Width, house.Height, GraphicsUnit.Pixel, attrs);
        }
        else g.DrawImageUnscaled(house!, (int)o.X, (int)o.Y);

        foreach (var sg in signs)
        {
            bool lit = sg.Id == hover || sg.Id == active;
            float breathe = (float)(Math.Sin(t * 2.6) + 1) / 2;
            if (sg.Id == "play" || lit)
            {
                int a = lit ? 150 : (int)(40 + 70 * breathe);
                using var halo = new GraphicsPath();
                halo.AddPolygon(SignPoints(sg, lit ? 1.045f : 1f));
                Color glowColor = sg.Id == "play" ? Theme.Lime : Theme.Gold;
                using var pen = new Pen(Color.FromArgb(a, glowColor), Math.Max(6, 16 * s)) { LineJoin = LineJoin.Round };
                g.DrawPath(pen, halo);
            }
            if (lit) g.DrawImageUnscaled(sg.Hover!, sg.HoverAt);
            else g.DrawImageUnscaled(sg.Normal!, sg.NormalAt);
        }

        g.DrawImageUnscaled(title!, Theme.S(18), Theme.S(12));
        DrawFooter(g);
    }

    static readonly Color Dark = ColorTranslator.FromHtml("#0D0E1E"), WarmTop = ColorTranslator.FromHtml("#FFE27C"), WarmBottom = ColorTranslator.FromHtml("#F39A2B");

    void DrawWindow(Graphics g, Win w, double t)
    {
        var b = Map(w.Box);
        b.Inflate(1, 1);
        var gr = w.Group;
        float light = (float)(gr.Light * (0.95 + 0.05 * Math.Sin(t * 11 + gr.Seed) * Math.Sin(t * 6.7 + gr.Seed * 2)));
        light = Math.Clamp(light, 0, 1);
        using (var dark = new SolidBrush(Dark)) g.FillRectangle(dark, b);
        if (light > 0.01)
            using (var warm = new LinearGradientBrush(b, Color.FromArgb((int)(255 * light), WarmTop), Color.FromArgb((int)(255 * light), WarmBottom), LinearGradientMode.Vertical))
                g.FillRectangle(warm, b);
        var figure = w.Id == "parlor" ? DogArt : w.Id == "tower" ? GhostArt : null;
        float fig = (float)gr.Figure;
        if (figure != null && fig > 0.01) DrawFaded(g, figure, Map(w.Box), fig);
        float curtain = (float)gr.Curtain;
        if (gr.Curtains && curtain > 0.01)
        {
            DrawCurtains(g, b, curtain, light);
            if (figure != null && fig > 0.01) DrawFaded(g, figure, Map(w.Box), fig * curtain * 0.5f * light);  // its shadow on the curtain
        }
    }

    static void DrawFaded(Graphics g, Bitmap art, RectangleF dest, float alpha)
    {
        var m = new ColorMatrix { Matrix33 = Math.Clamp(alpha, 0, 1) };
        using var attrs = new ImageAttributes();
        attrs.SetColorMatrix(m);
        g.DrawImage(art, Rectangle.Round(dest), 0, 0, art.Width, art.Height, GraphicsUnit.Pixel, attrs);
    }

    static void DrawCurtains(Graphics g, RectangleF b, float amount, float light)
    {
        Color shade = Lerp(ColorTranslator.FromHtml("#2E0B22"), ColorTranslator.FromHtml("#B4453E"), light);
        Color fold = Lerp(ColorTranslator.FromHtml("#1C0616"), ColorTranslator.FromHtml("#7E2830"), light);
        float half = b.Width / 2 + 1, cw = half * amount;
        foreach (var left in new[] { true, false })
        {
            var r = left ? new RectangleF(b.Left, b.Top, cw, b.Height) : new RectangleF(b.Right - cw, b.Top, cw, b.Height);
            if (r.Width < 0.5f) continue;
            using (var brush = new SolidBrush(shade)) g.FillRectangle(brush, r);
            using var pen = new Pen(fold, Math.Max(1, r.Width / 9));
            for (int i = 1; i < 4; i++)
            {
                float x = r.Left + r.Width * i / 4;
                g.DrawLine(pen, x, r.Top, x, r.Bottom);
            }
        }
        using var valance = new SolidBrush(fold);
        g.FillRectangle(valance, b.Left, b.Top, b.Width, Math.Max(2, b.Height * 0.06f * Math.Min(1, amount * 3)));
    }

    static Color Lerp(Color a, Color b, float f) => Color.FromArgb(
        (int)(a.R + (b.R - a.R) * f), (int)(a.G + (b.G - a.G) * f), (int)(a.B + (b.B - a.B) * f));

    void DrawFooter(Graphics g)
    {
        using var font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
        int pad = Theme.S(8), y = Height - Theme.S(30);
        void Pill(string text, Color color, bool right)
        {
            if (text.Length == 0) return;
            var size = TextRenderer.MeasureText(g, text, font);
            int x = right ? Width - size.Width - pad * 2 - Theme.S(14) : Theme.S(14);
            var r = new Rectangle(x, y, size.Width + pad * 2, Theme.S(24));
            using (var bg = Theme.Rounded(r, Theme.S(11)))
            using (var fill = new SolidBrush(Color.FromArgb(185, 10, 6, 18)))
                g.FillPath(fill, bg);
            TextRenderer.DrawText(g, text, font, r, color, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
        Pill(BuildId, Theme.CreamDim, false);   // bottom left: the build ID
        Pill(status, statusOk ? Theme.Good : Theme.Warn, true);   // bottom right: the status
    }

    // The build ID, the same as the game's for one build: just the version for a release, else
    // "<version> branch@commit[+diff hash]" (set at build time: InformationalVersion in the csproj).
    static readonly string BuildId = typeof(MansionScene).Assembly
        .GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
        .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion ?? "";

    // ----- Input -----
    string? SignAt(Point p) => signs.LastOrDefault(sg => sg.Hit != null && sg.Hit.IsVisible(p))?.Id;

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        var now = SignAt(e.Location);
        if (now == hover) return;
        hover = now;
        Cursor = hover != null ? Cursors.Hand : Cursors.Default;
        Invalidate();
    }

    protected override void OnMouseLeave(EventArgs e)
    {
        base.OnMouseLeave(e);
        if (hover == null) return;
        hover = null;
        Invalidate();
    }

    protected override void OnMouseClick(MouseEventArgs e)
    {
        base.OnMouseClick(e);
        if (e.Button == MouseButtons.Left && SignAt(e.Location) is string id) SignClicked?.Invoke(id);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) timer.Dispose();
        base.Dispose(disposing);
    }
}
