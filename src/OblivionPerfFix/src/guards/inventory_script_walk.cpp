// InventoryScriptWalkFix (S-080, GitHub issue #53; worker fix-s080 2026-11-24; game bug, default off until a release):
// a crash in the old engine's per-frame walk over an inventory's scripted items (game bug: also without OblivionPerfFix).
//
// The walk (build 19115871 exe+0x6647F90; called by a reference's script update for containers, NPCs and creatures, by
// Activate and by an item transfer) goes over the reference's inventory list (ExtraContainerChanges data: a list of
// 16-byte nodes {entry, next}; an entry = {list of its extra-data lists, count, item form}) and, for every extra-data
// list of an entry that carries a running item script, runs that script (exe+0x6630EE0). Across each script run it keeps
// three pointers: the outer node (R12), the entry ([rsp+0xA8]) and the inner node (R15). Its only guard against what the
// script changed is a recount: when the NUMBER of entries changed it starts again from the head, when the number of the
// entry's extra-data lists changed it goes on with the next outer node; otherwise it follows the nodes it kept. A script
// that adds one item and removes another keeps the count, and the list's removal can free exactly the node the walk
// keeps (removing the head entry copies node 2 into the head and frees node 2, exe+0x6553F80): the walk then follows a
// freed node and reads the allocator's free-block header as an entry pointer -> access violation at exe+0x664808F
// (issue #53's registers and memory: the node's first 16 bytes were an FMallocBinned2 free-block header).
//
// What the stock walk does after a script run (exe+0x6648157..): EBX = recount of the outer list. EBX != ESI (the count
// it started with) -> its restart: R12 = the head, ESI = a fresh count, then R12 = head->next (the head entry itself is
// skipped). Otherwise it recounts the kept entry's extra-data lists (reads the entry) into EDI: EDI != R13D -> next outer
// node through R12; equal -> next inner node through R15, later the next outer node through R12. So it reads freed memory
// only when the outer count is unchanged and the kept entry, the kept outer node, or (inner count unchanged) the kept
// inner node is gone.
//
// The fix: the script call is redirected to a small stub that runs the script exactly as before and then makes the same
// comparisons the walk is about to make (the stub passes ESI and R13D). Only when the walk would follow a pointer that
// is no longer in the list (the cases above) does the stub set ESI to an impossible count, so the walk takes its own
// "count changed" branch, as the stock game does whenever a script run changes the number of entries. In every other
// case (nothing lost; the walk diverts itself because a count changed; or a lost pointer it would not read) nothing is
// written and the walk goes on bit for bit as in the stock game. Cost: one walk of the outer list and of the kept entry's
// extra-data lists per item-script run (the stock walk recounts both after every run anyway).
// No cap on restarts: one is forced only when a script run freed a pointer the walk keeps; a fallback after a cap could
// only be the stock read of freed memory, and the stock count-change restart has no cap either.
#include "common.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// The walk's loop head, build 19115871 exe+0x664807A: R12 = outer node, the entry to [rsp+0A8h], R15 = its first inner node
//   49 8B 04 24                  mov rax,[r12]
//   48 89 84 24 A8 00 00 00      mov [rsp+0A8h],rax
//   48 85 C0 0F 84 ????????      test rax,rax; jz
//   4C 8B 38                     mov r15,[rax]
//   45 33 ED                     xor r13d,r13d
constexpr const char* kHead = "49 8B 04 24 48 89 84 24 A8 00 00 00 48 85 C0 0F 84 ?? ?? ?? ?? 4C 8B 38 45 33 ED";
// The script call and the recount after it, exe+0x6648138:
//   4C 8B 8C 24 98 00 00 00      mov r9,[rsp+98h]        the reference
//   4C 8B C0 48 8B 15 ????????   mov r8,rax; mov rdx,[..]
//   48 8B CD C6 44 24 20 00      mov rcx,rbp; mov byte [rsp+20h],0
//   E8 [script run]              call exe+0x6630EE0      <- to the stub
//   8B AC 24 A0 00 00 00 84 C0   mov ebp,[rsp+0A0h]; test al,al
//   4C 8B B4 24 90 00 00 00      mov r14,[rsp+90h]       the list field
//   B8 01 00 00 00 40 0F B6 ED 0F 45 E8 33 DB 89 AC 24 A0 00 00 00
//   49 8B 0E 48 85 C9 74 ??      mov rcx,[r14] ...       recount of the outer list into EBX
//   66 66 66 0F 1F 84 00 00 00 00 00 48 83 39 00 8D 43 01 48 8B 49 08 0F 44 C3 8B D8 48 85 C9 75 EB
//   3B F3 75 ??                  cmp esi,ebx; jne        count changed -> (restart)
//   48 8B 84 24 A8 00 00 00      mov rax,[rsp+0A8h]      the kept entry, read without a check
constexpr const char* kCall =
    "4C 8B 8C 24 98 00 00 00 4C 8B C0 48 8B 15 ?? ?? ?? ?? 48 8B CD C6 44 24 20 00 E8 ?? ?? ?? ?? "
    "8B AC 24 A0 00 00 00 84 C0 4C 8B B4 24 90 00 00 00 B8 01 00 00 00 40 0F B6 ED 0F 45 E8 33 DB 89 AC 24 A0 00 00 00 "
    "49 8B 0E 48 85 C9 74 ?? 66 66 66 0F 1F 84 00 00 00 00 00 48 83 39 00 8D 43 01 48 8B 49 08 0F 44 C3 8B D8 48 85 C9 75 EB "
    "3B F3 75 ?? 48 8B 84 24 A8 00 00 00";
