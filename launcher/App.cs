using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Controls.Documents;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;
using Avalonia.Threading;

namespace FrightWorks;

// The Avalonia application: Fluent's dark theme recoloured in the launcher's purples, lime and cream, and the start
// (an offer to copy an earlier version's files first, then the launcher window).
sealed class App : Application
{
    public static WindowIcon? Icon
    {
        get
        {
            using var s = Skin.Resource("frightworks.ico");
            return s == null ? null : new WindowIcon(s);
        }
    }

    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        Styles.Add(new FluentTheme());
        var r = Resources;
        IBrush field = Skin.Brush(Skin.Field), hover = Skin.Brush(Skin.FieldHover), purple = Skin.Brush(Skin.Purple),
               purpleLight = Skin.Brush(Skin.Light(Skin.Purple, 0.25f)), deep = Skin.Brush(Skin.PurpleDeep),
               cream = Skin.Brush(Skin.Cream), dim = Skin.Brush(Skin.CreamDim);
        void Set(IBrush brush, params string[] keys) { foreach (var key in keys) r[key] = brush; }
        // The accent (focus rings, ticked boxes) in the signs' purple.
        r["SystemAccentColor"] = Skin.Purple;
        r["SystemAccentColorLight1"] = Skin.Light(Skin.Purple, 0.15f);
        r["SystemAccentColorLight2"] = Skin.Light(Skin.Purple, 0.3f);
        r["SystemAccentColorLight3"] = Skin.Light(Skin.Purple, 0.45f);
        r["SystemAccentColorDark1"] = Skin.PurpleDeep;
        r["SystemAccentColorDark2"] = Skin.PurpleDeep;
        r["SystemAccentColorDark3"] = Skin.PurpleDeep;
        // Drop-down lists: the dark field, cream text, purple for the highlighted choice.
        Set(field, "ComboBoxBackground", "ComboBoxBackgroundUnfocused", "ComboBoxBackgroundDisabled", "ComboBoxDropDownBackground", "ComboBoxItemBackground");
        Set(hover, "ComboBoxBackgroundPointerOver", "ComboBoxBackgroundPressed");
        Set(deep, "ComboBoxBorderBrush", "ComboBoxBorderBrushDisabled", "ComboBoxDropDownBorderBrush", "ComboBoxBackgroundBorderBrushUnfocused");
        Set(purple, "ComboBoxBorderBrushPointerOver", "ComboBoxBorderBrushPressed", "ComboBoxBackgroundBorderBrushFocused");
        Set(cream, "ComboBoxForeground", "ComboBoxForegroundFocused", "ComboBoxForegroundFocusedPressed", "ComboBoxDropDownGlyphForeground",
            "ComboBoxDropDownGlyphForegroundFocused", "ComboBoxDropDownGlyphForegroundFocusedPressed", "ComboBoxItemForeground",
            "ComboBoxItemForegroundPointerOver", "ComboBoxItemForegroundPressed", "ComboBoxItemForegroundSelected",
            "ComboBoxItemForegroundSelectedPointerOver", "ComboBoxItemForegroundSelectedPressed");
        Set(dim, "ComboBoxForegroundDisabled", "ComboBoxDropDownGlyphForegroundDisabled", "ComboBoxItemForegroundDisabled");
        Set(hover, "ComboBoxItemBackgroundPointerOver", "ComboBoxItemBackgroundPressed");
        Set(purple, "ComboBoxItemBackgroundSelected", "ComboBoxItemBackgroundSelectedPointerOver", "ComboBoxItemBackgroundSelectedPressed");
        // Check boxes (the mods list): cream text, a cream outline, purple when ticked.
        Set(cream, "CheckBoxForegroundUnchecked", "CheckBoxForegroundChecked", "CheckBoxForegroundUncheckedPointerOver",
            "CheckBoxForegroundCheckedPointerOver", "CheckBoxForegroundUncheckedPressed", "CheckBoxForegroundCheckedPressed",
            "CheckBoxCheckGlyphForegroundChecked", "CheckBoxCheckGlyphForegroundCheckedPointerOver", "CheckBoxCheckGlyphForegroundCheckedPressed");
        Set(dim, "CheckBoxCheckBackgroundStrokeUnchecked", "CheckBoxCheckBackgroundStrokeUncheckedPointerOver", "CheckBoxCheckBackgroundStrokeUncheckedPressed");
        Set(purple, "CheckBoxCheckBackgroundFillChecked", "CheckBoxCheckBackgroundStrokeCheckedPointerOver", "CheckBoxCheckBackgroundStrokeCheckedPressed");
        Set(purpleLight, "CheckBoxCheckBackgroundFillCheckedPointerOver", "CheckBoxCheckBackgroundFillCheckedPressed");
        // Scroll bars: a purple thumb on a dark track.
        r["ScrollBarThumbBackgroundColor"] = Skin.Purple;
        r["ScrollBarPanningThumbBackgroundColor"] = Skin.Purple;
        Set(purpleLight, "ScrollBarThumbFillPointerOver", "ScrollBarThumbFillPressed");
        Set(Skin.Brush(Skin.Argb(70, 0, 0, 0)), "ScrollBarBackground", "ScrollBarBackgroundPointerOver", "ScrollBarTrackFill", "ScrollBarTrackFillPointerOver");
        Set(Brushes.Transparent, "ScrollBarTrackStroke", "ScrollBarTrackStrokePointerOver", "ScrollBarButtonBackground");
        Set(dim, "ScrollBarButtonArrowForeground");
        Set(cream, "ScrollBarButtonArrowForegroundPointerOver", "ScrollBarButtonArrowForegroundPressed");

