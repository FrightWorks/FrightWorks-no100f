using System.Globalization;
using Avalonia;
using Avalonia.VisualTree;
using Avalonia.Automation;
using Avalonia.Automation.Peers;
using Avalonia.Automation.Provider;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Templates;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using Avalonia.Media.Imaging;

namespace FrightWorks;

// Colours, fonts, drawing helpers and the themed controls of the launcher (night purples, lime and cream, as on the
// signs). Sizes are in device-independent pixels: Avalonia scales them with the monitor's display scaling.
static class Skin
{
    static Color Hex(string s) => Color.Parse(s);
    public static readonly Color
        Ink = Hex("#120A1C"), Cream = Hex("#F6E7B8"), CreamDim = Hex("#C9BC98"),
        Lime = Hex("#C8E64A"), LimeDeep = Hex("#9CB834"), Gold = Hex("#FFD46B"),
        Purple = Hex("#6E42AC"), PurpleDeep = Hex("#46257C"),
        BoardTop = Hex("#2A1946"), BoardBottom = Hex("#1B1030"), Field = Hex("#130A24"), FieldHover = Hex("#2A1A48"),
        Good = Hex("#A6E37E"), Warn = Hex("#F4A85E");

    public static IBrush Brush(Color c) => new ImmutableSolidColorBrush(c);
    public static Color WithAlpha(Color c, int a) => Color.FromArgb((byte)Math.Clamp(a, 0, 255), c.R, c.G, c.B);
    public static Color Argb(int a, int r, int g, int b) => Color.FromArgb((byte)a, (byte)r, (byte)g, (byte)b);

    // Body text: Segoe UI on Windows (as before), Inter (bundled) elsewhere. Sizes are the old point sizes in pixels.
    public static readonly FontFamily BodyFamily =
        OperatingSystem.IsWindows() ? new FontFamily("Segoe UI") : new FontFamily("fonts:Inter#Inter");
    public const double BodySize = 14, HeadingSize = 17.33, SmallSize = 12.67;

    // Luckiest Guy (launcher/fonts, Apache 2.0), the signs' lettering.
    public static readonly FontFamily Display = new("avares://FrightWorks/fonts#Luckiest Guy");
    static readonly Typeface DisplayFace = new(Display);

    public static Stream? Resource(string name) => typeof(Skin).Assembly.GetManifestResourceStream("FrightWorks." + name);

    public static Bitmap? LoadBitmap(string name)
    {
        using var s = Resource(name);
        return s == null ? null : new Bitmap(s);
    }

    // ----- Text in the display face -----
    public enum Align { Near, Center }

    // The outline of text, centred (or left-aligned) in a box, the way the signs and planks draw it. em is in pixels.
    public static Geometry TextGeometry(string text, double em, Rect box, Align align = Align.Center)
    {
        var formatted = new FormattedText(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, DisplayFace, em, Brushes.Black);  // without a brush BuildGeometry returns nothing
        double x = align == Align.Center ? box.X + (box.Width - formatted.Width) / 2 : box.X;
        double y = box.Y + (box.Height - formatted.Height) / 2;
        return formatted.BuildGeometry(new Point(x, y)) ?? new StreamGeometry();
    }

    public static double TextWidth(string text, double em) =>
        new FormattedText(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, DisplayFace, em, null).Width;

    // Outlined: the outline is drawn first and the fill over it, so half of it shows.
    public static void Text(DrawingContext g, string text, double em, Rect box, Color fill, Color outline, double outlineWidth,
                            Align align = Align.Center)
    {
        var path = TextGeometry(text, em, box, align);
        if (outlineWidth > 0) g.DrawGeometry(null, new Pen(Brush(outline), outlineWidth, lineJoin: PenLineJoin.Round), path);
        g.DrawGeometry(Brush(fill), null, path);
    }

