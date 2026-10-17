// AtmosphereDirtyFilter: Altar's AtmosphericTool (weather / time of day) writes its blended values into the
// sun, moon, height fog, sky atmosphere and sky light every frame through reflection
// "accessors": Accessor->Set(entry, &value) followed by Accessor->PostSet(object), and PostSet
// always calls UActorComponent::MarkRenderStateDirty. So these components' render state is
// destroyed and recreated every frame, even when no value changed. For the sun that means
// FScene::RemoveLight + AddLight, and AddToScene loops over all ~13k primitives (about
// 20% of the render thread at the Cheydinhal cathedral).
//
// Stock UE setters only dirty the render state when the value actually changes. We do the same:
// the accessor vtables' Set and PostSet are wrapped; Set snapshots the target object
// (UClass::PropertiesSize bytes) and compares it after the original Set, and PostSet is skipped
// only when the immediately preceding Set on this thread, for the same accessor and object,
// changed nothing. Anything unexpected falls through to the original behaviour.
//
// AtmosphereLightFastPath, the light fast path: during time-of-day and weather blends the sun's Intensity, Temperature and
// IndirectLightingIntensity change every frame, so the filter forwards them and the sun is still
// rebuilt every frame (~25% of the render thread). Stock ULightComponent setters for exactly these
// fields do not rebuild the light: SetIntensity calls UpdateColorAndBrightness, and
// SetTemperature / SetIndirectLightingIntensity / SetVolumetricScatteringIntensity call
// Scene->UpdateLightColorAndBrightness, which pushes the new colour, indirect scale and
// volumetric scattering to the existing light. So when a Set changed nothing but those fields on a
// light component that may change at runtime, we call the game's own UpdateColorAndBrightness
// (stock: the fast update, or a full rebuild only when the light must be added to or removed from
// the scene) and skip PostSet. Every offset comes from the game's code:
//  - UpdateColorAndBrightness is found by pattern; it gives the Intensity offset and its fallback
//    call must be the same MarkRenderStateDirty that PostSet calls;
//  - the other fields are those whose setters have the stock "store, then
//    World->Scene->UpdateLightColorAndBrightness(this)" shape (same World/Scene offsets and the
//    same FScene vtable slot UpdateColorAndBrightness uses); they also give the Mobility offset;
//  - ULightComponent's UClass is the class that declares the UFunction whose native thunk contains
//    one of those setters; objects are tested with UE's IsChildOf fast path (struct base chain).
//
// The cloud path: the same blends also move the sun's and moon's CloudShadowOnSurfaceStrength and
// CloudScatteredLuminanceScale a little every few frames (Echo Mine -> Brotch Camp: ~800 and ~390 Sets a minute), so
// AtmosphereDirtyFilter forwarded those Sets and the light was still removed from and added to the scene (~10% of the render thread on
// that walk). UE 5.3 has no setter for these two: they live only in the render proxy (FDirectionalLightSceneProxy copies
// them in its constructor) and only the renderer reads them, through the proxy's getters, for the atmosphere lights.
// Every frame, before it renders anything that uses them, the renderer writes the sun's other atmosphere values into
// the same proxy on the render thread: FDeferredShadingSceneRenderer::Render calls
// Proxy->SetAtmosphereRelatedProperties for each atmosphere light (PrepareSunLightProxy, or
// ResetAtmosphereLightsProperties without a sky atmosphere). So when a Set changed nothing but these two (and the fast
// fields above) on a movable directional light with a proxy, the game thread leaves the two new values for that proxy
// and skips PostSet, and a wrapper of that virtual (the proxy vtable's slot) copies them into the proxy right after the
// renderer's own values. They reach the proxy at most a frame earlier than the rebuild would have put them there.
// Values the render thread has not picked up within kCloudLateUs (a light that is not an atmosphere light, or no frame
// rendered) are applied the stock way: the next Set rebuilds the light. A Set that rebuilds the light drops what was
// left for its proxy. A light that is not in the scene (no proxy: hidden, like the moon by day; its rebuild made none)
// has nothing to update: a proxy made later, when it is shown again (an intensity or visibility change, which rebuilds
// it), copies the values the component has then. A light with any other proxy is rebuilt as before. Found by shape:
//  - the proxy constructor's copies (exe+0x301A92A): mov eax,[rbx+c]; mov [rdi+p],eax seven times (CloudShadowExtent,
//    Strength, OnAtmosphereStrength, OnSurfaceStrength, DepthBias, MapResolutionScale, RaySampleCountScale in UE's
//    order), then movups xmm0,[rbx+c]; ...; movups [rdi+p],xmm0 (CloudScatteredLuminanceScale): component and proxy
//    offsets of the 4th copy and of the colour;
//  - the constructor's vtable store before them (lea rax,[vtable] ... mov [rdi],rax); that vtable must hold
//    SetAtmosphereRelatedProperties (movups xmm0,[rdx]; movups [rcx+a],xmm0; movups xmm0,[r8]; movups [rcx+b],xmm0;
//    ret) and the two getters of the proxy offsets found above; other vtables with that setter in the same slot and the
//    same getters (subclasses) are wrapped too;
//  - the component's SceneProxy: the pointer member whose object has one of those vtables (found on first use, checked
//    on every use).
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace opf {
namespace {

// The shared PostSet: if (Obj->IsA<UActorComponent>) Obj->MarkRenderStateDirty;  (exe+0x4574080)
constexpr char kPostSetPattern[] =
    "48 85 D2 74 ?? 53 48 83 EC 20 48 8B DA E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 30 48 63 40 38 "
    "3B 42 38 7F ?? 48 8B C8 48 8B 42 30 4C 39 04 C8 75 ?? 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 20 5B C3";

// Accessor vtable slots.
constexpr int kSlotSet = 2;
constexpr int kSlotPostSet = 3;
constexpr int kMaxAccessorVtables = 64;

// UE 5.3 layout: UObjectBase::ClassPrivate and UStruct::PropertiesSize (verified on this build:
// DirectionalLightComponent 0x500, ExponentialHeightFogComponent 0x3A0).
constexpr size_t kClassPrivate = 0x10;
constexpr size_t kPropertiesSize = 0x58;
constexpr int32_t kMinObjectSize = 0x30;
constexpr int32_t kMaxObjectSize = 0x4000;

using SetFn = void (*)(void* accessor, void** entry, const void* value);
using PostSetFn = void (*)(void* accessor, void* object);

struct HookedVtable {
    void** vtable;
    SetFn originalSet;
};
HookedVtable g_vtables[kMaxAccessorVtables];
int g_numVtables = 0;
PostSetFn g_originalPostSet = nullptr;

std::atomic<uint64_t> g_skipped{0}, g_forwarded{0}, g_unsized{0}, g_fastPath{0};
std::atomic<bool> g_enabled{true};  // switched live from the ini; off = original behaviour

// ---- light fast path ----

// ULightComponent::UpdateColorAndBrightness (exe+0x304C150 in build 19115871).
constexpr char kUcabPattern[] =
    "40 53 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 48 8B D9 48 85 C0 75 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? "
    "48 8B 88 ?? ?? ?? ?? 48 85 C9 74 ?? 8B 93 ?? ?? ?? ?? 48 8D 83 ?? ?? ?? ?? 0F 57 C0 83 E2 01 "
    "75 ?? 0F 2F 00 73 ?? 48 8B CB E8";
constexpr size_t kUcabWorldDisp = 9, kUcabSceneDisp = 34, kUcabIntensityDisp = 52, kUcabDirtyCall = 72;
constexpr char kUcabSceneCall[] = "48 8B 01 48 8B D3 FF 90";  // Scene->vtable[slot](Scene, this)
constexpr size_t kUcabSceneCallSearch = 0x60;
// PostSet's call to MarkRenderStateDirty.
constexpr size_t kPostSetDirtyCall = 51;

// Stock setter tail: if (allowed && Field != New) { Field = New; World->Scene->UpdateLightColorAndBrightness(this); }
// %s: World offset, Scene offset, FScene vtable slot (little-endian hex, from UpdateColorAndBrightness).
constexpr char kSceneSetterFmt[] =
    "38 83 ?? ?? ?? ?? 74 ?? 0F 2E B3 ?? ?? ?? ?? 74 ?? 48 8B 83 %s F3 0F 11 B3 ?? ?? ?? ?? "
    "48 85 C0 75 ?? 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 %s 48 85 C9 74 ?? "
    "48 8B 01 48 8B D3 FF 90 %s";
constexpr size_t kSetterMobilityDisp = 2, kSetterCompareDisp = 11, kSetterStoreDisp = 28;
// SetLightColor (exe+0x3045580): the same tail for the FColor - if (allowed && LightColor != New) { LightColor = New;
// World->Scene->UpdateLightColorAndBrightness(this); } and the UpdateColorAndBrightnessEvent broadcast, as at the end of
// UpdateColorAndBrightness. A weather blend moves the sun's colour (a thunderstorm: ~40 rebuilds per 5 s, 2026-10-13).
constexpr char kColorSetterFmt[] =
    "38 87 ?? ?? ?? ?? 74 ?? 39 9F ?? ?? ?? ?? 74 ?? 89 9F ?? ?? ?? ?? 48 8B 87 %s 48 85 C0 75 ?? 48 8B CF E8 ?? ?? ?? ?? "
    "48 85 C0 74 ?? 48 8B 88 %s 48 85 C9 74 ?? 48 8B 01 48 8B D7 FF 90 %s";
constexpr size_t kColorMobilityDisp = 2, kColorCompareDisp = 10, kColorStoreDisp = 18;
constexpr size_t kThunkSearch = 0x100;  // a setter inside an exec thunk starts this far in at most

// UE 5.3 UObject/UStruct layout (UObjectBase::ClassPrivate is kClassPrivate above).
constexpr size_t kStructBaseChain = 0x30;  // FStructBaseChain: array pointer, then depth (int32)
constexpr size_t kStructBaseDepth = 0x38;
constexpr size_t kSuperStruct = 0x40;
constexpr size_t kChildren = 0x48;         // UField* list of UFunctions
constexpr size_t kFieldNext = 0x28;
constexpr size_t kFuncScanBegin = 0x60, kFuncScanEnd = 0xE0;  // UFunction::Func (0xD8 in this build) lies in here
constexpr int kMaxClassDepth = 32, kMaxChildren = 2048;

using UcabFn = void (*)(void* lightComponent);

struct LightFast {
    UcabFn ucab = nullptr;
    int32_t mobility = 0;
    std::vector<uint32_t> fields;     // 4-byte fields with a stock fast-path setter
    std::vector<void*> setterThunks;  // native exec thunks containing such a setter
    std::atomic<void*> lightClass{nullptr};
    std::atomic<bool> enabled{true};
    bool ready = false;
} g_light;

uint32_t ReadU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }

// ---- cloud path ----

// FDirectionalLightSceneProxy's constructor (exe+0x301A780): the cloud fields' copies (exe+0x301A92A).
constexpr char kCloudCopies[] =
    "8B 83 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? "
    "8B 83 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? 8B 83 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? "
    "8B 83 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? 0F 10 83 ?? ?? ?? ?? 8B 87 ?? ?? ?? ?? 0F 11 87 ?? ?? ?? ??";
constexpr size_t kSurfaceSrc = 3 * 12 + 2, kSurfaceDst = 3 * 12 + 8;  // the 4th float copy
constexpr size_t kScaleSrc = 87, kScaleDst = 100;
constexpr size_t kVtableStoreSearch = 0x400;
// FDirectionalLightSceneProxy::SetAtmosphereRelatedProperties (exe+0x3043800, vtable exe+0x75E9400 slot +0x118)
constexpr char kSetAtmosphere[] = "0F 10 02 0F 11 81 ?? ?? ?? ?? 41 0F 10 00 0F 11 81 ?? ?? ?? ?? C3";
constexpr int kMaxProxySlots = 128, kMaxCloudVtables = 4, kCloudEntries = 8;
constexpr uint64_t kCloudLateUs = 250000;
constexpr int kMaxProxyScans = 64;  // SceneProxy look-ups on lights without a proxy before giving up
// FLightSceneInfo's constructor (exe+0x1F17260): the bloom values copied from the proxy's component
// (mov rax,[rdi+LightComponent]; mov ecx,[rax+c]; mov [rbx+i],ecx: BloomScale, BloomThreshold, BloomMaxBrightness, BloomTint)
constexpr char kBloomCopies[] =
    "48 8B 47 ?? 8B 88 ?? ?? ?? ?? 89 8B ?? ?? ?? ?? 48 8B 47 ?? 8B 88 ?? ?? ?? ?? 89 8B ?? ?? ?? ?? "
    "48 8B 47 ?? 8B 88 ?? ?? ?? ?? 89 8B ?? ?? ?? ?? 48 8B 47 ?? 8B 88 ?? ?? ?? ?? 89 8B ?? ?? ?? ??";
constexpr size_t kBloomStride = 16, kBloomSrc = 6, kBloomDst = 12, kBloomCtorSearch = 0x200;
// FScene::AddLight (exe+0x2261D40): Proxy->LightSceneInfo = new FLightSceneInfo(Proxy, true)
constexpr char kAddLightInfo[] = "41 B0 01 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? 48 89 83 ?? ?? ?? ??";
constexpr size_t kAddLightCall = 9, kAddLightStore = 17;
constexpr int kMaxCloudFields = 16;
constexpr uint32_t kMaxCloudBytes = 96;
constexpr size_t kCtorCopySearch = 0x240;  // the directional ctor's copies before the cloud run; the base ctor's body

using SetAtmosphereFn = void (*)(void* proxy, const void* transmittance, const void* sunDisc);

// A component field that only the render side copies and reads every frame: where the copy lives.
enum CloudTarget : uint8_t { kInProxy, kInLightInfo };
struct CloudField {
    const char* name = nullptr;
    uint32_t comp = 0, size = 0, dst = 0, slot = 0;  // slot: its bytes in CloudEntry::values
    CloudTarget target = kInProxy;
};

struct CloudPath {
    uint32_t compSurface = 0, compScale = 0;    // component: float, FLinearColor
    uint32_t proxySurface = 0, proxyScale = 0;  // FDirectionalLightSceneProxy
    CloudField fields[kMaxCloudFields];
    int numFields = 0;
    uint32_t valueBytes = 0;
    uint32_t lightInfo = 0;  // FLightSceneProxy::LightSceneInfo (0: the light-info fields are left out)
    uint32_t component = 0;  // FLightSceneProxy::LightComponent (0: not found; only the sky-light-only fields are used)
    void** vtables[kMaxCloudVtables] = {};
    int numVtables = 0;
    int slot = -1;
    SetAtmosphereFn original = nullptr;
    std::atomic<int32_t> sceneProxy{-1};  // ULightComponent::SceneProxy offset, found on first use
    std::atomic<int> scans{0};
    std::atomic<bool> enabled{true};
    bool ready = false, hooked = false;
} g_cloud;

// Values left for one proxy (game thread -> render thread), under g_cloudLock.
struct CloudEntry {
    void* proxy = nullptr;
    uint8_t values[kMaxCloudBytes] = {};  // every field's component value (CloudField::slot)
    uint64_t sinceUs = 0;     // waiting since (not moved on by newer values)
    bool pending = false;
};
struct RebuildMark {
    const void* proxy = nullptr;
    uint64_t sinceUs = 0;
};
constexpr int kCloudRebuilding = 4;
SRWLOCK g_cloudLock = SRWLOCK_INIT;
RebuildMark g_cloudRebuilding[kCloudRebuilding];  // proxies whose light was just sent to a rebuild (under the lock)
unsigned g_cloudRebuildingNext = 0;
CloudEntry g_cloudEntries[kCloudEntries];
std::atomic<int> g_cloudPending{0};
std::atomic<uint64_t> g_cloudPosted{0}, g_cloudApplied{0}, g_cloudLate{0}, g_cloudNoProxy{0}, g_cloudOther{0},
    g_cloudInStep{0}, g_cloudOutOfStep{0}, g_cloudRebuilt{0}, g_cloudDuringRebuild{0}, g_cloudNotSky{0};
// The light components the renderer drew as sky (atmosphere) lights lately: it calls SetAtmosphereRelatedProperties
// every frame for those only, so only their values can wait for it. Keyed by the component, which a rebuild keeps (its
// new proxy is drawn a frame later).
struct SeenLight {
    std::atomic<const void*> component{nullptr};
    std::atomic<uint64_t> us{0};
};
constexpr int kSeenLights = 4;
SeenLight g_cloudSeen[kSeenLights];

bool BeingRebuilt(const void* proxy, uint64_t now);

uint64_t NowUs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), freq);
}