        // The body face everywhere, drop-down lists' popups included.
        Styles.Add(new Style(x => x.Is<TopLevel>())
        {
            Setters =
            {
                new Setter(TextElement.FontFamilyProperty, Skin.BodyFamily),
                new Setter(TextElement.FontSizeProperty, Skin.BodySize),
                new Setter(TextElement.ForegroundProperty, cream),
            },
        });
    }

    public override void OnFrameworkInitializationCompleted()
    {
        // Anything a handler or posted job throws on the UI thread and nothing caught: ask instead of closing the launcher.
        Dispatcher.UIThread.UnhandledException += (_, e) =>
        {
            e.Handled = true;
            Fault.Report(e.Exception).Watch();
        };
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            var args = desktop.Args ?? Array.Empty<string>();
            Dispatcher.UIThread.Post(() => Start(desktop, args).Watch());
        }
        base.OnFrameworkInitializationCompleted();
    }

    // "--shots <dir>": draw every page to <dir>/<page>.png and exit. "--selftest": build every page, then exit 0.
    static async Task Start(IClassicDesktopStyleApplicationLifetime desktop, string[] args)
    {
        try
        {
            int shots = Array.IndexOf(args, "--shots");
            bool selftest = args.Contains("--selftest");
            var paths = DataPaths.Find();
            if (shots < 0 && !selftest)
            {
                if (paths.Portable) paths.RemoveOldShippedCopies();
            }
            var window = new MainWindow(shots >= 0 && shots + 1 < args.Length ? args[shots + 1] : null,
                                        dataPaths: paths, enableServices: shots < 0 && !selftest);
            desktop.MainWindow = window;
            desktop.ShutdownMode = ShutdownMode.OnMainWindowClose;
            if (selftest) window.Opened += (_, _) => window.Close();
            window.Show();
            if (shots < 0 && !selftest)
            {
                await DataImport.OfferAtStart(paths, MainWindow.AppTitle, window);
                if (window.IsVisible) window.CompleteStartup();
            }
        }
        catch (Exception e)
        {
            // Nothing to carry on with before the window is up: say what went wrong, then close.
            await Fault.Report(e, canContinue: false);
            desktop.Shutdown(1);
        }
    }
}

// Errors nobody caught. Like the old launcher (Windows Forms' own handler), the player gets a dialog naming the problem with
// Continue or Quit instead of a launcher that vanishes. Hooked up in App (UI thread jobs and handlers), Program (other
// threads) and Watch (tasks started and not awaited: `_ = Task()` hides their exceptions).
static class Fault
{
    static bool showing;

    // Tests set this to look at the error instead of opening a dialog.
    internal static Func<Exception, Task>? Override;

    // The window the dialog sits over: the launcher's, or (headless tests have no desktop lifetime) the one a test names.
    internal static Window? Owner;

    // Runs the task for its side effects; if it fails, reports it. Use instead of `_ = Task()`.
    public static async void Watch(this Task task)
    {
        try { await task; }
        catch (OperationCanceledException) { }
        catch (Exception e) { await Report(e); }
    }

    // Shows the error on the UI thread. Returns when the player has answered; Quit closes the launcher (exit code 1).
    // canContinue = false (the program is going down anyway, or nothing is up yet): the only button is Quit.
    public static async Task Report(Exception e, bool canContinue = true)
    {
        try
        {
            Console.Error.WriteLine(e);
            if (!Dispatcher.UIThread.CheckAccess())
            {
                await Dispatcher.UIThread.InvokeAsync(() => Report(e, canContinue));
                return;
            }
            if (Override != null) { await Override(e); return; }
            if (showing) return;  // one question at a time: a repeating fault would stack dialogs
            showing = true;
            try
            {
                var desktop = Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime;
                var owner = (Owner ?? desktop?.MainWindow) is { IsVisible: true } w ? w : null;
                var text = new List<string>
                {
                    "The launcher hit an unexpected error:\n" + e.Message,
                    e.GetType().FullName + (canContinue ? "\n\nContinue tries to carry on with the launcher; Quit closes it." : ""),
                };
                var buttons = canContinue ? new[] { ("Continue", true), ("Quit", false) } : new[] { ("Quit", true) };
                int pick = await Dialogs.Choose(owner, MainWindow.AppTitle, "Something went wrong", text, buttons);
                if (!canContinue || pick == 1) Quit();  // the dialog's X or Escape (-1) carries on, like Continue
            }
            finally { showing = false; }
        }
        catch (Exception again)
        {
            // The dialog itself failed: nothing left to show it with.
            Console.Error.WriteLine(again);
            Quit();
        }
    }

    static void Quit()
    {
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop) desktop.Shutdown(1);
        else Environment.Exit(1);
    }
}
