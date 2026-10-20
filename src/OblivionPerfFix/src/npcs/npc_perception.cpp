// NpcPerceptionParallel: each character's perception round tests its line of sight to every other character in range
// with one to three physics ray casts, one pair after another on the game thread. In a crowd of 40 that is ~8 % of the
// game thread's frame (the author's research: ~4 ms per frame; see docs/research.md). The pair tests of one round are
// independent read-only queries of the physics scene, so this fix runs them on helper threads (and the game thread)
// as soon as the round has gathered its candidates, with the game's own pair-test function, and hands each result to
// the round's loop when it asks for that pair. The loop itself, its result maps and its messages to the old engine run
// unchanged, in the game's order; the results are the ones the game's function produces for the same pair at the same
// point of the frame (the timer tick runs after the tick groups, while the old engine waits at its sync point, so
// nothing moves between the early test and the loop's own call). Any pair the fix did not test in advance (a character
// without a root component, a queue that was full) is tested by the game's call as before.
//
// The round, build 19115871 (exe+0x485FA60, a timer delegate of the character; the offsets are checked by pattern):
//   class = ClassGetter() (exe+0x446B6FA0); world = GetWorld(self) (exe+0x2DC0350);
//   GetActorsOfClass(world, class, &array) (exe+0x31A9020); settings = Settings() (exe+0x4711300);
//   for each other in array: other != null; other->Class (+0x10) IsA class (class hierarchy at +0x30, depth +0x38);
//     other != self; [[other+0x7B8]+0x120] != null; self->vtable[+0xA90](self, other) (a yes/no); then
//     dist²(self root +0x1A0 -> +0x280, other root) < ((r*scale)*f) * (r*f) with r = settings+0x4E8, f = +0x4EC,
//     scale = a global float; then state = 3, seen = PairTest(self, other, &state, 3, 1) (exe+0x485A170);
//     the pair's state and seen go into the character's map; Notify(self, other) (exe+0x4865DB0).
// Two calls are redirected: the gather (our stub also receives self, which the loop keeps in rdi) and the pair test.
//
// NpcPerceptionParallel=1 on, 2 = count only (the game's calls, with statistics in the log), 0 off. Start-up, then live.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

bool Fail(const char* why)
{
    Log("[perception] %s: fix disabled", why);
    return false;
}

using GatherFn = void (*)(void* world, void* cls, void* arr);
using PairFn = uint8_t (*)(void* self, void* other, uint8_t* state, int maxRays, uint8_t flag);
using ClassFn = void* (*)();
using WorldFn = void* (*)(void* self);
using SettingsFn = uint8_t* (*)();

// ---- code patterns (addresses of build 19115871 in the comments) ----

// The round's gather (exe+0x485FAFB..): call ClassGetter; mov rcx,rdi; mov rbx,rax; call GetWorld; lea r8,[rsp+60h];
// mov rdx,rbx; mov rcx,rax; call GetActorsOfClass; call Settings; mov r14,[rsp+60h]; movsxd rcx,[rsp+68h];
// lea r13,[r14+rcx*8]; cmp r14,r13
constexpr char kGather[] =
    "E8 ?? ?? ?? ?? 48 8B CF 48 8B D8 E8 ?? ?? ?? ?? 4C 8D 44 24 60 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "4C 8B 74 24 60 48 63 4C 24 68 4D 8D 2C CE 4D 3B F5";
