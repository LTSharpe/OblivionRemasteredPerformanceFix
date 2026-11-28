// MenuLoadKeep: what the menus load synchronously stays loaded across garbage collections (S-050 dialogue open, S-051 menu
// opens, S-069 neighbours). Design of 2026-11-25 (worker menudesign), built by worker menubuild.
//
// What stock does (build 19115871, findings menuedge / menupair / menucost): Altar's menu and dialogue commands reach their
// widget Blueprint classes (WBP_ModernMenu_Dialog, _Message, _Credits, ...), their item Blueprint classes and their icon
// textures through FSoftObjectPath::TryLoad (exe+0x10F8A90) on the game thread: a synchronous load (5-12 ms for the dialogue
// class, 19.6 ms for Credits, 15-25 ms of icons and item classes in the first inventory / pause open after a GC). The objects
// are referenced weakly: the next garbage collection (every ~61 s in a played game) purges them once the menu is closed, so
// the NEXT open loads them again. A menu reopened before the next GC finds them resident and pays nothing.
//
// The key: the object TryLoad returns gets UE's RootSet flag (FUObjectItem.Flags bit 30 in GUObjectArray, set with an interlocked
// OR as UObjectBase::AddToRoot does) when (a) the game is in a menu, a dialogue or on the main menu (the freeze subsystem holds a
// NAMED freeze and freeze mode 0; a loading screen, mode 1, does not count: review S1-1, lab data), (b) the object's class is a widget Blueprint class, an item Blueprint
// class or a texture (class name from the FName pool: WidgetBlueprintGeneratedClass, VModdableBlueprintGeneratedClass,
// Texture2D; the input mapping contexts, kind 3 below, need no menu state), (c) it is not garbage / unreachable / being loaded, and (d) the estimated memory of what we keep stays under
// MenuLoadKeepMB. A GC cluster member is rooted itself (UE 5.3 GarbageCollection.cpp: a root-set cluster root or member goes onto
// KeepClusterRefsList, so its cluster stays whole). What is kept stays until the key is switched to 0 or to the observe mode:
// there is no release at a loading screen or on world travel, so the cap is a session-long budget.
// A fourth kind (imckeep, 2026-11-25; S-050 / S-051): UInputMappingContext data assets (the UI's IMC_UI_Specific_* contexts). A
// Blueprint-called native (exe+0x491ED70) loops a soft-object array of them and calls LoadSynchronous on each at about 40 % of the
// menu and dialogue opens (the GC has purged them since the last edge): 29 loads per call, 4-6 ms. They are loaded while the
// freeze subsystem shows named 0 (lab data c1), so they are NOT gated on the menu state; they are tiny cooked data assets (a list
// of key bindings, ~64 KB counted each), at most kMaxData of them, and they never hit the MenuLoadKeepMB cap (their ~64 KB each
// are added to the total). Only the loads THAT loop makes are kept (review S2-1, fix1126): its LoadSynchronous call
// (exe+0x491EE04, found by pattern) is pointed through a near stub at a wrapper that marks the game thread while it runs; an
// input mapping context loaded from anywhere else (gameplay contexts, quick keys, lockpicking, the Settings page: the game's
// own key remapping stores per-context rebinds for IMC_Game_*, IMC_UI_Specific_QuickKeys / LockPickNavigation /
// Common_SwitchTab, none of which is in the loop's list) is left to the game. Not found: kind 3 off, the rest as before.
// Textures, classes and their data are immutable cooked assets: the game then sees after a GC exactly what it sees in stock when
// no GC ran between two opens (same objects, same view-model compare results); nothing drawn changes.
//   [Fixes] MenuLoadKeep      absent/0 = not installed (start-up), then live: 0 = off (our roots removed at the next call),
//                             1 = keep, 2 (lab) = observe only: one line per game-thread load of the classes above (class,
//                             object, ms, bytes estimate, cluster state, first or repeated load in this session, freeze
//                             state, caller). The public build reads 2 as 1.
//   [Fixes] MenuLoadKeepMB    cap of the kept set in MB (default 64; textures counted at 256 KB, classes at 64 KB until the
//                             lab's observe numbers set better estimates; counted per object, NOT measured: what the object
//                             references (a class's meshes and materials, a texture's mips) comes on top), then no more
//                             roots until the key is switched off; logged once.
//   (A preload of the menu classes at the main menu is not built: LoadPackageAsync returns no object to root; see the
//   menubuild findings.)
// Sites: the PE time stamp of build 19115871, FSoftObjectPath::TryLoad by a unique byte pattern at the known place (its first
// 6 bytes relocated), GUObjectArray from ~UObjectBase (object_replace_guard.cpp), the FName pool (diagnostics/tick_split.cpp's
// search, a copy here), the freeze subsystem (FindFreezeSubsystem, called here). Any miss: not installed (or no class known:
// nothing kept), with a log line. Not together with MenuIconKeep (lab): both would own the same RootSet bits.
// Epic: Unreal Engine 5.3 UObjectArray.h / GarbageCollection.cpp (RootSet, clusters); the idea only, no source text copied.
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {

constexpr uint32_t kTestedStamp = 0xF19077A4u;  // build 19115871
constexpr uint32_t kTryLoadRva = 0x10F8A90;
// FSoftObjectPath::TryLoad(this, FUObjectSerializeContext*): home store of rbx, push rdi, sub rsp,60h, xor edi,edi, mov rbx,rcx,
// mov [rsp+70h],rdi, cmp [rcx],rdi; je; cmp dword [rcx+18h],1; jle (the two rel32 are this function's own branches).
constexpr char kTryLoadPattern[] =
    "48 89 5C 24 10 57 48 83 EC 60 33 FF 48 8B D9 48 89 7C 24 70 48 39 39 0F 84 ?? ?? ?? ?? 83 79 18 01 0F 8E ?? ?? ?? ??";
constexpr char kTryLoadPrologue[] = "48 89 5C 24 10 57";  // mov [rsp+10h],rbx; push rdi (relocated as is)

// GUObjectArray (FUObjectArray, UE 5.3) and FUObjectItem, as object_replace_guard.cpp / menu_icon_keep.cpp read them
constexpr size_t kArrayChunks = 0x10, kArrayNum = 0x24;
struct Item {
    void* object;
    int32_t flags;
    int32_t clusterRootIndex;  // > 0: a member (the cluster root's object index); < 0: a cluster root (-index - 1)
    int32_t serialNumber;
    int32_t pad;
};
static_assert(sizeof(Item) == 24);
constexpr int32_t kGarbage = 1 << 21, kReachableInCluster = 1 << 23, kClusterRoot = 1 << 24, kAsync = 1 << 26,
                  kAsyncLoading = 1 << 27, kUnreachable = 1 << 28, kPendingKill = 1 << 29, kRootSet = 1 << 30;
constexpr int32_t kBadFlags = kGarbage | kUnreachable | kPendingKill | kAsync | kAsyncLoading;
constexpr int kMaxKept = 4096;
constexpr uint64_t kEstTexture = 256 * 1024, kEstClass = 64 * 1024, kEstData = 64 * 1024;
constexpr int kMaxData = 256;  // input mapping contexts kept at most (the game has ~15-30)
constexpr int kKindClass = 1, kKindTexture = 2, kKindData = 3;

constexpr size_t kObjClassOff = 0x10, kObjNameOff = 0x18, kObjIndexOff = 0xC, kObjOuterOff = 0x20;  // UObjectBase: Class, NamePrivate (index, number), InternalIndex

using TryLoadFn = void* (*)(void* path, void* context);
TryLoadFn g_orig = nullptr;
uint8_t* g_array = nullptr;
std::atomic<int> g_mode{0};
std::atomic<uint64_t> g_capBytes{64ull << 20};
DWORD g_gt = 0;
int g_depth = 0;  // TryLoad calls itself for a sub-object path: only the outermost call is looked at

struct Kept {
    void* obj;
    int32_t index;
    int32_t serial;    // FUObjectItem.SerialNumber when rooted
    const void* cls;   // the object's class when rooted
    uint64_t est;
    bool data;         // kind 3: counted in g_numData, not against the MB cap
};
Kept g_kept[kMaxKept];
int g_numKept = 0;
int g_numData = 0;
uint64_t g_keptBytes = 0;
bool g_capLogged = false;
// imckeep / fix1126: the exe+0x491ED70 loop's LoadSynchronous call
using LoadSyncFn = void* (*)(void* softPtr);
LoadSyncFn g_loadSync = nullptr;  // FSoftObjectPtr::LoadSynchronous (the call's original target)
uint8_t* g_loopCall = nullptr;    // the E8 call in the loop
int g_inLoop = 0;                 // > 0 while the game thread runs that call

// class verdicts by class pointer: 1 wanted (and which estimate), 2 not wanted; unknown (0) = the name could not be read yet
struct Verdict {
    const void* cls;
    uint8_t v;      // 1 wanted, 2 not
    uint8_t kind;   // kKindClass / kKindTexture / kKindData when wanted
};
constexpr int kVerdicts = 128;
Verdict g_verdict[kVerdicts];
int g_numVerdicts = 0;

struct Stats {
    uint64_t loads, us, wanted, rooted, already, skipped, capped, clusterMember, outOfMenu, unknownClass, unrooted;
} g_s = {};

// ---- the FName pool (copy of diagnostics/tick_split.cpp's search; found once on the monitor thread) ----
std::atomic<const uint8_t*> g_pool{nullptr};
std::atomic<bool> g_poolSearched{false};

bool PoolAt(const uint8_t* p)  // p = the qword {CurrentBlock, CurrentByteCursor}
{
    __try {
        const uint64_t q = *reinterpret_cast<const uint64_t*>(p);
        const uint32_t lo = static_cast<uint32_t>(q), hi = static_cast<uint32_t>(q >> 32);
        if (lo < 1 || lo >= 8192 || hi == 0 || hi >= (1u << 18) || (hi & 1)) return false;
        const uint64_t* const blocks = reinterpret_cast<const uint64_t*>(p + 8);
        if (blocks[lo + 1] != 0) return false;
        for (uint32_t i = 0; i <= lo; ++i)
            if (blocks[i] <= 0x10000) return false;
        const uint8_t* const b0 = reinterpret_cast<const uint8_t*>(blocks[0]);
        return (*reinterpret_cast<const uint16_t*>(b0) >> 6) == 4 && memcmp(b0 + 2, "None", 4) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const uint8_t* FindNamePool()
{
    const GameModule& game = Game();
    for (const uint8_t* r = game.data.begin; r < game.data.end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(r, &mbi, sizeof mbi)) break;
        const uint8_t* const end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                              (prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY);
        if (readable)
            for (const uint8_t* p = r < game.data.begin + 8 ? game.data.begin + 8 : r; p + 16 <= end && p + 16 <= game.data.end; p += 8) {
                const uint64_t q = *reinterpret_cast<const uint64_t*>(p);
                const uint32_t lo = static_cast<uint32_t>(q);
                if (lo >= 1 && lo < 8192 && (q >> 32) && PoolAt(p)) return p - 8;
            }
        r = end;
    }
    return nullptr;
}

// An object's own name ("Texture2D", "WBP_ModernMenu_Dialog_C"), or false.
bool ObjectName(const void* obj, char* out, size_t size)
{
    const uint8_t* const pool = g_pool.load(std::memory_order_acquire);
    if (!pool || !obj || size < 16) return false;
    __try {
        const uint32_t index = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(obj) + kObjNameOff);
        const uint32_t number = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(obj) + kObjNameOff + 4);
        const uint8_t* const block = *reinterpret_cast<const uint8_t* const*>(pool + 16 + (index >> 16) * 8);
        if (!block) return false;
        const uint8_t* const e = block + (index & 0xFFFF) * 2;
        const uint16_t hdr = *reinterpret_cast<const uint16_t*>(e);
        const size_t n = hdr >> 6;
        if (n == 0 || n + 12 >= size) return false;
        for (size_t i = 0; i < n; ++i) {
            const int c = (hdr & 1) ? reinterpret_cast<const wchar_t*>(e + 2)[i] : reinterpret_cast<const char*>(e + 2)[i];
            out[i] = c >= 32 && c < 127 && c != ',' ? static_cast<char>(c) : '?';
        }
        out[n] = 0;
        if (number) snprintf(out + n, size - n, "_%u", number - 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The classes whose loads are kept: the class OBJECT's name of the loaded object. Returns the kind (0 = not wanted).
int WantedKind(const char* name)
{
    if (!strcmp(name, "WidgetBlueprintGeneratedClass") || !strcmp(name, "VModdableBlueprintGeneratedClass")) return kKindClass;
    if (!strcmp(name, "Texture2D")) return kKindTexture;
    if (!strcmp(name, "InputMappingContext")) return kKindData;
    return 0;
}

// 0 unknown (name not readable yet), 1 wanted (kind in *kind), 2 not wanted
int ClassVerdict(const void* cls, int* kind)
{
    *kind = 0;
    for (int i = 0; i < g_numVerdicts; ++i)
        if (g_verdict[i].cls == cls) {
            *kind = g_verdict[i].kind;
            return g_verdict[i].v;
        }
    char name[96];
    if (!ObjectName(cls, name, sizeof name)) return 0;
    *kind = WantedKind(name);
    if (g_numVerdicts < kVerdicts) g_verdict[g_numVerdicts++] = {cls, static_cast<uint8_t>(*kind ? 1 : 2), static_cast<uint8_t>(*kind)};
    return *kind ? 1 : 2;
}

bool IsGt();
// the loop's LoadSynchronous call, redirected here: marks the game thread while it runs (the loads it makes may be kept)
void* LoopLoadHook(void* softPtr)
{
    if (!IsGt()) return g_loadSync(softPtr);
    ++g_inLoop;
    void* const r = g_loadSync(softPtr);
    --g_inLoop;
    return r;
}

bool IsGt()
{
    if (g_gt) return GetCurrentThreadId() == g_gt;
    PWSTR name = nullptr;
    bool gt = false;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name) {
        gt = wcscmp(name, L"GameThread") == 0;
        LocalFree(name);
    }
    if (gt) g_gt = GetCurrentThreadId();
    return gt;
}

uint64_t NowUs()
{
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart) * 1000000ull / static_cast<uint64_t>(f.QuadPart);
}

Item* ItemAt(int32_t index)
{
    if (!g_array || index < 0) return nullptr;
    int32_t num;
    memcpy(&num, g_array + kArrayNum, 4);
    if (index >= num) return nullptr;
    Item** chunks;
    memcpy(&chunks, g_array + kArrayChunks, 8);
    if (!chunks || !chunks[index >> 16]) return nullptr;
    return chunks[index >> 16] + (index & 0xFFFF);
}

// In a menu, a dialogue or on the main menu: the freeze subsystem holds a NAMED freeze and its mode is 0. Lab mode 2 (menubuild
// c1, 2026-11-25): menus and pages show named 1 / 2 with mode 0; loading screens show mode 1 (named 1 or 0): the item classes of
// a load showed named 1 mode 1. Unknown (subsystem not found) = not in a menu: nothing is kept.
bool InMenuState(int32_t* named, int32_t* mode)
{
    *named = *mode = 0;
    return ReadMenuFreeze(named, mode) && *named != 0 && *mode == 0;
}

// Game thread: remove the roots we set, only where the item still holds the same object (pointer, serial number and class).
void Unroot()
{
    for (int i = 0; i < g_numKept; ++i) {
        Item* const it = ItemAt(g_kept[i].index);
        if (it && it->object == g_kept[i].obj && it->serialNumber == g_kept[i].serial &&
            *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(it->object) + kObjClassOff) == g_kept[i].cls) {
            _InterlockedAnd(reinterpret_cast<volatile long*>(&it->flags), ~kRootSet);
            ++g_s.unrooted;
        }
    }
    g_numKept = 0;
    g_numData = 0;
    g_keptBytes = 0;
    g_capLogged = false;
}

