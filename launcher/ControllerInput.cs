namespace FrightWorks;

[Flags]
enum LauncherButtons
{
    None = 0, Up = 1, Down = 2, Left = 4, Right = 8,
    Confirm = 16, Back = 32, Previous = 64, Next = 128,
    ScrollUp = 256, ScrollDown = 512,
}

readonly record struct ControllerSample(uint Id, LauncherButtons Buttons, bool CoarseHorizontal = false);

// New devices and focus/modal/game transitions require neutral. Only movement repeats.
sealed class ControllerInput
{
    const LauncherButtons Repeatable = LauncherButtons.Up | LauncherButtons.Down | LauncherButtons.Left |
        LauncherButtons.Right | LauncherButtons.ScrollUp | LauncherButtons.ScrollDown;
    LauncherButtons previous, repeating;
    bool armed;
    long nextRepeat;

    public void Reset() { armed = false; previous = repeating = LauncherButtons.None; }

    public LauncherButtons Read(LauncherButtons buttons, long milliseconds)
    {
        if (!armed)
        {
            armed = buttons == LauncherButtons.None;
            previous = buttons;
            return LauncherButtons.None;
        }
        var pressed = buttons & ~previous;
        previous = buttons;
        var movement = buttons & Repeatable;
        if ((movement & (LauncherButtons.Up | LauncherButtons.Down)) == (LauncherButtons.Up | LauncherButtons.Down))
            movement &= ~(LauncherButtons.Up | LauncherButtons.Down);
        if ((movement & (LauncherButtons.Left | LauncherButtons.Right)) == (LauncherButtons.Left | LauncherButtons.Right))
            movement &= ~(LauncherButtons.Left | LauncherButtons.Right);
        movement = First(movement);
        var result = First(pressed & ~Repeatable);
        if (movement != repeating)
        {
            repeating = movement;
            nextRepeat = milliseconds + 400;
            if (result == LauncherButtons.None) result = movement;
        }
        else if (movement != LauncherButtons.None && milliseconds >= nextRepeat)
        {
            nextRepeat = milliseconds + 100;
            if (result == LauncherButtons.None) result = movement;
        }
        return result;
    }

    static LauncherButtons First(LauncherButtons value) => (LauncherButtons)((int)value & -(int)value);

    public static ControllerSample SampleFromSdl(uint id, uint buttons, short x, short y, short scrollY) =>
        new(id, FromSdl(buttons, x, y, scrollY),
            (x < -16000 || x > 16000) && (buttons & ((1u << 13) | (1u << 14))) == 0);

    public static LauncherButtons FromSdl(uint buttons, short x, short y, short scrollY)
    {
        LauncherButtons result = 0;
        if ((buttons & (1u << 0)) != 0) result |= LauncherButtons.Confirm;
        if ((buttons & (1u << 1)) != 0) result |= LauncherButtons.Back;
        if ((buttons & (1u << 9)) != 0) result |= LauncherButtons.Previous;
        if ((buttons & (1u << 10)) != 0) result |= LauncherButtons.Next;
        if ((buttons & (1u << 11)) != 0 || y < -16000) result |= LauncherButtons.Up;
        if ((buttons & (1u << 12)) != 0 || y > 16000) result |= LauncherButtons.Down;
        if ((buttons & (1u << 13)) != 0 || x < -16000) result |= LauncherButtons.Left;
        if ((buttons & (1u << 14)) != 0 || x > 16000) result |= LauncherButtons.Right;
        if (scrollY < -16000) result |= LauncherButtons.ScrollUp;
        if (scrollY > 16000) result |= LauncherButtons.ScrollDown;
        return result;
    }
}