bool IsCloudVtable(const void* vtable)
{
    for (int i = 0; i < g_cloud.numVtables; ++i)
        if (g_cloud.vtables[i] == vtable) return true;
    return false;
}

// A pointer member's object, read without trusting it: committed, readable, not a guard page.
bool ReadObjectVtable(uintptr_t p, void** vtable)
{
    if (p < 0x10000 || p > 0x7FFFFFFFFFF0ull || (p & 7)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return false;
    __try {
        *vtable = *reinterpret_cast<void* const*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class LightProxy { Directional, None, Other };

// The light's render proxy: a directional one (wrapped vtable, *proxy set), none (the light is not in the scene: hidden,
// e.g. the moon by day), or anything else / not known (rebuild as before). Game thread: the component holds its proxy
// pointer only while the proxy lives.
LightProxy FindLightProxy(const uint8_t* obj, int32_t size, void** proxy)
{
    int32_t off = g_cloud.sceneProxy.load(std::memory_order_relaxed);
    if (off < 0) {
        if (g_cloud.scans.fetch_add(1, std::memory_order_relaxed) >= kMaxProxyScans) return LightProxy::Other;
        for (int32_t o = static_cast<int32_t>(kClassPrivate) + 8; o + 8 <= size; o += 8) {
            void* vt = nullptr;
            if (ReadObjectVtable(*reinterpret_cast<const uintptr_t*>(obj + o), &vt) && IsCloudVtable(vt)) { off = o; break; }
        }
        if (off < 0) return LightProxy::Other;  // found only on a light that has its proxy
        g_cloud.sceneProxy.store(off, std::memory_order_relaxed);
        Log("[atm] cloud path: the light component's render proxy pointer at 0x%X", off);
    }
    if (off + 8 > size) return LightProxy::Other;
    *proxy = *reinterpret_cast<void* const*>(obj + off);
    if (!*proxy) return LightProxy::None;
    return IsCloudVtable(*static_cast<void* const*>(*proxy)) ? LightProxy::Directional : LightProxy::Other;
}

// Where a field's render-side copy lives for this proxy: the proxy, or its FLightSceneInfo (nullptr if it has none).
const uint8_t* FieldTarget(const uint8_t* proxy, const CloudField& f)
{
    if (f.target == kInProxy) return proxy;
    return *reinterpret_cast<const uint8_t* const*>(proxy + g_cloud.lightInfo);
}

// The first field whose render-side copy differs from the component's value in before (nullptr: all in step).
const CloudField* MismatchedField(const uint8_t* proxy, const uint8_t* before)
{
    for (int i = 0; i < g_cloud.numFields; ++i) {
        const CloudField& f = g_cloud.fields[i];
        const uint8_t* target = FieldTarget(proxy, f);
        if (target && memcmp(target + f.dst, before + f.comp, f.size) != 0) return &f;
    }
    return nullptr;
}

void NoteSkyLight(const void* component, uint64_t now)  // render thread
{
    SeenLight* oldest = &g_cloudSeen[0];
    for (SeenLight& s : g_cloudSeen) {
        if (s.component.load(std::memory_order_relaxed) == component) {
            s.us.store(now, std::memory_order_release);
            return;
        }
        if (s.us.load(std::memory_order_relaxed) < oldest->us.load(std::memory_order_relaxed)) oldest = &s;
    }
    oldest->us.store(now, std::memory_order_relaxed);
    oldest->component.store(component, std::memory_order_release);
}

bool DrawnAsSkyLight(const void* component, uint64_t now)
{
    for (const SeenLight& s : g_cloudSeen)
        if (s.component.load(std::memory_order_acquire) == component && now - s.us.load(std::memory_order_acquire) <= kCloudLateUs)
            return true;
    return false;
}

// Game thread: leave the component's field values for its proxy. False = apply them the stock way (rebuild).
// before = the component as it was before this Set: with nothing waiting for the proxy, the proxy must hold its values
// (counted: the check that the renderer's copy kept up with the component).
bool PostCloud(const uint8_t* before, const uint8_t* obj, int32_t size)
{
    void* proxy = nullptr;
    const LightProxy kind = FindLightProxy(obj, size, &proxy);
    // Not in the scene: nothing to update. A proxy made later (the light shown again: its intensity or visibility
    // changes, which rebuild it) copies the component's values as they are then.
    if (kind == LightProxy::None) {
        g_cloudNoProxy.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (kind == LightProxy::Other) {
        if (g_cloudOther.fetch_add(1, std::memory_order_relaxed) < 4) {
            void* vt = proxy ? *static_cast<void* const*>(proxy) : nullptr;
            const GameModule& game = Game();
            Log("[atm] cloud path: light %p (size 0x%X) has proxy %p with vtable %s0x%llX: rebuilt as before", obj, size, proxy,
                game.rdata.Contains(vt) ? "exe+" : "", static_cast<unsigned long long>(
                    game.rdata.Contains(vt) ? static_cast<uint8_t*>(vt) - game.base : reinterpret_cast<uintptr_t>(vt)));
        }
        return false;
    }
    const uint64_t now = NowUs();
    // Another directional light (e.g. the weather's lightning light): the renderer never calls the setter for it, and
    // fields such as its specular scale or source angle matter to it, so it is rebuilt as before.
    if (g_cloud.component && !DrawnAsSkyLight(obj, now)) {
        g_cloudNotSky.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool ok = true;
    AcquireSRWLockExclusive(&g_cloudLock);
    CloudEntry* e = nullptr;
    for (CloudEntry& c : g_cloudEntries)
        if (c.pending && c.proxy == proxy) { e = &c; break; }
    if (e && now - e->sinceUs > kCloudLateUs) {  // not picked up: rebuild, as the game would
        e->pending = false;
        g_cloudPending.fetch_sub(1, std::memory_order_relaxed);
        g_cloudLate.fetch_add(1, std::memory_order_relaxed);
        ok = false;
    } else if (!e) {
        const auto* p = static_cast<const uint8_t*>(proxy);
        if (BeingRebuilt(proxy, now)) {
            g_cloudDuringRebuild.fetch_add(1, std::memory_order_relaxed);  // the rebuild replaces this proxy
        } else {
            const CloudField* bad = MismatchedField(p, before);
            if (!bad) {
                g_cloudInStep.fetch_add(1, std::memory_order_relaxed);
            } else if (g_cloudOutOfStep.fetch_add(1, std::memory_order_relaxed) < 4) {
                float held = 0, had = 0;
                memcpy(&held, FieldTarget(p, *bad) + bad->dst, 4);
                memcpy(&had, before + bad->comp, 4);
                Log("[atm] cloud path: proxy %p holds %s %.4f, its light had %.4f", proxy, bad->name, held, had);
            }
        }
        for (CloudEntry& c : g_cloudEntries)
            if (!c.pending || now - c.sinceUs > kCloudLateUs) { e = &c; break; }  // a free or abandoned one
        if (e) {
            if (e->pending) g_cloudLate.fetch_add(1, std::memory_order_relaxed);
            else g_cloudPending.fetch_add(1, std::memory_order_relaxed);
            e->proxy = proxy;
            e->sinceUs = now;
            e->pending = true;
        } else {
            ok = false;
        }
    }
    if (ok) {
        for (int i = 0; i < g_cloud.numFields; ++i) {
            const CloudField& f = g_cloud.fields[i];
            memcpy(e->values + f.slot, obj + f.comp, f.size);
        }
        g_cloudPosted.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_cloudLock);
    return ok;
}

// Game thread, a forwarded PostSet (the light is rebuilt from its current values): nothing left is owed to its proxy.
// The proxy is remembered as being rebuilt until the next frame or so: a cloud change in between finds the old proxy,
// which the rebuild replaces (not counted as out of step).
void ForgetCloud(const uint8_t* obj, int32_t size)
{
    const int32_t off = g_cloud.sceneProxy.load(std::memory_order_relaxed);
    if (off < 0 || off + 8 > size) return;
    void* proxy = *reinterpret_cast<void* const*>(obj + off);
    if (!proxy || !IsCloudVtable(*static_cast<void* const*>(proxy))) return;
    g_cloudRebuilt.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_cloudLock);
    for (CloudEntry& c : g_cloudEntries)
        if (c.pending && c.proxy == proxy) {
            c.pending = false;
            g_cloudPending.fetch_sub(1, std::memory_order_relaxed);
        }
    g_cloudRebuilding[g_cloudRebuildingNext++ % kCloudRebuilding] = {proxy, NowUs()};
    ReleaseSRWLockExclusive(&g_cloudLock);
}

bool BeingRebuilt(const void* proxy, uint64_t now)  // under g_cloudLock
{
    for (const RebuildMark& m : g_cloudRebuilding)
        if (m.proxy == proxy && now - m.sinceUs < kCloudLateUs) return true;
    return false;
}

// Render thread, in place of the proxy's SetAtmosphereRelatedProperties: the renderer's values, then ours.
void SetAtmosphereHook(void* proxy, const void* transmittance, const void* sunDisc)
{
    g_cloud.original(proxy, transmittance, sunDisc);
    if (g_cloud.component) NoteSkyLight(*reinterpret_cast<void* const*>(static_cast<uint8_t*>(proxy) + g_cloud.component), NowUs());
    if (g_cloudPending.load(std::memory_order_acquire) == 0) return;
    AcquireSRWLockExclusive(&g_cloudLock);
    for (CloudEntry& c : g_cloudEntries)
        if (c.pending && c.proxy == proxy) {
            auto* p = static_cast<uint8_t*>(proxy);
            for (int i = 0; i < g_cloud.numFields; ++i) {
                const CloudField& f = g_cloud.fields[i];
                if (uint8_t* target = const_cast<uint8_t*>(FieldTarget(p, f))) memcpy(target + f.dst, c.values + f.slot, f.size);
            }
            c.pending = false;
            g_cloudPending.fetch_sub(1, std::memory_order_relaxed);
            g_cloudApplied.fetch_add(1, std::memory_order_relaxed);
        }
    ReleaseSRWLockExclusive(&g_cloudLock);
}

// The vtable's slot holding the setter, if it also holds the getters of both proxy fields; else -1.
int CloudVtableSlot(const GameModule& game, void* const* vtable, const uint8_t* setter)
{
    if (!game.rdata.Contains(vtable)) return -1;
    char text[64];
    snprintf(text, sizeof text, "F3 0F 10 81 %02X %02X %02X %02X C3", g_cloud.proxySurface & 0xFF, g_cloud.proxySurface >> 8 & 0xFF,
             g_cloud.proxySurface >> 16 & 0xFF, g_cloud.proxySurface >> 24);
    const Pattern surfaceGetter = ParsePattern(text);
    snprintf(text, sizeof text, "0F 10 81 %02X %02X %02X %02X 48 8B C2 0F 11 02 C3", g_cloud.proxyScale & 0xFF,
             g_cloud.proxyScale >> 8 & 0xFF, g_cloud.proxyScale >> 16 & 0xFF, g_cloud.proxyScale >> 24);
    const Pattern scaleGetter = ParsePattern(text);
    int slot = -1;
    bool surface = false, scale = false;
    for (int i = 0; i < kMaxProxySlots && game.rdata.Contains(vtable + i); ++i) {
        auto* fn = static_cast<const uint8_t*>(vtable[i]);
        if (!game.text.Contains(fn)) break;
        if (fn == setter) {
            if (slot >= 0) return -1;
            slot = i;
        }
        surface = surface || MatchesAt(fn, surfaceGetter);
        scale = scale || MatchesAt(fn, scaleGetter);
    }
    return surface && scale ? slot : -1;
}

// The offsets the exe's generated property tables give these property names (FGenericPropertyParams: the name pointer
// first, ArrayDim and Offset as uint16 at +0x30 / +0x32; one entry per class that declares the name). out[i] = names[i].
void PropertyOffsets(const GameModule& game, const char* const* names, int count, std::vector<uint32_t>* out)
{
    std::vector<const uint8_t*> strs[kMaxCloudFields];
    for (int i = 0; i < count && i < kMaxCloudFields; ++i) {
        const size_t n = strlen(names[i]) + 1;
        const uint8_t* end = game.rdata.end;
        for (const uint8_t* p = game.rdata.begin + 1; p + n <= end; ++p) {
            p = static_cast<const uint8_t*>(memchr(p, names[i][0], end - p));
            if (!p || p + n > end) break;
            if (p[-1] == 0 && memcmp(p, names[i], n) == 0) strs[i].push_back(p);
        }
    }
    for (auto* const* s = reinterpret_cast<void* const*>(game.rdata.begin); s + 7 <= reinterpret_cast<void* const*>(game.rdata.end); ++s)
        for (int i = 0; i < count && i < kMaxCloudFields; ++i)
            for (const uint8_t* str : strs[i])
                if (*s == str) {
                    const auto* params = reinterpret_cast<const uint8_t*>(s);
                    uint16_t dim, off;
                    memcpy(&dim, params + 0x30, 2);
                    memcpy(&off, params + 0x32, 2);
                    if (dim == 1) out[i].push_back(off);
                }
}

// In [from, to): mov r32,[reg+comp]; mov [reg+dst],r32 (the same r32), comp one of the given offsets.
bool FindFieldCopy(const uint8_t* from, const uint8_t* to, const std::vector<uint32_t>& comps, uint32_t* comp, uint32_t* dst)
{
    for (const uint8_t* p = from; p + 12 <= to; ++p) {
        if (p[0] != 0x8B || (p[1] & 0xC0) != 0x80 || (p[1] & 7) == 4 || p[6] != 0x89 || (p[7] & 0xC0) != 0x80 ||
            (p[7] & 7) == 4 || ((p[1] ^ p[7]) & 0x38) != 0)
            continue;
        const uint32_t c = ReadU32(p + 2);
        if (std::find(comps.begin(), comps.end(), c) == comps.end()) continue;
        *comp = c;
        *dst = ReadU32(p + 8);
        return *dst < 0x1000;
    }
    return false;
}

void AddCloudField(const char* name, uint32_t comp, uint32_t size, uint32_t dst, CloudTarget target)
{
    if (g_cloud.numFields == kMaxCloudFields || g_cloud.valueBytes + size > kMaxCloudBytes) return;
    CloudField& f = g_cloud.fields[g_cloud.numFields++];
    f = {name, comp, size, dst, g_cloud.valueBytes, target};
    g_cloud.valueBytes += size;
}

// The sun fields Altar's weather blend moves (lab probe, the Echo Mine walk and weather transitions, 2026-10-13), each
// only where the game's own code shows a plain copy into the render side and its reflected name agrees; one not found
// stays out (its Sets keep rebuilding the light). c = the directional proxy constructor's cloud copies, vtableStore =
// that constructor's vtable store (the base constructor is the call right before it). In UE 5.3 the renderer reads every
// one of them from the proxy (or the FLightSceneInfo) each frame, after SetAtmosphereRelatedProperties.
void FindCloudFields(const GameModule& game, const uint8_t* c, const uint8_t* vtableStore)
{
    static const char* const kNames[] = {"CloudShadowOnSurfaceStrength", "CloudScatteredLuminanceScale",
                                         "CloudShadowOnAtmosphereStrength", "OcclusionMaskDarkness", "LightSourceAngle",
                                         "LightSourceSoftAngle", "SpecularScale", "SpecularScaleWater", "BloomScale",
                                         "BloomThreshold", "BloomMaxBrightness", "BloomTint"};
    constexpr int kCount = static_cast<int>(std::size(kNames));
    static_assert(kCount <= kMaxCloudFields);
    std::vector<uint32_t> offs[kCount];
    PropertyOffsets(game, kNames, kCount, offs);
    const auto named = [&](int i, uint32_t comp) { return std::find(offs[i].begin(), offs[i].end(), comp) != offs[i].end(); };
    // FLightSceneInfo's constructor (its bloom copies): where the proxy keeps its FLightSceneInfo and its component
    const auto bloom = FindAll(game.text, ParsePattern(kBloomCopies));
    const auto stores = FindAll(game.text, ParsePattern(kAddLightInfo));
    const uint8_t* bloomAt = nullptr;
    if (bloom.size() == 1 && stores.size() == 1) {
        const uint8_t* ctor = RipTarget(stores[0] + kAddLightCall, 1, 5);
        const uint32_t infoOff = ReadU32(stores[0] + kAddLightStore);
        if (ctor <= bloom[0] && bloom[0] < ctor + kBloomCtorSearch && infoOff < 0x1000 && infoOff % 8 == 0) {
            g_cloud.lightInfo = infoOff;
            bloomAt = bloom[0];
            const uint8_t* b0 = bloom[0];
            if (b0[3] && b0[3] == b0[kBloomStride + 3] && b0[3] == b0[2 * kBloomStride + 3] && b0[3] == b0[3 * kBloomStride + 3] &&
                b0[3] % 8 == 0)
                g_cloud.component = b0[3];  // mov rax,[rdi+LightComponent] before each copy
        }
    }
    // the two cloud values (found with the hook) and the cloud run's 3rd copy: read for sky (atmosphere) lights only
    if (named(0, g_cloud.compSurface)) AddCloudField(kNames[0], g_cloud.compSurface, 4, g_cloud.proxySurface, kInProxy);
    if (named(1, g_cloud.compScale)) AddCloudField(kNames[1], g_cloud.compScale, 16, g_cloud.proxyScale, kInProxy);
    if (named(2, ReadU32(c + 2 * 12 + 2))) AddCloudField(kNames[2], ReadU32(c + 2 * 12 + 2), 4, ReadU32(c + 2 * 12 + 8), kInProxy);
    // the others matter to every directional light: only with the check that the renderer draws the light as a sky light
    if (!g_cloud.component) return;
    // the directional proxy constructor's other plain copies
    uint32_t comp = 0, dst = 0;
    for (int i = 3; i <= 5; ++i)
        if (FindFieldCopy(c - kCtorCopySearch, c, offs[i], &comp, &dst)) AddCloudField(kNames[i], comp, 4, dst, kInProxy);
    // the base FLightSceneProxy constructor: call base; lea rax,[vtable]
    if (vtableStore && vtableStore[-5] == 0xE8) {
        const uint8_t* base = RipTarget(vtableStore - 5, 1, 5);
        if (game.text.Contains(base) && game.text.Contains(base + kCtorCopySearch))
            for (int i = 6; i <= 7; ++i)
                if (FindFieldCopy(base, base + kCtorCopySearch, offs[i], &comp, &dst)) AddCloudField(kNames[i], comp, 4, dst, kInProxy);
    }
    if (bloomAt)
        for (int k = 0; k < 4; ++k) {
            const uint8_t* b = bloomAt + k * kBloomStride;
            if (named(8 + k, ReadU32(b + kBloomSrc)))
                AddCloudField(kNames[8 + k], ReadU32(b + kBloomSrc), 4, ReadU32(b + kBloomDst), kInLightInfo);
        }
}

bool FindLightCloudPath(const GameModule& game)
{
    const auto copies = FindAll(game.text, ParsePattern(kCloudCopies));
    if (copies.size() != 1) {
        Log("[atm] cloud path: proxy constructor's cloud copies found %zu times (expected 1): off", copies.size());
        return false;
    }
    const uint8_t* c = copies[0];
    g_cloud.compSurface = ReadU32(c + kSurfaceSrc);
    g_cloud.proxySurface = ReadU32(c + kSurfaceDst);
    g_cloud.compScale = ReadU32(c + kScaleSrc);
    g_cloud.proxyScale = ReadU32(c + kScaleDst);
    if (g_cloud.compSurface > 0x2000 || g_cloud.compScale > 0x2000 || g_cloud.proxySurface > 0x2000 || g_cloud.proxyScale > 0x2000) {
        Log("[atm] cloud path: implausible field offsets: off");
        return false;
    }
    const auto setters = FindAll(game.text, ParsePattern(kSetAtmosphere));
    if (setters.size() != 1) {
        Log("[atm] cloud path: SetAtmosphereRelatedProperties found %zu times (expected 1): off", setters.size());
        return false;
    }
    const uint8_t* setter = setters[0];
    // the constructor's vtable store, the nearest one before the copies: lea rax,[vtable]; (up to 2 bytes); mov [rdi],rax
    void** vtable = nullptr;
    const uint8_t* vtableStore = nullptr;
    for (const uint8_t* p = c - 10; p > c - kVtableStoreSearch; --p)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05 &&
            ((p[7] == 0x48 && p[8] == 0x89 && p[9] == 0x07) || (p[9] == 0x48 && p[10] == 0x89 && p[11] == 0x07))) {
            vtable = reinterpret_cast<void**>(RipTarget(p, 3, 7));
            vtableStore = p;
            break;
        }
    const int slot = vtable ? CloudVtableSlot(game, vtable, setter) : -1;
    if (slot < 0) {
        Log("[atm] cloud path: the proxy vtable or its setter/getters not found: off");
        return false;
    }
    g_cloud.slot = slot;
    g_cloud.vtables[g_cloud.numVtables++] = vtable;
    // subclasses: other vtables with the setter in the same slot and the same getters
    for (auto** s = reinterpret_cast<void**>(game.rdata.begin); s + 1 <= reinterpret_cast<void**>(game.rdata.end); ++s) {
        if (*s != setter) continue;
        void** other = s - slot;
        if (other == vtable || CloudVtableSlot(game, other, setter) != slot) continue;
        if (g_cloud.numVtables == kMaxCloudVtables) {
            Log("[atm] cloud path: more than %d proxy vtables: off", kMaxCloudVtables);
            g_cloud.numVtables = 0;
            return false;
        }
        g_cloud.vtables[g_cloud.numVtables++] = other;
    }
    g_cloud.original = reinterpret_cast<SetAtmosphereFn>(const_cast<uint8_t*>(setter));
    FindCloudFields(game, c, vtableStore);
    if (g_cloud.numFields < 2) {
        Log("[atm] cloud path: the cloud fields' names not confirmed by the property tables: off");
        return false;
    }
    g_cloud.ready = true;
    Log("[atm] cloud path: component 0x%X/0x%X -> proxy 0x%X/0x%X, SetAtmosphereRelatedProperties exe+0x%X in slot +0x%X of "
        "%d vtable(s) (exe+0x%X)", g_cloud.compSurface, g_cloud.compScale, g_cloud.proxySurface, g_cloud.proxyScale,
        static_cast<unsigned>(setter - game.base), slot * 8, g_cloud.numVtables,
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(vtable) - game.base));
    char list[640] = {};
    for (int i = 0; i < g_cloud.numFields; ++i) {
        const CloudField& f = g_cloud.fields[i];
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s 0x%X->%s0x%X", i ? ", " : "", f.name, f.comp,
                 f.target == kInProxy ? "" : "info+", f.dst);
    }
    Log("[atm] cloud path: %d fields (light info at proxy+0x%X, its component at +0x%X: only lights drawn as sky lights): %s",
        g_cloud.numFields, g_cloud.lightInfo, g_cloud.component, list);
    return true;
}

bool InstallLightCloudPath()
{
    int hooked = 0;
    for (int i = 0; i < g_cloud.numVtables; ++i)
        if (WritePointer(&g_cloud.vtables[i][g_cloud.slot], reinterpret_cast<void*>(&SetAtmosphereHook))) ++hooked;
    g_cloud.hooked = hooked == g_cloud.numVtables;
    if (!g_cloud.hooked) {
        // a vtable left unwrapped would never apply its values: back to the stock rebuild for every light
        for (int i = 0; i < g_cloud.numVtables; ++i) WritePointer(&g_cloud.vtables[i][g_cloud.slot], reinterpret_cast<void*>(g_cloud.original));
        Log("[atm] cloud path: vtable write failed: off");
    }
    return g_cloud.hooked;
}

bool IsChildOf(const uint8_t* cls, const uint8_t* parent)
{
    const int32_t depth = *reinterpret_cast<const int32_t*>(parent + kStructBaseDepth);
    if (depth < 0 || depth > *reinterpret_cast<const int32_t*>(cls + kStructBaseDepth)) return false;
    auto* chain = *reinterpret_cast<const uint8_t* const* const*>(cls + kStructBaseChain);
    return chain && chain[depth] == parent + kStructBaseChain;
}

bool DeclaresSetterThunk(const uint8_t* cls)
{
    int n = 0;
    for (auto* field = *reinterpret_cast<const uint8_t* const*>(cls + kChildren); field && n < kMaxChildren;
         field = *reinterpret_cast<const uint8_t* const*>(field + kFieldNext), ++n)
        for (size_t off = kFuncScanBegin; off + 8 <= kFuncScanEnd; off += 8) {
            void* p = *reinterpret_cast<void* const*>(field + off);
            if (std::find(g_light.setterThunks.begin(), g_light.setterThunks.end(), p) != g_light.setterThunks.end())
                return true;
        }
    return false;
}

bool IsLightComponent(const void* object)
{
    auto* cls = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(object) + kClassPrivate);
    if (!cls) return false;
    if (auto* light = static_cast<const uint8_t*>(g_light.lightClass.load(std::memory_order_acquire)))
        return IsChildOf(cls, light);

    // Not discovered yet: look for the declaring class in this object's class chain, once per class.
    thread_local const uint8_t* t_notLight[32];
    thread_local int t_numNotLight = 0;
    for (int i = 0; i < t_numNotLight; ++i)
        if (t_notLight[i] == cls) return false;
    int depth = 0;
    for (auto* c = cls; c && depth < kMaxClassDepth; c = *reinterpret_cast<const uint8_t* const*>(c + kSuperStruct), ++depth)
        if (DeclaresSetterThunk(c)) {
            g_light.lightClass.store(const_cast<uint8_t*>(c), std::memory_order_release);
            Log("[atm] light fast path: ULightComponent class at %p", c);
            return true;
        }
    if (t_numNotLight < 32) t_notLight[t_numNotLight++] = cls;
    return false;
}

