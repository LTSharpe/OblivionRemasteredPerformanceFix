// SlotClickPayloadCopy (S-085): clicking a save on the in-game Load page (and probably, through the same handler, on the Save page)
// spends 6-13 ms of one game-thread frame copying the save's bytes (2026-11-26 lab probe: 8 copies of the whole save plus
// a zero fill and a memcpy per click; 4.0 MB save 5.9-8.4 ms, 5.7 MB save 8.5-12.8 ms; every copy into fresh pages).
//
// Altar's slot-click handler (exe+0x4999A40 in build 19115871; it ends in the Blueprint event
// "Send_On_Item_Clicked_Handler") gets the save container's byte array as a std::vector<uint8_t> P0 from its slot lookup
// (exe+0x48BCE90), then hands it BY VALUE down a chain; each hand-off runs the vector's copy constructor (exe+0x48A7100:
// operator new, then memmove):
//   c1  handler    exe+0x4999C3B  P0 (handler local, only destroyed afterwards)  -> T1, the bridge's by-value argument
//   c2  bridge     exe+0x4995E8F  T1 (the bridge's by-value parameter, only destroyed afterwards) -> T2, argument of 0x4995F90
//   c3  0x4995F90  exe+0x4995FF4  T2 (its by-value parameter, only destroyed afterwards) -> the member +0x48 of the object it
//                                 builds (the event's command, kept)
//   c4  0x4995F90  exe+0x4996028  the member -> a lambda capture L1.vec; L1.vec -> c5 L2.vec -> c6 -> c7 -> the by-value
//                                 argument of exe+0x49956D0, which copies it once more (c8) and destroys everything without
//                                 reading a byte: its whole body is copies and destructors (checked by its full byte shape).
// So c1-c3 copy from vectors that are dead right after the copy (C++ would have moved them), and c4-c8 feed nothing.
//
// The fix (SlotClickPayloadCopy=1): c1, c2 and c3 become moves (the destination takes the source's buffer, the source is
// left empty; its later destructor then frees nothing), and c4 makes an empty vector, so c5-c8 copy an empty vector
// (the constructor's size-0 path: no allocation). The event's command gets the same bytes in the buffer the lookup filled
// (same size and capacity: every vector of the chain has capacity == size); nothing else read the dropped copies.
// What is left per click is the lookup's own zero fill and memcpy into P0 (~1-1.7 ms).
//
// Found at start-up by byte shape, each step checked against the next (all must hold, else the key stays off with a log
// line): the handler's event block (lea r9 = "Send_On_Item_Clicked_Handler", c1 to one of the exe's two identical copy
// constructors), the bridge's block (c2 to the same constructor, the call of 0x4995F90), 0x4995F90's capture chain (c3-c7
// to the same constructor, the call of the no-op), the no-op's whole body (its copy to the same constructor, only string
// copies and frees otherwise). The sites are rel32 call retargets to two stubs next to the exe (`jmp [rip] hook`); the
// hooks switch live (0 = they call the game's constructor, the game's behaviour).
#include "common.h"


#include <atomic>
#include <cstring>
#include <intrin.h>

