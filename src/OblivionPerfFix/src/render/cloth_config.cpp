// ClothConfigCache: Chaos cloth copies its whole configuration again every frame.
//
// UE 5.3's FClothSimulationProxy::Tick_GameThread (ChaosClothAsset plugin, behind every
// UChaosClothComponent: robes, capes, banners) runs InitializeConfigs on every simulated frame:
// FClothingSimulationConfig::Initialize(Component.PropertyCollections) for each of the proxy's two
// configs (cloth and solver). Initialize frees the simulation-side property collections and their
// facades and deep-copies every LOD's property collection again (allocations, two strings per
// property, a key map rebuild). In stock 5.3.2 nothing changes the component's collections at
// runtime except ResetConfigProperties (a rebuild) and the component's own ClearDirtyFlags, so
// nearly every copy reproduces what is already there. At the Cheydinhal chapel square this was
// ~9% of the game thread, the frame-rate limiter. UE 5.4-5.6 still copy every frame.
//
// We redirect that one call (the call in the InitializeConfigs loop; the config constructors and
// all other callers are untouched) to a wrapper that skips Initialize when the config already
// holds exactly what the copy would produce: the same number of LODs and, for every LOD, the five
// property arrays the simulation reads through FCollectionPropertyFacade (Key, LowValue, HighValue,
// StringValue, Flags) byte-identical between the component's collection and the config's copy.
// The facades' key map is built from the Key array alone, so it is equal too. Anything else (any
// difference, or a layout we cannot confirm) calls the original. The test is stateless, so it does
// not depend on object addresses (recreated proxies, reused memory, several configs or
// components), and dirty flags need no special rule: identical flags in, identical flags out (the
// solver reads values every frame and flags for animatable properties, both from the facade). The
// configs are only written here on the game thread while no cloth task runs; we only read at that
// point. After every real copy the comparison runs again and must find the config equal to its
// input (that is what a skip relies on); if it ever does not, the fix switches itself off. Any
// access fault while comparing also switches it off. Switched live from the ini.
//
// The comparison's time is almost all cache misses: every key and string value is its own small heap
// block, and a plain compare waits for one miss after another. So before comparing, a warm-up pass
// reads the same pointers level by level over all LODs of both sides (facades, then their arrays,
// then the strings) and prefetches each level before reading the next, so the misses overlap. It only
// prefetches; the decision is the unchanged comparison below, so what is skipped is exactly what was
// skipped before. A fault in the warm-up is ignored (the comparison then runs as it always did).
//
// Layout, verified from the game's code at start-up (and the pointers again on every call):
//   component: PropertyCollections TArray<TSharedPtr<FManagedArrayCollection>> (the call's second
//              argument), CollectionPropertyFacades TArray<TUniquePtr<FCollectionPropertyFacade>>
//              right after it (these are the views the component's ClearDirtyFlags writes through);
//   config:    PropertyCollections at +0x08, Properties (facade pointers) at +0x18;
//   facade:    views {T* data, int32 num}: Key +0x08 (FString), LowValue +0x18, HighValue +0x28
//              (FVector3f), StringValue +0x38 (FString), Flags +0x48 (uint8); collection at +0xA8.
#include "common.h"

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace opf {
namespace {

// ---- code patterns (addresses of build 19115871 in the comments) ----

// The InitializeConfigs loop in FClothSimulationProxy::Tick_GameThread (exe+0x53CEEF0, call at +0x0E):
//   mov rdx,[rdi+ClothComponent] ; mov rcx,[rbx] ; add rdx,PropertyCollections ; call Initialize ;
//   add rbx,8 ; cmp rbx,rsi ; jne
constexpr char kLoopPattern[] = "48 8B 57 ?? 48 8B 0B 48 81 C2 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 C3 08 48 3B DE 75 ??";
constexpr size_t kLoopInputDisp = 10, kLoopCall = 14;
constexpr size_t kLoopMaxDistance = 0x300;  // the loop lies this close after Tick_GameThread's entry

// UChaosClothComponent::TickComponent (exe+0x53CEE00): call Tick_GameThread, then
//   if (simulating && CollectionPropertyFacades.IsValidIndex(PredictedLOD)) CollectionPropertyFacades[LOD]->ClearDirtyFlags;
constexpr char kTickPattern[] =
    "E8 ?? ?? ?? ?? 48 63 8B ?? ?? ?? ?? 84 C0 74 ?? 85 C9 78 ?? 3B 8B ?? ?? ?? ?? 7D ?? "
    "48 8B 83 ?? ?? ?? ?? 48 8B 0C C8 48 85 C9 74 ?? E8";
constexpr size_t kTickFacadesNumDisp = 22, kTickFacadesDataDisp = 31, kTickClearCall = 44;
// FCollectionPropertyFacade::ClearDirtyFlags (exe+0x176D0C0): Flags view data at +0x48, num at +0x50.
constexpr char kClearPattern[] = "48 8B 41 48 45 33 C0 48 63 51 50";

// FClothingSimulationConfig::Initialize(const TArray<TSharedPtr<const FManagedArrayCollection>>&)
// (exe+0x53B50A0): PropertyCollections.Reset, Properties.Reset.
constexpr char kInitPattern[] =
    "48 89 5C 24 18 55 57 41 54 41 55 41 56 48 83 EC 30 48 8D 69 ?? 48 8B FA 8B 52 08 4C 8D 71 ?? 48 8B CD E8";
constexpr size_t kInitCollectionsDisp = 20, kInitPropertiesDisp = 30;
// ... MakeUnique<FCollectionPropertyMutableFacade>(PropertyCollection): new 0xB8 bytes, ctor(facade, &shared ptr).
constexpr char kInitFacadePattern[] = "B9 B8 00 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B D7 48 8B C8 E8";
constexpr size_t kInitFacadeCtorCall = 21, kInitSearch = 0x200;
// Facade constructor (exe+0x1762AC0): stores the collection pointer at +0xA8/+0xB0, then
// UpdateArrays(this) and RebuildKeyIndices (KeyIndices at +0x58, sized from KeyArray.Num at +0x10).
constexpr char kCtorSharedPattern[] = "48 89 81 A8 00 00 00 48 89 B1 B0 00 00 00";
constexpr char kCtorUpdatePattern[] = "48 8B CB 48 89 03 E8 ?? ?? ?? ?? 8B 73 10 48 8D 4B 58";
constexpr size_t kCtorUpdateCall = 6, kCtorSearch = 0x120;
// UpdateArrays (exe+0x178D240): Key = GetArray<FString> -> +0x08, LowValue / HighValue =
// GetArray<FVector3f> -> +0x18 / +0x28, StringValue = GetArray<FString> -> +0x38, then Flags -> +0x48.
constexpr char kUpdatePattern[] =
    "4C 8D 05 ?? ?? ?? ?? 4C 8B F1 48 8D 54 24 ?? E8 ?? ?? ?? ?? "
    "4C 8D 05 ?? ?? ?? ?? 49 8B CE 48 8D 54 24 ?? 0F 10 00 41 0F 11 46 08 E8 ?? ?? ?? ?? "
    "4C 8D 05 ?? ?? ?? ?? 49 8B CE 48 8D 54 24 ?? 0F 10 00 41 0F 11 46 18 E8 ?? ?? ?? ?? "
    "4C 8D 05 ?? ?? ?? ?? 49 8B CE 48 8D 54 24 ?? 0F 10 00 41 0F 11 46 28 E8 ?? ?? ?? ?? "
    "49 8B B6 A8 00 00 00 0F 10 00 41 0F 11 46 38";
constexpr size_t kUpdateCalls[4] = {15, 43, 71, 99};  // Key, LowValue, HighValue, StringValue getters
constexpr char kUpdateFlagsStore[] = "41 0F 11 46 48";
constexpr size_t kUpdatePatternSearch = 0x40, kUpdateSearch = 0x180;

// ---- layout (implied by the exact bytes of the patterns above) ----
constexpr size_t kFacadeCollection = 0xA8;
constexpr size_t kViewKey = 0x08, kViewLow = 0x18, kViewHigh = 0x28, kViewString = 0x38, kViewFlags = 0x48;

// Sanity bounds: anything larger is not a cloth config, so it is copied as before.
constexpr int32_t kMaxLods = 64, kMaxProperties = 4096, kMaxStringLength = 1 << 16;

struct UeArray { uint8_t* data; int32_t num; int32_t max; };    // TArray
struct UeShared { uint8_t* object; void* controller; };          // TSharedPtr
struct UeView { const uint8_t* data; int32_t num; };              // TConstArrayView
struct UeString { const wchar_t* data; int32_t num; int32_t max; };  // FString (num includes the terminator)

using InitializeFn = void (*)(void* config, const void* propertyCollections);

enum Result : int {
    kEqual,       // the config already holds what the copy would produce: skipped
    kDiffFlags,   // only flags differ (typically the frame after ClearDirtyFlags)
    kDiffValues,  // keys, values or strings differ
    kDiffShape,   // number of LODs or properties differs
    kUnverified,  // pointers or view sizes not as expected
    kDisabled,    // switched off in the ini, or after a fault
    kFault,       // access fault while comparing (the fix then stays off)
    kNumResults
};

struct State {
    uint8_t* callSite = nullptr;
    uint8_t* initialize = nullptr;
    uint8_t* stub = nullptr;
    uint8_t* stubRegion = nullptr;
    uint32_t inputOffset = 0;       // component: PropertyCollections
    uint32_t facadesOffset = 0;     // component: CollectionPropertyFacades
    size_t facadesFromInput = 0;
    size_t configCollections = 0;   // config: PropertyCollections
    size_t configProperties = 0;    // config: Properties
    InitializeFn original = nullptr;
    bool found = false;
    bool installed = false;
} g;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_broken{false};
std::atomic<bool> g_loggedUnverified{false};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_results[kNumResults] = {};
std::atomic<uint64_t> g_checked{0}, g_checkFailed{0};
std::atomic<int32_t> g_maxLods{0}, g_maxProperties{0};  // largest config seen after a copy (for the log)
// Warm-up before the comparison: 1 on (the shipped behaviour); lab: 0 = compare without it, 2 = verify
// (compare without, then warm up and compare again, count disagreements; the first result decides).
std::atomic<int> g_warm{1};
std::atomic<uint64_t> g_warmFaults{0}, g_verified{0}, g_verifyDiffs{0};
std::atomic<uint64_t> g_decideTicks{0}, g_decideCount{0};  // lab: TSC ticks spent deciding (warm-up + comparison)

uint32_t ReadU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }

const UeView& ViewAt(const uint8_t* facade, size_t offset) { return *reinterpret_cast<const UeView*>(facade + offset); }

// Byte-exact equality of two buffers, inline: the keys are ~100 short strings per LOD, and a CRT memcmp
// call per string was most of the comparison's cost (1.7% of the game thread at the chapel square).
// Reads exactly n bytes from each side.
inline bool SameBytes(const void* pa, const void* pb, size_t n)
{
    auto* a = static_cast<const uint8_t*>(pa);
    auto* b = static_cast<const uint8_t*>(pb);
    for (; n >= 16; a += 16, b += 16, n -= 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(x, y)) != 0xFFFF) return false;
    }
    if (n >= 8) {
        uint64_t x, y;
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
        if (x != y) return false;
        a += 8, b += 8, n -= 8;
    }
    for (; n; ++a, ++b, --n)
        if (*a != *b) return false;
    return true;
}

