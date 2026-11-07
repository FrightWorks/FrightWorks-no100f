using System.Runtime.InteropServices;
using Rectangle = System.Drawing.Rectangle;

namespace FrightWorks;

// The connected monitors for Settings > Display > Fullscreen monitor, with the same ids, labels, order and matching as
// the game (aurora-main/lib/window.cpp query_monitors, find_monitor; monitor_labels.hpp): both read Windows' display config.
// Windows only: the game doesn't pick a fullscreen monitor elsewhere yet (MainWindow.MonitorChoice).
static class Monitors
{
    public sealed record Monitor(string Id, string Label);

    public static List<Monitor> List()
    {
        if (!OperatingSystem.IsWindows()) return new List<Monitor>();
        var targets = Targets();
        var screens = Screens().OrderByDescending(s => s.Primary).Where(s => targets.ContainsKey(s.DeviceName)).ToList();
        var labels = Labels(screens.Select(s => (targets[s.DeviceName].name, s.Bounds, s.Primary)).ToList());
        return screens.Select((s, i) => new Monitor(targets[s.DeviceName].id, labels[i])).ToList();
    }

    // The desktop's monitors in Windows' order (what WinForms' Screen.AllScreens gave): device name, bounds, main.
    static List<(string DeviceName, Rectangle Bounds, bool Primary)> Screens()
    {
        var list = new List<(string, Rectangle, bool)>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (monitor, _, _, _) =>
        {
            var info = new MonitorInfoEx { Size = Marshal.SizeOf<MonitorInfoEx>() };
            if (GetMonitorInfo(monitor, ref info))
                list.Add((info.Device, Rectangle.FromLTRB(info.Monitor.Left, info.Monitor.Top, info.Monitor.Right, info.Monitor.Bottom),
                          (info.Flags & 1) != 0 /* MONITORINFOF_PRIMARY */));
            return true;
        }, IntPtr.Zero);
        return list;
    }

    // "VS248 (1920x1080, above right)": name, size and place relative to the main monitor. Monitors whose labels would
    // match get a number, left to right, then top to bottom: "VS248 (1920x1080, right 2)". Same text as the game; both
    // are checked against the same cases (tools/launcher-layout-tests, runtime/tests/monitor_labels_tests.cpp).
    public static List<string> Labels(IReadOnlyList<(string name, Rectangle bounds, bool primary)> monitors)
    {
        var main = monitors.FirstOrDefault(m => m.primary).bounds;
        string Place((string name, Rectangle bounds, bool primary) m)
        {
            if (m.primary) return "main";
            string vertical = m.bounds.Bottom <= main.Top ? "above" : m.bounds.Top >= main.Bottom ? "below" : "";
            string horizontal = m.bounds.Right <= main.Left ? "left" : m.bounds.Left >= main.Right ? "right" : "";
            return (vertical + " " + horizontal).Trim();
        }
        var labels = new string[monitors.Count];
        var clashes = Enumerable.Range(0, monitors.Count).GroupBy(i =>
            (monitors[i].name, monitors[i].bounds.Width, monitors[i].bounds.Height, place: Place(monitors[i])));
        foreach (var clash in clashes)
        {
            int number = 0;
            foreach (int i in clash.OrderBy(i => monitors[i].bounds.Left).ThenBy(i => monitors[i].bounds.Top))
            {
                number++;
                string tail = (clash.Key.place + (clash.Count() > 1 ? " " + number : "")).Trim();
                labels[i] = $"{clash.Key.name} ({clash.Key.Width}x{clash.Key.Height}{(tail.Length > 0 ? ", " + tail : "")})";
            }
        }
        return labels.ToList();
    }

    // What a saved id means now: that monitor, else the only connected one of its model (plugged into another port).
    public static Monitor? Find(List<Monitor> monitors, string id)
    {
        var exact = monitors.FirstOrDefault(m => m.Id == id);
        if (exact != null) return exact;
        int modelEnd = id.IndexOf('/');
        if (modelEnd < 0) return null;
        var sameModel = monitors.Where(m => m.Id.StartsWith(id[..(modelEnd + 1)], StringComparison.Ordinal)).ToList();
        return sameModel.Count == 1 ? sameModel[0] : null;
    }

    // Desktop device name (\\.\DISPLAY1) -> monitor id and name. Mirrored monitors share one desktop: the first stands for them.
    static Dictionary<string, (string id, string name)> Targets()
    {
        var targets = new Dictionary<string, (string id, string name)>();
        DisplayPath[] paths;
        uint pathCount, modeCount;
        int result;
        do
        {
            result = GetDisplayConfigBufferSizes(QdcOnlyActivePaths, out pathCount, out modeCount);
            if (result != 0) return targets;
            paths = new DisplayPath[pathCount];
            result = QueryDisplayConfig(QdcOnlyActivePaths, ref pathCount, paths, ref modeCount, new DisplayMode[modeCount], IntPtr.Zero);
        } while (result == ErrorInsufficientBuffer);
        if (result != 0) return targets;

        for (uint p = 0; p < pathCount; p++)
        {
            var source = new SourceName { Header = Header(1, Marshal.SizeOf<SourceName>(), paths[p].Source.AdapterId, paths[p].Source.Id) };
            if (GetSourceName(ref source) != 0 || targets.ContainsKey(source.GdiName)) continue;
            var target = new TargetName { Header = Header(2, Marshal.SizeOf<TargetName>(), paths[p].Target.AdapterId, paths[p].Target.Id) };
            if (GetTargetName(ref target) != 0) continue;
            string id = IdFromPath(target.DevicePath);
            if (id.Length > 0) targets[source.GdiName] = (id, target.FriendlyName.Length > 0 ? target.FriendlyName : "Monitor");
        }
        return targets;
    }

    // "\\?\DISPLAY#ACI2498#5&2d9ad897&0&UID4353#{guid}" -> "ACI2498/5&2d9ad897&0&UID4353": the model, then the instance
    // (tells identical monitors apart). No '#': GameConfig reads one as a comment.
    static string IdFromPath(string path)
    {
        var parts = path.Split('#');
        return parts.Length >= 4 ? parts[1] + "/" + parts[2] : "";
    }

    static DeviceInfoHeader Header(uint type, int size, Luid adapter, uint id) =>
        new() { Type = type, Size = (uint)size, AdapterId = adapter, Id = id };

    const uint QdcOnlyActivePaths = 2;
    const int ErrorInsufficientBuffer = 122;

    [StructLayout(LayoutKind.Sequential)] struct Luid { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct PathSource { public Luid AdapterId; public uint Id; public uint ModeIndex; public uint Status; }
    [StructLayout(LayoutKind.Sequential)]
    struct PathTarget
    {
        public Luid AdapterId;
        public uint Id, ModeIndex, Technology, Rotation, Scaling, RefreshNumerator, RefreshDenominator, ScanLine;
        public int Available;
        public uint Status;
    }
    [StructLayout(LayoutKind.Sequential)] struct DisplayPath { public PathSource Source; public PathTarget Target; public uint Flags; }
    [StructLayout(LayoutKind.Sequential, Size = 64)] struct DisplayMode { public uint Unused; }
    [StructLayout(LayoutKind.Sequential)] struct DeviceInfoHeader { public uint Type; public uint Size; public Luid AdapterId; public uint Id; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct SourceName
    {
        public DeviceInfoHeader Header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string GdiName;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct TargetName
    {
        public DeviceInfoHeader Header;
        public uint Flags, Technology;
        public ushort ManufacturerId, ProductId;
        public uint ConnectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string FriendlyName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DevicePath;
    }

    [StructLayout(LayoutKind.Sequential)] struct Rect32 { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct MonitorInfoEx
    {
        public int Size;
        public Rect32 Monitor, Work;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
    }
    delegate bool MonitorEnumProc(IntPtr monitor, IntPtr dc, IntPtr rect, IntPtr data);

    [DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorEnumProc callback, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetMonitorInfoW")] static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfoEx info);
    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint pathCount, out uint modeCount);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint pathCount, [Out] DisplayPath[] paths, ref uint modeCount, [Out] DisplayMode[] modes, IntPtr topology);
    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")] static extern int GetSourceName(ref SourceName info);
    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")] static extern int GetTargetName(ref TargetName info);
}
