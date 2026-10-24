// RtSceneBufferKeep: the ray tracing scene's TLAS buffer keeps its size instead of being re-created smaller, and the three
// buffers FRayTracingScene::Create makes directly are timed (for the stats line).
//
// UE 5.3 FRayTracingScene::Create (RayTracingScene.cpp:88, exe+0x2208A30 in build 19115871) re-creates the TLAS result
// buffer ("FRayTracingScene::SceneBuffer", a committed acceleration-structure buffer, the size rounded up to 16 MiB) when
// the scene needs more than it has, or less than half of it. Riding through the world the instance count goes up and down,
// so the buffer is re-created at the same places every pass. At 4K with the process near its video memory budget, the new
// allocation's residency is the render thread waiting in D3D12 (NtGdiDdDDIWaitForSynchronizationObjectFromCpu: the paging
// fence while video memory makes room) for ~180 ms: a 180-200 ms frame and a second one behind it (measured 2026-10-20,
// docs/research.md). Mode 1 removes the "less than half" re-creation: a buffer that is big enough stays. The TLAS built
// into it is the same (it is built at the start of the buffer it is given; the layers' views use the scene's own offsets),
// so the picture is identical; only the buffer's capacity differs (at most the largest size this session already needed).
// Growth re-creates it as before.
//
// How: the check "need > size: re-create; need < size / 2: re-create" is
//   48 3B C8 (cmp rcx,rax) | 77 0C (ja re-create) | 48 D1 E8 (shr rax,1) | 48 3B C8 (cmp rcx,rax) | 0F 83 rel32 (jae keep)
// The eight bytes ja/shr/cmp become a call to a stub (5 bytes + 3-byte nop) that does the same and, in mode 1, answers
// "keep" for the shrink case (CF clear for the jae that follows). Registers are left as the game's code leaves them.
// The diagnostic: the three `call [rax+0x140]` (RHICreateBuffer) in Create, recognised by the debug names they pass
// (L"FRayTracingScene::SceneBuffer" / "InstanceUploadBuffer" / "TransformUploadBuffer"), become call [rip+slot] to timers.
// Both are installed only at start-up (key != 0); after that 1 = keep, 2 = installed but off (the game's way, still timed),
// live; 0 after the install also means the game's way.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// The size rounding (16 MiB) and the check, build 19115871 exe+0x2208B9D:
// add rcx,0FFFFFFh; and rcx,0FFFFFFFFFF000000h; mov [rbp+78h],rcx; test rax,rax; je recreate; mov eax,[rax+20h];
// cmp rcx,rax; ja recreate; shr rax,1; cmp rcx,rax; jae keep
constexpr char kCheck[] =
    "48 81 C1 FF FF FF 00 48 81 E1 00 00 00 FF 48 89 4D ?? 48 85 C0 74 ?? 8B 40 ?? 48 3B C8 77 0C 48 D1 E8 48 3B C8 0F 83 ?? ?? ?? ??";
constexpr size_t kCheckJe = 21, kCheckJa = 29, kCheckShr = 31, kCheckCmp = 34, kCheckJae = 37, kCheckEnd = 43;
// Create's prologue: mov r11,rsp; push rbp; push r15; lea rbp,[r11-...]
constexpr char kPrologue[] = "4C 8B DC 55 41 57 49 8D AB";
constexpr size_t kPrologueSearch = 0x400, kFunctionSearch = 0xC00;
// A create call: mov rcx,[rip+GDynamicRHI] ... mov rax,[rcx]; call [rax+slot]
constexpr char kCreateCall[] = "48 8B 01 FF 90 ?? ?? 00 00";
constexpr size_t kNameSearch = 0x140, kRhiSearch = 0x40;

enum Site { kScene = 0, kInstanceUpload, kTransformUpload, kSites };
const wchar_t* const kNames[kSites] = {L"FRayTracingScene::SceneBuffer", L"FRayTracingScene::InstanceUploadBuffer",
                                       L"FRayTracingScene::TransformUploadBuffer"};