// 1 same, 0 different, -1 implausible.
int SameStrings(const UeView& a, const UeView& b, int32_t n)
{
    auto* x = reinterpret_cast<const UeString*>(a.data);
    auto* y = reinterpret_cast<const UeString*>(b.data);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t len = x[i].num;
        if (len < 0 || len > kMaxStringLength || y[i].num < 0 || y[i].num > kMaxStringLength) return -1;
        if (len != y[i].num) return 0;
        if (len == 0) continue;
        if (!x[i].data || !y[i].data) return -1;
        if (!SameBytes(x[i].data, y[i].data, static_cast<size_t>(len) * sizeof(wchar_t))) return 0;
    }
    return 1;
}

// a = the component's facade (input), b = the config's facade (current simulation copy).
int CompareFacades(const uint8_t* a, const uint8_t* b)
{
    static constexpr size_t kViews[5] = {kViewKey, kViewLow, kViewHigh, kViewString, kViewFlags};
    const int32_t na = ViewAt(a, kViewKey).num, nb = ViewAt(b, kViewKey).num;
    if (na < 0 || na > kMaxProperties || nb < 0 || nb > kMaxProperties) return kUnverified;
    for (size_t v : kViews)  // the attributes of one group all have the group's size
        if (ViewAt(a, v).num != na || ViewAt(b, v).num != nb) return kUnverified;
    if (na != nb) return kDiffShape;
    if (na == 0) return kEqual;
    for (size_t v : kViews)
        if (!ViewAt(a, v).data || !ViewAt(b, v).data) return kUnverified;

    const size_t n = static_cast<size_t>(na);
    int same = SameStrings(ViewAt(a, kViewKey), ViewAt(b, kViewKey), na);
    if (same < 0) return kUnverified;
    if (!same) return kDiffValues;
    if (!SameBytes(ViewAt(a, kViewLow).data, ViewAt(b, kViewLow).data, n * 12) ||
        !SameBytes(ViewAt(a, kViewHigh).data, ViewAt(b, kViewHigh).data, n * 12))
        return kDiffValues;
    same = SameStrings(ViewAt(a, kViewString), ViewAt(b, kViewString), na);
    if (same < 0) return kUnverified;
    if (!same) return kDiffValues;
    return SameBytes(ViewAt(a, kViewFlags).data, ViewAt(b, kViewFlags).data, n) ? kEqual : kDiffFlags;
}

