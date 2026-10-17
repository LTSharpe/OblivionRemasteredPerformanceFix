// TrapCrashGuard: crash in the old engine's trap listener (OblivionThread, a freed actor).
//
// Traps (falling rocks, swinging logs, spike plates) work through one TESTrapListener per Havok world. When a trap's
// collision touches an actor, the listener records the actor's reference in the trap's list (a raw pointer); after
// every Havok step its update (bhkWorld::Update -> listener vtable, build 19115871: exe+0x66D67E0) calls the actor's
// trap-hit function (Actor vtable +0x250, exe+0x65A8B90) for each recorded actor, which applies the trap's damage and
// push-back. An actor leaves the list when the contact ends, when its character controller leaves the Havok world,
// or when the actor is destroyed - and that last clean-up (exe+0x657AF30, called by the reference destructors) finds
// the listener through the actor's current parent cell only. The 2026-10-12 crash (Kvatch Oblivion gate, the bridge
// whose rocks fall: a clannfear hit by the rocks, thrown far by their push-back, then destroyed) is the case where
// that clean-up misses: the next Havok step called the trap-hit function through the freed actor's memory, whose
// first bytes were then Unreal's free-list header (0x00000001E3140380: an 896-byte block, canary E3), and the game
// crashed on the OblivionThread at exe+0x66D683E.
//
// The fix wraps the update (one vtable slot, found by its code and checked through RTTI to be TESTrapListener's).
// Before the game's update runs, each recorded actor is checked: its memory must be readable, its first qword a
// vtable in .rdata, and that vtable's class must derive from Actor (RTTI: Actor in its base-class list; the answer
// is kept per vtable). An entry that fails is removed with the game's own removal function (exe+0x66D6560, what the
// game does when a contact ends; it also drops the trap's node when that was its last actor), and the update then
// runs as before. Entries of live actors are never touched, so traps hit, hurt and push exactly as in the game.
// What it cannot see: the freed block already reused by another actor before the next physics step; the stock game
// would call the trap on that actor too.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

