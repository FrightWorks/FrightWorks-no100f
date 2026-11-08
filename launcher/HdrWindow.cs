using System.Globalization;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using static ScoobyReMasked.Skin;

namespace ScoobyReMasked;

sealed class HdrWindow : Window
{
    readonly string configPath;
    readonly HdrIllustration illustration = new();
    internal readonly Slider PaperSlider;
    internal readonly Slider PeakSlider;

    public HdrWindow(string configPath, string status, Window owner)
    {
        this.configPath = configPath;
        Title = MainWindow.AppTitle + " - HDR calibration";
        Icon = App.Icon;
        Background = Brush(BoardBottom);
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var screen = owner.Screens.ScreenFromWindow(owner) ?? owner.Screens.Primary;
        Width = Math.Min(760, screen == null ? 760 : screen.WorkingArea.Width / screen.Scaling - 40);
        Height = Math.Min(680, screen == null ? 680 : screen.WorkingArea.Height / screen.Scaling - 80);
        MinWidth = Math.Min(420, Width);
        MinHeight = Math.Min(360, Height);
        var body = new StackPanel { Margin = new Thickness(20), Spacing = 5 };
        body.Children.Add(Skin.Title("HDR brightness"));
        // Only the Windows game has the F10 HDR page; elsewhere the launcher is the only place to change these.
        body.Children.Add(Note(OperatingSystem.IsWindows()
            ? "Launcher changes apply when the game starts. In-game F10 brightness sliders update live; " +
              "turning HDR on or off requires a restart."
            : "Launcher changes apply when the game starts.", 680));
        body.Children.Add(Note("With launcher controller navigation, use the left stick for larger steps and the D-pad for precise 0.1-nit adjustments.", 680));
        if (!OperatingSystem.IsWindows())
            body.Children.Add(Note("Windows HDR defaults are unavailable here. Saved targets remain editable, but Linux HDR " +
                                   "output and calibration are not verified by this launcher.", 680));
        var config = new GameConfig(configPath);
        var enabled = new ThemedCombo();
        enabled.Items.Add("Off");
        enabled.Items.Add("On (next game start)");
        enabled.SelectedIndex = config.Get("video", "hdr") == "true" ? 1 : 0;
        enabled.SelectionCommitted += (_, _) => Save("hdr", enabled.SelectedIndex == 1 ? "true" : "false");
        var table = new FormTable();
        table.Add(FieldLabel("HDR"), enabled);
        body.Children.Add(table);
        PaperSlider = AddSlider(body, "Paper white", "hdr_paper_white_nits",
                                HdrSettings.PaperMin, HdrSettings.PaperMax, HdrSettings.PaperDefault, 10);
        body.Children.Add(Note("Normal picture brightness: choose a comfortable level.", 680));
        PeakSlider = AddSlider(body, "Peak highlights", "hdr_peak_nits",
                               HdrSettings.PeakMin, HdrSettings.PeakMax, HdrSettings.PeakDefault, 100);
        body.Children.Add(Note("Peak affects the brightest tones, not overall brightness. The effective game peak never falls " +
                               "below paper white. If the highlight checker is solid white, lower paper white and peak " +
                               "temporarily until its detail returns; high targets may be clipped by the display.", 680));
        body.Children.Add(illustration);
        body.Children.Add(Note("These are SDR illustrations, not HDR test patterns or measured nit levels. For live scRGB " +
                               "patterns, use Windows HDR and the game's Direct3D output, then F10 > Video > HDR calibration.", 680));
        body.Children.Add(Note(status, 680));
        body.Children.Add(Note("Windows/desktop settings are never changed.", 680));
        body.Children.Add(new PlankButton("Done", (_, _) => Close(), primary: true));
        Content = new ScrollViewer { Content = body, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                                     VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        Opened += (_, _) => PaperSlider.Focus(NavigationMethod.Tab);
        UpdateIllustration();
    }

    Slider AddSlider(StackPanel body, string label, string key, decimal min, decimal max, decimal fallback, double controllerStep)
    {
        body.Children.Add(FieldLabel(label));
        decimal value = HdrSettings.ReadNits(new GameConfig(configPath), key, min, max, fallback);
        var readout = Note(value.ToString("0.0", CultureInfo.InvariantCulture) + " nits");
        var slider = new ControllerSlider(controllerStep) { Minimum = (double)min, Maximum = (double)max, Value = (double)value,
                                  SmallChange = 0.1, LargeChange = 10, Focusable = true, Name = key };
        AutomationProperties.SetName(slider, label + " in nits");
        bool updating = false;
        slider.PropertyChanged += (_, change) =>
        {
            if (change.Property != RangeBase.ValueProperty || updating) return;
            decimal nits = Math.Round((decimal)slider.Value, 1, MidpointRounding.AwayFromZero);
            updating = true;
            slider.Value = (double)nits;
            updating = false;
            readout.Text = nits.ToString("0.0", CultureInfo.InvariantCulture) + " nits";
            Save(key, HdrSettings.Raw(nits));
            UpdateIllustration();
        };
        body.Children.Add(slider);
        body.Children.Add(readout);
        body.Children.Add(Note($"{min} - {max} nits"));
        return slider;
    }

    void Save(string key, string value)
    {
        var config = new GameConfig(configPath);
        config.Set("video", key, value);
        try { config.Save(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Dialogs.Message(this, "Couldn't save HDR settings: " + e.Message).Watch();
        }
    }

    void UpdateIllustration()
    {
        if (PaperSlider == null || PeakSlider == null) return;
        illustration.Paper = PaperSlider.Value;
        illustration.Peak = PeakSlider.Value;
        illustration.InvalidateVisual();
    }

    // Deliberately SDR: the launcher does not negotiate an HDR swapchain or measure the panel.
    sealed class HdrIllustration : Control
    {
        public double Paper, Peak;
        public HdrIllustration() => Height = 95;

        public override void Render(DrawingContext context)
        {
            base.Render(context);
            double width = Math.Max(1, Bounds.Width);
            double paper = Math.Clamp(Paper / 1000, 0, 1);
            double peak = Math.Clamp(Math.Max(Paper, Peak) / 10000, 0, 1);
            context.FillRectangle(Brush(Field), new Rect(0, 0, width, Bounds.Height));
            var normal = new SolidColorBrush(Color.FromRgb((byte)(45 + paper * 180),
                                                         (byte)(40 + paper * 165), (byte)(55 + paper * 170)));
            context.FillRectangle(normal, new Rect(10, 10, Math.Max(1, width * 0.48 - 15), 75));
            var highlight = new SolidColorBrush(Color.FromRgb((byte)(160 + peak * 95),
                                                            (byte)(150 + peak * 105), (byte)(170 + peak * 85)));
            context.FillRectangle(highlight, new Rect(width * 0.52, 10, Math.Max(1, width * 0.48 - 10), 75));
        }
    }
}