namespace opf {
namespace {

// std::vector<uint8_t> copy constructor (exe+0x48A7100 and exe+0x66EA500): zero the three pointers, n = other.end -
// other.begin, operator new (the 32-byte aligned path for n >= 4 KB), then memmove(dst, other.begin, n). Its frame
// (push r14; sub rsp,20h) puts the caller's return address at [rsp+30h] inside the memmove call.
constexpr char kCopyCtor[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 C0 4C 8B F2 48 89 01 48 8B F1 48 89 41 08 "
    "48 89 41 10 48 8B 5A 08 48 2B 1A 74 7B 48 B8 FF FF FF FF FF FF FF 7F 48 3B D8 0F 87 87 00 00 00 48 81 FB 00 10 "
    "00 00 72 28 48 8D 4B 27 48 3B CB 76 6F E8 ?? ?? ?? ?? 48 85 C0 74 0E 48 8D 78 27 48 83 E7 E0 48 89 47 F8 EB 12 "
    "FF 15 ?? ?? ?? ?? CC 48 8B CB E8 ?? ?? ?? ?? 48 8B F8 48 89 3E 48 8D 0C 1F 48 89 7E 08 48 89 4E 10 48 8B CF 49 "
    "8B 16 49 8B 5E 08 48 2B DA 4C 8B C3 E8 ?? ?? ?? ?? 48 8D 04 3B 48 89 46 08 48 8B 5C 24 30 48 8B C6 48 8B 74 24 "
    "38 48 8B 7C 24 40 48 83 C4 20 41 5E C3";
constexpr size_t kCtorFillCall = 0x9F;
// The click handler (exe+0x4999A40), lookup result 1: ... c1 (lea rdx,[rsp+50h] = P0; lea rcx,[rsp+90h] = T1; call ctor),
// the event name in r9, call of the event bridge (exe+0x4995E00).
constexpr char kClickBlock[] =
    "48 8D 4C 24 70 48 8B E8 E8 ?? ?? ?? ?? 48 8D 54 24 50 48 8B F8 48 8D 8C 24 90 00 00 00 E8 ?? ?? ?? ?? 49 8B CE "
    "48 8B D8 E8 ?? ?? ?? ?? 40 88 74 24 40 48 8D 0D ?? ?? ?? ?? 48 89 7C 24 38 4C 8D 0D ?? ?? ?? ?? 48 89 5C 24 30 "
    "45 33 C0 C7 44 24 28 00 00 00 00 48 8B D0 48 89 4C 24 20 48 8B CD E8 ?? ?? ?? ??";
constexpr size_t kClickCopyCall = 0x1D, kClickNameLea = 0x3E, kClickEventCall = 0x60, kClickFromHandler = 0x1DE;
constexpr char kEventName[] = "Send_On_Item_Clicked_Handler";
// The handler's entry (frame, cookie, P0 at [rsp+50h] zeroed, the slot lookup's call) and its end: P0 freed once
// (mov rcx,[rsp+50h]; test; je; mov rdx,[rsp+60h] ...): P0 is not read between c1 and its destructor (no other
// [rsp+50h..60h] access in the handler).
constexpr char kHandlerEntry[] =
    "4C 8B DC 48 81 EC B8 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 01 00 00 49 89 5B 18 4C 8D 44 24 50 "
    "49 89 73 F8 0F 57 C0 49 89 7B F0 48 8B DA 4D 89 73 E8 4C 8B F1 48 8B 89 68 01 00 00 F3 0F 7F 44 24 50 48 C7 44 "
    "24 60 00 00 00 00 E8 ?? ?? ?? ??";
constexpr size_t kHandlerLookupCall = 0x50;
// P0's destructor at the handler's end (exe+0x4999CFE): mov rcx,[rsp+50h]; test rcx,rcx; je; mov rdx,[rsp+60h]
constexpr char kHandlerP0Free[] = "48 8B 4C 24 50 48 85 C9 74 35 48 8B 54 24 60";
constexpr size_t kHandlerLen = 0x320;
constexpr int kHandlerP0Uses = 6;  // rsp-disp8 accesses to +50h/+58h/+60h in the handler (build 19115871: +0x22, 0x44, 0x49, 0x1ED, 0x2C0, 0x2CA)
// The bridge's prologue load of its by-value vector parameter T1: mov r13,[rsp+100h] (exe+0x4995E1A)
constexpr char kBridgeT1[] = "4C 8B AC 24 00 01 00 00";
constexpr size_t kBridgeT1Load = 0x1A;
// The builder's prologue: mov r15,[rsp+178h] = T2 (exe+0x4995FB8), mov r12,rcx = the command (exe+0x4995FC6)
constexpr char kBuildT2[] = "4C 8B BC 24 78 01 00 00";
constexpr char kBuildThisMov[] = "4C 8B E1";
constexpr size_t kBuildT2Load = 0x28, kBuildThis = 0x36;
// The event bridge (exe+0x4995E00), from the string copy of its name argument: c2 (rdx = r13 = T1, its by-value
// parameter; rcx = [rsp+40h] = T2), then the call of 0x4995F90 with T2 (rax) as an argument; afterwards (not part of the
// pattern) a virtual call that gets r14 / rbp / r15 only, then T1 freed.
constexpr char kBridgeBlock[] =
    "49 8B D4 48 8D 4C 24 58 E8 ?? ?? ?? ?? 49 8B D5 48 8D 4C 24 40 48 8B D8 E8 ?? ?? ?? ?? 44 0F B6 8C 24 10 01 00 00 "
    "4D 8B C7 44 88 4C 24 38 48 8B D6 4C 8B 8C 24 F0 00 00 00 48 8B CF 48 89 5C 24 30 48 89 44 24 28 8B 84 24 F8 00 "
    "00 00 89 44 24 20 E8 ?? ?? ?? ??";
constexpr size_t kBridgeCopyCall = 0x18, kBridgeBuildCall = 0x51;
// After the build call the bridge makes one virtual call that gets r14 / rbp / r15 (not r13 = &T1), then frees T1:
// mov rax,[r14]; mov r8,r15; mov rdx,rbp; mov rcx,r14; call [rax+0D0h]; mov rcx,[r13]; xor ebx,ebx; test rcx,rcx
constexpr char kBridgeAfter[] = "49 8B 06 4D 8B C7 48 8B D5 49 8B CE FF 90 D0 00 00 00 49 8B 4D 00 33 DB 48 85 C9";
// 0x4995F90 (builds the event's command at r12 from its arguments; r15 = T2): the member +0x28 string copy, c3 (member
// +0x48 <- T2), then the capture chain: L1 {byte, string, c4 vec <- member +0x48, int}; a call with rcx = rsi only;
// L2 {byte, string, c5 vec <- L1.vec}; c6 [rsp+B0h] <- L2.vec; a string; c7 [rsp+C8h] <- c6; the no-op with (c7, string, byte).
constexpr char kChain[] =
    "48 83 C1 28 E8 ?? ?? ?? ?? 49 8B D7 49 8D 4C 24 48 E8 ?? ?? ?? ?? 8B 84 24 70 01 00 00 49 8D 54 24 28 41 89 44 24 60 "
    "48 8D 4C 24 28 41 0F B6 44 24 20 88 44 24 20 E8 ?? ?? ?? ?? 49 8D 54 24 48 48 8D 4C 24 48 E8 ?? ?? ?? ?? 41 8B 44 "
    "24 60 48 8B CE 89 44 24 60 E8 ?? ?? ?? ?? 0F B6 44 24 20 48 8D 54 24 28 48 8D 4C 24 70 88 44 24 68 E8 ?? ?? ?? ?? "
    "48 8D 54 24 48 48 8D 8C 24 90 00 00 00 E8 ?? ?? ?? ?? 8B 44 24 60 48 8D 54 24 70 0F B6 7C 24 68 48 8D 8C 24 E0 00 "
    "00 00 89 84 24 A8 00 00 00 E8 ?? ?? ?? ?? 48 8D 94 24 90 00 00 00 48 8B F0 48 8D 8C 24 B0 00 00 00 E8 ?? ?? ?? ?? "
    "48 8B D6 48 8D 8C 24 00 01 00 00 48 8B E8 E8 ?? ?? ?? ?? 48 8B D5 48 8D 8C 24 C8 00 00 00 48 8B D8 E8 ?? ?? ?? ?? "
    "44 0F B6 C7 48 8B D3 48 8B C8 E8 ?? ?? ?? ??";
constexpr size_t kChainStringCall = 0x04, kChainC3 = 0x11, kChainC4 = 0x45, kChainC5 = 0x80, kChainC6 = 0xBA, kChainC7 = 0xE0,
                 kChainNoOpCall = 0xEF;
constexpr size_t kChainStringCalls[4] = {0x36, 0x6E, 0xA2, 0xCD};
// After the no-op call, 0x4995F90 frees the c6 vector and the L3 string, destroys L2 and L1 (one destructor), then frees
// T2 (r15): mov rcx,[rbp]; ...; the two lambda destructor calls (lea rcx,[rsp+68h] / [rsp+20h]) and mov rcx,[r15].
constexpr char kChainTail[] = "48 8D 4C 24 68 48 89 5E 10 48 C7 46 18 0F 00 00 00 88 1E E8 ?? ?? ?? ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? "
                              "49 8B 0F 48 85 C9";
constexpr size_t kChainTailFrom = 0x176;  // its offset from the chain block (exe+0x4996159 - exe+0x4995FE3)
constexpr size_t kTailDtorCalls[2] = {0x13, 0x1D};
// exe+0x49956D0, the no-op: copies its vector argument (call ctor) and frees the copy, copies the string argument twice
// and frees both copies, frees its two arguments, returns. Nothing else.
constexpr char kNoOp[] =
    "48 89 5C 24 20 55 57 41 56 48 83 EC 60 48 8B DA 4C 8B F1 48 8B D1 48 8D 4C 24 20 E8 ?? ?? ?? ?? 33 ED 48 8B F8 48 8B "
    "08 48 85 C9 74 3C 48 8B 50 10 48 2B D1 48 81 FA 00 10 00 00 72 1C 4C 8B 41 F8 48 83 C2 27 49 2B C8 48 8D 41 F8 48 83 "
    "F8 1F 0F 87 6F 01 00 00 49 8B C8 E8 ?? ?? ?? ?? 48 89 2F 48 89 6F 08 48 89 6F 10 48 8B D3 48 89 B4 24 90 00 00 00 48 "
    "8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 40 48 8B F8 E8 ?? ?? ?? ?? 48 8B F0 48 8B 50 18 48 83 FA 10 72 30 48 "
    "8B 08 48 FF C2 48 81 FA 00 10 00 00 72 1C 4C 8B 41 F8 48 83 C2 27 49 2B C8 48 8D 41 F8 48 83 F8 1F 0F 87 09 01 00 00 "
    "49 8B C8 E8 ?? ?? ?? ?? 48 89 6E 10 48 C7 46 18 0F 00 00 00 40 88 2E 48 8B 57 18 48 83 FA 10 72 30 48 8B 0F 48 FF C2 "
    "48 81 FA 00 10 00 00 72 1C 4C 8B 41 F8 48 83 C2 27 49 2B C8 48 8D 41 F8 48 83 F8 1F 0F 87 C7 00 00 00 49 8B C8 E8 ?? "
    "?? ?? ?? 48 89 6F 10 48 C7 47 18 0F 00 00 00 40 88 2F 49 8B 0E 48 85 C9 74 3C 49 8B 56 10 48 2B D1 48 81 FA 00 10 00 "
    "00 72 1C 4C 8B 41 F8 48 83 C2 27 49 2B C8 48 8D 41 F8 48 83 F8 1F 0F 87 86 00 00 00 49 8B C8 E8 ?? ?? ?? ?? 49 89 2E "
    "49 89 6E 08 49 89 6E 10 48 8B 53 18 48 83 FA 10 72 2C 48 8B 0B 48 FF C2 48 81 FA 00 10 00 00 72 18 4C 8B 41 F8 48 83 "
    "C2 27 49 2B C8 48 8D 41 F8 48 83 F8 1F 77 45 49 8B C8 E8 ?? ?? ?? ?? 48 8B B4 24 90 00 00 00 48 89 6B 10 48 C7 43 18 "
    "0F 00 00 00 40 88 2B 48 8B 9C 24 98 00 00 00 48 83 C4 60 41 5E 5F 5D C3";
constexpr size_t kNoOpCopy = 0x1B;
constexpr size_t kNoOpStrings[2] = {0x79, 0x89};
constexpr size_t kNoOpFrees[5] = {0x59, 0xC6, 0x10F, 0x157, 0x198};

struct Vec { uint8_t *first, *last, *end; };  // MSVC std::vector<uint8_t>
using CtorFn = Vec* (*)(Vec* dst, const Vec* src);

bool g_installed = false;
std::atomic<int> g_mode{0};
CtorFn g_ctor = nullptr;
SlotClickCopySites g_sites = {};
std::atomic<uint64_t> g_moves{0}, g_moveBytes{0}, g_empties{0}, g_emptyBytes{0};
uint64_t g_logged = 0;

bool FixOn(int mode) { return mode == 1 || mode == 3 || mode == 4; }

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

double Ms(uint64_t ticks)
{
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    return static_cast<double>(ticks) / perMs;
}


}  // namespace

// c1-c3: the destination takes the source's buffer; the source (dead: only destroyed afterwards) is left empty.
void* SlotClickMoveHook(void* dstP, void* srcP)
{
    Vec* const dst = static_cast<Vec*>(dstP);
    Vec* const src = static_cast<Vec*>(srcP);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!FixOn(mode)) return g_ctor(dst, src);
    *dst = *src;
    *src = Vec{nullptr, nullptr, nullptr};
    g_moves.fetch_add(1, std::memory_order_relaxed);
    g_moveBytes.fetch_add(static_cast<uint64_t>(dst->last - dst->first), std::memory_order_relaxed);
    return dst;
}

// c4: the capture chain that feeds the no-op starts from an empty vector.
void* SlotClickEmptyHook(void* dstP, void* srcP)
{
    Vec* const dst = static_cast<Vec*>(dstP);
    const Vec* const src = static_cast<const Vec*>(srcP);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!FixOn(mode)) return g_ctor(dst, src);
    *dst = Vec{nullptr, nullptr, nullptr};
    g_empties.fetch_add(1, std::memory_order_relaxed);
    g_emptyBytes.fetch_add(static_cast<uint64_t>(src->last - src->first), std::memory_order_relaxed);
    return dst;
}


