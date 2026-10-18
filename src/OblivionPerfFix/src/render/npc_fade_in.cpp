// NpcFadeInNoRebuild: a character's fade-in (dither) value reaches the GPU without recreating its render state.
//
// Altar added a bool and a float to UPrimitiveComponent (bDoDither +0x316, DitherScale +0x318) that the scene proxy copies at construction
// (FPrimitiveSceneProxy ctor exe+0x35B14A0: proxy+0x7D = (bool == 1), proxy+0x80 = float) and that only reach the GPU
// through the proxy's uniform / GPU-scene parameter builder (exe+0x34BA3A0 reads proxy+0x7D/+0x80). Their setters
// (SetDoDither exe+0x3044B20, SetDitherScale exe+0x3044B00) call MarkRenderStateDirty on every change. Every character
// that spawns fades in: UVCharacterFadeInOutComponent's tick (exe+0x48D5A40) lowers the value from 5 to 0 over 1.25 s and
// its actor loop (exe+0x48CD850) sets it on every component of the actor (body, clothes, equipment), so during the fade
// every component of that character recreates its render state every frame. For skinned meshes
// that is a new FSkeletalMeshObjectGPUSkin, new skin-cache entries and a new ray tracing BLAS each time (AllocTrace,
// ~160 components recreated ~50 times each in 16 s outdoors in Anvil; at a dock exit 3.5 GiB of BLAS allocated and
// freed in 2.5 s, the acceleration structure pool +1.5 GiB, the game over its video memory budget).
//
// The fix replaces the float setter: an unchanged value returns as before; a changed value on a component whose
// render state is live and not already marked dirty, whose proxy still matches the bool, is stored, written into the
// proxy (proxy+0x80, one aligned 4-byte store) and pushed to the GPU scene with the engine's own per-primitive update,
// FScene::UpdateCustomPrimitiveData (the path SetCustomPrimitiveDataFloat uses: its render command re-uploads the
// primitive's GPU-scene data, built from the proxy). Anything else (no proxy, already dirty, the bool changed): the
// game's MarkRenderStateDirty, unchanged. The bool setter is not touched (its changes, at the start and end of a fade,
// still recreate). The proxy value is written at once, so a render-thread rebuild for another reason in the same frame
// may show the new value a frame early (invisible: the value moves by 4 per second).
//
// Found by shape (build 19115871 addresses in the comments); the proxy offset in the component is read from
// FScene::UpdateCustomPrimitiveData's code at the first use. Any mismatch: the game's behaviour. Installed only before
// the game's code runs; the key is live.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

// The float setter (exe+0x3044B00): ucomiss xmm1,[rcx+318h]; je ret; movss [rcx+318h],xmm1; jmp MarkRenderStateDirty
constexpr char kSetter[] = "0F 2E 89 ?? ?? 00 00 74 0D F3 0F 11 89 ?? ?? 00 00 E9 ?? ?? ?? ?? C3";
// The bool setter right after it (exe+0x3044B20): cmp dl,[rcx+316h]; je; mov [rcx+316h],dl; jmp MarkRenderStateDirty
constexpr char kBoolSetter[] = "3A 91 ?? ?? 00 00 74 0B 88 91 ?? ?? 00 00 E9 ?? ?? ?? ?? C3";
// SetCustomPrimitiveDataFloat (exe+0x3044320) ... mov rax,[rdi+World]; test rax,rax; jne; mov rcx,rdi; call GetWorld;
// test rax,rax; je; mov rcx,[rax+Scene]; test rcx,rcx; je; mov rax,[rcx]; mov rdx,rdi; call [rax+UpdateCustomPrimitiveData]
constexpr char kCustomData[] =
    "48 8B 87 ?? ?? 00 00 48 85 C0 75 ?? 48 8B CF E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 ?? ?? 00 00 48 85 C9 74 ?? "
    "48 8B 01 48 8B D7 FF 50 ??";
// The proxy constructor's copy (exe+0x35B1A93): cmp byte [rsi+316h],1; sete al; mov [rdi+7Dh],al; mov eax,[rsi+318h];
// mov [rdi+80h],eax
constexpr char kProxyCopy[] = "80 BE ?? ?? 00 00 01 0F 94 C0 88 47 ?? 8B 86 ?? ?? 00 00 89 87 ?? ?? 00 00";

using MarkFn = void (*)(uint8_t* comp);
using GetWorldFn = uint8_t* (*)(uint8_t* comp);
using UpdateFn = void (*)(void* scene, uint8_t* comp);

