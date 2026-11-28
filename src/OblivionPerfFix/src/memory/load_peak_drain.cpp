// LoadPeakDrain (worker s001port, 2026-11-26; S-001; public tree, default 0 until a release; needs LoadPeakKeepsBack's loading
// state): behind a loading screen near the video memory budget, the engine's own short-lived memory of the load goes back sooner,
// so less of it is held in the load's peak second.
//
// Why (bench s001fix r5-r8, VramWatch fast lines): at a fast travel's peak second the engine itself holds 0.8-0.9 GB of
// fast-allocator upload pages (4 MB each, 200-600 made per load for texture streaming's initial data) and 0.6-0.95 GB of emptied
// ray-tracing pools, both larger than our keeps. The engine gives the pages back one a frame once unused for 10 frames (UE 5.3
// D3D12Allocation.cpp FD3D12FastAllocatorPagePool::CleanupPages, "Only release at most one page per frame"; called from
// D3D12CommandContext.cpp RHIEndFrame; unchanged through 5.7) and an empty pool 20 frames after it went empty
// (FD3D12PoolAllocator::CleanUpAllocations); at the loading screen's frame rate that is seconds, the second over the budget that
// makes Windows demote. With the key on, while the loading-screen state (LoadingScreenNow) is 1 and the game is within
// LoadPeakKeepsBackMarginMB of its budget (LoadPeakDrainNow):
//  - pages: the inlined CleanupPages runs up to LoadPeakDrainPages (hidden, 4) times in the same RHIEndFrame instead of once (each
//    run is the game's own: the first page unused for 10 frames, never below d3d12.FastAllocator.MinPagesToRetain). During
//    LoadPeakKeepsBack's episode that floor stays UploadMemoryKeep's UploadPagesToKeep (24 pages = 96 MB) instead of the game's
//    5: the page pool is never left emptier than in play with UploadMemoryKeep on, so the first streaming burst after the screen
//    (a cell crossing: S-083, a fresh page's Map costs 3-4 ms under the allocator's lock) finds the pages it found before.
//    Each page release is ~7.5 ms on the RHI thread (findings rhifree), so 4 a frame adds up to ~30 ms to a loading frame's RHI
//    work: behind the screen only, and these are releases the game would make anyway, partly in the first 5 s of play.
//  - pools: GpuPoolKeepMB's 20-frame clean-up hook passes a 2-frame lag for allocators without defragmentation (the ray-tracing
//    acceleration-structure pools; the game's own rule there, no keep), so an emptied pool goes 18 frames sooner.
//  - spares: GpuPoolSpareAhead makes no spare (requests wait) from the first loading frame on (LoadPeakKeepsBack's own
//    give-back acts only at its once-a-second tick).
// Identical results: only when an EMPTY page or pool is given back changes (nothing drawn sits in it); the same pages and pools,
// chosen by the game's own rules. The loading screen is not held: extra loading time can only come from the RHI work above
// (measured: findings s001port).
// How (pages): the inlined page release ends with `mov edx,28h; mov rcx,rbx; call operator delete` (the page object); that call
// becomes a call to a stub that makes the same call, then asks LoadPeakDrainRepeat(); on yes it returns to the cleanup's pool-size
// guard (`mov eax,[MinPagesToRetain]; cmp [rsi+Pool.Num],eax; jle leave`), still inside the fast allocator's lock, with the
// registers the guard and its loop use (rsi = the device, r13 = 0: non-volatile, kept by the calls) unchanged; on no it returns
// where the game's call would have. Found by byte pattern (the tail as UploadPageReleaseAsync's prototype, rhifree), the guard
// 0x40-0x100 bytes before it; any mismatch: a log line, nothing written. Start-up install (LoadPeakDrain != 0 or
// UploadPageScreenRelease != 0), then live.
//
// UploadPageScreenRelease (worker s083build, 2026-11-26; S-001, S-083; public tree, default 0 until a release): the same stub, a
// second rule, at EVERY loading screen (not only near the budget). Why (s001port chain p2, 17 loads, kmt): a load makes 182-254
// fast-allocator pages in its last 2-4 s (the new area's textures), they become idle in its last ~0.5-1 s, and the game releases
// one a frame, so 71-229 idle pages are still held when the screen ends and go one a frame in the first 1-4 s of play (up to
// 9.6 s at a low frame rate): 218-986 ms of RHI-thread time per load, each release 4-7.5 ms, right when the player starts to
// move. 94-97 % of a release is the driver's DestroyAllocation2 call itself (rhifree's timed releases against KmtProbe, s083build
// relsplit.py), so only where it runs can change; moving it to another thread was measured worse for the first seconds of play
// (rhifreetest). So with this key on, while the loading state is 1 and no fast-allocator page was made on demand for 100 ms (the
// load's burst is over: our releases do not hold the allocator's lock against its creations), the clean-up repeats while this
// frame's extra releases stay within UploadPageScreenReleaseMs (hidden, 24) of RHI time (the next one's cost is estimated from
// the last measured). Same pages, same rule (idle for 10 frames, never below the floor), behind the screen instead of after it.
// Needs UploadMemoryKeep (its per-frame clean-up counter; and UploadPagePreGrow's probe, installed with either key, for the "no
// page made" time): without UploadMemoryKeep neither key installs the probe and the repeat stops after the first frame.
#include "common.h"

