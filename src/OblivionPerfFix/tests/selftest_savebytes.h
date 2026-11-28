// SaveBytesBulk self-test, included by selftest.cpp: the sites on the mapped exe, then the game's own
// FArrayProperty::SerializeItem (with the game's FByteProperty::SerializeItem as the inner property's function) and the hook
// on the same arrays into a recording archive: the bytes written, the CountBytes arguments and the serialized-property
// pushes must give the same archive contents; then every condition that must send the array to the game's function.
#pragma once
#include <random>

namespace svb {

// A recording binary archive with the layout the game's array code reads (+0x08 fast-path load buffer, +0x28.. flags,
// +0x40 version, +0x60 custom property list, +0x70 serialized property).
struct Archive {
    void** vtable;
    uintptr_t* fplb;
    uint8_t pad10[0x18];
    uint8_t flags[8];      // +0x28
    uint8_t pad30[0x10];
    int32_t ueVer;         // +0x40
    uint8_t pad44[0x1C];
    void* customList;      // +0x60
    uint8_t pad68[8];
    void* serializedProp;  // +0x70
    uint8_t pad78[0x18];
    size_t off = 0;        // +0x90: the read position (Tell / Seek are machine-code stubs that read / write it, as FMemoryArchive's)
    uint8_t pad98[0x60];
    std::vector<uint8_t> bytes;
    std::vector<int64_t> calls;  // Serialize lengths
    size_t countNum = 0, countMax = 0;
    int pushes = 0, pops = 0, unexpected = 0;
    // A loading memory archive (FMemoryReader's rules): the data, the offset, TotalSize; Serialize copies all or nothing and
    // sets the error bit (+0x29 bit 1) when it would run past the end. totalKnown false = TotalSize answers -1 (the default).
    bool readMode = false, totalKnown = true;
    std::vector<uint8_t> src;
    size_t off2 = 0;  // a second cursor, for the archive whose Serialize does not move the member Tell reads
};
static_assert(offsetof(Archive, flags) == 0x28 && offsetof(Archive, ueVer) == 0x40 && offsetof(Archive, customList) == 0x60 &&
                  offsetof(Archive, serializedProp) == 0x70 && offsetof(Archive, off) == 0x90, "archive layout");

void Unexpected(Archive* a) { ++a->unexpected; }
void CountBytes(Archive* a, size_t num, size_t max) { a->countNum += num; a->countMax += max; }
int64_t TotalSize(Archive* a) { return a->readMode && a->totalKnown ? static_cast<int64_t>(a->src.size()) : -1; }
void Serialize(Archive* a, uint8_t* data, int64_t n)
{
    a->calls.push_back(n);
    if (a->readMode) {
        if (n <= 0 || (a->flags[1] & 2)) return;
        if (a->off + static_cast<size_t>(n) > a->src.size()) { a->flags[1] |= 2; return; }
        memcpy(data, a->src.data() + a->off, static_cast<size_t>(n));
        a->off += static_cast<size_t>(n);
        return;
    }
    a->bytes.insert(a->bytes.end(), data, data + n);
}
// Seeks the archive's code check must not accept (C++, so not the plain store): one that does nothing, one that only goes forward.
void SeekNoop(Archive* a, int64_t) { ++a->pops; --a->pops; }
void SeekForward(Archive* a, int64_t pos)
{
    if (pos > static_cast<int64_t>(a->off)) a->off = static_cast<size_t>(pos);
}
// Serialize that reads from off2 and leaves off (the member Tell / Seek use) alone: an archive the code check accepts but whose
// Tell does not move over the peek (the post-peek check's case).
void SerializeOwnCursor(Archive* a, uint8_t* data, int64_t n)
{
    a->calls.push_back(n);
    if (n <= 0 || a->off2 + static_cast<size_t>(n) > a->src.size()) return;
    memcpy(data, a->src.data() + a->off2, static_cast<size_t>(n));
    a->off2 += static_cast<size_t>(n);
}
// A proxy (FArchiveProxy's layout here: the inner archive pointer at +0x90, in the off member): Tell / Seek are forwarder stubs;
// TotalSize and Serialize forward in C++ (the code check does not look at them).
Archive* InnerOf(Archive* a) { return reinterpret_cast<Archive*>(a->off); }
int64_t TotalSize(Archive* a);
void Serialize(Archive* a, uint8_t* data, int64_t n);
int64_t ProxyTotal(Archive* a) { return TotalSize(InnerOf(a)); }
void ProxySerialize(Archive* a, uint8_t* data, int64_t n) { a->calls.push_back(n); Serialize(InnerOf(a), data, n); }

// Machine-code stubs (an executable page): the shapes the hook accepts, and some it must refuse.
constexpr uint8_t kStubLeafTell[] = {0x48, 0x8B, 0x81, 0x90, 0, 0, 0, 0xC3};          // mov rax,[rcx+90h]; ret
constexpr uint8_t kStubLeafSeek[] = {0x48, 0x89, 0x91, 0x90, 0, 0, 0, 0xC3};          // mov [rcx+90h],rdx; ret
constexpr uint8_t kStubFwdTell[] = {0x48, 0x8B, 0x89, 0x90, 0, 0, 0, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x28};               // jmp [rax+28h]
constexpr uint8_t kStubFwdSeek[] = {0x48, 0x8B, 0x89, 0x90, 0, 0, 0, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0, 0xA0, 0x01, 0, 0};  // jmp [rax+1A0h]
constexpr uint8_t kStubTell8[] = {0x48, 0x8B, 0x41, 0x10, 0xC3};                      // mov rax,[rcx+10h]; ret
constexpr uint8_t kStubSeek8[] = {0x48, 0x89, 0x51, 0x10, 0xC3};                      // mov [rcx+10h],rdx; ret
constexpr uint8_t kStubSeek98[] = {0x48, 0x89, 0x91, 0x98, 0, 0, 0, 0xC3};            // another member than Tell's
constexpr uint8_t kStubRet[] = {0xC3};
constexpr uint8_t kStubFwdSeekToTell[] = {0x48, 0x8B, 0x89, 0x90, 0, 0, 0, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x28};  // a "Seek" that forwards to Tell's slot
enum StubId { kLeafTell, kLeafSeek, kFwdTell, kFwdSeek, kTell8, kSeek8, kSeek98, kRet, kFwdSeekToTell, kStubCount };
uint8_t* g_stubs = nullptr;
void* Stub(int id) { return g_stubs ? g_stubs + 32 * id : nullptr; }
bool MakeStubs()
{
    if (g_stubs) return true;
    uint8_t* const page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page) return false;
    memset(page, 0xCC, 4096);
    const std::pair<const uint8_t*, size_t> code[kStubCount] = {
        {kStubLeafTell, sizeof kStubLeafTell}, {kStubLeafSeek, sizeof kStubLeafSeek}, {kStubFwdTell, sizeof kStubFwdTell},
        {kStubFwdSeek, sizeof kStubFwdSeek},   {kStubTell8, sizeof kStubTell8},       {kStubSeek8, sizeof kStubSeek8},
        {kStubSeek98, sizeof kStubSeek98},     {kStubRet, sizeof kStubRet},           {kStubFwdSeekToTell, sizeof kStubFwdSeekToTell}};
    for (int i = 0; i < kStubCount; ++i) memcpy(page + 32 * i, code[i].first, code[i].second);
    DWORD old = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    g_stubs = page;
    return true;
}
void Push(Archive* a, void*, bool) { ++a->pushes; }
void Pop(Archive* a, void*, bool) { ++a->pops; }

