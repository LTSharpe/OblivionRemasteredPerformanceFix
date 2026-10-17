// ScreenshotParallel (working name): the rest of the menu screenshot's cost is copies into fresh 33 MB arrays.
//
// Opening the player menu (inventory, map, ...) takes a screenshot of the game view (MenuScreenshotFilter and ScreenshotReadbackConvert are about its two
// pixel loops). What is left at 4K (measured 2026-10-15) is mostly whole-image copies: on the render thread
// the readback's row copy out of the staging buffer (ReadSurfaceDataNoMSAARaw) and the conversion to FColor (ScreenshotReadbackConvert);
// on the game thread Altar's copy of the delegate's bitmap (a TArray<FColor> assignment), ImageResize's zero fill of
// its destination and its filter (MenuScreenshotFilter), and CreateTexture2DFromImage's two copies (CopyImage into the mip's bulk
// data, then FBulkData::GetCopy when the texture resource is created). Each 3840x2160 image is 33 MB, more than
// FMallocBinned2 caches (BINNED2_MAX_CACHED_OS_FREES_BYTE_LIMIT / 4 = 16 MB), so every one is fresh memory from the OS
// and each copy pays ~8,100 first-touch page faults on one thread: ~5-9 ms per copy where a copy into warm memory
// takes ~2.5 ms.
//
// The fix spreads these copies over a few helper threads (the caller takes part and waits for all of them before it
// returns, so nothing runs later than before), which spreads the page faults too. The bytes are the same: a memcpy
// cut into pieces is a memcpy, the row copy copies the same rows, and MenuScreenshotFilter and ScreenshotReadbackConvert compute each output row from the
// same input rows. ImageResize's zero fill (TArray version: DstData.Empty(N); DstData.AddZeroed(N); then the view
// version) is skipped: the view version (UE 5.3 ImageUtils.cpp) writes every one of the N = DstWidth * DstHeight pixels
// for any sizes, so the zeros are never read. It is kept when the source and destination are the same array (Empty
// would have destroyed the source) or the fill does not start at the array's data.
//
// Patched at start-up (one call each, byte patterns, targets checked against the exe's memcpy / memset import thunks):
//   CopyImage's same-format memcpy, the five memcpy calls in FBulkData::GetCopy, the memcpy in the TArray<4-byte>
//   assignment Altar's screenshot delegate uses, the row loop in ReadSurfaceDataNoMSAARaw (a jump to a stub that
//   calls RowCopy and returns after the loop), the memset in ImageResize's TArray version.
// Only copies of at least 4 MB use the helpers (everything smaller goes straight to memcpy), one job at a time (a
// second caller meanwhile copies on its own thread). The ini switches it live: 0 = the game's behaviour everywhere.
#include "common.h"

#include <emmintrin.h>

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr size_t kMinBytes = 4u << 20;  // copies below this go straight to memcpy
constexpr size_t kChunk = 1u << 20;     // about this much per part
constexpr int kMaxParts = 32;

// ---- helper pool: one job at a time; the caller works on it too, so a helper that wakes late costs nothing ----
using PartFn = void (*)(const void* ctx, int part);
std::atomic<bool> g_poolBusy{false};
std::atomic<uint64_t> g_claim{0};  // generation (bits 40-63) | parts (bits 20-39) | next part (bits 0-19)
std::atomic<int> g_done{0};
WaitGate g_doneGate;  // the caller waiting for parts a helper is copying
PartFn g_fn = nullptr;
const void* g_ctx = nullptr;
HANDLE g_wake = nullptr;
int g_helpers = 0;
std::atomic<uint64_t> g_jobs{0}, g_jobsSerial{0}, g_helperParts{0};

bool ClaimAndRun()
{
    for (;;) {
        uint64_t c = g_claim.load(std::memory_order_acquire);
        const uint64_t parts = (c >> 20) & 0xFFFFF, next = c & 0xFFFFF;
        if (next >= parts) return false;
        if (!g_claim.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel)) continue;
        // claimed: the job of this generation cannot finish (and its fn / ctx cannot change) before this part is done
        g_fn(g_ctx, static_cast<int>(next));
        g_done.fetch_add(1, std::memory_order_release);
        WakeWord(g_done, g_doneGate);
        return true;
    }
}

DWORD WINAPI HelperMain(void*)
{
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        while (ClaimAndRun()) g_helperParts.fetch_add(1, std::memory_order_relaxed);
    }
}

