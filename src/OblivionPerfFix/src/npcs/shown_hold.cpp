// PacingGateShownHoldMs / PacingGateShownHoldEnd (hold2 2026-11-13, review rev96g, holdfix, build97; public since 1.5.0,
// default 0 until the release flips it; 1000 / 10 when on): a visual-only hold of a far character's spawn fade-in while
// PacingBaseGate holds its heavy units (appearance / equipment). Part of NpcCreationPacing; needs PacingBaseGate.
//
// The game: every character whose UE actor spawns fades in by dither. UVCharacterFadeInOutComponent (fade component)
// keeps a value (+0xBC) that its tick (exe+0x48D5A40, state 1 = FadeIn) lowers from the max (+0xD0, 5) by rate (+0xC0,
// 4/s); at 0 the fade ends (state 0) and the end call switches the character's AI controller tick on (rev96e B1). The
// value reaches the meshes only through one function, the applier exe+0x48CD850
// Apply(component, owner, bool doDither, float scale): every primitive of the owner (and attached actors) gets
// SetDoDither(doDither || scale > 0) and SetDitherScale(scale) (static meshes: a flag only). Its callers are every native
// writer of a character's dither: the tick (max(floor, value)), SetFadeState, the old engine's actor alpha (the floor
// setter exe+0x48CCE90: floor alone), the appearance refresh's floor re-apply (exe+0x48C8060: floor alone), the
// camera-occluder fade, and the Blueprint UFUNCTION SetDitherOnCharacterVisibleComponents.
//
// Why: with PacingBaseGate a far character's appearance can wait for a frame with room while its spawn fade-in already
// runs, so its placeholder body starts to show (the gate's own "faint first stage"); units forced out at the fade's end
// landed in slow frames. The hold keeps such a body hidden until its units ran and makes the gate run them before the
// fade ends (the due rule in npc_pacing.cpp). Imperial City flight: units forced into a slow frame 3-5 per flight with the
// key on against 6-10 off.
//
// The hold: an entry detour on the applier raises only the float it is handed, for one character at a time: while a far
// character in its spawn fade-in (state 1) has heavy units held by the gate, what reaches its meshes stays at the fade's
// max (fully dithered: the stock first frame of every spawn), so a character whose appearance has not been built
// (placeholder body) is not drawn. A hold opens only in the fade's first quarter unit (game value >= max - 0.25, at most
// ~5 % stipple shown before), so nothing visible is hidden again. It ends when the actor leaves the gate's hold list (its
// units ran, or it came near / the gate let them go), when the hold has lasted PacingGateShownHoldMs (wall time), or when
// the game's own value reaches PacingGateShownHoldEnd + the soft window (0.5) in time at the fade's own rate (End 10:
// value 1.5, 0.875 s after the fade started at 4 per second), whichever is first. Then the shown level falls with the
// game's value, H = H_release x value / value_release (never rising): H_release is the max (5), so the shown fade runs
// 5 / value_release times the stock fade's speed (End 10: 3.3x; End 5: 5x; more when a long frame jumps the value below
// the threshold; holdfix F6), and the meshes get max(the writer's own value, H): H reaches 0 exactly when the game's fade
// ends, and the end call (state already 0) passes unchanged, so the meshes are left exactly as the game leaves them, at
// the game's time. Nothing the game reads is changed: the fade component's value, floor, state, rate, its end and the
// controller switch, and the old engine's actor alpha are untouched; only the dither handed to the meshes is raised
// (never lowered), so the character is never more visible than in the game at any moment, for every writer. What changes
// is WHEN a held far character becomes visible: later in its fade-in, then faster.
// PacingBaseGate reads the shown level (FadeLevel: max(game level, H)), so a held character does not count as "about to
// show". Off (0), not installed, not on the game thread, an unexpected state, a fault: the game's value, unchanged.
// Start-up install (5 bytes at the applier's entry, before the game's code runs: not on the OBSE64 route); the key is
// live afterwards.
//
// Review fixes in this file: F3 (every own fade end with units still queued is counted, "AI before built"), F4 (an
// equipment unit run before the appearance is counted, and the actor's open hold gives its queued units a deadline 0.1 s
// after that unit: ShownHoldUrgentOwners; fix102 = rev102a M1, before: due at once; also equipment that ran just before
// the hold opened, S3a), F5 (a slot of an actor destroyed mid-hold is reused after 2 s; fix102 M2: swept every 0.25 s, and
// a slot not called for 0.1 s or with another object at its owner's address is never urgent), F6 (speed text), F7
// (thresholds in time at the fade's own rate; the rate and max are logged when not 4 / 5), F8 (the layout is
// cross-checked with pacing's, the rate at value + 4 included: a mismatch leaves the key off). The due rule (F1, F2, F9,
// the spread, F4's deadline in the spread) is in npc_pacing.cpp.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace opf {
namespace {

// The applier (exe+0x48CD850): push rbp; push rsi; push rdi; push r12; push r13; lea rbp,[rsp-240h]; sub rsp,340h;
// movaps [rsp+310h],xmm7; mov rax,[cookie]; xor rax,rsp; mov [rbp+200h],rax; lea rsi,[rcx+list]; xor edi,edi;
// movaps xmm7,xmm3; movzx r12d,r8b; mov r13,rdx; mov [rsi+8],edi; cmp [rsi+0Ch],edi
constexpr char kApplier[] =
    "40 55 56 57 41 54 41 55 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
    "48 33 C4 48 89 85 ?? ?? ?? ?? 48 8D B1 ?? ?? 00 00 33 FF 0F 28 FB 45 0F B6 E0 4C 8B EA 89 7E 08 39 7E 0C";
constexpr size_t kPrologue = 6;  // push rbp (40 55); push rsi; push rdi; push r12 (41 54): position independent

// The tick body's fixed instructions (from its start, build 19115871): the component layout is read from them.
struct BodyField {
    size_t at;
    const char* bytes;  // opcode bytes before the disp32
    size_t len;
};
constexpr BodyField kState = {0x0D, "\x8B\x89", 2};              // mov ecx,[rcx+state]
constexpr BodyField kRate = {0x25, "\xF3\x0F\x59\x8B", 4};       // mulss xmm1,[rbx+rate]
constexpr BodyField kValue = {0x2D, "\xF3\x0F\x58\x8B", 4};      // addss xmm1,[rbx+value]
constexpr BodyField kMax = {0x35, "\x0F\x2F\x8B", 3};            // comiss xmm1,[rbx+max]
constexpr BodyField kOwner = {0x5B, "\x48\x8B\xBB", 3};          // mov rdi,[rbx+owner]
constexpr BodyField kActorComp = {0x96, "\x48\x8B\x87", 3};      // mov rax,[rdi+actor's fade component]
constexpr BodyField kFloor = {0xA6, "\xF3\x0F\x10\x98", 4};      // movss xmm3,[rax+floor]
constexpr BodyField kValue2 = {0xAE, "\xF3\x0F\x5F\x9B", 4};     // maxss xmm3,[rbx+value]
constexpr size_t kBodyTailJmp = 0xC0;                            // jmp applier (FadeOut's tail call)

using ApplyFn = void (*)(uint8_t* comp, uint8_t* owner, bool doDither, float scale);

struct Layout {
    uint32_t state = 0, rate = 0, value = 0, max = 0, owner = 0, actorComp = 0, floor = 0;
};

enum : uint8_t { kFree = 0, kHold, kConv, kDone };
enum : uint8_t { kWhyNone = 0, kWhyRan, kWhyBound, kWhyClock };

struct Slot {
    uintptr_t comp, owner;
    uint64_t openUs;
    float h, hRel, vRel, lastV;
    uint8_t phase, why;
    float onFirst;   // the newest LastRenderTimeOnScreen of the fade list's meshes at the first look during the hold
    uint8_t onSeen;  // 0 no look yet, 1 looked, 2 drawn on screen during the hold (a newer on-screen time seen)
    uint64_t lastUs;  // holdfix F5: the time of its last applier call (a slot of a destroyed actor goes stale)
    uint8_t eqFirst;  // holdfix F4: an equipment unit ran while the hold was open and the appearance unit had not
    uint64_t eqUs;    // fix102 (rev102a M1): when that equipment unit ran (the actor's units are due eqUs + kEqDeadlineUs)
    int32_t ownerIdx;  // fix102 (rev102a M2): the owner's UObject InternalIndex at the hold's open (address reuse check)
    uint8_t eqLate;    // fix102: listed past its deadline (counted once)
};
constexpr int kSlots = 64;
constexpr float kOpenWindow = 0.25f;  // a hold opens only while the game's value is within this of the max
constexpr uint64_t kStaleUs = 2000000;  // holdfix F5: a hold slot not called for this long is reused
constexpr int kFading = 128;            // holdfix F3: own components seen in their spawn fade-in (for the end call)
constexpr float kStockRate = 4.0f;      // the fade rate the thresholds were set for (per second)
// fix102 (rev102a M1): an open hold whose equipment ran before its appearance gives the actor's queued units a deadline
// this long after that equipment unit instead of "due at once": until then the gate's room rule may run them as before.
// Play log of build97 (2026-11-15, 42 holds, 34 of them equipment-first): holds lasted 41-85 ms on average (max 208), so
// usually the room rule runs the appearance before the deadline and nothing is forced; units still queued go in the
// spread's rate (soft-due, deadline order), never all in one replay. About 0.1-0.2 s of a weapon on a hidden far body
// instead of up to ~0.9 s.
constexpr uint64_t kEqDeadlineUs = 100000;
// fix102 (rev102a M2): a held slot not called for this long is not live (a held fade calls the applier every frame): the
// equipment-first mark and the urgent list ignore it; the sweep frees slots not called for kStaleUs
constexpr uint64_t kLiveUs = 100000;
constexpr uint64_t kSweepEveryUs = 250000;
// fix102 (rev102a S3a): an equipment unit that ran before the actor's hold opened is remembered this long by owner (the
// hold opens within the fade's first quarter unit, ~62 ms at the stock rate)
constexpr uint64_t kEqNoteUs = 500000;
constexpr int kEqNotes = 16;
struct EqNote {
    uintptr_t owner;
    int32_t idx;
    uint64_t us;
};

struct State {
    bool found = false, installed = false;
    uint8_t* applier = nullptr;
    ApplyFn orig = nullptr;
    Layout l;
    Slot slot[kSlots] = {};
    int active = 0;  // slots in kHold / kConv
    // self-test
    bool (*testHeld)(uintptr_t owner) = nullptr;
    uint64_t (*testNow)() = nullptr;
    int testGt = -1;  // -1 real, 0 / 1 forced
    bool (*testQueued)(uintptr_t) = nullptr;
    uint32_t list = 0, onScreen = 0;  // fade list (+0xE0) and UPrimitiveComponent::LastRenderTimeOnScreen
    uint32_t epoch = 0;
    // holdfix F3: own fade components with a call in state 1 (comp 0: empty), held or not
    uintptr_t fadeComp[kFading] = {}, fadeOwner[kFading] = {};
    int fadeNext = 0;
    // holdfix F7: the fade rate and max last logged (logged at the first call and when they differ from 4 / 5)
    float loggedRate = -1.0f, loggedMax = -1.0f;
    int rateLogs = 0;
    EqNote eqNote[kEqNotes] = {};  // fix102 (S3a): equipment that ran before the hold opened
    int eqNext = 0;
    uint64_t sweptUs = 0;  // fix102 (M2): the last sweep
};
State a;
std::atomic<int> g_holdMs{0};
std::atomic<int> g_endTenths{10};  // PacingGateShownHoldEnd: the game's fade value (tenths) where held units are due now
constexpr float kDueSoft = 0.5f;   // the gate's soft window above it (npc_pacing kGateDueSoft): the hold ends there at the latest
std::atomic<uint32_t> g_epoch{0};  // bumped when the key goes to 0: the game thread drops every slot at its next call
// per stats interval (game thread writes, stats thread reads and zeroes)
std::atomic<uint64_t> s_calls{0}, s_raised{0}, s_opened{0}, s_late{0}, s_full{0}, s_relRan{0}, s_relBound{0}, s_relClock{0},
    s_ended{0}, s_dropped{0}, s_below{0}, s_offGt{0}, s_faults{0}, s_holdUsSum{0}, s_holdUsMax{0}, s_dueNow{0}, s_dueSoft{0},
    s_aiEarly{0}, s_drawn{0}, s_notDrawn{0}, s_aiEarlyHeld{0}, s_reused{0}, s_eqFirst{0}, s_dueReplayMax{0}, s_swept{0},
    s_eqBefore{0}, s_eqDeadline{0};

uint64_t NowUs()
{
    if (a.testNow) return a.testNow();
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart / f.QuadPart * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart);
}

