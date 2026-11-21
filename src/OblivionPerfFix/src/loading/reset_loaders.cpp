// ResetLoadersNoFlush (worker epicport, 2026-11-12): ResetLoaders(UObject*) of a package that has no loader linker no
// longer makes the game thread wait for every background load in flight. The change is Epic's: Unreal Engine 5.4.0,
// commit 0d790f0ba5 (CL 29406554, "Save and ResetLoaders only flush loading requests specific to the package when
// there's already pending async requests", Linker.cpp). This file is our hook code for the game's UE 5.3 binary; no
// engine source text is copied.
//
// The problem (our measurements, a play recording of 2026-11-07): UObject::Rename without
// REN_ForceNoResetLoaders calls ResetLoaders(GetPackage()) (also StaticDuplicateObject and a few load paths). In UE 5.3
// ResetLoaders(UObject*) is: IsAsyncLoading() (for a log line), FlushAsyncLoading() = wait until EVERY async package
// load in flight is finished, then FLinkerManager::ResetLoaders(InPkg), which detaches the package's FLinkerLoad if it
// has one. In this game drawing a bow (exe+0x4868020) creates and renames the "Drawn Arrow Mesh Component": the rename's
// flush waited 18-20 ms for an unrelated load in flight (a Niagara system's post-load) in one of our play recordings.
//
// Epic's reasoning (5.4): the flush only exists so that the linker being reset is not in use by the loader; a package
// without an FLinkerLoad (every package of an IoStore / Zen-loaded cooked game: the async loader creates none) has
// nothing to reset, so 5.4 neither flushes nor resets for it (and with a linker flushes only that linker's requests).
// What this hook does (game thread or any thread ResetLoaders runs on): for InPkg != null it asks the game's own
// functions the same question FLinkerManager::ResetLoaders asks first: the package (InPkg->GetPackage(), the call it
// makes, exe+0x11169A0) and that package's linker (FLinkerLoad::FindExistingLinkerForPackage, exe+0x107AD80: reads
// UPackage::LinkerLoad). No package or a linker = the game's ResetLoaders, unchanged (more conservative than 5.4, which
// would flush only that linker). No linker = return: FLinkerManager::ResetLoaders would have done nothing for this
// package (UE 5.3 LinkerManager.cpp: it only detaches a found linker), and the flush is skipped as in 5.4. A null InPkg
// (the "reset all loaders" call) is unchanged. Identical result for the renamed object; the loads in flight finish when
// the loader finishes them, as without the rename.
//
// Key ResetLoadersNoFlush: 0 = the game's way; 1 = skip as above. Installed at start-up when != 0 (lab: always, to count
// the calls with 0), then live. Lab: each call is counted by caller, with "loads in flight" and "linker found" (the
// precondition, checked on this binary at run time: a linker found is logged).
#include "common.h"

#include <atomic>
#include <intrin.h>

