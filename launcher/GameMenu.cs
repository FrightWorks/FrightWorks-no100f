using System.Globalization;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Automation.Peers;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.VisualTree;
using static FrightWorks.Skin;

namespace FrightWorks;

// The launcher's menus, laid out like the game's F10 menu (settings_overlay.cpp, menu_rows.h): a full-window screen over
// the dimmed mansion with a title and tabs (LB / RB) at the top, one full-width row per setting on the left, a details
// panel for the selected row on the right (under the list in a narrow window) and the button prompts at the bottom.
// Up / down pick a row (stopping at the ends), left / right change its value (wrapping), A switches or opens it.

// The button prompts' pictures: the game's own icons (Play/button_icons, made from Kenney's Input Prompts by
// tools/make-button-icons.py), for the controller in use or the keyboard, whichever was used last.
static class Prompts
{
    public enum Button { Confirm, Back, Previous, Next, Change, Move, Extra }

    static string? folder;
    static Gamepads.Kind kind = Gamepads.Kind.Xbox;
    static readonly Dictionary<string, Bitmap?> cache = new();

    // The last input came from a controller (else the keyboard, which is also the start).
    public static bool Pad { get; private set; }
    public static event Action? Changed;

    public static void SetFolder(string play)
    {
        folder = Path.Combine(play, "button_icons");
        cache.Clear();
    }

    public static void UsedKeyboard()
    {
        if (!Pad) return;
        Pad = false;
        Changed?.Invoke();
    }

    public static void UsedPad(Gamepads.Kind? padKind)
    {
        var k = padKind ?? kind;
        if (Pad && k == kind) return;
        Pad = true;
        kind = k;
        Changed?.Invoke();
    }

    // The icons come by button position: Nintendo's south button is its B, PlayStation's is the cross.
    static string Family => kind switch { Gamepads.Kind.PlayStation => "playstation", Gamepads.Kind.Switch => "nintendo", _ => "xbox" };

    static string Name(Button b) => Pad
        ? b switch
        {
            Button.Confirm => "south", Button.Back => "east", Button.Previous => "left_shoulder", Button.Next => "right_shoulder",
            Button.Change => "dpad_horizontal", Button.Extra => "west", _ => "dpad_vertical",
        }
        : b switch
        {
            Button.Confirm => "enter", Button.Back => "escape", Button.Previous => "q", Button.Next => "e",
            Button.Change => "arrows_horizontal", Button.Extra => "delete", _ => "arrows_vertical",
        };

    static Bitmap? Icon(Button b)
    {
        if (folder == null) return null;
        var path = Path.Combine(folder, Pad ? Family : "keyboard", Name(b) + ".png");
        if (!cache.TryGetValue(path, out var bitmap))
        {
            try { bitmap = File.Exists(path) ? new Bitmap(path) : null; }
            catch (Exception) { bitmap = null; }
            cache[path] = bitmap;
        }
        return bitmap;
    }

    // Without the icon files (a fresh source checkout): the button's name in a light chip.
    static string Fallback(Button b) => Pad
        ? b switch
        {
            Button.Confirm => kind == Gamepads.Kind.PlayStation ? "Cross" : kind == Gamepads.Kind.Switch ? "B" : "A",
            Button.Back => kind == Gamepads.Kind.PlayStation ? "Circle" : kind == Gamepads.Kind.Switch ? "A" : "B",
            Button.Previous => kind == Gamepads.Kind.PlayStation ? "L1" : kind == Gamepads.Kind.Switch ? "L" : "LB",
            Button.Next => kind == Gamepads.Kind.PlayStation ? "R1" : kind == Gamepads.Kind.Switch ? "R" : "RB",
            Button.Extra => kind == Gamepads.Kind.PlayStation ? "Square" : kind == Gamepads.Kind.Switch ? "Y" : "X",
            Button.Change => "◂▸", _ => "▴▾",
        }
        : b switch
        {
            Button.Confirm => "Enter", Button.Back => "Esc", Button.Previous => "Q", Button.Next => "E",
            Button.Change => "←→", Button.Extra => "Del", _ => "↑↓",
        };

    public static double Width(Button b, double height)
    {
        var icon = Icon(b);
        if (icon != null) return height * icon.Size.Width / icon.Size.Height;
        return Math.Max(height, MenuText.Make(Fallback(b), height * 0.45, Ink, FontWeight.Bold).Width + height * 0.5);
    }

    // Draws the prompt with its top-left corner at (x, y); returns its width.
    public static double Draw(DrawingContext g, Button b, double x, double y, double height)
    {
        var icon = Icon(b);
        if (icon != null)
        {
            double w = height * icon.Size.Width / icon.Size.Height;
            g.DrawImage(icon, new Rect(x, y, w, height));
            return w;
        }
        var text = MenuText.Make(Fallback(b), height * 0.45, Ink, FontWeight.Bold);
        double width = Math.Max(height, text.Width + height * 0.5);
        var r = new Rect(x, y + height * 0.1, width, height * 0.8);
        g.DrawGeometry(Brush(Cream), null, Rounded(r, r.Height / 2));
        g.DrawText(text, new Point(r.X + (r.Width - text.Width) / 2, r.Y + (r.Height - text.Height) / 2));
        return width;
    }
}

static class MenuText
{
    static readonly Typeface DisplayFace = new(Skin.Display);

    // One line of body text; with a width, cut with an ellipsis.
    public static FormattedText Make(string text, double size, Color color, FontWeight weight = FontWeight.Normal, double maxWidth = double.PositiveInfinity)
    {
        var t = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface(Skin.BodyFamily, FontStyle.Normal, weight), size, Skin.Brush(color));
        Fit(t, maxWidth);
        return t;
    }

    // The signs' lettering (Luckiest Guy), for titles, tabs and headings.
    public static FormattedText Display(string text, double size, Color color, double maxWidth = double.PositiveInfinity)
    {
        var t = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, DisplayFace, size, Skin.Brush(color));
        Fit(t, maxWidth);
        return t;
    }

    static void Fit(FormattedText t, double maxWidth)
    {
        if (double.IsInfinity(maxWidth)) return;
        t.MaxTextWidth = Math.Max(1, maxWidth);
        t.MaxLineCount = 1;
        t.Trimming = TextTrimming.CharacterEllipsis;
    }

    // Display text with a soft dark drop under it, as the F10 titles have. Returns the text drawn (for its size).
    public static FormattedText DrawDisplay(DrawingContext g, string text, double size, Color color, Point at, double maxWidth = double.PositiveInfinity)
    {
        g.DrawText(Display(text, size, Skin.Argb(170, 8, 4, 16), maxWidth), at + new Vector(0, 2.5));
        var main = Display(text, size, color, maxWidth);
        g.DrawText(main, at);
        return main;
    }
}

