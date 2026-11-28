// DfAtlasKeep (worker dfatlas, 2026-11-26; S-001): the mesh distance-field brick atlas is not re-created smaller.
//
// UE 5.3 FDistanceFieldSceneData::ResizeBrickAtlasIfNeeded (DistanceFieldStreaming.cpp:929; exe+0x1DA0EF0 in build 19115871,
// render thread, once a frame from the distance-field update) sizes the 3D texture "DistanceFields.DistanceFieldBrickTexture"
// from the brick allocator's high-water mark: Z = ceil(MaxSize x GDistanceFieldBlockAllocatorSizeInBricks / XY^2) bricks,
// rounded up to a power of two while <= r.DistanceFields.BrickAtlasMaxSizeZ (32), else to a multiple of 4; XY is
// r.DistanceFields.BrickAtlasSizeXYInBricks (128, read-only); 8^3 voxels a brick at PF_G8 = 8 MB per Z brick. When the
// texture's size differs (`!=`, in both directions; unchanged in UE 5.4.4-5.7.4) it makes a new texture, copies the old one
// into it (CopyDistanceFieldAtlas) and ConvertToExternalTexture allocates it at once: a committed resource the render thread
// waits for. After a fast travel the allocator's top falls, the atlas is re-made SMALLER, and the next stream-in on the road
// re-makes it larger again (old + new alive together for the copy). Near the video-memory budget that allocation waits for
// Windows' paging: 140-176 ms on the render thread in the Fort Empire repro (findings fortdf1126, 3 of 4 passes; 3-6 ms with
// headroom), plus 7-19 frames of 20-28 ms after it.
//
// With the key on, a call that would make the atlas SMALLER keeps the one it has: the hook returns the function's own
// no-resize path, GraphBuilder.RegisterExternalTexture(DistanceFieldBrickVolumeTexture) (the call the function itself makes
// when the size matches, found inside it). Growth goes through the game's function unchanged. Kept only while all hold: the
// texture exists, r.DistanceFields.ResizeAtlasEveryFrame is 0, the texture's size is (XY x 8, XY x 8, Z x 8) with Z =
// BrickTextureDimensionsInBricks.Z (this+0x408; the function writes it with the texture), XY matches the cvar, Z is a power of
// two and <= the cvar's BrickAtlasMaxSizeZ, and the size the game computes is smaller than Z.
//
// Why the image is identical (UE 5.3 source): which bricks exist and where they are is decided by the block allocator and the
// streaming code, which never read the texture's Z (allocation, NumBricksBeforeDroppingMips from the cvar, the indirection
// table's global brick indices). Brick coordinates are (i mod XY, (i / XY) mod XY, i / XY^2) on the CPU (GetBrickCoordinate)
// and in the shaders (DistanceFieldLightingShared.ush: Mask.x/.y and Log2.x/.y only), so no coordinate depends on Z. Only the
// normalised Z coordinate does: AtlasUV.z = brickZ x 8/D + local x 7/D + 0.5/D with D = 8Z (SetupAtlasParameters'
// TexelSize = 1/D). With both Z values powers of two (the kept one by the rule above, the game's smaller one because it is
// below the power-of-two limit), every term and every IEEE operation on them scales exactly by the power of two, so the
// sampler's texel coordinate u x D is bit for bit the same; filtering never crosses a brick (half-voxel borders inside each
// brick), so the wrap mode never applies. Bricks above the game's size are never indexed; their contents are undefined in
// the game too (a new atlas is not cleared, the copy covers the old size). The game's own later growth beyond the kept size
// is a normal re-creation. Video memory: the atlas stays at the largest power-of-two size this session already had, at most
// BrickAtlasMaxSizeZ x 8 MB (256 MB at the defaults); a larger atlas shrinks as in the game.
//
// Lines: one per re-creation (`[dfatlas] q <qpc ms> Z <old> -> <new> ...`) with the call's time, the last video-memory
// sample and the loading-screen state; one per kept episode (its start); a stats line when something happened.
// Install: entry detour (an aligned 5-byte jump; the 5-byte mov [rsp+20h],rbx moved into the trampoline), start-up when the
// key is != 0, then live: 1 keep, 0 / 2 the game's way (still logged).
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// The function's head up to its no-resize return (build 19115871 exe+0x1DA0EF0, offsets in the comments):
//   +0x00 mov [rsp+20h],rbx; push rbp/rsi/rdi/r12-r15; lea rbp,[rsp-..]; sub rsp,..; security cookie
//   +0x30 mov rax,[rip+XY cvar data]; mov r13,rdx; mov [rsp+38h],r8; mov r14,rcx; mov [rsp+58h],rcx; mov r15d,1
//   +0x4D mov ebx,[rax+4] (XY, render-thread value); mov r8d,ebx
//   +0x53 mov eax,[rip+GDistanceFieldBlockAllocatorSizeInBricks]; +0x59 imul eax,[rcx+3D0h] (the allocator's MaxSize)
//   +0x60 mov rcx,[rip+BrickAtlasMaxSizeZ cvar data]; imul r8d,ebx; dec eax; add eax,r8d; cdq; idiv r8d; cmp eax,[rcx+4]; jg
//   +0x79 power of two (bsr); +0x97 else round up to 4
//   +0xA6 mov rcx,[rip+ResizeAtlasEveryFrame cvar data]; +0xAD lea rsi,[r14+3F8h] (DistanceFieldBrickVolumeTexture)
//   +0xC4 mov edi,[rcx+4]; mov rcx,[rsi]; test; je; mov rax,[rcx]; call [rax+10h] (GetDesc); lea ecx,[rbx*8]
//   +0xD9 movzx edx,word [rax+34h] (Depth); cmp [rax+2Ch],ecx (X); jne; cmp [rax+30h],ecx (Y); jne; lea eax,[r12*8]; cmp; jne
//   +0xF3 test edi,edi; jne; xor r8d,r8d; mov rdx,rsi; mov rcx,r13; +0x100 call RegisterExternalTexture; +0x105 jmp return
constexpr char kHead[] =
    "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 4C 8B EA 4C 89 44 24 ?? 4C 8B F1 48 89 4C 24 ?? 41 BF 01 00 00 00 8B 58 04 44 8B C3 "
    "8B 05 ?? ?? ?? ?? 0F AF 81 D0 03 00 00 48 8B 0D ?? ?? ?? ?? 44 0F AF C3 FF C8 41 03 C0 99 41 F7 F8 3B 41 04 7F 1E "
    "85 C0 41 8B CF 45 8B E7 0F 45 C8 FF C9 48 8D 04 4D 01 00 00 00 48 0F BD C8 41 D3 E4 EB 0F "
    "83 C0 03 99 83 E2 03 44 8D 24 02 41 83 E4 FC "
    "48 8B 0D ?? ?? ?? ?? 49 8D B6 F8 03 00 00 89 5C 24 ?? 89 5C 24 ?? 48 89 74 24 ?? 8B 79 04 48 8B 0E 48 85 C9 74 3E "
    "48 8B 01 FF 50 10 8D 0C DD 00 00 00 00 0F B7 50 34 39 48 2C 75 28 39 48 30 75 23 "
    "42 8D 04 E5 00 00 00 00 3B D0 75 17 85 FF 75 13 45 33 C0 48 8B D6 49 8B CD E8 ?? ?? ?? ?? E9";
