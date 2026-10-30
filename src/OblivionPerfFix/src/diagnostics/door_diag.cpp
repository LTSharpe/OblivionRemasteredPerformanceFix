// Diagnostic DoorDiag (off by default; [Fixes] DoorDiag=1, read at start-up). Not a fix: with
// DoorDiagEarlyStream=0 nothing the game does changes, the hooks only time and count. Written for the feasibility check
// of "stream the destination's textures behind the loading screen" (StreamBehindLoadingScreen). Logs, tag [doordiag]:
//
//  - the loading-screen timeline of every door, city gate or fast travel (game thread, once per frame from
//    NpcCreationPacing's frame hook): when the transition's freeze starts, when its heavy loading frames end and the
//    loading screen only idles, and when the freeze ends;
//  - every "stream all resources" call (the game's fade-to-game handler makes one; it blocks until the new view's
//    textures are streamed in): its duration, its caller and the time since the idle loading screen began;
//  - the render-target pool: every bulk free of unused targets (as on world clean-up while
//    r.bFlushRenderTargetsOnWorldCleanup > 0) with what it freed and its caller, and every slow or allocating
//    find-free-element call (name, time, pool memory change); for the virtual shadow map pools also how many pooled
//    targets of the same size were free or still held when a new one was made.
//
// DoorDiagEarlyStream=1 (experiment, switchable live): once per transition, when the loading screen starts to idle,
// the game thread calls the same "stream all resources" once (time limit 0.5 s). The game's own call at the fade-in
// still runs; if it gets short, the destination's textures can be streamed behind the loading screen. While the early
// call runs the loading screen does not draw (up to 0.5 s of its idle time).
//
// Found by byte pattern (build 19115871 addresses in the comments); anything unexpected: [doordiag] not installed.
//  - the flush sites (exe+0x2285EF6, exe+0x2292181): cmp dword [flag],0 / jle +0Ch / lea rcx,[pool] / call free-all;
//    all matches must name the same flag, pool and function;
//  - free-all (exe+0x26DD640) and find-free-element (exe+0x26DC760): their 11-byte prologues (pushes, sub rsp; nothing
//    rip-relative) move to trampolines, the entry becomes a jmp to a stub (one atomic 8-byte write); find-free-element
//    must be called with lea rcx,[pool] somewhere;
//  - the element-size function (exe+0x26D67E0), called by free-all;
//  - stream-all (exe+0x30D6CB0): its single vtable slot is replaced (one pointer write).
#include "common.h"

#include <atomic>
#include <cstring>
#include <intrin.h>