// ----- Rows -----

// One row of a menu: its name on the left, its value on the right. It can take the keyboard focus, which is the menu's
// selection; a click selects it (and on its value, changes it).
abstract class MenuRow : Control
{
    public const double RowHeight = 46;
    const double Pad = 14, LabelLeft = 22;
    string label;
    bool selected;

    protected MenuRow(string label, string? details)
    {
        this.label = label;
        Details = details;
        Focusable = true;
        FocusAdorner = null;  // the selection highlight is the focus mark
        Height = RowHeight;
        Margin = new Thickness(0, 1, 0, 1);
        Cursor = new Cursor(StandardCursorType.Hand);
        AutomationProperties.SetName(this, label);
    }

    public string Label
    {
        get => label;
        set
        {
            if (label == value) return;
            label = value;
            AutomationProperties.SetName(this, value);
            InvalidateVisual();
            MenuScreen.Of(this)?.RowChanged(this);
        }
    }

    // The details panel's text while this row is selected.
    public string? Details { get; set; }

    public bool Selected
    {
        get => selected;
        internal set
        {
            if (selected == value) return;
            selected = value;
            InvalidateVisual();
        }
    }

    // What A does here, for the prompt bar (null: nothing). Changes: left / right change the value.
    public virtual string? ConfirmLabel => "Select";
    public virtual bool Changes => false;
    public virtual void Change(int direction) { }
    public virtual void Activate() { }
    // X (Delete): a second action, when ExtraLabel is set.
    public virtual string? ExtraLabel => null;
    public virtual void Extra() { }
    // A row in a mode of its own (a pack being moved) takes the actions first (true: used) and shows its own prompts.
    // Busy: in that mode; Settle ends it (the selection is leaving it).
    public virtual bool Handle(LauncherButtons action) => false;
    public virtual IReadOnlyList<PromptBar.Item>? OwnPrompts => null;
    public virtual bool Busy => false;
    public virtual void Settle() { }

    // On / off drawn the console way, like a two-value choice: an arrow each side of On or Off and two dots (Off, On).
    // Left turns it off and right on, so the arrow that would do nothing now is dimmed.
    protected void DrawSwitch(DrawingContext g, bool on, bool arrows = true)
    {
        var v = ValueRect;
        bool enabled = IsEffectivelyEnabled;
        double mid = v.Height / 2 - 3;
        Color Arrow(bool live) => !enabled ? WithAlpha(CreamDim, 70) : !live ? WithAlpha(CreamDim, 60) : Selected ? Lime : WithAlpha(CreamDim, 150);
        if (arrows)
        {
            MenuRow.Arrow(g, new Point(v.X + 12, mid), -1, Arrow(on));
            MenuRow.Arrow(g, new Point(v.Right - 12, mid), 1, Arrow(!on));
        }
        var text = MenuText.Make(on ? "On" : "Off", 15, on && enabled ? Cream : TextColor(false), on ? FontWeight.SemiBold : FontWeight.Normal);
        g.DrawText(text, new Point(Math.Round(v.X + (v.Width - text.Width) / 2), Math.Round(mid - text.Height / 2)));
        double y = mid + text.Height / 2 + 5, x = v.X + v.Width / 2 - 4.5;
        g.DrawEllipse(Brush(!on && enabled ? Lime : WithAlpha(CreamDim, 90)), null, new Point(x, y), 2, 2);
        g.DrawEllipse(Brush(on && enabled ? Lime : WithAlpha(CreamDim, 90)), null, new Point(x + 9, y), 2, 2);
    }

    protected double ValueWidth => Math.Clamp(Bounds.Width * 0.46, 170, 360);
    protected Rect ValueRect => new(Bounds.Width - Pad - ValueWidth, 0, ValueWidth, Bounds.Height);
    protected virtual bool HasValue => true;

    protected override void OnGotFocus(FocusChangedEventArgs e)
    {
        base.OnGotFocus(e);
        MenuScreen.Of(this)?.RowFocused(this);
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (!Focusable || !IsEffectivelyEnabled || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        e.Handled = true;
        Focus(NavigationMethod.Pointer);
        Click(e.GetPosition(this));
    }

    // A click: by default it does what A does.
    protected virtual void Click(Point at) => Activate();

    // The pointer moves the selection, as the pad does (only when it really moves: a list scrolled by the pad under a
    // resting pointer keeps the pad's row). No scrolling here: the row is already under the pointer.
    static Point lastPointer = new(double.NaN, double.NaN);

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        if (TopLevel.GetTopLevel(this) is not { } top) return;
        var at = e.GetPosition(top);
        if (at == lastPointer) return;
        lastPointer = at;
        if (!selected && Focusable && IsEffectivelyEnabled && MenuScreen.Of(this) is { Selected.Busy: not true } screen)
            screen.Select(this, scroll: false);
    }