int Compare(const uint8_t* config, const UeArray* in)
{
    const auto* inFacades = reinterpret_cast<const UeArray*>(reinterpret_cast<const uint8_t*>(in) + g.facadesFromInput);
    const auto* outCollections = reinterpret_cast<const UeArray*>(config + g.configCollections);
    const auto* outFacades = reinterpret_cast<const UeArray*>(config + g.configProperties);
    const int32_t lods = in->num;
    if (lods < 0 || lods > kMaxLods || inFacades->num != lods) return kUnverified;
    if (outCollections->num != lods || outFacades->num != lods) return kDiffShape;
    int result = kEqual;
    for (int32_t i = 0; i < lods; ++i) {
        const uint8_t* inCollection = reinterpret_cast<const UeShared*>(in->data)[i].object;
        const uint8_t* inFacade = reinterpret_cast<uint8_t* const*>(inFacades->data)[i];
        const uint8_t* outCollection = reinterpret_cast<const UeShared*>(outCollections->data)[i].object;
        const uint8_t* outFacade = reinterpret_cast<uint8_t* const*>(outFacades->data)[i];
        if (!inCollection || !inFacade || !outCollection || !outFacade) return kUnverified;
        // Each facade must view exactly its collection (this also confirms the layout at run time).
        if (*reinterpret_cast<uint8_t* const*>(inFacade + kFacadeCollection) != inCollection ||
            *reinterpret_cast<uint8_t* const*>(outFacade + kFacadeCollection) != outCollection)
            return kUnverified;
        const int r = CompareFacades(inFacade, outFacade);
        if (r == kDiffFlags) result = kDiffFlags;  // keep looking: another LOD may differ in values
        else if (r != kEqual) return r;
    }
    return result;
}