const char* const kShort[kSites] = {"SceneBuffer", "InstanceUploadBuffer", "TransformUploadBuffer"};

struct BufferDesc { uint32_t size, stride, usage; };  // FRHIBufferDesc
using CreateFn = void** (*)(void* rhi, void** out, uint8_t* cmdList, const BufferDesc* desc, uint32_t access, uint8_t* createInfo);

struct Sites {
    uint8_t* check = nullptr;
    uint8_t* function = nullptr;
    uint8_t* call[kSites] = {};  // the call [rax+slot] instructions
    uint32_t slot = 0;
};
Sites g;
bool g_installed = false;
int g_mode = 0;
// The stub's data (read-write, near the game's code): [0] keep flag (byte), [8] kept count, [16] shrinks re-created count.
uint8_t* g_data = nullptr;
uint8_t* g_stub = nullptr;
uint8_t* g_slots = nullptr;  // three hook pointers for call [rip+disp32]

std::atomic<uint64_t> s_creates[kSites], s_us[kSites], s_maxUs[kSites], s_slow[kSites];
std::atomic<uint32_t> s_lastSize[kSites];
uint64_t s_keptLogged = 0, s_shrinkLogged = 0;

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), freq);
}

void Max(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v)) {
    }
}

template <int N>
void** CreateHook(void* rhi, void** out, uint8_t* cmdList, const BufferDesc* desc, uint32_t access, uint8_t* createInfo)
{
    const CreateFn create = reinterpret_cast<CreateFn>((*reinterpret_cast<void***>(rhi))[g.slot / 8]);
    const uint64_t t0 = NowUs();
    void** const r = create(rhi, out, cmdList, desc, access, createInfo);
    const uint64_t us = NowUs() - t0;
    const uint32_t size = desc ? desc->size : 0;
    const uint32_t was = s_lastSize[N].exchange(size);
    s_creates[N].fetch_add(1, std::memory_order_relaxed);
    s_us[N].fetch_add(us, std::memory_order_relaxed);
    Max(s_maxUs[N], us);
    if (us >= 5000) s_slow[N].fetch_add(1, std::memory_order_relaxed);
    // Logged: TLAS buffer creations that change its size or take >= 1 ms, and any creation >= 5 ms. At the menu and on
    // loading screens the TLAS buffer is re-made every frame at the same size (up to ~126 lines/s if each were logged);
    // those are only counted (the periodic stats line), so the log's size limit is not reached in a long session.
    if (RtSceneLogCreate(N, size, was, us)) {
        Log("[rtscene] %s created: %.1f MiB (was %.1f MiB) in %.1f ms at QPC %llu ms", kShort[N], size / 1048576.0, was / 1048576.0,
            us / 1000.0, static_cast<unsigned long long>(t0 / 1000));
    }
    return r;
}

bool Fail(const char* why)
{
    Log("[rtscene] %s: RtSceneBufferKeep unavailable", why);
    return false;
}

uint32_t Disp32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// The stub (see the header): entered with the flags of the game's "cmp rcx,rax" (need vs size).
constexpr size_t kStubSize = 42;
bool BuildStub(uint8_t* code, const uint8_t* data, const uint8_t* shrBytes, const uint8_t* cmpBytes)
{
    uint8_t s[kStubSize] = {
        0x77, 0x25,                                // 0: ja grow (39)
        0, 0, 0,                                   // 2: shr rax,1 (the game's bytes)
        0, 0, 0,                                   // 5: cmp rcx,rax (the game's bytes)
        0x73, 0x1F,                                // 8: jae done (41): no shrink
        0x80, 0x3D, 0, 0, 0, 0, 0x00,              // 10: cmp byte [rip+flag],0
        0x74, 0x0A,                                // 17: je shrink (29)
        0xF0, 0x48, 0xFF, 0x05, 0, 0, 0, 0,        // 19: lock inc qword [rip+kept]
        0xF8, 0xC3,                                // 27: clc; ret (the jae keeps the buffer)
        0xF0, 0x48, 0xFF, 0x05, 0, 0, 0, 0,        // 29: shrink: lock inc qword [rip+shrinks]
        0xF9, 0xC3,                                // 37: stc; ret (re-created as in the game)
        0xF9, 0xC3,                                // 39: grow: stc; ret
        0xC3,                                      // 41: done: ret (CF clear from the cmp)
    };
    memcpy(s + 2, shrBytes, 3);
    memcpy(s + 5, cmpBytes, 3);
    bool ok = true;
    auto put = [&](size_t at, size_t end, const uint8_t* target) {
        const int64_t d64 = target - (code + end);
        if (d64 != static_cast<int32_t>(d64)) ok = false;
        const int32_t d = static_cast<int32_t>(d64);
        memcpy(s + at, &d, 4);
    };
    put(12, 17, data);
    put(23, 27, data + 8);
    put(33, 37, data + 16);
    if (!ok) return false;
    memcpy(code, s, sizeof s);
    return true;
}

}  // namespace

