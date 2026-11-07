using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace FrightWorks;

// Questions and messages in the launcher's own colours (Avalonia has no message box; the old launcher's update and
// Xbox questions already looked like this). Every dialog is modal over the launcher, or on its own before it opens.
static class Dialogs
{
    public enum Answer { Yes, No, Cancel }

    // The index of the button pressed, or -1 when the window was closed (Escape or the title bar's X).
    public static async Task<int> Choose(Window? owner, string windowTitle, string? heading, IEnumerable<string> paragraphs,
                                         params (string Text, bool Primary)[] buttons)
    {
        var dialog = Make(windowTitle, owner);
        var body = new StackPanel { Orientation = Orientation.Vertical, MaxWidth = 560 };
        if (heading != null)
        {
            var title = Skin.Title(heading);
            title.Margin = new Thickness(0, 0, 0, 6);
            body.Children.Add(title);
        }
        foreach (var p in paragraphs)
        {
            var note = Skin.Note(p);
            if (heading == null) note.Foreground = Skin.Brush(Skin.Cream);
            body.Children.Add(note);
        }
        var row = new WrapPanel { Margin = new Thickness(0, 8, 0, 0) };
        int answer = -1;
        PlankButton? first = null;
        for (int i = 0; i < buttons.Length; i++)
        {
            int index = i;
            var button = new PlankButton(buttons[i].Text, (_, _) => { answer = index; dialog.Close(); }, primary: buttons[i].Primary);
            first ??= buttons[i].Primary ? button : null;
            if (buttons[i].Text is "No" or "Cancel" or "Keep my layout" or "Not now") first = button;
            row.Children.Add(button);
        }
        body.Children.Add(row);
        dialog.Content = new Border { Padding = new Thickness(20, 14, 20, 14), Child = body };
        dialog.Opened += (_, _) => (first ?? row.Children.OfType<PlankButton>().FirstOrDefault())?.Focus(NavigationMethod.Tab);
        await ShowModal(dialog, owner);
        return answer;
    }

    public static Task<int> Choose(Window? owner, string heading, IEnumerable<string> paragraphs, params (string Text, bool Primary)[] buttons) =>
        Choose(owner, MainWindow.AppTitle, heading, paragraphs, buttons);

    public static Task Message(Window? owner, string text) =>
        Choose(owner, MainWindow.AppTitle, null, Paragraphs(text), ("OK", true));

    public static async Task<bool> YesNo(Window? owner, string text) =>
        await Choose(owner, MainWindow.AppTitle, null, Paragraphs(text), ("Yes", true), ("No", false)) == 0;

    public static async Task<Answer> YesNoCancel(Window? owner, string text) =>
        await Choose(owner, MainWindow.AppTitle, null, Paragraphs(text), ("Yes", true), ("No", false), ("Cancel", false)) switch
        {
            0 => Answer.Yes, 1 => Answer.No, _ => Answer.Cancel,
        };

    public static async Task<bool> OkCancel(Window? owner, string text) =>
        await Choose(owner, MainWindow.AppTitle, null, Paragraphs(text), ("OK", true), ("Cancel", false)) == 0;

    // A blank line starts a new paragraph; single line breaks stay inside one.
    static IEnumerable<string> Paragraphs(string text) => text.Split("\n\n");

    // A dialog window in the board's colours.
    public static Window Make(string title, Window? owner) => new()
    {
        Title = title, SizeToContent = SizeToContent.WidthAndHeight, CanResize = false, CanMinimize = false, CanMaximize = false,
        ShowInTaskbar = owner == null, WindowStartupLocation = owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen,
        Background = Skin.Brush(Skin.BoardBottom), Icon = App.Icon,
    };

    public static async Task ShowModal(Window dialog, Window? owner)
    {
        var navigation = ControllerNavigation.For(owner);
        var focused = owner?.FocusManager?.GetFocusedElement() as Control;
        navigation?.Register(dialog);
        dialog.KeyDown += (_, e) => { if (e.Key == Key.Escape) { e.Handled = true; dialog.Close(); } };
        try
        {
            if (owner is { IsVisible: true })
            {
                if (owner.WindowState == WindowState.Minimized) owner.WindowState = WindowState.Normal;
                await dialog.ShowDialog(owner);
                return;
            }
            var closed = new TaskCompletionSource();
            dialog.Closed += (_, _) => closed.TrySetResult();
            dialog.Show();
            await closed.Task;
        }
        finally
        {
            navigation?.Unregister(dialog);
            if (owner is { IsActive: true } && focused is { IsEffectivelyVisible: true, IsEffectivelyEnabled: true })
                focused.Focus(NavigationMethod.Directional);
        }
    }
}