namespace {

bool Fail(const char* why)
{
    Log("[slotclick] %s: SlotClickPayloadCopy not installed", why);
    return false;
}

uint8_t* CallTarget(const uint8_t* call) { return call[0] == 0xE8 || call[0] == 0xE9 ? RipTarget(call, 1, 5) : nullptr; }

// The one place in [from, from + len) where the pattern matches (nullptr if none or several).
uint8_t* FindIn(uint8_t* from, size_t len, const Pattern& p)
{
    const GameModule& game = Game();
    uint8_t* hit = nullptr;
    for (uint8_t* q = from; q + p.size() <= from + len && q + p.size() <= game.text.end; ++q)
        if (MatchesAt(q, p)) {
            if (hit) return nullptr;
            hit = q;
        }
    return hit;
}

bool Retarget(uint8_t* call, const uint8_t* to)
{
    const int64_t rel = to - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

bool Reach(const uint8_t* from, const uint8_t* to)
{
    const int64_t rel = to - (from + 5);
    return rel == static_cast<int32_t>(rel);
}

void Put64(uint8_t* at, const void* p) { memcpy(at, &p, 8); }

uint8_t* EmitJump(uint8_t* at, const void* hook)
{
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, 6);
    Put64(at + 6, hook);
    return at;
}


}  // namespace

