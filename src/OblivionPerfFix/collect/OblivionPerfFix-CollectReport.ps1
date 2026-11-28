<#
OblivionPerfFix-CollectReport.ps1 - collects what is needed to look at a crash or hang of Oblivion Remastered into ONE zip.

What it does, exactly:
  * reads files (the mod's logs and crash report, its ini, two of the game's ini files, the text files of the game's crash
    folders from the same game session as the newest crash / hang record (the two newest when none can be matched),
    optionally the game's crash dump), the names, sizes and dates (not the contents) of the files in the game's program
    folder, the size and a short SHA-256 hash of the mod's DLL, the ini / log files of other tools that hook the game
    (OptiScaler, ReShade, Special K, UE4SS) when present, and a few Windows event-log entries about the game and the
    graphics driver;
  * writes ONE zip file (at most 10 MB) next to this script (or on the Desktop / in %TEMP% when that folder is read-only),
    opens the folder with the zip selected, and copies a short text summary to the clipboard for pasting into a bug report.
    Older OblivionPerfFix-report-<date>.zip files in the same folder are deleted: only the newest report is kept.
  It changes nothing else: no game file, no setting, no registry entry. It sends nothing anywhere (no network code at all).
  Needs no administrator rights. PowerShell 5.1 (the one in Windows 10 / 11) is enough.
  Your Windows user name is replaced by <user> in every text file, the computer name by <pc>, and the ids the game's crash
  context holds (MachineId, LoginId, EpicAccountId, UserName) are blanked. The crash dump (UEMinidump.dmp) is binary and not
  scrubbed: that is why it is only added after a question, and it is the first thing left out when the zip would pass 10 MB.

Parameters (all optional; the defaults are what a player wants):
  -GameDir <folder>        the folder with OblivionPerfFix.ini / .log (default: the folder of this script; when that has none
                           of them, the script looks for the game on Steam and in XboxGames)
  -DocumentsRoot <folders> where "My Games\Oblivion Remastered\Saved" is looked for (default: the usual places, OneDrive too)
  -OutDir <folder>         where the zip goes (default: next to this script)
  -Minidump Ask|Yes|No     the crash dump question (default Ask; Enter at the question = yes)
  -Days <n>                how many days of Windows events (default 3)
  -NoEventLog              skip the Windows event log
  -NoClipboard             do not copy the summary to the clipboard
  -NoOpen                  do not open the folder afterwards
  -NoPause                 do not wait for a key at the end
  -MaxZipMB <n>            size limit of the zip (default 10)
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string[]]$DocumentsRoot,
    [string]$OutDir,
    [ValidateSet('Ask', 'Yes', 'No')][string]$Minidump = 'Ask',
    [int]$Days = 3,
    [switch]$NoEventLog,
    [switch]$NoClipboard,
    [switch]$NoOpen,
    [switch]$NoPause,
    [double]$MaxZipMB = 10
)

$ErrorActionPreference = 'Continue'
$ScriptVersion = '3'
$script:TextCap = 4MB    # per text file: the start (up to 64 KB) and the newest part are kept above this (smaller when the zip is too big)
$HeadBytes = 64KB
$MaxEvents = 60
$OtherLogCap = 256KB     # logs of other tools (OptiScaler ...): only the end
$ZipLimit = [int64]($MaxZipMB * 1MB)
$CrashFormUrl = 'https://github.com/LTSharpe/OblivionRemasteredPerformanceFix/issues/new?template=crash_report.yml'

try { Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem } catch { }

$here = $PSScriptRoot
if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $here) { $here = (Get-Location).Path }

# ---------------------------------------------------------------- scrub
$userNames = New-Object System.Collections.Generic.List[string]
function Add-Name($list, $n) { if ($n -and $n.Length -ge 2 -and -not $list.Contains($n)) { $list.Add($n) } }
Add-Name $userNames $env:USERNAME
if ($env:USERPROFILE) { Add-Name $userNames (Split-Path -Leaf $env:USERPROFILE) }
try {
    $fso = New-Object -ComObject Scripting.FileSystemObject
    if ($env:USERPROFILE -and (Test-Path -LiteralPath $env:USERPROFILE)) { Add-Name $userNames ($fso.GetFolder($env:USERPROFILE).ShortName) }
} catch { }
$userRx = @()
foreach ($n in $userNames) { $userRx += ,(New-Object System.Text.RegularExpressions.Regex ('(?<![\p{L}\p{N}])' + [regex]::Escape($n) + '(?![\p{L}\p{N}])'), 'IgnoreCase') }
$pcRx = $null
if ($env:COMPUTERNAME -and $env:COMPUTERNAME.Length -ge 3) {
    $pcRx = New-Object System.Text.RegularExpressions.Regex ('(?<![\p{L}\p{N}])' + [regex]::Escape($env:COMPUTERNAME) + '(?![\p{L}\p{N}])'), 'IgnoreCase'
}
$idRx = New-Object System.Text.RegularExpressions.Regex '(<(MachineId|LoginId|EpicAccountId|UserName|bi_session_id|correlation_id)>)[^<]*(</\2>)'
function Protect-Text([string]$t) {
    $t = $idRx.Replace($t, '$1$3')
    foreach ($r in $userRx) { $t = $r.Replace($t, '<user>') }
    if ($pcRx) { $t = $pcRx.Replace($t, '<pc>') }
    return $t
}

# ---------------------------------------------------------------- file helpers
function Open-Shared([string]$path) {
    # the game may still hold its log open (a hung game): read with every share mode
    return New-Object System.IO.FileStream ($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]'ReadWrite,Delete'))
}
function Read-Capped([string]$path, [int64]$cap, [int64]$head) {
    $fs = Open-Shared $path
    try {
        $len = $fs.Length
        if ($len -le $cap) {
            $b = New-Object byte[] $len
            $o = 0
            while ($o -lt $len) { $n = $fs.Read($b, $o, [int]($len - $o)); if ($n -le 0) { break }; $o += $n }
            return @{ Bytes = $b; Cut = 0; Length = $len }
        }
        $h = New-Object byte[] $head
        [void]$fs.Read($h, 0, [int]$head)
        $tailLen = $cap - $head
        $tailStart = $len - $tailLen
        $tailStart = $tailStart - ($tailStart % 4)         # keeps UTF-16 and UTF-8 starts aligned
        $tailLen = $len - $tailStart
        [void]$fs.Seek($tailStart, 'Begin')
        $t = New-Object byte[] $tailLen
        $o = 0
        while ($o -lt $tailLen) { $n = $fs.Read($t, $o, [int]($tailLen - $o)); if ($n -le 0) { break }; $o += $n }
        return @{ Bytes = $null; Head = $h; Tail = $t; Cut = ($tailStart - $head); Length = $len }
    } finally { $fs.Dispose() }
}
function Convert-Bytes([byte[]]$b) {
    if ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { return [Text.Encoding]::Unicode.GetString($b, 2, $b.Length - 2) }
    if ($b.Length -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF) { return [Text.Encoding]::BigEndianUnicode.GetString($b, 2, $b.Length - 2) }
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { return [Text.Encoding]::UTF8.GetString($b, 3, $b.Length - 3) }
    try { return (New-Object Text.UTF8Encoding ($false, $true)).GetString($b) }
    catch { try { return [Text.Encoding]::GetEncoding(1252).GetString($b) } catch { return [Text.Encoding]::UTF8.GetString($b) } }
}
function Get-TextOf([string]$path) {
    # returns the scrubbed text of a file (capped)
    $r = Read-Capped $path $script:TextCap ([math]::Min($HeadBytes, [int64]($script:TextCap / 4)))
    if ($r.Bytes -ne $null) { return (Protect-Text (Convert-Bytes $r.Bytes)) }
    $mb = [math]::Round($r.Length / 1MB, 1)
    return ((Protect-Text (Convert-Bytes $r.Head)) + "`r`n[... $($r.Cut) bytes cut here by OblivionPerfFix-CollectReport: the file is $mb MB; its start and its newest part are kept ...]`r`n" + (Protect-Text (Convert-Bytes $r.Tail)))
}

