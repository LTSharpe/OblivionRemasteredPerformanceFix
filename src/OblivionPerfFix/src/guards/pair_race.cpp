// PairReleaseOnce (game bug fix; worker pairrace, 2026-11-06; findings `crash4590f5c`; public since 1.3.0, workers port130a/port130c
// 2026-11-08) and lab PairRaceProbe. [Fixes] PairReleaseOnce, default 1 (any != 0 = on; "game bug fixes" section), start-up only.
// Lab PairRaceProbe (default off) reads its own [Fixes] key at start-up. Whose bug: the double release is in the game's own code; in our
// tests only our NpcCreationPacing made it collide (findings crash4590f5c: 10 of 44 widened flow teardowns with pacing on, 0 of 44
// with it off); not seen without the mod.
//
// The bug (the game's own code). Altar's pairing session keeps its command buffers as ids of a 128 KB buffer pool. A session's
// teardown releases every id twice: once early, without the manager lock, and again in the session's destructor, under it.
//   - exe+0x4591010 RunSession, at +0xE0: call exe+0x4596FB0 (releases the ring's ids, leaves them in the ring)
//   - exe+0x4596CE0 (manager vtable +8, drain and destroy): the same loop inlined at +0x33
//   - then exe+0x4596C30 (table erase, takes the manager lock) -> exe+0x458EF50 (destructor): releases the same ids again.
// When nobody has taken the id in between the second release is free(null) + clearing a clear bit. If the old engine thread's
// Acquire took it in between, the second release frees that session's new buffer and clears its bit; the next Acquire hands out
// the same id, and two sessions share one buffer.
//
// PairRaceProbe=1 (lab only): pass-through vtable patches on the pool's Release (+8) and Acquire (+0x10). Each early release opens a window on
// its id, booked to a path by the return address and a stack walk:
//   run      RunSession's early release (exe+0x45910F0)
//   pretick  exe+0x4596CE0 reached from the pre-tick handler's loop (exe+0x4899D85 -> exe+0x489C880)
//   flow     exe+0x4596CE0 reached from the flow runner (exe+0x4881F73 -> exe+0x489C880)
//   other    exe+0x4596CE0 from anywhere else
// The destructor's release closes it. Counted per path: windows, their length, how far pacing's queue was behind, Acquires while
// any window was open, Acquires that took the window's own id (a collision), and stray second releases. A stray release is decided
// inside the pool's own lock from the pool's slot (non-null although this id's early release nulled it) and is SKIPPED, so a run
// with collisions neither shares a buffer nor crashes: the count is what stock would have corrupted.
// PairRaceWidenUs=N (re-read every second): wait N us on the releasing thread after each early release, to widen the window.
//
// PairReleaseOnce=1: the fix. Removes the early releases, so each id is released once, by the destructor, under the manager lock that
// also covers session creation. Between the two releases the game only runs the session's vtable +0x20 (exe+0x4597310, counters and
// the run flag: checked here), so nothing can see the buffer live a few microseconds longer. Every piece is found by byte pattern and
// matched first; any mismatch patches nothing, with a log line. How, with a fire counter that changes nothing else:
//   - RunSession's call of the early release (+0xE0) is redirected to a 16-byte stub: cmp qword [rcx+60h],0; je; lock inc [count];
//     ret (the early release's own first test: a session with a buffer ring counts). The call's other effects were the release.
//   - the drain's "cmp qword [rax+60h],0; je +34h" before the inlined loop (+0x2E, 7 bytes) becomes "call stub; jmp +34h": the same
//     test and count on rax in the stub, then always the jump the je took for a session without a ring (+0x69: session vtable +0x20
//     and the erase; flags are not read there).
// The stats line counts the teardowns whose early release was skipped (both paths), every stats interval when it changed.
// (Lab, before 1.3.0: a 5-byte NOP over the call and the je made a jmp; same effect, no count.)
//
// The log tag is [pairrace]. Addresses are build 19115871's.
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace opf {
namespace {

// exe+0x4596FB0: the session's early release: for each ring entry with id >= 0: [rcx+60h]->[+8](id). The Release returns to +0x40.
constexpr char kFirstRelease[] =
    "40 56 48 83 EC 20 48 83 79 60 00 48 8B F1 74 43 48 89 5C 24 30 48 8B 59 68 48 89 7C 24 38 48 63 79 70 48 C1 E7 04 48 03 FB 48 3B DF 74 1B 66 90 "
    "8B 13 85 D2 78 0A 48 8B 4E 60 48 8B 01 FF 50 08";
constexpr size_t kFirstReleaseRet = 0x40;
// exe+0x458EF50: the session's destructor (vtable slot 0): the same loop. The Release returns to +0x50.
constexpr char kDtor[] =
    "40 57 48 83 EC 20 48 83 79 60 00 48 8D 05 ?? ?? ?? ?? 48 89 6C 24 38 48 8B F9 48 89 01 8B EA 74 42 48 89 5C 24 30 48 8B 59 68 48 89 74 24 40 "
    "48 63 71 70 48 C1 E6 04 48 03 F3 48 3B DE 74 1A 90 8B 13 85 D2 78 0A 48 8B 4F 60 48 8B 01 FF 50 08";
constexpr size_t kDtorRet = 0x50;
// exe+0x4596CE0 (manager vtable +8): find the session; the inlined early release (je at +0x33 over it, the Release returns to
// +0x60); session vtable +0x20; erase (+0x89)
constexpr char kDrain[] =
    "40 53 55 56 57 41 55 41 56 48 83 EC 78 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 68 48 8B E9 44 8B EA 48 83 E9 80 E8 ?? ?? ?? ?? 48 8B F0 "
    "48 83 78 60 00 74 34 48 8B 58 68 48 63 78 70 48 C1 E7 04 48 03 FB 48 3B DF 74 20 0F 1F 80 00 00 00 00 8B 13 85 D2 78 0A 48 8B 4E 60 48 8B 01 "
    "FF 50 08 48 83 C3 10 48 3B DF 75 E7 48 8B 06 48 8B CE 4C 89 A4 24 C0 00 00 00 4C 89 7C 24 70 FF 50 20 41 8B D5 48 8D 8D 80 00 00 00 E8";
constexpr size_t kDrainCmp = 0x2E, kDrainJe = 0x33, kDrainRet = 0x60, kDrainErase = 0x89;
constexpr uint8_t kDrainJeRel = 0x34;
// exe+0x4596C30: table erase under the manager lock (+0x70): the session's destructor through vtable slot 0 with delete = 1
constexpr char kErase[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 59 70 8B F2 48 8B F9 48 8B CB FF 15 ?? ?? ?? ?? 8B 47 08 3B 47 34 74 3E 8B 4F 48 4C 8D 4F 38 "
    "4D 8B 41 08 FF C9 48 23 CE 4D 85 C0 4D 0F 45 C8 41 8B 04 89 83 F8 FF 74 1E 48 8B 17 0F 1F 40 00 48 98 48 8D 0C 40 39 34 CA 48 8D 04 CA 74 0A "
    "8B 40 10 83 F8 FF 75 E9 33 C0 48 8B 48 08 48 85 C9 74 0A 48 8B 01 BA 01 00 00 00 FF 10";
// exe+0x4591010 RunSession (manager vtable +0x70): the head as npc_pacing.cpp matches it...
constexpr char kRunSession[] =
    "48 89 5C 24 18 55 56 41 54 41 56 41 57 48 83 EC 20 48 8D A9 80 00 00 00 44 8B F2 48 8B CD E8 ?? ?? ?? ?? "
    "41 8B D6 48 8B CD 48 8B F0 E8 ?? ?? ?? ?? 45 33 FF 48 8B D8 44 8B 60 50 45 85 E4 75 ?? 48 8D 4C 24 50 4C 8B F8 "
    "FF 15 ?? ?? ?? ?? 0F 57 C0 F2 48 0F 2A 44 24 50 F2 0F 59 05 ?? ?? ?? ?? F2 0F 58 05 ?? ?? ?? ?? F2 0F 11 83 88 00 00 00 "
    "48 89 7C 24 58 66 66 66 0F 1F 84 00 00 00 00 00 66 C7 43 4C 01 00 48 8B 03 48 8B CB FF 50 28 8B 4B 50 85 C9 75 08 "
    "3C 02 74 ?? 3C 03 74 ?? 80 7B 4C 00 75 E2 40 B7 04 85 C9 74 ?? 80 7B 4D 00 75 CF";
// ...and its tail at +0xDD: mov rcx,r15; call early release (+0xE0); session vtable +0x20; call erase (+0xF4)
constexpr char kRunTail[] = "49 8B CF E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 20 41 8B D6 48 8B CD E8";
constexpr size_t kRunTailAt = 0xDD, kRunCall = 0xE0, kRunErase = 0xF4;
// exe+0x4597310, session vtable +0x20: counters and the run flag only
constexpr char kCounters[] = "8B 41 58 29 41 54 C6 41 4C 01 C3";

constexpr uint8_t kJmpShort = 0xEB;
// The fire counters' stubs (16 bytes each): cmp qword [reg+60h],0; je ret; lock inc qword [rip+count]; ret
constexpr uint8_t kStubRcx[16] = {0x48, 0x83, 0x79, 0x60, 0x00, 0x74, 0x08, 0xF0, 0x48, 0xFF, 0x05, 0, 0, 0, 0, 0xC3};
constexpr uint8_t kStubRax[16] = {0x48, 0x83, 0x78, 0x60, 0x00, 0x74, 0x08, 0xF0, 0x48, 0xFF, 0x05, 0, 0, 0, 0, 0xC3};
constexpr size_t kStubCountRel = 11, kStubCountEnd = 15;

PairRaceDiscovery g_d{};
volatile uint64_t* g_count = nullptr;  // [0] RunSession's skipped early releases, [1] the drain's (written by the stubs)
uint64_t g_logged[2] = {};             // stats thread: the counts at the last line
bool g_fixOn = false;

uint32_t RvaOf(const void* p)
{
    const uint8_t* const b = Game().base;
    const uint8_t* const a = static_cast<const uint8_t*>(p);
    return a >= b && a < b + Game().sizeOfImage ? static_cast<uint32_t>(a - b) : 0;
}


// the fix's two writes (both or neither), each to a counting stub; code and counters in memory near the game
bool PatchEarlyReleases(const PairRaceDiscovery& d)
{
    uint8_t* const code = AllocNearGame(64);
    uint8_t* const data = AllocNearGame(64);
    if (!code || !data) return false;
    auto* const count = reinterpret_cast<volatile uint64_t*>(data);
    count[0] = count[1] = 0;
    uint8_t stubs[2][16];
    memcpy(stubs[0], kStubRcx, 16);
    memcpy(stubs[1], kStubRax, 16);
    for (int i = 0; i < 2; ++i) {
        const int64_t rel = reinterpret_cast<const uint8_t*>(const_cast<const uint64_t*>(&count[i])) - (code + 16 * i + kStubCountEnd);
        if (rel != static_cast<int32_t>(rel)) return false;
        const int32_t rel32 = static_cast<int32_t>(rel);
        memcpy(stubs[i] + kStubCountRel, &rel32, 4);
    }
    memcpy(code, stubs, sizeof stubs);
    DWORD old;
    if (!VirtualProtect(code, 64, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    // RunSession: call early release -> call stub 0 (rel32 only)
    const int64_t r0 = code - (d.runCall + 5);
    // drain: cmp qword [rax+60h],0; je +34h -> call stub 1; jmp +34h (the same target)
    const int64_t r1 = (code + 16) - (d.drain + kDrainCmp + 5);
    if (r0 != static_cast<int32_t>(r0) || r1 != static_cast<int32_t>(r1)) return false;
    uint8_t run[5] = {0xE8};
    const int32_t r0_32 = static_cast<int32_t>(r0);
    memcpy(run + 1, &r0_32, 4);
    uint8_t drain[7] = {0xE8, 0, 0, 0, 0, kJmpShort, kDrainJeRel};
    const int32_t r1_32 = static_cast<int32_t>(r1);
    memcpy(drain + 1, &r1_32, 4);
    uint8_t saved[5];
    memcpy(saved, d.runCall, sizeof saved);
    if (!WriteCode(d.runCall, run)) return false;
    if (!WriteCode(d.drain + kDrainCmp, drain)) {
        WriteCode(d.runCall, saved);
        return false;
    }
    g_count = count;
    return true;
}

}  // namespace

// Matches everything, writes nothing. *missing names the first piece that did not match.
bool PairRaceDiscover(const GameModule& game, PairRaceDiscovery* out, const char** missing)
{
    PairRaceDiscovery d{};
    auto one = [&](const char* pattern, const char* what) -> uint8_t* {
        const auto all = FindAll(game.text, ParsePattern(pattern));
        if (all.size() != 1) {
            if (missing && !*missing) *missing = what;
            return nullptr;
        }
        return all[0];
    };
    auto slot = [&](const void* target, const char* what) -> void** {
        void** found = nullptr;
        int hits = 0;
        const uintptr_t want = reinterpret_cast<uintptr_t>(target);
        for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
            if (*reinterpret_cast<const uintptr_t*>(p) == want) { found = reinterpret_cast<void**>(p); ++hits; }
        if (hits != 1 && missing && !*missing) *missing = what;
        return hits == 1 ? found : nullptr;
    };
    auto fail = [&](const char* what) {
        if (missing && !*missing) *missing = what;
        return false;
    };
    d.first = one(kFirstRelease, "early release");
    d.dtor = one(kDtor, "session destructor");
    d.drain = one(kDrain, "drain and destroy");
    d.erase = one(kErase, "table erase");
    d.runSession = one(kRunSession, "RunSession");
    if (!d.first || !d.dtor || !d.drain || !d.erase || !d.runSession) return false;
    void** const dtorSlot = slot(d.dtor, "session destructor slot");
    if (!dtorSlot) return false;
    if (!game.rdata.Contains(dtorSlot + 4) || !MatchesAt(static_cast<uint8_t*>(dtorSlot[4]), ParsePattern(kCounters)))
        return fail("session vtable +0x20 (not counters only)");
    if (!MatchesAt(d.runSession + kRunTailAt, ParsePattern(kRunTail))) return fail("RunSession tail");
    d.runCall = d.runSession + kRunCall;
    if (RipTarget(d.runCall, 1, 5) != d.first) return fail("RunSession's early release call");
    if (RipTarget(d.runSession + kRunErase, 1, 5) != d.erase) return fail("RunSession's erase call");
    if (d.drain[kDrainJe] != 0x74 || d.drain[kDrainJe + 1] != kDrainJeRel) return fail("drain's je");
    if (d.drain[kDrainJe + 2 + kDrainJeRel] != 0x48 || d.drain[kDrainJe + 3 + kDrainJeRel] != 0x8B) return fail("drain's je target");
    if (RipTarget(d.drain + kDrainErase, 1, 5) != d.erase) return fail("drain's erase call");
    d.firstRet = d.first + kFirstReleaseRet;
    d.dtorRet = d.dtor + kDtorRet;
    d.drainRet = d.drain + kDrainRet;
    *out = d;
    return true;
}

// Start-up (version.dll route, before game code): the fix when fixKey != 0; lab: also PairRaceProbe (own key).
bool InstallPairRace(bool beforeGameCode, int fixKey, const wchar_t* iniPath)
{
    (void)iniPath;
    const bool probe = false;
    const bool fix = fixKey != 0;
    if (!fix) Log("[pairrace] PairReleaseOnce off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (!probe && !fix) return false;
    if (!beforeGameCode) {
        Log("[pairrace] PairReleaseOnce: start-up only (version.dll), not installed");
        return false;
    }
    PairRaceDiscovery d{};
    const char* missing = nullptr;
    if (!PairRaceDiscover(Game(), &d, &missing)) {
        Log("[pairrace] PairReleaseOnce: the game's code not recognised (%s): off", missing ? missing : "?");
        return false;
    }
    g_d = d;
    if (fix) {
        if (!PatchEarlyReleases(d)) Log("[pairrace] PairReleaseOnce: could not write: off");
        else {
            g_fixOn = true;
            Log("[pairrace] PairReleaseOnce installed: the early releases at exe+0x%X (RunSession) and exe+0x%X (drain) skipped and "
                "counted; each pairing buffer is released once, by the session's destructor (exe+0x%X)",
                RvaOf(d.runCall), RvaOf(d.drain + kDrainCmp), RvaOf(d.dtor));
        }
    }
    return true;
}

// Stats interval (monitor thread): the skipped early releases, when the count changed.
void LogPairReleaseOnceStats()
{
    if (!g_fixOn || !g_count) return;
    const uint64_t run = g_count[0], drain = g_count[1];
    if (run == g_logged[0] && drain == g_logged[1]) return;
    Log("[pairrace] PairReleaseOnce: %llu early releases skipped since start (RunSession %llu, drain %llu; +%llu since the last line)",
        static_cast<unsigned long long>(run + drain), static_cast<unsigned long long>(run), static_cast<unsigned long long>(drain),
        static_cast<unsigned long long>(run + drain - g_logged[0] - g_logged[1]));
    g_logged[0] = run;
    g_logged[1] = drain;
}

// ---- self-test ----

// The fix's writes on the mapped image: both early releases now go to the counting stubs, the rest untouched, the stubs count a
// session with a ring and not one without, and a second discovery no longer matches. 0 = passed.
int PairReleaseOnceTestInstall()
{
    PairRaceDiscovery d{};
    const char* missing = nullptr;
    if (!PairRaceDiscover(Game(), &d, &missing)) return 1;
    uint8_t* const after = d.runCall + 5;
    const uint8_t eraseCall = d.runSession[kRunErase];
    volatile uint64_t* const savedCount = g_count;
    if (!PatchEarlyReleases(d)) return 2;
    uint8_t* const stubRun = RipTarget(d.runCall, 1, 5);
    uint8_t* const stubDrain = RipTarget(d.drain + kDrainCmp, 1, 5);
    int fail = 0;
    if (d.runCall[0] != 0xE8 || stubRun == d.first || memcmp(stubRun, kStubRcx, kStubCountRel) != 0) fail = 3;
    else if (d.drain[kDrainCmp] != 0xE8 || d.drain[kDrainJe] != kJmpShort || d.drain[kDrainJe + 1] != kDrainJeRel ||
             memcmp(stubDrain, kStubRax, kStubCountRel) != 0)
        fail = 4;
    else if (after[0] != 0x48 || d.runSession[kRunErase] != eraseCall) fail = 5;
    // the jump lands where the je did: session vtable +0x20 and the erase; the destructor's release is untouched
    else if (d.drain[kDrainJe + 2 + kDrainJeRel] != 0x48 || d.drain[kDrainJe + 2 + kDrainJeRel + 1] != 0x8B) fail = 6;
    else if (!MatchesAt(d.dtor, ParsePattern(kDtor))) fail = 7;
    if (!fail) {
        PairRaceDiscovery again{};
        missing = nullptr;
        if (PairRaceDiscover(Game(), &again, &missing)) fail = 8;  // already patched or a changed game: stays off
    }
    if (!fail) {
        // the stubs, run: rcx (RunSession) and rax (drain) hold the session; a ring at +0x60 counts, none does not
        alignas(16) static uint8_t ringSession[0x80] = {}, emptySession[0x80] = {};
        *reinterpret_cast<void**>(ringSession + 0x60) = ringSession;
        using RcxFn = void (*)(void* session);
        reinterpret_cast<RcxFn>(stubRun)(ringSession);
        reinterpret_cast<RcxFn>(stubRun)(emptySession);
        // the rax stub through a thunk: mov rax,rcx; call stub; ret
        uint8_t* const thunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        bool thunkRan = false;
        if (thunk) {
            const int64_t rel = stubDrain - (thunk + 8);
            if (rel == static_cast<int32_t>(rel)) {
                uint8_t t[9] = {0x48, 0x89, 0xC8, 0xE8, 0, 0, 0, 0, 0xC3};
                const int32_t rel32 = static_cast<int32_t>(rel);
                memcpy(t + 4, &rel32, 4);
                memcpy(thunk, t, sizeof t);
                FlushInstructionCache(GetCurrentProcess(), thunk, sizeof t);
                reinterpret_cast<RcxFn>(thunk)(ringSession);
                reinterpret_cast<RcxFn>(thunk)(ringSession);
                reinterpret_cast<RcxFn>(thunk)(emptySession);
                thunkRan = true;
            }
            VirtualFree(thunk, 0, MEM_RELEASE);
        }
        if (g_count[0] != 1 || (thunkRan && g_count[1] != 2)) fail = 9;
    }
    g_count = savedCount;
    return fail;
}

}  // namespace opf