// No C++ objects in here, so the comparison can be guarded with SEH.
int CompareGuarded(const uint8_t* config, const UeArray* in)
{
    __try {
        return Compare(config, in);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return kFault;
    }
}

// ---- warm-up: prefetches what Compare will read; decides nothing ----

inline void PrefetchBytes(const void* p, size_t bytes)
{
    const uintptr_t end = reinterpret_cast<uintptr_t>(p) + bytes;
    for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~uintptr_t{63}; a < end; a += 64)
        _mm_prefetch(reinterpret_cast<const char*>(a), _MM_HINT_T0);
}

// Follows the same pointers as Compare, within the same bounds, but level by level over every LOD of
// both sides, so each level's cache misses are in flight together instead of one after another.
void Warm(const uint8_t* config, const UeArray* in)
{
    const auto* inFacades = reinterpret_cast<const UeArray*>(reinterpret_cast<const uint8_t*>(in) + g.facadesFromInput);
    const auto* outFacades = reinterpret_cast<const UeArray*>(config + g.configProperties);
    const int32_t lods = in->num;
    if (lods <= 0 || lods > kMaxLods || inFacades->num != lods || outFacades->num != lods) return;  // Compare decides at once
    const uint8_t* facades[2 * kMaxLods];
    int32_t n = 0;
    for (int32_t i = 0; i < lods; ++i) {  // 1: the facades (views at +0x08..+0x57, collection pointer at +0xA8)
        const uint8_t* a = reinterpret_cast<uint8_t* const*>(inFacades->data)[i];
        const uint8_t* b = reinterpret_cast<uint8_t* const*>(outFacades->data)[i];
        if (!a || !b) break;
        PrefetchBytes(a, kFacadeCollection + 8);
        PrefetchBytes(b, kFacadeCollection + 8);
        facades[n++] = a;
        facades[n++] = b;
    }
    static constexpr size_t kViews[5] = {kViewKey, kViewLow, kViewHigh, kViewString, kViewFlags};
    static constexpr size_t kElement[5] = {sizeof(UeString), 12, 12, sizeof(UeString), 1};
    for (int32_t k = 0; k < n; ++k) {  // 2: the five arrays
        const int32_t num = ViewAt(facades[k], kViewKey).num;
        if (num <= 0 || num > kMaxProperties) continue;
        for (int v = 0; v < 5; ++v) {
            const UeView& view = ViewAt(facades[k], kViews[v]);
            if (view.num == num && view.data) PrefetchBytes(view.data, static_cast<size_t>(num) * kElement[v]);
        }
    }
    for (int32_t k = 0; k < n; ++k) {  // 3: the keys' and string values' characters
        const int32_t num = ViewAt(facades[k], kViewKey).num;
        if (num <= 0 || num > kMaxProperties) continue;
        static constexpr size_t kStringViews[2] = {kViewKey, kViewString};
        for (size_t v : kStringViews) {
            const UeView& view = ViewAt(facades[k], v);
            if (view.num != num || !view.data) continue;
            const auto* s = reinterpret_cast<const UeString*>(view.data);
            for (int32_t i = 0; i < num; ++i)
                if (s[i].num > 0 && s[i].num <= kMaxStringLength && s[i].data)
                    PrefetchBytes(s[i].data, static_cast<size_t>(s[i].num) * sizeof(wchar_t));
        }
    }
}