bool RtSceneLogCreate(int site, uint32_t size, uint32_t was, uint64_t us)
{
    return (site == kScene && (size != was || us >= 1000)) || us >= 5000;
}

bool FindRtSceneKeep()
{
    if (g.check) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kCheck));
    if (hits.size() != 1) {
        Log("[rtscene] the TLAS buffer's size check found %zu times (expected 1): RtSceneBufferKeep unavailable", hits.size());
        return false;
    }
    Sites s;
    s.check = hits[0];
    // je and jae both leave the region: je to the re-creation right after the jae, ja to the same place
    if (s.check[kCheckJe] != 0x74 || kCheckJe + 2 + static_cast<int8_t>(s.check[kCheckJe + 1]) != static_cast<int>(kCheckEnd))
        return Fail("the null check does not go to the re-creation");
    const Pattern prologue = ParsePattern(kPrologue);
    for (uint8_t* p = s.check - 1; p > s.check - kPrologueSearch; --p)
        if (MatchesAt(p, prologue)) { s.function = p; break; }
    if (!s.function) return Fail("FRayTracingScene::Create's start not found");
    // the create calls, named by the lea of their debug name before them
    const Pattern call = ParsePattern(kCreateCall);
    uint8_t* const end = s.function + kFunctionSearch;
    void** rhi = nullptr;
    for (uint8_t* p = s.check; p < end; ++p) {
        if (!MatchesAt(p, call)) continue;
        const uint32_t slot = Disp32(p + 5);
        // mov rcx,[rip+GDynamicRHI] shortly before
        void** r = nullptr;
        for (uint8_t* q = p - 7; q > p - kRhiSearch; --q)
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x0D) { r = reinterpret_cast<void**>(RipTarget(q, 3, 7)); break; }
        if (!r || !game.data.Contains(r)) continue;
        int which = -1;
        for (uint8_t* q = p - 7; q > p - kNameSearch && which < 0; --q) {
            if (q[0] != 0x48 || q[1] != 0x8D || (q[2] & 0xC7) != 0x05) continue;
            const uint8_t* t = RipTarget(q, 3, 7);
            if (!game.rdata.Contains(t)) continue;
            for (int k = 0; k < kSites; ++k) {
                const size_t n = (wcslen(kNames[k]) + 1) * sizeof(wchar_t);
                if (game.rdata.Contains(t + n - 1) && memcmp(t, kNames[k], n) == 0) { which = k; break; }
            }
        }
        if (which < 0 || s.call[which]) continue;
        if (s.slot && s.slot != slot) return Fail("the create calls use different RHI slots");
        if (rhi && rhi != r) return Fail("the create calls read different RHI pointers");
        s.slot = slot;
        rhi = r;
        s.call[which] = p + 3;
    }
    if (!s.call[kScene]) return Fail("the TLAS buffer's creation not found after the check");
    if (s.call[kScene] < s.check + kCheckEnd || s.call[kScene] > s.check + 0x200) return Fail("the TLAS buffer's creation is not where expected");
    if (s.slot > 0x800 || (s.slot & 7)) return Fail("unexpected RHI slot");
    g = s;
    Log("[rtscene] FRayTracingScene::Create exe+0x%X: TLAS size check exe+0x%X, RHICreateBuffer (slot +0x%X) for SceneBuffer exe+0x%X, "
        "InstanceUploadBuffer exe+0x%X, TransformUploadBuffer exe+0x%X",
        static_cast<unsigned>(g.function - game.base), static_cast<unsigned>(g.check - game.base), g.slot,
        static_cast<unsigned>(g.call[kScene] - game.base), g.call[kInstanceUpload] ? static_cast<unsigned>(g.call[kInstanceUpload] - game.base) : 0u,
        g.call[kTransformUpload] ? static_cast<unsigned>(g.call[kTransformUpload] - game.base) : 0u);
    return true;
}

