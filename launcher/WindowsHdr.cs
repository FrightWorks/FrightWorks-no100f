using System.Runtime.InteropServices;

namespace ScoobyReMasked;

// Read-only display queries. Advanced-color capability alone does not mean Windows HDR is enabled.
static class WindowsHdr
{
    internal sealed record Snapshot(bool? Enabled, float? PaperWhite, float? Peak, string Status);

    public static Snapshot Read(string? deviceName)
    {
        if (!OperatingSystem.IsWindows())
            return new(null, null, null, "Windows HDR defaults are unavailable on this platform. Saved targets are kept.");
        if (string.IsNullOrEmpty(deviceName))
            return new(null, null, null, "The game's Windows display could not be identified; using safe HDR defaults.");
        const uint activePaths = 2;
        int error = 0;
        for (int attempt = 0; attempt < 3; attempt++)
        {
            error = GetDisplayConfigBufferSizes(activePaths, out uint pathCount, out uint modeCount);
            if (error != 0) break;
            var paths = new DisplayPath[pathCount];
            var modes = new DisplayMode[modeCount];
            error = QueryDisplayConfig(activePaths, ref pathCount, paths, ref modeCount, modes, IntPtr.Zero);
            if (error == 122) continue; // The topology changed between the two calls.
            if (error != 0) break;
            var targets = new List<DisplayPath>();
            foreach (var path in paths.Take((int)pathCount))
            {
                var name = new SourceName { Header = Header(1, Marshal.SizeOf<SourceName>(), path.Source.Adapter, path.Source.Id) };
                int nameError = DisplayConfigGetDeviceInfo(ref name);
                if (nameError != 0)
                    return new(null, null, null, $"Windows display name query failed ({nameError}); using safe HDR defaults.");
                if (name.Name == deviceName) targets.Add(path);
            }
            // Mirrored outputs may have different HDR state/calibration despite sharing a GDI source.
            if (targets.Count != 1)
                return new(null, null, null, $"Windows HDR defaults unavailable for {deviceName} " +
                    $"({targets.Count} matching outputs); using safe defaults.");
            var target = targets[0].Target;
            var color2 = new AdvancedColor2 { Header = Header(15, Marshal.SizeOf<AdvancedColor2>(), target.Adapter, target.Id) };
            error = DisplayConfigGetDeviceInfo(ref color2);
            (float? Peak, bool? Hdr, string Status)? output = null;
            bool? enabled;
            if (error == 0)
                enabled = HdrActive(color2.Flags, color2.ActiveMode);
            else if (error is 87 or 50) // Older Windows does not support GET_ADVANCED_COLOR_INFO_2.
            {
                var color = new AdvancedColor { Header = Header(9, Marshal.SizeOf<AdvancedColor>(), target.Adapter, target.Id) };
                error = DisplayConfigGetDeviceInfo(ref color);
                if (error != 0) break;
                if ((color.Flags & 2) == 0) enabled = false;
                else
                {
                    // Legacy advancedColorEnabled also includes SDR Auto Color Management.
                    output = ReadOutput(deviceName);
                    enabled = output.Value.Hdr;
                }
            }
            else break;
            if (enabled == null)
                return new(null, null, null, $"Windows HDR state unavailable ({output?.Status}); using safe defaults.");
            if (enabled == false)
                return new(false, null, null, $"Windows HDR is off on {deviceName}; using standard luminance defaults.");

            var white = new SdrWhite { Header = Header(11, Marshal.SizeOf<SdrWhite>(), target.Adapter, target.Id) };
            int whiteError = DisplayConfigGetDeviceInfo(ref white);
            // DISPLAYCONFIG_SDR_WHITE_LEVEL uses 1000 = 80 cd/m2, not a peak-brightness measurement.
            float? paper = whiteError == 0 ? white.Level * 80f / 1000f : null;
            var (peak, dxgiHdr, peakStatus) = output ?? ReadOutput(deviceName);
            if (dxgiHdr != true) peak = null;
            bool validPaper = HdrSettings.ValidNits(paper, HdrSettings.PaperMin, HdrSettings.PaperMax) != null;
            bool validPeak = HdrSettings.ValidNits(peak, HdrSettings.PeakMin, HdrSettings.PeakMax) != null;
            string status = $"Initial HDR defaults from Windows on {deviceName}. " +
                (validPaper ? "Paper white uses Windows SDR content brightness. " :
                    $"SDR white unavailable/out of range ({whiteError}); keeping 200 nits. ") +
                (validPeak ? "Peak uses display-reported maximum luminance." :
                    $"Peak unavailable/out of range ({peakStatus}); keeping 1000 nits.");
            return new(true, validPaper ? paper : null, validPeak ? peak : null, status);
        }
        return new(null, null, null, $"Windows HDR query failed ({error}); using safe HDR defaults.");
    }

    static DeviceHeader Header(uint type, int size, Luid adapter, uint id) =>
        new() { Type = type, Size = (uint)size, Adapter = adapter, Id = id };

    internal static bool HdrActive(uint flags, uint activeMode) => (flags & 2) != 0 && activeMode == 2;

