// CreatureAnimPreload (lab kfload / kfbuild 2026-11-13; [Fixes] CreatureAnimPreload, default 0 until a release; 1 = on).
// Located at start-up, then live 0 / 1 (one byte).
//
// Game behaviour (the same in the 2006 game, not this mod's): when a creature's 3D is built, the old engine asks for
// every animation file (.kf) of the creature's skeleton folder by name. A file that is not in the old engine's animation
// cache is loaded right there, on the old-engine thread: first it pauses the old engine's background loader threads and
// waits, without a time limit, for each to finish its current task (tens of ms when a cell is loading), then it reads the
// file, then it lets the loaders go again; the game thread waits for the old-engine thread meanwhile (one long frame,
// up to 64 ms in play). The step that queues a creature for loading normally asks the background loader to read those
// files first, but only when the creature's skeleton model is not cached. The two caches are emptied by different rules:
// an animation file is dropped as soon as no actor uses it, a skeleton model stays until the next loading screen. So a
// creature type that comes back after all its instances unloaded finds its skeleton cached, queues no animation files,
// and loads every one of them in the foreground with a loader pause each.
// Summoned creatures already take the other path: their queue step passes "queue the folder's animations anyway"
// (QueuedMagicItem's flags (1, 1)). With this key the creature queue step passes the same flag, so a returning creature's
// missing files are read by the background loader (the stock QueuedKF task, inserted by its stock finish step; several
// creatures of one kind queued together may each read a missing file, as on a kind's first load: the first copy is
// inserted, the others are adopted and dropped) before its 3D is completed; files that are cached become requests that
// are finished at once, on the queuing thread (QueuedKF +0x50 notifies the creature's task synchronously; its completion
// is not delayed by them). The creature's 3D then completes after its files are read, behind whatever the background
// loader is doing (as on a creature kind's first load), not with the game stopped meanwhile. Same files, same animations.
//
// How: QueuedCreature's queue step (vtable slot 11 of the old engine's QueuedCreature, exe+0x67EBF60 in build 19115871)
// calls QueueModelAnims(loader, model, priority, parent, ref, flag6 = 1, flag7 = 0); the immediate of
// "mov byte [rsp+30h], 0" (flag7) becomes 1. Checked before writing: the site is unique in the exe and the byte is 0 (a
// game build that already passes 1 needs nothing: refused), its call target starts with QueueModelAnims's prologue, and
// the "flag7 then flag6" test occurs exactly once in that function's first 0x400 bytes. Any mismatch: unavailable, one
// log line, nothing written.
#include "common.h"

#include <atomic>

namespace opf {
namespace {

// QueuedCreature slot 11 (exe+0x67EBF8A): mov eax,[rbx+10h]; mov r9,rbx; mov rcx,[ModelLoader]; mov rdx,rdi (model);
// sar rax,10h; movzx r8d,al (priority); mov rax,[rbx+28h] (ref); mov byte [rsp+30h],0 (flag7); mov byte [rsp+28h],1 (flag6);
// mov [rsp+20h],rax; call QueueModelAnims (exe+0x67E9A00)
constexpr char kSite[] =
    "8B 43 10 4C 8B CB 48 8B 0D ?? ?? ?? ?? 48 8B D7 48 C1 F8 10 44 0F B6 C0 48 8B 43 28 C6 44 24 30 ?? "
    "C6 44 24 28 01 48 89 44 24 20 E8 ?? ?? ?? ??";
constexpr size_t kFlag7 = 32;  // the immediate of "mov byte [rsp+30h], imm8"
constexpr size_t kCall = 43;   // E8 rel32
// QueueModelAnims's prologue: push rbp, rbx, rdi, r12-r15; lea rbp,[rsp-1A0h]; sub rsp,2A0h
constexpr char kQueuePrologue[] =
    "40 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 FE FF FF 48 81 EC A0 02 00 00";
// ... and its decision: cmp [rbp+210h] (flag7), sil(0); je skip; cmp [rbp+208h] (flag6), sil; je skip
constexpr char kDecision[] = "40 38 B5 10 02 00 00 0F 84 ?? ?? ?? ?? 40 38 B5 08 02 00 00 0F 84";
constexpr size_t kDecisionSearch = 0x400;

uint8_t* g_site = nullptr;
std::atomic<int> g_on{-1};  // -1 = not written yet

}  // namespace

uint8_t* CreatureAnimPreloadLocate(const GameModule& game, const char** why)
{
    const auto hits = FindAll(game.text, ParsePattern(kSite));
    if (hits.size() != 1) {
        *why = hits.empty() ? "the creature queue step was not found" : "the creature queue step is not unique";
        return nullptr;
    }
    uint8_t* const at = hits[0];
    uint8_t* const fn = RipTarget(at + kCall, 1, 5);
    if (!game.text.Contains(fn) || !game.text.Contains(fn + kDecisionSearch) || !MatchesAt(fn, ParsePattern(kQueuePrologue))) {
        *why = "the queue step's call target is not the animation queue function";
        return nullptr;
    }
    const Pattern decision = ParsePattern(kDecision);
    int found = 0;
    for (size_t i = 0; i < kDecisionSearch; ++i)
        if (MatchesAt(fn + i, decision)) ++found;
    if (found != 1) {
        *why = "the animation queue function's flag test is not where expected";
        return nullptr;
    }
    if (at[kFlag7] != 0) {
        // Located once, at start-up, before anything is written: a 1 here is the game's own (a build that already queues a
        // creature's animation files), any other value is unknown code. Either way there is nothing to change.
        *why = at[kFlag7] == 1 ? "the game already queues creature animation files (flag byte 1)"
                               : "the flag byte holds an unexpected value";
        return nullptr;
    }
    return at + kFlag7;
}

bool FindCreatureAnimPreload()
{
    if (g_site) return true;
    const GameModule& game = Game();
    const char* why = "";
    uint8_t* site = CreatureAnimPreloadLocate(game, &why);
    if (!site) {
        Log("[creatureanim] %s: CreatureAnimPreload unavailable (nothing changed)", why);
        return false;
    }
    g_site = site;
    Log("[creatureanim] creature queue step's flag byte at exe+0x%X (%u)", static_cast<unsigned>(site - game.base), *site);
    return true;
}

void SetCreatureAnimPreload(bool enabled)
{
    if (!g_site || g_on.load() == static_cast<int>(enabled)) return;
    // One byte of an immediate, written only by this function; the old-engine thread reads it each time a creature is queued.
    const uint8_t v = enabled ? 1 : 0;
    if (*g_site != v && !WriteCode(g_site, std::span<const uint8_t>(&v, 1))) {
        Log("[creatureanim] VirtualProtect failed: CreatureAnimPreload not switched");
        return;
    }
    const bool first = g_on.exchange(enabled ? 1 : 0) < 0;
    if (enabled || !first)
        Log("[creatureanim] %s", enabled ? "a creature's missing animation files are loaded in the background (as for summons)"
                                         : "creature animation files as before (a returning creature type may load them in the foreground)");
}

uint8_t* CreatureAnimPreloadSite() { return g_site; }

}  // namespace opf