    // ----- Shapes -----
    public static Geometry Rounded(Rect r, double radius)
    {
        double d = Math.Max(1, radius);
        var g = new StreamGeometry();
        using (var c = g.Open())
        {
            var arc = new Size(d, d);
            c.BeginFigure(new Point(r.X + d, r.Y), true);
            c.LineTo(new Point(r.Right - d, r.Y));
            c.ArcTo(new Point(r.Right, r.Y + d), arc, 0, false, SweepDirection.Clockwise);
            c.LineTo(new Point(r.Right, r.Bottom - d));
            c.ArcTo(new Point(r.Right - d, r.Bottom), arc, 0, false, SweepDirection.Clockwise);
            c.LineTo(new Point(r.X + d, r.Bottom));
            c.ArcTo(new Point(r.X, r.Bottom - d), arc, 0, false, SweepDirection.Clockwise);
            c.LineTo(new Point(r.X, r.Y + d));
            c.ArcTo(new Point(r.X + d, r.Y), arc, 0, false, SweepDirection.Clockwise);
            c.EndFigure(true);
        }
        return g;
    }

    public static Geometry Polygon(IReadOnlyList<Point> points, FillRule rule = FillRule.EvenOdd)
    {
        var g = new StreamGeometry();
        using var c = g.Open();
        c.SetFillRule(rule);
        c.BeginFigure(points[0], true);
        for (int i = 1; i < points.Count; i++) c.LineTo(points[i]);
        c.EndFigure(true);
        return g;
    }

    public static Geometry Polyline(IReadOnlyList<Point> points)
    {
        var g = new StreamGeometry();
        using var c = g.Open();
        c.BeginFigure(points[0], false);
        for (int i = 1; i < points.Count; i++) c.LineTo(points[i]);
        c.EndFigure(false);
        return g;
    }

    // A cardinal spline through the points (GDI+ DrawCurve / AddCurve): Bezier segments whose control points are
    // tension / 3 of the neighbours' difference away. closed = filled shape (the end joins the start with a line).
    public static Geometry Curve(IReadOnlyList<Point> p, double tension, bool filled)
    {
        var g = new StreamGeometry();
        using var c = g.Open();
        c.BeginFigure(p[0], filled);
        for (int i = 0; i < p.Count - 1; i++)
        {
            Point p0 = p[Math.Max(0, i - 1)], p1 = p[i], p2 = p[i + 1], p3 = p[Math.Min(p.Count - 1, i + 2)];
            double t = tension / 3;
            c.CubicBezierTo(new Point(p1.X + t * (p2.X - p0.X), p1.Y + t * (p2.Y - p0.Y)),
                            new Point(p2.X - t * (p3.X - p1.X), p2.Y - t * (p3.Y - p1.Y)), p2);
        }
        c.EndFigure(filled);
        return g;
    }

    // A plank: a slightly uneven board with cut corners, the shape of the small buttons.
    public static Geometry Plank(Rect r, int seed)
    {
        var rnd = new Random(seed);
        double c = Math.Min(r.Height * 0.22, 8), j = Math.Max(1, r.Height * 0.04);
        double J() => (rnd.NextDouble() * 2 - 1) * j;
        return Polygon(new[]
        {
            new Point(r.Left + c, r.Top + J()), new Point(r.Right - c, r.Top + J()), new Point(r.Right, r.Top + c),
            new Point(r.Right + J(), r.Bottom - c), new Point(r.Right - c, r.Bottom + J()), new Point(r.Left + c, r.Bottom + J()),
            new Point(r.Left, r.Bottom - c), new Point(r.Left + J(), r.Top + c),
        });
    }

    public static void Nail(DrawingContext g, double x, double y, double radius)
    {
        g.DrawEllipse(Brush(Ink), null, new Rect(x - radius - 1, y - radius - 1, radius * 2 + 2, radius * 2 + 2));
        g.DrawEllipse(Brush(Hex("#9AA1AD")), null, new Rect(x - radius, y - radius, radius * 2, radius * 2));
        g.DrawEllipse(Brush(Argb(200, 255, 255, 255)), null, new Rect(x - radius * 0.55, y - radius * 0.6, radius * 0.6, radius * 0.6));
    }