    protected override void OnPointerEntered(PointerEventArgs e) { base.OnPointerEntered(e); InvalidateVisual(); }
    protected override void OnPointerExited(PointerEventArgs e) { base.OnPointerExited(e); InvalidateVisual(); }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == IsEffectivelyEnabledProperty) InvalidateVisual();
    }

    // Something about the value changed: redraw it, and the prompts and details if this row is the selected one.
    protected void ValueChanged()
    {
        InvalidateVisual();
        MenuScreen.Of(this)?.RowChanged(this);
    }

    protected Color TextColor(bool bright) => !IsEffectivelyEnabled ? WithAlpha(CreamDim, 110) : bright ? Cream : CreamDim;

    public override void Render(DrawingContext g)
    {
        var r = new Rect(Bounds.Size);
        if (selected)
        {
            g.DrawGeometry(Brush(Argb(210, 72, 45, 118)), new Pen(Brush(WithAlpha(Lime, 120)), 1), Rounded(r.Deflate(0.5), 9));
            g.DrawGeometry(Brush(Lime), null, Rounded(new Rect(6, 10, 4, Math.Max(4, r.Height - 20)), 2));
        }
        else if (IsPointerOver && Focusable && IsEffectivelyEnabled)
            g.DrawGeometry(Brush(Argb(24, 255, 255, 255)), null, Rounded(r, 9));
        double right = HasValue ? ValueRect.X - 12 : r.Width - Pad;
        var text = MenuText.Make(label, 15.5, TextColor(selected), FontWeight.SemiBold, Math.Max(1, right - LabelLeft));
        g.DrawText(text, new Point(LabelLeft, Math.Round((r.Height - text.Height) / 2)));
        if (HasValue) RenderValue(g);
    }

    protected abstract void RenderValue(DrawingContext g);

    // Screen readers: a choice is a combo box, a switch a check box, an action a button.
    protected override AutomationPeer OnCreateAutomationPeer() => new RowPeer(this);

    sealed class RowPeer(MenuRow row) : ControlAutomationPeer(row)
    {
        protected override AutomationControlType GetAutomationControlTypeCore() => row switch
        {
            ChoiceRow => AutomationControlType.ComboBox,
            ToggleRow => AutomationControlType.CheckBox,
            ActionRow => AutomationControlType.Button,
            _ => AutomationControlType.Text,
        };
    }

    // A small filled triangle pointing left (-1) or right (1).
    protected static void Arrow(DrawingContext g, Point center, int direction, Color color, double size = 6)
    {
        var tip = new Point(center.X + direction * size, center.Y);
        var a = new Point(center.X - direction * size * 0.6, center.Y - size);
        var b = new Point(center.X - direction * size * 0.6, center.Y + size);
        g.DrawGeometry(Brush(color), null, Polygon(new[] { tip, a, b }));
    }
}

// One of a list of values (a drop-down, laid out the F10 way): left / right step through them, wrapping; A steps too, or
// opens the whole list in a picker when there are more than six. Mirrors the ComboBox API the settings code used:
// SelectedIndexChanged fires on any change, from code too; SelectionCommitted only when the player picks (also the
// value it already had).
sealed class ChoiceRow : MenuRow
{
    public readonly List<object?> Items = new();
    int index = -1;
    public event EventHandler? SelectedIndexChanged;
    public event EventHandler? SelectionCommitted;

    public ChoiceRow(string label, string? details = null) : base(label, details) { }