constexpr size_t kCallSite = 26;          // the E8 in kCall
constexpr size_t kHeadToCall = 0xBE;      // kCall - kHead in build 19115871 (checked: the same function)
// The path after the call (checked jump by jump, so ESI = -1 is known to reach the restart):
//  kCall's last "3B F3 75 xx" (exe+0x66481A5, count changed) -> kUnlock (exe+0x66481D5), which ends in "EB xx" ->
//  kCompare (exe+0x6648209): cmp edi,r13d; jne kRestart; cmp esi,ebx; jne kRestart+4; then "4D 8B 7F 08" mov r15,[r15+8]
//  kRestart (exe+0x6648227): cmp esi,ebx; je kNext; mov r12,[r14]; xor esi,esi (the restart); kNext: mov r12,[r12+8]
constexpr const char* kUnlock =
    "48 8B 0D ?? ?? ?? ?? 33 D2 48 81 C1 88 00 00 00 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? B2 01 48 81 C1 88 00 00 00 E8 ?? ?? ?? ?? EB";
constexpr size_t kUnlockJmp = 42;  // the EB
constexpr const char* kCompare = "41 3B FD 75 ?? 3B F3 75 ?? 4D 8B 7F 08";
constexpr const char* kRestart = "3B F3 74 ?? 4D 8B 26 33 F6 49 8B CC 4D 85 E4";
constexpr const char* kNext = "4D 8B 64 24 08";
// The item-script run, exe+0x6630EE0 (five arguments, the fifth a byte: movzx ebx,byte [rsp+0A0h])
constexpr const char* kScriptRun =
    "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 60 0F B6 9C 24 A0 00 00 00 49 8B F1";
constexpr size_t kWalkList = 0x90, kWalkEntry = 0xA8;  // the walk's stack slots (its rsp at the call)

