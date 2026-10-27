// ChaosMaxPayloadSize ([Fixes] ChaosMaxPayloadSize, default 10000000 cm; 0 = the game's value; worker sqprobe 2026-10-27,
// public since 1.2.3).
//
// What it fixes: every Chaos scene query (the game thread's sweeps, overlaps and raycasts: pawn movement, physics
// controllers, overlap updates, traces; also the workers' Control Rig traces and the physics thread's broad phase) walks
// the acceleration tree's GlobalPayloads list element by element before it uses the tree. That list holds the elements
// "too big for the tree": bounds larger than p.MaxPayloadSize (UE 5.3 AABBTree.h, `Extents().Max() > MaxPayloadBounds`,
// 100000 cm = 1 km in this game). This game's static tree holds ~730 such elements, ~713 of them 1.6 km tall columns
// (z -80000..80000, 4-50 m wide) spread over the whole map, plus a few large boxes (up to 8 km). So each query tests ~730
// world-wide elements (a pre-filter call with three virtual calls, then a bounds test that rejects ~99 %) before it walks
// the tree, which finds what is near in ~14 element tests (measured with a lab probe: 117-154k global element
// tests per game-thread frame at the IC Waterfront, Vilverin and Skingrad sk-d; 95 % of all elements visited).
//
// What it does: before the engine reads its settings (DllMain on the version.dll route, OBSEPlugin_Load on OBSE64), it sets
// the console variable's bound value p.MaxPayloadSize (FBroadPhaseConfig::MaxPayloadSize, a float) from the game's 100000
// to the key's value (default when on: 10000000 = 100 km), so every acceleration tree the game builds keeps those
// elements in the tree. Only when the value is still the game's 100000 (a player's own Engine.ini line is applied later and
// still wins). The same as `p.MaxPayloadSize=10000000` under [SystemSettings] in Engine.ini. On the OBSE64 route the trees
// use the new value too; only the console's own copy of the variable (what the console prints for it) keeps showing
// 100000 there, as for NaniteInitialRootPages.
//
// Why the results are equivalent (not identical; review payrev 2026-10-27): the GlobalPayloads loop and the tree's leaves
// run the same element test on the same stored bounds (PrePreFilterHelper, then the AABB / ray / swept-box test on
// Elem.Bounds) and call the visitor with the same payload; tree nodes only skip elements whose bounds miss the query (a
// node's bounds contain its elements'). So an overlap visits the same set of elements and a sweep or raycast keeps the
// same closest hit. The one difference: stock always visits the >1 km bodies first, the fix in tree order. That matters
// only for exact ties between such a body and another one: a raycast starting inside both (distance 0) stops at the first
// visited, and multi-overlap arrays are in visit order. The columns do block raycasts (~0.5 narrow-phase visits per game
// frame at Vilverin / sk-d, ~11 % of them at distance 0), but their distance-0 stops kept the same share of all raycast
// visits with the fix (0.278 % vs 0.287 %): no other body took their place in the measured scenes. Ties among the tree's
// own bodies already depend on its shape, which stock varies (time-sliced rebuilds as cells stream in; the dynamic tree's
// insertion order).
// Nothing drawn changes (the render scene does not use these trees).
//
// Clean off: the name, the registration (lea r8,[value]; mov [rsp+20h],flags; lea rdx,[name]) and a read of the value
// elsewhere in the code must be found once, the value must be in .data and be 100000.0f; otherwise a log line, nothing set.
#include "common.h"

#include <cstring>
#include <cwchar>

