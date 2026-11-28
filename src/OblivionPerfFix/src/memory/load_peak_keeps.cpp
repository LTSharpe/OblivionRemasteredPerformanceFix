// LoadPeakKeepsBack (worker s001fix 2026-11-25, ported to the public tree by s001port 2026-11-26; S-001): our own video-memory
// keeps are given back while a loading screen is up and the game is near its video memory budget, so they are not part of
// the load's peak. Also the loading-screen state that this key and RtSceneBufferGrowToPeak read (LoadingScreenNow).
//
// Why (bench s001fix r5: a real map fast travel at a constant outside video memory): the last second of a fast travel holds
// the old and the new area at once (11993 MB against 10391 MB five seconds later), for one second over the DXGI budget (11859
// MB). Windows answers by demoting 512-768 MB of the game to system RAM, and pages 2.3-2.9 GB back in during the next 90 s;
// every new block of video memory waits in that paging queue: 4-8 paging waits of 36-175 ms in each ride after a fast travel,
// none in 3 min of the same ride from a fresh start (the player's "slower after a fast travel / a load"). In that peak second
// our own fixes held 380-460 MB the game itself would not have: UploadMemoryKeep's empty upload pools (248-272 MB) and
// GpuPoolSpareAhead's spares (132-164 MB). These keeps exist to save a creation in play; behind a loading screen a creation
// costs nothing the player can see, so there they only add to the peak.
//
// With the key on: once a second (the monitor thread), while the loading state (below) is a loading screen and the game's video
// memory (now, or its highest in the last 60 s) is within LoadPeakKeepsBackMarginMB (hidden, 2560) of the budget, MemoryRelief's
// "upload" stage is on: UploadMemoryKeep hands its empty upload pools back (the game's own 20-frame rule, no release ramp, and
// only in frames whose game-thread state is still the loading screen: a screen that ends before the hand-back keeps them, so the
// hand-back's one long clean-up frame never reaches the fade-in), GpuPoolKeepMB keeps one pool per allocator per frame less,
// GpuPoolSpareAhead destroys its spares. After the screen everything is as configured again; the keeps fill up by themselves.
// "Near" is a size, not a share: what the load holds twice for a second is an area (~1.6 GB on our 16 GB card) plus our keeps
// (0.4-0.5 GB), whatever the card. On a 16 GB card (budget 12.6-12.9 GB) 2560 MB acts as the former 80 % did (7 of 7 loads in
// a play session); on an 8 GB card most outdoor loads act, which is right there (the action is the game's own behaviour).
// The 60 s look-back means a door from a busy outdoor area into an interior acts too (the keeps then start empty in the
// interior; nothing else differs: they refill there). The budget used is the lowest Windows gave the game in the last 10
// minutes, but never under 70 % of the highest of the session (one low sample, e.g. while another program held video memory or
// a display-mode change, cannot pin the rule for the rest of the session; review s001rev S2-2). The budget is two-valued in our
// runs (15.4 GB without contention, 10.8-11.9 GB from the first fast travel on), so the lowest is the honest one at a load.
// Identical results: the keeps go back to the game's own behaviour for the length of a loading screen (their own equivalence:
// placement only). Measured (bench r7 / r8, 4 + 4 cycles, with RtSceneBufferGrowToPeak on in both arms): slow new blocks >= 20 ms
// per ride 20 off against 7 on, drops with a peak >= 50 ms 11 against 3; no longer loading screens.
//
// The loading state (review s001rev section 3): written once per game-thread frame from NpcCreationPacing's frame hook with the
// hitch ledger's rule (a travel pending, or the freeze subsystem's loading mode with no named freeze: 1; else 0), read by the
// monitor and render threads with one relaxed load. -1 = unknown (no frame hook: NpcCreationPacing=0; no reader key on; the
// freeze subsystem not found): both keys then do nothing. The frame hook reads the freeze state only while a reader key is on.
// [Fixes] LoadPeakKeepsBack = 0 (default until the release): off; 1: on. Live. Hidden LoadPeakKeepsBackMarginMB (512..8192, 2560).
#include "common.h"

