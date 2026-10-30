// VtStreamingUploads (off by default): more streamed virtual texture pages uploaded per frame.
//
// After stepping outdoors the view is new: the surfaces of the area (walls, roofs, stairs) use streaming virtual
// textures, whose pages are read from disk and uploaded to the graphics card as the frames ask for them. The number of
// streamed pages uploaded per frame is capped by r.VT.MaxUploadsPerFrame.Streaming, 4 in this game. At the Imperial
// City Waterfront lighthouse exit (5120x2160) the stair walls ~40 m away come in blotchy, without mortar lines, and
// reach full detail about 8 s after the fade-in; with the cap at 64 (set from the console) they are final as the fade
// completes (docs/research.md, VtStreamingUploads; 2 + 2 runs).
//
// The variable is registered in code with default 0 and flags RenderThreadSafe | Scalability (exe+0xA69B5B, build
// 19115871); the 4 comes from the game's scalability settings, applied after every ini layer, so no ini line can set it
// (Engine.ini [SystemSettings] and [ConsoleVariables] and DeviceProfiles.ini were tried). This finds the registration by
// name and shape, as GpuSceneParallelUpload does (render/gpuscene_upload.cpp), and, once per second from the DLL's
// monitor, writes N into both copies of the value (the game thread's and the render thread's) whenever it is not N. A
// later re-application of the graphics settings writes the game's value back; the next check writes N again (counted).
// While the key is N > 0, N wins over any other value. Switching the key to 0 puts back the value it replaced, if the
// value is still the fix's N.
//
// What changes: only how many already-requested pages are uploaded in one frame (the same pages, sooner). The settled
// picture is the same; detail arrives sooner after a door and during travel. The cost is more upload work per frame,
// measured on flights (docs/research.md). Default 0 = the game's value.
#include "common.h"

#include <cstring>

namespace opf {
namespace {

constexpr wchar_t kName[] = L"r.VT.MaxUploadsPerFrame.Streaming";
// lea rdx,[name]; xor r8d,r8d (default 0); mov [rsp+30h],rdi; mov dword [rsp+20h],60h (RenderThreadSafe | Scalability);
// call [rax+18h] (RegisterConsoleVariable, int); lea rcx,[vtable]; mov [auto+8],rax; mov [auto],rcx
constexpr char kRegister[] =
    "48 8D 15 ?? ?? ?? ?? 45 33 C0 48 89 7C 24 30 C7 44 24 20 60 00 00 00 FF 50 18 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
    "48 89 0D ?? ?? ?? ??";
constexpr size_t kObjStore = 33, kAutoStore = 40, kRegisterLen = 47;
// Then (the auto-variable's constructor): mov rax,[rcx]; call [rax+58h] (AsVariableInt); lea rcx,[...]; mov [auto+10h],rax
constexpr char kAsInt[] = "48 8B 01 FF 50 58 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ??";
constexpr size_t kDataStore = 13, kAsIntWindow = 0x100;
constexpr int kMax = 1024;          // a sane upper bound for the key
constexpr int kRewriteLogLines = 5; // rewrites after the first set logged one by one, then only counted

int32_t* volatile* g_slot = nullptr;  // where the registration stores TConsoleVariableData<int32>* (two int32 values)
bool g_searched = false;
int g_ours = 0;                       // the value the fix wrote (0 = not written / given back)
int32_t g_replaced = 0;               // the value it replaced (the game's), put back when switched off
long g_rewrites = 0;                  // times the value was found changed back and written again

const uint8_t* FindName(const Section& s)
{
    const size_t bytes = sizeof(kName);
    const uint8_t* hit = nullptr;
    for (const uint8_t* p = s.begin; p + bytes <= s.end; p += 2) {
        if (memcmp(p, kName, bytes) != 0) continue;
        if (hit) return nullptr;
        hit = p;
    }
    return hit;
}

void Write(int32_t* value, int32_t v)
{
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&value[0]), v);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&value[1]), v);
}

}  // namespace