    public static IBrush Vertical(Color top, Color bottom) => new LinearGradientBrush
    {
        StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
        EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
        GradientStops = { new GradientStop(top, 0), new GradientStop(bottom, 1) },
    };

    // A stable seed from text (string.GetHashCode differs between runs).
    public static int Seed(string text)
    {
        unchecked
        {
            int h = (int)2166136261;
            foreach (char ch in text) h = (h ^ ch) * 16777619;
            return h;
        }
    }

    // WinForms' ControlPaint.Light: the colour moved towards its own highlight in the HLS space Windows uses, so hover
    // colours stay what they were.
    public static Color Light(Color color, float amount)
    {
        const int Range = 240, HLSMax = Range, RGBMax = 255, Undefined = HLSMax * 2 / 3, HilightAdj = 500;
        int r = color.R, gr = color.G, b = color.B;
        int max = Math.Max(Math.Max(r, gr), b), min = Math.Min(Math.Min(r, gr), b), sum = max + min, dif = max - min;
        int lum = ((sum * HLSMax) + RGBMax) / (2 * RGBMax), sat, hue;
        if (dif == 0) { sat = 0; hue = Undefined; }
        else
        {
            sat = lum <= HLSMax / 2 ? ((dif * HLSMax) + (sum / 2)) / sum : ((dif * HLSMax) + ((2 * RGBMax - sum) / 2)) / (2 * RGBMax - sum);
            int rd = (((max - r) * (HLSMax / 6)) + (dif / 2)) / dif;
            int gd = (((max - gr) * (HLSMax / 6)) + (dif / 2)) / dif;
            int bd = (((max - b) * (HLSMax / 6)) + (dif / 2)) / dif;
            hue = r == max ? bd - gd : gr == max ? (HLSMax / 3) + rd - bd : ((2 * HLSMax) / 3) + gd - rd;
            if (hue < 0) hue += HLSMax;
            if (hue > HLSMax) hue -= HLSMax;
        }
        int oneLum = (int)((lum * (1000L - HilightAdj) + (Range + 1L) * HilightAdj) / 1000);
        int newLum = lum + (int)((oneLum - lum) * amount);

        int HueToRgb(int n1, int n2, int h)
        {
            if (h < 0) h += HLSMax;
            if (h > HLSMax) h -= HLSMax;
            if (h < HLSMax / 6) return n1 + (((n2 - n1) * h + (HLSMax / 12)) / (HLSMax / 6));
            if (h < HLSMax / 2) return n2;
            if (h < (HLSMax * 2) / 3) return n1 + (((n2 - n1) * (((HLSMax * 2) / 3) - h) + (HLSMax / 12)) / (HLSMax / 6));
            return n1;
        }
        if (sat == 0)
        {
            byte v = (byte)((newLum * RGBMax) / HLSMax);
            return Color.FromRgb(v, v, v);
        }
        int m2 = newLum <= HLSMax / 2 ? (newLum * (HLSMax + sat) + (HLSMax / 2)) / HLSMax
                                      : newLum + sat - ((newLum * sat) + (HLSMax / 2)) / HLSMax;
        int m1 = 2 * newLum - m2;
        return Color.FromRgb((byte)((HueToRgb(m1, m2, hue + HLSMax / 3) * RGBMax + (HLSMax / 2)) / HLSMax),
                             (byte)((HueToRgb(m1, m2, hue) * RGBMax + (HLSMax / 2)) / HLSMax),
                             (byte)((HueToRgb(m1, m2, hue - HLSMax / 3) * RGBMax + (HLSMax / 2)) / HLSMax));
    }