namespace opf {
namespace {

// TESTrapListener's update (build 19115871: exe+0x66D67E0, vtable exe+0x8633E80 slot 5):
//   48 89 5C 24 08 57 48 83 EC 30   prologue
//   48 8B 59 [phantoms]             mov rbx,[rcx+30h]   trigger phantoms (updated first, not touched here)
//   48 8B F9 0F 29 74 24 20 0F 28 F1 48 85 DB 74 ?? ?? ??
//   0F 28 CE 48 8B CB E8 ????????   movaps xmm1,xmm6; mov rcx,rbx; call phantom update
//   48 8B 5B ?? 48 85 DB 75 ??      next phantom
//   48 8B 7F [nodes]                mov rdi,[rdi+28h]   trap nodes
//   48 85 FF 74 ?? ?? ?? ??
//   4C 8B 47 [actors]               mov r8,[rdi+20h]    the node's actor entries
//   4D 85 C0 74 ?? (7-byte nop)
//   49 8B 48 [target]               mov rcx,[r8+18h]    the actor
//   48 8B D7 49 8B 58 [next]        mov rdx,rdi; mov rbx,[r8+8]
//   48 8B 01 FF 90 [slot32]         mov rax,[rcx]; call [rax+250h]   (the crash: rax was not a vtable)
//   4C 8B C3 48 85 DB 75 ??
//   48 8B 7F [nodeNext]             mov rdi,[rdi+8]
//   48 85 FF 75
constexpr const char* kUpdate =
    "48 89 5C 24 08 57 48 83 EC 30 48 8B 59 ?? 48 8B F9 0F 29 74 24 20 0F 28 F1 48 85 DB 74 ?? ?? ?? "
    "0F 28 CE 48 8B CB E8 ?? ?? ?? ?? 48 8B 5B ?? 48 85 DB 75 ?? 48 8B 7F ?? 48 85 FF 74 ?? ?? ?? ?? "
    "4C 8B 47 ?? 4D 85 C0 74 ?? ?? ?? ?? ?? ?? ?? ?? 49 8B 48 ?? 48 8B D7 49 8B 58 ?? 48 8B 01 FF 90 ?? ?? ?? ?? "
    "4C 8B C3 48 85 DB 75 ?? 48 8B 7F ?? 48 85 FF 75";
constexpr size_t kUpdateNodes = 55, kUpdateActors = 67, kUpdateTarget = 83, kUpdateNext = 90, kUpdateSlot = 96,
                 kUpdateNodeNext = 111;

// The game's removal of one actor from a trap node (build 19115871: exe+0x66D6560; (listener, node, actor)): unlinks
// the first entry whose actor matches, frees it, and removes the node from the listener when its list is empty.
//   48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9
//   4C 8B 4A [actors] 49 8B C9 4D 85 C9 74 ?? 0F 1F 40 00
//   4C 39 41 [target] 74 ?? 48 8B 49 [next] 48 85 C9 75 ?? EB ??   find the entry
//   ... unlink ... 4C 89 4B [actors] 0F B6 41 FF 48 2B C8 E8 (free) 90 48 83 7B [actors] 00 75 ?? 48 8B D3 48 8B CF E8 (remove node)
constexpr const char* kRemove =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 4C 8B 4A ?? 49 8B C9 4D 85 C9 74 ?? 0F 1F 40 00 "
    "4C 39 41 ?? 74 ?? 48 8B 49 ?? 48 85 C9 75 ?? EB ?? 49 3B C9 75 ?? 4C 8B 49 ?? EB ?? 49 8B C1 90 "
    "48 8D 50 ?? 48 8B 02 48 3B C8 74 ?? 48 85 C0 75 ?? EB ?? 48 8B 40 ?? 48 89 02 4C 89 4B ?? "
    "0F B6 41 FF 48 2B C8 E8 ?? ?? ?? ?? 90 48 83 7B ?? 00 75 ?? 48 8B D3 48 8B CF E8";
constexpr size_t kRemoveActors = 19, kRemoveTarget = 35, kRemoveNext = 41, kRemoveStore = 93, kRemoveEmpty = 110;

using UpdateFn = void (*)(void* listener, float dt);
using RemoveFn = void (*)(void* listener, void* node, void* actor);

struct Layout {
    uint32_t nodes = 0, actors = 0, target = 0, next = 0, nodeNext = 0, slot = 0;
};

Layout g_layout;
void** g_slot = nullptr;         // the listener vtable's update slot (now the guard)
uint8_t* g_update = nullptr;     // the game's update
uint8_t* g_remove = nullptr;     // the game's removal of one actor
const uint8_t* g_actorType = nullptr;  // RTTI type descriptor of Actor
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_firstSeen{false};  // the first call is logged (shows the guard is in the physics step)
std::atomic<uint64_t> g_checked{0}, g_removed{0};
uint64_t g_removedLogged = 0;

// Per-vtable answers of IsActorVtable (the listener runs on the OblivionThread only; a racing reader at worst asks
// RTTI again). 0 = empty.
constexpr int kCache = 32;
std::atomic<uintptr_t> g_actorVtables[kCache];
std::atomic<uintptr_t> g_otherVtables[kCache];

uint32_t Disp8(const uint8_t* at) { return *at; }
uint32_t Disp32(const uint8_t* at) { return *reinterpret_cast<const uint32_t*>(at); }

// ---- RTTI (MSVC x64: every pointer is an image-relative 32-bit offset) ----
struct Col {        // _RTTICompleteObjectLocator, at vtable[-1]
    uint32_t signature, offset, cdOffset, typeDescriptor, hierarchy, self;
};
struct Hierarchy {  // _RTTIClassHierarchyDescriptor
    uint32_t signature, attributes, numBases, baseArray;
};

const Col* ColOf(const GameModule& game, const void* const* vtable)
{
    if (!game.rdata.Contains(vtable - 1) || !game.rdata.Contains(vtable)) return nullptr;
    const auto* col = static_cast<const Col*>(vtable[-1]);
    if (!game.rdata.Contains(col) || !game.rdata.Contains(reinterpret_cast<const uint8_t*>(col) + sizeof(Col) - 1)) return nullptr;
    if (col->signature != 1 || game.base + col->self != reinterpret_cast<const uint8_t*>(col)) return nullptr;
    return col;
}

// The class of this vtable (a complete object's vtable: COL offset 0) has `type` among its bases (or is it).
bool DerivesFrom(const GameModule& game, const void* const* vtable, const uint8_t* type)
{
    const Col* col = ColOf(game, vtable);
    if (!col || col->offset != 0) return false;
    const auto* h = reinterpret_cast<const Hierarchy*>(game.base + col->hierarchy);
    if (!game.rdata.Contains(h) || h->numBases == 0 || h->numBases > 256) return false;
    const auto* bases = reinterpret_cast<const uint32_t*>(game.base + h->baseArray);
    if (!game.rdata.Contains(bases) || !game.rdata.Contains(bases + h->numBases - 1)) return false;
    for (uint32_t i = 0; i < h->numBases; ++i) {
        const uint8_t* bcd = game.base + bases[i];  // _RTTIBaseClassDescriptor: type descriptor first
        if (!game.rdata.Contains(bcd)) return false;
        if (game.base + *reinterpret_cast<const uint32_t*>(bcd) == type) return true;
    }
    return false;
}

// The type descriptor whose name is `mangled` (".?AVActor@@"): name at +0x10, in .data.
const uint8_t* FindTypeDescriptor(const GameModule& game, const char* mangled)
{
    std::string pattern;
    for (const char* c = mangled;; ++c) {
        char hex[4];
        sprintf_s(hex, "%02X ", static_cast<unsigned char>(*c));
        pattern += hex;
        if (!*c) break;
    }
    const auto hits = FindAll(game.data, ParsePattern(pattern));
    if (hits.size() != 1 || hits[0] - 0x10 < game.data.begin) return nullptr;
    return hits[0] - 0x10;
}

bool IsActorVtable(uintptr_t vt)
{
    const GameModule& game = Game();
    const auto* vtable = reinterpret_cast<const void* const*>(vt);
    if (!game.rdata.Contains(vtable)) return false;  // freed memory, a heap pointer: not cached
    for (auto& c : g_actorVtables) {
        const uintptr_t v = c.load(std::memory_order_relaxed);
        if (v == vt) return true;
        if (!v) break;
    }
    for (auto& c : g_otherVtables) {
        const uintptr_t v = c.load(std::memory_order_relaxed);
        if (v == vt) return false;
        if (!v) break;
    }
    const bool actor = DerivesFrom(game, vtable, g_actorType) && game.rdata.Contains(vtable + g_layout.slot / sizeof(void*)) &&
                       game.text.Contains(vtable[g_layout.slot / sizeof(void*)]);
    auto& cache = actor ? g_actorVtables : g_otherVtables;
    for (auto& c : cache) {
        uintptr_t expected = 0;
        if (c.compare_exchange_strong(expected, vt) || expected == vt) break;
    }
    return actor;
}

// First qword of the recorded actor, or false if its memory cannot be read (given back to Windows).
bool ReadFirst(const void* p, uintptr_t* out)
{
    __try {
        *out = *static_cast<const volatile uintptr_t*>(p);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

uint32_t FormIdOf(const uint8_t* ref)
{
    uintptr_t q = 0;
    if (!ref || !ReadFirst(ref + 0x10, &q)) return 0;  // TESForm: form ID at +0x10 (the trap-hit function reads it there)
    return static_cast<uint32_t>(q);
}

void RemoveStale(uint8_t* listener)
{
    const Layout& L = g_layout;
    uint8_t* node = *reinterpret_cast<uint8_t**>(listener + L.nodes);
    while (node) {
        // The removal frees the node when its last actor goes: read the next node first.
        uint8_t* const nextNode = *reinterpret_cast<uint8_t**>(node + L.nodeNext);
        for (uint8_t* e = *reinterpret_cast<uint8_t**>(node + L.actors); e;) {
            uint8_t* const next = *reinterpret_cast<uint8_t**>(e + L.next);
            uint8_t* const actor = *reinterpret_cast<uint8_t**>(e + L.target);
            uintptr_t vt = 0;
            g_checked.fetch_add(1, std::memory_order_relaxed);
            if (!actor || !ReadFirst(actor, &vt) || !IsActorVtable(vt)) {
                // node+0x18 is the trap's own reference (the listener's add function stores it there).
                const uint32_t trap = FormIdOf(*reinterpret_cast<uint8_t**>(node + 0x18));
                const bool freedBlock = (vt >> 24 & 0xFF) == 0xE3 && vt >> 32 < 0x100;  // FMallocBinned2 free-block header
                Log("[trap] trap %08X: actor at %p no longer exists (first qword %016llX%s): removed from the trap's list "
                    "instead of crashing", trap, static_cast<void*>(actor), static_cast<unsigned long long>(vt),
                    freedBlock ? ", a freed memory block" : "");
                reinterpret_cast<RemoveFn>(g_remove)(listener, node, actor);
                g_removed.fetch_add(1, std::memory_order_relaxed);
            }
            e = next;
        }
        node = nextNode;
    }
}

void GuardedUpdate(void* listener, float dt)
{
    if (!g_firstSeen.load(std::memory_order_relaxed) && !g_firstSeen.exchange(true))
        Log("[trap] first trap listener update through the guard (listener %p, step %.4f s)", listener, dt);
    if (g_enabled.load(std::memory_order_relaxed)) RemoveStale(static_cast<uint8_t*>(listener));
    reinterpret_cast<UpdateFn>(g_update)(listener, dt);
}

}  // namespace

bool ApplyTrapTargetGuard()
{
    const GameModule& game = Game();
    const auto updates = FindAll(game.text, ParsePattern(kUpdate));
    // The removal is compiled next to the update (0x280 bytes before it in build 19115871): search 1 MB around it.
    std::vector<uint8_t*> removes;
    if (updates.size() == 1) {
        constexpr ptrdiff_t kNear = 0x100000;
        const Section around{std::max(game.text.begin, updates[0] - kNear), std::min(game.text.end, updates[0] + kNear)};
        removes = FindAll(around, ParsePattern(kRemove));
    }
    if (updates.size() != 1 || removes.size() != 1) {
        Log("[trap] trap listener code not found (update %zu, removal %zu matches): crash guard not installed", updates.size(),
            removes.size());
        return false;
    }
    uint8_t* const update = updates[0];
    uint8_t* const remove = removes[0];
    Layout L;
    L.nodes = Disp8(update + kUpdateNodes);
    L.actors = Disp8(update + kUpdateActors);
    L.target = Disp8(update + kUpdateTarget);
    L.next = Disp8(update + kUpdateNext);
    L.nodeNext = Disp8(update + kUpdateNodeNext);
    L.slot = Disp32(update + kUpdateSlot);
    // The removal must use the update's layout, and the trap's reference sits at +0x18 of a node (logging only).
    if (Disp8(remove + kRemoveActors) != L.actors || Disp8(remove + kRemoveStore) != L.actors ||
        Disp8(remove + kRemoveEmpty) != L.actors || Disp8(remove + kRemoveTarget) != L.target ||
        Disp8(remove + kRemoveNext) != L.next || L.slot % sizeof(void*) != 0 || L.slot > 0x2000) {
        Log("[trap] trap listener layout differs between update and removal: crash guard not installed");
        return false;
    }

    // The one .rdata slot holding the update, in TESTrapListener's vtable.
    void** slot = nullptr;
    int slots = 0;
    for (auto** p = reinterpret_cast<void**>(game.rdata.begin); reinterpret_cast<uint8_t*>(p + 1) <= game.rdata.end; ++p) {
        if (*p == update || *p == reinterpret_cast<void*>(&GuardedUpdate)) {
            slot = p;
            ++slots;
        }
    }
    const uint8_t* const listenerType = FindTypeDescriptor(game, ".?AVTESTrapListener@@");
    g_actorType = FindTypeDescriptor(game, ".?AVActor@@");
    bool named = false;
    if (slots == 1 && listenerType) {
        // Walk back to the vtable's start (the slot before it holds the complete object locator).
        for (int i = 1; i <= 64 && !named; ++i) {
            void* const* vt = slot - i + 1;
            const Col* col = ColOf(game, vt);
            if (col) named = col->offset == 0 && game.base + col->typeDescriptor == listenerType;
            if (col || !game.text.Contains(slot[-i])) break;  // a COL ends the walk; so does anything that is not code
        }
    }
    if (!named || !g_actorType) {
        Log("[trap] trap listener vtable not confirmed (%d slots, TESTrapListener RTTI %s, Actor RTTI %s): crash guard not installed",
            slots, listenerType ? "found" : "missing", g_actorType ? "found" : "missing");
        return false;
    }
    g_layout = L;
    g_update = update;
    g_remove = remove;
    g_slot = slot;
    if (*slot == reinterpret_cast<void*>(&GuardedUpdate)) {
        Log("[trap] crash guard already installed");
        return true;
    }
    if (!WritePointer(slot, reinterpret_cast<void*>(&GuardedUpdate))) {
        Log("[trap] could not write the vtable slot: crash guard not installed");
        g_slot = nullptr;
        return false;
    }
    Log("[trap] trap listener update exe+0x%llX (slot exe+0x%llX), removal exe+0x%llX; nodes +0x%X, actors +0x%X/+0x%X/+0x%X, "
        "trap-hit slot +0x%X: crash guard installed",
        static_cast<unsigned long long>(update - game.base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(slot) - game.base),
        static_cast<unsigned long long>(remove - game.base), L.nodes, L.actors, L.target, L.next, L.slot);
    return true;
}

void SetTrapTargetGuardEnabled(bool enabled) { g_enabled = enabled; }

void LogTrapTargetGuardStats()
{
    const uint64_t n = g_removed.load();
    if (!g_slot || n == g_removedLogged) return;
    Log("[trap] actors removed from trap lists after they no longer existed: %llu so far (%llu trap contacts checked)",
        static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_checked.load()));
    g_removedLogged = n;
}

bool TrapTargetGuardInfo(TrapGuardInfo* out)
{
    if (!g_slot) return false;
    out->update = g_update;
    out->remove = g_remove;
    out->slot = g_slot;
    out->hook = reinterpret_cast<void*>(&GuardedUpdate);
    out->actorType = g_actorType;
    out->nodes = g_layout.nodes;
    out->actors = g_layout.actors;
    out->target = g_layout.target;
    out->next = g_layout.next;
    out->nodeNext = g_layout.nodeNext;
    out->hitSlot = g_layout.slot;
    out->checked = g_checked.load();
    out->removed = g_removed.load();
    return true;
}

void TrapTargetGuardTestFunctions(void* fakeUpdate, void* fakeRemove)
{
    if (fakeUpdate) g_update = static_cast<uint8_t*>(fakeUpdate);
    if (fakeRemove) g_remove = static_cast<uint8_t*>(fakeRemove);
}

bool TrapTargetGuardTestIsActor(const void* object)
{
    uintptr_t vt = 0;
    return object && ReadFirst(object, &vt) && IsActorVtable(vt);
}

}  // namespace opf
