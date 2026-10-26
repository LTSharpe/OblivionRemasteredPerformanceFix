<#
.SYNOPSIS
  Shows what the sampled threads were doing during the slow frames of one benchmark segment.
  Joins the PresentMon frame log (QPC time base) with a stack timeline from Sample-Timeline.ps1.
  For each slow frame it lists, per thread, the stacks seen while that frame was being produced,
  reduced to their innermost frames; then a summary over all slow frames of the segment.
  Frames are labelled from tools\symbols.csv (rva,name,note) where known.

.EXAMPLE
  .\Analyze-Hitch.ps1 -Run stutter1 -Segment inn-door/exit
  .\Analyze-Hitch.ps1 -Run stutter1 -Segment inn-door/exit -MinMs 80 -Depth 10 -Threads GameThread
#>
param(
    [Parameter(Mandatory)][string]$Run,              # benchmark label or run folder
    [Parameter(Mandatory)][string]$Segment,          # scene id as in result.json, e.g. inn-door/exit
    [double]$MinMs = 50,                             # frames at least this long count as slow
    [int]$Depth = 8,                                 # innermost frames to show per stack
    [int]$Top = 3,                                   # stacks per thread per frame
    [string[]]$Threads,                              # default: all sampled
    [switch]$SummaryOnly
)
$ErrorActionPreference = 'Stop'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path $PSScriptRoot
$benchDir = Join-Path $root 'captures\bench'
$dir = if (Test-Path (Join-Path $Run 'result.json')) { (Resolve-Path $Run).Path } else {
    (Get-ChildItem $benchDir -Directory | Where-Object { $_.Name -match ('^\d{8}-\d{6}-' + [regex]::Escape($Run) + '$') } | Sort-Object Name | Select-Object -Last 1).FullName }
if (-not $dir) { throw "no benchmark run '$Run'" }
$result = Get-Content (Join-Path $dir 'result.json') -Raw | ConvertFrom-Json
$seg = $result.sessions | ForEach-Object { $_.scenes } | Where-Object { $_.id -eq $Segment } | Select-Object -First 1
if (-not $seg) { throw "segment '$Segment' not in $dir\result.json" }
$prefix = Join-Path $dir ('stacks\' + ($Segment -replace '[\\/]', '-'))
if (-not (Test-Path "$prefix-timeline.csv")) { throw "no stack timeline at $prefix-timeline.csv (run the benchmark with -Stacks)" }

# symbols: function start RVA -> name
$sym = @{}
$symFile = Join-Path $PSScriptRoot 'symbols.csv'
if (Test-Path $symFile) { Import-Csv $symFile | ForEach-Object { $sym[('0x{0:X}' -f [Convert]::ToUInt64(($_.rva -replace '^0x', ''), 16))] = $_.name } }
function Label([string]$frame) {
    $f = $frame -replace '^OblivionRemastered-Win64-Shipping\.exe', 'exe'
    if ($f -match '^exe\+(0x[0-9A-F]+)$' -and $sym.ContainsKey($Matches[1])) { return "$($sym[$Matches[1]]) [$f]" }
    $f
}

$stacks = [IO.File]::ReadAllLines("$prefix-stacks.txt")
$samples = New-Object System.Collections.Generic.List[object]
foreach ($line in [IO.File]::ReadAllLines("$prefix-timeline.csv") | Select-Object -Skip 1) {
    $p = $line.Split(',')
    if ($Threads -and $Threads -notcontains $p[1]) { continue }
    $samples.Add([pscustomobject]@{ T = [double]$p[0]; Thread = $p[1]; Stack = [int]$p[2] })
}

# frames of the segment: a frame "covers" the time from its CPU start until the next frame's CPU start
$frames = New-Object System.Collections.Generic.List[object]
$reader = New-Object IO.StreamReader (Join-Path $dir 'perf-frames.csv')
$cols = $reader.ReadLine().Split(','); $it = [array]::IndexOf($cols, 'CPUStartQPCTimeInMs'); $ift = [array]::IndexOf($cols, 'MsBetweenPresents')
while ($null -ne ($line = $reader.ReadLine())) {
    $p = $line.Split(','); $t = 0.0; $d = 0.0
    if ([double]::TryParse($p[$it], [ref]$t) -and [double]::TryParse($p[$ift], [ref]$d) -and $t -ge $seg.t0Ms -and $t -le $seg.t1Ms) { $frames.Add([pscustomobject]@{ T = $t; Ms = $d }) }
}
$reader.Dispose()

function Inner([string]$stack) {
    # root-first "Thread;f1;f2;...": keep the innermost frames, collapse system wait frames
    $f = $stack.Split(';') | Select-Object -Skip 1
    $keep = @($f | Select-Object -Last $Depth | ForEach-Object { Label $_ })
    ($keep -join ' > ')
}

$slow = @()
for ($i = 0; $i -lt $frames.Count; $i++) {
    # MsBetweenPresents of frame i is the gap before its present: the work happened from the previous frame's start
    if ($frames[$i].Ms -ge $MinMs) {
        $from = if ($i -gt 0) { $frames[$i - 1].T } else { $frames[$i].T - $frames[$i].Ms }
        $slow += [pscustomobject]@{ From = $from; To = $frames[$i].T; Ms = $frames[$i].Ms }
    }
}
"{0}: {1} frames, {2} of them >= {3} ms; {4} stack samples ({5})" -f $Segment, $frames.Count, $slow.Count, $MinMs, $samples.Count, (($samples | Group-Object Thread | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ')
if ($seg.PSObject.Properties['tAMs']) { "arrival at +{0:N2} s" -f (($seg.tAMs - $seg.t0Ms) / 1000) }

$total = @{}
foreach ($h in $slow) {
    $in = @($samples | Where-Object { $_.T -ge $h.From -and $_.T -le $h.To })
    if (-not $SummaryOnly) { "`n=== +{0:N2} s: {1:N0} ms frame ({2} samples)" -f (($h.From - $seg.t0Ms) / 1000), $h.Ms, $in.Count }
    foreach ($g in $in | Group-Object Thread) {
        $by = $g.Group | Group-Object { Inner $stacks[$_.Stack] } | Sort-Object Count -Descending
        foreach ($b in $by) { $key = "$($g.Name)|$($b.Name)"; $total[$key] = [double]$total[$key] + $h.Ms * $b.Count / $g.Count }
        if (-not $SummaryOnly) {
            "  [{0}]" -f $g.Name
            $by | Select-Object -First $Top | ForEach-Object { "    {0,3:N0}%  {1}" -f (100 * $_.Count / $g.Count), $_.Name }
        }
    }
}
"`n=== where the slow-frame time went (ms of slow frames attributed per thread and stack)"
foreach ($t in ($total.Keys | ForEach-Object { $_.Split('|')[0] } | Sort-Object -Unique)) {
    "[$t]"
    $total.GetEnumerator() | Where-Object { $_.Key.StartsWith("$t|") } | Sort-Object Value -Descending | Select-Object -First 12 | ForEach-Object { "  {0,6:N0} ms  {1}" -f $_.Value, $_.Key.Substring($t.Length + 1) }
}