    public int SelectedIndex
    {
        get => index;
        set
        {
            if (index == value) return;
            index = value;
            ValueChanged();
            SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public object? SelectedItem => index >= 0 && index < Items.Count ? Items[index] : null;
    public int ItemCount => Items.Count;

    // After Items changed in place.
    public void Refresh() => ValueChanged();

    bool Long => Items.Count > 6;
    public override bool Changes => Items.Count > 1;
    // A short list: A steps it too, but the prompts show only Change (as F10's do).
    public override string? ConfirmLabel => Long ? "List" : null;

    public override void Change(int direction)
    {
        if (!IsEffectivelyEnabled || Items.Count == 0) return;
        int next = index < 0 ? (direction > 0 ? 0 : Items.Count - 1) : ((index + direction) % Items.Count + Items.Count) % Items.Count;
        CommitSelection(next);
    }

    public void CommitSelection(int value)
    {
        SelectedIndex = value;
        SelectionCommitted?.Invoke(this, EventArgs.Empty);
    }

    public override void Activate()
    {
        if (!IsEffectivelyEnabled || Items.Count <= 1) return;
        if (Long) OpenList().Watch(); else Change(1);
    }

    async Task OpenList()
    {
        if (TopLevel.GetTopLevel(this) is not Window window) return;
        int? pick = await ControllerDialogs.Choose(window, Items.ToArray(), index, Label);
        if (pick is int value) CommitSelection(value);
        Focus(NavigationMethod.Directional);
    }

    protected override void Click(Point at)
    {
        var v = ValueRect;
        if (!v.Contains(at)) return;  // on the name: just selected
        if (at.X < v.X + 34) Change(-1);
        else if (at.X > v.Right - 34) Change(1);
        else Activate();
    }

    protected override void RenderValue(DrawingContext g)
    {
        var v = ValueRect;
        bool dots = Items.Count is > 1 and <= 8;
        double mid = v.Height / 2 - (dots ? 3 : 0);
        var arrow = !IsEffectivelyEnabled ? WithAlpha(CreamDim, 70) : Selected ? Lime : WithAlpha(CreamDim, 150);
        if (Items.Count > 1)
        {
            Arrow(g, new Point(v.X + 12, mid), -1, arrow);
            Arrow(g, new Point(v.Right - 12, mid), 1, arrow);
        }
        var text = MenuText.Make(SelectedItem?.ToString() ?? "", 15, TextColor(true), FontWeight.Normal, Math.Max(1, v.Width - 64));
        g.DrawText(text, new Point(Math.Round(v.X + (v.Width - text.Width) / 2), Math.Round(mid - text.Height / 2)));
        if (!dots) return;
        const double gap = 9;
        double x = v.X + v.Width / 2 - (Items.Count - 1) * gap / 2, y = mid + text.Height / 2 + 5;
        for (int i = 0; i < Items.Count; i++)
            g.DrawEllipse(Brush(i == index && IsEffectivelyEnabled ? Lime : WithAlpha(CreamDim, 90)), null, new Point(x + i * gap, y), 2, 2);
    }
}

// On / off: A (or a click) flips it, left turns it off, right on. Mirrors the CheckBox API the settings code used.
sealed class ToggleRow : MenuRow
{
    bool? isChecked = false;
    public event EventHandler? IsCheckedChanged;

    public ToggleRow(string label, string? details = null) : base(label, details) { }

    public bool? IsChecked
    {
        get => isChecked;
        set
        {
            if (isChecked == value) return;
            isChecked = value;
            ValueChanged();
            IsCheckedChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public override bool Changes => true;
    public override string? ConfirmLabel => "Switch";
    public override void Change(int direction) { if (IsEffectivelyEnabled) IsChecked = direction > 0; }
    public override void Activate() { if (IsEffectivelyEnabled) IsChecked = isChecked != true; }

    // A click on an arrow sets it as left / right do; anywhere else on the row flips it.
    protected override void Click(Point at)
    {
        var v = ValueRect;
        if (v.Contains(at) && at.X < v.X + 34) Change(-1);
        else if (v.Contains(at) && at.X > v.Right - 34) Change(1);
        else Activate();
    }

    protected override void RenderValue(DrawingContext g) => DrawSwitch(g, isChecked == true);
}

// A switch that can be moved up or down its list (the texture packs, whose order is their priority). Left / right switch
// it, A picks it up, up / down carry it, A puts it down (Moved fires when its place changed), B puts it back. The mouse
// drags it by its name; a click on the switch flips it. Its list is the OrderRows in its panel, kept next to each other.
sealed class OrderRow : MenuRow
{
    bool on, carrying, dragging;
    int startIndex;
    Point? pressAt;
    string? extraLabel;
    Action? extraAction;
    public event EventHandler? IsCheckedChanged;
    public event EventHandler? Moved;

    public OrderRow(string key, string label, string? details = null) : base(label, details) => Key = key;

    public string Key { get; }

    public bool IsChecked
    {
        get => on;
        set
        {
            if (on == value) return;
            on = value;
            ValueChanged();
            IsCheckedChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    // X: a second action for this row (Remove), or none.
    public void SetExtra(string? label, Action? action)
    {
        extraLabel = label;
        extraAction = action;
        ValueChanged();
    }

    List<OrderRow> Siblings => (Parent as Panel)?.Children.OfType<OrderRow>().ToList() ?? new List<OrderRow> { this };
    public int Index => Siblings.IndexOf(this);
    bool Movable => Siblings.Count > 1;
    public bool Carrying => carrying;
    public override bool Busy => carrying;

    public override bool Changes => !carrying;
    public override string? ConfirmLabel => Movable ? "Move" : "Switch";
    public override string? ExtraLabel => carrying ? null : extraLabel;
    public override void Extra() { if (!carrying) extraAction?.Invoke(); }
    public override void Change(int direction) { if (IsEffectivelyEnabled && !carrying) IsChecked = direction > 0; }

    public override void Activate()
    {
        if (!IsEffectivelyEnabled) return;
        if (Movable) PickUp(); else IsChecked = !on;
    }

    public override void Settle() => PutDown(true);

    void PickUp()
    {
        carrying = true;
        startIndex = Index;
        ValueChanged();
    }

    void PutDown(bool keep)
    {
        if (!carrying) return;
        if (!keep)
            while (Index != startIndex && Step(Index < startIndex ? 1 : -1)) { }
        carrying = dragging = false;
        pressAt = null;
        ValueChanged();
        if (keep && Index != startIndex) Moved?.Invoke(this, EventArgs.Empty);
    }

    // Swaps places with the next row up (-1) or down (1). The neighbour is the one taken out and put back, so this row
    // stays in the tree and keeps its focus and the pointer.
    bool Step(int direction)
    {
        if (Parent is not Panel panel) return false;
        var rows = Siblings;
        int to = rows.IndexOf(this) + direction;
        if (to < 0 || to >= rows.Count) return false;
        var other = rows[to];
        int mine = panel.Children.IndexOf(this);
        panel.Children.Remove(other);
        panel.Children.Insert(mine, other);
        this.BringIntoView();
        ValueChanged();
        return true;
    }

    public override bool Handle(LauncherButtons action)
    {
        if (!carrying) return false;
        switch (action)
        {
            case LauncherButtons.Up: Step(-1); return true;
            case LauncherButtons.Down: Step(1); return true;
            case LauncherButtons.Confirm: PutDown(true); return true;
            case LauncherButtons.Back: PutDown(false); return true;
            case LauncherButtons.ScrollUp or LauncherButtons.ScrollDown: return false;
            default: return true;  // nothing else while carrying: no switching, no page change
        }
    }

    public override IReadOnlyList<PromptBar.Item>? OwnPrompts => carrying
        ? new PromptBar.Item[]
        {
            new(new[] { Prompts.Button.Move }, "Move", null),
            new(new[] { Prompts.Button.Confirm }, "Put down", () => PutDown(true)),
            new(new[] { Prompts.Button.Back }, "Cancel", () => PutDown(false)),
        }
        : null;

    // ----- The mouse: a click on the switch flips it; a press on the name and a move up or down drags the row -----
    protected override void Click(Point at)
    {
        var v = ValueRect;
        if (carrying || !v.Contains(at)) return;
        if (at.X < v.X + 34) Change(-1);
        else if (at.X > v.Right - 34) Change(1);
        else IsChecked = !on;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        var at = e.GetPosition(this);
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed && !ValueRect.Contains(at) && Movable && IsEffectivelyEnabled)
            pressAt = at;
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        var at = e.GetPosition(this);
        if (pressAt is not { } start)
        {
            Cursor = new Cursor(ValueRect.Contains(at) || !Movable ? StandardCursorType.Hand : StandardCursorType.SizeNorthSouth);
            return;
        }
        if (!dragging)
        {
            if (Math.Abs(at.Y - start.Y) < 6) return;
            if (!carrying) PickUp();
            dragging = true;
        }
        if (at.Y < 0) Step(-1);
        else if (at.Y > Bounds.Height) Step(1);
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        pressAt = null;
        if (dragging) PutDown(true);
    }

    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e)
    {
        base.OnPointerCaptureLost(e);
        pressAt = null;
        if (dragging) PutDown(true);
    }

    public override void Render(DrawingContext g)
    {
        base.Render(g);
        if (!carrying) return;
        var r = new Rect(Bounds.Size);
        g.DrawGeometry(null, new Pen(Brush(Lime), 2), Rounded(r.Deflate(1), 9));
        // Up and down marks where the left arrow was (left / right do nothing while it is carried): this row moves.
        double x = ValueRect.X + 12, y = r.Height / 2;
        g.DrawGeometry(Brush(Lime), null, Polygon(new[] { new Point(x, y - 12), new Point(x - 6, y - 4), new Point(x + 6, y - 4) }));
        g.DrawGeometry(Brush(Lime), null, Polygon(new[] { new Point(x, y + 12), new Point(x - 6, y + 4), new Point(x + 6, y + 4) }));
    }

    protected override void RenderValue(DrawingContext g) => DrawSwitch(g, on, arrows: !carrying);
}

// Opens or does something: A or a click. Its value (a status, a key) shows on the right, before a small arrow.
sealed class ActionRow : MenuRow
{
    readonly Action action;
    string confirm;
    string? value;
    Color valueColor = CreamDim;

    public ActionRow(string label, Action action, string confirm = "Select", string? details = null) : base(label, details)
    {
        this.action = action;
        this.confirm = confirm;
    }

    public string? Value { get => value; set { if (this.value == value) return; this.value = value; ValueChanged(); } }
    public Color ValueColor { get => valueColor; set { valueColor = value; InvalidateVisual(); } }
    public string Confirm { get => confirm; set { if (confirm == value) return; confirm = value; ValueChanged(); } }
    public override string? ConfirmLabel => IsEffectivelyEnabled ? confirm : null;

    public void PerformClick() => Activate();
    public override void Activate() { if (IsEffectivelyEnabled) action(); }

    protected override bool HasValue => true;

    protected override void RenderValue(DrawingContext g)
    {
        var v = ValueRect;
        var arrow = !IsEffectivelyEnabled ? WithAlpha(CreamDim, 70) : Selected ? Lime : WithAlpha(CreamDim, 150);
        Arrow(g, new Point(v.Right - 10, v.Height / 2), 1, arrow, 5.5);
        if (string.IsNullOrEmpty(value)) return;
        var text = MenuText.Make(value, 14.5, IsEffectivelyEnabled ? valueColor : WithAlpha(valueColor, 110), FontWeight.Normal, Math.Max(1, v.Width - 30));
        g.DrawText(text, new Point(Math.Round(v.Right - 26 - text.Width), Math.Round((v.Height - text.Height) / 2)));
    }
}

// A name and a value that can't be changed (a key on the keyboard): the selection skips it.
sealed class InfoRow : MenuRow
{
    readonly string value;

    public InfoRow(string label, string value) : base(label, null)
    {
        this.value = value;
        Focusable = false;
        Height = 34;
        Cursor = Cursor.Default;
    }

    public override string? ConfirmLabel => null;

    protected override void RenderValue(DrawingContext g)
    {
        var v = ValueRect;
        var text = MenuText.Make(value, 14.5, CreamDim, FontWeight.Normal, v.Width);
        g.DrawText(text, new Point(Math.Round(v.Right - text.Width), Math.Round((v.Height - text.Height) / 2)));
    }
}

// A group's heading in the signs' lettering, with a line to the right and an optional dim line of text under it.
sealed class MenuHeading : Control
{
    readonly string title;
    readonly string? hint;
    FormattedText? hintText;

    public MenuHeading(string title, string? hint = null)
    {
        this.title = title;
        this.hint = hint;
        Margin = new Thickness(0, 10, 0, 2);
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        double width = double.IsInfinity(availableSize.Width) ? 600 : availableSize.Width;
        hintText = null;
        if (hint != null)
        {
            hintText = MenuText.Make(hint, 13.5, CreamDim);
            hintText.MaxTextWidth = Math.Max(1, width - 22);
        }
        return new Size(width, 34 + (hintText != null ? hintText.Height + 4 : 0));
    }

    public override void Render(DrawingContext g)
    {
        var text = MenuText.DrawDisplay(g, title, 21, Lime, new Point(8, 4), Bounds.Width - 10);
        double lineX = 8 + text.Width + 14;
        if (lineX < Bounds.Width)
            g.DrawLine(new Pen(Brush(Argb(80, 150, 110, 210)), 1), new Point(lineX, 19.5), new Point(Bounds.Width - 4, 19.5));
        if (hintText != null) g.DrawText(hintText, new Point(22, 34));
    }
}

// ----- The menu screen -----

// A full-window menu: the title and its tabs, the selected page's rows, the details panel and the prompt bar.
sealed class MenuScreen : Panel
{
    public string Title { get; }
    readonly Action onClose;
    readonly MenuHeader header;
    readonly ScrollViewer scroller;
    readonly Decorator host = new();
    readonly Border details;
    readonly TextBlock detailsTitle, detailsText;
    readonly PromptBar prompts;
    readonly Backdrop backdrop;
    readonly List<(string Name, StackPanel Page)> pages = new();
    MenuRow? selected;
    double dividerTop, dividerBottom;

    public MenuScreen(string title, Action onClose)
    {
        Title = title;
        this.onClose = onClose;
        IsVisible = false;
        ClipToBounds = true;
        header = new MenuHeader(title, i => SelectTab(i), Step);
        scroller = new ScrollViewer
        {
            Content = host, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, AllowAutoHide = false,
        };
        detailsTitle = new TextBlock { FontFamily = Skin.Display, FontSize = 21, Foreground = Brush(Lime), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 10) };
        detailsText = new TextBlock { FontFamily = BodyFamily, FontSize = 14.5, Foreground = Brush(Cream), TextWrapping = TextWrapping.Wrap, LineHeight = 21 };
        details = new Border
        {
            Background = Brush(Argb(110, 16, 9, 30)), BorderBrush = Brush(Argb(150, 110, 66, 172)), BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(12), Padding = new Thickness(18, 14, 18, 14),
            Child = new ScrollViewer
            {
                Content = new StackPanel { Children = { detailsTitle, detailsText } },
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Hidden,
            },
        };
        prompts = new PromptBar { RightText = MansionScene.BuildId };
        backdrop = new Backdrop(this);
        Children.Add(backdrop);
        Children.Add(header);
        Children.Add(scroller);
        Children.Add(details);
        Children.Add(prompts);
        // The menu owns these keys (as F10 does); a tunnel handler, so the scroll viewer doesn't scroll with the arrows.
        AddHandler(KeyDownEvent, OnMenuKey, RoutingStrategies.Tunnel);
    }

    // The pad / keyboard prompts follow the last device used. Subscribed only while shown in a window, so a menu that is
    // thrown away (a file browser) isn't kept alive by the static event.
    void PromptsChanged() { header.InvalidateVisual(); prompts.InvalidateVisual(); }
    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e) { base.OnAttachedToVisualTree(e); Prompts.Changed += PromptsChanged; }
    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e) { base.OnDetachedFromVisualTree(e); Prompts.Changed -= PromptsChanged; }

    // X when the selected row has no second action of its own (a file browser's Up a folder), and what the prompt bar
    // calls B.
    public (string Label, Action Run)? ScreenExtra { get => screenExtra; set { screenExtra = value; Refresh(); } }
    (string Label, Action Run)? screenExtra;
    public string BackLabel { get; init; } = "Back";

    internal static MenuScreen? Of(Control row) => row.FindAncestorOfType<MenuScreen>();

    // A row still in a mode of its own (a pack picked up) is put down when the menu closes.
    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == IsVisibleProperty && !IsVisible) selected?.Settle();
    }

    public ScrollViewer Scroller => scroller;
    public int SelectedTab { get; private set; }
    public int TabCount => pages.Count;
    public MenuRow? Selected => selected;
    public StackPanel Page => pages[SelectedTab].Page;
    public IEnumerable<StackPanel> Pages => pages.Select(p => p.Page);  // all of them, open or not

    // A menu without tabs: its one page.
    public StackPanel Body
    {
        get
        {
            if (pages.Count == 0) AddTab("", NewPage());
            return pages[0].Page;
        }
    }

    public static StackPanel NewPage() => new() { Orientation = Orientation.Vertical, Margin = new Thickness(0, 0, 14, 12) };

    public void AddTab(string name, StackPanel page)
    {
        pages.Add((name, page));
        header.Tabs = pages.Where(p => p.Name.Length > 0).Select(p => p.Name).ToArray();
        if (pages.Count == 1) SelectTab(0);
    }

    public void SelectTab(int index)
    {
        if (index < 0 || index >= pages.Count) return;
        bool changed = SelectedTab != index || host.Child != pages[index].Page;
        SelectedTab = index;
        header.Selected = index;
        if (!changed) return;
        host.Child = pages[index].Page;
        scroller.Offset = default;  // a new page starts at its top
        Select(null, focus: false);
        if (IsVisible) { UpdateLayout(); SelectFirst(); }
    }

    // LB / RB: the previous or next tab, without wrapping (at either end nothing happens).
    bool Step(int direction)
    {
        int to = SelectedTab + direction;
        if (pages.Count < 2 || to < 0 || to >= pages.Count) return false;
        SelectTab(to);
        return true;
    }

    // The rows the selection can land on, in page order.
    public IEnumerable<MenuRow> Rows => Page.GetVisualDescendants().OfType<MenuRow>()
        .Where(r => r.Focusable && r.IsEffectivelyVisible && r.IsEffectivelyEnabled);

    bool InView(Control c)
    {
        var at = c.TranslatePoint(default, scroller);
        return at != null && at.Value.Y >= -1 && at.Value.Y + c.Bounds.Height <= scroller.Bounds.Height + 1;
    }

    public void SelectFirst()
    {
        var rows = Rows.ToList();
        Select(rows.FirstOrDefault(InView) ?? rows.FirstOrDefault());
    }

    public void Select(MenuRow? row, bool focus = true, bool scroll = true)
    {
        if (selected is { Busy: true } busy && busy != row) busy.Settle();
        if (selected != row)
        {
            if (selected != null) selected.Selected = false;
            selected = row;
            if (row != null) row.Selected = true;
        }
        if (row != null && focus)
        {
            row.Focus(NavigationMethod.Directional);
            if (scroll) row.BringIntoView();
        }
        Refresh();
    }

    internal void RowFocused(MenuRow row)
    {
        if (row != selected) Select(row, focus: false);
    }

    internal void RowChanged(MenuRow row)
    {
        if (row == selected) Refresh();
    }

    // The details and the prompts for the selected row.
    internal void Refresh()
    {
        detailsTitle.Text = selected?.Label ?? (pages.Count > 0 && pages[SelectedTab].Name.Length > 0 ? pages[SelectedTab].Name : Title);
        detailsText.Text = selected?.Details ?? "";
        detailsText.IsVisible = !string.IsNullOrEmpty(detailsText.Text);
        if (selected?.OwnPrompts is { } own) { prompts.Items = own; return; }
        var items = new List<PromptBar.Item>();
        if (selected is { Changes: true, IsEffectivelyEnabled: true }) items.Add(new(new[] { Prompts.Button.Change }, "Change", null));
        if (selected?.ConfirmLabel is { } confirm) items.Add(new(new[] { Prompts.Button.Confirm }, confirm, () => selected?.Activate()));
        if (selected is { ExtraLabel: { } extra, IsEffectivelyEnabled: true }) items.Add(new(new[] { Prompts.Button.Extra }, extra, () => selected?.Extra()));
        else if (screenExtra is { } screen) items.Add(new(new[] { Prompts.Button.Extra }, screen.Label, screen.Run));
        items.Add(new(new[] { Prompts.Button.Back }, BackLabel, onClose));
        if (pages.Count > 1) items.Add(new(new[] { Prompts.Button.Previous, Prompts.Button.Next }, "Page", null));
        prompts.Items = items;
    }

    // A pad or key action. Back closes the menu.
    public void Handle(LauncherButtons action)
    {
        if (selected != null && selected.Handle(action)) return;
        switch (action)
        {
            case LauncherButtons.Extra:
                if (selected is { ExtraLabel: not null, IsEffectivelyEnabled: true }) selected.Extra();
                else screenExtra?.Run();
                return;
            case LauncherButtons.Back: onClose(); return;
            case LauncherButtons.Previous: Step(-1); return;
            case LauncherButtons.Next: Step(1); return;
            case LauncherButtons.Left: selected?.Change(-1); return;
            case LauncherButtons.Right: selected?.Change(1); return;
            case LauncherButtons.Confirm: selected?.Activate(); return;
            case LauncherButtons.Up or LauncherButtons.Down:
            {
                var rows = Rows.ToList();
                if (rows.Count == 0) return;
                int at = selected == null ? -1 : rows.IndexOf(selected);
                if (at < 0) { SelectFirst(); return; }
                Select(rows[Math.Clamp(at + (action == LauncherButtons.Down ? 1 : -1), 0, rows.Count - 1)]);
                return;
            }
            case LauncherButtons.ScrollUp or LauncherButtons.ScrollDown:
            {
                double step = action == LauncherButtons.ScrollDown ? 70 : -70;
                scroller.Offset = new Vector(0, Math.Clamp(scroller.Offset.Y + step, 0, Math.Max(0, scroller.Extent.Height - scroller.Viewport.Height)));
                UpdateLayout();
                if (selected == null || !InView(selected)) SelectFirst();
                return;
            }
        }
    }

    void OnMenuKey(object? sender, KeyEventArgs e)
    {
        if (!IsVisible) return;
        LauncherButtons action = e.Key switch
        {
            Key.Up => LauncherButtons.Up, Key.Down => LauncherButtons.Down, Key.Left => LauncherButtons.Left, Key.Right => LauncherButtons.Right,
            Key.Enter or Key.Space => LauncherButtons.Confirm, Key.Escape or Key.Back => LauncherButtons.Back,
            Key.Q or Key.PageUp => LauncherButtons.Previous, Key.E or Key.PageDown => LauncherButtons.Next,
            Key.Delete => LauncherButtons.Extra,
            _ => LauncherButtons.None,
        };
        if (action == LauncherButtons.None) return;
        e.Handled = true;
        Prompts.UsedKeyboard();
        Handle(action);
    }

    // ----- Layout: the header across the top, list and details in the middle, prompts at the bottom -----
    const double PromptHeight = 40, NarrowBelow = 760;

    double Side(double width) => width >= 1100 ? 36 : width >= 700 ? 24 : 14;

    (Rect header, Rect list, Rect details, Rect prompts) Layout(Size size)
    {
        double side = Side(size.Width), inner = Math.Max(0, size.Width - 2 * side);
        header.Measure(new Size(inner, double.PositiveInfinity));
        var head = new Rect(side, 12, inner, header.DesiredSize.Height);
        dividerTop = head.Bottom + 8;
        var bar = new Rect(side, size.Height - PromptHeight - 8, inner, PromptHeight);
        dividerBottom = bar.Y - 6;
        double top = dividerTop + 12, bottom = dividerBottom - 12, height = Math.Max(0, bottom - top);
        if (inner >= NarrowBelow)
        {
            double detailsWidth = Math.Clamp(inner * 0.32, 250, 420);
            double listWidth = inner - detailsWidth - 24;
            return (head, new Rect(side, top, listWidth, height), new Rect(side + listWidth + 24, top, detailsWidth, height), bar);
        }
        double detailsHeight = Math.Min(130, height * 0.35);
        return (head, new Rect(side, top, inner, Math.Max(0, height - detailsHeight - 12)),
                new Rect(side, top + height - detailsHeight, inner, detailsHeight), bar);
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        var size = new Size(double.IsInfinity(availableSize.Width) ? 1180 : availableSize.Width, double.IsInfinity(availableSize.Height) ? 830 : availableSize.Height);
        var (_, list, side, bar) = Layout(size);
        backdrop.Measure(size);
        scroller.Measure(list.Size);
        details.Measure(side.Size);
        prompts.Measure(bar.Size);
        return size;
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var (head, list, side, bar) = Layout(finalSize);
        backdrop.Arrange(new Rect(finalSize));
        backdrop.InvalidateVisual();  // the dividers follow the header's height
        header.Arrange(head);
        scroller.Arrange(list);
        details.Arrange(side);
        prompts.Arrange(bar);
        return finalSize;
    }

    // The dim over the mansion and the lines under the header and over the prompts (a Panel can't draw itself). It also
    // takes the pointer over the whole window, so nothing behind the menu reacts.
    sealed class Backdrop : Control
    {
        readonly MenuScreen screen;
        public Backdrop(MenuScreen screen) => this.screen = screen;

        public override void Render(DrawingContext g)
        {
            var r = new Rect(Bounds.Size);
            g.FillRectangle(Vertical(Argb(251, 30, 18, 52), Argb(253, 18, 10, 32)), r);
            var line = new Pen(Brush(Argb(110, 110, 66, 172)), 1);
            double side = screen.Side(r.Width);
            g.DrawLine(line, new Point(side, Math.Round(screen.dividerTop) + 0.5), new Point(r.Width - side, Math.Round(screen.dividerTop) + 0.5));
            g.DrawLine(line, new Point(side, Math.Round(screen.dividerBottom) + 0.5), new Point(r.Width - side, Math.Round(screen.dividerBottom) + 0.5));
        }
    }
}

