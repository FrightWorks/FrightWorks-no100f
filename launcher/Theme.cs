using System.ComponentModel;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;

namespace FrightWorks;

// Colours, the display font and the themed controls of the launcher (night purples, lime and cream, as on the signs).
static class Theme
{
    static Color Hex(string s) => ColorTranslator.FromHtml(s);
    public static readonly Color
        Ink = Hex("#120A1C"), Cream = Hex("#F6E7B8"), CreamDim = Hex("#C9BC98"),
        Lime = Hex("#C8E64A"), LimeDeep = Hex("#9CB834"), Gold = Hex("#FFD46B"),
        Purple = Hex("#6E42AC"), PurpleDeep = Hex("#46257C"),
        BoardTop = Hex("#2A1946"), BoardBottom = Hex("#1B1030"), Field = Hex("#130A24"), FieldHover = Hex("#2A1A48"),
        Good = Hex("#A6E37E"), Warn = Hex("#F4A85E");

    // Fonts are in points and grow with Windows display scaling; pixel sizes go through S().
    public static float K = 1f;
    public static int S(int v) => (int)Math.Round(v * K);
    public static Size S(int w, int h) => new(S(w), S(h));
    public static Padding S(int l, int t, int r, int b) => new(S(l), S(t), S(r), S(b));

    public static Font Body => new("Segoe UI", 10.5f);
    public static Font BodyBold => new("Segoe UI", 10.5f, FontStyle.Bold);
    public static Font Heading => new("Segoe UI Semibold", 13f);

    public static Stream? Resource(string name) => typeof(Theme).Assembly.GetManifestResourceStream("FrightWorks." + name);

    public static byte[] ReadAll(Stream s) { using var m = new MemoryStream(); s.CopyTo(m); return m.ToArray(); }