#include <atomic>
#include <windows.h>

namespace opf {
namespace {
std::atomic<int> g_loadingNow{-1};
std::atomic<bool> g_loadingWanted{false};
std::atomic<bool> g_handBackGate{false};
std::atomic<bool> g_near{false};     // the last tick: video memory (now or the 60-s peak) within the margin of the budget
std::atomic<bool> g_drainOn{false};  // LoadPeakDrain

constexpr int kRing = 60;           // seconds of video memory history
constexpr int kBudgetWindow = 600;  // seconds of budget history (the lowest of these is used)
uint64_t g_ring[kRing] = {};        // MB
int g_ringAt = 0;
uint32_t g_budgets[kBudgetWindow] = {};  // MB, 0 = no sample
int g_budgetAt = 0;
uint64_t g_sessionMaxBudget = 0;    // MB
uint64_t g_loggedBudget = 0;
bool g_active = false, g_onLogged = false, g_unknownLogged = false;
uint64_t g_episodes = 0, g_skipped = 0, g_skippedLogged = 0;
ULONGLONG g_since = 0, g_lastStats = 0;
uint64_t g_usedAtStart = 0;
unsigned g_unknownSeconds = 0;
}  // namespace

int LoadingStateOf(int32_t named, int32_t mode, bool travel, bool known)
{
    if (travel) return 1;
    if (!known) return -1;
    return named == 0 && mode == 1 ? 1 : 0;
}

void LoadingStateFrame(bool travelPending)
{
    if (!g_loadingWanted.load(std::memory_order_relaxed)) {
        if (g_loadingNow.load(std::memory_order_relaxed) != -1) g_loadingNow.store(-1, std::memory_order_relaxed);
        return;
    }
    int32_t named = -1, mode = -1;
    const bool known = ReadMenuFreeze(&named, &mode);
    g_loadingNow.store(LoadingStateOf(named, mode, travelPending, known), std::memory_order_relaxed);
}

int LoadingScreenNow() { return g_loadingNow.load(std::memory_order_relaxed); }

void SetLoadingStateWanted(bool on) { g_loadingWanted.store(on, std::memory_order_relaxed); }

bool LoadPeakHandBackNow() { return !g_handBackGate.load(std::memory_order_relaxed) || LoadingScreenNow() == 1; }

void SetLoadPeakHandBackGate(bool on) { g_handBackGate.store(on, std::memory_order_relaxed); }

bool LoadPeakDrainNow()
{
    return g_drainOn.load(std::memory_order_relaxed) && g_near.load(std::memory_order_relaxed) && LoadingScreenNow() == 1;
}

void SetLoadPeakDrain(bool on) { g_drainOn.store(on, std::memory_order_relaxed); }

uint64_t LoadPeakBudgetFloor(uint64_t windowMin, uint64_t sessionMax)
{
    const uint64_t floor = sessionMax * 7 / 10;
    return windowMin > floor ? windowMin : floor;
}

// The decision alone (self-test), MB.
bool LoadPeakKeepsDecide(bool on, int loading, uint64_t usedNow, uint64_t peakRecent, uint64_t budget, uint64_t marginMB)
{
    if (!on || loading != 1 || !budget) return false;
    if (marginMB < 512) marginMB = 512;
    if (marginMB > 8192) marginMB = 8192;
    const uint64_t used = usedNow > peakRecent ? usedNow : peakRecent;
    return used + marginMB >= budget;
}

// Monitor thread, once a second. True: our video-memory keeps are given back now (and without the upload release ramp).
bool LoadPeakKeepsTick(bool on, int marginMB)
{
    uint64_t usedB = 0, budgetB = 0;
    const bool known = LatestGameVram(&usedB, &budgetB);
    const uint64_t used = usedB >> 20, budgetNow = budgetB >> 20;
    uint64_t peak = 0;
    for (const uint64_t v : g_ring) peak = v > peak ? v : peak;  // the last 60 s, before this second
    if (known) {
        g_ring[g_ringAt] = used;
        g_ringAt = (g_ringAt + 1) % kRing;
        if (budgetNow) {
            g_budgets[g_budgetAt] = static_cast<uint32_t>(budgetNow);
            g_budgetAt = (g_budgetAt + 1) % kBudgetWindow;
            if (budgetNow > g_sessionMaxBudget) g_sessionMaxBudget = budgetNow;
        }
    }
    uint64_t windowMin = 0;
    for (const uint32_t b : g_budgets)
        if (b && (!windowMin || b < windowMin)) windowMin = b;
    const uint64_t budget = windowMin ? LoadPeakBudgetFloor(windowMin, g_sessionMaxBudget) : 0;
    const int margin = marginMB < 512 ? 512 : marginMB > 8192 ? 8192 : marginMB;
    const int loading = LoadingScreenNow();
    if (on && !g_onLogged) {
        g_onLogged = true;
        Log("[loadkeeps] LoadPeakKeepsBack on: behind a loading screen, with the game's video memory within %d MB of its budget, our "
            "keeps (UploadMemoryKeep, GpuPoolKeepMB, GpuPoolSpareAhead) are given back until the screen ends",
            margin);
    }
    if (!on) g_onLogged = false;  // off -> on logs again (review S3-2)
    // the loading state is unknown for a while with the key on: say why once
    if (on && loading < 0) {
        if (++g_unknownSeconds == 30 && !g_unknownLogged) {
            g_unknownLogged = true;
            Log("[loadkeeps] no loading-screen state after 30 s (NpcCreationPacing's frame hook not installed, or the freeze state not "
                "found): LoadPeakKeepsBack and RtSceneBufferGrowToPeak do nothing");
        }
    } else g_unknownSeconds = 0;
    if (known && budget && budget != g_loggedBudget &&
        (!g_loggedBudget || (budget > g_loggedBudget ? budget - g_loggedBudget : g_loggedBudget - budget) * 20 >= g_loggedBudget)) {
        if (on)
            Log("[loadkeeps] budget used: %llu MB (lowest of the last 10 min %llu MB, highest of the session %llu MB, floor 70 %%)",
                static_cast<unsigned long long>(budget), static_cast<unsigned long long>(windowMin),
                static_cast<unsigned long long>(g_sessionMaxBudget));
        g_loggedBudget = budget;
    }
    g_near.store(known && LoadPeakKeepsDecide(true, 1, used, peak, budget, static_cast<uint64_t>(margin)), std::memory_order_relaxed);
    const bool next = known && LoadPeakKeepsDecide(on, loading, used, peak, budget, static_cast<uint64_t>(margin));
    if (on && known && loading == 1 && !next && !g_active) ++g_skipped;  // a loading-screen second with room
    if (next != g_active) {
        const ULONGLONG now = GetTickCount64();
        if (next) {
            g_since = now;
            g_usedAtStart = used;
            ++g_episodes;
            Log("[loadkeeps] loading screen near the budget (video memory %llu MB now, %llu MB at most in the last minute, budget %llu MB): "
                "our keeps given back (%llu)",
                static_cast<unsigned long long>(used), static_cast<unsigned long long>(peak), static_cast<unsigned long long>(budget),
                static_cast<unsigned long long>(g_episodes));
        } else if (!on) {
            Log("[loadkeeps] LoadPeakKeepsBack switched off during a loading screen: our keeps as configured again");
        } else {
            Log("[loadkeeps] loading screen over after %.1f s (video memory %llu MB, was %llu MB): our keeps as configured again",
                (now - g_since) / 1000.0, static_cast<unsigned long long>(used), static_cast<unsigned long long>(g_usedAtStart));
        }
        g_active = next;
    }
    // stats (review S3-1): loading-screen seconds left alone, once a minute when they changed
    const ULONGLONG t = GetTickCount64();
    if (on && g_skipped != g_skippedLogged && t - g_lastStats >= 60000) {
        g_lastStats = t;
        Log("[loadkeeps] %llu loading screens acted on so far; %llu loading-screen seconds with room left alone",
            static_cast<unsigned long long>(g_episodes), static_cast<unsigned long long>(g_skipped));
        g_skippedLogged = g_skipped;
    }
    return g_active;
}

}  // namespace opf