constexpr size_t kXyAt = 0x30, kBsAt = 0x53, kMaxZAt = 0x60, kEveryAt = 0xA6, kRegisterAt = 0x100;
// Where the function stores BrickTextureDimensionsInBricks with the new texture (exe+0x1DA13A5, head +0x4B5):
//   movsd [r14+400h],xmm0 (X, Y); mov rcx,r13; mov [r14+408h],r12d (Z); call ConvertToExternalTexture
constexpr char kStore[] = "F2 41 0F 11 86 00 04 00 00 49 8B CD 45 89 A6 08 04 00 00 E8";
constexpr size_t kStoreFrom = 0x105, kStoreTo = 0x700;
constexpr uint8_t kPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x20};
// FDistanceFieldSceneData fields (from the code above)
constexpr size_t kMaxSize = 0x3D0, kTexture = 0x3F8, kDimX = 0x400, kDimY = 0x404, kDimZ = 0x408;
// FPooledRenderTargetDesc (GetDesc's result): Extent.X, Extent.Y, Depth (uint16)
constexpr size_t kDescX = 0x2C, kDescY = 0x30, kDescDepth = 0x34;
constexpr int kBrickSize = 8;

using ResizeFn = void* (*)(uint8_t* self, void* graphBuilder, void* shaderMap);
using RegisterFn = void* (*)(void* graphBuilder, void* pooledTexture, uint32_t flags);
using GetDescFn = const uint8_t* (*)(void* pooled);

