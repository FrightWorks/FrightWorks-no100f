# Starts Play\FrightWorks Game.exe, waits, saves a PNG of the game window only (PrintWindow, no desktop input), stops it.
# Usage: capture-window.ps1 [-Seconds 20] [-Out <png>] [-Every N]   (-Every: a shot every N s as <out>-<s>s.png)
# Rumble is off for the run; Play\UserData\Config.toml comes back byte for byte.
param([int]$Seconds = 20, [string]$Out = '', [int]$Every = 0)
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'debug\kit-common.ps1')  # rumble off for the run
if (-not $Out) { $Out = Join-Path $root 'autotest\window.png' }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  // The biggest visible top-level window of a process (MainWindowHandle can be 0 for large windows).
  public static IntPtr FindFor(uint id) {
    IntPtr best = IntPtr.Zero; long bestArea = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); RECT r;
      if (p == id && IsWindowVisible(h) && GetClientRect(h, out r) && (long)r.R * r.B > bestArea) { bestArea = (long)r.R * r.B; best = h; }
      return true; }, IntPtr.Zero);
    return best;
  }
}
"@
[Win]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force (Join-Path $root 'autotest') | Out-Null
function Shot([string]$file) {
    $p.Refresh()
    $h = [Win]::FindFor([uint32]$p.Id)
    $r = New-Object Win+RECT
    [Win]::GetClientRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R)), ([Math]::Max(1, $r.B))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [Win]::PrintWindow($h, $dc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "saved $file ($($r.R)x$($r.B))"
}
$cfg = Join-Path $root 'Play\UserData\Config.toml'
$userCfg = Set-RumbleOffForRun $cfg
$p = $null
try {
    $p = Start-Process (Join-Path $root 'Play\FrightWorks Game.exe') -WorkingDirectory (Join-Path $root 'Play') -PassThru `
        -RedirectStandardOutput "$Out.log" -RedirectStandardError "$Out.err.log"
    if ($Every -gt 0) {
        for ($t = $Every; $t -le $Seconds -and -not $p.HasExited; $t += $Every) {
            Start-Sleep -Seconds $Every
            Shot ([IO.Path]::ChangeExtension($Out, $null).TrimEnd('.') + "-$($t)s.png")
        }
    } else {
        Start-Sleep -Seconds $Seconds
        Shot $Out
    }
} finally {
    if ($p -and -not $p.HasExited) { $p.Kill(); $p.WaitForExit(5000) | Out-Null }
    Restore-ConfigBytes $cfg $userCfg
}
