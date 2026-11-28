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
//
// SaveBytesBulkRead: the read side, in the same hook (the same two vtable slots). The Load Game page of the main menu (the
// click on a save and its confirmation) reads a save object on the game thread (UGameplayStatics::LoadGameFromSlot, a
// pre-tick delegate): the container's one byte array (2.5-6.3 MB) and the save list's 66 thumbnails (1.6 MB), each byte
// through FByteProperty::SerializeItem and one virtual Serialize(&b, 1): 176-201 ms per click (S-069). The game's own
// FArrayProperty::SerializeItem has a branch that reads a whole numeric array with one Serialize(data, n), but only for
// archives with unversioned property serialisation (Epic: UE 5.3 PropertyArray.cpp, `if (bUPS) ... CanBulkSerialize`; the
// same in 5.4-5.7 and main). The flag is read ONCE at the function's entry (the byte at archive+0x28, bit 6), so for a plain
// byte array in a plain loading archive the hook sets that bit for the one call, lets the game's own function do the count,
// the resize and the single Serialize, puts the bit back and makes the CountBytes call the per-element path makes. Before
// that it peeks the count (Tell, Serialize(&n, 4), Seek back) and refuses (the game's function, untouched) whatever is
// short, damaged or not an ordinary memory read: a bulk read of data that falls short copies nothing, the per-element loop
// copies what is there. The peek is only made on an archive whose Tell and Seek (followed through FArchiveProxy forwarders)
// are recognised from their machine code as the plain read and write of one position member (FMemoryArchive's), so the Seek
// back is certain before a byte is read; any other archive (a Seek that does nothing or only goes forward included) goes to
// the game's function untouched. After the peek Tell must have moved by 4 and be back after the Seek; a type that does not is
// left to the game from then on (defence in depth). The key acts on
// every versioned loading archive, not only the save load (the lab logs the archive types that take it). Install-time: four
// byte checks inside the game's function (the branch must be as disassembled): the key is tied to this build of the game.
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
// The read side: more archive bytes (+0x29 bit 1 error, +0x2B bit 2 counting memory) and vtable slots (+0x28 Tell, +0x30
// TotalSize, +0x1A0 Seek), the cast flag FNumericProperty, and the smallest array worth a peek (the per-element loop is
// under 30 us below it).
constexpr size_t kFlags1 = 0x29, kTellSlot = 0x28, kTotalSizeSlot = 0x30, kSeekSlot = 0x1A0;
constexpr uint64_t kCastNumericProperty = 0x1000000;  // CASTCLASS_FNumericProperty
constexpr int32_t kReadMinCount = 512;
constexpr uint8_t kFlagUnversioned = 0x40;            // archive +0x28 bit 6: ArUseUnversionedPropertySerialization

using ArraySerializeFn = void (*)(void* self, void* slot, void* value, const void* defaults);
using GetArchiveFn = uint8_t* (*)(void* const* slot);
using SaveToMemoryFn = bool (*)(void* saveGame, void* outBytes);
using ReadBackFn = bool (*)(void* viewport, void* bitmap, const void* rect);
using SerializeFn = void (*)(void* ar, void* data, int64_t length);
using CountBytesFn = void (*)(void* ar, size_t num, size_t max);
using TellFn = int64_t (*)(void* ar);  // Tell and TotalSize
using SeekFn = void (*)(void* ar, int64_t pos);

struct ScriptArray { uint8_t* data; int32_t num, max; };

