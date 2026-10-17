// GpuSceneParallelUpload (0.27.x, GpuSceneParallelUpload): the GPU scene's instance upload of a newly streamed area on all workers.
//
// When a big World Partition area arrives (a wood or a rock field: Kvatch below the north cliff, the Brotch camp, the
// Imperial City's stone bridge), its instanced meshes are added to the renderer's GPU scene, and the next frame's
// GPU-scene update writes the per-instance data of every new instance (the transform, compressed, its bounds, custom
// data) into an upload buffer: UE 5.3's FGPUScene::UploadGeneral, the "GPUScene Upload Instances Task"
// (GPUScene.cpp:1144). UE already cuts that work into batches of at most 127 instances (FInstanceBatcher) for a
// ParallelFor, but in 5.3 the ParallelFor is run with EParallelForFlags::ForceSingleThread unless the console variable
// r.GPUScene.ParallelUpdate is not 0, and its default is 0 (UE 5.8 runs it in parallel above 2048 items). So one worker
// thread writes the instances of a whole area one after the other: about 50 ms at Kvatch C (exe+0x1DEBFF0, the batch
// lambda, > exe+0x328C0E0 per instance; worker residue, 2026-10-15). The render thread waits for that task, and the game
// thread waits for the render thread at the end of its next frame: the arrival's longest frame (about 50 ms at the
// user's settings, with every physics fix on).
//
// This sets r.GPUScene.ParallelUpdate to 1. The same code runs, batch by batch; only the threads that run the batches
// differ. Each batch writes its instances into slots of the upload buffers that were sized and assigned before the
// ParallelFor (BeginPreSized, Set_GetRef at offsets computed by the batcher; the primitive pass writes slot ItemIndex),
// so no two batches touch the same memory and the buffers' contents are identical, bit for bit, in any order. The
// upload is still waited for in the same place of the same frame: nothing is later than in the stock game.
//
// The variable is an ordinary render-thread-safe int (TConsoleVariableData<int32>: the game thread's value and the
// render thread's copy). The fix finds its registration by shape (the name, default 0, flags ECVF_RenderThreadSafe,
// then where the auto-variable stores the data pointer AsVariableInt returns) and, once per second from the DLL's
// monitor, writes 1 into both values while the key is on and the value is still the stock 0. Switching the key off
// puts back 0 if the value is still the fix's 1. A value set by the player (Engine.ini, the console) is left alone.
// Any mismatch in the search: the fix stays off with a log line.
#include "common.h"

#include <cstring>

namespace opf {
namespace {

constexpr wchar_t kName[] = L"r.GPUScene.ParallelUpdate";
// lea rdx,[name]; xor r8d,r8d (default 0); mov [rsp+30h],rdi; mov dword [rsp+20h],20h (ECVF_RenderThreadSafe);
// call [rax+18h] (RegisterConsoleVariable, int); lea rcx,[vtable]; mov [auto+8],rax; mov [auto],rcx
constexpr char kRegister[] =
    "48 8D 15 ?? ?? ?? ?? 45 33 C0 48 89 7C 24 30 C7 44 24 20 20 00 00 00 FF 50 18 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
    "48 89 0D ?? ?? ?? ??";
constexpr size_t kObjStore = 33, kAutoStore = 40, kRegisterLen = 47;
// Then (the auto-variable's constructor): mov rax,[rcx]; call [rax+58h] (AsVariableInt); lea rcx,[...]; mov [auto+10h],rax
constexpr char kAsInt[] = "48 8B 01 FF 50 58 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ??";
constexpr size_t kDataStore = 13, kAsIntWindow = 0x100;

int32_t* volatile* g_slot = nullptr;  // where the registration stores TConsoleVariableData<int32>* (two int32 values)
bool g_searched = false;
bool g_ours = false;                  // the value is 1 because the fix wrote it
bool g_reportedOther = false;
bool g_yielded = false;              // someone else changed the value after the fix: not touched until the key goes off

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

bool FindGpuSceneParallelUpload()
{
    if (g_slot) return true;
    if (g_searched) return false;
    g_searched = true;
    const GameModule& game = Game();
    const uint8_t* const name = FindName(game.rdata);
    if (!name) {
        Log("[gpuscene-par] the name r.GPUScene.ParallelUpdate not found (once) in the game: GpuSceneParallelUpload unavailable");
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
        Log("[gpuscene-par] its registration (default 0, render-thread-safe) found %d times (expected once): GpuSceneParallelUpload unavailable", n);
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
        Log("[gpuscene-par] its value pointer is not where expected (%d stores): GpuSceneParallelUpload unavailable", m);
        return false;
    }
    g_slot = reinterpret_cast<int32_t* volatile*>(autoVar + 0x10);
    Log("[gpuscene-par] r.GPUScene.ParallelUpdate registered at exe+0x%X, value pointer at exe+0x%X",
        static_cast<unsigned>(site - game.base), static_cast<unsigned>(autoVar + 0x10 - game.base));
    return true;
}

void SetGpuSceneParallelUpload(bool enabled)
{
    if (!g_slot && !(enabled && FindGpuSceneParallelUpload())) return;
    int32_t* const value = *g_slot;
    if (!value) return;  // not registered yet (the game's static initialisation): next second
    if (!enabled) g_yielded = false;
    if (enabled && !g_ours && !g_yielded) {
        if (value[0] == 0 && value[1] == 0) {
            Write(value, 1);
            g_ours = true;
            Log("[gpuscene-par] r.GPUScene.ParallelUpdate set to 1: the GPU scene's instance upload runs on all workers");
        } else if (!g_reportedOther) {
            g_reportedOther = true;
            Log("[gpuscene-par] r.GPUScene.ParallelUpdate is %d (set by the game's or the player's config): left as it is", value[0]);
        }
    } else if (!enabled && g_ours) {
        g_ours = false;
        if (value[0] == 1) {
            Write(value, 0);
            Log("[gpuscene-par] r.GPUScene.ParallelUpdate put back to 0");
        }
    } else if (g_ours && value[0] != 1) {
        g_ours = false;  // changed by someone else since: theirs now
        g_yielded = true;
        Log("[gpuscene-par] r.GPUScene.ParallelUpdate changed to %d after the fix: left as it is", value[0]);
    }
}

void GpuSceneParallelUploadTestSetup(int32_t* volatile* slot)
{
    g_slot = slot;
    g_searched = true;
    g_ours = false;
    g_reportedOther = false;
    g_yielded = false;
}

int32_t* volatile* GpuSceneParallelUploadSlotForTest()
{
    return g_slot;
}

}  // namespace opf