void* g_archiveVtable[80];
void* g_innerVtable[48];
int g_fakeOrigCalls = 0;
int MinAlignment() { return 1; }
Archive* g_fakeAr = nullptr;     // read-side tests: the archive FakeOriginal reports on
uint8_t g_fakeSeenFlags = 0xFF;  // its flag byte as FakeOriginal saw it
void FakeOriginal(void*, void*, void*, const void*)
{
    ++g_fakeOrigCalls;
    if (g_fakeAr) g_fakeSeenFlags = g_fakeAr->flags[0];
}
void* MemsetForGame(void* d, int v, size_t n) { return memset(d, v, n); }

struct Fakes {
    alignas(16) uint8_t cls[0x40] = {};
    alignas(16) uint8_t inner[0x80] = {};
    alignas(16) uint8_t array[0x80] = {};
    uintptr_t fplb[3] = {0, 0, 0};
    Archive ar{};
    struct S { void* a; void* ar; } s{};
    void* xh = nullptr;
    void* slot = nullptr;
    Fakes()
    {
        *reinterpret_cast<uint64_t*>(cls + 0x10) = 0x40 | 0x1000000;  // CASTCLASS_FByteProperty | CASTCLASS_FNumericProperty
        *reinterpret_cast<void***>(inner) = g_innerVtable;
        *reinterpret_cast<uint8_t**>(inner + 0x08) = cls;
        *reinterpret_cast<int32_t*>(inner + 0x34) = 1;
        *reinterpret_cast<uint64_t*>(inner + 0x38) = 0x1040000200ull;  // PropertyFlags: ZeroConstructor | no destructor (the read side)
        *reinterpret_cast<void**>(array + 0x78) = inner;
        ar.vtable = g_archiveVtable;
        ar.fplb = fplb;
        ar.flags[0] = 0x04;  // saving
        ar.ueVer = 522;
        s = {nullptr, &ar};
        xh = &s;
        slot = &xh;
    }
};