enum KeepResult { kRooted, kAlready, kSkipped, kCapped };

// Roots obj itself unless it must not be (a cluster member too: the engine puts a root-set member on KeepClusterRefsList, which
// keeps its cluster alive). est = the estimate of its memory in bytes.
KeepResult KeepObject(void* obj, uint64_t est, bool data = false)
{
    int32_t index;
    memcpy(&index, static_cast<uint8_t*>(obj) + kObjIndexOff, 4);
    Item* const it = ItemAt(index);
    if (!it || it->object != obj) return kSkipped;
    const int32_t f = it->flags;
    if (f & kRootSet) return kAlready;  // rooted by the game, or by us earlier
    if (f & kBadFlags) return kSkipped;
    if (g_numKept >= kMaxKept) return kCapped;
    if (data ? g_numData >= kMaxData : g_keptBytes + est > g_capBytes.load(std::memory_order_relaxed)) return kCapped;
    _InterlockedOr(reinterpret_cast<volatile long*>(&it->flags), kRootSet);
    g_kept[g_numKept++] = {obj, index, it->serialNumber, *reinterpret_cast<void* const*>(static_cast<uint8_t*>(obj) + kObjClassOff), est, data};
    if (data) ++g_numData;
    g_keptBytes += est;
    if (it->clusterRootIndex != 0) ++g_s.clusterMember;
    return kRooted;
}



