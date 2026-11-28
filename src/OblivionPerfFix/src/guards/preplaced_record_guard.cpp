// PrePlacedRecordGuard (worker crash072b 2026-11-21, S-072; record recreation: worker recguard 2026-11-23;
// [Fixes] PrePlacedRecordGuard, 0 off, 1 on (recreate), 2 the old exit only; start-up only).
//
// The crash (Nexus report on 1.6.0-beta, "when traversing"; findings crash072 / crash072b): Altar's adoption of a cell's
// pre-placed actor (exe+0x472EFB0, the level flow's "Pair_Pre_Placed_Actors" command) looks up the cell's level by name in
// the pre-placed actor subsystem (+0xC8: level name -> VAssociatedAltarCellToPrePlacedActors) and then the reference's GUID
// in that level's record map (+0x28: GUID -> VInitializedPrePlacedActor). When the LEVEL is missing the adoption does
// nothing and returns (the reference keeps no UE actor until its cell is attached again). When the level is there but the
// GUID is not, it takes the "record without an actor" branch: it spawns the actor (exe+0x47364C0) and stores it into the
// record it did not find (`mov rax,[r15]` with r15 = 0 at +0x2E9): EXCEPTION_ACCESS_VIOLATION reading 0x0. A record that is
// found but whose value is null (exe+0x472F112) ends the same way (writing 0x28).
//
// What fills and clears the records (Ghidra, crash072b / recguard): the old engine's pairing of the reference
// (exe+0x6586690) first sends "Send_Oblivion_State_Pre_Placed_Actors" (game-thread handler exe+0x4732370: finds or creates
// the level and the record; a new record is a fresh object whose only set field is the "claimed by the old engine" byte
// +0x40 = 1) into the main queue and then the adoption, with the same cell name and GUID, into the level's flow session; a
// UE pre-placed actor registers its own record when it begins play (exe+0x4824540: actor +0x28, loaded +0x41). A record
// goes away when its UE actor ends play while not claimed (AltarActor_EndPlay exe+0x4829C10; the level goes too when its
// map is empty), and through "Remove_PrePlaced_From_TMap" (exe+0x47314B0) when it has no actor and is not loaded. The game
// assumes, without checking, that the record exists whenever a not-cancelled adoption runs. It does not when, for example,
// two references share one GUID in one cell (30 such pairs in the game's own plugins): one reference's unpair removes the
// shared record while the other's adoption is still waiting in the flow.
//
// The fix (mode 1): the adoption's "GUID not found" branch and its "record without value" branch go through a small stub
// to our function, which creates the record exactly as the game does: it copies the adoption's cell-name string with the
// game's own string copy and calls the game's own Send_State handler with the adoption's GUID, that copy and "claimed"
// (the same call the pairing posts for this reference), then looks the GUID up again in the same level map with the
// adoption's own hash and look-up. With the record there the stub continues the game's adoption at its record checks
// (exe+0x472F10C): the new record has no actor and is not loaded, so the game spawns and pairs the actor as it does after
// every Send_State, and every later step (EndPlay, unpair, Remove) sees a record the game made. If the adoption's pairing
// entry was cancelled meanwhile (entry +0x12, which the flow command checks before the call), or the record still is not
// there, the stub takes the adoption's own exit instead, the one the game takes when the level is missing (r15 / r13
// restored from the adoption's own saves, the cell-name string argument freed, return): the reference then has no UE actor
// until its cell is attached again. Mode 2 always takes that exit (the earlier guard, also for a record found empty). Every adoption whose record exists
// runs exactly as before (the stub is reached only on the two crashing branches).
//
// Found by one byte pattern over the level check, the GUID look-up and the record checks (unique); the jz targets (the
// exit, the spawn branch's `xor r15d,r15d` and the label after it), the exit's register restores, the Send_State handler
// (its own pattern; three of its calls go to the same functions as the adoption's start), the flow command's string copy
// and cancel test are checked. Without the handler or the copy only mode 2 is installed; any other mismatch: not
// installed, one log line.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// Inside the adoption (build 19115871: exe+0x472F07E): test rbx,rbx; jz EXIT (level missing); mov rax,[rbx]; test rax,rax;
// jz EXIT; lea rbx,[rax+28h] (the record map); mov [rsp+0C0h],r13; mov rax,[rbp+3Fh]; mov [rsp+0B8h],r15; (the GUID copy
// into [rbp-51h]); call Hash(guid, 10h); lea r8,[rbp-51h]; mov edx,eax; mov rcx,rbx; call FindId; cmp eax,-1;
// jz SPAWN (+0x6E, GUID not found); movsxd r15,eax; shl r15,5; add r15,[rbx]; jz SPAWN; add r15,10h; jz SPAWN+3;
// RECHECK (+0x8E): mov rax,[r15]; test rax,rax; jz SPAWN+3 (+0x94, value null); cmp byte [rax+41h],0; jz SPAWN+3;
// mov rbx,[rax+28h]; test rbx,rbx; jz SPAWN+3
constexpr char kLookup[] =
    "48 85 DB 0F 84 ?? ?? ?? ?? 48 8B 03 48 85 C0 0F 84 ?? ?? ?? ?? 48 8D 58 28 4C 89 AC 24 C0 00 00 00 48 8B 45 3F "
    "4C 89 BC 24 B8 00 00 00 8B 48 08 8B 50 04 44 8B 68 0C 8B 00 89 4D C7 89 55 C3 89 55 B3 BA 10 00 00 00 89 4D B7 "
    "48 8D 4D AF 89 45 BF 89 45 AF 44 89 6D BB E8 ?? ?? ?? ?? 4C 8D 45 AF 8B D0 48 8B CB E8 ?? ?? ?? ?? 83 F8 FF "
    "0F 84 ?? ?? ?? ?? 4C 63 F8 49 C1 E7 05 4C 03 3B 0F 84 ?? ?? ?? ?? 49 83 C7 10 0F 84 ?? ?? ?? ?? "
    "49 8B 07 48 85 C0 0F 84 ?? ?? ?? ?? 80 78 41 00 0F 84 ?? ?? ?? ?? 48 8B 58 28 48 85 DB 0F 84";
