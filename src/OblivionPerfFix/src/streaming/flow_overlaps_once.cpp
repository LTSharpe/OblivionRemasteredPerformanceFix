// FlowOverlapsOnce (lab otflows 2026-10-26; equivalence check flowonce / flowonce2 2026-11-15/16; public tree flowport
// 2026-11-16, S-007; [Fixes] FlowOverlapsOnce, default 0 until the release; installed at start-up when != 0 (lab build:
// always, so it can be switched live), then live 0/1).
//
// What the game does: when a cell is added to the world, the old engine adopts each of its pre-placed pairable actors
// (flora, pickables, clutter, furniture, doors, containers) in one call on the game thread (exe+0x472EFB0, the level-flow
// adoption command). Inside that one call it changes each of the actor's primitive components' collision profile four
// times and puts it back where it started (a pickable: VPInteractable -> BlockAllDynamic -> VPInteractable -> NoCollision
// -> VPInteractable). UE 5.3's UPrimitiveComponent::SetCollisionProfileName(Name, bUpdateOverlaps = true) ->
// OnComponentCollisionSettingsChanged runs a full UpdateOverlaps (an overlap query for the component and its attached
// children) after each change: four queries per component where the last one alone decides the result; the overlap work
// is most of the adoptions' time (lab measurements: findings 2026-10-26 "otflows", 2026-11-16 "flowport").
//
// What this changes: while an adoption call runs (outermost call, game thread, key on), the UpdateOverlaps call inside
// OnComponentCollisionSettingsChanged (exe+0x35884B0 +0x5F; reached only for a registered, non-template component whose
// settings changed and whose caller asked for the update) is not run for a component that overlapped nothing when the
// adoption first asked to update it. The component is remembered once, in the order of its last request. A component that
// already overlaps something keeps every update in place, as the game does (an existing overlap is ended and begun again
// by the profile changes exactly as before). When the adoption call returns, each remembered component that is still
// valid and registered gets that same call, UpdateOverlaps(nullptr, bDoNotifies = true, nullptr), once, in that order.
// Nothing else in the adoption changes: the profile switches, the physics filter data and the bodies are the game's.
//
// Engine analogue (Epic, UE 5.3 SceneComponent.cpp USceneComponent::UpdateOverlaps / EndScopedMovementUpdate; the same in
// 5.7): inside an FScopedMovementUpdate the engine itself does not query overlaps after a change; it marks the component
// and runs one UpdateOverlaps when the scope ends, so overlaps that exist only between two changes inside the scope are
// never begun or ended. This applies that "update overlaps once at the end" to the adoption call, which Altar did not
// wrap in a scope.
//
// Why the end state is the game's: an overlap update reconciles a component's OverlappingComponents with a fresh query at
// its current settings and position (pairs kept symmetric by Begin/EndComponentOverlap; the filter checks both sides'
// responses). Inside the adoption nothing else runs on the game thread (no tick, physics step or other flow), overlap
// state is game-thread-only, and UObjects are freed only by garbage collection (never inside this call), so after the
// deferred updates every component's overlap set, its partners' sets and its physics volume are those of a query at its
// final settings, which is what the game's last update of it computes. Measured with a lab state check: the overlap
// partners and water state of every adopted actor at the end of each adoption matched the game's (within the game's own
// run-to-run variation on a short ride, identical in every adoption of a longer one).
// What differs: Begin/End overlap notifications fire for the net change only, at the end of the call; a pair that the
// game began in the intermediate BlockAllDynamic profile and ended again inside the same call is not notified at all.
// The receivers of those notifications in this game (every native overlap delegate binding and NotifyActorBegin/EndOverlap
// override in the exe, and the Blueprint receivers met on our test routes) were read: the weather texture boxes, audio
// trigger boxes, door preload boxes, light-rig volumes and the butterfly and crawling-insect Blueprints react to the
// player only; trap and pawn receivers to characters only; the water state of a placed object
// (UVPhysicsControllerComponent) is set by the Begin and cleared by the End of the same water body (net zero); a trap hit
// box's target list gets the actor added and removed again (net zero; the hit box's tick, which the add switches on, does
// nothing with an empty list). These dropped pairs therefore leave nothing behind that we found. Known order difference: the in-place updates of components that already
// overlapped run before the end-of-adoption ones, so a receiver where the last Begin wins (a placed object's water body
// when two water bodies overlap it) can end on the other one; not seen beyond the game's own run-to-run variation.
//
// How: the adoption's entry and the call at +0x5F are found by pattern (one match each; UpdateOverlaps' prologue checked;
// the overlap list offset read from BeginComponentOverlap); the entry's first 6 bytes go to a trampoline, the entry jumps
// to the hook, the call goes to the deferral. Any mismatch: not installed, one log line.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// Altar's pre-placed actor adoption (exe+0x472EFB0; 14 integer-class arguments): mov rax,rsp; push rbp; push r14;
// lea rbp,[rax-17h]; sub rsp,0E8h; ... mov rcx,[rip+x]; mov [rbp-61h],rax; call
constexpr char kAdopt[] =
    "48 8B C4 55 41 56 48 8D 68 E9 48 81 EC E8 00 00 00 48 89 58 E8 4C 8B F2 48 89 70 E0 33 D2 48 89 78 D8 41 8B F1 "
    "4C 89 60 D0 41 8B F8 48 8B 85 87 00 00 00 4C 8B E1 48 8B 0D ?? ?? ?? ?? 48 89 45 9F E8";