namespace opf {
namespace {

float* g_var = nullptr;
float g_set = 0.0f;       // the value we wrote (0: none)
float g_lastLive = 0.0f;
bool g_liveLogged = false;

void Fail(const char* why) { Log("[chaosmps] ChaosMaxPayloadSize: %s: not set (the game's p.MaxPayloadSize)", why); }

// every 2-byte aligned place in the section that holds the UTF-16 text with its terminator (at most `max`)
int FindWideAll(const Section& s, const wchar_t* text, const uint8_t** out, int max)
{
    const size_t n = (wcslen(text) + 1) * sizeof(wchar_t);
    int found = 0;
    for (const uint8_t* p = s.begin; p + n <= s.end && found < max; p += 2)
        if (memcmp(p, text, n) == 0) out[found++] = p;
    return found;
}

}  // namespace

float* FindChaosMaxPayloadVar()
{
    if (g_var) return g_var;
    const GameModule& game = Game();
    const uint8_t* names[4] = {};
    const int nNames = FindWideAll(game.rdata, L"p.MaxPayloadSize", names, 4);
    if (!nNames) { Fail("the name p.MaxPayloadSize not found"); return nullptr; }
    const Pattern reg = ParsePattern("4C 8D 05 ?? ?? ?? ?? C7 44 24 20 ?? ?? ?? ?? 48 8D 15");
    float* var = nullptr;
    int regs = 0;
    for (uint8_t* p = game.text.begin + 15; p + 7 <= game.text.end; ++p) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;  // lea rdx,[name]
        const uint8_t* const t = RipTarget(p, 3, 7);
        bool isName = false;
        for (int i = 0; i < nNames; ++i) isName = isName || t == names[i];
        if (!isName) continue;
        ++regs;
        if (MatchesAt(p - 15, reg)) var = reinterpret_cast<float*>(RipTarget(p - 15, 3, 7));
    }
    if (regs != 1 || !var || !game.data.Contains(reinterpret_cast<uint8_t*>(var))) { Fail("its registration not as expected"); return nullptr; }
    // a read of the value outside the registration: movss xmm,[rip+value] (F3 0F 10 /r, mod 00 rm 101) or
    // cvtss2sd xmm,[rip+value] (F3 0F 5A /r)
    int reads = 0;
    for (uint8_t* p = game.text.begin; p + 8 <= game.text.end; ++p) {
        if (p[0] != 0xF3) continue;
        const uint8_t* q = p + 1;
        if ((*q & 0xF0) == 0x40) ++q;  // REX
        if (q[0] != 0x0F || (q[1] != 0x10 && q[1] != 0x5A) || (q[2] & 0xC7) != 0x05) continue;
        int32_t rel;
        memcpy(&rel, q + 3, 4);
        if (q + 7 + rel == reinterpret_cast<uint8_t*>(var)) ++reads;
    }
    if (reads < 1) { Fail("no read of its value found in the code"); return nullptr; }
    g_var = var;
    Log("[chaosmps] p.MaxPayloadSize at exe+0x%X (now %g; %d read(s) in the code)",
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(var) - game.base), static_cast<double>(*var), reads);
    return var;
}

// At start-up on both routes (DllMain on version.dll, OBSEPlugin_Load on OBSE64: both before the engine reads its settings
// and before any physics scene exists, like NaniteInitialRootPages); the game's 100000 only. The trees read the value at
// every structure creation (CreateAccelerationPerBucket_Threaded, its only reads), so an Engine.ini value applied later wins.
bool SetChaosMaxPayloadSize(int cm, bool beforeGameCode)
{
    if (cm <= 0) { Log("[chaosmps] ChaosMaxPayloadSize=0: the game's p.MaxPayloadSize (100000 cm)"); return false; }
    if (cm <= 100000 || cm > 1000000000) { Fail("value out of range (100001..1000000000 cm)"); return false; }
    float* const var = FindChaosMaxPayloadVar();
    if (!var) return false;
    if (*var != 100000.0f) {
        Log("[chaosmps] p.MaxPayloadSize is %g, not the game's 100000: left alone", static_cast<double>(*var));
        return false;
    }
    *var = static_cast<float>(cm);  // .data, writable; one aligned 4-byte store
    g_set = static_cast<float>(cm);
    Log("[chaosmps] ChaosMaxPayloadSize=%d: p.MaxPayloadSize 100000 -> %g cm%s (elements up to that size go into the acceleration "
        "trees instead of the list every query walks; an Engine.ini value still wins)", cm, static_cast<double>(*var),
        beforeGameCode ? "" : " on the OBSE64 route, before the engine starts");
    return true;
}

// Periodic report (main.cpp): the live value once, then whenever it changes (the game's own config or a later patch could
// set p.MaxPayloadSize after our start-up write).
void LogChaosMaxPayloadSizeLive()
{
    if (!g_var) return;
    const float v = *g_var;
    if (g_liveLogged && v == g_lastLive) return;
    g_liveLogged = true;
    g_lastLive = v;
    Log("[chaosmps] p.MaxPayloadSize now %g cm%s", static_cast<double>(v),
        g_set == 0.0f ? " (key off)" : v == g_set ? " (as set by ChaosMaxPayloadSize)" : " (NOT our value: changed by the game's or the player's settings)");
}

}  // namespace opf