// True if every byte that differs between before and after lies in a fast-path field (*fast) or a cloud field (*cloud),
// and something differs.
bool ClassifyChange(const uint8_t* before, const uint8_t* after, int32_t size, bool* fast, bool* cloud)
{
    *fast = *cloud = false;
    for (int32_t i = 0; i < size; ++i) {
        if (before[i] == after[i]) continue;
        const uint32_t u = static_cast<uint32_t>(i);
        const auto in = [u](uint32_t f) { return u >= f && u < f + 4; };
        if (std::any_of(g_light.fields.begin(), g_light.fields.end(), in)) *fast = true;
        else if (g_cloud.ready && std::any_of(g_cloud.fields, g_cloud.fields + g_cloud.numFields,
                                              [u](const CloudField& f) { return u >= f.comp && u < f.comp + f.size; }))
            *cloud = true;
        else return false;
    }
    return *fast || *cloud;
}

bool TryLightFastPath(const uint8_t* before, void* object, int32_t size)
{
    if (!g_light.ready) return false;
    auto* obj = static_cast<uint8_t*>(object);
    bool fast, cloud;
    if (!ClassifyChange(before, obj, size, &fast, &cloud)) return false;
    if (fast && !g_light.enabled.load(std::memory_order_relaxed)) return false;
    if (cloud && !(g_cloud.hooked && g_cloud.enabled.load(std::memory_order_relaxed))) return false;
    if (!IsLightComponent(object)) return false;
    // AreDynamicDataChangesAllowed: a static light keeps the original (rebuild) behaviour.
    if (g_light.mobility >= size || obj[g_light.mobility] == 0 /* EComponentMobility::Static */) return false;
    if (cloud && !PostCloud(before, obj, size)) return false;
    if (fast) {
        g_light.ucab(object);
        g_fastPath.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool FindLightFastPath(const GameModule& game, const uint8_t* postSet)
{
    const auto ucabHits = FindAll(game.text, ParsePattern(kUcabPattern));
    if (ucabHits.size() != 1) {
        Log("[atm] light fast path: UpdateColorAndBrightness found %zu times (expected 1): off", ucabHits.size());
        return false;
    }
    const uint8_t* ucab = ucabHits[0];
    if (RipTarget(ucab + kUcabDirtyCall, 1, 5) != RipTarget(postSet + kPostSetDirtyCall, 1, 5)) {
        Log("[atm] light fast path: UpdateColorAndBrightness does not call PostSet's MarkRenderStateDirty: off");
        return false;
    }
    const auto sceneCall = ParsePattern(kUcabSceneCall);
    const uint8_t* slotAt = nullptr;
    for (const uint8_t* p = ucab + kUcabDirtyCall; p < ucab + kUcabDirtyCall + kUcabSceneCallSearch; ++p)
        if (MatchesAt(p, sceneCall)) { slotAt = p + sceneCall.size(); break; }
    if (!slotAt) {
        Log("[atm] light fast path: no scene call in UpdateColorAndBrightness: off");
        return false;
    }
    auto hex4 = [](uint32_t v) {
        char s[16];
        snprintf(s, sizeof s, "%02X %02X %02X %02X", v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24);
        return std::string(s);
    };
    char pattern[512];
    snprintf(pattern, sizeof pattern, kSceneSetterFmt, hex4(ReadU32(ucab + kUcabWorldDisp)).c_str(),
             hex4(ReadU32(ucab + kUcabSceneDisp)).c_str(), hex4(ReadU32(slotAt)).c_str());

    std::vector<uint32_t> fields{ReadU32(ucab + kUcabIntensityDisp)};
    int32_t mobility = -1;
    std::vector<const uint8_t*> setters;
    for (const uint8_t* hit : FindAll(game.text, ParsePattern(pattern))) {
        const uint32_t field = ReadU32(hit + kSetterCompareDisp);
        const int32_t mob = static_cast<int32_t>(ReadU32(hit + kSetterMobilityDisp));
        if (field != ReadU32(hit + kSetterStoreDisp) || (mobility >= 0 && mob != mobility)) {
            Log("[atm] light fast path: inconsistent setter at exe+0x%X: off", static_cast<unsigned>(hit - game.base));
            return false;
        }
        mobility = mob;
        if (std::find(fields.begin(), fields.end(), field) == fields.end()) fields.push_back(field);
        setters.push_back(hit);
    }
    if (setters.empty()) {
        Log("[atm] light fast path: no stock fast-path setters found: off");
        return false;
    }
    char colorPattern[512];
    snprintf(colorPattern, sizeof colorPattern, kColorSetterFmt, hex4(ReadU32(ucab + kUcabWorldDisp)).c_str(),
             hex4(ReadU32(ucab + kUcabSceneDisp)).c_str(), hex4(ReadU32(slotAt)).c_str());
    const auto colorHits = FindAll(game.text, ParsePattern(colorPattern));
    uint32_t colorField = 0;
    if (colorHits.size() == 1 && ReadU32(colorHits[0] + kColorCompareDisp) == ReadU32(colorHits[0] + kColorStoreDisp) &&
        static_cast<int32_t>(ReadU32(colorHits[0] + kColorMobilityDisp)) == mobility &&
        std::find(fields.begin(), fields.end(), ReadU32(colorHits[0] + kColorCompareDisp)) == fields.end()) {
        colorField = ReadU32(colorHits[0] + kColorCompareDisp);
        fields.push_back(colorField);
    } else {
        Log("[atm] light fast path: SetLightColor's shape found %zu times: colour changes rebuild the light as before", colorHits.size());
    }

    // Native exec thunks containing a setter: .rdata {const char* name, FNativeFuncPtr func} pairs.
    std::vector<void*> thunks;
    for (auto** slot = reinterpret_cast<void**>(game.rdata.begin) + 1; slot + 1 <= reinterpret_cast<void**>(game.rdata.end); ++slot) {
        auto* fn = static_cast<const uint8_t*>(*slot);
        if (!game.text.Contains(fn) || !game.rdata.Contains(slot[-1])) continue;
        if (memcmp(slot[-1], "Set", 3) != 0) continue;
        for (const uint8_t* s : setters)
            if (s > fn && s < fn + kThunkSearch && std::find(thunks.begin(), thunks.end(), fn) == thunks.end())
                thunks.push_back(const_cast<uint8_t*>(fn));
    }
    if (thunks.empty()) {
        Log("[atm] light fast path: no native thunk contains a fast-path setter: off");
        return false;
    }

    std::sort(fields.begin(), fields.end());
    g_light.ucab = reinterpret_cast<UcabFn>(const_cast<uint8_t*>(ucab));
    g_light.mobility = mobility;
    g_light.fields = fields;
    g_light.setterThunks = thunks;
    g_light.ready = true;
    char list[128] = {};
    for (uint32_t f : fields) snprintf(list + strlen(list), sizeof list - strlen(list), " 0x%X", f);
    Log("[atm] light fast path: UpdateColorAndBrightness at exe+0x%X, fields%s (LightColor 0x%X), Mobility at 0x%X, %zu "
        "setters, %zu thunks",
        static_cast<unsigned>(ucab - game.base), list, colorField, mobility, setters.size(), thunks.size());
    return true;
}


// The last Set on this thread; PostSet follows it directly in the ATM apply loop.
struct LastSet {
    void* accessor = nullptr;
    void* object = nullptr;
    bool unchanged = false;
    std::vector<uint8_t> snapshot;
};
thread_local LastSet t_last;

int32_t ObjectSize(const void* object)
{
    auto* cls = *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(object) + kClassPrivate);
    if (!cls) return 0;
    const int32_t size = *reinterpret_cast<const int32_t*>(cls + kPropertiesSize);
    return size >= kMinObjectSize && size <= kMaxObjectSize ? size : 0;
}

SetFn OriginalSetFor(void* accessor)
{
    void** vtable = *static_cast<void***>(accessor);
    for (int i = 0; i < g_numVtables; ++i)
        if (g_vtables[i].vtable == vtable) return g_vtables[i].originalSet;
    return nullptr;
}

void SetHook(void* accessor, void** entry, const void* value)
{
    LastSet& last = t_last;
    last.accessor = nullptr;
    const SetFn original = OriginalSetFor(accessor);
    if (!original) return;  // unreachable: only hooked vtables point here

    if (!g_enabled.load(std::memory_order_relaxed)) {
        original(accessor, entry, value);
        return;
    }
    void* object = entry ? *entry : nullptr;
    const int32_t size = object ? ObjectSize(object) : 0;
    if (size) {
        last.snapshot.resize(size);
        memcpy(last.snapshot.data(), object, size);
    }
    original(accessor, entry, value);
    last.accessor = accessor;
    last.object = object;
    // "unchanged" means PostSet has nothing left to do: no change, or the light fast path applied it.
    last.unchanged = size && (memcmp(last.snapshot.data(), object, size) == 0 ||
                              TryLightFastPath(last.snapshot.data(), object, size));
    if (!size) g_unsized.fetch_add(1, std::memory_order_relaxed);
}

void PostSetHook(void* accessor, void* object)
{
    LastSet& last = t_last;
    const bool skip = last.unchanged && last.accessor == accessor && last.object == object;
    last.accessor = nullptr;
    last.unchanged = false;
    if (skip) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_forwarded.fetch_add(1, std::memory_order_relaxed);
    if (object && g_cloud.hooked && g_cloud.sceneProxy.load(std::memory_order_relaxed) >= 0 && IsLightComponent(object))
        if (const int32_t size = ObjectSize(object)) ForgetCloud(static_cast<const uint8_t*>(object), size);
    g_originalPostSet(accessor, object);
}

}  // namespace

