// Run-Benchmark.ps1 -CpuLimit N (a lab option). Puts the game in a Job object whose CPU affinity is N logical processors (one per
// physical core), from its first milliseconds: Steam starts the launcher stub OblivionRemastered.exe, which starts the
// shipping exe; a poll thread (1 ms) assigns each of them to the job as soon as it appears, so the shipping exe is normally
// born inside the job (child processes inherit it). A job's affinity cannot be widened by the process (SetProcessAffinityMask
// with other processors fails), unlike a plain affinity mask, which something in the game reset to all processors ~3 s after
// launch (seen 2026-10-23). No kill-on-close: the game outlives the runner as usual.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace OblvBench {
public static class CpuLimitJob {
    [StructLayout(LayoutKind.Sequential)]
    struct BASIC_LIMIT { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct PROCESSENTRY32 { public uint dwSize, cntUsage, th32ProcessID; public IntPtr th32DefaultHeapID; public uint th32ModuleID, cntThreads, th32ParentProcessID; public int pcPriClassBase; public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile; }
    [StructLayout(LayoutKind.Sequential)]
    struct LPI { public UIntPtr ProcessorMask; public int Relationship; public long Reserved0, Reserved1; }
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr sa, string name);
    [DllImport("kernel32", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int cls, ref BASIC_LIMIT info, int len);
    [DllImport("kernel32", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
    [DllImport("kernel32", SetLastError = true)] static extern bool IsProcessInJob(IntPtr proc, IntPtr job, out bool result);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32", SetLastError = true)] static extern bool GetProcessAffinityMask(IntPtr proc, out UIntPtr procMask, out UIntPtr sysMask);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool Process32First(IntPtr snap, ref PROCESSENTRY32 e);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool Process32Next(IntPtr snap, ref PROCESSENTRY32 e);
    [DllImport("kernel32", SetLastError = true)] static extern bool GetLogicalProcessorInformation(IntPtr buf, ref uint len);

    const uint PROCESS_SET_QUOTA = 0x0100 | 0x0200 /* PROCESS_SET_INFORMATION: the job applies its affinity */, PROCESS_TERMINATE = 0x0001, PROCESS_QUERY_LIMITED = 0x1000;
    static IntPtr s_job = IntPtr.Zero;
    static Thread s_thread;
    static volatile bool s_stop;
    static readonly List<string> s_log = new List<string>();
    static readonly HashSet<uint> s_done = new HashSet<uint>();

    // N logical processors, the lowest one of each of the first N physical cores (a CPU without SMT); 0 if N is more than the cores
    public static ulong MaskForCores(int n) {
        uint len = 0; GetLogicalProcessorInformation(IntPtr.Zero, ref len);
        IntPtr buf = Marshal.AllocHGlobal((int)len);
        try {
            if (!GetLogicalProcessorInformation(buf, ref len)) return 0;
            int size = Marshal.SizeOf(typeof(LPI)); ulong mask = 0; int got = 0;
            for (int off = 0; off + size <= len && got < n; off += size) {
                var e = (LPI)Marshal.PtrToStructure(new IntPtr(buf.ToInt64() + off), typeof(LPI));
                if (e.Relationship != 0) continue;                   // RelationProcessorCore
                ulong m = e.ProcessorMask.ToUInt64(); if (m == 0) continue;
                mask |= m & (~m + 1); got++;                          // its lowest logical processor
            }
            return got == n ? mask : 0;
        } finally { Marshal.FreeHGlobal(buf); }
    }

    static void Note(string s) { lock (s_log) s_log.Add(s); }

    static List<KeyValuePair<uint, string>> Snapshot() {
        var r = new List<KeyValuePair<uint, string>>();
        IntPtr snap = CreateToolhelp32Snapshot(2, 0);   // TH32CS_SNAPPROCESS
        if (snap == IntPtr.Zero || snap == new IntPtr(-1)) return r;
        try {
            var e = new PROCESSENTRY32(); e.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32));
            for (bool ok = Process32First(snap, ref e); ok; ok = Process32Next(snap, ref e)) r.Add(new KeyValuePair<uint, string>(e.th32ProcessID, e.szExeFile));
        } finally { CloseHandle(snap); }
        return r;
    }

    // Creates the job and starts the poll thread: processes with these exe names that appear from now on join the job.
    public static string Arm(ulong mask, string[] exeNames, int timeoutMs) {
        Disarm();
        lock (s_log) { s_log.Clear(); s_done.Clear(); }
        s_job = CreateJobObject(IntPtr.Zero, null);
        if (s_job == IntPtr.Zero) return "CreateJobObject failed " + Marshal.GetLastWin32Error();
        var lim = new BASIC_LIMIT(); lim.LimitFlags = 0x10; lim.Affinity = new UIntPtr(mask);   // JOB_OBJECT_LIMIT_AFFINITY
        if (!SetInformationJobObject(s_job, 2, ref lim, Marshal.SizeOf(typeof(BASIC_LIMIT)))) return "SetInformationJobObject failed " + Marshal.GetLastWin32Error();
        var before = new HashSet<uint>(); foreach (var p in Snapshot()) before.Add(p.Key);
        var names = new HashSet<string>(exeNames, StringComparer.OrdinalIgnoreCase);
        s_stop = false;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        s_thread = new Thread(() => {
            while (!s_stop && sw.ElapsedMilliseconds < timeoutMs) {
                foreach (var p in Snapshot()) {
                    if (before.Contains(p.Key) || !names.Contains(p.Value) || s_done.Contains(p.Key)) continue;
                    lock (s_log) s_done.Add(p.Key);
                    IntPtr h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED, false, p.Key);
                    if (h == IntPtr.Zero) { Note(p.Value + " pid " + p.Key + ": OpenProcess failed " + Marshal.GetLastWin32Error()); continue; }
                    bool inJob; IsProcessInJob(h, s_job, out inJob);
                    if (inJob) Note(p.Value + " pid " + p.Key + " born in the job (" + sw.ElapsedMilliseconds + " ms after arming)");
                    else if (AssignProcessToJobObject(s_job, h)) Note(p.Value + " pid " + p.Key + " assigned " + sw.ElapsedMilliseconds + " ms after arming");
                    else Note(p.Value + " pid " + p.Key + ": AssignProcessToJobObject failed " + Marshal.GetLastWin32Error());
                    CloseHandle(h);
                }
                Thread.Sleep(1);
            }
        });
        s_thread.IsBackground = true; s_thread.Start();
        return string.Format("job armed: affinity 0x{0:X} ({1} logical processors)", mask, CountBits(mask));
    }

    static int CountBits(ulong m) { int c = 0; for (; m != 0; m &= m - 1) c++; return c; }

    public static void Disarm() { s_stop = true; if (s_thread != null) { s_thread.Join(2000); s_thread = null; } }

    // What happened, plus the given process's job membership and affinity now
    public static string Report(int pid) {
        var sb = new StringBuilder();
        lock (s_log) foreach (var s in s_log) sb.Append(s).Append("; ");
        IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED, false, (uint)pid);
        if (h != IntPtr.Zero) {
            bool inJob = false; if (s_job != IntPtr.Zero) IsProcessInJob(h, s_job, out inJob);
            UIntPtr pm, sm; GetProcessAffinityMask(h, out pm, out sm);
            sb.AppendFormat("game pid {0}: in the job {1}, process affinity 0x{2:X} ({3} logical processors)", pid, inJob ? "yes" : "NO", pm.ToUInt64(), CountBits(pm.ToUInt64()));
            CloseHandle(h);
        } else sb.Append("game pid " + pid + ": not readable");
        return sb.ToString();
    }
}
}