constexpr size_t kGatherClassCall = 0, kGatherWorldCall = 11, kGatherCall = 27, kGatherSettingsCall = 32;
// The loop's head (exe+0x485FB3F): mov rsi,rax; movss xmm7,[scale]; mov [rsp+178h],r12; mov rbx,[r14]
constexpr char kScale[] = "48 8B F0 F3 0F 10 3D ?? ?? ?? ?? 4C 89 A4 24 78 01 00 00 49 8B 1E";
constexpr size_t kScaleInsn = 3;
// The loop's checks (exe+0x485FB63): mov rdx,[rbx+10h]; lea r8,[rax+30h]; movsxd rax,[rax+38h]; cmp eax,[rdx+38h]; jg;
// mov rcx,rax; mov rax,[rdx+30h]; cmp [rax+rcx*8],r8; jnz; cmp rbx,rdi; jz; mov rax,[rbx+7B8h]; cmp qword [rax+120h],0;
// jz; movss xmm1,[rsi+4E8h]; mov r15b,3; movss xmm0,[rsi+4ECh]; movaps xmm6,xmm1; mov rax,[rdi]; mov rdx,rbx;
// mulss xmm6,xmm7; mov rcx,rdi; xor r12b,r12b; mov [rsp+30h],r15b; mulss xmm1,xmm0; mulss xmm6,xmm0; mulss xmm6,xmm1;
// call [rax+0A90h]; test al,al; jz; mov rax,[rbx+1A0h]
constexpr char kChecks[] =
    "48 8B 53 10 4C 8D 40 30 48 63 40 38 3B 42 38 0F 8F ?? ?? ?? ?? 48 8B C8 48 8B 42 30 4C 39 04 C8 0F 85 ?? ?? ?? ?? "
    "48 3B DF 0F 84 ?? ?? ?? ?? 48 8B 83 B8 07 00 00 48 83 B8 20 01 00 00 00 0F 84 ?? ?? ?? ?? F3 0F 10 8E E8 04 00 00 "
    "41 B7 03 F3 0F 10 86 EC 04 00 00 0F 28 F1 48 8B 07 48 8B D3 F3 0F 59 F7 48 8B CF 45 32 E4 44 88 7C 24 30 "
    "F3 0F 59 C8 F3 0F 59 F0 F3 0F 59 F1 FF 90 90 0A 00 00 84 C0 0F 84 ?? ?? ?? ?? 48 8B 83 A0 01 00 00";
// The location read (exe+0x485FBEC): mov rax,[rbx+1A0h]; movsd xmm2,[zero.z]; movups xmm3,[zero]; test rax,rax; jz +1Dh;
// movups xmm0,[rax+280h]
constexpr char kLocation[] = "48 8B 83 A0 01 00 00 F2 0F 10 15 ?? ?? ?? ?? 0F 10 1D ?? ?? ?? ?? 48 85 C0 74 1D 0F 10 80 80 02 00 00";
// The pair test (exe+0x485FCC6): mov r9d,3; mov byte [rsp+20h],1; lea r8,[rsp+30h]; mov rdx,rbx; mov rcx,rdi;
// call PairTest; movzx r15d,byte [rsp+30h]; movzx r12d,al
constexpr char kPair[] = "41 B9 03 00 00 00 C6 44 24 20 01 4C 8D 44 24 30 48 8B D3 48 8B CF E8 ?? ?? ?? ?? 44 0F B6 7C 24 30 44 0F B6 E0";
constexpr size_t kPairCall = 22;

constexpr uint32_t kClassOff = 0x10, kHierarchyOff = 0x30, kDepthOff = 0x38, kPawnOff = 0x7B8, kPawnLinkOff = 0x120,
                   kRootOff = 0x1A0, kLocationOff = 0x280, kRadiusOff = 0x4E8, kFactorOff = 0x4EC, kFilterSlot = 0xA90;

struct TArrayView {
    void** data;
    int32_t num, max;
};

struct State {
    const GameModule* game = nullptr;
    GatherFn gather = nullptr;
    PairFn pair = nullptr;
    ClassFn classGetter = nullptr;
    WorldFn world = nullptr;
    SettingsFn settings = nullptr;
    const float* scale = nullptr;
    uint8_t* gatherSite = nullptr;  // the call instructions
    uint8_t* pairSite = nullptr;
    bool found = false, installed = false;
    std::atomic<int> mode{0};  // 0 off, 1 on, 2 count only
    int minPairs = 4;
} s;

// ---- the round's jobs (one owner: the game thread inside the round) ----
constexpr uint32_t kCap = 2048;
struct Job {
    void* self;
    void* other;
    std::atomic<uint32_t> state;  // 0 queued, 1 claimed, 2 done
    uint8_t outState, seen;
};
Job g_jobs[kCap];
// generation (bits 48-63) | jobs pushed (bits 24-47) | next to claim (bits 0-23)
std::atomic<uint64_t> g_claim{0};
std::atomic<uint32_t> g_done{0};
constexpr uint64_t kPushOne = 1ull << 24;
uint32_t Pushed(uint64_t c) { return static_cast<uint32_t>((c >> 24) & 0xFFFFFF); }
std::atomic<bool> g_window{false};
HANDLE g_active = nullptr;
WaitGate g_claimGate, g_doneGate;
int g_helpers = 0;
// the round being consumed by the loop (game thread only)
void* g_roundSelf = nullptr;
uint32_t g_roundCount = 0, g_roundNext = 0;

std::atomic<uint64_t> g_rounds{0}, g_candidates{0}, g_pairs{0}, g_hits{0}, g_misses{0}, g_byHelper{0}, g_byOwner{0}, g_skipped{0},
    g_full{0}, g_roundUs{0}, g_pairUs{0}, g_waitUs{0};
