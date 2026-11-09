# Edits of the player's Config.toml for a test run, and the undo. Dot-source it: . "$PSScriptRoot\config-lines.ps1" (adjust the path).
#
#   Set-ConfigLine <file> <section> <key> <line>  one setting of a Config.toml, returns the line it replaced
#   Set-RumbleOffForRun <file>  rumble off for a game started on the player's own Config.toml; returns the file's bytes
#   Restore-ConfigBytes <file> <bytes>  puts those bytes back (none: there was no file, so it is deleted)
#
# Must parse in Windows PowerShell 5.1. The undo state lives in this script's process (see Set-ConfigLine), which is why this is
# PowerShell and not Python: one call per setting, undone by the matching calls in reverse order, all in the same process.

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
