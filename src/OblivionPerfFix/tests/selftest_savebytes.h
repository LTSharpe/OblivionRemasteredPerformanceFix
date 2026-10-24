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
    uint8_t pad78[0x80];
    std::vector<uint8_t> bytes;
    std::vector<int64_t> calls;  // Serialize lengths
    size_t countNum = 0, countMax = 0;
    int pushes = 0, pops = 0, unexpected = 0;
};
static_assert(offsetof(Archive, flags) == 0x28 && offsetof(Archive, ueVer) == 0x40 && offsetof(Archive, customList) == 0x60 &&
                  offsetof(Archive, serializedProp) == 0x70, "archive layout");

void Unexpected(Archive* a) { ++a->unexpected; }
void CountBytes(Archive* a, size_t num, size_t max) { a->countNum += num; a->countMax += max; }
int64_t Tell(Archive* a) { return static_cast<int64_t>(a->bytes.size()); }
void Serialize(Archive* a, const uint8_t* data, int64_t n)
{
    a->calls.push_back(n);
    a->bytes.insert(a->bytes.end(), data, data + n);
}
void Seek(Archive*, int64_t) {}
void Push(Archive* a, void*, bool) { ++a->pushes; }
void Pop(Archive* a, void*, bool) { ++a->pops; }

void* g_archiveVtable[80];
void* g_innerVtable[48];
int g_fakeOrigCalls = 0;
int MinAlignment() { return 1; }
void FakeOriginal(void*, void*, void*, const void*) { ++g_fakeOrigCalls; }

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
    for (auto& v : g_archiveVtable) v = reinterpret_cast<void*>(&Unexpected);
    g_archiveVtable[0x10 / 8] = reinterpret_cast<void*>(&CountBytes);
    g_archiveVtable[0x28 / 8] = reinterpret_cast<void*>(&Tell);
    g_archiveVtable[0x170 / 8] = reinterpret_cast<void*>(&Serialize);
    g_archiveVtable[0x1A0 / 8] = reinterpret_cast<void*>(&Seek);
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
    Check(!InstallSaveBytes(false, true, true), "savebytes: refuses to install after game code started");
    Check(!InstallSaveBytes(true, false, false), "savebytes: nothing installed with both keys off");
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
    Check(InstallSaveBytes(true, true, true), "savebytes: array/byte SerializeItem, SaveGameToMemory and its calls found, installed");
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
    SaveBytesTestSetup(arrayFn, getArchive, byteFn, false);
    LogSaveBytesStats();
}

}  // namespace svb