constexpr size_t kLevelJz = 0x03;     // jz EXIT (level missing)
constexpr size_t kHashCall = 0x58;    // call Hash(&guid, 16)
constexpr size_t kFindCall = 0x66;    // call FindId(map, hash, &guid)
constexpr size_t kGuidJz = 0x6E;      // jz SPAWN (GUID not found): retargeted
constexpr size_t kRecheck = 0x8E;     // mov rax,[r15]: where the stub continues with r15 = the new record's slot
constexpr size_t kNullJz = 0x94;      // jz SPAWN+3 (record without value): retargeted
constexpr size_t kAdoptFromLookup = 0xCE;  // the adoption's entry, before the pattern
// The exit's register restores (EXIT - 0x10): mov r15,[rsp+0B8h]; mov r13,[rsp+0C0h]; then at EXIT mov rdx,[r14+18h]
constexpr char kRestore[] = "4C 8B BC 24 B8 00 00 00 4C 8B AC 24 C0 00 00 00 49 8B 56 18";
constexpr size_t kRestoreBack = 0x10;
constexpr char kSpawn[] = "45 33 FF";  // xor r15d,r15d (the spawn branch's start; SPAWN+3 = the spawn with r15 kept)
// The adoption's entry after its first 6 bytes (FlowOverlapsOnce may have patched those): lea rbp,[rax-17h]; sub rsp,0E8h
constexpr char kAdoptTail[] = "48 8D 68 E9 48 81 EC E8 00 00 00";
// The adoption's start: its world (+0x41), game instance (+0x4E) and Altar data (+0x56) calls
constexpr size_t kAdoptCalls[3] = {0x41, 0x4E, 0x56};

