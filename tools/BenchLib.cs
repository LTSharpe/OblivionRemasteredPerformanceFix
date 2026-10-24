// Benchmark support library, compiled by Run-Benchmark.ps1 / Compare-Benchmark.ps1 via Add-Type
// (Windows PowerShell 5.1 = C# 5: no string interpolation, no "?." and no "out var").
//   Clock        QPC milliseconds, the same time base PresentMon writes with --qpc_time_ms
//   ProcSampler  background sampler of per-thread CPU time, process I/O and memory
//   FrameLog     PresentMon CSV reader and per-window frame statistics
//   ImageTools   window capture, frame averaging and image comparison metrics
//   GameHider    hooks the game's window thread (tools/benchhook): window off-screen, no taskbar button, never active
//   WindowKeeper Steam's launch dialog off-screen, focus back to the PC user, cursor released (fallbacks for the game)
//   GameAudio    mutes the game in the volume mixer during a run
//   SettingsSave reads and sets the game's options in Save_Settings.sav (start-up window size)
//   GameWatch    notices within seconds when the game exits, crashes, hangs, stalls or the driver times out
//   GameFlags    reads engine state from the game's memory (PSO precompile running)
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace OblvBench
{
    public static class Clock
    {
        public static double QpcMs() { return Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency; }
    }

    // ---------------------------------------------------------------- engine state read from the game's memory

    public static class GameFlags
    {
        [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);

        // FShaderPipelineCache::IsPrecompiling() in build 19115871 (2026-10-06, drift): exe+0x26FDB30
        //   48 8B 05 <disp32>      mov rax, [FShaderPipelineCache::ShaderPipelineCache]
        //   48 85 C0 74 09         test rax, rax; je ret
        //   0F B6 80 40 01 00 00   movzx eax, byte ptr [rax+140h]      (bHasShaderPipelineTask)
        // After a launch the game precompiles its bundled PSO cache for ~2-3 min: all threads run slower meanwhile
        // (canal 34 vs 53 fps) and every door holds its loading screen up to FadeInExtraDurationPSOCompilation (5 s) longer.
        const long PsoGetterRva = 0x26FDB30;
        static readonly byte[] PsoGetterHead = { 0x48, 0x8B, 0x05 };
        static readonly byte[] PsoGetterTail = { 0x48, 0x85, 0xC0, 0x74, 0x09, 0x0F, 0xB6, 0x80, 0x40, 0x01, 0x00, 0x00 };

        static byte[] Read(IntPtr h, long addr, int n)
        {
            byte[] b = new byte[n]; IntPtr got;
            return ReadProcessMemory(h, new IntPtr(addr), b, new IntPtr(n), out got) && got.ToInt64() == n ? b : null;
        }

        /// 1 = the game is precompiling its PSO cache, 0 = not, -1 = unknown (another game build, or not readable).
        public static int PsoPrecompiling(int pid, long exeBase)
        {
            IntPtr h = OpenProcess(0x0010 | 0x1000, false, (uint)pid);     // VM_READ | QUERY_LIMITED_INFORMATION
            if (h == IntPtr.Zero) return -1;
            try
            {
                byte[] code = Read(h, exeBase + PsoGetterRva, 19);
                if (code == null) return -1;
                for (int i = 0; i < 3; i++) if (code[i] != PsoGetterHead[i]) return -1;
                for (int i = 0; i < PsoGetterTail.Length; i++) if (code[7 + i] != PsoGetterTail[i]) return -1;
                long global = exeBase + PsoGetterRva + 7 + BitConverter.ToInt32(code, 3);
                byte[] p = Read(h, global, 8);
                if (p == null) return -1;
                long cache = BitConverter.ToInt64(p, 0);
                if (cache == 0) return 0;
                byte[] f = Read(h, cache + 0x140, 1);
                return f == null ? -1 : (f[0] != 0 ? 1 : 0);
            }
            finally { CloseHandle(h); }
        }
    }

    // ---------------------------------------------------------------- process sampler

    public sealed class ThreadUse
    {
        public string Name;       // normalised: "RenderThread", "Background Worker", ...
        public double CpuMs;      // CPU time used inside the queried window
        public int Threads;       // number of threads with that name
    }

    public sealed class ProcSampler : IDisposable
    {
        [StructLayout(LayoutKind.Sequential)]
        struct IO_COUNTERS { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }
        [StructLayout(LayoutKind.Sequential)]
        struct PROCESS_MEMORY_COUNTERS_EX
        {
            public uint cb; public uint PageFaultCount; public UIntPtr PeakWorkingSetSize, WorkingSetSize,
                QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage,
                PagefileUsage, PeakPagefileUsage, PrivateUsage;
        }
        [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
        [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
        [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
        [DllImport("kernel32.dll")] static extern bool GetThreadTimes(IntPtr h, out long c, out long e, out long k, out long u);
        [DllImport("kernel32.dll")] static extern bool GetProcessIoCounters(IntPtr h, out IO_COUNTERS c);
        [DllImport("psapi.dll")] static extern bool GetProcessMemoryInfo(IntPtr h, out PROCESS_MEMORY_COUNTERS_EX c, uint cb);
        // System memory state (2026-10-06, drift): SystemPerformanceInformation (class 2). Offsets checked against the
        // perf counters: +0x2C AvailablePages (= Memory\Available MBytes / 4 KB), +0x50 PageReadCount (= raw
        // Memory\Pages Input/sec: pages read from disk to resolve hard faults, the paging that stalls the game).
        [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, byte[] buf, int len, out int ret);
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        const uint QUERY_LIMITED = 0x1000, THREAD_QUERY_LIMITED = 0x0800;

        sealed class Th
        {
            public uint Tid; public IntPtr Handle; public string Name = "";
            public int First;                                   // index of this thread's first sample
            public List<double> Cpu = new List<double>();       // cumulative CPU ms per sample
        }

        readonly int pid; readonly int intervalMs;
        readonly IntPtr hProc;
        readonly Dictionary<uint, Th> threads = new Dictionary<uint, Th>();
        readonly List<double> times = new List<double>();       // QPC ms of each sample
        readonly List<double> ioRead = new List<double>();      // cumulative bytes
        readonly List<double> priv = new List<double>();        // private bytes
        readonly List<double> ws = new List<double>();          // working set bytes
        readonly List<double> faults = new List<double>();      // cumulative page faults of the game (soft + hard)
        readonly List<double> avail = new List<double>();       // system available memory, MB (-1: unknown)
        readonly List<double> pageIns = new List<double>();     // cumulative system pages read for hard faults
        readonly List<double> hardFaults = new List<double>();  // cumulative hard page faults of the game itself
        readonly List<bool> fgOwn = new List<bool>();           // the game owned the foreground window at this sample
        readonly byte[] spi = new byte[0x400];
        byte[] procInfo = new byte[1 << 20];
        double pageInBase; uint pageInLast; bool pageInSeen;
        readonly object gate = new object();
        Thread worker; volatile bool stop;

        public ProcSampler(int processId, int intervalMs)
        {
            pid = processId; this.intervalMs = Math.Max(50, intervalMs);
            hProc = OpenProcess(QUERY_LIMITED, false, (uint)pid);
            if (hProc == IntPtr.Zero) throw new InvalidOperationException("OpenProcess failed for pid " + pid);
        }

        public void Start()
        {
            worker = new Thread(Loop); worker.IsBackground = true; worker.Priority = ThreadPriority.AboveNormal;
            worker.Start();
        }

        public void Stop() { stop = true; if (worker != null) worker.Join(3000); }

        public void Dispose()
        {
            Stop();
            foreach (Th t in threads.Values) CloseHandle(t.Handle);
            threads.Clear(); CloseHandle(hProc);
        }

        public int SampleCount { get { lock (gate) return times.Count; } }

        // SystemProcessInformation (class 5): SYSTEM_PROCESS_INFORMATION.HardFaultCount (+0x10, the process's page faults
        // that needed a disk read: pagefile, image or mapped file), UniqueProcessId (+0x50). -1 if not found.
        double GameHardFaults()
        {
            for (int attempt = 0; attempt < 3; attempt++)
            {
                int got;
                int st = NtQuerySystemInformation(5, procInfo, procInfo.Length, out got);
                if (st == unchecked((int)0xC0000004)) { procInfo = new byte[Math.Max(procInfo.Length * 2, got + 65536)]; continue; }
                if (st != 0) return -1;
                int off = 0;
                while (off + 0x58 <= procInfo.Length)
                {
                    long id = BitConverter.ToInt64(procInfo, off + 0x50);
                    if (id == pid) return BitConverter.ToUInt32(procInfo, off + 0x10);
                    int next = BitConverter.ToInt32(procInfo, off);
                    if (next <= 0) break;
                    off += next;
                }
                return -1;
            }
            return -1;
        }

        static string ReadName(IntPtr h)
        {
            IntPtr p;
            if (GetThreadDescription(h, out p) < 0 || p == IntPtr.Zero) return "";
            string s = Marshal.PtrToStringUni(p); LocalFree(p); return s ?? "";
        }

        void RefreshThreads(int sampleIndex)
        {
            Process proc;
            try { proc = Process.GetProcessById(pid); } catch { return; }
            using (proc)
            {
                foreach (ProcessThread pt in proc.Threads)
                {
                    uint tid = (uint)pt.Id; Th t;
                    if (!threads.TryGetValue(tid, out t))
                    {
                        IntPtr h = OpenThread(THREAD_QUERY_LIMITED, false, tid);
                        if (h == IntPtr.Zero) continue;
                        t = new Th(); t.Tid = tid; t.Handle = h; t.First = sampleIndex;
                        threads[tid] = t;
                    }
                    if (t.Name.Length == 0) t.Name = ReadName(t.Handle);   // names can be set after creation
                }
            }
        }

        void Loop()
        {
            int n = 0;
            double next = Clock.QpcMs();
            while (!stop)
            {
                lock (gate)
                {
                    if (n % 4 == 0) RefreshThreads(times.Count);
                    times.Add(Clock.QpcMs());
                    foreach (Th t in threads.Values)
                    {
                        long c, e, k, u;
                        double ms = GetThreadTimes(t.Handle, out c, out e, out k, out u) ? (k + u) / 10000.0
                                  : (t.Cpu.Count > 0 ? t.Cpu[t.Cpu.Count - 1] : 0);
                        t.Cpu.Add(ms);
                    }
                    IO_COUNTERS io; ioRead.Add(GetProcessIoCounters(hProc, out io) ? (double)io.ReadBytes : 0);
                    PROCESS_MEMORY_COUNTERS_EX m = new PROCESS_MEMORY_COUNTERS_EX(); m.cb = (uint)Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS_EX));
                    bool okMem = GetProcessMemoryInfo(hProc, out m, m.cb);
                    priv.Add(okMem ? (double)m.PrivateUsage.ToUInt64() : 0);
                    ws.Add(okMem ? (double)m.WorkingSetSize.ToUInt64() : 0);
                    faults.Add(okMem ? (double)m.PageFaultCount : (faults.Count > 0 ? faults[faults.Count - 1] : 0));
                    int got;
                    if (NtQuerySystemInformation(2, spi, spi.Length, out got) == 0 && got >= 0x58)
                    {
                        avail.Add(BitConverter.ToUInt32(spi, 0x2C) * 4096.0 / 1048576.0);
                        uint pr = BitConverter.ToUInt32(spi, 0x50);
                        if (pageInSeen && pr < pageInLast) pageInBase += 4294967296.0;   // 32-bit counter wrapped
                        pageInLast = pr; pageInSeen = true;
                        pageIns.Add(pageInBase + pr);
                    }
                    else { avail.Add(-1); pageIns.Add(pageIns.Count > 0 ? pageIns[pageIns.Count - 1] : 0); }
                    // the game's own hard faults (SystemProcessInformation, HardFaultCount at +0x10), about once a second
                    double hf = n % 4 == 0 ? GameHardFaults() : -1;
                    hardFaults.Add(hf >= 0 ? hf : (hardFaults.Count > 0 ? hardFaults[hardFaults.Count - 1] : 0));
                    uint fgPid = 0; IntPtr fgw = GetForegroundWindow();
                    if (fgw != IntPtr.Zero) GetWindowThreadProcessId(fgw, out fgPid);
                    fgOwn.Add(fgPid == (uint)pid);
                }
                n++;
                next += intervalMs;
                double wait = next - Clock.QpcMs();
                if (wait > 0) Thread.Sleep((int)wait); else next = Clock.QpcMs();
            }
        }

        // fractional sample position of time t
        double Pos(double t)
        {
            int n = times.Count;
            if (n == 0) return 0;
            if (t <= times[0]) return 0;
            if (t >= times[n - 1]) return n - 1;
            int lo = 0, hi = n - 1;
            while (hi - lo > 1) { int mid = (lo + hi) / 2; if (times[mid] <= t) lo = mid; else hi = mid; }
            return lo + (t - times[lo]) / (times[hi] - times[lo]);
        }

        static double At(List<double> v, int first, double pos)
        {
            double p = pos - first;
            if (v.Count == 0) return 0;
            if (p <= 0) return v[0];
            if (p >= v.Count - 1) return v[v.Count - 1];
            int i = (int)p; double f = p - i;
            return v[i] + (v[i + 1] - v[i]) * f;
        }

        public static string Normalize(string name)
        {
            if (string.IsNullOrEmpty(name)) return "(unnamed)";
            return Regex.Replace(name, @"\s*#?\d+$", "").Trim();
        }

        /// CPU time per thread name inside [t0, t1] (QPC ms), busiest first.
        public ThreadUse[] Query(double t0, double t1)
        {
            Dictionary<string, ThreadUse> by = new Dictionary<string, ThreadUse>();
            lock (gate)
            {
                double p0 = Pos(t0), p1 = Pos(t1);
                foreach (Th t in threads.Values)
                {
                    double d = At(t.Cpu, t.First, p1) - At(t.Cpu, t.First, p0);
                    string key = Normalize(t.Name);
                    ThreadUse u;
                    if (!by.TryGetValue(key, out u)) { u = new ThreadUse(); u.Name = key; by[key] = u; }
                    u.CpuMs += d; u.Threads++;
                }
            }
            List<ThreadUse> list = new List<ThreadUse>(by.Values);
            list.Sort(delegate(ThreadUse a, ThreadUse b) { return b.CpuMs.CompareTo(a.CpuMs); });
            return list.ToArray();
        }

        public double IoReadMB(double t0, double t1)
        {
            lock (gate) { return (At(ioRead, 0, Pos(t1)) - At(ioRead, 0, Pos(t0))) / 1048576.0; }
        }

        public double PrivateGB(double t)
        {
            lock (gate) { return At(priv, 0, Pos(t)) / 1073741824.0; }
        }

        public double WorkingSetGB(double t)
        {
            lock (gate) { return At(ws, 0, Pos(t)) / 1073741824.0; }
        }

        /// Lowest system available memory (MB) sampled inside [t0, t1]; -1 if unknown.
        public double AvailMinMB(double t0, double t1)
        {
            lock (gate)
            {
                double min = double.MaxValue;
                for (int i = 0; i < times.Count && i < avail.Count; i++)
                    if (times[i] >= t0 && times[i] <= t1 && avail[i] >= 0 && avail[i] < min) min = avail[i];
                if (min == double.MaxValue) { double v = At(avail, 0, Pos(t1)); return v >= 0 ? v : -1; }
                return min;
            }
        }

        /// Share (0..1) of the samples inside [t0, t1] at which the game owned the foreground window; -1 if none.
        /// A game that is not in front behaves differently (Oblivion Remastered turns frame generation off and lowers
        /// its sound; Windows may throttle it), so on-screen runs record it.
        public double ForegroundShare(double t0, double t1)
        {
            lock (gate)
            {
                int n = 0, on = 0;
                for (int i = 0; i < times.Count && i < fgOwn.Count; i++)
                    if (times[i] >= t0 && times[i] <= t1) { n++; if (fgOwn[i]) on++; }
                return n == 0 ? -1 : (double)on / n;
            }
        }

        /// System-wide pages read from disk to resolve hard faults, per second, inside [t0, t1] (the paging that
        /// stalls the game when memory is short; Memory\Pages Input/sec).
        public double PageInsPerSec(double t0, double t1)
        {
            if (t1 <= t0) return 0;
            lock (gate) { return (At(pageIns, 0, Pos(t1)) - At(pageIns, 0, Pos(t0))) / ((t1 - t0) / 1000.0); }
        }

        /// The highest page-in rate over any ~1 s stretch inside [t0, t1].
        public double PageInsMaxPerSec(double t0, double t1)
        {
            lock (gate)
            {
                double max = 0; int j = 0;
                for (int i = 0; i < times.Count && i < pageIns.Count; i++)
                {
                    if (times[i] < t0) continue;
                    if (times[i] > t1) break;
                    if (j < i) j = i;
                    while (j < times.Count - 1 && times[j] - times[i] < 1000 && times[j + 1] <= t1) j++;
                    double dt = times[j] - times[i];
                    if (dt >= 500) max = Math.Max(max, (pageIns[j] - pageIns[i]) / (dt / 1000.0));
                }
                return max;
            }
        }

        /// The game's own hard page faults (that needed a disk read) per second inside [t0, t1]; sampled about once a second.
        public double HardFaultsPerSec(double t0, double t1)
        {
            if (t1 <= t0) return 0;
            lock (gate) { return (At(hardFaults, 0, Pos(t1)) - At(hardFaults, 0, Pos(t0))) / ((t1 - t0) / 1000.0); }
        }

        /// The game's page faults (soft and hard) per second inside [t0, t1].
        public double FaultsPerSec(double t0, double t1)
        {
            if (t1 <= t0) return 0;
            lock (gate) { return (At(faults, 0, Pos(t1)) - At(faults, 0, Pos(t0))) / ((t1 - t0) / 1000.0); }
        }

        /// Every sample interval inside [t0, t1]: busy % of one core for the busiest thread names, I/O read
        /// MB/s and frames-free columns, to see which thread is pegged during a hitch.
        public void WriteWindow(string path, double t0, double t1, int top)
        {
            ThreadUse[] use = Query(t0, t1);
            List<string> names = new List<string>();
            for (int i = 0; i < use.Length && names.Count < top; i++) if (use[i].CpuMs > 0) names.Add(use[i].Name);
            lock (gate)
            {
                using (StreamWriter w = new StreamWriter(path, false, new UTF8Encoding(false)))
                {
                    w.Write("Ms,IoReadMBps");
                    foreach (string nme in names) w.Write("," + nme.Replace(',', ' '));
                    w.WriteLine();
                    for (int i = 1; i < times.Count; i++)
                    {
                        if (times[i] < t0) continue;
                        if (times[i - 1] > t1) break;
                        double dt = times[i] - times[i - 1];
                        if (dt <= 0) continue;
                        Dictionary<string, double> sum = new Dictionary<string, double>();
                        foreach (Th th in threads.Values)
                        {
                            int a = i - 1 - th.First, b = i - th.First;
                            if (a < 0 || b >= th.Cpu.Count) continue;
                            string k = Normalize(th.Name); double cur; sum.TryGetValue(k, out cur); sum[k] = cur + th.Cpu[b] - th.Cpu[a];
                        }
                        w.Write((times[i] - t0).ToString("F0", CultureInfo.InvariantCulture));
                        w.Write("," + ((ioRead[i] - ioRead[i - 1]) / 1048576.0 / (dt / 1000.0)).ToString("F1", CultureInfo.InvariantCulture));
                        foreach (string nme in names) { double v; sum.TryGetValue(nme, out v); w.Write("," + (100.0 * v / dt).ToString("F0", CultureInfo.InvariantCulture)); }
                        w.WriteLine();
                    }
                }
            }
        }

        /// Per-second timeline (busy % of one core per thread name, I/O, memory) for later inspection.
        public void WriteTimeline(string path, double originMs)
        {
            lock (gate)
            {
                if (times.Count < 2) return;
                List<string> names = new List<string>();
                foreach (Th t in threads.Values) { string k = Normalize(t.Name); if (!names.Contains(k)) names.Add(k); }
                names.Sort();
                using (StreamWriter w = new StreamWriter(path, false, new UTF8Encoding(false)))
                {
                    w.Write("Sec,IoReadMBps,PrivateGB");
                    foreach (string nme in names) w.Write("," + nme.Replace(',', ' '));
                    w.WriteLine();
                    for (double t = times[0]; t + 1000 <= times[times.Count - 1]; t += 1000)
                    {
                        double p0 = Pos(t), p1 = Pos(t + 1000);
                        Dictionary<string, double> sum = new Dictionary<string, double>();
                        foreach (Th th in threads.Values)
                        {
                            string k = Normalize(th.Name); double d = At(th.Cpu, th.First, p1) - At(th.Cpu, th.First, p0);
                            double cur; sum.TryGetValue(k, out cur); sum[k] = cur + d;
                        }
                        w.Write(((t - originMs) / 1000.0).ToString("F1", CultureInfo.InvariantCulture));
                        w.Write("," + ((At(ioRead, 0, p1) - At(ioRead, 0, p0)) / 1048576.0).ToString("F1", CultureInfo.InvariantCulture));
                        w.Write("," + (At(priv, 0, p1) / 1073741824.0).ToString("F2", CultureInfo.InvariantCulture));
                        foreach (string nme in names) { double v; sum.TryGetValue(nme, out v); w.Write("," + (v / 10.0).ToString("F1", CultureInfo.InvariantCulture)); }
                        w.WriteLine();
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- frame log

    public sealed class FrameStats
    {
        public int Frames;
        public double Seconds, AvgFps, AvgMs, P50Ms, P95Ms, P99Ms, P999Ms, WorstMs, Low1Fps, Low01Fps;
        public int Hitches;          // frames longer than max(2 x median, 40 ms)
        public double HitchMs;       // time lost in those frames beyond the median
        public int Over50, Over100, Over250;
        public double LongestGapMs;  // longest stretch without a present (loading stall)
        public double CpuBusyMs, GpuBusyMs;
        public double GpuBoundPct;   // share of frames whose GPU-busy time is >= 90% of the frame time
    }

    public sealed class FrameLog
    {
        public double[] T;       // frame CPU start, QPC ms
        public double[] Ft;      // ms between presents
        public double[] Cpu;     // MsCPUBusy
        public double[] Gpu;     // MsGPUBusy
        public int Count { get { return T.Length; } }

        static double Num(string s)
        {
            double v;
            return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : double.NaN;
        }

        public static FrameLog Load(string path)
        {
            List<double> t = new List<double>(), ft = new List<double>(), cpu = new List<double>(), gpu = new List<double>();
            using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (StreamReader r = new StreamReader(fs))
            {
                string header = r.ReadLine();
                if (header == null) throw new InvalidDataException("empty frame log " + path);
                string[] cols = header.Split(',');
                int it = Array.IndexOf(cols, "CPUStartQPCTimeInMs"), ift = Array.IndexOf(cols, "MsBetweenPresents"),
                    ic = Array.IndexOf(cols, "MsCPUBusy"), ig = Array.IndexOf(cols, "MsGPUBusy");
                if (it < 0 || ift < 0) throw new InvalidDataException("frame log needs CPUStartQPCTimeInMs (run PresentMon with --qpc_time_ms) and MsBetweenPresents");
                string line;
                while ((line = r.ReadLine()) != null)
                {
                    string[] f = line.Split(',');
                    if (f.Length <= Math.Max(it, ift)) continue;
                    double tt = Num(f[it]), d = Num(f[ift]);
                    if (double.IsNaN(tt) || double.IsNaN(d) || d <= 0) continue;
                    t.Add(tt); ft.Add(d);
                    cpu.Add(ic >= 0 && ic < f.Length ? Num(f[ic]) : double.NaN);
                    gpu.Add(ig >= 0 && ig < f.Length ? Num(f[ig]) : double.NaN);
                }
            }
            FrameLog log = new FrameLog();
            log.T = t.ToArray(); log.Ft = ft.ToArray(); log.Cpu = cpu.ToArray(); log.Gpu = gpu.ToArray();
            return log;
        }

        static double Pct(double[] sorted, double p)
        {
            if (sorted.Length == 0) return 0;
            return sorted[Math.Min(sorted.Length - 1, (int)Math.Floor(sorted.Length * p))];
        }

        /// Statistics for frames whose CPU start lies in [t0, t1] (QPC ms).
        public FrameStats Stats(double t0, double t1)
        {
            FrameStats s = new FrameStats();
            List<double> v = new List<double>();
            double cpu = 0, gpu = 0; int nc = 0, ng = 0, bound = 0;
            double prevEnd = t0;
            for (int i = 0; i < T.Length; i++)
            {
                if (T[i] < t0) continue;
                if (T[i] > t1) break;
                v.Add(Ft[i]);
                if (!double.IsNaN(Cpu[i])) { cpu += Cpu[i]; nc++; }
                if (!double.IsNaN(Gpu[i])) { gpu += Gpu[i]; ng++; if (Gpu[i] >= 0.9 * Ft[i]) bound++; }
                s.LongestGapMs = Math.Max(s.LongestGapMs, T[i] - prevEnd);
                prevEnd = T[i];
            }
            s.LongestGapMs = Math.Max(s.LongestGapMs, t1 - prevEnd);
            s.Frames = v.Count; s.Seconds = (t1 - t0) / 1000.0;
            if (v.Count == 0) return s;
            double sum = 0; foreach (double d in v) sum += d;
            double[] sorted = v.ToArray(); Array.Sort(sorted);
            s.AvgMs = sum / v.Count; s.AvgFps = 1000.0 / s.AvgMs;
            s.P50Ms = Pct(sorted, 0.50); s.P95Ms = Pct(sorted, 0.95); s.P99Ms = Pct(sorted, 0.99); s.P999Ms = Pct(sorted, 0.999);
            s.WorstMs = sorted[sorted.Length - 1];
            s.Low1Fps = 1000.0 / s.P99Ms; s.Low01Fps = 1000.0 / s.P999Ms;
            double limit = Math.Max(2 * s.P50Ms, 40);
            foreach (double d in v)
            {
                if (d > limit) { s.Hitches++; s.HitchMs += d - s.P50Ms; }
                if (d > 50) s.Over50++;
                if (d > 100) s.Over100++;
                if (d > 250) s.Over250++;
            }
            s.CpuBusyMs = nc > 0 ? cpu / nc : 0; s.GpuBusyMs = ng > 0 ? gpu / ng : 0;
            s.GpuBoundPct = ng > 0 ? 100.0 * bound / ng : 0;
            return s;
        }

        /// Frame times (ms) inside [t0, t1] with their offset from t0, for plots.
        public double[][] Series(double t0, double t1)
        {
            List<double[]> rows = new List<double[]>();
            for (int i = 0; i < T.Length; i++)
            {
                if (T[i] < t0) continue;
                if (T[i] > t1) break;
                rows.Add(new double[] { T[i] - t0, Ft[i] });
            }
            return rows.ToArray();
        }
    }

    // ---------------------------------------------------------------- images

    public sealed class ImageDiff
    {
        public int Width, Height;
        public double MeanLumaA, MeanLumaB, LumaDeltaPct;   // brightness and its relative change (B vs A)
        public double MeanAbsDiff;                           // mean |A-B| per channel, 0..255
        public double Psnr;                                  // dB, higher = closer
        public double Ssim;                                  // 0..1 on luma, 1 = identical structure
        public double ChangedPct;                            // share of 16x16 blocks whose mean luma differs by > 8
        public double ColorShift;                            // length of the mean RGB difference vector
        public double SharpnessA, SharpnessB, SharpnessDeltaPct;  // mean gradient magnitude (detail)
    }

    public static class ImageTools
    {
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
        [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
        [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);
        [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
        static bool dpiSet;

        public static int[] ClientSize(IntPtr hwnd)
        {
            if (!dpiSet) { SetProcessDPIAware(); dpiSet = true; }
            RECT rc; GetClientRect(hwnd, out rc);
            return new int[] { rc.R - rc.L, rc.B - rc.T };
        }

        /// Client area of a window via PrintWindow(PW_RENDERFULLCONTENT): works while the window is covered.
        public static Bitmap Capture(IntPtr hwnd)
        {
            if (!dpiSet) { SetProcessDPIAware(); dpiSet = true; }
            RECT rc, wr; GetClientRect(hwnd, out rc); GetWindowRect(hwnd, out wr);
            POINT pt = new POINT(); ClientToScreen(hwnd, ref pt);
            int w = rc.R - rc.L, h = rc.B - rc.T;
            if (w <= 0 || h <= 0) throw new InvalidOperationException("window has no client area (minimised?)");
            using (Bitmap full = new Bitmap(wr.R - wr.L, wr.B - wr.T, PixelFormat.Format32bppArgb))
            {
                using (Graphics g = Graphics.FromImage(full))
                {
                    IntPtr hdc = g.GetHdc();
                    try { PrintWindow(hwnd, hdc, 2); } finally { g.ReleaseHdc(hdc); }
                }
                return full.Clone(new Rectangle(pt.X - wr.L, pt.Y - wr.T, w, h), PixelFormat.Format24bppRgb);
            }
        }

        /// PrintWindow waits on the window's thread: a frozen game (driver hang) would block the caller for as
        /// long as it stays frozen (a run once sat in it for 3 minutes). The capture runs on its own thread and
        /// the caller gives up after timeoutMs (TimeoutException); the abandoned thread ends when the window
        /// answers or the process is gone.
        public static void CaptureToFile(IntPtr hwnd, string path)
        {
            CaptureToFile(hwnd, path, 10000);
        }

        public static void CaptureToFile(IntPtr hwnd, string path, int timeoutMs)
        {
            Exception error = null;
            var t = new Thread(() =>
            {
                try { using (Bitmap b = Capture(hwnd)) b.Save(path, ImageFormat.Png); }
                catch (Exception e) { error = e; }
            }) { IsBackground = true, Name = "Capture" };
            t.Start();
            if (!t.Join(timeoutMs)) throw new TimeoutException("the game window did not answer the capture within " + timeoutMs + " ms (frozen?)");
            if (error != null) throw error;
        }

        // 24bpp BGR rows without padding
        static byte[] Pixels(Bitmap bmp, out int w, out int h)
        {
            w = bmp.Width; h = bmp.Height;
            BitmapData d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            try
            {
                byte[] px = new byte[w * h * 3];
                for (int y = 0; y < h; y++) Marshal.Copy(new IntPtr(d.Scan0.ToInt64() + (long)y * d.Stride), px, y * w * 3, w * 3);
                return px;
            }
            finally { bmp.UnlockBits(d); }
        }

        static byte[] Load(string path, out int w, out int h)
        {
            using (Bitmap b = new Bitmap(path)) return Pixels(b, out w, out h);
        }

        static void Save(byte[] px, int w, int h, string path)
        {
            using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb))
            {
                BitmapData d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
                for (int y = 0; y < h; y++) Marshal.Copy(px, y * w * 3, new IntPtr(d.Scan0.ToInt64() + (long)y * d.Stride), w * 3);
                bmp.UnlockBits(d);
                bmp.Save(path, ImageFormat.Png);
            }
        }

        /// Per-pixel mean of several same-sized images: suppresses temporal noise (TAA, Lumen, foliage sway).
        public static void Average(string[] paths, string outPath)
        {
            int w = 0, h = 0; int[] acc = null;
            foreach (string p in paths)
            {
                int pw, ph; byte[] px = Load(p, out pw, out ph);
                if (acc == null) { w = pw; h = ph; acc = new int[px.Length]; }
                else if (pw != w || ph != h) throw new InvalidOperationException("image sizes differ: " + p);
                for (int i = 0; i < px.Length; i++) acc[i] += px[i];
            }
            byte[] o = new byte[acc.Length];
            for (int i = 0; i < o.Length; i++) o[i] = (byte)((acc[i] + paths.Length / 2) / paths.Length);
            Save(o, w, h, outPath);
        }

        static float[] Luma(byte[] px, int w, int h)
        {
            float[] l = new float[w * h];
            for (int i = 0, j = 0; i < l.Length; i++, j += 3) l[i] = 0.0722f * px[j] + 0.7152f * px[j + 1] + 0.2126f * px[j + 2];
            return l;
        }

        static double Sharpness(float[] l, int w, int h)
        {
            double sum = 0; int n = 0;
            for (int y = 0; y < h - 1; y++)
                for (int x = 0; x < w - 1; x++)
                {
                    int i = y * w + x;
                    sum += Math.Abs(l[i + 1] - l[i]) + Math.Abs(l[i + w] - l[i]); n++;
                }
            return n > 0 ? sum / n : 0;
        }

        // Mean SSIM over 8x8 windows, stride 4, on luma
        static double SsimOf(float[] a, float[] b, int w, int h)
        {
            const double C1 = 6.5025, C2 = 58.5225; const int N = 8, STEP = 4;
            double total = 0; int count = 0;
            for (int y = 0; y + N <= h; y += STEP)
                for (int x = 0; x + N <= w; x += STEP)
                {
                    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
                    for (int yy = 0; yy < N; yy++)
                    {
                        int row = (y + yy) * w + x;
                        for (int xx = 0; xx < N; xx++)
                        {
                            double va = a[row + xx], vb = b[row + xx];
                            sa += va; sb += vb; saa += va * va; sbb += vb * vb; sab += va * vb;
                        }
                    }
                    double n = N * N, ma = sa / n, mb = sb / n;
                    double var_a = saa / n - ma * ma, var_b = sbb / n - mb * mb, cov = sab / n - ma * mb;
                    total += ((2 * ma * mb + C1) * (2 * cov + C2)) / ((ma * ma + mb * mb + C1) * (var_a + var_b + C2));
                    count++;
                }
            return count > 0 ? total / count : 1;
        }

        /// Compares image B against reference A. Optionally writes a heat map (|A-B| x 8) and a
        /// side-by-side sheet (A | B | heat map) for review.
        public static ImageDiff Compare(string pathA, string pathB, string heatPath, string sheetPath)
        {
            return Compare(pathA, pathB, heatPath, sheetPath, null);
        }

        /// maskNorm: rectangles to ignore (x, y, width, height as fractions of the image), e.g. an
        /// overlay's fps counter. Both images are blacked out there before measuring.
        public static ImageDiff Compare(string pathA, string pathB, string heatPath, string sheetPath, double[] maskNorm)
        {
            int w, h, wb, hb;
            byte[] a = Load(pathA, out w, out h), b = Load(pathB, out wb, out hb);
            if (w != wb || h != hb) throw new InvalidOperationException("image sizes differ: " + w + "x" + h + " vs " + wb + "x" + hb);
            for (int m = 0; maskNorm != null && m + 3 < maskNorm.Length; m += 4)
            {
                int x0 = Math.Max(0, (int)(maskNorm[m] * w)), y0 = Math.Max(0, (int)(maskNorm[m + 1] * h));
                int x1 = Math.Min(w, (int)Math.Ceiling((maskNorm[m] + maskNorm[m + 2]) * w)), y1 = Math.Min(h, (int)Math.Ceiling((maskNorm[m + 1] + maskNorm[m + 3]) * h));
                for (int y = y0; y < y1; y++)
                {
                    Array.Clear(a, (y * w + x0) * 3, (x1 - x0) * 3);
                    Array.Clear(b, (y * w + x0) * 3, (x1 - x0) * 3);
                }
            }
            ImageDiff r = new ImageDiff(); r.Width = w; r.Height = h;
            float[] la = Luma(a, w, h), lb = Luma(b, w, h);
            double sa = 0, sb = 0; foreach (float v in la) sa += v; foreach (float v in lb) sb += v;
            r.MeanLumaA = sa / la.Length; r.MeanLumaB = sb / lb.Length;
            r.LumaDeltaPct = r.MeanLumaA > 0 ? 100.0 * (r.MeanLumaB - r.MeanLumaA) / r.MeanLumaA : 0;

            double abs = 0, sq = 0; double[] chan = new double[3];
            bool wantHeat = !string.IsNullOrEmpty(heatPath), wantSheet = !string.IsNullOrEmpty(sheetPath);
            byte[] heat = wantHeat || wantSheet ? new byte[a.Length] : null;
            for (int i = 0; i < a.Length; i++)
            {
                int d = b[i] - a[i];
                abs += Math.Abs(d); sq += (double)d * d; chan[i % 3] += d;
            }
            r.MeanAbsDiff = abs / a.Length;
            double mse = sq / a.Length;
            r.Psnr = mse > 0 ? 10 * Math.Log10(255.0 * 255.0 / mse) : 99;
            double n = la.Length;
            r.ColorShift = Math.Sqrt(Math.Pow(chan[0] / n, 2) + Math.Pow(chan[1] / n, 2) + Math.Pow(chan[2] / n, 2));
            r.Ssim = SsimOf(la, lb, w, h);
            r.SharpnessA = Sharpness(la, w, h); r.SharpnessB = Sharpness(lb, w, h);
            r.SharpnessDeltaPct = r.SharpnessA > 0 ? 100.0 * (r.SharpnessB - r.SharpnessA) / r.SharpnessA : 0;

            const int BLK = 16; int changed = 0, blocks = 0;
            for (int y = 0; y + BLK <= h; y += BLK)
                for (int x = 0; x + BLK <= w; x += BLK)
                {
                    double d = 0;
                    for (int yy = 0; yy < BLK; yy++) { int row = (y + yy) * w + x; for (int xx = 0; xx < BLK; xx++) d += Math.Abs(lb[row + xx] - la[row + xx]); }
                    blocks++; if (d / (BLK * BLK) > 8) changed++;
                }
            r.ChangedPct = blocks > 0 ? 100.0 * changed / blocks : 0;

            if (heat != null)
            {
                for (int i = 0, j = 0; i < la.Length; i++, j += 3)
                {
                    int v = (int)Math.Min(255, Math.Abs(lb[i] - la[i]) * 8);
                    heat[j] = (byte)(255 - v); heat[j + 1] = (byte)(255 - v); heat[j + 2] = 255;   // white = same, red = different
                    if (v == 0) { heat[j] = heat[j + 1] = heat[j + 2] = 255; }
                }
                if (wantHeat) Save(heat, w, h, heatPath);
                if (wantSheet)
                {
                    byte[] sheet = new byte[w * 3 * h * 3];
                    for (int y = 0; y < h; y++)
                    {
                        Buffer.BlockCopy(a, y * w * 3, sheet, y * w * 9, w * 3);
                        Buffer.BlockCopy(b, y * w * 3, sheet, y * w * 9 + w * 3, w * 3);
                        Buffer.BlockCopy(heat, y * w * 3, sheet, y * w * 9 + w * 6, w * 3);
                    }
                    Save(sheet, w * 3, h, sheetPath);
                }
            }
            return r;
        }
    }

    // ---------------------------------------------------------------- staying out of the PC user's way

    /// Installs tools\benchhook\OblvBenchHook.dll as a thread-specific WH_CBT hook on the main thread of each
    /// game process as soon as it starts (polled every 20 ms; the engine creates its window seconds later).
    /// Inside the game the DLL keeps the window off-screen, out of the taskbar and never active (see
    /// BenchHook.cpp). The hook lasts as long as this thread; the DLL pins itself in the game, so a parked
    /// game stays hidden after the run. Shared state: the mapping Local\OblvBenchHook (layout in BenchHook.cpp).
    /// On-screen runs (-Visible): puts the game window in front with the keyboard focus, as a player's click would,
    /// without sending any input. Windows lets only the foreground process hand the foreground on; the runner (started
    /// from a console, maybe minutes ago) may no longer be it, so it joins the input of the current foreground
    /// window's thread for the call (AttachThreadInput), the documented way for a cooperating process.
    public static class Foreground
    {
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
        [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
        [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
        [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
        [DllImport("user32.dll")] static extern bool AllowSetForegroundWindow(uint pid);
        [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint min, uint max, uint remove);
        [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
        [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr w, l; public uint time; public int x, y; }

        public static bool IsGame(int processId)
        {
            uint p = 0; IntPtr f = GetForegroundWindow();
            if (f != IntPtr.Zero) GetWindowThreadProcessId(f, out p);
            return p == (uint)processId;
        }

        /// "already", "direct", "attached" or "failed (...)"; never restores a minimised game (D3D12 crash risk).
        public static string Bring(IntPtr hwnd, int processId)
        {
            if (hwnd == IntPtr.Zero) return "failed (no window)";
            if (IsGame(processId)) return "already";
            if (IsIconic(hwnd)) return "failed (the game is minimised)";
            AllowSetForegroundWindow((uint)processId);
            if (SetForegroundWindow(hwnd) && IsGame(processId)) return "direct";
            MSG m; PeekMessage(out m, IntPtr.Zero, 0, 0, 0);           // this thread needs a message queue to attach
            uint me = GetCurrentThreadId(), dummy;
            IntPtr fg = GetForegroundWindow();
            uint fgThread = fg != IntPtr.Zero ? GetWindowThreadProcessId(fg, out dummy) : 0;
            bool att = fgThread != 0 && fgThread != me && AttachThreadInput(me, fgThread, true);
            try { BringWindowToTop(hwnd); SetForegroundWindow(hwnd); }
            finally { if (att) AttachThreadInput(me, fgThread, false); }
            for (int i = 0; i < 10 && !IsGame(processId); i++) Thread.Sleep(50);
            return IsGame(processId) ? "attached" : (att ? "failed (still behind)" : "failed (could not attach)");
        }
    }

    public static class GameHider
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibrary(string path);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)] static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SetWindowsHookEx(int id, IntPtr proc, IntPtr module, uint threadId);
        [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr hook);
        [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
        delegate bool EnumProc(IntPtr h, IntPtr l);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
        [StructLayout(LayoutKind.Sequential)] struct POWER_THROTTLING { public uint Version, ControlMask, StateMask; }
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetProcessInformation(IntPtr process, int infoClass, ref POWER_THROTTLING info, int size);
        const int WH_CBT = 5;
        // Shared layout (BenchHook.cpp): version 0, show 4, offX 8, offY 12, lastHwnd 16, created 24, moved 28,
        // activationsBlocked 32, minimiseBlocked 36, pid 40
        const int OffShow = 4, OffLastHwnd = 16, OffCreated = 24, OffMoved = 28, OffActivations = 32, OffMinimise = 36, OffPid = 40;
        // since 2026-10-06 (no crash UI): guard 44 (1 no WER dialogs, 2 crash reporter blocked, 4 windows of all game
        // threads), crashReportsBlocked 48, otherWindows 52, blockedPid 56, testFlags 60, focusReturned 64, crashArgs 72 (128 WCHAR)
        const int OffGuard = 44, OffCrashBlocked = 48, OffOtherWindows = 52, OffBlockedPid = 56, OffTestFlags = 60, OffFocusReturned = 64, OffCrashArgs = 72;
        // since 2026-10-11: capturable 68 (1 = the game window is owned instead of a tool window, for OBS game capture)
        const int OffCapturable = 68;
        // since 2026-10-12 (observer mode): observer 328 (set here: 1 = NPC AI treats the player as not there),
        // talkGuard 332 (in the game talkGuardPid 336: 1 installed, < 0 not), talksRefused 340, lastRefusedForm 344, talkSlot 348,
        // activateSlot 352, lastRefusedCaller 356, targetSlot 360, targetsRefused 364
        const int OffObserver = 328, OffTalkGuard = 332, OffTalkGuardPid = 336, OffTalksRefused = 340, OffLastRefusedForm = 344, OffTalkSlot = 348;
        const int OffActivateSlot = 352, OffLastRefusedCaller = 356, OffTargetSlot = 360, OffTargetsRefused = 364, OffTargetCallers = 368;

        static readonly object sync = new object();
        static System.IO.MemoryMappedFiles.MemoryMappedFile map;
        static System.IO.MemoryMappedFiles.MemoryMappedViewAccessor view;
        static Thread thread;
        static volatile bool running;
        static string[] names = new string[0];
        static string dllPath;
        static readonly List<string> log = new List<string>();

        static void Note(string m) { lock (sync) log.Add(string.Format("{0:HH:mm:ss.fff} {1}", DateTime.Now, m)); }
        public static string[] TakeLog() { lock (sync) { string[] r = log.ToArray(); log.Clear(); return r; } }
        public static bool Running { get { return running; } }

        /// In a tool GUI (not the game) the hook DLL uses "Local\OblvBenchHook.Tools": a host that hides a tool
        /// calls UseToolMapping() before Start, so the game's pid and counters stay apart from the tool's.
        static string mapping = @"Local\OblvBenchHook";
        public static void UseToolMapping() { lock (sync) { if (view == null) mapping = @"Local\OblvBenchHook.Tools"; } }

        static void OpenShared()
        {
            if (view != null) return;
            map = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateOrOpen(mapping, 4096);
            view = map.CreateViewAccessor(0, 4096);
        }

        /// Starts hooking every new process of these names (without ".exe") and sets "hidden".
        public static void Start(string hookDll, string[] processNames) { Start(hookDll, processNames, false); }

        /// show = true (watch mode, -Visible): the game gets the DLL too, but its window is left alone; the game-side
        /// parts (no crash UI, the NPC conversation guard of spectator mode) work as in a hidden run.
        public static void Start(string hookDll, string[] processNames, bool show)
        {
            lock (sync)
            {
                OpenShared();
                view.Write(OffShow, show ? 1 : 0);
                names = processNames ?? new string[0];
                dllPath = hookDll;
                if (running) return;
                running = true;
                thread = new Thread(Loop) { IsBackground = true, Name = "GameHider" };
                thread.Start();
            }
        }

        /// Removes the hooks (a game that has the DLL keeps its window where it is, hidden or shown).
        public static void Stop()
        {
            Thread t;
            lock (sync) { running = false; t = thread; thread = null; }
            if (t != null) t.Join(3000);
        }

        /// false = keep the game off-screen and inactive (the default); true = let the PC user see and use it.
        public static void SetShow(bool show) { lock (sync) { OpenShared(); view.Write(OffShow, show ? 1 : 0); } }
        /// Run-Benchmark -Record: hidden game windows created from now on are owned windows instead of tool windows
        /// (no taskbar button either way), so OBS's game capture can find the game. Set by Start-Unobtrusive -Capturable:$x.
        public static void SetCapturable(bool on) { lock (sync) { OpenShared(); view.Write(OffCapturable, on ? 1 : 0); } }
        public static bool Capturable { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffCapturable) != 0; } catch (Exception) { return false; } } } }
        public static bool ShowRequested { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffShow) != 0; } catch (Exception) { return false; } } } }

        /// Spectator mode's observer: true = the hook in the game makes NPC AI treat the player as not there (no AI
        /// package gets the player as its target; conversations NPCs start with the player are refused: forced greetings,
        /// which `tdetect` does not stop). NPCs among themselves go on as before.
        public static void SetObserver(bool on) { lock (sync) { OpenShared(); view.Write(OffObserver, on ? 1 : 0); } }

        /// The NPC conversation guard in this game process: 1 installed; 0 not there (an older hook build, or no hook in
        /// this game); -1 RTTI not found, -2 StartConversation command not found, -3 its vtable slot not confirmed,
        /// -4 the slot could not be written (BenchHook.cpp, "observer mode").
        public static int TalkGuardFor(int gamePid)
        {
            lock (sync)
            {
                try { OpenShared(); return view.ReadInt32(OffTalkGuardPid) == gamePid ? view.ReadInt32(OffTalkGuard) : 0; }
                catch (Exception) { return 0; }
            }
        }
        public static string TalkGuardText(int status)
        {
            switch (status)
            {
                case 1:
                    lock (sync)
                    {
                        OpenShared();
                        return string.Format("installed, process SetPackageTarget slot +0x{2:X}, Character StartConversation slot +0x{0:X}, TESNPC Activate slot +0x{1:X}",
                            view.ReadInt32(OffTalkSlot), view.ReadInt32(OffActivateSlot), view.ReadInt32(OffTargetSlot));
                    }
                case 0: return "not in this game: an older hook build, or the hook is not in it";
                case -1: return "not installed: Actor/Character/PlayerCharacter/TESNPC/process RTTI not found (game update?)";
                case -2: return "not installed: the StartConversation script command not found (game update?)";
                case -3: return "not installed: StartConversation's vtable slot not confirmed (game update?)";
                case -4: return "not installed: a vtable slot could not be written";
                case -5: return "not installed: TESNPC's Activate slot not confirmed (game update?)";
                case -6: return "not installed: the AI processes' SetPackageTarget slot not confirmed (game update?)";
                default: return "unknown status " + status;
            }
        }
        /// Conversation starts refused so far (all games since the mapping was created), the NPC refused last, and where
        /// in the game that refused call came from (exe RVA).
        public static int TalksRefused { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffTalksRefused); } catch (Exception) { return 0; } } } }
        public static string LastRefusedForm { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffLastRefusedForm).ToString("X8"); } catch (Exception) { return "?"; } } } }
        public static string LastRefusedCaller { get { lock (sync) { try { OpenShared(); return "exe+0x" + view.ReadInt32(OffLastRefusedCaller).ToString("X"); } catch (Exception) { return "?"; } } } }
        /// AI package targets refused so far: an NPC's package (Find, Ambush, Follow...) aimed at the player found no target.
        public static int TargetsRefused { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffTargetsRefused); } catch (Exception) { return 0; } } } }
        /// Where in the game's AI the refused package targets came from (up to 8 call sites): "exe+0x...=count ...".
        public static string TargetCallers
        {
            get
            {
                lock (sync)
                {
                    try
                    {
                        OpenShared();
                        var parts = new List<string>();
                        for (int i = 0; i < 8; i++)
                        {
                            int rva = view.ReadInt32(OffTargetCallers + 8 * i), n = view.ReadInt32(OffTargetCallers + 8 * i + 4);
                            if (rva != 0) parts.Add(string.Format("exe+0x{0:X}={1}", rva, n));
                        }
                        return string.Join(" ", parts.ToArray());
                    }
                    catch (Exception) { return "?"; }
                }
            }
        }

        /// Counters from inside the game: windows taken over, positions forced off-screen, activations refused.
        public static string Stats()
        {
            lock (sync)
            {
                OpenShared();
                return string.Format("pid={0} window=0x{1:X} created={2} moved={3} activationsBlocked={4} minimiseBlocked={5} show={6} crashGuard={7} crashReportsBlocked={8} otherWindows={9} focusReturned={10} talkGuard={11}@{12} observer={13} talksRefused={14} targetsRefused={15}",
                    view.ReadInt32(OffPid), view.ReadInt64(OffLastHwnd), view.ReadInt32(OffCreated), view.ReadInt32(OffMoved),
                    view.ReadInt32(OffActivations), view.ReadInt32(OffMinimise), view.ReadInt32(OffShow),
                    view.ReadInt32(OffGuard), view.ReadInt32(OffCrashBlocked), view.ReadInt32(OffOtherWindows), view.ReadInt32(OffFocusReturned),
                    view.ReadInt32(OffTalkGuard), view.ReadInt32(OffTalkGuardPid), view.ReadInt32(OffObserver), view.ReadInt32(OffTalksRefused),
                    view.ReadInt32(OffTargetsRefused));
            }
        }

        /// The process the hook last took a window over in (0 = none yet).
        public static int HookedPid { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffPid); } catch (Exception) { return 0; } } } }

        /// Activations of the game's windows the hook has refused so far (all games since the mapping was created).
        public static int ActivationsBlocked { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffActivations); } catch (Exception) { return 0; } } } }

        /// What the hook has in place against crash UI in the game (BenchHook.cpp): 1 no Windows error-report dialogs,
        /// 2 UE's crash reporter cannot start, 4 windows of every game thread kept off-screen; 0 = an older hook build.
        public static int CrashGuardFlags { get { lock (sync) { try { OpenShared(); return view.ReadInt32(OffGuard); } catch (Exception) { return 0; } } } }

        /// Crash reporter starts the hook refused in this game process (it crashed), and the end of the refused
        /// command line (UE's crash folder and CrashGUID); 0 / null if none.
        public static int CrashReportsBlockedFor(int gamePid)
        {
            lock (sync)
            {
                try { OpenShared(); return view.ReadInt32(OffBlockedPid) == gamePid ? view.ReadInt32(OffCrashBlocked) : 0; }
                catch (Exception) { return 0; }
            }
        }
        public static string BlockedCrashArgs()
        {
            lock (sync)
            {
                try
                {
                    OpenShared();
                    var b = new byte[256]; view.ReadArray(OffCrashArgs, b, 0, b.Length);
                    string s = Encoding.Unicode.GetString(b); int z = s.IndexOf('\0');
                    return z >= 0 ? s.Substring(0, z) : s;
                }
                catch (Exception) { return null; }
            }
        }

        /// Test switch (tools\Test-BenchFault.ps1): true lets UE's crash reporter start, to check that
        /// CrashGuard (tools\Crash-Guard.ps1) keeps it off the screen on its own.
        public static void SetCrashReporterAllowed(bool allowed) { lock (sync) { OpenShared(); view.Write(OffTestFlags, allowed ? 1 : 0); } }

        static uint MainThread(Process p)
        {
            uint best = 0; DateTime first = DateTime.MaxValue;
            foreach (ProcessThread t in p.Threads)
            {
                try { if (t.StartTime < first) { first = t.StartTime; best = (uint)t.Id; } }
                catch (Exception) { if (best == 0) best = (uint)t.Id; }
            }
            return best;
        }

        /// Windows 11 treats a process whose windows are all off-screen (or occluded) and that is silent as
        /// background work: it may run it with EcoQoS (lower clocks) and stops honouring its timer-resolution
        /// requests (Sleep(1) becomes ~15.6 ms). The hidden, muted benchmark game looks exactly like that, the
        /// game the player has in front never does: opt it out of both (documented per-process switches).
        public static string ForceForegroundQoS(int pid)
        {
            IntPtr h = OpenProcess(0x0200 | 0x1000, false, pid);              // PROCESS_SET_INFORMATION | QUERY_LIMITED
            if (h == IntPtr.Zero) return "power throttling: cannot open pid " + pid + " (error " + Marshal.GetLastWin32Error() + ")";
            try
            {
                var st = new POWER_THROTTLING { Version = 1, ControlMask = 0x1 | 0x4, StateMask = 0 };   // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, both off
                if (!SetProcessInformation(h, 4, ref st, Marshal.SizeOf(typeof(POWER_THROTTLING))))   // ProcessPowerThrottling
                    return "power throttling: opt-out failed for pid " + pid + " (error " + Marshal.GetLastWin32Error() + ")";
                return "power throttling off for pid " + pid + " (no EcoQoS, timer resolution honoured as for a foreground game)";
            }
            finally { CloseHandle(h); }
        }

        static bool HasVisibleWindow(int pid)
        {
            bool found = false;
            EnumWindows((h, l) => { uint wp; GetWindowThreadProcessId(h, out wp); if (wp == (uint)pid && IsWindowVisible(h)) { found = true; return false; } return true; }, IntPtr.Zero);
            return found;
        }

        static void Loop()
        {
            IntPtr module = IntPtr.Zero, proc = IntPtr.Zero;
            var hooks = new Dictionary<int, IntPtr>();
            try
            {
                module = LoadLibrary(dllPath);
                if (module != IntPtr.Zero) proc = GetProcAddress(module, "CbtProc");
                if (proc == IntPtr.Zero) { Note("cannot load the hook DLL " + dllPath + " (error " + Marshal.GetLastWin32Error() + "): the game window is only kept off-screen from outside"); return; }
                while (running)
                {
                    string[] n; lock (sync) n = names;
                    var alive = new HashSet<int>();
                    foreach (string name in n)
                        foreach (Process p in Process.GetProcessesByName(name))
                        {
                            alive.Add(p.Id);
                            if (!hooks.ContainsKey(p.Id))
                            {
                                // A thread can only be hooked once it is a GUI thread (its first user32 call; error 87
                                // before that), which the game's main thread becomes long before it creates its window:
                                // try again every poll until it works or the window is up anyway.
                                bool late = HasVisibleWindow(p.Id);
                                uint tid = MainThread(p);
                                IntPtr h = tid == 0 ? IntPtr.Zero : SetWindowsHookEx(WH_CBT, proc, module, tid);
                                int err = Marshal.GetLastWin32Error();
                                if (h != IntPtr.Zero || late)
                                {
                                    hooks[p.Id] = h;
                                    Note(ForceForegroundQoS(p.Id));
                                    if (h == IntPtr.Zero) Note(string.Format("could not hook {0} pid {1} thread {2} before its window came up (error {3}): kept off-screen from outside", name, p.Id, tid, err));
                                    else Note(string.Format("hooked {0} pid {1} main thread {2}{3}", name, p.Id, tid, late ? " (its window was already up: hidden from its next move on)" : ""));
                                }
                            }
                            p.Dispose();
                        }
                    foreach (int id in new List<int>(hooks.Keys))
                        if (!alive.Contains(id)) { if (hooks[id] != IntPtr.Zero) UnhookWindowsHookEx(hooks[id]); hooks.Remove(id); }
                    // waiting for a start: look often (the window comes seconds after the process); else relax
                    Thread.Sleep(hooks.Count == 0 ? 20 : 250);
                }
            }
            catch (Exception e) { Note("hider error: " + e.Message); }
            finally { foreach (IntPtr h in hooks.Values) if (h != IntPtr.Zero) UnhookWindowsHookEx(h); }
        }
    }

    /// Background thread (with its own message loop for window events) that keeps an unattended run from
    /// bothering the PC user, next to GameHider:
    ///   - Steam's game-launch dialog ("Launching...") goes off-screen the moment it shows, and if it took the
    ///     focus the focus goes back. Steam's other windows (library, store, friends) are never touched, so
    ///     Steam can be used as usual during a run;
    ///   - fallback for the game when the hook was not in place: a game window on a monitor is moved off it,
    ///     and when it takes the focus the focus goes back to the window the PC user had (never minimised: a game
    ///     restored from minimised crashes);
    ///   - a cursor confined by the game is released (UE clips the cursor to its viewport while it has focus);
    /// Nothing is done to the game while the PC user asked to see it (GameHider.SetShow(true)) or with -Visible.
    public static class WindowKeeper
    {
        delegate bool EnumProc(IntPtr h, IntPtr l);
        delegate void WinEventProc(IntPtr hook, uint ev, IntPtr h, int idObject, int idChild, uint thread, uint time);
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
        [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr h; public uint msg; public IntPtr w, l; public uint time; public int x, y; }
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
        [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
        [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
        [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
        [DllImport("user32.dll")] static extern bool AttachThreadInput(uint from, uint to, bool attach);
        [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr h, int cmd);
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] static extern bool GetClipCursor(out RECT r);
        [DllImport("user32.dll")] static extern bool ClipCursor(IntPtr r);
        [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr module, WinEventProc proc, uint pid, uint thread, uint flags);
        [DllImport("user32.dll")] static extern bool UnhookWinEvent(IntPtr hook);
        [DllImport("user32.dll")] static extern uint MsgWaitForMultipleObjects(uint n, IntPtr[] handles, bool all, uint ms, uint mask);
        [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint min, uint max, uint remove);
        [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
        [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG m);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);

        const uint EVENT_SYSTEM_FOREGROUND = 3, EVENT_OBJECT_SHOW = 0x8002, EVENT_OBJECT_NAMECHANGE = 0x800C;
        const uint WINEVENT_OUTOFCONTEXT = 0, WINEVENT_SKIPOWNPROCESS = 2, GA_ROOT = 2, GW_OWNER = 4;
        const uint SWP_NOSIZE = 1, SWP_NOZORDER = 4, SWP_NOACTIVATE = 0x10, SWP_ASYNCWINDOWPOS = 0x4000;
        const uint QS_ALLINPUT = 0x4FF, PM_REMOVE = 1;
        // Steam's launch dialog; its other windows are the PC user's
        static readonly Regex SteamLaunchTitle = new Regex(@"^(Launching|Preparing to launch|Starting)\b", RegexOptions.IgnoreCase);

        static readonly object sync = new object();
        static string[] steamNames = new string[0], gameNames = new string[0];
        static Thread thread;
        static volatile bool running;
        static volatile int generation;
        static readonly List<string> log = new List<string>();
        static readonly HashSet<IntPtr> logged = new HashSet<IntPtr>();
        static Dictionary<uint, string> pids = new Dictionary<uint, string>();   // pid -> "s:name" (Steam) or "g:name" (game)
        static IntPtr userWindow;                                                // last foreground window not of Steam or the game
        static WinEventProc callback;                                            // kept alive while hooked

        /// Starts the thread (or only replaces the lists if it runs). Process names without ".exe".
        public static void Keep(string[] steam, string[] game)
        {
            lock (sync)
            {
                steamNames = steam ?? new string[0];
                gameNames = game ?? new string[0];
                generation++;
                if (running) return;
                running = true;
                thread = new Thread(Loop) { IsBackground = true, Name = "WindowKeeper" };
                thread.Start();
            }
        }

        public static void Stop()
        {
            Thread t;
            lock (sync) { running = false; t = thread; thread = null; }
            if (t != null) t.Join(2000);
        }

        /// What it did so far, one line each, and clears the list.
        public static string[] TakeLog() { lock (sync) { string[] r = log.ToArray(); log.Clear(); return r; } }

        static void Note(string m) { lock (sync) log.Add(string.Format("{0:HH:mm:ss.fff} {1}", DateTime.Now, m)); }

        static int[] OffScreen()
        {
            return new int[] { GetSystemMetrics(76) + GetSystemMetrics(78) + 64, GetSystemMetrics(77) };   // right of SM_X/CXVIRTUALSCREEN, at SM_YVIRTUALSCREEN
        }

        static bool OnScreen(IntPtr h)
        {
            RECT r; if (!GetWindowRect(h, out r)) return false;
            int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
            return r.R > vx && r.L < vx + vw && r.B > vy && r.T < vy + vh;
        }

        /// Moves a window off every monitor without activating it (asynchronous: never waits on the window's thread).
        public static void MoveOffScreen(IntPtr h)
        {
            int[] p = OffScreen();
            SetWindowPos(h, IntPtr.Zero, p[0], p[1], 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }

        [DllImport("user32.dll")] static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
        [DllImport("user32.dll")] static extern IntPtr SetWindowLongPtr(IntPtr h, int index, IntPtr value);
        [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint action, uint param, out RECT r, uint ini);
        const long WS_EX_TOOLWINDOW = 0x80, WS_EX_APPWINDOW = 0x40000;

        /// SetWindowLongPtr on another process's window waits for its thread: on a worker, given up after 3 s
        /// (or not waited for at all).
        static void SetExStyle(IntPtr h, bool tool) { SetExStyle(h, tool, true); }
        static void SetExStyle(IntPtr h, bool tool, bool wait)
        {
            var t = new Thread(() =>
            {
                long ex = GetWindowLongPtr(h, -20).ToInt64();
                long want = tool ? (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW : (ex & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW;
                if (want != ex) SetWindowLongPtr(h, -20, new IntPtr(want));
            }) { IsBackground = true };
            t.Start(); if (wait) t.Join(3000);
        }

        /// The game window onto the primary monitor (centred in its work area), with a taskbar button, in front.
        /// Call GameHider.SetShow(true) first, or the hook moves it straight back.
        public static void BringOnScreen(IntPtr h)
        {
            SetExStyle(h, false);
            RECT wa, r;
            if (SystemParametersInfo(0x30, 0, out wa, 0) && GetWindowRect(h, out r))   // SPI_GETWORKAREA
            {
                int x = wa.L + Math.Max(0, ((wa.R - wa.L) - (r.R - r.L)) / 2), y = wa.T + Math.Max(0, ((wa.B - wa.T) - (r.B - r.T)) / 2);
                SetWindowPos(h, IntPtr.Zero, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
            }
            IntPtr fg = GetForegroundWindow();
            uint pid, owner = GetWindowThreadProcessId(fg, out pid), me = GetCurrentThreadId();
            bool attached = owner != 0 && AttachThreadInput(me, owner, true);
            try { SetForegroundWindow(h); }
            finally { if (attached) AttachThreadInput(me, owner, false); }
        }

        /// The game window back off-screen without a taskbar button (after BringOnScreen).
        public static void SendOffScreen(IntPtr h)
        {
            SetExStyle(h, true);
            MoveOffScreen(h);
            if (GetForegroundWindow() == h) GiveFocusBack(h, "game");
        }

        /// Titles are logged only for the game's and Steam's windows. Any other window (a browser, a chat, a document)
        /// is named by its process and class, so run logs and shared reports say nothing about what else is open on
        /// the PC. OBLV_BENCH_LOG_TITLES=1 logs every title (for debugging the harness on your own PC).
        static readonly bool logAllTitles = Environment.GetEnvironmentVariable("OBLV_BENCH_LOG_TITLES") == "1";

        static string Describe(IntPtr h)
        {
            var c = new StringBuilder(128); GetClassName(h, c, 128);
            if (Tag(h) != null || logAllTitles)
            {
                var t = new StringBuilder(256); GetWindowText(h, t, 256);
                return "'" + t.ToString().Trim() + "' (" + c + ")";
            }
            uint pid; GetWindowThreadProcessId(h, out pid);
            string name = "?";
            try { using (var p = Process.GetProcessById((int)pid)) name = p.ProcessName; } catch (Exception) { }
            return "<" + name + " window> (" + c + ")";
        }

        static string Tag(IntPtr h)
        {
            uint pid; GetWindowThreadProcessId(h, out pid);
            string tag; lock (sync) { if (!pids.TryGetValue(pid, out tag)) tag = null; }
            return tag;
        }

        static void RefreshPids()
        {
            string[] s, g; lock (sync) { s = steamNames; g = gameNames; }
            var d = new Dictionary<uint, string>();
            foreach (string n in s) foreach (Process p in Process.GetProcessesByName(n)) { d[(uint)p.Id] = "s:" + n; p.Dispose(); }
            foreach (string n in g) foreach (Process p in Process.GetProcessesByName(n)) { d[(uint)p.Id] = "g:" + n; p.Dispose(); }
            lock (sync) pids = d;
        }

        static bool GameGuarded() { return !GameHider.ShowRequested; }

        /// A window that should not be on a monitor: Steam's launch dialog, or a game window while guarded.
        static bool ShouldHide(IntPtr h, string tag)
        {
            if (tag == null || !IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER) != IntPtr.Zero && tag[0] == 's') return false;
            if (tag[0] == 'g') return GameGuarded();
            var t = new StringBuilder(256); GetWindowText(h, t, 256);
            return SteamLaunchTitle.IsMatch(t.ToString().Trim());
        }

        static void Handle(IntPtr h, string tag, string why)
        {
            if (!ShouldHide(h, tag)) return;
            if (tag[0] == 's')
            {
                // Steam's launch dialog is hidden (moving it off-screen lost against Steam re-activating it,
                // 2026-10-06). If it has the focus, the focus goes back to the PC user's window explicitly: hiding
                // alone lets Windows pick the next window, which was the desktop once.
                bool hadFocus = GetForegroundWindow() == h;
                ShowWindowAsync(h, 0);
                if (hadFocus) GiveFocusBack(h, tag.Substring(2));
                bool once; lock (sync) once = logged.Add(h);
                if (once) Note(string.Format("hid ({0}) {1} {2}", why, tag.Substring(2), Describe(h)));
                return;
            }
            if (!OnScreen(h)) return;
            MoveOffScreen(h);
            if (tag[0] == 'g' && !GameHider.Capturable) SetExStyle(h, true, false);   // no taskbar button (a game shown earlier, or one the hook missed); owned instead when capturable
            bool first; lock (sync) first = logged.Add(h);
            if (first) Note(string.Format("moved off-screen ({0}) {1} {2}", why, tag.Substring(2), Describe(h)));
            if (GetForegroundWindow() == h) GiveFocusBack(h, tag.Substring(2));
        }

        static void OnEvent(IntPtr hook, uint ev, IntPtr h, int idObject, int idChild, uint thread, uint time)
        {
            try
            {
                if (h == IntPtr.Zero || idObject != 0 || idChild != 0) return;
                if (ev == EVENT_SYSTEM_FOREGROUND) { OnForeground(h); return; }
                if (GetAncestor(h, GA_ROOT) != h) return;
                Handle(h, Tag(h), ev == EVENT_OBJECT_SHOW ? "shown" : "renamed");
            }
            catch (Exception) { }
        }

        static void OnForeground(IntPtr h)
        {
            string tag = Tag(h);
            // the window to give the focus back to is never one of Steam's or the game's (2026-10-06: the launch
            // dialog, untitled when it took the focus, was remembered as the PC user's and kept it for 2 s)
            if (tag == null) { userWindow = h; return; }
            if (tag[0] == 'g') { if (GameGuarded()) GiveFocusBack(h, tag.Substring(2)); return; }
            Handle(h, tag, "took the focus");   // Steam's launch dialog is hidden, which hands the focus on (else it is the PC user's own Steam use)
        }

        /// A background process may not take the foreground, unless it shares the input state of the thread
        /// that owns the foreground window for the duration of the call.
        static void GiveFocusBack(IntPtr from, string what)
        {
            IntPtr to = userWindow;
            if (to == IntPtr.Zero || !IsWindow(to) || to == from) return;
            uint pid, owner = GetWindowThreadProcessId(from, out pid), me = GetCurrentThreadId();
            bool attached = AttachThreadInput(me, owner, true);
            try { SetForegroundWindow(to); }
            finally { if (attached) AttachThreadInput(me, owner, false); }
            Note("focus returned to " + Describe(to) + " from " + what + " " + Describe(from));
        }

        static int nullSweeps, lastRefused;
        static long nextNullFix;

        /// No window in front at all: a refused activation of the game (when Steam had passed it the right to take the
        /// foreground) or Steam's launch dialog hidden while it had the focus can leave it so, and the PC user's typing
        /// then goes nowhere until they click (2026-10-06, at a launch). The foreground still belongs to one of those
        /// threads, so it is handed back to the PC user's window through the input state of each game / Steam window
        /// thread in turn (the hook in the game does the same from inside; this is the fallback). True when done.
        static bool ReturnFocusFromNowhere()
        {
            IntPtr to = userWindow;
            if (to == IntPtr.Zero || !IsWindow(to)) return false;
            var game = new List<uint>(); var steam = new List<uint>();
            EnumWindows((w, l) =>
            {
                string t = Tag(w);
                if (t == null) return true;
                uint p, th = GetWindowThreadProcessId(w, out p);
                var list = t[0] == 'g' ? game : steam;
                if (!list.Contains(th) && list.Count < 32) list.Add(th);
                return true;
            }, IntPtr.Zero);
            var threads = new List<uint>(game); threads.AddRange(steam);   // the game's threads first
            uint me = GetCurrentThreadId();
            foreach (uint th in threads)
            {
                bool attached = AttachThreadInput(me, th, true);
                try { SetForegroundWindow(to); }
                finally { if (attached) AttachThreadInput(me, th, false); }
                if (GetForegroundWindow() == to) { Note("focus returned to " + Describe(to) + " (no window had it)"); return true; }
            }
            return false;
        }

        /// UE confines the cursor to its viewport while it has the focus. Only a confinement that lies on a game
        /// window, or is a sliver (a window off-screen clipped to the desktop's edge), is the game's: release it.
        /// Others are the PC user's: Windows itself confines the cursor to the work area while a window is dragged
        /// or resized (released by mistake once on 2026-10-06, when anything outside the window in front counted).
        static void ReleaseCursor()
        {
            RECT c; if (!GetClipCursor(out c)) return;
            int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
            if (c.L <= vx && c.T <= vy && c.R >= vx + vw && c.B >= vy + vh) return;     // not confined
            IntPtr fg = GetForegroundWindow();
            string fgTag = Tag(fg);
            if (fgTag != null && fgTag[0] == 'g') return;                                 // the PC user has the game in front
            bool sliver = c.R - c.L < 50 || c.B - c.T < 50;
            bool onGame = false;
            EnumWindows((w, l) =>
            {
                string t = Tag(w); RECT r;
                if (t != null && t[0] == 'g' && IsWindowVisible(w) && GetWindowRect(w, out r) && c.L < r.R && c.R > r.L && c.T < r.B && c.B > r.T) { onGame = true; return false; }
                return true;
            }, IntPtr.Zero);
            if (!sliver && !onGame) return;                                               // the PC user's (an app, or a window being dragged)
            ClipCursor(IntPtr.Zero);
            Note(string.Format("released the cursor (confined to {0},{1}-{2},{3})", c.L, c.T, c.R, c.B));
        }

        static void Loop()
        {
            var hooks = new List<IntPtr>();
            callback = OnEvent;
            try
            {
                RefreshPids();
                // out-of-context events arrive through this thread's message queue within a millisecond or two
                hooks.Add(SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, IntPtr.Zero, callback, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS));
                hooks.Add(SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, IntPtr.Zero, callback, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS));
                hooks.Add(SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, IntPtr.Zero, callback, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS));
                int tick = 0, seen = generation;
                var sw = Stopwatch.StartNew();
                long next = 0;
                while (running)
                {
                    MsgWaitForMultipleObjects(0, null, false, 50, QS_ALLINPUT);
                    MSG m;
                    while (PeekMessage(out m, IntPtr.Zero, 0, 0, PM_REMOVE)) { TranslateMessage(ref m); DispatchMessage(ref m); }
                    if (sw.ElapsedMilliseconds < next) continue;
                    next = sw.ElapsedMilliseconds + 50;
                    try
                    {
                        // a sweep every 50 ms catches what the events missed; the process list every 250 ms
                        if (tick++ % 5 == 0 || seen != generation) { seen = generation; RefreshPids(); }
                        bool game = false;
                        lock (sync) foreach (string t in pids.Values) if (t[0] == 'g') { game = true; break; }
                        IntPtr fg = GetForegroundWindow();
                        if (fg != IntPtr.Zero && Tag(fg) == null) userWindow = fg;
                        // no window in front for two sweeps while a game runs: see ReturnFocusFromNowhere
                        // (at once when the hook has just refused an activation of the game: then it is ours; else after
                        // two sweeps, so a transient one of the PC user's, e.g. clicking the taskbar, is left alone)
                        int refused = GameHider.ActivationsBlocked;
                        bool justRefused = refused != lastRefused; lastRefused = refused;
                        if (fg != IntPtr.Zero) nullSweeps = 0;
                        else if ((++nullSweeps == 2 || justRefused) && game && GameGuarded() && sw.ElapsedMilliseconds >= nextNullFix)
                            { if (!ReturnFocusFromNowhere()) nextNullFix = sw.ElapsedMilliseconds + 5000; nullSweeps = 0; }
                        EnumWindows((h, l) => { Handle(h, Tag(h), "sweep"); return true; }, IntPtr.Zero);
                        if (game && GameGuarded()) ReleaseCursor();
                    }
                    catch (Exception) { }
                }
            }
            finally { foreach (IntPtr h in hooks) if (h != IntPtr.Zero) UnhookWinEvent(h); }
        }
    }

    /// Mutes the game in the Windows volume mixer (its audio sessions) so a run is silent; the game still
    /// mixes its audio exactly as in play, so CPU work is unchanged. Windows remembers an app's mute across
    /// restarts, so every planned close un-mutes first (BenchBridge Stop-Game), and a game that went down
    /// while muted is noted in bench-audio-muted.txt (runtime folder) and un-muted the next time it runs under the bench.
    public static class GameAudio
    {
        [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IMMDeviceEnumerator
        {
            [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
        }
        [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IMMDeviceCollection
        {
            [PreserveSig] int GetCount(out int count);
            [PreserveSig] int Item(int index, out IMMDevice device);
        }
        [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IMMDevice
        {
            [PreserveSig] int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        }
        [ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IAudioSessionManager2
        {
            [PreserveSig] int GetAudioSessionControl(IntPtr guid, int flags, out IntPtr ctl);
            [PreserveSig] int GetSimpleAudioVolume(IntPtr guid, int flags, out IntPtr vol);
            [PreserveSig] int GetSessionEnumerator(out IAudioSessionEnumerator e);
        }
        [ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IAudioSessionEnumerator
        {
            [PreserveSig] int GetCount(out int count);
            [PreserveSig] int GetSession(int index, out IAudioSessionControl2 session);
        }
        [ComImport, Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IAudioSessionControl2
        {
            [PreserveSig] int GetState(out int state);
            [PreserveSig] int GetDisplayName(out IntPtr name);
            [PreserveSig] int SetDisplayName(IntPtr name, IntPtr ctx);
            [PreserveSig] int GetIconPath(out IntPtr path);
            [PreserveSig] int SetIconPath(IntPtr path, IntPtr ctx);
            [PreserveSig] int GetGroupingParam(out Guid g);
            [PreserveSig] int SetGroupingParam(ref Guid g, IntPtr ctx);
            [PreserveSig] int RegisterAudioSessionNotification(IntPtr n);
            [PreserveSig] int UnregisterAudioSessionNotification(IntPtr n);
            [PreserveSig] int GetSessionIdentifier(out IntPtr id);
            [PreserveSig] int GetSessionInstanceIdentifier(out IntPtr id);
            [PreserveSig] int GetProcessId(out uint pid);
        }
        [ComImport, Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface ISimpleAudioVolume
        {
            [PreserveSig] int SetMasterVolume(float level, IntPtr ctx);
            [PreserveSig] int GetMasterVolume(out float level);
            [PreserveSig] int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, IntPtr ctx);
            [PreserveSig] int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
        }
        [ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IAudioMeterInformation
        {
            [PreserveSig] int GetPeakValue(out float peak);
        }
        [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCo { }

        static readonly object sync = new object();
        static Thread thread;
        static volatile bool running;
        static string[] names = new string[0];
        static readonly List<string> log = new List<string>();
        static readonly HashSet<uint> mutedPids = new HashSet<uint>();

        public static string[] TakeLog() { lock (sync) { string[] r = log.ToArray(); log.Clear(); return r; } }
        static void Note(string m) { lock (sync) log.Add(string.Format("{0:HH:mm:ss.fff} {1}", DateTime.Now, m)); }
        public static bool Running { get { return running; } }
        /// Written when a session gets muted, deleted when an un-mute reached the game: while it exists, Windows
        /// may still have the game muted for its next launch.
        public static string FlagPath;

        /// Visits every render session of the given processes; mute: true/false sets it, null only reads.
        /// Returns how many sessions were found (and set).
        static int Visit(HashSet<uint> target, bool? mute, List<string> changed)
        {
            int muted; float peak; return Visit(target, mute, changed, out muted, out peak);
        }

        /// peak: the loudest of the sessions' meters (0..1)
        static int Visit(HashSet<uint> target, bool? mute, List<string> changed, out int muted, out float peak)
        {
            int n = 0; muted = 0; peak = 0;
            var en = (IMMDeviceEnumerator)new MMDeviceEnumeratorCo();
            IMMDeviceCollection col;
            if (en.EnumAudioEndpoints(0, 1, out col) != 0) return 0;      // eRender, DEVICE_STATE_ACTIVE
            int count; col.GetCount(out count);
            Guid iid = typeof(IAudioSessionManager2).GUID;
            for (int i = 0; i < count; i++)
            {
                IMMDevice dev; if (col.Item(i, out dev) != 0) continue;
                object o; if (dev.Activate(ref iid, 23, IntPtr.Zero, out o) != 0) continue;   // CLSCTX_ALL
                var mgr = (IAudioSessionManager2)o;
                IAudioSessionEnumerator se; if (mgr.GetSessionEnumerator(out se) != 0) continue;
                int c; se.GetCount(out c);
                for (int k = 0; k < c; k++)
                {
                    IAudioSessionControl2 s; if (se.GetSession(k, out s) != 0) continue;
                    try
                    {
                        uint pid; s.GetProcessId(out pid);
                        if (!target.Contains(pid)) continue;
                        n++;
                        var v = (ISimpleAudioVolume)s;
                        bool m; v.GetMute(out m);
                        if (mute.HasValue && m != mute.Value) { v.SetMute(mute.Value, IntPtr.Zero); m = mute.Value; if (changed != null) changed.Add(pid.ToString()); }
                        if (m) muted++;
                        float p; if (((IAudioMeterInformation)s).GetPeakValue(out p) == 0 && p > peak) peak = p;
                    }
                    finally { Marshal.ReleaseComObject(s); }
                }
            }
            return n;
        }

        static HashSet<uint> Pids(string[] processNames)
        {
            var set = new HashSet<uint>();
            foreach (string n in processNames) foreach (Process p in Process.GetProcessesByName(n)) { set.Add((uint)p.Id); p.Dispose(); }
            return set;
        }

        /// A crashing lab game (OblivionPerfFix crash_dump.cpp, 2026-10-21) un-mutes its own sessions for its next launch
        /// and first writes its pid to bench-audio-released.txt next to FlagPath: that process is not muted again. Only
        /// when the file was written after the process started (a stale file never un-mutes a later game).
        static void Released(HashSet<uint> target)
        {
            if (target.Count == 0 || string.IsNullOrEmpty(FlagPath)) return;
            try
            {
                string f = Path.Combine(Path.GetDirectoryName(FlagPath), "bench-audio-released.txt");
                if (!File.Exists(f)) return;
                DateTime written = File.GetLastWriteTime(f);
                foreach (string line in File.ReadAllLines(f))
                {
                    uint pid;
                    if (!uint.TryParse(line.Trim(), out pid) || !target.Contains(pid)) continue;
                    try { using (Process p = Process.GetProcessById((int)pid)) if (written >= p.StartTime) { target.Remove(pid); bool first; lock (sync) first = releasedPids.Add(pid); if (first) Note("the crashing game (pid " + pid + ") restores its own audio: not muted again"); } }
                    catch (Exception) { }
                }
            }
            catch (Exception) { }
        }
        static readonly HashSet<uint> releasedPids = new HashSet<uint>();
        /// Keeps every audio session of these processes muted (checked every 100 ms) until Stop().
        public static void Start(string[] processNames)
        {
            lock (sync)
            {
                names = processNames ?? new string[0];
                if (running) return;
                running = true;
                thread = new Thread(Loop) { IsBackground = true, Name = "GameAudio" };
                thread.SetApartmentState(ApartmentState.MTA);
                thread.Start();
            }
        }

        public static void Stop()
        {
            Thread t;
            lock (sync) { running = false; t = thread; thread = null; }
            if (t != null) t.Join(2000);
        }

        static void Loop()
        {
            while (running)
            {
                try
                {
                    string[] n; lock (sync) n = names;
                    var target = Pids(n);
                    Released(target);
                    if (target.Count > 0)
                    {
                        var changed = new List<string>();
                        Visit(target, true, changed);
                        foreach (string p in changed) { bool first; lock (sync) first = mutedPids.Add(uint.Parse(p)); if (first) Note("muted the game's audio (pid " + p + ")"); }
                        if (changed.Count > 0 && !string.IsNullOrEmpty(FlagPath))
                            try { File.WriteAllText(FlagPath, string.Format("{0:s} the benchmark muted the game (pid {1}) in the volume mixer; Windows keeps that for its next launch until it is un-muted" + Environment.NewLine, DateTime.Now, changed[0])); } catch (Exception) { }
                    }
                }
                catch (Exception e) { Note("audio error: " + e.Message); }
                Thread.Sleep(100);
            }
        }

        /// Audio sessions of these processes: { found, muted } (read on an MTA thread).
        public static int[] State(string[] processNames)
        {
            int found = 0, muted = 0; float peak;
            Query(processNames, out found, out muted, out peak);
            return new int[] { found, muted };
        }

        /// The loudest meter of these processes' audio sessions (0..1; -1: no session). The session meter shows
        /// what the game plays, also while the session is muted in the mixer.
        public static float Peak(string[] processNames)
        {
            int found, muted; float peak;
            Query(processNames, out found, out muted, out peak);
            return found > 0 ? peak : -1;
        }

        static void Query(string[] processNames, out int found, out int muted, out float peak)
        {
            int f = 0, m = 0; float p = 0; Exception error = null;
            var t = new Thread(() => { try { f = Visit(Pids(processNames), null, null, out m, out p); } catch (Exception e) { error = e; } });
            t.SetApartmentState(ApartmentState.MTA);
            t.Start();
            if (!t.Join(5000)) throw new TimeoutException("the audio session query did not finish within 5 s");
            if (error != null) throw error;
            found = f; muted = m; peak = p;
        }

        /// Sets the mute of every audio session of these processes now (on an MTA thread). Returns the number
        /// of sessions found; 0 means the process has no audio session (not running, or audio not started).
        public static int SetMute(string[] processNames, bool mute)
        {
            int found = 0; Exception error = null;
            var t = new Thread(() => { try { found = Visit(Pids(processNames), mute, null); } catch (Exception e) { error = e; } });
            t.SetApartmentState(ApartmentState.MTA);
            t.Start();
            if (!t.Join(5000)) throw new TimeoutException("the audio session query did not finish within 5 s");
            if (error != null) throw error;
            if (!mute && found > 0)
            {
                Note("un-muted the game's audio (" + found + " session(s))");
                if (!string.IsNullOrEmpty(FlagPath)) try { File.Delete(FlagPath); } catch (Exception) { }
            }
            return found;
        }
    }

    /// The game keeps its options as console commands in a string map ("SavedSettings") of the UE save
    /// Save_Settings.sav and applies them at start-up (r.SetRes, Altar.GraphicsOptions.WindowMode, ...).
    /// The benchmark sets the start-up window there before it launches the game (the save guard restores the
    /// file once the game has closed), so the game opens at the profile's size and never at the player's.
    public static class SettingsSave
    {
        static string ReadFString(byte[] d, ref int pos)
        {
            int n = BitConverter.ToInt32(d, pos); pos += 4;
            if (n == 0) return "";
            if (n > 0) { string s = Encoding.GetEncoding(28591).GetString(d, pos, n - 1); pos += n; return s; }
            n = -n; string u = Encoding.Unicode.GetString(d, pos, 2 * (n - 1)); pos += 2 * n; return u;
        }

        static void WriteFString(BinaryWriter w, string s)
        {
            if (s.Length == 0) { w.Write(0); return; }
            bool ascii = true; foreach (char ch in s) if (ch > 127) { ascii = false; break; }
            if (ascii) { w.Write(s.Length + 1); w.Write(Encoding.ASCII.GetBytes(s)); w.Write((byte)0); }
            else { w.Write(-(s.Length + 1)); w.Write(Encoding.Unicode.GetBytes(s)); w.Write((short)0); }
        }

        static int Find(byte[] d, byte[] what)
        {
            for (int i = 0; i + what.Length <= d.Length; i++)
            {
                int k = 0; while (k < what.Length && d[i + k] == what[k]) k++;
                if (k == what.Length) return i;
            }
            return -1;
        }

        /// Reads the SavedSettings map: key -> value, in file order.
        public static List<KeyValuePair<string, string>> Read(string path)
        {
            int a, b; return Parse(File.ReadAllBytes(path), out a, out b);
        }

        // mapStart = offset of the int64 size field; dataStart = first byte of the map's data
        static List<KeyValuePair<string, string>> Parse(byte[] d, out int sizeField, out int dataStart)
        {
            byte[] tag = new byte[4 + 14];
            BitConverter.GetBytes(14).CopyTo(tag, 0); Encoding.ASCII.GetBytes("SavedSettings").CopyTo(tag, 4);
            int pos = Find(d, tag);
            if (pos < 0) throw new InvalidDataException("no SavedSettings property");
            pos += tag.Length;
            if (ReadFString(d, ref pos) != "MapProperty") throw new InvalidDataException("SavedSettings is not a MapProperty");
            sizeField = pos;
            long size = BitConverter.ToInt64(d, pos); pos += 8;
            if (ReadFString(d, ref pos) != "StrProperty" || ReadFString(d, ref pos) != "StrProperty") throw new InvalidDataException("SavedSettings is not a string map");
            if (d[pos++] != 0) throw new InvalidDataException("unexpected property GUID");
            dataStart = pos;
            if (BitConverter.ToInt32(d, pos) != 0) throw new InvalidDataException("map with removed keys");
            int count = BitConverter.ToInt32(d, pos + 4); pos += 8;
            var list = new List<KeyValuePair<string, string>>();
            for (int i = 0; i < count; i++) { string k = ReadFString(d, ref pos); string v = ReadFString(d, ref pos); list.Add(new KeyValuePair<string, string>(k, v)); }
            if (pos - dataStart != size) throw new InvalidDataException(string.Format("map size {0} does not match its {1} bytes of entries", size, pos - dataStart));
            return list;
        }

        /// Sets (or adds) entries of the SavedSettings map. Returns one line per change ("key: old -> new").
        public static string[] Set(string path, System.Collections.IDictionary values)
        {
            byte[] d = File.ReadAllBytes(path);
            int sizeField, dataStart;
            var list = Parse(d, out sizeField, out dataStart);
            long oldSize = BitConverter.ToInt64(d, sizeField);
            var changes = new List<string>();
            foreach (System.Collections.DictionaryEntry e in values)
            {
                string k = Convert.ToString(e.Key, CultureInfo.InvariantCulture), v = Convert.ToString(e.Value, CultureInfo.InvariantCulture);
                int i = list.FindIndex(x => x.Key == k);
                if (i >= 0) { if (list[i].Value != v) { changes.Add(k + ": " + list[i].Value + " -> " + v); list[i] = new KeyValuePair<string, string>(k, v); } }
                else { changes.Add(k + ": (none) -> " + v); list.Add(new KeyValuePair<string, string>(k, v)); }
            }
            if (changes.Count == 0) return new string[0];
            var data = new MemoryStream();
            using (var w = new BinaryWriter(data, Encoding.ASCII, true))
            {
                w.Write(0); w.Write(list.Count);
                foreach (var kv in list) { WriteFString(w, kv.Key); WriteFString(w, kv.Value); }
            }
            var o = new MemoryStream();
            o.Write(d, 0, sizeField);
            o.Write(BitConverter.GetBytes(data.Length), 0, 8);
            o.Write(d, sizeField + 8, dataStart - sizeField - 8);
            data.WriteTo(o);
            long rest = dataStart + oldSize;
            o.Write(d, (int)rest, d.Length - (int)rest);
            string tmp = path + ".bench-tmp";
            File.WriteAllBytes(tmp, o.ToArray());
            int s2, d2; Parse(File.ReadAllBytes(tmp), out s2, out d2);        // re-read before replacing the real file
            File.Copy(tmp, path, true); File.Delete(tmp);
            return changes.ToArray();
        }
    }

    // ---------------------------------------------------------------- plots

    public static class Plot
    {
        /// Frame time over time as a PNG. rows = { offsetMs, frameMs } (FrameLog.Series); marks are
        /// vertical lines at the given offsets (ms) with labels. The y axis is square-root scaled so
        /// normal frames and multi-second stalls both stay readable.
        public static void FrameTimes(double[][] rows, string path, string title, double[] marks, string[] markLabels)
        {
            const int W = 1100, H = 320, L = 58, RM = 16, T = 30, B = 34;
            double tMax = 1000, yMax = 50;
            foreach (double[] r in rows) { if (r[0] > tMax) tMax = r[0]; if (r[1] > yMax) yMax = r[1]; }
            if (marks != null) foreach (double m in marks) if (m > tMax) tMax = m;
            double sMax = Math.Sqrt(yMax * 1.05);
            using (Bitmap bmp = new Bitmap(W, H, PixelFormat.Format24bppRgb))
            using (Graphics g = Graphics.FromImage(bmp))
            using (Font font = new Font("Segoe UI", 9f))
            using (Pen grid = new Pen(Color.FromArgb(225, 225, 225)))
            using (Pen ok = new Pen(Color.FromArgb(60, 120, 200)))
            using (Pen slow = new Pen(Color.FromArgb(235, 150, 30)))
            using (Pen bad = new Pen(Color.FromArgb(210, 40, 40), 2f))
            using (Pen mark = new Pen(Color.FromArgb(40, 160, 70)))
            {
                g.Clear(Color.White);
                g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
                int pw = W - L - RM, ph = H - T - B;
                g.DrawString(title, font, Brushes.Black, L, 6);
                double[] levels = { 16.7, 33.3, 50, 100, 250, 500, 1000, 2000, 5000, 10000 };
                foreach (double lv in levels)
                {
                    if (lv > yMax * 1.05) break;
                    int y = T + ph - (int)(ph * Math.Sqrt(lv) / sMax);
                    g.DrawLine(grid, L, y, L + pw, y);
                    g.DrawString(lv.ToString(lv < 100 ? "0.#" : "0", CultureInfo.InvariantCulture) + " ms", font, Brushes.Gray, 2, y - 8);
                }
                double step = tMax > 40000 ? 10000 : tMax > 16000 ? 5000 : tMax > 6000 ? 2000 : 1000;
                for (double t = 0; t <= tMax; t += step)
                {
                    int x = L + (int)(pw * t / tMax);
                    g.DrawLine(grid, x, T, x, T + ph);
                    g.DrawString((t / 1000).ToString("0", CultureInfo.InvariantCulture) + " s", font, Brushes.Gray, x - 10, T + ph + 6);
                }
                foreach (double[] r in rows)
                {
                    int x = L + (int)(pw * r[0] / tMax);
                    int y = T + ph - (int)(ph * Math.Sqrt(r[1]) / sMax);
                    g.DrawLine(r[1] > 100 ? bad : r[1] > 33.4 ? slow : ok, x, T + ph, x, y);
                }
                for (int i = 0; marks != null && i < marks.Length; i++)
                {
                    int x = L + (int)(pw * marks[i] / tMax);
                    g.DrawLine(mark, x, T, x, T + ph);
                    if (markLabels != null && i < markLabels.Length) g.DrawString(markLabels[i], font, Brushes.DarkGreen, x + 3, T + 2);
                }
                g.DrawRectangle(Pens.Gray, L, T, pw, ph);
                bmp.Save(path, ImageFormat.Png);
            }
        }
    }

    // ---------------------------------------------------------------- game watchdog

    /// Background thread that notices, within a second or two, when the game is gone or no longer
    /// usable, so the runner never waits on a game that cannot answer:
    ///   exited    the process ended (crash or quit)
    ///   crashed   UE's CrashReportClient or Windows error reporting opened for it
    ///   dialog    the game shows a message box (fatal error, "video driver crashed", ...)
    ///   hung      its window has not answered Windows messages for HungSec seconds
    ///   stalled   the OblvBench mod's heartbeat (bench-heartbeat.txt in the runtime folder, rewritten every second from
    ///             the game thread's frame loop) has not advanced for StallSec seconds
    ///   driver    a display driver timeout (TDR, System log "Display" event 4101) since the watch began
    /// Problem is null while all is well. Dismiss() closes what such a failure leaves on screen (crash
    /// reporter, error-report and AMD driver pop-ups that appeared since the watch began, the game's
    /// message boxes) without sending any report.
    public static class GameWatch
    {
        delegate bool EnumProc(IntPtr h, IntPtr l);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
        [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc f, IntPtr l);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
        [DllImport("user32.dll")] static extern bool IsHungAppWindow(IntPtr h);
        [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
        const uint WM_CLOSE = 0x10;

        static readonly string[] AmdProcesses = { "RadeonSoftware", "AMDRSServ", "amdow", "AMDRSSrcExt", "AMDBugReportTool", "AMDBugReportForm", "cncmd", "AMDSoftwareInstaller" };

        static readonly object sync = new object();
        static Thread thread;
        static volatile bool running;
        static int pid;
        static Process proc;
        static DateTime since;                 // local time the watch began
        static string heartbeatPath;
        static string problem, kind;
        static readonly List<string> log = new List<string>();
        static HashSet<IntPtr> amdBefore = new HashSet<IntPtr>();
        static long lastFrames = -1;
        static DateTime lastAdvance;
        static DateTime hungSince = DateTime.MinValue;
        static DateTime nextEventCheck;
        static string amdNote;                 // an AMD pop-up or a GPU-timeout report since the watch began: explains a failure

        public static int HungSec = 20, StallSec = 30;   // a game frozen by a debugger (like a driver hang) never looks "hung" to Windows: the stall catches it
        /// The frame loop may stop legitimately for a while (a start-up shader compile): stall checks pause until this time (UTC).
        public static DateTime StallGraceUntil = DateTime.MinValue;

        public static string Problem { get { lock (sync) return problem; } }
        public static string Kind { get { lock (sync) return kind; } }
        public static int Pid { get { return pid; } }
        public static bool Running { get { return running; } }
        /// Seconds since the heartbeat last advanced (-1 before the first one).
        public static double HeartbeatAge { get { lock (sync) return lastFrames < 0 ? -1 : (DateTime.UtcNow - lastAdvance).TotalSeconds; } }
        public static long HeartbeatFrames { get { lock (sync) return lastFrames; } }

        public static void Start(int processId, string heartbeat)
        {
            Stop();
            lock (sync)
            {
                pid = processId; heartbeatPath = heartbeat; problem = null; kind = null; amdNote = null;
                lastFrames = -1; lastAdvance = DateTime.UtcNow; hungSince = DateTime.MinValue;
                since = DateTime.Now; nextEventCheck = DateTime.UtcNow;
                try { proc = Process.GetProcessById(processId); } catch (Exception) { proc = null; }
                amdBefore = AmdWindows();
                running = true;
                thread = new Thread(Loop) { IsBackground = true, Name = "GameWatch" };
                thread.Start();
            }
        }

        public static void Stop()
        {
            Thread t;
            lock (sync) { running = false; t = thread; thread = null; }
            if (t != null) t.Join(3000);
        }

        public static string[] TakeLog() { lock (sync) { string[] r = log.ToArray(); log.Clear(); return r; } }

        static void Note(string k, string what)
        {
            lock (sync)
            {
                if (problem != null) return;
                if (amdNote != null) what += " (" + amdNote + ")";
                problem = what; kind = k;
                log.Add(string.Format("{0:HH:mm:ss} {1}: {2}", DateTime.Now, k, what));
            }
        }

        static void Loop()
        {
            while (running)
            {
                try { Check(); } catch (Exception e) { lock (sync) log.Add("watch error: " + e.Message); }
                Thread.Sleep(500);
            }
        }

        static void Check()
        {
            if (problem != null) return;
            // exited
            bool exited = proc == null;
            if (!exited) { try { proc.Refresh(); exited = proc.HasExited; } catch (Exception) { exited = true; } }
            // the hook in the game (BenchHook.cpp) keeps UE's crash reporter from starting and counts it: that is a crash
            bool reporterBlocked = GameHider.CrashReportsBlockedFor(pid) > 0;
            if (exited)
            {
                string code = "";
                try { if (proc != null) code = " (exit code 0x" + proc.ExitCode.ToString("X") + ")"; } catch (Exception) { }
                if (reporterBlocked) Note("crashed", "the game crashed: UE wrote its crash report, its crash reporter was kept from starting" + code);
                else Note(CrashReporterOpen() ? "crashed" : "exited", "the game process ended" + code);
                return;
            }
            if (reporterBlocked) { Note("crashed", "the game crashed: UE wrote its crash report, its crash reporter was kept from starting"); return; }
            // crash reporter / Windows error reporting
            if (CrashReporterOpen()) { Note("crashed", "UE's crash reporter opened"); return; }
            // a message box of the game (fatal error, device lost, ...)
            string box = GameMessageBox();
            // lab (-CpuLimit below the game's minimum spec): the game's own "Thank you for installing" box, which a player with
            // such a CPU answers with OK, is answered the same way (IDOK) a few times instead of ending the run
            if (box != null && AcceptBoxPrefix != null && box.StartsWith(AcceptBoxPrefix, StringComparison.Ordinal) && AcceptedBoxes < 20)
            { AcceptedBoxes++; AnswerGameBoxOk(box); return; }
            if (box != null) { Note("dialog", "the game shows a message: " + box); return; }
            // hung window: the game's own visible UnrealWindow (Process.MainWindowHandle can be an early helper
            // window that never pumps messages: a restarted game was called "hung" while it ran fine). A window
            // only counts as hung while the frame heartbeat has stopped too (the game thread pumps the messages).
            ReadHeartbeat();
            IntPtr h = GameWindow();
            bool framesStopped = lastFrames < 0 || (DateTime.UtcNow - lastAdvance).TotalSeconds >= 10;
            if (h != IntPtr.Zero && IsHungAppWindow(h) && framesStopped)
            {
                if (hungSince == DateTime.MinValue) hungSince = DateTime.UtcNow;
                else if ((DateTime.UtcNow - hungSince).TotalSeconds >= HungSec) { Note("hung", string.Format("the game window has not responded for {0:F0} s", (DateTime.UtcNow - hungSince).TotalSeconds)); return; }
            }
            else hungSince = DateTime.MinValue;
            // heartbeat of the frame loop
            if (lastFrames >= 0 && DateTime.UtcNow > StallGraceUntil && (DateTime.UtcNow - lastAdvance).TotalSeconds >= StallSec)
            {
                Note("stalled", string.Format("no game frame for {0:F0} s (frame {1})", (DateTime.UtcNow - lastAdvance).TotalSeconds, lastFrames));
                return;
            }
            // driver timeouts (the System log is slow to query: every 5 s)
            if (DateTime.UtcNow >= nextEventCheck)
            {
                nextEventCheck = DateTime.UtcNow.AddSeconds(5);
                string tdr = DisplayTimeoutSince(since);
                if (tdr != null) { Note("driver", tdr); return; }
                if (amdNote == null) { string lk = LiveKernelEventSince(since); if (lk != null) lock (sync) { amdNote = lk; log.Add(string.Format("{0:HH:mm:ss} {1}", DateTime.Now, lk)); } }
                if (amdNote == null)
                    foreach (IntPtr w in AmdWindows())
                        if (!amdBefore.Contains(w)) { lock (sync) { amdNote = "AMD pop-up '" + WindowText(w) + "', probably a driver timeout"; log.Add(string.Format("{0:HH:mm:ss} {1}", DateTime.Now, amdNote)); } break; }
            }
        }

        static void ReadHeartbeat()
        {
            if (string.IsNullOrEmpty(heartbeatPath) || !File.Exists(heartbeatPath)) return;
            string text;
            try
            {
                using (var fs = new FileStream(heartbeatPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var r = new StreamReader(fs)) text = r.ReadToEnd();
            }
            catch (Exception) { return; }
            string[] p = text.Trim().Split(' ');
            long frames;
            if (p.Length < 2 || !long.TryParse(p[1], out frames)) return;
            lock (sync)
            {
                if (frames != lastFrames) { lastFrames = frames; lastAdvance = DateTime.UtcNow; }
            }
        }

        static bool CrashReporterOpen()
        {
            foreach (Process p in Process.GetProcessesByName("CrashReportClient"))
            {
                bool mine = false;
                try { mine = p.StartTime >= since.AddSeconds(-2); } catch (Exception) { }
                p.Dispose();
                if (mine) return true;
            }
            foreach (Process p in Process.GetProcessesByName("WerFault"))
            {
                bool mine = false;
                try { mine = p.StartTime >= since && (p.MainWindowTitle.IndexOf("Oblivion", StringComparison.OrdinalIgnoreCase) >= 0); } catch (Exception) { }
                p.Dispose();
                if (mine) return true;
            }
            return false;
        }

        /// The game's visible top-level UnrealWindow, or IntPtr.Zero.
        static IntPtr GameWindow() { return FindGameWindow(pid); }

        /// The visible top-level UnrealWindow of that process, or IntPtr.Zero. Use this, not Process.MainWindowHandle:
        /// that skips owned windows (the game's window under Run-Benchmark -Record, 2026-10-11) and can be an early
        /// helper window.
        public static IntPtr FindGameWindow(int processId)
        {
            IntPtr found = IntPtr.Zero;
            EnumWindows((w, l) =>
            {
                uint wp; GetWindowThreadProcessId(w, out wp);
                if (wp == (uint)processId && IsWindowVisible(w) && ClassOf(w) == "UnrealWindow") { found = w; return false; }
                return true;
            }, IntPtr.Zero);
            return found;
        }

        static string WindowText(IntPtr h)
        {
            var t = new StringBuilder(512); GetWindowText(h, t, 512); return t.ToString().Trim();
        }

        static string ClassOf(IntPtr h)
        {
            var c = new StringBuilder(256); GetClassName(h, c, 256); return c.ToString();
        }

        /// Text of a visible message box (#32770) owned by the game process, or null.
        /// Lab: a game message box whose text starts with this is answered with OK (see the watch loop); null = none.
        public static string AcceptBoxPrefix = null;
        public static int AcceptedBoxes = 0;
        public static string AcceptLog = "";
        static void AnswerGameBoxOk(string box)
        {
            EnumWindows((h, l) =>
            {
                uint wp; GetWindowThreadProcessId(h, out wp);
                if (wp != (uint)pid || !IsWindowVisible(h) || ClassOf(h) != "#32770") return true;
                var buttons = new List<string>();
                EnumChildWindows(h, (c, l2) =>
                {
                    if (ClassOf(c) != "Button") return true;
                    string t = WindowText(c); buttons.Add(t);
                    if (t.Replace("&", "").Trim().Equals("OK", StringComparison.OrdinalIgnoreCase)) PostMessage(c, 0x00F5 /* BM_CLICK */, IntPtr.Zero, IntPtr.Zero);
                    return true;
                }, IntPtr.Zero);
                bool ok = PostMessage(h, 0x0111 /* WM_COMMAND */, new IntPtr(1) /* IDOK */, IntPtr.Zero);
                if (AcceptLog.Length < 2000) AcceptLog += DateTime.Now.ToString("HH:mm:ss.fff") + " '" + box + "' buttons [" + string.Join(",", buttons.ToArray()) + "] IDOK posted " + ok + "; ";
                return true;
            }, IntPtr.Zero);
        }

        static string GameMessageBox()
        {
            string found = null;
            EnumWindows((h, l) =>
            {
                uint wp; GetWindowThreadProcessId(h, out wp);
                if (wp != (uint)pid || !IsWindowVisible(h) || ClassOf(h) != "#32770") return true;
                var parts = new List<string> { WindowText(h) };
                EnumChildWindows(h, (c, l2) => { if (ClassOf(c) == "Static") { string s = WindowText(c); if (s.Length > 0) parts.Add(s); } return true; }, IntPtr.Zero);
                found = string.Join(" | ", parts.ToArray());
                return false;
            }, IntPtr.Zero);
            return found;
        }

        static HashSet<IntPtr> AmdWindows()
        {
            var pids = new HashSet<uint>();
            foreach (string n in AmdProcesses) foreach (Process p in Process.GetProcessesByName(n)) { pids.Add((uint)p.Id); p.Dispose(); }
            var set = new HashSet<IntPtr>();
            if (pids.Count == 0) return set;
            EnumWindows((h, l) => { uint wp; GetWindowThreadProcessId(h, out wp); if (pids.Contains(wp) && IsWindowVisible(h)) set.Add(h); return true; }, IntPtr.Zero);
            return set;
        }

        /// A display driver timeout (TDR) since `from` in the System log ("Display" event 4101; not written on
        /// every Windows build, this one has none), else null.
        public static string DisplayTimeoutSince(DateTime from)
        {
            string r = FirstEvent("System", "*[System[Provider[@Name='Display'] and (EventID=4101) and TimeCreated[@SystemTime>='" + Utc(from) + "']]]", null);
            return r == null ? null : "display driver timeout (TDR) " + r;
        }

        /// A Windows Error Reporting "LiveKernelEvent" 141/117 (GPU engine timeout) written since `from`, else null.
        /// WER often writes queued reports of older timeouts later (all at once after a boot), so this is only a
        /// note that explains another failure, never a failure by itself.
        public static string LiveKernelEventSince(DateTime from)
        {
            string r = FirstEvent("Application", "*[System[Provider[@Name='Windows Error Reporting'] and (EventID=1001) and TimeCreated[@SystemTime>='" + Utc(from) + "']]]",
                                  new Regex(@"LiveKernelEvent.*P1: (141|117)\b", RegexOptions.Singleline));
            return r == null ? null : "GPU timeout reported (LiveKernelEvent) " + r;
        }

        static string Utc(DateTime t) { return t.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.000Z"); }

        static string FirstEvent(string logName, string xpath, Regex mustMatch)
        {
            try
            {
                var query = new System.Diagnostics.Eventing.Reader.EventLogQuery(logName, System.Diagnostics.Eventing.Reader.PathType.LogName, xpath);
                using (var reader = new System.Diagnostics.Eventing.Reader.EventLogReader(query))
                {
                    for (var ev = reader.ReadEvent(); ev != null; ev = reader.ReadEvent())
                    {
                        using (ev)
                        {
                            string msg = "";
                            try { msg = ev.FormatDescription() ?? ""; } catch (Exception) { }
                            if (mustMatch != null && !mustMatch.IsMatch(msg)) continue;
                            msg = Regex.Replace(msg, @"\s+", " ").Trim();
                            if (msg.Length > 160) msg = msg.Substring(0, 160);
                            return string.Format("at {0:HH:mm:ss}: {1}", ev.TimeCreated, msg);
                        }
                    }
                }
            }
            catch (Exception) { }
            return null;
        }

        /// Closes what a failed game leaves behind, without sending anything: UE's crash reporter and Windows
        /// error-report dialogs started since the watch began (ended), the game's own message boxes (WM_CLOSE),
        /// and AMD pop-ups (driver timeout, bug report tool) that appeared since the watch began (WM_CLOSE).
        /// Returns one line per action.
        public static string[] Dismiss()
        {
            var done = new List<string>();
            foreach (Process p in Process.GetProcessesByName("CrashReportClient"))
            {
                try { if (p.StartTime >= since.AddSeconds(-2)) { p.Kill(); done.Add("ended UE crash reporter (pid " + p.Id + ")"); } } catch (Exception) { }
                p.Dispose();
            }
            foreach (Process p in Process.GetProcessesByName("WerFault"))
            {
                try
                {
                    if (p.StartTime >= since && p.MainWindowTitle.IndexOf("Oblivion", StringComparison.OrdinalIgnoreCase) >= 0)
                    { done.Add("ended Windows error reporting '" + p.MainWindowTitle + "'"); p.Kill(); }
                }
                catch (Exception) { }
                p.Dispose();
            }
            EnumWindows((h, l) =>
            {
                uint wp; GetWindowThreadProcessId(h, out wp);
                if (wp == (uint)pid && IsWindowVisible(h) && ClassOf(h) == "#32770") { done.Add("closed game message '" + WindowText(h) + "'"); PostMessage(h, WM_CLOSE, IntPtr.Zero, IntPtr.Zero); }
                return true;
            }, IntPtr.Zero);
            foreach (IntPtr h in AmdWindows())
            {
                if (amdBefore.Contains(h)) continue;
                done.Add("closed AMD window '" + WindowText(h) + "' (" + ClassOf(h) + ")");
                PostMessage(h, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
            }
            return done.ToArray();
        }
    }

    // ---------------------------------------------------------------- crash UI guard (outside the game)

    /// Keeps the crash UI of one benchmark game off the PC user's screen from outside, for as long as the game runs and
    /// LingerSec after it has gone. tools\Crash-Guard.ps1 runs it in a hidden process of its own, so it also covers
    /// hand probing between benchmark runs. It backs up the hook inside the game (BenchHook.cpp: no Windows error-report
    /// dialogs, UE's crash reporter never starts, the game's message boxes off-screen), e.g. for a game the hook missed:
    ///   - UE's crash reporter (CrashReportClient started by the game): ended at once, before it can show or send
    ///     anything (it needs about a second to open its window; a window it does show is hidden first);
    ///   - Windows error reporting (WerFault for the game's process id): its windows hidden, the process ended;
    ///   - AMD's bug report tool started while the game ran (it follows driver timeouts): hidden and closed;
    ///   - the game's own message boxes: moved off-screen at once and closed after DialogCloseSec (the runner's
    ///     watchdog reads them first when a run is waiting); their text goes to the log;
    ///   - the game's main window on a monitor while the PC user has not asked to see it: moved off-screen.
    /// Windows are caught as they are created or shown (WinEvent hooks over all processes, a few ms); processes by a
    /// Toolhelp snapshot every 500 ms (pid, parent, name: cheap). Nothing in the event path can block (no WMI, no
    /// window messages sent): a WMI query there hung the first version at a fail-fast crash (2026-10-06). Log: LogPath.
    public static class CrashGuard
    {
        delegate void WinEventProc(IntPtr hook, uint ev, IntPtr h, int idObject, int idChild, uint thread, uint time);
        delegate bool EnumProc(IntPtr h, IntPtr l);
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
        [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr h; public uint msg; public IntPtr w, l; public uint time; public int x, y; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct PROCESSENTRY32W
        {
            public uint dwSize, cntUsage, th32ProcessID; public IntPtr th32DefaultHeapID; public uint th32ModuleID, cntThreads, th32ParentProcessID;
            public int pcPriClassBase; public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
        }
        [StructLayout(LayoutKind.Sequential)] struct UNICODE_STRING { public ushort Length, MaximumLength; public IntPtr Buffer; }
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32FirstW(IntPtr snap, ref PROCESSENTRY32W e);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32NextW(IntPtr snap, ref PROCESSENTRY32W e);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("ntdll.dll")] static extern int NtQueryInformationProcess(IntPtr process, int infoClass, IntPtr info, int size, out int returned);
        [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr module, WinEventProc proc, uint pid, uint thread, uint flags);
        [DllImport("user32.dll")] static extern bool UnhookWinEvent(IntPtr hook);
        [DllImport("user32.dll")] static extern uint MsgWaitForMultipleObjects(uint n, IntPtr[] handles, bool all, uint ms, uint mask);
        [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint min, uint max, uint remove);
        [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
        [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG m);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
        [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc f, IntPtr l);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
        [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
        [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr h, int cmd);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
        [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
        const uint EVENT_OBJECT_CREATE = 0x8000, EVENT_OBJECT_SHOW = 0x8002, WINEVENT_OUTOFCONTEXT = 0, WINEVENT_SKIPOWNPROCESS = 2;
        const uint GA_ROOT = 2, QS_ALLINPUT = 0x4FF, PM_REMOVE = 1, WM_CLOSE = 0x10, TH32CS_SNAPPROCESS = 2;
        const uint SWP_NOSIZE = 1, SWP_NOZORDER = 4, SWP_NOACTIVATE = 0x10, SWP_ASYNCWINDOWPOS = 0x4000;
        const int SW_HIDE = 0;

        public static int LingerSec = 20, DialogCloseSec = 5;
        public static string LogPath;

        static int gamePid;
        static HashSet<int> before = new HashSet<int>();                                    // processes that ran when the guard started
        static Dictionary<int, string> kind = new Dictionary<int, string>();                 // pid -> "crc" | "wer" | "amd" | "" (none of ours)
        static readonly HashSet<int> checkedWer = new HashSet<int>();
        static readonly HashSet<int> ended = new HashSet<int>();
        static readonly Dictionary<IntPtr, DateTime> dialogs = new Dictionary<IntPtr, DateTime>();   // game message box -> close due
        static readonly HashSet<IntPtr> logged = new HashSet<IntPtr>();
        static readonly Stopwatch clock = Stopwatch.StartNew();
        static long lastSnap = -1000;
        static WinEventProc callback;

        static void Log(string m)
        {
            string line = string.Format("{0:yyyy-MM-dd HH:mm:ss.fff} [{1}] {2}", DateTime.Now, Process.GetCurrentProcess().Id, m);
            try { if (!string.IsNullOrEmpty(LogPath)) File.AppendAllText(LogPath, line + Environment.NewLine); } catch (Exception) { }
        }

        static string ClassOf(IntPtr h) { var c = new StringBuilder(256); GetClassName(h, c, 256); return c.ToString(); }
        static string TextOf(IntPtr h) { var t = new StringBuilder(512); GetWindowText(h, t, 512); return t.ToString().Trim(); }

        /// Title and static texts (a message box's message; GetWindowText never waits on another process's window).
        static string Describe(IntPtr h)
        {
            var parts = new List<string> { "'" + TextOf(h) + "' (" + ClassOf(h) + ")" };
            EnumChildWindows(h, (c, l) => { if (ClassOf(c) == "Static") { string s = TextOf(c); if (s.Length > 0) parts.Add(s.Replace("\r", " ").Replace("\n", " ")); } return true; }, IntPtr.Zero);
            return string.Join(" | ", parts.ToArray());
        }

        static bool OnScreen(IntPtr h)
        {
            RECT r; if (!GetWindowRect(h, out r)) return false;
            int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
            return r.R > vx && r.L < vx + vw && r.B > vy && r.T < vy + vh;
        }

        static void MoveOffScreen(IntPtr h)
        {
            SetWindowPos(h, IntPtr.Zero, GetSystemMetrics(76) + GetSystemMetrics(78) + 64, GetSystemMetrics(77), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }

        /// The command line of a process (NtQueryInformationProcess, ProcessCommandLineInformation): no WMI, no waiting.
        static string CommandLineOf(int pid)
        {
            IntPtr hp = OpenProcess(0x1000, false, pid);   // PROCESS_QUERY_LIMITED_INFORMATION
            if (hp == IntPtr.Zero) return "";
            IntPtr buf = Marshal.AllocHGlobal(8192);
            try
            {
                int got;
                if (NtQueryInformationProcess(hp, 60, buf, 8192, out got) != 0) return "";
                var us = (UNICODE_STRING)Marshal.PtrToStructure(buf, typeof(UNICODE_STRING));
                return us.Buffer == IntPtr.Zero ? "" : Marshal.PtrToStringUni(us.Buffer, us.Length / 2);
            }
            catch (Exception) { return ""; }
            finally { Marshal.FreeHGlobal(buf); CloseHandle(hp); }
        }

        /// Which processes belong to the game's crash UI (Toolhelp snapshot: pid, parent, exe name).
        static void Snapshot(bool first)
        {
            lastSnap = clock.ElapsedMilliseconds;
            var now = new Dictionary<int, string>();
            IntPtr snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == IntPtr.Zero || snap == new IntPtr(-1)) return;
            try
            {
                var e = new PROCESSENTRY32W { dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32W)) };
                for (bool ok = Process32FirstW(snap, ref e); ok; ok = Process32NextW(snap, ref e))
                {
                    int pid = (int)e.th32ProcessID;
                    if (first) { before.Add(pid); continue; }
                    string exe = e.szExeFile ?? "", k = "";
                    if (exe.Equals("CrashReportClient.exe", StringComparison.OrdinalIgnoreCase) && (int)e.th32ParentProcessID == gamePid) k = "crc";
                    else if (exe.Equals("WerFault.exe", StringComparison.OrdinalIgnoreCase) && !before.Contains(pid))
                    {
                        string prev;
                        if (kind.TryGetValue(pid, out prev)) k = prev;
                        else if (checkedWer.Add(pid) && Regex.IsMatch(CommandLineOf(pid), @"-p\s+" + gamePid + @"\b")) k = "wer";
                    }
                    else if ((exe.Equals("AMDBugReportTool.exe", StringComparison.OrdinalIgnoreCase) || exe.Equals("AMDBugReportForm.exe", StringComparison.OrdinalIgnoreCase)) && !before.Contains(pid)) k = "amd";   // the form (its visible window) is a separate process in newer drivers (2026-10-07)
                    now[pid] = k;
                }
            }
            finally { CloseHandle(snap); }
            if (!first) kind = now;
        }

        static string KindOf(int pid)
        {
            string k;
            if (kind.TryGetValue(pid, out k)) return k;
            if (clock.ElapsedMilliseconds - lastSnap >= 50) { Snapshot(false); if (kind.TryGetValue(pid, out k)) return k; }   // a new process
            return "";
        }

        static void End(int pid, string what)
        {
            if (!ended.Add(pid)) return;
            try { using (Process p = Process.GetProcessById(pid)) { p.Kill(); } Log("ended " + what + " (pid " + pid + ")"); }
            catch (Exception e) { Log("could not end " + what + " (pid " + pid + "): " + e.Message); }
        }

        /// A top-level window was created, shown or found by the sweep.
        static void Handle(IntPtr h, string why)
        {
            if (h == IntPtr.Zero || GetAncestor(h, GA_ROOT) != h) return;
            uint wp; GetWindowThreadProcessId(h, out wp);
            int pid = (int)wp;
            if (pid == 0) return;
            if (pid == gamePid)
            {
                if (GameHider.ShowRequested || !IsWindowVisible(h)) return;
                string cls = ClassOf(h);
                if (cls == "UnrealWindow")
                {
                    if (!OnScreen(h)) return;
                    MoveOffScreen(h);
                    if (logged.Add(h)) Log("moved the game window off a monitor (" + why + ")");
                    return;
                }
                if (OnScreen(h)) MoveOffScreen(h);
                if (!dialogs.ContainsKey(h)) { dialogs[h] = DateTime.UtcNow.AddSeconds(DialogCloseSec); Log("game window kept off-screen (" + why + "): " + Describe(h)); }
                return;
            }
            string k = KindOf(pid);
            if (k == "") return;
            if (IsWindowVisible(h)) ShowWindowAsync(h, SW_HIDE);
            if (logged.Add(h)) Log(string.Format("hid {0} window ({1}): {2}", k, why, Describe(h)));
            if (k == "crc") End(pid, "UE crash reporter");
            else if (k == "wer") End(pid, "Windows error reporting for the game");
            else PostMessage(h, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }

        static void OnEvent(IntPtr hook, uint ev, IntPtr h, int idObject, int idChild, uint thread, uint time)
        {
            try { if (idObject == 0 && idChild == 0) Handle(h, ev == EVENT_OBJECT_SHOW ? "shown" : "created"); } catch (Exception) { }
        }

        static void Poll()
        {
            Snapshot(false);
            foreach (var kv in kind)
            {
                if (kv.Value == "crc") End(kv.Key, "UE crash reporter");
                else if (kv.Value == "wer") End(kv.Key, "Windows error reporting for the game");
            }
            EnumWindows((h, l) => { if (IsWindowVisible(h)) Handle(h, "sweep"); return true; }, IntPtr.Zero);
            foreach (IntPtr h in new List<IntPtr>(dialogs.Keys))
            {
                if (!IsWindow(h)) { dialogs.Remove(h); continue; }
                if (DateTime.UtcNow >= dialogs[h] && !GameHider.ShowRequested)
                {
                    Log("closed game window " + Describe(h));
                    PostMessage(h, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
                    dialogs[h] = DateTime.UtcNow.AddSeconds(30);   // once more in 30 s if it is still there
                }
            }
        }

        /// Blocks until the game has gone and LingerSec have passed.
        public static void Run(int pid)
        {
            gamePid = pid;
            Process game = null;
            DateTime gameStart = DateTime.Now;
            try { game = Process.GetProcessById(pid); gameStart = game.StartTime; } catch (Exception) { }
            Snapshot(true);
            before.Remove(pid);
            Log(string.Format("guarding game pid {0} (started {1:HH:mm:ss}); crash guard in the game: flags {2}", pid, gameStart, GameHider.CrashGuardFlags));
            callback = OnEvent;
            var hooks = new List<IntPtr>();
            hooks.Add(SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, IntPtr.Zero, callback, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS));
            hooks.Add(SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, IntPtr.Zero, callback, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS));
            DateTime goneAt = DateTime.MinValue;
            long next = 0;
            try
            {
                while (true)
                {
                    MsgWaitForMultipleObjects(0, null, false, 50, QS_ALLINPUT);
                    MSG m;
                    while (PeekMessage(out m, IntPtr.Zero, 0, 0, PM_REMOVE)) { TranslateMessage(ref m); DispatchMessage(ref m); }
                    if (clock.ElapsedMilliseconds < next) continue;
                    next = clock.ElapsedMilliseconds + 500;
                    try { Poll(); } catch (Exception e) { Log("poll error: " + e.Message); }
                    bool alive = false;
                    try { alive = game != null && !game.HasExited; } catch (Exception) { }
                    if (alive) { goneAt = DateTime.MinValue; continue; }
                    if (goneAt == DateTime.MinValue) { goneAt = DateTime.UtcNow; Log("the game has gone; watching " + LingerSec + " s more"); }
                    if ((DateTime.UtcNow - goneAt).TotalSeconds >= LingerSec) break;
                }
            }
            finally { foreach (IntPtr h in hooks) if (h != IntPtr.Zero) UnhookWinEvent(h); }
            int blocked = GameHider.CrashReportsBlockedFor(pid);
            if (blocked > 0) Log("the hook in the game kept UE's crash reporter from starting " + blocked + "x (" + GameHider.BlockedCrashArgs() + ")");
            Log("done");
        }
    }
}
