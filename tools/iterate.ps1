# One porting iteration: (optionally) rebuild, boot the game for -Seconds, and print what stopped it.
#   powershell -ExecutionPolicy Bypass -File tools\iterate.ps1 [-NoBuild] [-SkipTranslate] [-Seconds 25] [-Capture]
# Also refreshes the AGENTS.md heartbeat so a scheduled resume knows a session is active.
# Rumble is off for the run; Play\UserData\Config.toml comes back byte for byte.
param([switch]$NoBuild, [switch]$SkipTranslate, [int]$Seconds = 25, [switch]$Capture)
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'debug\kit-common.ps1')  # rumble off for the run
$agents = Join-Path $root 'AGENTS.md'
(Get-Content $agents -Raw) -replace 'Heartbeat: .*', ("Heartbeat: {0:yyyy-MM-dd HH:mm} (main session active)" -f (Get-Date)) | Set-Content $agents -NoNewline
if (-not $NoBuild) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'build-no100f.ps1'), '-SkipTools')
    if ($SkipTranslate) { $a += '-SkipTranslate' }
    & powershell @a *> (Join-Path $root 'build-logs\iterate-build.log')
    if ($LASTEXITCODE -ne 0) {
        "BUILD FAILED"; Get-Content (Join-Path $root 'build-logs\status.txt') -Tail 3
        Select-String -Path (Join-Path $root 'build-logs\*.log') -Pattern 'error' | Select -First 12 | % { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
        exit 1
    }
}
$play = Join-Path $root 'Play'; $log = Join-Path $root 'build-logs\boot.log'
$cfg = Join-Path $play 'UserData\Config.toml'
$userCfg = Set-RumbleOffForRun $cfg
$p = $null
try {
    $p = Start-Process (Join-Path $play 'FrightWorks Game.exe') -WorkingDirectory $play -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru
    $null = $p.WaitForExit($Seconds * 1000)
    $alive = -not $p.HasExited
    if ($alive -and $Capture) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'capture-window.ps1') -Seconds 1 -Out (Join-Path $root 'build-logs\boot.png') | Select -Last 1 }
} finally {
    if ($p) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep 1 }
    Restore-ConfigBytes $cfg $userCfg
}
"alive after $Seconds s: $alive"
$lines = Get-Content $log, "$log.out" -ErrorAction SilentlyContinue
"log lines: $($lines.Count)"
$lines | Select-String 'Reason :|Address:|Entry  :|not translated|not a registered|FATAL|panic|Unhandled|HINT|MMIO|\[vi\] fps|\[mode\]|\[disc\]|exception|Fatal' |
    Select -First 25 | % { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
"--- last lines:"; $lines | Select -Last 6 | % { $_.Substring(0, [Math]::Min(200, $_.Length)) }