void OnLoaded(void* path, void* obj, uint64_t us, int mode, void* caller)
{
    ++g_s.loads;
    g_s.us += us;
    const void* const cls = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(obj) + kObjClassOff);
    int kind = 0;
    const int verdict = ClassVerdict(cls, &kind);
    if (verdict == 0) ++g_s.unknownClass;
    const bool wanted = verdict == 1;
    if (wanted) ++g_s.wanted;
    int32_t named = 0, fmode = 0;
    bool inMenu = false;
    if (wanted && mode == 1) {
        // input mapping contexts: only the exe+0x491ED70 loop's loads, with no menu state needed (see header)
        inMenu = kind == kKindData ? g_inLoop > 0 : InMenuState(&named, &fmode);
        if (inMenu) {
            switch (KeepObject(obj, kind == kKindTexture ? kEstTexture : kind == kKindData ? kEstData : kEstClass, kind == kKindData)) {
                case kRooted: ++g_s.rooted; break;
                case kAlready: ++g_s.already; break;
                case kSkipped: ++g_s.skipped; break;
                case kCapped:
                    ++g_s.capped;
                    if (!g_capLogged) {
                        g_capLogged = true;
                        Log("[menuloadkeep] cap reached: %llu KB kept (estimated) in %d objects, no more roots until the key is switched off",
                            static_cast<unsigned long long>(g_keptBytes >> 10), g_numKept);
                    }
                    break;
            }
        } else ++g_s.outOfMenu;
    }
    (void)path;
    (void)caller;
}