// The stub (at the call site's target): runs the hook with the walk's kept pointers and counts, then sets ESI to an
// impossible count when the hook asks for the restart.
//   0  48 83 EC 68                sub rsp,68h
//   4  0F B6 84 24 90 00 00 00    movzx eax,byte [rsp+90h]   the fifth argument (the walk's [rsp+20h])
//  12  48 89 44 24 20             mov [rsp+20h],rax
//  17  4C 89 64 24 28             mov [rsp+28h],r12          outer node
//  22  4C 89 7C 24 30             mov [rsp+30h],r15          inner node
//  27  48 8D 44 24 70             lea rax,[rsp+70h]          the walk's rsp
//  32  48 89 44 24 38             mov [rsp+38h],rax
//  37  C6 44 24 58 00             mov byte [rsp+58h],0       restart flag
//  42  48 8D 44 24 58             lea rax,[rsp+58h]
//  47  48 89 44 24 40             mov [rsp+40h],rax
//  52  89 74 24 48                mov [rsp+48h],esi          the outer count the walk compares with
//  56  44 89 6C 24 50             mov [rsp+50h],r13d         the inner count the walk compares with
//  61  FF 15 15 00 00 00          call [rip+15h]             -> ScriptRunHook (qword at 88)
//  67  80 7C 24 58 00             cmp byte [rsp+58h],0
//  72  74 05                      je 79
//  74  BE FF FF FF FF             mov esi,-1                 the walk's count: "changed" -> its restart
//  79  48 83 C4 68                add rsp,68h
//  83  C3                         ret                        (rax = the script run's result, untouched)
constexpr uint8_t kStub[84] = {
    0x48, 0x83, 0xEC, 0x68, 0x0F, 0xB6, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x20, 0x4C, 0x89,
    0x64, 0x24, 0x28, 0x4C, 0x89, 0x7C, 0x24, 0x30, 0x48, 0x8D, 0x44, 0x24, 0x70, 0x48, 0x89, 0x44, 0x24, 0x38, 0xC6,
    0x44, 0x24, 0x58, 0x00, 0x48, 0x8D, 0x44, 0x24, 0x58, 0x48, 0x89, 0x44, 0x24, 0x40, 0x89, 0x74, 0x24, 0x48, 0x44,
    0x89, 0x6C, 0x24, 0x50, 0xFF, 0x15, 0x15, 0x00, 0x00, 0x00, 0x80, 0x7C, 0x24, 0x58, 0x00, 0x74, 0x05, 0xBE, 0xFF,
    0xFF, 0xFF, 0xFF, 0x48, 0x83, 0xC4, 0x68, 0xC3};
constexpr size_t kStubHook = 88, kStubUnwind = 96, kStubTable = 104, kStubSize = 128;
// UNWIND_INFO: version 1, prolog 4 bytes, one code: at 4 UWOP_ALLOC_SMALL (0x68 - 8) / 8 = 12
constexpr uint8_t kUnwind[8] = {0x01, 0x04, 0x01, 0x00, 0x04, 0xC2, 0x00, 0x00};

constexpr size_t kMaxNodes = 1u << 20;  // a longer list is taken as damaged: left to the stock walk (it would hang there)

struct Node {
    void* item;
    Node* next;
};

using ScriptRunFn = uint64_t (*)(void* script, void* a2, void* a3, void* ref, uint64_t a5);

uint8_t* g_site = nullptr;   // the walk's script call (now to the stub)
uint8_t* g_stub = nullptr;
ScriptRunFn g_scriptRun = nullptr;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_runs{0}, g_restarts{0}, g_tooLong{0};
uint64_t g_restartsLogged = 0;

}  // namespace