void RunParts(int parts, PartFn fn, const void* ctx)
{
    bool expected = false;
    if (parts <= 1 || !g_helpers || !g_poolBusy.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        g_jobsSerial.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < parts; ++i) fn(ctx, i);
        return;
    }
    g_fn = fn;
    g_ctx = ctx;
    g_done.store(0, std::memory_order_relaxed);
    const uint64_t gen = (g_claim.load(std::memory_order_relaxed) >> 40) + 1;
    g_claim.store((gen << 40) | (static_cast<uint64_t>(parts) << 20), std::memory_order_release);
    ReleaseSemaphore(g_wake, g_helpers, nullptr);
    while (ClaimAndRun()) {
    }
    WaitWord(g_done, g_doneGate, [parts](int d) { return d >= parts; });
    g_jobs.fetch_add(1, std::memory_order_relaxed);
    g_poolBusy.store(false, std::memory_order_release);
}

int PartsFor(size_t bytes)
{
    const size_t p = bytes / kChunk;
    return p < 2 ? 2 : p > kMaxParts ? kMaxParts : static_cast<int>(p);
}

bool StartPool()
{
    if (g_helpers) return true;
    int n = static_cast<int>(UsableLogicalProcessors()) / 4;  // 16 hardware threads -> 4 helpers (+ the caller)
    n = n < 1 ? 1 : n > 4 ? 4 : n;
    g_wake = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    if (!g_wake) return false;
    int started = 0;
    for (int i = 0; i < n; ++i) {
        HANDLE h = CreateThread(nullptr, 64 * 1024, HelperMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!h) break;
        wchar_t name[32];
        swprintf_s(name, L"OblivionPerfFix copy %d", i + 1);
        DescribeThread(h, name);
        CloseHandle(h);
        ++started;
    }
    g_helpers = started;
    return started > 0;
}

// ---- state shared with the stubs ----
struct alignas(16) StubData {
    uint8_t on;          // read by the memset stub
    uint8_t pad[7];
    uint64_t zeroSkips;  // incremented by the memset stub
};
StubData g_stub = {};
std::atomic<bool> g_on{false};
bool g_installed = false;
std::atomic<uint64_t> g_copies{0}, g_copyBytes{0}, g_copyTicks{0}, g_rowCopies{0}, g_rowBytes{0}, g_rowTicks{0};
std::atomic<uint64_t> g_bandJobs{0}, g_bandTicks{0};
uint64_t g_logged = 0;
int g_sites = 0;
uint8_t* g_rowSite = nullptr;
uint8_t* g_memsetSite = nullptr;
uint8_t* g_memsetStub = nullptr;
uint8_t* g_rowStub = nullptr;

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

struct CopyJob {
    uint8_t* dst;
    const uint8_t* src;
    size_t bytes, chunk;
};

void CopyPart(const void* ctx, int part)
{
    const auto* j = static_cast<const CopyJob*>(ctx);
    const size_t a = j->chunk * static_cast<size_t>(part);
    if (a >= j->bytes) return;
    const size_t n = j->bytes - a < j->chunk ? j->bytes - a : j->chunk;
    memcpy(j->dst + a, j->src + a, n);
}

struct RowJob {
    uint8_t* dst;
    const uint8_t* src;
    uint64_t bytesPerLine, rows, rowsPerPart;
    uint32_t pitch;
};

void RowPart(const void* ctx, int part)
{
    const auto* j = static_cast<const RowJob*>(ctx);
    const uint64_t y0 = j->rowsPerPart * static_cast<uint64_t>(part);
    const uint64_t y1 = y0 + j->rowsPerPart < j->rows ? y0 + j->rowsPerPart : j->rows;
    uint8_t* d = j->dst + y0 * j->bytesPerLine;
    const uint8_t* s = j->src + y0 * j->pitch;
    for (uint64_t y = y0; y < y1; ++y, d += j->bytesPerLine, s += j->pitch) memcpy(d, s, j->bytesPerLine);
}

}  // namespace

// memcpy with the same result (the game's VCRUNTIME memcpy is a memmove: overlapping ranges go to memmove).
void* BigMemcpyHook(void* dst, const void* src, size_t bytes)
{
    const uintptr_t d = reinterpret_cast<uintptr_t>(dst), s = reinterpret_cast<uintptr_t>(src);
    if (bytes < kMinBytes || !g_on.load(std::memory_order_relaxed) || (d < s + bytes && s < d + bytes))
        return memmove(dst, src, bytes);
    const uint64_t t0 = Ticks();
    const int parts = PartsFor(bytes);
    CopyJob job{static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(src), bytes, ((bytes + parts - 1) / parts + 4095) & ~size_t{4095}};
    RunParts(parts, CopyPart, &job);
    g_copyTicks.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    g_copyBytes.fetch_add(bytes, std::memory_order_relaxed);
    g_copies.fetch_add(1, std::memory_order_relaxed);
    return dst;
}

