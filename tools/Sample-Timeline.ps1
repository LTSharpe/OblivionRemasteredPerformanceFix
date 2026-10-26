<#
.SYNOPSIS
  Samples call stacks of named game threads like Sample-Stacks.ps1, but keeps every sample with
  its time stamp (QPC milliseconds, the time base of PresentMon's --qpc_time_ms and of the
  benchmark's result.json), so the stacks can be matched to individual slow frames with
  Analyze-Hitch.ps1. Sampling suspends the threads briefly: use it to find out what a hitch is
  doing, not for fps numbers.
  Writes <OutPrefix>-timeline.csv (QpcMs,Thread,Stack index) and <OutPrefix>-stacks.txt (one
  collapsed stack per line, root first, frames as module+0xFUNCSTART).

.EXAMPLE
  .\Sample-Timeline.ps1 -OutPrefix captures\bench\x\stacks\inn-door-exit -Seconds 16
  .\Sample-Timeline.ps1 -OutPrefix captures\t1 -Threads GameThread,OblivionThread -Seconds 10
  .\Sample-Timeline.ps1 -OutPrefix captures\t2 -Threads 'GameThread,RenderThread 0,*Worker*' -Sampler fast -IntervalMs 2
  Thread names may use wildcards (-like). -Sampler fast (FastSampler.cs: own x64 unwinder, ~20-60 us per thread
  sample instead of ~1 ms with dbghelp) samples every matching thread each -IntervalMs.
#>
param(
    [Parameter(Mandatory)][string]$OutPrefix,
    [double]$Seconds = 15,
    [string[]]$Threads = @('GameThread', 'OblivionThread', 'RenderThread 0', 'RHIThread'),
    [int]$MaxFrames = 48,
    [hashtable[]]$Hints = @(@{ Lo = 0x5C7550; Hi = 0x5C75EF; RetOff = 0x28 }),
    [string]$ProcessName = 'OblivionRemastered-Win64-Shipping',
    [ValidateSet('dbghelp', 'fast')][string]$Sampler = 'dbghelp',
    [double]$IntervalMs = 2
)

$Threads = @($Threads | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })   # "a,b" arrives as one string with powershell -File
if ($Sampler -eq 'fast') { if (-not ('FastSampler' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'FastSampler.cs') } }
elseif (-not ('StackSampler' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'StackSampler.cs') }
if (-not ('ThreadName' -as [type])) {
    Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class ThreadName {
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint a, bool i, uint id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr d);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    public static string Get(uint tid) {
        IntPtr h = OpenThread(0x0800, false, tid); if (h == IntPtr.Zero) return "";
        try { IntPtr p; if (GetThreadDescription(h, out p) < 0 || p == IntPtr.Zero) return "";
              string s = Marshal.PtrToStringUni(p); LocalFree(p); return s; } finally { CloseHandle(h); }
    }
}
'@
}

$proc = Get-Process $ProcessName -ErrorAction Stop | Select-Object -First 1
$targets = @(foreach ($t in $proc.Threads) { $n = [ThreadName]::Get([uint32]$t.Id); if (@($Threads | Where-Object { $n -like $_ }).Count) { [pscustomobject]@{ Tid = [uint32]$t.Id; Name = $n } } })
if (-not $targets.Count) { throw "None of the threads [$($Threads -join ', ')] found" }
$mods = $proc.Modules
$exeBase = [uint64]$proc.MainModule.BaseAddress.ToInt64()
$hintLo = [uint64[]]($Hints | ForEach-Object { $exeBase + [uint64]$_.Lo }); $hintHi = [uint64[]]($Hints | ForEach-Object { $exeBase + [uint64]$_.Hi })
$hintOff = [int[]]($Hints | ForEach-Object { $_.RetOff })
if ($Sampler -eq 'fast') {
    $r = [FastSampler]::Run($proc.Id, [uint32[]]$targets.Tid, [string[]]$targets.Name,
        [uint64[]]($mods | ForEach-Object { [uint64]$_.BaseAddress.ToInt64() }), [uint64[]]($mods | ForEach-Object { [uint64]$_.ModuleMemorySize }),
        [string[]]$mods.ModuleName, $Seconds, $MaxFrames, $IntervalMs, $hintLo, $hintHi, $hintOff)
} else {
    $r = [StackSampler]::Run($proc.Id, [uint32[]]$targets.Tid, [string[]]$targets.Name,
        [uint64[]]($mods | ForEach-Object { [uint64]$_.BaseAddress.ToInt64() }), [string[]]$mods.ModuleName, $Seconds, $MaxFrames,
        $hintLo, $hintHi, $hintOff)
}

New-Item -ItemType Directory -Force (Split-Path $OutPrefix) | Out-Null
$inv = [Globalization.CultureInfo]::InvariantCulture
$sb = New-Object Text.StringBuilder
[void]$sb.AppendLine('QpcMs,Thread,Stack')
for ($i = 0; $i -lt $r.SampleMs.Count; $i++) {
    [void]$sb.Append($r.SampleMs[$i].ToString('F2', $inv)).Append(',').Append($targets[$r.SampleThread[$i]].Name).Append(',').AppendLine($r.SampleStack[$i])
}
[IO.File]::WriteAllText("$OutPrefix-timeline.csv", $sb.ToString())
[IO.File]::WriteAllLines("$OutPrefix-stacks.txt", [string[]]$r.StackList)
"{0} samples of {1} in {2:N1} s ({3:N0}/s per thread), {4} failed -> {5}-timeline.csv" -f $r.Samples, ($targets.Name -join ', '), $r.Seconds, ($r.Samples / $r.Seconds / $targets.Count), $r.Failed, $OutPrefix
if ($Sampler -eq 'fast') { "fast sampler: {0:N0} us per thread sample (suspended + unwound), {1} walks stopped early" -f $r.SuspendUsAvg, $r.Truncated }
