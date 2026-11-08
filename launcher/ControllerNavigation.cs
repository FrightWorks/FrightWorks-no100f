using System.Runtime.CompilerServices;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Automation.Peers;
using Avalonia.Automation.Provider;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.LogicalTree;
using Avalonia.Threading;
using Avalonia.VisualTree;

namespace FrightWorks;

// Input is dispatched only inside this launcher's active window or one of its registered dialogs.
sealed class ControllerNavigation : IDisposable
{
    static readonly ConditionalWeakTable<Window, ControllerNavigation> owners = new();
    readonly MainWindow owner;
    readonly Gamepads gamepads;
    readonly List<Window> dialogs = new();
    readonly Dictionary<uint, ControllerInput> inputs = new();
    readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(16) };
    Window? context;
    readonly HashSet<Window> dispatching = new();
    public bool Used { get; private set; }
    // True only while a pad Confirm is running a button's handler: actions started then (a file picker) were invoked by the pad.
    internal bool Activating { get; private set; }
    internal Window? Modal => dialogs.LastOrDefault();

    public ControllerNavigation(MainWindow owner, Gamepads gamepads)
    {
        this.owner = owner;
        this.gamepads = gamepads;
        owners.Add(owner, this);
        timer.Tick += (_, _) => Feed(ActiveWindow(), gamepads.Samples, Environment.TickCount64, owner.GameOwnsControllers);
    }

    public static ControllerNavigation? For(Window? window) =>
        window != null && owners.TryGetValue(window, out var navigation) ? navigation : null;

    public void Start() => timer.Start();
    public void Reset()
    {
        context = null;
        foreach (var input in inputs.Values) input.Reset();
    }

    public void Register(Window dialog)
    {
        owners.Add(dialog, this);
        dialogs.Add(dialog);
        Reset();
    }

    public void Unregister(Window dialog)
    {
        owners.Remove(dialog);
        dialogs.Remove(dialog);
        Reset();
    }

    Window? ActiveWindow()
    {
        if (dialogs.Count > 0) return dialogs[^1] is { IsActive: true, IsVisible: true } d ? d : null;
        return owner is { IsActive: true, IsVisible: true, IsEnabled: true, WindowState: not WindowState.Minimized } ? owner : null;
    }

    // Tests inject foreground/sample state here, never start SDL or inject operating-system keys.
    internal void Feed(Window? target, IReadOnlyList<ControllerSample> samples, long time, bool gameOwnsControllers)
    {
        if (gameOwnsControllers || (target != null && dispatching.Contains(target)) ||
            (target != owner && (target == null || !dialogs.Contains(target))) ||
            (dialogs.Count > 0 && target != dialogs[^1])) target = null;
        if (context != target) { Reset(); context = target; }
        var present = samples.Select(s => s.Id).ToHashSet();
        foreach (uint id in inputs.Keys.Where(id => !present.Contains(id)).ToArray()) inputs.Remove(id);
        LauncherButtons action = LauncherButtons.None;
        bool coarseHorizontal = false;
        foreach (var sample in samples)
        {
            if (!inputs.TryGetValue(sample.Id, out var input)) inputs.Add(sample.Id, input = new ControllerInput());
            if (target == null) { input.Reset(); continue; }
            var next = input.Read(sample.Buttons, time);
            if (action == LauncherButtons.None && next != LauncherButtons.None)
            {
                action = next;
                coarseHorizontal = sample.CoarseHorizontal;
            }
        }
        if (target == null || action == LauncherButtons.None) return;
        Used = true;
        Dispatch(target, action, coarseHorizontal).Watch();
    }

    async Task Dispatch(Window window, LauncherButtons action, bool coarseHorizontal)
    {
        if (!dispatching.Add(window)) return;
        try
        {
            if (action == LauncherButtons.Back)
            {
                var dropdown = Targets(window).OfType<ComboBox>().FirstOrDefault(c => c.IsDropDownOpen);
                if (dropdown != null) { dropdown.IsDropDownOpen = false; Focus(dropdown); }
                else if (window == owner) owner.CloseBoards();
                else window.Close();
                Reset();
                return;
            }
            if (window == owner && !owner.boards.Values.Any(b => b.IsVisible))
            {
                owner.scene.Navigate(action);
                return;
            }
            var targets = Targets(window).ToList();
            var focused = window.FocusManager?.GetFocusedElement() as Control;
            if (focused != null && !targets.Contains(focused))
                focused = focused.GetVisualAncestors().OfType<Control>().FirstOrDefault(targets.Contains);
            focused ??= targets.OfType<ComboBox>().FirstOrDefault(c => c.IsDropDownOpen);
            int index = focused == null ? -1 : targets.IndexOf(focused);
            if (action is LauncherButtons.ScrollUp or LauncherButtons.ScrollDown)
            {
                var scroll = focused?.GetVisualAncestors().OfType<ScrollViewer>().FirstOrDefault() ??
                    window.GetVisualDescendants().OfType<ScrollViewer>().FirstOrDefault(s => s.IsEffectivelyVisible);
                if (scroll != null)
                {
                    double step = action == LauncherButtons.ScrollDown ? 70 : -70;
                    scroll.Offset = new Vector(scroll.Offset.X, Math.Clamp(scroll.Offset.Y + step, 0, Math.Max(0, scroll.Extent.Height - scroll.Viewport.Height)));
                    window.UpdateLayout();
                    FocusFirst(scroll);
                }
                return;
            }
            int direction = action is LauncherButtons.Up or LauncherButtons.Left or LauncherButtons.Previous ? -1 : 1;
            if (action is LauncherButtons.Previous or LauncherButtons.Next)
            {
                // A page with tabs: the bumpers switch tab. Anywhere else (dialogs, browsers, Mods, Extras) they keep moving
                // to the previous/next control, which is also how a dialog's toolbar is reached.
                var bar = window.GetVisualDescendants().OfType<TabBar>().FirstOrDefault(t => t.IsEffectivelyVisible);
                if (bar != null) bar.Step(direction); else Move();
                return;
            }
            if (focused is ListBox list && action is LauncherButtons.Up or LauncherButtons.Down)
            {
                if (list.ItemCount > 0) list.SelectedIndex = Math.Clamp(list.SelectedIndex + direction, 0, list.ItemCount - 1);
                list.ScrollIntoView(list.SelectedIndex);
                return;
            }
            if (action is LauncherButtons.Left or LauncherButtons.Right)
            {
                if (focused is Slider slider)
                {
                    double step = coarseHorizontal && slider is ControllerSlider coarse ? coarse.ControllerStep : slider.SmallChange;
                    slider.Value = Math.Clamp(slider.Value + direction * step, slider.Minimum, slider.Maximum);
                    return;
                }
                if (focused is NumericUpDown number)
                {
                    decimal value = Math.Clamp(number.Value ?? 0, number.Minimum, number.Maximum), step = number.Increment;
                    if (direction > 0)
                        number.Value = value >= number.Maximum || (value > 0 && step > decimal.MaxValue - value)
                            ? number.Maximum : Math.Min(number.Maximum, value + step);
                    else
                        number.Value = value <= number.Minimum || (value < 0 && step > value - decimal.MinValue)
                            ? number.Minimum : Math.Max(number.Minimum, value - step);
                    return;
                }
            }
            if (action != LauncherButtons.Confirm) { Move(); return; }
            switch (focused)
            {
                case PlankButton plank:
                    Activating = true;
                    try { plank.PerformClick(); }
                    finally { Activating = false; }
                    break;
                case RadioButton radio: radio.IsChecked = true; break;
                case ToggleButton toggle: toggle.IsChecked = toggle.IsChecked != true; break;
                case Button button:
                    Activating = true;
                    try
                    {
                        if (ControlAutomationPeer.CreatePeerForElement(button) is IInvokeProvider invoke) invoke.Invoke();
                        else throw new NotSupportedException("The button does not expose an accessible invoke action.");
                    }
                    finally { Activating = false; }
                    break;
                case ComboBox combo:
                    combo.IsDropDownOpen = false;
                    int? selected = await ControllerDialogs.Choose(window, combo.Items.Cast<object?>().ToArray(), combo.SelectedIndex, LabelFor(combo));
                    if (selected is int value)
                    {
                        if (combo is ThemedCombo themed) themed.CommitSelection(value);
                        else combo.SelectedIndex = value;
                    }
                    break;
                case TextBox text:
                    if (text is ControllerTextBox) break;
                    var edited = await ControllerDialogs.Text(window, "Enter text", text.Text ?? "");
                    if (edited != null) text.Text = edited;
                    break;
                case ControllerList choices: choices.Activate?.Invoke(); break;
            }

            void Move()
            {
                if (targets.Count == 0) return;
                int next = index < 0 ? 0 : (index + direction + targets.Count) % targets.Count;
                if (index >= 0 && action is LauncherButtons.Up or LauncherButtons.Down or LauncherButtons.Left or LauncherButtons.Right)
                {
                    var center = targets[index].TranslatePoint(new Point(targets[index].Bounds.Width / 2, targets[index].Bounds.Height / 2), window);
                    bool vertical = action is LauncherButtons.Up or LauncherButtons.Down;
                    if (center is Point origin)
                    {
                        double best = double.MaxValue;
                        bool bestAligned = false;
                        for (int i = 0; i < targets.Count; i++)
                        {
                            var at = targets[i].TranslatePoint(new Point(targets[i].Bounds.Width / 2, targets[i].Bounds.Height / 2), window);
                            if (at is not Point point) continue;
                            double along = (vertical ? point.Y - origin.Y : point.X - origin.X) * direction;
                            // Narrow buttons share a form column with stretched fields despite different centers.
                            double halfSpan = vertical
                                ? (targets[index].Bounds.Width + targets[i].Bounds.Width) / 2
                                : (targets[index].Bounds.Height + targets[i].Bounds.Height) / 2;
                            double across = Math.Max(0, Math.Abs(vertical ? point.X - origin.X : point.Y - origin.Y) - halfSpan);
                            double score = along + 4 * across;
                            bool aligned = across == 0;
                            if (along > 2 && ((aligned && !bestAligned) || (aligned == bestAligned && score < best)))
                            {
                                best = score;
                                bestAligned = aligned;
                                next = i;
                            }
                        }
                    }
                }
                Focus(targets[next]);
            }
        }
        finally { dispatching.Remove(window); }
    }

    // The setting's name for the popup: the label in the same form row, else its accessible name.
    internal static string LabelFor(ComboBox combo)
    {
        if (combo.Parent is Grid grid)
        {
            int row = Grid.GetRow(combo), column = Grid.GetColumn(combo);
            var label = grid.Children.OfType<TextBlock>().FirstOrDefault(t => Grid.GetRow(t) == row && Grid.GetColumn(t) < column);
            if (label?.Text is { Length: > 0 } text) return text.TrimEnd(':', ' ');
        }
        return AutomationProperties.GetName(combo) is { Length: > 0 } name ? name : "Choose a setting";
    }

    internal static IEnumerable<Control> Targets(Control root)
    {
        if (!root.IsEffectivelyVisible || !root.IsEffectivelyEnabled) yield break;
        if (root is PlankButton or Button or ComboBox or Slider or NumericUpDown or TextBox or ListBox)
        {
            if (root.Focusable) yield return root;
            yield break; // The control owns its template parts, not separate navigation stops.
        }
        foreach (var child in root.GetLogicalChildren().OfType<Control>())
            foreach (var target in Targets(child)) yield return target;
    }

    static bool InView(Control control)
    {
        foreach (var scroll in control.GetVisualAncestors().OfType<ScrollViewer>())
        {
            var at = control.TranslatePoint(default, scroll);
            if (at == null || !new Rect(scroll.Bounds.Size).Contains(new Rect(at.Value, control.Bounds.Size))) return false;
        }
        return true;
    }

    internal static void FocusFirst(Control root)
    {
        var targets = Targets(root).ToList();
        var target = targets.FirstOrDefault(InView) ?? targets.FirstOrDefault();
        if (target != null) Focus(target);
    }

    internal static void Focus(Control control)
    {
        control.Focus(NavigationMethod.Directional);
        control.BringIntoView();
    }

    public void Dispose()
    {
        timer.Stop();
        foreach (var dialog in dialogs.ToArray()) Unregister(dialog);
        owners.Remove(owner);
    }
}

sealed class ControllerList : ListBox
{
    public ControllerList() => Focusable = true;
    public Action? Activate { get; set; }
    protected override Type StyleKeyOverride => typeof(ListBox);
    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Enter) { Activate?.Invoke(); e.Handled = true; }
        else base.OnKeyDown(e);
    }
}

// A row of tab buttons (the open one has Selected set). The bumpers step through it without wrapping: at either end they
// do nothing. Mouse and keyboard use the buttons as before.
sealed class TabBar : WrapPanel
{
    public bool Step(int direction)
    {
        var tabs = Children.OfType<PlankButton>().ToList();
        int at = tabs.FindIndex(t => t.Selected), to = at + direction;
        if (at < 0 || to < 0 || to >= tabs.Count) return false;
        tabs[to].PerformClick();
        ControllerNavigation.Focus(tabs[to]);  // the new tab's own button: D-pad Down goes on into its page
        return true;
    }
}
