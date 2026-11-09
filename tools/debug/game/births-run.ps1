# Particle births per emitter at fps60 off/on per level (needs tools\debug\patches\postool.patch: postool "births").
# births-run.ps1 [-Levels i004,i001] [-Seconds 10]   (powershell -File passes "a,b" as one string: split here)  -> autotest\manual\births\<level>-fps60<bool>.txt
# Runs with the test config (manual-session prepare); at the end manual-session restore puts your settings back.
param([string[]]$Levels = @('i004', 'i001', 'h001', 'e001', 'h002'), [int]$Seconds = 10)
$Levels = $Levels | ForEach-Object { $_ -split ',' } | Where-Object { $_ -match '^[a-z]\d{3}$' }
if (-not $Levels) { throw 'no valid level names (expected like i004)' }
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
. (Join-Path $PSScriptRoot '..\game-run.ps1')  # Stop-CheckoutGames: only this checkout's game
Set-Location $root
$out = Join-Path $root 'autotest\manual\births'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $root 'Play\FrightWorks Game.exe'
$cfg = Join-Path $root 'Play\UserData\Config.toml'
function Stop-Game { Stop-CheckoutGames $root; Start-Sleep 1 }
try {
  foreach ($lvl in $Levels) {
    Stop-Game
    powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 prepare -Boot $lvl -AllPowerups | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'manual-session prepare failed' }
    foreach ($fps in @('false', 'true')) {
      (Get-Content $cfg) -replace '^fps60 = .*', "fps60 = $fps" | Set-Content $cfg
      Remove-Item -Recurse Play\postool, Play\postool.txt -ErrorAction SilentlyContinue
      $t0 = Get-Date
      Start-Process $exe -WorkingDirectory (Join-Path $root 'Play')
      $deadline = (Get-Date).AddSeconds(90); $hit = $false
      while (-not $hit -and (Get-Date) -lt $deadline) {
        Start-Sleep 2
        $log = Get-ChildItem Play\UserData\Logs\*\console.log | Where-Object LastWriteTime -gt $t0 | Sort-Object LastWriteTime | Select-Object -Last 1
        $hit = $log -and (Select-String $log.FullName -Pattern "$lvl\.HIP" -Quiet)
        if ($log -and (Get-ChildItem (Split-Path $log.FullName) -Filter 'crash_*' -ErrorAction SilentlyContinue)) { throw "$lvl fps60=$fps : game crashed, see $(Split-Path $log.FullName)" }
      }
      if (-not $hit) { "$lvl fps60=$fps : level never loaded"; Stop-Game; continue }
      Start-Sleep 15
      Set-Content Play\postool.txt "births $Seconds"
      $deadline = (Get-Date).AddSeconds($Seconds + 20)
      while (-not (Test-Path Play\postool\births.txt) -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
      Start-Sleep 1
      if (Test-Path Play\postool\births.txt) { Copy-Item Play\postool\births.txt "$out\$lvl-fps60$fps.txt" -Force; "$lvl fps60=$fps : ok" }
      else { "$lvl fps60=$fps : no births.txt" }
      # extra probe output from overrides, if any (e.g. streaks.txt)
      Get-ChildItem Play\postool\*.txt -Exclude births.txt, done.txt -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName "$out\$lvl-fps60$fps-$($_.Name)" -Force }
      powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 shot -Label "births-$lvl-fps60$fps" | Out-Null
      Stop-Game
    }
  }
} finally {
  Stop-Game
  powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 restore | Out-Null
  Remove-Item -Recurse Play\postool, Play\postool.txt -ErrorAction SilentlyContinue
  'restored user setup'
}
