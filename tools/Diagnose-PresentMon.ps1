<#
.SYNOPSIS
  Why does PresentMon record no frames of a process? A few seconds of side-by-side captures, one line of verdict.

.DESCRIPTION
  Run-Benchmark.ps1 calls this when PresentMon writes no frames of the game (2026-10-09: most runs in the evening,
  intermittent; a PresentMon started by hand saw nothing either). For -Seconds it records at the same time:
    - a raw ETW trace of Microsoft-Windows-DXGI (decoded with tracerpt): does the process call Present at all,
      with which flags (1 = DXGI_PRESENT_TEST: the window counts as occluded and nothing is shown) and results
      (0x087A0001 = DXGI_STATUS_OCCLUDED);
    - PresentMon on the process, CPU side only (--no_track_display --no_track_gpu): a present is written when the
      call returns, without waiting for DWM to show it or for the GPU;
    - PresentMon on all processes as the runner runs it (display tracked): does anything complete, for anyone?
  and lists the ETW sessions that enable the present-event providers (a provider can be in at most 8 sessions)
  and AMD Software's own PresentMon. Writes <OutDir>\presentmon-diag.json and returns the result object.

.EXAMPLE
  tools\Diagnose-PresentMon.ps1 -ProcessId (Get-Process OblivionRemastered-Win64-Shipping).Id -OutDir $env:TEMP\pmdiag
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [Parameter(Mandatory)][string]$OutDir,
    [int]$Seconds = 4
)
$ErrorActionPreference = 'Stop'
$presentMon = Join-Path (Split-Path $PSScriptRoot) 'third_party\PresentMon-2.6.0-x64.exe'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$etl = Join-Path $OutDir 'dxgi.etl'
$cpuCsv = Join-Path $OutDir 'pm-cpuonly.csv'
$allCsv = Join-Path $OutDir 'pm-all.csv'
foreach ($f in $etl, $cpuCsv, $allCsv) { Remove-Item -LiteralPath $f -ErrorAction SilentlyContinue }

