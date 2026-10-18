// MemoryRelief and the GPU crash report.
//
// Why (2026-10-09): a play session on the test PC ended in a GPU hang (driver timeout, "GPU Crash dump Triggered")
// after 31 minutes at 4K with frame generation, climbing the road to Bruma. UE's crash report says only that the
// D3D12 device was removed. The engine collects DRED data (Lightweight DRED, on by default in 5.3 shipping: the
// render-graph pass markers each command list had completed on the GPU, and page-fault data) but writes it only to
// its log, which the shipping build does not have, so the pass that hung is lost. The report also showed Windows
// short of memory at that moment: 0.9 GB of RAM available and 1.7 GB of commit left (game 21 GB, everything else
// ~41 GB of a 64 GB limit), as in most GPU crashes on record on that PC, stock game included.
//
// GPU crash report ([General] GpuCrashReport, default on; diagnostics only). When the game reports a GPU crash, UE
// raises exception 0x8000 (WindowsPlatformCrashContext's GPU-crash code) on the thread that noticed it, inside
// TerminateOnGPUCrash(device). A vectored handler then finds the game's D3D12 device on that thread's registers and
// stack (a pointer whose object has its vtable in the D3D12 runtime, asked through COM for the device directly or as
// the device of a queue, list or resource) and writes to OblivionPerfFix.log, before UE's crash reporter runs: the
// device-removed reason and DRED device state, every command list the GPU had started but not finished with the pass
// markers that were open where it stopped, the page-fault address and the allocations around it, and the memory
// samples of the last 30 seconds. The handler only reads and logs; the exception then goes on as before. Nothing is
// hooked: the device cannot be caught at creation (on AMD GPUs the engine creates it inside the driver's AGS
// extension, past every import of the exe), and a remembered pointer could belong to one of the engine's short-lived
// test devices.
//
// Memory line ([mem], with the other statistics every StatsIntervalSeconds): Windows' available RAM and commit, the
// game's private bytes and working set, and its video memory use against the budget Windows gives it (DXGI
// QueryVideoMemoryInfo on the GPU the game uses most memory on), with the extremes of the interval. Sampled once a
// second.
//
// MemoryRelief (default on). LumenKeepArrayCapacity and UploadMemoryKeep keep memory on purpose (Lumen arrays keep their capacity, empty
// upload pools stay for the session: up to ~0.5 GB of RAM and commit) and parallel translate (RdgParallelTranslate) costs ~0.7 GB
// of video memory (its own command allocators and the paused buffer defragmentation). When Windows itself runs
// short, that memory is better given back: while available RAM stays under MemoryReliefRamMB (1024) for 3 seconds,
// UploadMemoryKeep behaves as the game does (kept upload pools are released over its frame ramp); while available commit plus the
// room the page files can still grow by stays under MemoryReliefCommitMB (1024), LumenKeepArrayCapacity does as well (the Lumen arrays
// shrink at their next consolidation; see UpdateRelief for why RAM alone does not need that). RAM and commit go back
// once there is twice the threshold free for 60 seconds. While the game's video memory stays over the budget Windows
// gives it, upload retention is suspended first, then parallel translate if needed, only while that helps (see
// VramRelief). Each change is logged. What is drawn never changes.
#include "common.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {

constexpr DWORD kUeGpuCrashException = 0x8000;  // UE: ReportGPUCrash raises it (WindowsPlatformCrashContext.cpp)
constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

// ---- finding the game's device at the crash ----

struct Range {
    const uint8_t* lo = nullptr;
    const uint8_t* hi = nullptr;
};

Range ModuleRange(const wchar_t* name)
{
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(name));
    if (!base) return {};
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