    // Embedded art as a premultiplied bitmap (the fastest kind for GDI+ to blend).
    public static Bitmap? LoadBitmap(string name)
    {
        using var s = Resource(name);
        if (s == null) return null;
        using var src = new Bitmap(new MemoryStream(ReadAll(s)));
        var b = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppPArgb);
        using var g = Graphics.FromImage(b);
        g.DrawImage(src, new Rectangle(0, 0, src.Width, src.Height));
        return b;
    }

    // Luckiest Guy (launcher\fonts, Apache 2.0), the signs' lettering. Memory fonts only draw through GDI+ paths,
    // never through Label/TextRenderer, so every use goes through Text() below.
    static readonly PrivateFontCollection fonts = new();
    static FontFamily? display;
    public static FontFamily Display => display ??= LoadDisplay();

    static FontFamily LoadDisplay()
    {
        try
        {
            using var s = Resource("LuckiestGuy-Regular.ttf");
            if (s != null)
            {
                var data = ReadAll(s);
                var mem = Marshal.AllocCoTaskMem(data.Length);  // kept for the app's life
                Marshal.Copy(data, 0, mem, data.Length);
                fonts.AddMemoryFont(mem, data.Length);
                if (fonts.Families.Length > 0) return fonts.Families[0];
            }
        }
        catch { }
        return FontFamily.Families.Any(f => f.Name == "Segoe UI Black") ? new FontFamily("Segoe UI Black") : new FontFamily("Segoe UI");
    }

    // Text in the display face, outlined, centred in a box. em is in pixels.
    public static GraphicsPath TextPath(string text, float em, RectangleF box, StringAlignment align = StringAlignment.Center)
    {
        var path = new GraphicsPath();
        using var format = new StringFormat { Alignment = align, LineAlignment = StringAlignment.Center };
        var face = Display;
        path.AddString(text, face, (int)(face.IsStyleAvailable(FontStyle.Regular) ? FontStyle.Regular : FontStyle.Bold), em, box, format);
        return path;
    }

    public static void Text(Graphics g, string text, float em, RectangleF box, Color fill, Color outline, float outlineWidth,
                            StringAlignment align = StringAlignment.Center)
    {
        using var path = TextPath(text, em, box, align);
        if (outlineWidth > 0)
            using (var pen = new Pen(outline, outlineWidth) { LineJoin = LineJoin.Round }) g.DrawPath(pen, path);
        using var brush = new SolidBrush(fill);
        g.FillPath(brush, path);
    }

    public static GraphicsPath Rounded(RectangleF r, float radius)
    {
        var p = new GraphicsPath();
        float d = Math.Max(2, radius * 2);
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    // A plank: a slightly uneven board with cut corners, the shape of the small buttons and the boards' title strip.
    public static GraphicsPath Plank(RectangleF r, int seed)
    {
        var rnd = new Random(seed);
        float c = Math.Min(r.Height * 0.22f, 8f * K), j = Math.Max(1f, r.Height * 0.04f);
        float J() => (float)(rnd.NextDouble() * 2 - 1) * j;
        var p = new GraphicsPath();
        p.AddPolygon(new[]
        {
            new PointF(r.Left + c, r.Top + J()), new PointF(r.Right - c, r.Top + J()), new PointF(r.Right, r.Top + c),
            new PointF(r.Right + J(), r.Bottom - c), new PointF(r.Right - c, r.Bottom + J()), new PointF(r.Left + c, r.Bottom + J()),
            new PointF(r.Left, r.Bottom - c), new PointF(r.Left + J(), r.Top + c),
        });
        return p;
    }

    public static void Nail(Graphics g, float x, float y, float radius)
    {
        using (var ink = new SolidBrush(Ink)) g.FillEllipse(ink, x - radius - 1, y - radius - 1, radius * 2 + 2, radius * 2 + 2);
        using (var steel = new SolidBrush(Hex("#9AA1AD"))) g.FillEllipse(steel, x - radius, y - radius, radius * 2, radius * 2);
        using var shine = new SolidBrush(Color.FromArgb(200, 255, 255, 255));
        g.FillEllipse(shine, x - radius * 0.55f, y - radius * 0.6f, radius * 0.6f, radius * 0.6f);
    }

    public static void HighQuality(Graphics g)
    {
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
    }

    public static Label Note(string text, int width = 520) => new()
    {
        Text = text, AutoSize = true, MaximumSize = new Size(S(width), 0), ForeColor = CreamDim, BackColor = Color.Transparent,
        Font = Body, Margin = S(0, 4, 0, 6),
    };

    public static Label Title(string text) => new()
    {
        Text = text, AutoSize = true, ForeColor = Lime, BackColor = Color.Transparent, Font = Heading, Margin = S(0, 12, 0, 2),
    };

    public static Label FieldLabel(string text) => new()
    {
        Text = text, AutoSize = true, ForeColor = Cream, BackColor = Color.Transparent, Font = BodyBold, Margin = S(0, 9, 14, 6),
    };
}