bool InstallRtSceneKeep(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!g.check && !FindRtSceneKeep()) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    uint8_t* const data = AllocNearGame(64);
    uint8_t* const stub = AllocNearGame(64);
    uint8_t* const slots = AllocNearGame(64);
    if (!data || !stub || !slots) return Fail("no free memory near the game's code");
    memset(data, 0, 64);
    if (!BuildStub(stub, data, g.check + kCheckShr, g.check + kCheckCmp)) return Fail("the stub's data is out of reach");
    DWORD old;
    if (!VirtualProtect(stub, 64, PAGE_EXECUTE_READ, &old)) return Fail("could not make the stub executable");
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    void* const hooks[kSites] = {reinterpret_cast<void*>(&CreateHook<kScene>), reinterpret_cast<void*>(&CreateHook<kInstanceUpload>),
                                 reinterpret_cast<void*>(&CreateHook<kTransformUpload>)};
    memcpy(slots, hooks, sizeof hooks);
    if (!VirtualProtect(slots, 64, PAGE_READONLY, &old)) return Fail("could not make the timer slots read-only");
    // the check: ja/shr/cmp (8 bytes) -> call stub; nop dword (3 bytes)
    uint8_t* const at = g.check + kCheckJa;
    const int64_t rel = stub - (at + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the stub is out of reach");
    uint8_t patch[8] = {0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x00};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 1, &rel32, 4);
    // the timers (each six bytes: call [rax+slot] -> call [rip+slot pointer]), all prepared and in reach before anything
    // is written
    uint8_t timer[kSites][6] = {}, orig[kSites][6] = {};
    for (int k = 0; k < kSites; ++k) {
        uint8_t* const c = g.call[k];
        if (!c) continue;
        const int64_t r = (slots + 8 * k) - (c + 6);
        if (r != static_cast<int32_t>(r)) return Fail("a timer slot is out of reach");
        const int32_t r32 = static_cast<int32_t>(r);
        timer[k][0] = 0xFF;
        timer[k][1] = 0x15;
        memcpy(timer[k] + 2, &r32, 4);
        memcpy(orig[k], c, 6);
    }
    // the check first: if it cannot be written, nothing is patched; if a timer cannot be, the ones written and the
    // check are put back
    uint8_t origCheck[8];
    memcpy(origCheck, at, sizeof origCheck);
    if (!WriteCode(at, patch)) return Fail("could not patch the size check");
    for (int k = 0; k < kSites; ++k) {
        if (!g.call[k] || WriteCode(g.call[k], timer[k])) continue;
        for (int j = 0; j < k; ++j)
            if (g.call[j]) WriteCode(g.call[j], orig[j]);
        WriteCode(at, origCheck);
        return Fail("could not patch a create call (the patches are undone)");
    }
    g_data = data;
    g_stub = stub;
    g_slots = slots;
    g_installed = true;
    Log("[rtscene] installed: TLAS buffer size check through the stub, the three buffer creations timed");
    return true;
}