// Send_State handler (build 19115871: exe+0x4732370; rcx unused, rdx GUID*, r8 std::string by value (destroyed by it),
// r9b claimed): prologue, the same three calls as the adoption's start (+0x3F, +0x54, +0x5C), the GUID copy, the string's
// conversion and lea r14,[rsi+0C8h] (the level map).
constexpr char kSendState[] =
    "48 89 5C 24 08 48 89 74 24 10 55 57 41 54 41 56 41 57 48 8D 6C 24 F0 48 81 EC 10 01 00 00 48 8B 05 ?? ?? ?? ?? "
    "48 33 C4 48 89 45 00 48 8B 0D ?? ?? ?? ?? 48 8B DA 33 D2 45 0F B6 F9 49 8B F8 E8 ?? ?? ?? ?? 45 33 E4 48 85 C0 "
    "75 05 41 8B F4 EB 14 48 8B C8 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 70 78 48 83 7F 18 10 48 8B D7 8B 03 "
    "89 44 24 28 8B 43 04 89 44 24 2C 8B 43 08 89 44 24 30 8B 43 0C 89 44 24 34 72 03 48 8B 17 48 8D 4C 24 38 E8 "
    "?? ?? ?? ?? 8B 44 24 40 4C 8D B6 C8 00 00 00";
constexpr size_t kSendCalls[3] = {0x3F, 0x54, 0x5C};
constexpr size_t kSendReadFrom = 0x3F;  // the lowest offset read (call targets) for a hooked entry
// The flow command that calls the adoption (build 19115871: exe+0x657CBB0, inside exe+0x657CBA0): the cancel test
// (call Cancelled(entry): movzx eax,byte [rcx+12h]; ret), then the copy of the string argument (std::string copy ctor).
constexpr char kFlowCmd[] =
    "48 8B 49 10 48 85 C9 74 0D E8 ?? ?? ?? ?? 84 C0 74 04 B0 01 EB 02 32 C0 84 C0 0F 85 ?? ?? ?? ?? 0F 10 43 68 "
    "48 8D 93 88 00 00 00 48 8D 8C 24 80 00 00 00 0F 29 44 24 70 E8";