constexpr size_t kAdoptEntry = 6;  // mov rax,rsp; push rbp; push r14: whole instructions, position-independent
// UPrimitiveComponent::OnComponentCollisionSettingsChanged(bool bUpdateOverlaps) (exe+0x35884B0): test byte [rcx+8Ch],1
// (bRegistered); IsTemplate (edx = 30h); GetCollisionEnabled (vt+5B0h); test dil,dil; xor r9d,r9d; mov r8b,1;
// xor edx,edx; mov rcx,rbx; call UpdateOverlaps (+0x5F)
constexpr char kSettingsChanged[] =
    "48 89 5C 24 08 57 48 83 EC 20 F6 81 8C 00 00 00 01 0F B6 FA 48 8B D9 0F 84 ?? ?? ?? ?? BA 30 00 00 00 E8 ?? ?? ?? ?? "
    "84 C0 75 ?? 48 8B 03 48 8B CB FF 90 B0 05 00 00 8D 48 FF F7 C1 F9 FF FF FF 75 ?? 83 F8 07 74 ?? 48 8B CB E8 ?? ?? ?? ?? "
    "40 84 FF 74 ?? 45 33 C9 41 B0 01 33 D2 48 8B CB E8 ?? ?? ?? ??";
constexpr size_t kUpdateCall = 0x5F;
// USceneComponent::UpdateOverlaps(PendingOverlaps, bDoNotifies, OverlapsAtEndLocation) (exe+0x3092010): push rbx; sub rsp,20h
constexpr char kUpdateOverlaps[] = "40 53 48 83 EC 20";
// UPrimitiveComponent::BeginComponentOverlap (exe+0x3022670): ... lea rbx,[rcx+OverlappingComponents] (+0x43, disp32 at +0x46)
constexpr char kBeginOverlap[] =
    "48 85 C9 0F 84 ?? ?? ?? ?? 44 88 44 24 18 55 56 41 54 41 55 48 8D AC 24 D8 FB FF FF 48 81 EC 28 05 00 00 8B 41 08 "
    "45 0F B6 E0 4C 8B EA 48 8B F1 A9 00 00 00 60 0F 85 ?? ?? ?? ?? 48 89 9C 24 58 05 00 00 48 8D 99 ?? ?? ?? ?? 48 8B CB E8";
constexpr size_t kOverlapListDisp = 0x46;
constexpr uint32_t kRegistered = 0x8C;  // UActorComponent::bRegistered (bit 0), the byte the matched test reads
constexpr uint32_t kObjectFlags = 0x08, kInvalidFlags = 0x60000000;  // RF_PendingKill | RF_Garbage (UObject flags)

using AdoptFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

AdoptFn g_adopt = nullptr;            // trampoline: the adoption's first instructions, then a jump back
const uint8_t* g_tramp = nullptr;     // the trampoline (kept for the self-test when g_adopt is replaced by a fake)
FlowOverlapsUpdateFn g_update = nullptr;  // UpdateOverlaps
uint32_t g_overlapList = 0;           // UPrimitiveComponent::OverlappingComponents (TArray: data, Num at +8)
std::atomic<int> g_mode{0};           // the ini's value (0 the game's way, 1 once at the end)
bool g_installed = false;
uint8_t* g_adoptAt = nullptr;
uint8_t* g_siteAt = nullptr;

// game thread only (the adoption and its overlap updates run there)
thread_local int t_depth = 0;
thread_local bool t_batch = false;
constexpr int kMaxPending = 512;
thread_local void* t_pending[kMaxPending];
thread_local int t_nPending = 0;
thread_local bool t_overflow = false;
thread_local const void* t_inPlace[kMaxPending];  // components that had overlaps when first seen: updated in place (stock)
thread_local int t_nInPlace = 0;

struct Stats {
    std::atomic<uint64_t> adoptions{}, batched{}, deferred{}, inPlace{}, flushed{}, skipped{}, overflows{};
};
Stats s;

int Mode()
{
    return g_mode.load(std::memory_order_relaxed);
}

