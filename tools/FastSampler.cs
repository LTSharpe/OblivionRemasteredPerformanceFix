// Fast stack sampler for another process (investigation only). Same output as StackSampler.cs, but it unwinds the
// x64 stacks itself from the modules' .pdata/.xdata (read once from the target's mapped images) and reads the stack
// on demand while the thread is suspended, instead of dbghelp's StackWalk64. About 20-60 us per thread sample instead
// of ~1 ms, so all ~20 worker threads can be sampled at a few hundred Hz.
// Unwinding follows the documented x64 rules (as in Wine's RtlVirtualUnwind): prolog offset, frame register,
// chained unwind info, epilog detection, machine frames. Code without .pdata is treated as a leaf ([rsp] = return).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

public static class FastSampler
{
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr n, out IntPtr r);
    [DllImport("ntdll.dll")] static extern int NtQueryInformationThread(IntPtr h, int cls, byte[] info, int len, out int ret);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);

    const uint PROCESS_ACCESS = 0x0400 | 0x0010;
    const uint THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040;
    const int CONTEXT_SIZE = 1232;

    public class Result
    {
        public Dictionary<string, int> Stacks = new Dictionary<string, int>();
        public int Samples, Failed, Truncated;
        public double Seconds, SuspendUsAvg;
        public List<double> SampleMs = new List<double>();
        public List<int> SampleThread = new List<int>();
        public List<int> SampleStack = new List<int>();
        public List<string> StackList = new List<string>();
    }

    class Mod { public ulong Base, End; public string Name; public uint[] B, E, U; }

    static IntPtr proc;
    static Mod[] mods;
    static readonly Dictionary<ulong, byte[]> imgPages = new Dictionary<ulong, byte[]>();   // immutable image pages
    static readonly Dictionary<ulong, byte[]> stkPages = new Dictionary<ulong, byte[]>();   // per sample
    static readonly Stack<byte[]> pagePool = new Stack<byte[]>();
    static ulong stackBase;
    public static Dictionary<string, int> StopReasons = new Dictionary<string, int>();   // why walks ended early (debugging)
    static string why;

    static byte[] ImgPage(ulong page)
    {
        byte[] p;
        if (imgPages.TryGetValue(page, out p)) return p;
        p = new byte[4096]; IntPtr r;
        if (!ReadProcessMemory(proc, new IntPtr((long)page), p, new IntPtr(4096), out r)) p = null;
        imgPages[page] = p;
        return p;
    }
    static bool ImgByte(ulong a, out byte v)
    {
        var p = ImgPage(a & ~0xFFFUL); v = 0;
        if (p == null) return false;
        v = p[(int)(a & 0xFFF)]; return true;
    }
    static byte IB(ulong a) { byte v; ImgByte(a, out v); return v; }
    static ushort IU16(ulong a) { return (ushort)(IB(a) | (IB(a + 1) << 8)); }
    static uint IU32(ulong a) { return (uint)(IU16(a) | (IU16(a + 2) << 16)); }

    static bool Stk(ulong a, out ulong v)
    {
        v = 0;
        if (a < 0x10000 || a + 8 > stackBase) return false;
        ulong page = a & ~0xFFFUL;
        byte[] p;
        if (!stkPages.TryGetValue(page, out p))
        {
            p = pagePool.Count > 0 ? pagePool.Pop() : new byte[4096];
            IntPtr r;
            if (!ReadProcessMemory(proc, new IntPtr((long)page), p, new IntPtr(4096), out r)) { pagePool.Push(p); return false; }
            stkPages[page] = p;
        }
        int o = (int)(a & 0xFFF);
        if (o + 8 <= 4096) { v = BitConverter.ToUInt64(p, o); return true; }
        byte[] q;   // the value straddles two pages
        ulong page2 = page + 4096;
        if (!stkPages.TryGetValue(page2, out q))
        {
            q = pagePool.Count > 0 ? pagePool.Pop() : new byte[4096];
            IntPtr r;
            if (!ReadProcessMemory(proc, new IntPtr((long)page2), q, new IntPtr(4096), out r)) { pagePool.Push(q); return false; }
            stkPages[page2] = q;
        }
        var tmp = new byte[8];
        Array.Copy(p, o, tmp, 0, 4096 - o); Array.Copy(q, 0, tmp, 4096 - o, 8 - (4096 - o));
        v = BitConverter.ToUInt64(tmp, 0); return true;
    }
    static void ReleaseStack() { foreach (var p in stkPages.Values) pagePool.Push(p); stkPages.Clear(); }

    static Mod FindMod(ulong a)
    {
        foreach (var m in mods) if (a >= m.Base && a < m.End) return m;
        return null;
    }
    static int FindFunc(Mod m, uint rva)
    {
        if (m.B == null) return -1;
        int lo = 0, hi = m.B.Length - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            if (rva < m.B[mid]) hi = mid - 1; else if (rva >= m.E[mid]) lo = mid + 1; else return mid;
        }
        return -1;
    }

    static void LoadPdata(Mod m)
    {
        // PE header from the mapped image: exception directory = data directory 3
        uint pe = IU32(m.Base + 0x3C);
        ulong opt = m.Base + pe + 24;
        uint rva = IU32(opt + 112 + 3 * 8), size = IU32(opt + 112 + 3 * 8 + 4);
        int n = (int)(size / 12);
        if (rva == 0 || n == 0) return;
        var buf = new byte[n * 12]; IntPtr r;
        if (!ReadProcessMemory(proc, new IntPtr((long)(m.Base + rva)), buf, new IntPtr(buf.Length), out r)) return;
        m.B = new uint[n]; m.E = new uint[n]; m.U = new uint[n];
        for (int i = 0; i < n; i++)
        {
            m.B[i] = BitConverter.ToUInt32(buf, i * 12); m.E[i] = BitConverter.ToUInt32(buf, i * 12 + 4); m.U[i] = BitConverter.ToUInt32(buf, i * 12 + 8);
        }
    }

    // ctx registers by x64 number: 0 rax 1 rcx 2 rdx 3 rbx 4 rsp 5 rbp 6 rsi 7 rdi 8..15 r8..r15
    static bool InEpilog(ulong pc, Mod m, int fi)
    {
        byte b0 = IB(pc);
        if ((b0 & 0xF8) == 0x48)
        {
            byte b1 = IB(pc + 1), b2 = IB(pc + 2);
            switch (b1)
            {
                case 0x81: if (b0 == 0x48 && b2 == 0xC4) { pc += 7; break; } return false;
                case 0x83: if (b0 == 0x48 && b2 == 0xC4) { pc += 4; break; } return false;
                case 0x8D:
                    if ((b0 & 0x06) != 0) return false;
                    if (((b2 >> 3) & 7) != 4) return false;
                    if ((b2 & 7) == 4) return false;
                    if ((b2 >> 6) == 1) { pc += 4; break; }
                    if ((b2 >> 6) == 2) { pc += 7; break; }
                    return false;
            }
        }
        for (int guard = 0; guard < 16; guard++)
        {
            byte c = IB(pc);
            if ((c & 0xF0) == 0x40) { pc++; c = IB(pc); }
            if (c >= 0x58 && c <= 0x5F) { pc++; continue; }
            if (c == 0xC2 || c == 0xC3) return true;
            ulong fb = m.Base + m.B[fi], fe = m.Base + m.E[fi];
            if (c == 0xE9) { pc = pc + 5 + (ulong)(long)(int)IU32(pc + 1); if (pc >= fb && pc < fe) continue; return false; }
            if (c == 0xEB) { pc = pc + 2 + (ulong)(long)(sbyte)IB(pc + 1); if (pc >= fb && pc < fe) continue; return false; }
            if (c == 0xF3) return IB(pc + 1) == 0xC3;
            return false;
        }
        return false;
    }

    static bool DoEpilog(ulong pc, ulong[] R, ref ulong rip)
    {
        for (int guard = 0; guard < 32; guard++)
        {
            int rex = 0; byte c = IB(pc);
            if ((c & 0xF0) == 0x40) { rex = c & 0x0F; pc++; c = IB(pc); }
            ulong v;
            if (c >= 0x58 && c <= 0x5F) { if (!Stk(R[4], out v)) return false; R[c - 0x58 + (rex & 1) * 8] = v; R[4] += 8; pc++; continue; }
            if (c == 0x81) { R[4] += (ulong)(long)(int)IU32(pc + 2); pc += 6; continue; }
            if (c == 0x83) { R[4] += (ulong)(long)(sbyte)IB(pc + 2); pc += 3; continue; }
            if (c == 0x8D)
            {
                byte mr = IB(pc + 1); int reg = (mr & 7) + (rex & 1) * 8;
                if ((mr >> 6) == 1) { R[4] = R[reg] + (ulong)(long)(sbyte)IB(pc + 2); pc += 3; }
                else { R[4] = R[reg] + (ulong)(long)(int)IU32(pc + 2); pc += 6; }
                continue;
            }
            if (c == 0xC2) { if (!Stk(R[4], out v)) return false; rip = v; R[4] += 8UL + IU16(pc + 1); return true; }
            if (c == 0xC3 || c == 0xF3) { if (!Stk(R[4], out v)) return false; rip = v; R[4] += 8; return true; }
            if (c == 0xE9) { pc += 5 + (ulong)(long)(int)IU32(pc + 1); continue; }
            if (c == 0xEB) { pc += 2 + (ulong)(long)(sbyte)IB(pc + 1); continue; }
            return false;
        }
        return false;
    }

    static int OpSize(int op, int info)
    {
        switch (op)
        {
            case 1: return info != 0 ? 3 : 2;
            case 4: case 6: case 8: return 2;
            case 5: case 7: case 9: return 3;
            default: return 1;
        }
    }

    // One unwind step; returns false when the walk cannot continue.
    static bool Step(ref ulong rip, ulong[] R, bool leafFrame)
    {
        Mod m = FindMod(rip);
        if (m == null) { why = "nomod"; return false; }
        uint rva = (uint)(rip - m.Base);
        int fi = FindFunc(m, leafFrame ? rva : rva - 1);
        ulong v;
        why = "stack";
        if (fi < 0)
        {
            why = "leaf-stack";
            if (!Stk(R[4], out v)) return false;
            rip = v; R[4] += 8; return true;
        }
        ulong prologOffset = rva - m.B[fi];
        uint unwind = m.U[fi];
        bool first = true, machframe = false;
        for (int chain = 0; chain < 8; chain++)
        {
            ulong ui = m.Base + (unwind & ~1u);
            byte h0 = IB(ui), prolog = IB(ui + 1), count = IB(ui + 2), fr = IB(ui + 3);
            int flags = h0 >> 3, frameReg = fr & 0xF, frameOff = fr >> 4;
            ulong po = first ? prologOffset : ulong.MaxValue;
            ulong frame = R[4];
            if (frameReg != 0)
            {
                bool fpSet = true;
                if (po < prolog)
                {
                    fpSet = false;
                    for (int i = 0; i < count; )
                    {
                        byte co = IB(ui + 4 + (ulong)i * 2), oi = IB(ui + 5 + (ulong)i * 2);
                        if ((oi & 0xF) == 3 && co <= po) fpSet = true;
                        i += OpSize(oi & 0xF, oi >> 4);
                    }
                }
                if (fpSet) frame = R[frameReg] - (ulong)(frameOff * 16);
            }
            if (first && po >= prolog && InEpilog(rip, m, fi))
            { why = "epilog"; return DoEpilog(rip, R, ref rip); }
            why = "codes";
            for (int i = 0; i < count; )
            {
                ulong cp = ui + 4 + (ulong)i * 2;
                byte co = IB(cp), oi = IB(cp + 1);
                int op = oi & 0xF, info = oi >> 4, sz = OpSize(op, info);
                if (po < co) { i += sz; continue; }
                switch (op)
                {
                    case 0: if (!Stk(R[4], out v)) return false; R[info] = v; R[4] += 8; break;
                    case 1: R[4] += info == 0 ? (ulong)IU16(cp + 2) * 8 : IU32(cp + 2); break;
                    case 2: R[4] += (ulong)(info * 8 + 8); break;
                    case 3: R[4] = frame; break;
                    case 4: if (!Stk(frame + (ulong)IU16(cp + 2) * 8, out v)) return false; R[info] = v; break;
                    case 5: if (!Stk(frame + IU32(cp + 2), out v)) return false; R[info] = v; break;
                    case 10:
                        {
                            ulong off = info == 0 ? 0UL : 8UL; ulong nrip, nrsp;
                            if (!Stk(R[4] + off, out nrip) || !Stk(R[4] + off + 24, out nrsp)) return false;
                            rip = nrip; R[4] = nrsp; machframe = true; break;
                        }
                }
                i += sz;
            }
            if ((flags & 4) == 0) break;
            ulong ch = ui + 4 + (ulong)(2 * (count + (count & 1)));
            unwind = IU32(ch + 8); first = false;
        }
        if (!machframe)
        {
            if (!Stk(R[4], out v)) return false;
            rip = v; R[4] += 8;
        }
        return true;
    }

    static string Frame(ulong pc, bool leaf)
    {
        Mod m = FindMod(pc);
        if (m == null) return "?0x" + pc.ToString("X");
        uint rva = (uint)(pc - m.Base);
        int fi = FindFunc(m, leaf ? rva : rva - 1);
        if (fi < 0) return m.Name + "@0x" + rva.ToString("X");
        return m.Name + "+0x" + m.B[fi].ToString("X");
    }

    public static Result Run(int pid, uint[] tids, string[] tidNames, ulong[] modBases, ulong[] modSizes, string[] modNames,
                             double seconds, int maxFrames, double intervalMs,
                             ulong[] hintLo, ulong[] hintHi, int[] hintRetOff)
    {
        var res = new Result();
        proc = OpenProcess(PROCESS_ACCESS, false, pid);
        if (proc == IntPtr.Zero) throw new Exception("OpenProcess failed");
        imgPages.Clear();
        mods = new Mod[modBases.Length];
        for (int i = 0; i < modBases.Length; i++)
        {
            mods[i] = new Mod { Base = modBases[i], End = modBases[i] + modSizes[i], Name = modNames[i] };
            LoadPdata(mods[i]);
        }
        var threads = new IntPtr[tids.Length];
        var sbase = new ulong[tids.Length];
        var tbi = new byte[48];
        for (int i = 0; i < tids.Length; i++)
        {
            threads[i] = OpenThread(THREAD_ACCESS, false, tids[i]);
            if (threads[i] == IntPtr.Zero) throw new Exception("OpenThread failed for " + tids[i]);
            int rl;
            if (NtQueryInformationThread(threads[i], 0, tbi, 48, out rl) != 0) throw new Exception("NtQueryInformationThread failed");
            ulong teb = BitConverter.ToUInt64(tbi, 8);
            var sb = new byte[8]; IntPtr r;
            ReadProcessMemory(proc, new IntPtr((long)(teb + 8)), sb, new IntPtr(8), out r);
            sbase[i] = BitConverter.ToUInt64(sb, 0);
        }
        IntPtr ctxRaw = Marshal.AllocHGlobal(CONTEXT_SIZE + 16);
        IntPtr ctx = new IntPtr((ctxRaw.ToInt64() + 15) & ~15L);
        var R = new ulong[16];
        var addrs = new List<ulong>(maxFrames);
        var parts = new List<string>(maxFrames + 1);
        var stackIndex = new Dictionary<string, int>();
        double suspendTicks = 0;
        timeBeginPeriod(1);
        var sw = Stopwatch.StartNew();
        double next = 0;
        try
        {
            while (sw.Elapsed.TotalSeconds < seconds)
            {
                for (int t = 0; t < threads.Length; t++)
                {
                    addrs.Clear();
                    long ts0 = Stopwatch.GetTimestamp();
                    double sampleMs = ts0 * 1000.0 / Stopwatch.Frequency;
                    if (SuspendThread(threads[t]) == 0xFFFFFFFF) { res.Failed++; continue; }
                    bool truncated = false;
                    try
                    {
                        Marshal.WriteInt32(ctx, 0x30, 0x100003); // CONTEXT_AMD64 | CONTROL | INTEGER
                        if (!GetThreadContext(threads[t], ctx)) { res.Failed++; continue; }
                        // CONTEXT: Rax 0x78, Rcx 0x80, Rdx 0x88, Rbx 0x90, Rsp 0x98, Rbp 0xA0, Rsi 0xA8, Rdi 0xB0, R8..R15 0xB8.., Rip 0xF8
                        for (int k = 0; k < 16; k++) R[k] = (ulong)Marshal.ReadInt64(ctx, 0x78 + k * 8);
                        ulong rip = (ulong)Marshal.ReadInt64(ctx, 0xF8);
                        stackBase = sbase[t];
                        bool leaf = true;
                        for (int h = 0; h < hintLo.Length; h++)
                        {
                            if (rip < hintLo[h] || rip >= hintHi[h]) continue;
                            ulong v;
                            if (Stk(R[4] + (ulong)hintRetOff[h], out v)) { addrs.Add(rip); rip = v; R[4] = R[4] + (ulong)hintRetOff[h] + 8; leaf = false; }
                            break;
                        }
                        while (addrs.Count < maxFrames)
                        {
                            if (rip == 0) break;
                            addrs.Add(rip | (leaf ? 0UL : 1UL << 63));
                            ulong prevRsp = R[4];
                            ulong before = rip;
                            if (!Step(ref rip, R, leaf))
                            {
                                string k = why + " " + Frame(before, leaf) + " rip+0x" + (before - (FindMod(before) != null ? FindMod(before).Base : 0)).ToString("X");
                                int n; StopReasons.TryGetValue(k, out n); StopReasons[k] = n + 1;
                                break;
                            }
                            leaf = false;
                            if (R[4] <= prevRsp && rip != 0) { truncated = true; break; }   // no progress: corrupt walk
                        }
                    }
                    finally { ResumeThread(threads[t]); ReleaseStack(); }
                    suspendTicks += Stopwatch.GetTimestamp() - ts0;
                    if (truncated) res.Truncated++;
                    parts.Clear();
                    parts.Add(tidNames[t]);
                    for (int i = addrs.Count - 1; i >= 0; i--)
                    {
                        bool isLeaf = (addrs[i] >> 63) == 0;
                        parts.Add(Frame(addrs[i] & ~(1UL << 63), isLeaf));
                    }
                    string stack = string.Join(";", parts);
                    int c; res.Stacks.TryGetValue(stack, out c); res.Stacks[stack] = c + 1;
                    int si;
                    if (!stackIndex.TryGetValue(stack, out si)) { si = res.StackList.Count; stackIndex[stack] = si; res.StackList.Add(stack); }
                    res.SampleMs.Add(sampleMs); res.SampleThread.Add(t); res.SampleStack.Add(si);
                    res.Samples++;
                }
                next += intervalMs;
                double now = sw.Elapsed.TotalMilliseconds;
                if (next < now) next = now;
                while (sw.Elapsed.TotalMilliseconds < next - 1.2) Thread.Sleep(1);
                while (sw.Elapsed.TotalMilliseconds < next) Thread.SpinWait(50);
            }
        }
        finally
        {
            timeEndPeriod(1);
            res.Seconds = sw.Elapsed.TotalSeconds;
            res.SuspendUsAvg = res.Samples > 0 ? suspendTicks * 1e6 / Stopwatch.Frequency / res.Samples : 0;
            Marshal.FreeHGlobal(ctxRaw);
            foreach (var h in threads) CloseHandle(h);
            CloseHandle(proc);
        }
        return res;
    }
}
