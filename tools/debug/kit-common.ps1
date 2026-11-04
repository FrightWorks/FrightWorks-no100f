# Shared by the debug kit's scripts (dot-source it: . "$PSScriptRoot\kit-common.ps1", adjusting the path).
#
# Several checkouts (git worktrees, see worktree.ps1) can run side by side, each with its own Play\. These helpers keep
# a script to its own checkout, and let only one checkout run the game for a test at a time.
#
#   Get-CheckoutGames <root>    game processes started from inside <root> (its Play\, autotest\ copies, ...)
#   Stop-CheckoutGames <root>   kills those, nothing else: other worktrees' games and the user's own game live on
#   Enter-GameLock <root> <why> waits until no other checkout holds the game-run lock, then takes it for <root>
#   Exit-GameLock <root>        gives it back (only if <root> holds it)
#   Get-GameLock                the current holder, or $null
#   Set-ConfigLine <file> <section> <key> <line>  one setting of a Config.toml, returns the line it replaced
#   Set-RumbleOffForRun <file>  rumble off for a game started on the player's own Config.toml; returns the file's bytes
#   Restore-ConfigBytes <file> <bytes>  puts those bytes back (none: there was no file, so it is deleted)
#
# The lock is one file per Windows user: %LOCALAPPDATA%\FrightWorks-kit\game-run.lock (JSON: root, why, since).
# Builds don't take it; only game runs do (manual-session prepare/start take it, stop/restore give it back; play-patched
# holds it while its game is open). Two games at once fight over the screen (window captures) and the GPU (timing and
# audio measurements). A lock is stale, and taken over, when its checkout has no game running and it is older than
# 3 minutes (a script that died between prepare and launch). The user's own game (launcher, or Play\ by hand) never
# takes the lock and is never waited for.

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

function Get-GameLockPath {
    Join-Path $env:LOCALAPPDATA 'FrightWorks-kit\game-run.lock'
}

function Get-GameLock {
    $path = Get-GameLockPath
    try { return (Get-Content -LiteralPath $path -Raw -ErrorAction Stop | ConvertFrom-Json) } catch { return $null }
}

function Test-GameLockStale($Lock) {
    if (-not $Lock -or -not $Lock.root -or -not (Test-Path -LiteralPath $Lock.root)) { return $true }
    if (@(Get-CheckoutGames $Lock.root).Count -gt 0) { return $false }
    $since = [datetime]::MinValue
    if (-not [datetime]::TryParse([string]$Lock.since, [ref]$since)) { return $true }
    return ((Get-Date) - $since).TotalMinutes -gt 3
}

function Enter-GameLock([string]$Root, [string]$Why, [int]$WaitMinutes = 60) {
    $Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    $path = Get-GameLockPath
    [IO.Directory]::CreateDirectory((Split-Path $path)) | Out-Null
    $deadline = (Get-Date).AddMinutes($WaitMinutes)
    $told = $false
    while ($true) {
        $lock = Get-GameLock
        if ($lock -and [string]::Equals([string]$lock.root, $Root, [StringComparison]::OrdinalIgnoreCase)) {
            $lock.since = (Get-Date).ToString('o'); $lock.why = $Why  # ours already: refresh it
            ($lock | ConvertTo-Json -Compress) | Set-Content -LiteralPath $path -Encoding utf8
            return
        }
        if ($lock -and (Test-GameLockStale $lock)) {
            Write-Host "game lock: taking over a stale lock from $($lock.root) ($($lock.why))"
            Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
            $lock = $null
        }
        if (-not $lock) {
            $json = @{ root = $Root; why = $Why; since = (Get-Date).ToString('o') } | ConvertTo-Json -Compress
            try {
                # CreateNew: of two checkouts asking at the same moment, one wins and the other waits.
                $stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try { $bytes = [Text.Encoding]::UTF8.GetBytes($json); $stream.Write($bytes, 0, $bytes.Length) }
                finally { $stream.Dispose() }
                return
            } catch [IO.IOException] { continue }
        }
        if ((Get-Date) -gt $deadline) {
            throw "game lock: still held by $($lock.root) ($($lock.why), since $($lock.since)) after $WaitMinutes min"
        }
        if (-not $told) {
            Write-Host "game lock: $($lock.root) is running the game ($($lock.why)); waiting (up to $WaitMinutes min)"
            $told = $true
        }
        Start-Sleep -Seconds 5
    }
}