uint64_t g_logged = 0;
std::atomic<bool> g_slotsLogged{false};

uint64_t Us()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(f.QuadPart));
}

void RunJob(Job& j, bool owner)
{
    const uint64_t t0 = Us();
    j.outState = 3;
    j.seen = s.pair(j.self, j.other, &j.outState, 3, 1);
    g_pairUs.fetch_add(Us() - t0, std::memory_order_relaxed);
    j.state.store(2, std::memory_order_release);
    (owner ? g_byOwner : g_byHelper).fetch_add(1, std::memory_order_relaxed);
    g_done.fetch_add(1, std::memory_order_acq_rel);
    WakeWord(g_done, g_doneGate);
}

bool RunOne(bool owner)
{
    uint64_t c = g_claim.load(std::memory_order_acquire);
    uint32_t i;
    for (;;) {
        i = static_cast<uint32_t>(c & 0xFFFFFF);
        if (i >= Pushed(c)) return false;
        if (g_claim.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_acquire)) break;
    }
    Job& j = g_jobs[i];
    j.state.store(1, std::memory_order_relaxed);
    RunJob(j, owner);
    return true;
}

DWORD WINAPI HelperMain(void*)
{
    for (;;) {
        WaitForSingleObject(g_active, INFINITE);
        HelperThreadCache();
        while (g_window.load(std::memory_order_acquire)) {
            const uint64_t c = g_claim.load(std::memory_order_acquire);
            if (RunOne(false)) continue;
            WaitWord(g_claim, g_claimGate, [c](uint64_t v) { return v != c; });
        }
    }
}

bool StartHelpers(int n = 0)
{
    if (g_helpers) return true;
    if (n <= 0) {
        n = static_cast<int>(UsableLogicalProcessors()) / 4;
        n = n < 1 ? 1 : n > 4 ? 4 : n;
    }
    g_active = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_active) return false;
    for (int i = 0; i < n; ++i) {
        HANDLE h = CreateThread(nullptr, 256 * 1024, HelperMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!h) break;
        wchar_t name[48];
        swprintf_s(name, L"OblivionPerfFix perception %d", i + 1);
        DescribeThread(h, name);
        CloseHandle(h);
        ++g_helpers;
    }
    return g_helpers > 0;
}

// The loop's own checks, on one candidate (the same reads, in the same order; false = the loop would not test it).
bool LoopWouldTest(uint8_t* self, uint8_t* other, uint8_t* cls, const uint8_t* settings, double* selfLoc, float scale)
{
    if (!other) return false;
    uint8_t* const otherClass = *reinterpret_cast<uint8_t**>(other + kClassOff);
    const int32_t depth = *reinterpret_cast<const int32_t*>(cls + kDepthOff);
    if (depth > *reinterpret_cast<const int32_t*>(otherClass + kDepthOff)) return false;
    uint8_t* const chain = *reinterpret_cast<uint8_t**>(otherClass + kHierarchyOff);
    if (*reinterpret_cast<uint8_t**>(chain + static_cast<int64_t>(depth) * 8) != cls + kHierarchyOff) return false;
    if (other == self) return false;
    uint8_t* const pawn = *reinterpret_cast<uint8_t**>(other + kPawnOff);
    if (!*reinterpret_cast<void**>(pawn + kPawnLinkOff)) return false;
    const float r = *reinterpret_cast<const float*>(settings + kRadiusOff);
    const float f = *reinterpret_cast<const float*>(settings + kFactorOff);
    float t = r * scale;  // the game's float products, in its order
    const float rf = r * f;
    t = t * f;
    t = t * rf;
    const auto filter = reinterpret_cast<uint8_t (*)(void*, void*)>((*reinterpret_cast<void***>(self))[kFilterSlot / 8]);
    if (!filter(self, other)) return false;
    uint8_t* const root = *reinterpret_cast<uint8_t**>(other + kRootOff);
    if (!root || !selfLoc) return false;  // the game uses a zero vector there: left to its own call
    const double* const o = reinterpret_cast<const double*>(root + kLocationOff);
    const double dx = o[0] - selfLoc[0], dy = o[1] - selfLoc[1], dz = o[2] - selfLoc[2];
    const double d = dx * dx + dy * dy + dz * dz;
    return d < static_cast<double>(t);  // NaN: not tested, as the game's comisd/jnc
}