// Committed, readable, and no guard page (touching another thread's stack guard would break its growth).
bool Readable(const void* p, size_t size)
{
    const auto a = reinterpret_cast<uintptr_t>(p);
    if (a < 0x10000 || a > 0x00007FFFFFFFFFF0ull || (a & 7)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return false;
    return a + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// p is a D3D12 runtime object: the device itself, or something that has one (ID3D12DeviceChild). AddRef'd, or null.
ID3D12Device* AskForDevice(IUnknown* p)
{
    __try {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&d))) && d) return d;
        ID3D12DeviceChild* child = nullptr;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&child))) && child) {
            const HRESULT hr = child->GetDevice(IID_PPV_ARGS(&d));
            child->Release();
            if (SUCCEEDED(hr) && d) return d;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

struct Finder {
    Range mods[3];
    int candidates = 0;
};

bool ReadWord(const void* at, const uint8_t** out)
{
    __try {
        *out = *static_cast<const uint8_t* const*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A COM object of the runtime: its first word points to a vtable in a read-only part of a D3D12 module, and the vtable's
// first entry (QueryInterface) into the same module. Only then is it asked anything.
ID3D12Device* TryCandidate(Finder& f, uintptr_t value)
{
    const void* p = reinterpret_cast<const void*>(value);
    const uint8_t *vtable = nullptr, *qi = nullptr;
    if (!Readable(p, sizeof(void*)) || !ReadWord(p, &vtable)) return nullptr;
    const Range* mod = nullptr;
    for (const Range& m : f.mods)
        if (m.lo && vtable >= m.lo && vtable < m.hi) mod = &m;
    if (!mod) return nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(vtable, &mbi, sizeof mbi) || (mbi.Protect & 0xFF) != PAGE_READONLY) return nullptr;
    if (!ReadWord(vtable, &qi) || qi < mod->lo || qi >= mod->hi) return nullptr;
    ++f.candidates;
    return AskForDevice(reinterpret_cast<IUnknown*>(value));
}

// Registers first, then the words in [lo, hi) (the crashing thread's stack).
ID3D12Device* FindDevice(const CONTEXT* ctx, const uintptr_t* lo, const uintptr_t* hi, int* candidates)
{
    Finder f;
    f.mods[0] = ModuleRange(L"D3D12Core.dll");
    f.mods[1] = ModuleRange(L"d3d12.dll");
    f.mods[2] = ModuleRange(L"D3D12SDKLayers.dll");
    ID3D12Device* d = nullptr;
    if (ctx) {
        const DWORD64 regs[] = {ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->R8,
                                ctx->R9,  ctx->R10, ctx->R11, ctx->R12, ctx->R13, ctx->R14, ctx->R15};
        for (DWORD64 r : regs)
            if (!d) d = TryCandidate(f, r);
    }
    for (const uintptr_t* w = lo; !d && w < hi; ++w) d = TryCandidate(f, *w);
    *candidates = f.candidates;
    return d;
}

// ---- memory samples ----

struct Sample {
    ULONGLONG tick;
    uint64_t ramFree, ramTotal, commitFree, commitLimit;  // Windows (GlobalMemoryStatusEx)
    uint64_t commitGrow;                                  // room the page files can still grow by (PageFileGrowRoom)
    uint64_t priv, ws;                                    // the game process
    uint64_t vram, vramBudget, shared, sharedBudget;      // the game on its GPU (0 = not known yet)
};

constexpr int kRing = 32;
Sample g_ring[kRing];
std::atomic<unsigned> g_ringNext{0};

IDXGIAdapter3* g_adapter = nullptr;  // the GPU the game uses (set once by the monitor thread)
ULONGLONG g_nextAdapterTry = 0;

// The hardware adapter on which this process uses the most video memory (the game's GPU once it renders).
void OpenAdapter()
{
    const ULONGLONG now = GetTickCount64();
    if (g_adapter || now < g_nextAdapterTry) return;
    g_nextAdapterTry = now + 10000;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi || !GetModuleHandleW(L"D3D12Core.dll")) return;
    auto create = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(IID_PPV_ARGS(&factory))) || !factory) return;
    IDXGIAdapter3* best = nullptr;
    uint64_t bestUsage = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        IDXGIAdapter3* a3 = nullptr;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (SUCCEEDED(a->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&a3))) && a3 &&
            SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) && local.CurrentUsage > bestUsage) {
            if (best) best->Release();
            best = a3;
            bestUsage = local.CurrentUsage;
        } else if (a3) a3->Release();
        a->Release();
    }
    factory->Release();
    if (best && bestUsage >= (256ull << 20)) {
        g_adapter = best;
        DXGI_ADAPTER_DESC1 desc{};
        best->GetDesc1(&desc);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
        Log("[mem] the game's GPU: %s, %.1f GB video memory", name, desc.DedicatedVideoMemory / kGB);
    } else if (best) best->Release();
}

// For RdgParallelTranslate's automatic mode: the largest dedicated video memory among the hardware GPUs, asked once (monitor thread;
// dxgi.dll is loaded here, never under the loader lock). The game runs on the biggest GPU of the PC in practice.
uint64_t g_largestGpuMB = 0;
char g_largestGpuName[128] = "";
bool g_largestGpuAsked = false;

}  // namespace

IDXGIAdapter3* GameAdapter() { return g_adapter; }

uint64_t LargestGpuMemoryMB(const char** name)
{
    if (!g_largestGpuAsked) {
        g_largestGpuAsked = true;
        if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll")) {
            auto create = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
            IDXGIFactory1* factory = nullptr;
            if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) && factory) {
                IDXGIAdapter1* a = nullptr;
                for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(a->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                        (desc.DedicatedVideoMemory >> 20) > g_largestGpuMB) {
                        g_largestGpuMB = desc.DedicatedVideoMemory >> 20;
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_largestGpuName, sizeof g_largestGpuName,
                                            nullptr, nullptr);
                    }
                    a->Release();
                }
                factory->Release();
            }
        }
    }
    if (name) *name = g_largestGpuName;
    return g_largestGpuMB;
}

