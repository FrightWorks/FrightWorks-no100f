# tools\debug\holiday-shots.ps1 -Cases "h001:off,h002:12-25" [-Wait 45]
# Needs a build with PR #13 ([game] holiday). -Cases is ONE comma-separated string (powershell -File joins arrays).
# For each level:holiday case: manual-session prepare writes the test Config + boot mod, then [game] holiday is appended,
# the game is launched directly, and one shot is saved as autotest\manual\shot-*-hol-<level>-<date>.png.
# manual-session stop after each case puts your settings back and removes the zz-manual boot mod.
# The human may need to skip a cutscene in a window; a ~0.5 MB shot means it caught the intro - rerun that case.
param([string]$Cases, [int]$Wait = 45)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repo
$ms = Join-Path $repo 'tools\debug\manual-session.ps1'
$cfg = Join-Path $repo 'Play\UserData\Config.toml'
try {
    foreach ($c in ($Cases -split ",")) {
        $level, $date = $c -split ':'
        Remove-Item Env:NO100F_*, Env:AX_*, Env:AUDIO_* -ErrorAction SilentlyContinue
        & powershell -ExecutionPolicy Bypass -File $ms prepare -Boot $level -AllPowerups | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'manual-session prepare failed (game running?)' }
        Add-Content -LiteralPath $cfg -Value "`n[game]`nholiday = `"$date`"" -Encoding utf8
        $log = Join-Path $repo "autotest\manual\hol-$level-$date.err.log"
        Start-Process (Join-Path $repo 'Play\FrightWorks-No100F-Game.exe') -WorkingDirectory (Join-Path $repo 'Play') `
            -RedirectStandardError $log -RedirectStandardOutput ($log -replace 'err', 'out')
        Start-Sleep -Seconds $Wait
        & powershell -ExecutionPolicy Bypass -File $ms shot -Label "hol-$level-$date" | Select-Object -Last 1
        & powershell -ExecutionPolicy Bypass -File $ms stop | Out-Null
        Remove-Item $log, ($log -replace 'err', 'out') -ErrorAction SilentlyContinue
    }
} finally {
    & powershell -ExecutionPolicy Bypass -File $ms stop | Out-Null  # closes the game, your settings back
}
