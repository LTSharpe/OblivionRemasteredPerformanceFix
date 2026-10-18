// FrozenClothStretchFix (a game bug fix): a frozen cloth keeps its pose when its level of detail changes.
// Seen in play as a robe or tabard stretched into a long strip back to where its wearer stood (Ilav's robe, 2026-10-13).
// WriteSimulationData calls Solver::Update(0) on an LOD mismatch even while suspended. Ordinary
// inertia scales leave particles behind the new reference frame, with no physics step to catch up.
// Redirect only that call. For explicitly suspended components, request the engine's per-cloth
// teleport before the update. Cloth::Update retains reset precedence and consumes the request.
// No velocity/config changes, particle corrections, or hooks in the normal simulation tick.
#include "common.h"
#include <atomic>
#include <cstring>

namespace opf {
namespace {
using UpdateFn = void (*)(uint8_t*, float);
struct Array { uint8_t** data; int32_t num, max; };
constexpr int kMaxCloths = 64;
constexpr char kCall[] =
    "49 8B 5E 78 8B F8 49 8B 0F 48 8B D3 89 44 24 24 E8 ?? ?? ?? ?? "
    "3B F8 74 0B 0F 57 C9 48 8B CB E8 ?? ?? ?? ?? 49 8B 56 78 49 8B 0F E8";
constexpr size_t kCallOffset = 31; // exe+0x53CF40B (build 19115871)
constexpr char kProxy[] = "4C 8B F1 33 DB 48 8B 49 58 48 81 C1 F0 05 00 00";
constexpr char kArray[] = "4D 8B BE 90 00 00 00 49 63 86 98 00 00 00 49 8D 04 C7";
constexpr char kSuspended[] = "F6 83 18 09 00 00 08 75 ?? 48 83 BB 40 09 00 00 00";
constexpr char kSolverCloths[] = "4C 8B 79 70 48 63 41 78 4D 8D 2C C7";
// Cloth::Update tests reset (+44) before teleport (+45); teleport zeros the linear/angular scales.
constexpr char kTeleport[] =
    "41 80 7F 45 00 74 19 48 C7 44 24 58 00 00 00 00 0F 57 F6 C7 44 24 60 00 00 00 00 E9";
constexpr char kReset[] = "41 80 7F 44 00 48 8D 35 ?? ?? ?? ?? F3 44 0F 10 1D ?? ?? ?? ?? 45 0F 57 ED 74 72";
constexpr char kConsume[] = "66 41 C7 47 44 00 00";

UpdateFn g_original = nullptr;
uint8_t* g_call = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false}, g_broken{false};
std::atomic<uint64_t> g_updates{0}, g_teleports{0}, g_resets{0}, g_fallbacks{0};

// Preflight the complete collection before changing anything. These objects are owned by the
// proxy; this game-thread path has already completed/released its parallel simulation task.
int Prepare(uint8_t* solver, uint8_t* proxy, uint8_t** cloths)
{
    __try {
        if (!solver || !proxy || *reinterpret_cast<void**>(proxy) ||
            *reinterpret_cast<uint8_t**>(proxy + 0x78) != solver) return 0;
        uint8_t* comp = *reinterpret_cast<uint8_t**>(proxy + 0x58);
        if (!comp || !(comp[0x918] & 8)) return 0;
        if (*reinterpret_cast<uint8_t**>(comp + 0x940) != proxy) return 0;
        const Array a = *reinterpret_cast<Array*>(proxy + 0x90);
        const Array b = *reinterpret_cast<Array*>(solver + 0x70);
        if (!a.data || !b.data || a.num < 1 || a.num > kMaxCloths || a.max < a.num ||
            b.num != a.num || b.max < b.num) return 0;
        for (int i = 0; i < a.num; ++i) {
            uint8_t* c = a.data[i];
            if (!c || c != b.data[i] || c[0x44] > 1 || c[0x45] > 1) return 0;
            for (int j = 0; j < i; ++j) if (cloths[j] == c) return 0;
            cloths[i] = c;
        }
        return a.num;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_broken.exchange(true)) Log("[frozencloth] unexpected layout/access: fix disabled for this process");
        return 0;
    }
}