// The round's gather, redirected: the game's call, then (mode 1) this round's pair tests in advance.
void GatherHook(void* world, void* cls, void* arr, void* selfArg)
{
    s.gather(world, cls, arr);
    const int mode = s.mode.load(std::memory_order_relaxed);
    if (!mode) return;
    uint8_t* const self = static_cast<uint8_t*>(selfArg);
    const TArrayView* const a = static_cast<const TArrayView*>(arr);
    g_rounds.fetch_add(1, std::memory_order_relaxed);
    g_candidates.fetch_add(a->num > 0 ? a->num : 0, std::memory_order_relaxed);
    if (!g_slotsLogged.exchange(true)) {
        void** const vt = *reinterpret_cast<void***>(self);
        Log("[perception] first round: the filter is exe+0x%llX, the eye position exe+0x%llX (character vtable exe+0x%llX)",
            static_cast<unsigned long long>(static_cast<uint8_t*>(vt[kFilterSlot / 8]) - s.game->base),
            static_cast<unsigned long long>(static_cast<uint8_t*>(vt[kFilterSlot / 8 + 1]) - s.game->base),
            static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(vt) - s.game->base));
    }
    if (mode != 1 || a->num < 2 || !a->data) return;
    const uint64_t t0 = Us();
    uint8_t* const roundClass = static_cast<uint8_t*>(s.classGetter());
    const uint8_t* const settings = s.settings();
    const float scale = *s.scale;
    uint8_t* const selfRoot = *reinterpret_cast<uint8_t**>(self + kRootOff);
    double* const selfLoc = selfRoot ? reinterpret_cast<double*>(selfRoot + kLocationOff) : nullptr;
    // collect the pairs the loop will test, in the loop's order
    uint32_t m = 0;
    for (int32_t i = 0; i < a->num && m < kCap; ++i) {
        uint8_t* const other = static_cast<uint8_t*>(a->data[i]);
        if (!LoopWouldTest(self, other, roundClass, settings, selfLoc, scale)) continue;
        Job& j = g_jobs[m++];
        j.self = self;
        j.other = other;
        j.state.store(0, std::memory_order_relaxed);
    }
    if (static_cast<int32_t>(m) < s.minPairs) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        g_roundCount = 0;
        return;
    }
    if (m == kCap) g_full.fetch_add(1, std::memory_order_relaxed);
    g_pairs.fetch_add(m, std::memory_order_relaxed);
    // publish: a new generation with m jobs; helpers wake; the owner works too, then waits for the stragglers
    g_done.store(0, std::memory_order_relaxed);
    const uint64_t gen = ((g_claim.load(std::memory_order_relaxed) >> 48) + 1) & 0xFFFF;
    g_claim.store((gen << 48) | (static_cast<uint64_t>(m) << 24), std::memory_order_release);
    g_window.store(true, std::memory_order_release);
    SetEvent(g_active);
    WakeWord(g_claim, g_claimGate);
    while (RunOne(true)) {}
    const uint64_t w0 = Us();
    WaitWord(g_done, g_doneGate, [m](uint32_t v) { return v >= m; });
    g_waitUs.fetch_add(Us() - w0, std::memory_order_relaxed);
    g_window.store(false, std::memory_order_release);
    ResetEvent(g_active);
    WakeWord(g_claim, g_claimGate);
    g_roundSelf = self;
    g_roundCount = m;
    g_roundNext = 0;
    g_roundUs.fetch_add(Us() - t0, std::memory_order_relaxed);
}