struct Sites {
    uint8_t* fn = nullptr;
    uint8_t* const* xyCvar = nullptr;     // globals holding TConsoleVariableData<int32>* (+4 = the render thread's value)
    uint8_t* const* maxZCvar = nullptr;
    uint8_t* const* everyCvar = nullptr;
    const int32_t* blockBricks = nullptr;  // GDistanceFieldBlockAllocatorSizeInBricks
    RegisterFn registerTexture = nullptr;
};
Sites g;
ResizeFn g_original = nullptr;  // the trampoline (or a fake in the self-test)
bool g_installed = false, g_searched = false;
std::atomic<int> g_mode{0};

std::atomic<uint64_t> s_calls{0}, s_resizes{0}, s_grows{0}, s_shrinks{0}, s_keptFrames{0}, s_resizeUs{0}, s_maxUs{0};
std::atomic<int32_t> s_lastKeptZ{0}, s_lastKeptGameZ{0};
std::atomic<int32_t> s_peakZ{0};
uint64_t s_logged = 0;

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMicroseconds(static_cast<uint64_t>(c.QuadPart), freq);
}

int32_t CvarInt(uint8_t* const* slot, int32_t fallback)
{
    if (!slot || !*slot) return fallback;
    int32_t v;
    memcpy(&v, *slot + 4, 4);
    return v;
}

bool IsPow2(int32_t v) { return v > 0 && (v & (v - 1)) == 0; }

void LogLine(const char* what, int32_t z0, int32_t z1, int32_t xy, uint64_t t0, uint64_t us)
{
    uint64_t used = 0, budget = 0;
    const bool vram = LatestGameVram(&used, &budget);
    const double mbPerZ = static_cast<double>(xy) * xy * kBrickSize * kBrickSize * kBrickSize / 1048576.0;
    char frame[32] = "";
    Log("[dfatlas] %sq %llu Z %d -> %d (XY %d, %.0f MB, was %.0f) call %.2f ms, vram %llu / %llu MB%s, loading %d, %s", frame,
        static_cast<unsigned long long>(t0 / 1000), z0, z1, xy, z1 * mbPerZ, z0 * mbPerZ, us / 1000.0,
        static_cast<unsigned long long>(used >> 20), static_cast<unsigned long long>(budget >> 20), vram ? "" : " (no sample)",
        LoadingScreenNow(), what);
}

}  // namespace

int32_t DfAtlasGameZ(int32_t maxSize, int32_t blockBricks, int32_t xy, int32_t maxZ)
{
    // the function's arithmetic (32-bit, wrapping as the game's imul/add; signed idiv)
    const int32_t xy2 = static_cast<int32_t>(static_cast<uint32_t>(xy) * static_cast<uint32_t>(xy));
    if (xy2 <= 0) return -1;
    const int32_t n = static_cast<int32_t>(static_cast<uint32_t>(blockBricks) * static_cast<uint32_t>(maxSize) - 1u + static_cast<uint32_t>(xy2));
    const int32_t d = n / xy2;
    if (d <= maxZ) {
        const uint32_t c = static_cast<uint32_t>(d != 0 ? d : 1) - 1u;
        const uint64_t v = 2ull * c + 1ull;
        unsigned long index = 0;
        _BitScanReverse64(&index, v);
        return static_cast<int32_t>(1u << (index & 31));
    }
    const int32_t t = static_cast<int32_t>(static_cast<uint32_t>(d) + 3u);
    return static_cast<int32_t>((static_cast<uint32_t>(t) + ((static_cast<uint32_t>(t >> 31)) & 3u)) & ~3u);
}