// A small wooden plank button: purple (or lime for the main action), lighter on hover.
sealed class PlankButton : Button
{
    bool hover, down;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)] public bool Primary { get; init; }
    bool selected;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)] public bool Selected { get => selected; set { selected = value; Invalidate(); } }  // a tab that is open
    readonly int seed;

    public PlankButton(string text, EventHandler onClick, bool primary = false)
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.SupportsTransparentBackColor | ControlStyles.ResizeRedraw, true);
        BackColor = Color.Transparent;
        UseVisualStyleBackColor = false;
        // ButtonBase assumes its own painter fills the rectangle; our plank leaves transparent corners.
        SetStyle(ControlStyles.Opaque, false);
        UseMnemonic = false;
        Text = text;
        Primary = primary;
        seed = text.GetHashCode();
        Cursor = Cursors.Hand;
        Margin = Theme.S(0, 6, 10, 6);
        Click += onClick;
        using var path = Theme.TextPath(text, EmPx, new RectangleF(0, 0, 2000, 200));
        var bounds = path.GetBounds();
        Size = new Size((int)Math.Ceiling(bounds.Width) + Theme.S(34), Theme.S(primary ? 42 : 36));
    }

    float EmPx => Theme.S(Primary ? 20 : 16);

    public override Size GetPreferredSize(Size proposedSize)
    {
        using var path = Theme.TextPath(Text, EmPx, new RectangleF(0, 0, 2000, 200));
        return new Size((int)Math.Ceiling(path.GetBounds().Width) + Theme.S(34), Theme.S(Primary ? 42 : 36));
    }

    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
    protected override void OnTextChanged(EventArgs e)
    {
        base.OnTextChanged(e);
        using var path = Theme.TextPath(Text, EmPx, new RectangleF(0, 0, 2000, 200));
        Width = (int)Math.Ceiling(path.GetBounds().Width) + Theme.S(34);
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.HighQuality(g);
        var r = new RectangleF(2, 2, Width - 5, Height - 5);
        if (down) r.Offset(0, 1);
        bool lime = Primary || selected;
        Color top = lime ? Theme.Lime : Theme.Purple, bottom = lime ? Theme.LimeDeep : Theme.PurpleDeep;
        if (hover && Enabled) { top = ControlPaint.Light(top, 0.25f); bottom = ControlPaint.Light(bottom, 0.25f); }
        if (!Enabled) { top = Color.FromArgb(70, 62, 86); bottom = Color.FromArgb(52, 46, 66); }
        using var plank = Theme.Plank(r, seed);
        using (var fill = new LinearGradientBrush(r, top, bottom, LinearGradientMode.Vertical)) g.FillPath(fill, plank);
        using (var grain = new Pen(Color.FromArgb(45, 0, 0, 0), 1f))
            for (int i = 1; i < 3; i++) g.DrawLine(grain, r.Left + 8, r.Top + r.Height * i / 3, r.Right - 8, r.Top + r.Height * i / 3 + 1);
        using (var edge = new Pen(Theme.Ink, Theme.S(2)) { LineJoin = LineJoin.Round }) g.DrawPath(edge, plank);
        Color text = !Enabled ? Color.FromArgb(150, 140, 160) : lime ? Theme.Ink : Theme.Cream;
        Theme.Text(g, Text, EmPx, r, text, Theme.Ink, lime || !Enabled ? 0 : Theme.S(3));
        if (Focused && ShowFocusCues)
            ControlPaint.DrawFocusRectangle(g, Rectangle.Inflate(ClientRectangle, -Theme.S(6), -Theme.S(6)), text, bottom);
    }
}

// Dark drop-down list in the board's colours.
sealed class ThemedCombo : ComboBox
{
    public ThemedCombo()
    {
        DropDownStyle = ComboBoxStyle.DropDownList;
        AutoSize = true;
        DrawMode = DrawMode.OwnerDrawFixed;
        FlatStyle = FlatStyle.Standard;
        BackColor = Theme.Field;
        ForeColor = Theme.Cream;
        Font = Theme.Body;
        ItemHeight = Theme.S(24);
        Width = Theme.S(320);
        Cursor = Cursors.Hand;
        Margin = Theme.S(0, 4, 0, 4);
    }

    public override Size GetPreferredSize(Size proposedSize)
    {
        var size = base.GetPreferredSize(proposedSize);
        // Owner-drawn item heights can exceed the stock combo's font-based preferred height.
        size.Height = Math.Max(size.Height, Height);
        return size;
    }

    protected override void OnDpiChangedAfterParent(EventArgs e)
    {
        base.OnDpiChangedAfterParent(e);
        ItemHeight = Math.Max(Theme.S(24), Font.Height + Theme.S(6));
    }

    protected override void OnDropDown(EventArgs e)
    {
        int textWidth = Items.Cast<object>().Select(item => TextRenderer.MeasureText(GetItemText(item), Font).Width).DefaultIfEmpty(0).Max();
        DropDownWidth = Math.Min(Screen.FromControl(this).WorkingArea.Width,
            Math.Max(Width, textWidth + SystemInformation.GetVerticalScrollBarWidthForDpi(DeviceDpi) + Theme.S(16)));
        base.OnDropDown(e);
    }

