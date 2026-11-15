// ImpactPreload (worker impactload 2026-11-12; [Fixes] ImpactPreload, default 0 until a release, the lab ini switches it on).
// Installed at start-up when ImpactPreload != 0 (needs NpcCreationPacing: it runs from its frame-end hook), then live 0/1.
//
// What it fixes (findings "impactload"; the GAME's behaviour, not ours): when a weapon hits a character, the game's hit handler
// (exe+0x47749E0) schedules two short timers (1/15 s): one spawns the blood effect, the other a "pawn impact" effect chosen by
// the surface of the hit (BPCI_PawnImpact_Metal, ..._Stone, ...). Each timer gets its effect's actor class from a soft class
// pointer with LoadSynchronous, so the FIRST hit of each surface in a session (and again after the class was garbage
// collected) loads that blueprint and everything it uses on the game thread: 65 ms in a play recording near Mortal Camp
// (2026-11-11, the play's worst frame, 84 ms), 92-103 ms frames at the same place the day before.
// Where the classes are listed: the game's settings object (the CDO of Altar's settings class) holds the blood effect class
// (BloodVFXBlueprintClass, a TSoftClassPtr at +0x78) and the impact table (ImpactSystemDataTable, an FSoftObjectPath at +0x240,
// a UDataTable). The game reads the table on its first use (exe+0x47F5430: TryLoad, then GetAllRows into a global list of row
// pointers, then the table is rooted); a row is one surface (TargetMaterial byte at +8) with, among others, its pawn impact
// class (PawnSparkBlueprintClass, a TSoftClassPtr at +0x80).
//
// The fix: as soon as the settings object exists (it does at the main menu), the table is requested with LoadPackageAsync if it
// is not loaded; once it is, the game's own row-list set-up is called (exactly what the first hit would do: the same rows, the
// table rooted by the game's code); then every row's pawn impact class and the blood effect class not loaded yet are requested
// in the background (LoadPackageAsync, priority 0, no delegate), and each one is added to the root set when it is loaded (the
// game's own way of keeping its table). The game's LoadSynchronous then finds the class loaded (ResolveObject) and spawns the
// same actor of the same class at the same moment. Identical in what is drawn and played: the same objects, loaded earlier.
// Cost: the classes stay in memory for the session (the log line gives the process memory before and after). Requests are made
// once; a package not loaded 60 s after its request is given up (the game then loads it itself, as stock).
// Patterns (any mismatch: not installed, one log line): the row-list function (its settings-class getter call, the settings
// object at class+0x110, the table path at +0x240, TryLoad, the row list, GUObjectArray's count and chunk table, the root bit
// it sets), the getter's static-class cache, the pawn impact timer's LoadSynchronous of row+0x80, the hit handler's reads of the
// row path (+0x88) and of the blood class (+0x78 / +0x80 / +0x90); ResolveObject, ToString, FMemory::Free, LoadPackageAsync
// from create_preload.cpp's patterns.
#include "common.h"

#include <psapi.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// exe+0x47F5430 (the impact table's row lookup; first call sets up the row list)
constexpr char kRowLookup[] =
    "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 30 48 8B E9 E8 ?? ?? ?? ?? 48 8B D8 48 83 B8 10 01 00 00 00 75 08 48 8B C8 E8 ?? ?? ?? "
    "?? 8B 05 ?? ?? ?? ?? 48 8B 8B 10 01 00 00 85 C0 0F 85 ?? ?? ?? ?? 48 81 C1 40 02 00 00 33 D2 E8";
