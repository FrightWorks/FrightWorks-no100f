# perf-cutout.ps1 -Label <name> [-Boot h001] [-Spot ''] [-Hd | -Pack <full path>] [-Interp 0|120|180] [-Modes 0,3] [-Rounds 3]:
# needs a Play\ build with the cut-out modes + the dev menu "perf" command. Opens the game (look.ps1), then for each
# round and mode: "cutout N", 6 s settle, "perf" (draw calls + present timing) → autotest\texhunt\perf\<label>.txt.
param([Parameter(Mandatory)][string]$Label, [string]$Boot = 'h001', [string]$Spot = '', [switch]$Hd, [string]$Pack = '',
      [int]$Interp = 0, [string]$Modes = '0,3', [int]$Rounds = 3)
$ErrorActionPreference = 'Stop'
$env:NO100F_DEV_TOOLS = '1'  # dev menu (F8) on for this session's game (built-in dev menu; older builds use the patch)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path   # repo root
. (Join-Path $PSScriptRoot '..\game-run.ps1')  # Get-/Stop-CheckoutGames: only this checkout's game
Set-Location $root
$cmd = Join-Path $root 'Play\devmenu.txt'
$log = Join-Path $root 'Play\devmenu.log'
$outDir = Join-Path $root 'autotest\texhunt\perf'
New-Item -ItemType Directory -Force $outDir | Out-Null
$res = Join-Path $outDir "$Label.txt"
$out = Join-Path $env:TEMP "perf-look-$PID.txt"
$modeList = $Modes -split ',' | ForEach-Object { [int]$_ }

function Send([string]$line, [string]$expect) {
    Remove-Item $log -ErrorAction SilentlyContinue
    Set-Content $cmd $line
    $t = Get-Date
    while (-not (Test-Path $log) -and ((Get-Date) - $t).TotalSeconds -lt 15) { Start-Sleep -Milliseconds 200 }
    $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
    if ($text -notmatch $expect) { throw "'$line' not answered: $text" }
    $text.Trim()
}

$lookArgs = @('-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $root 'tools\debug\texhunt\look.ps1')`"", '-Boot', $Boot)
if ($Spot) { $lookArgs += @('-Spot', $Spot) }
if ($Pack) { $lookArgs += @('-Pack', "`"$Pack`"") } elseif ($Hd) { $lookArgs += '-Hd' }
if ($Interp) { $lookArgs += @('-Interp', $Interp) }
$look = Start-Process powershell -ArgumentList $lookArgs -PassThru -NoNewWindow -RedirectStandardOutput $out
try {
    $t = Get-Date
    while (-not ((Test-Path $out) -and (Select-String $out -Pattern 'game open' -Quiet))) {
        if ($look.HasExited -or ((Get-Date) - $t).TotalSeconds -gt 180) { throw "look.ps1 did not open the game: $(Get-Content $out -Raw)" }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep 5
    "# $Label boot $Boot spot '$Spot' hd $([bool]($Hd -or $Pack)) interp $Interp" | Set-Content $res
    for ($r = 1; $r -le $Rounds; ++$r) {
        foreach ($m in $modeList) {
            Send "cutout $m" "cutout mode $m" | Out-Null
            Start-Sleep 6
            $line = "round $r mode $m  " + ((Send 'perf' 'perf draws') -replace '^perf -> ', '')
            $line; $line | Add-Content $res
        }
    }
} finally {
    Get-CheckoutGames $root | ForEach-Object { (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue).CloseMainWindow() | Out-Null }
    if (-not $look.WaitForExit(30000)) { Stop-CheckoutGames $root; $look.WaitForExit(15000) | Out-Null }
    Get-Content $out | Select-String 'restored' | ForEach-Object Line
    Remove-Item $out -ErrorAction SilentlyContinue
}