// The loop's pair test, redirected: the result computed in advance for this pair, else the game's call.
uint8_t PairHook(void* self, void* other, uint8_t* state, int maxRays, uint8_t flag)
{
    const int mode = s.mode.load(std::memory_order_relaxed);
    if (mode == 1 && self == g_roundSelf && maxRays == 3 && flag == 1) {
        for (uint32_t i = g_roundNext; i < g_roundCount; ++i) {
            Job& j = g_jobs[i];
            if (j.other != other) continue;
            g_roundNext = i + 1;
            *state = j.outState;
            g_hits.fetch_add(1, std::memory_order_relaxed);
            return j.seen;
        }
        g_misses.fetch_add(1, std::memory_order_relaxed);
    } else if (mode == 2) {
        const uint64_t t0 = Us();
        const uint8_t r = s.pair(self, other, state, maxRays, flag);
        g_pairUs.fetch_add(Us() - t0, std::memory_order_relaxed);
        g_pairs.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    return s.pair(self, other, state, maxRays, flag);
}

uint8_t* FindOne(const char* pattern, const char* what)
{
    const auto hits = FindAll(s.game->text, ParsePattern(pattern));
    if (hits.size() != 1) {
        Log("[perception] %s not found (%zu matches)", what, hits.size());
        return nullptr;
    }
    return hits[0];
}

uint8_t* CallTarget(uint8_t* call) { return RipTarget(call, 1, 5); }

bool Retarget(uint8_t* call, const uint8_t* to)
{
    const int64_t rel = to - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

}  // namespace

bool FindNpcPerceptionParallel()
{
    if (s.found) return true;
    s.game = &Game();
    uint8_t* const gather = FindOne(kGather, "the perception round's gather");
    uint8_t* const scale = FindOne(kScale, "the loop's head");
    uint8_t* const checks = FindOne(kChecks, "the loop's checks");
    uint8_t* const location = FindOne(kLocation, "the loop's location read");
    uint8_t* const pair = FindOne(kPair, "the loop's pair test");
    if (!gather || !scale || !checks || !location || !pair) return false;
    // all in one function, in this order, within a few hundred bytes
    if (!(gather < scale && scale < checks && checks < location && location < pair && pair - gather < 0x300))
        return Fail("the round's pieces are not where expected");
    s.classGetter = reinterpret_cast<ClassFn>(CallTarget(gather + kGatherClassCall));
    s.world = reinterpret_cast<WorldFn>(CallTarget(gather + kGatherWorldCall));
    s.gather = reinterpret_cast<GatherFn>(CallTarget(gather + kGatherCall));
    s.settings = reinterpret_cast<SettingsFn>(CallTarget(gather + kGatherSettingsCall));
    s.pair = reinterpret_cast<PairFn>(CallTarget(pair + kPairCall));
    s.scale = reinterpret_cast<const float*>(RipTarget(scale + kScaleInsn, 4, 8));
    {
        const void* const targets[5] = {reinterpret_cast<const void*>(s.classGetter), reinterpret_cast<const void*>(s.world), reinterpret_cast<const void*>(s.gather),
                                        reinterpret_cast<const void*>(s.settings), reinterpret_cast<const void*>(s.pair)};
        const char* const names[5] = {"class getter", "GetWorld", "gather", "settings", "pair test"};
        for (int i = 0; i < 5; ++i)
            if (!s.game->text.Contains(static_cast<const uint8_t*>(targets[i]))) {
                Log("[perception] the %s call's target exe+0x%llX is outside the game's code", names[i],
                    static_cast<unsigned long long>(static_cast<const uint8_t*>(targets[i]) - s.game->base));
                return Fail("a call target is outside the game's code");
            }
    }
    if (!s.game->data.Contains(reinterpret_cast<const uint8_t*>(s.scale)) && !s.game->rdata.Contains(reinterpret_cast<const uint8_t*>(s.scale)))
        return Fail("the scale is not in the game's data");
    s.gatherSite = gather + kGatherCall;
    s.pairSite = pair + kPairCall;
    s.found = true;
    Log("[perception] round found: gather exe+0x%llX -> exe+0x%llX, pair test exe+0x%llX -> exe+0x%llX, class exe+0x%llX, settings exe+0x%llX (+0x%X, +0x%X), scale exe+0x%llX",
        static_cast<unsigned long long>(s.gatherSite - s.game->base), static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(s.gather) - s.game->base),
        static_cast<unsigned long long>(s.pairSite - s.game->base), static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(s.pair) - s.game->base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(s.classGetter) - s.game->base),
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(s.settings) - s.game->base), kRadiusOff, kFactorOff,
        static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(s.scale) - s.game->base));
    return true;
}

bool InstallNpcPerceptionParallel(bool beforeGameCode)
{
    if (s.installed) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    if (!FindNpcPerceptionParallel()) return false;
    if (!StartHelpers()) return Fail("no helper threads");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    // stub 0 (the gather): mov r9,rdi (the loop keeps self in rdi); jmp [rip+0]; GatherHook
    // stub 1 (the pair test): jmp [rip+0]; PairHook
    const uint8_t movR9Rdi[3] = {0x49, 0x89, 0xF9};
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const gatherHook = reinterpret_cast<void*>(&GatherHook);
    void* const pairHook = reinterpret_cast<void*>(&PairHook);
    memcpy(region, movR9Rdi, 3);
    memcpy(region + 3, jmpAbs, 6);
    memcpy(region + 9, &gatherHook, 8);
    memcpy(region + 32, jmpAbs, 6);
    memcpy(region + 38, &pairHook, 8);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    for (const uint8_t* site : {s.gatherSite, s.pairSite}) {
        const int64_t rel = region - (site + 5);
        if (rel != static_cast<int32_t>(rel)) return Fail("stubs out of reach");
    }
    if (!Retarget(s.gatherSite, region) || !Retarget(s.pairSite, region + 32)) return Fail("could not patch a call");
    s.installed = true;
    Log("[perception] installed: 2 calls redirected, %d helper threads", g_helpers);
    return true;
}