int OverlapCount(const void* comp)
{
    __try {
        return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(comp) + g_overlapList + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool StillValid(const void* comp)
{
    __try {
        const uint8_t* const c = static_cast<const uint8_t*>(comp);
        return (*reinterpret_cast<const uint32_t*>(c + kObjectFlags) & kInvalidFlags) == 0 && (c[kRegistered] & 1) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the call at OnComponentCollisionSettingsChanged +0x5F: UpdateOverlaps(nullptr, true, nullptr)
uint64_t SettingsChangedUpdate(void* comp, const void* pending, uint64_t doNotifies, const void* atEnd)
{
    if (!t_batch || pending || atEnd || (doNotifies & 0xFF) != 1) return g_update(comp, pending, doNotifies, atEnd);
    if (t_nPending >= kMaxPending) {  // more than this in one adoption: from here on as the game does
        t_overflow = true;
        return g_update(comp, pending, doNotifies, atEnd);
    }
    int i = 0;
    while (i < t_nPending && t_pending[i] != comp) ++i;
    if (i == t_nPending) {
        // first request of this component in this adoption (or it was seen with overlaps before): only a component that
        // overlaps nothing is deferred, so an overlap that exists already is ended and begun again exactly as in the game
        int k = 0;
        while (k < t_nInPlace && t_inPlace[k] != comp) ++k;
        if (k < t_nInPlace || OverlapCount(comp) != 0) {
            if (k == t_nInPlace && t_nInPlace < kMaxPending) t_inPlace[t_nInPlace++] = comp;
            s.inPlace.fetch_add(1, std::memory_order_relaxed);
            return g_update(comp, pending, doNotifies, atEnd);
        }
    } else {  // requested again: keep the order of the last request
        memmove(t_pending + i, t_pending + i + 1, (t_nPending - i - 1) * sizeof(void*));
        --t_nPending;
    }
    t_pending[t_nPending++] = comp;
    s.deferred.fetch_add(1, std::memory_order_relaxed);
    return 0;  // the caller does not use the result (its next instruction reloads rax)
}

void Flush()
{
    // t_batch is off while these run: settings changes they cause (none expected) update in place as the game does
    uint64_t n = 0, skipped = 0;
    for (int i = 0; i < t_nPending; ++i) {
        void* const comp = t_pending[i];
        if (!StillValid(comp)) { ++skipped; continue; }
        g_update(comp, nullptr, 1, nullptr);
        ++n;
    }
    t_nPending = 0;
    s.flushed.fetch_add(n, std::memory_order_relaxed);
    s.skipped.fetch_add(skipped, std::memory_order_relaxed);
}

uint64_t AdoptHook(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9,
                   uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15, uint64_t a16)
{
    const bool outer = t_depth == 0;
    const bool batch = outer && Mode() == 1;
    if (batch) {
        t_batch = true;
        t_nPending = 0;
        t_nInPlace = 0;
        t_overflow = false;
    }
    ++t_depth;
    const uint64_t r = g_adopt(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
    if (batch) {
        // the depth stays raised while the end-of-adoption updates run: their notifications call receivers, and an
        // adoption one of them might start is then nested (run as the game runs it, t_batch being off), never a new
        // outermost one that would reset this call's pending list (review rev106c S1)
        t_batch = false;
        if (t_overflow) s.overflows.fetch_add(1, std::memory_order_relaxed);
        Flush();
        s.batched.fetch_add(1, std::memory_order_relaxed);
    }
    --t_depth;
    if (outer) {
        s.adoptions.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

bool Discover(const GameModule& game, FlowOverlapsDiscovery* d, const char** why)
{
    const auto adopt = FindAll(game.text, ParsePattern(kAdopt));
    const auto changed = FindAll(game.text, ParsePattern(kSettingsChanged));
    const auto begin = FindAll(game.text, ParsePattern(kBeginOverlap));
    d->adopts = static_cast<int>(adopt.size());
    d->changes = static_cast<int>(changed.size());
    d->begins = static_cast<int>(begin.size());
    if (adopt.size() != 1 || changed.size() != 1 || begin.size() != 1) { *why = "code not recognised (pattern count)"; return false; }
    uint32_t off = 0;
    memcpy(&off, begin[0] + kOverlapListDisp, 4);
    if (off < 0x100 || off > 0x1000) { *why = "overlap list offset unexpected"; return false; }
    uint8_t* const site = changed[0] + kUpdateCall;
    uint8_t* const update = RipTarget(site, 1, 5);
    if (!game.text.Contains(update) || !MatchesAt(update, ParsePattern(kUpdateOverlaps))) {
        *why = "UpdateOverlaps not recognised";
        return false;
    }
    d->adopt = adopt[0];
    d->site = site;
    d->update = update;
    d->overlapList = off;
    return true;
}

bool Install(const GameModule& game, const char** why)
{
    FlowOverlapsDiscovery d{};
    if (!Discover(game, &d, why)) return false;
    uint8_t* const mem = AllocNearGame(128);
    if (!mem) { *why = "no memory near the game's code"; return false; }
    // mem+0: trampoline (the adoption's first 6 bytes, jmp [rip] back); +32: jump to AdoptHook; +64: jump to
    // SettingsChangedUpdate (the patched call's target)
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = d.adopt + kAdoptEntry;
    memcpy(mem, d.adopt, kAdoptEntry);
    memcpy(mem + kAdoptEntry, jmpAbs, 6);
    memcpy(mem + kAdoptEntry + 6, &resume, 8);
    const void* const h1 = reinterpret_cast<void*>(&AdoptHook);
    const void* const h2 = reinterpret_cast<void*>(&SettingsChangedUpdate);
    memcpy(mem + 32, jmpAbs, 6);
    memcpy(mem + 32 + 6, &h1, 8);
    memcpy(mem + 64, jmpAbs, 6);
    memcpy(mem + 64 + 6, &h2, 8);
    DWORD old;
    if (!VirtualProtect(mem, 128, PAGE_EXECUTE_READ, &old)) { *why = "could not make the stubs executable"; return false; }
    FlushInstructionCache(GetCurrentProcess(), mem, 128);
    const auto rel = [](const uint8_t* from, const uint8_t* to, int32_t* out) {
        const int64_t v = to - (from + 5);
        if (v != static_cast<int32_t>(v)) return false;
        *out = static_cast<int32_t>(v);
        return true;
    };
    int32_t r1, r2;
    if (!rel(d.site, mem + 64, &r1) || !rel(d.adopt, mem + 32, &r2)) { *why = "out of reach"; return false; }
    g_adopt = reinterpret_cast<AdoptFn>(mem);
    g_tramp = mem;
    g_update = reinterpret_cast<FlowOverlapsUpdateFn>(d.update);
    g_overlapList = d.overlapList;
    // the call site first (it does nothing different while no adoption batches), then the adoption's entry jump
    uint8_t call[5] = {0xE8};
    memcpy(call + 1, &r1, 4);
    if (!WriteCode(d.site, std::span<const uint8_t>(call, 5))) { *why = "could not patch the call"; return false; }
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    memcpy(jmp + 1, &r2, 4);
    if (!WriteCode(d.adopt, std::span<const uint8_t>(jmp, kAdoptEntry))) {
        *why = "could not patch the adoption (the call stays redirected, which changes nothing while no adoption batches)";
        return false;
    }
    g_adoptAt = d.adopt;
    g_siteAt = d.site;
    g_installed = true;
    return true;
}

}  // namespace

bool FlowOverlapsDiscover(const GameModule& game, FlowOverlapsDiscovery* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool FlowOverlapsTestInstall()
{
    const char* why = "";
    return g_installed || Install(Game(), &why);
}

void FlowOverlapsTestSetFakes(FlowOverlapsAdoptFn adopt, FlowOverlapsUpdateFn update)
{
    g_adopt = reinterpret_cast<AdoptFn>(adopt);
    g_update = update;
}

uint64_t FlowOverlapsTestAdopt(uint64_t a1) { return AdoptHook(a1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }

uint64_t FlowOverlapsTestRequest(void* comp) { return SettingsChangedUpdate(comp, nullptr, 1, nullptr); }

void FlowOverlapsTestCounts(uint64_t* deferred, uint64_t* inPlace, uint64_t* flushed, uint64_t* skipped)
{
    *deferred = s.deferred.load();
    *inPlace = s.inPlace.load();
    *flushed = s.flushed.load();
    *skipped = s.skipped.load();
}

uint32_t FlowOverlapsListOffset() { return g_overlapList; }

const uint8_t* FlowOverlapsTestTrampoline() { return g_tramp; }

void SetFlowOverlapsOnce(int mode) { g_mode.store(mode == 1 ? 1 : 0, std::memory_order_relaxed); }


// Start-up only (version.dll route): the adoption's entry is patched before any level flow runs.
bool InstallFlowOverlapsOnce(bool beforeGameCode, int mode)
{
    if (g_installed) return true;
    if (!mode) {
        Log("[flowov] FlowOverlapsOnce off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[flowov] FlowOverlapsOnce: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const char* why = "";
    if (!Install(Game(), &why)) {
        Log("[flowov] FlowOverlapsOnce: %s, not installed", why);
        return false;
    }
    SetFlowOverlapsOnce(mode);
    Log("[flowov] FlowOverlapsOnce installed (adoption exe+0x%X, its overlap update call exe+0x%X): FlowOverlapsOnce=%d",
        static_cast<unsigned>(g_adoptAt - Game().base), static_cast<unsigned>(g_siteAt - Game().base), g_mode.load());
    return true;
}

void LogFlowOverlapsOnceStats()
{
    if (!g_installed) return;
    static uint64_t last = 0;
    const uint64_t a = s.adoptions.load();
    if (a == last || s.batched.load() == 0) return;
    last = a;
    Log("[flowov] %llu adoptions (%llu with FlowOverlapsOnce); overlap updates moved to the end %llu, run there %llu, skipped "
        "(no longer valid) %llu, kept in place (component already overlapped) %llu, adoptions past %d requests %llu",
        static_cast<unsigned long long>(a), static_cast<unsigned long long>(s.batched.load()),
        static_cast<unsigned long long>(s.deferred.load()), static_cast<unsigned long long>(s.flushed.load()),
        static_cast<unsigned long long>(s.skipped.load()), static_cast<unsigned long long>(s.inPlace.load()), kMaxPending,
        static_cast<unsigned long long>(s.overflows.load()));
}

}  // namespace opf