namespace {

struct Found {
    uint8_t *ctor, *other, *handler, *c1, *c2, *c3, *c4, *noop;
    size_t ctorCount;
};

// A call's target, with our two stubs read as the game's constructor (the self-test runs the finder after the install).
uint8_t* Target(const uint8_t* call)
{
    uint8_t* const t = CallTarget(call);
    return g_installed && t && (t == g_sites.moveStub || t == g_sites.emptyStub) ? g_sites.copyCtor : t;
}

// Every byte-shape check of the chain; nullptr = all hold, else the reason (the self-test runs it on a changed byte too).
const char* FindSites(Found& f)
{
    const GameModule& game = Game();
    f = Found{};
    const auto ctorHits = FindAll(game.text, ParsePattern(kCopyCtor));
    const auto blockHits = FindAll(game.text, ParsePattern(kClickBlock));
    f.ctorCount = ctorHits.size();
    if (blockHits.size() != 1 || ctorHits.empty() || ctorHits.size() > 4) {
        Log("[slotclick] the slot-click handler's event block found %zu times (expected 1), std::vector<uint8_t>'s copy "
            "constructor %zu times (expected 1-4)", blockHits.size(), ctorHits.size());
        return "code not as expected";
    }
    uint8_t* const block = blockHits[0];
    f.c1 = block + kClickCopyCall;
    f.ctor = Target(f.c1);
    bool ctorKnown = false;
    for (uint8_t* h : ctorHits) {
        if (h == f.ctor) ctorKnown = true;
        else f.other = h;
    }
    if (!ctorKnown) return "the click handler's copy is not one of the copy constructors found";
    const char* const name = reinterpret_cast<const char*>(RipTarget(block + kClickNameLea, 3, 7));
    if (!game.rdata.Contains(name) || memcmp(name, kEventName, sizeof kEventName) != 0)
        return "the click block does not name Send_On_Item_Clicked_Handler";
    f.handler = block - kClickFromHandler;
    if (!game.text.Contains(f.handler) || !MatchesAt(f.handler, ParsePattern(kHandlerEntry)))
        return "the click handler's entry is not as expected";
    if (!FindIn(f.handler, kHandlerLen, ParsePattern(kHandlerP0Free))) return "the click handler's end is not as expected";
    // P0 ([rsp+50h], 24 bytes): the handler's only rsp-relative accesses to +50h / +58h / +60h are the six known ones (the
    // zeroing x2, the lookup's out argument, c1's source, the destructor's two loads): nothing else reads P0
    {
        int n = 0;
        const uint8_t* const h = f.handler;
        for (size_t i = 0; i + 6 <= kHandlerLen; ++i) {
            if ((h[i] & 0xC7) == 0x44 && h[i + 1] == 0x24 && (h[i + 2] == 0x50 || h[i + 2] == 0x58 || h[i + 2] == 0x60)) ++n;
            if ((h[i] & 0xC7) == 0x84 && h[i + 1] == 0x24 && (h[i + 2] == 0x50 || h[i + 2] == 0x58 || h[i + 2] == 0x60) &&
                h[i + 3] == 0 && h[i + 4] == 0 && h[i + 5] == 0) n += 100;
        }
        if (n != kHandlerP0Uses) return "the click handler touches its payload vector at more places than expected";
    }
    // the bridge: T1 is its by-value parameter (mov r13,[rsp+100h] in the prologue), c2, the build call, then a virtual call
    // without T1 and T1's destructor
    uint8_t* const bridge = CallTarget(block + kClickEventCall);
    if (!bridge || !game.text.Contains(bridge) || !MatchesAt(bridge + kBridgeT1Load, ParsePattern(kBridgeT1)))
        return "the event bridge's prologue is not as expected";
    uint8_t* const bb = FindIn(bridge, 0x100, ParsePattern(kBridgeBlock));
    if (!bb || Target(bb + kBridgeCopyCall) != f.ctor || !MatchesAt(bb + kBridgeBuildCall + 5, ParsePattern(kBridgeAfter)))
        return "the event bridge is not as expected";
    f.c2 = bb + kBridgeCopyCall;
    // the command builder: T2 its by-value parameter (mov r15,[rsp+178h]; mov r12,rcx in the prologue), c3 .. c7, the no-op
    uint8_t* const build = CallTarget(bb + kBridgeBuildCall);
    if (!build || !game.text.Contains(build) || !MatchesAt(build + kBuildT2Load, ParsePattern(kBuildT2)) ||
        !MatchesAt(build + kBuildThis, ParsePattern(kBuildThisMov)))
        return "the command builder's prologue is not as expected";
    uint8_t* const chain = FindIn(build, 0x200, ParsePattern(kChain));
    if (!chain) return "the command builder's capture chain is not as expected";
    const size_t copies[5] = {kChainC3, kChainC4, kChainC5, kChainC6, kChainC7};
    for (size_t off : copies)
        if (Target(chain + off) != f.ctor) return "a copy in the capture chain does not call the same constructor";
    uint8_t* const strCopy = CallTarget(chain + kChainStringCall);
    for (size_t off : kChainStringCalls)
        if (CallTarget(chain + off) != strCopy) return "the capture chain's string copies are not one function";
    if (!MatchesAt(chain + kChainTailFrom, ParsePattern(kChainTail)) ||
        CallTarget(chain + kChainTailFrom + kTailDtorCalls[0]) != CallTarget(chain + kChainTailFrom + kTailDtorCalls[1]))
        return "the command builder's end is not as expected";
    f.noop = CallTarget(chain + kChainNoOpCall);
    if (!f.noop || !game.text.Contains(f.noop) || !MatchesAt(f.noop, ParsePattern(kNoOp)) || CallTarget(f.noop + kNoOpCopy) != f.ctor)
        return "the capture chain's last call is not the expected no-op";
    for (size_t off : kNoOpStrings)
        if (CallTarget(f.noop + off) != strCopy) return "the no-op's string copies are not the chain's";
    uint8_t* const freeFn = CallTarget(f.noop + kNoOpFrees[0]);
    for (size_t off : kNoOpFrees)
        if (CallTarget(f.noop + off) != freeFn) return "the no-op's frees are not one function";
    f.c3 = chain + kChainC3;
    f.c4 = chain + kChainC4;
    return nullptr;
}

}  // namespace