namespace {

// How far Windows can still raise the commit limit by growing the page files (bytes): per page file the room up to its
// maximum size (a system-managed one grows up to 3x RAM), at most the free space of its volume less 4 GB. When commit
// runs out, Windows grows the page file and the allocation goes through, so "commit available" under the current limit
// is not yet a shortage (2026-10-09: the limit went 63.9 -> 68 GB in each 4K run on the dev PC). 0 if unknown.
uint64_t PageFileGrowRoom()
{
    using QuerySystemInformationFn = LONG(WINAPI*)(ULONG cls, void* info, ULONG size, ULONG* returned);
    static const auto query = reinterpret_cast<QuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return 0;
    // SYSTEM_PAGEFILE_INFORMATION_EX (SystemPageFileInformationEx = 144, Windows 8.1+), sizes in pages
    struct PageFileInfo {
        ULONG next, totalSize, totalInUse, peakUsage;
        USHORT nameLength, nameMax;
        const wchar_t* name;
        ULONG minimumSize, maximumSize;
    };
    static_assert(sizeof(PageFileInfo) == 40, "SYSTEM_PAGEFILE_INFORMATION_EX layout");
    alignas(8) uint8_t buf[2048];
    ULONG got = 0;
    if (query(144, buf, sizeof buf, &got) < 0 || got < sizeof(PageFileInfo)) return 0;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uint64_t room = 0;
    for (ULONG at = 0; at + sizeof(PageFileInfo) <= got;) {
        const auto* e = reinterpret_cast<const PageFileInfo*>(buf + at);
        if (e->maximumSize > e->totalSize && e->name) {
            // "\??\C:\pagefile.sys": the volume is the letter before the first colon
            const size_t n = e->nameLength / 2;
            wchar_t root[4] = {0, L':', L'\\', 0};
            for (size_t i = 1; i < n && !root[0]; ++i)
                if (e->name[i] == L':') root[0] = e->name[i - 1];
            ULARGE_INTEGER freeBytes{};
            if (root[0] && GetDiskFreeSpaceExW(root, &freeBytes, nullptr, nullptr)) {
                const uint64_t grow = static_cast<uint64_t>(e->maximumSize - e->totalSize) * si.dwPageSize;
                const uint64_t disk = freeBytes.QuadPart > (4ull << 30) ? freeBytes.QuadPart - (4ull << 30) : 0;
                room += grow < disk ? grow : disk;
            }
        }
        if (!e->next) break;
        at += e->next;
    }
    return room;
}

Sample TakeSample()
{
    Sample s{};
    s.tick = GetTickCount64();
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        s.ramFree = ms.ullAvailPhys;
        s.ramTotal = ms.ullTotalPhys;
        s.commitFree = ms.ullAvailPageFile;
        s.commitLimit = ms.ullTotalPageFile;
    }
    s.commitGrow = PageFileGrowRoom();
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) {
        s.priv = pm.PrivateUsage;
        s.ws = pm.WorkingSetSize;
    }
    if (g_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
            s.vram = local.CurrentUsage;
            s.vramBudget = local.Budget;
        }
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
            s.shared = shared.CurrentUsage;
            s.sharedBudget = shared.Budget;
        }
    }
    return s;
}

// Extremes since the last [mem] line.
struct Interval {
    uint64_t ramFreeMin = UINT64_MAX, commitFreeMin = UINT64_MAX, privMax = 0, vramMax = 0, vramBudgetAtMax = 0;
    unsigned overBudgetSeconds = 0;
};
Interval g_interval;

// ---- MemoryRelief ----

// Two memory levels (an earlier design had one, which on the dev PC was on for every 4K run and brought back the stalls
// of LumenKeepArrayCapacity and UploadMemoryKeep on the Silver Road):
// - RAM: available RAM under MemoryReliefRamMB. Only UploadMemoryKeep gives back: its upload pools are resident (the GPU reads
//   them), while the capacity LumenKeepArrayCapacity keeps beyond the arrays' contents is never touched again, so Windows can page it
//   out like any idle memory.
// - commit: available commit, plus the room the page files can still grow by, under MemoryReliefCommitMB. Both give
//   back (both hold commit). With a system-managed page file and free disk space this does not happen: Windows grows
//   the page file instead. Under half the threshold (512 MB by default) commit relief starts at once, not after 3 s:
//   without a page file (commit limit = RAM) a streaming burst can use up 1 GB of commit within those seconds.
std::atomic<bool> g_reliefRam{false}, g_reliefCommit{false}, g_reliefVram{false}, g_reliefVramTranslate{false};
int g_ramPressureSeconds = 0, g_ramClearSeconds = 0, g_commitPressureSeconds = 0, g_commitClearSeconds = 0;
unsigned g_ramEpisodes = 0, g_commitEpisodes = 0;

// Video-memory relief: stage 1 releases kept upload pools/pages using UploadMemoryKeep's existing ramp; stage 2 also pauses
// parallel translate. Each saved ~0.36-0.38 GB in the maintainers' tests; upload retention is cheaper to sacrifice.
// GPU scene capacity is deliberately left alone. After 15 consecutive over-budget samples, try stage 1 for 30 s,
// judging its last 10 s (at most 2 over). Escalate only on failure or 15 s of renewed pressure after a success.
// If stage 2 fails, restore configured fixes and back off 10/20/40 min. A successful stage stays until 60 s with
// 512 MB headroom per active stage, leaving room to restore both fixes. Unknown budgets cannot prove success:
// cancel VRAM relief and require fresh pressure samples. RAM/commit relief is independent and still takes priority.
// Step runs once per second. Counts are bounded even in multi-day sessions.
class VramRelief {
public:
    static constexpr int kEngageSeconds = 15;
    static constexpr int kProbeSeconds = 30;
    static constexpr int kProbeJudgeSeconds = 10;
    static constexpr int kProbeOverAllowed = 2;
    static constexpr int kClearSeconds = 60;
    static constexpr uint64_t kHeadroomMB = 512;
    static constexpr int kBackoffMinutes = 10;
    static constexpr int kBackoffMaxMinutes = 40;