namespace opf {
namespace {

using ResetFn = void (*)(void* obj);
using ObjFn = void* (*)(void* obj);
using BoolFn = bool (*)();

// ResetLoaders(UObject*) (exe+0x1080F90): push rbx; sub rsp,20h; mov rbx,rcx; call [IsAsyncLoading];
// mov ecx,-1; call FlushAsyncLoading; call FLinkerManager::Get; mov rdx,rbx; mov rcx,rax; add rsp,20h; pop rbx;
// jmp FLinkerManager::ResetLoaders
constexpr char kReset[] =
    "40 53 48 83 EC 20 48 8B D9 FF 15 ?? ?? ?? ?? B9 FF FF FF FF E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D3 48 8B C8 48 83 C4 20 5B E9";
constexpr size_t kPrologue = 9, kIsAsync = 9, kFlushCall = 20, kMgrJmp = 41;
// FLinkerManager::ResetLoaders(UObject*) (exe+0x10939E0), near its start: test rdx,rdx; je ALL; mov rcx,rdx;
// call GetPackage; mov r15,rax; test rax,rax; je ALL; mov rcx,rax; xor edi,edi; call FindExistingLinkerForPackage;
// mov r14,rax; test rax,rax; je END
constexpr char kMgrHead[] =
    "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B CA E8 ?? ?? ?? ?? 4C 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B C8 33 FF E8 ?? ?? ?? ?? "
    "4C 8B F0 48 85 C0 0F 84";
constexpr size_t kMgrSearch = 0x40, kAllJe1 = 3, kPkgCall = 12, kAllJe2 = 23, kLinkerCall = 34;
// FLinkerLoad::FindExistingLinkerForPackage: test rcx,rcx; je; mov rax,[rcx+LinkerLoad]; ret; xor eax,eax; ret
constexpr char kLinkerFn[] = "48 85 C9 74 05 48 8B 41 ?? C3 33 C0 C3";

ResetFn g_game = nullptr;  // trampoline
ObjFn g_getPackage = nullptr, g_findLinker = nullptr;
BoolFn* g_isAsyncLoading = nullptr;  // the engine's IsAsyncLoading function pointer (CoreGlobals)
uint8_t* g_tramp = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> s_calls{0}, s_inFlight{0}, s_skipped{0}, s_skippedInFlight{0}, s_linker{0}, s_nullPkg{0},
    s_flushUs{0}, s_maxFlushUs{0};
std::atomic<uint64_t> s_linkerEver{0};


double QpcMs()
{
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / static_cast<double>(q.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * f;
}

void MaxInto(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

}  // namespace

// The hook (public for the self-test). Decides with the game's own functions; the game's ResetLoaders otherwise.
void ResetLoadersNoFlushHook(void* obj)
{
    s_calls.fetch_add(1, std::memory_order_relaxed);
    const bool inFlight = g_isAsyncLoading && *g_isAsyncLoading && (*g_isAsyncLoading)();  // the game's ResetLoaders asks too
    if (inFlight) s_inFlight.fetch_add(1, std::memory_order_relaxed);
    bool noLinker = false;
    if (obj) {
        void* const pkg = g_getPackage(obj);
        if (pkg) {
            if (g_findLinker(pkg)) {
                s_linker.fetch_add(1, std::memory_order_relaxed);
                if (s_linkerEver.fetch_add(1, std::memory_order_relaxed) < 5)
                    Log("[resetloaders] package %p has a loader linker: the game's ResetLoaders runs (flush)", pkg);
            } else {
                noLinker = true;
            }
        } else {
            s_nullPkg.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        s_nullPkg.fetch_add(1, std::memory_order_relaxed);
    }
    if (noLinker && g_on.load(std::memory_order_relaxed)) {
        s_skipped.fetch_add(1, std::memory_order_relaxed);
        if (inFlight) s_skippedInFlight.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const double t0 = inFlight ? QpcMs() : 0;
    g_game(obj);
    if (inFlight) {
        const uint64_t us = static_cast<uint64_t>((QpcMs() - t0) * 1000);
        s_flushUs.fetch_add(us, std::memory_order_relaxed);
        MaxInto(s_maxFlushUs, us);
    }
}

namespace {

uint8_t* CallOrJmpTarget(uint8_t* at) { return (at[0] == 0xE8 || at[0] == 0xE9) ? RipTarget(at, 1, 5) : nullptr; }

struct Found {
    uint8_t *reset = nullptr, *flush = nullptr, *mgrReset = nullptr, *getPackage = nullptr, *findLinker = nullptr;
    BoolFn* isAsync = nullptr;
    uint32_t linkerOff = 0;
};

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto hits = FindAll(game.text, ParsePattern(kReset));
    if (hits.size() != 1) { *why = "ResetLoaders not found once"; return false; }
    uint8_t* const reset = hits[0];
    BoolFn* const isAsync = reinterpret_cast<BoolFn*>(RipTarget(reset + kIsAsync, 2, 6));
    if (!game.data.Contains(isAsync)) { *why = "IsAsyncLoading's pointer is not in .data"; return false; }
    uint8_t* const flush = CallOrJmpTarget(reset + kFlushCall);
    uint8_t* const mgr = CallOrJmpTarget(reset + kMgrJmp);
    if (!flush || !game.text.Contains(flush) || !mgr || !game.text.Contains(mgr)) {
        *why = "FlushAsyncLoading / FLinkerManager::ResetLoaders not found";
        return false;
    }
    const Pattern head = ParsePattern(kMgrHead);
    uint8_t* h = nullptr;
    for (size_t i = 0; i < kMgrSearch && !h; ++i)
        if (MatchesAt(mgr + i, head)) h = mgr + i;
    if (!h) { *why = "FLinkerManager::ResetLoaders' package / linker test not found"; return false; }
    // both "no object" and "no package" go to the same place (UE 5.3: reset all loaders); "no linker" elsewhere
    if (RipTarget(h + kAllJe1, 2, 6) != RipTarget(h + kAllJe2, 2, 6)) { *why = "FLinkerManager::ResetLoaders' branches do not match"; return false; }
    uint8_t* const getPackage = CallOrJmpTarget(h + kPkgCall);
    uint8_t* const findLinker = CallOrJmpTarget(h + kLinkerCall);
    if (!getPackage || !game.text.Contains(getPackage) || !findLinker || !game.text.Contains(findLinker) ||
        !MatchesAt(findLinker, ParsePattern(kLinkerFn))) {
        *why = "GetPackage / FindExistingLinkerForPackage not as expected";
        return false;
    }
    f->reset = reset;
    f->flush = flush;
    f->mgrReset = mgr;
    f->getPackage = getPackage;
    f->findLinker = findLinker;
    f->isAsync = isAsync;
    f->linkerOff = findLinker[8];
    return true;
}

}  // namespace

bool InstallResetLoadersNoFlush(bool beforeGameCode, bool key)
{
    if (g_installed) return true;
    if (!key) {
        Log("[resetloaders] ResetLoadersNoFlush off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[resetloaders] ResetLoadersNoFlush: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) {
        Log("[resetloaders] ResetLoadersNoFlush not installed: %s", why);
        return false;
    }
    // region: [0,16) jmp to the hook; [16,48) trampoline = the 9-byte prologue + jmp back to reset+9
    uint8_t* const region = AllocNearGame(64);
    if (!region) { Log("[resetloaders] ResetLoadersNoFlush not installed: no memory near the game's code"); return false; }
    const int64_t rel = region - (f.reset + 5);
    if (rel != static_cast<int32_t>(rel)) {
        FreeOwnCode(region);
        Log("[resetloaders] ResetLoadersNoFlush not installed: out of reach");
        return false;
    }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&ResetLoadersNoFlushHook);
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    uint8_t* const tramp = region + 16;
    memcpy(tramp, f.reset, kPrologue);
    memcpy(tramp + kPrologue, jmpAbs, sizeof jmpAbs);
    uint8_t* const back = f.reset + kPrologue;
    memcpy(tramp + kPrologue + sizeof jmpAbs, &back, sizeof back);
    DWORD old;
    if (!VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old)) {
        FreeOwnCode(region);
        Log("[resetloaders] ResetLoadersNoFlush not installed: could not make the trampoline executable");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    g_game = reinterpret_cast<ResetFn>(tramp);
    g_tramp = tramp;
    g_getPackage = reinterpret_cast<ObjFn>(f.getPackage);
    g_findLinker = reinterpret_cast<ObjFn>(f.findLinker);
    g_isAsyncLoading = f.isAsync;
    g_on = key;
    uint8_t bytes[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(bytes + 1, &rel32, 4);
    if (!WriteCode(f.reset, bytes)) {
        g_game = nullptr;
        g_tramp = nullptr;
        FreeOwnCode(region);
        Log("[resetloaders] ResetLoadersNoFlush not installed: could not patch the function");
        return false;
    }
    g_installed = true;
    Log("[resetloaders] ResetLoaders exe+0x%X (FlushAsyncLoading exe+0x%X, FLinkerManager::ResetLoaders exe+0x%X, GetPackage "
        "exe+0x%X, FindExistingLinkerForPackage exe+0x%X = UPackage +0x%X): installed, %s",
        static_cast<unsigned>(f.reset - game.base), static_cast<unsigned>(f.flush - game.base),
        static_cast<unsigned>(f.mgrReset - game.base), static_cast<unsigned>(f.getPackage - game.base),
        static_cast<unsigned>(f.findLinker - game.base), f.linkerOff,
        key ? "a package without a loader linker is not flushed" : "off (counting only)");
    return true;
}

void SetResetLoadersNoFlush(bool on)
{
    if (!g_installed || g_on.load() == on) return;
    g_on = on;
    Log("[resetloaders] %s", on ? "ResetLoaders of a package without a loader linker no longer waits for loads in flight"
                                : "ResetLoaders waits for every load in flight, as before");
}

void LogResetLoadersNoFlushStats()
{
    if (!g_installed) return;
    const uint64_t calls = s_calls.exchange(0);
    if (!calls) return;
    const uint64_t flight = s_inFlight.exchange(0), skipped = s_skipped.exchange(0), sf = s_skippedInFlight.exchange(0),
                   linker = s_linker.exchange(0), nullPkg = s_nullPkg.exchange(0), fus = s_flushUs.exchange(0),
                   mfus = s_maxFlushUs.exchange(0);
    Log("[resetloaders] since last report: %llu ResetLoaders calls (%llu with loads in flight), %llu not flushed (no loader "
        "linker; %llu of them with loads in flight), %llu with a linker, %llu without a package; the game's flush with loads "
        "in flight took %.1f ms (longest %.1f ms)",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(flight), static_cast<unsigned long long>(skipped),
        static_cast<unsigned long long>(sf), static_cast<unsigned long long>(linker), static_cast<unsigned long long>(nullPkg),
        fus / 1000.0, mfus / 1000.0);
}

// self-test: discovery on the mapped exe (nothing patched) and the hook's decision with fakes
bool ResetLoadersNoFlushTestDiscover(const GameModule& game, uint32_t rva[5], uint32_t* linkerOff)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return false;
    rva[0] = static_cast<uint32_t>(f.reset - game.base);
    rva[1] = static_cast<uint32_t>(f.flush - game.base);
    rva[2] = static_cast<uint32_t>(f.mgrReset - game.base);
    rva[3] = static_cast<uint32_t>(f.getPackage - game.base);
    rva[4] = static_cast<uint32_t>(f.findLinker - game.base);
    *linkerOff = f.linkerOff;
    return true;
}

void ResetLoadersNoFlushTestSetup(void* game, void* getPackage, void* findLinker, void* isAsyncPtr, bool on)
{
    g_game = reinterpret_cast<ResetFn>(game);
    g_getPackage = reinterpret_cast<ObjFn>(getPackage);
    g_findLinker = reinterpret_cast<ObjFn>(findLinker);
    g_isAsyncLoading = reinterpret_cast<BoolFn*>(isAsyncPtr);
    g_on = on;
}

void ResetLoadersNoFlushTestCounts(uint64_t* calls, uint64_t* skipped, uint64_t* linker)
{
    *calls = s_calls.exchange(0);
    *skipped = s_skipped.exchange(0);
    *linker = s_linker.exchange(0);
    s_inFlight = 0; s_skippedInFlight = 0; s_nullPkg = 0; s_flushUs = 0; s_maxFlushUs = 0;
}

}  // namespace opf