    // The wheel over a closed list scrolls the page instead of changing the choice (it used to change a setting, and save
    // it, just by scrolling past).
    protected override void WndProc(ref Message m)
    {
        const int WM_MOUSEWHEEL = 0x020A;
        if (m.Msg == WM_MOUSEWHEEL && !DroppedDown)
        {
            Control? scroller = Parent;
            while (scroller != null && !(scroller is ScrollableControl { AutoScroll: true })) scroller = scroller.Parent;
            if (scroller != null) SendMessage(scroller.Handle, m.Msg, m.WParam, m.LParam);
            return;
        }
        base.WndProc(ref m);
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    protected override void OnDrawItem(DrawItemEventArgs e)
    {
        if (e.Index < 0) return;
        bool selected = (e.State & DrawItemState.Selected) != 0 && (e.State & DrawItemState.ComboBoxEdit) == 0;
        using (var back = new SolidBrush(selected ? Theme.Purple : Theme.Field)) e.Graphics.FillRectangle(back, e.Bounds);
        var r = e.Bounds;
        r.Inflate(-Theme.S(6), 0);
        TextRenderer.DrawText(e.Graphics, Items[e.Index]?.ToString(), Font, r, Theme.Cream,
                              TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
        e.DrawFocusRectangle();
    }
}

// A thin progress bar in lime on the dark field (the stock bar ignores colours under visual styles).
sealed class ThemedBar : Control
{
    int value, maximum = 100;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)] public int Value { get => value; set { this.value = value; Invalidate(); } }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)] public int Maximum { get => maximum; set { maximum = Math.Max(1, value); Invalidate(); } }

    public ThemedBar()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
        Size = Theme.S(480, 12);
        Margin = Theme.S(0, 6, 0, 6);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Field);
        int w = (int)((long)Width * Math.Clamp(value, 0, maximum) / maximum);
        using (var fill = new SolidBrush(Theme.Lime)) g.FillRectangle(fill, 0, 0, w, Height);
        using var edge = new Pen(Theme.Ink);
        g.DrawRectangle(edge, 0, 0, Width - 1, Height - 1);
    }
}

// A page over the left half of the mansion: a dark board with a title plank, a close plank and scrolling content.
sealed class Board : Panel
{
    public readonly Panel Body;
    readonly string title;
    readonly PlankButton close;
    Size regionSize;
    int regionDpi;

    // Buffer the native child windows as a unit, not just the board's painted background.
    protected override CreateParams CreateParams
    {
        get { var cp = base.CreateParams; cp.ExStyle |= 0x02000000; return cp; } // WS_EX_COMPOSITED
    }

    protected override void OnDpiChangedAfterParent(EventArgs e)
    {
        base.OnDpiChangedAfterParent(e);
        PerformLayout();
    }

    public Board(string title, Action onClose)
    {
        this.title = title;
        DoubleBuffered = true;
        ResizeRedraw = true;
        BackColor = Theme.BoardBottom;
        Padding = Theme.S(26, 74, 18, 22);
        Visible = false;
        close = new PlankButton("X", (_, _) => onClose())
        {
            Anchor = AnchorStyles.Top | AnchorStyles.Right, AccessibleName = "Close " + title,
        };
        Controls.Add(close);
        Body = new ScrollBody { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Theme.BoardBottom };
        Controls.Add(Body);
    }

    // The page's panels and labels are see-through over the board, which doesn't move. Windows scrolls by shifting the
    // old pixels and repainting only the new strip, which smeared the rows; repaint the whole page after every scroll.
    sealed class ScrollBody : Panel
    {
        bool sizing;
        public ScrollBody() => DoubleBuffered = true;

        protected override void OnLayout(LayoutEventArgs e)
        {
            if (!sizing)
            {
                sizing = true;
                try
                {
                    // Reserve the vertical gutter even before the hidden page has a preferred height.
                    int width = Math.Max(1, Width - Padding.Horizontal - SystemInformation.GetVerticalScrollBarWidthForDpi(DeviceDpi));
                    foreach (Control child in Controls) Fit(child, width);
                }
                finally { sizing = false; }
            }
            base.OnLayout(e);
        }