void* TryLoadHook(void* path, void* context)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != 1 && g_numKept && IsGt()) Unroot();  // off or observe: release what was kept
    if (!mode) return g_orig(path, context);
    if (!IsGt() || g_depth > 0) return g_orig(path, context);
    void* const caller = _ReturnAddress();
    ++g_depth;
    const uint64_t t0 = NowUs();
    void* const r = g_orig(path, context);
    const uint64_t us = NowUs() - t0;
    --g_depth;
    if (r) OnLoaded(path, r, us, mode, caller);
    return r;
}

bool BuildSlot(uint8_t* fn, const Pattern& expect, void* hook, uint8_t* slot)
{
    const size_t n = expect.bytes.size();
    if (n < 5 || n > 24 || !MatchesAt(fn, expect)) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + n;
    memcpy(slot, fn, n);
    memcpy(slot + n, jmpAbs, 6);
    memcpy(slot + n + 6, &resume, 8);
    memcpy(slot + 64, jmpAbs, 6);
    memcpy(slot + 64 + 6, &hook, 8);
    return true;
}

bool Patch(uint8_t* fn, size_t n, uint8_t* stub)
{
    const int64_t rel = stub - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t code[24];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE9;
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &rel32, 4);
    return WriteCode(fn, std::span<const uint8_t>(code, n));
}

}  // namespace

// The loop exe+0x491ED70's per-element block: movups xmm0,[rdi+8]; mov rax,[rdi]; lea rdx,[rdi+18h]; lea rcx,[rsp+38h];
// mov [rsp+20h],rax; movups [rsp+28h],xmm0; call (build the soft pointer); lea rcx,[rsp+20h]; call LoadSynchronous (+0x24);
// mov rbp,rax; test; je; call (StaticClass); mov rcx,[rbp+10h]; add rax,30h ... and (+0x82) add rdi,28h; cmp rdi,r14; jne.
constexpr char kLoopBlock[] =
    "0F 10 47 08 48 8B 07 48 8D 57 18 48 8D 4C 24 ?? 48 89 44 24 ?? 0F 11 44 24 ?? E8 ?? ?? ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? "
    "48 8B E8 48 85 C0 74 ?? E8 ?? ?? ?? ?? 48 8B 4D 10 48 83 C0 30";
constexpr size_t kLoopCallAt = 0x24;
constexpr char kLoopStep[] = "48 83 C7 28 49 3B FE 0F 85";
constexpr size_t kLoopStepAt = 0x82;

uint8_t* MenuLoadKeepFindLoopCall(const GameModule& game)
{
    const auto hits = FindAll(game.text, ParsePattern(kLoopBlock));
    if (hits.size() != 1 || !MatchesAt(hits[0] + kLoopStepAt, ParsePattern(kLoopStep))) return nullptr;
    uint8_t* const call = hits[0] + kLoopCallAt;
    return game.text.Contains(RipTarget(call, 1, 5)) ? call : nullptr;
}

// Points the loop's call at a near stub (jmp [rip] -> LoopLoadHook); false (nothing changed) on any failure.
bool InstallLoopMark(const GameModule& game)
{
    uint8_t* const call = MenuLoadKeepFindLoopCall(game);
    if (!call) return false;
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&LoopLoadHook);
    memcpy(stub, jmpAbs, 6);
    memcpy(stub + 6, &hook, 8);
    DWORD old;
    if (!VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    g_loadSync = reinterpret_cast<LoadSyncFn>(RipTarget(call, 1, 5));
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) return false;
    g_loopCall = call;
    return true;
}

// TryLoad by its pattern (nullptr unless exactly one match, at the known place).
uint8_t* MenuLoadKeepFindTryLoad(const GameModule& game)
{
    const auto hits = FindAll(game.text, ParsePattern(kTryLoadPattern));
    if (hits.size() != 1 || hits[0] != game.base + kTryLoadRva) return nullptr;
    return MatchesAt(hits[0], ParsePattern(kTryLoadPrologue)) ? hits[0] : nullptr;
}

