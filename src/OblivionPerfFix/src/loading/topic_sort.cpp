// TopicSort (worker gapfix, 2026-11-16; S-058): the old engine's dialogue-topic list sort runs as a merge sort instead of a
// bubble sort, with the game's own comparison, so the list comes out in exactly the same order.
//
// What the game does (game behaviour, also without OblivionPerfFix; our reading of build 19115871): the function at
// exe+0x684EC60 sorts a linked list of dialogue topics (nodes {item, next}; the data handler's topic list when called
// with no list: at start-up from the data load exe+0x6625980, after every plugin file is read; also the player's known
// topics when one is added, "LOC_ME_TopicAdded", and two other lists) by the topic's name (the string at item +0x38, an
// empty string when there is none) with the C runtime's _stricmp. It counts the list's items (n), then makes n - 1 passes
// over the list from its head, each pass swapping the items of two neighbouring nodes when _stricmp(left, right) > 0 and
// stopping at the first node with no item. That is a bubble sort with no early exit: n - 1 passes of n - 1 comparisons
// even when the list is already in order. With the few thousand topics of the game's plugins that is millions of
// _stricmp calls (about 0.25-0.45 s in our start-ups), on the old engine's thread while the start-up screen is black and
// the game thread waits for it.
//
// What this does: the function's entry jumps to our sort. It reads the list the same way (the same default list, the same
// item count, the same stop at the first node with no item), sorts the items of that run of nodes with a bottom-up merge
// sort that calls the game's own _stricmp import on the game's own name strings, and writes the items back into the same
// nodes in the new order. Nodes, their links and every node after the run are untouched.
// Identical: the bubble sort only ever swaps two neighbours when the right one is strictly smaller (_stricmp > 0), so
// items that compare equal never pass each other, and it stops only when no neighbours are out of order (n - 1 passes are
// always enough for the run's at most n items). _stricmp compares the two strings' characters one by one after mapping
// each through the same lower-case table, so it orders the strings as a fixed key per string does: a total preorder.
// Under such an order there is exactly one sorted arrangement that keeps equal items in their old order, and a stable
// merge sort (on a tie the left item is taken first; the right one only when _stricmp(left, right) > 0, the game's own
// swap test) produces that same arrangement. The self-test runs the game's own function from the exe and ours on the
// same random and topic-shaped lists and compares every node.
// If the merge buffer cannot be allocated, the game's function runs (trampoline: its first three instructions, then a jump
// back).
// The proof needs one comparison order: a node with no name must compare as the same string on the left and on the right
// of a comparison. The game uses two default-name pointers (one per side); both must point at an empty string, else the
// hook is not installed.
//
// Key TopicSort: 0 = the game's bubble sort; 1 = the merge sort. The entry is written at start-up only (version.dll route:
// before the game's code runs), so switching it on needs a restart; with it installed, 1 -> 0 goes back to the game's sort live.
#include "common.h"

#include <atomic>

