// EquipRefreshOneSpawn (worker povkeep, 2026-11-25; S-050 dialogue edges; evidence: findings tags
// povkeep and revpovkeep): a stock game inefficiency (also without the mod), not a bug.
// Altar's equipment-slot refresh (exe+0x4830170 on Steam build 19115871; rcx = the character, dl = the slot 2 / 4 / 7 / 8)
// re-applies the slot's clothing actor: it calls UChildActorComponent::SetChildActorClass(cac, info->Class, nullptr) and
// right after that the component's virtual CreateChildActor() (vtable +0x608, an empty TFunction). On a registered
// component UE 5.3's SetChildActorClass ends with that same CreateChildActor() (ChildActorName = None, DestroyChildActor,
// CreateChildActor), and CreateChildActor itself starts with DestroyChildActor. So for a slot whose class does not change,
// stock destroys the clothing actor, builds it (construction script, cloth simulation builds, BeginPlay), destroys that
// one at once and builds it again. The refresh runs for every slot at each first/third-person switch (a conversation's
// start and end in third person, the view toggle), on equip and when a scene or character is set up.
// With the key on, the refresh's call of SetChildActorClass (one call site, found from the refresh's pattern) goes to a
// hook that skips it when it would only rebuild what the next instruction rebuilds anyway: same class pointer as the
// component's, no template argument, the component's template null or of that class, a live child actor (not garbage /
// pending kill, the refresh's own flag test) of exactly that class. The refresh's own CreateChildActor() then destroys the
// old actor and builds the new one as in stock, from the same class with the instance data of the actor before it, and the
// rest of the refresh (visibility (DirtyOnly), the child's flag, the three Blueprint events) runs on it unchanged. The clothing actor is
// built once instead of twice; the final actor is still freshly built (its cloth starts from the reference pose as in
// stock). Anything else (another class, a template, no or a dying child): the game's call. Measured (bench, robe +
// gauntlets, third person, n = 8 ABBA): a view switch 13.6-16.0 ms -> 8.4-10.7 ms on the game thread; lab verify 73 slots
// compared with the game's double build in 73 slots (also world entry, equip, a quick load), 0 value differences.
// Premise checked at install (fail closed): the call is followed by `mov rax,[rdi]` + a zeroed TFunction + `call
// [rax+S]` on the same component, and SetChildActorClass's registered branch ends in `call [rax+S]` with the same S.
// Coexists with ClothRegisterLiteRefresh (it redirects the refresh's callers, this the call inside; EquipRefreshOneSpawn
// SetClass() gives it the original target). Keys ([Fixes]): EquipRefreshOneSpawn 0 = the game's way (not installed),
// 1 = on; read at start-up (a restart switches it on), then live.
//
// EquipRefreshKeepActor (worker clothkeep, 2026-11-26; S-050; evidence: findings tag clothkeep): an
// optional tweak (may change the look: a cape's or robe's cloth keeps swinging instead of starting again). User decision
// 2026-11-25: the player's clothing actor may be kept on a same-item refresh at the view switch. Inside SwitchPOV
// (exe+0x4899380, entry hook here; only a real view change: the new view differs from the controller's current one and
// SwitchPOV's skip flag is clear, so the forced same-view calls of the pairing-command executor exe+0x472A936 at scene /
// world set-up stay stock (review revclothkeep S1-1); the player's first/third-person switch: a conversation's start and end
// in third person and the view toggle) a same-class refresh (the EquipRefreshOneSpawn guard above) skips BOTH SetChildActorClass and the
// refresh's own CreateChildActor() (its `call [rax+S]` at refresh+0x8E goes to a hook that skips it only right after this
// skip, same thread and component): the clothing actor that is already there stays (no EndPlay / BeginPlay, no
// construction script, no spawn cloth build). The rest of the refresh runs on it as in stock: SetVisibility(view, DirtyOnly:
// r8b = 1 is EVisibilityPropagation::DirtyOnly, only the component's own flag changes),
// the child's flag 0x3043CA0, the three Blueprint events (leader pose for the view, OnAttachedToPhenotype, the third; a
// robe's OnAttachedToPhenotype rebuilds its cloth proxy itself, so such a piece's cloth still starts again).
// UE 5.3 SetChildActorClass / CreateChildActor for a same class without a template only destroy the actor and spawn one of
// the same class at the component, attached SnapToTargetNotIncludingScale, with the old actor's component instance data
// and attached actors carried over, so the kept actor differs only in what a fresh spawn resets (cloth state, anim state,
// runtime material values, the object's name). The skipped CreateChildActor's by-value TFunction is empty (zeroed by the
// refresh), so not calling it leaves nothing to destroy. Outside SwitchPOV (equip, scene set-up, NPCs) nothing changes.
// Keys ([Fixes], optional tweaks): EquipRefreshKeepActor 0 = the game's way (not installed), 1 = on; read at start-up,
// then live. Works with or without EquipRefreshOneSpawn (it uses the same call-site hook).
#include "common.h"

