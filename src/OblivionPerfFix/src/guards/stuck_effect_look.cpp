// StuckEffectLookFix (game bug #48, "stuck effect look"; default off until it has been tested in play, then on as a game bug
// fix): a status effect's look (fire, frost, shock, drain, an enchanted hit...) stays on a character's weapons, quiver or
// on a dismounted rider after the effect has ended. Worker stuckfx2, 2026-11-20; cause from the full dump of a player's red
// quiver (stuckfx-dump, 2026-11-19; findings tags stuckfx, stuckfx2).
//
// The cause (stock blueprint /Game/Dev/PairingActors/BPC_StatusEffect, one component per character and per weapon):
//  - an overlay effect creates its dynamic material instances (OverlayMIDs) and sets one as the OverlayMaterial of every
//    mesh its collector "Get All Mesh Components" returns: the owner's own meshes and its attached actors (armour, the
//    shield, the weapons, the quiver), and with the apply flags also an attached pawn (a horse's rider) with everything
//    attached to it;
//  - its removal (RemoveStatusEffect) first empties OverlayMIDs, then runs the same collector with GetAttachedWeapons
//    false and sets OverlayMaterial to none on what it returns, and sets its form to none. Every weapon (sword, bow,
//    staff, quiver and arrows) keeps the last overlay effect's look until the weapon's actor is made again (re-equip,
//    load); a rider who got off the horse before the horse's effect ended keeps the horse's look everywhere.
//
// The fix. Every blueprint call of MeshComponent.SetOverlayMaterial reaches the engine's exec function (found by its
// registered name and its shape; an entry detour). A call made by a status-effect component (the calling frame's object
// is a BPC_StatusEffect_C) is remembered per mesh: which material instance, set by which component, and what the mesh
// showed before it (a short history; a change made by anything else resets it). Once per frame, on the game thread, a
// remembered mesh whose overlay is still a status-effect MID whose component shows no effect any more (form none, or
// the component gone) is a leftover: it gets the newest look under it whose component still shows it (the MID is in
// that component's OverlayMIDs) and would still reach the mesh (the component's actor is the mesh's own actor or one the
// mesh is attached under now: never the horse's look on a rider who has got off), else none. "None" is exactly what the stock removal does to the meshes it reaches, with
// the same two steps first for a static mesh (bDisallowNanite off, SetVisibleInRayTracing(true)). Nothing is done while
// the effect's component still has a form (its look legitimately showing, fading, or about to be replaced by its next
// effect), nothing to a mesh whose overlay was set by anything else, and nothing to calls of the game's own.
#include "common.h"

#include <atomic>
#include <cstring>
#include <cwchar>
#include <windows.h>