bool FindVtStreamingUploads()
{
    if (g_slot) return true;
    if (g_searched) return false;
    g_searched = true;
    const GameModule& game = Game();
    const uint8_t* const name = FindName(game.rdata);
    if (!name) {
        Log("[vtup] the name r.VT.MaxUploadsPerFrame.Streaming not found (once) in the game: VtStreamingUploads unavailable");
        return false;
    }
    const Pattern reg = ParsePattern(kRegister), asInt = ParsePattern(kAsInt);
    uint8_t* site = nullptr;
    int n = 0;
    for (uint8_t* p : FindAll(game.text, reg)) {
        if (RipTarget(p, 3, 7) != name) continue;
        site = p;
        ++n;
    }
    if (n != 1) {
        Log("[vtup] its registration (default 0, render-thread-safe, scalability) found %d times (expected once): VtStreamingUploads unavailable", n);
        return false;
    }
    uint8_t* const objSlot = RipTarget(site + kObjStore, 3, 7);
    uint8_t* const autoVar = RipTarget(site + kAutoStore, 3, 7);
    uint8_t* dataSlot = nullptr;
    int m = 0;
    for (uint8_t* p = site + kRegisterLen; p + asInt.size() <= site + kRegisterLen + kAsIntWindow; ++p) {
        if (!MatchesAt(p, asInt)) continue;
        if (RipTarget(p + kDataStore, 3, 7) != autoVar + 0x10) continue;
        if (!dataSlot) dataSlot = p + kDataStore;
        ++m;
    }
    if (objSlot != autoVar + 8 || m != 1 || !game.data.Contains(autoVar + 0x10) ||
        (reinterpret_cast<uintptr_t>(autoVar) & 7) != 0) {
        Log("[vtup] its value pointer is not where expected (%d stores): VtStreamingUploads unavailable", m);
        return false;
    }
    g_slot = reinterpret_cast<int32_t* volatile*>(autoVar + 0x10);
    Log("[vtup] r.VT.MaxUploadsPerFrame.Streaming registered at exe+0x%X, value pointer at exe+0x%X",
        static_cast<unsigned>(site - game.base), static_cast<unsigned>(autoVar + 0x10 - game.base));
    return true;
}

void SetVtStreamingUploads(int uploads)
{
    if (uploads < 0) uploads = 0;
    if (uploads > kMax) uploads = kMax;
    if (!g_slot && !(uploads > 0 && FindVtStreamingUploads())) return;
    int32_t* const value = *g_slot;
    if (!value) return;  // not registered yet (the game's static initialisation): next second
    if (uploads == 0) {
        if (!g_ours) return;
        if (value[0] == g_ours) {
            Write(value, g_replaced);
            Log("[vtup] r.VT.MaxUploadsPerFrame.Streaming put back to %d", g_replaced);
        }
        g_ours = 0;
        return;
    }
    if (value[0] == uploads && value[1] == uploads) return;  // already N (the fix's, or set so by someone else)
    if (!g_ours) {
        g_replaced = value[0];
        Write(value, uploads);
        g_ours = uploads;
        Log("[vtup] r.VT.MaxUploadsPerFrame.Streaming %d -> %d: streamed virtual texture pages uploaded per frame", g_replaced, uploads);
        return;
    }
    // ours before, changed since: the key's value changed, or the game's settings were applied again
    const int32_t was = value[0];
    Write(value, uploads);
    if (was == g_ours) {
        Log("[vtup] r.VT.MaxUploadsPerFrame.Streaming %d -> %d (key changed)", was, uploads);
    } else {
        g_replaced = was;  // the game's value now: the one to put back
        if (++g_rewrites <= kRewriteLogLines)
            Log("[vtup] r.VT.MaxUploadsPerFrame.Streaming was set to %d again (the game's settings applied): %d again", was, uploads);
    }
    g_ours = uploads;
}

void LogVtStreamingUploadsStats()
{
    if (!g_ours || !g_slot || !*g_slot) return;
    Log("[vtup] r.VT.MaxUploadsPerFrame.Streaming %d (the game's %d); set again after the game's settings %ld time(s) so far",
        (*g_slot)[0], g_replaced, g_rewrites);
}

void VtStreamingUploadsTestSetup(int32_t* volatile* slot)
{
    g_slot = slot;
    g_searched = true;
    g_ours = 0;
    g_replaced = 0;
    g_rewrites = 0;
}

int32_t* volatile* VtStreamingUploadsSlotForTest()
{
    return g_slot;
}

}  // namespace opf