# ---------------------------------------------------------------- where is everything
$notes = New-Object System.Collections.Generic.List[string]     # lines for manifest.txt: what was not found, and why
$quietNotes = New-Object System.Collections.Generic.HashSet[string]   # notes that go into manifest.txt only, not on the screen
function Note([string]$s, [switch]$Quiet) { $notes.Add($s); if ($Quiet) { [void]$quietNotes.Add($s) } }

function Test-ModFolder([string]$d) {
    if (-not $d -or -not (Test-Path -LiteralPath $d -PathType Container)) { return $false }
    foreach ($p in 'OblivionPerfFix.ini', 'OblivionPerfFix*.log', 'OblivionPerfFix-crash*.txt', 'OblivionPerfFix-gpu*.txt') {
        if (@(Get-ChildItem -LiteralPath $d -Filter $p -File -ErrorAction SilentlyContinue).Count) { return $true }
    }
    return $false
}
function Get-GameRoots {
    # likely game folders: Steam libraries and XboxGames on every fixed drive
    $roots = New-Object System.Collections.Generic.List[string]
    $steam = New-Object System.Collections.Generic.List[string]
    try { $sp = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath; if ($sp) { $steam.Add(($sp -replace '/', '\')) } } catch { }
    foreach ($pf in ${env:ProgramFiles(x86)}, $env:ProgramFiles) { if ($pf) { $steam.Add((Join-Path $pf 'Steam')) } }
    foreach ($s in @($steam)) {
        $vdf = Join-Path $s 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            try { foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $steam.Add(($m.Groups[1].Value -replace '\\\\', '\')) } } catch { }
        }
    }
    foreach ($s in $steam) { Add-Unique $roots (Join-Path $s 'steamapps\common\Oblivion Remastered') }
    try {
        foreach ($dr in [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady }) {
            $x = Join-Path $dr.RootDirectory.FullName 'XboxGames'
            $roots.Add((Join-Path $x 'The Elder Scrolls IV - Oblivion Remastered'))
            $roots.Add((Join-Path $x 'The Elder Scrolls IV - Oblivion Remastered\Content'))
        }
    } catch { }
    return @($roots)
}
function Add-Unique($list, [string]$path) {
    # a folder once, whatever the letter case of its path
    foreach ($x in $list) { if ($x -ieq $path) { return } }
    $list.Add($path)
}
function Get-ModFolders([string]$start) {
    $found = New-Object System.Collections.Generic.List[string]
    if ($start -and (Test-ModFolder $start)) { Add-Unique $found (Resolve-Path -LiteralPath $start).Path; return @($found) }
    $cands = @()
    foreach ($root in (Get-GameRoots)) {
        foreach ($sub in 'OblivionRemastered\Binaries\Win64', 'OblivionRemastered\Binaries\Win64\OBSE\Plugins', 'OblivionRemastered\Binaries\WinGDK') {
            $cands += (Join-Path $root $sub)
        }
    }
    foreach ($c in $cands) { if (Test-ModFolder $c) { Add-Unique $found (Resolve-Path -LiteralPath $c).Path } }
    return @($found)
}
function Find-GameExe([string]$dir) {
    # next to the DLL, or up to three folders higher (OBSE\Plugins), or in the Win64 / WinGDK sibling of a Binaries folder
    $names = 'OblivionRemastered-Win64-Shipping.exe', 'OblivionRemastered-WinGDK-Shipping.exe'
    $d = $dir
    for ($i = 0; $i -lt 4 -and $d; $i++) {
        foreach ($n in $names) { $p = Join-Path $d $n; if (Test-Path -LiteralPath $p -PathType Leaf) { return $p } }
        foreach ($s in 'Win64', 'WinGDK') {
            foreach ($n in $names) { $p = Join-Path (Join-Path $d $s) $n; if (Test-Path -LiteralPath $p -PathType Leaf) { return $p } }
        }
        $d = Split-Path -Parent $d
    }
    return $null
}
function Get-SavedFolders {
    $docs = New-Object System.Collections.Generic.List[string]
    if ($DocumentsRoot) { foreach ($d in $DocumentsRoot) { $docs.Add($d) } }
    else {
        # the game uses the plain (not OneDrive-redirected) Documents under your profile folder; the shell's Documents may differ
        if ($env:USERPROFILE) { $docs.Add((Join-Path $env:USERPROFILE 'Documents')) }
        try { $docs.Add([Environment]::GetFolderPath('MyDocuments')) } catch { }
        foreach ($od in $env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial) { if ($od) { $docs.Add((Join-Path $od 'Documents')) } }
    }
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($d in $docs) {
        if (-not $d) { continue }
        $s = Join-Path $d 'My Games\Oblivion Remastered\Saved'
        if (Test-Path -LiteralPath $s -PathType Container) { Add-Unique $out $s }
    }
    if (-not $DocumentsRoot -and $env:LOCALAPPDATA) {
        # a Game Pass install may keep it in its package folder: look a few levels down (bounded)
        try {
            $pk = Join-Path $env:LOCALAPPDATA 'Packages'
            foreach ($p in Get-ChildItem -LiteralPath $pk -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'Oblivion|Bethesda|Altar' }) {
                foreach ($s in Get-ChildItem -LiteralPath $p.FullName -Directory -Recurse -Depth 5 -Filter Saved -ErrorAction SilentlyContinue) {
                    Add-Unique $out $s.FullName
                }
            }
        } catch { }
    }
    return @($out)
}

# ---------------------------------------------------------------- the zip
$zipItems = New-Object System.Collections.Generic.List[object]   # for the printed list and manifest.txt
$script:zip = $null
$script:zipFs = $null
function Get-FileTime([string]$path) {
    try { return (Get-Item -LiteralPath $path -ErrorAction Stop).LastWriteTime } catch { return $null }
}
function Add-Text([string]$entry, [string]$text, [string]$source, $time = $null) {
    $b = (New-Object Text.UTF8Encoding $false).GetBytes($text)
    $e = $script:zip.CreateEntry($entry, [IO.Compression.CompressionLevel]::Optimal)
    $st = $e.Open()
    try { $st.Write($b, 0, $b.Length) } finally { $st.Dispose() }
    $zipItems.Add([pscustomobject]@{ Entry = $entry; Bytes = $b.Length; Source = $source; Time = $time })
}
function Add-TextFile([string]$entry, [string]$path) {
    try { $t = Get-TextOf $path } catch { Note "could not read $entry ($($_.Exception.Message))"; return }
    Add-Text $entry $t (Split-Path -Leaf $path) (Get-FileTime $path)
}
function Get-TailTextOf([string]$path, [int64]$cap) {
    # the END of a file (scrubbed), starting at a line start
    $fs = Open-Shared $path
    try {
        $len = $fs.Length
        $start = [math]::Max([int64]0, $len - $cap)
        $start = $start - ($start % 4)
        [void]$fs.Seek($start, 'Begin')
        $n = [int]($len - $start)
        $b = New-Object byte[] $n
        $o = 0
        while ($o -lt $n) { $k = $fs.Read($b, $o, $n - $o); if ($k -le 0) { break }; $o += $k }
    } finally { $fs.Dispose() }
    $skip = 0
    if ($start -gt 0) {
        for ($i = 0; $i -lt [math]::Min($b.Length, 8192); $i++) { if ($b[$i] -eq 10) { $skip = $i + 1; break } }
        if ($skip -gt 0) { $b2 = New-Object byte[] ($b.Length - $skip); [Array]::Copy($b, $skip, $b2, 0, $b2.Length); $b = $b2 }
    }
    $t = Protect-Text (Convert-Bytes $b)
    if ($start -gt 0) { $t = "[... the first $($start + $skip) bytes of this $([math]::Round($len / 1MB, 1)) MB file are cut: only its end is kept by OblivionPerfFix-CollectReport ...]`r`n" + $t }
    return $t
}
function Add-TailFile([string]$entry, [string]$path, [int64]$cap) {
    try { $t = Get-TailTextOf $path ([math]::Min($cap, $script:TextCap)) } catch { Note "could not read $entry ($($_.Exception.Message))"; return }
    Add-Text $entry $t (Split-Path -Leaf $path) (Get-FileTime $path)
}
function Add-BinaryFile([string]$entry, [string]$path) {
    $fs = $null
    try { $fs = Open-Shared $path } catch { Note "could not read $entry ($($_.Exception.Message))"; return }
    try {
        $e = $script:zip.CreateEntry($entry, [IO.Compression.CompressionLevel]::Optimal)
        $st = $e.Open()
        try {
            $buf = New-Object byte[] (1MB)
            while (($n = $fs.Read($buf, 0, $buf.Length)) -gt 0) {
                $st.Write($buf, 0, $n)
                if ($script:zipFs.Length -gt $ZipLimit) { throw 'ZIPCAP' }
            }
        } finally { $st.Dispose() }
        $zipItems.Add([pscustomobject]@{ Entry = $entry; Bytes = $fs.Length; Source = (Split-Path -Leaf $path); Time = (Get-FileTime $path) })
    } catch {
        if ($_.Exception.Message -eq 'ZIPCAP') { throw }
        Note "could not read $entry ($($_.Exception.Message))"
    } finally { $fs.Dispose() }
}

function Get-PeInfo([string]$path) {
    try {
        $fs = Open-Shared $path
        try {
            $b = New-Object byte[] 0x400
            [void]$fs.Read($b, 0, $b.Length)
            $pe = [BitConverter]::ToInt32($b, 0x3C)
            if ($pe -gt 0 -and ($pe + 100) -lt $b.Length -and $b[$pe] -eq 0x50 -and $b[$pe + 1] -eq 0x45) {
                $ts = [BitConverter]::ToUInt32($b, $pe + 8)
                $si = [BitConverter]::ToUInt32($b, $pe + 24 + 56)
                $when = [DateTime]::SpecifyKind((New-Object DateTime 1970, 1, 1).AddSeconds($ts), 'Utc').ToString('yyyy-MM-dd HH:mm:ss') + ' UTC'
                return ('TimeDateStamp 0x{0:X8} ({1}), SizeOfImage 0x{2:X}' -f $ts, $when, $si)
            }
        } finally { $fs.Dispose() }
    } catch { }
    return 'PE header not readable'
}
function Format-Ev($e) {
    $msg = ''
    try { $msg = $e.Message } catch { }
    if (-not $msg) { $msg = '(no message text available; data: ' + (($e.Properties | ForEach-Object { $_.Value }) -join ' | ') + ')' }
    $msg = ($msg -replace "\s*\r?\n\s*", ' / ')
    if ($msg.Length -gt 1200) { $msg = $msg.Substring(0, 1200) + ' ...' }
    return ('{0}  id {1}  {2}  level {3}: {4}' -f $e.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'), $e.Id, $e.ProviderName, $e.LevelDisplayName, $msg)
}

# ---------------------------------------------------------------- listing of the game's program folder, other tools' files
function Get-FolderListing([string]$dir) {
    # names, sizes and dates only; never contents
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("Folder: $dir")
    $items = @(Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue | Sort-Object @{ Expression = { -not $_.PSIsContainer } }, Name | Select-Object -First 400)
    if (-not $items.Count) { [void]$sb.AppendLine('  (empty or not readable)') }
    foreach ($i in $items) {
        if ($i.PSIsContainer) { [void]$sb.AppendLine(('  {0}  {1,14}  {2}\' -f $i.LastWriteTime.ToString('yyyy-MM-dd HH:mm'), '<folder>', $i.Name)) }
        else { [void]$sb.AppendLine(('  {0}  {1,14:N0}  {2}' -f $i.LastWriteTime.ToString('yyyy-MM-dd HH:mm'), $i.Length, $i.Name)) }
    }
    return $sb.ToString()
}
function Get-OtherModFiles([string]$exeDir, [string[]]$docRoots) {
    # ini / log files of tools that hook the game: [entry, path]; looked for in the exe folder, its parent and their ue4ss folders
    $out = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    $defs = @(
        @{ Tool = 'OptiScaler'; Files = @('OptiScaler.ini', 'OptiScaler.log') },
        @{ Tool = 'ReShade'; Files = @('ReShade.ini', 'ReShade.log', 'ReShade64.ini', 'ReShade64.log') },
        @{ Tool = 'SpecialK'; Files = @('SpecialK.ini', 'SpecialK.log') },
        @{ Tool = 'UE4SS'; Files = @('UE4SS-settings.ini', 'UE4SS.log', 'Mods\mods.txt') }
    )
    $places = @(@('exe', $exeDir), @('exe-ue4ss', (Join-Path $exeDir 'ue4ss')))
    $parent = Split-Path -Parent $exeDir
    if ($parent) { $places += ,@('parent', $parent); $places += ,@('parent-ue4ss', (Join-Path $parent 'ue4ss')) }
    foreach ($d in $defs) {
        foreach ($pl in $places) {
            foreach ($f in $d.Files) {
                $p = Join-Path $pl[1] $f
                if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { continue }
                $key = (Resolve-Path -LiteralPath $p).Path.ToLowerInvariant()
                if ($seen[$key]) { continue }
                $seen[$key] = $true
                $out.Add([pscustomobject]@{ Entry = ("other-mods/{0}/{1}/{2}" -f $d.Tool, $pl[0], ($f -replace '\\', '/')); Path = $p })
            }
        }
    }
    # Special K keeps its files in Documents\My Mods\SpecialK\Profiles\<game>
    foreach ($dr in $docRoots) {
        $pr = Join-Path $dr 'My Mods\SpecialK\Profiles'
        foreach ($pf in @(Get-ChildItem -LiteralPath $pr -Directory -Filter 'Oblivion*' -ErrorAction SilentlyContinue | Select-Object -First 2)) {
            foreach ($f in 'SpecialK.ini', 'logs\game.log', 'logs\crash.log') {
                $p = Join-Path $pf.FullName $f
                if (Test-Path -LiteralPath $p -PathType Leaf) { $out.Add([pscustomobject]@{ Entry = ("other-mods/SpecialK/profile/{0}" -f ($f -replace '\\', '/')); Path = $p }) }
            }
        }
    }
    return ,$out.ToArray()
}

# ---------------------------------------------------------------- the pasteable summary
$SumBegin = '---- OblivionPerfFix summary: paste this ----'
$SumEnd = '---- end of summary ----'
function Limit-Summary([string]$s, [int]$max) {
    # a record's summary ends with the last few events: drop the newest of them first when the text is long
    if ($s.Length -le $max) { return $s }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($l in ($s -split "`r?`n")) { $lines.Add($l) }
    $hdr = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -like 'last events*') { $hdr = $i; break } }
    while ((($lines -join "`r`n").Length -gt $max) -and $hdr -ge 0 -and $lines.Count -gt ($hdr + 2)) { $lines.RemoveAt($lines.Count - 2) }
    return ($lines -join "`r`n")
}
function Get-NewestSummary([string[]]$dirs) {
    $files = @()
    foreach ($d in $dirs) { foreach ($n in 'OblivionPerfFix-crash.txt', 'OblivionPerfFix-crash.1.txt') { $p = Join-Path $d $n; if (Test-Path -LiteralPath $p -PathType Leaf) { $files += Get-Item -LiteralPath $p } } }
    $rx = New-Object System.Text.RegularExpressions.Regex ('(?s)' + [regex]::Escape($SumBegin) + '\r?\n.*?' + [regex]::Escape($SumEnd)), 'None'
    foreach ($f in @($files | Sort-Object LastWriteTime -Descending)) {
        try {
            $r = Read-Capped $f.FullName 16MB 8MB
            if ($r.Bytes -ne $null) { $t = Convert-Bytes $r.Bytes } else { $t = (Convert-Bytes $r.Head) + "`n" + (Convert-Bytes $r.Tail) }
            $ms = $rx.Matches($t)
            if ($ms.Count) {
                $blk = ($ms[$ms.Count - 1].Value -split "`r?`n") -join "`r`n"
                # the record's header (after the summary block) names the game process: "process: pid 1234, started 2026-11-28 15:12:30, up 150.0 s"
                $prx = '(?m)^process: pid (\d+), started (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)'
                $last = $ms[$ms.Count - 1]
                $pm = [regex]::Match($t.Substring($last.Index + $last.Length), $prx)
                if (-not $pm.Success) { $all = [regex]::Matches($t, $prx); if ($all.Count) { $pm = $all[$all.Count - 1] } }
                $recPid = $null; $recStart = $null
                if ($pm.Success) {
                    $recPid = [int]$pm.Groups[1].Value
                    try { $recStart = [datetime]::ParseExact($pm.Groups[2].Value, 'yyyy-MM-dd HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture) } catch { }
                }
                return @{ Text = (Limit-Summary (Protect-Text $blk) 3500); File = $f.Name; When = $f.LastWriteTime; RecPid = $recPid; Start = $recStart }
            }
        } catch { }
    }
    return $null
}
function Get-FallbackSummary([string[]]$dirs, [string]$sysText) {
    # no crash / hang record: the start-up lines of the newest OblivionPerfFix log, or (no log) what this script read itself
    $log = $null
    foreach ($d in $dirs) {
        $c = @(Get-ChildItem -LiteralPath $d -Filter 'OblivionPerfFix*.log' -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch 'frames|gcmark|hang|ledger|ioq|kmt|gtwd' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
        if ($c.Count -and (-not $log -or $c[0].LastWriteTime -gt $log.LastWriteTime)) { $log = $c[0] }
    }
    $o = New-Object System.Collections.Generic.List[string]
    $o.Add($SumBegin)
    $o.Add('no crash or hang record found: start-up information from the log')
    $haveLog = $false
    if ($log) {
        try {
            $fs = Open-Shared $log.FullName
            try { $n = [int][math]::Min($fs.Length, 1MB); $b = New-Object byte[] $n; $k = 0; while ($k -lt $n) { $q = $fs.Read($b, $k, $n - $k); if ($q -le 0) { break }; $k += $q } } finally { $fs.Dispose() }
            $lines = (Convert-Bytes $b) -split "`r?`n"
            $haveLog = $true
            $v = $lines | Where-Object { $_ -match 'OblivionPerfFix \S+ loaded as ' } | Select-Object -First 1
            if ($v -and $v -match '(OblivionPerfFix \S+ loaded as .+)$') { $o.Add($Matches[1]) }
            $g = $lines | Where-Object { $_ -match ' game exe: ' } | Select-Object -First 1
            if ($g -and $g -match '(game exe: .+)$') { $o.Add($Matches[1]) }
            $s = $lines | Where-Object { $_ -match '\[crash\] system: ' } | Select-Object -First 1
            if ($s -and $s -match '\[crash\] (system: .+)$') { $o.Add(($Matches[1] -replace '\s{2,}', ' ' -replace '\s+,', ',')) }
            $gp = $lines | Where-Object { $_ -match '\[crash\] GPU: ' } | Select-Object -First 1
            if ($gp -and $gp -match '\[crash\] (GPU: .+)$') { $o.Add($Matches[1]) }
            else {
                $gp = $lines | Where-Object { $_ -match "\[mem\] the game's GPU: " } | Select-Object -First 1
                if ($gp -and $gp -match "\[mem\] (the game's GPU: .+)$") { $o.Add($Matches[1]) }
            }
            $rx = New-Object System.Text.RegularExpressions.Regex '\[mods\] (?<name>\S+)(?<flag> \((?:exe|OPF)\))? \[(?<cls>[^:\]]+): [^\]]*\] 0x\w+ size 0x\w+ stamp 0x\w+ version (?<ver>[\d.]+) \| (?<prod>[^|]*) \| (?<desc>[^|]*) \|'
            $sysCls = 'System32', 'SysWOW64', 'WinSxS', 'Windows', 'driver store'
            $mods = New-Object System.Collections.Generic.List[string]
            $anyMods = $false
            foreach ($l in $lines) {
                $m = $rx.Match($l)
                if (-not $m.Success) { continue }
                $anyMods = $true
                if ($m.Groups['flag'].Success -or ($sysCls -contains $m.Groups['cls'].Value)) { continue }
                $t = $m.Groups['name'].Value
                $prod = $m.Groups['prod'].Value.Trim(); $ver = $m.Groups['ver'].Value
                $inner = ''
                if ($prod -and $prod -ne '-') { $inner = $prod }
                if ($ver -and $ver -ne '0.0.0.0') { $inner = (($inner + ' ' + $ver).Trim()) }
                if ($inner) { $t += " ($inner)" }
                if ($m.Groups['cls'].Value -ne 'game') { $t += " [$($m.Groups['cls'].Value)]" }
                if ($mods.Count -lt 16) { $mods.Add($t) }
            }
            if ($mods.Count) { $o.Add("other DLLs in the game (not Windows'): " + ($mods -join ', ')) }
            elseif ($anyMods) { $o.Add("other DLLs in the game (not Windows'): none") }
            else { $o.Add('other DLLs in the game: not listed in this log (older version of the mod)') }
            $o.Add('log: ' + $log.Name + ', ' + $log.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))
        } catch { $haveLog = $false }
    }
    if (-not $haveLog) {
        $o.Add('no OblivionPerfFix log found (the mod may not have loaded)')
        foreach ($l in ($sysText -split "`r?`n")) { if ($l -match '^(Windows|CPU|GPU|Game exe): ') { $o.Add($l) } }
    }
    $o.Add($SumEnd)
    $t = ($o -join "`r`n")
    if ($t.Length -gt 2000) { $t = $t.Substring(0, 1900) + "`r`n" + $SumEnd }
    return @{ Text = (Protect-Text $t); File = $null; When = $null }
}
function Set-ClipText([string]$t) {
    try { Set-Clipboard -Value $t -ErrorAction Stop; return $true } catch { }
    try {
        $tmp = Join-Path $env:TEMP ('opf-clip-' + [guid]::NewGuid().ToString('N') + '.txt')
        [IO.File]::WriteAllText($tmp, $t, [Text.Encoding]::Unicode)
        & cmd.exe /c ('clip < "' + $tmp + '"')
        $ok = ($LASTEXITCODE -eq 0)
        Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
        return $ok
    } catch { return $false }
}
function Wait-AnyKey {
    Write-Host ' Press any key to close this window.'
    try { [void]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown') } catch { try { Read-Host ' (Enter to close)' | Out-Null } catch { } }
}

function Find-ModDll([string]$d) {
    foreach ($n in 'version.dll', 'OblivionPerfFix.dll') { $p = Join-Path $d $n; if (Test-Path -LiteralPath $p -PathType Leaf) { return $p } }
    return $null
}
function Get-CrashFolderInfo($cf) {
    # a game crash folder: the time of its CrashContext, the game process id in it, and the process start (time minus SecondsSinceStart)
    $ctx = Join-Path $cf.FullName 'CrashContext.runtime-xml'
    $when = $cf.LastWriteTime; $cpid = $null; $cstart = $null
    if (Test-Path -LiteralPath $ctx -PathType Leaf) {
        $ft = Get-FileTime $ctx
        if ($ft) { $when = $ft }
        try {
            $r = Read-Capped $ctx 1MB 512KB
            if ($r.Bytes -ne $null) { $x = Convert-Bytes $r.Bytes } else { $x = Convert-Bytes $r.Head }
            $m = [regex]::Match($x, '<ProcessId>\s*(\d+)\s*</ProcessId>')
            if ($m.Success) { $cpid = [int]$m.Groups[1].Value }
            $m = [regex]::Match($x, '<SecondsSinceStart>\s*(\d+)\s*</SecondsSinceStart>')
            if ($m.Success) { $cstart = $when.AddSeconds(-[int]$m.Groups[1].Value) }
        } catch { }
    }
    return [pscustomobject]@{ Folder = $cf; Name = $cf.Name; When = $when; ProcId = $cpid; Start = $cstart; Match = $false }
}
function Test-CrashMatch($ci, $rec) {
    # does this crash folder belong to the game session of the newest record?
    if ($rec.RecPid) {
        if ($ci.ProcId) {
            if ($ci.ProcId -ne $rec.RecPid) { return $false }
            if ($rec.Start -and $ci.Start) { return ([math]::Abs(($ci.Start - $rec.Start).TotalSeconds) -le 120) }   # same id, but not an id reused days later
            if ($rec.Start) { return ($ci.When -ge $rec.Start.AddSeconds(-10)) }
            return $true
        }
        if ($rec.Start) { return ($ci.When -ge $rec.Start.AddSeconds(-10) -and $ci.When -le $rec.When.AddMinutes(30)) }
        return $false
    }
    return ([math]::Abs(($ci.When - $rec.When).TotalMinutes) -le 10)    # a record without a process line (older mod version): by time
}

# ---------------------------------------------------------------- start
$sep = ('-' * 64)
Write-Host $sep
Write-Host ' OblivionPerfFix: collect a crash / hang report'
Write-Host ' Reads some files, writes ONE zip next to this script. Changes nothing else, sends nothing.'
Write-Host $sep

if ($GameDir) { $startDir = $GameDir } else { $startDir = $here }
$modDirs = @(Get-ModFolders $startDir)
$savedDirs = @(Get-SavedFolders)

# a tool started from the wrong folder makes a zip without the mod's DLL or ini: say so first
$warnLines = New-Object System.Collections.Generic.List[string]
if (-not $modDirs.Count) {
    $warnLines.Add("No OblivionPerfFix files were found next to this tool or in the usual game folders: the zip will hold little of use. Copy this tool into the folder that holds version.dll and run it again.")
} else {
    $startFull = $null
    try { $startFull = (Resolve-Path -LiteralPath $startDir -ErrorAction Stop).Path } catch { }
    $startIsMod = $false
    foreach ($m in $modDirs) { if ($startFull -and $m -ieq $startFull) { $startIsMod = $true } }
    foreach ($m in $modDirs) {
        $miss = @()
        if (-not (Find-ModDll $m)) { $miss += 'version.dll / OblivionPerfFix.dll' }
        if (-not (Test-Path -LiteralPath (Join-Path $m 'OblivionPerfFix.ini') -PathType Leaf)) { $miss += 'OblivionPerfFix.ini' }
        if ($miss.Count) { $warnLines.Add("The folder $m has no " + ($miss -join ' and no ') + ': it may not be the folder of the mod.') }
    }
    if (-not $startIsMod) {
        $warnLines.Add("This tool was started from a folder without the mod's DLL or ini ($startDir); it used the game folder it found instead. To be sure it is the right game, copy this tool next to version.dll and run it again.")
    }
}
foreach ($w in $warnLines) { Write-Host (' WARNING: ' + (Protect-Text $w)) -ForegroundColor Yellow }

if (-not $modDirs.Count) {
    Note 'no OblivionPerfFix files found (no ini, log or crash report): the mod may not be installed, or this script is not in the mod folder'
    Write-Host ' No OblivionPerfFix files found next to this script or in the usual game folders.'
    Write-Host ' Copy this script into the game folder that holds version.dll (OblivionRemastered\Binaries\Win64, or WinGDK on Game Pass).'
}
foreach ($m in $modDirs) { Write-Host (' Mod folder : ' + $m) }
foreach ($s in $savedDirs) { Write-Host (' Game data  : ' + $s) }
if (-not $savedDirs.Count) { Note 'no "My Games\Oblivion Remastered\Saved" folder found (checked the profile Documents and OneDrive Documents)' }

# the crash dump question comes first, so that the rest runs unattended
$crashFolders = @()
foreach ($s in $savedDirs) {
    $c = Join-Path $s 'Crashes'
    if (Test-Path -LiteralPath $c) { $crashFolders += @(Get-ChildItem -LiteralPath $c -Directory -Filter 'UECC-*' -ErrorAction SilentlyContinue) }
}
$crashAll = @($crashFolders | Sort-Object LastWriteTime -Descending | Select-Object -First 12 | ForEach-Object { Get-CrashFolderInfo $_ })
$crashFolders = @()
$crashList = New-Object System.Collections.Generic.List[string]    # for manifest.txt: every crash folder found, its time and process id, and whether it is in the zip
# the newest crash / hang record of the mod says which game session (process id, start time) to look for
$sum = $null
if ($modDirs.Count) { $sum = Get-NewestSummary $modDirs }
$haveRecord = $sum -ne $null
if ($crashAll.Count) {
    $pick = @()
    $how = ''
    if ($haveRecord) {
        $recDesc = 'the record from ' + $sum.When.ToString('yyyy-MM-dd HH:mm')
        if ($sum.RecPid) { $recDesc = 'process id ' + $sum.RecPid + $(if ($sum.Start) { ', started ' + $sum.Start.ToString('yyyy-MM-dd HH:mm:ss') } else { '' }) }
        foreach ($ci in $crashAll) { $ci.Match = Test-CrashMatch $ci $sum }
        $pick = @($crashAll | Where-Object { $_.Match } | Select-Object -First 3)
        if ($pick.Count) {
            $how = 'same game session as the newest record'
            Note ("game crash folders: only the $($pick.Count) from the same game session as the newest crash / hang record ($recDesc) are included") -Quiet
        } else {
            $pick = @($crashAll | Select-Object -First 2)
            $how = 'two newest (no folder matched the record''s session)'
            Note ("no crash folder of the game could be matched to the session of the newest crash / hang record ($recDesc): the two newest are included, they may come from an earlier game session")
        }
    } else {
        $pick = @($crashAll | Select-Object -First 2)
        $how = 'two newest (no crash or hang record to match them with)'
        Note 'the mod has no crash or hang record, so the game''s crash folders cannot be matched to a session: the two newest are included'
    }
    $crashFolders = @($pick | ForEach-Object { $_.Folder })
    foreach ($ci in $crashAll) {
        $inc = 'not included'
        if ($pick | Where-Object { $_.Name -eq $ci.Name }) { $inc = 'INCLUDED' }
        $pidText = 'process id unknown'
        if ($ci.ProcId) { $pidText = 'process id ' + $ci.ProcId }
        $crashList.Add(('{0}  file time {1}  {2}  {3}' -f $ci.Name, $ci.When.ToString('yyyy-MM-dd HH:mm:ss'), $pidText, $inc))
    }
    $crashList.Add("rule used: $how")
}
$dumps = @()
foreach ($cf in $crashFolders) {
    $d = Join-Path $cf.FullName 'UEMinidump.dmp'
    if (Test-Path -LiteralPath $d -PathType Leaf) { $dumps += (Get-Item -LiteralPath $d) }
}
$addDumps = $false
if ($dumps.Count) {
    $mb = [math]::Round((($dumps | Measure-Object Length -Sum).Sum) / 1MB, 1)
    if ($Minidump -eq 'Yes') { $addDumps = $true }
    elseif ($Minidump -eq 'Ask') {
        Write-Host ''
        Write-Host (' The game left {0} crash dump file(s) (UEMinidump.dmp, {1} MB in all).' -f $dumps.Count, $mb)
        Write-Host ' A minidump is a binary file: it may still contain your Windows user name and some of the game''s memory'
        Write-Host (' (it is not scrubbed). It helps a lot to find the cause. It is added only if the zip stays under {0} MB.' -f $MaxZipMB)
        $answered = $false
        for ($i = 0; $i -lt 3 -and -not $answered; $i++) {
            $a = ''
            try { $a = Read-Host ' Add the crash dump to the zip? [Y/n]' } catch { $a = '' }
            if ($a -eq $null) { $a = '' }
            $a = $a.Trim()
            if ($a -eq '' -or $a -match '^[yYjJoOsS]') { $addDumps = $true; $answered = $true }
            elseif ($a -match '^[nN]') { $addDumps = $false; $answered = $true }
        }
        if (-not $answered) { Write-Host ' (no clear answer: crash dump left out)' }
    }
}

# the zip's place: next to this script, else Desktop, else %TEMP%
$stamp = Get-Date -Format 'yyyy-MM-dd_HHmm'
$zipName = "OblivionPerfFix-report-$stamp.zip"
$outDirs = @()
if ($OutDir) { $outDirs += $OutDir } else { $outDirs += $here }
try { $outDirs += [Environment]::GetFolderPath('Desktop') } catch { }
$outDirs += $env:TEMP
$zipPath = $null
foreach ($od in $outDirs) {
    if (-not $od) { continue }
    try {
        if (-not (Test-Path -LiteralPath $od)) { New-Item -ItemType Directory -Path $od -Force -ErrorAction Stop | Out-Null }
        $p = Join-Path (Resolve-Path -LiteralPath $od -ErrorAction Stop).Path $zipName
        $tf = New-Object System.IO.FileStream ($p, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $tf.Dispose()
        $zipPath = $p
        break
    } catch { Write-Host (' (cannot write into ' + $od + ': ' + $_.Exception.Message + ')') }
}
if (-not $zipPath) {
    Write-Host ' Could not create the zip file anywhere. Nothing was collected.'
    if (-not $NoPause) { Wait-AnyKey }
    exit 1
}

# ---- things that are the same for every try of the zip: system text, Windows events, the game's program folder
$exePaths = @()
foreach ($m in $modDirs) { $exe = Find-GameExe $m; if ($exe -and ($exePaths -notcontains $exe)) { $exePaths += $exe } }
$sy = New-Object System.Text.StringBuilder
[void]$sy.AppendLine("OblivionPerfFix-CollectReport $ScriptVersion, collected $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (UTC offset $([TimeZoneInfo]::Local.BaseUtcOffset))")
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    [void]$sy.AppendLine("Windows: $($os.Caption), version $($os.Version), build $($os.BuildNumber), $($os.OSArchitecture)")
    [void]$sy.AppendLine("RAM: $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB visible, $([math]::Round($os.FreePhysicalMemory / 1MB, 1)) GB free")
} catch { [void]$sy.AppendLine('Windows: ' + [Environment]::OSVersion.VersionString) }
try {
    foreach ($c in @(Get-CimInstance Win32_Processor -ErrorAction Stop)) { [void]$sy.AppendLine("CPU: $($c.Name.Trim()), $($c.NumberOfCores) cores, $($c.NumberOfLogicalProcessors) threads") }
} catch { }
try {
    foreach ($g in @(Get-CimInstance Win32_VideoController -ErrorAction Stop)) {
        $dd = ''
        if ($g.DriverDate) { $dd = ', driver date ' + $g.DriverDate.ToString('yyyy-MM-dd') }
        [void]$sy.AppendLine("GPU: $($g.Name), driver $($g.DriverVersion)$dd")
    }
} catch { }
[void]$sy.AppendLine("Culture: $((Get-Culture).Name), PowerShell $($PSVersionTable.PSVersion)")
foreach ($m in $modDirs) {
    $dllP = Find-ModDll $m
    $where = Split-Path -Leaf $m
    if (-not $dllP) { [void]$sy.AppendLine("Mod DLL: not found in the mod folder ($where)"); continue }
    try {
        $di = Get-Item -LiteralPath $dllP
        $sha = [Security.Cryptography.SHA256]::Create()
        $dfs = Open-Shared $dllP
        try { $hb = $sha.ComputeHash($dfs) } finally { $dfs.Dispose(); $sha.Dispose() }
        $hex = (($hb | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 16)
        [void]$sy.AppendLine("Mod DLL: $($di.Name) in $where, $($di.Length) bytes, SHA-256 (first 16 hex digits) $hex, written $($di.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))")
    } catch { [void]$sy.AppendLine("Mod DLL: $dllP could not be read ($($_.Exception.Message))") }
}
foreach ($exe in $exePaths) {
    $ei = Get-Item -LiteralPath $exe
    $ver = ''
    try { $ver = ', file version ' + $ei.VersionInfo.FileVersion } catch { }
    [void]$sy.AppendLine("Game exe: $($ei.Name), $($ei.Length) bytes, written $($ei.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))$ver, $(Get-PeInfo $exe)")
}
if (-not $exePaths.Count) { [void]$sy.AppendLine('Game exe: not found next to the mod folder') }
$sysText = Protect-Text $sy.ToString()

$eventText = $null
if (-not $NoEventLog) {
    # only the game's exe and display-driver resets, found by event id and provider, not by message text
    $since = (Get-Date).AddDays(-[math]::Abs($Days))
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("Windows events of the last $Days day(s) about the game and the graphics driver (collected $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))")
    try {
        $app = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001, 1002; StartTime = $since } -ErrorAction SilentlyContinue)
        # by the event's own data (the exe name is data, not translated text; the message is only rendered for the hits)
        $app = @($app | Where-Object {
            $vals = @($_.Properties | ForEach-Object { '' + $_.Value })
            ($vals -join ' ') -match 'OblivionRemastered' -or
            ($_.Id -eq 1001 -and $vals.Count -gt 5 -and $vals[2] -eq 'LiveKernelEvent' -and $vals[5] -in '117', '141', '193')
        } | Select-Object -First $MaxEvents)
        [void]$sb.AppendLine(''); [void]$sb.AppendLine('== Application log: ids 1000 (crash), 1002 (hang), 1001 (error report) naming the game, and 1001 graphics-driver reset reports (LiveKernelEvent 117 / 141 / 193) ==')
        if (-not $app.Count) { [void]$sb.AppendLine('(none)') }
        foreach ($e in $app) { [void]$sb.AppendLine((Format-Ev $e)) }
    } catch { Note "Application event log not readable: $($_.Exception.Message)" }
    try {
        $sysProv = 'Display', 'nvlddmkm', 'amdkmdag', 'amdwddmg', 'igfx', 'igfxn'
        $sys = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = $sysProv; Id = 4101, 14, 153; StartTime = $since } -ErrorAction SilentlyContinue | Select-Object -First $MaxEvents)
        [void]$sb.AppendLine(''); [void]$sb.AppendLine('== System log: display driver resets (Display 4101; nvlddmkm / amdkmdag / igfx 4101, 14, 153) ==')
        if (-not $sys.Count) { [void]$sb.AppendLine('(none)') }
        foreach ($e in $sys) { [void]$sb.AppendLine((Format-Ev $e)) }
    } catch { Note "System event log not readable: $($_.Exception.Message)" }
    $eventText = Protect-Text $sb.ToString()
}

$exeDir = $null
if ($exePaths.Count) { $exeDir = Split-Path -Parent $exePaths[0] }
$docRoots = @()
foreach ($s in $savedDirs) { $r = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $s)); if ($r) { $docRoots += $r } }
$listText = $null
if ($exeDir) {
    $sbl = New-Object System.Text.StringBuilder
    [void]$sbl.AppendLine("Names, sizes and dates of the files in the game's program folder and the usual mod folders below it (no contents). Collected $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss').")
    [void]$sbl.AppendLine('')
    [void]$sbl.Append((Get-FolderListing $exeDir))
    foreach ($sub in 'OBSE', 'OBSE\Plugins', 'ue4ss', 'ue4ss\Mods') {
        $sp = Join-Path $exeDir $sub
        if (Test-Path -LiteralPath $sp -PathType Container) { [void]$sbl.AppendLine(''); [void]$sbl.Append((Get-FolderListing $sp)) }
    }
    $listText = Protect-Text $sbl.ToString()
} else { Note 'the game program folder was not found, so no folder listing and no OptiScaler / ReShade / Special K / UE4SS files' }
$otherFiles = @()
if ($exeDir) { $otherFiles = Get-OtherModFiles $exeDir $docRoots }

# ---- the pasteable summary (clipboard): the newest crash / hang record's, else start-up lines
if (-not $sum) { $sum = Get-FallbackSummary $modDirs $sysText }
$summaryText = Protect-Text $sum.Text
$preNotes = @($notes)

# ---------------------------------------------------------------- one try at the zip
function Invoke-Collect([bool]$withDump) {
    $multi = $modDirs.Count -gt 1
    foreach ($m in $modDirs) {
        $label = 'mod'
        if ($multi) { $parts = @($m -split '[\\/]' | Where-Object { $_ }); $label = 'mod/' + ((@($parts | Select-Object -Last 2) -join '-') -replace '[^A-Za-z0-9._-]', '_') }
        $files = @()
        foreach ($p in 'OblivionPerfFix.ini', 'OblivionPerfFix*.log', 'OblivionPerfFix-crash*.txt', 'OblivionPerfFix-gpu*.txt') {
            $files += @(Get-ChildItem -LiteralPath $m -Filter $p -File -ErrorAction SilentlyContinue)
        }
        $files = @($files | Sort-Object FullName -Unique)
        foreach ($f in $files) { Add-TextFile ("$label/" + $f.Name) $f.FullName }
        if (-not (@($files | Where-Object { $_.Name -like 'OblivionPerfFix-crash*' }).Count)) {
            Note "$label : no OblivionPerfFix-crash.txt (no crash or hang was recorded, or this mod version writes no crash report)" -Quiet
        }
        if (-not (@($files | Where-Object { $_.Name -like '*.log' }).Count)) { Note "$label : no OblivionPerfFix.log (the mod did not load, or LogFile=0)" }
        $dllP = Join-Path $m 'version.dll'
        if (-not (Test-Path -LiteralPath $dllP)) { $dllP = Join-Path $m 'OblivionPerfFix.dll' }
        if (Test-Path -LiteralPath $dllP) {
            $di = Get-Item -LiteralPath $dllP
            Note ("$label : {0} is {1} bytes, written {2}" -f $di.Name, $di.Length, $di.LastWriteTime.ToString('yyyy-MM-dd HH:mm')) -Quiet
        } else { Note "$label : no version.dll / OblivionPerfFix.dll in this folder" }
    }
    # the fallback log when the normal one could not be written
    if ($env:TEMP) {
        foreach ($f in @(Get-ChildItem -LiteralPath $env:TEMP -Filter 'OblivionPerfFix.*.log' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 3)) {
            Add-TextFile ('temp/' + $f.Name) $f.FullName
        }
    }

    # ---- the game's own files
    $si = 0
    foreach ($s in $savedDirs) {
        $lab = 'game'
        if ($savedDirs.Count -gt 1) { $lab = 'game' + $si }
        $si++
        foreach ($cfg in 'Windows', 'WinGDK') {
            foreach ($n in 'Engine.ini', 'GameUserSettings.ini') {
                $p = Join-Path $s "Config\$cfg\$n"
                if (Test-Path -LiteralPath $p -PathType Leaf) { Add-TextFile "$lab/Config/$cfg/$n" $p }
            }
        }
        # the game's own log, if the build writes one (the shipping build usually does not)
        $lg = Join-Path $s 'Logs'
        if (Test-Path -LiteralPath $lg) {
            foreach ($f in @(Get-ChildItem -LiteralPath $lg -Filter 'OblivionRemastered*.log' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 2)) {
                Add-TextFile "$lab/Logs/$($f.Name)" $f.FullName
            }
        }
    }
    foreach ($cf in $crashFolders) {
        foreach ($f in @(Get-ChildItem -LiteralPath $cf.FullName -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq 'CrashContext.runtime-xml' -or $_.Extension -eq '.log' })) {
            Add-TextFile ("game-crashes/$($cf.Name)/" + $f.Name) $f.FullName
        }
    }
    if ($savedDirs.Count -and -not $crashFolders.Count) { Note 'the game has no Crashes\UECC-* folder (no crash was caught by the game itself)' -Quiet }
    if ($dumps.Count -and -not $addDumps) { Note 'crash dump(s) left out (not requested)' }

    # ---- other tools that hook the game
    if ($listText) { Add-Text 'game-folder-listing.txt' $listText 'folder listing' }
    foreach ($of in $otherFiles) { Add-TailFile $of.Entry $of.Path $OtherLogCap }
    if ($exeDir -and -not $otherFiles.Count) { Note 'no OptiScaler / ReShade / Special K / UE4SS ini or log found next to the game program' -Quiet }

    if ($eventText) { Add-Text 'windows-events.txt' $eventText 'Windows event log' }
    Add-Text 'system.txt' $sysText 'system'
    Add-Text 'summary.txt' ($summaryText + "`r`n") 'pasteable summary'

    # the crash dump goes last, so that nothing else is lost when it is what pushes the zip over the limit (it throws ZIPCAP then)
    if ($withDump) {
        foreach ($cf in $crashFolders) {
            $d = Join-Path $cf.FullName 'UEMinidump.dmp'
            if (Test-Path -LiteralPath $d -PathType Leaf) { Add-BinaryFile ("game-crashes/$($cf.Name)/UEMinidump.dmp") $d }
        }
    }

    # ---- manifest.txt
    $mf = New-Object System.Text.StringBuilder
    [void]$mf.AppendLine("OblivionPerfFix-CollectReport $ScriptVersion, $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
    foreach ($w in $warnLines) { [void]$mf.AppendLine('WARNING: ' + (Protect-Text $w)) }
    if ($warnLines.Count) { [void]$mf.AppendLine('') }
    [void]$mf.AppendLine('Text files have the Windows user name replaced by <user> and the computer name by <pc>.')
    [void]$mf.AppendLine('The crash dump, if present below, is binary and not scrubbed.')
    [void]$mf.AppendLine("The zip is limited to $MaxZipMB MB.")
    [void]$mf.AppendLine('')
    [void]$mf.AppendLine('Files in this zip:')
    [void]$mf.AppendLine("(file time = when the original file was last written, local time, UTC offset $([TimeZoneInfo]::Local.BaseUtcOffset))")
    foreach ($i in $zipItems) {
        if ($i.Time) { $tt = 'file time ' + $i.Time.ToString('yyyy-MM-dd HH:mm:ss') } else { $tt = 'made when collected' }
        [void]$mf.AppendLine(('  {0}  ({1} bytes, {2})' -f $i.Entry, $i.Bytes, $tt))
    }
    [void]$mf.AppendLine('  manifest.txt')
    if ($crashList.Count) { [void]$mf.AppendLine(''); [void]$mf.AppendLine("The game's crash folders (newest first; which belong to the same game session as the newest record of the mod):"); foreach ($l in $crashList) { [void]$mf.AppendLine('  ' + $l) } }
    if ($notes.Count) { [void]$mf.AppendLine(''); [void]$mf.AppendLine('Notes:'); foreach ($n in $notes) { [void]$mf.AppendLine('  ' + (Protect-Text $n)) } }
    Add-Text 'manifest.txt' $mf.ToString() 'manifest'
}
function Invoke-Attempt($at) {
    $zipItems.Clear(); $notes.Clear(); $quietNotes.Clear()
    foreach ($n in $preNotes) { Note $n }
    foreach ($n in $dropNotes) { Note $n }
    if ($shortNote) { Note $shortNote }
    $script:TextCap = $at.Cap
    $script:zipFs = New-Object System.IO.FileStream ($zipPath, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $script:zip = New-Object System.IO.Compression.ZipArchive ($script:zipFs, [IO.Compression.ZipArchiveMode]::Create, $false)
    try { Invoke-Collect $at.Dump } finally { $script:zip.Dispose() }
}

$attempts = @(@{ Dump = $addDumps; Cap = 4MB })
if ($addDumps) { $attempts += @{ Dump = $false; Cap = 4MB } }
foreach ($c in 1MB, 256KB, 64KB, 16KB) { $attempts += @{ Dump = $false; Cap = $c } }
$dropNotes = @()
$shortNote = $null
$under = $false
for ($ai = 0; $ai -lt $attempts.Count; $ai++) {
    $at = $attempts[$ai]
    $over = $false
    try { Invoke-Attempt $at } catch {
        if ($_.Exception.Message -eq 'ZIPCAP') { $over = $true } else { throw }
    }
    if (-not $over -and (Get-Item -LiteralPath $zipPath).Length -le $ZipLimit) { $under = $true; break }
    if ($at.Dump) { $dropNotes += "the crash dump was left out: with it the zip would be over $MaxZipMB MB" }
    elseif ($ai + 1 -lt $attempts.Count) { $shortNote = "the text files were cut to at most $([math]::Round($attempts[$ai + 1].Cap / 1KB)) KB each (start and newest part kept) so that the zip stays under $MaxZipMB MB" }
}
if (-not $under) { $notes.Add("the zip is still over $MaxZipMB MB with the shortest logs") }

# only the newest report zip is kept in the folder
$removedZips = 0
$zipDir = Split-Path -Parent $zipPath
foreach ($oz in @(Get-ChildItem -LiteralPath $zipDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^OblivionPerfFix-report-\d{4}-\d\d-\d\d_\d{4}\.zip$' -and $_.FullName -ne $zipPath })) {
    try { Remove-Item -LiteralPath $oz.FullName -Force -ErrorAction Stop; $removedZips++ } catch { $notes.Add("the older report $($oz.Name) could not be removed ($($_.Exception.Message))") }
}

# ---------------------------------------------------------------- report
$zi = Get-Item -LiteralPath $zipPath
Write-Host ''
Write-Host (' Put in the zip ({0} files; sizes in manifest.txt inside):' -f ($zipItems.Count))
$line = '  '
foreach ($e in @($zipItems | ForEach-Object { $_.Entry })) {
    if ($line.Length + $e.Length -gt 96) { Write-Host $line; $line = '  ' }
    $line += ' ' + $e
}
if ($line.Trim()) { Write-Host $line }
$shown = @($notes | Where-Object { -not $quietNotes.Contains($_) })
if ($shown.Count) { Write-Host ''; Write-Host ' Notes:'; foreach ($n in $shown) { Write-Host ('   ' + (Protect-Text $n)) } }
Write-Host ''
$copied = $false
if (-not $NoClipboard) { $copied = Set-ClipText $summaryText }
Write-Host ' Summary for the bug report:'
Write-Host $sep
foreach ($l in ($summaryText -split "`r?`n")) { Write-Host (' ' + $l) }
Write-Host $sep
Write-Host ''
$opened = -not $NoOpen
if ($opened) { $whereText = 'in the folder that just opened' } else { $whereText = 'at ' + $zipPath }
if ($zi.Length -lt 100KB) { $sizeText = '{0:N0} KB' -f ([math]::Max(1, $zi.Length / 1KB)) } else { $sizeText = '{0:N1} MB' -f ($zi.Length / 1MB) }
Write-Host (' Done. Your report: {0} ({1}), {2}.' -f (Split-Path -Leaf $zipPath), $sizeText, $whereText)
if ($removedZips) { Write-Host (' {0} older report zip(s) in this folder were deleted: only the newest report is kept.' -f $removedZips) }
else { Write-Host ' Running this tool again replaces this zip: only the newest report is kept in the folder.' }
foreach ($w in $warnLines) { Write-Host (' WARNING: ' + (Protect-Text $w)) -ForegroundColor Yellow }
if ($copied) {
    if ($haveRecord) { Write-Host (' A short summary of the crash / hang record from {0} is already copied (shown above).' -f $sum.When.ToString('yyyy-MM-dd HH:mm')) }
    else { Write-Host ' No crash was recorded yet, so a short summary of your set-up is copied instead.' }
} elseif ($NoClipboard) { Write-Host ' (the summary was not copied: -NoClipboard)' }
else { Write-Host ' The summary could not be copied: select the text above and copy it yourself.' }
Write-Host ''
Write-Host ' 1. Nexus: open the mod''s Bugs tab, first write what you were doing when it happened,'
Write-Host '    then paste (Ctrl+V) the summary. Nexus takes no files; we will ask for the zip if we need it.'
Write-Host ' 2. GitHub: open'
Write-Host ('      ' + $CrashFormUrl)
Write-Host '    first say what you were doing, then paste the summary and drag the zip into the box.'
Write-Host ' 3. No GitHub account and we asked for the zip: upload it to any file-sharing service you like'
Write-Host '    (for example OneDrive, Google Drive, Dropbox or MEGA), share it as "anyone with the link",'
Write-Host '    and paste the link in the bug report or a Nexus message.'
Write-Host ' Nothing was sent anywhere by this tool.'
Write-Host $sep

if ($opened) {
    try { Start-Process -FilePath 'explorer.exe' -ArgumentList ('/select,"' + $zipPath + '"') } catch { }
}
if (-not $NoPause) { Wait-AnyKey }
exit 0