        static void Fit(Control control, int width)
        {
            width = Math.Max(1, width - control.Margin.Horizontal);
            control.SuspendLayout();
            try
            {
                if (control is FlowLayoutPanel flow)
                {
                    if (flow.AutoSize)
                    {
                        flow.MaximumSize = new Size(width, 0);
                        flow.MinimumSize = new Size(width, 0);
                        flow.Width = width;
                    }
                    else flow.Width = Math.Min(Theme.S(480), width);
                    int contentWidth = (flow.AutoSize ? width : flow.Width) - flow.Padding.Horizontal -
                        (flow.AutoScroll ? SystemInformation.GetVerticalScrollBarWidthForDpi(flow.DeviceDpi) : 0);
                    foreach (Control child in flow.Controls) Fit(child, contentWidth);
                }
                else if (control is TableLayoutPanel table)
                {
                    table.MaximumSize = new Size(width, 0);
                    table.MinimumSize = new Size(width, 0);
                    table.Width = width;
                    if (table.ColumnStyles.Count == 0)
                    {
                        table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
                        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
                    }
                    foreach (Control child in table.Controls)
                    {
                        int column = table.GetPositionFromControl(child).Column;
                        if (column < 0) column = table.Controls.GetChildIndex(child) % table.ColumnCount;
                        int columnWidth = (int)(width * (column == 0 ? 0.35f : 0.65f));
                        if (child is ComboBox) child.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
                        else Fit(child, columnWidth);
                    }
                }
                else if (control is Label or CheckBox)
                    control.MaximumSize = new Size(width, 0);
                else if (control is PlankButton button)
                {
                    var preferred = button.GetPreferredSize(Size.Empty);
                    button.Size = new Size(Math.Min(preferred.Width, width), preferred.Height);
                }
                else control.Width = Math.Min(control.Width, width);
            }
            finally { control.ResumeLayout(true); }
        }

        protected override void OnScroll(ScrollEventArgs se) { base.OnScroll(se); Repaint(); }
        protected override void OnMouseWheel(MouseEventArgs e) { base.OnMouseWheel(e); Repaint(); }

        void Repaint()
        {
            Invalidate(true);
            Update();
        }
    }

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (close == null) return;  // layout runs from the constructor, before the close plank exists
        close.Location = new Point(Width - close.Width - Theme.S(16), Theme.S(16));
        if ((regionSize != Size || regionDpi != DeviceDpi) && Width > 0 && Height > 0)
        {
            regionSize = Size;
            regionDpi = DeviceDpi;
            using var shape = Theme.Rounded(new RectangleF(0, 0, Width, Height), Theme.S(16));
            var old = Region;
            Region = new Region(shape);
            old?.Dispose();
        }
    }

    protected override void OnPaintBackground(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.HighQuality(g);
        var r = new RectangleF(0, 0, Width, Height);
        using (var fill = new LinearGradientBrush(r, Theme.BoardTop, Theme.BoardBottom, LinearGradientMode.Vertical)) g.FillRectangle(fill, r);
        // Faint planks across the board.
        using (var seam = new Pen(Color.FromArgb(40, 0, 0, 0), Theme.S(2)))
            for (float y = Theme.S(70); y < Height; y += Theme.S(56)) g.DrawLine(seam, 0, y, Width, y + Theme.S(2));
        var inner = new RectangleF(Theme.S(3), Theme.S(3), Width - Theme.S(6) - 1, Height - Theme.S(6) - 1);
        using (var shape = Theme.Rounded(inner, Theme.S(14)))
        {
            using (var rim = new Pen(Theme.Purple, Theme.S(3))) g.DrawPath(rim, shape);
        }
        using (var outer = Theme.Rounded(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), Theme.S(16)))
        using (var ink = new Pen(Theme.Ink, Theme.S(2))) g.DrawPath(ink, outer);
        Theme.Text(g, title, Theme.S(34), new RectangleF(Theme.S(26), Theme.S(14), Width - Theme.S(120), Theme.S(52)),
                   Theme.Cream, Theme.Ink, Theme.S(5), StringAlignment.Near);
        float n = Theme.S(4);
        Theme.Nail(g, Theme.S(13), Theme.S(13), n);
        Theme.Nail(g, Theme.S(13), Height - Theme.S(13), n);
        Theme.Nail(g, Width - Theme.S(13), Height - Theme.S(13), n);
    }
}