bool ApplyAtmosphereDirtyFix()
{
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kPostSetPattern));
    if (hits.size() != 1) {
        Log("[atm] PostSet pattern found %zu times (expected 1): fix disabled", hits.size());
        return false;
    }
    void* postSet = hits[0];
    Log("[atm] PostSet at exe+0x%X", static_cast<unsigned>(hits[0] - game.base));
    // optional: the filter works without them; the cloud path needs the fast path's light class test
    if (FindLightFastPath(game, hits[0]) && FindLightCloudPath(game)) InstallLightCloudPath();

    // Every accessor vtable has the shared PostSet in slot 3.
    HookedVtable found[kMaxAccessorVtables];
    int count = 0;
    for (auto** slot = reinterpret_cast<void**>(game.rdata.begin); slot + 1 <= reinterpret_cast<void**>(game.rdata.end); ++slot) {
        if (*slot != postSet) continue;
        void** vtable = slot - kSlotPostSet;
        if (!game.text.Contains(vtable[0]) || !game.text.Contains(vtable[kSlotSet])) continue;
        if (count == kMaxAccessorVtables) {
            Log("[atm] more than %d accessor vtables: fix disabled", kMaxAccessorVtables);
            return false;
        }
        found[count++] = {vtable, reinterpret_cast<SetFn>(vtable[kSlotSet])};
    }
    if (count == 0) {
        Log("[atm] no accessor vtables found: fix disabled");
        return false;
    }

    g_originalPostSet = reinterpret_cast<PostSetFn>(postSet);
    for (int i = 0; i < count; ++i) g_vtables[i] = found[i];
    g_numVtables = count;
    // Set first: a PostSet still unhooked for a moment simply behaves as before.
    int hooked = 0;
    for (int i = 0; i < count; ++i) {
        void** vtable = found[i].vtable;
        if (WritePointer(&vtable[kSlotSet], reinterpret_cast<void*>(&SetHook)) &&
            WritePointer(&vtable[kSlotPostSet], reinterpret_cast<void*>(&PostSetHook)))
            ++hooked;
        else
            Log("[atm] vtable exe+0x%X: write failed", static_cast<unsigned>(reinterpret_cast<uint8_t*>(vtable) - game.base));
    }
    Log("[atm] %d accessor vtables hooked", hooked);
    return hooked > 0;
}