bool OnGt() { return a.testGt >= 0 ? a.testGt != 0 : NpcPacingOnGameThread(); }
bool Held(uintptr_t owner) { return a.testHeld ? a.testHeld(owner) : NpcGateHoldsActor(owner); }

void AtomicMaxU(std::atomic<uint64_t>& m, uint64_t v)
{
    uint64_t c = m.load(std::memory_order_relaxed);
    while (v > c && !m.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
    }
}

// fix102 (rev102a M2): the owner's UObject InternalIndex (UObjectBase +0xC; stable for the object's life, a different
// object at a reused address has its own); INT32_MIN when it cannot be read
int32_t OwnerIndex(uintptr_t owner)
{
    if (!owner) return INT32_MIN;
    __try {
        return *reinterpret_cast<const int32_t*>(owner + 0xC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return INT32_MIN;
    }
}

// fix102 (rev102a M2): a held slot that still belongs to a live hold of the same object (called within kLiveUs, the
// owner's index unchanged): only such a slot is marked or listed as urgent
bool LiveHold(const Slot& s, uint64_t now)
{
    return s.phase == kHold && now >= s.lastUs && now - s.lastUs < kLiveUs && OwnerIndex(s.owner) == s.ownerIdx;
}

Slot* FindSlot(uintptr_t comp)
{
    if (!comp) return nullptr;
    for (Slot& s : a.slot)
        if (s.phase != kFree && s.comp == comp) return &s;
    return nullptr;
}

void FreeSlot(Slot* s)
{
    if (s->phase == kHold || s->phase == kConv) --a.active;
    s->phase = kFree;
    s->comp = s->owner = 0;
}

Slot* TakeSlot(Slot& s, uintptr_t comp, uintptr_t owner, uint64_t now)
{
    s = Slot{};
    s.comp = comp, s.owner = owner, s.lastUs = now;
    s.phase = kFree, s.why = kWhyNone;
    return &s;
}

Slot* AllocSlot(uintptr_t comp, uintptr_t owner, uint64_t now)
{
    for (Slot& s : a.slot)
        if (s.phase == kFree) return TakeSlot(s, comp, owner, now);
    // reuse a finished one (its spawn is past the hold; it only kept that spawn from being held twice)
    for (Slot& s : a.slot)
        if (s.phase == kDone) return TakeSlot(s, comp, owner, now);
    // holdfix F5 (rev96g): a held slot not called for kStaleUs belongs to an actor destroyed mid-hold (cell unload, a save
    // loaded): without this the table fills after 64 of them and the key silently stops holding
    for (Slot& s : a.slot)
        if ((s.phase == kHold || s.phase == kConv) && now > s.lastUs && now - s.lastUs >= kStaleUs) {
            --a.active;
            s_reused.fetch_add(1, std::memory_order_relaxed);
            return TakeSlot(s, comp, owner, now);
        }
    return nullptr;
}

// fix102 (rev102a M2): held / converging slots not called for kStaleUs (an actor destroyed mid-hold) are freed every
// kSweepEveryUs, not only when the table is full: a stale slot no longer keeps the hook active or its address urgent
void SweepStale(uint64_t now)
{
    if (now >= a.sweptUs && now - a.sweptUs < kSweepEveryUs) return;
    a.sweptUs = now;
    for (Slot& s : a.slot)
        if ((s.phase == kHold || s.phase == kConv) && now > s.lastUs && now - s.lastUs >= kStaleUs) {
            FreeSlot(&s);
            s_swept.fetch_add(1, std::memory_order_relaxed);
        }
}

// Opens a hold on a new slot (fix102 S3a: an equipment unit of this owner that ran shortly before marks it at once, with
// that unit's time)
void OpenHold(Slot* s, uintptr_t owner, float mx, uint64_t now)
{
    s->phase = kHold;
    s->h = mx;
    s->openUs = now;
    s->ownerIdx = OwnerIndex(owner);
    ++a.active;
    s_opened.fetch_add(1, std::memory_order_relaxed);
    for (EqNote& e : a.eqNote) {
        if (e.owner != owner) continue;
        if (e.idx == s->ownerIdx && now >= e.us && now - e.us <= kEqNoteUs) {
            s->eqFirst = 1;
            s->eqUs = e.us;
            s_eqFirst.fetch_add(1, std::memory_order_relaxed);
            s_eqBefore.fetch_add(1, std::memory_order_relaxed);
        }
        e = EqNote{};
    }
}

// holdfix F3 (rev96g): every end of an actor's own spawn fade-in, held or not (a late hold, a full table, a long frame that
// jumped the window, a character never held). A call in state 1 is remembered; the first call after it in state 0 is the
// end call (the tick sets state 0 before it calls the applier; SetFadeState(0) has no native caller, rev96g). Counted when
// the actor still has queued units (its AI starts before it is built), separately when the gate still holds some of them.
void NoteFading(uintptr_t comp, uintptr_t owner, int state)
{
    int k = -1, empty = -1;
    for (int i = 0; i < kFading && k < 0; ++i) {
        if (a.fadeComp[i] == comp) k = i;
        else if (empty < 0 && !a.fadeComp[i]) empty = i;
    }
    if (state == 1) {
        if (k < 0) {
            if (empty < 0) {  // full: the oldest insert place goes (round robin); that fade's end is then not seen
                empty = a.fadeNext;
                a.fadeNext = (a.fadeNext + 1) % kFading;
            }
            k = empty;
            a.fadeComp[k] = comp;
        }
        a.fadeOwner[k] = owner;
        return;
    }
    if (k < 0) return;
    const bool sameOwner = a.fadeOwner[k] == owner;
    a.fadeComp[k] = 0, a.fadeOwner[k] = 0;
    if (state != 0 || !sameOwner) return;  // FadeOut / another state, or another actor at a reused address: not an end
    if (a.testQueued ? a.testQueued(owner) : NpcGateActorQueued(owner)) {
        s_aiEarly.fetch_add(1, std::memory_order_relaxed);
        // build97 (rev96g delta follow-up): measured by the gate's own queued units (Gated), not the hold list
        if (a.testHeld ? a.testHeld(owner) : NpcGateActorGated(owner)) s_aiEarlyHeld.fetch_add(1, std::memory_order_relaxed);
    }
}

void ClearFading()
{
    for (int i = 0; i < kFading; ++i) a.fadeComp[i] = a.fadeOwner[i] = 0;
    a.fadeNext = 0;
}


void Release(Slot* s, float v, uint64_t now, uint8_t why)
{
    if (s->onSeen) (s->onSeen == 2 ? s_drawn : s_notDrawn).fetch_add(1, std::memory_order_relaxed);
    s->phase = kConv;
    s->why = why;
    s->hRel = s->h;
    s->vRel = std::max(v, 1e-3f);
    (why == kWhyRan ? s_relRan : why == kWhyBound ? s_relBound : s_relClock).fetch_add(1, std::memory_order_relaxed);
    const uint64_t held = now > s->openUs ? now - s->openUs : 0;
    s_holdUsSum.fetch_add(held, std::memory_order_relaxed);
    AtomicMaxU(s_holdUsMax, held);
}

// The decision for one applier call (game thread). state / v / mx: the component's state, value and max as the game has
// them now; arg: the dither the writer hands to the meshes. Returns what reaches the meshes (>= arg).
// rate: the component's fade rate per second (sanitised): the clock threshold is a time to the fade's end (holdfix F7)
float Step(uintptr_t comp, uintptr_t owner, int state, float v, float mx, float arg, uint64_t now, float rate)
{
    const uint32_t epoch = g_epoch.load(std::memory_order_relaxed);
    if (epoch != a.epoch) {  // the key was switched off meanwhile: no slot survives it
        for (Slot& x : a.slot) x.phase = kFree;
        a.active = 0;
        a.epoch = epoch;
        ClearFading();
        for (EqNote& e : a.eqNote) e = EqNote{};
    }
    NoteFading(comp, owner, state);  // holdfix F3: the AI-before-built count at every own fade end
    Slot* s = FindSlot(comp);
    if (s && s->owner != owner) {  // another actor's component at a reused address
        FreeSlot(s);
        s = nullptr;
    }
    if (s) s->lastUs = now;  // holdfix F5
    SweepStale(now);         // fix102 (rev102a M2): after this call's own slot is refreshed (it is never swept here)
    // only the spawn fade-in (state 1) with a sane value; the end call (state already 0), FadeOut, anything else: the game's
    if (state != 1 || !(mx > 0.5f && mx <= 10.0f) || !(v >= 0.0f && v <= mx + 1e-3f)) {
        if (s) {
            (state == 0 ? s_ended : s_dropped).fetch_add(1, std::memory_order_relaxed);
            FreeSlot(s);
        }
        return arg;
    }
    if (s && v > s->lastV + 1e-4f) {  // the value went up: a new fade-in (SetFadeState again): a new spawn
        FreeSlot(s);
        s = nullptr;
    }
    const bool held = Held(owner);
    if (!s) {
        if (!held) return arg;
        s = AllocSlot(comp, owner, now);
        if (!s) {
            s_full.fetch_add(1, std::memory_order_relaxed);
            return arg;
        }
        s->lastV = v;
        if (v < mx - kOpenWindow) {  // found too late in its fade: not held (one decision per spawn)
            s->phase = kDone;
            s_late.fetch_add(1, std::memory_order_relaxed);
            return arg;
        }
        OpenHold(s, owner, mx, now);
    }
    s->lastV = v;
    if (s->phase == kHold) {
        const uint64_t boundUs = static_cast<uint64_t>(std::max(0, g_holdMs.load(std::memory_order_relaxed))) * 1000;
        if (!held) Release(s, v, now, kWhyRan);
        else if (now - s->openUs >= boundUs) Release(s, v, now, kWhyBound);
        else if (v <= (ShownHoldEndValue() + kDueSoft) * rate / kStockRate) Release(s, v, now, kWhyClock);  // F7: in time
    }
    if (s->phase == kConv) {
        const float h = s->hRel * v / s->vRel;
        s->h = std::min(s->h, std::max(0.0f, h));
        if (s->h <= 0.0f) {
            s->phase = kDone;
            --a.active;
        }
    }
    if (s->phase != kHold && s->phase != kConv) return arg;
    if (arg >= s->h) return arg;
    s_raised.fetch_add(1, std::memory_order_relaxed);
    return s->h;
}

float Decide(uint8_t* comp, uint8_t* owner, float arg)
{
    if (!OnGt()) {
        if (a.active) s_offGt.fetch_add(1, std::memory_order_relaxed);
        return arg;
    }
    s_calls.fetch_add(1, std::memory_order_relaxed);
    __try {
        if (!comp || !owner) return arg;
        if (*reinterpret_cast<uint8_t* const*>(comp + a.l.owner) != owner ||
            *reinterpret_cast<uint8_t* const*>(owner + a.l.actorComp) != comp)
            return arg;  // not the actor's own spawn fade component
        const int32_t state = *reinterpret_cast<const int32_t*>(comp + a.l.state);
        const float v = *reinterpret_cast<const float*>(comp + a.l.value);
        const float mx = *reinterpret_cast<const float*>(comp + a.l.max);
        float rate = *reinterpret_cast<const float*>(comp + a.l.rate);
        // holdfix F7 (rev96g): rate (+0xC0) and max (+0xD0) have no native writer in the component's code (Blueprint
        // defaults?): logged at the first call and when they differ from 4 / 5 (a few lines at most)
        if (state == 1 && a.rateLogs < 8 && (a.rateLogs == 0 || ((rate != kStockRate || mx != 5.0f) && (rate != a.loggedRate || mx != a.loggedMax)))) {
            ++a.rateLogs;
            a.loggedRate = rate, a.loggedMax = mx;
            Log("[shownhold] fade rate %.3f per second, max %.3f%s", rate, mx,
                rate == kStockRate && mx == 5.0f ? " (as assumed)" : ": not 4 / 5, the hold's thresholds follow the rate");
        }
        if (!(rate >= 0.5f && rate <= 20.0f)) rate = kStockRate;
        const float shown =
            Step(reinterpret_cast<uintptr_t>(comp), reinterpret_cast<uintptr_t>(owner), state, v, mx, arg, NowUs(), rate);
        if (shown < arg) {  // cannot happen (Step returns arg or more): counted, and the game's value goes out
            s_below.fetch_add(1, std::memory_order_relaxed);
            return arg;
        }
        return shown;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_faults.fetch_add(1, std::memory_order_relaxed);
        return arg;
    }
}

void ApplyHook(uint8_t* comp, uint8_t* owner, bool doDither, float scale)
{
    if (g_holdMs.load(std::memory_order_relaxed) > 0) scale = Decide(comp, owner, scale);
    a.orig(comp, owner, doDither, scale);
}

uint32_t Disp32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

bool ReadField(const uint8_t* body, const BodyField& f, uint32_t* out)
{
    if (memcmp(body + f.at, f.bytes, f.len) != 0) return false;
    *out = Disp32(body + f.at + f.len);
    return *out >= 0x30 && *out < 0x1000;
}

bool Find()
{
    if (a.found) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kApplier));
    uint32_t jr = 0, br = 0, lo = 0, cl = 0, sr = 0;
    if (hits.size() != 1 || !FadeInUnseenInfo(&jr, &br, &lo, &cl, &sr)) {
        Log("[shownhold] the fade's applier was not recognised (%zu matches) or the fade tick not found: PacingGateShownHoldMs "
            "not available",
            hits.size());
        return false;
    }
    const uint8_t* const body = game.base + br;
    Layout l;
    if (!ReadField(body, kState, &l.state) || !ReadField(body, kRate, &l.rate) || !ReadField(body, kValue, &l.value) ||
        !ReadField(body, kMax, &l.max) || !ReadField(body, kOwner, &l.owner) || !ReadField(body, kActorComp, &l.actorComp) ||
        !ReadField(body, kFloor, &l.floor) || Disp32(body + kValue2.at + kValue2.len) != l.value ||
        memcmp(body + kValue2.at, kValue2.bytes, kValue2.len) != 0 || body[kBodyTailJmp] != 0xE9 ||
        RipTarget(body + kBodyTailJmp, 1, 5) != hits[0] || Disp32(hits[0] + 0x33) != lo) {
        Log("[shownhold] the fade tick's layout or its call of the applier not recognised: PacingGateShownHoldMs not available");
        return false;
    }
    // holdfix F8 (rev96g): the offsets must equal pacing's own (its due rule and FadeLevel read the same component)
    uint32_t p[5] = {};
    if (NpcPacingFadeLayout(p)) {
        // build97 (rev96g delta follow-up): pacing reads the fade rate at value + 4, so the hook's rate must sit there too
        if (p[0] != l.state || p[1] != l.value || p[2] != l.owner || p[3] != l.floor || p[4] != l.actorComp ||
            p[1] + 4 != l.rate) {
            Log("[shownhold] the fade layout differs from pacing's (state +0x%X/+0x%X, value +0x%X/+0x%X, rate +0x%X/+0x%X, "
                "owner +0x%X/+0x%X, floor +0x%X/+0x%X, actor's component +0x%X/+0x%X): PacingGateShownHoldMs not available",
                l.state, p[0], l.value, p[1], l.rate, p[1] + 4, l.owner, p[2], l.floor, p[3], l.actorComp, p[4]);
            return false;
        }
    } else {
        Log("[shownhold] pacing's fade layout not found yet: the hook's layout is not cross-checked");
    }
    a.applier = hits[0];
    a.l = l;
    a.list = lo;
    a.onScreen = cl + 4;  // LastRenderTimeOnScreen follows LastRenderTime
    a.found = true;
    return true;
}

}  // namespace