namespace opf {
namespace {

constexpr char kFlushSite[] = "83 3D ?? ?? ?? ?? 00 7E 0C 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";
constexpr char kFreeAll[] = "40 53 57 41 55 41 57 48 83 EC 28 44 8B 79 28";
constexpr char kFindFree[] =
    "40 53 55 56 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 68 48 8B DA 4C 89 4C 24 20 49 8B F0";
constexpr char kElemSize[] = "48 89 5C 24 08 57 48 83 EC 20 8B 41 54 48 8B D9 85 C0 74 ?? 83 79 58 00";
constexpr char kStreamAll[] =
    "40 53 48 83 EC 40 F3 0F 10 05 ?? ?? ?? ?? 48 8B D9 0F 29 74 24 30 0F 28 F1 0F 29 7C 24 20 F3 0F 10 3D";
constexpr size_t kPrologue = 11;

// World drawing (2026-10-22, for "draw the frozen world behind the loading screen"): Unreal's per-viewport switch
// bDisableWorldRendering. UWorld::GetGameViewport (exe+0x385EE90): mov rdx,rcx; mov rcx,[GEngine];
// call GetWorldContextFromWorld; test rax,rax; je; mov rax,[rax+200h] (the context's GameViewport)
constexpr char kGetViewport[] =
    "48 83 EC 28 48 8B D1 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 74 0C 48 8B 80 ?? ?? 00 00 48 83 C4 28 C3";
// UGameplayStatics::SetEnableWorldRendering's exec thunk (exe+0x31D8600), its tail: call GetGameViewport; test; je;
// mov ecx,[rax+6Ch]; movzx edx,sil; and ecx,~2; xor edx,1; add edx,edx; or edx,ecx; mov [rax+6Ch],edx (bit 1 = !bEnable)
constexpr char kEnableWorldRendering[] =
    "E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 48 ?? 40 0F B6 D6 83 E1 FD 83 F2 01 03 D2 0B D1 89 50";
// UEngine::GetCurrentPlayWorld (exe+0x37E3350), as in npc_pacing.cpp
constexpr char kCurrentPlayWorld[] =
    "48 89 5C 24 08 48 8B 81 ?? ?? 00 00 33 DB 48 63 89 ?? ?? 00 00 44 8B D3 48 8D 0C C8 48 3B C1 74 ?? 4C 8B 00 48 85 D2 "
    "74 ?? 4D 8B 88 ?? ?? 00 00 4C 3B CA 74 ?? 4D 85 D2 75 ?? 41 80 38 01 75 ?? 4D 8B 90 ?? ?? 00 00 48 83 C0 08 48 3B C1 75 D1";
using PlayWorldFn = void* (*)(void* engine, void* possiblePlayWorld);
using GameViewportFn = void* (*)(void* world);

// pool layout (from find-free-element / free-all): hash per element +0x10, elements +0x20, count +0x28, memory +0x40;
// an element's reference count +0x68 (1 = only the pool holds it)
constexpr size_t kPoolElems = 0x20, kPoolCount = 0x28, kPoolMemory = 0x40, kElemRefs = 0x68;

using FindFreeFn = uint64_t (*)(uint8_t* pool, void* desc, void** out, const wchar_t* name);
using FreeAllFn = void (*)(uint8_t* pool);
using ElemSizeFn = int32_t (*)(void* elem);
using StreamAllFn = void (*)(void* manager, float timeLimit);

struct State {
    bool found = false, installed = false;
    uint8_t* pool = nullptr;
    int32_t* flushFlag = nullptr;
    uint8_t* findFree = nullptr;
    uint8_t* freeAll = nullptr;
    uint8_t* elemSize = nullptr;
    uint8_t* streamAll = nullptr;
    void** streamAllSlot = nullptr;
    FindFreeFn origFindFree = nullptr;  // trampolines
    FreeAllFn origFreeAll = nullptr;
    StreamAllFn origStreamAll = nullptr;
    std::atomic<void*> manager{nullptr};  // the streaming manager seen in a stream-all call
    std::atomic<bool> earlyStream{false};
    // loading-screen timeline (game thread only)
    bool loading = false, heavySeen = false, idleLogged = false, earlyDone = false;
    int quiet = 0;
    uint64_t lastFrameUs = 0, loadStartUs = 0, idleStartUs = 0;
    // counters (any thread)
    std::atomic<uint64_t> findCalls{0}, findSlow{0}, findAlloc{0}, flushes{0}, streamCalls{0};
    // world drawing (game thread only): the viewport's bDisableWorldRendering bit, logged when it changes
    void** gEngine = nullptr;
    PlayWorldFn playWorld = nullptr;
    GameViewportFn gameViewport = nullptr;
    uint32_t drawFlagOffset = 0;
    int drawOff = -1;  // last value seen: 1 = world drawing off, 0 = on, -1 = unknown
};
State g;
thread_local bool t_early = false;  // the stream-all being timed is DoorDiagEarlyStream's own

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), freq);
}

unsigned Rva(const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - Game().base); }

// jmp [rip+0]; dq target
void WriteAbsJump(uint8_t* at, const void* target)
{
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + sizeof jmp, &target, sizeof target);
}

// The function's first 5 bytes become "jmp rel32 stub" in one atomic 8-byte write.
bool PatchEntry(uint8_t* fn, uint8_t* stub, bool beforeGameCode)
{
    const int64_t rel = stub - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t insn[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    if ((reinterpret_cast<uintptr_t>(fn) & 7) == 0) {
        auto* q = reinterpret_cast<volatile LONG64*>(fn);
        DWORD old;
        if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const LONG64 before = *q;
        LONG64 after = before;
        memcpy(&after, insn, sizeof insn);
        const bool ok = InterlockedCompareExchange64(q, after, before) == before;
        VirtualProtect(fn, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 8);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, insn);
}

// The only qword in .rdata that holds this code address (a vtable slot), or nullptr.
void** FindSlot(const GameModule& game, const uint8_t* target)
{
    void** found = nullptr;
    int hits = 0;
    const uintptr_t want = reinterpret_cast<uintptr_t>(target);
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uintptr_t*>(p) == want) { found = reinterpret_cast<void**>(p); ++hits; }
    return hits == 1 ? found : nullptr;
}