void SetAtmosphereDirtyFilterEnabled(bool enabled)
{
    if (g_enabled.exchange(enabled) != enabled && g_numVtables > 0)
        Log("[atm] filter %s", enabled ? "enabled" : "disabled (original behaviour)");
}

void SetAtmosphereLightFastPathEnabled(bool enabled)
{
    if (g_light.enabled.exchange(enabled) != enabled && g_light.ready)
        Log("[atm] light fast path %s", enabled ? "enabled" : "disabled (full rebuild)");
}

void SetAtmosphereLightCloudPathEnabled(bool enabled)
{
    if (g_cloud.enabled.exchange(enabled) != enabled && g_cloud.hooked)
        Log("[atm] cloud path %s", enabled ? "enabled" : "disabled (cloud value changes rebuild the light)");
}

bool AtmosphereCloudPathInfo(AtmosphereCloudPathInfoOut* out)
{
    if (!g_cloud.ready) return false;
    out->setter = reinterpret_cast<uint8_t*>(g_cloud.original);
    out->vtable = g_cloud.vtables[0];
    out->numVtables = g_cloud.numVtables;
    out->slot = g_cloud.slot;
    out->hooked = g_cloud.hooked;
    out->compSurface = g_cloud.compSurface;
    out->compScale = g_cloud.compScale;
    out->proxySurface = g_cloud.proxySurface;
    out->proxyScale = g_cloud.proxyScale;
    out->sceneProxy = g_cloud.sceneProxy.load();
    out->posted = g_cloudPosted.load();
    out->applied = g_cloudApplied.load();
    out->late = g_cloudLate.load();
    out->pending = g_cloudPending.load();
    out->inStep = g_cloudInStep.load();
    out->outOfStep = g_cloudOutOfStep.load();
    out->duringRebuild = g_cloudDuringRebuild.load();
    out->rebuilt = g_cloudRebuilt.load();
    out->fields.clear();
    for (int i = 0; i < g_cloud.numFields; ++i) {
        const CloudField& f = g_cloud.fields[i];
        out->fields.push_back({f.name, f.comp, f.size, f.dst, f.target == kInLightInfo});
    }
    out->lightInfo = g_cloud.lightInfo;
    out->component = g_cloud.component;
    out->notSky = g_cloudNotSky.load();
    return true;
}