// ReadSurfaceDataNoMSAARaw's row loop: for (Y < SizeY) { memcpy(Dest, Src, BytesPerLine); Dest += BytesPerLine; Src += Pitch; }
void RowCopyHook(uint8_t* dst, const uint8_t* src, uint64_t bytesPerLine, uint32_t pitch, uint64_t rows)
{
    const uint64_t total = bytesPerLine * rows;
    if (total < kMinBytes || rows < 2 || !g_on.load(std::memory_order_relaxed)) {
        for (uint64_t y = 0; y < rows; ++y, dst += bytesPerLine, src += pitch) memcpy(dst, src, bytesPerLine);
        return;
    }
    const uint64_t t0 = Ticks();
    int parts = PartsFor(total);
    if (static_cast<uint64_t>(parts) > rows) parts = static_cast<int>(rows);
    RowJob job{dst, src, bytesPerLine, rows, (rows + parts - 1) / parts, pitch};
    RunParts(parts, RowPart, &job);
    g_rowTicks.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    g_rowBytes.fetch_add(total, std::memory_order_relaxed);
    g_rowCopies.fetch_add(1, std::memory_order_relaxed);
}

// For MenuScreenshotFilter and ScreenshotReadbackConvert: run fn(ctx, y0, y1) over row bands of an image on the helpers when the fix is on and the image is
// big enough; otherwise (or with the fix off) fn(ctx, 0, rows) on the caller. Returns true when the helpers were used.
bool ScreenshotParallelRows(uint32_t rows, size_t bytes, void (*fn)(const void* ctx, uint32_t y0, uint32_t y1), const void* ctx)
{
    if (!g_on.load(std::memory_order_relaxed) || !g_helpers || bytes < kMinBytes || rows < 2) {
        fn(ctx, 0, rows);
        return false;
    }
    struct Band {
        void (*fn)(const void*, uint32_t, uint32_t);
        const void* ctx;
        uint32_t rows, per;
    };
    int parts = PartsFor(bytes);
    if (static_cast<uint32_t>(parts) > rows) parts = static_cast<int>(rows);
    const Band band{fn, ctx, rows, (rows + parts - 1) / static_cast<uint32_t>(parts)};
    const uint64_t t0 = Ticks();
    RunParts(parts, [](const void* c, int part) {
        const auto* b = static_cast<const Band*>(c);
        const uint32_t y0 = b->per * static_cast<uint32_t>(part);
        if (y0 >= b->rows) return;
        b->fn(b->ctx, y0, y0 + b->per < b->rows ? y0 + b->per : b->rows);
    }, &band);
    g_bandTicks.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    g_bandJobs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace {

bool Fail(const char* why)
{
    Log("[shotpar] %s: fix not installed", why);
    return false;
}

// Is the call at `call` (E8 rel32) to a "jmp [rip+disp]" thunk through the exe's import slot of VCRUNTIME140!name?
bool CallsImport(const uint8_t* call, const char* name)
{
    const GameModule& game = Game();
    if (call[0] != 0xE8) return false;
    const uint8_t* thunk = RipTarget(call, 1, 5);
    if (!game.text.Contains(thunk) || thunk[0] != 0xFF || thunk[1] != 0x25) return false;
    void** slot = FindImportSlot(game.base, "VCRUNTIME140.dll", name);
    return slot && reinterpret_cast<const uint8_t*>(RipTarget(thunk, 2, 6)) == reinterpret_cast<const uint8_t*>(slot);
}

uint8_t* FindOne(const char* pattern, const char* what)
{
    const auto hits = FindAll(Game().text, ParsePattern(pattern));
    if (hits.size() != 1) {
        Log("[shotpar] %s not found (%zu matches)", what, hits.size());
        return nullptr;
    }
    return hits[0];
}

bool Retarget(uint8_t* call, const uint8_t* to)
{
    const int64_t rel = to - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

void Put64(uint8_t* at, const void* p) { memcpy(at, &p, 8); }

// CopyImage (same format): ... imul r8,rax; movsxd rax,[r15]; imul r8,rax; call memcpy; jmp
constexpr char kCopyImage[] = "49 63 47 04 4C 0F AF C0 49 63 07 4C 0F AF C0 E8 ?? ?? ?? ?? E9";
constexpr size_t kCopyImageCall = 15;
// FBulkData::GetCopy: three places, five memcpy calls
constexpr char kGetCopyA[] = "48 8B 58 10 48 85 DB 74 04 48 8B 5B 08 4C 8B C6 48 8B D3 49 8B CA E8 ?? ?? ?? ?? 40 84 ED";
constexpr size_t kGetCopyACall = 22;
constexpr char kGetCopyB[] = "48 85 D2 74 06 48 8B 52 08 EB 03 48 8B D3 4C 8B C6 48 8B C8 E8 ?? ?? ?? ?? 48 8B 37";
constexpr size_t kGetCopyBCall = 20;
constexpr char kGetCopyC[] =
    "4C 8B C8 8B 4F 14 C1 E9 1E F6 C1 01 75 16 48 8B 1F 4C 8B C6 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? "
    "48 8B 07 48 85 C0 74 28 48 8B 18 48 85 DB 75 22 48 8B 58 10 48 85 DB 74 17 48 8B 5B 08 4C 8B C6 48 8B D3 49 8B C9 "
    "E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 33 DB 4C 8B C6 48 8B D3 49 8B C9 E8";
constexpr size_t kGetCopyCCalls[3] = {26, 74, 95};
// Altar's screenshot delegate (exe+0x48C2AF0): stores width / height, then Bitmap = the delegate's array (TArray<4-byte>
// operator=, CopyToEmpty: ResizeTo, then memcpy at +0x5D)
constexpr char kDelegate[] = "49 8B D6 89 AF 8C 03 00 00 48 8D 8F 98 03 00 00 89 B7 90 03 00 00 E8";
constexpr size_t kDelegateCall = 22;
constexpr char kAssign[] =
    "40 53 48 83 EC 20 48 8B D9 48 3B CA 74 5E 44 8B 41 0C 48 89 74 24 30 48 8B 32 48 89 7C 24 38 48 63 7A 08 89 79 08 "
    "85 FF 75 1B 45 85 C0 75 16 48 8B 74 24 30 48 8B C1 89 79 0C 48 8B 7C 24 38 48 83 C4 20 5B C3 8B D7 E8 ?? ?? ?? ?? "
    "85 FF 74 12 48 8B 0B 4C 8B C7 49 C1 E0 02 48 8B D6 E8 ?? ?? ?? ??";
constexpr size_t kAssignCall = 0x5D;
// ReadSurfaceDataNoMSAARaw: rsi = OutData, rdi = mapped source, r15 = BytesPerLine, r14d = pitch, r13d = SizeY
//   test r13d,r13d; je done; mov r12d,r14d; mov r14d,r13d;
//   loop: mov r8,r15; mov rdx,rdi; mov rcx,rsi; call memcpy; add rsi,r15; add rdi,r12; sub r14,1; jne loop; done:
constexpr char kRowLoop[] =
    "45 85 ED 74 20 45 8B E6 45 8B F5 4D 8B C7 48 8B D7 48 8B CE E8 ?? ?? ?? ?? 49 03 F7 49 03 FC 49 83 EE 01 75 E6";
constexpr size_t kRowLoopCall = 20, kRowLoopLen = 0x25;
// ImageResize (TArray version): rdi = &DstData, r15 = &SrcData, ebx = N, rsi = DstData.Num after Empty (0):
//   mov rax,[rdi]; xor edx,edx; movsxd r8,ebx; shl r8,2; lea rcx,[rax+rsi*4]; call memset; then the view arguments
constexpr char kZeroFill[] =
    "48 8B 07 33 D2 4C 63 C3 49 C1 E0 02 48 8D 0C B0 E8 ?? ?? ?? ?? 48 8B 07 4C 8D 44 24 50 48 89 44 24 40 44 8B CD "
    "8B 47 08 41 8B D4 89 44 24 48 41 8B CD 49 8B 07 48 89 44 24 50 41 8B 47 08 89 44 24 58 "
    "0F B6 84 24 C8 00 00 00 88 44 24 38 0F B6 84 24 C0 00 00 00 88 44 24 30 48 8D 44 24 40 48 89 44 24 28 "
    "44 89 74 24 20 E8";
constexpr size_t kZeroFillCall = 16;

}  // namespace

bool ApplyScreenshotParallelFix(bool beforeGameCode)
{
    if (g_installed) return true;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    // find everything first; patch only if all of it is there
    uint8_t* memcpyCalls[8] = {};
    int n = 0;
    if (uint8_t* p = FindOne(kCopyImage, "CopyImage's memcpy")) memcpyCalls[n++] = p + kCopyImageCall; else return Fail("CopyImage");
    if (uint8_t* p = FindOne(kGetCopyA, "GetCopy (1)")) memcpyCalls[n++] = p + kGetCopyACall; else return Fail("GetCopy");
    if (uint8_t* p = FindOne(kGetCopyB, "GetCopy (2)")) memcpyCalls[n++] = p + kGetCopyBCall; else return Fail("GetCopy");
    if (uint8_t* p = FindOne(kGetCopyC, "GetCopy (3)")) {
        for (size_t off : kGetCopyCCalls) memcpyCalls[n++] = p + off;
    } else return Fail("GetCopy");
    uint8_t* const del = FindOne(kDelegate, "the screenshot delegate's array copy");
    if (!del) return Fail("delegate");
    uint8_t* const assign = RipTarget(del + kDelegateCall, 1, 5);
    if (!Game().text.Contains(assign) || !MatchesAt(assign, ParsePattern(kAssign))) return Fail("the delegate's array copy is not the expected TArray assignment");
    memcpyCalls[n++] = assign + kAssignCall;
    for (int i = 0; i < n; ++i)
        if (!CallsImport(memcpyCalls[i], "memcpy")) {
            Log("[shotpar] exe+0x%llX does not call memcpy", static_cast<unsigned long long>(memcpyCalls[i] - Game().base));
            return Fail("unexpected copy call");
        }
    uint8_t* const row = FindOne(kRowLoop, "the readback's row loop");
    if (!row || !CallsImport(row + kRowLoopCall, "memcpy")) return Fail("row loop");
    uint8_t* const zf = FindOne(kZeroFill, "ImageResize's zero fill");
    if (!zf || !CallsImport(zf + kZeroFillCall, "memset")) return Fail("zero fill");
    const uint8_t* const memsetThunk = RipTarget(zf + kZeroFillCall, 1, 5);

    if (!StartPool()) return Fail("no helper threads");
    uint8_t* const region = AllocNearGame(256);
    if (!region) return Fail("no free memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    // [0, 14): jmp BigMemcpyHook
    memcpy(region, jmpAbs, 6);
    Put64(region + 6, reinterpret_cast<const void*>(&BigMemcpyHook));
    // [16, 70): the row loop's replacement, then back after the loop
    uint8_t* const rs = region + 16;
    const uint8_t rowStub[] = {
        0x48, 0x83, 0xEC, 0x30,        // sub rsp,30h
        0x48, 0x89, 0xF1,              // mov rcx,rsi    dst
        0x48, 0x89, 0xFA,              // mov rdx,rdi    src
        0x4D, 0x89, 0xF8,              // mov r8,r15     bytes per line
        0x45, 0x89, 0xF1,              // mov r9d,r14d   pitch
        0x44, 0x89, 0xE8,              // mov eax,r13d   rows
        0x48, 0x89, 0x44, 0x24, 0x20,  // mov [rsp+20h],rax
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax,RowCopyHook
        0xFF, 0xD0,                    // call rax
        0x48, 0x83, 0xC4, 0x30,        // add rsp,30h
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // jmp [rip]: after the loop
    static_assert(sizeof rowStub == 54);
    memcpy(rs, rowStub, sizeof rowStub);
    Put64(rs + 26, reinterpret_cast<const void*>(&RowCopyHook));
    Put64(rs + 46, row + kRowLoopLen);
    // [80, 129): ImageResize's zero fill, skipped when on, the arrays differ and it starts at the data
    uint8_t* const ms = region + 80;
    const uint8_t memsetStub[] = {
        0x49, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,  // mov r10,&g_stub
        0x41, 0x80, 0x3A, 0x00,              // cmp byte [r10],0
        0x74, 19,                            // je fill
        0x4C, 0x39, 0xFF,                    // cmp rdi,r15   (DstData == SrcData)
        0x74, 14,                            // je fill
        0x48, 0x3B, 0x0F,                    // cmp rcx,[rdi] (the fill starts at the data)
        0x75, 9,                             // jne fill
        0xF0, 0x49, 0xFF, 0x42, 0x08,        // lock inc qword [r10+8]
        0x48, 0x89, 0xC8,                    // mov rax,rcx  (memset returns its destination)
        0xC3,                                // ret
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // fill: jmp [rip]: memset thunk
    static_assert(sizeof memsetStub == 49);
    memcpy(ms, memsetStub, sizeof memsetStub);
    Put64(ms + 2, &g_stub);
    Put64(ms + 41, memsetThunk);
    DWORD old;
    VirtualProtect(region, 256, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 256);
    auto reach = [](const uint8_t* from, const uint8_t* to) {
        const int64_t rel = to - (from + 5);
        return rel == static_cast<int32_t>(rel);
    };
    for (int i = 0; i < n; ++i)
        if (!reach(memcpyCalls[i], region)) return Fail("stubs out of reach");
    if (!reach(row, rs) || !reach(zf + kZeroFillCall, ms)) return Fail("stubs out of reach");

    for (int i = 0; i < n; ++i)
        if (!Retarget(memcpyCalls[i], region)) return Fail("could not patch a copy call");  // (start-up: nothing runs yet)
    uint8_t jmp[5] = {0xE9};
    const int32_t relRow = static_cast<int32_t>(rs - (row + 5));
    memcpy(jmp + 1, &relRow, 4);
    if (!WriteCode(row, jmp)) return Fail("could not patch the row loop");
    if (!Retarget(zf + kZeroFillCall, ms)) return Fail("could not patch the zero fill");
    g_sites = n;
    g_rowSite = row;
    g_rowStub = rs;
    g_memsetSite = zf + kZeroFillCall;
    g_memsetStub = ms;
    g_installed = true;
    Log("[shotpar] installed: %d copy calls (CopyImage exe+0x%llX, GetCopy x5, delegate array copy exe+0x%llX), row loop "
        "exe+0x%llX, ImageResize zero fill exe+0x%llX; %d helper threads",
        n, static_cast<unsigned long long>(memcpyCalls[0] - Game().base),
        static_cast<unsigned long long>(memcpyCalls[n - 1] - Game().base), static_cast<unsigned long long>(row - Game().base),
        static_cast<unsigned long long>(g_memsetSite - Game().base), g_helpers);
    return true;
}

void SetScreenshotParallel(int mode)
{
    if (!g_installed) return;
    const bool on = mode != 0;
    if (g_on.exchange(on) == on) return;
    g_stub.on = on ? 1 : 0;
    Log("[shotpar] %s", on ? "screenshot copies on the helper threads, ImageResize's zero fill skipped" : "screenshot copies as the game does them");
}

bool ScreenshotParallelOn() { return g_on.load(std::memory_order_relaxed) && g_helpers > 0; }

void LogScreenshotParallelStats()
{
    if (!g_installed) return;
    const uint64_t sum = g_copies.load() + g_rowCopies.load() + g_bandJobs.load() + g_stub.zeroSkips + g_jobsSerial.load();
    if (sum == g_logged) return;
    g_logged = sum;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / static_cast<double>(f.QuadPart);
    auto avg = [&](uint64_t ticks, uint64_t n) { return n ? ms * static_cast<double>(ticks) / static_cast<double>(n) : 0.0; };
    Log("[shotpar] big copies %llu (%.0f MB, %.2f ms each), readback row copies %llu (%.2f ms each), row-band jobs %llu "
        "(%.2f ms each), zero fills skipped %llu; jobs %llu (parts by helpers %llu), run alone %llu",
        static_cast<unsigned long long>(g_copies.load()), static_cast<double>(g_copyBytes.load()) / 1048576.0,
        avg(g_copyTicks.load(), g_copies.load()), static_cast<unsigned long long>(g_rowCopies.load()),
        avg(g_rowTicks.load(), g_rowCopies.load()), static_cast<unsigned long long>(g_bandJobs.load()),
        avg(g_bandTicks.load(), g_bandJobs.load()), static_cast<unsigned long long>(g_stub.zeroSkips),
        static_cast<unsigned long long>(g_jobs.load()), static_cast<unsigned long long>(g_helperParts.load()),
        static_cast<unsigned long long>(g_jobsSerial.load()));
}

bool ScreenshotParallelInfo(ScreenshotParallelSites* out)
{
    if (!g_installed) return false;
    out->copySites = g_sites;
    out->rowSite = g_rowSite;
    out->rowStub = g_rowStub;
    out->memsetSite = g_memsetSite;
    out->memsetStub = g_memsetStub;
    out->zeroSkips = &g_stub.zeroSkips;
    out->helpers = g_helpers;
    return true;
}

}  // namespace opf