    using Event = VramReliefEvent;

    // VramReliefPauseTranslate (hidden: off by default): whether a failed stage 1 escalates to pausing parallel
    // translate. Off, it backs off instead: no fix is switched off for video memory (CommandAllocatorTrim trims first, at once).
    void AllowTranslateStage(bool allow) { translateStage_ = allow; }

    Event Step(uint64_t vram, uint64_t budget, bool enabled)
    {
        if (!enabled || !budget) {
            const bool was = Active();
            state_ = State::Off;
            over_ = roomy_ = 0;
            return was ? (enabled ? Event::BudgetUnknown : Event::SwitchedOff) : Event::None;
        }
        const bool over = vram > budget;
        const bool roomy = vram <= budget && budget - vram >= (HeadroomMB() << 20);
        over_ = over ? (over_ < kEngageSeconds ? over_ + 1 : over_) : 0;
        roomy_ = roomy ? (roomy_ < kClearSeconds ? roomy_ + 1 : roomy_) : 0;
        if (backoffLeft_) --backoffLeft_;
        switch (state_) {
        case State::Off:
            if (over_ >= kEngageSeconds && !backoffLeft_) {
                StartProbe(State::UploadProbe);
                engagedVram_ = vram;
                ++episodes_;
                return Event::Engaged;
            }
            break;
        case State::UploadProbe:
        case State::TranslateProbe:
            ++probe_;
            if (probe_ > kProbeSeconds - kProbeJudgeSeconds && over) ++probeOver_;
            if (probe_ >= kProbeSeconds) {
                if (probeOver_ <= kProbeOverAllowed) {
                    state_ = Paused() ? State::TranslateHold : State::UploadHold;
                    backoffMinutes_ = 0;
                    if (!episodeHelped_) ++helped_;
                    episodeHelped_ = true;
                    return Event::Helped;
                }
                if (!Paused() && translateStage_) {
                    StartProbe(State::TranslateProbe);
                    return Event::Escalated;
                }
                GiveUp();
                return Event::NotHelping;
            }
            break;
        case State::UploadHold:
        case State::TranslateHold:
            if (roomy_ >= kClearSeconds) {
                state_ = State::Off;
                return Event::Cleared;
            }
            if (over_ >= kEngageSeconds) {
                if (!Paused() && translateStage_) {
                    StartProbe(State::TranslateProbe);
                    return Event::Escalated;
                }
                GiveUp();
                return Event::NoLongerHelping;
            }
            break;
        }
        return Event::None;
    }

    bool Active() const { return state_ != State::Off; }
    bool Paused() const { return state_ == State::TranslateProbe || state_ == State::TranslateHold; }
    uint64_t HeadroomMB() const { return Paused() ? 2 * kHeadroomMB : kHeadroomMB; }
    uint64_t EngagedVram() const { return engagedVram_; }
    int BackoffMinutes() const { return backoffMinutes_; }
    unsigned Episodes() const { return episodes_; }
    unsigned Helped() const { return helped_; }

private:
    enum class State { Off, UploadProbe, UploadHold, TranslateProbe, TranslateHold };

    void StartProbe(State state)
    {
        if (state == State::UploadProbe) episodeHelped_ = false;
        state_ = state;
        probe_ = probeOver_ = over_ = roomy_ = 0;
    }

    void GiveUp()
    {
        state_ = State::Off;
        backoffMinutes_ = backoffMinutes_ ? (backoffMinutes_ * 2 < kBackoffMaxMinutes ? backoffMinutes_ * 2 : kBackoffMaxMinutes)
                                          : kBackoffMinutes;
        backoffLeft_ = backoffMinutes_ * 60;
        over_ = 0;
    }

    State state_ = State::Off;
    int over_ = 0, roomy_ = 0, probe_ = 0, probeOver_ = 0, backoffLeft_ = 0, backoffMinutes_ = 0;
    uint64_t engagedVram_ = 0;
    unsigned episodes_ = 0, helped_ = 0;
    bool episodeHelped_ = false;
    bool translateStage_ = true;
};
VramRelief g_vramRelief;