constexpr size_t kCancelCall = 0x09;
constexpr size_t kCopyCall = 0x38;
constexpr char kCancelFn[] = "0F B6 41 ?? C3";
constexpr char kStrCopy[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 C0 0F 57 C0 0F 11 01 48 89 41 10 4C 8B F2 "
    "48 89 41 18 48 8B D9 48 83 7A 18 10";

// MSVC std::string (x64): 16 bytes inline buffer / heap pointer, size, capacity
struct MsvcString {
    uint8_t buf[16];
    uint64_t size;
    uint64_t cap;
};
using SendStateFn = void (*)(void* unused, const uint32_t* guid, MsvcString* byValue, uint64_t claimed);
using StrCopyFn = MsvcString* (*)(MsvcString* dst, const MsvcString* src);
using HashFn = uint32_t (*)(const void* data, uint32_t size);
using FindIdFn = int32_t (*)(void* set, uint32_t hash, const void* key);

SendStateFn g_sendState = nullptr;
StrCopyFn g_strCopy = nullptr;
HashFn g_hash = nullptr;
FindIdFn g_findId = nullptr;
uint32_t g_cancelOff = 0x12;
int g_mode = 0;

std::atomic<uint64_t> g_recreated{0};    // GUID missing, record made, adoption continued
std::atomic<uint64_t> g_recreatedNull{0};  // record without value, record made, adoption continued
std::atomic<uint64_t> g_cancelled{0};    // the pairing entry was cancelled: exit
std::atomic<uint64_t> g_failed{0};       // no record after Send_State: exit
std::atomic<uint64_t> g_exitOnly{0};     // mode 2: exit
std::atomic<uint64_t> g_events{0};
alignas(16) uint8_t g_lastGuid[16] = {};
uint64_t g_logged = 0;
bool g_installed = false;
PrePlacedRecordGuardInfo g_info{};

uint8_t* JzTarget(uint8_t* jz) { return RipTarget(jz, 2, 6); }
uint8_t* CallTarget(uint8_t* call) { return call[0] == 0xE8 ? RipTarget(call, 1, 5) : nullptr; }

// The slot (pair + 0x10) of `guid` in a level's record map, or nullptr.
void** FindSlot(void* map, const uint32_t* guid)
{
    const int32_t i = g_findId(map, g_hash(guid, 16), guid);
    if (i < 0) return nullptr;
    uint8_t* const data = *reinterpret_cast<uint8_t**>(map);
    if (!data) return nullptr;
    return reinterpret_cast<void**>(data + static_cast<int64_t>(i) * 0x20 + 0x10);
}


// Called from the stubs (game thread, inside the adoption): the new record's slot to continue with, or nullptr = exit.
void** Recreate(const uint8_t* entry, const MsvcString* name, const uint8_t* guidAt, void* map, bool nullValue)
{
    uint32_t guid[4];
    memcpy(guid, guidAt, 16);
    memcpy(g_lastGuid, guid, 16);
    const uint64_t ev = g_events.fetch_add(1, std::memory_order_relaxed);
    CrashEvent(kEvKey, guid[0], guid[1], nullValue, "preplaced");  // CrashReport's ring: an adoption without a record
    const char* const what = nullValue ? "found without value" : "missing";
    if (g_mode != 1 || !g_sendState || !g_strCopy) {
        g_exitOnly.fetch_add(1, std::memory_order_relaxed);
        if (ev < 16)
            Log("[preplaced] adoption of GUID %08X-%08X-%08X-%08X: record %s, left without an actor (old exit)", guid[0],
                guid[1], guid[2], guid[3], what);
        return nullptr;
    }
    if (!entry || entry[g_cancelOff] != 0) {
        g_cancelled.fetch_add(1, std::memory_order_relaxed);
        if (ev < 16)
            Log("[preplaced] adoption of GUID %08X-%08X-%08X-%08X: record %s and the pairing cancelled, left without an "
                "actor",
                guid[0], guid[1], guid[2], guid[3], what);
        return nullptr;
    }
    MsvcString copy;
    g_strCopy(&copy, name);
    g_sendState(nullptr, guid, &copy, 1);  // destroys the copy
    void** const slot = FindSlot(map, guid);
    if (!slot || !*slot) {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        if (ev < 16)
            Log("[preplaced] adoption of GUID %08X-%08X-%08X-%08X: record %s and not there after the game's Send_State, "
                "left without an actor",
                guid[0], guid[1], guid[2], guid[3], what);
        return nullptr;
    }
    (nullValue ? g_recreatedNull : g_recreated).fetch_add(1, std::memory_order_relaxed);
    if (ev < 16)
        Log("[preplaced] adoption of GUID %08X-%08X-%08X-%08X: record %s, made as the game's Send_State makes it; the "
            "adoption continues",
            guid[0], guid[1], guid[2], guid[3], what);
    return slot;
}

void** RecreateMissing(const uint8_t* entry, const MsvcString* name, const uint8_t* guidAt, void* map)
{
    return Recreate(entry, name, guidAt, map, false);
}
void** RecreateNull(const uint8_t* entry, const MsvcString* name, const uint8_t* guidAt, void* map)
{
    return Recreate(entry, name, guidAt, map, true);
}

bool Discover(const GameModule& game, PrePlacedRecordGuardInfo* d, const char** why)
{
    const auto hits = FindAll(game.text, ParsePattern(kLookup));
    d->matches = static_cast<int>(hits.size());
    if (hits.size() != 1) { *why = "adoption's record lookup not found (pattern count)"; return false; }
    uint8_t* const p = hits[0];
    uint8_t* const exitAt = JzTarget(p + kLevelJz);
    uint8_t* const site = p + kGuidJz;
    uint8_t* const nullSite = p + kNullJz;
    uint8_t* const spawn = JzTarget(site);
    uint8_t* const restore = exitAt - kRestoreBack;
    if (!game.text.Contains(restore) || !MatchesAt(restore, ParsePattern(kRestore))) {
        *why = "the adoption's exit is not the expected one";
        return false;
    }
    if (!game.text.Contains(spawn) || !MatchesAt(spawn, ParsePattern(kSpawn))) {
        *why = "the GUID branch does not lead to the expected spawn";
        return false;
    }
    if (p < game.base + kAdoptFromLookup || !MatchesAt(p - kAdoptFromLookup + 6, ParsePattern(kAdoptTail))) {
        *why = "the lookup is not inside the expected adoption";
        return false;
    }
    if (site[0] != 0x0F || site[1] != 0x84) { *why = "the GUID branch is already patched"; return false; }
    if (nullSite[0] != 0x0F || nullSite[1] != 0x84 || JzTarget(nullSite) != spawn + 3) {
        *why = "the record-without-value branch is not the expected one (or already patched)";
        return false;
    }
    d->adopt = p - kAdoptFromLookup;
    d->site = site;
    d->nullSite = nullSite;
    d->recheck = p + kRecheck;
    d->restore = restore;
    d->spawn = spawn;
    d->hash = CallTarget(p + kHashCall);
    d->findId = CallTarget(p + kFindCall);
    if (!d->hash || !d->findId || !game.text.Contains(d->hash) || !game.text.Contains(d->findId)) {
        *why = "the adoption's GUID hash / look-up calls are not the expected ones";
        return false;
    }
    // The record maker (optional: without it only the old exit is installed)
    const auto ss = FindAllCallable(game.text, ParsePattern(kSendState), "PrePlacedRecordGuard Send_State", kSendReadFrom);
    d->sendStateMatches = static_cast<int>(ss.size());
    if (ss.size() == 1) {
        bool same = true;
        for (int i = 0; i < 3; ++i) {
            uint8_t* const a = CallTarget(d->adopt + kAdoptCalls[i]);
            uint8_t* const b = CallTarget(ss[0] + kSendCalls[i]);
            if (!a || a != b) same = false;
        }
        if (same) d->sendState = ss[0];
    }
    const auto fc = FindAll(game.text, ParsePattern(kFlowCmd));
    d->flowCmdMatches = static_cast<int>(fc.size());
    uint8_t* copy = nullptr;
    uint8_t* cancel = nullptr;
    bool agree = !fc.empty();
    for (uint8_t* f : fc) {
        uint8_t* const c = CallTarget(f + kCopyCall);
        uint8_t* const k = CallTarget(f + kCancelCall);
        if (!copy) { copy = c; cancel = k; }
        if (c != copy || k != cancel) agree = false;
    }
    if (agree && copy && cancel && game.text.Contains(copy) && game.text.Contains(cancel) &&
        MatchesAtCallable(copy, ParsePattern(kStrCopy), "PrePlacedRecordGuard string copy") &&
        MatchesAt(cancel, ParsePattern(kCancelFn))) {
        d->strCopy = copy;
        d->cancelOff = cancel[3];
    }
    return true;
}

// The stub for one branch: calls fn(entry r12, name r14, &guid [rbp-51h], map rbx); a slot -> r15 = it, continue at the
// record checks; nullptr -> the adoption's exit restores. The adoption's rsp is 16-aligned here; every register the
// adoption still needs is callee-saved (rbx, rsi, rdi, rbp, r12-r15) or rewritten before use (rax, rcx, rdx, r8-r11, xmm0-5).
size_t BuildStub(uint8_t* code, void* fn, const uint8_t* recheck, const uint8_t* exitRestore)
{
    size_t n = 0;
    const auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t v : b) code[n++] = v; };
    const auto put64 = [&](const void* v) { memcpy(code + n, &v, 8); n += 8; };
    put({0x48, 0x83, 0xEC, 0x20});        // sub rsp,20h
    put({0x4C, 0x89, 0xE1});              // mov rcx,r12
    put({0x4C, 0x89, 0xF2});              // mov rdx,r14
    put({0x4C, 0x8D, 0x45, 0xAF});        // lea r8,[rbp-51h]
    put({0x49, 0x89, 0xD9});              // mov r9,rbx
    put({0x48, 0xB8});                    // mov rax,fn
    put64(fn);
    put({0xFF, 0xD0});                    // call rax
    put({0x48, 0x83, 0xC4, 0x20});        // add rsp,20h
    put({0x48, 0x85, 0xC0});              // test rax,rax
    put({0x74, 0x11});                    // jz exit
    put({0x49, 0x89, 0xC7});              // mov r15,rax
    put({0xFF, 0x25, 0, 0, 0, 0});        // jmp [rip] -> the record checks
    put64(recheck);
    put({0xFF, 0x25, 0, 0, 0, 0});        // exit: jmp [rip] -> the exit's restores
    put64(exitRestore);
    return n;
}