    // ----- Text blocks for the boards -----
    public static TextBlock Note(string text, double width = 520) => new()
    {
        Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = width, HorizontalAlignment = HorizontalAlignment.Left,
        Foreground = Brush(CreamDim), FontFamily = BodyFamily, FontSize = BodySize, Margin = new Thickness(0, 4, 0, 6),
    };

    public static TextBlock Title(string text) => new()
    {
        Text = text, Foreground = Brush(Lime), FontFamily = BodyFamily, FontSize = HeadingSize, FontWeight = FontWeight.SemiBold,
        Margin = new Thickness(0, 12, 0, 2), TextWrapping = TextWrapping.Wrap,
    };

    public static TextBlock FieldLabel(string text) => new()
    {
        Text = text, Foreground = Brush(Cream), FontFamily = BodyFamily, FontSize = BodySize, FontWeight = FontWeight.Bold,
        Margin = new Thickness(0, 9, 14, 6), TextWrapping = TextWrapping.Wrap, HorizontalAlignment = HorizontalAlignment.Left,
    };
}

// A small wooden plank button: purple (or lime for the main action), lighter on hover. Enter or Space presses it.
sealed class PlankButton : Control
{
    string text;
    readonly int seed;
    bool down, selected;
    public bool Primary { get; }
    public event EventHandler? Click;

    public PlankButton(string text, EventHandler onClick, bool primary = false)
    {
        this.text = text;
        Primary = primary;
        seed = Skin.Seed(text);
        Focusable = true;
        Cursor = new Cursor(StandardCursorType.Hand);
        Margin = new Thickness(0, 6, 10, 6);
        HorizontalAlignment = HorizontalAlignment.Left;
        AutomationProperties.SetName(this, text);
        Click += onClick;
    }

    public string Text
    {
        get => text;
        set
        {
            if (text == value) return;
            text = value;
            AutomationProperties.SetName(this, value);
            InvalidateMeasure();
            InvalidateVisual();
        }
    }

    public bool Selected { get => selected; set { selected = value; InvalidateVisual(); } }  // a tab that is open

    double EmPx => Primary ? 20 : 16;

    protected override Size MeasureOverride(Size availableSize) =>
        new(Math.Min(Math.Ceiling(Skin.TextWidth(text, EmPx)) + 34, availableSize.Width), Primary ? 42 : 36);

    public void PerformClick()
    {
        if (IsEffectivelyEnabled) Click?.Invoke(this, EventArgs.Empty);
    }

