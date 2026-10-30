// ObjectReplaceCrashGuard: crash guard for UE's "replace an existing object in place" (game bug, also without the mod).
//
// The crash: "LowLevelFatalError UObjectArray.cpp:234 Attempting to add CombatMovement_IDLE at index 249606 but another
// object (0x...) exists at that index" (a player's crash on 2026-10-21; ProcedureExecution_WANDER in bench run vc-f8h on
// 2026-10-12). The game's AI creates its per-controller helper objects with a fixed name: the combat-movement setter
// (build 19115871: exe+0x47E5690) ends the current movement object, drops it (no destroy) and calls
// NewObject(controller, "CombatMovement_<mode>"). Switching back to a mode before the next garbage collection finds the
// old, unreferenced object under that name, and StaticAllocateObject (exe+0x111F2D0) replaces it in place: BeginDestroy,
// FinishDestroy, then under GUObjectArray's lock Obj->~UObject() with index recycling off, which frees the object's slot
// in GUObjectArray (~UObjectBase -> FreeUObjectIndex), then the same memory is constructed again and AllocateUObjectIndex
// re-adds it at the old index. The fatal check fires when the slot still holds the object at that point: in both crash
// dumps the slot held exactly the state after FinishDestroy (serial reset, flags 0) with the free never done. ~UObjectBase
// skips the free when the object's class pointer reads null (or GIsCriticalError is set, which was ruled out); why the
// object's header changed in that window is not known (docs/research.md, ObjectReplaceCrashGuard).
//
// The guard takes over that one destructor call (the 8 bytes "mov rcx,rdi; mov rax,[rdi]; call [rax]" become
// "mov rcx,rdi; call" to a function that makes the same call) and afterwards checks the slot the object will be re-added at. When the game freed it,
// nothing else happens: every in-place re-creation runs exactly as before. Only when the slot still holds the destroyed
// object, i.e. when the game is about to stop with the fatal error, the guard does what FreeUObjectIndex does with
// recycling off (object, flags, cluster root and serial number of that slot set to zero; the index is not put on the free
// list) and logs the object's header before and after the destructor, so the next occurrence also tells why. The game
// then re-adds the new object at that index as it always does. The lock is held by the game around this call.
//
// Mode (start-up install, then live): 0 off (not installed), 1 on; lab: 2 = installed, log only (the game still stops).
// Lab build: each time the guard finds a kept slot (modes 1 and 2) it also writes a small minidump
// (%TEMP%\OblivionPerfFix-crash\OblivionPerfFix-guard-<time>.dmp; stacks, the memory they point to, the object, its slot,
// class and outer), at most 3 per game; the public build only logs.
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace opf {
namespace {

// StaticAllocateObject's replace-in-place branch, the destroy under the lock (build 19115871: exe+0x111F8E5):
//   48 8D 0D [cs]        lea rcx, [GUObjectArray + 0x30]        (ObjObjectsCritical)
//   FF 15 ?? ?? ?? ??    call [EnterCriticalSection]
//   0F B6 1D [rc]        movzx ebx, byte [GUObjectArray + 0xB4] (bShouldRecycleObjectIndices, saved)
//   33 D2                xor edx, edx
//   40 88 35 [rc]        mov [GUObjectArray + 0xB4], sil        (false)
//   48 8B CF             mov rcx, rdi                           <- patched: mov rcx,rdi; call jump (8 bytes)
//   48 8B 07             mov rax, [rdi]
//   FF 10                call [rax]                             (Obj->~UObject(): vtable slot 0, flags 0)
//   48 8D 0D [cs]        lea rcx, [GUObjectArray + 0x30]
//   FF 15 ?? ?? ?? ??    call [LeaveCriticalSection]
constexpr const char* kSite =
    "48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 0F B6 1D ?? ?? ?? ?? 33 D2 40 88 35 ?? ?? ?? ?? "
    "48 8B CF 48 8B 07 FF 10 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ??";
constexpr size_t kSiteCs1 = 0, kSiteRecycle1 = 13, kSiteRecycle2 = 22, kSiteCall = 29, kSiteCs2 = 37;
// The same branch, earlier: OldIndex = Obj->InternalIndex stored to the frame (exe+0x111F7FF):
//   8B 4F 0C             mov ecx, [rdi+0Ch]                     (InternalIndex)
//   3B 0D [n]            cmp ecx, [GUObjectArray + 0x24]        (NumElements)
//   89 4C 24 [off]       mov [rsp+off], ecx                     (OldIndex)
// and the re-add: AddObject(..., OldIndex, ...) reads it back (exe+0x111F7BD):
//   44 8B 4C 24 [off]    mov r9d, [rsp+off]
//   0F BA EB 09          bts ebx, 9                             (RF_NeedInitialization)
constexpr const char* kOldIndex = "8B 4F 0C 3B 0D ?? ?? ?? ?? 89 4C 24 ??";
constexpr const char* kReAdd = "44 8B 4C 24 ?? 0F BA EB 09";
constexpr size_t kSearchBack = 0x200;
// ~UObjectBase (exe+0x110BEE0): frees the slot only if GObjInitialized && ClassPrivate (+0x10) && !GIsCriticalError.
//   48 8D 05 [vt]        lea rax, [UObjectBase vtable]
//   48 89 01             mov [rcx], rax
//   80 3D [init] 00      cmp byte [GObjInitialized], 0
//   74 ??                je  done
//   48 83 79 10 00       cmp qword [rcx+10h], 0                  (ClassPrivate)
//   74 ??                je  done
//   80 3D [crit] 00      cmp byte [GIsCriticalError], 0
//   75 ??                jne done
//   48 8B D1             mov rdx, rcx
//   48 8D 0D [array]     lea rcx, [GUObjectArray]
//   E9                   jmp FreeUObjectIndex
constexpr const char* kDtor =
    "48 8D 05 ?? ?? ?? ?? 48 89 01 80 3D ?? ?? ?? ?? 00 74 ?? 48 83 79 10 00 74 ?? 80 3D ?? ?? ?? ?? 00 75 ?? "
    "48 8B D1 48 8D 0D ?? ?? ?? ?? E9";
constexpr size_t kDtorVtable = 0, kDtorInit = 10, kDtorCrit = 26, kDtorArray = 38;

// GUObjectArray (FUObjectArray, UE 5.3)
constexpr size_t kArrayChunks = 0x10;    // FUObjectItem** (64K items per chunk)
constexpr size_t kArrayNum = 0x24;       // NumElements
constexpr size_t kArrayCritical = 0x30;  // ObjObjectsCritical
constexpr size_t kArrayRecycle = 0xB4;   // bShouldRecycleObjectIndices
struct Item {                            // FUObjectItem, 24 bytes
    void* object;
    int32_t flags;
    int32_t clusterRootIndex;
    int32_t serialNumber;
    int32_t pad;
};
static_assert(sizeof(Item) == 24);
// UObjectBase: vtable +0, ObjectFlags +8, InternalIndex +0xC, ClassPrivate +0x10, NamePrivate +0x18, OuterPrivate +0x20
struct Header {
    void* vtable;
    uint32_t flags;
    int32_t index;
    void* cls;
    uint64_t name;
    void* outer;
};

uint8_t* g_site = nullptr;
uint8_t* g_stub = nullptr;
uint8_t* g_array = nullptr;
const uint8_t* g_baseVtable = nullptr;
const uint8_t* g_objInit = nullptr;
const uint8_t* g_critical = nullptr;
int32_t g_oldIndexOffset = 0;
std::atomic<int> g_mode{1};
std::atomic<uint64_t> g_replaced{0}, g_kept{0}, g_released{0};
uint64_t g_replacedLogged = 0, g_keptLogged = 0;
std::atomic<int> g_detailLines{0};

Header ReadHeader(const uint8_t* obj)
{
    Header h;
    memcpy(&h.vtable, obj, 8);
    memcpy(&h.flags, obj + 8, 4);
    memcpy(&h.index, obj + 0xC, 4);
    memcpy(&h.cls, obj + 0x10, 8);
    memcpy(&h.name, obj + 0x18, 8);
    memcpy(&h.outer, obj + 0x20, 8);
    return h;
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

struct Addr {
    char text[32];
};
Addr Where(const void* p)  // "exe+0x..." inside the game's image, else the plain address
{
    const GameModule& game = Game();
    const auto* b = static_cast<const uint8_t*>(p);
    Addr a;
    if (game.base && b >= game.base && b < game.base + game.sizeOfImage)
        snprintf(a.text, sizeof a.text, "exe+0x%llX", static_cast<unsigned long long>(b - game.base));
    else
        snprintf(a.text, sizeof a.text, "%p", p);
    return a;
}


// Called by the game in place of its Obj->~UObject() (the patch: mov rcx,rdi; call jump), with GUObjectArray's lock held and
// recycling off. The old index is read from the caller's frame, [its rsp + off]; the jump in between does not touch the stack,
// so this is a normal C++ frame directly under StaticAllocateObject's (stack walks and exceptions unwind as before).
__declspec(noinline) void __cdecl ReplaceDestroy(uint8_t* obj)
{
    const auto* const callerRsp = static_cast<const uint8_t*>(_AddressOfReturnAddress()) + 8;
    int32_t oldIndex;
    memcpy(&oldIndex, callerRsp + g_oldIndexOffset, 4);
    g_replaced.fetch_add(1, std::memory_order_relaxed);
    const Header before = ReadHeader(obj);
    bool forced = false;
    using Dtor = void(__cdecl*)(void*, uint32_t);
    Dtor dtor;
    memcpy(&dtor, before.vtable, 8);
    dtor(obj, 0);  // what the game does here: Obj->~UObject()
    const Header after = ReadHeader(obj);
    if (forced) memcpy(obj + 0x10, &before.cls, 8);
    Item* const item = ItemAt(oldIndex);
    if (!item || item->object != obj) return;  // the slot was freed: the stock outcome
    const uint64_t n = g_kept.fetch_add(1) + 1;
    const int mode = g_mode.load(std::memory_order_relaxed);
    const Item seen = *item;
    if (mode == 1) {
        // FreeUObjectIndex with recycling off: the index is not put on the free list (the game re-adds the object there)
        item->object = nullptr;
        item->flags = 0;
        item->clusterRootIndex = 0;
        item->serialNumber = 0;
        g_released.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_detailLines.fetch_add(1) < 20)
        Log("[replace] object %p (index %d) kept its GUObjectArray slot after its destruction during an in-place re-creation "
            "(#%llu; the game stops here with UObjectArray.cpp:234): %s. Before the destructor: vtable %s, flags 0x%X, "
            "index %d, class %p, name %08llX, outer %p; after: vtable %s, class %p, index %d; slot flags 0x%X, serial %d; "
            "GIsCriticalError %d, objects initialised %d; thread %lu%s",
            obj, oldIndex, static_cast<unsigned long long>(n), mode == 1 ? "slot released by the guard" : "left as it is (log only)",
            Where(before.vtable).text, before.flags, before.index, before.cls, static_cast<unsigned long long>(before.name),
            before.outer, Where(after.vtable).text, after.cls, after.index, static_cast<unsigned>(seen.flags), seen.serialNumber,
            g_critical ? *g_critical : -1, g_objInit ? *g_objInit : -1, GetCurrentThreadId(),
            forced ? " (lab test: class pointer cleared before the destructor)" : "");
}

bool Fail(const char* why)
{
    Log("[replace] %s: crash guard not installed", why);
    return false;
}

const uint8_t* FindNear(const uint8_t* site, const char* text)
{
    const GameModule& game = Game();
    const Pattern p = ParsePattern(text);
    for (const uint8_t* a = site - p.size(); a + kSearchBack >= site && a >= game.text.begin; --a)
        if (MatchesAt(a, p)) return a;
    return nullptr;
}

}  // namespace

bool ApplyObjectReplaceGuard()
{
    if (g_site) {
        Log("[replace] crash guard already installed");
        return true;
    }
    const GameModule& game = Game();
    const auto sites = FindAll(game.text, ParsePattern(kSite));
    if (sites.size() != 1) {
        Log("[replace] StaticAllocateObject's replace-in-place destroy not found (%zu matches): crash guard not installed", sites.size());
        return false;
    }
    uint8_t* const at = sites[0];
    uint8_t* const cs = RipTarget(at + kSiteCs1, 3, 7);
    uint8_t* const array = cs - kArrayCritical;
    if (RipTarget(at + kSiteCs2, 3, 7) != cs || RipTarget(at + kSiteRecycle1, 3, 7) != array + kArrayRecycle ||
        RipTarget(at + kSiteRecycle2, 3, 7) != array + kArrayRecycle)
        return Fail("the destroy's lock and recycling switch are not GUObjectArray's as in build 19115871");
    // OldIndex: stored from InternalIndex after a NumElements check, read back for AddObject at the same frame offset
    const uint8_t* const store = FindNear(at, kOldIndex);
    const uint8_t* const reAdd = FindNear(at, kReAdd);
    if (!store || !reAdd || RipTarget(store + 3, 2, 6) != array + kArrayNum || store[12] != reAdd[4] || store[12] >= 0x80)
        return Fail("the old index's place in StaticAllocateObject's frame differs from build 19115871");
    const auto dtors = FindAll(game.text, ParsePattern(kDtor));
    if (dtors.size() != 1 || RipTarget(dtors[0] + kDtorArray, 3, 7) != array)
        return Fail("~UObjectBase differs from build 19115871");
    uint8_t* const stub = AllocNearGame(64);
    if (!stub) return Fail("no memory near the game's code");
    // jump: jmp [rip+0]; dq ReplaceDestroy (no stack change: ReplaceDestroy returns straight to the game)
    memset(stub, 0xCC, 64);
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmp, sizeof jmp);
    void* const fn = reinterpret_cast<void*>(&ReplaceDestroy);
    memcpy(stub + 6, &fn, 8);
    DWORD old;
    if (!VirtualProtect(stub, 64, PAGE_EXECUTE_READ, &old)) return Fail("could not make the jump executable");
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    uint8_t* const call = at + kSiteCall;
    const int64_t rel = stub - (call + 8);
    if (rel != static_cast<int32_t>(rel)) return Fail("the jump is out of reach");
    uint8_t patch[8] = {0x48, 0x8B, 0xCF, 0xE8, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 4, &rel32, 4);
    g_array = array;
    g_oldIndexOffset = store[12];
    g_baseVtable = RipTarget(dtors[0] + kDtorVtable, 3, 7);
    g_objInit = RipTarget(dtors[0] + kDtorInit, 2, 7);
    g_critical = RipTarget(dtors[0] + kDtorCrit, 2, 7);
    g_stub = stub;
    if (!WriteCode(call, patch)) {
        g_stub = nullptr;
        return Fail("could not patch the destroy call");
    }
    g_site = at;
    Log("[replace] StaticAllocateObject's in-place destroy exe+0x%llX (GUObjectArray exe+0x%llX, old index at [rsp+0x%X]): crash guard installed",
        static_cast<unsigned long long>(call - game.base), static_cast<unsigned long long>(array - game.base), g_oldIndexOffset);
    return true;
}

void SetObjectReplaceGuardMode(int mode)
{
    if (mode == 2) mode = 1;
    g_mode = mode;
}

void LogObjectReplaceGuardStats()
{
    if (!g_site) return;
    const uint64_t n = g_replaced.load(), k = g_kept.load();
    if (n == g_replacedLogged && k == g_keptLogged) return;
    Log("[replace] in-place object re-creations: %llu so far (%llu since last report); slot kept after the destruction: %llu "
        "(released by the guard %llu)"
        ,
        static_cast<unsigned long long>(n), static_cast<unsigned long long>(n - g_replacedLogged), static_cast<unsigned long long>(k),
        static_cast<unsigned long long>(g_released.load())
    );
    g_replacedLogged = n;
    g_keptLogged = k;
}

uint8_t* FindObjectArrayFromDtor()
{
    // ~UObjectBase's lea rcx,[GUObjectArray] before its jump to FreeUObjectIndex (exactly one match, as the guard requires)
    const auto dtors = FindAll(Game().text, ParsePattern(kDtor));
    return dtors.size() == 1 ? RipTarget(dtors[0] + kDtorArray, 3, 7) : nullptr;
}

bool ObjectReplaceGuardInfo(ObjectReplaceGuardInfoOut* out)
{
    if (!g_site) return false;
    out->site = g_site;
    out->call = g_site + kSiteCall;
    out->stub = g_stub;
    out->objectArray = g_array;
    out->oldIndexOffset = g_oldIndexOffset;
    out->baseVtable = g_baseVtable;
    out->objInitialized = g_objInit;
    out->criticalError = g_critical;
    out->replaced = g_replaced.load();
    out->kept = g_kept.load();
    out->released = g_released.load();
    return true;
}

bool ObjectReplaceGuardTestPatched()
{
    if (!g_site) return false;
    const uint8_t* const c = g_site + kSiteCall;
    return c[0] == 0x48 && c[1] == 0x8B && c[2] == 0xCF && c[3] == 0xE8 && RipTarget(c + 3, 1, 5) == g_stub;
}

void ObjectReplaceGuardTestArray(uint8_t* fakeArray)
{
    g_array = fakeArray;
    g_detailLines = 0;
}



}  // namespace opf