uint8_t* FindOne(const GameModule& game, const char* pattern)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}

void Narrow(const wchar_t* w, char* out, size_t n)
{
    size_t i = 0;
    __try {
        for (; w && w[i] && i + 1 < n; ++i) out[i] = (w[i] >= 32 && w[i] < 127) ? static_cast<char>(w[i]) : '?';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    out[i] = 0;
}

// The pool's element count, memory figure, and its elements that only the pool holds (count and memory), SEH-guarded.
struct PoolStat {
    uint32_t count = 0, free = 0;
    int32_t memory = 0, freeMemory = 0;
};
bool ReadPool(const uint8_t* pool, PoolStat* s, bool sizes)
{
    __try {
        s->count = *reinterpret_cast<const uint32_t*>(pool + kPoolCount);
        s->memory = *reinterpret_cast<const int32_t*>(pool + kPoolMemory);
        void* const* elems = *reinterpret_cast<void* const* const*>(pool + kPoolElems);
        for (uint32_t i = 0; elems && i < s->count && i < 100000; ++i) {
            uint8_t* e = static_cast<uint8_t*>(elems[i]);
            if (!e || *reinterpret_cast<const int32_t*>(e + kElemRefs) != 1) continue;
            ++s->free;
            if (sizes) s->freeMemory += reinterpret_cast<ElemSizeFn>(g.elemSize)(e);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// For a newly made element: pooled elements of the same size that are free (refcount 1) or still held (> 1).
bool SameSize(const uint8_t* pool, void* made, int32_t* size, uint32_t* freeSame, uint32_t* heldSame)
{
    __try {
        *size = reinterpret_cast<ElemSizeFn>(g.elemSize)(made);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(pool + kPoolCount);
        void* const* elems = *reinterpret_cast<void* const* const*>(pool + kPoolElems);
        for (uint32_t i = 0; elems && i < count && i < 100000; ++i) {
            uint8_t* e = static_cast<uint8_t*>(elems[i]);
            if (!e || e == made || reinterpret_cast<ElemSizeFn>(g.elemSize)(e) != *size) continue;
            if (*reinterpret_cast<const int32_t*>(e + kElemRefs) == 1) ++*freeSame; else ++*heldSame;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

double SinceIdleMs(uint64_t now) { return g.idleStartUs ? (now - g.idleStartUs) / 1000.0 : -1.0; }

uint64_t FindFreeHook(uint8_t* pool, void* desc, void** out, const wchar_t* name)
{
    int32_t memBefore = 0;
    __try { memBefore = *reinterpret_cast<const int32_t*>(pool + kPoolMemory); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const void* before = out ? *out : nullptr;
    const uint64_t t0 = NowUs();
    const uint64_t result = g.origFindFree(pool, desc, out, name);
    const uint64_t t1 = NowUs();
    g.findCalls.fetch_add(1, std::memory_order_relaxed);
    int32_t memAfter = memBefore;
    __try { memAfter = *reinterpret_cast<const int32_t*>(pool + kPoolMemory); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const bool kept = (result & 0xFF) != 0;  // the target it already had still fits
    const bool grew = memAfter > memBefore;
    const bool slow = t1 - t0 >= 500;
    if (kept && !slow) return result;
    if (grew) g.findAlloc.fetch_add(1, std::memory_order_relaxed);
    if (slow) g.findSlow.fetch_add(1, std::memory_order_relaxed);
    char n[96];
    Narrow(name, n, sizeof n);
    const bool vsm = strstr(n, "Shadow.Virtual") != nullptr;
    if (!grew && !slow && !vsm) return result;
    void* const made = out ? *out : nullptr;
    int32_t size = 0;
    uint32_t freeSame = 0, heldSame = 0;
    if (vsm && made && made != before) SameSize(pool, made, &size, &freeSame, &heldSame);
    Log("[doordiag] pool %s \"%s\": %.2f ms, pool memory %+d (now %d), thread %lu%s; same-size pooled targets: %u free, %u held "
        "(size %d); %.0f ms after the idle loading screen began",
        grew ? "MISS (new target)" : (kept ? "kept" : "reused"), n, (t1 - t0) / 1000.0, memAfter - memBefore, memAfter,
        GetCurrentThreadId(), made != before ? "" : " (same target)", freeSame, heldSame, size, SinceIdleMs(t1));
    return result;
}

void FreeAllHook(uint8_t* pool)
{
    void* const caller = _ReturnAddress();
    PoolStat a{}, b{};
    const bool okA = ReadPool(pool, &a, true);
    const uint64_t t0 = NowUs();
    g.origFreeAll(pool);
    const uint64_t t1 = NowUs();
    const bool okB = ReadPool(pool, &b, false);
    g.flushes.fetch_add(1, std::memory_order_relaxed);
    if (!okA || !okB || a.free == 0) return;
    Log("[doordiag] pool flush (all unused) from exe+0x%X: %u of %u targets freed (memory %d -> %d, freed %d), %.2f ms",
        Rva(caller), a.free, a.count, a.memory, b.memory, a.freeMemory, (t1 - t0) / 1000.0);
}

void StreamAllHook(void* manager, float timeLimit)
{
    void* const caller = _ReturnAddress();
    g.manager.store(manager, std::memory_order_relaxed);
    const uint64_t t0 = NowUs();
    g.origStreamAll(manager, timeLimit);
    const uint64_t t1 = NowUs();
    g.streamCalls.fetch_add(1, std::memory_order_relaxed);
    Log("[doordiag] stream-all%s from exe+0x%X (time limit %.2f s): %.1f ms; %.0f ms after the idle loading screen began; "
        "ends at qpc %.1f ms",
        t_early ? " (DoorDiagEarlyStream)" : "", t_early ? 0u : Rva(caller), timeLimit, (t1 - t0) / 1000.0, SinceIdleMs(t1),
        t1 / 1000.0);
}

bool Find(const GameModule& game)
{
    const auto sites = FindAll(game.text, ParsePattern(kFlushSite));
    for (uint8_t* s : sites) {
        uint8_t* const flag = RipTarget(s, 2, 7);  // cmp dword [rip+disp32],imm8: next instruction at +7
        uint8_t* const pool = RipTarget(s + 9, 3, 7);
        uint8_t* const fn = RipTarget(s + 16, 1, 5);
        if (!MatchesAt(fn, ParsePattern(kFreeAll))) continue;
        if (!g.freeAll) { g.flushFlag = reinterpret_cast<int32_t*>(flag); g.pool = pool; g.freeAll = fn; continue; }
        if (g.flushFlag != reinterpret_cast<int32_t*>(flag) || g.pool != pool || g.freeAll != fn) {
            Log("[doordiag] the world clean-up flush sites disagree (exe+0x%X): not installed", Rva(s));
            return false;
        }
    }
    if (!g.freeAll) {
        Log("[doordiag] world clean-up flush site not found (%zu candidates): not installed", sites.size());
        return false;
    }
    g.findFree = FindOne(game, kFindFree);
    g.elemSize = FindOne(game, kElemSize);
    g.streamAll = FindOne(game, kStreamAll);
    if (!g.findFree || !g.elemSize || !g.streamAll) {
        Log("[doordiag] find-free-element %p, element size %p, stream-all %p: not all found once: not installed", g.findFree,
            g.elemSize, g.streamAll);
        return false;
    }
    // find-free-element must be called with the pool (lea rcx,[pool] ... call), free-all must call the size function
    int poolCalls = 0;
    for (uint8_t* lea : FindAll(game.text, ParsePattern("48 8D 0D ?? ?? ?? ??"))) {
        if (RipTarget(lea, 3, 7) != g.pool) continue;
        for (uint8_t* p = lea + 7; p < lea + 0x60 && p + 5 <= game.text.end; ++p)
            if (*p == 0xE8 && RipTarget(p, 1, 5) == g.findFree) { ++poolCalls; break; }
    }
    bool sizeCall = false;
    for (uint8_t* p = g.freeAll; p < g.freeAll + 0x80; ++p)
        if (*p == 0xE8 && RipTarget(p, 1, 5) == g.elemSize) { sizeCall = true; break; }
    g.streamAllSlot = FindSlot(game, g.streamAll);
    if (poolCalls < 5 || !sizeCall || !g.streamAllSlot) {
        Log("[doordiag] checks failed (find-free-element called with the pool %d times, size call %d, stream-all slot %p): "
            "not installed", poolCalls, sizeCall ? 1 : 0, static_cast<void*>(g.streamAllSlot));
        return false;
    }
    // world drawing: optional (the rest of the diagnostic works without it)
    {
        uint8_t* const vp = FindOne(game, kGetViewport);
        uint8_t* const pw = FindOne(game, kCurrentPlayWorld);
        uint8_t* const sw = FindOne(game, kEnableWorldRendering);
        if (vp && pw && sw && RipTarget(sw, 1, 5) == vp && sw[0x1D] == sw[0x0C]) {
            g.gEngine = reinterpret_cast<void**>(RipTarget(vp + 7, 3, 7));
            g.playWorld = reinterpret_cast<PlayWorldFn>(pw);
            g.gameViewport = reinterpret_cast<GameViewportFn>(vp);
            g.drawFlagOffset = sw[0x0C];  // the field the switch reads and writes (0x6C)
            Log("[doordiag] world drawing: viewport getter exe+0x%X, GEngine exe+0x%X, bDisableWorldRendering = bit 1 of "
                "viewport+0x%X (from SetEnableWorldRendering exe+0x%X)", Rva(vp), Rva(g.gEngine), g.drawFlagOffset, Rva(sw));
        } else {
            Log("[doordiag] world drawing: viewport getter %p, current world %p, switch %p (or checks failed): not logged", vp,
                pw, sw);
        }
    }
    g.found = true;
    Log("[doordiag] pool exe+0x%X, flush flag exe+0x%X (now %d), free-all exe+0x%X, find-free-element exe+0x%X (%d pool call "
        "sites), element size exe+0x%X, stream-all exe+0x%X (slot exe+0x%X)", Rva(g.pool), Rva(g.flushFlag), *g.flushFlag,
        Rva(g.freeAll), Rva(g.findFree), poolCalls, Rva(g.elemSize), Rva(g.streamAll), Rva(g.streamAllSlot));
    return true;
}

}  // namespace

bool FindDoorDiag(DoorDiagInfoOut* out)
{
    if (!g.found && !Find(Game())) return false;
    if (out) {
        out->pool = g.pool;
        out->flushFlag = g.flushFlag;
        out->freeAll = g.freeAll;
        out->findFree = g.findFree;
        out->elemSize = g.elemSize;
        out->streamAll = g.streamAll;
        out->streamAllSlot = g.streamAllSlot;
    }
    return true;
}

bool InstallDoorDiag(bool beforeGameCode)
{
    if (g.installed) return true;
    if (!FindDoorDiag(nullptr)) return false;
    if (!FindFreezeSubsystem()) Log("[doordiag] freeze subsystem not found: no loading-screen timeline");
    constexpr size_t kRegion = 0x1000;
    uint8_t* const region = AllocNearGame(kRegion);
    if (!region) {
        Log("[doordiag] no free memory near the game's code: not installed");
        return false;
    }
    void* findOrig = nullptr;
    void* freeOrig = nullptr;
    // trampolines first (region + 0x00 / + 0x40), then the entries
    WriteAbsJump(region, reinterpret_cast<const void*>(&FindFreeHook));
    memcpy(region + 0x20, g.findFree, kPrologue);
    WriteAbsJump(region + 0x20 + kPrologue, g.findFree + kPrologue);
    findOrig = region + 0x20;
    WriteAbsJump(region + 0x40, reinterpret_cast<const void*>(&FreeAllHook));
    memcpy(region + 0x60, g.freeAll, kPrologue);
    WriteAbsJump(region + 0x60 + kPrologue, g.freeAll + kPrologue);
    freeOrig = region + 0x60;
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, kRegion);
    g.origFindFree = reinterpret_cast<FindFreeFn>(findOrig);
    g.origFreeAll = reinterpret_cast<FreeAllFn>(freeOrig);
    g.origStreamAll = reinterpret_cast<StreamAllFn>(g.streamAll);
    if (!PatchEntry(g.findFree, region, beforeGameCode) || !PatchEntry(g.freeAll, region + 0x40, beforeGameCode) ||
        !WritePointer(g.streamAllSlot, reinterpret_cast<void*>(&StreamAllHook))) {
        Log("[doordiag] patching failed: diagnostic partly or not installed (restart the game without DoorDiag)");
        return false;
    }
    g.installed = true;
    Log("[doordiag] installed: loading-screen timeline, stream-all timing, render-target pool misses and flushes");
    return true;
}

void SetDoorDiagEarlyStream(bool on)
{
    if (g.earlyStream.exchange(on) != on && g.installed)
        Log("[doordiag] DoorDiagEarlyStream %s", on ? "on (experiment: stream-all once when the loading screen idles)" : "off");
}

namespace {
// The game viewport's bDisableWorldRendering bit (1 = the world is not drawn), or -1; game thread, SEH-guarded.
int ReadWorldDrawingOff()
{
    if (!g.gameViewport) return -1;
    __try {
        void* const engine = *g.gEngine;
        if (!engine) return -1;
        void* const world = g.playWorld(engine, nullptr);
        if (!world) return -1;
        uint8_t* const vp = static_cast<uint8_t*>(g.gameViewport(world));
        if (!vp) return -1;
        return (*reinterpret_cast<const uint32_t*>(vp + g.drawFlagOffset) >> 1) & 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

void DoorDiagFrameEnd()
{
    if (!g.installed) return;
    const uint64_t now = NowUs();
    const uint64_t frameUs = g.lastFrameUs ? now - g.lastFrameUs : 0;
    g.lastFrameUs = now;
    const int drawOff = ReadWorldDrawingOff();
    if (drawOff >= 0 && drawOff != g.drawOff) {
        Log("[doordiag] world drawing %s (viewport bDisableWorldRendering %d), %s%.0f ms after the freeze began; qpc %.1f ms",
            drawOff ? "OFF" : "on", drawOff, g.loading ? "" : "not frozen, ", g.loadStartUs ? (now - g.loadStartUs) / 1000.0 : 0.0,
            now / 1000.0);
        g.drawOff = drawOff;
    }
    int32_t named = -1, mode = -1;
    const bool frozen = ReadMenuFreeze(&named, &mode) && mode == 1 && named == 0;
    if (frozen && !g.loading) {
        g.loading = true;
        g.heavySeen = g.idleLogged = g.earlyDone = false;
        g.quiet = 0;
        g.loadStartUs = now;
        g.idleStartUs = 0;
        Log("[doordiag] transition freeze starts (loading screen)");
        return;
    }
    if (!frozen && g.loading) {
        g.loading = false;
        Log("[doordiag] transition freeze ends after %.0f ms (idle part %.0f ms)", (now - g.loadStartUs) / 1000.0,
            g.idleStartUs ? (now - g.idleStartUs) / 1000.0 : 0.0);
        return;
    }
    if (!g.loading) return;
    if (frameUs > 100000) {
        g.heavySeen = true;
        g.quiet = 0;
        g.idleLogged = false;  // heavy work again: the idle part starts later
        g.idleStartUs = 0;
        return;
    }
    if (!g.heavySeen || g.idleLogged) return;
    if (frameUs < 30000) ++g.quiet; else g.quiet = 0;
    if (g.quiet < 5) return;
    g.idleLogged = true;
    g.idleStartUs = now;
    Log("[doordiag] loading screen idles from %.0f ms after the freeze began", (now - g.loadStartUs) / 1000.0);
    void* const manager = g.manager.load(std::memory_order_relaxed);
    if (!g.earlyStream.load(std::memory_order_relaxed) || g.earlyDone) return;
    g.earlyDone = true;
    if (!manager) {
        Log("[doordiag] DoorDiagEarlyStream: no streaming manager seen yet (needs one fade-in first): skipped");
        return;
    }
    t_early = true;
    StreamAllHook(manager, 0.5f);
    t_early = false;
}

void LogDoorDiagStats()
{
    if (!g.installed) return;
    Log("[doordiag] so far: find-free-element %llu calls (%llu allocating, %llu slow >= 0.5 ms), %llu pool flushes, "
        "%llu stream-all calls",
        static_cast<unsigned long long>(g.findCalls.load()), static_cast<unsigned long long>(g.findAlloc.load()),
        static_cast<unsigned long long>(g.findSlow.load()), static_cast<unsigned long long>(g.flushes.load()),
        static_cast<unsigned long long>(g.streamCalls.load()));
}

}  // namespace opf
