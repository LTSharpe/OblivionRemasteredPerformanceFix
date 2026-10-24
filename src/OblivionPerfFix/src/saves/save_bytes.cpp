// SaveBytesBulk: the save's byte arrays written in one piece (and, in the lab build, SaveStallTimers: timers around the
// parts of a save that run on the game thread).
//
// Every save (quicksave, autosave on doors / fast travel / rest / wait, manual save) writes the save list
// saves_meta.sav with UGameplayStatics::SaveGameToSlot on the game thread (Altar.Save.AsyncMetadataSave is off), twice:
// once as the save starts and once when it has been written. SaveGameToMemory serialises the list object with tagged
// properties, and in this build (2026-10-19, worker savestall) ~1.6 MB of it are TArray<uint8> (each save's thumbnail).
// FArrayProperty::SerializeItem (exe+0x10BB690 in build 19115871) only writes a numeric array in one call when the
// archive uses unversioned property serialisation; a tagged save goes through Inner->SerializeItem once per element,
// and FByteProperty::SerializeItem (exe+0x10BC190) does Ar.Serialize(&byte, 1) per byte: ~20-30 ms per metadata save
// (stack samples, 480p and every resolution alike), so ~50 ms of game-thread time per save.
//
// The fix (SaveBytesBulk=1): only inside SaveGameToMemory (its call sites are redirected; a thread-local depth marks the
// scope) and only for an array of plain bytes (inner property is FByteProperty without an enum, whose SerializeItem is
// the game's own function, element size 1, an ordinary TArray) written to a plain binary saving archive (saving, not
// loading, not text, not unversioned, no byte swapping, no custom property list, no fast-path load buffer), the array is
// written as the stock code writes it, but in one piece: the int32 count through Serialize(&n, 4) (as stock), CountBytes
// (as stock), then Serialize(data, n) instead of n times Serialize(&data[i], 1). For any archive that appends what it is
// given (FMemoryWriter behind FObjectAndNameAsStringProxyArchive, as SaveGameToMemory builds it) the bytes are the same.
// Everything else goes to the game's function. The self-test runs the game's own function and the hook on the same
// arrays into a recording archive and compares every call.
//
// Installed only while the game starts and only when the key is on (call sites); the two vtable slots of
// FArrayProperty::SerializeItem are pointer writes; then live (0 = byte by byte, as the game). The screenshot read-back
// call in ProcessScreenShots is redirected here for SaveThumbnailAsync (save_thumb.cpp), installed when that key is on.
#include "common.h"

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace opf {
namespace {

// FArrayProperty::SerializeItem's entry (build 19115871, exe+0x10BB690): the home store of the slot, the frame, the
// cookie, mov r13,rcx, the two GetUnderlyingArchive calls, and mov rcx,[r13+78h] (Inner).
constexpr char kArraySerialize[] =
    "48 89 54 24 10 55 53 56 57 41 54 41 55 41 57 48 8D AC 24 20 FF FF FF 48 81 EC E0 01 00 00 48 8B 05 ?? ?? ?? ?? "
    "48 33 C4 48 89 85 C8 00 00 00 4C 8B E9 48 89 4C 24 60 48 8D 8D 28 01 00 00 4C 89 4C 24 58 4D 8B F9 4D 8B E0 "
    "E8 ?? ?? ?? ?? 48 8D 8D 28 01 00 00 48 8B F0 E8 ?? ?? ?? ?? 49 8B 4D 78";
constexpr size_t kGetArchiveCall = 0x49;  // E8 rel32: FStructuredArchiveSlot::GetUnderlyingArchive (mov rax,[rcx]; mov rax,[rax]; mov rax,[rax+8]; ret)
// FByteProperty::SerializeItem's entry (exe+0x10BC190): ..., call GetUnderlyingArchive, cmp qword [rbx+70h],0 (Enum),
// mov rdi,rax, jz, then the enum path's virtual call.
constexpr char kByteSerialize[] =
    "48 89 5C 24 18 48 89 74 24 20 48 89 54 24 10 57 48 83 EC 30 48 8B D9 49 8B F0 48 8D 4C 24 48 E8 ?? ?? ?? ?? "
    "48 83 7B 70 00 48 8B F8 74 36 48 8B 10 48 8B C8 FF 52 78";
// UGameplayStatics::SaveGameToMemory (exe+0x31BF380): frame, rbx = OutSaveData, rdi = SaveGameObject, null check.
constexpr char kSaveToMemory[] =
    "48 89 5C 24 10 48 89 7C 24 18 55 48 8D AC 24 30 FF FF FF 48 81 EC D0 01 00 00 48 8B DA 48 8B F9 48 85 C9 0F 84";
// The read-back call in UGameViewportClient::ProcessScreenShots (exe+0x3222FA8: GetViewportScreenShot(Viewport, Bitmap,
// Rect), the non-HDR branch; the result goes into sil).
constexpr char kReadBackCall[] = "48 8D 55 A7 89 45 EF 8B 45 9B 89 45 F3 E8 ?? ?? ?? ?? 0F B6 F0 45 32 F6";
constexpr size_t kReadBackCallOff = 13;

constexpr size_t kSerializeItemSlot = 0xB8;   // FProperty::SerializeItem in the property vtables
constexpr size_t kArrayFlagsOff = 0x70;       // FArrayProperty::ArrayFlags (bit 0: UsesMemoryImageAllocator)
constexpr size_t kInnerOff = 0x78;            // FArrayProperty::Inner
constexpr size_t kElementSizeOff = 0x34;      // FProperty::ElementSize
constexpr size_t kEnumOff = 0x70;             // FByteProperty::Enum
constexpr size_t kFieldClassOff = 0x08;       // FField::ClassPrivate
constexpr size_t kCastFlagsOff = 0x10;        // FFieldClass::CastFlags
constexpr uint64_t kCastByteProperty = 0x40;  // CASTCLASS_FByteProperty
// FArchive: +0x08 ActiveFPLB (start, end), +0x28.. the flag bits the game's array code tests: +0x28 bit 0 loading,
// bit 2 saving, bit 4 text, bit 6 unversioned; +0x2A bit 0 byte swapping (the count's swap path); +0x2B bit 7 custom
// property list (with the list at +0x60). Vtable: +0x10 CountBytes, +0x170 Serialize.
constexpr size_t kFplbOff = 0x08, kFlags0 = 0x28, kFlags2 = 0x2A, kFlags3 = 0x2B;
constexpr size_t kCountBytesSlot = 0x10, kSerializeSlot = 0x170;

using ArraySerializeFn = void (*)(void* self, void* slot, void* value, const void* defaults);
using GetArchiveFn = uint8_t* (*)(void* const* slot);
using SaveToMemoryFn = bool (*)(void* saveGame, void* outBytes);
using ReadBackFn = bool (*)(void* viewport, void* bitmap, const void* rect);
using SerializeFn = void (*)(void* ar, void* data, int64_t length);
using CountBytesFn = void (*)(void* ar, size_t num, size_t max);

struct ScriptArray { uint8_t* data; int32_t num, max; };

ArraySerializeFn g_arrayOrig = nullptr;
GetArchiveFn g_getArchive = nullptr;
void* g_byteSerialize = nullptr;
SaveToMemoryFn g_saveToMemory = nullptr;
ReadBackFn g_readBack = nullptr;
bool g_installed = false;
bool g_bulkInstalled = false;  // SaveGameToMemory's calls and the vtable slots written
std::atomic<int> g_bulk{0};  // 0 off, 1 on, 2 on + verify (each SaveGameToMemory also runs the game's way and is compared)
std::atomic<bool> g_timers{false};
thread_local int t_depth = 0;
thread_local bool t_noBulk = false;
std::atomic<uint64_t> s_verified{0}, s_mismatch{0};
thread_local uint64_t t_bulkArrays = 0, t_bulkBytes = 0;
std::atomic<uint64_t> s_bulkArrays{0}, s_bulkBytes{0}, s_fallbacks{0}, s_saves{0}, s_saveTicks{0}, s_readBacks{0}, s_readBackTicks{0};
std::atomic<uint32_t> g_readBackThread{0};

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

template <class T> T Slot(void* object, size_t offset) { return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[offset / 8]); }

// FindAll with the search started at a rarer byte of the pattern (anchor): memchr stops less often (a pattern that starts
// with 48 stops ~9 million times in this exe); every hit is checked against the whole pattern.
std::vector<uint8_t*> FindAnchored(const Section& text, const char* pattern, size_t anchor)
{
    const Pattern full = ParsePattern(pattern);
    Pattern tail;
    tail.bytes.assign(full.bytes.begin() + static_cast<std::ptrdiff_t>(anchor), full.bytes.end());
    tail.care.assign(full.care.begin() + static_cast<std::ptrdiff_t>(anchor), full.care.end());
    std::vector<uint8_t*> out;
    for (uint8_t* h : FindAll(text, tail))
        if (h - anchor >= text.begin && MatchesAt(h - anchor, full)) out.push_back(h - anchor);
    return out;
}

// The array can be written in one piece with the same bytes as the game's per-element loop.
bool CanBulk(void* self, void* slot, uint8_t*& ar)
{
    if (t_depth <= 0 || t_noBulk || g_bulk.load(std::memory_order_relaxed) <= 0) return false;
    uint8_t* const p = static_cast<uint8_t*>(self);
    if (p[kArrayFlagsOff] & 1) return false;
    uint8_t* const inner = *reinterpret_cast<uint8_t**>(p + kInnerOff);
    if (!inner) return false;
    if ((*reinterpret_cast<void***>(inner))[kSerializeItemSlot / 8] != g_byteSerialize) return false;
    const uint8_t* const cls = *reinterpret_cast<uint8_t**>(inner + kFieldClassOff);
    if (!cls || !(*reinterpret_cast<const uint64_t*>(cls + kCastFlagsOff) & kCastByteProperty)) return false;
    if (*reinterpret_cast<void**>(inner + kEnumOff) != nullptr) return false;
    if (*reinterpret_cast<int32_t*>(inner + kElementSizeOff) != 1) return false;
    void* const slotValue = slot;
    ar = g_getArchive(&slotValue);
    if (!ar) return false;
    const uint8_t f0 = ar[kFlags0];
    if ((f0 & 0x55) != 0x04) return false;  // saving; not loading, text or unversioned
    if (ar[kFlags2] & 1) return false;      // byte swapping
    if (ar[kFlags3] & 0x80) return false;   // custom property list
    // No room in a fast-path load buffer (the game's count and element code would read from it): start + 1 > end.
    const uintptr_t* const fplb = *reinterpret_cast<const uintptr_t* const*>(ar + kFplbOff);
    if (!fplb || fplb[0] + 1 <= fplb[1]) return false;
    return true;
}

void ArraySerializeHook(void* self, void* slot, void* value, const void* defaults)
{
    uint8_t* ar = nullptr;
    if (!CanBulk(self, slot, ar)) {
        if (t_depth > 0) s_fallbacks.fetch_add(1, std::memory_order_relaxed);
        g_arrayOrig(self, slot, value, defaults);
        return;
    }
    ScriptArray* const arr = static_cast<ScriptArray*>(value);
    int32_t n = arr->num;
    Slot<SerializeFn>(ar, kSerializeSlot)(ar, &n, 4);
    Slot<CountBytesFn>(ar, kCountBytesSlot)(ar, static_cast<size_t>(arr->num), static_cast<size_t>(arr->max));
    if (arr->num > 0) Slot<SerializeFn>(ar, kSerializeSlot)(ar, arr->data, arr->num);
    ++t_bulkArrays;
    t_bulkBytes += static_cast<uint64_t>(arr->num);
    s_bulkArrays.fetch_add(1, std::memory_order_relaxed);
    s_bulkBytes.fetch_add(static_cast<uint64_t>(arr->num), std::memory_order_relaxed);
}

bool SaveToMemoryHook(void* saveGame, void* outBytes)
{
    ++t_depth;
    const uint64_t t0 = Ticks();
    const bool ok = g_saveToMemory(saveGame, outBytes);
    const uint64_t dt = Ticks() - t0;
    --t_depth;
    s_saves.fetch_add(1, std::memory_order_relaxed);
    s_saveTicks.fetch_add(dt, std::memory_order_relaxed);
    return ok;
}

bool ReadBackHook(void* viewport, void* bitmap, const void* rect)
{
    g_readBackThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const uint64_t t0 = Ticks();
    if (SaveThumbTakeOver(viewport, rect)) {  // SaveThumbnailAsync: the save's picture arrives later, off this thread
        return false;
    }
    const bool ok = g_readBack(viewport, bitmap, rect);
    const uint64_t dt = Ticks() - t0;
    s_readBacks.fetch_add(1, std::memory_order_relaxed);
    s_readBackTicks.fetch_add(dt, std::memory_order_relaxed);
    return ok;
}

bool Fail(const char* why)
{
    Log("[savestall] %s: SaveBytesBulk / SaveThumbnailAsync unavailable", why);
    return false;
}

// A thunk near the game's code: jmp [rip+0]; dq target.
uint8_t* MakeThunk(void* target)
{
    uint8_t* const region = AllocNearGame(16);
    if (!region) return nullptr;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &target, sizeof target);
    DWORD old;
    VirtualProtect(region, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 16);
    return region;
}