#include <atomic>
#include <cstring>
#include <intrin.h>

namespace opf {

namespace {
// the refresh from refresh+0x0A (its first 10 bytes may carry a lab probe's jump) through the CreateChildActor vcall
constexpr char kRefreshBody[] =
    "57 48 83 EC 60 0F B6 F2 48 8B D9 44 8B C6 41 83 E8 02 74 ?? 41 83 E8 02 74 ?? 41 83 E8 03 74 ?? 41 83 F8 01 74 ?? "
    "33 FF EB ?? 48 8B B9 ?? ?? 00 00 EB ?? 48 8B B9 ?? ?? 00 00 EB ?? 48 8B B9 ?? ?? 00 00 EB ?? 48 8B B9 ?? ?? 00 00 "
    "40 0F B6 D6 E8 ?? ?? ?? ?? 45 33 C0 48 8B CF 48 8B 90 ?? ?? 00 00 E8 ?? ?? ?? ?? "
    "48 8B 07 48 8D 54 24 20 48 8B CF 48 C7 44 24 20 00 00 00 00 48 C7 44 24 30 00 00 00 00 FF 90 ?? ?? ?? ??";
constexpr size_t kBodyAt = 0x0A;          // the pattern starts at refresh+0x0A
constexpr size_t kSetClassCall = 0x6C;    // refresh+0x6C: call SetChildActorClass
constexpr size_t kVcallDisp = 0x90;       // refresh+0x8E: call [rax+S] (disp32 at +0x90)
// SetChildActorClass's registered branch: the template guard around `call [rax+S]` (CreateChildActor)
constexpr char kSetClassTail[] =
    "48 8B 9E ?? ?? 00 00 48 8D 54 24 60 4C 89 BE ?? ?? 00 00 48 8B CE 48 8B 06 4C 89 74 24 60 4C 89 74 24 70 "
    "FF 90 ?? ?? ?? ?? 48 89 9E ?? ?? 00 00";
constexpr size_t kTailTpl1 = 3, kTailTpl2 = 15, kTailVcall = 37, kTailTpl3 = 44;
constexpr char kStoreClass[] = "48 89 91 ?? ?? 00 00";           // SetChildActorClass's first store: ChildActorClass
constexpr char kLoadChild[] = "48 8B BF ?? ?? 00 00 48 85 FF";  // refresh: mov rdi,[rdi+ChildActor]; test rdi,rdi
constexpr char kDeadTest[] = "8B 47 08 A9 00 00 00 60 75";      // refresh: mov eax,[rdi+8]; test eax,60000000h; jne
constexpr uint32_t kDeadFlags = 0x60000000u;
// EquipRefreshKeepActor: SwitchPOV exe+0x4899380 from +0x0A (its first 10 bytes, two stores, are relocated into the trampoline)
constexpr char kSwitchBody[] =
    "57 48 83 EC 20 80 B9 ?? ?? 00 00 00 41 0F B6 E8 0F B6 F2 48 8B F9 0F 85 ?? ?? ?? ?? "
    "0F B6 81 ?? ?? 00 00 88 81 ?? ?? 00 00 3A C2 75 ?? 80 B9 ?? ?? 00 00 00 0F 84";
constexpr uint8_t kSwitchPrologue[10] = {0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
constexpr size_t kSwitchAt = 0x0A;
constexpr size_t kVcall = 0x8E;  // refresh+0x8E: FF 90 disp32 = call [rax+S] (CreateChildActor)

using SetClassFn = void (*)(uint8_t* cac, void* cls, void* tmpl);
SetClassFn g_setClass = nullptr;  // the game's SetChildActorClass
uint8_t* g_call = nullptr;        // the refresh's call of it
uint32_t g_classOff = 0, g_childOff = 0, g_tplOff = 0;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> s_once{0}, s_game{0};

// EquipRefreshKeepActor
using SwitchFn = uint64_t (*)(uint8_t* ctrl, uint64_t pov, uint64_t remember, uint64_t r9);
using CreateFn = void (*)(uint8_t* cac, void* customizer);
SwitchFn g_switchOrig = nullptr;      // trampoline: SwitchPOV's relocated first 10 bytes, then back
uint8_t* g_switch = nullptr;          // SwitchPOV (patched)
uint8_t* g_vcallSite = nullptr;       // the refresh's CreateChildActor call (patched)
uint32_t g_vslot = 0;
std::atomic<bool> g_keepOn{false};
std::atomic<bool> g_povLab{false};    // lab: PovSwitchLab wraps the game's SwitchPOV
// game thread only (SwitchPOV and the refresh run there; any other thread is left to the game)
DWORD g_povThread = 0;
int g_povDepth = 0;
uint32_t g_curOff = 0, g_skipOff = 0;  // SwitchPOV's view byte and skip flag in the controller (disp32 at SwitchPOV+0x29 / +0x11)
bool g_lastScope = false;             // the last SwitchPOV entry on the game thread was a real view change (lab probe line)
void* g_lastCaller = nullptr;         // the game's caller of that SwitchPOV call (lab probe line)
DWORD g_skipThread = 0;
const uint8_t* g_skipCac = nullptr;   // the component whose next CreateChildActor() call from the refresh is skipped
std::atomic<uint64_t> s_kept{0}, s_keepNot{0};
// EquipRefreshKeepCloth (dlgedge): the clothing actors kept inside the current outermost SwitchPOV call (game thread only)
constexpr int kMaxKept = 16;
const uint8_t* g_keptActors[kMaxKept];
int g_numKept = 0;
uint32_t g_switchSerial = 0;  // counts outermost view switches (EquipRefreshKeepCloth: builds seen inside the current one)

bool InViewSwitch() { return g_povDepth > 0 && GetCurrentThreadId() == g_povThread; }

void ArmSkip(const uint8_t* cac)
{
    g_skipThread = GetCurrentThreadId();
    g_skipCac = cac;
}

// a real view change: SwitchPOV's skip flag clear and the new view differs from the current one (read before the call)
bool RealViewChange(const uint8_t* ctrl, uint64_t pov)
{
    if (!ctrl || !g_curOff) return false;
    __try {
        return ctrl[g_skipOff] == 0 && static_cast<uint8_t>(pov) != ctrl[g_curOff];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t CallSwitch(uint8_t* ctrl, uint64_t pov, uint64_t remember, uint64_t r9)
{
    return g_switchOrig(ctrl, pov, remember, r9);
}

uint64_t SwitchHook(uint8_t* ctrl, uint64_t pov, uint64_t remember, uint64_t r9)
{
    const DWORD tid = GetCurrentThreadId();
    if (g_povDepth > 0 && tid != g_povThread) return g_switchOrig(ctrl, pov, remember, r9);  // another thread: as stock
    const bool real = RealViewChange(ctrl, pov);
    g_lastScope = real;
    g_lastCaller = _ReturnAddress();  // reached by jumps from SwitchPOV's entry: the game's call site
    if (!real) return CallSwitch(ctrl, pov, remember, r9);  // forced same view (scene / world set-up): outside the keep's scope
    g_povThread = tid;
    if (g_povDepth == 0) { g_numKept = 0; ++g_switchSerial; }  // EquipRefreshKeepCloth: a new outermost view switch
    ++g_povDepth;
    uint64_t r = 0;
    __try {
        r = CallSwitch(ctrl, pov, remember, r9);
    } __finally {
        if (--g_povDepth == 0) g_numKept = 0;  // also on an unwind (review S3-2)
    }
    return r;
}

// the refresh's `call [rax+S]` (CreateChildActor(empty TFunction)): skipped only right after the keep skipped SetChildActorClass
void VcallHook(uint8_t* cac, void* customizer)
{
    const DWORD tid = GetCurrentThreadId();
    const bool skip = g_skipCac && cac == g_skipCac && tid == g_skipThread;
    if (g_skipThread == tid) g_skipCac = nullptr;  // armed for one call only
    if (skip) return;  // the actor already there stays; the empty TFunction owns nothing
    const uint8_t* const vt = *reinterpret_cast<uint8_t* const*>(cac);
    reinterpret_cast<CreateFn>(*reinterpret_cast<void* const*>(vt + g_vslot))(cac, customizer);
}

template <typename T> T ReadAt(const uint8_t* p, size_t off)
{
    T v{};
    __try { memcpy(&v, p + off, sizeof v); } __except (EXCEPTION_EXECUTE_HANDLER) { v = T{}; }
    return v;
}

const uint8_t* Within(const uint8_t* from, size_t len, const Pattern& p, int* count = nullptr)
{
    const uint8_t* first = nullptr;
    int n = 0;
    for (size_t i = 0; i + p.size() <= len; ++i)
        if (MatchesAt(from + i, p)) { if (!first) first = from + i; ++n; }
    if (count) *count = n;
    return first;
}

void Hook(uint8_t* cac, void* cls, void* tmpl)
{
    const bool redundant = EquipRefreshOneSpawnRedundant(cac, cls, tmpl);
    const bool keepOn = g_vcallSite && g_keepOn.load(std::memory_order_relaxed) && InViewSwitch();
    if (redundant && keepOn) {
        ArmSkip(cac);  // and the refresh's CreateChildActor() right after: the actor already there stays
        s_kept.fetch_add(1, std::memory_order_relaxed);
        if (g_numKept < kMaxKept) g_keptActors[g_numKept++] = ReadAt<const uint8_t*>(cac, g_childOff);  // EquipRefreshKeepCloth
        return;
    }
    if (keepOn) s_keepNot.fetch_add(1, std::memory_order_relaxed);
    if (g_on.load(std::memory_order_relaxed) && redundant) {
        s_once.fetch_add(1, std::memory_order_relaxed);
        return;  // the refresh's own CreateChildActor() right after builds the actor (once)
    }
    s_game.fetch_add(1, std::memory_order_relaxed);
    g_setClass(cac, cls, tmpl);
}

bool Fail(const char* why)
{
    Log("[onespawn] %s: EquipRefreshOneSpawn off", why);
    return false;
}

}  // namespace

// true when SetChildActorClass(cac, cls, tmpl) would only destroy and rebuild what the refresh's CreateChildActor() rebuilds
bool EquipRefreshOneSpawnRedundant(const uint8_t* cac, const void* cls, const void* tmpl)
{
    if (tmpl || !cac || !cls || !g_childOff) return false;
    if (ReadAt<const void*>(cac, g_classOff) != cls) return false;
    const uint8_t* const t = ReadAt<const uint8_t*>(cac, g_tplOff);
    if (t && ReadAt<const void*>(t, 0x10) != cls) return false;  // stock would clear a template of another class
    const uint8_t* const child = ReadAt<const uint8_t*>(cac, g_childOff);
    if (!child || (ReadAt<uint32_t>(child, 8) & kDeadFlags)) return false;
    return ReadAt<const void*>(child, 0x10) == cls;
}

bool EquipRefreshOneSpawnLocate(const GameModule& game, EquipOneSpawnSites* out, const char** why)
{
    *out = {};
    const auto hits = FindAll(game.text, ParsePattern(kRefreshBody));
    if (hits.size() != 1) { *why = "the equipment-slot refresh not found once"; return false; }
    uint8_t* const fn = hits[0] - kBodyAt;
    uint8_t* const call = fn + kSetClassCall;
    if (call[0] != 0xE8) { *why = "the refresh's SetChildActorClass call is not a direct call"; return false; }
    uint8_t* target = RipTarget(call, 1, 5);
    if (!game.text.Contains(target)) {  // already ours (a second install) or another mod's
        *why = "the refresh's SetChildActorClass call is already redirected";
        return false;
    }
    int32_t slot = 0;
    memcpy(&slot, fn + kVcallDisp, 4);
    const uint8_t* const st = Within(target, 0x30, ParsePattern(kStoreClass));
    int tails = 0;
    const uint8_t* const tail = Within(target, 0x600, ParsePattern(kSetClassTail), &tails);
    if (!st || tails != 1) { *why = "SetChildActorClass not recognised"; return false; }
    int32_t tailSlot = 0;
    memcpy(&tailSlot, tail + kTailVcall, 4);
    uint32_t tpl[3] = {};
    memcpy(&tpl[0], tail + kTailTpl1, 4);
    memcpy(&tpl[1], tail + kTailTpl2, 4);
    memcpy(&tpl[2], tail + kTailTpl3, 4);
    if (tailSlot != slot || slot <= 0 || slot > 0x2000 || (slot & 7)) {
        *why = "the refresh's virtual call after SetChildActorClass is not SetChildActorClass's own CreateChildActor call";
        return false;
    }
    uint32_t cls = 0, child = 0;
    memcpy(&cls, st + 3, 4);
    const uint8_t* const lc = Within(call, 0x90, ParsePattern(kLoadChild));
    if (!lc || !Within(lc, 0x40, ParsePattern(kDeadTest))) { *why = "the refresh's child actor test not found"; return false; }
    memcpy(&child, lc + 3, 4);
    if (tpl[0] != tpl[1] || tpl[0] != tpl[2] || child != cls + 8 || tpl[0] != cls + 0x10 || cls > 0x1000) {
        *why = "odd component offsets";
        return false;
    }
    const auto sw = FindAll(game.text, ParsePattern(kSwitchBody));
    out->switchPov = sw.size() == 1 ? sw[0] - kSwitchAt : nullptr;  // EquipRefreshKeepActor only (checked at its install)
    out->switchCur = out->switchSkip = 0;
    if (out->switchPov) {
        memcpy(&out->switchSkip, out->switchPov + 0x11, 4);  // cmp byte [rcx+skip],0
        memcpy(&out->switchCur, out->switchPov + 0x29, 4);   // movzx eax, byte [rcx+cur]
    }
    out->vcall = fn + kVcall;
    out->refresh = fn;
    out->call = call;
    out->setClass = target;
    out->vslot = static_cast<uint32_t>(slot);
    out->classOff = cls;
    out->childOff = child;
    out->templateOff = tpl[0];
    return true;
}

namespace {
// EquipRefreshKeepActor: SwitchPOV's entry (scope) and the refresh's CreateChildActor call; false (keep off) with a log line
bool InstallKeep(const EquipOneSpawnSites& s, bool lab)
{
    const GameModule& game = Game();
    auto off = [](const char* why) {
        Log("[onespawn] %s: EquipRefreshKeepActor off", why);
        return false;
    };
    if (!s.switchPov) return off("SwitchPOV not found once");
    if (memcmp(s.switchPov, kSwitchPrologue, sizeof kSwitchPrologue) != 0) return off("SwitchPOV's first bytes changed");
    if (!s.switchCur || !s.switchSkip || s.switchCur > 0x2000 || s.switchSkip > 0x2000) return off("SwitchPOV's view fields odd");
    const uint8_t* const v = s.vcall;
    uint32_t disp = 0;
    memcpy(&disp, v + 2, 4);
    if (v[0] != 0xFF || v[1] != 0x90 || disp != s.vslot) return off("the refresh's CreateChildActor call not as expected");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return off("no memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    // region+0: SwitchPOV's first 10 bytes + jmp back; region+32: jmp SwitchHook; region+48: jmp VcallHook
    memcpy(region, s.switchPov, sizeof kSwitchPrologue);
    memcpy(region + 10, jmpAbs, 6);
    uint8_t* const back = s.switchPov + sizeof kSwitchPrologue;
    memcpy(region + 16, &back, 8);
    uint8_t* const swStub = region + 32;
    memcpy(swStub, jmpAbs, 6);
    void* const swHook = reinterpret_cast<void*>(&SwitchHook);
    memcpy(swStub + 6, &swHook, 8);
    uint8_t* const vcStub = region + 48;
    memcpy(vcStub, jmpAbs, 6);
    void* const vcHook = reinterpret_cast<void*>(&VcallHook);
    memcpy(vcStub + 6, &vcHook, 8);
    DWORD old;
    if (!VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old)) return off("stub protection");
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t relSw = swStub - (s.switchPov + 5), relVc = vcStub - (s.vcall + 5);
    if (relSw != static_cast<int32_t>(relSw) || relVc != static_cast<int32_t>(relVc)) return off("stub out of reach");
    g_vslot = s.vslot;
    g_curOff = s.switchCur;
    g_skipOff = s.switchSkip;
    g_switchOrig = reinterpret_cast<SwitchFn>(region);
    uint8_t vc[6] = {0xE8, 0, 0, 0, 0, 0x90};  // call stub; nop (the 6-byte call [rax+S] it replaces)
    const int32_t vc32 = static_cast<int32_t>(relVc);
    memcpy(vc + 1, &vc32, 4);
    if (!WriteCode(s.vcall, vc)) return off("CreateChildActor call patch");
    g_vcallSite = s.vcall;
    uint8_t jb[5] = {0xE9};
    const int32_t sw32 = static_cast<int32_t>(relSw);
    memcpy(jb + 1, &sw32, 4);
    if (!WriteCode(s.switchPov, jb)) {
        g_vcallSite = nullptr;  // the patched call then always calls CreateChildActor (no skip is armed without the scope)
        return off("SwitchPOV patch");
    }
    g_switch = s.switchPov;
    Log("[onespawn] keep installed: SwitchPOV exe+0x%X (entry; real view changes only: view +0x%X, skip flag +0x%X), the refresh's "
        "CreateChildActor call exe+0x%X (slot +0x%X)%s",
        static_cast<unsigned>(s.switchPov - game.base), s.switchCur, s.switchSkip, static_cast<unsigned>(s.vcall - game.base), s.vslot,
        lab ? " (lab: verify / probe only)" : "");
    return true;
}
}  // namespace

bool InstallEquipRefreshOneSpawn(bool beforeGameCode, int mode, int keep, bool labPov)
{
    if (g_call) return true;
    if (mode == 0 && keep == 0 && !labPov) {
        Log("[onespawn] EquipRefreshOneSpawn and EquipRefreshKeepActor off in OblivionPerfFix.ini (take a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) return Fail("installation requires game startup");
    const GameModule& game = Game();
    EquipOneSpawnSites s;
    const char* why = "";
    if (!EquipRefreshOneSpawnLocate(game, &s, &why)) return Fail(why);
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) return Fail("no memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    void* const hook = reinterpret_cast<void*>(&Hook);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    if (!VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old)) return Fail("stub protection");
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int64_t rel = stub - (s.call + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("stub out of reach");
    g_setClass = reinterpret_cast<SetClassFn>(s.setClass);
    g_classOff = s.classOff;
    g_childOff = s.childOff;
    g_tplOff = s.templateOff;
    g_on.store(mode != 0);
    const int32_t rel32 = static_cast<int32_t>(rel);
    if (!WriteCode(s.call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4))) return Fail("call patch");
    g_call = s.call;
    if (keep != 0 || labPov) InstallKeep(s, labPov && keep == 0);  // after the call patch (review S3-1)
    g_keepOn.store(keep != 0 && g_switch);
    Log("[onespawn] installed: the equipment-slot refresh exe+0x%X's SetChildActorClass call exe+0x%X (-> exe+0x%X, CreateChildActor "
        "slot +0x%X after it and inside it; class +0x%X, child +0x%X, template +0x%X); EquipRefreshOneSpawn %d",
        static_cast<unsigned>(s.refresh - game.base), static_cast<unsigned>(s.call - game.base),
        static_cast<unsigned>(s.setClass - game.base), s.vslot, s.classOff, s.childOff, s.templateOff, mode != 0 ? 1 : 0);
    if (keep != 0 || labPov) Log("[onespawn] EquipRefreshKeepActor %d", g_keepOn.load() ? 1 : 0);
    return true;
}

void SetEquipRefreshKeepActor(int keep)
{
    if (!g_call || !g_switch) return;
    const bool on = keep != 0;
    if (g_keepOn.exchange(on) != on) Log("[onespawn] EquipRefreshKeepActor %d", on ? 1 : 0);
}

// EquipRefreshKeepCloth (dlgedge): true when `obj` is a clothing actor kept by the current view switch on this thread
bool EquipRefreshKeepIsKeptActor(const void* obj)
{
    if (!obj || !InViewSwitch()) return false;  // InViewSwitch is only ever true with SwitchPOV's hook (or the self-test's scope)
    for (int i = 0; i < g_numKept; ++i)
        if (g_keptActors[i] == obj) return true;
    return false;
}

uint32_t EquipRefreshKeepSwitchSerial() { return g_switchSerial; }

bool EquipRefreshKeepLastScope() { return g_lastScope; }
void* EquipRefreshPovCaller() { return g_lastCaller; }
bool EquipRefreshKeepIsOn() { return g_switch && g_keepOn.load(std::memory_order_relaxed); }
bool EquipRefreshKeepInViewSwitch() { return g_switch && InViewSwitch(); }
void EquipRefreshKeepArmSkip(const uint8_t* cac) { if (g_vcallSite) ArmSkip(cac); }
bool EquipRefreshPovHooked() { return g_switch != nullptr; }
void EquipRefreshSetPovLab(bool on) { g_povLab.store(on && g_switch); }
uint64_t EquipRefreshPovGame(uint8_t* ctrl, uint64_t pov, uint64_t remember, uint64_t r9)
{
    return g_switchOrig(ctrl, pov, remember, r9);
}

void SetEquipRefreshOneSpawn(int mode)
{
    if (!g_call) return;
    const bool on = mode != 0;
    if (g_on.exchange(on) != on) Log("[onespawn] EquipRefreshOneSpawn %d", on ? 1 : 0);
}

bool EquipRefreshOneSpawnIsOn() { return g_call && g_on.load(std::memory_order_relaxed); }

uint8_t* EquipRefreshOneSpawnSetClass() { return g_call ? reinterpret_cast<uint8_t*>(g_setClass) : nullptr; }

void EquipRefreshOneSpawnGameCall(uint8_t* cac, void* cls, void* tmpl) { g_setClass(cac, cls, tmpl); }

void LogEquipRefreshOneSpawnStats()
{
    const uint64_t kept = s_kept.exchange(0), keepNot = s_keepNot.exchange(0);
    if (kept || keepNot)
        Log("[onespawn] %llu same-class refreshes in a view switch kept their clothing actor (EquipRefreshKeepActor), %llu others in a "
            "view switch left to the game",
            static_cast<unsigned long long>(kept), static_cast<unsigned long long>(keepNot));
    const uint64_t once = s_once.exchange(0), game = s_game.exchange(0);
    if (!once && !game) return;
    Log("[onespawn] %llu same-class equipment refreshes built their clothing once (the game builds it twice), %llu the game's way",
        static_cast<unsigned long long>(once), static_cast<unsigned long long>(game));
}

// self-test: the hook against a fake SetChildActorClass; returns the previous target (to restore with a second call)
void* EquipRefreshOneSpawnTestSetup(void* fakeSetClass, uint32_t classOff, uint32_t childOff, uint32_t tplOff)
{
    void* const prev = reinterpret_cast<void*>(g_setClass);
    g_setClass = reinterpret_cast<SetClassFn>(fakeSetClass);
    g_classOff = classOff;
    g_childOff = childOff;
    g_tplOff = tplOff;
    return prev;
}
void* EquipRefreshOneSpawnTestHook() { return reinterpret_cast<void*>(&Hook); }
// self-test (EquipRefreshKeepActor): the view-switch scope by hand, the CreateChildActor hook, the trampoline
void EquipRefreshKeepTestScope(bool in)
{
    if (in) {
        g_povThread = GetCurrentThreadId();
        if (g_povDepth == 0) { g_numKept = 0; ++g_switchSerial; }
        ++g_povDepth;
    } else if (g_povDepth > 0 && --g_povDepth == 0) {
        g_numKept = 0;
    }
}
void EquipRefreshKeepTestKept(const void* actor)  // self-test: an actor kept by the current (test) view switch
{
    if (g_numKept < kMaxKept) g_keptActors[g_numKept++] = static_cast<const uint8_t*>(actor);
}
void* EquipRefreshKeepTestVcallHook() { return reinterpret_cast<void*>(&VcallHook); }
void* EquipRefreshKeepTestSwitchHook() { return reinterpret_cast<void*>(&SwitchHook); }
void* EquipRefreshKeepTestSwitchOrig(void* fake)  // swap the trampoline for a fake; returns the previous one
{
    void* const prev = reinterpret_cast<void*>(g_switchOrig);
    g_switchOrig = reinterpret_cast<SwitchFn>(fake);
    return prev;
}
uint32_t EquipRefreshKeepTestViewOff() { return g_curOff; }
uint32_t EquipRefreshKeepTestSkipOff() { return g_skipOff; }
uint8_t* EquipRefreshKeepTestSwitchTrampoline() { return reinterpret_cast<uint8_t*>(g_switchOrig); }

}  // namespace opf