#include <atomic>
#include <cstring>
#include <windows.h>

namespace opf {
namespace {
// dec Pool.Num / call (shrink) / mov rcx,[rbx+8] / test / je / mov eax,-1 / lock xadd [rcx+8],eax / cmp eax,1 / jne / mov rax,[rcx] /
// mov edx,1 / call [rax] / mov edx,28h / mov rcx,rbx / >> call operator delete <<
constexpr char kTail[] =
    "FF 4F 08 48 8B CF E8 ?? ?? ?? ?? 48 8B 4B 08 48 85 C9 74 19 B8 FF FF FF FF F0 0F C1 41 08 83 F8 01 75 0A 48 8B 01 "
    "BA 01 00 00 00 FF 10 BA 28 00 00 00 48 8B CB E8";
constexpr size_t kTailCall = 53;  // the `E8` of call operator delete
// the guard: mov eax,[MinPagesToRetain] / cmp [rsi+Pool.Num],eax / jle (rel32) / mov rax,[rsi+x] / mov dl,1
constexpr char kGuard[] = "8B 05 ?? ?? ?? ?? 39 86 ?? ?? ?? ?? 0F 8E ?? ?? ?? ?? 48 8B 86 ?? ?? ?? ?? B2 01";
constexpr size_t kGuardMin = 0x40, kGuardMax = 0x100;

struct Sites { uint8_t* call = nullptr; uint8_t* guard = nullptr; uint8_t* del = nullptr; uint8_t* leave = nullptr; };
Sites g;
bool g_installed = false;
std::atomic<int> g_pages{4};
std::atomic<uint32_t> g_frame{0xFFFFFFFFu}, g_inFrame{0};
std::atomic<uint64_t> s_extra{0}, s_maxInFrame{0}, s_framesDrained{0};
// UploadPageScreenRelease (RHI thread state below, written only inside the fast allocator's lock)
std::atomic<bool> g_screenOn{false};
std::atomic<uint32_t> g_screenCapUs{24000};
uint64_t g_firstQpc = 0, g_prevQpc = 0, g_estUs = 8000, g_qpf = 0;
bool g_prevScreen = false;  // the repeat that just ran was the screen rule's (else LoadPeakDrain's)
std::atomic<uint64_t> s_screenExtra{0}, s_screenFrames{0}, s_screenMaxUs{0}, s_screenUs{0}, s_drainUs{0};
uint8_t* g_stub = nullptr;

bool Fail(const char* why)
{
    Log("[loaddrain] %s: LoadPeakDrain's page part unavailable", why);
    return false;
}
}  // namespace

// The decision (self-test): another page release in this frame? `already` = repeats made in this frame so far.
bool LoadPeakDrainRepeatDecide(bool drain, uint32_t already, int pagesPerFrame)
{
    if (!drain) return false;
    const uint32_t cap = pagesPerFrame < 1 ? 1u : pagesPerFrame > 16 ? 16u : static_cast<uint32_t>(pagesPerFrame);
    return already + 1 < cap;  // the game's own release is the first of the frame
}

// UploadPageScreenRelease's decision (self-test): another release now? spentUs = this frame's extra releases so far, nextUs = the
// next one's estimated cost.
bool UploadPageScreenReleaseDecide(bool on, int loading, uint64_t sinceCreateMs, uint64_t spentUs, uint64_t nextUs, uint64_t capUs)
{
    return on && loading == 1 && sinceCreateMs >= 100 && spentUs + nextUs <= capUs;
}

// RHI thread, from the stub after each page release (inside the fast allocator's lock).
extern "C" bool LoadPeakDrainRepeat()
{
    if (!g_qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpf = static_cast<uint64_t>(f.QuadPart);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t now = static_cast<uint64_t>(c.QuadPart);
    const uint32_t frame = UploadCleanupFrames();
    const bool newFrame = g_frame.exchange(frame, std::memory_order_relaxed) != frame;
    if (newFrame) {
        g_inFrame.store(0, std::memory_order_relaxed);
        g_firstQpc = now;  // the game's own release of this frame is done: the extra time counts from here
    } else {
        g_estUs = (now - g_prevQpc) * 1000000 / g_qpf;  // the last repeat (scan + release): the next one's estimate
    }
    g_prevQpc = now;
    const uint32_t already = g_inFrame.load(std::memory_order_relaxed);
    bool more = LoadPeakDrainRepeatDecide(LoadPeakDrainNow(), already, g_pages.load(std::memory_order_relaxed));
    bool screen = false;
    if (!more && g_screenOn.load(std::memory_order_relaxed)) {
        const uint64_t created = UploadPageLastCreateQpc();
        const uint64_t sinceCreateMs = !created ? ~0ull : now > created ? (now - created) * 1000 / g_qpf : 0;
        const uint64_t spentUs = (now - g_firstQpc) * 1000000 / g_qpf;
        screen = more = UploadPageScreenReleaseDecide(true, LoadingScreenNow(), sinceCreateMs, spentUs, g_estUs,
                                                      g_screenCapUs.load(std::memory_order_relaxed));
        if (!newFrame && spentUs > s_screenMaxUs.load(std::memory_order_relaxed)) s_screenMaxUs.store(spentUs, std::memory_order_relaxed);
    }
    if (!newFrame && g_inFrame.load(std::memory_order_relaxed))  // the repeat that just ran: its rule's RHI time (review S3-5)
        (g_prevScreen ? s_screenUs : s_drainUs).fetch_add(g_estUs, std::memory_order_relaxed);
    g_prevScreen = more && screen;
    if (more) {
        if (already == 0) (screen ? s_screenFrames : s_framesDrained).fetch_add(1, std::memory_order_relaxed);
        g_inFrame.store(already + 1, std::memory_order_relaxed);
        (screen ? s_screenExtra : s_extra).fetch_add(1, std::memory_order_relaxed);
        if (already + 1 > s_maxInFrame.load(std::memory_order_relaxed)) s_maxInFrame.store(already + 1, std::memory_order_relaxed);
    }
    return more;
}

void SetUploadPageScreenRelease(bool on, int capMs)
{
    const int ms = capMs < 4 ? 4 : capMs > 100 ? 100 : capMs;
    g_screenCapUs.store(static_cast<uint32_t>(ms) * 1000, std::memory_order_relaxed);
    const bool was = g_screenOn.exchange(on);
    if (was == on || !g_installed) return;
    if (on) Log("[loaddrain] UploadPageScreenRelease on: behind loading screens idle upload pages go back within %d ms of RHI time a frame", ms);
    else Log("[loaddrain] UploadPageScreenRelease off: the game's one page a frame (LoadPeakDrain's rule aside)");
}

bool FindLoadPeakDrain(const GameModule& game, uint32_t* callRva, uint32_t* guardRva)
{
    const auto hits = FindAll(game.text, ParsePattern(kTail));
    if (hits.size() != 1) {
        Log("[loaddrain] the fast allocator's page release found %zu times (expected 1)", hits.size());
        return false;
    }
    Sites s;
    s.call = hits[0] + kTailCall;
    const Pattern guard = ParsePattern(kGuard);
    for (size_t back = kGuardMin; back <= kGuardMax && !s.guard; ++back)
        if (MatchesAt(hits[0] - back, guard)) s.guard = hits[0] - back;
    if (!s.guard) return Fail("the cleanup's pool-size guard not found before it");
    if (!game.data.Contains(RipTarget(s.guard, 2, 6))) return Fail("the guard does not read a game variable");
    s.del = RipTarget(s.call, 1, 5);
    s.leave = s.call + 5;
    // the guard's jle must leave to the instruction after the call (the lock's release)
    if (RipTarget(s.guard + 12, 2, 6) != s.leave) return Fail("the guard does not leave where the page release ends");
    if (!game.text.Contains(s.del)) return Fail("operator delete not in the game's code");
    g = s;
    if (callRva) *callRva = static_cast<uint32_t>(s.call - game.base);
    if (guardRva) *guardRva = static_cast<uint32_t>(s.guard - game.base);
    return true;
}

bool InstallLoadPeakDrain(bool beforeGameCode)
{
    if (g_installed) return true;
    const GameModule& game = Game();
    if (!g.call && !FindLoadPeakDrain(game, nullptr, nullptr)) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    uint8_t* const stub = AllocNearGame(96);
    if (!stub) return Fail("no free memory near the game's code");
    // stub: sub rsp,28h; call [rip+del]; call [rip+repeat]; add rsp,28h; test al,al; jz done; mov rax,[rip+guard]; mov [rsp],rax;
    // done: ret; then the three 8-byte slots
    uint8_t s[] = {
        0x48, 0x83, 0xEC, 0x28,              // 0: sub rsp,28h
        0xFF, 0x15, 0, 0, 0, 0,              // 4: call [rip+slotDel]
        0xFF, 0x15, 0, 0, 0, 0,              // 10: call [rip+slotRepeat]
        0x48, 0x83, 0xC4, 0x28,              // 16: add rsp,28h
        0x84, 0xC0,                          // 20: test al,al
        0x74, 0x0B,                          // 22: jz done (35)
        0x48, 0x8B, 0x05, 0, 0, 0, 0,        // 24: mov rax,[rip+slotGuard]
        0x48, 0x89, 0x04, 0x24,              // 31: mov [rsp],rax
        0xC3,                                // 35: done: ret
    };
    constexpr size_t kSlots = 40;
    auto put = [&](size_t at, size_t end, size_t slot) {
        const int32_t d = static_cast<int32_t>(static_cast<int64_t>(kSlots + slot) - static_cast<int64_t>(end));
        memcpy(s + at, &d, 4);
    };
    put(6, 10, 0);
    put(12, 16, 8);
    put(27, 31, 16);
    memset(stub, 0xCC, 96);
    memcpy(stub, s, sizeof s);
    void* const slots[3] = {g.del, reinterpret_cast<void*>(&LoadPeakDrainRepeat), g.guard};
    memcpy(stub + kSlots, slots, sizeof slots);
    DWORD old;
    if (!VirtualProtect(stub, 96, PAGE_EXECUTE_READ, &old)) return Fail("could not make the stub executable");
    FlushInstructionCache(GetCurrentProcess(), stub, 96);
    const int64_t rel = stub - (g.call + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the stub is out of reach");
    uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 1, &rel32, 4);
    if (!WriteCode(g.call, patch)) return Fail("could not patch the call");
    g_stub = stub;
    g_installed = true;
    const GameModule& gm = Game();
    Log("[loaddrain] installed: the fast allocator's page release exe+0x%X repeats to its guard exe+0x%X behind loading screens near "
        "the budget",
        static_cast<unsigned>(g.call - gm.base), static_cast<unsigned>(g.guard - gm.base));
    return true;
}

bool LoadPeakDrainTestPatched()
{
    if (!g_installed) return false;
    return g.call[0] == 0xE8 && RipTarget(g.call, 1, 5) == g_stub && *reinterpret_cast<void**>(g_stub + 40) == g.del &&
           *reinterpret_cast<void**>(g_stub + 56) == g.guard;
}

void SetLoadPeakDrainPages(int pages) { g_pages.store(pages, std::memory_order_relaxed); }

void LogLoadPeakDrainStats()
{
    const uint64_t extra = s_extra.exchange(0), frames = s_framesDrained.exchange(0), mx = s_maxInFrame.exchange(0);
    const uint64_t sExtra = s_screenExtra.exchange(0), sFrames = s_screenFrames.exchange(0), sMax = s_screenMaxUs.exchange(0),
                   sUs = s_screenUs.exchange(0), dUs = s_drainUs.exchange(0);
    if (extra)
        Log("[loaddrain] since last report: %llu extra fast-allocator page releases behind loading screens in %llu frames (at most %llu "
            "extra in one frame; %.0f ms of RHI time)",
            static_cast<unsigned long long>(extra), static_cast<unsigned long long>(frames), static_cast<unsigned long long>(mx), dUs / 1000.0);
    if (sExtra)
        Log("[loaddrain] since last report: UploadPageScreenRelease released %llu idle upload pages behind loading screens in %llu frames "
            "(about %.0f ms of extra RHI time, at most %.1f ms in one frame)",
            static_cast<unsigned long long>(sExtra), static_cast<unsigned long long>(sFrames), sUs / 1000.0, sMax / 1000.0);
}

}  // namespace opf
