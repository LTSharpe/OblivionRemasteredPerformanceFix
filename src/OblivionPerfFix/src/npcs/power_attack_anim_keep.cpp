// PowerAttackAnimKeep (lab humananim 2026-11-15; [Fixes] PowerAttackAnimKeep, default 0; 1 = on).
// Installed at start-up when != 0 (version.dll route), then live 0 / 1.
//
// Game behaviour (not this mod's): the old engine keeps its animation files (.kf) in a map, each entry with a use
// count. When a character's or creature's 3D is built, the game asks for every file of the skeleton's folder by name,
// and a file that is not in the map is loaded right there, on the old-engine thread: first it pauses the old engine's
// background loader threads and waits, without a time limit, for each to finish its current task (tens of ms when a
// cell is loading), then it reads the file, then it lets the loaders go again; the game thread waits for the old-engine
// thread meanwhile (86 ms in one frame of a recorded ride, 72 ms of it the waits for the loaders, ours).
// For people the game counts on the folder's files being in the map, because every person in the world, the player
// included, keeps the ones it uses. The power-attack files (animation groups 0x16-0x1A) are different: a person asks
// for them with the rest of the folder and lets go of them at once, with a bare decrement that leaves the entry in the
// map at count 0; and the game's clean-up of unused entries on loading screens skips entries of exactly these groups
// (count 0 and group 0x16-0x1A: kept). We read that rule as: these files may stay loaded while nobody uses them.
// But a character that holds one (a separate step adds power-attack animations to some characters; which ones we
// have not established) keeps its count above 0, and when the last holder lets go, the release function takes the
// entry out of the map and destroys it, because its count reached 0 by that release. The next person built then
// loads the file again in the foreground, one loader pause per file.
// With this key that one step is left out for power-attack entries: when a release brings such an entry's count to 0,
// the entry stays in the map at count 0, the state the game itself leaves these files in after every person's build.
// Nothing else changes: the decrement is the game's, every other group is removed and destroyed as before, and a
// later request finds the entry and counts it up again (the game's own path for an entry at count 0).
// What stays loaded longer: power-attack files that the game had loaded and would have destroyed at such a release.
// They follow the game's own rules for a power-attack entry at count 0: the loading purge keeps them, and the only
// other code that destroys them is the tear-down of the whole loader (exe+0x67E4120, before the loader object is
// freed), so they stay until the game closes, as the files that people only asked for and dropped already do. At
// most the power-attack files of the folders met in a session: 33 files / 1.4 MB on disk for the people's folder, 259
// files / 6.0 MB on disk for every folder of the game together (people, first person, idle animations, every creature
// kind; counted by the animation's group name in each file). The size in memory was not measured.
// Why only these groups: they are the only ones the game itself leaves at count 0 and keeps through its purge, and
// the only ones a person's build loaded in the foreground in play (17 of 17 files of that step in about 60 probed
// minutes); every other group of the people's folder is held by every person in the world, the player included, and
// creatures' other files are requested ahead by CreatureAnimPreload.
//
// How: the release function (exe+0x67EC8A0 in build 19115871; (loader, name, remove)) decrements the entry's count and,
// with remove set, re-reads it: "mov rax,[rsp+30h]; mov ecx,[rax+18h]; test ecx,ecx; jne done" (exe+0x67EC8E3), then
// removes the entry from the map and destroys it. The first two instructions (8 bytes) become "call stub; mov ecx,eax;
// nop"; rax still holds the entry there (loaded at exe+0x67EC8D5, not written since), the stub is "mov rcx,rax;
// jmp [hook]", and the hook returns the count the game would have read, or 1 when the count is 0, the key is on and the
// entry's group is a power attack: the game then takes its own "still in use" branch. Group = the low byte of the word
// at [[entry+10h]+10h], the game's own getter (exe+0x67E2580).
// Checked before writing: the release tail is unique; the loading purge's test of the same fields (count at +18h,
// sequence at +10h, group in 0x16..0x1A kept; exe+0x67E74E6) is unique, both of its getter calls go to one function
// that is exactly the 8-byte getter, and its destroy call goes to the function the release's destroy call goes to.
// Any mismatch: not installed, one log line, nothing written.
#include "common.h"

#include <atomic>