bool InstallShownHold(bool beforeGameCode)
{
    if (a.installed) return true;
    if (!Find()) return false;
    if (!beforeGameCode) {
        Log("[shownhold] PacingGateShownHoldMs: start-up install only, not installed");
        return false;
    }
    uint8_t* const region = AllocNearGame(64);
    if (!region) {
        Log("[shownhold] no memory near the game's code: not installed");
        return false;
    }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&ApplyHook);
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    uint8_t* const tramp = region + 16;
    memcpy(tramp, a.applier, kPrologue);
    memcpy(tramp + kPrologue, jmpAbs, sizeof jmpAbs);
    uint8_t* const back = a.applier + kPrologue;
    memcpy(tramp + kPrologue + sizeof jmpAbs, &back, sizeof back);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = region - (a.applier + 5);
    if (rel != static_cast<int32_t>(rel)) {
        Log("[shownhold] out of reach: not installed");
        return false;
    }
    a.orig = reinterpret_cast<ApplyFn>(tramp);
    uint8_t insn[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, 4);
    if (!WriteCode(a.applier, insn)) {
        Log("[shownhold] code write failed: not installed");
        return false;
    }
    a.installed = true;
    const uint8_t* base = Game().base;
    Log("[shownhold] installed: the fade's applier exe+0x%X (component: state +0x%X, value +0x%X, rate +0x%X, max +0x%X, floor "
        "+0x%X, owner +0x%X; actor's fade component +0x%X)",
        static_cast<unsigned>(a.applier - base), a.l.state, a.l.value, a.l.rate, a.l.max, a.l.floor, a.l.owner, a.l.actorComp);
    return true;
}

