# Record the running game's client area to an mp4.
# Records this checkout's game (Play\ or autotest\ copies), never another worktree's or the user's own.
# record.ps1 -Out <file.mp4> [-Seconds 5] [-Method ddagrab|gdigrab] [-Toggle]
#   ddagrab (default): Desktop Duplication of the monitor holding the window, with the window made topmost for the
#     capture so nothing covers it; DPI-aware, works on any monitor. Window capture of the swapchain (gdigrab
#     title=...) is black, so both methods grab a desktop region.
#   gdigrab: the older desktop grab (primary monitor coordinates, lossless qp 10), used for the PR #14 flicker counts.
#   -Toggle creates Play\fogstat.txt for the duration (FOGSTAT/FOGCOV probes).
param([Parameter(Mandatory)][string]$Out, [int]$Seconds = 5, [ValidateSet('ddagrab', 'gdigrab')][string]$Method = 'ddagrab',
      [switch]$Toggle)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $PSScriptRoot '..\game-run.ps1')
$ffBin = & (Join-Path $PSScriptRoot 'get-ffmpeg.ps1')   # PATH, else tools\debug\local\ffmpeg (downloaded once)
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class RecWin {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
}
'@ -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Windows.Forms
[RecWin]::SetProcessDPIAware() | Out-Null
$g = @(Get-CheckoutGames $root | ForEach-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue }) |
    Sort-Object WorkingSet64 -Descending | Select-Object -First 1
if (-not $g) { throw "no game running from $root" }
$h = $g.MainWindowHandle
[RecWin]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
$r = New-Object RecWin+R; [RecWin]::GetClientRect($h, [ref]$r) | Out-Null
$p = New-Object RecWin+P; [RecWin]::ClientToScreen($h, [ref]$p) | Out-Null
$w = $r.Rt - $r.L; $w -= $w % 2; $hgt = $r.B - $r.T; $hgt -= $hgt % 2   # x264 needs even sizes
$ffmpeg = Join-Path $ffBin 'ffmpeg.exe'
$flag = Join-Path $root 'Play\fogstat.txt'
New-Item -ItemType Directory -Force (Split-Path ([IO.Path]::GetFullPath($Out))) | Out-Null
if ($Toggle) { Remove-Item $flag -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 300; New-Item $flag -ItemType File -Force | Out-Null }
try {
    if ($Method -eq 'gdigrab') {
        & $ffmpeg -hide_banner -loglevel error -y -f gdigrab -framerate 60 -offset_x $p.X -offset_y $p.Y `
          -video_size "${w}x${hgt}" -t $Seconds -i desktop -c:v libx264 -preset ultrafast -qp 10 $Out
    } else {
        $screens = [System.Windows.Forms.Screen]::AllScreens; $idx = 0
        for ($i = 0; $i -lt $screens.Count; $i++) { if ($screens[$i].Bounds.Contains($p.X, $p.Y)) { $idx = $i; break } }
        $b = $screens[$idx].Bounds
        [RecWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013) | Out-Null   # HWND_TOPMOST, no move/size/activate
        Start-Sleep -Milliseconds 500
        $src = "ddagrab=output_idx=${idx}:framerate=60:draw_mouse=0:offset_x=$($p.X - $b.X):offset_y=$($p.Y - $b.Y):video_size=${w}x${hgt}"
        & $ffmpeg -hide_banner -loglevel error -y -f lavfi -i $src -t $Seconds -vf 'hwdownload,format=bgra' `
          -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p $Out
    }
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed ($LASTEXITCODE)" }
} finally {
    if ($Method -eq 'ddagrab') { [RecWin]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x0013) | Out-Null }   # HWND_NOTOPMOST
    if ($Toggle) { Remove-Item $flag -ErrorAction SilentlyContinue }
}
"recorded $Out (${w}x${hgt}, $Method)"