ArraySerializeFn g_arrayOrig = nullptr;
GetArchiveFn g_getArchive = nullptr;
void* g_byteSerialize = nullptr;
SaveToMemoryFn g_saveToMemory = nullptr;
ReadBackFn g_readBack = nullptr;
bool g_installed = false;
bool g_bulkInstalled = false;  // SaveGameToMemory's calls and the vtable slots written
bool g_readInstalled = false;  // the four byte checks of the array function's bulk branch held, the vtable slots written
std::atomic<int> g_bulkRead{0};  // SaveBytesBulkRead: 0 off, 1 on (lab 2: on + each bulk read also done the game's way and compared)
std::atomic<int> g_bulk{0};  // 0 off, 1 on, 2 on + verify (each SaveGameToMemory also runs the game's way and is compared)
std::atomic<bool> g_timers{false};
thread_local int t_depth = 0;
thread_local bool t_noBulk = false;
std::atomic<uint64_t> s_verified{0}, s_mismatch{0};
thread_local uint64_t t_bulkArrays = 0, t_bulkBytes = 0;
std::atomic<uint64_t> s_bulkArrays{0}, s_bulkBytes{0}, s_fallbacks{0}, s_saves{0}, s_saveTicks{0}, s_readBacks{0}, s_readBackTicks{0};
std::atomic<uint32_t> g_readBackThread{0};
// SaveBytesBulkRead counters: arrays read in one piece (bytes, QPC ticks), and why a plain byte array of a loading archive went
// to the game's function (flags: not an ordinary tagged memory read; fastpath: a fast-path buffer with room; peek: no count to
// read or Tell / TotalSize unusable; size: below 512 or longer than the data left); anomalies: the array or the archive's error
// bit after the call differs from what was peeked (should stay 0).
std::atomic<uint64_t> s_readArrays{0}, s_readBytes{0}, s_readTicks{0}, s_rejFlags{0}, s_rejFast{0}, s_rejPeek{0}, s_rejSize{0},
                      s_readAnomalies{0}, s_readVerified{0}, s_readMismatch{0};
std::atomic<bool> s_anomalyLogged{false};
// Archives (by vtable) whose Tell did not move by 4 over the peek: left to the game from then on. Any thread may add one: the
// slot is claimed with fetch_add and published as an atomic pointer; readers look at the claimed slots only (review s069fix).
constexpr int kBadVtables = 8;
std::atomic<void*> g_badVtable[kBadVtables] = {};
std::atomic<int> g_numBadVtables{0};
std::atomic<uint64_t> s_rejDishonest{0}, s_rejSeek{0};
// Archive types (by vtable) whose Tell / Seek code was not recognised, logged once each (the first 8).
std::atomic<void*> g_unprovenLogged[kBadVtables] = {};
std::atomic<int> g_numUnprovenLogged{0};

bool InList(const std::atomic<void*>* list, const std::atomic<int>& num, const void* vt)
{
    const int n = std::min(num.load(std::memory_order_acquire), kBadVtables);
    for (int i = 0; i < n; ++i)
        if (list[i].load(std::memory_order_acquire) == vt) return true;
    return false;
}

// Adds vt (no lock: two threads adding the same type at once may both add it, which only costs a slot); false when full.
bool AddToList(std::atomic<void*>* list, std::atomic<int>& num, void* vt)
{
    const int k = num.fetch_add(1, std::memory_order_acq_rel);
    if (k >= kBadVtables) { num.store(kBadVtables, std::memory_order_release); return false; }
    list[k].store(vt, std::memory_order_release);
    return true;
}

// The peek is only made where putting the position back is certain BEFORE any byte is read (review S2-1): the archive's Tell
// and Seek, followed through FArchiveProxy forwarders, must end in the plain read and the plain write of one position member,
// as FMemoryArchive's (Epic, UE 5.3 MemoryArchive.h: Seek sets Offset, Tell returns it, both final) compile to; in this exe
// 30+ archive vtables share that Seek (exe+0xD8B970 in build 19115871). Recognised by the machine code, not by an address:
//   Tell  mov rax,[rcx+d] ; ret           48 8B 81 d32 C3  |  48 8B 41 d8 C3
//   Seek  mov [rcx+d],rdx ; ret           48 89 91 d32 C3  |  48 89 51 d8 C3   (the same d)
//   proxy mov rcx,[rcx+i] ; mov rax,[rcx] ; jmp [rax+slot]  (48 8B 89 i32 | 48 8B 49 i8) 48 8B 01 (48)? FF (60 s8 | A0 s32)
// A Seek that does nothing, only goes forward, or anything else is not recognised: the game's function, nothing read ahead.
// The bytes are compared in order and the first difference stops (nothing past a mismatching instruction is read).
bool PlainPositionMember(const uint8_t* c, bool load, int32_t& disp)
{
    if (c[0] != 0x48 || c[1] != (load ? 0x8B : 0x89)) return false;
    const uint8_t reg = load ? 0x01 : 0x11;  // modrm: rax <- [rcx+d] / [rcx+d] <- rdx
    if (c[2] == (0x80 | reg)) { memcpy(&disp, c + 3, 4); return c[7] == 0xC3; }
    if (c[2] == (0x40 | reg)) { disp = static_cast<int8_t>(c[3]); return c[4] == 0xC3; }
    return false;
}