void LogVramEvent(VramRelief::Event e, const VramRelief& r, uint64_t vram, uint64_t budget)
{
    switch (e) {
    case VramRelief::Event::Engaged:
        Log("[mem] video memory over budget for %d s (%.2f of %.2f GB): VRAM stage 1, upload retention suspended; "
            "parallel translate unchanged; trial %d s",
            VramRelief::kEngageSeconds, vram / kGB, budget / kGB, VramRelief::kProbeSeconds);
        break;
    case VramRelief::Event::Escalated:
        Log("[mem] upload relief insufficient (%.2f of %.2f GB): VRAM stage 2, parallel translate also paused; trial %d s",
            vram / kGB, budget / kGB, VramRelief::kProbeSeconds);
        break;
    case VramRelief::Event::Helped:
        Log("[mem] VRAM stage %d helped (%.2f -> %.2f GB, budget %.2f GB): hold until %.1f GB under budget for %d s",
            r.Paused() ? 2 : 1, r.EngagedVram() / kGB, vram / kGB, budget / kGB,
            r.HeadroomMB() / 1024.0, VramRelief::kClearSeconds);
        break;
    case VramRelief::Event::NotHelping:
        Log("[mem] VRAM relief insufficient (%.2f -> %.2f GB, budget %.2f GB): VRAM overrides cleared; "
            "next try in %d min at the earliest", r.EngagedVram() / kGB, vram / kGB, budget / kGB, r.BackoffMinutes());
        break;
    case VramRelief::Event::NoLongerHelping:
        Log("[mem] video memory over budget despite VRAM relief (%.2f of %.2f GB for %d s): VRAM overrides cleared; "
            "next try in %d min at the earliest", vram / kGB, budget / kGB, VramRelief::kEngageSeconds, r.BackoffMinutes());
        break;
    case VramRelief::Event::Cleared:
        Log("[mem] video memory has headroom for %d s (%.2f of %.2f GB): VRAM overrides cleared",
            VramRelief::kClearSeconds, vram / kGB, budget / kGB);
        break;
    case VramRelief::Event::BudgetUnknown:
        Log("[mem] video memory budget unavailable: VRAM overrides cleared");
        break;
    case VramRelief::Event::SwitchedOff:
    case VramRelief::Event::None:
        break;
    }
}

void UpdateRelief(const Sample& s, const Config& cfg)
{
    const uint64_t ramLimit = static_cast<uint64_t>(cfg.memoryReliefRamMB) << 20;
    const uint64_t commitLimit = static_cast<uint64_t>(cfg.memoryReliefCommitMB) << 20;
    const uint64_t commitRoom = s.commitFree + s.commitGrow;
    const bool ramShort = s.ramTotal && s.ramFree < ramLimit;
    const bool ramClear = s.ramFree >= 2 * ramLimit;
    const bool commitShort = s.ramTotal && commitRoom < commitLimit;
    const bool commitCritical = s.ramTotal && commitRoom < commitLimit / 2;
    const bool commitClear = commitRoom >= 2 * commitLimit;

    g_ramPressureSeconds = ramShort ? g_ramPressureSeconds + 1 : 0;
    g_ramClearSeconds = ramClear ? g_ramClearSeconds + 1 : 0;
    g_commitPressureSeconds = commitShort ? g_commitPressureSeconds + 1 : 0;
    g_commitClearSeconds = commitClear ? g_commitClearSeconds + 1 : 0;

    g_vramRelief.AllowTranslateStage(cfg.vramReliefPauseTranslate);
    // The video-memory stages are off by default (VramRelief=1 brings them back): in three real over-budget
    // runs (an ~8.2 GB budget) stage 1 never got the game under the budget (8.30 -> 8.64 / 8.75 GB) and its upload
    // hand-back added 25-73 ms clean-ups on the RHI thread; CommandAllocatorTrim gives back what the game does not need instead.
    const VramRelief::Event vramEvent = g_vramRelief.Step(s.vram, s.vramBudget, cfg.memoryRelief && cfg.vramRelief);
    // CommandAllocatorTrim's own step (its ini switch, not MemoryRelief's): while over the budget, command allocator pools keep only the
    // last few seconds' need.
    // "Tight": within AllocatorTrimHeadroomMB of the budget; it ends only 512 MB below that (no flip-flop at the line).
    const uint64_t headroom = static_cast<uint64_t>((std::max)(cfg.allocatorTrimHeadroomMB, 0)) << 20;
    static bool tight = false;  // unknown budget: as the game
    if (!headroom) tight = true;
    else if (s.vramBudget && s.vram + headroom > s.vramBudget) tight = true;
    else if (s.vramBudget && s.vram + headroom + (512ull << 20) <= s.vramBudget) tight = false;
    SetAllocatorTrimPressure(s.vramBudget && s.vram > s.vramBudget, tight);
    if (!cfg.memoryRelief) {
        g_reliefVram = false;
        g_reliefVramTranslate = false;
        if (g_reliefRam.exchange(false) | g_reliefCommit.exchange(false) || vramEvent == VramRelief::Event::SwitchedOff)
            Log("[mem] memory relief switched off in the ini");
        return;
    }
    LogVramEvent(vramEvent, g_vramRelief, s.vram, s.vramBudget);
    g_reliefVram = g_vramRelief.Active();
    g_reliefVramTranslate = g_vramRelief.Paused();
    if (!g_reliefRam && g_ramPressureSeconds >= 3) {
        g_reliefRam = true;
        ++g_ramEpisodes;
        Log("[mem] Windows is short of RAM (%.2f GB available): UploadMemoryKeep gives its kept upload memory back until it recovers",
            s.ramFree / kGB);
    } else if (g_reliefRam && g_ramClearSeconds >= 60) {
        g_reliefRam = false;
        Log("[mem] RAM recovered (%.2f GB available for a minute): UploadMemoryKeep keeps upload memory again", s.ramFree / kGB);
    }
    if (!g_reliefCommit && (g_commitPressureSeconds >= 3 || commitCritical)) {
        g_reliefCommit = true;
        ++g_commitEpisodes;
        Log("[mem] Windows is short of commit (%.2f GB available, page files can grow %.2f GB more): LumenKeepArrayCapacity and UploadMemoryKeep give "
            "their kept memory back until it recovers", s.commitFree / kGB, s.commitGrow / kGB);
    } else if (g_reliefCommit && g_commitClearSeconds >= 60) {
        g_reliefCommit = false;
        Log("[mem] commit recovered (%.2f GB available + %.2f GB page file growth for a minute): LumenKeepArrayCapacity and UploadMemoryKeep back on",
            s.commitFree / kGB, s.commitGrow / kGB);
    }
}