    protected override void OnPointerEntered(PointerEventArgs e) { base.OnPointerEntered(e); InvalidateVisual(); }
    protected override void OnPointerExited(PointerEventArgs e) { base.OnPointerExited(e); InvalidateVisual(); }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        down = true;
        e.Pointer.Capture(this);
        e.Handled = true;
        InvalidateVisual();
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        if (!down) return;
        down = false;
        e.Pointer.Capture(null);
        e.Handled = true;
        InvalidateVisual();
        if (new Rect(Bounds.Size).Contains(e.GetPosition(this))) PerformClick();
    }

    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e)
    {
        base.OnPointerCaptureLost(e);
        down = false;
        InvalidateVisual();
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (e.Key == Key.Enter) { PerformClick(); e.Handled = true; }
        else if (e.Key == Key.Space) { down = true; InvalidateVisual(); e.Handled = true; }
    }

    protected override void OnKeyUp(KeyEventArgs e)
    {
        base.OnKeyUp(e);
        if (e.Key != Key.Space || !down) return;
        down = false;
        InvalidateVisual();
        e.Handled = true;
        PerformClick();
    }

    protected override void OnGotFocus(FocusChangedEventArgs e) { base.OnGotFocus(e); InvalidateVisual(); }
    protected override void OnLostFocus(FocusChangedEventArgs e) { base.OnLostFocus(e); InvalidateVisual(); }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == IsEffectivelyEnabledProperty) InvalidateVisual();
    }

    public override void Render(DrawingContext g)
    {
        var r = new Rect(2, 2, Bounds.Width - 5, Bounds.Height - 5);
        if (down) r = r.Translate(new Vector(0, 1));
        bool lime = Primary || selected, enabled = IsEffectivelyEnabled;
        Color top = lime ? Skin.Lime : Skin.Purple, bottom = lime ? Skin.LimeDeep : Skin.PurpleDeep;
        if (IsPointerOver && enabled) { top = Skin.Light(top, 0.25f); bottom = Skin.Light(bottom, 0.25f); }
        if (!enabled) { top = Color.FromRgb(70, 62, 86); bottom = Color.FromRgb(52, 46, 66); }
        var plank = Skin.Plank(r, seed);
        g.DrawGeometry(Skin.Vertical(top, bottom), null, plank);
        var grain = new Pen(Skin.Brush(Skin.Argb(45, 0, 0, 0)), 1);
        for (int i = 1; i < 3; i++)
            g.DrawLine(grain, new Point(r.Left + 8, r.Top + r.Height * i / 3), new Point(r.Right - 8, r.Top + r.Height * i / 3 + 1));
        g.DrawGeometry(null, new Pen(Skin.Brush(Skin.Ink), 2, lineJoin: PenLineJoin.Round), plank);
        Color color = !enabled ? Color.FromRgb(150, 140, 160) : lime ? Skin.Ink : Skin.Cream;
        Skin.Text(g, text, EmPx, r, color, Skin.Ink, lime || !enabled ? 0 : 3);
        if (IsFocused && PseudoClasses.Contains(":focus-visible"))
            g.DrawRectangle(null, new Pen(Skin.Brush(color), 1, new DashStyle(new double[] { 1, 2 }, 0)), new Rect(Bounds.Size).Deflate(6));
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new Peer(this);

    sealed class Peer : ControlAutomationPeer, IInvokeProvider
    {
        public Peer(PlankButton owner) : base(owner) { }
        protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Button;
        protected override string? GetNameCore() => AutomationProperties.GetName(Owner) is { Length: > 0 } name ? name : ((PlankButton)Owner).Text;
        protected override bool IsContentElementCore() => true;
        protected override bool IsControlElementCore() => true;
        public void Invoke() => ((PlankButton)Owner).PerformClick();
    }
}

// Dark drop-down list in the board's colours (App styles it). The wheel over a closed list scrolls the page instead of
// changing the choice (it used to change a setting, and save it, just by scrolling past).
sealed class ThemedCombo : ComboBox
{
    protected override Type StyleKeyOverride => typeof(ComboBox);
    public event EventHandler? SelectedIndexChanged;
    public event EventHandler? SelectionCommitted;

    public void CommitSelection(int index)
    {
        SelectedIndex = index;
        SelectionCommitted?.Invoke(this, EventArgs.Empty);
    }

    void Committed() => SelectionCommitted?.Invoke(this, EventArgs.Empty);

    protected override Control CreateContainerForItemOverride(object? item, int index, object? recycleKey) => new ChoiceItem(this);

    sealed class ChoiceItem : ComboBoxItem
    {
        readonly ThemedCombo combo;
        public ChoiceItem(ThemedCombo combo) => this.combo = combo;
        protected override Type StyleKeyOverride => typeof(ComboBoxItem);
        protected override AutomationPeer OnCreateAutomationPeer() => new ChoicePeer(this);
        sealed class ChoicePeer : ListItemAutomationPeer, ISelectionItemProvider
        {
            readonly ChoiceItem item;
            public ChoicePeer(ChoiceItem item) : base(item) => this.item = item;
            void ISelectionItemProvider.Select()
            {
                base.Select();
                item.combo.Committed();
            }
        }
    }

    public override bool UpdateSelectionFromEvent(Control source, Avalonia.Interactivity.RoutedEventArgs e)
    {
        bool selected = base.UpdateSelectionFromEvent(source, e);
        if (selected) Committed();
        return selected;
    }