bool ProxyForwarder(const uint8_t* c, int32_t& innerOff, int32_t& slot)
{
    if (c[0] != 0x48 || c[1] != 0x8B) return false;
    size_t i;
    if (c[2] == 0x89) { memcpy(&innerOff, c + 3, 4); i = 7; }
    else if (c[2] == 0x49) { innerOff = static_cast<int8_t>(c[3]); i = 4; }
    else return false;
    if (c[i] != 0x48 || c[i + 1] != 0x8B || c[i + 2] != 0x01) return false;
    i += 3;
    if (c[i] == 0x48) ++i;
    if (c[i] != 0xFF) return false;
    if (c[i + 1] == 0x60) { slot = c[i + 2]; return true; }
    if (c[i + 1] == 0xA0) { memcpy(&slot, c + i + 2, 4); return true; }
    return false;
}

constexpr int kMaxProxyDepth = 4;
bool SeekableByCodeUnguarded(const uint8_t* ar)
{
    for (int depth = 0; depth < kMaxProxyDepth && ar; ++depth) {
        const uint8_t* const* const vt = *reinterpret_cast<const uint8_t* const* const*>(ar);
        const uint8_t* const tell = vt[kTellSlot / 8];
        const uint8_t* const seek = vt[kSeekSlot / 8];
        int32_t dTell = 0, dSeek = 0;
        if (PlainPositionMember(tell, true, dTell)) return PlainPositionMember(seek, false, dSeek) && dTell == dSeek && dTell >= 8;
        int32_t iTell = 0, iSeek = 0, sTell = 0, sSeek = 0;
        if (!ProxyForwarder(tell, iTell, sTell) || !ProxyForwarder(seek, iSeek, sSeek)) return false;
        if (iTell != iSeek || iTell < 8 || sTell != static_cast<int32_t>(kTellSlot) || sSeek != static_cast<int32_t>(kSeekSlot)) return false;
        ar = *reinterpret_cast<const uint8_t* const*>(ar + iTell);
    }
    return false;
}