// A fault here only means no warm-up: the guarded comparison then decides as it always did.
bool WarmGuarded(const uint8_t* config, const UeArray* in)
{
    __try {
        Warm(config, in);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// The skip decision: warm-up (mode 1), then the comparison. Lab mode 2 also compares without the
// warm-up first and counts any disagreement (the result without it decides).
int Decide(const uint8_t* config, const UeArray* in, int warm)
{
    int verify = -1;
    if (warm == 2) verify = CompareGuarded(config, in);
    if (warm && !WarmGuarded(config, in)) g_warmFaults.fetch_add(1, std::memory_order_relaxed);
    const int result = CompareGuarded(config, in);
    if (verify < 0) return result;
    g_verified.fetch_add(1, std::memory_order_relaxed);
    if (verify != result) g_verifyDiffs.fetch_add(1, std::memory_order_relaxed);
    return verify;
}

// Called instead of FClothingSimulationConfig::Initialize from the InitializeConfigs loop.
void InitializeHook(void* config, const void* propertyCollections)
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    int result = kDisabled;
    if (g_enabled.load(std::memory_order_relaxed) && !g_broken.load(std::memory_order_relaxed)) {
        result = Decide(static_cast<const uint8_t*>(config), static_cast<const UeArray*>(propertyCollections),
                        g_warm.load(std::memory_order_relaxed));
        if (result == kEqual) {
            g_results[kEqual].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (result == kFault) {
            if (!g_broken.exchange(true)) Log("[cloth] access fault while comparing a cloth config: fix switched off, configs are copied as before");
        } else if (result == kUnverified && !g_loggedUnverified.exchange(true)) {
            Log("[cloth] a cloth config did not have the expected layout: copied as before (logged once)");
        }
    }
    g_results[result].fetch_add(1, std::memory_order_relaxed);
    g.original(config, propertyCollections);

    // Self-check of what the skip relies on: right after a real copy the config must compare equal to
    // its input. If it ever does not, our notion of "equal" is not the copy's, so the fix switches off.
    if (result == kDiffFlags || result == kDiffValues || result == kDiffShape) {
        if (CompareGuarded(static_cast<const uint8_t*>(config), static_cast<const UeArray*>(propertyCollections)) == kEqual) {
            g_checked.fetch_add(1, std::memory_order_relaxed);
            // Equal right after the copy, so the layout was confirmed: record the size for the log.
            const auto* in = static_cast<const UeArray*>(propertyCollections);
            const auto* facades = reinterpret_cast<const UeArray*>(static_cast<const uint8_t*>(config) + g.configProperties);
            if (in->num > g_maxLods.load(std::memory_order_relaxed)) g_maxLods.store(in->num, std::memory_order_relaxed);
            for (int32_t i = 0; i < facades->num; ++i) {
                const int32_t props = ViewAt(reinterpret_cast<uint8_t* const*>(facades->data)[i], kViewKey).num;
                if (props > g_maxProperties.load(std::memory_order_relaxed)) g_maxProperties.store(props, std::memory_order_relaxed);
            }
        } else {
            g_checkFailed.fetch_add(1, std::memory_order_relaxed);
            if (!g_broken.exchange(true))
                Log("[cloth] a fresh copy did not compare equal to its input: fix switched off, configs are copied as before");
        }
    }
}

bool Find(const GameModule& game)
{
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    auto within = [&](const uint8_t* begin, size_t size, const Pattern& p) -> const uint8_t* {
        for (const uint8_t* at = begin; at + p.size() <= begin + size && at + p.size() <= game.text.end; ++at)
            if (MatchesAt(at, p)) return at;
        return nullptr;
    };

    const auto loops = FindAll(game.text, ParsePattern(kLoopPattern));
    if (loops.size() != 1) {
        Log("[cloth] InitializeConfigs loop found %zu times (expected 1): fix disabled", loops.size());
        return false;
    }
    uint8_t* const call = loops[0] + kLoopCall;
    uint8_t* const initialize = RipTarget(call, 1, 5);
    const uint32_t inputOffset = ReadU32(loops[0] + kLoopInputDisp);

    // The component's tick: same Tick_GameThread, facades array right after the property collections.
    const auto ticks = FindAll(game.text, ParsePattern(kTickPattern));
    if (ticks.size() != 1) {
        Log("[cloth] UChaosClothComponent::TickComponent found %zu times (expected 1): fix disabled", ticks.size());
        return false;
    }
    const uint8_t* const tickGameThread = RipTarget(ticks[0], 1, 5);
    const uint32_t facadesOffset = ReadU32(ticks[0] + kTickFacadesDataDisp);
    const uint32_t facadesNumOffset = ReadU32(ticks[0] + kTickFacadesNumDisp);
    const uint8_t* const clearDirty = RipTarget(ticks[0] + kTickClearCall, 1, 5);
    if (call < tickGameThread || call >= tickGameThread + kLoopMaxDistance) {
        Log("[cloth] loop at exe+0x%X is not in Tick_GameThread (exe+0x%X): fix disabled", rva(call), rva(tickGameThread));
        return false;
    }
    if (facadesOffset != inputOffset + 0x10 || facadesNumOffset != facadesOffset + 8) {
        Log("[cloth] component layout: collections +0x%X, facades +0x%X/+0x%X (expected collections + 0x10): fix disabled",
            inputOffset, facadesOffset, facadesNumOffset);
        return false;
    }
    if (!game.text.Contains(clearDirty) || !MatchesAt(clearDirty, ParsePattern(kClearPattern))) {
        Log("[cloth] ClearDirtyFlags at exe+0x%X not as expected (Flags view): fix disabled", rva(clearDirty));
        return false;
    }

    // Initialize: config layout and the facade it creates.
    if (!game.text.Contains(initialize) || !MatchesAt(initialize, ParsePattern(kInitPattern))) {
        Log("[cloth] Initialize at exe+0x%X not as expected: fix disabled", rva(initialize));
        return false;
    }
    const size_t configCollections = initialize[kInitCollectionsDisp];
    const size_t configProperties = initialize[kInitPropertiesDisp];
    const uint8_t* const facadeAlloc = within(initialize, kInitSearch, ParsePattern(kInitFacadePattern));
    if (configProperties != configCollections + 0x10 || !facadeAlloc) {
        Log("[cloth] Initialize: config arrays +0x%zX/+0x%zX, facade allocation %s: fix disabled", configCollections,
            configProperties, facadeAlloc ? "found" : "not found");
        return false;
    }
    const uint8_t* const ctor = RipTarget(facadeAlloc + kInitFacadeCtorCall, 1, 5);
    const uint8_t* const ctorUpdate = game.text.Contains(ctor) ? within(ctor, kCtorSearch, ParsePattern(kCtorUpdatePattern)) : nullptr;
    if (!ctorUpdate || !within(ctor, kCtorSearch, ParsePattern(kCtorSharedPattern))) {
        Log("[cloth] facade constructor at exe+0x%X not as expected: fix disabled", rva(ctor));
        return false;
    }
    const uint8_t* const update = RipTarget(ctorUpdate + kCtorUpdateCall, 1, 5);
    const Pattern updatePattern = ParsePattern(kUpdatePattern);
    const uint8_t* const views = game.text.Contains(update) ? within(update, kUpdatePatternSearch + updatePattern.size(), updatePattern) : nullptr;
    const uint8_t* getter[4] = {};
    if (views)
        for (int i = 0; i < 4; ++i) getter[i] = RipTarget(views + kUpdateCalls[i], 1, 5);
    const bool viewsOk = views && getter[0] == getter[3] && getter[1] == getter[2] && getter[0] != getter[1] &&
                         within(views + updatePattern.size(), kUpdateSearch, ParsePattern(kUpdateFlagsStore));
    if (!viewsOk) {
        Log("[cloth] facade UpdateArrays at exe+0x%X not as expected (view layout): fix disabled", rva(update));
        return false;
    }

    g.callSite = call;
    g.initialize = initialize;
    g.inputOffset = inputOffset;
    g.facadesOffset = facadesOffset;
    g.facadesFromInput = facadesOffset - inputOffset;
    g.configCollections = configCollections;
    g.configProperties = configProperties;
    g.found = true;
    Log("[cloth] InitializeConfigs call at exe+0x%X -> Initialize exe+0x%X; component collections +0x%X, facades +0x%X; "
        "config arrays +0x%zX/+0x%zX; facade views checked (ctor exe+0x%X, UpdateArrays exe+0x%X, ClearDirtyFlags exe+0x%X)",
        rva(call), rva(initialize), inputOffset, facadesOffset, configCollections, configProperties, rva(ctor), rva(update),
        rva(clearDirty));
    return true;
}

// A free 64 KB-aligned region within rel32 reach of the game's code.
uint8_t* AllocNear(size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const GameModule& game = Game();
    const uintptr_t base = reinterpret_cast<uintptr_t>(game.base);
    const uintptr_t end = base + game.sizeOfImage;
    constexpr uintptr_t kReach = 0x70000000;  // well inside +-2 GB from every byte of the image
    for (uintptr_t a = (end + gran - 1) & ~(gran - 1); a + size < base + kReach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_FREE && a + size <= regionEnd)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<uint8_t*>(p);
        a = (regionEnd + gran - 1) & ~(gran - 1);
    }
    for (uintptr_t a = (base & ~(gran - 1)) - gran; a > gran && end - a < kReach; a -= gran)
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return static_cast<uint8_t*>(p);
    return nullptr;
}

// Points the call at a stub "jmp [rip+0]; dq hook". The stub sits at the original target's address
// modulo 64 KB, so the new rel32 keeps its low 16 bits and only two bytes of the instruction change;
// when those lie in one aligned qword the switch is a single atomic write (safe while the game runs).
bool Redirect(uint8_t* call, void* hook, bool beforeGameCode)
{
    constexpr size_t kRegion = 0x20000;
    uint8_t* const original = RipTarget(call, 1, 5);
    uint8_t* const region = AllocNear(kRegion);
    if (!region) {
        Log("[cloth] no free memory near the game's code for the call stub: fix disabled");
        return false;
    }
    uint8_t* const stub = region + (reinterpret_cast<uintptr_t>(original) & 0xFFFF);
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, jmp, sizeof jmp);
    memcpy(stub + sizeof jmp, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, kRegion, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 14);

    const int64_t rel = stub - (call + 5);
    if (rel != static_cast<int32_t>(rel)) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[cloth] call stub out of reach: fix disabled");
        return false;
    }
    uint8_t insn[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(insn + 1, &rel32, sizeof rel32);
    size_t first = 5, last = 0;
    for (size_t i = 0; i < 5; ++i)
        if (insn[i] != call[i]) { first = std::min(first, i); last = i; }
    const uintptr_t lo = reinterpret_cast<uintptr_t>(call) + first, hi = reinterpret_cast<uintptr_t>(call) + last;
    const uintptr_t qword = lo & ~uintptr_t{7};
    bool ok;
    if (first == 5) {
        ok = true;  // unreachable: the stub is never the original target
    } else if (hi < qword + 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(qword);
        ok = VirtualProtect(reinterpret_cast<void*>(qword), 8, PAGE_EXECUTE_READWRITE, &old) != 0;
        if (ok) {
            const LONG64 before = *q;
            LONG64 after = before;
            for (size_t i = first; i <= last; ++i)
                reinterpret_cast<uint8_t*>(&after)[reinterpret_cast<uintptr_t>(call) + i - qword] = insn[i];
            ok = InterlockedCompareExchange64(q, after, before) == before;
            VirtualProtect(reinterpret_cast<void*>(qword), 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(qword), 8);
        }
    } else if (beforeGameCode) {
        ok = WriteCode(call, insn);
    } else {
        Log("[cloth] the call cannot be switched atomically while the game runs: fix disabled");
        ok = false;
    }
    if (!ok) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[cloth] writing the call at exe+0x%X failed: fix disabled", static_cast<unsigned>(call - Game().base));
        return false;
    }
    g.stub = stub;
    g.stubRegion = region;
    return true;
}

}  // namespace