bool DfAtlasKeepDecide(int32_t gameZ, int32_t curZ, int32_t dimX, int32_t dimY, int32_t xy, int32_t maxZ, int32_t descX,
                       int32_t descY, int32_t descDepth, int32_t everyFrame)
{
    if (everyFrame != 0 || xy <= 0 || dimX != xy || dimY != xy) return false;
    if (descX != xy * kBrickSize || descY != xy * kBrickSize || descDepth != curZ * kBrickSize) return false;
    if (!IsPow2(curZ) || curZ > maxZ) return false;
    return gameZ >= 1 && gameZ < curZ;
}

namespace {

void* Hook(uint8_t* self, void* graphBuilder, void* shaderMap)
{
    s_calls.fetch_add(1, std::memory_order_relaxed);
    void* const tex = self ? *reinterpret_cast<void**>(self + kTexture) : nullptr;
    int32_t z0 = 0, dimX = 0, dimY = 0;
    if (self) {
        memcpy(&z0, self + kDimZ, 4);
        memcpy(&dimX, self + kDimX, 4);
        memcpy(&dimY, self + kDimY, 4);
    }
    const int32_t xy = CvarInt(g.xyCvar, 0);
    if (tex && g_mode.load(std::memory_order_relaxed) == 1 && g.registerTexture && g.blockBricks) {
        int32_t maxSize;
        memcpy(&maxSize, self + kMaxSize, 4);
        const int32_t maxZ = CvarInt(g.maxZCvar, 0);
        const int32_t gameZ = DfAtlasGameZ(maxSize, *g.blockBricks, xy, maxZ);
        const uint8_t* const desc = (*reinterpret_cast<GetDescFn* const*>(tex))[2](tex);
        int32_t descX = 0, descY = 0;
        uint16_t depth = 0;
        if (desc) {
            memcpy(&descX, desc + kDescX, 4);
            memcpy(&descY, desc + kDescY, 4);
            memcpy(&depth, desc + kDescDepth, 2);
        }
        if (desc && DfAtlasKeepDecide(gameZ, z0, dimX, dimY, xy, maxZ, descX, descY, depth, CvarInt(g.everyCvar, 1))) {
            s_keptFrames.fetch_add(1, std::memory_order_relaxed);
            if (s_lastKeptZ.exchange(z0) != z0 || s_lastKeptGameZ.exchange(gameZ) != gameZ)
                LogLine("kept (the game would make it smaller)", z0, gameZ, xy, NowUs(), 0);
            return g.registerTexture(graphBuilder, self + kTexture, 0);
        }
    }
    const uint64_t t0 = NowUs();
    void* const r = g_original(self, graphBuilder, shaderMap);
    const uint64_t us = NowUs() - t0;
    void* const tex1 = self ? *reinterpret_cast<void**>(self + kTexture) : nullptr;
    if (tex1 != tex) {
        int32_t z1 = 0;
        if (self) memcpy(&z1, self + kDimZ, 4);
        s_resizes.fetch_add(1, std::memory_order_relaxed);
        if (tex && z1 > z0) s_grows.fetch_add(1, std::memory_order_relaxed);
        if (tex && z1 < z0) s_shrinks.fetch_add(1, std::memory_order_relaxed);
        s_resizeUs.fetch_add(us, std::memory_order_relaxed);
        uint64_t cur = s_maxUs.load(std::memory_order_relaxed);
        while (us > cur && !s_maxUs.compare_exchange_weak(cur, us)) {
        }
        int32_t pk = s_peakZ.load(std::memory_order_relaxed);
        while (z1 > pk && !s_peakZ.compare_exchange_weak(pk, z1)) {
        }
        s_lastKeptZ.store(0, std::memory_order_relaxed);
        LogLine(!tex ? "first" : z1 > z0 ? "grow" : z1 < z0 ? "shrink" : "same size", z0, z1, xy, t0, us);
    }
    return r;
}

bool Fail(const char* why)
{
    Log("[dfatlas] DfAtlasKeep not installed: %s", why);
    return false;
}

bool Detour(uint8_t* fn, void* hook, uint8_t* region, bool beforeGameCode)
{
    if (memcmp(fn, kPrologue, sizeof kPrologue) != 0) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + sizeof kPrologue;
    memcpy(region, kPrologue, sizeof kPrologue);
    memcpy(region + 5, jmpAbs, sizeof jmpAbs);
    memcpy(region + 11, &resume, sizeof resume);
    memcpy(region + 32, jmpAbs, sizeof jmpAbs);
    memcpy(region + 38, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = (region + 32) - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t jmp[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    const uintptr_t at = reinterpret_cast<uintptr_t>(fn);
    if ((at & 7) + sizeof jmp <= 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(at & ~uintptr_t{7});
        if (!CodeSiteProtect(const_cast<LONG64*>(q), 8, &old)) return false;
        const LONG64 was = *q;
        LONG64 now = was;
        memcpy(reinterpret_cast<uint8_t*>(&now) + (at & 7), jmp, sizeof jmp);
        const bool ok = InterlockedCompareExchange64(q, now, was) == was;
        VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
        if (ok) RecordOwnWrite(fn, sizeof jmp);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, std::span<const uint8_t>(jmp, sizeof jmp));
}

}  // namespace

// The function by its head pattern (exactly once), the store of the atlas dimensions after it, and the globals it reads
// (each in the game's data section) and the RegisterExternalTexture it calls (in its code). nullptr fn = not found.
bool DfAtlasKeepFind(const GameModule& game, DfAtlasSites* out)
{
    Sites s;
    const auto hits = FindAll(game.text, ParsePattern(kHead));
    if (hits.size() != 1) return false;
    s.fn = hits[0];
    const Pattern store = ParsePattern(kStore);
    int stores = 0;
    for (uint8_t* p = s.fn + kStoreFrom; p < s.fn + kStoreTo && p + store.size() <= game.text.end; ++p)
        if (MatchesAt(p, store)) ++stores;
    if (stores != 1) return false;
    s.xyCvar = reinterpret_cast<uint8_t* const*>(RipTarget(s.fn + kXyAt, 3, 7));
    s.blockBricks = reinterpret_cast<const int32_t*>(RipTarget(s.fn + kBsAt, 2, 6));
    s.maxZCvar = reinterpret_cast<uint8_t* const*>(RipTarget(s.fn + kMaxZAt, 3, 7));
    s.everyCvar = reinterpret_cast<uint8_t* const*>(RipTarget(s.fn + kEveryAt, 3, 7));
    uint8_t* const reg = RipTarget(s.fn + kRegisterAt, 1, 5);
    if (!game.data.Contains(s.xyCvar) || !game.data.Contains(s.blockBricks) || !game.data.Contains(s.maxZCvar) ||
        !game.data.Contains(s.everyCvar) || !game.text.Contains(reg))
        return false;
    s.registerTexture = reinterpret_cast<RegisterFn>(reg);
    if (out) {
        out->fn = s.fn;
        out->registerTexture = reg;
        out->xyCvar = reinterpret_cast<const void*>(s.xyCvar);
        out->blockBricks = reinterpret_cast<const void*>(s.blockBricks);
        out->maxZCvar = reinterpret_cast<const void*>(s.maxZCvar);
        out->everyCvar = reinterpret_cast<const void*>(s.everyCvar);
    }
    g = s;
    return true;
}

bool InstallDfAtlasKeep(bool beforeGameCode)
{
    if (g_installed) return true;
    if (g_searched) return false;
    g_searched = true;
    const GameModule& game = Game();
    if (!DfAtlasKeepFind(game, nullptr))
        return Fail("FDistanceFieldSceneData::ResizeBrickAtlasIfNeeded not found (byte pattern; another game version?)");
    uint8_t* const region = AllocNearGame(64);
    if (!region) return Fail("no free memory near the game's code");
    g_original = reinterpret_cast<ResizeFn>(region);
    if (!Detour(g.fn, reinterpret_cast<void*>(&Hook), region, beforeGameCode))
        return Fail("ResizeBrickAtlasIfNeeded's first instruction changed, another mod hooks it, or the jump could not be placed");
    g_installed = true;
    Log("[dfatlas] installed: FDistanceFieldSceneData::ResizeBrickAtlasIfNeeded exe+0x%llX detoured (RegisterExternalTexture exe+0x%llX; "
        "XY %d, BrickAtlasMaxSizeZ %d, block %d bricks)",
        static_cast<unsigned long long>(g.fn - game.base), static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g.registerTexture) - game.base),
        CvarInt(g.xyCvar, -1), CvarInt(g.maxZCvar, -1), g.blockBricks ? *g.blockBricks : -1);
    return true;
}