// No C++ objects here: __try needs a plain frame. Any fault while reading = not recognised.
bool SeekableByCode(const uint8_t* ar)
{
    __try {
        return SeekableByCodeUnguarded(ar);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}


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

// SaveBytesBulkRead: the array is a plain byte array (the same property as CanBulk, but both the byte and the numeric cast
// flag, so that the game's own bulk branch is certainly the one taken).
bool PlainByteArray(void* self)
{
    uint8_t* const p = static_cast<uint8_t*>(self);
    if (p[kArrayFlagsOff] & 1) return false;
    uint8_t* const inner = *reinterpret_cast<uint8_t**>(p + kInnerOff);
    if (!inner) return false;
    if ((*reinterpret_cast<void***>(inner))[kSerializeItemSlot / 8] != g_byteSerialize) return false;
    const uint8_t* const cls = *reinterpret_cast<uint8_t**>(inner + kFieldClassOff);
    if (!cls || (*reinterpret_cast<const uint64_t*>(cls + kCastFlagsOff) & (kCastByteProperty | kCastNumericProperty)) !=
                    (kCastByteProperty | kCastNumericProperty)) return false;
    if (*reinterpret_cast<void**>(inner + kEnumOff) != nullptr) return false;
    return *reinterpret_cast<int32_t*>(inner + kElementSizeOff) == 1;
}

// A loading archive of the ordinary kind: loading and not saving / transacting / text / unversioned; no error yet; no byte
// swapping; no custom property list; not counting memory.
bool PlainLoadingArchive(const uint8_t* ar)
{
    return (ar[kFlags0] & 0x5D) == 0x01 && !(ar[kFlags1] & 0x02) && !(ar[kFlags2] & 0x01) && !(ar[kFlags3] & 0x84);
}

// Sets the unversioned bit for the duration of one call to the game's function and puts it back (only that bit is written).
struct FlagFlip {
    uint8_t* flags;
    explicit FlagFlip(uint8_t* ar) : flags(ar + kFlags0) { *flags = static_cast<uint8_t>(*flags | kFlagUnversioned); }
    ~FlagFlip() { *flags = static_cast<uint8_t>(*flags & ~kFlagUnversioned); }
    FlagFlip(const FlagFlip&) = delete;
    FlagFlip& operator=(const FlagFlip&) = delete;
};


// One bulk read of a plain byte array, or false (nothing touched, the caller runs the game's function). mode 2 (lab): the
// array is also read the game's way and compared; the game's result is the one left.
bool TryBulkRead(void* self, void* slot, void* value, const void* defaults, int mode)
{
    if (!PlainByteArray(self)) return false;
    void* const slotValue = slot;
    uint8_t* const ar = g_getArchive(&slotValue);
    if (!ar) return false;
    if (!PlainLoadingArchive(ar)) {
        if ((ar[kFlags0] & 0x41) == 0x01) s_rejFlags.fetch_add(1, std::memory_order_relaxed);  // a versioned loading archive; not the cooked loads
        return false;
    }
    void* const vtable = *reinterpret_cast<void* const*>(ar);
    if (InList(g_badVtable, g_numBadVtables, vtable)) { s_rejDishonest.fetch_add(1, std::memory_order_relaxed); return false; }
    // No room in a fast-path load buffer (the game's count and element code would read from it): start + 1 > end.
    const uintptr_t* const fplb = *reinterpret_cast<const uintptr_t* const*>(ar + kFplbOff);
    if (!fplb || fplb[0] + 1 <= fplb[1]) { s_rejFast.fetch_add(1, std::memory_order_relaxed); return false; }
    // Only where the Seek back is certain from the archive's code, decided before anything is read (review S2-1).
    if (!SeekableByCode(ar)) {
        s_rejSeek.fetch_add(1, std::memory_order_relaxed);
        if (!InList(g_unprovenLogged, g_numUnprovenLogged, vtable) && AddToList(g_unprovenLogged, g_numUnprovenLogged, vtable))
            Log("[savestall] SaveBytesBulkRead: an archive type (vtable exe+0x%llX) whose Tell / Seek are not a plain position member: "
                "left to the game (nothing read ahead)", static_cast<unsigned long long>(static_cast<const uint8_t*>(vtable) - Game().base));
        return false;
    }
    // Peek the count: the game's function reads it itself from the same place after the Seek back.
    const int64_t pos = Slot<TellFn>(ar, kTellSlot)(ar);
    const int64_t total = Slot<TellFn>(ar, kTotalSizeSlot)(ar);  // -1 for an archive that does not know (the default)
    if (pos < 0 || total - pos < 4) { s_rejPeek.fetch_add(1, std::memory_order_relaxed); return false; }
    int32_t n = 0;
    Slot<SerializeFn>(ar, kSerializeSlot)(ar, &n, 4);
    const int64_t afterPeek = Slot<TellFn>(ar, kTellSlot)(ar);
    Slot<SeekFn>(ar, kSeekSlot)(ar, pos);
    if (afterPeek != pos + 4 || Slot<TellFn>(ar, kTellSlot)(ar) != pos) {
        // Not reachable for the archives recognised above unless their Serialize does not move the position their Tell reports
        // (UE's tagged-property loader measures every property as a Tell difference, so such an archive would be broken for stock
        // loads too). Defence in depth: remember the type, the game's function from then on.
        AddToList(g_badVtable, g_numBadVtables, vtable);
        s_rejDishonest.fetch_add(1, std::memory_order_relaxed);
        s_readAnomalies.fetch_add(1, std::memory_order_relaxed);
        Log("[savestall] SaveBytesBulkRead: an archive (vtable exe+0x%llX) did not move by 4 over the peek (Tell after the peek %lld, after the Seek %lld, "
            "wanted %lld + 4 / %lld): this archive type is left to the game from now on",
            static_cast<unsigned long long>(static_cast<const uint8_t*>(vtable) - Game().base), static_cast<long long>(afterPeek),
            static_cast<long long>(Slot<TellFn>(ar, kTellSlot)(ar)), static_cast<long long>(pos), static_cast<long long>(pos));
        return false;
    }
    if (n < kReadMinCount || static_cast<int64_t>(n) > total - pos - 4) { s_rejSize.fetch_add(1, std::memory_order_relaxed); return false; }
    ScriptArray* const arr = static_cast<ScriptArray*>(value);
    const uint64_t t0 = Ticks();
    {
        FlagFlip flip(ar);  // restored when the call returns
        g_arrayOrig(self, slot, value, defaults);
    }
    (void)mode;
    // The size notice the per-element path makes and the game's bulk branch skips.
    Slot<CountBytesFn>(ar, kCountBytesSlot)(ar, static_cast<size_t>(arr->num), static_cast<size_t>(arr->max));
    s_readArrays.fetch_add(1, std::memory_order_relaxed);
    s_readBytes.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    s_readTicks.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    if (arr->num != n || (ar[kFlags1] & 0x02)) {
        // Should not happen (the count was checked against the data left). The game's bulk branch allocates without
        // initialising: leave zeros, as the per-element path does for what it did not read, never heap garbage (review S2-2).
        if (arr->data && arr->num > 0) memset(arr->data, 0, static_cast<size_t>(arr->num));
        s_readAnomalies.fetch_add(1, std::memory_order_relaxed);
        if (!s_anomalyLogged.exchange(true))
            Log("[savestall] SaveBytesBulkRead anomaly: count peeked %d, array holds %d, archive error bit %d", n, arr->num, (ar[kFlags1] >> 1) & 1);
    }
    return true;
}

// The read side of the hook: true = the array has been read (in one piece, or the game's way when timing).
bool ReadArray(void* self, void* slot, void* value, const void* defaults)
{
    const int mode = g_bulkRead.load(std::memory_order_relaxed);
    return mode > 0 && TryBulkRead(self, slot, value, defaults, mode);
}

void ArraySerializeHook(void* self, void* slot, void* value, const void* defaults)
{
    uint8_t* ar = nullptr;
    if (!CanBulk(self, slot, ar)) {
        if (g_bulkRead.load(std::memory_order_relaxed) > 0 && ReadArray(self, slot, value, defaults)) return;
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
    CreateAheadDropAll(0);  // CreateAhead: no actor built ahead is held while a save is made (game thread only; no-op unless on)
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

// The four places inside FArrayProperty::SerializeItem (offsets from its start, build 19115871) that SaveBytesBulkRead relies on:
// the archive's flag byte read ONCE (movzx edi, byte [rax+28h]), the bUPS test on it (test dil,40h; je), the Inner class's cast
// flag test (bt rcx,18h: FNumericProperty = the game's CanBulkSerialize) and the bulk branch's one Serialize call
// (call [rax+170h]). Any other bytes: the function is not the one this was written for.
bool SaveBytesReadInstalled() { return g_readInstalled; }

bool SaveBytesReadBranchOk(const uint8_t* arrayFn)
{
    static const struct { size_t off; uint8_t len; uint8_t bytes[6]; } kChecks[] = {
        {0x66, 4, {0x0F, 0xB6, 0x78, 0x28}},
        {0xA7, 6, {0x40, 0xF6, 0xC7, 0x40, 0x0F, 0x84}},
        {0xB9, 5, {0x48, 0x0F, 0xBA, 0xE1, 0x18}},
        {0x221, 6, {0xFF, 0x90, 0x70, 0x01, 0x00, 0x00}},
    };
    for (const auto& c : kChecks)
        if (memcmp(arrayFn + c.off, c.bytes, c.len) != 0) return false;
    return true;
}

bool InstallSaveBytes(bool beforeGameCode, bool bulk, bool thumb, bool bulkRead)
{
    if (g_installed) return true;
    if (!bulk && !thumb && !bulkRead) return false;
    const uint64_t install0 = Ticks();
    const GameModule& game = Game();
    std::vector<uint8_t*> arrayHits, byteHits, saveHits, readHits;
    if (bulk || bulkRead) {
        arrayHits = FindAnchored(game.text, kArraySerialize, 17);
        byteHits = FindAnchored(game.text, kByteSerialize, 22);
        if (arrayHits.size() != 1 || byteHits.size() != 1) {
            Log("[savestall] FArrayProperty::SerializeItem found %zu times, FByteProperty::SerializeItem %zu "
                "(expected 1 each): SaveBytesBulk / SaveBytesBulkRead unavailable", arrayHits.size(), byteHits.size());
            bulk = false;
            bulkRead = false;
        }
    }
    if (bulk) {
        saveHits = FindAnchored(game.text, kSaveToMemory, 13);
        if (saveHits.size() != 1) {
            Log("[savestall] SaveGameToMemory found %zu times (expected 1): SaveBytesBulk unavailable", saveHits.size());
            bulk = false;
        }
    }
    if (bulkRead && !SaveBytesReadBranchOk(arrayHits[0])) {
        Log("[savestall] SaveBytesBulkRead unavailable: the array function's bulk branch is not as expected");
        bulkRead = false;
    }
    if (thumb) {
        readHits = FindAnchored(game.text, kReadBackCall, 3);
        if (readHits.size() != 1) {
            Log("[savestall] the screenshot read-back call found %zu times (expected 1): SaveThumbnailAsync unavailable", readHits.size());
            thumb = false;
        }
    }
    if (!bulk && !thumb && !bulkRead) return false;
    if (!beforeGameCode) return Fail("only installed while the game starts");
    uint8_t* arrayFn = nullptr;
    uint8_t* getArchive = nullptr;
    uint8_t* saveFn = nullptr;
    std::vector<void**> slots;
    std::vector<uint8_t*> calls;
    double scanMs = 0;
    if (bulk || bulkRead) {
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
                Log("[savestall] %zu vtable slots hold FArrayProperty::SerializeItem (expected 1-4): SaveBytesBulk / SaveBytesBulkRead unavailable", slots.size());
                bulk = false;
                bulkRead = false;
            }
        } else {
            Log("[savestall] GetUnderlyingArchive is not as expected: SaveBytesBulk / SaveBytesBulkRead unavailable");
            bulk = false;
            bulkRead = false;
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
    if (!bulk && !thumb && !bulkRead) return false;
    const bool arrays = bulk || bulkRead;
    uint8_t* const saveThunk = bulk ? MakeThunk(reinterpret_cast<void*>(&SaveToMemoryHook)) : nullptr;
    uint8_t* const readThunk = thumb ? MakeThunk(reinterpret_cast<void*>(&ReadBackHook)) : nullptr;
    if ((bulk && !saveThunk) || (thumb && !readThunk)) return Fail("no free memory near the game's code");
    if (arrays) {
        g_arrayOrig = reinterpret_cast<ArraySerializeFn>(arrayFn);
        g_getArchive = reinterpret_cast<GetArchiveFn>(getArchive);
        g_byteSerialize = byteHits[0];
    }
    if (bulk) {
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
    if (arrays)
        for (void** s : slots)
            if (!WritePointer(s, reinterpret_cast<void*>(&ArraySerializeHook))) return Fail("a vtable slot could not be written");
    g_bulkInstalled = bulk;
    g_readInstalled = bulkRead;
    g_installed = arrays || g_readBack;
    if (bulkRead)
        Log("[savestall] FArrayProperty::SerializeItem exe+0x%X: the bulk branch's four byte checks hold, SaveBytesBulkRead installed (%zu vtable slots)",
            static_cast<unsigned>(arrayFn - game.base), slots.size());
    if (bulk)
        Log("[savestall] FArrayProperty::SerializeItem exe+0x%X (%zu vtable slots), FByteProperty::SerializeItem exe+0x%X, "
            "SaveGameToMemory exe+0x%X (%zu calls, found in %.1f ms): SaveBytesBulk installed",
            static_cast<unsigned>(arrayFn - game.base), slots.size(), static_cast<unsigned>(byteHits[0] - game.base),
            static_cast<unsigned>(saveFn - game.base), calls.size(), scanMs);
    Log("[savestall] read-back call %s; %.1f ms in all", g_readBack ? (thumbOn ? "redirected, SaveThumbnailAsync installed" : "redirected (timer only)")
                                                                    : "not redirected", Ms(Ticks() - install0));
    return g_installed;
}

void SetSaveBytes(int bulk, bool timers, int thumbAsync, int bulkRead)
{
    if (!g_installed) return;
    SetSaveThumbAsync(thumbAsync, timers);
    bulk = bulk != 0;
    bulkRead = bulkRead != 0;
    timers = false;
    if (!g_readInstalled) bulkRead = 0;
    if (g_bulkRead.exchange(bulkRead) != bulkRead)
        Log("[savestall] %s", bulkRead == 2 ? "SaveBytesBulkRead 2: byte arrays read in one piece, and each also read the game's way and compared"
                              : bulkRead ? "SaveBytesBulkRead on: a save's byte arrays are read in one piece"
                                         : "SaveBytesBulkRead off: a save's byte arrays are read byte by byte, as before");
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
    {
        const uint64_t ra = s_readArrays.exchange(0), rb = s_readBytes.exchange(0), rt = s_readTicks.exchange(0), rf = s_rejFlags.exchange(0),
                       rq = s_rejFast.exchange(0), rp = s_rejPeek.exchange(0), rs = s_rejSize.exchange(0), an = s_readAnomalies.exchange(0),
                       rv = s_readVerified.exchange(0), rm = s_readMismatch.exchange(0);
        const uint64_t rd = s_rejDishonest.exchange(0), rk = s_rejSeek.exchange(0);
        if (ra || rf || rq || rp || rs || an || rd || rk)
            Log("[savestall] reads: %llu byte arrays (%llu bytes, %.1f ms) in one piece, left to the game: %llu (flags %llu, fast-path %llu, "
                "seek not plain %llu, peek %llu, size %llu, archive type refused %llu), %llu anomalies",
                static_cast<unsigned long long>(ra), static_cast<unsigned long long>(rb), Ms(rt), static_cast<unsigned long long>(rf + rq + rk + rp + rs + rd),
                static_cast<unsigned long long>(rf), static_cast<unsigned long long>(rq), static_cast<unsigned long long>(rk), static_cast<unsigned long long>(rp),
                static_cast<unsigned long long>(rs), static_cast<unsigned long long>(rd), static_cast<unsigned long long>(an));
        if (rv || rm)
            Log("[savestall] read verify: %llu arrays identical, %llu different", static_cast<unsigned long long>(rv), static_cast<unsigned long long>(rm));
    }
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
void SaveBytesTestSetup(void* arrayOrig, void* getArchive, void* byteSerialize, bool bulk, int bulkRead)
{
    g_arrayOrig = reinterpret_cast<ArraySerializeFn>(arrayOrig);
    g_getArchive = reinterpret_cast<GetArchiveFn>(getArchive);
    g_byteSerialize = byteSerialize;
    g_bulk = bulk ? 1 : 0;
    g_bulkRead = bulkRead;
    for (auto& v : g_badVtable) v.store(nullptr);
    g_numBadVtables = 0;
}

bool SaveBytesSeekableTest(const void* archive) { return SeekableByCode(static_cast<const uint8_t*>(archive)); }

void SaveBytesTestCall(void* self, void* slot, void* value, bool inSave)
{
    if (inSave) ++t_depth;
    ArraySerializeHook(self, slot, value, nullptr);
    if (inSave) --t_depth;
}

}  // namespace opf