bool ApplyClothConfigFix(bool beforeGameCode)
{
    if (g.installed) return true;
    const GameModule& game = Game();
    if (!g.found && !Find(game)) return false;
    g.original = reinterpret_cast<InitializeFn>(g.initialize);
    if (!Redirect(g.callSite, reinterpret_cast<void*>(&InitializeHook), beforeGameCode)) return false;
    g.installed = true;
    Log("[cloth] config cache installed (call stub at %p)", static_cast<void*>(g.stub));
    return true;
}

void SetClothConfigCacheEnabled(bool enabled)
{
    if (g_enabled.exchange(enabled) != enabled && g.installed)
        Log("[cloth] config cache %s", enabled ? "enabled" : "disabled (configs are copied every frame as before)");
}

void LogClothConfigStats()
{
    static uint64_t last[kNumResults] = {}, lastCalls = 0;
    const uint64_t calls = g_calls.load();
    if (!g.installed || calls == lastCalls) return;
    uint64_t d[kNumResults];
    for (int i = 0; i < kNumResults; ++i) {
        const uint64_t v = g_results[i].load();
        d[i] = v - last[i];
        last[i] = v;
    }
    static uint64_t lastChecked = 0, lastCheckFailed = 0;
    const uint64_t checked = g_checked.load(), checkFailed = g_checkFailed.load();
    Log("[cloth] config init since last report: %llu calls, %llu skipped (copy unchanged), %llu copied (%llu flags changed, "
        "%llu values changed, %llu LOD/size changed, %llu layout unverified, %llu fix off, %llu faults); re-check after copy: "
        "%llu equal, %llu different; largest config %d LODs x %d properties",
        calls - lastCalls, d[kEqual], calls - lastCalls - d[kEqual], d[kDiffFlags], d[kDiffValues], d[kDiffShape], d[kUnverified],
        d[kDisabled], d[kFault], checked - lastChecked, checkFailed - lastCheckFailed, g_maxLods.load(), g_maxProperties.load());
    lastCalls = calls;
    lastChecked = checked;
    lastCheckFailed = checkFailed;
    static uint64_t lastWarmFaults = 0;
    const uint64_t warmFaults = g_warmFaults.load();
    if (warmFaults != lastWarmFaults)
        Log("[cloth] %llu warm-up faults since last report (ignored: the comparison decided as before)", warmFaults - lastWarmFaults);
    lastWarmFaults = warmFaults;
}