    public ThemedCombo()
    {
        Cursor = new Cursor(StandardCursorType.Hand);
        Margin = new Thickness(0, 4, 0, 4);
        MinWidth = 80;
        HorizontalAlignment = HorizontalAlignment.Stretch;
        VerticalAlignment = VerticalAlignment.Top;
        SelectionChanged += (_, _) => SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
        AddHandler(PointerReleasedEvent, (_, e) =>
        {
            if (e.InitialPressMouseButton == MouseButton.Left && e.Source is Control source &&
                (source is ComboBoxItem || source.GetVisualAncestors().OfType<ComboBoxItem>().Any()))
                SelectionCommitted?.Invoke(this, EventArgs.Empty);
        }, Avalonia.Interactivity.RoutingStrategies.Bubble, handledEventsToo: true);
        // One line each, cut with an ellipsis when the list is narrower than a choice.
        ItemTemplate = new FuncDataTemplate<object?>((item, _) =>
            new TextBlock { Text = item?.ToString(), TextWrapping = TextWrapping.NoWrap, TextTrimming = TextTrimming.CharacterEllipsis });
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        if (IsDropDownOpen) base.OnPointerWheelChanged(e);
        // Closed: not handled, so it bubbles up to the board's scroll viewer.
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        bool commit = IsDropDownOpen && e.Key is Key.Enter or Key.Space;
        base.OnKeyDown(e);
        if (commit) SelectionCommitted?.Invoke(this, EventArgs.Empty);
    }
}

// A thin progress bar in lime on the dark field.
sealed class ThemedBar : Control
{
    int value, maximum = 100;
    public int Value { get => value; set { this.value = value; InvalidateVisual(); } }
    public int Maximum { get => maximum; set { maximum = Math.Max(1, value); InvalidateVisual(); } }

    public ThemedBar()
    {
        Height = 12;
        HorizontalAlignment = HorizontalAlignment.Stretch;
        Margin = new Thickness(0, 6, 0, 6);
    }

    public override void Render(DrawingContext g)
    {
        var all = new Rect(Bounds.Size);
        g.FillRectangle(Skin.Brush(Skin.Field), all);
        double w = all.Width * Math.Clamp(value, 0, maximum) / maximum;
        g.FillRectangle(Skin.Brush(Skin.Lime), new Rect(0, 0, w, all.Height));
        g.DrawRectangle(null, new Pen(Skin.Brush(Skin.Ink), 1), all.Deflate(0.5));
    }
}

// A page over the left half of the mansion: a dark board with a title, a close plank and scrolling content.
sealed class Board : Panel
{
    public readonly StackPanel Body;
    public readonly ScrollViewer Scroller;
    readonly PlankButton close;
    readonly Backdrop backdrop;
    static readonly Thickness Inset = new(26, 74, 18, 22);

    public Board(string title, Action onClose)
    {
        BoardTitle = title;
        IsVisible = false;
        backdrop = new Backdrop(title);
        close = new PlankButton("X", (_, _) => onClose()) { Margin = default };
        AutomationProperties.SetName(close, "Close " + title);
        // The gutter before the scroll bar is the page's margin: a ScrollViewer's Padding is left out when it measures
        // the page but taken off when it lays it out, so the page came out wider than the room it got.
        Body = new StackPanel { Orientation = Orientation.Vertical, Margin = new Thickness(0, 0, 14, 0) };
        Scroller = new ScrollViewer
        {
            Content = Body, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, AllowAutoHide = false,
        };
        Children.Add(backdrop);
        Children.Add(Scroller);
        Children.Add(close);
    }

    public string BoardTitle { get; }