bool RedirectCall(uint8_t* call, uint8_t* thunk)
{
    const int64_t rel = thunk - (call + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const int32_t rel32 = static_cast<int32_t>(rel);
    return WriteCode(call + 1, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&rel32), 4));
}

}  // namespace

// A hit of the E8 scan is taken as a call of SaveGameToMemory only if it lies inside a function of the exe's unwind data
// (.pdata) and the caller tests the result right after the call (84 C0, test al,al, within the next 8 bytes): all four
// call sites of build 19115871 do (exe+0x48CC37C after a mov ecx,[rbx+0Ch]). Random bytes E8 xx xx xx xx whose rel32 lands
// on SaveGameToMemory (about 4e-4 per build) would be refused, so nothing is written inside another instruction.
bool SaveCallSiteValid(const GameModule& game, const uint8_t* call)
{
    if (!game.text.Contains(call) || !game.text.Contains(call + 5 + 8) || call[0] != 0xE8) return false;
    if (!PrimaryFunction(game, call)) return false;
    for (int i = 0; i < 7; ++i)
        if (call[5 + i] == 0x84 && call[6 + i] == 0xC0) return true;
    return false;
}

bool InstallSaveBytes(bool beforeGameCode, bool bulk, bool thumb)
{
    if (g_installed) return true;
    if (!bulk && !thumb) return false;
    const uint64_t install0 = Ticks();
    const GameModule& game = Game();
    std::vector<uint8_t*> arrayHits, byteHits, saveHits, readHits;
    if (bulk) {
        arrayHits = FindAnchored(game.text, kArraySerialize, 17);
        byteHits = FindAnchored(game.text, kByteSerialize, 22);
        saveHits = FindAnchored(game.text, kSaveToMemory, 13);
        if (arrayHits.size() != 1 || byteHits.size() != 1 || saveHits.size() != 1) {
            Log("[savestall] FArrayProperty::SerializeItem found %zu times, FByteProperty::SerializeItem %zu, SaveGameToMemory %zu "
                "(expected 1 each): SaveBytesBulk unavailable", arrayHits.size(), byteHits.size(), saveHits.size());
            bulk = false;
        }
    }
    if (thumb) {
        readHits = FindAnchored(game.text, kReadBackCall, 3);
        if (readHits.size() != 1) {
            Log("[savestall] the screenshot read-back call found %zu times (expected 1): SaveThumbnailAsync unavailable", readHits.size());
            thumb = false;
        }
    }
    if (!bulk && !thumb) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    uint8_t* arrayFn = nullptr;
    uint8_t* getArchive = nullptr;
    uint8_t* saveFn = nullptr;
    std::vector<void**> slots;
    std::vector<uint8_t*> calls;
    double scanMs = 0;
    if (bulk) {
        arrayFn = arrayHits[0];
        getArchive = RipTarget(arrayFn + kGetArchiveCall, 1, 5);
        static const uint8_t kGetArchiveBody[] = {0x48, 0x8B, 0x01, 0x48, 0x8B, 0x00, 0x48, 0x8B, 0x40, 0x08, 0xC3};
        // The vtable slots holding FArrayProperty::SerializeItem at +0xB8 (FArrayProperty and one subclass in this build),
        // checked against the FByteProperty vtables' layout: the slot before is a code pointer, as is the slot after.
        if (game.text.Contains(getArchive) && memcmp(getArchive, kGetArchiveBody, sizeof kGetArchiveBody) == 0) {
            for (uint8_t* q = game.rdata.begin; q + 8 <= game.rdata.end; q += 8) {
                void** const s = reinterpret_cast<void**>(q);
                if (*s != arrayFn) continue;
                if (!game.text.Contains(s[-1]) || !game.text.Contains(s[1])) continue;
                slots.push_back(s);
            }
            if (slots.empty() || slots.size() > 4) {
                Log("[savestall] %zu vtable slots hold FArrayProperty::SerializeItem (expected 1-4): SaveBytesBulk unavailable", slots.size());
                bulk = false;
            }
        } else {
            Log("[savestall] GetUnderlyingArchive is not as expected: SaveBytesBulk unavailable");
            bulk = false;
        }
    }
    if (bulk) {
        // SaveGameToMemory's direct calls (E8 rel32 to it) anywhere in the code: one SSE2 pass over E8 (1.7 million stops in
        // this build; 16 bytes per step; the byte-by-byte loop before took 60-150 ms). The four call sites of build 19115871
        // (exe+0x3198FC4 AsyncSaveGameToSlot, 0x31BF6B6 SaveGameToSlot = the save list, 0x48B23D5 and 0x48CC37C in the save's
        // own tasks) could also be found by four byte patterns, but each pattern scan is itself a ~10 ms pass over the code,
        // and this pass also finds the calls of a later build whatever their surroundings. Each hit is checked
        // (SaveCallSiteValid); a refused one is left alone (its saves are then written the game's way).
        saveFn = saveHits[0];
        const uint64_t scan0 = Ticks();
        std::vector<uint8_t*> hits;
        {
            // 16 bytes at a time: the E8 positions of each block by SSE2 compare, then each one's rel32 checked
            const __m128i e8 = _mm_set1_epi8(static_cast<char>(0xE8));
            uint8_t* p = game.text.begin;
            for (; p + 16 + 4 <= game.text.end; p += 16) {
                unsigned m = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), e8)));
                while (m) {
                    unsigned long bit;
                    _BitScanForward(&bit, m);
                    m &= m - 1;
                    uint8_t* const c = p + bit;
                    int32_t rel;
                    memcpy(&rel, c + 1, 4);
                    if (c + 5 + rel == saveFn) hits.push_back(c);
                }
            }
            for (; p + 5 <= game.text.end; ++p) {
                int32_t rel;
                memcpy(&rel, p + 1, 4);
                if (*p == 0xE8 && p + 5 + rel == saveFn) hits.push_back(p);
            }
        }
        scanMs = Ms(Ticks() - scan0);
        for (uint8_t* c : hits) {
            if (SaveCallSiteValid(game, c)) calls.push_back(c);
            else Log("[savestall] a call-like byte sequence to SaveGameToMemory at exe+0x%X is not a checked call site: left alone",
                     static_cast<unsigned>(c - game.base));
        }
        if (calls.empty() || calls.size() > 8) {
            Log("[savestall] %zu checked calls to SaveGameToMemory (expected 1-8): SaveBytesBulk unavailable", calls.size());
            bulk = false;
        }
    }
    if (!bulk && !thumb) return false;
    uint8_t* const saveThunk = bulk ? MakeThunk(reinterpret_cast<void*>(&SaveToMemoryHook)) : nullptr;
    uint8_t* const readThunk = thumb ? MakeThunk(reinterpret_cast<void*>(&ReadBackHook)) : nullptr;
    if ((bulk && !saveThunk) || (thumb && !readThunk)) return Fail("no free memory near the game's code");
    if (bulk) {
        g_arrayOrig = reinterpret_cast<ArraySerializeFn>(arrayFn);
        g_getArchive = reinterpret_cast<GetArchiveFn>(getArchive);
        g_byteSerialize = byteHits[0];
        g_saveToMemory = reinterpret_cast<SaveToMemoryFn>(saveFn);
        for (uint8_t* c : calls)
            if (!RedirectCall(c, saveThunk)) return Fail("a SaveGameToMemory call could not be redirected");
    }
    bool thumbOn = false;
    if (thumb) {
        uint8_t* const call = readHits[0] + kReadBackCallOff;
        uint8_t* const target = RipTarget(call, 1, 5);
        if (game.text.Contains(target) && RedirectCall(call, readThunk)) {
            g_readBack = reinterpret_cast<ReadBackFn>(target);
            thumbOn = InstallSaveThumbAsync(call, target);  // SaveThumbnailAsync (save_thumb.cpp)
        } else Log("[savestall] the screenshot read-back call could not be redirected: SaveThumbnailAsync unavailable");
    }
    if (bulk)
        for (void** s : slots)
            if (!WritePointer(s, reinterpret_cast<void*>(&ArraySerializeHook))) return Fail("a vtable slot could not be written");
    g_bulkInstalled = bulk;
    g_installed = bulk || g_readBack;
    if (bulk)
        Log("[savestall] FArrayProperty::SerializeItem exe+0x%X (%zu vtable slots), FByteProperty::SerializeItem exe+0x%X, "
            "SaveGameToMemory exe+0x%X (%zu calls, found in %.1f ms): SaveBytesBulk installed",
            static_cast<unsigned>(arrayFn - game.base), slots.size(), static_cast<unsigned>(byteHits[0] - game.base),
            static_cast<unsigned>(saveFn - game.base), calls.size(), scanMs);
    Log("[savestall] read-back call %s; %.1f ms in all", g_readBack ? (thumbOn ? "redirected, SaveThumbnailAsync installed" : "redirected (timer only)")
                                                                    : "not redirected", Ms(Ticks() - install0));
    return g_installed;
}

