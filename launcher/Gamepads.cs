using System.Runtime.InteropServices;

namespace FrightWorks;

// Connected gamepads, read with the game's own SDL3 library so the launcher sees the same controller types as the game
// (button prompts, Xbox layout). SDL runs on its own thread (its device notifications need that thread's message
// loop); HIDAPI is off, so the launcher never sends anything to a controller. Paused while the game runs.
// On Linux the game has SDL built in: a libSDL3.so.0 beside the game is used if there is one, else the system's.
sealed class Gamepads : IDisposable
{
    public enum Kind { Other, Xbox, PlayStation, Switch, GameCube }
    public sealed record Pad(string Name, Kind Kind);

    public static string LibraryName => OperatingSystem.IsWindows() ? "SDL3.dll" : "libSDL3.so.0";
    readonly string sdlPath;
    readonly object gate = new();
    IReadOnlyList<Pad> pads = Array.Empty<Pad>();
    IReadOnlyList<ControllerSample> samples = Array.Empty<ControllerSample>();
    Thread? thread;
    volatile bool stop;

    public Gamepads(string gameFolder) => sdlPath = Path.Combine(gameFolder, LibraryName);

    // False when SDL3 is missing or failed to start (no detection; the launcher works without it).
    public bool Available { get; private set; } = true;
    public string? Error { get; private set; }

    public IReadOnlyList<Pad> Connected { get { lock (gate) return pads; } }
    public IReadOnlyList<ControllerSample> Samples { get { lock (gate) return samples; } }
    internal bool Polling => thread != null;

    public void Start()
    {
        // A thread still winding down after a timed-out Pause() counts as running: never two pollers.
        if (thread is { IsAlive: true } || !Available) return;
        stop = false;
        thread = new Thread(Run) { IsBackground = true, Name = "Gamepads" };
        thread.Start();
    }

    // Lets the game have the controllers to itself.
    public void Pause()
    {
        stop = true;
        // Wait for the handles to close, but never hang the UI thread on a stalled SDL call: after 2 s the game
        // starts anyway (Start() will not spawn a second poller while this one is alive).
        if (thread == null || thread.Join(2000)) thread = null;
        lock (gate) { pads = Array.Empty<Pad>(); samples = Array.Empty<ControllerSample>(); }
    }

    public void Dispose() => Pause();

    // SDL refuses a hint when the user's environment already sets it, even to the value wanted here. That is fine; only a
    // different value is a real conflict (HIDAPI left on would let SDL talk to the pad).
    static bool SetHint(Api api, string name, string wanted) => HintAccepted(api.SetHint(name, wanted), api.GetHint(name), wanted);

    internal static bool HintAccepted(bool set, string? current, string wanted) => set || current == wanted;

    void Run()
    {
        IntPtr lib = IntPtr.Zero;
        try
        {
            bool loaded = File.Exists(sdlPath) ? NativeLibrary.TryLoad(sdlPath, out lib)
                        : !OperatingSystem.IsWindows() && NativeLibrary.TryLoad(LibraryName, out lib);
            if (!loaded) throw new InvalidOperationException($"{LibraryName} is missing or could not be loaded.");
            var api = new Api(lib);
            if (!SetHint(api, "SDL_JOYSTICK_HIDAPI", "0") || !SetHint(api, "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"))
                throw new InvalidOperationException("SDL could not configure read-only launcher controller input.");
            var handles = new Dictionary<uint, IntPtr>();
            var unreadable = new HashSet<uint>();  // listed, but SDL_OpenGamepad failed (busy, no permission): skipped
            try
            {
                if (!api.Init(0x2000 /* SDL_INIT_GAMEPAD */)) throw new InvalidOperationException(api.Error());
                while (!stop)
                {
                    api.PumpEvents();
                    var list = new List<Pad>();
                    var input = new List<ControllerSample>();
                    var present = new HashSet<uint>();
                    IntPtr ids = api.GetGamepads(out int count);
                    if (ids == IntPtr.Zero) throw new InvalidOperationException(api.Error());
                    try
                    {
                        for (int i = 0; i < count; i++)
                        {
                            uint id = (uint)Marshal.ReadInt32(ids, i * 4);
                            present.Add(id);
                            string name = Marshal.PtrToStringUTF8(api.GetGamepadNameForID(id)) ?? "Controller";
                            var kind = KindOf(api.GetGamepadTypeForID(id));
                            if (!handles.TryGetValue(id, out var handle))
                            {
                                if (unreadable.Contains(id)) { list.Add(new Pad(name, kind)); continue; }
                                handle = api.Open(id);
                                if (handle == IntPtr.Zero)
                                {
                                    if (!api.IsGamepad(id)) { present.Remove(id); continue; } // Unplugged after enumeration.
                                    // One pad that can't be opened must not take the others down with it.
                                    unreadable.Add(id);
                                    Console.Error.WriteLine($"Launcher controller input: {name} could not be opened: {api.Error()}");
                                    list.Add(new Pad(name, kind));
                                    continue;
                                }
                                handles.Add(id, handle);
                            }
                            list.Add(new Pad(name, kind));
                            uint buttons = 0;
                            for (int b = 0; b <= 14; b++) if (api.Button(handle, b)) buttons |= 1u << b;
                            input.Add(ControllerInput.SampleFromSdl(id, buttons,
                                api.Axis(handle, 0), api.Axis(handle, 1), api.Axis(handle, 3)));
                        }
                    }
                    finally { api.Free(ids); }
                    unreadable.RemoveWhere(id => !present.Contains(id));
                    foreach (uint id in handles.Keys.Where(id => !present.Contains(id)).ToArray())
                    {
                        api.Close(handles[id]);
                        handles.Remove(id);
                    }
                    lock (gate) { pads = list; samples = input; }
                    Thread.Sleep(16);
                }
            }
            finally
            {
                foreach (var handle in handles.Values) api.Close(handle);
                api.Quit();
            }
        }
        catch (Exception e)
        {
            Error = e.Message;
            Available = false;
            Console.Error.WriteLine("Launcher controller input: " + e);
        }
        finally
        {
            // SDL stays loaded: unloading it while its threads wind down is not worth the risk.
            lock (gate) { pads = Array.Empty<Pad>(); samples = Array.Empty<ControllerSample>(); }
        }
    }