void UpdateHook(uint8_t* solver, float dt, uint8_t* proxy)
{
    if (g_on.load(std::memory_order_relaxed) && !g_broken.load(std::memory_order_relaxed) && dt == 0.f) {
        uint8_t* cloths[kMaxCloths]{};
        const int n = Prepare(solver, proxy, cloths);
        if (n) {
            for (int i = 0; i < n; ++i) {
                if (cloths[i][0x44]) { ++g_resets; continue; } // preserve a pending reset
                if (!cloths[i][0x45]) { cloths[i][0x45] = 1; ++g_teleports; }
            }
            ++g_updates;
        } else ++g_fallbacks;
    }
    // Never swallow/retry exceptions from the original engine operation.
    g_original(solver, dt);
}

bool HasOne(const Section& section, const char* pattern)
{
    return FindAll(section, ParsePattern(pattern)).size() == 1;
}
bool Fail(const char* why) { Log("[frozencloth] %s: FrozenClothStretchFix unavailable", why); return false; }
} // namespace

bool InstallFrozenClothStretchFix(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) return Fail("installation requires game startup");
    const auto& game = Game();
    const auto calls = FindAll(game.text, ParsePattern(kCall));
    if (calls.size() != 1) return Fail("LOD-maintenance call not uniquely identified");
    uint8_t* call = calls[0] + kCallOffset;
    if (call - game.text.begin < 0x200) return Fail("invalid caller range");
    const Section caller{call - 0x200, call};
    uint8_t* update = RipTarget(call, 1, 5);
    if (!game.text.Contains(update) || !game.text.Contains(update + 0x110)) return Fail("invalid solver update");
    if (!HasOne(caller, kProxy) || !HasOne(caller, kArray) || !HasOne(game.text, kSuspended) ||
        !HasOne({update, update + 0x110}, kSolverCloths)) return Fail("proxy/component/solver layouts disagree");
    const auto teleports = FindAll(game.text, ParsePattern(kTeleport));
    if (teleports.size() != 1 || teleports[0] - game.text.begin < 0x100 ||
        !game.text.Contains(teleports[0] + 0xA00) ||
        !HasOne({teleports[0] - 0x100, teleports[0]}, kReset) ||
        !HasOne({teleports[0], teleports[0] + 0xA00}, kConsume)) return Fail("cloth reset/teleport semantics unverified");
    uint8_t* stub = AllocNearGame(32);
    if (!stub) return Fail("no nearby trampoline memory");
    // Third argument r8 = caller's nonvolatile r14 (proxy). Preserve rcx solver and xmm1 delta.
    const uint8_t code[] = {0x4D,0x8B,0xC6, 0xFF,0x25,0,0,0,0};
    memcpy(stub, code, sizeof code);
    void* hook = reinterpret_cast<void*>(&UpdateHook);
    memcpy(stub + sizeof code, &hook, sizeof hook);
    DWORD old;
    if (!VirtualProtect(stub, 32, PAGE_EXECUTE_READ, &old)) { VirtualFree(stub, 0, MEM_RELEASE); return Fail("trampoline protection"); }
    FlushInstructionCache(GetCurrentProcess(), stub, 32);
    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) { VirtualFree(stub, 0, MEM_RELEASE); return Fail("trampoline out of reach"); }
    uint8_t patch[5]{0xE8};
    const int32_t disp = static_cast<int32_t>(rel);
    memcpy(patch + 1, &disp, 4);
    g_original = reinterpret_cast<UpdateFn>(update);
    if (!WriteCode(call, patch)) { VirtualFree(stub, 0, MEM_RELEASE); return Fail("call patch failed"); }
    g_call = call; g_installed = true;
    Log("[frozencloth] suspended LOD call exe+0x%X, Solver::Update exe+0x%X: installed (per-cloth teleport; reset precedence retained)",
        static_cast<unsigned>(call - game.base), static_cast<unsigned>(update - game.base));
    return true;
}

void SetFrozenClothStretchFix(bool on)
{
    if (g_installed && g_on.exchange(on) != on) Log("[frozencloth] %s", on ? "enabled" : "disabled");
}
void LogFrozenClothStats()
{
    const auto n = g_updates.exchange(0), t = g_teleports.exchange(0), r = g_resets.exchange(0), f = g_fallbacks.exchange(0);
    if (n || f) Log("[frozencloth] %llu suspended LOD updates, %llu cloth teleports, %llu pending resets preserved, %llu fallbacks", n,t,r,f);
}
void* FrozenClothTestCallSite() { return g_call; }
void FrozenClothTestOriginal(void* fn) { g_original = reinterpret_cast<UpdateFn>(fn); }
void FrozenClothTestCall(uint8_t* solver, float dt, uint8_t* proxy) { UpdateHook(solver,dt,proxy); }
} // namespace opf