bool InstallMenuLoadKeep(bool beforeGameCode)
{
    if (!beforeGameCode) {
        Log("[menuloadkeep] MenuLoadKeep needs the start-up install: not installed");
        return false;
    }
    const GameModule& game = Game();
    if (game.timeDateStamp != kTestedStamp) {
        Log("[menuloadkeep] MenuLoadKeep: not the tested game build (time stamp %08X): not installed", game.timeDateStamp);
        return false;
    }
    if (!FindFreezeSubsystem()) {
        Log("[menuloadkeep] MenuLoadKeep: the freeze subsystem (the menu state) was not found: not installed");
        return false;
    }
    uint8_t* const fn = MenuLoadKeepFindTryLoad(game);
    g_array = FindObjectArrayFromDtor();
    if (!fn || !g_array) {
        Log("[menuloadkeep] MenuLoadKeep: %s not found: not installed", fn ? "GUObjectArray" : "FSoftObjectPath::TryLoad");
        return false;
    }
    uint8_t* const region = AllocNearGame(96);
    if (!region) return false;
    const Pattern p = ParsePattern(kTryLoadPrologue);
    if (!BuildSlot(fn, p, reinterpret_cast<void*>(&TryLoadHook), region)) return false;
    DWORD old;
    if (!VirtualProtect(region, 96, PAGE_EXECUTE_READ, &old)) {
        Log("[menuloadkeep] MenuLoadKeep: the detour slot could not be made executable: not installed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region, 96);
    g_orig = reinterpret_cast<TryLoadFn>(region);
    if (!Patch(fn, p.bytes.size(), region + 64)) {
        Log("[menuloadkeep] MenuLoadKeep: patch failed: not installed");
        return false;
    }
    Log("[menuloadkeep] FSoftObjectPath::TryLoad exe+0x%X detoured, GUObjectArray exe+0x%llX: MenuLoadKeep installed", kTryLoadRva,
        static_cast<unsigned long long>(g_array - game.base));
    if (InstallLoopMark(game))
        Log("[menuloadkeep] the menu input-context loop's LoadSynchronous call (exe+0x%X) marked: its input mapping contexts are kept",
            static_cast<unsigned>(g_loopCall - game.base));
    else
        Log("[menuloadkeep] the menu input-context loop's LoadSynchronous call not found: input mapping contexts are not kept "
            "(classes and textures as before)");
    return true;
}

void SetMenuLoadKeep(int mode, int capMB)
{
    if (mode > 1) mode = 1;
    if (capMB < 1) capMB = 1;
    if (capMB > 1024) capMB = 1024;
    g_capBytes.store(static_cast<uint64_t>(capMB) << 20, std::memory_order_relaxed);
    if (g_orig && g_mode.exchange(mode) != mode)
        Log("[menuloadkeep] %s", mode == 2 ? "MenuLoadKeep 2: observing the game thread's synchronous loads (nothing kept)"
                              : mode == 1 ? "MenuLoadKeep on: classes and textures a menu loads, and the menus' input mapping contexts, stay loaded across garbage collections"
                                          : "MenuLoadKeep off: the kept objects are released at the next load");
}

// ---- MenuLoadPreload (worker dlgedge, 2026-11-26; S-050): the first conversation's and the first persuasion's loads, made
// behind the session's first loading screen instead (and kept by MenuLoadKeep). Stock loads them synchronously on the game
// thread in the frame that opens the menu: WBP_ModernMenu_Dialog (4.5 ms) and the menus' ~29 input mapping contexts (9.3 ms
// cold) at the first dialogue open, WBP_ModernMenu_Persuasion (6.2 ms) and WBP_ModernMenu_QuestSkillPopup (2.4 ms, the
// tutorial popup over it) at the first persuasion (findings dlgdiag1126). With the key on, once per session, while a loading
// screen is up (freeze mode 1: the world is frozen and hidden) and from its 30th frame on, one item per frame: (1) the game's
// own input-context function exe+0x491ED70 (the same loop, through the loop mark: MenuLoadKeep keeps what it loads, kind 3);
// (2)-(4) FSoftObjectPath::TryLoad (the original, past our detour) of the three classes, kept with KeepObject (kind 1, the
// class check as for the game's own loads). A screen that ends first leaves the rest for the next loading screen. Same objects,
// loaded earlier and kept resident: what the game sees at the first open is what it sees in stock at any later open before a
// garbage collection (MenuLoadKeep's argument). [Fixes] MenuLoadPreload 0 = off, 1 = on (needs MenuLoadKeep 1); live.
namespace {
using FNameCtorFn = uint64_t* (*)(uint64_t* out, const wchar_t* name, int findType);
using ImcFn = void* (*)(void* outArray);  // TArray<UInputMappingContext*> by value (rcx = the return slot)
using FreeFn = void (*)(void* p);
std::atomic<int> g_preload{0};
FNameCtorFn g_fnameCtor = nullptr;
ImcFn g_imcFn = nullptr;
FreeFn g_free = nullptr;
constexpr int kPreItems = 4;     // the input contexts + the 3 classes
int g_preStep = 0;            // next item (0 .. kPreItems - 1); kPreItems = done
int g_preLoadingFrames = 0;   // frames of the current loading screen
bool g_preFindTried = false;
struct PreClass { const wchar_t* pkg; const wchar_t* asset; };
constexpr PreClass kPreClasses[3] = {
    {L"/Game/UI/Modern/GameMenuLayer/Dialog/WBP_ModernMenu_Dialog", L"WBP_ModernMenu_Dialog_C"},
    {L"/Game/UI/Modern/GameMenuLayer/Persuasion/WBP_ModernMenu_Persuasion", L"WBP_ModernMenu_Persuasion_C"},
    {L"/Game/UI/Modern/GameMenuLayer/GenericMenu/WBP_ModernMenu_QuestSkillPopup", L"WBP_ModernMenu_QuestSkillPopup_C"},
};
constexpr int kPreAfterFrames = 30;
int g_preTestFreeze = -2;               // self-test: the freeze mode to report (-2 = the game's; -1 = not readable)
void (*g_preTestItem)(int) = nullptr;   // self-test: receives the items instead of loading them
// the loop function's prologue (mov [rsp+18h],rbx; mov [rsp+20h],rdi; push r14; sub rsp,50h; mov rbx,rcx; call) and its free
constexpr char kImcFnStart[] = "48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 50 48 8B D9 E8";
constexpr char kImcFree[] = "48 8B 4C 24 ?? 48 85 C9 74 05 E8";

}  // namespace

// the input-context function (exe+0x491ED70) and the FMemory::Free its loop calls (exe+0xE26500), from the loop's pattern (also on
// the mapped exe, before or after the loop mark); false if not recognised
bool MenuLoadPreloadLocate(const GameModule& game, uint8_t** fn, uint8_t** freeFn)
{
    *fn = *freeFn = nullptr;
    const auto hits = FindAll(game.text, ParsePattern(kLoopBlock));
    if (hits.size() != 1) return false;
    uint8_t* const call = hits[0] + kLoopCallAt;
    uint8_t* const f = PrimaryFunction(game, call);
    if (!f || !MatchesAt(f, ParsePattern(kImcFnStart))) return false;
    const Pattern pf = ParsePattern(kImcFree);
    for (uint8_t* q = call; q < call + 0x80; ++q)
        if (MatchesAt(q, pf)) {
            uint8_t* const fr = RipTarget(q + 10, 1, 5);
            if (!game.text.Contains(fr)) return false;
            *fn = f;
            *freeFn = fr;
            return true;
        }
    return false;
}

namespace {
bool PreloadFind()
{
    g_preFindTried = true;
    g_fnameCtor = reinterpret_cast<FNameCtorFn>(FindFNameCtor());
    uint8_t *fn = nullptr, *fr = nullptr;
    if (g_loopCall && MenuLoadPreloadLocate(Game(), &fn, &fr)) {
        g_imcFn = reinterpret_cast<ImcFn>(fn);
        g_free = reinterpret_cast<FreeFn>(fr);
    }
    Log("[menuloadkeep] MenuLoadPreload: FName constructor %s, the input-context function %s", g_fnameCtor ? "found" : "NOT found",
        g_imcFn ? "found" : "not found (input contexts not preloaded)");
    return g_fnameCtor != nullptr;
}

// one item; returns false when it could not be done now
void PreloadItem(int step)
{
    const uint64_t t0 = NowUs();
    if (step == 0) {
        if (!g_imcFn) return;
        struct { void* data; int32_t num, max; } out{};
        g_imcFn(&out);
        const int n = out.num;
        if (out.data) g_free(out.data);
        Log("[menuloadkeep] MenuLoadPreload: the menus' input mapping contexts loaded behind the loading screen (%d, %.2f ms; %d kept)", n,
            (NowUs() - t0) / 1000.0, g_numData);
        return;
    }
    static_assert(kPreItems == 1 + static_cast<int>(sizeof kPreClasses / sizeof kPreClasses[0]), "items = input contexts + classes");
    if (step < 1 || step >= kPreItems) return;  // never past the class list (review of the p2 crash: step 4 read past it)
    const PreClass& c = kPreClasses[step - 1];
    struct { uint64_t pkg, asset; const wchar_t* sub; int32_t num, max; } path{};
    g_fnameCtor(&path.pkg, c.pkg, 1);  // FNAME_Add
    g_fnameCtor(&path.asset, c.asset, 1);
    void* const obj = (path.pkg && path.asset) ? g_orig(&path, nullptr) : nullptr;
    const char* res = "not loaded";
    if (obj) {
        int kind = 0;
        const void* const cls = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(obj) + kObjClassOff);
        if (ClassVerdict(cls, &kind) != 1 || kind != kKindClass) res = "loaded, not a kept class (left to the game)";
        else switch (KeepObject(obj, kEstClass)) {
            case kRooted: res = "kept"; break;
            case kAlready: res = "already kept"; break;
            case kCapped: res = "loaded, MenuLoadKeepMB cap reached (not kept)"; break;
            default: res = "loaded, not kept"; break;
        }
    }
    Log("[menuloadkeep] MenuLoadPreload: %ls behind the loading screen: %s (%.2f ms)", c.asset, res, (NowUs() - t0) / 1000.0);
}
}  // namespace

// game thread, once per frame (NpcCreationPacing's frame hook)
void MenuLoadKeepFrame()
{
    if (g_preStep >= kPreItems || !g_preload.load(std::memory_order_relaxed) || g_mode.load(std::memory_order_relaxed) != 1 || !g_orig) return;
    if (!g_preTestItem && !IsGt()) return;
    int32_t named = 0, mode = 0;
    const bool read = g_preTestFreeze == -2 ? ReadMenuFreeze(&named, &mode) : (mode = g_preTestFreeze, g_preTestFreeze >= 0);
    if (!read || mode != 1) { g_preLoadingFrames = 0; return; }
    if (++g_preLoadingFrames < kPreAfterFrames) return;
    if (!g_preFindTried && !PreloadFind()) { g_preStep = kPreItems; return; }
    if (!g_fnameCtor) { g_preStep = kPreItems; return; }
    if (!g_pool.load(std::memory_order_acquire) && g_preStep >= 1) return;  // class names not readable yet: wait (MenuLoadKeepTick)
    if (g_preTestItem) g_preTestItem(g_preStep++);
    else PreloadItem(g_preStep++);
    if (g_preStep >= kPreItems) Log("[menuloadkeep] MenuLoadPreload: done for this session");
}

// self-test (review S3-10): the step machine with a fake freeze mode and item sink; returns 0 when every check passes
namespace {
int g_preTestItems[16];
int g_preTestNum = 0;
void PreTestSink(int step) { if (g_preTestNum < 16) g_preTestItems[g_preTestNum++] = step; }
}  // namespace
int MenuLoadPreloadTestSteps()
{
    const TryLoadFn savedOrig = g_orig;
    const int savedMode = g_mode.load(), savedPre = g_preload.load();
    const uint8_t* const savedPool = g_pool.load();
    const FNameCtorFn savedCtor = g_fnameCtor;
    const bool savedTried = g_preFindTried;
    static uint8_t dummy[16];
    g_orig = reinterpret_cast<TryLoadFn>(&dummy[0]);  // only tested for non-null here
    g_mode.store(1);
    g_preload.store(1);
    g_fnameCtor = reinterpret_cast<FNameCtorFn>(&dummy[1]);
    g_preFindTried = true;
    g_preTestItem = &PreTestSink;
    int fail = 0;
    auto frames = [](int freeze, int n) { g_preTestFreeze = freeze; for (int i = 0; i < n; ++i) MenuLoadKeepFrame(); };
    auto reset = [] { g_preStep = 0; g_preLoadingFrames = 0; g_preTestNum = 0; };
    // 1: no loading screen (mode 0 / not readable): nothing; a screen: from its 30th frame one item per frame, 0..3, then done
    reset();
    g_pool.store(dummy);
    frames(0, 60);
    frames(-1, 5);
    if (g_preTestNum != 0) fail |= 1;
    frames(1, 29);
    if (g_preTestNum != 0) fail |= 2;
    frames(1, 1);
    if (g_preTestNum != 1 || g_preTestItems[0] != 0) fail |= 4;
    frames(1, 20);
    if (g_preTestNum != kPreItems || g_preTestItems[1] != 1 || g_preTestItems[2] != 2 || g_preTestItems[3] != 3 || g_preStep != kPreItems) fail |= 8;
    frames(0, 2);
    frames(1, 60);
    if (g_preTestNum != kPreItems) fail |= 16;  // once per session
    // 2: a screen that ends after item 0: the count starts again at the next screen, which does item 1 at its 30th frame
    reset();
    frames(1, 30);
    frames(0, 1);
    frames(1, 29);
    if (g_preTestNum != 1) fail |= 32;
    frames(1, 1);
    if (g_preTestNum != 2 || g_preTestItems[1] != 1) fail |= 64;
    // 3: FName pool not found yet: item 0 (no names needed) goes, the classes wait for it
    reset();
    g_pool.store(nullptr);
    frames(1, 40);
    if (g_preTestNum != 1 || g_preStep != 1) fail |= 128;
    g_pool.store(dummy);
    frames(1, 1);
    if (g_preTestNum != 2) fail |= 256;
    // 4: key off or MenuLoadKeep not in mode 1: nothing
    reset();
    g_preload.store(0);
    frames(1, 40);
    g_preload.store(1);
    g_mode.store(2);
    frames(1, 40);
    if (g_preTestNum != 0) fail |= 512;
    g_preTestItem = nullptr;
    g_preTestFreeze = -2;
    reset();
    g_orig = savedOrig;
    g_mode.store(savedMode);
    g_preload.store(savedPre);
    g_pool.store(savedPool);
    g_fnameCtor = savedCtor;
    g_preFindTried = savedTried;
    return fail;
}

void SetMenuLoadPreload(int on)
{
    const int v = on ? 1 : 0;
    if (g_preload.exchange(v) != v) Log("[menuloadkeep] MenuLoadPreload %d%s", v, v && !g_orig ? " (no effect: MenuLoadKeep not installed)" : "");
}

// Monitor thread, about once a second: find the FName pool (once) while the key is on.
void MenuLoadKeepTick()
{
    static int tries = 0;
    if (!g_orig || !g_mode.load(std::memory_order_relaxed) || g_poolSearched.load() || ++tries > 5) return;
    const uint8_t* const pool = FindNamePool();
    g_pool.store(pool, std::memory_order_release);
    if (pool) {
        g_poolSearched.store(true);
        Log("[menuloadkeep] FName pool at exe+0x%X", static_cast<unsigned>(pool - Game().base));
    } else Log("[menuloadkeep] FName pool not found (try %d of 5): no class is known, nothing is kept", tries);
}

void LogMenuLoadKeepStats()
{
    if (!g_orig || !g_s.loads) return;
    Log("[menuloadkeep] mode %d: %llu game-thread loads (%.1f ms), kept classes %llu (not in a menu %llu, class unknown %llu), rooted %llu "
        "(%llu of them cluster members), already rooted %llu, skipped %llu, capped %llu, unrooted %llu; kept now %d objects (%d of them the menus' input mapping contexts), ~%llu KB (estimated)",
        g_mode.load(), static_cast<unsigned long long>(g_s.loads), g_s.us / 1000.0, static_cast<unsigned long long>(g_s.wanted),
        static_cast<unsigned long long>(g_s.outOfMenu), static_cast<unsigned long long>(g_s.unknownClass),
        static_cast<unsigned long long>(g_s.rooted), static_cast<unsigned long long>(g_s.clusterMember),
        static_cast<unsigned long long>(g_s.already), static_cast<unsigned long long>(g_s.skipped), static_cast<unsigned long long>(g_s.capped),
        static_cast<unsigned long long>(g_s.unrooted), g_numKept, g_numData, static_cast<unsigned long long>(g_keptBytes >> 10));
}

// Self-test: the class names, the root / cluster / skip / cap / unroot logic on a fake GUObjectArray. 0 = all checks passed.
void* g_testInLoopSeen = nullptr;
void* FakeLoadSync(void* softPtr)
{
    g_testInLoopSeen = g_inLoop > 0 ? softPtr : nullptr;
    return softPtr;
}

int MenuLoadKeepTestLogic()
{
    int fail = 0;
    {   // the loop mark: set only while the redirected call runs, on the game thread
        const LoadSyncFn saved = g_loadSync;
        const DWORD savedGt = g_gt;
        g_loadSync = &FakeLoadSync;
        g_gt = GetCurrentThreadId();
        int token = 0;
        if (LoopLoadHook(&token) != &token || g_testInLoopSeen != &token || g_inLoop != 0) fail |= 1 << 21;
        g_gt = GetCurrentThreadId() + 1;  // another thread: no mark
        g_testInLoopSeen = &token;
        if (LoopLoadHook(&token) != &token || g_testInLoopSeen != nullptr || g_inLoop != 0) fail |= 1 << 22;
        g_gt = savedGt;
        g_loadSync = saved;
    }
    if (WantedKind("WidgetBlueprintGeneratedClass") != kKindClass) fail |= 1;
    if (WantedKind("VModdableBlueprintGeneratedClass") != kKindClass) fail |= 1;
    if (WantedKind("Texture2D") != kKindTexture) fail |= 1;
    if (WantedKind("InputMappingContext") != kKindData) fail |= 1;
    if (WantedKind("SoundWave") || WantedKind("StaticMesh") || WantedKind("Texture2D_0") || WantedKind("") || WantedKind("InputAction") ||
        WantedKind("InputMappingContext_0") || WantedKind("BlueprintGeneratedClass") || WantedKind("PlayerMappableInputConfig"))
        fail |= 1;

    static Item items[16];
    static Item* chunks[1] = {items};
    static uint8_t fakeArray[0x40];
    static uint8_t objs[16][0x30];
    memset(items, 0, sizeof items);
    memset(fakeArray, 0, sizeof fakeArray);
    Item** pc = chunks;
    memcpy(fakeArray + kArrayChunks, &pc, 8);
    const int32_t num = 16;
    memcpy(fakeArray + kArrayNum, &num, 4);
    uint8_t* const savedArray = g_array;
    const int savedKept = g_numKept, savedData = g_numData;
    const uint64_t savedBytes = g_keptBytes, savedCap = g_capBytes.load();
    const bool savedCapLogged = g_capLogged;
    g_array = fakeArray;
    g_numKept = 0;
    g_numData = 0;
    g_keptBytes = 0;
    g_capBytes.store(10 * 64 * 1024);
    for (int32_t i = 0; i < 16; ++i) {
        items[i].object = objs[i];
        items[i].serialNumber = 100 + i;
        memcpy(objs[i] + kObjIndexOff, &i, 4);
        const void* const cls = &objs[15];
        memcpy(objs[i] + kObjClassOff, &cls, 8);
    }
    items[2].flags = kRootSet;                         // the game's root: left alone
    items[3].flags = kGarbage;                         // garbage: skipped
    items[4].flags = kUnreachable;                     // unreachable: skipped
    items[5].flags = kAsyncLoading;                    // still loading: skipped
    items[6].object = objs[7];                         // the index holds another object: skipped
    items[8].flags = kClusterRoot;                     // a cluster root: rooted itself
    items[8].clusterRootIndex = -1;
    items[9].flags = 0;                                // a cluster member, not flagged this GC: rooted itself
    items[9].clusterRootIndex = 8;
    const KeepResult want[7] = {kRooted, kRooted, kAlready, kSkipped, kSkipped, kSkipped, kSkipped};
    for (int i = 0; i < 7; ++i)
        if (KeepObject(objs[i], kEstClass) != want[i]) fail |= 4 << i;
    if (KeepObject(objs[0], kEstClass) != kAlready) fail |= 2;  // a second time: already rooted, not added twice
    if (!(items[0].flags & kRootSet) || !(items[1].flags & kRootSet) || g_numKept != 2) fail |= 2;
    if (items[3].flags != kGarbage || items[4].flags != kUnreachable || items[5].flags != kAsyncLoading) fail |= 2;
    if (KeepObject(objs[9], kEstClass) != kRooted || !(items[9].flags & kRootSet) || g_numKept != 3) fail |= 1 << 12;
    if (KeepObject(objs[8], kEstClass) != kRooted || !(items[8].flags & kRootSet) || g_numKept != 4) fail |= 1 << 13;
    // cap 640 KB; 4 kept so far (256 KB): the first 256 KB object fits (512 KB), the next two do not (768 KB)
    int capped = 0;
    for (int32_t i = 10; i < 13; ++i)
        if (KeepObject(objs[i], 4 * kEstClass) == kCapped) ++capped;
    if (capped != 2 || g_keptBytes != 256 * 1024 + 256 * 1024) fail |= 1 << 15;
    // input mapping contexts (kind 3): rooted although the byte cap is full, counted in kMaxData, unrooted with the rest
    {
        const int dataBefore = g_numData;
        const uint64_t cap = g_capBytes.load();
        g_capBytes.store(0);  // nothing more fits the byte cap ...
        if (KeepObject(objs[13], kEstData) != kCapped) fail |= 1 << 17;                                    // ... a class: capped
        if (KeepObject(objs[13], kEstData, true) != kRooted || g_numData != dataBefore + 1) fail |= 1 << 18;  // ... data: kept
        if (KeepObject(objs[13], kEstData, true) != kAlready || g_numData != dataBefore + 1) fail |= 1 << 19;
        g_capBytes.store(cap);
        const int saved = g_numData;
        g_numData = kMaxData;  // the count cap
        if (KeepObject(objs[14], kEstData, true) != kCapped || (items[14].flags & kRootSet)) fail |= 1 << 20;
        g_numData = saved;
    }
    items[1].object = objs[7];  // slot reused by another object before the unroot: left alone
    items[1].flags |= 0x100;
    items[9].serialNumber = 999;  // the slot was freed and reused (new serial number): left alone
    {
        const void* const other = &objs[14];
        memcpy(objs[10] + kObjClassOff, &other, 8);  // a kept object whose class changed: left alone
    }
    Unroot();
    if (items[0].flags != 0 || items[2].flags != kRootSet || !(items[1].flags & kRootSet) || !(items[9].flags & kRootSet) || !(items[10].flags & kRootSet) ||
        (items[8].flags & kRootSet) || (items[13].flags & kRootSet) || g_numKept != 0 || g_numData != 0 || g_keptBytes != 0) fail |= 1 << 16;
    g_array = savedArray;
    g_numKept = savedKept;
    g_numData = savedData;
    g_keptBytes = savedBytes;
    g_capBytes.store(savedCap);
    g_capLogged = savedCapLogged;
    g_s = {};
    return fail;
}

}  // namespace opf