// Whether the walk, going on as the stock code does, would follow a pointer the script run took out of the list
// (0: it would not: nothing lost, or a count changed so it diverts itself, or the lost pointer is one it does not read).
// outerCount / innerCount = the walk's ESI / R13D. Pure; the self-test runs it on fake lists.
int InventoryScriptWalkLost(void* const* listField, const void* outer, const void* entry, const void* inner, uint32_t outerCount,
                            uint32_t innerCount)
{
    size_t n = 0;
    uint32_t count = 0;
    bool outerLinked = false, entryLinked = false;
    for (const Node* p = static_cast<const Node*>(*listField); p; p = p->next) {
        if (++n > kMaxNodes) return kInvWalkTooLong;
        if (p->item) ++count;  // the walk counts the nodes holding an entry
        if (p == outer) outerLinked = true;
        if (p->item == entry) entryLinked = true;
    }
    if (count != outerCount) return 0;          // EBX != ESI: the walk's own restart, nothing kept is read
    if (!entryLinked) return kInvWalkEntryGone;  // it recounts the kept entry's lists next
    if (!outerLinked) return kInvWalkOuterGone;  // it goes on through R12 in both of its branches
    n = 0;
    count = 0;
    bool innerLinked = false;
    for (const Node* q = *static_cast<const Node* const*>(entry); q; q = q->next) {
        if (++n > kMaxNodes) return kInvWalkTooLong;
        if (q->item) ++count;
        if (q == inner) innerLinked = true;
    }
    if (count == innerCount && !innerLinked) return kInvWalkInnerGone;  // EDI == R13D: it goes on through R15
    return 0;                                                            // EDI != R13D: next outer node through R12 (linked)
}

namespace {

uint64_t ScriptRunHook(void* script, void* a2, void* a3, void* ref, uint64_t a5, void* outer, void* inner, uint8_t* walkRsp,
                       uint8_t* restart, uint32_t outerCount, uint32_t innerCount)
{
    const uint64_t result = g_scriptRun(script, a2, a3, ref, a5);
    const bool on = g_on.load(std::memory_order_relaxed);
    if (!on) return result;
    g_runs.fetch_add(1, std::memory_order_relaxed);
    void* const* listField = *reinterpret_cast<void* const* const*>(walkRsp + kWalkList);
    void* const entry = *reinterpret_cast<void* const*>(walkRsp + kWalkEntry);
    const int lost = InventoryScriptWalkLost(listField, outer, entry, inner, outerCount, innerCount);
    if (lost == kInvWalkTooLong) g_tooLong.fetch_add(1, std::memory_order_relaxed);
    const bool redo = on && lost != 0 && lost != kInvWalkTooLong;
    if (redo) {
        *restart = 1;
        g_restarts.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

bool Fail(const char* why)
{
    Log("[invwalk] %s: InventoryScriptWalkFix not installed", why);
    return false;
}

}  // namespace

bool InstallInventoryScriptWalkFix()
{
    if (g_stub) {
        Log("[invwalk] already installed");
        return true;
    }
    const GameModule& game = Game();
    const auto heads = FindAll(game.text, ParsePattern(kHead));
    const auto calls = FindAll(game.text, ParsePattern(kCall));
    if (heads.size() != 1 || calls.size() != 1) {
        Log("[invwalk] the inventory item-script walk not found (%zu / %zu matches): InventoryScriptWalkFix not installed",
            heads.size(), calls.size());
        return false;
    }
    if (calls[0] - heads[0] != static_cast<ptrdiff_t>(kHeadToCall)) return Fail("the walk's loop head and script call are not where expected");
    // The path after the call, jump by jump (rel8 targets): count changed -> unlock block -> compare block -> restart.
    const Pattern callPat = ParsePattern(kCall);
    const uint8_t* const changed = calls[0] + callPat.size() - 12;  // "3B F3 75 xx" before the entry reload
    auto rel8 = [&](const uint8_t* jcc) { return jcc + 2 + static_cast<int8_t>(jcc[1]); };
    auto at = [&](const uint8_t* p, const char* pat) {
        const Pattern pp = ParsePattern(pat);
        return game.text.Contains(p) && game.text.Contains(p + pp.size()) && MatchesAt(p, pp);
    };
    const uint8_t* const unlock = rel8(changed + 2);
    const uint8_t* const compare = at(unlock, kUnlock) ? rel8(unlock + kUnlockJmp) : nullptr;
    const uint8_t* const restartAt = compare && at(compare, kCompare) ? rel8(compare + 3) : nullptr;
    if (!restartAt || rel8(compare + 7) != restartAt + 4 || !at(restartAt, kRestart) || !at(rel8(restartAt + 2), kNext))
        return Fail("the walk's path from the script call to its restart (count changed) is not as expected");
    uint8_t* const site = calls[0] + kCallSite;
    uint8_t* const run = RipTarget(site, 1, 5);
    if (!game.text.Contains(run) || !MatchesAtCallable(run, ParsePattern(kScriptRun), "invwalk"))
        return Fail("the walk's call does not reach the item-script run");
    uint8_t* const stub = AllocNearGame(kStubSize);
    if (!stub) return Fail("no memory near the game's code");
    memcpy(stub, kStub, sizeof kStub);
    void* const hook = reinterpret_cast<void*>(&ScriptRunHook);
    memcpy(stub + kStubHook, &hook, sizeof hook);
    memcpy(stub + kStubUnwind, kUnwind, sizeof kUnwind);
    auto* const table = reinterpret_cast<RUNTIME_FUNCTION*>(stub + kStubTable);
    table->BeginAddress = 0;
    table->EndAddress = sizeof kStub;
    table->UnwindData = static_cast<DWORD>(kStubUnwind);
    DWORD old;
    if (!VirtualProtect(stub, kStubSize, PAGE_EXECUTE_READ, &old)) {
        FreeOwnCode(stub);
        return Fail("stub protection");
    }
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);
    // Stacks through the stub walk (crash dumps of a crash inside an item script).
    if (!RtlAddFunctionTable(table, 1, reinterpret_cast<DWORD64>(stub))) Log("[invwalk] RtlAddFunctionTable failed (stacks through the stub will not walk)");
    const int64_t rel = stub - (site + 5);
    if (rel != static_cast<int32_t>(rel)) {
        RtlDeleteFunctionTable(table);
        FreeOwnCode(stub);
        return Fail("stub out of reach");
    }
    g_scriptRun = reinterpret_cast<ScriptRunFn>(run);
    uint8_t patch[5]{0xE8};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 1, &rel32, 4);
    if (!WriteCode(site, patch)) {
        RtlDeleteFunctionTable(table);
        FreeOwnCode(stub);
        return Fail("could not write the walk's script call");
    }
    g_site = site;
    g_stub = stub;
    Log("[invwalk] inventory item-script walk exe+0x%llX: script call exe+0x%llX (item-script run exe+0x%llX) to the stub; a kept "
        "list node or entry that a script run freed makes the walk restart from the head (its own count-changed path): "
        "installed",
        static_cast<unsigned long long>(heads[0] - game.base - 0xEA), static_cast<unsigned long long>(site - game.base),
        static_cast<unsigned long long>(run - game.base));
    return true;
}