namespace opf {
namespace {

// The release's tail (exe+0x67EC8D5): mov rax,[rsp+30h]; lock dec [rax+18h]; test sil,sil; je done; mov rax,[rsp+30h];
// mov ecx,[rax+18h]; test ecx,ecx; jne done; mov rcx,[rdi+8]; mov rdx,rbx; mov rax,[rcx]; call [rax+28h] (map remove);
// mov rcx,[rsp+30h]; test rcx,rcx; je done; mov edx,1; call destroy
constexpr char kRelease[] =
    "48 8B 44 24 30 F0 FF 48 18 40 84 F6 74 2D 48 8B 44 24 30 8B 48 18 85 C9 75 21 48 8B 4F 08 48 8B D3 48 8B 01 FF 50 28 "
    "48 8B 4C 24 30 48 85 C9 74 0A BA 01 00 00 00 E8 ?? ?? ?? ??";
constexpr size_t kPatch = 14;         // "mov rax,[rsp+30h]; mov ecx,[rax+18h]" (8 bytes)
constexpr size_t kReleaseDestroy = 54;  // E8 rel32
// The loading purge's rule for an animation entry (exe+0x67E74E6): mov eax,[rcx+18h]; test eax,eax; jne next;
// mov rbx,[rbp+28h]; mov rcx,[rbx+10h]; test rcx,rcx; je remove; call group; cmp eax,16h; jl remove; mov rcx,[rbx+10h];
// call group; cmp eax,1Bh; jl next; remove: mov rcx,[rsi+8]; mov rax,[rcx]; mov rdx,rdi; call [rax+28h]; mov edx,1;
// mov rcx,rbx; call destroy
constexpr char kPurge[] =
    "8B 41 18 85 C0 75 ?? 48 8B 5D 28 48 8B 4B 10 48 85 C9 74 ?? E8 ?? ?? ?? ?? 83 F8 16 7C ?? 48 8B 4B 10 E8 ?? ?? ?? ?? "
    "83 F8 1B 7C ?? 48 8B 4E 08 48 8B 01 48 8B D7 FF 50 28 BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ??";
constexpr size_t kPurgeGroup1 = 20, kPurgeGroup2 = 34, kPurgeDestroy = 65;  // E8 rel32 each
// The group getter (exe+0x67E2580): movzx eax, word [rcx+10h]; movzx eax, al; ret
constexpr char kGetter[] = "0F B7 41 10 0F B6 C0 C3";

constexpr size_t kEntrySequence = 0x10, kEntryCount = 0x18, kSequenceGroup = 0x10;
constexpr uint8_t kPowerFirst = 0x16, kPowerLast = 0x1A;

std::atomic<int> g_on{0};
std::atomic<uint64_t> g_kept{0};
bool g_installed = false;
uint8_t* g_site = nullptr;  // the 8 patched bytes


// What the game's "mov ecx, [entry+18h]" would have read, or 1 for a power-attack entry at count 0 with the key on.
uint32_t __fastcall CountHook(const uint8_t* entry)
{
    const uint32_t count = *reinterpret_cast<const volatile uint32_t*>(entry + kEntryCount);
    if (count != 0 || !g_on.load(std::memory_order_relaxed)) return count;
    const uint8_t* const sequence = *reinterpret_cast<const uint8_t* const*>(entry + kEntrySequence);
    if (!sequence) return 0;
    const uint8_t group = static_cast<uint8_t>(*reinterpret_cast<const uint16_t*>(sequence + kSequenceGroup));
    if (group < kPowerFirst || group > kPowerLast) return 0;
    g_kept.fetch_add(1, std::memory_order_relaxed);
    return 1;  // the game's own "still in use" branch: the entry stays in the map at count 0, nothing is destroyed
}

bool Discover(const GameModule& game, PowerAttackAnimKeepDiscovery* out, const char** why)
{
    *out = {};
    const auto rel = FindAll(game.text, ParsePattern(kRelease));
    out->releases = static_cast<int>(rel.size());
    if (rel.size() != 1) { *why = rel.empty() ? "the animation release's tail was not found" : "the animation release's tail is not unique"; return false; }
    const auto purge = FindAll(game.text, ParsePattern(kPurge));
    out->purges = static_cast<int>(purge.size());
    if (purge.size() != 1) { *why = purge.empty() ? "the loading purge's power-attack rule was not found" : "the loading purge's power-attack rule is not unique"; return false; }
    uint8_t* const getter = RipTarget(purge[0] + kPurgeGroup1, 1, 5);
    if (getter != RipTarget(purge[0] + kPurgeGroup2, 1, 5) || !game.text.Contains(getter) || !game.text.Contains(getter + 8) ||
        !MatchesAt(getter, ParsePattern(kGetter))) {
        *why = "the purge's group getter is not the one expected";
        return false;
    }
    uint8_t* const destroy = RipTarget(rel[0] + kReleaseDestroy, 1, 5);
    if (!game.text.Contains(destroy) || destroy != RipTarget(purge[0] + kPurgeDestroy, 1, 5)) {
        *why = "the release and the purge do not destroy an entry with the same function";
        return false;
    }
    out->site = rel[0] + kPatch;
    out->purge = purge[0];
    out->getter = getter;
    out->destroy = destroy;
    return true;
}

bool Install(const GameModule& game, const char** why)
{
    PowerAttackAnimKeepDiscovery d{};
    if (!Discover(game, &d, why)) return false;
    uint8_t* const mem = AllocNearGame(64);
    if (!mem) { *why = "no memory near the game's code"; return false; }
    // stub: mov rcx, rax (the entry); jmp [rip] -> CountHook
    const uint8_t stub[9] = {0x48, 0x89, 0xC1, 0xFF, 0x25, 0, 0, 0, 0};
    const void* const hook = reinterpret_cast<void*>(&CountHook);
    memcpy(mem, stub, sizeof stub);
    memcpy(mem + sizeof stub, &hook, 8);
    DWORD old;
    if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) { *why = "could not make the stub executable"; return false; }
    FlushInstructionCache(GetCurrentProcess(), mem, 64);
    uint8_t b[8] = {0xE8, 0, 0, 0, 0, 0x8B, 0xC8, 0x90};  // call stub; mov ecx, eax; nop
    const int64_t rel = mem - (d.site + 5);
    if (rel != static_cast<int32_t>(rel)) { *why = "out of reach"; return false; }
    const int32_t r = static_cast<int32_t>(rel);
    memcpy(b + 1, &r, 4);
    if (!WriteCode(d.site, std::span<const uint8_t>(b, 8))) { *why = "write failed"; return false; }
    g_site = d.site;
    g_installed = true;
    return true;
}

}  // namespace