void SetRtSceneKeep(int mode)
{
    if (!g_installed) return;
    const bool keep = mode == 1;
    if (g_mode == mode) return;
    g_mode = mode;
    reinterpret_cast<volatile uint8_t*>(g_data)[0] = keep ? 1 : 0;
    Log(keep ? "[rtscene] on: the TLAS buffer is not re-created smaller (grows as before)"
             : "[rtscene] timing only: the TLAS buffer is re-created as in the game");
}

void LogRtSceneKeepStats()
{
    if (!g_installed) return;
    const uint64_t kept = reinterpret_cast<volatile uint64_t*>(g_data + 8)[0];
    const uint64_t shrinks = reinterpret_cast<volatile uint64_t*>(g_data + 16)[0];
    uint64_t creates[kSites], us[kSites], mx[kSites], slow[kSites], total = 0;
    for (int k = 0; k < kSites; ++k) {
        creates[k] = s_creates[k].exchange(0);
        us[k] = s_us[k].exchange(0);
        mx[k] = s_maxUs[k].exchange(0);
        slow[k] = s_slow[k].exchange(0);
        total += creates[k];
    }
    if (!total && kept == s_keptLogged && shrinks == s_shrinkLogged) return;
    Log("[rtscene] since last report: TLAS buffer kept instead of shrinking in %llu frames, re-created smaller %llu times; creations "
        "SceneBuffer %llu (%.1f ms, longest %.1f, %llu >= 5 ms; now %.0f MiB), InstanceUpload %llu (%.1f ms, longest %.1f), TransformUpload "
        "%llu (%.1f ms, longest %.1f)",
        kept - s_keptLogged, shrinks - s_shrinkLogged, creates[0], us[0] / 1000.0, mx[0] / 1000.0, slow[0],
        s_lastSize[0].load() / 1048576.0, creates[1], us[1] / 1000.0, mx[1] / 1000.0, creates[2], us[2] / 1000.0, mx[2] / 1000.0);
    s_keptLogged = kept;
    s_shrinkLogged = shrinks;
}

// Self-test: the stub against the game's check, executed through a small thunk that does the game's "mov rax,size;
// cmp rcx,rax" before calling it and turns the following jae into 1 (keep) / 0 (re-create).
int RtSceneKeepTestDecide(uint64_t need, uint64_t size)
{
    static uint8_t* thunk = nullptr;
    if (!g_installed) return -1;
    if (!thunk) {
        thunk = AllocNearGame(64);
        if (!thunk) return -1;
        uint8_t t[] = {0x48, 0x8B, 0xC2,              // mov rax,rdx
                       0x48, 0x3B, 0xC8,              // cmp rcx,rax
                       0xE8, 0, 0, 0, 0,              // call stub
                       0x73, 0x03,                    // jae keep
                       0x31, 0xC0, 0xC3,              // xor eax,eax; ret
                       0xB8, 0x01, 0, 0, 0, 0xC3};    // keep: mov eax,1; ret
        const int32_t rel = static_cast<int32_t>(g_stub - (thunk + 11));
        memcpy(t + 7, &rel, 4);
        memcpy(thunk, t, sizeof t);
        DWORD old;
        VirtualProtect(thunk, 64, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), thunk, 64);
    }
    return reinterpret_cast<int (*)(uint64_t, uint64_t)>(thunk)(need, size);
}

bool RtSceneKeepTestPatched()
{
    if (!g_installed) return false;
    const uint8_t* at = g.check + kCheckJa;
    if (at[0] != 0xE8 || RipTarget(at, 1, 5) != g_stub || at[5] != 0x0F || at[6] != 0x1F || at[7] != 0x00) return false;
    if (g.check[kCheckJae] != 0x0F || g.check[kCheckJae + 1] != 0x83) return false;
    for (int k = 0; k < kSites; ++k)
        if (g.call[k] && (g.call[k][0] != 0xFF || g.call[k][1] != 0x15 || *reinterpret_cast<void**>(RipTarget(g.call[k], 2, 6)) == nullptr))
            return false;
    return g.call[kInstanceUpload] && g.call[kTransformUpload];
}

}  // namespace opf
