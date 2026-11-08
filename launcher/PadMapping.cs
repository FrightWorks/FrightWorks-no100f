namespace FrightWorks;

// The gamepad button mapping in Config.toml, the same keys the game's F10 > Controls writes
// (runtime_config.h, controller_button_names.h): [controller] a ... right = the pad button that presses that GameCube
// button, and xbox_black / xbox_white for the original Xbox layout. Values are positional pad button names ("south" =
// A on Xbox, Cross on PlayStation, B on Nintendo), optionally "@threshold" for triggers and stick directions and
// ",second" for a second binding; the launcher keeps both suffixes it doesn't edit.
static class PadMapping
{
    public sealed record Choice(string Name, string Label)
    {
        public override string ToString() => Label;
    }

    // controller_button_names.h kNativeButtons, in a friendlier order. "unmapped" (the analog trigger) is offered for L
    // and R only, "disabled" = no button.
    public static readonly Choice[] Inputs =
    {
        new("south", "South (A / Cross)"), new("east", "East (B / Circle)"),
        new("west", "West (X / Square)"), new("north", "North (Y / Triangle)"),
        new("left_shoulder", "Left bumper (LB / L1)"), new("right_shoulder", "Right bumper (RB / R1)"),
        new("left_trigger", "Left trigger (LT / L2)"), new("right_trigger", "Right trigger (RT / R2)"),
        new("start", "Start / Options"), new("back", "Back / Select / Create"), new("guide", "Guide / Home / PS"),
        new("left_stick", "Left stick click (L3)"), new("right_stick", "Right stick click (R3)"),
        new("dpad_up", "D-pad Up"), new("dpad_down", "D-pad Down"), new("dpad_left", "D-pad Left"), new("dpad_right", "D-pad Right"),
        new("right_stick_up", "Right stick up"), new("right_stick_down", "Right stick down"),
        new("right_stick_left", "Right stick left"), new("right_stick_right", "Right stick right"),
        new("left_stick_up", "Left stick up"), new("left_stick_down", "Left stick down"),
        new("left_stick_left", "Left stick left"), new("left_stick_right", "Left stick right"),
        new("misc1", "Misc 1 / Share / Mic"), new("touchpad", "Touchpad click"),
        new("right_paddle1", "Right paddle 1"), new("left_paddle1", "Left paddle 1"),
        new("right_paddle2", "Right paddle 2"), new("left_paddle2", "Left paddle 2"),
        new("disabled", "Off"),
    };

    public sealed record Row(string Key, string Label, string Default, bool AnalogTrigger = false, bool Xbox = false);

    // The defaults are what F10's GameCube preset writes for a gamepad (aurora's default mapping).
    public static readonly Row[] Rows =
    {
        new("a", "A (jump)", "south"),
        new("b", "B (head smash, back)", "east"),
        new("x", "X (dig, enter, talk)", "west"),
        new("y", "Y (on-screen display)", "north"),
        new("start", "Start (pause)", "start"),
        new("z", "Z (+ L bubble, + R sneak)", "right_shoulder"),
        new("l", "L (gum)", "unmapped", AnalogTrigger: true),
        new("r", "R (run)", "unmapped", AnalogTrigger: true),
        new("up", "D-pad Up", "dpad_up"),
        new("down", "D-pad Down", "dpad_down"),
        new("left", "D-pad Left", "dpad_left"),
        new("right", "D-pad Right", "dpad_right"),
        // Original Xbox layout only (hle/no100f_xbox_layout.h).
        new("xbox_black", "Black (sneak)", "right_shoulder", Xbox: true),
        new("xbox_white", "White (soap bubble)", "left_shoulder", Xbox: true),
    };

    public static IEnumerable<Choice> ChoicesFor(Row row)
    {
        if (row.AnalogTrigger)
            yield return new("unmapped", row.Key == "l" ? "Left trigger, analog (default)" : "Right trigger, analog (default)");
        foreach (var choice in Inputs) yield return choice;
    }

    // "south@30,east" -> ("south", "@30", ",east").
    public static (string Name, string Threshold, string Second) Split(string value)
    {
        int comma = value.IndexOf(',');
        string first = comma < 0 ? value : value[..comma];
        string second = comma < 0 ? "" : value[comma..];
        int at = first.IndexOf('@');
        return (at < 0 ? first.Trim() : first[..at].Trim(), at < 0 ? "" : first[at..].Trim(), second.Trim());
    }

    // The new value for a row whose pad button changes: the second binding stays, the old threshold goes (it belonged
    // to the old input; a trigger or stick direction without one presses at half travel).
    public static string Compose(string name, string? oldValue)
    {
        string second = oldValue == null ? "" : Split(oldValue).Second;
        return name + second;
    }
}