void SetShownHold(int ms, int endTenths)
{
    const int e = std::clamp(endTenths, 5, 25);
    if (g_endTenths.exchange(e) != e) Log("[shownhold] PacingGateShownHoldEnd %d (fade value %.1f)", e, e / 10.0);
    const int v = a.installed ? std::clamp(ms, 0, 2000) : 0;
    const int was = g_holdMs.exchange(v);
    if (!v) g_epoch.fetch_add(1);  // off: the hook passes the game's values at once; the slots go at the next call
    if (was != v)
        Log("[shownhold] PacingGateShownHoldMs %d%s", v,
            v ? ": a far character whose heavy units the gate holds stays hidden in its spawn fade-in (bounded), then fades in"
              : " (the game's fade)");
}

bool ShownHoldOn() { return g_holdMs.load(std::memory_order_relaxed) > 0; }
float ShownHoldEndValue() { return g_endTenths.load(std::memory_order_relaxed) / 10.0f; }
void ShownHoldCountDue(bool now) { (now ? s_dueNow : s_dueSoft).fetch_add(1, std::memory_order_relaxed); }
// holdfix F2: the due units one gate replay ran (the most per stats interval is logged)
void ShownHoldCountDueReplay(int n)
{
    if (n > 0) AtomicMaxU(s_dueReplayMax, static_cast<uint64_t>(n));
}
// holdfix F4 (rev96g): the gate ran an equipment unit of this actor while its appearance unit is still queued; counted once
// per held spawn when its hold is open (its weapon / shield, static meshes that are never dithered, may show on a fully
// dithered body until the appearance runs). fix102: only a live hold of the same object is marked (rev102a M2), with the
// time (M1: its units' deadline); with no open hold the owner is remembered for kEqNoteUs and marked when its hold opens
// (S3a: equipment that ran before the hold opened). Game thread.
void ShownHoldNoteEquipFirst(uintptr_t owner)
{
    if (!owner || !ShownHoldOn()) return;
    const uint64_t now = NowUs();
    bool open = false;
    for (Slot& s : a.slot) {
        if (s.owner != owner || !LiveHold(s, now)) continue;
        open = true;
        if (!s.eqFirst) {
            s.eqFirst = 1;
            s.eqUs = now;
            s_eqFirst.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (open) return;
    int k = -1;
    for (int i = 0; i < kEqNotes && k < 0; ++i)
        if (a.eqNote[i].owner == owner) k = i;
    if (k < 0) {
        k = a.eqNext;
        a.eqNext = (a.eqNext + 1) % kEqNotes;
    }
    a.eqNote[k] = EqNote{owner, OwnerIndex(owner), now};
}

// The shown level PacingBaseGate reads for this actor's fade component (0: none). Game thread.
float ShownHoldLevel(const uint8_t* comp)
{
    if (!a.active || !ShownHoldOn() || a.epoch != g_epoch.load(std::memory_order_relaxed)) return 0.0f;
    const Slot* s = FindSlot(reinterpret_cast<uintptr_t>(comp));
    return s && (s->phase == kHold || s->phase == kConv) ? s->h : 0.0f;
}

// rev96g follow-up (port102): how much faster than the game's fade this component's shown level falls (hRel / vRel while a
// released hold converges, else 1): PacingHairActor's fade rate reads the shown level and assumed the game's speed. Game
// thread.
float ShownHoldSpeed(const uint8_t* comp)
{
    if (!a.active || !ShownHoldOn() || a.epoch != g_epoch.load(std::memory_order_relaxed)) return 1.0f;
    const Slot* s = FindSlot(reinterpret_cast<uintptr_t>(comp));
    if (!s || s->phase != kConv || !(s->vRel > 0.0f) || !(s->hRel > s->vRel)) return 1.0f;
    return std::min(s->hRel / s->vRel, 50.0f);
}

// holdfix F4 bounded fix (port102; fix102 = rev102a M1 / M2): the actors of live open holds whose equipment ran before
// their appearance (their weapon or shield can show on the hidden body), with the seconds left to their deadline
// (kEqDeadlineUs after that equipment unit; 0: past it). The gate counts their queued units in the spread at that
// deadline (the room rule may still run them first) and makes them soft-due once it has passed. A slot not called for
// kLiveUs or with another object at its address is not listed; the table is swept. Game thread.
int ShownHoldUrgentOwners(uintptr_t* out, float* leftS, int max)
{
    if (!a.active || !ShownHoldOn() || a.epoch != g_epoch.load(std::memory_order_relaxed)) return 0;
    const uint64_t now = NowUs();
    SweepStale(now);
    int n = 0;
    for (Slot& s : a.slot) {
        if (n >= max) break;
        if (!s.eqFirst || !LiveHold(s, now)) continue;
        const uint64_t due = s.eqUs + kEqDeadlineUs;
        out[n] = s.owner;
        leftS[n++] = due > now ? static_cast<float>(due - now) / 1e6f : 0.0f;
        if (due <= now && !s.eqLate) {  // still held past its deadline (its units still queued): counted once
            s.eqLate = 1;
            s_eqDeadline.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return n;
}

void LogShownHoldStats()
{
    if (!a.installed) return;
    const uint64_t calls = s_calls.exchange(0), raised = s_raised.exchange(0), opened = s_opened.exchange(0),
                   late = s_late.exchange(0), full = s_full.exchange(0), ran = s_relRan.exchange(0), bound = s_relBound.exchange(0),
                   clock = s_relClock.exchange(0), ended = s_ended.exchange(0), dropped = s_dropped.exchange(0),
                   below = s_below.exchange(0), offGt = s_offGt.exchange(0), faults = s_faults.exchange(0),
                   sum = s_holdUsSum.exchange(0), mx = s_holdUsMax.exchange(0), dn = s_dueNow.exchange(0),
                   ds = s_dueSoft.exchange(0), early = s_aiEarly.exchange(0), drawn = s_drawn.exchange(0),
                   notDrawn = s_notDrawn.exchange(0), earlyHeld = s_aiEarlyHeld.exchange(0), reused = s_reused.exchange(0),
                   eqFirst = s_eqFirst.exchange(0), dueMax = s_dueReplayMax.exchange(0), swept = s_swept.exchange(0),
                   eqBefore = s_eqBefore.exchange(0), eqDeadline = s_eqDeadline.exchange(0);
    if (!(opened || raised || late || below || offGt || faults || ran || bound || clock || dn || ds || early || reused || full ||
          swept))
        return;
    const uint64_t rel = ran + bound + clock;
    (void)drawn, (void)notDrawn;
    Log("[shownhold] %llu holds opened (%llu found too late, %llu table full); released: %llu units ran, %llu bound %d ms, %llu "
        "fade clock; hold ms avg %.0f max %.0f; %llu of %llu applier calls raised; %llu fades ended, %llu dropped (state); "
        "shown below the game's %llu; off-thread %llu; faults %llu; due units run %llu now + %llu soft (end %.1f); fades "
        "ended with units queued (AI before built) %llu",
        (unsigned long long)opened, (unsigned long long)late, (unsigned long long)full, (unsigned long long)ran,
        (unsigned long long)bound, g_holdMs.load(), (unsigned long long)clock, rel ? sum / 1000.0 / rel : 0.0, mx / 1000.0,
        (unsigned long long)raised, (unsigned long long)calls, (unsigned long long)ended, (unsigned long long)dropped,
        (unsigned long long)below, (unsigned long long)offGt, (unsigned long long)faults, (unsigned long long)dn,
        (unsigned long long)ds, ShownHoldEndValue(), (unsigned long long)early);
    // holdfix (rev96g F2-F5): a second line, so that the first keeps its format for the existing log readers
    Log("[shownhold] holdfix: AI before built %llu with units the gate holds; stale slots reused %llu, table full %llu; "
        "equipment ran before the appearance while held %llu (%llu of them before the hold opened; units due within %.1f s, "
        "past that deadline %llu); most due units in one replay %llu; stale held slots swept %llu",
        (unsigned long long)earlyHeld, (unsigned long long)reused, (unsigned long long)full, (unsigned long long)eqFirst,
        (unsigned long long)eqBefore, kEqDeadlineUs / 1e6, (unsigned long long)eqDeadline, (unsigned long long)dueMax,
        (unsigned long long)swept);
}

// ---- self-test ----
bool ShownHoldFind(uint32_t out[8])
{
    if (!Find()) return false;
    out[0] = static_cast<uint32_t>(a.applier - Game().base);
    out[1] = a.l.state, out[2] = a.l.value, out[3] = a.l.rate, out[4] = a.l.max, out[5] = a.l.floor, out[6] = a.l.owner;
    out[7] = a.l.actorComp;
    return true;
}
void ShownHoldTestSetup(bool (*held)(uintptr_t), uint64_t (*now)(), int gt, int ms, int endTenths)
{
    g_endTenths = endTenths;
    a.testHeld = held;
    a.testNow = now;
    a.testGt = gt;
    for (Slot& s : a.slot) s.phase = kFree;
    a.active = 0;
    ClearFading();
    for (EqNote& e : a.eqNote) e = EqNote{};
    a.sweptUs = 0;
    // fix102: counters of earlier tests (holds opened without the applier) do not carry into the next one's
    for (std::atomic<uint64_t>* c : {&s_calls, &s_raised, &s_opened, &s_late, &s_full, &s_relRan, &s_relBound, &s_relClock,
                                     &s_ended, &s_dropped, &s_below, &s_offGt, &s_faults, &s_holdUsSum, &s_holdUsMax, &s_dueNow,
                                     &s_dueSoft, &s_aiEarly, &s_drawn, &s_notDrawn, &s_aiEarlyHeld, &s_reused, &s_eqFirst,
                                     &s_dueReplayMax, &s_swept, &s_eqBefore, &s_eqDeadline})
        c->store(0);
    g_holdMs = ms;
}
void ShownHoldTestQueued(bool (*queued)(uintptr_t)) { a.testQueued = queued; }
// fix102: open a hold for this component / owner without the applier, or keep the open one live (its last call = now) when
// touch; returns the slots in kHold / kConv
int ShownHoldTestHold(uintptr_t comp, uintptr_t owner, bool touch)
{
    a.epoch = g_epoch.load();
    if (touch) {
        const uint64_t now = NowUs();
        Slot* s = FindSlot(comp);
        if (!s && (s = AllocSlot(comp, owner, now)) != nullptr) OpenHold(s, owner, 5.0f, now);
        if (s) s->lastUs = now;
    }
    return a.active;
}
uint64_t ShownHoldTestAiEarly() { return s_aiEarly.exchange(0); }
void ShownHoldTestDueCounts(uint64_t out[2]) { out[0] = s_dueSoft.exchange(0), out[1] = s_dueNow.exchange(0); }
// What the applier would be handed for this call (the hook's decision without calling the game).
float ShownHoldTestCall(uint8_t* comp, uint8_t* owner, float scale)
{
    return g_holdMs.load() > 0 ? Decide(comp, owner, scale) : scale;
}
void ShownHoldTestCounts(uint64_t out[6])
{
    out[0] = s_opened.exchange(0), out[1] = s_late.exchange(0), out[2] = s_relRan.exchange(0), out[3] = s_relBound.exchange(0);
    out[4] = s_relClock.exchange(0), out[5] = s_below.exchange(0);
    s_calls = 0, s_raised = 0, s_full = 0, s_ended = 0, s_dropped = 0, s_offGt = 0, s_faults = 0, s_holdUsSum = 0, s_holdUsMax = 0;
}

}  // namespace opf