    protected override Size MeasureOverride(Size availableSize)
    {
        backdrop.Measure(availableSize);
        close.Measure(availableSize);
        Scroller.Measure(new Size(Math.Max(0, availableSize.Width - Inset.Left - Inset.Right),
                                  Math.Max(0, availableSize.Height - Inset.Top - Inset.Bottom)));
        return availableSize;
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        backdrop.Arrange(new Rect(finalSize));
        Scroller.Arrange(new Rect(Inset.Left, Inset.Top, Math.Max(0, finalSize.Width - Inset.Left - Inset.Right),
                                  Math.Max(0, finalSize.Height - Inset.Top - Inset.Bottom)));
        close.Arrange(new Rect(finalSize.Width - close.DesiredSize.Width - 16, 16, close.DesiredSize.Width, close.DesiredSize.Height));
        Clip = Skin.Rounded(new Rect(finalSize), 16);
        return finalSize;
    }

    // The board itself: planks, rims, nails and the title. It also takes the pointer over the whole board.
    sealed class Backdrop : Control
    {
        readonly string title;
        public Backdrop(string title) => this.title = title;

        public override void Render(DrawingContext g)
        {
            var r = new Rect(Bounds.Size);
            g.FillRectangle(Skin.Vertical(Skin.BoardTop, Skin.BoardBottom), r);
            // Faint planks across the board.
            var seam = new Pen(Skin.Brush(Skin.Argb(40, 0, 0, 0)), 2);
            for (double y = 70; y < r.Height; y += 56) g.DrawLine(seam, new Point(0, y), new Point(r.Width, y + 2));
            g.DrawGeometry(null, new Pen(Skin.Brush(Skin.Purple), 3), Skin.Rounded(new Rect(3, 3, r.Width - 7, r.Height - 7), 14));
            g.DrawGeometry(null, new Pen(Skin.Brush(Skin.Ink), 2), Skin.Rounded(new Rect(0.5, 0.5, r.Width - 1.5, r.Height - 1.5), 16));
            Skin.Text(g, title, 34, new Rect(26, 14, Math.Max(1, r.Width - 120), 52), Skin.Cream, Skin.Ink, 5, Skin.Align.Near);
            Skin.Nail(g, 13, 13, 4);
            Skin.Nail(g, 13, r.Height - 13, 4);
            Skin.Nail(g, r.Width - 13, r.Height - 13, 4);
        }
    }
}

// Lays its child out at the full width given, up to a cap, against the left edge (a capped Stretch would centre it).
sealed class LeftCap : Decorator
{
    readonly double cap;
    public LeftCap(double cap, Control child)
    {
        this.cap = cap;
        Child = child;
        HorizontalAlignment = HorizontalAlignment.Stretch;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        if (Child == null) return default;
        double width = Math.Min(availableSize.Width, cap);
        Child.Measure(new Size(width, availableSize.Height));
        return new Size(double.IsInfinity(width) ? Child.DesiredSize.Width : width, Child.DesiredSize.Height);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        Child?.Arrange(new Rect(0, 0, Math.Min(finalSize.Width, cap), finalSize.Height));
        return finalSize;
    }
}

// Two columns, label and field, like the old table: the label column sizes to the labels, up to 240 pixels or 40% of
// the width (then labels wrap), and the field fills the rest.
sealed class FormTable : Grid
{
    readonly List<Control> labels = new();

    public FormTable()
    {
        ColumnDefinitions = new ColumnDefinitions("Auto,*");
        Margin = new Thickness(0, 2, 0, 6);
        HorizontalAlignment = HorizontalAlignment.Stretch;
    }

    public void Add(Control label, Control field)
    {
        int row = RowDefinitions.Count;
        RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        SetRow(label, row);
        SetColumn(label, 0);
        SetRow(field, row);
        SetColumn(field, 1);
        label.MaxWidth = 240;
        labels.Add(label);
        Children.Add(label);
        Children.Add(field);
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        double cap = double.IsInfinity(availableSize.Width) ? 240 : Math.Clamp(availableSize.Width * 0.4, 60, 240);
        foreach (var label in labels)
            if (label.MaxWidth != cap) label.MaxWidth = cap;
        return base.MeasureOverride(availableSize);
    }
}