// ---- the crash report ----

const char* RemovedReasonName(HRESULT hr)
{
    switch (static_cast<uint32_t>(hr)) {
    case 0: return "S_OK (not removed)";
    case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG (the GPU stopped making progress: driver timeout)";
    case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET";
    case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
    case 0x8007000E: return "E_OUTOFMEMORY";
    default: return "other";
    }
}

const char* DeviceStateName(D3D12_DRED_DEVICE_STATE s)
{
    switch (s) {
    case D3D12_DRED_DEVICE_STATE_HUNG: return "hung";
    case D3D12_DRED_DEVICE_STATE_FAULT: return "fault";
    case D3D12_DRED_DEVICE_STATE_PAGEFAULT: return "page fault";
    default: return "unknown";
    }
}

void Narrow(const wchar_t* w, const char* a, char* out, int size)
{
    out[0] = 0;
    if (w && *w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, nullptr, nullptr);
    else if (a && *a) strncpy_s(out, size, a, _TRUNCATE);
    else strcpy_s(out, size, "?");
}

const wchar_t* ContextAt(const D3D12_AUTO_BREADCRUMB_NODE1* n, UINT index)
{
    for (UINT i = 0; i < n->BreadcrumbContextsCount; ++i)
        if (n->pBreadcrumbContexts[i].BreadcrumbIndex == index) return n->pBreadcrumbContexts[i].pContextString;
    return nullptr;
}

void Append(char* buf, size_t size, size_t& len, const char* text)
{
    while (*text && len + 1 < size) buf[len++] = *text++;
    buf[len] = 0;
}

// One line per command list that the GPU had started but not finished: where it stopped, as the open pass markers.
void LogBreadcrumbs(const D3D12_AUTO_BREADCRUMB_NODE1* head)
{
    unsigned done = 0, notStarted = 0, inFlight = 0, shown = 0;
    for (const auto* n = head; n; n = n->pNext) {
        const UINT count = n->BreadcrumbCount;
        const UINT last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (last >= count) { ++done; continue; }
        if (last == 0) { ++notStarted; continue; }
        ++inFlight;
        if (shown >= 12) continue;
        ++shown;
        // The markers still open after the last completed operation (the game's DRED records markers only).
        const wchar_t* stack[24];
        int depth = 0;
        for (UINT i = 0; i < last && n->pCommandHistory; ++i) {
            const D3D12_AUTO_BREADCRUMB_OP op = n->pCommandHistory[i];
            if (op == D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT) {
                if (depth < 24) stack[depth] = ContextAt(n, i);
                ++depth;
            } else if (op == D3D12_AUTO_BREADCRUMB_OP_ENDEVENT && depth > 0) --depth;
        }
        char queue[96], list[96], line[900], part[160];
        Narrow(n->pCommandQueueDebugNameW, n->pCommandQueueDebugNameA, queue, sizeof queue);
        Narrow(n->pCommandListDebugNameW, n->pCommandListDebugNameA, list, sizeof list);
        size_t len = 0;
        line[0] = 0;
        for (int d = 0; d < depth && d < 24; ++d) {
            Narrow(stack[d], nullptr, part, sizeof part);
            if (d) Append(line, sizeof line, len, " > ");
            Append(line, sizeof line, len, part);
        }
        const D3D12_AUTO_BREADCRUMB_OP nextOp = n->pCommandHistory ? n->pCommandHistory[last] : D3D12_AUTO_BREADCRUMB_OP_SETMARKER;
        Narrow(ContextAt(n, last), nullptr, part, sizeof part);
        Log("[gpucrash]   %s, list %s: %u of %u operations done; open: %s; next: %s %s", queue, list, last, count,
            len ? line : "(none)",
            nextOp == D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT ? "begin" : nextOp == D3D12_AUTO_BREADCRUMB_OP_ENDEVENT ? "end" : "op",
            part);
    }
    Log("[gpucrash] DRED command lists: %u stopped part-way (%u shown above), %u not started, %u finished", inFlight, shown,
        notStarted, done);
}

void LogAllocations(const char* what, const D3D12_DRED_ALLOCATION_NODE1* n)
{
    char name[160];
    int i = 0;
    for (; n && i < 8; n = n->pNext, ++i) {
        Narrow(n->ObjectNameW, n->ObjectNameA, name, sizeof name);
        Log("[gpucrash]   %s: %s (type %d)", what, name, static_cast<int>(n->AllocationType));
    }
    if (!i) Log("[gpucrash]   %s: none", what);
}