    // SDL_GamepadType, as the game sorts it (hle/input/pad.cpp).
    static Kind KindOf(int type) => type switch
    {
        2 or 3 => Kind.Xbox,                 // XBOX360, XBOXONE (also third-party pads in XInput mode)
        4 or 5 or 6 => Kind.PlayStation,     // PS3, PS4, PS5
        7 or 8 or 9 or 10 => Kind.Switch,    // Switch Pro, Joy-Cons
        11 => Kind.GameCube,                 // GameCube adapter
        _ => Kind.Other,
    };

    public static string Describe(Kind kind) => kind switch
    {
        Kind.Xbox => "Xbox",
        Kind.PlayStation => "PlayStation",
        Kind.Switch => "Nintendo Switch",
        Kind.GameCube => "GameCube",
        _ => "other",
    };

    // The few SDL3 functions used, bound from the game's library.
    sealed class Api
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)] delegate bool InitFn(uint flags);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)]
        delegate bool SetHintFn([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr GetHintFn([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void VoidFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr GetGamepadsFn(out int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr NameFn(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int TypeFn(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FreeFn(IntPtr p);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr OpenFn(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void CloseFn(IntPtr pad);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)] delegate bool ButtonFn(IntPtr pad, int button);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate short AxisFn(IntPtr pad, int axis);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr ErrorFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)] delegate bool IsGamepadFn(uint id);

        public readonly Func<uint, bool> Init;
        public readonly Func<string, string, bool> SetHint;
        public readonly Func<string, string?> GetHint;
        public readonly Action PumpEvents, Quit;
        readonly GetGamepadsFn getGamepads;
        public readonly Func<uint, IntPtr> GetGamepadNameForID;
        public readonly Func<uint, int> GetGamepadTypeForID;
        public readonly Action<IntPtr> Free;
        public readonly Func<uint, IntPtr> Open;
        public readonly Action<IntPtr> Close;
        public readonly Func<IntPtr, int, bool> Button;
        public readonly Func<IntPtr, int, short> Axis;
        public readonly Func<string> Error;
        public readonly Func<uint, bool> IsGamepad;

        public Api(IntPtr lib)
        {
            T F<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(lib, name));
            var init = F<InitFn>("SDL_Init"); Init = f => init(f);
            var hint = F<SetHintFn>("SDL_SetHint"); SetHint = (n, v) => hint(n, v);
            var getHint = F<GetHintFn>("SDL_GetHint"); GetHint = n => Marshal.PtrToStringUTF8(getHint(n));
            var pump = F<VoidFn>("SDL_PumpEvents"); PumpEvents = () => pump();
            var quit = F<VoidFn>("SDL_Quit"); Quit = () => quit();
            getGamepads = F<GetGamepadsFn>("SDL_GetGamepads");
            var name = F<NameFn>("SDL_GetGamepadNameForID"); GetGamepadNameForID = id => name(id);
            var type = F<TypeFn>("SDL_GetGamepadTypeForID"); GetGamepadTypeForID = id => type(id);
            var free = F<FreeFn>("SDL_free"); Free = p => free(p);
            var open = F<OpenFn>("SDL_OpenGamepad"); Open = id => open(id);
            var close = F<CloseFn>("SDL_CloseGamepad"); Close = p => close(p);
            var button = F<ButtonFn>("SDL_GetGamepadButton"); Button = (p, b) => button(p, b);
            var axis = F<AxisFn>("SDL_GetGamepadAxis"); Axis = (p, a) => axis(p, a);
            var error = F<ErrorFn>("SDL_GetError");
            Error = () => Marshal.PtrToStringUTF8(error()) is { Length: > 0 } text ? text : "SDL controller input failed.";
            var isGamepad = F<IsGamepadFn>("SDL_IsGamepad"); IsGamepad = id => isGamepad(id);
        }

        public IntPtr GetGamepads(out int count) => getGamepads(out count);
    }
}