// The title and the tabs: LB, the tab names (the open one in lime, underlined), RB. In a window too narrow for both on
// one line the tabs go under the title, smaller if they still don't fit.
sealed class MenuHeader : Control
{
    readonly string title;
    readonly Action<int> select;
    readonly Func<int, bool> step;
    string[] tabs = Array.Empty<string>();
    int selected;
    readonly List<(Rect Box, int Tab)> hits = new();
    const double TitleSize = 38, TabSize = 25, IconHeight = 28, TabGap = 26, LineHeight = 50;

    public MenuHeader(string title, Action<int> select, Func<int, bool> step)
    {
        this.title = title;
        this.select = select;
        this.step = step;
    }

    public string[] Tabs { get => tabs; set { tabs = value; InvalidateMeasure(); InvalidateVisual(); } }
    public int Selected { get => selected; set { selected = value; InvalidateVisual(); } }

    double TitleWidth => MenuText.Display(title, TitleSize, Lime).Width;

    // The tabs' width at a font size, the LB / RB icons included.
    double TabsWidth(double size) => tabs.Length == 0 ? 0
        : tabs.Sum(t => MenuText.Display(t, size, Cream).Width) + TabGap * (tabs.Length - 1) + 2 * (Prompts.Width(Prompts.Button.Previous, IconHeight) + 18);