void LogDred(ID3D12Device* device)
{
    const HRESULT reason = device->GetDeviceRemovedReason();
    Log("[gpucrash] device removed reason 0x%08X %s", static_cast<uint32_t>(reason), RemovedReasonName(reason));
    ID3D12DeviceRemovedExtendedData2* dred2 = nullptr;
    ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred2))) && dred2) {
        Log("[gpucrash] DRED device state: %s", DeviceStateName(dred2->GetDeviceState()));
        dred1 = dred2;
    } else if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred1))) || !dred1) {
        Log("[gpucrash] no DRED interface (DRED off?)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    const HRESULT hrCrumbs = dred1->GetAutoBreadcrumbsOutput1(&crumbs);
    if (SUCCEEDED(hrCrumbs)) LogBreadcrumbs(crumbs.pHeadAutoBreadcrumbNode);
    else Log("[gpucrash] DRED breadcrumbs not available (0x%08X: DRED off for this device?)", static_cast<uint32_t>(hrCrumbs));
    if (dred2) {
        D3D12_DRED_PAGE_FAULT_OUTPUT2 pf{};
        if (SUCCEEDED(dred2->GetPageFaultAllocationOutput2(&pf)) && pf.PageFaultVA) {
            Log("[gpucrash] GPU page fault at VA 0x%llX (flags 0x%X)", static_cast<unsigned long long>(pf.PageFaultVA),
                static_cast<unsigned>(pf.PageFaultFlags));
            LogAllocations("allocation there", pf.pHeadExistingAllocationNode);
            LogAllocations("recently freed there", pf.pHeadRecentFreedAllocationNode);
        } else Log("[gpucrash] no GPU page fault recorded");
    } else {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
        if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&pf)) && pf.PageFaultVA) {
            Log("[gpucrash] GPU page fault at VA 0x%llX", static_cast<unsigned long long>(pf.PageFaultVA));
            LogAllocations("allocation there", pf.pHeadExistingAllocationNode);
            LogAllocations("recently freed there", pf.pHeadRecentFreedAllocationNode);
        } else Log("[gpucrash] no GPU page fault recorded");
    }
    dred1->Release();
}

