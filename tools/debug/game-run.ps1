# Shared by the debug kit's scripts (dot-source it: . "$PSScriptRoot\game-run.ps1", adjusting the path).
#
# Several checkouts (git worktrees, see worktree.ps1) can run side by side, each with its own Play\. These helpers keep
# a script to its own checkout, and make game runs share the machine through the test kit's run slots.
#
#   Get-CheckoutGames <root>    game processes started from inside <root> (its Play\, autotest\ copies, ...)
#   Stop-CheckoutGames <root>   kills those, nothing else: other worktrees' games and the user's own game live on
#   Enter-GameSlot <root> <why> [-Shared] [-Persist]   waits for a run slot (tools/testkit/slots.py) and holds it for <root>
#   Exit-GameSlot <root> [-Persisted]   gives it back: what this script run took; -Persisted also what an earlier script run took with -Persist
#
# Slots: exclusive by default (the whole machine: window captures, timing and audio measurements, the human's own check);
# -Shared takes one of the shared slots (a run that needs neither the screen nor exact timing). The slot is held by a small
# Python process (slots.py hold) that this script keeps on a pipe: when the script ends or dies, the pipe closes and the
# slot is given back. -Persist is for manual-session, whose prepare/start and stop/restore are separate script runs: a
# background holder keeps the slot until Exit-GameSlot (stop/restore) or for at most 4 hours. A script that runs while its
# own checkout already holds a slot (iterate -Capture calling capture-window.ps1, texhunt\look.ps1 calling manual-session)
# reuses it and gives nothing back. Builds never take a slot. The user's own game (launcher, or Play\ by hand) never takes
# one and is never waited for. Must parse in Windows PowerShell 5.1.

. (Join-Path $PSScriptRoot '..\invoke-python.ps1')  # Find-Python

function Get-CheckoutGames([string]$Root) {
    $prefix = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\') + '\'
    Get-CimInstance Win32_Process -Filter "Name = 'FrightWorks Game.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) }
}

function Stop-CheckoutGames([string]$Root) {
    foreach ($p in @(Get-CheckoutGames $Root)) {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

$script:SlotHeld = @{}  # root -> the slots.py process (this script's own slot), 'persist' or 'reused'
$script:SlotDepth = @{}  # root -> how many Enter-GameSlot calls of this script run are open
$script:SlotShared = @{}  # root -> true if that slot is a shared one
$script:SlotPython = $null
$script:SlotsPy = Join-Path $PSScriptRoot '..\testkit\slots.py'

function Get-SlotsPython {
    if (-not $script:SlotPython) {
        $script:SlotPython = Find-Python
        if (-not $script:SlotPython) { throw 'Python 3.8+ is required for the game-run slots and was not found on PATH (python, py or python3).' }
    }
    $script:SlotPython
}

# One argument of a command line (Windows rules: quotes around a text with a space or quote, backslashes before a quote doubled).
function ConvertTo-ProcessArgument([string]$Text) {
    if ($Text -eq '' -or $Text -match '[\s"]') { return '"' + ($Text -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' }
    return $Text
}

function Enter-GameSlot([string]$Root, [string]$Why, [switch]$Shared, [switch]$Persist, [int]$WaitMinutes = 60) {
    $Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    if ($script:SlotHeld.ContainsKey($Root)) {  # this script run has it already
        if ($script:SlotShared[$Root] -and -not $Shared) { throw "game slot: $Root holds a shared slot in this script; it cannot be made exclusive" }
        $script:SlotDepth[$Root]++
        return
    }
    $script:SlotDepth[$Root] = 1
    $script:SlotShared[$Root] = [bool]$Shared
    $python = Get-SlotsPython
    $slotsPy = (Resolve-Path -LiteralPath $script:SlotsPy).Path
    $pyArgs = @($python.Pre) + @('-I', '-X', 'utf8', $slotsPy, 'hold', '--why', $Why, '--root', $Root, '--wait-minutes', "$WaitMinutes")
    if ($Shared) { $pyArgs += '--shared' }
    if ($Persist) {
        $pyArgs += '--detach'
        $lines = @(& $python.Path @pyArgs)
        $code = $LASTEXITCODE
        $last = if ($lines.Count) { "$($lines[-1])" } else { '' }
        foreach ($l in $lines) { if (-not "$l".StartsWith('HELD') -and -not "$l".StartsWith('REUSED')) { Write-Host "$l" } }
        if ($code -ne 0 -or -not $last -or $last.StartsWith('FAILED')) { throw "game slot: $last" }
        $script:SlotHeld[$Root] = if ($last.StartsWith('REUSED')) { 'reused' } else { 'persist' }
        return
    }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $python.Path
    $info.Arguments = (($pyArgs | ForEach-Object { ConvertTo-ProcessArgument "$_" }) -join ' ')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.StandardOutputEncoding = New-Object Text.UTF8Encoding($false)
    $proc = [Diagnostics.Process]::Start($info)
    while ($true) {
        $line = $proc.StandardOutput.ReadLine()
        if ($null -eq $line -or $line.StartsWith('FAILED')) {
            try { $proc.Kill() } catch { }
            throw "game slot: $(if ($line) { $line } else { 'the slot holder ended before it had a slot' })"
        }
        if ($line.StartsWith('HELD')) { $script:SlotHeld[$Root] = $proc; return }
        if ($line.StartsWith('REUSED')) {
            $proc.StandardInput.Close()
            $script:SlotHeld[$Root] = 'reused'
            return
        }
        Write-Host $line
    }
}

function Exit-GameSlot([string]$Root, [switch]$Persisted) {
    $Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    if ($script:SlotDepth.ContainsKey($Root) -and $script:SlotDepth[$Root] -gt 1) { $script:SlotDepth[$Root]--; return }
    $script:SlotDepth.Remove($Root)
    $held = $script:SlotHeld[$Root]
    $script:SlotHeld.Remove($Root)
    if ($held -is [Diagnostics.Process]) {
        try { $held.StandardInput.WriteLine('release'); $held.StandardInput.Close() } catch { }
        if (-not $held.WaitForExit(15000)) { try { $held.Kill() } catch { } }
    } elseif ($held -eq 'reused') {
        return  # somebody else's slot
    } elseif ($held -eq 'persist' -or $Persisted) {
        # a background holder (-Persist), here or from an earlier script run (manual-session stop/restore): this checkout's, if any
        $python = Get-SlotsPython
        $pre = @($python.Pre)
        & $python.Path @pre -I -X utf8 (Resolve-Path -LiteralPath $script:SlotsPy).Path release --root $Root
    }
}