# ---- other trace sessions on the present providers, before ours start (a provider can be enabled in at most 8)
if (-not ('OblvPm.EtwEnable' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace OblvPm
{
    // Which trace sessions have a provider enabled: EnumerateTraceGuidsEx(TraceGuidQueryInfo) gives the logger ids
    // per provider instance, ControlTrace(QUERY) by logger id gives the session name. No elevation needed.
    public static class EtwEnable
    {
        [DllImport("advapi32.dll")]
        static extern int EnumerateTraceGuidsEx(int infoClass, ref Guid inBuf, int inSize, IntPtr outBuf, int outSize, out int returned);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int ControlTraceW(ulong handle, string name, IntPtr props, int code);

        static string SessionName(ulong loggerId)
        {
            int size = 120 + 2 * 1024 * 2;
            IntPtr p = Marshal.AllocHGlobal(size);
            try
            {
                for (int i = 0; i < size; i++) Marshal.WriteByte(p, i, 0);
                Marshal.WriteInt32(p, 0, size);                 // Wnode.BufferSize
                Marshal.WriteInt32(p, 44, 0x00020000);          // Wnode.Flags = WNODE_FLAG_TRACED_GUID
                Marshal.WriteInt32(p, 116, 120);                // LoggerNameOffset
                Marshal.WriteInt32(p, 112, 120 + 2048);         // LogFileNameOffset
                if (ControlTraceW(loggerId, null, p, 0) != 0) return "logger " + loggerId;   // EVENT_TRACE_CONTROL_QUERY
                return Marshal.PtrToStringUni(p + 120);
            }
            finally { Marshal.FreeHGlobal(p); }
        }

        // returns the session names that enable the provider (empty if none; null if the query failed)
        public static string[] Sessions(Guid provider)
        {
            int size = 4096, returned;
            for (int attempt = 0; attempt < 4; attempt++)
            {
                IntPtr buf = Marshal.AllocHGlobal(size);
                try
                {
                    int err = EnumerateTraceGuidsEx(1, ref provider, 16, buf, size, out returned);   // TraceGuidQueryInfo
                    if (err == 122) { size = Math.Max(returned, size * 2); continue; }               // ERROR_INSUFFICIENT_BUFFER
                    if (err == 4312 || err == 2) return new string[0];                               // provider not registered
                    if (err != 0) return null;
                    var names = new List<string>();
                    int instances = Marshal.ReadInt32(buf, 0);                                       // TRACE_GUID_INFO
                    int off = 8;
                    for (int i = 0; i < instances; i++)
                    {
                        int next = Marshal.ReadInt32(buf, off);                                      // TRACE_PROVIDER_INSTANCE_INFO
                        int enables = Marshal.ReadInt32(buf, off + 4);
                        for (int e = 0; e < enables; e++)
                        {
                            ulong logger = (ulong)Marshal.ReadInt16(buf, off + 16 + e * 32 + 6) & 0xFFFF;   // TRACE_ENABLE_INFO.LoggerId
                            string n = SessionName(logger);
                            if (!names.Contains(n)) names.Add(n);
                        }
                        if (next == 0) break;
                        off += next;
                    }
                    return names.ToArray();
                }
                finally { Marshal.FreeHGlobal(buf); }
            }
            return null;
        }
    }
}
'@
}
$providers = [ordered]@{ DXGI = 'CA11C036-0102-4A2D-A6AD-F03CFED5D3C9'; DxgKrnl = '802EC45A-1E99-4B83-9920-87C98277BA9D'
                          'Dwm-Core' = '9E9BBA3C-2E38-40CB-99F4-9E8281425164'; Win32k = '8C416C79-D49B-4F01-A467-E56D3AA8234C' }
$sessions = [ordered]@{}
foreach ($kv in $providers.GetEnumerator()) {
    $s = [OblvPm.EtwEnable]::Sessions([guid]$kv.Value)      # 'logger n': a session we may not query (system loggers)
    $sessions[$kv.Key] = if ($null -eq $s) { 'query failed' } else { "$($s.Count): $($s -join ', ')" }
}
$amd = @(Get-CimInstance Win32_Process -Filter "Name like 'PresentMon%'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -notlike '*\third_party\*' } | ForEach-Object { "$($_.Name) pid $($_.ProcessId): $($_.CommandLine)" })

# ---- the three captures, side by side
& logman stop OblvPmDiag -ets 2>&1 | Out-Null
& logman start OblvPmDiag -ets -o $etl -p Microsoft-Windows-DXGI 0x8000000000000002 4 -bs 64 -nb 16 64 2>&1 | Out-Null
$common = @('--no_console_stats', '--qpc_time_ms', '--stop_existing_session', '--timed', $Seconds, '--terminate_after_timed')
$pmCpu = Start-Process $presentMon -PassThru -WindowStyle Hidden -RedirectStandardOutput "$cpuCsv.out" -RedirectStandardError "$cpuCsv.err" -ArgumentList (
    @('--process_id', $ProcessId, '--output_file', "`"$cpuCsv`"", '--session_name', 'OblvPmDiagCpu', '--no_track_display', '--no_track_gpu') + $common)
$pmAll = Start-Process $presentMon -PassThru -WindowStyle Hidden -RedirectStandardOutput "$allCsv.out" -RedirectStandardError "$allCsv.err" -ArgumentList (
    @('--output_file', "`"$allCsv`"", '--session_name', 'OblvPmDiagAll') + $common)
Start-Sleep -Seconds $Seconds
foreach ($p in $pmCpu, $pmAll) { if (-not $p.WaitForExit(15000)) { $p.Kill() } }
& logman stop OblvPmDiag -ets 2>&1 | Out-Null

# ---- what the game did: DXGI Present_Start (42: Flags) / Present_Stop (43: Result)
$calls = 0; $test = 0; $results = @{}; $others = 0
$etlFile = Get-ChildItem -LiteralPath $OutDir -Filter 'dxgi*.etl' | Select-Object -First 1
if ($etlFile) {
    $xml = Join-Path $OutDir 'dxgi.xml'
    & tracerpt $etlFile.FullName -o $xml -of XML -y -lr 2>&1 | Out-Null
    if (Test-Path -LiteralPath $xml) {
        [xml]$x = Get-Content -LiteralPath $xml -Raw
        foreach ($e in @($x.Events.Event)) {
            $id = [int]$e.System.EventID
            if ($id -ne 42 -and $id -ne 43) { continue }
            if ([int]$e.System.Execution.ProcessID -ne $ProcessId) { if ($id -eq 42) { $others++ }; continue }
            $data = @{}; foreach ($d in @($e.EventData.Data)) { $data[$d.Name] = "$($d.'#text')".Trim() }
            if ($id -eq 42) {
                $calls++
                if (([uint32]$data['Flags'] -band 1) -ne 0) { $test++ }
            } else {
                $r = '0x{0:X8}' -f [uint32]([int64]$data['Result'] -band 0xFFFFFFFF)
                $results[$r] = 1 + [int]$results[$r]
            }
        }
        Remove-Item -LiteralPath $xml -ErrorAction SilentlyContinue
    }
}
function Count-Rows([string]$csv) { if (Test-Path -LiteralPath $csv) { @(Import-Csv -LiteralPath $csv) } else { @() } }
$cpuRows = Count-Rows $cpuCsv
$allRows = Count-Rows $allCsv
$allGame = @($allRows | Where-Object { $_.ProcessID -eq "$ProcessId" }).Count
$allApps = ($allRows | Group-Object Application | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ', '

$verdict =
    if (-not $etlFile) { 'no raw DXGI trace could be recorded (logman refused: not in Performance Log Users?)' }
    elseif ($calls -eq 0) { "the game called Present 0 times in $Seconds s (not presenting: loading, hung or stopped rendering)" }
    elseif ($test -eq $calls) { "the game sends only test presents ($test, DXGI_PRESENT_TEST): Windows reports its window as occluded, so no frame is shown or counted" }
    elseif ($cpuRows.Count -gt 0 -and $allGame -eq 0) { "the game presents ($calls calls) but PresentMon never sees them reach the display; CPU-side capture works ($($cpuRows.Count) frames)" }
    elseif ($cpuRows.Count -eq 0) { "the game presents ($calls calls) but PresentMon gets none of its events, also CPU-side (ETW delivery: see the sessions)" }
    else { "everything works now ($calls calls, CPU-side $($cpuRows.Count), display-tracked $allGame frames): the failure has passed" }

$res = [ordered]@{
    time = (Get-Date).ToString('s'); processId = $ProcessId; seconds = $Seconds; verdict = $verdict
    presentCalls = $calls; testPresents = $test; presentResults = $results; otherProcessPresentCalls = $others
    cpuOnlyFrames = $cpuRows.Count; displayTrackedGameFrames = $allGame; displayTrackedAll = $allApps
    sessionsPerProvider = $sessions; amdPresentMon = $amd
}
$res | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDir 'presentmon-diag.json') -Encoding utf8
[pscustomobject]$res