constexpr size_t kGetterCall = 0x12, kCountRead = 0x2C, kCdoRead = 0x32, kTablePath = 0x41;
// +0xA0: the row list passed to GetAllRows (lea r8, [list]); +0xC4: GUObjectArray (NumElements, chunk table); +0xF0: bit 30
constexpr char kRowListArg[] = "4C 8D 05 ?? ?? ?? ?? 48 8B CB 48 0F 45 54 24 20 E8";
constexpr size_t kRowListAt = 0xA0;
constexpr char kObjArray[] = "8B 43 0C 3B 05 ?? ?? ?? ?? 7D 1E 8B C8 0F B7 C0 48 C1 E9 10 48 8D 14 40 48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 4C 8D 04 D1";
constexpr size_t kObjArrayAt = 0xC4;
constexpr char kRootBit[] = "41 8B 40 08 0F BA E0 1E";
constexpr size_t kRootBitAt = 0xF0;
// the settings class getter (exe+0x46AD100): mov rax, [static class]; test rax, rax; jnz
constexpr char kGetter[] = "4C 8B DC 48 83 EC 78 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85";
// the pawn impact timer (exe+0x4752CA0): rcx = [rbx+0x10] + 0x80 (the row's class), then LoadSynchronous
constexpr char kSparkLoad[] = "48 8B 4B 10 48 83 E9 80 48 89 BC 24 ?? ?? ?? ?? E8";
// the hit handler (exe+0x47749E0): the row's path at +0x88 (FString at +0x98); the blood class at +0x78 (path +0x80, +0x90)
constexpr char kRowRead[] = "48 83 BF 88 00 00 00 00 0F 84 ?? ?? ?? ?? 0F 10 87 88 00 00 00 33 DB 48 8D 97 98 00 00 00";
constexpr char kBloodRead[] = "48 8B 4F 78 48 8D 97 90 00 00 00 48 89 4D ?? 48 8D 4D ?? 0F 10 87 80 00 00 00";
constexpr ptrdiff_t kHandlerSpan = 0x1000;  // both reads in one function
constexpr uint32_t kCdo = 0x110, kTable = 0x240, kRowClass = 0x80, kBlood = 0x78, kSoftPath = 8;  // path after the weak pointer
constexpr uint32_t kRootSet = 1u << 30;  // EInternalObjectFlags::RootSet

using ResolveFn = void* (*)(const void* sp);
using ToStringFn = void* (*)(const void* sp, void* outStr);
using FreeFn = void (*)(void*);
using LoadAsyncFn = int32_t (*)(const void* name, void* delegate, int32_t priority, uint32_t flags, int32_t pie);
using InitRowsFn = void* (*)(void* physMat);

struct FStr {
    wchar_t* data;
    int32_t num, max;
};

constexpr int kMax = 128;           // classes tracked (the table has one row per surface)
constexpr uint64_t kGiveUpMs = 60000;
constexpr uint64_t kTableGiveUpMs = 60000;
constexpr int kEvery = 15;          // frames between checks while working

enum State { kIdle = 0, kTableWait, kClassesWait, kDone, kFailed };

struct Entry {
    const uint8_t* path;  // the FSoftObjectPath (in the row or the settings object)
    uint64_t pkg;         // its package FName (dedupe)
    uint64_t asked;       // GetTickCount64 of the request (0 = not requested yet, or was loaded already)
    uint8_t done;         // 1 rooted, 2 given up
    uint64_t memBefore;   // process private bytes when it was requested (one request at a time)
    double mb;            // private bytes added while it loaded (MB; only this load was ours)
    uint32_t ms;          // request to loaded
    char name[48];        // the class's asset name (for the log)
};

struct State_ {
    bool installed = false;
    ImpactPreloadSites s{};
    ResolveFn resolve = nullptr;
    ToStringFn toString = nullptr;
    FreeFn free = nullptr;
    LoadAsyncFn loadAsync = nullptr;
    InitRowsFn initRows = nullptr;
    // self-test fakes
    const void* (*fakeResolve)(const void*) = nullptr;
    void (*fakeRequest)(const void*) = nullptr;
    void (*fakeInit)(void*) = nullptr;
    // game thread
    int state = kIdle;
    int frames = 0;
    uint64_t tableAsked = 0;
    Entry e[kMax] = {};
    int n = 0;
    int rows = 0;
    uint64_t startMs = 0;
    uint64_t memStart = 0, memEnd = 0;
    bool namesLogged = false;
    uint64_t doneMs = 0;
    std::atomic<int> mode{0};  // 0 off, 1 preload, 2 watch only (lab: log first uses, request nothing)
    uint8_t used[128] = {};  // rows whose class the game has used since (logged once)
    int scans = 0;
    // stats
    std::atomic<int> requested{0}, rooted{0}, resident{0}, failed{0};
    std::atomic<int> logged{-1};
} g;

uint64_t PrivateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) return 0;
    return pmc.PrivateUsage;
}