void SetDfAtlasKeep(int mode)
{
    if (!g_installed) return;
    if (mode < 0 || mode > 2) mode = 1;
    if (g_mode.exchange(mode) != mode)
        Log(mode == 1 ? "[dfatlas] DfAtlasKeep 1: the distance-field brick atlas is not re-created smaller (grows as before)"
                      : "[dfatlas] DfAtlasKeep %d: the brick atlas is re-created as in the game (re-creations still logged)", mode);
}

void LogDfAtlasKeepStats()
{
    if (!g_installed) return;
    const uint64_t resizes = s_resizes.load(), kept = s_keptFrames.load();
    const uint64_t sum = resizes * 0x100000000ull + kept;
    if (sum == s_logged) return;
    s_logged = sum;
    Log("[dfatlas] stats: %llu calls, %llu re-creations (%llu larger, %llu smaller; %.1f ms in all, longest %.1f ms), kept in %llu frames; "
        "largest Z %d bricks",
        static_cast<unsigned long long>(s_calls.load()), static_cast<unsigned long long>(resizes),
        static_cast<unsigned long long>(s_grows.load()), static_cast<unsigned long long>(s_shrinks.load()), s_resizeUs.load() / 1000.0,
        s_maxUs.load() / 1000.0, static_cast<unsigned long long>(kept), s_peakZ.load());
}