    // DXGI interfaces are queried by IID before using their documented COM vtable slots.
    // Keep ownership local: even an enumeration failure must release every acquired interface.
    static unsafe (float? Peak, bool? Hdr, string Status) ReadOutput(string deviceName)
    {
        var factoryId = new Guid("770aae78-f26f-4dba-a829-253c83d1b387"); // IDXGIFactory1
        var output6Id = new Guid("068346e8-aaec-4b84-add7-137f513f77a1"); // IDXGIOutput6
        int hr = CreateDXGIFactory1(ref factoryId, out IntPtr factory);
        if (hr < 0) return (null, null, $"DXGI factory 0x{hr:X8}");
        try
        {
            var enumerateAdapters = (delegate* unmanaged[Stdcall]<IntPtr, uint, IntPtr*, int>)Slot(factory, 12);
            for (uint a = 0; ; a++)
            {
                IntPtr adapter;
                hr = enumerateAdapters(factory, a, &adapter);
                if (hr == unchecked((int)0x887A0002)) break; // DXGI_ERROR_NOT_FOUND
                if (hr < 0) return (null, null, $"DXGI adapter 0x{hr:X8}");
                try
                {
                    var enumerateOutputs = (delegate* unmanaged[Stdcall]<IntPtr, uint, IntPtr*, int>)Slot(adapter, 7);
                    for (uint o = 0; ; o++)
                    {
                        IntPtr output;
                        hr = enumerateOutputs(adapter, o, &output);
                        if (hr == unchecked((int)0x887A0002)) break;
                        if (hr < 0) return (null, null, $"DXGI output 0x{hr:X8}");
                        try
                        {
                            var getDesc = (delegate* unmanaged[Stdcall]<IntPtr, OutputDesc*, int>)Slot(output, 7);
                            OutputDesc desc;
                            hr = getDesc(output, &desc);
                            if (hr < 0) return (null, null, $"DXGI description 0x{hr:X8}");
                            if (new string(desc.DeviceName) != deviceName) continue;
                            hr = Marshal.QueryInterface(output, in output6Id, out IntPtr output6);
                            if (hr < 0) return (null, null, $"IDXGIOutput6 0x{hr:X8}");
                            try
                            {
                                var getDesc1 = (delegate* unmanaged[Stdcall]<IntPtr, OutputDesc1*, int>)Slot(output6, 27);
                                OutputDesc1 desc1;
                                hr = getDesc1(output6, &desc1);
                                return hr < 0 ? (null, null, $"DXGI luminance 0x{hr:X8}") :
                                    (desc1.MaxLuminance, desc1.ColorSpace == 12, $"DXGI color space {desc1.ColorSpace}");
                            }
                            finally { Marshal.Release(output6); }
                        }
                        finally { Marshal.Release(output); }
                    }
                }
                finally { Marshal.Release(adapter); }
            }
            return (null, null, "no matching DXGI output");
        }
        finally { Marshal.Release(factory); }
    }

    static unsafe void* Slot(IntPtr instance, int index) => (*(void***)instance)[index];

    [StructLayout(LayoutKind.Sequential)] struct Luid { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct DeviceHeader { public uint Type, Size; public Luid Adapter; public uint Id; }
    [StructLayout(LayoutKind.Sequential)] struct PathSource { public Luid Adapter; public uint Id, Mode, Flags; }
    [StructLayout(LayoutKind.Sequential)] struct PathTarget
    {
        public Luid Adapter;
        public uint Id, Mode, Technology, Rotation, Scaling, RefreshNumerator, RefreshDenominator, Scanline, Available, Flags;
    }
    [StructLayout(LayoutKind.Sequential)] struct DisplayPath { public PathSource Source; public PathTarget Target; public uint Flags; }
    [StructLayout(LayoutKind.Sequential, Size = 64)] struct DisplayMode { }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct SourceName
    {
        public DeviceHeader Header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
    }
    [StructLayout(LayoutKind.Sequential)] struct AdvancedColor { public DeviceHeader Header; public uint Flags, Encoding, BitsPerChannel; }
    [StructLayout(LayoutKind.Sequential)] struct AdvancedColor2 { public DeviceHeader Header; public uint Flags, Encoding, BitsPerChannel, ActiveMode; }
    [StructLayout(LayoutKind.Sequential)] struct SdrWhite { public DeviceHeader Header; public uint Level; }
    [StructLayout(LayoutKind.Sequential)] unsafe struct OutputDesc
    {
        public fixed char DeviceName[32];
        public int Left, Top, Right, Bottom, Attached, Rotation;
        public IntPtr Monitor;
    }
    [StructLayout(LayoutKind.Sequential)] unsafe struct OutputDesc1
    {
        public OutputDesc Base;
        public uint BitsPerColor, ColorSpace;
        public fixed float Chromaticities[8];
        public float MinLuminance, MaxLuminance, MaxFullFrameLuminance;
    }

    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint pathCount,
        [Out] DisplayPath[] paths, ref uint modeCount, [Out] DisplayMode[] modes, IntPtr topology);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref SourceName request);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref AdvancedColor request);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref AdvancedColor2 request);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref SdrWhite request);
    [DllImport("dxgi.dll")] static extern int CreateDXGIFactory1(ref Guid iid, out IntPtr factory);
}