void SetNpcPerceptionParallel(int mode, int minPairs)
{
    if (!s.installed) return;
    s.minPairs = minPairs < 1 ? 1 : minPairs;
    const int m = mode == 2 ? 2 : mode ? 1 : 0;
    if (s.mode.exchange(m) != m)
        Log("[perception] %s", m == 1 ? "line-of-sight tests of a perception round run on helper threads"
                             : m == 2 ? "counting only (the game's calls)" : "perception rounds as the game does them");
}

void LogNpcPerceptionStats()
{
    if (!s.installed) return;
    const uint64_t r = g_rounds.load();
    if (r == g_logged) return;
    g_logged = r;
    const uint64_t p = g_pairs.load();
    Log("[perception] rounds %llu (%.1f candidates each), pair tests %llu (%.1f us each): in advance helpers %llu, game thread %llu; "
        "used %llu, not prepared %llu; rounds skipped (under %d pairs) %llu, full %llu; round %.0f us each, of it waiting %.0f us",
        static_cast<unsigned long long>(r), r ? static_cast<double>(g_candidates.load()) / r : 0.0, static_cast<unsigned long long>(p),
        p ? static_cast<double>(g_pairUs.load()) / p : 0.0, static_cast<unsigned long long>(g_byHelper.load()),
        static_cast<unsigned long long>(g_byOwner.load()), static_cast<unsigned long long>(g_hits.load()),
        static_cast<unsigned long long>(g_misses.load()), s.minPairs, static_cast<unsigned long long>(g_skipped.load()),
        static_cast<unsigned long long>(g_full.load()), r ? static_cast<double>(g_roundUs.load()) / r : 0.0,
        r ? static_cast<double>(g_waitUs.load()) / r : 0.0);
}

// ---- self-test ----
bool NpcPerceptionInfo(NpcPerceptionInfoOut* out)
{
    if (!s.found) return false;
    out->gatherSite = s.gatherSite;
    out->pairSite = s.pairSite;
    out->gather = reinterpret_cast<void*>(s.gather);
    out->pair = reinterpret_cast<void*>(s.pair);
    out->classGetter = reinterpret_cast<void*>(s.classGetter);
    out->world = reinterpret_cast<void*>(s.world);
    out->settings = reinterpret_cast<void*>(s.settings);
    out->scale = s.scale;
    out->helpers = g_helpers;
    return true;
}

bool NpcPerceptionTestSetup(const NpcPerceptionTest& t, int helpers)
{
    s.game = &Game();
    s.gather = reinterpret_cast<GatherFn>(t.gather);
    s.pair = reinterpret_cast<PairFn>(t.pair);
    s.classGetter = reinterpret_cast<ClassFn>(t.classGetter);
    s.world = reinterpret_cast<WorldFn>(t.world);
    s.settings = reinterpret_cast<SettingsFn>(t.settings);
    s.scale = t.scale;
    if (!StartHelpers(helpers)) return false;
    for (auto* c : {&g_rounds, &g_candidates, &g_pairs, &g_hits, &g_misses, &g_byHelper, &g_byOwner, &g_skipped, &g_full, &g_roundUs, &g_pairUs, &g_waitUs})
        c->store(0);
    g_slotsLogged.store(true);
    g_roundSelf = nullptr;
    g_roundCount = g_roundNext = 0;
    s.installed = true;
    s.mode.store(1);
    s.minPairs = 1;
    return true;
}

void NpcPerceptionTestGather(void* world, void* cls, void* arr, void* self) { GatherHook(world, cls, arr, self); }
uint8_t NpcPerceptionTestPair(void* self, void* other, uint8_t* state, int maxRays, uint8_t flag) { return PairHook(self, other, state, maxRays, flag); }
NpcPerceptionCounts NpcPerceptionTestCounts()
{
    return {g_rounds.load(), g_pairs.load(), g_hits.load(), g_misses.load(), g_byHelper.load(), g_byOwner.load(), g_skipped.load()};
}

}  // namespace opf