function Exit-GameLock([string]$Root) {
    $Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    $lock = Get-GameLock
    if ($lock -and [string]::Equals([string]$lock.root, $Root, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath (Get-GameLockPath) -Force -ErrorAction SilentlyContinue
    }
}

$script:ConfigLinesAdded = @{}  # what Set-ConfigLine added: section -> the text it put before that section's header; '' = the file

# Sets $Key in [$Section] of the Config.toml at $Path to $Line ($null = remove the line); returns the line it replaced
# ($null if none). A new line goes at the end of its section; a missing section is added at the end of the file (no
# Config.toml yet = start one). Putting the old lines back takes out again what was added: a section added here once it is empty,
# the file once it is empty (the game then writes its defaults on the next start). Calls that put lines back go in the
# reverse order of the calls that set them. Never adds a second table or key: the game's parser would reject the whole
# file and run on its defaults, rumble included.
function Set-ConfigLine([string]$Path, [string]$Section, [string]$Key, $Line) {
    $added = $script:ConfigLinesAdded
    $bom = $false
    $text = ''
    if (Test-Path -LiteralPath $Path) {
        $bytes = [IO.File]::ReadAllBytes($Path)
        $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
        $text = [IO.File]::ReadAllText($Path)
    } elseif ($null -ne $Line) {
        $added[''] = $true
    }
    $nl = if ($text.Contains("`r`n") -or -not $text) { "`r`n" } else { "`n" }
    $old = $null
    $head = [regex]::Match($text, '(?m)^[ \t]*\[[ \t]*' + [regex]::Escape($Section) + '[ \t]*\][ \t]*(?:#[^\r\n]*)?(\r?\n|\z)')
    if ($head.Success) {
        $start = $head.Index + $head.Length
        $next = [regex]::Match($text.Substring($start), '(?m)^[ \t]*\[')
        $end = if ($next.Success) { $start + $next.Index } else { $text.Length }
        $body = $text.Substring($start, $end - $start)
        $m = [regex]::Match($body, '(?m)^[ \t]*' + [regex]::Escape($Key) + '[ \t]*=[^\r\n]*(\r?\n)?')
        if ($m.Success) {
            $old = $m.Value.TrimEnd("`r", "`n")
            $pre = $body.Substring(0, $m.Index)
            if ($null -ne $Line) {
                $pre += $Line + $m.Value.Substring($old.Length)
            } elseif (-not $m.Groups[1].Success) {
                # it was the last line, with no newline: drop the newline before it (the header's, if it was the first)
                if ($pre) { $pre = $pre -replace '\r?\n\z', '' } else { $start -= $head.Groups[1].Length }
            }
            $body = $pre + $body.Substring($m.Index + $m.Length)
        } elseif ($null -ne $Line) {
            $content = $body.TrimEnd("`r", "`n")
            $body = if ($content) { $content + $nl + $Line + $body.Substring($content.Length) }
                    elseif (-not $head.Groups[1].Value) { $nl + $Line }  # the header is the file's last line, no newline
                    else { $Line + $nl + $body }
        }
        if ($null -eq $Line -and -not $body.Trim() -and $added.ContainsKey($Section)) {
            $before = $text.Substring(0, $head.Index)
            $gap = $added[$Section]
            if ($end -eq $text.Length -and $before.EndsWith($gap)) { $before = $before.Substring(0, $before.Length - $gap.Length) }
            $text = $before + $text.Substring($end)
            $added.Remove($Section)
        } else {
            $text = $text.Substring(0, $start) + $body + $text.Substring($end)
        }
    } elseif ($null -ne $Line) {
        $gap = ''
        if ($text -and -not $text.EndsWith("`n")) { $gap += $nl }
        if ($text) { $gap += $nl }
        $text += $gap + "[$Section]" + $nl + $Line + $nl
        $added[$Section] = $gap
    }
    if (-not $text.Trim() -and $added.ContainsKey('')) {
        Remove-Item -LiteralPath $Path -ErrorAction SilentlyContinue
        $added.Remove('')
    } else {
        [IO.Directory]::CreateDirectory((Split-Path $Path)) | Out-Null
        [IO.File]::WriteAllText($Path, $text, (New-Object Text.UTF8Encoding($bom)))
    }
    return $old
}

# Rumble is always off in test runs ([controller] rumble = false). This pair is for the scripts that start the game on the
# player's own Config.toml instead of manual-session's test config. The whole file goes back, not just the line: the game
# rewrites it when its window moves or resizes.
function Set-RumbleOffForRun([string]$Path) {
    $before = $null
    if (Test-Path -LiteralPath $Path) { $before = [IO.File]::ReadAllBytes($Path) }
    Set-ConfigLine $Path 'controller' 'rumble' 'rumble = false' | Out-Null
    return ,$before  # the comma keeps the byte array in one piece
}

function Restore-ConfigBytes([string]$Path, $Bytes) {
    if ($null -eq $Bytes) { Remove-Item -LiteralPath $Path -ErrorAction SilentlyContinue }
    else { [IO.File]::WriteAllBytes($Path, [byte[]]$Bytes) }
}
