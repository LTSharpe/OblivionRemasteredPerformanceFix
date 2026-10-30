// FadeVtFlushSkip (switch, off by default): skips the virtual texture flush the game runs during a door's
// fade-in.
//
// Fade-to-game (exe+0x4877E80) arms, besides the unfreeze timer, a second timer when the transition asks for it
// (transition+0xBB): delay FadeInDuration (VAltarUserInterfaceSettings +0x188, 2.5 s) x 0.6, i.e. 1.5 s into the
// fade-in. Its delegate (vtable exe+0x7D7BCA8, Execute / ExecuteIfSafe thunks exe+0x4881630 / exe+0x48817D0) calls
// exe+0x48774E0, which runs the console command r.VT.Flush: every virtual texture page is dropped and fetched again
// while the picture is ~60-70 % faded in. At the Imperial City Waterfront lighthouse exit the distant walls and roofs
// lose their detail (-9..-17 % 1-px detail) and refill over ~2 s with VtStreamingUploads 64, ~8 s with the game's 4
// uploads per frame (docs/research.md, FadeVtFlushSkip).
//
// With FadeVtFlushSkip=1 (installed only if on at start-up, then switched live) the delegate's two .rdata slots point
// to stubs that skip the call (and log it) while the key is on and pass through to the game's thunks otherwise. The
// timer itself is armed and fires as before. Why the game flushes is not known (fresh pages for the new location, e.g.
// runtime virtual textures generated before the area had loaded): skipping it changes the picture during the fade (no
// drop and refill) and, in 14 transitions x 4 runs, not the settled picture beyond run-to-run noise
// (docs/research.md). Not "identical output" in this project's sense, so off by default.
//
// Search: the flush function by its prologue up to `lea rdx,[name]`, the name being the only UTF-16 "r.VT.Flush" in
// .rdata; the two thunks (add rcx,18h / jmp flush; and the ExecuteIfSafe wrapper calling it, returning true) by
// pattern and target; each thunk's .rdata slot by value (exactly one each, adjacent). Anything else: not installed.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr wchar_t kName[] = L"r.VT.Flush";
// mov [rsp+18h],rbx / push r14 / sub rsp,30h / mov r14,rcx / mov rcx,[console manager] / test / jnz / call / mov rcx,[..]
// / mov rax,[rcx] / lea rdx,[L"r.VT.Flush"] / mov r8b,1 / call [rax+0C0h] (FindConsoleObject)
constexpr char kFlush[] =
    "48 89 5C 24 18 41 56 48 83 EC 30 4C 8B F1 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
    "48 8B 01 48 8D 15 ?? ?? ?? ?? 41 B0 01 FF 90 C0 00 00 00";
constexpr size_t kFlushName = 0x29;
constexpr char kExec[] = "48 83 C1 18 E9 ?? ?? ?? ??";
constexpr char kExecIfSafe[] = "48 83 EC 28 48 83 C1 18 E8 ?? ?? ?? ?? B0 01 48 83 C4 28 C3";

using ExecFn = void (*)(void* delegateInstance);
using ExecIfSafeFn = bool (*)(void* delegateInstance);

void** g_execSlot = nullptr;
void** g_execIfSafeSlot = nullptr;
ExecFn g_origExec = nullptr;
ExecIfSafeFn g_origExecIfSafe = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_skipped{0}, g_passed{0};

const uint8_t* FindName(const Section& s)
{
    const size_t bytes = sizeof(kName);
    const uint8_t* hit = nullptr;
    for (const uint8_t* p = s.begin; p + bytes <= s.end; p += 2) {
        if (memcmp(p, kName, bytes) != 0) continue;
        if (hit) return nullptr;
        hit = p;
    }
    return hit;
}

void** SlotOf(const GameModule& game, const uint8_t* fn)
{
    void** found = nullptr;
    int hits = 0;
    for (uint8_t* p = game.rdata.begin; fn && p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uint8_t* const*>(p) == fn) { found = reinterpret_cast<void**>(p); ++hits; }
    return hits == 1 ? found : nullptr;
}

void Skipped()
{
    const uint32_t n = ++g_skipped;
    if (n <= 20) Log("[vtflush] skipped the fade-in's r.VT.Flush (%u so far)", n);
}

void ExecHook(void* delegateInstance)
{
    if (g_on.load(std::memory_order_relaxed)) { Skipped(); return; }
    ++g_passed;
    g_origExec(delegateInstance);
}

bool ExecIfSafeHook(void* delegateInstance)
{
    if (g_on.load(std::memory_order_relaxed)) { Skipped(); return true; }  // bound and safe; the call just does nothing
    ++g_passed;
    return g_origExecIfSafe(delegateInstance);
}