    bool TwoLines(double width) => tabs.Length > 0 && TitleWidth + 48 + TabsWidth(TabSize) > width;

    protected override Size MeasureOverride(Size availableSize)
    {
        double width = double.IsInfinity(availableSize.Width) ? 1100 : availableSize.Width;
        return new Size(width, TwoLines(width) ? 2 * LineHeight : LineHeight);
    }

    public override void Render(DrawingContext g)
    {
        hits.Clear();
        double width = Bounds.Width;
        double titleHeight = MenuText.Display(title, TitleSize, Lime, width).Height;
        var titleText = MenuText.DrawDisplay(g, title, TitleSize, Lime, new Point(0, (LineHeight - titleHeight) / 2), width);
        if (tabs.Length == 0) return;
        bool two = TwoLines(width);
        double size = TabSize;
        if (two && TabsWidth(size) > width) size = Math.Max(14, size * width / TabsWidth(size));
        double x = two ? 0 : Math.Max(titleText.Width + 48, Math.Min(width - TabsWidth(size), titleText.Width + 80));
        double top = two ? LineHeight : 0, mid = top + LineHeight / 2;
        double iconWidth = Prompts.Draw(g, Prompts.Button.Previous, x, mid - IconHeight / 2, IconHeight);
        hits.Add((new Rect(x, mid - IconHeight / 2, iconWidth, IconHeight), -1));
        x += iconWidth + 18;
        for (int i = 0; i < tabs.Length; i++)
        {
            bool open = i == selected;
            var color = open ? Lime : Argb(255, 176, 160, 132);
            var text = MenuText.Display(tabs[i], size, color);
            var at = new Point(x, Math.Round(mid - text.Height / 2));
            MenuText.DrawDisplay(g, tabs[i], size, color, at);
            if (open) g.DrawGeometry(Brush(Lime), null, Rounded(new Rect(x, at.Y + text.Height + 2, text.Width, 3), 1.5));
            hits.Add((new Rect(x - TabGap / 2, top, text.Width + TabGap, LineHeight), i));
            x += text.Width + TabGap;
        }
        x += 18 - TabGap;
        iconWidth = Prompts.Draw(g, Prompts.Button.Next, x, mid - IconHeight / 2, IconHeight);
        hits.Add((new Rect(x, mid - IconHeight / 2, iconWidth, IconHeight), -2));
    }

