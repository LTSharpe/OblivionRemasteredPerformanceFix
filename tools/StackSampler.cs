// Samples call stacks of selected threads in another process by suspending the
// thread, reading its context and walking the stack with dbghelp (x64 unwind
// tables, no PDBs needed). Frames are reduced to "module+0xFUNCSTART" using the
// image's .pdata, so samples aggregate per function.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class StackSampler
{
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32.dll")] static extern IntPtr GetProcAddress(IntPtr mod, string name);
    [DllImport("kernel32.dll")] static extern IntPtr LoadLibrary(string name);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);
    [DllImport("dbghelp.dll", SetLastError = true)] static extern bool SymInitialize(IntPtr proc, string path, bool invade);
    [DllImport("dbghelp.dll")] static extern bool SymCleanup(IntPtr proc);
    [DllImport("dbghelp.dll")] static extern uint SymSetOptions(uint opts);
    [DllImport("dbghelp.dll")] static extern IntPtr SymFunctionTableAccess64(IntPtr proc, ulong addr);
    [DllImport("dbghelp.dll")] static extern ulong SymGetModuleBase64(IntPtr proc, ulong addr);
    [DllImport("dbghelp.dll")] static extern bool StackWalk64(uint machine, IntPtr proc, IntPtr thread, IntPtr frame,
        IntPtr ctx, IntPtr readMem, IntPtr funcTable, IntPtr moduleBase, IntPtr translate);

    const uint PROCESS_ACCESS = 0x0400 | 0x0010;          // QUERY_INFORMATION | VM_READ
    const uint THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040;   // SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
    const int CONTEXT_SIZE = 1232;
    const int FRAME_SIZE = 1024;

    public class Result
    {
        public Dictionary<string, int> Stacks = new Dictionary<string, int>();   // collapsed, root-first, ';'-joined
        public Dictionary<string, uint> FuncEnd = new Dictionary<string, uint>(); // "mod+0xBEGIN" -> end RVA
        public int Samples, Failed;
        public double Seconds;
        // Every sample in order, for lining stacks up with frame times: QPC milliseconds (the time base
        // PresentMon writes with --qpc_time_ms), index into the thread list, index into StackList.
        public List<double> SampleMs = new List<double>();
        public List<int> SampleThread = new List<int>();
        public List<int> SampleStack = new List<int>();
        public List<string> StackList = new List<string>();
    }

    static string ModName(ulong b, ulong[] bases, string[] names)
    {
        for (int i = 0; i < bases.Length; i++) if (bases[i] == b) return names[i];
        return "0x" + b.ToString("X");
    }

    public static Result Run(int pid, uint[] tids, string[] tidNames, ulong[] modBases, string[] modNames,
                             double seconds, int maxFrames)
    {
        return Run(pid, tids, tidNames, modBases, modNames, seconds, maxFrames, new ulong[0], new ulong[0], new int[0]);
    }

    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr n, out IntPtr r);

    // Manual unwind hints for code without .pdata: if RIP is in [hintLo[i], hintHi[i]),
    // the return address is at [RSP + hintRetOff[i]]. The leaf frame is recorded and
    // the walk continues from the caller.
    public static Result Run(int pid, uint[] tids, string[] tidNames, ulong[] modBases, string[] modNames,
                             double seconds, int maxFrames, ulong[] hintLo, ulong[] hintHi, int[] hintRetOff)
    {
        var res = new Result();
        IntPtr proc = OpenProcess(PROCESS_ACCESS, false, pid);
        if (proc == IntPtr.Zero) throw new Exception("OpenProcess failed");
        var threads = new IntPtr[tids.Length];
        for (int i = 0; i < tids.Length; i++)
        {
            threads[i] = OpenThread(THREAD_ACCESS, false, tids[i]);
            if (threads[i] == IntPtr.Zero) throw new Exception("OpenThread failed for " + tids[i]);
        }

        SymSetOptions(0x00000004 | 0x00000010);  // DEFERRED_LOADS | UNDNAME
        if (!SymInitialize(proc, "", true)) throw new Exception("SymInitialize failed");
        IntPtr dbghelp = LoadLibrary("dbghelp.dll");
        IntPtr pFuncTable = GetProcAddress(dbghelp, "SymFunctionTableAccess64");
        IntPtr pModBase = GetProcAddress(dbghelp, "SymGetModuleBase64");

        IntPtr ctxRaw = Marshal.AllocHGlobal(CONTEXT_SIZE + 16);
        IntPtr ctx = new IntPtr((ctxRaw.ToInt64() + 15) & ~15L);
        IntPtr frame = Marshal.AllocHGlobal(FRAME_SIZE);
        var addrs = new List<ulong>(maxFrames);
        var parts = new List<string>(maxFrames + 1);
        var stackIndex = new Dictionary<string, int>();

        timeBeginPeriod(1);
        var sw = Stopwatch.StartNew();
        try
        {
            while (sw.Elapsed.TotalSeconds < seconds)
            {
                for (int t = 0; t < threads.Length; t++)
                {
                    addrs.Clear();
                    double sampleMs = Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency;
                    if (SuspendThread(threads[t]) == 0xFFFFFFFF) { res.Failed++; continue; }
                    try
                    {
                        for (int i = 0; i < CONTEXT_SIZE; i += 8) Marshal.WriteInt64(ctx, i, 0);
                        Marshal.WriteInt32(ctx, 0x30, 0x10000B); // CONTEXT_AMD64 | CONTROL | INTEGER | FLOATING_POINT
                        if (!GetThreadContext(threads[t], ctx)) { res.Failed++; continue; }
                        ulong rip0 = (ulong)Marshal.ReadInt64(ctx, 0xF8), rsp0 = (ulong)Marshal.ReadInt64(ctx, 0x98);
                        for (int h = 0; h < hintLo.Length; h++)
                        {
                            if (rip0 < hintLo[h] || rip0 >= hintHi[h]) continue;
                            var rb = new byte[8]; IntPtr rr;
                            if (ReadProcessMemory(proc, new IntPtr((long)(rsp0 + (ulong)hintRetOff[h])), rb, new IntPtr(8), out rr))
                            {
                                addrs.Add(rip0);
                                Marshal.WriteInt64(ctx, 0xF8, BitConverter.ToInt64(rb, 0));                 // Rip = return address
                                Marshal.WriteInt64(ctx, 0x98, (long)(rsp0 + (ulong)hintRetOff[h] + 8));    // Rsp after ret
                            }
                            break;
                        }
                        for (int i = 0; i < FRAME_SIZE; i += 8) Marshal.WriteInt64(frame, i, 0);
                        Marshal.WriteInt64(frame, 0, Marshal.ReadInt64(ctx, 0xF8));  // AddrPC = Rip
                        Marshal.WriteInt32(frame, 12, 3);                              // AddrModeFlat
                        Marshal.WriteInt64(frame, 32, Marshal.ReadInt64(ctx, 0xA0));  // AddrFrame = Rbp
                        Marshal.WriteInt32(frame, 44, 3);
                        Marshal.WriteInt64(frame, 48, Marshal.ReadInt64(ctx, 0x98));  // AddrStack = Rsp
                        Marshal.WriteInt32(frame, 60, 3);
                        while (addrs.Count < maxFrames &&
                               StackWalk64(0x8664, proc, threads[t], frame, ctx, IntPtr.Zero, pFuncTable, pModBase, IntPtr.Zero))
                        {
                            ulong pc = (ulong)Marshal.ReadInt64(frame, 0);
                            if (pc == 0) break;
                            addrs.Add(pc);
                        }
                    }
                    finally { ResumeThread(threads[t]); }

                    // Resolve after resuming so the target thread is stopped as briefly as possible
                    parts.Clear();
                    parts.Add(tidNames[t]);
                    for (int i = addrs.Count - 1; i >= 0; i--)
                    {
                        ulong pc = addrs[i];
                        ulong mb = SymGetModuleBase64(proc, pc);
                        if (mb == 0) { parts.Add("?0x" + pc.ToString("X")); continue; }
                        string mod = ModName(mb, modBases, modNames);
                        IntPtr rf = SymFunctionTableAccess64(proc, pc);
                        if (rf != IntPtr.Zero)
                        {
                            uint begin = (uint)Marshal.ReadInt32(rf, 0), end = (uint)Marshal.ReadInt32(rf, 4);
                            string key = mod + "+0x" + begin.ToString("X");
                            res.FuncEnd[key] = end;
                            parts.Add(key);
                        }
                        else parts.Add(mod + "@0x" + (pc - mb).ToString("X"));
                    }
                    string stack = string.Join(";", parts);
                    int c; res.Stacks.TryGetValue(stack, out c); res.Stacks[stack] = c + 1;
                    int si;
                    if (!stackIndex.TryGetValue(stack, out si)) { si = res.StackList.Count; stackIndex[stack] = si; res.StackList.Add(stack); }
                    res.SampleMs.Add(sampleMs); res.SampleThread.Add(t); res.SampleStack.Add(si);
                    res.Samples++;
                }
                Thread.Sleep(1);
            }
        }
        finally
        {
            timeEndPeriod(1);
            res.Seconds = sw.Elapsed.TotalSeconds;
            Marshal.FreeHGlobal(ctxRaw); Marshal.FreeHGlobal(frame);
            SymCleanup(proc);
            foreach (var h in threads) CloseHandle(h);
            CloseHandle(proc);
        }
        return res;
    }
}