bool Find(FadeVtFlushInfo* out)
{
    const GameModule& game = Game();
    const uint8_t* const name = FindName(game.rdata);
    if (!name) { Log("[vtflush] the name r.VT.Flush not found (once) in the game: not installed"); return false; }
    uint8_t* flush = nullptr;
    int n = 0;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kFlush))) {
        if (RipTarget(p + kFlushName, 3, 7) != name) continue;
        flush = p;
        ++n;
    }
    if (n != 1) { Log("[vtflush] the fade's flush function found %d times (expected once): not installed", n); return false; }
    uint8_t* exec = nullptr;
    uint8_t* execIfSafe = nullptr;
    int ne = 0, ns = 0;
    for (uint8_t* t : FindAll(game.text, ParsePattern(kExec))) if (RipTarget(t, 5, 9) == flush) { exec = t; ++ne; }
    for (uint8_t* t : FindAll(game.text, ParsePattern(kExecIfSafe))) if (RipTarget(t, 9, 13) == flush) { execIfSafe = t; ++ns; }
    void** const es = (ne == 1) ? SlotOf(game, exec) : nullptr;
    void** const ess = (ns == 1) ? SlotOf(game, execIfSafe) : nullptr;
    if (!es || !ess || ess != es + 1) {
        Log("[vtflush] the flush timer's delegate thunks (%d/%d) or their slots (%p/%p) not as expected: not installed", ne, ns,
            static_cast<void*>(es), static_cast<void*>(ess));
        return false;
    }
    g_execSlot = es;
    g_execIfSafeSlot = ess;
    g_origExec = reinterpret_cast<ExecFn>(exec);
    g_origExecIfSafe = reinterpret_cast<ExecIfSafeFn>(execIfSafe);
    if (out) {
        out->flush = flush;
        out->exec = exec;
        out->execIfSafe = execIfSafe;
        out->execSlot = es;
        out->execIfSafeSlot = ess;
    }
    Log("[vtflush] fade-in VT flush exe+0x%X (r.VT.Flush), timer delegate thunks exe+0x%X/+0x%X in slots exe+0x%X/+0x%X",
        static_cast<unsigned>(flush - game.base), static_cast<unsigned>(exec - game.base),
        static_cast<unsigned>(execIfSafe - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(es) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(ess) - game.base));
    return true;
}

}  // namespace

bool FindFadeVtFlush(FadeVtFlushInfo* out) { return Find(out); }

bool InstallFadeVtFlushSkip()
{
    if (g_installed) return true;
    if (!g_execSlot && !Find(nullptr)) return false;
    if (!WritePointer(g_execSlot, reinterpret_cast<void*>(&ExecHook))) {
        Log("[vtflush] writing the delegate's Execute slot failed: not installed");
        return false;
    }
    if (!WritePointer(g_execIfSafeSlot, reinterpret_cast<void*>(&ExecIfSafeHook))) {
        WritePointer(g_execSlot, reinterpret_cast<void*>(g_origExec));
        Log("[vtflush] writing the delegate's ExecuteIfSafe slot failed: not installed");
        return false;
    }
    g_installed = true;
    Log("[vtflush] installed: the fade-in's r.VT.Flush can be skipped (FadeVtFlushSkip, )");
    return true;
}

void SetFadeVtFlushSkip(bool on)
{
    if (!g_installed) return;
    if (g_on.exchange(on) != on) Log("[vtflush] FadeVtFlushSkip %s", on ? "on: the fade-in's r.VT.Flush is skipped" : "off: the game's flush runs");
}

void LogFadeVtFlushStats()
{
    if (!g_installed) return;
    Log("[vtflush] fade-in flushes skipped %u, passed to the game %u", g_skipped.load(), g_passed.load());
}

void FadeVtFlushTestCall(void* delegateInstance, bool ifSafe)
{
    if (ifSafe) ExecIfSafeHook(delegateInstance);
    else ExecHook(delegateInstance);
}

void FadeVtFlushTestSetup(void** execSlot, void** execIfSafeSlot, void* origExec, void* origExecIfSafe, bool installed)
{
    g_execSlot = execSlot;
    g_execIfSafeSlot = execIfSafeSlot;
    g_origExec = reinterpret_cast<ExecFn>(origExec);
    g_origExecIfSafe = reinterpret_cast<ExecIfSafeFn>(origExecIfSafe);
    g_installed = installed;
    g_on = false;
    g_skipped = 0;
    g_passed = 0;
}

uint32_t FadeVtFlushSkippedForTest() { return g_skipped.load(); }

}  // namespace opf