bool AtmosphereCloudTestPost(const uint8_t* before, const uint8_t* component, int32_t size)
{
    return g_cloud.hooked && PostCloud(before, component, size);
}
void AtmosphereCloudTestForget(const uint8_t* component, int32_t size) { ForgetCloud(component, size); }
void AtmosphereCloudTestAge(uint64_t us)  // self-test: make what waits look older
{
    AcquireSRWLockExclusive(&g_cloudLock);
    for (CloudEntry& c : g_cloudEntries) c.sinceUs -= us;
    for (RebuildMark& m : g_cloudRebuilding) m.sinceUs -= us;
    ReleaseSRWLockExclusive(&g_cloudLock);
}

bool AtmosphereLightFastPathInfo(uint8_t** ucab, int32_t* mobility, std::vector<uint32_t>* fields, size_t* thunks)
{
    if (!g_light.ready) return false;
    *ucab = reinterpret_cast<uint8_t*>(g_light.ucab);
    *mobility = g_light.mobility;
    *fields = g_light.fields;
    *thunks = g_light.setterThunks.size();
    return true;
}

void LogAtmosphereStats()
{
    static uint64_t lastSkipped = 0, lastForwarded = 0, lastFast = 0;
    const uint64_t skipped = g_skipped.load(), forwarded = g_forwarded.load(), fast = g_fastPath.load();
    if (g_numVtables == 0 || skipped + forwarded == lastSkipped + lastForwarded) return;
    Log("[atm] PostSet: %llu skipped (%llu of them light updates via the fast path), %llu forwarded since last report; %llu Sets without size",
        skipped - lastSkipped, fast - lastFast, forwarded - lastForwarded, g_unsized.load());
    lastSkipped = skipped;
    lastForwarded = forwarded;
    lastFast = fast;
    static uint64_t lastPosted = 0, lastApplied = 0, lastLate = 0, lastNoProxy = 0, lastOther = 0, lastIn = 0, lastOut = 0,
                    lastRebuilt = 0, lastDuring = 0, lastNotSky = 0;
    const uint64_t posted = g_cloudPosted.load(), applied = g_cloudApplied.load(), late = g_cloudLate.load(),
                   noProxy = g_cloudNoProxy.load(), other = g_cloudOther.load(), in = g_cloudInStep.load(),
                   out = g_cloudOutOfStep.load(), rebuilt = g_cloudRebuilt.load(), during = g_cloudDuringRebuild.load(),
                   notSky = g_cloudNotSky.load();
    if (posted != lastPosted || late != lastLate || noProxy != lastNoProxy || other != lastOther || rebuilt != lastRebuilt ||
        notSky != lastNotSky)
        Log("[atm] cloud path: %llu cloud value changes left for the render proxy (%llu written by the render thread; before "
            "a change the proxy held the light's values %llu times, other values %llu times, %llu times it was being "
            "rebuilt), %llu not picked up in time (light rebuilt), %llu on lights not in the scene (nothing to update), %llu "
            "on other proxies (rebuilt), %llu on directional lights not drawn as sky lights (rebuilt); directional lights "
            "rebuilt for other changes: %llu",
            posted - lastPosted, applied - lastApplied, in - lastIn, out - lastOut, during - lastDuring, late - lastLate,
            noProxy - lastNoProxy, other - lastOther, notSky - lastNotSky, rebuilt - lastRebuilt);
    lastPosted = posted;
    lastApplied = applied;
    lastLate = late;
    lastNoProxy = noProxy;
    lastOther = other;
    lastIn = in;
    lastOut = out;
    lastRebuilt = rebuilt;
    lastDuring = during;
    lastNotSky = notSky;
}

}  // namespace opf