bool SlotClickTestFind() { Found f; return FindSites(f) == nullptr; }

bool InstallSlotClickPayloadCopy(bool beforeGameCode)
{
    if (g_installed) return true;
    const GameModule& game = Game();
    Found f;
    if (const char* why = FindSites(f)) return Fail(why);
    uint8_t* const ctor = f.ctor;
    uint8_t* const handler = f.handler;
    uint8_t *const c1 = f.c1, *const c2 = f.c2, *const c3 = f.c3, *const c4 = f.c4, *const noop = f.noop;
    if (!beforeGameCode) return Fail("only installed while the game starts");

    // region: page 0 code (the two stubs; lab: the probe's stubs), page 1 data (lab: the probe's threshold word)
    uint8_t* const region = AllocNearGame(8192);
    if (!region) return Fail("no free memory near the game's code");
    uint8_t* const moveStub = region;
    uint8_t* const emptyStub = region + 16;
    EmitJump(moveStub, reinterpret_cast<const void*>(&SlotClickMoveHook));
    EmitJump(emptyStub, reinterpret_cast<const void*>(&SlotClickEmptyHook));
    DWORD old;
    VirtualProtect(region, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 4096);
    if (!Reach(c1, moveStub) || !Reach(c2, moveStub) || !Reach(c3, moveStub) || !Reach(c4, emptyStub)) {
        VirtualFree(region, 0, MEM_RELEASE);
        return Fail("stubs out of reach");
    }
    g_ctor = reinterpret_cast<CtorFn>(ctor);
    if (!Retarget(c1, moveStub)) {
        VirtualFree(region, 0, MEM_RELEASE);  // nothing points into it yet
        return Fail("could not patch the handler's copy");
    }
    // after the first write the others are needed for nothing: a failed later write leaves that site the game's copy
    const bool ok2 = Retarget(c2, moveStub), ok3 = Retarget(c3, moveStub), ok4 = Retarget(c4, emptyStub);
    uint8_t* const other = f.other;
    g_sites.copyCtor = ctor;
    g_sites.otherCopyCtor = other;
    g_sites.handler = handler;
    g_sites.moveSites[0] = c1;
    g_sites.moveSites[1] = ok2 ? c2 : nullptr;
    g_sites.moveSites[2] = ok3 ? c3 : nullptr;
    g_sites.emptySite = ok4 ? c4 : nullptr;
    g_sites.noop = noop;
    g_sites.moveStub = moveStub;
    g_sites.emptyStub = emptyStub;
    g_installed = true;
    auto rva = [&](const uint8_t* p) { return p ? static_cast<unsigned>(p - game.base) : 0u; };
    Log("[slotclick] installed: std::vector<uint8_t> copy constructor exe+0x%X (%zu copies in the exe; the other one exe+0x%X "
        "left alone); moves at exe+0x%X (handler), exe+0x%X (event bridge), exe+0x%X (command builder); empty capture at "
        "exe+0x%X (feeds the no-op exe+0x%X)",
        rva(ctor), f.ctorCount, rva(other), rva(c1), rva(g_sites.moveSites[1]), rva(g_sites.moveSites[2]),
        rva(g_sites.emptySite), rva(noop));
    return true;
}