bool PatchJz(uint8_t* site, const uint8_t* to)
{
    const int64_t rel = to - (site + 6);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t jz[6] = {0x0F, 0x84, 0, 0, 0, 0};
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(jz + 2, &r32, 4);
    return WriteCode(site, std::span<const uint8_t>(jz, 6));
}


}  // namespace

bool PrePlacedRecordGuardDiscover(const GameModule& game, PrePlacedRecordGuardInfo* out)
{
    const char* why = "";
    return Discover(game, out, &why);
}

bool InstallPrePlacedRecordGuard(bool beforeGameCode, int mode, int labDrop)
{
    (void)labDrop;
    if (g_installed) return true;
    if (mode == 0) {
        Log("[preplaced] PrePlacedRecordGuard off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[preplaced] PrePlacedRecordGuard: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    PrePlacedRecordGuardInfo d{};
    const char* why = "";
    if (!Discover(Game(), &d, &why)) {
        Log("[preplaced] PrePlacedRecordGuard: %s (%d matches), not installed", why, d.matches);
        return false;
    }
    int m = mode == 2 ? 2 : 1;
    if (m == 1 && (!d.sendState || !d.strCopy)) {
        Log("[preplaced] PrePlacedRecordGuard: the game's record maker (%d matches) or its string copy (%d flow commands) "
            "not found: a missing record is not made; such an adoption leaves as for a missing level",
            d.sendStateMatches, d.flowCmdMatches);
        m = 2;
    }
    constexpr size_t kStub = 80;  // each stub: 69 bytes
    uint8_t* const stubs = AllocNearGame(2 * kStub);
    if (!stubs) {
        Log("[preplaced] PrePlacedRecordGuard: no memory near the game's code, not installed");
        return false;
    }
    uint8_t code[2 * kStub] = {};
    const size_t n1 = BuildStub(code, reinterpret_cast<void*>(&RecreateMissing), d.recheck, d.restore);
    const size_t n2 = BuildStub(code + kStub, reinterpret_cast<void*>(&RecreateNull), d.recheck, d.restore);
    if (n1 > kStub || n2 > kStub) return false;
    memcpy(stubs, code, sizeof code);
    DWORD old;
    if (!VirtualProtect(stubs, 2 * kStub, PAGE_EXECUTE_READ, &old)) {
        Log("[preplaced] PrePlacedRecordGuard: could not make the stubs executable, not installed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, 2 * kStub);
    g_sendState = reinterpret_cast<SendStateFn>(d.sendState);
    g_strCopy = reinterpret_cast<StrCopyFn>(d.strCopy);
    g_hash = reinterpret_cast<HashFn>(d.hash);
    g_findId = reinterpret_cast<FindIdFn>(d.findId);
    g_cancelOff = d.cancelOff ? d.cancelOff : 0x12;
    g_mode = m;
    if (!PatchJz(d.site, stubs)) {
        Log("[preplaced] PrePlacedRecordGuard: could not patch the adoption, not installed");
        return false;
    }
    if (!PatchJz(d.nullSite, stubs + kStub)) {
        // the first branch is guarded already; the second keeps the game's code
        Log("[preplaced] PrePlacedRecordGuard: could not patch the record-without-value branch (only the missing-record "
            "branch is guarded)");
        d.nullStub = nullptr;
    } else {
        d.nullStub = stubs + kStub;
    }
    d.stub = stubs;
    d.mode = m;
    g_info = d;
    g_installed = true;
    Log("[preplaced] PrePlacedRecordGuard installed, mode %d (adoption exe+0x%X: a GUID record missing at exe+0x%X or "
        "without value at exe+0x%X %s)",
        m, static_cast<unsigned>(d.adopt - Game().base), static_cast<unsigned>(d.site - Game().base),
        static_cast<unsigned>(d.nullSite - Game().base),
        m == 1 ? "is made by the game's own Send_State and the adoption goes on; a cancelled pairing leaves as a missing "
                 "level does"
               : "leaves as a missing level does");
    if (m == 1)
        Log("[preplaced] record maker exe+0x%X, string copy exe+0x%X, cancel byte +0x%X",
            static_cast<unsigned>(d.sendState - Game().base), static_cast<unsigned>(d.strCopy - Game().base), g_cancelOff);
    return true;
}

void LogPrePlacedRecordGuardStats()
{
    if (!g_installed) return;
    const uint64_t a = g_recreated.load(std::memory_order_relaxed), b = g_recreatedNull.load(std::memory_order_relaxed),
                   c = g_cancelled.load(std::memory_order_relaxed), f = g_failed.load(std::memory_order_relaxed),
                   x = g_exitOnly.load(std::memory_order_relaxed);
    const uint64_t n = a + b + c + f + x;
    if (n == g_logged) return;
    g_logged = n;
    uint32_t q[4];
    memcpy(q, g_lastGuid, 16);
    Log("[preplaced] pre-placed adoptions whose GUID record was missing or empty (the game would have crashed): %llu "
        "record made as the game makes it (%llu of them empty), %llu left without an actor until their cell is attached "
        "again (pairing cancelled %llu, no record after Send_State %llu, old exit %llu); last GUID %08X-%08X-%08X-%08X",
        static_cast<unsigned long long>(a + b), static_cast<unsigned long long>(b), static_cast<unsigned long long>(c + f + x),
        static_cast<unsigned long long>(c), static_cast<unsigned long long>(f), static_cast<unsigned long long>(x), q[0], q[1],
        q[2], q[3]);
}

bool PrePlacedRecordGuardState(PrePlacedRecordGuardInfo* out)
{
    if (!g_installed) return false;
    *out = g_info;
    out->recreated = g_recreated.load() + g_recreatedNull.load();
    out->exits = g_cancelled.load() + g_failed.load() + g_exitOnly.load();
    return true;
}

// self-test: the decision with fake game functions (nullptr restores the found ones). Returns the slot or nullptr.
void** PrePlacedRecordGuardTestRecreate(const PrePlacedRecordGuardFakes* f, const uint8_t* entry, const void* name,
                                        const uint8_t* guid, void* map, bool nullValue)
{
    const auto sv = g_sendState;
    const auto cp = g_strCopy;
    const auto h = g_hash;
    const auto fi = g_findId;
    const int mode = g_mode;
    if (f) {
        g_sendState = reinterpret_cast<SendStateFn>(f->sendState);
        g_strCopy = reinterpret_cast<StrCopyFn>(f->strCopy);
        g_hash = reinterpret_cast<HashFn>(f->hash);
        g_findId = reinterpret_cast<FindIdFn>(f->findId);
        g_mode = f->mode;
    }
    void** const r = Recreate(entry, static_cast<const MsvcString*>(name), guid, map, nullValue);
    g_sendState = sv;
    g_strCopy = cp;
    g_hash = h;
    g_findId = fi;
    g_mode = mode;
    return r;
}


}  // namespace opf