void SetInventoryScriptWalkFix(bool on) { g_on = on && g_stub; }

void LogInventoryScriptWalkStats()
{
    const uint64_t restarts = g_restarts.load(std::memory_order_relaxed);
    if (!g_stub || restarts == g_restartsLogged) return;
    Log("[invwalk] item-script runs checked %llu; a kept inventory node or entry freed by the script: %llu (walk restarted "
        "from the head instead of reading freed memory); damaged lists left alone %llu",
        static_cast<unsigned long long>(g_runs.load()), static_cast<unsigned long long>(restarts),
        static_cast<unsigned long long>(g_tooLong.load()));
    g_restartsLogged = restarts;
}

bool InventoryScriptWalkInfo(InventoryScriptWalkInfoOut* out)
{
    if (!g_stub) return false;
    out->site = g_site;
    out->stub = g_stub;
    out->scriptRun = reinterpret_cast<void*>(g_scriptRun);
    out->hook = reinterpret_cast<void*>(&ScriptRunHook);
    out->runs = g_runs.load();
    out->restarts = g_restarts.load();
    return true;
}

void InventoryScriptWalkTestSetup(void* fakeScriptRun)
{
    if (fakeScriptRun) g_scriptRun = reinterpret_cast<ScriptRunFn>(fakeScriptRun);
    g_runs = 0;
    g_restarts = 0;
    g_tooLong = 0;
}

}  // namespace opf