// Self-test: the hook's decision path with fakes (never on a live detour). 0 = pass, else failed-check bits.
namespace {
struct FakePooled {
    void* const* vtable;
    uint8_t desc[0x40];
};
int g_fakeOriginal = 0, g_fakeRegister = 0;
void* g_fakeNewTex = nullptr;
int32_t g_fakeNewZ = 0;
const uint8_t* FakeGetDesc(void* pooled) { return static_cast<FakePooled*>(pooled)->desc; }
void* FakeRegister(void*, void*, uint32_t) { ++g_fakeRegister; return &g_fakeRegister; }
void* FakeResize(uint8_t* self, void*, void*)
{
    ++g_fakeOriginal;
    if (g_fakeNewTex) {
        memcpy(self + kTexture, &g_fakeNewTex, sizeof g_fakeNewTex);
        memcpy(self + kDimZ, &g_fakeNewZ, 4);
    }
    return &g_fakeOriginal;
}
}  // namespace

int DfAtlasKeepTestLogic()
{
    if (g_installed) return 0x80000000;
    int fail = 0;
    static int32_t xyData[2] = {128, 128}, maxZData[2] = {32, 32}, everyData[2] = {0, 0};
    static int32_t blockBricks = 16;
    static uint8_t* xyPtr = reinterpret_cast<uint8_t*>(xyData);
    static uint8_t* maxZPtr = reinterpret_cast<uint8_t*>(maxZData);
    static uint8_t* everyPtr = reinterpret_cast<uint8_t*>(everyData);
    static void* vt[4] = {nullptr, nullptr, reinterpret_cast<void*>(&FakeGetDesc), nullptr};
    const Sites saved = g;
    const ResizeFn savedOrig = g_original;
    const int savedMode = g_mode.load();
    g.xyCvar = &xyPtr;
    g.maxZCvar = &maxZPtr;
    g.everyCvar = &everyPtr;
    g.blockBricks = &blockBricks;
    g.registerTexture = &FakeRegister;
    g_original = &FakeResize;
    static FakePooled tex{vt, {}}, tex2{vt, {}};
    alignas(16) static uint8_t self[0x410];
    auto setup = [&](int32_t curZ, int32_t gameZBricks, int32_t descDepth) {
        memset(self, 0, sizeof self);
        void* t = &tex;
        memcpy(self + kTexture, &t, sizeof t);
        const int32_t xy = 128;
        memcpy(self + kDimX, &xy, 4);
        memcpy(self + kDimY, &xy, 4);
        memcpy(self + kDimZ, &curZ, 4);
        // MaxSize in blocks of 16 bricks: Z bricks x 128^2 / 16
        const int32_t maxSize = gameZBricks * 128 * 128 / 16;
        memcpy(self + kMaxSize, &maxSize, 4);
        const int32_t ext = 128 * kBrickSize;
        memcpy(tex.desc + kDescX, &ext, 4);
        memcpy(tex.desc + kDescY, &ext, 4);
        const uint16_t d = static_cast<uint16_t>(descDepth);
        memcpy(tex.desc + kDescDepth, &d, 2);
        g_fakeOriginal = g_fakeRegister = 0;
        g_fakeNewTex = nullptr;
    };
    auto run = [&]() { Hook(self, nullptr, nullptr); };
    // 1: keep on, atlas 16, the game wants 8: kept (the no-resize path), the game's function not called
    g_mode = 1;
    setup(16, 8, 16 * 8);
    run();
    if (g_fakeOriginal != 0 || g_fakeRegister != 1) fail |= 1;
    // 2: keep off: the game's function
    g_mode = 0;
    setup(16, 8, 16 * 8);
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0) fail |= 2;
    // 3: growth (the game wants 32 > 16): the game's function
    g_mode = 1;
    setup(16, 32, 16 * 8);
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0) fail |= 4;
    // 4: same size: the game's function (its own no-resize path)
    setup(16, 16, 16 * 8);
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0) fail |= 8;
    // 5: atlas above BrickAtlasMaxSizeZ (36 > 32, not a power of two): shrinks as in the game
    setup(36, 8, 36 * 8);
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0) fail |= 0x10;
    // 6: the texture's depth does not match the recorded Z: the game's function
    setup(16, 8, 8 * 8);
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0) fail |= 0x20;
    // 7: ResizeAtlasEveryFrame on: the game's function
    everyData[1] = 1;
    setup(16, 8, 16 * 8);
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0) fail |= 0x40;
    everyData[1] = 0;
    // 8: no texture yet: the game's function; a re-creation it makes is seen (texture pointer and Z change)
    setup(16, 8, 16 * 8);
    memset(self + kTexture, 0, 8);
    g_fakeNewTex = &tex2;
    g_fakeNewZ = 8;
    const uint64_t resizesBefore = s_resizes.load();
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0 || s_resizes.load() != resizesBefore + 1) fail |= 0x80;
    // 9: a power-of-two atlas AT the limit (32) kept for a smaller need (16)
    setup(32, 16, 32 * 8);
    run();
    if (g_fakeOriginal != 0 || g_fakeRegister != 1) fail |= 0x100;
    // 10: mode 2 (installed, the game's way)
    g_mode = 2;
    setup(16, 8, 16 * 8);
    run();
    if (g_fakeOriginal != 1 || g_fakeRegister != 0) fail |= 0x200;
    g = saved;
    g_original = savedOrig;
    g_mode = savedMode;
    s_lastKeptZ = 0;
    s_lastKeptGameZ = 0;
    return fail;
}

}  // namespace opf