    int? HitAt(Point p) => hits.FirstOrDefault(h => h.Box.Contains(p)) is { Box.Width: > 0 } hit ? hit.Tab : null;

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        Cursor = HitAt(e.GetPosition(this)) != null ? new Cursor(StandardCursorType.Hand) : Cursor.Default;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed || HitAt(e.GetPosition(this)) is not int tab) return;
        e.Handled = true;
        if (tab == -1) step(-1);
        else if (tab == -2) step(1);
        else select(tab);
    }
}

// The button prompts along the bottom: an icon (or two) and what it does, each clickable when it has an action. Text on
// the right (the build ID) when there is room. Centred = drawn in the middle on a dark pill (the main screen).
sealed class PromptBar : Control
{
    public sealed record Item(Prompts.Button[] Buttons, string Label, Action? Click);

    IReadOnlyList<Item> items = Array.Empty<Item>();
    readonly List<(Rect Box, Item Item)> hits = new();
    public string RightText { get; init; } = "";
    public bool Centred { get; init; }
    const double IconHeight = 28;

    void PromptsChanged() { InvalidateMeasure(); InvalidateVisual(); }
    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e) { base.OnAttachedToVisualTree(e); Prompts.Changed += PromptsChanged; }
    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e) { base.OnDetachedFromVisualTree(e); Prompts.Changed -= PromptsChanged; }

    public IReadOnlyList<Item> Items { get => items; set { items = value; InvalidateMeasure(); InvalidateVisual(); } }

    // Centred, it is only as wide as its pill, so the pointer reaches what is beside it.
    protected override Size MeasureOverride(Size availableSize) => Centred
        ? new(Math.Min(ContentWidth(15, 26, true) + 24, double.IsInfinity(availableSize.Width) ? double.MaxValue : availableSize.Width), 32)
        : new(double.IsInfinity(availableSize.Width) ? 600 : availableSize.Width, 40);

    // Width of the items at a label size and gap; labels = false draws the icons alone.
    double ContentWidth(double size, double gap, bool labels) => items.Sum(i =>
        i.Buttons.Sum(b => Prompts.Width(b, IconHeight)) + 3 * (i.Buttons.Length - 1) +
        (labels ? 8 + MenuText.Make(i.Label, size, Cream).Width : 0)) + gap * Math.Max(0, items.Count - 1);

    public override void Render(DrawingContext g)
    {
        hits.Clear();
        double width = Bounds.Width, height = Bounds.Height;
        double size = 15, gap = 26;
        bool labels = true;
        var right = RightText.Length > 0 && !Centred ? MenuText.Make(RightText, 12.5, WithAlpha(CreamDim, 170)) : null;
        double room = width - (right != null ? right.Width + 30 : 0);
        if (ContentWidth(size, gap, true) > room) { right = null; room = width; }
        if (ContentWidth(size, gap, true) > room) { size = 13; gap = 14; }
        if (ContentWidth(size, gap, true) > room) labels = false;
        if (Centred) room = width - 24;
        if (ContentWidth(size, gap, true) > room) { size = 13; gap = 14; }
        if (ContentWidth(size, gap, true) > room) labels = false;
        double total = ContentWidth(size, gap, labels);
        double x = Centred ? Math.Round((width - total) / 2) : 0, iconY = (height - IconHeight) / 2;
        if (Centred && items.Count > 0)
            g.DrawGeometry(Brush(Argb(185, 10, 6, 18)), null, Rounded(new Rect(0, 0, width, height), height / 2));
        foreach (var item in items)
        {
            double start = x;
            foreach (var b in item.Buttons) x += Prompts.Draw(g, b, x, iconY, IconHeight) + 3;
            x -= 3;
            if (labels)
            {
                var text = MenuText.Make(item.Label, size, Cream, FontWeight.SemiBold);
                g.DrawText(text, new Point(x + 8, Math.Round((height - text.Height) / 2)));
                x += 8 + text.Width;
            }
            if (item.Click != null) hits.Add((new Rect(start, 0, x - start, height), item));
            x += gap;
        }
        if (right != null) g.DrawText(right, new Point(width - right.Width, Math.Round((height - right.Height) / 2)));
    }

    Item? HitAt(Point p) => hits.FirstOrDefault(h => h.Box.Contains(p)).Item;

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        Cursor = HitAt(e.GetPosition(this)) != null ? new Cursor(StandardCursorType.Hand) : Cursor.Default;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed || HitAt(e.GetPosition(this)) is not { Click: { } click }) return;
        e.Handled = true;
        click();
    }
}