namespace opf {
namespace {

// the whole sort up to the end of its pass loop (exe+0x684EC60): push r14; sub rsp,30h; mov r14,rcx; test rcx,rcx; jnz;
// mov r14,[rip+handler]; add r14,imm32 (the handler's topic list); count the items; dec; jle; the passes: the item at
// +0x38 (name) of this and the next node, defaults lea [rip+""], call [rip+_stricmp]; jle; swap the two items; next
constexpr char kSort[] =
    "41 56 48 83 EC 30 4C 8B F1 48 85 C9 75 0E 4C 8B 35 ?? ?? ?? ?? 49 81 C6 ?? ?? ?? ?? 33 D2 49 8B CE 4D 85 F6 74 1F 66 66 "
    "0F 1F 84 00 00 00 00 00 48 83 39 00 8D 42 01 48 8B 49 08 0F 44 C2 8B D0 48 85 C9 75 EB FF CA 85 D2 0F 8E ?? ?? ?? ?? 48 89 "
    "5C 24 40 48 89 6C 24 48 48 89 74 24 50 48 89 7C 24 28 4C 89 7C 24 20 44 8B FA 0F 1F 44 00 00 49 8B DE 4D 85 F6 74 5B 48 8B "
    "33 48 85 F6 74 53 48 8B 6B 08 48 85 ED 74 41 48 8B 6D 00 48 85 ED 74 38 48 8B 45 38 48 8D 15 ?? ?? ?? ?? 48 85 C0 48 8D 0D "
    "?? ?? ?? ?? 48 0F 45 D0 48 8B 46 38 48 85 C0 48 0F 45 C8 FF 15 ?? ?? ?? ?? 85 C0 7E 0A 48 8B 43 08 48 89 2B 48 89 30 48 8B "
    "5B 08 48 85 DB 75 A5 49 83 EF 01 75 97";
constexpr size_t kGlobalMov = 0x0E;   // mov r14,[rip+disp32] (7 bytes)
constexpr size_t kListAdd = 0x15;     // add r14,imm32 (7 bytes, imm at +3)
constexpr size_t kDefNext = 0x96;     // lea rdx,[rip+disp32]: the next item's default name
constexpr size_t kDefCur = 0xA0;      // lea rcx,[rip+disp32]: this item's default name
constexpr size_t kCmpCall = 0xB6;     // call [rip+disp32]: _stricmp
constexpr size_t kPrologue = 9;       // push r14; sub rsp,30h; mov r14,rcx: moved to the trampoline (no rip-relative bytes)
constexpr uint32_t kName = 0x38;      // the item's name pointer (fixed in the pattern: mov rax,[rbp+38h] / [rsi+38h])
constexpr uint64_t kMaxLines = 60;

struct Node {
    uint8_t* item;
    Node* next;
};
using SortFn = void (*)(Node* list);
using CmpFn = int (*)(const char*, const char*);

bool g_installed = false;
std::atomic<int> g_mode{0};
SortFn g_orig = nullptr;       // the trampoline (the game's sort)
void** g_cmpSlot = nullptr;    // the game's _stricmp import slot (read at each call, as the game does)
uint8_t** g_handler = nullptr; // the data handler pointer (the default list = *g_handler + g_listOffset)
int32_t g_listOffset = 0;
const char* g_defNext = "";
const char* g_defCur = "";
std::atomic<uint64_t> s_calls{0}, s_items{0}, s_us{0}, s_fallback{0}, s_lines{0}, s_verified{0}, s_mismatch{0};


const char* NameOf(const uint8_t* item, const char* def)
{
    const char* const p = *reinterpret_cast<const char* const*>(item + kName);
    return p ? p : def;
}

// The game's swap test: the right item goes before the left one only when _stricmp(left, right) > 0.
bool RightFirst(CmpFn cmp, const uint8_t* left, const uint8_t* right) { return cmp(NameOf(left, g_defCur), NameOf(right, g_defNext)) > 0; }

// Stable bottom-up merge sort of a[0..n) using buf[0..n) (on a tie the left run's item comes first).
uint8_t** MergeSort(CmpFn cmp, uint8_t** a, uint8_t** buf, size_t n)
{
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            const size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            if (mid >= hi || !RightFirst(cmp, a[mid - 1], a[mid])) {  // already in order (or one run): copy
                while (i < hi) buf[k++] = a[i++];
                continue;
            }
            while (i < mid && j < hi) buf[k++] = RightFirst(cmp, a[i], a[j]) ? a[j++] : a[i++];
            while (i < mid) buf[k++] = a[i++];
            while (j < hi) buf[k++] = a[j++];
        }
        uint8_t** const t = a;
        a = buf;
        buf = t;
    }
    return a;
}


// Our sort of the list (the game's list rules). False: no buffer (nothing changed; the caller runs the game's sort).
bool SortList(Node* list, size_t* items)
{
    *items = 0;
    if (!list) return true;
    size_t n = 0;  // the game's count: every node with an item, over the whole list
    for (Node* p = list; p; p = p->next) n += p->item != nullptr;
    *items = n;
    if (n < 2) return true;  // the game: n - 1 passes, none
    size_t run = 0;           // the nodes the passes walk: from the head up to the first node with no item
    for (Node* p = list; p && p->item; p = p->next) ++run;
    if (run < 2) return true;
    uint8_t** const mem = static_cast<uint8_t**>(HeapAlloc(GetProcessHeap(), 0, 2 * run * sizeof(uint8_t*)));
    if (!mem) return false;
    size_t i = 0;
    for (Node* p = list; i < run; p = p->next) mem[i++] = p->item;
    const CmpFn cmp = reinterpret_cast<CmpFn>(*g_cmpSlot);
    uint8_t** const sorted = MergeSort(cmp, mem, mem + run, run);
    i = 0;
    for (Node* p = list; i < run; p = p->next) p->item = sorted[i++];
    HeapFree(GetProcessHeap(), 0, mem);
    return true;
}

}  // namespace