inline void SaveBytesSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    const bool stubs = MakeStubs();
    Check(stubs, "savebytesread: the machine-code Tell / Seek stubs (an executable page)");
    if (!stubs) return;
    for (auto& v : g_archiveVtable) v = reinterpret_cast<void*>(&Unexpected);
    g_archiveVtable[0x10 / 8] = reinterpret_cast<void*>(&CountBytes);
    g_archiveVtable[0x28 / 8] = Stub(kLeafTell);
    g_archiveVtable[0x30 / 8] = reinterpret_cast<void*>(&TotalSize);
    g_archiveVtable[0x170 / 8] = reinterpret_cast<void*>(&Serialize);
    g_archiveVtable[0x1A0 / 8] = Stub(kLeafSeek);
    g_archiveVtable[0x228 / 8] = reinterpret_cast<void*>(&Push);
    g_archiveVtable[0x230 / 8] = reinterpret_cast<void*>(&Pop);
    uint8_t* const arrayFn = game.base + 0x10BB690;
    uint8_t* const byteFn = game.base + 0x10BC190;
    uint8_t* const getArchive = game.base + 0xF32EE0;
    for (auto& v : g_innerVtable) v = reinterpret_cast<void*>(&MinAlignment);
    g_innerVtable[0xB8 / 8] = byteFn;

    void** const slotA = reinterpret_cast<void**>(game.base + 0x6E84060);
    void** const slotB = reinterpret_cast<void**>(game.base + 0x6E94C08);
    Check(*slotA == arrayFn && *slotB == arrayFn, "savebytes: FArrayProperty::SerializeItem in both vtables at +0xB8 (exe+0x6E84060, +0x6E94C08)");
    Check(!InstallSaveBytes(false, true, true, true), "savebytes: refuses to install after game code started");
    Check(!InstallSaveBytes(true, false, false, false), "savebytes: nothing installed with all keys off");
    Check(!SaveBytesReadInstalled(), "savebytesread: not installed yet");
    Check(*slotA == arrayFn && *slotB == arrayFn, "savebytes: the vtable slots untouched while not installed");
    // The scan's hit check: the four call sites pass; an E8 call not followed by test al,al and a copy outside the code fail.
    {
        int valid = 0;
        for (uint32_t rva : {0x3198FC4u, 0x31BF6B6u, 0x48B23D5u, 0x48CC37Cu}) valid += SaveCallSiteValid(game, game.base + rva);
        Check(valid == 4, "savebytes: the four SaveGameToMemory call sites pass the hit check (inside a .pdata function, 84 C0 within 8 bytes)");
        Check(PrimaryFunction(game, game.base + 0x31BF6B6) != nullptr && PrimaryFunction(game, game.base + 0x31BF6B6) <= game.base + 0x31BF6B6,
              "savebytes: PrimaryFunction finds the function holding a call site");
        Check(game.base[0x3222FA8] == 0xE8 && !SaveCallSiteValid(game, game.base + 0x3222FA8),
              "savebytes: an E8 call whose result is not tested right after (the read-back call exe+0x3222FA8) is refused");
        uint8_t copy[32];
        memcpy(copy, game.base + 0x31BF6B6, sizeof copy);
        Check(!SaveCallSiteValid(game, copy), "savebytes: the same bytes outside the game's code are refused");
    }
    Check(InstallSaveBytes(true, true, true, true), "savebytes: array/byte SerializeItem, SaveGameToMemory and its calls found, installed");
    Check(SaveBytesReadInstalled(), "savebytesread: the bulk branch's four byte checks held, installed with the same vtable slots");
    Check(*slotA != arrayFn && *slotB != arrayFn, "savebytes: both vtable slots now point to the hook");
    {
        int32_t rel;
        int redirected = 0;
        for (uint32_t rva : {0x3198FC4u, 0x31BF6B6u, 0x48B23D5u, 0x48CC37Cu}) {
            memcpy(&rel, game.base + rva + 1, 4);
            if (game.base[rva] == 0xE8 && game.base + rva + 5 + rel != game.base + 0x31BF380) ++redirected;
        }
        Check(redirected == 4, "savebytes: the four SaveGameToMemory calls (exe+0x3198FC4, 0x31BF6B6, 0x48B23D5, 0x48CC37C) redirected");
        memcpy(&rel, game.base + 0x3222FA8 + 1, 4);
        Check(game.base + 0x3222FA8 + 5 + rel != game.base + 0x37E4F30, "savebytes: the screenshot read-back call exe+0x3222FA8 redirected (timer)");
        // SaveThumbnailAsync (save_thumb.cpp): ReadPixels' render command lambda at both call sites and its flush.
        int thumb = 0;
        for (auto [rva, target] : {std::pair{0x37F36C7u, 0x37D2C20u}, {0x37E2B7Du, 0x37D2C20u}, {0x37F375Eu, 0x26BC840u}, {0x3222FFFu, 0x31F5A20u}}) {
            memcpy(&rel, game.base + rva + 1, 4);
            if (game.base[rva] == 0xE8 && game.base + rva + 5 + rel != game.base + target) ++thumb;
        }
        Check(thumb == 4, "savethumb: the ReadSurfaceCommand lambda calls (exe+0x37F36C7, 0x37E2B7D), ReadPixels' flush (exe+0x37F375E) and the screenshot broadcast (exe+0x3222FFF) redirected");
    }
    // SaveThumbnailAsync's HDR route: GetViewportScreenShotHDR's call, ReadLinearColorPixels' lambda calls and flush.
    {
        uint8_t* hdrCall = nullptr;
        void* toFColor = nullptr;
        const bool hdr = SaveThumbHdrInfo(&hdrCall, &toFColor);
        Check(hdr && hdrCall == game.base + 0x3222F80 && toFColor == game.base + 0xEAB6A0,
              "savethumb: the HDR route found (read call exe+0x3222F80, ToFColorSRGB exe+0xEAB6A0)");
        int32_t rel;
        int redirected = 0;
        for (auto [rva, target] : {std::pair{0x3222F80u, 0x37E4FB0u}, {0x37F3427u, 0x37D2B50u}, {0x37E2A7Du, 0x37D2B50u}, {0x37F34BEu, 0x26BC840u}}) {
            memcpy(&rel, game.base + rva + 1, 4);
            if (game.base[rva] == 0xE8 && game.base + rva + 5 + rel != game.base + target) ++redirected;
        }
        Check(redirected == 4, "savethumb: the HDR read call (exe+0x3222F80), ReadLinearColorPixels' lambda calls (exe+0x37F3427, 0x37E2A7D) and its flush (exe+0x37F34BE) redirected");
        uint32_t scale;
        memcpy(&scale, game.base + 0x709E164, 4);
        Check(scale == 0x3A802008u, "savethumb: the D3D12 read's R10G10B10A2 factor at exe+0x709E164 is 1/1023.f, as the tables use");
        if (hdr) {
            // The game's own converter (exe+0x251C400, format 18) and quantiser on the same pixels as the tables: every
            // 10-bit value in every channel, then random pixels.
            std::vector<uint32_t> in;
            for (uint32_t v = 0; v < 1024; ++v) in.push_back(v | v << 10 | v << 20 | (v & 3) << 30);
            for (uint32_t v = 0; v < 1024; ++v) in.push_back(v | (1023 - v) << 10 | ((v * 7) & 1023) << 20 | (3 - (v & 3)) << 30);
            std::mt19937 r10(11);
            for (int i = 0; i < 8192; ++i) in.push_back(static_cast<uint32_t>(r10()));
            const uint32_t n = static_cast<uint32_t>(in.size());
            std::vector<float> lin(static_cast<size_t>(n) * 4);
            uint8_t flags[32] = {};
            using ConvertFn = void (*)(char format, uint32_t w, uint32_t h, const void* in, int32_t pitch, void* out, const void* flags);
            reinterpret_cast<ConvertFn>(game.base + 0x251C400)(18, n, 1, in.data(), static_cast<int32_t>(n * 4), lin.data(), flags);
            using QuantFn = uint32_t* (*)(const float* linear, uint32_t* out);
            std::vector<uint32_t> stock(n), ours(n);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t c = 0;
                reinterpret_cast<QuantFn>(toFColor)(&lin[static_cast<size_t>(i) * 4], &c);
                stock[i] = c | 0xFF000000u;
            }
            SaveThumbTestHdrConvert(in.data(), n, ours.data());
            uint32_t diff = 0;
            for (uint32_t i = 0; i < n; ++i) diff += stock[i] != ours[i];
            Check(diff == 0, "savethumb: the HDR tables give the game's bytes (its converter exe+0x251C400 and ToFColorSRGB) on all 1024 values per channel and 8192 random pixels");
            if (diff) Log("[selftest] savethumb HDR: %u of %u pixels differ", diff, n);
        }
    }
    // SaveThumbnailAsync's delivery: the save system's cache (not a save callback) before the save callbacks, each group
    // from the end of the invocation list (the broadcast's walk).
    {
        int order[8] = {};
        const bool game2[2] = {false, true};  // the game's binding order: the cache (Initialize), then the save's callback
        const int n2 = SaveThumbTestDeliverOrder(game2, 2, order);
        Check(n2 == 2 && order[0] == 0 && order[1] == 1, "savethumb: the save system's cache gets the picture before the save's callback");
        const bool mixed[4] = {true, false, true, false};
        const int n4 = SaveThumbTestDeliverOrder(mixed, 4, order);
        Check(n4 == 4 && order[0] == 3 && order[1] == 1 && order[2] == 2 && order[3] == 0,
              "savethumb: every listener called once: the caches from the end of the list first, then the save callbacks from the end");
    }

    // The game's function and the hook on the same arrays: same archive contents.
    std::mt19937 rng(7);
    bool same = true, fewer = true, clean = true;
    for (int32_t n : {0, 1, 2, 3, 17, 255, 4096, 100003, (1 << 20) + 5}) {
        for (int prop = 0; prop < 2; ++prop) {
            std::vector<uint8_t> data(static_cast<size_t>(n) + 1);
            for (auto& b : data) b = static_cast<uint8_t>(rng());
            struct { uint8_t* data; int32_t num, max; } arr{n ? data.data() : nullptr, n, n + 3};
            Fakes game1, hook1;
            if (prop) { game1.ar.serializedProp = game1.array; hook1.ar.serializedProp = hook1.array; }
            reinterpret_cast<void (*)(void*, void*, void*, const void*)>(arrayFn)(game1.array, game1.slot, &arr, nullptr);
            SaveBytesTestSetup(arrayFn, getArchive, byteFn, true);
            SaveBytesTestCall(hook1.array, hook1.slot, &arr, true);
            same &= game1.ar.bytes == hook1.ar.bytes && game1.ar.countNum == hook1.ar.countNum &&
                    game1.ar.countMax == hook1.ar.countMax && game1.ar.bytes.size() == 4 + static_cast<size_t>(n);
            fewer &= hook1.ar.calls.size() == (n ? 2u : 1u) && game1.ar.calls.size() == 1u + static_cast<size_t>(n);
            clean &= game1.ar.unexpected == 0 && hook1.ar.unexpected == 0 && game1.ar.pushes == game1.ar.pops &&
                     hook1.ar.pushes == 0 && hook1.ar.pops == 0;
        }
    }
    Check(same, "savebytes: the hook writes the same bytes and CountBytes as the game's per-element loop (0 to 1M+5 elements)");
    Check(fewer, "savebytes: the game writes the count and one call per byte; the hook the count and one call");
    Check(clean, "savebytes: no other archive function used; the game's pushes balanced, the hook pushes nothing");

    // Every condition that must leave the array to the game's function (a fake original counts the calls).
    auto falls = [&](auto change, bool inSave = true, bool bulk = true) {
        Fakes f;
        uint8_t one = 1;
        struct { uint8_t* data; int32_t num, max; } arr{&one, 1, 1};
        change(f);
        SaveBytesTestSetup(reinterpret_cast<void*>(&FakeOriginal), getArchive, byteFn, bulk);
        g_fakeOrigCalls = 0;
        SaveBytesTestCall(f.array, f.slot, &arr, inSave);
        return g_fakeOrigCalls == 1 && f.ar.bytes.empty();
    };
    auto none = [](Fakes&) {};
    Check(!falls(none), "savebytes: a plain byte array in a save is written by the hook");
    Check(falls(none, false), "savebytes: outside SaveGameToMemory: the game's function");
    Check(falls(none, true, false), "savebytes: SaveBytesBulk off: the game's function");
    Check(falls([](Fakes& f) { *reinterpret_cast<void**>(f.inner + 0x70) = f.cls; }), "savebytes: a byte property with an enum: the game's function");
    Check(falls([](Fakes& f) { *reinterpret_cast<int32_t*>(f.inner + 0x34) = 2; }), "savebytes: element size 2: the game's function");
    Check(falls([](Fakes& f) { *reinterpret_cast<uint64_t*>(f.cls + 0x10) = 0x80; }), "savebytes: not a byte property: the game's function");
    Check(falls([](Fakes& f) { *reinterpret_cast<void***>(f.inner) = g_archiveVtable; }),
          "savebytes: an inner property with another SerializeItem: the game's function");
    Check(falls([](Fakes& f) { f.array[0x70] = 1; }), "savebytes: an array with the memory-image allocator: the game's function");
    Check(falls([](Fakes& f) { f.ar.flags[0] = 0x05; }), "savebytes: a loading archive: the game's function");
    Check(falls([](Fakes& f) { f.ar.flags[0] = 0x00; }), "savebytes: an archive that does not save: the game's function");
    Check(falls([](Fakes& f) { f.ar.flags[0] = 0x14; }), "savebytes: a text archive: the game's function");
    Check(falls([](Fakes& f) { f.ar.flags[0] = 0x44; }), "savebytes: unversioned serialization: the game's function (it has its own bulk path)");
    Check(falls([](Fakes& f) { f.ar.flags[2] = 0x01; }), "savebytes: byte swapping: the game's function");
    Check(falls([](Fakes& f) { f.ar.flags[3] = 0x80; }), "savebytes: a custom property list: the game's function");
    static uint8_t buf[16];
    Check(falls([](Fakes& f) { f.fplb[0] = reinterpret_cast<uintptr_t>(buf); f.fplb[1] = reinterpret_cast<uintptr_t>(buf + 16); }),
          "savebytes: a fast-path load buffer with room: the game's function");

    // ---- SaveBytesBulkRead: the game's own function with its bulk branch switched on for one call, against the per-element read ----
    {
        using ArrFn = void (*)(void*, void*, void*, const void*);
        // 1. The four byte checks of the branch, and a copy of the function with one byte changed.
        Check(SaveBytesReadBranchOk(arrayFn), "savebytesread: the bulk branch's four byte checks (+0x66 / +0xA7 / +0xB9 / +0x221) hold in the game's function");
        {
            uint8_t copy[0x240];
            memcpy(copy, arrayFn, sizeof copy);
            bool all = true;
            for (size_t off : {size_t{0x66}, size_t{0x68}, size_t{0xA7}, size_t{0xAB}, size_t{0xB9}, size_t{0xBD}, size_t{0x221}, size_t{0x225}}) {
                copy[off] ^= 1;
                all &= !SaveBytesReadBranchOk(copy);
                copy[off] ^= 1;
            }
            Check(all && SaveBytesReadBranchOk(copy), "savebytesread: one changed byte in any of the four places turns the key off (a copy of the function)");
        }
        // 2. The game's zero-construct (AddValues) calls memset through an import thunk of the mapped exe: point its slot at this CRT's.
        void** const memsetSlot = reinterpret_cast<void**>(game.base + 0x6D97750);
        int32_t thunkRel = 0;
        memcpy(&thunkRel, game.base + 0x6BFC464 + 2, 4);
        const bool thunkOk = game.base[0x6BFC464] == 0xFF && game.base[0x6BFC465] == 0x25 &&
                             reinterpret_cast<void**>(game.base + 0x6BFC464 + 6 + thunkRel) == memsetSlot;
        Check(thunkOk, "savebytesread: the memset import thunk exe+0x6BFC464 reads the slot exe+0x6D97750");
        void* const savedMemset = *memsetSlot;
        DWORD oldProt = 0;
        const bool writable = thunkOk && VirtualProtect(memsetSlot, sizeof(void*), PAGE_READWRITE, &oldProt);
        if (writable) *memsetSlot = reinterpret_cast<void*>(&MemsetForGame);

        struct SArr { uint8_t* data; int32_t num, max; };
        struct Result {
            std::vector<uint8_t> data;
            int32_t num = 0, max = 0;
            size_t off = 0;
            bool err = false;
            size_t cNum = 0, cMax = 0;
            std::vector<int64_t> calls;
            int unexpected = 0, pushes = 0, pops = 0;
            uint8_t flags0 = 0;
        };
        // Source stream: prefix bytes, the int32 count, payload bytes (random), trailing bytes.
        std::mt19937 rr(11);
        auto makeSrc = [&](size_t prefix, int32_t count, size_t payload, size_t trailing) {
            std::vector<uint8_t> s(prefix + 4 + payload + trailing);
            for (auto& b : s) b = static_cast<uint8_t>(rr());
            memcpy(s.data() + prefix, &count, 4);
            return s;
        };
        int hookMode = 1;  // the key's value for hook runs (lab: 2 = verify)
        // One run: the game's function alone, or through the hook (the key on), on a fresh fake loading archive.
        // vt: another vtable for the archive (nullptr = the plain memory reader); proxy: f.ar forwards to a second archive that
        // holds the data (the position, error bit and data are read from it).
        auto run = [&](bool hook, const std::vector<uint8_t>& src, size_t start, int32_t destMax, int32_t destNum, bool prop,
                       void** vt = nullptr, bool proxy = false) {
            Fakes f;
            Archive inner{};
            Archive& data = proxy ? inner : f.ar;
            f.ar.readMode = true;
            f.ar.flags[0] = 0x01;
            data.readMode = true;
            data.flags[0] = 0x01;
            data.vtable = g_archiveVtable;
            data.src = src;
            data.off = start;
            if (vt) f.ar.vtable = vt;
            if (proxy) f.ar.off = reinterpret_cast<size_t>(&inner);
            if (prop) f.ar.serializedProp = f.array;
            std::vector<uint8_t> dbuf(static_cast<size_t>(destMax > 0 ? destMax : 1), 0xCD);
            SArr arr{dbuf.data(), destNum, destMax};
            SaveBytesTestSetup(arrayFn, getArchive, byteFn, false, hook ? hookMode : 0);
            SaveBytesTestCall(f.array, f.slot, &arr, false);
            Result r;
            r.data.assign(dbuf.begin(), dbuf.begin() + std::min<size_t>(dbuf.size(), static_cast<size_t>(std::max(arr.num, 0))));
            r.num = arr.num; r.max = arr.max; r.off = data.off; r.err = (data.flags[1] & 2) != 0;
            r.cNum = f.ar.countNum; r.cMax = f.ar.countMax; r.calls = f.ar.calls;
            r.unexpected = f.ar.unexpected; r.pushes = f.ar.pushes; r.pops = f.ar.pops; r.flags0 = f.ar.flags[0];
            return r;
        };
        auto sameResult = [](const Result& a, const Result& b) {
            return a.data == b.data && a.num == b.num && a.max == b.max && a.off == b.off && a.err == b.err && a.cNum == b.cNum &&
                   a.cMax == b.cMax && a.flags0 == b.flags0;
        };
        if (writable) {
            // 3. Equivalence matrix: the same array, final offset, error bit, CountBytes and flag byte; the call lists differ as intended.
            bool rsame = true, calls = true, rclean = true, restored = true;
            int runs = 0;
            for (int32_t n : {512, 513, 4096, 100003, (1 << 20) + 5}) {
                for (int prop = 0; prop < 2; ++prop) {
                    for (int pre = 0; pre < 2; ++pre) {
                        const size_t prefix = pre ? 13 : 0;
                        const auto src = makeSrc(prefix, n, static_cast<size_t>(n), pre ? 5 : 0);
                        // destination Max == n (the game's resize is then skipped: the mapped exe has no allocator); Num 0, or 7 (the destruct call)
                        for (int32_t destNum : {0, 7}) {
                            const Result g = run(false, src, prefix, n, destNum, prop != 0);
                            const Result h = run(true, src, prefix, n, destNum, prop != 0);
                            ++runs;
                            rsame &= sameResult(g, h) && g.num == n && !g.err && g.off == prefix + 4 + static_cast<size_t>(n) &&
                                    std::equal(g.data.begin(), g.data.end(), src.begin() + static_cast<std::ptrdiff_t>(prefix + 4));
                            calls &= g.calls.size() == 1u + static_cast<size_t>(n) && h.calls.size() == 3u && h.calls[0] == 4 && h.calls[1] == 4 &&
                                     h.calls[2] == n;
                            rclean &= g.unexpected == 0 && h.unexpected == 0 && g.pushes == g.pops && h.pushes == h.pops;
                            restored &= h.flags0 == 0x01 && g.flags0 == 0x01;
                        }
                    }
                }
            }
            Check(rsame, "savebytesread: the hook reads the same array (bytes, Num, Max), error bit, final offset and CountBytes as the game's per-element loop (512 to 1M+5 bytes, with and without a prefix, a serialized property, an old Num)");
            Check(calls, "savebytesread: the game reads the count and one call per byte; the hook peeks the count, the game's function reads it again, then ONE call of n bytes");
            Check(rclean, "savebytesread: no other archive function used; pushes and pops balanced");
            Check(restored, "savebytesread: the archive's flag byte is back to 0x01 after the call");
            if (!rsame || !calls) Log("[selftest] savebytesread: %d runs", runs);

            // 4. Short data: count 700, 600 bytes after it: the game copies what is there and sets the error; the hook leaves it to the game.
            {
                const auto src = makeSrc(0, 700, 600, 0);
                const Result g = run(false, src, 0, 700, 0, false), h = run(true, src, 0, 700, 0, false);
                Check(sameResult(g, h) && g.err && g.off == 4 + 600, "savebytesread: short data (count 700, 600 bytes left): the same partial array and error as the game's loop");
                // the last property of a file: n == the bytes left exactly: read in one piece, no error
                const auto exact = makeSrc(9, 2000, 2000, 0);
                const Result ge = run(false, exact, 9, 2000, 0, false), he = run(true, exact, 9, 2000, 0, false);
                Check(sameResult(ge, he) && !he.err && he.off == exact.size() && he.calls.size() == 3u,
                      "savebytesread: n equal to the bytes left (a file that ends with the array): read in one piece, no error, same result");
                const auto over = makeSrc(9, 2001, 2000, 0);
                const Result go = run(false, over, 9, 2001, 0, false), ho = run(true, over, 9, 2001, 0, false);
                Check(sameResult(go, ho) && ho.err && ho.calls.size() == go.calls.size() + 1u,
                      "savebytesread: n one more than the bytes left: the game's way (partial array, error), the hook's peek left no trace");
            }
        }

        // 4b. Archives whose Seek cannot go back (review S2-1): refused BEFORE anything is read, so the game's own function gives the
        // same array, position, error bit and CountBytes as without the hook; the hook made no call of its own (no peek). Then a
        // proxy over a memory reader (FObjectAndNameAsStringProxyArchive over FMemoryReader, as LoadGameFromMemory builds it): taken.
        if (writable) {
            static void* noopVt[80];
            static void* fwdVt[80];
            static void* proxyVt[80];
            memcpy(noopVt, g_archiveVtable, sizeof noopVt);
            memcpy(fwdVt, g_archiveVtable, sizeof fwdVt);
            memcpy(proxyVt, g_archiveVtable, sizeof proxyVt);
            noopVt[0x1A0 / 8] = reinterpret_cast<void*>(&SeekNoop);
            fwdVt[0x1A0 / 8] = reinterpret_cast<void*>(&SeekForward);
            proxyVt[0x28 / 8] = Stub(kFwdTell);
            proxyVt[0x30 / 8] = reinterpret_cast<void*>(&ProxyTotal);
            proxyVt[0x170 / 8] = reinterpret_cast<void*>(&ProxySerialize);
            proxyVt[0x1A0 / 8] = Stub(kFwdSeek);
            bool noop = true, fwd = true, prx = true;
            for (int32_t n : {512, 4096, 100003}) {
                for (int pre = 0; pre < 2; ++pre) {
                    const size_t prefix = pre ? 13 : 0;
                    const auto src = makeSrc(prefix, n, static_cast<size_t>(n), pre ? 5 : 0);
                    const size_t end = prefix + 4 + static_cast<size_t>(n);
                    auto okData = [&](const Result& r) {
                        return r.num == n && !r.err && r.off == end && std::equal(r.data.begin(), r.data.end(), src.begin() + static_cast<std::ptrdiff_t>(prefix + 4));
                    };
                    {
                        const Result g = run(false, src, prefix, n, 0, false, noopVt), h = run(true, src, prefix, n, 0, false, noopVt);
                        noop &= sameResult(g, h) && okData(h) && h.calls == g.calls;
                    }
                    {
                        const Result g = run(false, src, prefix, n, 0, false, fwdVt), h = run(true, src, prefix, n, 0, false, fwdVt);
                        fwd &= sameResult(g, h) && okData(h) && h.calls == g.calls;
                    }
                    {
                        const Result g = run(false, src, prefix, n, 0, false, proxyVt, true), h = run(true, src, prefix, n, 0, false, proxyVt, true);
                        prx &= sameResult(g, h) && okData(h) && h.calls.size() == 3u && h.calls[2] == n;
                    }
                }
            }
            Check(noop, "savebytesread: an archive whose Seek does nothing: refused before any read; array, position, error bit, CountBytes and calls as the game's own read");
            Check(fwd, "savebytesread: an archive whose Seek only goes forward: refused before any read; array, position, error bit, CountBytes and calls as the game's own read");
            Check(prx, "savebytesread: a proxy over a memory reader (forwarder Tell / Seek): read in one piece, the same array, position and CountBytes as the game's read");
        }
        // 4c. The code check alone (nothing is called): shapes accepted and refused, and the census archive type of the game.
        {
            struct Obj { void** vt; uint8_t pad[0x88]; void* inner; uint8_t pad2[0x40]; };
            static_assert(offsetof(Obj, inner) == 0x90, "proxy layout");
            static void* v[6][80];
            for (auto& t : v) memcpy(t, g_archiveVtable, sizeof t);
            v[0][0x28 / 8] = Stub(kTell8);    v[0][0x1A0 / 8] = Stub(kSeek8);     // disp8 forms, same member: accepted
            v[1][0x28 / 8] = Stub(kLeafTell); v[1][0x1A0 / 8] = Stub(kSeek98);    // another member: refused
            v[2][0x28 / 8] = Stub(kLeafTell); v[2][0x1A0 / 8] = Stub(kRet);       // ret: refused
            v[3][0x28 / 8] = Stub(kFwdTell);  v[3][0x1A0 / 8] = Stub(kFwdSeek);   // proxy
            v[4][0x28 / 8] = Stub(kFwdTell);  v[4][0x1A0 / 8] = Stub(kFwdSeekToTell);  // a Seek that forwards to another slot: refused
            v[5][0x28 / 8] = Stub(kLeafTell); v[5][0x1A0 / 8] = Stub(kFwdSeek);   // leaf Tell, forwarding Seek: refused
            Obj leaf8{v[0]}, other{v[1]}, ret{v[2]}, leaf{g_archiveVtable};
            Obj p1{v[3]}, p2{v[3]}, p3{v[3]}, p4{v[3]}, pNull{v[3]}, pSlot{v[4]}, mixed{v[5]};
            p1.inner = &leaf; p2.inner = &p1; p3.inner = &p2; p4.inner = &p3;
            pSlot.inner = &leaf; mixed.inner = &leaf;
            Obj pRet{v[3]};
            pRet.inner = &ret;
            const bool shapes = SaveBytesSeekableTest(&leaf) && SaveBytesSeekableTest(&leaf8) && !SaveBytesSeekableTest(&other) &&
                                !SaveBytesSeekableTest(&ret) && SaveBytesSeekableTest(&p1) && SaveBytesSeekableTest(&p3) &&
                                !SaveBytesSeekableTest(&p4) && !SaveBytesSeekableTest(&pNull) && !SaveBytesSeekableTest(&pSlot) &&
                                !SaveBytesSeekableTest(&mixed) && !SaveBytesSeekableTest(&pRet);
            Check(shapes, "savebytesread: the code check: plain member Tell / Seek (disp8 / disp32) and up to 3 proxies accepted; another member, ret, "
                          "a null inner, 4 proxies, a forward to another slot, a mixed pair and a proxy over a ret Seek refused");
            // the game's own: the census type (exe+0x75CB5B8, forwarders over +0x90) over an FMemoryArchive-family vtable (exe+0x75CBB50,
            // Seek exe+0xD8B970 / Tell exe+0xD8CF50), build 19115871
            Obj gLeaf{reinterpret_cast<void**>(game.base + 0x75CBB50)}, gProxy{reinterpret_cast<void**>(game.base + 0x75CB5B8)};
            gProxy.inner = &gLeaf;
            Check(SaveBytesSeekableTest(&gProxy) && SaveBytesSeekableTest(&gLeaf),
                  "savebytesread: the game's save-load archive type (proxy exe+0x75CB5B8 over a memory reader exe+0x75CBB50) passes the code check");
        }

        // 5. Every condition that must leave the array to the game's function (a fake original sees the flag byte and counts calls).
        {
            auto rfalls = [&](auto change, int mode = 1, int32_t count = 1000, size_t payload = 1000) {
                Fakes f;
                f.ar.readMode = true;
                f.ar.flags[0] = 0x01;
                f.ar.src = makeSrc(0, count, payload, 0);
                change(f);
                uint8_t one = 1;
                SArr arr{&one, 1, 1};
                const uint8_t flagsBefore0 = f.ar.flags[0], flags1 = f.ar.flags[1];
                SaveBytesTestSetup(reinterpret_cast<void*>(&FakeOriginal), getArchive, byteFn, false, mode);
                g_fakeOrigCalls = 0;
                g_fakeSeenFlags = 0xFF;
                g_fakeAr = &f.ar;
                const size_t off0 = f.ar.off;
                SaveBytesTestCall(f.array, f.slot, &arr, false);
                g_fakeAr = nullptr;
                const bool untouched = f.ar.off == off0 && f.ar.flags[0] == flagsBefore0 && f.ar.flags[1] == flags1 && arr.num == 1 && arr.data == &one &&
                                       f.ar.countNum == 0;
                return g_fakeOrigCalls == 1 && g_fakeSeenFlags == flagsBefore0 && untouched;  // the game's function, flag byte as it was
            };
            auto rnone = [](Fakes&) {};
            // control: the plain case is taken: the original sees the unversioned bit, the flag byte is restored, CountBytes called
            {
                Fakes f;
                f.ar.readMode = true;
                f.ar.flags[0] = 0x01;
                f.ar.src = makeSrc(0, 1000, 1000, 0);
                uint8_t one = 1;
                SArr arr{&one, 1, 1};
                SaveBytesTestSetup(reinterpret_cast<void*>(&FakeOriginal), getArchive, byteFn, false, 1);
                g_fakeOrigCalls = 0;
                g_fakeSeenFlags = 0xFF;
                g_fakeAr = &f.ar;
                SaveBytesTestCall(f.array, f.slot, &arr, false);
                g_fakeAr = nullptr;
                Check(g_fakeOrigCalls == 1 && g_fakeSeenFlags == 0x41 && f.ar.flags[0] == 0x01 && f.ar.countNum == 1 && f.ar.countMax == 1 && f.ar.off == 0,
                      "savebytesread: a plain byte array in a loading archive: the game's function runs once with the unversioned bit set, the bit is put back, CountBytes called, the offset restored by the peek");
            }
            Check(rfalls(rnone, 0), "savebytesread: key off: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[0] = 0x00; }), "savebytesread: an archive that is not loading: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[0] = 0x05; }), "savebytesread: saving (and loading): the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[0] = 0x11; }), "savebytesread: a text archive: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[0] = 0x41; }), "savebytesread: unversioned serialization: the game's function (its bulk path is already on)");
            Check(rfalls([](Fakes& f) { f.ar.flags[0] = 0x09; }), "savebytesread: a transacting archive: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[1] = 0x02; }), "savebytesread: an archive with the error bit set: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[2] = 0x01; }), "savebytesread: byte swapping: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[3] = 0x80; }), "savebytesread: a custom property list: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.flags[3] = 0x04; }), "savebytesread: a counting-memory archive: the game's function");
            static uint8_t fbuf[16];
            Check(rfalls([](Fakes& f) { f.fplb[0] = reinterpret_cast<uintptr_t>(fbuf); f.fplb[1] = reinterpret_cast<uintptr_t>(fbuf + 16); }),
                  "savebytesread: a fast-path load buffer with room: the game's function");
            Check(rfalls([](Fakes& f) { f.ar.totalKnown = false; }), "savebytesread: TotalSize unknown (-1): the game's function");
            Check(rfalls([](Fakes& f) { f.ar.src.resize(3); }), "savebytesread: fewer than 4 bytes left: the game's function");
            // an archive whose Tell / Seek are recognised but whose Serialize does not move the member Tell reads: the post-peek
            // check finds it, the game's function runs without the bit, the type is remembered (no peek the second time)
            {
                static void* liarVt[80];
                memcpy(liarVt, g_archiveVtable, sizeof liarVt);
                liarVt[0x170 / 8] = reinterpret_cast<void*>(&SerializeOwnCursor);
                Fakes f;
                f.ar.readMode = true;
                f.ar.flags[0] = 0x01;
                f.ar.src = makeSrc(0, 1000, 1000, 0);
                f.ar.vtable = liarVt;
                uint8_t one = 1;
                SArr arr{&one, 1, 1};
                SaveBytesTestSetup(reinterpret_cast<void*>(&FakeOriginal), getArchive, byteFn, false, 1);
                g_fakeOrigCalls = 0;
                g_fakeSeenFlags = 0xFF;
                g_fakeAr = &f.ar;
                SaveBytesTestCall(f.array, f.slot, &arr, false);
                const size_t peekCalls = f.ar.calls.size();
                SaveBytesTestCall(f.array, f.slot, &arr, false);
                g_fakeAr = nullptr;
                Check(g_fakeOrigCalls == 2 && g_fakeSeenFlags == 0x01 && peekCalls == 1 && f.ar.calls.size() == 1 && f.ar.off == 0,
                      "savebytesread: an archive whose Tell does not move over the peek: found after it, the game's function runs without the bit, the type is remembered (no peek the second time)");
            }
            Check(rfalls(rnone, 1, 511, 511), "savebytesread: 511 bytes: the game's function (below the threshold)");
            Check(rfalls(rnone, 1, -7, 1000), "savebytesread: a negative count: the game's function");
            Check(rfalls(rnone, 1, 1001, 1000), "savebytesread: a count one longer than the data left: the game's function");
            Check(rfalls(rnone, 1, 0x7FFFFFF0, 1000), "savebytesread: a huge count: the game's function");
            Check(rfalls([](Fakes& f) { *reinterpret_cast<void**>(f.inner + 0x70) = f.cls; }), "savebytesread: a byte property with an enum: the game's function");
            Check(rfalls([](Fakes& f) { *reinterpret_cast<int32_t*>(f.inner + 0x34) = 2; }), "savebytesread: element size 2: the game's function");
            Check(rfalls([](Fakes& f) { *reinterpret_cast<uint64_t*>(f.cls + 0x10) = 0x40; }), "savebytesread: byte property that is not numeric: the game's function");
            Check(rfalls([](Fakes& f) { *reinterpret_cast<uint64_t*>(f.cls + 0x10) = 0x1000000; }), "savebytesread: numeric but not a byte property: the game's function");
            Check(rfalls([](Fakes& f) { *reinterpret_cast<void***>(f.inner) = g_archiveVtable; }), "savebytesread: an inner property with another SerializeItem: the game's function");
            Check(rfalls([](Fakes& f) { f.array[0x70] = 1; }), "savebytesread: an array with the memory-image allocator: the game's function");
        }
        if (writable) {
            *memsetSlot = savedMemset;
            DWORD tmp = 0;
            VirtualProtect(memsetSlot, sizeof(void*), oldProt, &tmp);
        }
    }
    SaveBytesTestSetup(arrayFn, getArchive, byteFn, false);
    LogSaveBytesStats();
}

}  // namespace svb