bool PowerAttackAnimKeepDiscover(const GameModule& game, PowerAttackAnimKeepDiscovery* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool PowerAttackAnimKeepTestInstall()
{
    const char* why = "";
    return g_installed || Install(Game(), &why);
}

uint32_t PowerAttackAnimKeepTestCount(const uint8_t* entry) { return CountHook(entry); }

uint64_t PowerAttackAnimKeepKept() { return g_kept.load(); }

void SetPowerAttackAnimKeep(int mode) { g_on.store(mode ? 1 : 0, std::memory_order_relaxed); }

// Start-up only (version.dll route): the patched instructions must not be running on another thread.
bool InstallPowerAttackAnimKeep(bool beforeGameCode, int mode)
{
    if (g_installed) return true;
    if (!mode) {
        Log("[poweranim] PowerAttackAnimKeep off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[poweranim] PowerAttackAnimKeep: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const char* why = "";
    if (!Install(Game(), &why)) {
        Log("[poweranim] PowerAttackAnimKeep: %s, not installed (nothing changed)", why);
        return false;
    }
    SetPowerAttackAnimKeep(mode);
    Log("[poweranim] PowerAttackAnimKeep installed (animation release exe+0x%X: a power-attack file whose last holder lets go "
        "stays in the animation map at count 0): PowerAttackAnimKeep=%d",
        static_cast<unsigned>(g_site - Game().base), g_on.load());
    return true;
}

void LogPowerAttackAnimKeepStats()
{
    if (!g_installed) return;
    static uint64_t last = 0;
    const uint64_t k = g_kept.load();
    if (k != last) {
        Log("[poweranim] power-attack animation files left loaded when their last holder let go: %llu (total %llu)",
            static_cast<unsigned long long>(k - last), static_cast<unsigned long long>(k));
        last = k;
    }
}


}  // namespace opf