// The entry's target (public for the self-test): the game's argument, nullptr = the data handler's topic list.
void TopicSortEntry(void* arg)
{
    Node* const list = arg ? static_cast<Node*>(arg)
                           : (g_handler && *g_handler ? reinterpret_cast<Node*>(*g_handler + g_listOffset) : nullptr);
    const int mode = g_mode.load(std::memory_order_relaxed);
    size_t items = 0;
    if (mode == 1 && SortList(list, &items)) {
        s_calls.fetch_add(1, std::memory_order_relaxed);
        s_items.fetch_add(items, std::memory_order_relaxed);
        return;
    }
    if (mode == 1) s_fallback.fetch_add(1, std::memory_order_relaxed);
    g_orig(static_cast<Node*>(arg));
}

namespace {

struct Found {
    uint8_t* fn = nullptr;
    void** cmpSlot = nullptr;
    uint8_t** handler = nullptr;
    int32_t listOffset = 0;
    const char* defNext = nullptr;
    const char* defCur = nullptr;
};

bool Discover(const GameModule& game, Found* f, const char** why)
{
    const auto hits = FindAll(game.text, ParsePattern(kSort));
    if (hits.size() != 1) { *why = "the topic list sort was not found once"; return false; }
    uint8_t* const fn = hits[0];
    void** const slot = reinterpret_cast<void**>(RipTarget(fn + kCmpCall, 2, 6));
    void** const want = FindImportSlot(game.base, "api-ms-win-crt-string-l1-1-0.dll", "_stricmp");
    void** const want2 = FindImportSlot(game.base, "ucrtbase.dll", "_stricmp");
    if (!slot || (slot != want && slot != want2)) { *why = "its comparison is not the _stricmp import"; return false; }
    const char* const dn = reinterpret_cast<const char*>(RipTarget(fn + kDefNext, 3, 7));
    const char* const dc = reinterpret_cast<const char*>(RipTarget(fn + kDefCur, 3, 7));
    if (!game.rdata.Contains(dn) || !game.rdata.Contains(dc)) { *why = "its default names are not in .rdata"; return false; }
    if (*dn != 0 || *dc != 0) { *why = "its default names are not both the empty string"; return false; }
    uint8_t** const handler = reinterpret_cast<uint8_t**>(RipTarget(fn + kGlobalMov, 3, 7));
    if (!game.data.Contains(handler)) { *why = "its default list's owner is not in .data"; return false; }
    int32_t off = 0;
    memcpy(&off, fn + kListAdd + 3, 4);
    if (off < 0 || off > 0x10000) { *why = "its default list offset is out of range"; return false; }
    f->fn = fn;
    f->cmpSlot = slot;
    f->handler = handler;
    f->listOffset = off;
    f->defNext = dn;
    f->defCur = dc;
    return true;
}

// Writes the entry jump (5 bytes) and builds the trampoline. On the self-test's mapped exe too.
bool Patch(const Found& f, const char** why)
{
    uint8_t* const region = AllocNearGame(64);
    if (!region) { *why = "no memory near the game's code"; return false; }
    // trampoline: the game's first three instructions (9 bytes, no rip-relative operand), then jmp [rip+0] -> fn + 9
    memcpy(region, f.fn, kPrologue);
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(region + kPrologue, jmpAbs, sizeof jmpAbs);
    uint8_t* const back = f.fn + kPrologue;
    memcpy(region + kPrologue + sizeof jmpAbs, &back, sizeof back);
    // the entry's target: jmp [rip+0] -> TopicSortEntry
    uint8_t* const stub = region + 32;
    memcpy(stub, jmpAbs, sizeof jmpAbs);
    void* const hook = reinterpret_cast<void*>(&TopicSortEntry);
    memcpy(stub + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    if (!VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old)) {
        FreeOwnCode(region);
        *why = "could not make the stubs executable";
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = stub - (f.fn + 5);
    if (rel != static_cast<int32_t>(rel)) { FreeOwnCode(region); *why = "out of reach"; return false; }
    g_orig = reinterpret_cast<SortFn>(region);
    g_cmpSlot = f.cmpSlot;
    g_handler = f.handler;
    g_listOffset = f.listOffset;
    g_defNext = f.defNext;
    g_defCur = f.defCur;
    uint8_t bytes[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(bytes + 1, &rel32, 4);
    if (!WriteCode(f.fn, bytes)) { *why = "could not write the entry"; return false; }
    return true;
}

}  // namespace

bool InstallTopicSort(bool beforeGameCode, int key)
{
    if (g_installed) return true;
    if (key == 0) {
        Log("[topicsort] TopicSort off in OblivionPerfFix.ini (takes a restart to switch on)");
        return false;
    }
    if (!beforeGameCode) {
        Log("[topicsort] TopicSort: only installed when the game starts (loaded late here), not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) {
        Log("[topicsort] TopicSort not installed: %s", why);
        return false;
    }
    g_mode = key == 1 ? 1 : 0;
    if (!Patch(f, &why)) {
        Log("[topicsort] TopicSort not installed: %s", why);
        return false;
    }
    g_installed = true;
    Log("[topicsort] dialogue topic sort exe+0x%X (_stricmp import exe+0x%X, default list [exe+0x%X]+0x%X): installed, %s",
        static_cast<unsigned>(f.fn - game.base), static_cast<unsigned>(reinterpret_cast<uint8_t*>(f.cmpSlot) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(f.handler) - game.base), static_cast<unsigned>(f.listOffset),
        "merge sort");
    return true;
}

void SetTopicSort(int key)
{
    const int mode = key == 1 ? 1 : 0;
    if (!g_installed || g_mode.load() == mode) return;
    g_mode = mode;
    Log("[topicsort] TopicSort now %d", mode);
}

void LogTopicSortStats()
{
    if (!g_installed) return;
    const uint64_t calls = s_calls.exchange(0);
    const uint64_t fallback = s_fallback.exchange(0);
    if (!calls && !fallback) return;
    const uint64_t items = s_items.exchange(0), us = s_us.exchange(0);
    (void)us;
    Log("[topicsort] since last report: %llu topic list sorts (%llu items), %llu ran the game's sort (no memory)",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(items), static_cast<unsigned long long>(fallback));
}

// self-test
bool TopicSortTestDiscover(const GameModule& game, uint32_t rva[5], int32_t* listOffset)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return false;
    rva[0] = static_cast<uint32_t>(f.fn - game.base);
    rva[1] = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(f.cmpSlot) - game.base);
    rva[2] = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(f.handler) - game.base);
    rva[3] = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(f.defCur) - game.base);
    rva[4] = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(f.defNext) - game.base);
    *listOffset = f.listOffset;
    return true;
}

// Patches the mapped exe's entry like the game's install (the self-test then calls the entry); mode as the key.
bool TopicSortTestInstall(const GameModule& game, int mode)
{
    Found f;
    const char* why = "";
    if (!Discover(game, &f, &why)) return false;
    g_mode = mode;
    if (!Patch(f, &why)) return false;
    g_installed = true;
    return true;
}

void TopicSortTestMode(int mode) { g_mode = mode; }

// Our sort alone (fake lists): cmp = the comparison to use, defaults "".
bool TopicSortTestSort(void* list, int (*cmp)(const char*, const char*))
{
    static void* slot = nullptr;
    slot = reinterpret_cast<void*>(cmp);
    void** const keep = g_cmpSlot;
    g_cmpSlot = &slot;
    size_t items = 0;
    const bool ok = SortList(static_cast<Node*>(list), &items);
    g_cmpSlot = keep;
    return ok;
}

}  // namespace opf