namespace opf {
namespace {

// MeshComponent.SetOverlayMaterial's exec function (build 19115871 exe+0x3054490): P_GET_OBJECT, P_FINISH, then the
// setter inlined: if (OverlayMaterial != New) { OverlayMaterial = New; PrecachePSOs() (virtual); MarkRenderStateDirty(); }
constexpr char kExecShape[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 33 F6 48 8B DA 48 89 74 24 30 48 8B F9 E8 ?? ?? ?? ?? 48 8B CB "
    "48 39 73 20 74 10 48 8B 53 18 4C 8D 44 24 30 E8 ?? ?? ?? ?? EB 1C 4C 8B 83 88 00 00 00 48 8D 54 24 30 49 8B 40 18 "
    "48 89 83 88 00 00 00 E8 ?? ?? ?? ?? 48 8B 43 20 48 85 C0 40 0F 95 C6 48 03 F0 48 8B 44 24 30 48 89 73 20 "
    "48 39 87 ?? ?? ?? ?? 74 1B 48 89 87 ?? ?? ?? ?? 48 8B CF 48 8B 07 FF 90 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ??";
constexpr size_t kExecCmpDisp = 112, kExecMovDisp = 121, kExecSlotDisp = 133, kExecDirtyCall = 140;
constexpr uint8_t kExecPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x10};  // mov [rsp+10h],rbx (whole; the next one starts at +5)
// PrimitiveComponent.SetVisibleInRayTracing's exec function (build 19115871 exe+0x2D89C10): one bool, then the setter call.
constexpr char kRtExecShape[] =
    "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 33 FF 48 8B DA 89 7C 24 38 48 8B F1 E8 ?? ?? ?? ?? 48 8B CB "
    "48 39 7B 20 74 10 48 8B 53 18 4C 8D 44 24 38 E8 ?? ?? ?? ?? EB 1C 4C 8B 83 88 00 00 00 48 8D 54 24 38 49 8B 40 18 "
    "48 89 83 88 00 00 00 E8 ?? ?? ?? ?? 48 8B 43 20 48 8B CE 48 85 C0 40 0F 95 C7 48 03 F8 83 7C 24 38 00 "
    "48 89 7B 20 0F 95 C2 E8 ?? ?? ?? ??";
constexpr size_t kRtExecCall = 114;

// UE 5.3 layouts (checked on the stuckfx dump): UObjectBase flags +0x08, InternalIndex +0x0C, ClassPrivate +0x10,
// NamePrivate +0x18, OuterPrivate +0x20; UStruct SuperStruct +0x40, ChildProperties +0x50; FField Next +0x18, NamePrivate
// +0x20; FProperty Offset_Internal +0x44; FBoolProperty ByteOffset +0x71, ByteMask +0x72; FFrame Object +0x18;
// GUObjectArray chunks +0x10, NumElements +0x24, FUObjectItem 24 bytes {Object, Flags, ClusterRootIndex, SerialNumber}.
constexpr size_t kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kStructSuper = 0x40, kStructProps = 0x50;
constexpr size_t kFieldNext = 0x18, kFieldName = 0x20, kPropOffset = 0x44, kBoolByteOffset = 0x71, kBoolByteMask = 0x72;
constexpr size_t kFrameObject = 0x18;
constexpr size_t kArrayChunks = 0x10, kArrayNum = 0x24, kItemSize = 24, kItemFlags = 8, kItemSerial = 16;
constexpr int32_t kItemDead = (1 << 21) | (1 << 28) | (1 << 29);  // EInternalObjectFlags Garbage, Unreachable, PendingKill

constexpr int kHist = 6;         // looks remembered per mesh (older ones: an unknown base, which gives none)
constexpr int kMaxMeshes = 1024;  // meshes remembered at once (more: not remembered, counted, the first one logged)
constexpr uint64_t kPeriodicFrames = 120;  // also a pass every ~2 s while meshes are remembered (a component that went away)

struct NameId {
    uint32_t index = 0, number = 0;
    bool operator==(const NameId& o) const { return index == o.index && number == o.number; }
    bool valid() const { return index != 0; }
};
using FNameFn = NameId* (*)(NameId* out, const wchar_t* name, int findType);
using ExecFn = void (*)(uint8_t* context, uint8_t* stack, void* result);
using VoidFn = void (*)(uint8_t* obj);
using RtFn = void (*)(uint8_t* comp, bool visible);

struct Ref {  // an object, its FUObjectItem (index, serial at the time it was seen) and its class
    uint8_t* p = nullptr;
    const uint8_t* cls = nullptr;
    int32_t index = -1, serial = 0;
};
struct Look {
    uint8_t* mid = nullptr;  // the material set (compared by address only; never read unless it is shown or listed)
    Ref by;                  // the status-effect component that set it; by.p == nullptr: not one of theirs (a base)
};
struct Mesh {
    Ref comp;
    bool isStatic = false;
    int n = 0;
    Look h[kHist];  // oldest first; h[n-1] is what the mesh shows
};

struct State {
    bool found = false, searched = false, installed = false;
    StuckEffectLookInfo info{};
    ExecFn original = nullptr;
    VoidFn markDirty = nullptr;
    RtFn rtSetter = nullptr;
    FNameFn fname = nullptr;
    uint8_t* objArray = nullptr;
    // resolved on the first call from a status-effect component
    NameId bpcName, staticName, midsName, formName, nanName, attachName;
    const uint8_t* bpcClass = nullptr;
    const uint8_t* staticClass = nullptr;
    uint32_t midsOffset = 0, formOffset = 0, nanOffset = 0;
    uint8_t nanMask = 0;
    bool resolved = false, resolveFailed = false;
    // SceneComponent.AttachParent, found on the first mesh remembered (0: not found, only the mesh's own actor counts)
    uint32_t attachOffset = 0;
    bool attachTried = false;
    // the remembered meshes (game thread only)
    Mesh meshes[kMaxMeshes];
    int count = 0;
    bool dirty = false;
    uint64_t frame = 0;
    bool inPass = false;
};
State g;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_seen{0}, g_cleared{0}, g_restored{0}, g_full{0}, g_faults{0};
std::atomic<DWORD> g_testGt{0};  // self-test: the thread that counts as the game thread (0: the game's own only)

bool Fail(const char* why)
{
    Log("[stuckfx] %s: StuckEffectLookFix not installed", why);
    return false;
}

template <class T> bool Rd(const void* p, T* v)
{
    if (!p) return false;
    __try {
        memcpy(v, p, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> bool Wr(void* p, const T& v)
{
    if (!p) return false;
    __try {
        memcpy(p, &v, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint8_t* Ptr(const uint8_t* p, size_t off)
{
    uint8_t* v = nullptr;
    return p && Rd(p + off, &v) ? v : nullptr;
}

// The FUObjectItem of an index: object, flags, serial (false = no such item)
bool ItemOf(int32_t index, uint8_t** obj, int32_t* flags, int32_t* serial)
{
    if (!g.objArray || index < 0) return false;
    int32_t num = 0;
    uint8_t* const* chunks = nullptr;
    if (!Rd(g.objArray + kArrayNum, &num) || !Rd(g.objArray + kArrayChunks, &chunks) || index >= num || !chunks) return false;
    uint8_t* chunk = nullptr;
    if (!Rd(chunks + (index >> 16), &chunk) || !chunk) return false;
    const uint8_t* item = chunk + kItemSize * (index & 0xFFFF);
    return Rd(item, obj) && Rd(item + kItemFlags, flags) && Rd(item + kItemSerial, serial);
}
Ref MakeRef(uint8_t* p)
{
    Ref r;
    int32_t index = -1, flags = 0, serial = 0;
    uint8_t* o = nullptr;
    if (!p || !Rd(p + kObjIndex, &index) || !ItemOf(index, &o, &flags, &serial) || o != p || (flags & kItemDead)) return r;
    r.p = p;
    r.cls = Ptr(p, kObjClass);
    r.index = index;
    r.serial = serial;
    return r;
}
// The object is still the one seen (same item, same serial if it had one, same class, not being destroyed)
bool Alive(const Ref& r)
{
    if (!r.p) return false;
    uint8_t* o = nullptr;
    int32_t flags = 0, serial = 0;
    return ItemOf(r.index, &o, &flags, &serial) && o == r.p && !(flags & kItemDead) && (!r.serial || serial == r.serial) &&
           Ptr(r.p, kObjClass) == r.cls;
}

NameId NameOf(const uint8_t* obj)
{
    NameId n;
    Rd(obj + kObjName, &n);
    return n;
}
NameId Lookup(const wchar_t* text)
{
    NameId n;
    if (g.fname) g.fname(&n, text, 0);  // FNAME_Find: none until the name exists
    return n;
}
// The class (or one of its parents) named `name`, or null
const uint8_t* ClassNamed(const uint8_t* cls, const NameId& name)
{
    for (int depth = 0; cls && depth < 24; ++depth) {
        if (NameOf(cls) == name) return cls;
        cls = Ptr(cls, kStructSuper);
    }
    return nullptr;
}
// A property of the struct (or a parent) by name: its FField, or null
const uint8_t* FieldNamed(const uint8_t* st, const NameId& name)
{
    for (int depth = 0; st && depth < 24; ++depth) {
        const uint8_t* f = Ptr(st, kStructProps);
        for (int k = 0; f && k < 1024; ++k) {
            NameId n;
            if (Rd(f + kFieldName, &n) && n == name) return f;
            f = Ptr(f, kFieldNext);
        }
        st = Ptr(st, kStructSuper);
    }
    return nullptr;
}
uint32_t OffsetOf(const uint8_t* field)
{
    int32_t off = -1;
    return field && Rd(field + kPropOffset, &off) && off > 0 && off < 0x4000 ? static_cast<uint32_t>(off) : 0;
}

// Is `obj` a status-effect component? Resolves the layout on the first one (game thread).
bool IsStatusEffect(const uint8_t* obj)
{
    if (!obj || g.resolveFailed) return false;
    const uint8_t* const cls = Ptr(obj, kObjClass);
    if (!cls) return false;
    if (g.resolved) return cls == g.bpcClass || (ClassNamed(cls, g.bpcName) == g.bpcClass && g.bpcClass);
    if (!g.bpcName.valid()) {
        g.bpcName = Lookup(L"BPC_StatusEffect_C");
        if (!g.bpcName.valid()) return false;  // the blueprint is not loaded yet: nothing of it can be calling
    }
    const uint8_t* const bpc = ClassNamed(cls, g.bpcName);
    if (!bpc) return false;
    g.midsName = Lookup(L"OverlayMIDs");
    g.formName = Lookup(L"form");
    g.attachName = Lookup(L"AttachParent");
    g.staticName = Lookup(L"StaticMeshComponent");
    g.nanName = Lookup(L"bDisallowNanite");
    g.midsOffset = OffsetOf(FieldNamed(bpc, g.midsName));
    g.formOffset = OffsetOf(FieldNamed(bpc, g.formName));
    if (!g.midsOffset || !g.formOffset || !g.staticName.valid() || !g.nanName.valid()) {
        g.resolveFailed = true;
        Log("[stuckfx] BPC_StatusEffect_C's OverlayMIDs / form (+0x%X / +0x%X) or the static mesh names not found: "
            "StuckEffectLookFix does nothing", g.midsOffset, g.formOffset);
        return false;
    }
    g.bpcClass = bpc;
    g.resolved = true;
    Log("[stuckfx] BPC_StatusEffect_C found: OverlayMIDs +0x%X, form +0x%X", g.midsOffset, g.formOffset);
    return true;
}
// Is the mesh a static mesh component? (its bDisallowNanite found once)
bool IsStaticMesh(const uint8_t* comp)
{
    const uint8_t* const cls = Ptr(comp, kObjClass);
    const uint8_t* const sm = cls ? ClassNamed(cls, g.staticName) : nullptr;
    if (!sm) return false;
    if (!g.staticClass) {
        const uint8_t* const f = FieldNamed(sm, g.nanName);
        uint8_t byteOff = 0, mask = 0;
        const uint32_t off = OffsetOf(f);
        if (off && Rd(f + kBoolByteOffset, &byteOff) && Rd(f + kBoolByteMask, &mask) && mask) {
            g.nanOffset = off + byteOff;
            g.nanMask = mask;
        }
        g.staticClass = sm;
        Log("[stuckfx] StaticMeshComponent's bDisallowNanite %s (+0x%X mask 0x%X)", g.nanMask ? "found" : "NOT found (left as is)",
            g.nanOffset, g.nanMask);
    }
    return true;
}

// SceneComponent.AttachParent's offset, from the first remembered mesh's class (once)
void ResolveAttach(const uint8_t* comp)
{
    if (g.attachTried) return;
    g.attachTried = true;
    const uint8_t* const cls = Ptr(comp, kObjClass);
    g.attachOffset = g.attachName.valid() && cls ? OffsetOf(FieldNamed(cls, g.attachName)) : 0;
    if (g.attachOffset) Log("[stuckfx] SceneComponent's AttachParent +0x%X", g.attachOffset);
    else Log("[stuckfx] SceneComponent's AttachParent NOT found: only looks of the mesh's own actor are put back");
}

uint8_t* OverlayOf(const uint8_t* comp) { return Ptr(comp, g.info.overlayOffset); }

// Would the look's component reach the mesh now? Its actor (the component's outer) is the mesh's own actor or the actor
// of a component the mesh is attached under (weapons under the body, a rider under the horse while mounted)
bool Reaches(const Look& l, const uint8_t* comp)
{
    const uint8_t* const owner = Ptr(l.by.p, kObjOuter);
    if (!owner) return false;
    const uint8_t* c = comp;
    for (int depth = 0; c && depth < 64; ++depth) {
        if (Ptr(c, kObjOuter) == owner) return true;
        if (!g.attachOffset) break;
        c = Ptr(c, g.attachOffset);
    }
    return false;
}

// The component shows an effect (has a form): its looks count as showing, wherever they are
bool Showing(const Ref& by) { return Alive(by) && Ptr(by.p, g.formOffset) != nullptr; }
// The material is in its component's OverlayMIDs (and so alive and part of a look it shows)
bool Listed(const Look& l)
{
    if (!Alive(l.by)) return false;
    uint8_t* const* data = nullptr;
    int32_t num = 0;
    if (!Rd(l.by.p + g.midsOffset, &data) || !Rd(l.by.p + g.midsOffset + 8, &num) || num <= 0 || num > 64 || !data) return false;
    for (int32_t i = 0; i < num; ++i) {
        uint8_t* m = nullptr;
        if (Rd(data + i, &m) && m == l.mid) return true;
    }
    return false;
}

// The engine's setter, as the exec function has it inlined (game thread)
void SetOverlay(uint8_t* comp, uint8_t* mid)
{
    if (OverlayOf(comp) == mid) return;
    if (!Wr(comp + g.info.overlayOffset, mid)) { ++g_faults; return; }
    void** vt = nullptr;
    VoidFn precache = nullptr;
    if (Rd(comp, &vt) && vt && Rd(vt + g.info.precacheSlot / 8, &precache) && precache) precache(comp);
    if (g.markDirty) g.markDirty(comp);
}

Mesh* FindMesh(const uint8_t* comp)
{
    for (int i = 0; i < g.count; ++i)
        if (g.meshes[i].comp.p == comp) return &g.meshes[i];
    return nullptr;
}
void DropMesh(Mesh* m) { *m = g.meshes[--g.count]; }
void Push(Mesh* m, uint8_t* mid, const Ref& by)
{
    if (m->n == kHist) {  // the oldest goes; the next one becomes an unknown base
        memmove(m->h, m->h + 1, sizeof(Look) * (kHist - 1));
        --m->n;
        m->h[0].by = Ref{};
    }
    m->h[m->n].mid = mid;
    m->h[m->n].by = by;
    ++m->n;
}

void LogAction(const char* what, const uint8_t*, const uint8_t*, const Look&, const uint8_t*)
{
    const uint64_t k = g_cleared.load() + g_restored.load();
    if (k <= 20) Log("[stuckfx] %s a status effect's look left on a mesh after the effect ended (%llu so far)", what,
                     static_cast<unsigned long long>(k));
}

// A blueprint call has set the mesh's overlay from `before` to `after`; `caller` is the calling frame's object.
void OnSet(uint8_t* comp, uint8_t* caller, uint8_t* before, uint8_t* after)
{
    Mesh* m = FindMesh(comp);
    if (m && !Alive(m->comp)) {  // a new object at a dead one's address: the old entry is no longer its
        DropMesh(m);
        m = nullptr;
    }
    if (!IsStatusEffect(caller)) {
        // anything else: a mesh it changes is no longer ours to follow
        if (m && after != before) DropMesh(m);
        return;
    }
    ++g_seen;
    g.dirty = true;
    if (!m) {
        if (!after || after == before) return;
        if (g.count == kMaxMeshes) {
            if (g_full++ == 0) Log("[stuckfx] %d meshes followed at once: further ones are not (stock), counted", kMaxMeshes);
            return;
        }
        const Ref r = MakeRef(comp);
        if (!r.p) return;
        ResolveAttach(comp);
        m = &g.meshes[g.count++];
        *m = Mesh{};
        m->comp = r;
        m->isStatic = IsStaticMesh(comp);
        if (before) Push(m, before, Ref{});
    } else if (m->n == 0 || m->h[m->n - 1].mid != before) {  // changed by something not seen: start again from what it showed
        m->n = 0;
        if (before) Push(m, before, Ref{});
    }
    if (after == before) return;
    if (!after) {  // the effect's own removal reached this mesh: done with it, as the stock game is
        DropMesh(m);
        return;
    }
    Push(m, after, MakeRef(caller));
}

// Once per frame after a status-effect call (and every ~2 s while meshes are remembered): leftovers are put right.
void Pass()
{
    g.inPass = true;
    for (int i = g.count - 1; i >= 0; --i) {
        Mesh* const m = &g.meshes[i];
        if (!Alive(m->comp) || m->n == 0) { DropMesh(m); continue; }
        uint8_t* const cur = OverlayOf(m->comp.p);
        const Look top = m->h[m->n - 1];
        if (top.mid != cur || !top.by.p) { DropMesh(m); continue; }  // changed since, or not a status-effect look
        if (Showing(top.by)) continue;                                // its effect is still on: as the game shows it
        // a leftover: the newest look under it that its component still shows and would still reach this mesh, else none
        int k = m->n - 2;
        uint8_t* repl = nullptr;
        for (; k >= 0; --k) {
            const Look& l = m->h[k];
            if (!l.by.p) break;  // a look that was not a status effect's (or an unknown base): none, as the stock removal
            if (Listed(l) && Reaches(l, m->comp.p)) { repl = l.mid; break; }
        }
        if (!repl && m->isStatic) {  // what the stock removal does to a static mesh before clearing its overlay
            if (g.nanMask) {
                uint8_t b = 0;
                if (Rd(m->comp.p + g.nanOffset, &b)) Wr(m->comp.p + g.nanOffset, static_cast<uint8_t>(b & ~g.nanMask));
            }
            if (g.rtSetter) g.rtSetter(m->comp.p, true);
        }
        LogAction(repl ? "restored" : "cleared", m->comp.p, cur, top, repl);
        SetOverlay(m->comp.p, repl);
        if (repl) {
            ++g_restored;
            m->n = k + 1;
        } else {
            ++g_cleared;
            DropMesh(m);
        }
    }
    g.inPass = false;
    g.dirty = false;
}

bool OnGt()
{
    const DWORD t = g_testGt.load(std::memory_order_relaxed);
    return (t && GetCurrentThreadId() == t) || NpcPacingOnGameThread();
}

void ExecHook(uint8_t* context, uint8_t* stack, void* result)
{
    if (!g_on.load(std::memory_order_relaxed) || g.inPass || !OnGt() || !g.info.overlayOffset) {
        g.original(context, stack, result);
        return;
    }
    uint8_t* const caller = Ptr(stack, kFrameObject);
    uint8_t* const before = OverlayOf(context);
    g.original(context, stack, result);
    OnSet(context, caller, before, OverlayOf(context));
}

// An entry detour: the first 5 bytes go to a trampoline that jumps back to fn + 5; fn starts with a jmp to a stub that
// jumps to the hook (one aligned qword written atomically: safe while the game runs).
bool Detour(uint8_t* fn, void* hook, uint8_t* region, bool beforeGameCode)
{
    if (memcmp(fn, kExecPrologue, sizeof kExecPrologue) != 0) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + sizeof kExecPrologue;
    memcpy(region, kExecPrologue, sizeof kExecPrologue);
    memcpy(region + 5, jmpAbs, sizeof jmpAbs);
    memcpy(region + 11, &resume, sizeof resume);
    memcpy(region + 32, jmpAbs, sizeof jmpAbs);
    memcpy(region + 38, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = (region + 32) - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t jmp[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    const uintptr_t at = reinterpret_cast<uintptr_t>(fn);
    if ((at & 7) + sizeof jmp <= 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(at & ~uintptr_t{7});
        if (!CodeSiteProtect(const_cast<LONG64*>(q), 8, &old)) return false;
        const LONG64 was = *q;
        LONG64 now = was;
        memcpy(reinterpret_cast<uint8_t*>(&now) + (at & 7), jmp, sizeof jmp);
        const bool ok = InterlockedCompareExchange64(q, now, was) == was;
        VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, std::span<const uint8_t>(jmp, sizeof jmp));
}

}  // namespace

bool FindStuckEffectLook(StuckEffectLookInfo* out)
{
    if (!g.searched) {
        g.searched = true;
        const GameModule& game = Game();
        uint8_t* const exec = FindExecByName("SetOverlayMaterial", kExecShape);
        if (!exec) return Fail("MeshComponent.SetOverlayMaterial's exec function not found by its name and shape");
        uint32_t cmp = 0, mov = 0, slot = 0;
        memcpy(&cmp, exec + kExecCmpDisp, 4);
        memcpy(&mov, exec + kExecMovDisp, 4);
        memcpy(&slot, exec + kExecSlotDisp, 4);
        if (cmp != mov || cmp < 0x100 || cmp > 0x2000 || cmp % 8 || slot % 8 || slot > 0x2000)
            return Fail("SetOverlayMaterial's inlined setter differs");
        uint8_t* const dirty = RipTarget(exec + kExecDirtyCall, 1, 5);
        uint8_t* const rtExec = FindExecByName("SetVisibleInRayTracing", kRtExecShape);
        uint8_t* const rt = rtExec ? RipTarget(rtExec + kRtExecCall, 1, 5) : nullptr;
        if (!game.text.Contains(dirty) || !rt || !game.text.Contains(rt))
            return Fail("MarkRenderStateDirty or SetVisibleInRayTracing not found");
        uint8_t* const fname = FindFNameCtor();
        if (!fname) return Fail("FName's constructor not found");
        uint8_t* const array = FindObjectArrayFromDtor();
        if (!array) return Fail("GUObjectArray not found");
        g.info.exec = exec;
        g.info.rtExec = rtExec;
        g.info.rtSetter = rt;
        g.info.markDirty = dirty;
        g.info.overlayOffset = cmp;
        g.info.precacheSlot = slot;
        g.info.objectArray = array;
        g.info.fnameCtor = fname;
        g.found = true;
        Log("[stuckfx] SetOverlayMaterial's exec exe+0x%llX (OverlayMaterial +0x%X, PrecachePSOs slot +0x%X, MarkRenderStateDirty "
            "exe+0x%llX), SetVisibleInRayTracing exe+0x%llX",
            static_cast<unsigned long long>(exec - game.base), cmp, slot, static_cast<unsigned long long>(dirty - game.base),
            static_cast<unsigned long long>(rt - game.base));
    }
    if (out && g.found) *out = g.info;
    return g.found;
}

bool InstallStuckEffectLook(bool beforeGameCode)
{
    if (g.installed) return true;
    if (!FindStuckEffectLook(nullptr)) return false;
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    g.original = reinterpret_cast<ExecFn>(region);
    g.markDirty = reinterpret_cast<VoidFn>(g.info.markDirty);
    g.rtSetter = reinterpret_cast<RtFn>(g.info.rtSetter);
    g.fname = reinterpret_cast<FNameFn>(g.info.fnameCtor);
    g.objArray = g.info.objectArray;
    if (!Detour(g.info.exec, reinterpret_cast<void*>(&ExecHook), region, beforeGameCode))
        return Fail("SetOverlayMaterial's first instruction changed or the detour could not be placed");
    g.installed = true;
    Log("[stuckfx] installed (SetOverlayMaterial's exec function detoured)");
    return true;
}

void SetStuckEffectLookFix(bool on)
{
    if (on && !g.installed) return;
    if (g_on.exchange(on) != on) Log("[stuckfx] StuckEffectLookFix %s", on ? "on" : "off");
}

void StuckEffectLookFrameEnd()
{
    if (!g.installed) return;
    ++g.frame;
    if (!g_on.load(std::memory_order_relaxed)) {
        g.count = 0;  // off: nothing followed (on again: from the next status-effect call)
        g.dirty = false;
        return;
    }
    if (g.dirty || (g.count && g.frame % kPeriodicFrames == 0)) Pass();
}

void LogStuckEffectLookStats()
{
    if (!g.installed) return;
    Log("[stuckfx] stats: %llu status-effect overlay calls seen, %llu leftover looks cleared, %llu restored to the look under "
        "them, %llu not followed (table full), %llu write faults",
        static_cast<unsigned long long>(g_seen.load()), static_cast<unsigned long long>(g_cleared.load()),
        static_cast<unsigned long long>(g_restored.load()), static_cast<unsigned long long>(g_full.load()),
        static_cast<unsigned long long>(g_faults.load()));
}

// ---- self-test: the rules on fake objects (a fake GUObjectArray, classes, components, MIDs) ----
namespace {
struct FakeObj {
    uint8_t b[0x800];
};
int g_testDirty = 0, g_testPrecache = 0, g_testRt = 0;
void TestDirty(uint8_t*) { ++g_testDirty; }
void TestPrecache(uint8_t*) { ++g_testPrecache; }
void TestRt(uint8_t*, bool v) { g_testRt += v ? 1 : 100; }
// the exec function's stand-in: sets the overlay to g_testNext, as P_GET_OBJECT + the inlined setter would
uint8_t* g_testNext = nullptr;
int g_testExecCalls = 0;
void TestExec(uint8_t* context, uint8_t*, void*)
{
    ++g_testExecCalls;
    memcpy(context + g.info.overlayOffset, &g_testNext, 8);
}
DWORD WINAPI TestOffThread(void* p)
{
    uint8_t* const* a = static_cast<uint8_t* const*>(p);
    ExecHook(a[0], a[1], nullptr);
    return 0;
}
// FName's stand-in: the names the fix looks up, one of them (g_testMissing) not found
const wchar_t* g_testMissing = nullptr;
NameId* TestFName(NameId* out, const wchar_t* name, int)
{
    static const wchar_t* const names[] = {L"BPC_StatusEffect_C", L"OverlayMIDs", L"form", L"StaticMeshComponent", L"bDisallowNanite",
                                           L"AttachParent"};
    *out = NameId{};
    if (g_testMissing && !wcscmp(name, g_testMissing)) return out;
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (!wcscmp(name, names[i])) out->index = 41 + i;
    return out;
}
}  // namespace

int StuckEffectLookTestLogic()
{
    constexpr int kObjs = 40;
    static FakeObj objs[kObjs];
    static uint8_t items[kObjs * kItemSize];
    static uint8_t* chunks[1];
    static uint8_t array[0x40];
    static void* vtable[0x100];
    static uint8_t* slots[kObjs];  // each component's OverlayMIDs storage (one entry)
    memset(objs, 0, sizeof objs);
    memset(items, 0, sizeof items);
    memset(array, 0, sizeof array);
    chunks[0] = items;
    uint8_t* const chunkTable = reinterpret_cast<uint8_t*>(chunks);
    memcpy(array + kArrayChunks, &chunkTable, 8);
    const int32_t num = kObjs;
    memcpy(array + kArrayNum, &num, 4);
    auto reg = [&](int i) {
        uint8_t* const p = objs[i].b;
        const int32_t idx = i, serial = 100 + i;
        memcpy(p + kObjIndex, &idx, 4);
        memcpy(items + kItemSize * i, &p, 8);
        memcpy(items + kItemSize * i + kItemSerial, &serial, 4);
        void** vt = vtable;
        memcpy(p, &vt, 8);
        return p;
    };
    static State keep;  // the test leaves the state as it found it (the search's results kept)
    keep = g;
    const uint32_t overlay = 0x5B0, mids = 0x1A0, form = 0xC0, nan = 0x5F4, attach = 0x2A0;
    // classes: 0 = BPC_StatusEffect_C, 1 = StaticMeshComponent, 2 = SkeletalMeshComponent (its own name), 3 = other caller
    uint8_t* const clsBpc = reg(0);
    uint8_t* const clsStatic = reg(1);
    uint8_t* const clsSkel = reg(2);
    uint8_t* const clsOther = reg(3);
    const NameId nBpc{11, 0}, nStatic{12, 0}, nSkel{13, 0}, nOther{14, 0};
    memcpy(clsBpc + kObjName, &nBpc, 8);
    memcpy(clsStatic + kObjName, &nStatic, 8);
    memcpy(clsSkel + kObjName, &nSkel, 8);
    memcpy(clsOther + kObjName, &nOther, 8);
    auto make = [&](int i, uint8_t* cls, uint8_t* outer = nullptr) {
        uint8_t* const p = reg(i);
        memcpy(p + kObjClass, &cls, 8);
        memcpy(p + kObjOuter, &outer, 8);
        return p;
    };
    auto attachTo = [&](uint8_t* c, uint8_t* parent) { memcpy(c + attach, &parent, 8); };
    // actors (the outers of their components)
    uint8_t* const actP = make(17, clsOther);   // the player
    uint8_t* const actS = make(18, clsOther);   // the player's sword
    uint8_t* const actH = make(19, clsOther);   // a horse
    uint8_t* const actQ = make(20, clsOther);   // the player's quiver
    uint8_t* const actR = make(21, clsOther);   // a rider
    uint8_t* const actRS = make(24, clsOther);  // the rider's sword
    uint8_t* const pawnFx = make(4, clsBpc, actP);    // the player's status-effect component
    uint8_t* const swordFx = make(5, clsBpc, actS);   // the sword's own status-effect component
    uint8_t* const horseFx = make(6, clsBpc, actH);   // the horse's
    uint8_t* const other = make(7, clsOther);         // some other blueprint
    uint8_t* const body = make(8, clsSkel, actP);     // the player's body
    uint8_t* const quiver = make(9, clsStatic, actQ); // the quiver (static mesh), attached under the body
    uint8_t* const sword = make(10, clsStatic, actS); // the sword, attached under the body
    uint8_t* const rider = make(11, clsSkel, actR);   // a rider's body
    uint8_t* const midA = make(12, clsOther);   // the player's effect MID
    uint8_t* const midG = make(13, clsOther);   // the sword's own look (its component's MID)
    uint8_t* const midH = make(14, clsOther);   // the horse's effect MID
    uint8_t* const matX = make(15, clsOther);   // a material something else sets
    uint8_t* const formF = make(16, clsOther);
    uint8_t* const riderFx = make(22, clsBpc, actR);        // the rider's status-effect component
    uint8_t* const riderSword = make(23, clsStatic, actRS); // the rider's sword, attached under the rider's body
    uint8_t* const horseMesh = make(25, clsSkel, actH);     // the horse's body
    uint8_t* const midR = make(26, clsOther);               // the rider's effect MID
    uint8_t* const midA2 = make(27, clsOther);              // the player's next effect MID
    uint8_t* const spare = make(28, clsStatic, actS);       // one more mesh
    attachTo(quiver, body);
    attachTo(sword, body);
    attachTo(riderSword, rider);

    State& s = g;
    const bool wasOn = g_on.load();
    s = State{};
    s.objArray = array;
    s.resolved = true;
    s.bpcClass = clsBpc;
    s.bpcName = nBpc;
    s.staticName = nStatic;
    s.staticClass = clsStatic;
    s.midsOffset = mids;
    s.formOffset = form;
    s.nanOffset = nan;
    s.nanMask = 2;
    s.attachOffset = attach;
    s.attachTried = true;
    s.info.overlayOffset = overlay;
    s.info.precacheSlot = 0x640;
    s.markDirty = &TestDirty;
    s.rtSetter = &TestRt;
    vtable[0x640 / 8] = reinterpret_cast<void*>(&TestPrecache);
    auto setMids = [&](uint8_t* fx, uint8_t* m) {  // OverlayMIDs = {m} (or empty); form set with it
        int32_t idx = 0;
        memcpy(&idx, fx + kObjIndex, 4);
        slots[idx] = m;
        uint8_t** data = m ? &slots[idx] : nullptr;
        const int32_t n = m ? 1 : 0;
        memcpy(fx + mids, &data, 8);
        memcpy(fx + mids + 8, &n, 4);
        uint8_t* const f = m ? formF : nullptr;
        memcpy(fx + form, &f, 8);
    };
    auto ov = [&](uint8_t* c) { uint8_t* v = nullptr; memcpy(&v, c + overlay, 8); return v; };
    auto bpSet = [&](uint8_t* caller, uint8_t* c, uint8_t* m) {  // a blueprint call through the exec function
        uint8_t* const before = ov(c);
        memcpy(c + overlay, &m, 8);
        OnSet(c, caller, before, m);
    };
    auto frameEnd = [&] { if (s.dirty) Pass(); };
    auto setDead = [&](int i, bool dead) { const int32_t f = dead ? kItemDead : 0; memcpy(items + kItemSize * i + kItemFlags, &f, 4); };
    int fail = 0;
    auto check = [&](bool ok, int id) { if (!ok && !fail) fail = id; };

    // 1. the red quiver: the player's effect on body and quiver; the removal reaches the body only
    setMids(pawnFx, midA);
    bpSet(pawnFx, body, midA);
    bpSet(pawnFx, quiver, midA);
    quiver[nan] |= 2;
    frameEnd();
    check(ov(body) == midA && ov(quiver) == midA, 1);  // showing: untouched
    setMids(pawnFx, nullptr);                          // RemoveStatusEffect: OverlayMIDs emptied, form none
    bpSet(pawnFx, body, nullptr);
    check(s.count == 1, 2);
    g_testRt = 0;
    frameEnd();
    check(ov(quiver) == nullptr && !(quiver[nan] & 2) && g_testRt == 1 && s.count == 0, 3);
    // 2. while the effect still shows (form set, even with OverlayMIDs empty for the next effect's load), nothing is done
    setMids(pawnFx, midA);
    bpSet(pawnFx, quiver, midA);
    const int32_t zero = 0;
    memcpy(pawnFx + mids + 8, &zero, 4);  // OverlayMIDs emptied by a new start, form still set
    frameEnd();
    s.dirty = true;
    frameEnd();
    check(ov(quiver) == midA, 4);
    setMids(pawnFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(quiver) == nullptr, 5);
    // 3. the sword's own look under the player's effect comes back when the player's effect ends
    setMids(swordFx, midG);
    bpSet(swordFx, sword, midG);
    setMids(pawnFx, midA);
    bpSet(pawnFx, sword, midA);
    setMids(pawnFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(sword) == midG && s.count == 1, 6);
    // ... and goes when the sword's own effect ends and its removal reaches it (the game's own call)
    setMids(swordFx, nullptr);
    bpSet(swordFx, sword, nullptr);
    frameEnd();
    check(ov(sword) == nullptr && s.count == 0, 7);
    // 4. a look under it that was not a status effect's (set by something else): none, as the stock removal
    bpSet(other, sword, matX);
    setMids(pawnFx, midA);
    bpSet(pawnFx, sword, midA);
    setMids(pawnFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(sword) == nullptr, 8);
    // 5. something else sets the mesh after the effect: left alone
    setMids(pawnFx, midA);
    bpSet(pawnFx, quiver, midA);
    bpSet(other, quiver, matX);
    setMids(pawnFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(quiver) == matX && s.count == 0, 9);
    memset(quiver + overlay, 0, 8);
    // 6. a change not seen (native code) is not undone: the mesh is dropped
    setMids(pawnFx, midA);
    bpSet(pawnFx, quiver, midA);
    memcpy(quiver + overlay, &matX, 8);
    setMids(pawnFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(quiver) == matX && s.count == 0, 10);
    memset(quiver + overlay, 0, 8);
    // 7. the rider: the horse's look on the rider, the rider gets off, the horse's effect ends (its removal reaches the
    // horse only); then the horse's component is destroyed while a newer look is left: both cleared
    setMids(horseFx, midH);
    bpSet(horseFx, rider, midH);
    setMids(horseFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(rider) == nullptr, 11);
    setMids(horseFx, midH);
    bpSet(horseFx, rider, midH);
    s.dirty = true;
    frameEnd();
    check(ov(rider) == midH, 12);  // still showing
    setDead(6, true);               // the horse's component goes away
    s.dirty = true;
    frameEnd();
    check(ov(rider) == nullptr, 13);
    setDead(6, false);
    // 8. off: the frame end forgets what was followed and changes nothing
    g_on.store(false);
    s.installed = true;
    setMids(pawnFx, midA);
    bpSet(pawnFx, quiver, midA);
    setMids(pawnFx, nullptr);
    StuckEffectLookFrameEnd();
    check(ov(quiver) == midA && s.count == 0, 14);
    // 9. a mesh destroyed before the pass is dropped without a write
    g_on.store(true);
    memset(quiver + overlay, 0, 8);
    setMids(pawnFx, midA);
    bpSet(pawnFx, quiver, midA);
    setMids(pawnFx, nullptr);
    setDead(9, true);
    const int dirtyBefore = g_testDirty;
    StuckEffectLookFrameEnd();
    check(ov(quiver) == midA && s.count == 0 && g_testDirty == dirtyBefore, 15);
    // 10. calls of other blueprints alone are not followed
    setDead(9, false);
    bpSet(other, body, matX);
    check(s.count == 0, 16);
    memset(quiver + overlay, 0, 8);
    memset(body + overlay, 0, 8);
    // 11. a mounted rider: the horse's look under the rider's own on the rider's sword comes back while the rider sits on
    // the horse (it reaches the sword through the rider), but not once the rider has got off (the horse still burning)
    attachTo(rider, horseMesh);
    setMids(horseFx, midH);
    bpSet(horseFx, riderSword, midH);
    setMids(riderFx, midR);
    bpSet(riderFx, riderSword, midR);
    setMids(riderFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(riderSword) == midH && s.count == 1, 17);
    setMids(riderFx, midR);
    bpSet(riderFx, riderSword, midR);
    attachTo(rider, nullptr);  // gets off
    setMids(riderFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(riderSword) == nullptr && ov(horseMesh) == nullptr && s.count == 0, 18);
    setMids(horseFx, nullptr);
    // 12. an ended look under the top one is skipped for a deeper one still showing (the sword's own)
    setMids(swordFx, midG);
    bpSet(swordFx, sword, midG);
    setMids(pawnFx, midA);
    bpSet(pawnFx, sword, midA);
    setMids(pawnFx, midA2);  // the player's next effect (midA no longer listed)
    bpSet(pawnFx, sword, midA2);
    setMids(pawnFx, nullptr);
    const uint64_t restoredBefore = g_restored.load();
    s.dirty = true;
    frameEnd();
    check(ov(sword) == midG && g_restored.load() == restoredBefore + 1, 19);
    // 13. the top look's component gone, a look under it still showing: put back
    setMids(pawnFx, midA);
    bpSet(pawnFx, sword, midA);
    setDead(4, true);
    s.dirty = true;
    frameEnd();
    check(ov(sword) == midG, 20);
    setDead(4, false);
    setMids(swordFx, nullptr);
    bpSet(swordFx, sword, nullptr);
    check(ov(sword) == nullptr && s.count == 0, 21);
    // 14. history overflow: seven looks keep the newest six, the oldest becomes an unknown base (which gives none)
    setMids(swordFx, midG);
    setMids(pawnFx, midA);
    for (int i = 0; i < 7; ++i) bpSet(i % 2 ? pawnFx : swordFx, sword, i % 2 ? midA : midG);
    const Mesh* const hm = FindMesh(sword);
    check(hm && hm->n == kHist && !hm->h[0].by.p && hm->h[kHist - 1].mid == midG, 22);
    setMids(swordFx, nullptr);
    setMids(pawnFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(sword) == nullptr && s.count == 0, 23);
    // 15. the table full: a further mesh is not followed (stock), counted
    s.count = kMaxMeshes;
    const uint64_t fullBefore = g_full.load();
    setMids(pawnFx, midA);
    bpSet(pawnFx, spare, midA);
    check(s.count == kMaxMeshes && g_full.load() == fullBefore + 1, 24);
    s.count = 0;
    memset(spare + overlay, 0, 8);
    setMids(pawnFx, nullptr);
    // 16. the hook's gates, through ExecHook: off, during the pass, off the game thread: the call goes on, not followed;
    // on, on the game thread: followed
    s.original = &TestExec;
    uint8_t frame[0x40] = {};
    memcpy(frame + kFrameObject, &pawnFx, 8);
    setMids(pawnFx, midA);
    g_testNext = midA;
    g_testExecCalls = 0;
    g_testGt.store(GetCurrentThreadId());
    g_on.store(false);
    ExecHook(quiver, frame, nullptr);
    check(ov(quiver) == midA && s.count == 0, 25);
    memset(quiver + overlay, 0, 8);
    g_on.store(true);
    s.inPass = true;
    ExecHook(quiver, frame, nullptr);
    s.inPass = false;
    check(ov(quiver) == midA && s.count == 0, 26);
    memset(quiver + overlay, 0, 8);
    uint8_t* args[2] = {quiver, frame};
    if (HANDLE t = CreateThread(nullptr, 0, &TestOffThread, args, 0, nullptr)) {
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
    check(ov(quiver) == midA && s.count == 0, 27);
    memset(quiver + overlay, 0, 8);
    ExecHook(quiver, frame, nullptr);
    check(ov(quiver) == midA && s.count == 1 && g_testExecCalls == 4, 28);
    setMids(pawnFx, nullptr);
    s.dirty = true;
    frameEnd();
    check(ov(quiver) == nullptr && s.count == 0, 29);
    g_testGt.store(0);
    // 17. the names and offsets looked up as in the game: a blueprint child class of BPC_StatusEffect_C with a property
    // chain, a static mesh class under SceneComponent; then one name missing: does nothing
    {
        static uint8_t cBpc[0x80], cChild[0x80], cScene[0x80], cStatic[0x80];
        static uint8_t fMids[0x80], fOther[0x80], fForm[0x80], fNan[0x80], fAttach[0x80];
        memset(cBpc, 0, sizeof cBpc);
        memset(cChild, 0, sizeof cChild);
        memset(cScene, 0, sizeof cScene);
        memset(cStatic, 0, sizeof cStatic);
        auto field = [&](uint8_t* f, uint32_t name, int32_t off, uint8_t* next) {
            memset(f, 0, 0x80);
            const NameId n{name, 0};
            memcpy(f + kFieldName, &n, 8);
            memcpy(f + kPropOffset, &off, 4);
            memcpy(f + kFieldNext, &next, 8);
        };
        auto cls = [&](uint8_t* c, uint32_t name, uint8_t* super, uint8_t* props) {
            const NameId n{name, 0};
            memcpy(c + kObjName, &n, 8);
            memcpy(c + kStructSuper, &super, 8);
            memcpy(c + kStructProps, &props, 8);
        };
        field(fOther, 99, 0x30, nullptr);
        field(fForm, 43, 0xC8, fOther);   // form
        field(fMids, 42, 0x1B0, fForm);   // OverlayMIDs
        field(fNan, 45, 0x5F0, nullptr);  // bDisallowNanite: byte +4, mask 0x08
        // the fake bool property has the layout verified on the game's own StaticMeshComponent (FieldSize +0x70, ByteOffset
        // +0x71, ByteMask +0x72, FieldMask +0x73; its bDisallowNanite read (1, 0, 2, 2) there), not the code's constants: a
        // wrong constant (once +0x79 / +0x7A) must fail here and not only in the game
        fNan[0x70] = 1;
        fNan[0x71] = 4;
        fNan[0x72] = 0x08;
        fNan[0x73] = 0x08;
        field(fAttach, 46, 0x2B8, nullptr);  // AttachParent
        cls(cBpc, 41, nullptr, fMids);
        cls(cChild, 77, cBpc, nullptr);
        cls(cScene, 78, nullptr, fAttach);
        cls(cStatic, 44, cScene, fNan);
        uint8_t* const fxObj = make(29, cChild, actS);
        uint8_t* const meshObj = make(30, cStatic, actS);
        auto fresh = [&] {
            s = State{};
            s.objArray = array;
            s.fname = &TestFName;
            s.info.overlayOffset = overlay;
            s.markDirty = &TestDirty;
        };
        fresh();
        g_testMissing = L"BPC_StatusEffect_C";  // the blueprint not loaded yet: no, and asked again next time
        check(!IsStatusEffect(fxObj) && !s.resolveFailed && !s.resolved, 30);
        g_testMissing = nullptr;
        check(IsStatusEffect(fxObj) && s.resolved && s.bpcClass == cBpc && s.midsOffset == 0x1B0 && s.formOffset == 0xC8 &&
                  IsStatusEffect(fxObj) && !IsStatusEffect(meshObj),
              31);
        OnSet(meshObj, fxObj, nullptr, midA);  // the first mesh: its static-mesh flag and AttachParent found
        check(s.count == 1 && s.meshes[0].isStatic && s.staticClass == cStatic && s.nanOffset == 0x5F4 && s.nanMask == 0x08 &&
                  s.attachTried && s.attachOffset == 0x2B8,
              32);
        fresh();
        g_testMissing = L"form";  // a property not found: nothing done, not asked again
        check(!IsStatusEffect(fxObj) && s.resolveFailed && !IsStatusEffect(fxObj), 33);
        fresh();
        g_testMissing = L"AttachParent";  // no AttachParent: only the mesh's own actor's looks count
        check(IsStatusEffect(fxObj), 34);
        OnSet(meshObj, fxObj, nullptr, midA);
        check(s.attachTried && !s.attachOffset, 35);
        g_testMissing = nullptr;
        check(kBoolByteOffset == 0x71 && kBoolByteMask == 0x72, 36);  // the verified FBoolProperty layout (see the fake above)
    }

    g = keep;
    g_on.store(wasOn);
    g_seen = g_cleared = g_restored = g_full = g_faults = 0;
    return fail;
}

}  // namespace opf