void SetClothConfigWarm(int mode)
{
    mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    if (g_warm.exchange(mode) != mode && g.installed) Log("[cloth] comparison warm-up mode %d", mode);
}

int ClothConfigTestDecide(void* config, const void* propertyCollections, int warm)
{
    return Decide(static_cast<const uint8_t*>(config), static_cast<const UeArray*>(propertyCollections), warm);
}

bool ClothConfigFixInfo(ClothConfigInfo* out)
{
    if (!g.found) return false;
    out->callSite = g.callSite;
    out->initialize = g.initialize;
    out->stub = g.stub;
    out->hook = reinterpret_cast<void*>(&InitializeHook);
    out->inputOffset = g.inputOffset;
    out->facadesOffset = g.facadesOffset;
    out->configCollections = static_cast<uint32_t>(g.configCollections);
    out->configProperties = static_cast<uint32_t>(g.configProperties);
    return true;
}

ClothConfigStats GetClothConfigStats()
{
    ClothConfigStats s;
    s.calls = g_calls.load();
    s.skipped = g_results[kEqual].load();
    s.flags = g_results[kDiffFlags].load();
    s.values = g_results[kDiffValues].load();
    s.shape = g_results[kDiffShape].load();
    s.unverified = g_results[kUnverified].load();
    s.disabled = g_results[kDisabled].load();
    s.faults = g_results[kFault].load();
    s.checked = g_checked.load();
    s.checkFailed = g_checkFailed.load();
    s.warmFaults = g_warmFaults.load();
    s.verified = g_verified.load();
    s.verifyDiffs = g_verifyDiffs.load();
    return s;
}

void ClothConfigTestOriginal(void* fakeInitialize)
{
    g.original = reinterpret_cast<InitializeFn>(fakeInitialize);
}

}  // namespace opf
