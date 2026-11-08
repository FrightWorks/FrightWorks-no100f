namespace FrightWorks;

static class ControllerLayout
{
    const string AutoKey = "auto_controller_layout";
    const string LastKey = "auto_controller_layout_value";

    public static bool NeedsConsent(GameConfig config) =>
        config.Get("launcher", AutoKey) == null && config.Get("controller", "layout") != null;

    public static bool Automatic(GameConfig config) =>
        config.Get("launcher", AutoKey) == "true" ||
        (config.Get("launcher", AutoKey) == null && config.Get("controller", "layout") == null);

    public static string? Match(IReadOnlyList<Gamepads.Pad> pads)
    {
        string? match = null;
        foreach (var pad in pads)
        {
            string? layout = pad.Kind switch
            {
                Gamepads.Kind.Xbox => "xbox_original",
                Gamepads.Kind.GameCube => "gamecube",
                _ => null,
            };
            if (layout == null || (match != null && match != layout)) return null;
            match = layout;
        }
        return match;
    }

    public static string Describe(string? layout) => layout switch
    {
        "xbox_original" => "Original Xbox",
        null or "gamecube" => "GameCube",
        _ => "(custom) " + layout,
    };

    public static void SetAutomatic(GameConfig config, bool automatic)
    {
        config.Set("launcher", AutoKey, automatic ? "true" : "false");
        config.Remove("launcher", LastKey);
    }

    public static bool SetManual(GameConfig config, string layout)
    {
        if (config.Get("launcher", AutoKey) == "false" && config.Get("launcher", LastKey) == null &&
            config.Get("controller", "layout") == layout) return false;
        SetAutomatic(config, false);
        config.SetString("controller", "layout", layout);
        return true;
    }

    // No button bindings are rewritten. Also honor changed layouts from older native F10 writers.
    public static bool ApplyDetected(GameConfig config, IReadOnlyList<Gamepads.Pad> pads)
    {
        if (!Automatic(config)) return false;
        string? current = config.Get("controller", "layout");
        string? last = config.Get("launcher", LastKey);
        if (last != null && current != last)
        {
            SetAutomatic(config, false);
            return true;
        }
        string? match = Match(pads);
        if (match == null || (current == match && last == match)) return false;
        config.SetString("controller", "layout", match);
        config.Set("launcher", AutoKey, "true");
        config.SetString("launcher", LastKey, match);
        return true;
    }
}