struct Sites {
    uint8_t* setter = nullptr;
    MarkFn markDirty = nullptr;
    GetWorldFn getWorld = nullptr;
    uint32_t floatOff = 0, boolOff = 0, worldOff = 0, sceneOff = 0, updateSlot = 0;
    uint32_t proxyBoolOff = 0, proxyFloatOff = 0;
};
Sites g;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<int32_t> g_proxyOff{-1};  // -1 = not read yet, 0 = could not be read (fix inactive)
std::atomic<uint64_t> s_calls{0}, s_cheap{0}, s_recreate{0};
constexpr uint8_t kRegistered = 1, kRenderStateCreated = 2, kRenderStateDirty = 0x80;  // UActorComponent flags (+0x8C)
constexpr uint32_t kFlagsOff = 0x8C;

uint32_t Disp32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// FScene::UpdateCustomPrimitiveData starts by reading Primitive->SceneProxy: mov r64,[rdx+disp32] (REX.W 8B, mod 10,
// rm rdx), within its first 32 bytes; the offset must be plausible for UPrimitiveComponent.
int32_t ReadProxyOffset(void* scene)
{
    __try {
        const uint8_t* fn = (*reinterpret_cast<uint8_t* const* const*>(scene))[g.updateSlot / 8];
        const GameModule& game = Game();
        if (!game.text.Contains(fn)) return 0;
        for (int i = 0; i < 32; ++i) {
            const uint8_t* p = fn + i;
            if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8B && (p[2] & 0xC7) == 0x82) {
                const uint32_t off = Disp32(p + 3);
                if (off >= 0x200 && off < 0x800 && (off & 7) == 0) {
                    Log("[fadein] FScene::UpdateCustomPrimitiveData exe+0x%X reads the scene proxy at component+0x%X",
                        static_cast<unsigned>(fn - game.base), off);
                    return static_cast<int32_t>(off);
                }
            }
        }
        Log("[fadein] FScene::UpdateCustomPrimitiveData exe+0x%X: scene proxy read not recognised, NpcFadeInNoRebuild inactive",
            static_cast<unsigned>(fn - game.base));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

void SetterHook(uint8_t* comp, float value)
{
    float& cur = *reinterpret_cast<float*>(comp + g.floatOff);
    if (!(cur < value || cur > value)) return;  // equal or unordered: as the game's ucomiss/je
    cur = value;
    s_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_on.load(std::memory_order_relaxed)) {
        const uint8_t flags = comp[kFlagsOff];
        if ((flags & (kRegistered | kRenderStateCreated)) == (kRegistered | kRenderStateCreated) && !(flags & kRenderStateDirty)) {
            uint8_t* const world = g.getWorld(comp);
            void* const scene = world ? *reinterpret_cast<void**>(world + g.sceneOff) : nullptr;
            int32_t proxyOff = g_proxyOff.load(std::memory_order_relaxed);
            if (scene && proxyOff < 0) {
                proxyOff = ReadProxyOffset(scene);
                g_proxyOff = proxyOff;
            }
            uint8_t* const proxy = scene && proxyOff > 0 ? *reinterpret_cast<uint8_t**>(comp + proxyOff) : nullptr;
            if (proxy && proxy[g.proxyBoolOff] == (comp[g.boolOff] == 1 ? 1 : 0)) {
                reinterpret_cast<std::atomic<float>*>(proxy + g.proxyFloatOff)->store(value, std::memory_order_relaxed);
                const UpdateFn update = reinterpret_cast<UpdateFn>((*reinterpret_cast<void***>(scene))[g.updateSlot / 8]);
                update(scene, comp);
                s_cheap.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    s_recreate.fetch_add(1, std::memory_order_relaxed);
    g.markDirty(comp);
}

bool Fail(const char* why)
{
    Log("[fadein] %s: NpcFadeInNoRebuild unavailable", why);
    return false;
}

}  // namespace

bool InstallNpcFadeInNoRebuild(bool beforeGameCode)
{
    if (g_installed) return true;
    const GameModule& game = Game();
    const auto setters = FindAll(game.text, ParsePattern(kSetter));
    const auto custom = FindAll(game.text, ParsePattern(kCustomData));
    const auto copies = FindAll(game.text, ParsePattern(kProxyCopy));
    Sites s;
    for (uint8_t* p : setters) {
        // the float setter followed (16-byte aligned) by the bool setter of the neighbouring field
        if (MatchesAt(p + 0x20, ParsePattern(kBoolSetter)) && Disp32(p + 0x22) + 2 == Disp32(p + 3)) { s.setter = p; break; }
    }
    if (!s.setter) return Fail("the fade-in value setter not found");
    s.floatOff = Disp32(s.setter + 3);
    s.boolOff = Disp32(s.setter + 0x22);
    s.markDirty = reinterpret_cast<MarkFn>(RipTarget(s.setter + 17, 1, 5));
    if (Disp32(s.setter + 13) != s.floatOff || RipTarget(s.setter + 0x2E, 1, 5) != reinterpret_cast<uint8_t*>(s.markDirty))
        return Fail("the setters disagree");
    // the SetCustomPrimitiveData* functions share this tail: all must agree
    if (custom.empty()) return Fail("SetCustomPrimitiveDataFloat's scene update not found");
    s.worldOff = Disp32(custom[0] + 3);
    s.getWorld = reinterpret_cast<GetWorldFn>(RipTarget(custom[0] + 15, 1, 5));
    s.sceneOff = Disp32(custom[0] + 28);
    s.updateSlot = custom[0][45];
    for (uint8_t* c : custom)
        if (Disp32(c + 3) != s.worldOff || RipTarget(c + 15, 1, 5) != reinterpret_cast<uint8_t*>(s.getWorld) ||
            Disp32(c + 28) != s.sceneOff || c[45] != s.updateSlot)
            return Fail("the custom primitive data updates disagree");
    uint8_t* copy = nullptr;
    for (uint8_t* p : copies)
        if (Disp32(p + 2) == s.boolOff && Disp32(p + 15) == s.floatOff) { copy = p; break; }
    if (!copy) return Fail("the scene proxy's copy of the fields not found");
    s.proxyBoolOff = copy[12];
    s.proxyFloatOff = Disp32(copy + 21);
    if (!game.text.Contains(reinterpret_cast<uint8_t*>(s.getWorld)) || s.sceneOff > 0x1000 || (s.updateSlot & 7) ||
        s.proxyFloatOff > 0x400)
        return Fail("unexpected offsets");
    if (!beforeGameCode) return Fail("only installed while the game starts");
    uint8_t* const region = AllocNearGame(16);
    if (!region) return Fail("no free memory near the game's code");
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&SetterHook);
    memcpy(region, jmpAbs, sizeof jmpAbs);
    memcpy(region + sizeof jmpAbs, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 16);
    const int64_t rel = region - (s.setter + 5);
    if (rel != static_cast<int32_t>(rel)) return Fail("the hook is out of reach");
    g = s;
    uint8_t bytes[5] = {0xE9};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(bytes + 1, &rel32, 4);
    if (!WriteCode(g.setter, bytes)) return Fail("could not patch the setter");
    g_installed = true;
    Log("[fadein] float setter exe+0x%X (component +0x%X, bool +0x%X; proxy +0x%X / +0x%X), MarkRenderStateDirty exe+0x%X, "
        "GetWorld exe+0x%X, scene world+0x%X, UpdateCustomPrimitiveData slot +0x%X: installed",
        static_cast<unsigned>(g.setter - game.base), g.floatOff, g.boolOff, g.proxyFloatOff, g.proxyBoolOff,
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g.markDirty) - game.base),
        static_cast<unsigned>(reinterpret_cast<uint8_t*>(g.getWorld) - game.base), g.sceneOff, g.updateSlot);
    return true;
}

