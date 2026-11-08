using System.Globalization;
using System.Drawing;

namespace FrightWorks;

static class HdrSettings
{
    public const decimal PaperMin = 80, PaperMax = 1000, PeakMin = 80, PeakMax = 10000;
    public const decimal PaperDefault = 200, PeakDefault = 1000;

    public static bool NeedsDefaults(GameConfig config) =>
        new[] { "hdr", "hdr_paper_white_nits", "hdr_peak_nits" }.Any(key => config.Get("video", key) == null);

    // Presence, not equality with an old default, identifies a saved preference.
    public static void InitializeMissing(GameConfig config, WindowsHdr.Snapshot display)
    {
        void SetMissing(string key, string value)
        {
            if (config.Get("video", key) == null) config.Set("video", key, value);
        }
        SetMissing("hdr", display.Enabled == true ? "true" : "false");
        SetMissing("hdr_paper_white_nits", Raw(ValidNits(display.PaperWhite, PaperMin, PaperMax) ?? PaperDefault));
        SetMissing("hdr_peak_nits", Raw(ValidNits(display.Peak, PeakMin, PeakMax) ?? PeakDefault));
    }

    public static decimal? ValidNits(float? value, decimal min, decimal max) =>
        value is float n && float.IsFinite(n) && n >= (float)min && n <= (float)max ? (decimal)n : null;

    public static decimal ReadNits(GameConfig config, string key, decimal min, decimal max, decimal fallback) =>
        decimal.TryParse(config.Get("video", key)?.Replace("_", ""), NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
            ? Math.Clamp(value, min, max) : fallback;

    public static string Raw(decimal value) => value.ToString("0.############################", CultureInfo.InvariantCulture);

    public static string? TargetDisplay(GameConfig config)
    {
        if (!OperatingSystem.IsWindows()) return null;
        var screens = Monitors.DesktopScreens();
        if (screens.Count == 0) return null;
        var primary = screens.FirstOrDefault(s => s.Primary);
        string primaryDevice = primary.DeviceName ?? screens[0].DeviceName;
        var target = TargetDeviceName(config, Monitors.List(), primaryDevice);
        if (target != null) return screens.Any(s => s.DeviceName == target) ? target : primaryDevice;
        return WindowDisplay(config, primaryDevice);
    }

    // Fullscreen uses the chosen monitor, or the primary when that saved monitor is disconnected.
    // Windowed and automatic fullscreen retain Aurora's saved-window selection.
    internal static string? TargetDeviceName(GameConfig config, List<Monitors.Monitor> monitors, string primaryDevice)
    {
        var mode = config.Get("video", "display_mode");
        if (mode == "windowed") return null;
        var saved = config.Get("video", "fullscreen_display");
        if (string.IsNullOrEmpty(saved) || saved == "auto") return null;
        return Monitors.Find(monitors, saved)?.DeviceName ?? primaryDevice;
    }

    static string WindowDisplay(GameConfig config, string primary)
    {
        if (!int.TryParse(config.Get("video", "window_x"), out int x) ||
            !int.TryParse(config.Get("video", "window_y"), out int y)) return primary;
        // Aurora restores this rectangle, or centers on the primary display when no position was saved.
        bool widescreen = config.Get("video", "aspect_mode") is string aspect ? aspect != "0" :
            config.Get("video", "force_16_9") == "true" || config.Get("video", "widescreen") != "false";
        int width = int.TryParse(config.Get("video", "window_width"), out int w) && w > 0 ? Math.Max(640, w) : widescreen ? 854 : 640;
        int height = int.TryParse(config.Get("video", "window_height"), out int h) && h > 0 ? Math.Max(480, h) : 480;
        return Monitors.DisplayFromRectangle(new Rectangle(x, y, width, height)) ?? primary;
    }
}