bool LogDredGuarded(ID3D12Device* device)
{
    __try {
        LogDred(device);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void LogSample(const char* prefix, const Sample& s, ULONGLONG now)
{
    Log("%s %+5.1f s: RAM free %.2f GB, commit free %.2f of %.1f GB, game private %.2f GB, working set %.2f GB, VRAM %.2f of %.2f GB, shared %.2f GB",
        prefix, (static_cast<double>(s.tick) - static_cast<double>(now)) / 1000.0, s.ramFree / kGB, s.commitFree / kGB,
        s.commitLimit / kGB, s.priv / kGB, s.ws / kGB, s.vram / kGB, s.vramBudget / kGB, s.shared / kGB);
}

std::atomic<bool> g_reported{false};
bool g_reportEnabled = false;

void WriteGpuCrashReport(const CONTEXT* ctx)
{
    Log("[gpucrash] the game reports a GPU crash (thread %lu); OblivionPerfFix %s state: parallel translate %s, memory relief %s/%s/%s",
        GetCurrentThreadId(), OPF_VERSION, RdgParallelTranslateActive() ? "on" : "off", g_reliefRam ? "RAM" : "-",
        g_reliefCommit ? "commit" : "-", g_reliefVram ? "VRAM" : "-");
    // The device: UE's TerminateOnGPUCrash got it as its argument, so it is in a register or on this stack.
    ULONG_PTR stackLo = 0, stackHi = 0;
    GetCurrentThreadStackLimits(&stackLo, &stackHi);
    auto* from = reinterpret_cast<const uintptr_t*>(ctx && ctx->Rsp > stackLo && ctx->Rsp < stackHi ? ctx->Rsp : stackLo);
    auto* to = reinterpret_cast<const uintptr_t*>(stackHi);
    if (to - from > 64 * 1024) to = from + 64 * 1024;  // 512 KB of stack at most
    int candidates = 0;
    if (ID3D12Device* device = FindDevice(ctx, from, to, &candidates)) {
        if (!LogDredGuarded(device)) Log("[gpucrash] reading DRED faulted");
        device->Release();
    } else Log("[gpucrash] the game's D3D12 device was not found on this thread (%d D3D12 objects seen): no DRED data", candidates);
    const ULONGLONG now = GetTickCount64();
    LogSample("[gpucrash] memory now", TakeSample(), now);
    // The last 30 seconds: every 5 s and the extremes.
    const unsigned next = g_ringNext.load();
    uint64_t ramMin = UINT64_MAX, commitMin = UINT64_MAX, vramMax = 0, budgetAtMax = 0;
    for (unsigned i = 0; i < kRing && i < next; ++i) {
        const Sample& s = g_ring[(next - 1 - i) % kRing];
        if (now - s.tick > 31000) break;
        ramMin = s.ramFree < ramMin ? s.ramFree : ramMin;
        commitMin = s.commitFree < commitMin ? s.commitFree : commitMin;
        if (s.vram > vramMax) vramMax = s.vram, budgetAtMax = s.vramBudget;
        if (i % 5 == 0) LogSample("[gpucrash] memory", s, now);
    }
    if (ramMin != UINT64_MAX)
        Log("[gpucrash] last 30 s: RAM free min %.2f GB, commit free min %.2f GB, VRAM max %.2f of %.2f GB", ramMin / kGB,
            commitMin / kGB, vramMax / kGB, budgetAtMax / kGB);
}

LONG CALLBACK GpuCrashHandler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != kUeGpuCrashException || g_reported.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;
    WriteGpuCrashReport(info->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}


}  // namespace

bool InstallGpuCrashReport()
{
    if (g_reportEnabled) return true;
    if (!AddVectoredExceptionHandler(1, &GpuCrashHandler)) {
        Log("[gpucrash] no exception handler: GPU crash report unavailable");
        return false;
    }
    g_reportEnabled = true;
    Log("[gpucrash] a GPU crash (driver timeout, device removed) will be written to this log with DRED and memory data");
    return true;
}

void UpdateMemory(const Config& cfg, MemoryRelief* relief)
{
    OpenAdapter();
    Sample s = TakeSample();
    g_ring[g_ringNext.load() % kRing] = s;
    g_ringNext.fetch_add(1);
    Interval& in = g_interval;
    if (s.ramFree < in.ramFreeMin) in.ramFreeMin = s.ramFree;
    if (s.commitFree < in.commitFreeMin) in.commitFreeMin = s.commitFree;
    if (s.priv > in.privMax) in.privMax = s.priv;
    if (s.vram > in.vramMax) in.vramMax = s.vram, in.vramBudgetAtMax = s.vramBudget;
    if (s.vramBudget && s.vram > s.vramBudget) ++in.overBudgetSeconds;
    UpdateRelief(s, cfg);
    relief->lumen = g_reliefCommit;
    relief->upload = g_reliefRam || g_reliefCommit || g_reliefVram;
    relief->translate = g_reliefVramTranslate;
    relief->osCache = g_reliefRam || g_reliefCommit;
}

void LogMemoryStats()
{
    const unsigned n = g_ringNext.load();
    if (!n) return;
    const Sample& s = g_ring[(n - 1) % kRing];
    Interval& in = g_interval;
    char vram[160] = "VRAM not known yet";
    if (s.vramBudget)
        sprintf_s(vram, "VRAM %.2f GB (max %.2f) of budget %.2f GB, %u s over budget, shared %.2f GB", s.vram / kGB,
                  in.vramMax / kGB, s.vramBudget / kGB, in.overBudgetSeconds, s.shared / kGB);
    char relief[32] = "";
    if (g_reliefRam) strcat_s(relief, "+RAM");
    if (g_reliefCommit) strcat_s(relief, "+commit");
    if (g_reliefVram) strcat_s(relief, g_reliefVramTranslate ? "+VRAM2" : "+VRAM1");
    Log("[mem] RAM free %.2f GB (min %.2f) of %.1f, commit free %.2f GB (min %.2f) of %.1f (page files can grow %.1f GB), game private %.2f GB (max %.2f), working set %.2f GB; %s; relief %s (episodes RAM %u, commit %u, VRAM %u of which helped %u)",
        s.ramFree / kGB, in.ramFreeMin / kGB, s.ramTotal / kGB, s.commitFree / kGB, in.commitFreeMin / kGB, s.commitLimit / kGB,
        s.commitGrow / kGB, s.priv / kGB, in.privMax / kGB, s.ws / kGB, vram, relief[0] ? relief + 1 : "off", g_ramEpisodes,
        g_commitEpisodes, g_vramRelief.Episodes(), g_vramRelief.Helped());
    in = Interval{};
}

// Self-test: the video-memory level, one second per step on a fresh state machine (reset = true starts one).
bool g_testTranslateStage = true;
void MemoryReliefTestAllowTranslate(bool allow) { g_testTranslateStage = allow; }

bool MemoryReliefTestCommitStep(uint64_t commitRoomMB)
{
    Sample s{};
    s.ramTotal = 32ull << 30;
    s.ramFree = 16ull << 30;
    s.commitLimit = 64ull << 30;
    s.commitFree = commitRoomMB << 20;
    Config cfg;  // the defaults: MemoryReliefCommitMB 1024, video-memory stages off
    UpdateRelief(s, cfg);
    return g_reliefCommit;
}

VramReliefEvent MemoryReliefTestVramStep(bool reset, uint64_t vramMB, uint64_t budgetMB, bool enabled, MemoryRelief* relief)
{
    static VramRelief r;
    if (reset) r = VramRelief{};
    r.AllowTranslateStage(g_testTranslateStage);
    const auto e = r.Step(vramMB << 20, budgetMB << 20, enabled);
    relief->upload = r.Active();
    relief->translate = r.Paused();
    return e;
}

// Self-test: the device search over a block of words (as on a stack), and the DRED part of the report.
void* GpuCrashReportTestFind(const void* words, size_t count)
{
    int candidates = 0;
    auto* lo = static_cast<const uintptr_t*>(words);
    return FindDevice(nullptr, lo, lo + count, &candidates);
}

bool GpuCrashReportTestDred(void* device) { return LogDredGuarded(static_cast<ID3D12Device*>(device)); }

}  // namespace opf