bool NpcFadeInInfo(NpcFadeInInfoOut* out)
{
    if (!g.setter) return false;
    out->setter = g.setter;
    out->markDirty = reinterpret_cast<uint8_t*>(g.markDirty);
    out->getWorld = reinterpret_cast<uint8_t*>(g.getWorld);
    out->floatOff = g.floatOff; out->boolOff = g.boolOff; out->sceneOff = g.sceneOff; out->updateSlot = g.updateSlot;
    out->proxyBoolOff = g.proxyBoolOff; out->proxyFloatOff = g.proxyFloatOff;
    return true;
}

void NpcFadeInTestSetup(void* fakeMarkDirty, void* fakeGetWorld, int32_t proxyOff)
{
    g.markDirty = reinterpret_cast<MarkFn>(fakeMarkDirty);
    g.getWorld = reinterpret_cast<GetWorldFn>(fakeGetWorld);
    g_proxyOff = proxyOff;
}

void NpcFadeInTestCall(uint8_t* comp, float value) { SetterHook(comp, value); }

void SetNpcFadeInNoRebuild(bool enabled)
{
    if (!g_installed || g_on.load() == enabled) return;
    g_on = enabled;
    Log("[fadein] %s", enabled ? "a changed fade-in value updates the GPU scene instead of recreating the render state"
                             : "a changed fade-in value recreates the render state, as before");
}

void LogNpcFadeInStats()
{
    const uint64_t calls = s_calls.exchange(0), cheap = s_cheap.exchange(0), rec = s_recreate.exchange(0);
    if (calls)
        Log("[fadein] since last report: %llu fade-in value changes, %llu sent to the GPU scene, %llu recreated the render state",
            calls, cheap, rec);
}

}  // namespace opf