const void* Resolve(const uint8_t* sp)
{
    if (g.fakeResolve) return g.fakeResolve(sp);
    __try {
        if (!*reinterpret_cast<const uint64_t*>(sp)) return nullptr;
        return g.resolve(sp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// LoadPackageAsync(package of the path), priority 0 (background), no delegate; false = no request made
bool Request(const uint8_t* sp)
{
    if (g.fakeRequest) {
        g.fakeRequest(sp);
        return true;
    }
    wchar_t buf[512];
    int n = 0;
    FStr out{};
    __try {
        g.toString(sp, &out);
        if (out.data && out.num > 1)
            while (n < out.num - 1 && n < 510 && out.data[n] && out.data[n] != L'.' && out.data[n] != L':') {
                buf[n] = out.data[n];
                ++n;
            }
        if (out.data) g.free(out.data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (n < 2) return false;
    buf[n] = 0;
    FStr name{buf, n + 1, n + 1};
    uint64_t delegate[2] = {0, 0};  // unbound FLoadPackageAsyncDelegate
    __try {
        g.loadAsync(&name, delegate, 0, 0, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// AddToRoot as the game's row-list set-up does it (bit 30 of the object's GUObjectArray item flags)
bool Root(const void* obj)
{
    __try {
        const int32_t idx = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(obj) + 0xC);
        if (idx < 0 || idx >= *g.s.objNum) return false;
        uint8_t* const item = (*g.s.objChunks)[idx >> 16] + static_cast<size_t>(idx & 0xFFFF) * 0x18;
        if (*reinterpret_cast<const void* const*>(item) != obj) return false;
        InterlockedOr(reinterpret_cast<volatile LONG*>(item + 8), static_cast<LONG>(kRootSet));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const uint8_t* Settings()
{
    __try {
        const uint8_t* cls = *g.s.classGlobal;
        if (!cls) return nullptr;
        return *reinterpret_cast<const uint8_t* const*>(cls + g.s.cdo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The class's asset name (the path's last segment, without the package part) for the log
void NoteName(const uint8_t* sp, char* tmp, int cap)
{
    tmp[0] = 0;
    if (g.fakeResolve) return;
    FStr out{};
    int n = 0;
    __try {
        g.toString(sp, &out);
        if (out.data && out.num > 1) {
            int start = 0;
            for (int i = 0; i < out.num - 1; ++i)
                if (out.data[i] == L'.') start = i + 1;
            for (int i = start; i < out.num - 1 && n < cap - 1; ++i) tmp[n++] = static_cast<char>(out.data[i] < 128 ? out.data[i] : '?');
        }
        if (out.data) g.free(out.data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    tmp[n] = 0;
}

void Add(const uint8_t* sp)
{
    uint64_t pkg = 0;
    __try {
        pkg = *reinterpret_cast<const uint64_t*>(sp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!pkg) return;  // no class in this slot
    for (int i = 0; i < g.n; ++i)
        if (g.e[i].pkg == pkg) return;
    if (g.n >= kMax) return;
    Entry& e = g.e[g.n++];
    e = Entry{};
    e.path = sp;
    e.pkg = pkg;
    NoteName(sp, e.name, sizeof e.name);
    if (const void* obj = Resolve(sp)) {
        e.done = Root(obj) ? 1 : 2;
        g.resident.fetch_add(1, std::memory_order_relaxed);
        if (e.done == 1) g.rooted.fetch_add(1, std::memory_order_relaxed);
        else g.failed.fetch_add(1, std::memory_order_relaxed);
    }
    // not loaded: requested by Step, one at a time
}

// the table is loaded: the game's row-list set-up (if not done yet), then every row's pawn impact class and the blood class
void Collect(const uint8_t* cdo)
{
    int32_t num = 0;
    __try {
        num = *reinterpret_cast<const int32_t*>(g.s.rowList + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        num = -1;
    }
    if (num == 0) {
        if (g.fakeInit) g.fakeInit(nullptr);
        else {
            __try {
                g.initRows(nullptr);  // physical material nullptr: sets up the list and finds no row
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    __try {
        uint8_t* const* rows = *reinterpret_cast<uint8_t* const* const*>(g.s.rowList);
        num = *reinterpret_cast<const int32_t*>(g.s.rowList + 8);
        if (num < 0 || num > 4096 || (num && !rows)) num = 0;
        g.rows = num;
        for (int i = 0; i < num; ++i)
            if (rows[i]) Add(rows[i] + g.s.rowClass + kSoftPath);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Add(cdo + g.s.blood + kSoftPath);
}

void Step()
{
    const uint8_t* const cdo = Settings();
    if (!cdo) return;  // the settings object does not exist yet: never created here
    const uint64_t now = GetTickCount64();
    if (g.state == kIdle) {
        g.startMs = now;
        g.memStart = g.fakeResolve ? 0 : PrivateBytes();
        if (!g.fakeResolve)
            Log("[impactpreload] the game's settings exist (frame %llu): %s", static_cast<unsigned long long>(NpcPacingFrame()),
                Resolve(cdo + g.s.table) ? "the impact table is loaded" : "requesting the impact table");
        if (Resolve(cdo + g.s.table)) {
            g.state = kClassesWait;
            Collect(cdo);
        } else if (Request(cdo + g.s.table)) {
            g.tableAsked = now;
            g.state = kTableWait;
            g.requested.fetch_add(1, std::memory_order_relaxed);
        } else {
            g.state = kFailed;
        }
        if (g.state != kClassesWait) return;
    } else if (g.state == kTableWait) {
        if (Resolve(cdo + g.s.table)) {
            g.state = kClassesWait;
            Collect(cdo);
        } else if (now - g.tableAsked > kTableGiveUpMs) {
            g.state = kFailed;
        }
        if (g.state != kClassesWait) return;
    }
    if (g.state == kClassesWait) {
        // one request at a time (a small background load each; its memory measured on its own)
        for (int i = 0; i < g.n; ++i) {
            Entry& e = g.e[i];
            if (e.done || !e.asked) continue;
            if (const void* obj = Resolve(e.path)) {
                e.done = Root(obj) ? 1 : 2;
                e.ms = static_cast<uint32_t>(now - e.asked);
                e.mb = g.fakeResolve ? 0.0 : (static_cast<double>(PrivateBytes()) - static_cast<double>(e.memBefore)) / 1048576.0;
                if (e.done == 1) g.rooted.fetch_add(1, std::memory_order_relaxed);
                else g.failed.fetch_add(1, std::memory_order_relaxed);
            } else if (now - e.asked > kGiveUpMs) {
                e.done = 2;
                g.failed.fetch_add(1, std::memory_order_relaxed);
            } else {
                return;  // still loading
            }
        }
        for (int i = 0; i < g.n; ++i) {
            Entry& e = g.e[i];
            if (e.done || e.asked) continue;
            e.memBefore = g.fakeResolve ? 0 : PrivateBytes();
            if (Request(e.path)) {
                e.asked = now ? now : 1;
                g.requested.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            e.done = 2;
            g.failed.fetch_add(1, std::memory_order_relaxed);
        }
        g.state = kDone;
        g.doneMs = now;
        g.memEnd = g.fakeResolve ? 0 : PrivateBytes();
        if (!g.fakeResolve)
            Log("[impactpreload] all hit effects loaded and kept (frame %llu, %.1f s after the first request; process private "
                "memory %+.1f MB over that time)", static_cast<unsigned long long>(NpcPacingFrame()), (now - g.startMs) / 1000.0,
                (static_cast<double>(g.memEnd) - static_cast<double>(g.memStart)) / 1048576.0);
    }
}

uint8_t* One(const GameModule& game, const char* pattern)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}

}  // namespace

bool ImpactPreloadDiscover(const GameModule& game, ImpactPreloadSites* out, const char** missing)
{
    *out = {};
    *missing = nullptr;
    const auto fail = [&](const char* what) { *missing = what; return false; };
    uint8_t* const f = One(game, kRowLookup);
    if (!f) return fail("the impact table's row lookup");
    if (!MatchesAt(f + kRowListAt, ParsePattern(kRowListArg)) || !MatchesAt(f + kObjArrayAt, ParsePattern(kObjArray)) ||
        !MatchesAt(f + kRootBitAt, ParsePattern(kRootBit)))
        return fail("the row lookup's list / object array / root bit");
    out->rowLookup = f;
    out->cdo = *reinterpret_cast<const uint32_t*>(f + kCdoRead + 3);
    out->table = *reinterpret_cast<const uint32_t*>(f + kTablePath + 3);
    if (out->cdo != kCdo || out->table != kTable) return fail("the settings object / table path offsets");
    out->rowList = RipTarget(f + kRowListAt, 3, 7);
    uint8_t* const count = RipTarget(f + kCountRead, 2, 6);
    if (count != out->rowList + 8) return fail("the row list's count");
    out->objNum = reinterpret_cast<int32_t*>(RipTarget(f + kObjArrayAt + 3, 2, 6));
    out->objChunks = reinterpret_cast<uint8_t***>(RipTarget(f + kObjArrayAt + 24, 3, 7));
    out->classGetter = RipTarget(f + kGetterCall, 1, 5);
    if (!game.text.Contains(out->classGetter) || !MatchesAt(out->classGetter, ParsePattern(kGetter)))
        return fail("the settings class getter");
    out->classGlobal = reinterpret_cast<uint8_t**>(RipTarget(out->classGetter + 7, 3, 7));
    if (!game.data.Contains(out->classGlobal) || !game.data.Contains(out->rowList) || !game.data.Contains(out->objNum) ||
        !game.data.Contains(out->objChunks))
        return fail("the globals' section");
    if (!(out->pawnSparkLoad = One(game, kSparkLoad))) return fail("the pawn impact timer's class load");
    out->rowClass = kRowClass;
    out->rowRead = One(game, kRowRead);
    out->bloodRead = One(game, kBloodRead);
    if (!out->rowRead || !out->bloodRead || out->rowRead < out->bloodRead || out->rowRead - out->bloodRead > kHandlerSpan)
        return fail("the hit handler's row and blood class reads");
    out->blood = kBlood;
    return true;
}

bool InstallImpactPreload(bool beforeGameCode)
{
    if (g.installed) return true;
    (void)beforeGameCode;  // only reads and calls at frame ends: any time is safe
    NpcPacingInfoOut pi{};
    if (!NpcPacingInfo(&pi)) {
        Log("[impactpreload] ImpactPreload: needs NpcCreationPacing, not installed");
        return false;
    }
    const GameModule& game = Game();
    const char* missing = nullptr;
    if (!ImpactPreloadDiscover(game, &g.s, &missing)) {
        Log("[impactpreload] ImpactPreload: %s not recognised, not installed", missing);
        return false;
    }
    SoftLoadApi api{};
    if (!FindSoftLoadApi(game, &api)) {
        Log("[impactpreload] ImpactPreload: ResolveObject / ToString / FMemory::Free / LoadPackageAsync not recognised, not installed");
        return false;
    }
    g.resolve = reinterpret_cast<ResolveFn>(api.resolve);
    g.toString = reinterpret_cast<ToStringFn>(api.toString);
    g.free = reinterpret_cast<FreeFn>(api.free);
    g.loadAsync = reinterpret_cast<LoadAsyncFn>(api.loadAsync);
    g.initRows = reinterpret_cast<InitRowsFn>(g.s.rowLookup);
    g.installed = true;
    Log("[impactpreload] ImpactPreload installed (row lookup exe+0x%llX, row list exe+0x%llX, settings class cache exe+0x%llX)",
        static_cast<unsigned long long>(g.s.rowLookup - game.base), static_cast<unsigned long long>(g.s.rowList - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g.s.classGlobal) - game.base));
    return true;
}

void SetImpactPreload(int mode)
{
    mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    if (g.mode.exchange(mode) != mode && g.installed)
        Log("[impactpreload] ImpactPreload %d: %s", mode,
            mode == 1 ? "the hit effects are loaded in the background and kept"
                      : mode == 2 ? "first uses are logged only (nothing is requested)" : "no more requests (what is loaded stays)");
}

// After the preload (or in mode 2): a row whose class the game's LoadSynchronous has used since (that call sets its
// TSoftClassPtr's weak pointer, row +0x80) is logged once with whether we kept it loaded: the first hit of each surface.
void UseScan()
{
    if (g.fakeResolve) return;
    __try {
        uint8_t* const* rows = *reinterpret_cast<uint8_t* const* const*>(g.s.rowList);
        const int32_t num = *reinterpret_cast<const int32_t*>(g.s.rowList + 8);
        for (int i = 0; i < num && i < 128 && rows; ++i) {
            if (g.used[i] || !rows[i]) continue;
            if (*reinterpret_cast<const int32_t*>(rows[i] + g.s.rowClass) == 0) continue;  // weak ObjectIndex not set yet
            g.used[i] = 1;
            const uint8_t* sp = rows[i] + g.s.rowClass + kSoftPath;
            const uint64_t pkg = *reinterpret_cast<const uint64_t*>(sp);
            int kept = 0;
            for (int k = 0; k < g.n; ++k)
                if (g.e[k].pkg == pkg) kept = g.e[k].done == 1 ? 1 : 2;
            char name[64];
            NoteName(sp, name, sizeof name);
            Log("[impactpreload] first use in this session of a surface's hit effect (row %d, surface %u): %s, %s", i,
                static_cast<unsigned>(rows[i][8]), name[0] ? name : "?",
                kept == 1 ? "kept loaded by ImpactPreload" : kept == 2 ? "requested but not loaded by ImpactPreload" : "not preloaded");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ImpactPreloadFrame()
{
    if (!g.installed) return;
    const int mode = g.mode.load(std::memory_order_relaxed);
    if (!mode) return;
    if (++g.frames < kEvery) return;
    g.frames = 0;
    if (mode == 2 || g.state == kDone) {
        if (++g.scans >= 4) {  // every 60 frames
            g.scans = 0;
            UseScan();
        }
        return;
    }
    if (g.state == kFailed) return;
    Step();
}

void LogImpactPreloadStats()
{
    if (!g.installed) return;
    const int key = g.state * 1000 + g.rooted.load() * 10 + (g.failed.load() ? 1 : 0);
    if (g.logged.exchange(key) == key) return;
    static const char* const kNames[] = {"waiting for the settings", "loading the impact table", "loading the effect classes",
                                         "done", "gave up (table not loaded)"};
    const uint64_t end = g.doneMs ? g.doneMs : GetTickCount64();
    const double s = g.startMs ? static_cast<double>(end - g.startMs) / 1000.0 : 0.0;
    Log("[impactpreload] %s: %d table rows, %d classes (%d already loaded), %d requested, %d kept loaded, %d not loaded "
        "(left to the game); %.1f s from the first request%s; process memory %+.1f MB over that time",
        kNames[g.state], g.rows, g.n, g.resident.load(), g.requested.load(), g.rooted.load(), g.failed.load(), s,
        g.doneMs ? " to the last class loaded" : "",
        g.memEnd && g.memStart ? (static_cast<double>(g.memEnd) - static_cast<double>(g.memStart)) / 1048576.0 : 0.0);
    if (g.state == kDone && !g.namesLogged && g.n) {
        g.namesLogged = true;
        char line[2048];
        size_t len = 0;
        double sum = 0;
        for (int i = 0; i < g.n && len < sizeof line - 96; ++i) {
            const Entry& e = g.e[i];
            int w;
            if (e.asked && e.done == 1) {
                sum += e.mb;
                w = snprintf(line + len, sizeof line - len, "%s%s (%u ms, %+.1f MB)", len ? ", " : "", e.name, e.ms, e.mb);
            } else {
                w = snprintf(line + len, sizeof line - len, "%s%s (%s)", len ? ", " : "", e.name,
                             e.done == 1 ? "was loaded" : "not loaded");
            }
            if (w > 0) len += static_cast<size_t>(w);
        }
        Log("[impactpreload] classes (one request at a time; memory added while each loaded, %+.1f MB in all): %s", sum, line);
    }
}

void ImpactPreloadTestSetup(const ImpactPreloadFakes* f)
{
    g.state = kIdle;
    g.frames = 0;
    g.n = 0;
    g.rows = 0;
    g.tableAsked = g.startMs = g.memStart = g.memEnd = 0;
    g.requested = g.rooted = g.resident = g.failed = 0;
    g.namesLogged = false;
    g.doneMs = 0;
    memset(g.used, 0, sizeof g.used);
    g.scans = 0;
    if (!f) {
        g.fakeResolve = nullptr;
        g.fakeRequest = nullptr;
        g.fakeInit = nullptr;
        g.installed = false;
        g.mode = 0;
        return;
    }
    g.fakeResolve = f->resolve;
    g.fakeRequest = f->request;
    g.fakeInit = f->initRows;
    g.s.classGlobal = f->classGlobal;
    g.s.rowList = f->rowList;
    g.s.objNum = f->objNum;
    g.s.objChunks = f->objChunks;
    g.s.cdo = kCdo;
    g.s.table = kTable;
    g.s.rowClass = kRowClass;
    g.s.blood = kBlood;
    g.installed = true;
    g.mode = 1;
}

ImpactPreloadTestState ImpactPreloadTestRead()
{
    return {g.state, g.requested.load(), g.rooted.load(), g.resident.load(), g.rows, g.failed.load()};
}

}  // namespace opf