void SetSlotClickPayloadCopy(int mode)
{
    if (!g_installed) return;
    mode = mode != 0 ? 1 : 0;
    if (g_mode.exchange(mode) == mode) return;
    Log("[slotclick] SlotClickPayloadCopy %d: %s", mode,
        mode == 1   ? "the slot click's dead copies of the save are moves / an empty vector (the same bytes reach the event)"
        : mode == 2 ? "lab: the game's copies, timed ([slotclick] line per click)"
        : mode == 3 ? "lab: the fix, timed"
        : mode == 4 ? "lab: the fix, timed, the remaining big fills on the helper threads (compared with their source)"
                    : "the game's copies");
}

void LogSlotClickPayloadCopyStats()
{
    if (!g_installed) return;
    const uint64_t sum = g_moves.load() + g_empties.load();
    if (sum == g_logged) return;
    g_logged = sum;
    Log("[slotclick] since start: %llu copies made moves (%.0f MB not copied), %llu copies into the no-op made empty (%.0f MB, and "
        "the 4 copies after each)",
        static_cast<unsigned long long>(g_moves.load()), static_cast<double>(g_moveBytes.load()) / 1048576.0,
        static_cast<unsigned long long>(g_empties.load()), static_cast<double>(g_emptyBytes.load()) / 1048576.0);
}

bool SlotClickPayloadCopyInfo(SlotClickCopySites* out)
{
    if (!g_installed) return false;
    *out = g_sites;
    return true;
}

void SlotClickTestSetCtor(void* ctor) { g_ctor = reinterpret_cast<CtorFn>(ctor); }


}  // namespace opf