void SetSaveBytes(int bulk, bool timers, int thumbAsync)
{
    if (!g_installed) return;
    SetSaveThumbAsync(thumbAsync, timers);
    bulk = bulk != 0;
    timers = false;
    if (g_bulk.exchange(bulk) != bulk && g_bulkInstalled)
        Log("[savestall] %s", bulk == 2 ? "SaveBytesBulk 2: in one piece, and every SaveGameToMemory also written the game's way and compared"
                              : bulk ? "SaveBytesBulk on: a save's byte arrays are written in one piece"
                                     : "SaveBytesBulk off: a save's byte arrays are written byte by byte, as before");
    if (g_timers.exchange(timers) != timers) Log("[savestall] SaveStallTimers %s", timers ? "on" : "off");
}

void LogSaveBytesStats()
{
    const uint64_t saves = s_saves.exchange(0), ticks = s_saveTicks.exchange(0), arrays = s_bulkArrays.exchange(0),
                   bytes = s_bulkBytes.exchange(0), falls = s_fallbacks.exchange(0), reads = s_readBacks.exchange(0),
                   readTicks = s_readBackTicks.exchange(0), verified = s_verified.exchange(0), mismatch = s_mismatch.exchange(0);
    LogSaveThumbStats();
    if (verified || mismatch)
        Log("[savestall] verify: %llu SaveGameToMemory outputs identical, %llu different", static_cast<unsigned long long>(verified),
            static_cast<unsigned long long>(mismatch));
    if (saves || reads)
        Log("[savestall] since last report: %llu SaveGameToMemory calls (%.1f ms), %llu byte arrays (%llu bytes) in one piece, "
            "%llu other arrays the game's way; %llu screenshot read-backs (%.1f ms)",
            static_cast<unsigned long long>(saves), Ms(ticks), static_cast<unsigned long long>(arrays),
            static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(falls), static_cast<unsigned long long>(reads),
            Ms(readTicks));
}

// Self-test: the hook with an explicit original and inner SerializeItem (fakes, or the mapped game's functions).
void SaveBytesTestSetup(void* arrayOrig, void* getArchive, void* byteSerialize, bool bulk)
{
    g_arrayOrig = reinterpret_cast<ArraySerializeFn>(arrayOrig);
    g_getArchive = reinterpret_cast<GetArchiveFn>(getArchive);
    g_byteSerialize = byteSerialize;
    g_bulk = bulk ? 1 : 0;
}

void SaveBytesTestCall(void* self, void* slot, void* value, bool inSave)
{
    if (inSave) ++t_depth;
    ArraySerializeHook(self, slot, value, nullptr);
    if (inSave) --t_depth;
}

}  // namespace opf
