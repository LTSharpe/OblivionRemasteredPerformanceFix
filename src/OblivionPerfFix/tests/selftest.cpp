// Offline self-test: maps the game exe into this process (relocated, no imports resolved), applies
// the fixes to that private copy and checks the result against build 19115871. No game code runs,
// with one exception: the descriptor heap guard test calls the engine's 5-instruction leaf
// FD3D12SubAllocatedOnlineHeap::GetTotalSize on a fake object.
// Usage: selftest.exe [path\to\OblivionRemastered-Win64-Shipping.exe]
#include "common.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <unknwn.h>
#include <mmdeviceapi.h>
#pragma comment(lib, "ole32.lib")

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <algorithm>
#include <tuple>
#include <string>
#include <thread>
#include <vector>

// ---- fakes for the descriptor heap guard test ----
struct FakeBlock { uint32_t baseSlot, size, sizeUsed; };
struct FakeHeap {
    void** vtable;
    uint8_t pad[0x20];
    FakeBlock* block;  // FD3D12SubAllocatedOnlineHeap::CurrentBlock (+0x28 in build 19115871)
};
static FakeBlock g_fakeBlock{0, 2000, 0};
static int g_fakeOpens = 0;
static void FakeOpenCommandList(void* heap)
{
    ++g_fakeOpens;
    static_cast<FakeHeap*>(heap)->block = &g_fakeBlock;
}

// ---- fakes for the submit batching test ----
struct FakeView { void** data; int32_t num; };
struct FakeQueue { uint64_t pad; uint32_t queueType; };  // FD3D12Queue::QueueType (+8)
struct FakePayload {
    void* vtable;
    FakeQueue* queue;  // FD3D12PayloadBase::Queue (+8)
    uint8_t rest[0x100];
};
struct FakeList { FakePayload** data; int32_t num, max; };  // FD3D12FinalizedCommands = TArray<FD3D12Payload*>
struct SubmitCall {
    std::vector<void*> lists;
    bool flush = false;
    bool payloads = false;
};
static std::mutex g_callsMutex;
static std::vector<SubmitCall> g_calls;
static void FakeSubmitLists(void*, const FakeView* v, bool flush)
{
    std::lock_guard lock(g_callsMutex);
    SubmitCall c;
    c.lists.assign(v->data, v->data + v->num);
    c.flush = flush;
    g_calls.push_back(c);
}
static void FakeSubmitPayloads(void*, const FakeView*)
{
    std::lock_guard lock(g_callsMutex);
    SubmitCall c;
    c.payloads = true;
    g_calls.push_back(c);
}
static std::vector<SubmitCall> TakeCalls()
{
    std::lock_guard lock(g_callsMutex);
    std::vector<SubmitCall> out;
    out.swap(g_calls);
    return out;
}

// ---- fakes for the cloth config cache test (layout as verified in build 19115871) ----
struct FakeUeArray { void* data; int32_t num, max; };
struct FakeShared { void* object; void* controller; };
struct FakeUeString { const wchar_t* data; int32_t num, max; };
struct ClothView { const void* data; int32_t num, pad; };
struct FakeFacade {
    void* vtable;
    ClothView key, low, high, str, flags;  // +0x08, +0x18, +0x28, +0x38, +0x48
    uint8_t keyIndices[0x50];             // +0x58
    void* collection;                     // +0xA8 (TSharedPtr object), +0xB0 (controller)
    void* controller;
};
static_assert(offsetof(FakeFacade, key) == 0x08 && offsetof(FakeFacade, str) == 0x38 && offsetof(FakeFacade, flags) == 0x48 &&
              offsetof(FakeFacade, collection) == 0xA8 && sizeof(FakeFacade) == 0xB8, "facade layout");
struct FakeConfig { uint64_t legacySingleLod; FakeUeArray collections; FakeUeArray properties; };  // +0x08, +0x18
struct FakeComponentArrays { FakeUeArray collections; FakeUeArray facades; };  // facades 0x10 after the collections
struct FakeLod {  // one LOD's property data and the facade viewing it
    FakeUeString keys[3], strings[3];
    float low[9], high[9];
    uint8_t flags[3];
    uint64_t collection;  // stands in for the FManagedArrayCollection
    FakeFacade facade;
};
static const wchar_t* const kFakeKeys[3] = {L"DampingCoefficient", L"WindVelocity", L"MaxDistance"};
static void FillLod(FakeLod& l, int salt)
{
    for (int i = 0; i < 3; ++i) {
        l.keys[i] = {kFakeKeys[i], static_cast<int32_t>(wcslen(kFakeKeys[i]) + 1), static_cast<int32_t>(wcslen(kFakeKeys[i]) + 1)};
        l.strings[i] = i == 2 ? FakeUeString{L"MaxDistanceMap", 15, 15} : FakeUeString{nullptr, 0, 0};
        l.flags[i] = i == 1 ? 3 : 1;  // Enabled, WindVelocity also Animatable
    }
    for (int i = 0; i < 9; ++i) {
        l.low[i] = static_cast<float>(salt) + 0.5f * static_cast<float>(i);
        l.high[i] = static_cast<float>(salt) + static_cast<float>(i);
    }
    l.facade = {};
    l.facade.key = {l.keys, 3, 0};
    l.facade.low = {l.low, 3, 0};
    l.facade.high = {l.high, 3, 0};
    l.facade.str = {l.strings, 3, 0};
    l.facade.flags = {l.flags, 3, 0};
    l.facade.collection = &l.collection;
}
static int g_fakeInitCalls = 0;
static const void* g_fakeInitArgs[2];
static bool g_fakeInitCopies = true;  // false: a broken "copy" that changes nothing
// Stands in for FClothingSimulationConfig::Initialize: copies each LOD's property data from the
// component's facades into the config's own arrays (the config keeps its own collections).
static void FakeInitialize(void* config, const void* collections)
{
    ++g_fakeInitCalls;
    g_fakeInitArgs[0] = config;
    g_fakeInitArgs[1] = collections;
    if (!g_fakeInitCopies) return;
    auto* cfg = static_cast<FakeConfig*>(config);
    auto* component = static_cast<const FakeComponentArrays*>(collections);
    for (int i = 0; i < component->collections.num; ++i) {
        const FakeFacade* src = static_cast<FakeFacade* const*>(component->facades.data)[i];
        FakeFacade* dst = static_cast<FakeFacade**>(cfg->properties.data)[i];
        const int n = src->key.num;
        for (int k = 0; k < n; ++k) {
            const_cast<FakeUeString*>(static_cast<const FakeUeString*>(dst->key.data))[k] = static_cast<const FakeUeString*>(src->key.data)[k];
            const_cast<FakeUeString*>(static_cast<const FakeUeString*>(dst->str.data))[k] = static_cast<const FakeUeString*>(src->str.data)[k];
        }
        memcpy(const_cast<void*>(dst->low.data), src->low.data, n * 12);
        memcpy(const_cast<void*>(dst->high.data), src->high.data, n * 12);
        memcpy(const_cast<void*>(dst->flags.data), src->flags.data, n);
        dst->key.num = dst->low.num = dst->high.num = dst->str.num = dst->flags.num = n;
    }
    cfg->collections.num = cfg->properties.num = component->collections.num;
}

// ---- fakes for the display mode list test (DisplayModeCache): a monitor with N modes, as USER32 EnumDisplaySettingsW reports
// them (it writes exactly the first dmSize = 0xBC bytes of the DEVMODEW and leaves the rest of the buffer alone) ----
struct FakeDisplay {
    DWORD count = 683;
    DWORD changedIndex = 0xFFFFFFFF;  // this mode reports another refresh rate
    DWORD curW = 3840, curH = 2160;   // ENUM_CURRENT_SETTINGS
    bool writeBeyond = false;         // a Windows that writes past dmSize: the fix must stay off
    int calls = 0;
};
static FakeDisplay g_fd;
static constexpr WORD kFakeWritten = 0xBC;
static void FakeMode(DWORD i, DEVMODEW* dm)
{
    DEVMODEW m;
    memset(&m, 0, sizeof m);
    wcscpy_s(m.dmDeviceName, L"cdd");
    m.dmSpecVersion = m.dmDriverVersion = 0x401;
    m.dmSize = kFakeWritten;
    m.dmFields = 0x7C0080;
    m.dmBitsPerPel = i % 2 ? 32 : 8;
    m.dmPelsWidth = 640 + (i / 6) % 40 * 80;
    m.dmPelsHeight = 480 + (i / 240) * 60;
    m.dmDisplayFrequency = 60 + i % 3 + (i == g_fd.changedIndex ? 15 : 0);
    memcpy(dm, &m, kFakeWritten);
    if (g_fd.writeBeyond) memset(reinterpret_cast<uint8_t*>(dm) + kFakeWritten, 0x11, sizeof(DEVMODEW) - kFakeWritten);
}
static BOOL WINAPI FakeEnumDisplaySettings(LPCWSTR device, DWORD mode, DEVMODEW* dm)
{
    ++g_fd.calls;
    if (!device || wcscmp(device, L"\\\\.\\DISPLAY1") != 0) return FALSE;
    if (mode == ENUM_CURRENT_SETTINGS) {
        FakeMode(0, dm);
        dm->dmPelsWidth = g_fd.curW;
        dm->dmPelsHeight = g_fd.curH;
        return TRUE;
    }
    if (mode >= g_fd.count) return FALSE;
    FakeMode(mode, dm);
    return TRUE;
}
using EnumDisplaySettingsFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
// GetMonitorsInfo's two loops: Altar's reuses one buffer (dmSize set once), UE's zeroes it before every call (dmSize
// 0). Returns the whole buffer after each successful call, and how many reads reached the fake Windows.
static std::vector<std::vector<uint8_t>> EnumerateModes(EnumDisplaySettingsFn fn, bool zeroEachCall, uint8_t fill, int* calls,
                                                        const wchar_t* device = L"\\\\.\\DISPLAY1", DWORD stopAfter = 0xFFFFFFFF)
{
    std::vector<std::vector<uint8_t>> out;
    const int before = g_fd.calls;
    DEVMODEW dm;
    memset(&dm, fill, sizeof dm);
    dm.dmSize = sizeof(DEVMODEW);
    dm.dmDriverExtra = 0;
    for (DWORD i = 0; i < stopAfter; ++i) {
        if (zeroEachCall) memset(&dm, 0, sizeof dm);
        if (!fn(device, i, &dm)) break;
        out.emplace_back(reinterpret_cast<uint8_t*>(&dm), reinterpret_cast<uint8_t*>(&dm) + sizeof dm);
    }
    if (calls) *calls = g_fd.calls - before;
    return out;
}

// ---- fakes for the focus restore test (RestoreFsrAfterAltTab): the game's focus handler and menu-visibility function, the UI
// subsystem's menu flag and two console variables, as the fix reads them ----
struct FakeCvar { void** vtable; bool value; int32_t flags; int32_t lastSetBy; int sets; };
static bool FakeGetBool(void* cvar) { return static_cast<FakeCvar*>(cvar)->value; }
static int32_t FakeGetFlags(void* cvar) { return static_cast<FakeCvar*>(cvar)->flags; }
static void FakeSet(void* cvar, const wchar_t* v, int32_t setBy)
{
    auto* c = static_cast<FakeCvar*>(cvar);
    c->value = v && v[0] == L'1';
    c->lastSetBy = setBy;
    ++c->sets;
}
static void* g_fakeCvarVtable[64];
static FakeCvar g_fakeFsr{g_fakeCvarVtable, true, 0x0A000004, 0, 0}, g_fakeAltarFsr{g_fakeCvarVtable, true, 0, 0, 0};
static uint8_t g_fakeUi[0x200];
static uint8_t g_fakeManager[0x100];
static uint8_t g_fakeFreeze[0x100];   // UVFreezeInMenuSubsystem: mode +0x3C, named-freeze count +0xB0
static bool g_fakeFreezeThere = true;
static uint32_t g_fakeMenuFlag = 0xE2;
static int g_fakeHandlerCalls = 0;
static void* g_fakeHandlerThis = nullptr;
// as the game's handler: loss -> FSR off if Altar.FSR3.Enabled; gain -> FSR on unless the menu flag is set
static uint64_t FakeFocusHandler(void* manager, uint8_t active)
{
    ++g_fakeHandlerCalls;
    g_fakeHandlerThis = manager;
    if (!g_fakeAltarFsr.value) return 0;
    if (!active) g_fakeFsr.value = false;
    else if (!g_fakeUi[g_fakeMenuFlag]) g_fakeFsr.value = true;
    return 0;
}
static uint64_t FakeMenuSetter(void* ui, uint8_t visible, void*, void*, uint64_t)
{
    static_cast<uint8_t*>(ui)[g_fakeMenuFlag] = visible ? 1 : 0;
    return 0;
}

using namespace opf;

// ---- fakes for LumenKeepArrayCapacity and UploadMemoryKeep (stream churn) ----
static int g_fakeRemoveAtCalls = 0;
static bool g_fakeRemoveAtShrink = true;
static void FakeRemoveAt(void*, int32_t, int32_t, bool allowShrinking)
{
    ++g_fakeRemoveAtCalls;
    g_fakeRemoveAtShrink = allowShrinking;
}
static int g_fakeShrinkCalls = 0;
static void FakeShrink(void*) { ++g_fakeShrinkCalls; }
static uint64_t g_fakeBuddyLag = 0, g_fakePoolLag = 0;
static bool g_fakePoolForce = false;
static void FakeBuddyClean(void*, uint64_t lag) { g_fakeBuddyLag = lag; }
// Stands in for TArray<80-byte>::ResizeGrow: ArrayMax from ArrayNum the way the game's does (Num + 3/8 Num + 16).
struct FakeTArray { void* data; int32_t num, max; };
static int32_t g_fakeGrowSawNum = 0, g_fakeGrowOldNum = 0;
static void FakeResizeGrow(void* array, int32_t oldNum)
{
    auto* a = static_cast<FakeTArray*>(array);
    g_fakeGrowSawNum = a->num;
    g_fakeGrowOldNum = oldNum;
    a->max = a->num + 3 * a->num / 8 + 16;
}
// GpuSceneKeepCapacity fakes: GetDataStrideInFloat4s (4 float4s per instance) and ResizeStructuredBufferIfNeeded (records the size).
static uint32_t FakeStride() { return 4; }
static uint32_t g_fakeResizeBytes = 0;
static void* FakeResizeStructured(void*, void**, uint32_t numBytes, const wchar_t*)
{
    g_fakeResizeBytes = numBytes;
    return nullptr;
}

static void FakePoolClean(void*, uint64_t lag, bool force)
{
    g_fakePoolLag = lag;
    g_fakePoolForce = force;
}

// ---- fakes for NpcCreationPacing (NPC creation pacing): a pairing queue simulator ----
// Commands of one OT frame are posted before the GT frame that may read them; reading past the last posted end command
// would mean the GT waits for an OT frame that itself waits for this GT frame: the simulator flags that as a deadlock.
enum FakeKind { kFakeEnd, kFakeCreate, kFakeGeneric };
struct FakeCmd {
    void* vtable;
    int kind;
    int id;
    void* entry;           // +0x10
    uintptr_t fn;          // +0x18
    uint64_t costUs;       // +0x20
    uint64_t pad;          // +0x28
    float x, x2, y, y2, z, z2;  // +0x30 / +0x38 / +0x40 (positions at 8-byte steps)
    uint8_t rest[0x28];
};
static_assert(offsetof(FakeCmd, entry) == 0x10 && offsetof(FakeCmd, fn) == 0x18 && offsetof(FakeCmd, x) == 0x30 &&
              offsetof(FakeCmd, y) == 0x38 && offsetof(FakeCmd, z) == 0x40, "fake command layout");
static uint64_t g_pcClock = 0;
static uint64_t PcClock() { return g_pcClock; }
static bool PcPlayer(double* x, double* y, double* z) { *x = 0; *y = 0; *z = 0; return true; }
static FakeCmd g_pcCmds[256];
static int g_pcNumCmds = 0;
static FakeCmd* g_pcQueue[4096];
static size_t g_pcHead = 0, g_pcTail = 0;
static long g_pcSem = 0;            // posted, not yet taken
static bool g_pcDeadlock = false;
static std::vector<int> g_pcRan;    // ids of commands executed (units and plain ones)
static std::vector<int> g_pcSeq;    // commands, delegates and flows, in the order they ran
static int g_pcEnds = 0;
static uint8_t g_pcSession[0x100];
static void* g_pcSemVtable[4];
static void* g_pcSemObj[1] = {g_pcSemVtable};
static void FakeSemUnlock(void*) { ++g_pcSem; }
static void* FakeFindSession(void*, int32_t) { return g_pcSession; }
static void* g_pcCreateHook = nullptr;
static void* g_pcGenericHook = nullptr;
static int g_pcOtherSessionRuns = 0;
static int g_pcRevEnds = 0;          // reverse end commands the GT sent (the OT may post its next frame after each)
static uint8_t g_pcWriter[0x100];    // the GT's writer session (open flag at +0x4E)
static uint8_t* FakeChooseWriter(void*) { return g_pcWriter; }
static uint8_t FakeRunSession(void*, int32_t key)
{
    if (key >= 1000) { g_pcRan.push_back(key); g_pcSeq.push_back(key); return 4; }  // a level flow: runs, done
    if (key != -1) { ++g_pcOtherSessionRuns; return 4; }
    g_pcSession[0x4C] = 1;
    for (;;) {
        if (g_pcHead == g_pcTail) { g_pcDeadlock = true; return 4; }
        --g_pcSem;
        FakeCmd* c = g_pcQueue[g_pcHead];
        *reinterpret_cast<FakeCmd**>(g_pcSession + 0x18) = c;
        if (c->kind == kFakeEnd) {
            ++g_pcHead;
            ++g_pcEnds;
            g_pcSession[0x4C] = 0;
        } else {
            using Exec = uint64_t (*)(void*);
            const uint64_t size = reinterpret_cast<Exec>(c->kind == kFakeCreate ? g_pcCreateHook : g_pcGenericHook)(c);
            if (size) ++g_pcHead;
        }
        if (!g_pcSession[0x4C]) return 4;
    }
}
static uint64_t FakeCmdExec(void* cmd)
{
    FakeCmd* c = static_cast<FakeCmd*>(cmd);
    g_pcRan.push_back(c->id);
    g_pcSeq.push_back(c->id);
    g_pcClock += c->costUs;
    return 0x80;
}
static void FakeEndHook(void*, void*, uint64_t, uint64_t) { ++g_pcRevEnds; g_pcWriter[0x4E] = 0; }
static void PcPost(int kind, int id, uint64_t costUs, float x = 0, uintptr_t fn = 1)
{
    FakeCmd& c = g_pcCmds[g_pcNumCmds++ % 256];
    memset(&c, 0, sizeof c);
    c.kind = kind;
    c.id = id;
    c.costUs = costUs;
    memcpy(reinterpret_cast<uint8_t*>(&c) + 0x60, &x, sizeof x);  // old-engine x at +0x60
    c.fn = fn;
    g_pcQueue[g_pcTail++] = &c;
    ++g_pcSem;
}
// delegates
struct FakeActor { uint8_t bytes[0x300]; };
static FakeActor g_pcActors[8], g_pcRoots[8];
static void* FakeWeakGet(const void* weak) { return *static_cast<void* const*>(weak); }
static std::vector<int> g_pcDelRan;
static int g_pcDtors = 0;
static int g_pcNestParent = -1;            // the delegate id whose run completes the nested ones at once
static void* g_pcNested[4] = {};
static bool (*g_pcNestHook)(void*) = nullptr;
static void (*g_pcInUnit)(int id) = nullptr;  // called inside each delegate unit (ClothBuildDefer's questions to pacing)
static bool FakeDelExec(void* inst)
{
    const uint8_t* p = static_cast<const uint8_t*>(inst);
    const int id = *reinterpret_cast<const int*>(p + 0x20);
    g_pcDelRan.push_back(id);
    if (g_pcInUnit) g_pcInUnit(id);
    g_pcSeq.push_back(id);
    g_pcClock += *reinterpret_cast<const uint64_t*>(p + 0x28);
    if (id == g_pcNestParent)
        for (void* n : g_pcNested)
            if (n) g_pcNestHook(n);
    return true;
}
static void* FakeDelDtor(void* inst, uint32_t) { ++g_pcDtors; return inst; }
static void PcActor(int i, double x)
{
    memset(&g_pcActors[i], 0, sizeof g_pcActors[i]);
    memset(&g_pcRoots[i], 0, sizeof g_pcRoots[i]);
    *reinterpret_cast<void**>(g_pcActors[i].bytes + 0x1A0) = &g_pcRoots[i];
    *reinterpret_cast<void**>(g_pcRoots[i].bytes + 0x20) = &g_pcActors[i];
    *reinterpret_cast<double*>(g_pcRoots[i].bytes + 0x280) = x;
}
struct FakeDelInst { void* vtable; uint64_t pad; uint64_t handle; void* weak; int id; int pad2; uint64_t costUs; };
static_assert(offsetof(FakeDelInst, weak) == 0x18 && offsetof(FakeDelInst, id) == 0x20 && offsetof(FakeDelInst, costUs) == 0x28,
              "fake delegate layout");

static int g_failures = 0;

// The self-test's own log (selftest.log) has a line containing text.
// Frame fence: fakes for RHIThreadFence (hands out events with the engine's layout: subsequents list head at +0,
// reference count at +0x38) and for the event recycler.
struct FakeEvent { uint64_t head; uint8_t pad[0x30]; volatile LONG ref; };
static FakeEvent g_fakeEvents[16];
static int g_fakeFences = 0, g_fakeRecycled = 0;
static void* g_fakeLastRecycled = nullptr;
static bool g_fakeFenceLock = false, g_fakeFenceAfterCommand = false;
static const uint8_t* g_fakeCommandVtable = nullptr;
static uint8_t g_fakeStack[4096];
static void** FakeThreadFence(void* cmdList, void** out, bool setLockFence)
{
    (void)cmdList;
    FakeEvent& e = g_fakeEvents[g_fakeFences++ % 16];
    e.head = 0;
    e.ref = 1;
    *out = &e;
    g_fakeFenceLock = setLockFence;
    g_fakeFenceAfterCommand = *reinterpret_cast<const uint8_t**>(g_fakeStack) == g_fakeCommandVtable;
    return out;
}
static void FakeRecycle(void* ev)
{
    ++g_fakeRecycled;
    g_fakeLastRecycled = ev;
}

static bool LogContains(const char* text)
{
    LogFlush();  // the log's writer thread may not have written the line yet
    bool hit = false;
    if (FILE* f = _wfsopen(L"selftest.log", L"r", _SH_DENYNO)) {
        char line[1024];
        while (!hit && fgets(line, sizeof line, f)) hit = strstr(line, text) != nullptr;
        fclose(f);
    }
    return hit;
}


// ---- FrameGenPacingTuning fakes: AMD's ffxCreateContext / ffxDestroyContext / ffxConfigure ----
namespace fgp {
int calls = 0;
uint64_t type = 0, key = 0;
float margin = 0, variance = 0;
bool spin = true, waitObj = true;
uint32_t spinTime = 0;
void* ctxSeen = nullptr;
uint32_t FakeCreate(void** ctx, void*, const void*) { *ctx = reinterpret_cast<void*>(0x1234); return 0; }
uint32_t FakeDestroy(void** ctx, const void*) { *ctx = nullptr; return 0; }
uint32_t FakeConfigure(void** ctx, const void* desc)
{
    ++calls;
    ctxSeen = *ctx;
    const auto* d = static_cast<const uint8_t*>(desc);
    memcpy(&type, d, 8);
    memcpy(&key, d + 16, 8);
    const uint8_t* t;
    memcpy(&t, d + 32, 8);
    memcpy(&margin, t, 4);
    memcpy(&variance, t + 4, 4);
    spin = t[8] != 0;
    memcpy(&spinTime, t + 12, 4);
    waitObj = t[16] != 0;
    return 0;
}
bool Near(float a, float b) { return a > b - 1e-6f && a < b + 1e-6f; }
// ffxQuery answering FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION (6) like a driver provider (bit 31 of the id set)
uint64_t queryType = 0;
void* queryCtx = nullptr;
uint32_t FakeQuery(void** ctx, void* desc)
{
    queryCtx = *ctx;
    auto* d = static_cast<uint8_t*>(desc);
    memcpy(&queryType, d, 8);
    if (queryType != 6) return 1;
    const uint64_t id = 0xF600000080401003ull;
    static const char name[] = "4.0.1";
    const char* p = name;
    memcpy(d + 16, &id, 8);
    memcpy(d + 24, &p, 8);
    return 0;
}
uint32_t FakeQueryFails(void**, void*) { return 2; }
// the same query answered like a provider inside AMD's DLL (bit 31 of the id clear)
uint32_t FakeQueryDll(void** ctx, void* desc)
{
    queryCtx = *ctx;
    auto* d = static_cast<uint8_t*>(desc);
    memcpy(&queryType, d, 8);
    if (queryType != 6) return 1;
    const uint64_t id = 0xF600000000401007ull;
    static const char name[] = "3.1.7";
    const char* p = name;
    memcpy(d + 16, &id, 8);
    memcpy(d + 24, &p, 8);
    return 0;
}
}  // namespace fgp

static void Check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// PerceptionLosPrefetch's sites, captured before any test rewrites the mapped exe (the lab PerceptionIgnoreReuse test installs
// into the same round and helper; our review of PR #55). The perclos block below checks these values.
struct PerclosSites {
    bool captured = false;
    const char* why = "not captured";
    uint32_t rr = 0, rc = 0, rh = 0, rf = 0, join = 0, ga = 0, gc = 0, eye = 0, lwi = 0;
};
static PerclosSites g_perclos;
static void PerclosCapture(const GameModule& game)
{
    g_perclos.captured = true;
    g_perclos.why = PerceptionLosTestDiscover(game, &g_perclos.rr, &g_perclos.rc, &g_perclos.rh, &g_perclos.rf);
    g_perclos.join = PerceptionLosTestJoinSite(game);
    PerceptionLosTestGatherSites(game, &g_perclos.ga, &g_perclos.gc);
    PerceptionLosTestGuardSites(game, &g_perclos.eye, &g_perclos.lwi);
}

// A fix whose code was not found (e.g. on another game build) leaves its hooks unset. Its remaining checks call
// through those hooks, so they are skipped with one FAIL line instead of crashing the self-test.
static bool Installed(bool ok, const char* fix)
{
    if (!ok) {
        Check(false, fix);
        printf("      (not installed: its remaining checks are skipped; its own log line is in selftest.log)\n");
    }
    return ok;
}

// A fast hash of a model's state (the models only compare hashes between runs): FNV-style steps over 8-byte words in
// four independent lanes, so the multiplies overlap. The PhysicsQueueShrinkOnce/39 models hash megabytes after every pass or removal.
static uint64_t HashWords(uint64_t seed, const void* data, size_t bytes)
{
    constexpr uint64_t kPrime = 1099511628211ull;
    const uint8_t* const p = static_cast<const uint8_t*>(data);
    uint64_t h[4] = {seed, seed ^ 0x9E3779B97F4A7C15ull, seed ^ 0xC2B2AE3D27D4EB4Full, seed ^ 0x165667B19E3779F9ull};
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32)
        for (int l = 0; l < 4; ++l) {
            uint64_t w;
            memcpy(&w, p + i + 8 * l, 8);
            h[l] = (h[l] ^ w) * kPrime;
        }
    for (; i < bytes; ++i) h[0] = (h[0] ^ p[i]) * kPrime;
    uint64_t r = bytes;
    for (int l = 0; l < 4; ++l) {
        r = (r ^ h[l]) * kPrime;
        r ^= r >> 32;
    }
    return r;
}

#include "selftest_bulkinsert.h"  // BulkIndexInsert's tree model and checks
#include "selftest_push.h"        // PhysicsPushParallel's sites and push-loop model
#include "selftest_shrink.h"      // PhysicsQueueShrinkOnce's site and catch-up model
#include "selftest_savebytes.h"     // SaveBytesBulk's sites and the game's array serialization vs the hook; SaveThumbnailAsync
#include "selftest_saverefid.h"     // SaveRefIdIndex's sites and the helper vs the linear search
#include "selftest_rigsetup.h"      // RigVmHashMemo / RigLogicMapCache: sites, memo and map replay with fakes
#include "selftest_rigtmpl.h"       // RigImportTemplate: discovery, offsets, the RemoveElement guard, cap and table with fakes
#include "selftest_farpawn.h"       // FarPawnMoveHalfRate / FarHorseMove: discovery, cadence, root motion, input mean, fakes
#include "selftest_createahead.h"   // CreateAhead: sites, parameter / scale rules, build / adopt / drop rules with fakes
#include "selftest_createpreload.h" // CreatePreload: sites, the form-asset map readers with a fake holder, per-command wait count
#include "selftest_cellpreload.h"   // CellObjectPreload: sites, the cell walk and the request rules with a fake look-up and clock
#include "selftest_impactpreload.h" // ImpactPreload: sites, the preload state machine with fakes
#include "selftest_npcbodies.h"     // NpcBodiesOneLock: sites and the one-hold wrapper with a fake scene lock
#include "selftest_rtscene.h"       // RtSceneBufferKeep: sites, patch and the stub's decisions
#include "selftest_poolkeep.h"      // GpuPoolKeepMB: sites, the keep decision, the hook on fake allocators, the patch
#include "selftest_rtbatch.h"       // RayTracingBuildBatch: sites, patch and the queue decision
#include "selftest_nanite.h"        // NaniteInitialRootPages (lab: NaniteStreamProbe / Nanite*MinMB with a fake resize)
#include "selftest_waterinfo.h"     // the water-info capture keys (lab: and WaterInfoProbe): sites and patches
#include "selftest_fpscap.h"        // FrameRateLimitAtStart: sites, install and the hook's decisions with fakes
#include "selftest_navlink.h"       // NavLinkFast: sites, the fast search and the skip rule against the game's search
#include "selftest_readback.h"     // ScreenshotReadbackConvert's site and the game's conversion vs the fast one
#include "selftest_parallel.h"     // ScreenshotParallel's sites, stubs and row bands
#include "selftest_oscache.h"      // OsPageCache's sites and the cache core with fake OS functions
#include "selftest_pso.h"          // PsoPrecompileFast's sites, trampoline and mode choices with fakes
#include "selftest_destroybatch.h" // PhysicsDestroyBatch: sites and the wrapper with fakes
#include "selftest_clothreg.h"    // ClothRegisterLite: sites, lite builds and their completion with fakes
#include "selftest_clothdefer.h"     // ClothBuildDefer: sites, hooks and the defer rule (after ClothRegisterLite)
#include "selftest_bodyspread.h"   // BodyRemovalSpread: sites on the mapped exe, the queue / drain / drop logic with fakes
#include "selftest_epicport.h"     // StreamOutEndPlaySlice, ResetLoadersNoFlush: sites on the mapped exe, the hooks with fakes
#include "selftest_barfinish.h"    // StartupBarFinish: the loops' step calls on the mapped exe, the hook in the game's loop with a fake step
#include "selftest_gapfix.h"       // TopicSort (the game's own sort vs ours on the mapped exe) and PluginPrefetch (path, list parse)
#include "selftest_gchold.h"       // GcPassAfterBusy: sites on the mapped exe, the hold with a fake engine frame by frame
#include "selftest_gcprep.h"       // GcPrepAhead: sites on the mapped exe, the step with a fake pending list through GcPassAfterBusy's hook
#include "selftest_gcmark.h"       // GcMarkPriority: sites on the mapped exe, refusals and the patch on fake code, the raise with fake thunks
#include "selftest_streamgran.h"    // StreamRegisterGranularity: the cvar's int on the mapped exe, set / live / left alone
#include "selftest_physcap.h"     // PhysicsSubstepCap: names in the mapped exe, find / write / live / left-alone rules on fake objects, Engine.ini lines
#include "selftest_hooks.h"       // other mods' hooks (call-only look-ups, refused patch sites) and the load stage
#include "selftest_navprio.h"      // NavQueryPriority: the name on the mapped exe, the set / live / left-alone rules with a fake console variable
#include "selftest_indirectargs.h"  // IndirectArgBufferPool: the cvar's int on the mapped exe, start-up rules
#include "selftest_filereads.h"     // ParallelFileReads: the reader on the mapped exe, the replacement against stand-ins and a real file
#include "selftest_lumennarrow.h"   // LumenCardsNarrow: sites, refusal, stub, unwind, the decision with fakes (lab: the proof)
#include "selftest_lumenscan.h"    // LumenScanCull: sites, the call-site stub, the cull against the game's own scan body on fakes
#include "selftest_waterinfo_skip.h"  // WaterInfoSkip: layout reads on the mapped exe, the same-input decision with fakes
#include "selftest_waterspread.h"     // WaterInfoSpread: sites + shared zone layout on the mapped exe, the defer / merge logic with fakes
#include "selftest_mapspread.h"       // MapPopupSpread: sites by pattern on the mapped exe, the keep / drop / reuse logic with fakes
#include "selftest_gpushot.h"         // GpuMenuShot: sites by pattern on the mapped exe, the shader arithmetic, the shader on this GPU
#include "selftest_thumbleft.h"        // SaveThumbLeftoverFix: sites by pattern, the removal decision, the bound-entry search
#include "selftest_repath.h"        // RepathParallel: discovery on the mapped exe, the key, the nav-edit counter, the link marker
#include "selftest_installorder.h"  // start-up install order: keys whose Install* reads another fix's install state come after it
#include "selftest_bufferfloor.h"   // InstanceBufferFloor: gate, the reserve (lab SceneBufferFloorMiB: sites, stub, hook, patches)
#include "selftest_pairrace.h"      // PairReleaseOnce: discovery, the fix's writes and counting stubs (lab PairRaceProbe: a fake pool)
#include "selftest_otframe.h"       // OtFrameTrim: discovery, install, the consumer and timer with fakes
#include "selftest_loosemiss.h"     // LooseMissCache: discovery, install, the finder's checks with a fake _access
#include "selftest_poweranim.h"     // PowerAttackAnimKeep: discovery, fakes, the hook, the game's release through the patch
#include "selftest_flowonce.h"      // FlowOverlapsOnce: discovery, install, the hook with a fake adoption and UpdateOverlaps
#include "selftest_animpar.h"      // AnimTeardownParallel's shapes and the helper queue with fakes
#include "selftest_cellbuf.h"      // CellBufferPacedEvict's shapes, the clamp stub and the buffer LRU with and without the fix
#include "selftest_rowpreload.h"   // RowPreload: sites (CellBufferPacedEvict's too) and the preload logic with fakes
#include "selftest_vramcounter.h"  // VramCounterFix: the Show VRAM counter
#include "selftest_hangcapture.h"  // GpuPassMarkers and the GPU crash report's per-queue DRED / history lines
#include "selftest_shaderscreen.h"  // ShaderScreenAfterDriverChange
#include "selftest_doordiag.h"     // diagnostic DoorDiag: sites (locate only)
#include "selftest_streamload.h"   // StreamBehindLoadingScreen: sites (locate only)
#include "selftest_ledger.h"       // diagnostic HitchLedger: sites, record/ring/csv format, cost per frame
#include "selftest_hdrmenu.h"      // HdrMenuRows: the engine helpers it calls
#include "selftest_hdrgamma.h"     // HdrGamma22: the HDR tone curve's block and the fit
#include "selftest_hdrmenubg.h"    // HdrMenuBackground: the menu background in HDR10
#include "selftest_hdrthumb.h"     // HdrSaveThumbnail: save thumbnails in HDR10
#include "selftest_objreplace.h"   // ObjectReplaceCrashGuard: the in-place destroy site and the stub with a fake GUObjectArray
#include "selftest_taskwait.h"     // TaskWaitDeadlockGuard: the task graph's worker wait, the wake-up protocol against fakes
#include "selftest_display_native.h"  // MonitorNativeFromWindows / FullscreenAllResolutions
#include "selftest_skinsss.h"      // SeparableSkinAbove4K: the switch logic against a fake variable
#include "selftest_scan.h"         // FindAll: the fast pattern search against the reference search
#include "selftest_ticksplit.h"        // diagnostic TickSplit: sites, entries/windows/top rows, cost per call (lab: perception split)

// ---- fakes for the parallel flush test (physics_flush.cpp, PhysicsFlushParallel) ----
namespace pfl {
struct Handle { void* gt; void* particle; uint32_t unique; uint8_t query[16]; uint8_t sim[16]; uint8_t canPreFilter; };
struct Call { bool update; const void* handleOrOp; Handle h; const void* bounds; uint8_t hasBounds; uint16_t index; DWORD tid; };
std::vector<Call> g_calls;
std::vector<Call> g_icalls;  // the physics-thread copy's calls (PhysicsFlushThreeWay)
alignas(16) uint8_t g_evolution[0x500];
alignas(16) uint8_t g_container[0x200];
struct ParticleHandle { void* container; int32_t index; int32_t pad; } g_particles[4];
uint32_t g_unique[4] = {101, 102, 103, 104};
void* g_gt[4] = {reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222), reinterpret_cast<void*>(0x3333),
                 reinterpret_cast<void*>(0x4444)};
double g_bounds[4][6];
uint8_t g_hasBounds[4] = {1, 0, 1, 1};
alignas(16) uint8_t g_ops[4 * 0x48];
int32_t g_dynamicFlag = 1;
void* g_vtable[64];
struct Coll { void** vtable; uint8_t pad[0x38]; } g_external, g_internal;
int g_originalCalls = 0;
void* g_externalSeenByOriginal = nullptr;
void* g_internalSeenByOriginal = nullptr;
DWORD g_originalTid = 0;
bool FakeRemove(void* coll, const void* op, uint16_t index)
{
    if (coll == &g_external) g_calls.push_back({false, op, {}, nullptr, 0, index, GetCurrentThreadId()});
    if (coll == &g_internal) g_icalls.push_back({false, op, {}, nullptr, 0, index, GetCurrentThreadId()});
    return false;
}
bool FakeUpdate(void* coll, const Handle* h, const void* bounds, uint8_t hasBounds, uint16_t index)
{
    if (coll == &g_external) g_calls.push_back({true, h, h ? *h : Handle{}, bounds, hasBounds, index, GetCurrentThreadId()});
    if (coll == &g_internal) g_icalls.push_back({true, h, h ? *h : Handle{}, bounds, hasBounds, index, GetCurrentThreadId()});
    return false;
}
void FakeQuery(void* particle, void* out)
{
    for (int i = 0; i < 16; ++i)
        static_cast<uint8_t*>(out)[i] |= static_cast<uint8_t>(0xA0 + static_cast<ParticleHandle*>(particle)->index);
}
void FakeSim(void* particle, void* out)
{
    for (int i = 0; i < 16; ++i)
        static_cast<uint8_t*>(out)[i] |= static_cast<uint8_t>(0xB0 + static_cast<ParticleHandle*>(particle)->index);
}
// The game's function, as far as the game-thread copy goes: it calls the copy's slots and sets its timestamp (+0xC).
void FakeOriginal(uint8_t* evolution)
{
    ++g_originalCalls;
    g_originalTid = GetCurrentThreadId();
    void* const ext = *reinterpret_cast<void**>(evolution + 0x2E8);
    void* const in = *reinterpret_cast<void**>(evolution + 0x2E0);
    g_externalSeenByOriginal = ext;
    g_internalSeenByOriginal = in;
    using Upd = bool (*)(void*, const void*, const void*, uint8_t, uint16_t);
    void* const* vi = *static_cast<void* const* const*>(in);
    reinterpret_cast<Upd>(vi[0x88 / 8])(in, nullptr, nullptr, 0, 0);  // lands in the second stand-in with PhysicsFlushThreeWay
    void* const* vt = *static_cast<void* const* const*>(ext);
    reinterpret_cast<Upd>(vt[0x88 / 8])(ext, nullptr, nullptr, 0, 0);  // lands in the stand-in when split
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(in) + 0xC) = *reinterpret_cast<int32_t*>(evolution + 0x358);
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(ext) + 0xC) = *reinterpret_cast<int32_t*>(evolution + 0x358);
    *reinterpret_cast<int32_t*>(evolution + 0x388) = 0;
}
int g_cisaCalls = 0;
LONG g_copySeen = 0;
volatile LONG* g_copyVarForFake = nullptr;
void FakeCisa(uint8_t*, uint8_t)
{
    ++g_cisaCalls;
    g_copySeen = g_copyVarForFake ? *g_copyVarForFake : 0;  // what the step's task would read
}
void Reset()
{
    g_calls.clear();
    g_icalls.clear();
    g_originalCalls = 0;
    g_externalSeenByOriginal = nullptr;
    g_internalSeenByOriginal = nullptr;
    memset(g_internal.pad, 0, sizeof g_internal.pad);
    memset(g_evolution, 0, sizeof g_evolution);
    for (void*& s : g_vtable) s = nullptr;
    g_vtable[0x80 / 8] = reinterpret_cast<void*>(&FakeRemove);
    g_vtable[0x88 / 8] = reinterpret_cast<void*>(&FakeUpdate);
    g_external.vtable = g_internal.vtable = g_vtable;
    memset(g_external.pad, 0, sizeof g_external.pad);
    *reinterpret_cast<uint32_t**>(g_container + 0x50) = g_unique;
    *reinterpret_cast<void***>(g_container + 0xE0) = g_gt;
    *reinterpret_cast<double**>(g_container + 0x170) = &g_bounds[0][0];
    *reinterpret_cast<uint8_t**>(g_container + 0x188) = g_hasBounds;
    memset(g_ops, 0, sizeof g_ops);
    // op 0: add (bucket 0), op 1: update (inner index 2), op 2: update in a dynamic-tree bucket (1: skipped), op 3: delete
    const uint16_t index[4] = {0x0000, 0x0010, 0x0008, 0x0000};
    const uint8_t operation[4] = {1, 2, 1, 0};
    for (int i = 0; i < 4; ++i) {
        g_particles[i] = {g_container, i, 0};
        uint8_t* op = g_ops + i * 0x48;
        *reinterpret_cast<void**>(op + 0x08) = &g_particles[i];
        *reinterpret_cast<uint16_t*>(op + 0x38) = index[i];
        op[0x40] = operation[i];
    }
    *reinterpret_cast<uint8_t**>(g_evolution + 0x380) = g_ops;
    *reinterpret_cast<int32_t*>(g_evolution + 0x388) = 4;
    *reinterpret_cast<void**>(g_evolution + 0x2E0) = &g_internal;
    *reinterpret_cast<void**>(g_evolution + 0x2E8) = &g_external;
    *reinterpret_cast<int32_t*>(g_evolution + 0x358) = 77;
}
}  // namespace pfl

// ---- fakes for the instance clear test (instance_clear.cpp, InstanceClearReserve) ----
namespace icr {
alignas(16) uint8_t g_component[0x900];
alignas(16) uint8_t g_render[0x100];
alignas(16) uint8_t g_buffer[0x80];
int g_resets = 0, g_resizes = 0;
int32_t g_prev = -1, g_newMax = -1;
size_t g_elem = 0;
uint32_t g_align = 0;
void FakeReset(uint8_t* cmd)
{
    ++g_resets;
    *reinterpret_cast<int32_t*>(cmd + 8) = 0;
    *reinterpret_cast<int32_t*>(cmd + 0xC) = 0;
}
void FakeResize(uint8_t*, int32_t prev, int32_t newMax, size_t elem, uint32_t align)
{
    ++g_resizes;
    g_prev = prev; g_newMax = newMax; g_elem = elem; g_align = align;
}
void Setup(int32_t rendered, int32_t instances, bool render)
{
    memset(g_component, 0, sizeof g_component);
    g_resets = g_resizes = 0;
    g_prev = g_newMax = -1;
    *reinterpret_cast<uint8_t**>(g_component + 0x6D0) = render ? g_render : nullptr;
    *reinterpret_cast<uint8_t**>(g_render + 0xC0) = g_buffer;
    *reinterpret_cast<int32_t*>(g_buffer + 0x40) = rendered;
    *reinterpret_cast<int32_t*>(g_component + 0x668) = instances;
    *reinterpret_cast<int32_t*>(g_component + 0x758 + 8) = 5;     // old commands (Reset clears them)
    *reinterpret_cast<int32_t*>(g_component + 0x758 + 0xC) = 8;
}
}  // namespace icr

// ---- fakes for the shared BLAS scratch test (blas_scratch.cpp, lab switch BlasSharedScratch) ----

// ---- fakes for the view rename guard test (view_rename_guard.cpp, ViewRenameGuard) ----
namespace vrg {
alignas(16) uint8_t g_base[0x100];  // FD3D12BaseShaderResource: RenameListenersCS at +0x10, ResourceLocation at +0x48
alignas(16) uint8_t g_view[0x100];  // FD3D12View: vtable, ResourceInfo at +0x10 (Base, Location, Resource, Residency);
                                    // a UAV's CounterResource at +0x88
uint8_t* const g_location = g_base + 0x48;  // UnderlyingResource at +0x10
alignas(16) uint8_t g_resA[16], g_resB[16];
int g_creates = 0, g_renamed = 0, g_moves = 0;
bool g_lockHeld = false, g_lockHeldInRenamed = false;
using CreateFn = void (*)(uint8_t*, uint8_t* const*, uintptr_t);
CreateFn g_entry = nullptr;  // the game's (patched) FD3D12View::CreateView
CRITICAL_SECTION* Lock() { return reinterpret_cast<CRITICAL_SECTION*>(g_base + 0x10); }
bool Held()
{
    CRITICAL_SECTION* cs = Lock();
    return cs->RecursionCount > 0 && cs->OwningThread == reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId()));
}
uint8_t*& Field(uint8_t* p, size_t offset) { return *reinterpret_cast<uint8_t**>(p + offset); }
void FakeCreateView(uint8_t* view, uint8_t* const* info, uintptr_t)
{
    ++g_creates;
    g_lockHeld = Held();
    memcpy(view + 0x10, info, 32);  // ResourceInfo = InResource
}
void FakeRenamed(uint8_t* view, uint8_t* base, uint8_t* location)  // like FD3D12*View_RHI::ResourceRenamed
{
    ++g_renamed;
    g_lockHeldInRenamed = Held();
    uint8_t* const info[4] = {base, location, Field(location, 0x10), nullptr};
    g_entry(view, info, 0);  // CreateView again, through the patched entry
}
bool FakeMoved(uint8_t* location, void*)
{
    ++g_moves;
    Field(location, 0x10) = g_resB;
    return true;
}
void* g_vtable[3] = {reinterpret_cast<void*>(&FakeRenamed), nullptr, nullptr};
void* g_uavVtable[3] = {reinterpret_cast<void*>(&FakeRenamed), nullptr, nullptr};  // slot 2 = the game's UAV UpdateDescriptor
}  // namespace vrg

// FrameGenCrashGuard's reproduction: the library's own function with the guard off, all 8 lists on a held queue (SEH: no C++ objects).
static bool FrameGenLibraryFaults(void* pool, void* queue, const wchar_t* name, uintptr_t* at, uintptr_t* address)
{
    __try {
        FrameGenCrashGuardTestAcquire(pool, queue, name);
    } __except (*at = reinterpret_cast<uintptr_t>(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
                *address = GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1],
                GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}


// ---- fakes for the frame generation camera test (framegen_camera.cpp, FrameGenCameraInfo) ----
// The helper threads' allocator caches: a fake set-up that stores a marker in a real TLS slot.
namespace htc {
uint32_t g_slot = 0;
std::atomic<int> g_setups{0};
int g_marker = 0;
void FakeSetup()
{
    g_setups.fetch_add(1);
    TlsSetValue(g_slot, &g_marker);
}
}  // namespace htc

// PipelinedSceneAdd's fakes: a scene (solver at +0x38, query structure at +0x28), a solver (lock at +0x200), the game's
// recursive SRW lock (count in a TLS slot at +8), AddActorToSolver and AddActorsToScene that record who did what.
namespace pst {
struct FakeLock { SRWLOCK srw; uint32_t tls; };
struct Proxy { std::atomic<int> reg{-1}; DWORD regTid = 0; int ins = -1; DWORD insTid = 0; };
constexpr int kMax = 1000;
Proxy g_proxies[kMax];
uint8_t* g_handlePtrs[kMax];
alignas(16) uint8_t g_scene[0x100];
alignas(16) uint8_t g_solver[0x400];
FakeLock g_lock;
volatile uint8_t g_flag = 1;
std::atomic<int> g_reg{0}, g_ins{0}, g_bad{0}, g_inJob{0}, g_probeHits{0};
std::atomic<bool> g_probeRun{false};
using StubFn = void (*)(uint8_t*, uint8_t*);
StubFn g_stub = nullptr;
struct Arr { uint8_t* data; int32_t num; int32_t max; };
void Lock()
{
    const uint64_t v = reinterpret_cast<uint64_t>(TlsGetValue(g_lock.tls));
    const uint32_t w = static_cast<uint32_t>(v >> 32) + 1;
    TlsSetValue(g_lock.tls, reinterpret_cast<void*>((static_cast<uint64_t>(w) << 32) | (v & 0xFFFFFFFFull)));
    if (w + static_cast<uint32_t>(v) == 1) AcquireSRWLockExclusive(&g_lock.srw);
}
void Unlock()
{
    const uint64_t v = reinterpret_cast<uint64_t>(TlsGetValue(g_lock.tls));
    const uint32_t w = static_cast<uint32_t>(v >> 32) - 1;
    TlsSetValue(g_lock.tls, reinterpret_cast<void*>((static_cast<uint64_t>(w) << 32) | (v & 0xFFFFFFFFull)));
    if (w + static_cast<uint32_t>(v) == 0) ReleaseSRWLockExclusive(&g_lock.srw);
}
void FakeToSolver(uint8_t* solver, uint8_t* proxy)
{
    if (solver != g_solver) g_bad.fetch_add(1);
    Lock();
    Proxy* const p = reinterpret_cast<Proxy*>(proxy);
    p->regTid = GetCurrentThreadId();
    p->reg.store(g_reg.fetch_add(1));
    Unlock();
}
void FakeAdd(uint8_t* scene, Arr* handles, uint8_t immediate)
{
    if (g_flag) Lock();
    uint8_t* const solver = *reinterpret_cast<uint8_t**>(scene + 0x38);
    g_inJob.store(1);
    for (int i = 0; i < handles->num; ++i) {
        uint8_t* const h = reinterpret_cast<uint8_t**>(handles->data)[i];
        g_stub(solver, h);
        if (immediate) {
            Proxy* const p = reinterpret_cast<Proxy*>(h);
            if (p->reg.load() < 0) g_bad.fetch_add(1);  // inserted before its registration
            if ((i & 7) == 0) SwitchToThread();
            p->insTid = GetCurrentThreadId();
            p->ins = g_ins.fetch_add(1);
        }
    }
    g_inJob.store(0);
    if (g_flag) Unlock();
}
// Runs the hook on n bodies; true if every body was registered on the calling thread and inserted in array order after
// its registration, by the helper when expectHelper, and the lock is free and the caller's count back to zero after.
bool Job(int n, bool expectHelper)
{
    for (int i = 0; i < n; ++i) {
        g_proxies[i].reg.store(-1); g_proxies[i].ins = -1; g_proxies[i].regTid = g_proxies[i].insTid = 0;
        g_handlePtrs[i] = reinterpret_cast<uint8_t*>(&g_proxies[i]);
    }
    g_reg = 0; g_ins = 0; g_bad = 0;
    Arr a{reinterpret_cast<uint8_t*>(g_handlePtrs), n, n};
    reinterpret_cast<void (*)(uint8_t*, Arr*, uint8_t)>(opf::PipelinedSceneAddTestHook())(g_scene, &a, 1);
    const DWORD me = GetCurrentThreadId();
    bool ok = g_bad.load() == 0 && g_reg.load() == n && g_ins.load() == n;
    for (int i = 0; i < n && ok; ++i)
        ok = g_proxies[i].reg.load() == i && g_proxies[i].ins == i && g_proxies[i].regTid == me &&
             (expectHelper ? g_proxies[i].insTid != me : g_proxies[i].insTid == me);
    ok = ok && TlsGetValue(g_lock.tls) == nullptr;
    // The probe thread may still hold its shared try-lock for an instant after the job's unlock (it saw g_inJob just
    // before the job cleared it): give it a moment instead of reading that as a lock left held.
    bool lockFree = false;
    for (int t = 0; t < 1000 && !lockFree; ++t) {
        if (TryAcquireSRWLockExclusive(&g_lock.srw)) { ReleaseSRWLockExclusive(&g_lock.srw); lockFree = true; }
        else SwitchToThread();
    }
    return ok && lockFree;
}
}  // namespace pst

// ParallelStaticBodies's fakes: bodies (actor handle at the fix's offset), transforms (0x60 bytes, the body index in the first 8), a
// helper (array pointers at +0/+8, the static flag) and a CreateShapesAndActors that records who did which body.
namespace sbt {
struct Arr { uint8_t* data; int32_t num; int32_t max; };
constexpr int kMax = 3000;
alignas(16) uint8_t g_bodies[kMax][0x200];
uint8_t* g_bodyPtrs[kMax];
alignas(16) uint8_t g_transforms[kMax][0x60];
uint32_t g_handleOff = 0x158, g_staticOff = 0x31;
std::atomic<int> g_order{0}, g_sbCalls{0}, g_bad{0};
int g_seq[kMax];
DWORD g_tid[kMax];
int g_maxView = 0;
int g_work = 20000;
std::atomic<int> g_inFake{0};
bool FakeCreate(void* helper)
{
    g_inFake.fetch_add(1);
    auto* h = static_cast<uint8_t*>(helper);
    Arr* b = reinterpret_cast<Arr**>(h)[0];
    Arr* t = reinterpret_cast<Arr**>(h)[1];
    g_sbCalls.fetch_add(1);
    if (b->num > g_maxView) g_maxView = b->num;
    if (b->num != t->num || h[g_staticOff] == 0) g_bad.fetch_add(1);
    for (int i = b->num - 1; i >= 0; --i) {
        uint8_t* body = reinterpret_cast<uint8_t**>(b->data)[i];
        const int idx = static_cast<int>((body - &g_bodies[0][0]) / 0x200);
        int64_t tIdx;
        memcpy(&tIdx, t->data + static_cast<size_t>(i) * 0x60, 8);
        if (tIdx != idx || *reinterpret_cast<void**>(body + g_handleOff)) g_bad.fetch_add(1);
        *reinterpret_cast<void**>(body + g_handleOff) = body;
        g_seq[idx] = g_order.fetch_add(1);
        g_tid[idx] = GetCurrentThreadId();
        const uint64_t t0 = GetTickCount64();
        for (volatile int k = 0; k < g_work; ++k) {}  // some work, so the helper threads get slices too
        (void)t0;
    }
    g_inFake.fetch_sub(1);
    return true;
}
alignas(16) uint8_t g_helper[0x80];
Arr g_b, g_t;
// n bodies, all without an actor; returns the number of CreateShapesAndActors calls the hook made.
int Run(int n, bool isStatic = true, int withActor = -1, int transforms = -1)
{
    for (int i = 0; i < n; ++i) {
        memset(g_bodies[i], 0, 0x200);
        g_bodyPtrs[i] = g_bodies[i];
        int64_t v = i;
        memcpy(g_transforms[i], &v, 8);
        g_seq[i] = -1;
        g_tid[i] = 0;
    }
    if (withActor >= 0) *reinterpret_cast<void**>(g_bodies[withActor] + g_handleOff) = reinterpret_cast<void*>(1);
    g_b = Arr{reinterpret_cast<uint8_t*>(g_bodyPtrs), n, n};
    g_t = Arr{&g_transforms[0][0], transforms >= 0 ? transforms : n, n};
    memset(g_helper, 0, sizeof g_helper);
    reinterpret_cast<Arr**>(g_helper)[0] = &g_b;
    reinterpret_cast<Arr**>(g_helper)[1] = &g_t;
    g_helper[g_staticOff] = isStatic ? 1 : 0;
    g_order = 0;
    g_sbCalls = 0;
    g_bad = 0;
    g_maxView = 0;
    return 0;
}
}  // namespace sbt
#include "selftest_pipebodies.h"  // PipelinedBodyCreation's sites and InitStaticBodies-flow model (uses pst's fakes)

namespace fgc {
// A view laid out as UE 5.3's FSceneView at the offsets the fix derived: ViewMatrix / InvViewMatrix / ViewOrigin in
// ViewMatrices, ViewLocation, WorldToMetersScale. The camera: yaw 30, pitch 10, roll 0 (UE axes: X forward, Y right, Z up).
alignas(16) uint8_t g_view[0x1000];
alignas(16) uint8_t g_desc[0xC0];  // ffxDispatchDescFrameGenerationPrepare (0xB8 bytes in 3.1.0)
double g_right[3], g_up[3], g_forward[3];
const double g_location[3] = {123456.5, -98765.25, 4321.75};
int g_dispatchCalls = 0;
const void* g_dispatchDesc = nullptr;

void Put(uint32_t offset, const double* v, int n) { memcpy(g_view + offset, v, sizeof(double) * n); }

void BuildView(const FrameGenCameraInfoOut& o)
{
    memset(g_view, 0, sizeof g_view);
    const double y = 30 * 3.14159265358979323846 / 180, p = 10 * 3.14159265358979323846 / 180;
    const double f[3] = {cos(p) * cos(y), cos(p) * sin(y), sin(p)};
    const double r[3] = {-sin(y), cos(y), 0};
    const double u[3] = {-sin(p) * cos(y), -sin(p) * sin(y), cos(p)};
    memcpy(g_forward, f, sizeof f);
    memcpy(g_right, r, sizeof r);
    memcpy(g_up, u, sizeof u);
    // ViewMatrix (world -> view, row vectors): columns right, up, forward; last row the translated origin.
    double v[16] = {};
    for (int row = 0; row < 3; ++row) {
        v[row * 4 + 0] = r[row];
        v[row * 4 + 1] = u[row];
        v[row * 4 + 2] = f[row];
    }
    for (int c = 0; c < 3; ++c) {
        const double* axis = c == 0 ? r : c == 1 ? u : f;
        v[12 + c] = -(g_location[0] * axis[0] + g_location[1] * axis[1] + g_location[2] * axis[2]);
    }
    v[15] = 1;
    // InvViewMatrix: rows right, up, forward, then the position.
    double inv[16] = {r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, g_location[0], g_location[1], g_location[2], 1};
    Put(o.matrices + 3 * 0x80, v, 16);
    Put(o.matrices + 4 * 0x80, inv, 16);
    Put(o.matrices + 15 * 0x80 + 0x18, g_location, 3);
    Put(o.location, g_location, 3);
    const float scale = 100.f;
    memcpy(g_view + o.scale, &scale, sizeof scale);
}

void ResetDesc()
{
    memset(g_desc, 0, sizeof g_desc);
    const uint64_t type = 0x00020004;
    memcpy(g_desc, &type, sizeof type);
}

struct Header { uint64_t type; const Header* pNext; };
struct Camera { Header header; float position[3], up[3], right[3], forward[3]; };

const Camera* Linked() { return reinterpret_cast<const Header*>(g_desc)->pNext ? reinterpret_cast<const Camera*>(reinterpret_cast<const Header*>(g_desc)->pNext) : nullptr; }

bool Same(const float* a, const double* b, double tol)
{
    for (int i = 0; i < 3; ++i)
        if (std::fabs(a[i] - b[i]) > tol) return false;
    return true;
}

int32_t FakeDispatch(void*, const void* desc)
{
    ++g_dispatchCalls;
    g_dispatchDesc = desc;
    return 0;
}

// Calls the stub as the game's code does: rbp = the frame (desc at [rbp+descDisp]), r10 = the view, rax = 0; returns
// rax ^ r10 afterwards (the view, if the stub kept both).
//   push rbp; mov rbp,rcx; mov r10,rdx; mov r11,r8; xor eax,eax; call r11; xor rax,r10; pop rbp; ret
const uint8_t kCaller[] = {0x55, 0x48, 0x89, 0xCD, 0x49, 0x89, 0xD2, 0x4D, 0x89, 0xC3, 0x31, 0xC0,
                           0x41, 0xFF, 0xD3, 0x4C, 0x31, 0xD0, 0x5D, 0xC3};
using CallerFn = uint64_t (*)(void* rbp, const void* view, void* stub);
}  // namespace fgc

// CellEvictSplit (fix102, rev102b M1): fake references, cell, ProcessLists and lock for CellEvictSplitStep's decision.
// A reference: vtable +0 (vt+0x310 = its base form, vt+0x2D0 = Set3D), 3D at +0x78, our base form pointer at +0x80. Set3D
// records the call and clears +0x78 (unless keep3d); it can raise the arrow count after the n-th call. The base form
// call can add a MagicHitEffect targeting another reference (a hit effect created between the walk and the call). The
// fake lock counts its depth: every base-form and Set3D call must run under it (rev102b S1).
namespace ces {
struct Node {
    void* item;
    Node* next;
};
alignas(16) uint8_t refs[12][0x100];
alignas(16) uint8_t cell[0x200];
Node nodes[16];
uint8_t npcBase[16], creaBase[16], statBase[16];
void* vt[0x320 / 8];
void* player = nullptr;
volatile int32_t arrows = 0;
int depth = 0, locks = 0, unlocked = 0, unlockedCalls = 0;
std::vector<int> calls;
bool keep3d = false;
int arrowAfter = -1;               // Set3D call count after which the arrow count becomes 1
int hitOnBaseOf = -1, hitTarget = -1;  // the base-form call of ref hitOnBaseOf adds a hit effect on ref hitTarget
// ProcessLists +0x90: {item, next}; a hit effect: vt (vt[1] = its RTTI), target at +0x30
alignas(16) uint8_t pl[0x100];
uint8_t* plPtr = pl;
alignas(16) uint8_t effects[4][0x40];
Node effNodes[4];
int nEff = 0;
alignas(16) uint8_t rttiHit[16], rttiChild[16];
void* effVt[2];
int Index(const void* ref)
{
    for (int i = 0; i < 12; ++i)
        if (ref == refs[i]) return i;
    return -1;
}
void AddHit(int target)
{
    uint8_t* const e = effects[nEff];
    memset(e, 0, sizeof effects[0]);
    *reinterpret_cast<void***>(e) = effVt;
    *reinterpret_cast<void**>(e + 0x30) = refs[target];
    // the list's head node is embedded at pl+0x90 (as the game's); later items are chained from it
    Node* const head = reinterpret_cast<Node*>(pl + 0x90);
    if (!nEff) {
        *head = {e, nullptr};
    } else {
        effNodes[nEff] = {e, nullptr};
        Node* n = head;
        while (n->next) n = n->next;
        n->next = &effNodes[nEff];
    }
    ++nEff;
}
const uint8_t* Rtti(const void*) { return rttiChild; }
const uint8_t* BaseOf(const void* ref)
{
    if (depth <= 0) ++unlocked;
    if (Index(ref) == hitOnBaseOf && hitTarget >= 0) {
        AddHit(hitTarget);
        hitOnBaseOf = -1;
    }
    return *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(ref) + 0x80);
}
void Set3D(void* ref, void*)
{
    if (depth <= 0) ++unlockedCalls;
    calls.push_back(Index(ref));
    if (!keep3d) *reinterpret_cast<void**>(static_cast<uint8_t*>(ref) + 0x78) = nullptr;
    if (arrowAfter >= 0 && static_cast<int>(calls.size()) >= arrowAfter) arrows = 1;
}
void Lock(void*, void*) { ++depth, ++locks; }
void Unlock(void*, void*) { --depth; }
// kinds: 'n' NPC_, 'c' CREA, 's' static, 'p' the player (an NPC_), 'x' an actor without 3D
void Build(const char* kinds)
{
    memset(cell, 0, sizeof cell);
    memset(pl, 0, sizeof pl);
    nEff = 0;
    calls.clear();
    arrows = 0, depth = 0, locks = 0, unlocked = 0, unlockedCalls = 0;
    keep3d = false, arrowAfter = -1, hitOnBaseOf = -1, hitTarget = -1;
    npcBase[8] = 0x23, creaBase[8] = 0x24, statBase[8] = 0x1F;
    vt[0x310 / 8] = reinterpret_cast<void*>(&BaseOf);
    vt[0x2D0 / 8] = reinterpret_cast<void*>(&Set3D);
    effVt[1] = reinterpret_cast<void*>(&Rtti);
    *reinterpret_cast<const uint8_t**>(rttiChild + 8) = rttiHit;
    *reinterpret_cast<const uint8_t**>(rttiHit + 8) = nullptr;
    player = nullptr;
    const int n = static_cast<int>(strlen(kinds));
    for (int i = 0; i < n; ++i) {
        uint8_t* const ref = refs[i];
        memset(ref, 0, sizeof refs[0]);
        *reinterpret_cast<void***>(ref) = vt;
        *reinterpret_cast<void**>(ref + 0x78) = kinds[i] == 'x' ? nullptr : ref;  // any non-null "3D"
        *reinterpret_cast<const uint8_t**>(ref + 0x80) = kinds[i] == 'c' ? creaBase : kinds[i] == 's' ? statBase : npcBase;
        if (kinds[i] == 'p') player = ref;
        nodes[i].item = ref;
        nodes[i].next = i + 1 < n ? &nodes[i + 1] : nullptr;
    }
    if (n) *reinterpret_cast<Node*>(cell + 0xB8) = {nodes[0].item, nodes[0].next};
}
}  // namespace ces

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);  // every line reaches a pipe or file, even if a check crashes
    const wchar_t* exe = argc > 1 ? argv[1]
        : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Oblivion Remastered\\OblivionRemastered\\Binaries\\Win64\\OblivionRemastered-Win64-Shipping.exe";
    HMODULE module = LoadLibraryExW(exe, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) {
        printf("cannot map %ls (error %lu)\n", exe, GetLastError());
        return 2;
    }
    // [General] LogFile: 1 (default) = the log is written as before; 0 = nothing is created, renamed or written at start-up
    // and the log appears only with the first [gpucrash] line. Run in its own folder before selftest.log is opened.
    {
        CreateDirectoryW(L"logfile", nullptr);
        auto put = [](const wchar_t* path, const char* text) {
            if (FILE* f = _wfsopen(path, L"wb", _SH_DENYNO)) { fputs(text, f); fclose(f); }
        };
        auto read = [](const wchar_t* path) {
            std::string s;
            if (FILE* f = _wfsopen(path, L"rb", _SH_DENYNO)) {
                char buf[512];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
                fclose(f);
            }
            return s;
        };
        const wchar_t* ini = L"logfile\\t.ini";
        put(ini, "[General]\r\nLogFile=0\r\n");
        // DrawBehindLoadingScreen (loadreveal; public 1.3.0): 3 by default and when missing, read when set; its bookkeeping
        Check(Config().drawBehindLoading == 3 && LoadConfig(ini).drawBehindLoading == 3, "DrawBehindLoadingScreen: 3 by default");
        put(ini, "[Fixes]\r\nDrawBehindLoadingScreen=0\r\n");
        Check(LoadConfig(ini).drawBehindLoading == 0, "DrawBehindLoadingScreen=0 is read");
        Check(Config().navQueryPriority == 1 && LoadConfig(ini).navQueryPriority == 1, "NavQueryPriority: 1 by default and when missing (1.6.0)");
        put(ini, "[Fixes]\r\nNavQueryPriority=0\r\n");
        Check(LoadConfig(ini).navQueryPriority == 0, "NavQueryPriority=0 is read");
        Check(Config().physicsSubstepCap == 1 && LoadConfig(ini).physicsSubstepCap == 1, "PhysicsSubstepCap: 1 by default and when missing (1.6.0)");
        put(ini, "[Fixes]\r\nPhysicsSubstepCap=0\r\n");
        Check(LoadConfig(ini).physicsSubstepCap == 0, "PhysicsSubstepCap=0 is read");
        put(ini, "[Fixes]\r\nDrawBehindLoadingScreen=0\r\n");
        {
            char why[160] = "";
            const bool ok = DrawBehindLoadingSelfTest(why, sizeof why);
            Check(ok, ok ? "DrawBehindLoadingScreen: start, frames, undo, game changes, fade-to-game, abort, off, D-S1/S2/S3" : why);
        }
        put(ini, "[General]\r\nLogFile=0\r\n");
        Check(!LoadConfig(ini).logFile, "LogFile=0 is read");
        put(ini, "[General]\r\nLogFile=1\r\n");
        Check(LoadConfig(ini).logFile, "LogFile=1 is read");
        put(ini, "[General]\r\nStatsIntervalSeconds=60\r\n");
        Check(LoadConfig(ini).logFile, "LogFile: a missing key means on");
        Check(Config().logFile, "LogFile: on by default");
        const wchar_t* l0 = L"logfile\\OblivionPerfFix.log";
        const wchar_t* l1 = L"logfile\\OblivionPerfFix.1.log";
        const wchar_t* l2 = L"logfile\\OblivionPerfFix.2.log";
        // off: earlier logs untouched, other lines dropped, the first crash report creates the log (rotating as usual)
        put(l0, "OLD0"); put(l1, "OLD1"); put(l2, "OLD2");
        LogOpen(l0, 2, false);
        Check(!LogEnabled(), "LogFile=0: LogEnabled is false");
        Log("loaded as test");
        Log("[mem] a statistics line");
        Check(read(l0) == "OLD0" && read(l1) == "OLD1" && read(l2) == "OLD2",
              "LogFile=0: start-up and ordinary lines touch no file (earlier logs stay where they are)");
        Log("[gpucrash] device removed reason 0x887A0006");
        Log("[mem] after the crash report");
        Log("[gpucrash] DRED command lists: 1 stopped");
        const std::string crash = read(l0);
        Check(crash.find("device removed reason") != std::string::npos && crash.find("DRED command lists") != std::string::npos &&
                  crash.find("LogFile=0") != std::string::npos,
              "LogFile=0: a GPU crash report creates the log with the report and a one-line note");
        Check(crash.find("loaded as test") == std::string::npos && crash.find("statistics line") == std::string::npos &&
                  crash.find("after the crash report") == std::string::npos,
              "LogFile=0: only the crash report is in that log");
        Check(read(l1) == "OLD0" && read(l2) == "OLD1", "LogFile=0: the crash log rotates the earlier logs like a normal start");
        LogClose();
        // on: as before
        put(l0, "OLD0"); put(l1, "OLD1"); put(l2, "OLD2");
        LogOpen(l0, 2, true);
        Check(LogEnabled(), "LogFile=1: LogEnabled is true");
        Log("loaded as test");
        Log("[mem] a statistics line");
        const std::string normal = read(l0);
        Check(normal.find("loaded as test") != std::string::npos && normal.find("statistics line") != std::string::npos &&
                  read(l1) == "OLD0" && read(l2) == "OLD1",
              "LogFile=1: the log is written and the earlier ones rotate (unchanged behaviour)");
        LogClose();
        // The log's writer thread (ourbugs1107): the same bytes, order and time stamps as the caller-written log; the
        // queue is written at a crash, at TerminateProcess and at ExitProcess (also the rest of a cut write); a full queue
        // drops lines (counted, one note) instead of making the caller wait.
        {
            auto has = [](const std::string& text, const char* what) { return text.find(what) != std::string::npos; };
            auto count = [](const std::string& text, const char* what) {
                int n = 0;
                for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
                return n;
            };
            auto lines = [](const std::string& text) {
                std::vector<std::string> v;
                for (size_t a = 0; a < text.size();) {
                    const size_t e = text.find("\r\n", a);
                    if (e == std::string::npos) { v.push_back(text.substr(a)); break; }
                    v.push_back(text.substr(a, e - a));
                    a = e + 2;
                }
                return v;
            };
            auto stampMs = [](const std::string& l) -> long long {
                int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
                if (sscanf_s(l.c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi, &s, &ms) != 7) return -1;
                return ((static_cast<long long>(d) * 24 + h) * 60 + mi) * 60000LL + s * 1000LL + ms;
            };
            auto lineOf = [](const std::vector<std::string>& v, const char* what) {
                for (size_t i = 0; i < v.size(); ++i)
                    if (v[i].find(what) != std::string::npos) return static_cast<long long>(i);
                return -1LL;
            };
            put(l0, "");
            LogOpen(l0, 0, true);
            LogGoAsync(nullptr);
            Check(LogTestAsync(), "log writer: after LogGoAsync the lines go through the background thread");
            Log("first line");
            Log("two\nparts");
            LogFlush();
            std::string text = read(l0);
            Check(has(text, "[log] lines are written by a background thread") && has(text, " first line\r\n") &&
                      has(text, " two\r\nparts\r\n") && !has(text, "\n\n") && count(text, "\r\n") == count(text, "\n"),
                  "log writer: the same bytes as the C runtime's text mode (CRLF, an inner \\n as \\r\\n)");

            // order and time stamps: lines queued while the writer is held keep their callers' times and order
            LogTestHoldWriter(true);
            Log("held A");
            Sleep(60);
            Log("held B");
            const bool queuedOnly = !has(read(l0), "held A");
            constexpr int kThreads = 4, kPer = 2000;
            std::vector<std::thread> threads;
            for (int th = 0; th < kThreads; ++th)
                threads.emplace_back([th] { for (int i = 0; i < kPer; ++i) Log("mt %d %d", th, i); });
            for (std::thread& t : threads) t.join();
            LogTestHoldWriter(false);
            LogFlush();
            text = read(l0);
            std::vector<std::string> v = lines(text);
            const long long a = lineOf(v, "held A"), b = lineOf(v, "held B");
            Check(queuedOnly, "log writer: a held writer leaves the lines in the queue (the caller does not write them)");
            Check(a >= 0 && b > a && stampMs(v[static_cast<size_t>(b)]) - stampMs(v[static_cast<size_t>(a)]) >= 50,
                  "log writer: time stamps are taken at the call, not at the write");
            {
                int next[kThreads] = {}, bad = 0;
                long long lastMs[kThreads] = {};
                for (const std::string& l : v) {
                    int th = -1, i = -1;
                    const size_t at = l.find(" mt ");
                    if (at == std::string::npos || sscanf_s(l.c_str() + at, " mt %d %d", &th, &i) != 2 || th < 0 || th >= kThreads) continue;
                    const long long ms = stampMs(l);
                    if (i != next[th] || ms < lastMs[th]) ++bad;
                    next[th] = i + 1;
                    lastMs[th] = ms;
                }
                bool all = bad == 0;
                for (int th = 0; th < kThreads; ++th) all = all && next[th] == kPer;
                Check(all && !has(text, "queue full"),
                      "log writer: 4 threads x 2000 lines: every line once, each thread's in its order with rising time stamps, none dropped");
            }

            // a crash: LogCrashMode writes the queue at once, later lines are written by their callers
            LogTestHoldWriter(true);
            Log("before the crash 1");
            Log("before the crash 2");
            const bool waited = !has(read(l0), "before the crash 1");
            LogCrashMode();
            text = read(l0);
            const bool flushed = has(text, "before the crash 1") && has(text, "before the crash 2") &&
                                 text.find("before the crash 1") < text.find("before the crash 2");
            Log("after the crash");
            Check(waited && flushed && has(read(l0), "after the crash") && !LogTestAsync(),
                  "log writer: at a crash every queued line is in the file at once, later lines too");
            LogClose();

            // a "[crash]" line (the crash handlers' own) writes the queue before it, also without LogCrashMode
            LogOpen(l0, 0, true);
            LogGoAsync(nullptr);
            LogFlush();
            LogTestHoldWriter(true);
            Log("queued line");
            Log("[crash] UE is reporting a crash (test)");
            text = read(l0);
            Check(has(text, "queued line") && has(text, "[crash] UE is reporting") && text.find("queued line") < text.find("[crash]"),
                  "log writer: a [crash] line is written at once, with every line before it");
            LogClose();

            // the game's TerminateProcess (the guard's body)
            LogOpen(l0, 0, true);
            LogGoAsync(nullptr);
            LogFlush();
            LogTestHoldWriter(true);
            Log("before TerminateProcess");
            const bool queuedT = !has(read(l0), "before TerminateProcess");
            LogBeforeExit();
            Check(queuedT && has(read(l0), "before TerminateProcess"), "log writer: TerminateProcess writes the queue first");
            LogClose();

            // ExitProcess ended the writer in the middle of a write: DLL_PROCESS_DETACH writes the rest of it, then the queue
            LogOpen(l0, 0, true);
            LogGoAsync(nullptr);
            LogFlush();
            LogTestHoldWriter(true);
            Log("cut line one");
            Log("cut line two");
            Log("cut line three");
            const bool cut = LogTestEndedMidWrite(30);  // 30 bytes: into the first line
            Log("queued after the cut");
            LogProcessExit(true);
            text = read(l0);
            v = lines(text);
            const long long c1 = lineOf(v, "cut line one"), c3 = lineOf(v, "cut line three"), q = lineOf(v, "queued after the cut");
            Check(cut && count(text, "cut line one") == 1 && count(text, "cut line two") == 1 && count(text, "cut line three") == 1 &&
                      count(text, "queued after the cut") == 1 && c1 >= 0 && c3 > c1 && q > c3 && stampMs(v[static_cast<size_t>(c1)]) >= 0,
                  "log writer: at ExitProcess the cut write is completed (no line lost or doubled), then the queue");
            LogClose();

            // a full queue: the caller does not wait; the lines are dropped, counted and noted before the next line
            LogOpen(l0, 0, true);
            LogTestSetCapacity(4096);
            LogGoAsync(nullptr);
            LogFlush();
            LogTestHoldWriter(true);
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            double worstUs = 0;
            for (int i = 0; i < 200; ++i) {
                QueryPerformanceCounter(&t0);
                Log("fill %03d ..........................................................................", i);
                QueryPerformanceCounter(&t1);
                worstUs = std::max(worstUs, (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart);
            }
            LogTestHoldWriter(false);
            LogFlush();
            Log("after the drops");
            LogFlush();
            text = read(l0);
            v = lines(text);
            int kept = 0, firstMissing = -1;
            for (int i = 0; i < 200; ++i) {
                char key[16];
                snprintf(key, sizeof key, "fill %03d ", i);
                if (has(text, key)) {
                    ++kept;
                    if (firstMissing >= 0) firstMissing = 1000;  // a gap: a later line kept after a dropped one
                } else if (firstMissing < 0) firstMissing = i;
            }
            unsigned long long noted = 0;
            const long long note = lineOf(v, "[log] queue full: "), after = lineOf(v, "after the drops");
            if (note >= 0) sscanf_s(v[static_cast<size_t>(note)].c_str() + v[static_cast<size_t>(note)].find("full: ") + 6, "%llu", &noted);
            Check(kept > 10 && kept < 200 && firstMissing == kept && noted == static_cast<unsigned long long>(200 - kept) &&
                      note >= 0 && after == note + 1 && worstUs < 5000,
                  "log writer: a full queue drops the newest lines without waiting (< 5 ms per call), counts them in one note before the next line");
            LogClose();
        }

        // The log can't be opened: OblivionPerfFix.<pid>.log next to it, then the same name in %TEMP%, the reasons at the
        // top. A handle without FILE_SHARE_WRITE stands in for a leftover game process holding the file.
        {
            const std::wstring pid = std::to_wstring(GetCurrentProcessId());
            const std::wstring lp = L"logfile\\OblivionPerfFix." + pid + L".log";
            CreateDirectoryW(L"logfile\\tmp", nullptr);
            wchar_t tmpDir[MAX_PATH], oldTmp[MAX_PATH] = L"";
            GetFullPathNameW(L"logfile\\tmp\\", MAX_PATH, tmpDir, nullptr);
            const DWORD hadTmp = GetEnvironmentVariableW(L"TMP", oldTmp, MAX_PATH);
            SetEnvironmentVariableW(L"TMP", tmpDir);  // nothing of this test lands in the real %TEMP%
            const std::wstring lt = std::wstring(tmpDir) + L"OblivionPerfFix." + pid + L".log";
            auto hold = [](const wchar_t* path) {
                return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            };
            auto exists = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
            auto has = [](const std::string& text, const char* what) { return text.find(what) != std::string::npos; };

            // step 1: another process holds the log
            put(l0, "HELD");
            HANDLE h0 = hold(l0);
            LogOpen(l0, 2, true);
            Log("loaded as test");
            LogClose();
            std::string text = read(lp.c_str());
            Check(has(text, "loaded as test"), "log fallback: a held log: the session's lines go to OblivionPerfFix.<pid>.log");
            Check(has(text, "could not open logfile\\OblivionPerfFix.log (Windows error 32") && has(text, "did not fully close") &&
                      has(text, "Only the log is affected") && text.find("could not open") < text.find("loaded as test"),
                  "log fallback: its top says which file failed, why, and that the fixes are not affected");
            Check(read(l0) == "HELD" && !exists(lt), "log fallback: the held log is untouched, %TEMP% not used");
            DeleteFileW(lp.c_str());

            // step 1 again: every log file read-only, so neither rotation nor the open works
            CloseHandle(h0);
            put(l0, "RO0"); put(l1, "RO1"); put(l2, "RO2");
            for (const wchar_t* f : {l0, l1, l2}) SetFileAttributesW(f, FILE_ATTRIBUTE_READONLY);
            LogOpen(l0, 2, true);
            Log("loaded as test");
            LogClose();
            text = read(lp.c_str());
            Check(has(text, "loaded as test") && has(text, "(Windows error 5") && has(text, "access denied"),
                  "log fallback: read-only logs: OblivionPerfFix.<pid>.log, reason access denied");
            DeleteFileW(lp.c_str());
            SetFileAttributesW(l2, FILE_ATTRIBUTE_NORMAL);
            SetFileAttributesW(l1, FILE_ATTRIBUTE_NORMAL);

            // a read-only log alone is no failure: rotation moves it away (Windows renames read-only files)
            LogOpen(l0, 2, true);
            Log("loaded as test");
            LogClose();
            Check(has(read(l0), "loaded as test") && read(l1) == "RO0" && !exists(lp),
                  "log: a read-only log alone rotates away and the normal log opens (no fallback)");
            SetFileAttributesW(l1, FILE_ATTRIBUTE_NORMAL);

            // step 2: the same folder fails too
            put(l0, "HELD"); put(lp.c_str(), "HELDP");
            h0 = hold(l0);
            HANDLE hp = hold(lp.c_str());
            LogOpen(l0, 2, true);
            Log("loaded as test");
            LogClose();
            text = read(lt.c_str());
            Check(has(text, "loaded as test"), "log fallback: both held: the session's lines go to %TEMP%");
            Check(has(text, "could not open logfile\\OblivionPerfFix.log (") && has(text, ("could not open logfile\\OblivionPerfFix." + std::to_string(GetCurrentProcessId()) + ".log (").c_str()),
                  "log fallback: the %TEMP% log names both files that failed");
            Check(read(l0) == "HELD" && read(lp.c_str()) == "HELDP", "log fallback: the held files are untouched");

            // nowhere left: no crash, nothing written, the fix goes on
            HANDLE ht = hold(lt.c_str());
            LogOpen(l0, 2, true);
            Log("loaded as test");
            Check(LogEnabled() && read(l0) == "HELD" && read(lp.c_str()) == "HELDP", "log fallback: all three held: Log does nothing, no crash");
            LogClose();
            CloseHandle(ht);
            DeleteFileW(lt.c_str());
            CloseHandle(hp);
            DeleteFileW(lp.c_str());

            // LogFile=0: no fallback file at start-up either; a crash report takes the fallback
            LogOpen(l0, 2, false);
            Log("loaded as test");
            Check(!exists(lp) && !exists(lt) && read(l0) == "HELD", "log fallback: LogFile=0: nothing created at start-up, also with a held log");
            Log("[gpucrash] device removed reason 0x887A0006");
            Log("[mem] after the crash report");
            LogClose();
            text = read(lp.c_str());
            Check(has(text, "device removed reason") && has(text, "LogFile=0") && has(text, "could not open") &&
                      !has(text, "loaded as test") && !has(text, "after the crash report"),
                  "log fallback: LogFile=0: a crash report takes the fallback, with both notes and nothing else");
            DeleteFileW(lp.c_str());

            // clean-up: only the two newest other <pid> logs stay; rotated logs and other names are left alone
            const wchar_t* const old[] = {L"logfile\\OblivionPerfFix.900000104.log", L"logfile\\OblivionPerfFix.900000108.log",
                                          L"logfile\\OblivionPerfFix.900000112.log", L"logfile\\OblivionPerfFix.900000116.log",
                                          L"logfile\\OblivionPerfFix.900000120.log"};
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER age{{now.dwLowDateTime, now.dwHighDateTime}};
            for (const wchar_t* f : old) {  // made oldest first, an hour apart
                put(f, "OLD");
                age.QuadPart -= 36000000000ULL;
            }
            for (const wchar_t* f : old) {
                age.QuadPart += 36000000000ULL;
                FILETIME ft{age.LowPart, age.HighPart};
                HANDLE file = CreateFileW(f, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                SetFileTime(file, nullptr, nullptr, &ft);
                CloseHandle(file);
            }
            const wchar_t* lOther = L"logfile\\OblivionPerfFix.notes.log";
            put(lOther, "KEEP"); put(l1, "KEEP1"); put(l2, "KEEP2");
            CloseHandle(h0);
            LogOpen(l0, 2, true);  // a normal start
            Log("loaded as test");
            LogClose();
            Check(exists(old[0]) && exists(old[4]), "log fallback: a normal start leaves earlier <pid> logs alone");
            put(l1, "KEEP1"); put(l2, "KEEP2");
            h0 = hold(l0);
            LogOpen(l0, 2, true);
            Log("loaded as test");
            LogClose();
            CloseHandle(h0);
            Check(!exists(old[0]) && !exists(old[1]) && !exists(old[2]) && exists(old[3]) && exists(old[4]) && exists(lp),
                  "log fallback: the new log and the two newest others stay, older <pid> logs are deleted");
            Check(read(lOther) == "KEEP" && exists(l2) && exists(l0), "log fallback: other names and the rotated logs stay");
            DeleteFileW(old[3]); DeleteFileW(old[4]); DeleteFileW(lOther); DeleteFileW(lp.c_str());

            // a folder name outside the C locale: the top line must survive (UTF-8)
            const std::wstring dirU = std::wstring(L"logfile\\") + static_cast<wchar_t>(0x041F);
            CreateDirectoryW(dirU.c_str(), nullptr);
            const std::wstring lu = dirU + L"\\OblivionPerfFix.log";
            const std::wstring lup = dirU + L"\\OblivionPerfFix." + pid + L".log";
            h0 = hold(lu.c_str());
            LogOpen(lu.c_str(), 0, true);
            Log("loaded as test");
            LogClose();
            CloseHandle(h0);
            text = read(lup.c_str());
            Check(has(text, "could not open logfile\\\xD0\x9F\\OblivionPerfFix.log (Windows error 32") && has(text, "loaded as test"),
                  "log fallback: a non-ASCII path is written in full (UTF-8)");
            DeleteFileW(lu.c_str()); DeleteFileW(lup.c_str());
            RemoveDirectoryW(dirU.c_str());

            // Game builds: the known pairs say nothing; anything else gets one plain line, and nothing is switched off by it
            put(l0, "");
            LogOpen(l0, 0, true);
            GameModule build;
            build.timeDateStamp = 0xF19077A4u; build.sizeOfImage = 0x9E1E000u;
            const bool steamKnown = NoteGameBuild(build);
            build.timeDateStamp = 0x94273715u; build.sizeOfImage = 0x94A7000u;
            const bool gamePassKnown = NoteGameBuild(build);
            Check(steamKnown && gamePassKnown && !has(read(l0), "not recognised"), "game build: Steam 19115871 and Game Pass: no line");
            build.timeDateStamp = 0xF19077A4u; build.sizeOfImage = 0x9E58000u;
            const bool sizeDiffers = NoteGameBuild(build);
            build.timeDateStamp = 0xA63B96A2u; build.sizeOfImage = 0x9E58000u;  // the build in issue #43
            const bool stampDiffers = NoteGameBuild(build);
            LogClose();
            text = read(l0);
            Check(!sizeDiffers && !stampDiffers && has(text, "game build not recognised (TimeDateStamp 0xA63B96A2, SizeOfImage 0x9E58000)") &&
                      has(text, "stays off"),
                  "game build: another stamp or size: one line with both values and what it means");

            if (hadTmp) SetEnvironmentVariableW(L"TMP", oldTmp); else SetEnvironmentVariableW(L"TMP", nullptr);
            RemoveDirectoryW(L"logfile\\tmp");
        }
        for (const wchar_t* f : {l0, l1, l2}) SetFileAttributesW(f, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(l0); DeleteFileW(l1); DeleteFileW(l2); DeleteFileW(ini);
        RemoveDirectoryW(L"logfile");
    }
    LogOpen(L"selftest.log");
    LogGoAsync(nullptr);  // the rest of the self-test logs through the writer thread, as the game does

    GameModule game;
    Check(InitGameModule(game, module), "game module sections found");
    SetLoadStage(LoadStage::InDllMain);  // the self-test runs the fixes as version.dll's DllMain does (the default is Late)
    if (game.timeDateStamp == 0xF19077A4u) Check(NoteGameBuild(game), "game build: this exe is the Steam build NoteGameBuild knows");
    PerclosCapture(game);  // PerceptionLosPrefetch's sites before any test rewrites the round or the helper (lab tests do)
    scantest::Begin();  // every search below is checked against the reference search
    scantest::RandomTests(game);
    hooks_test::Run(game);  // other mods' hooks and the load stage (selftest_hooks.h); puts the mapped exe back

    // Runtime helpers: long-uptime clocks and affinity-limited helper counts.
    Check(QpcToMicroseconds(184467440737095ULL, 10000000) == 18446744073709ULL,
          "QPC: long uptime does not overflow before division");
    Check(QpcToMicroseconds(9223372036854775807ULL, 10000000) == 922337203685477580ULL,
          "QPC: largest signed counter converts without signed overflow");
    Check(QpcToMicroseconds(10000001, 3000000) == 3333333,
          "QPC: fractional microseconds truncate as before");
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask) {
        const DWORD_PTR one = processMask & (~processMask + 1);
        if (SetProcessAffinityMask(GetCurrentProcess(), one)) {
            Check(UsableLogicalProcessors() == 1, "CPU count: one usable processor under process affinity");
            const DWORD_PTR rest = processMask & ~one;
            if (rest && SetProcessAffinityMask(GetCurrentProcess(), one | (rest & (~rest + 1))))
                Check(UsableLogicalProcessors() == 2, "CPU count: two usable processors under process affinity");
            Check(SetProcessAffinityMask(GetCurrentProcess(), processMask) != FALSE, "CPU count: original affinity restored");
        }
    }
    Check(UsableLogicalProcessors() > 0, "CPU count: usable processors available");

    // BoneUploadStores: expected sites (build 19115871) and their replacement bytes.
    struct Site { uint32_t rva; const char* bytes; };
    const Site sites[] = {
        {0x5C75CB, "C5 F8 11 21 90"}, {0x5C74CB, "C5 F8 11 21 90"},  // avx2 32+16
        {0x5C77C6, "C5 F8 11 09 90"}, {0x5C7896, "C5 F8 11 09 90"},  // avx 16+16+16
        {0x5C736E, "C5 F8 11 11 66 90"},                             // avx512 k-mask
    };
    // BoneUploadStores run for real: every variant of the game's bone copy (the ISPC export UpdateBoneData_CopyBones) on
    // test data, before and after the patch, against the SSE4 variant (plain stores only) as the reference. The game
    // picks the variant by CPU (exe+0x5C7D50: 5 = AVX-512 F/DQ/CD/BW/VL, e.g. Ryzen 7000/9000 and Intel 11th gen; 3 = AVX2;
    // 2 = AVX; 1 = SSE4). Variants this CPU cannot run are skipped: for AVX-512 run the self-test under Intel SDE,
    // `sde -icx -- selftest.exe <exe> --bones` (or on a CPU with AVX-512, as GitHub's runners usually are).
    struct BoneVariant { const char* name; uint32_t rva; int level; bool varying; };
    const BoneVariant boneVariants[] = {
        {"sse4", 0x5C7CE0, 1, false},           {"avx", 0x5C77F0, 2, false},
        {"avx2", 0x5C7550, 3, false},           {"avx2 varying, all lanes", 0x5C75F0, 3, true},
        {"avx512", 0x5C7310, 5, false},         {"avx512-target varying, all lanes", 0x5C7390, 3, true},
    };
    using CopyBones = void (*)(void* dst, const void* src, const uint16_t* map, int count);
    using CopyBonesVarying = void (*)(void* dst, const void* src, const uint16_t* map, int count, const void* mask);
    const int isaLevel = reinterpret_cast<int (*)()>(game.base + 0x5C7D50)();
    printf("      the game's bone copy variant on this CPU: level %d (%s)\n", isaLevel,
           isaLevel >= 5 ? "AVX-512" : isaLevel >= 3 ? "AVX2" : isaLevel >= 2 ? "AVX" : "SSE4");
    std::vector<uint8_t> boneSrc(97 * 64);
    std::vector<uint16_t> boneMap(1001);
    uint32_t boneSeed = 12345;
    auto next = [&] { boneSeed = boneSeed * 1664525u + 1013904223u; return boneSeed; };
    for (auto& b : boneSrc) b = static_cast<uint8_t>(next() >> 24);  // any bit pattern, NaNs and denormals included
    for (auto& m : boneMap) m = static_cast<uint16_t>(next() % 97);
    alignas(32) static const int32_t allLanes[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    constexpr size_t kGuard = 64;
    auto runBones = [&](const BoneVariant& v, int count) {
        std::vector<uint8_t> out(kGuard + boneMap.size() * 48 + kGuard, 0xCD);
        void* fn = game.base + v.rva;
        if (v.varying) reinterpret_cast<CopyBonesVarying>(fn)(out.data() + kGuard, boneSrc.data(), boneMap.data(), count, allLanes);
        else reinterpret_cast<CopyBones>(fn)(out.data() + kGuard, boneSrc.data(), boneMap.data(), count);
        return out;
    };
    const int boneCounts[] = {0, 1, 7, static_cast<int>(boneMap.size())};
    std::vector<std::vector<uint8_t>> bonesBefore;
    for (const BoneVariant& v : boneVariants)
        for (int count : boneCounts)
            if (isaLevel >= v.level) bonesBefore.push_back(runBones(v, count));

    Check(ApplyBoneUploadFix(), "bone upload fix applied");
    for (const Site& s : sites) {
        char what[64];
        snprintf(what, sizeof what, "store at exe+0x%X replaced", s.rva);
        Check(MatchesAt(game.base + s.rva, ParsePattern(s.bytes)), what);
    }
    Check(ApplyBoneUploadFix(), "second application finds the sites already patched");

    {
        std::vector<std::vector<uint8_t>> reference;
        for (int count : boneCounts) reference.push_back(runBones(boneVariants[0], count));
        size_t i = 0;
        for (const BoneVariant& v : boneVariants) {
            char what[128];
            if (isaLevel < v.level) {
                printf("SKIP  bone copy %s: this CPU cannot run it\n", v.name);
                continue;
            }
            bool samePatched = true, sameReference = true;
            for (size_t c = 0; c < std::size(boneCounts); ++c, ++i) {
                const std::vector<uint8_t> after = runBones(v, boneCounts[c]);
                samePatched = samePatched && after == bonesBefore[i];
                sameReference = sameReference && after == reference[c];
            }
            snprintf(what, sizeof what, "bone copy %s: the same bytes before and after the patch (guards untouched)", v.name);
            Check(samePatched, what);
            snprintf(what, sizeof what, "bone copy %s: the same bytes as the SSE4 variant", v.name);
            Check(sameReference, what);
        }
    }
    if (argc > 2 && wcscmp(argv[2], L"--bones") == 0) {
        printf("\n%s (%d failure(s))\n", g_failures ? "SELFTEST FAILED" : "SELFTEST OK", g_failures);
        return g_failures ? 1 : 0;
    }

    // AtmosphereDirtyFilter: 15 accessor vtables (build 19115871), PostSet slot no longer points at the original.
    Check(ApplyAtmosphereDirtyFix(), "atmosphere dirty filter applied");
    auto** postSetSlot = reinterpret_cast<void**>(game.base + 0x7AF3EA8 + 3 * 8);
    Check(*postSetSlot != game.base + 0x4574080, "vtable exe+0x7AF3EA8 PostSet slot hooked");

    // AtmosphereLightFastPath, light fast path: UpdateColorAndBrightness, ULightComponent field offsets (Intensity,
    // IndirectLightingIntensity, VolumetricScatteringIntensity, Temperature) and Mobility.
    uint8_t* ucab = nullptr;
    int32_t mobility = 0;
    std::vector<uint32_t> fields;
    size_t thunks = 0;
    Check(AtmosphereLightFastPathInfo(&ucab, &mobility, &fields, &thunks), "light fast path found");
    Check(ucab == game.base + 0x304C150, "UpdateColorAndBrightness at exe+0x304C150");
    Check(fields == std::vector<uint32_t>{0x2D4, 0x2D8, 0x2EC, 0x2F0, 0x2F8},
          "fast-path fields 0x2D4 0x2D8 (LightColor, from SetLightColor exe+0x3045580) 0x2EC 0x2F0 0x2F8");
    Check(mobility == 0x1A5, "Mobility at 0x1A5");
    Check(thunks >= 2, "native exec thunks of the fast-path setters found");

    // AtmosphereLightFastPath's cloud path: the directional light proxy's constructor copies, its SetAtmosphereRelatedProperties
    // wrapped; values left by the game thread are written into the proxy right after the renderer's own.
    {
        AtmosphereCloudPathInfoOut cp{};
        Check(AtmosphereCloudPathInfo(&cp), "cloud path found");
        Check(cp.compSurface == 0x4BC && cp.compScale == 0x4D0 && cp.proxySurface == 0x2C8 && cp.proxyScale == 0x2D8,
              "cloud path: component 0x4BC/0x4D0 -> proxy 0x2C8/0x2D8");
        Check(cp.setter == game.base + 0x3043800 && cp.vtable == reinterpret_cast<void**>(game.base + 0x75E9400) &&
                  cp.slot == 0x118 / 8 && cp.numVtables >= 1,
              "cloud path: SetAtmosphereRelatedProperties exe+0x3043800 in slot +0x118 of vtable exe+0x75E9400");
        Check(cp.hooked && cp.vtable[cp.slot] != cp.setter, "cloud path: the slot is wrapped");
        // The other sun fields Altar's weather moves, each confirmed by its reflected name
        struct Want { const char* name; uint32_t comp, size, dst; bool info; };
        const Want want[] = {{"CloudShadowOnSurfaceStrength", 0x4BC, 4, 0x2C8, false},
                             {"CloudScatteredLuminanceScale", 0x4D0, 16, 0x2D8, false},
                             {"CloudShadowOnAtmosphereStrength", 0x4B8, 4, 0x2C4, false},
                             {"OcclusionMaskDarkness", 0x438, 4, 0x248, false},
                             {"LightSourceAngle", 0x488, 4, 0x2A8, false},
                             {"LightSourceSoftAngle", 0x48C, 4, 0x2AC, false},
                             {"SpecularScale", 0x314, 4, 0x188, false},
                             {"SpecularScaleWater", 0x318, 4, 0x18C, false},
                             {"BloomScale", 0x38C, 4, 0xE8, true},
                             {"BloomThreshold", 0x390, 4, 0xEC, true},
                             {"BloomMaxBrightness", 0x394, 4, 0xF0, true},
                             {"BloomTint", 0x398, 4, 0xF4, true}};
        bool fieldsOk = cp.fields.size() == std::size(want) && cp.lightInfo == 0x150;
        for (size_t i = 0; fieldsOk && i < std::size(want); ++i) {
            const auto& f = cp.fields[i];
            fieldsOk = strcmp(f.name, want[i].name) == 0 && f.comp == want[i].comp && f.size == want[i].size &&
                       f.dst == want[i].dst && f.inLightInfo == want[i].info;
            if (!fieldsOk) printf("  field %zu: %s 0x%X (%u) -> 0x%X%s\n", i, f.name, f.comp, f.size, f.dst, f.inLightInfo ? " in info" : "");
        }
        Check(fieldsOk, "cloud path: 12 sun fields (cloud, occlusion, source angles, specular, bloom), light info at +0x150");
        Check(cp.component == 0x10, "cloud path: the proxy's light component at +0x10 (the sky-light check)");
        using SetAtm = void (*)(void*, const void*, const void*);
        const SetAtm setAtm = reinterpret_cast<SetAtm>(cp.vtable[cp.slot]);
        // a fake light component (0x500 bytes) holding a fake proxy (the real vtable) at 0x3A8
        alignas(16) static uint8_t proxy[0x300];
        alignas(16) static uint8_t comp[0x500];
        alignas(16) static uint8_t before[0x500];  // the component before each change
        alignas(16) static uint8_t lightInfo[0x100];  // its FLightSceneInfo
        *reinterpret_cast<void**>(proxy) = cp.vtable;
        *reinterpret_cast<void**>(proxy + 0x150) = lightInfo;
        *reinterpret_cast<void**>(proxy + 0x10) = comp;
        *reinterpret_cast<void**>(comp + 0x3A8) = proxy;
        const float surface = 0.625f, scale[4] = {0.5f, 0.75f, 1.0f, 1.0f}, surface2 = 0.25f;
        const float specular = 0.8f, angle = 0.5357f, bloomMax = 12.0f;
        const float transmittance[4] = {0.1f, 0.2f, 0.3f, 1.0f}, sunDisc[4] = {2.0f, 3.0f, 4.0f, 1.0f};
        setAtm(proxy, transmittance, sunDisc);  // the renderer draws it as a sky light
        memcpy(before, comp, sizeof comp);  // a fresh proxy: zero cloud values, as the component had
        memcpy(comp + 0x4BC, &surface, 4);
        memcpy(comp + 0x4D0, scale, 16);
        memcpy(comp + 0x314, &specular, 4);
        memcpy(comp + 0x488, &angle, 4);
        memcpy(comp + 0x394, &bloomMax, 4);
        Check(AtmosphereCloudTestPost(before, comp, 0x500), "cloud path: values left for the proxy");
        Check(AtmosphereCloudPathInfo(&cp) && cp.sceneProxy == 0x3A8 && cp.pending == 1,
              "cloud path: the component's proxy pointer found at 0x3A8, one proxy waiting");
        Check(*reinterpret_cast<const float*>(proxy + 0x2C8) == 0.0f, "cloud path: the game thread does not write the proxy");
        setAtm(proxy, transmittance, sunDisc);
        Check(memcmp(proxy + 0x27C, transmittance, 16) == 0 && memcmp(proxy + 0x26C, sunDisc, 16) == 0,
              "cloud path: the renderer's own values written (the game's setter ran)");
        Check(memcmp(proxy + 0x2C8, &surface, 4) == 0 && memcmp(proxy + 0x2D8, scale, 16) == 0,
              "cloud path: the cloud values written after them");
        Check(memcmp(proxy + 0x188, &specular, 4) == 0 && memcmp(proxy + 0x2A8, &angle, 4) == 0 &&
                  memcmp(lightInfo + 0xF0, &bloomMax, 4) == 0,
              "cloud path: specular scale and source angle into the proxy, bloom max brightness into its light info");
        Check(AtmosphereCloudPathInfo(&cp) && cp.pending == 0 && cp.applied == 1, "cloud path: nothing left waiting");
        // newer values overwrite what waits; a rebuild (forwarded PostSet) drops them
        memcpy(before, comp, sizeof comp);
        memcpy(comp + 0x4BC, &surface2, 4);
        Check(AtmosphereCloudTestPost(before, comp, 0x500) && AtmosphereCloudTestPost(comp, comp, 0x500),
              "cloud path: values left twice");
        Check(AtmosphereCloudPathInfo(&cp) && cp.inStep == 2 && cp.outOfStep == 0,
              "cloud path: before each change the proxy held the light's previous values");
        AtmosphereCloudTestForget(comp, 0x500);
        setAtm(proxy, transmittance, sunDisc);
        Check(memcmp(proxy + 0x2C8, &surface, 4) == 0 && AtmosphereCloudPathInfo(&cp) && cp.pending == 0,
              "cloud path: dropped for a rebuilt light, the proxy keeps its values");
        // not picked up within 250 ms (not an atmosphere light, nothing rendered): the next change rebuilds the light
        Check(AtmosphereCloudTestPost(comp, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.outOfStep == 0 &&
                  cp.duringRebuild == 1 && cp.rebuilt == 1,
              "cloud path: values left again while the light is being rebuilt (not counted out of step)");
        AtmosphereCloudTestAge(300000);  // the rebuild mark expires with the waiting values
        setAtm(proxy, transmittance, sunDisc);
        memcpy(before, comp, sizeof comp);
        memcpy(comp + 0x4BC, &surface, 4);
        Check(AtmosphereCloudTestPost(before, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.outOfStep == 0 && cp.inStep == 3,
              "cloud path: written late but written: in step again");
        AtmosphereCloudTestAge(300000);
        Check(!AtmosphereCloudTestPost(comp, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.pending == 0 && cp.late == 1,
              "cloud path: values not picked up in time: the light is rebuilt instead");
        // what waits for a light is dropped once the light has lost that proxy (dimmed to 0 through the fast path, which
        // removes it from the scene): a later proxy at the same address copies the component itself (issue #1)
        memcpy(proxy + 0x2C8, &surface, 4);  // the rebuild above built it from the component
        setAtm(proxy, transmittance, sunDisc);
        memcpy(before, comp, sizeof comp);
        memcpy(comp + 0x4BC, &surface2, 4);
        Check(AtmosphereCloudTestPost(before, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.pending == 1,
              "cloud path: values left for the proxy (before it goes away)");
        *reinterpret_cast<void**>(comp + 0x3A8) = nullptr;  // removed from the scene
        Check(AtmosphereCloudTestPost(comp, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.pending == 0,
              "cloud path: a light that lost its proxy drops what waited for it");
        *reinterpret_cast<void**>(comp + 0x3A8) = proxy;  // shown again, the new proxy at the old address
        setAtm(proxy, transmittance, sunDisc);
        Check(memcmp(proxy + 0x2C8, &surface, 4) == 0, "cloud path: the new proxy at that address gets no old values");
        // a forwarded PostSet while the light has no proxy drops them too
        memcpy(proxy + 0x2C8, &surface2, 4);  // built from the component when shown
        memcpy(before, comp, sizeof comp);
        memcpy(comp + 0x4BC, &surface, 4);
        Check(AtmosphereCloudTestPost(before, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.pending == 1,
              "cloud path: values left again");
        *reinterpret_cast<void**>(comp + 0x3A8) = nullptr;
        AtmosphereCloudTestForget(comp, 0x500);
        Check(AtmosphereCloudPathInfo(&cp) && cp.pending == 0, "cloud path: a rebuild without a proxy drops them");
        *reinterpret_cast<void**>(comp + 0x3A8) = proxy;
        // values left for one light are not written into another light's proxy that now lives at that address
        memcpy(proxy + 0x2C8, &surface, 4);
        setAtm(proxy, transmittance, sunDisc);
        memcpy(before, comp, sizeof comp);
        memcpy(comp + 0x4BC, &surface2, 4);
        Check(AtmosphereCloudTestPost(before, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.pending == 1,
              "cloud path: values left for the first light");
        *reinterpret_cast<void**>(proxy + 0x10) = before;  // the address now holds another light's proxy
        setAtm(proxy, transmittance, sunDisc);
        Check(memcmp(proxy + 0x2C8, &surface, 4) == 0 && AtmosphereCloudPathInfo(&cp) && cp.pending == 0,
              "cloud path: another light's proxy at that address is left alone, the values dropped");
        *reinterpret_cast<void**>(proxy + 0x10) = comp;
        memcpy(proxy + 0x2C8, &surface2, 4);  // as a rebuild would leave it
        Check(AtmosphereCloudPathInfo(&cp) && cp.outOfStep == 0, "cloud path: still never out of step");
        // a directional light the renderer does not draw as a sky light (the weather's lightning light): its specular
        // scale or source angle would never be written, so it is rebuilt as before
        {
            alignas(16) static uint8_t proxy2[0x300];
            alignas(16) static uint8_t comp2[0x500];
            *reinterpret_cast<void**>(proxy2) = cp.vtable;
            *reinterpret_cast<void**>(proxy2 + 0x10) = comp2;
            *reinterpret_cast<void**>(comp2 + 0x3A8) = proxy2;
            Check(!AtmosphereCloudTestPost(comp2, comp2, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.notSky == 1,
                  "cloud path: a directional light not drawn as a sky light is rebuilt as before");
        }
        // a pointer that is not a directional proxy: rebuild
        *reinterpret_cast<void**>(comp + 0x3A8) = comp + 0x100;
        Check(!AtmosphereCloudTestPost(comp, comp, 0x500), "cloud path: no directional proxy, no shortcut");
        *reinterpret_cast<void**>(comp + 0x3A8) = nullptr;
        Check(AtmosphereCloudTestPost(comp, comp, 0x500) && AtmosphereCloudPathInfo(&cp) && cp.pending == 0,
              "cloud path: a light without a proxy (hidden): nothing to update, nothing left waiting");
        // the sky's light choice (issue #1): two lit directional lights at one atmosphere index are rebuilt as in the game
        {
            const int32_t idx = AtmosphereSunIndexOffset();
            printf("  AtmosphereSunLightIndex at 0x%X\n", static_cast<unsigned>(idx));
            Check(idx > 0x30 && idx < 0x500, "sky light choice: AtmosphereSunLightIndex found in the property tables");
            alignas(16) static uint8_t sunProxy[0x300], thunderProxy[0x300];
            alignas(16) static uint8_t sun[0x500], thunder[0x500];
            *reinterpret_cast<void**>(sunProxy) = cp.vtable;
            *reinterpret_cast<void**>(thunderProxy) = cp.vtable;
            *reinterpret_cast<void**>(sun + 0x3A8) = sunProxy;
            *reinterpret_cast<void**>(sunProxy + 0x10) = sun;
            *reinterpret_cast<void**>(thunderProxy + 0x10) = thunder;
            if (idx > 0 && idx < 0x500) sun[idx] = thunder[idx] = 0;
            setAtm(sunProxy, transmittance, sunDisc);  // the renderer draws the sun as the sky light
            Check(!AtmosphereSkyChoiceTest(sun, 0x500) && !AtmosphereSkyChoiceTest(thunder, 0x500) &&
                      !AtmosphereSkyChoiceTest(sun, 0x500),
                  "sky light choice: the sun alone (the thunder light unlit): not contested");
            *reinterpret_cast<void**>(thunder + 0x3A8) = thunderProxy;  // a flash: both lit at index 0
            Check(AtmosphereSkyChoiceTest(thunder, 0x500) && AtmosphereSkyChoiceTest(sun, 0x500),
                  "sky light choice: two lit lights at index 0: both rebuilt as in the game");
            if (idx > 0 && idx < 0x500) thunder[idx] = 1;
            AtmosphereSkyChoiceTest(thunder, 0x500);
            Check(!AtmosphereSkyChoiceTest(sun, 0x500), "sky light choice: lit at different indices: the sun not contested");
            if (idx > 0 && idx < 0x500) thunder[idx] = 0;
            *reinterpret_cast<void**>(thunder + 0x3A8) = nullptr;  // the flash over
            Check(!AtmosphereSkyChoiceTest(thunder, 0x500) && !AtmosphereSkyChoiceTest(sun, 0x500),
                  "sky light choice: the thunder light out again: the sun alone, not contested");
            Sleep(150);  // the sun lit but no longer drawn as the sky light: another light holds its index
            Check(AtmosphereSkyChoiceTest(sun, 0x500), "sky light choice: a lit sun not drawn as the sky light is rebuilt");
            setAtm(sunProxy, transmittance, sunDisc);
            Check(!AtmosphereSkyChoiceTest(sun, 0x500), "sky light choice: drawn again: not contested");
        }
    }

    // RdgParallelTranslate: the RDG translate-priority site, switched on and off.
    Check(FindRdgParallelTranslateSite(), "RDG translate-priority site found");
    Check(RdgParallelTranslateSite() == game.base + 0x26B9B08, "site at exe+0x26B9B08");
    // Defrag guard: d3d12.VRAMBufferPoolDefrag is paused before parallel translate goes on, and comes back
    // a second after it went off.
    SetRdgParallelTranslateEnabled(true);
    Check(MatchesAt(game.base + 0x26B9B08, ParsePattern("41 0F 95 C0")), "without the defrag guard found, parallel translate stays off");
    Check(FindPoolDefragSwitch(), "defrag guard: buffer pool defrag switch found");
    uint8_t* defragVar = nullptr;
    uint8_t* maxCopyVar = nullptr;
    PoolDefragTestAddresses(&defragVar, &maxCopyVar);
    Check(defragVar == game.base + 0x8DD46A4 && maxCopyVar == game.base + 0x8DD46A8,
          "d3d12.VRAMBufferPoolDefrag at exe+0x8DD46A4, MaxCopySizePerFrame at exe+0x8DD46A8");
    Check(defragVar && *reinterpret_cast<int32_t*>(defragVar) == 1 && *reinterpret_cast<int32_t*>(maxCopyVar) == 32 * 1024 * 1024,
          "defrag on by default, 32 MB per frame");
    SetRdgParallelTranslateEnabled(true);
    Check(defragVar && *reinterpret_cast<int32_t*>(defragVar) == 0, "defrag paused when parallel translate is asked for");
    Check(MatchesAt(game.base + 0x26B9B08, ParsePattern("41 0F 95 C0")), "parallel translate waits until defrag has been paused a moment");
    Sleep(300);
    SetRdgParallelTranslateEnabled(true);
    Check(MatchesAt(game.base + 0x26B9B08, ParsePattern("41 B0 01 90")), "site forced to mov r8b,1");
    *reinterpret_cast<int32_t*>(defragVar) = 2;  // the engine applying an Engine.ini value after start-up
    SetRdgParallelTranslateEnabled(true);
    Check(*reinterpret_cast<int32_t*>(defragVar) == 0, "an Engine.ini value is paused again while parallel translate is on");
    SetRdgParallelTranslateEnabled(false);
    Check(MatchesAt(game.base + 0x26B9B08, ParsePattern("41 0F 95 C0")), "site restored to setne r8b");
    Check(*reinterpret_cast<int32_t*>(defragVar) == 0, "defrag still paused right after parallel translate went off");
    Sleep(1100);
    SetRdgParallelTranslateEnabled(false);
    Check(*reinterpret_cast<int32_t*>(defragVar) == 2, "defrag back to the game's setting a second later");
    *reinterpret_cast<int32_t*>(defragVar) = 1;
    Check(FindRdgParallelTranslateSite(), "site found again after a toggle");

    // Frame fence (translate_frame_fence.cpp): an RHI thread fence after every FRHICommandListImmediate::BeginFrame, so
    // buffer defragmentation can run while parallel translate is on.
    {
        Check(FindFrameFence(), "frame fence: BeginFrame, RHIThreadFence and the event release found");
        FrameFenceInfoOut ff{};
        Check(FrameFenceInfo(&ff), "frame fence info");
        Check(ff.beginFrame == game.base + 0x2652B70 && ff.commandVtable == game.base + 0x7286F28,
              "frame fence: BeginFrame exe+0x2652B70, FRHICommandBeginFrame vtable exe+0x7286F28");
        Check(ff.threadFence == game.base + 0x265EC40 && ff.recycle == game.base + 0xDD03A0 && ff.refCount == 0x38,
              "frame fence: RHIThreadFence exe+0x265EC40, event release +0x38 -> exe+0xDD03A0");
        Check(ff.completeShift == 26, "frame fence: graph event complete bit 26");
        Check(ff.rhiThreadFlag != nullptr, "frame fence: the RHI-thread flag BeginFrame reads");
        // The real BeginFrame runs below (through the detour and the trampoline) on a fake immediate command list: it
        // allocates its 16-byte command from the list's memory stack (+0x98 top, +0xA0 end), links it (+0x08) and, with
        // the RHI-thread flag set, skips the flush.
        if (ff.rhiThreadFlag) *ff.rhiThreadFlag = 1;
        Check(InstallFrameFence(true), "frame fence installed");
        Check(game.base[0x2652B70] == 0xE9, "frame fence: BeginFrame's entry jumps to the hook");
        FrameFenceTestFakes(reinterpret_cast<void*>(&FakeThreadFence), reinterpret_cast<void*>(&FakeRecycle));
        g_fakeCommandVtable = ff.commandVtable;
        alignas(16) static uint8_t list[0x300];
        void* link = nullptr;
        auto put = [&](size_t off, void* v) { memcpy(list + off, &v, sizeof v); };
        auto reset = [&] {
            memset(list, 0, sizeof list);
            memset(g_fakeStack, 0, sizeof g_fakeStack);
            put(0x98, g_fakeStack);
            put(0xA0, g_fakeStack + sizeof g_fakeStack);
            put(0x08, &link);
            link = nullptr;
        };
        auto beginFrame = reinterpret_cast<void (*)(void*)>(game.base + 0x2652B70);
        reset();
        beginFrame(list);
        Check(link == g_fakeStack && *reinterpret_cast<uint8_t**>(g_fakeStack) == ff.commandVtable && g_fakeFences == 0,
              "fence off: BeginFrame queues its command as the game does (trampoline), no fence");
        SetFrameFence(true);
        reset();
        beginFrame(list);
        Check(g_fakeFences == 1 && g_fakeFenceLock && g_fakeFenceAfterCommand && g_fakeRecycled == 0,
              "fence on: RHIThreadFence(true) right after the BeginFrame command; the first event is kept");
        SetFrameFence(true);
        Check(!FrameFenceReady(), "frame fence not in effect while the first fence has not run");
        reset();
        beginFrame(list);
        Check(g_fakeFences == 2 && g_fakeRecycled == 1 && g_fakeLastRecycled == &g_fakeEvents[1],
              "later fences are released at once (last reference -> recycled)");
        g_fakeEvents[0].head |= 1ull << 26;  // the RHI thread ran the first fence
        SetFrameFence(true);
        Check(FrameFenceReady() && g_fakeRecycled == 2 && g_fakeLastRecycled == &g_fakeEvents[0],
              "frame fence in effect once the first fence has run (its event released)");
        SetFrameFence(false);
        reset();
        beginFrame(list);
        Check(!FrameFenceReady() && g_fakeFences == 2, "fence off again: no fence after BeginFrame");

        // With the translate switch: fenced, defragmentation keeps running while parallel translate is on.
        SetRdgUseFrameFence(true);
        SetRdgParallelTranslateEnabled(true);
        Check(*reinterpret_cast<int32_t*>(defragVar) == 0 && MatchesAt(game.base + 0x26B9B08, ParsePattern("41 0F 95 C0")),
              "translate asked for before the fence is in effect: defrag paused first, as before");
        reset();
        beginFrame(list);
        g_fakeEvents[(g_fakeFences - 1) % 16].head |= 1ull << 26;
        SetRdgParallelTranslateEnabled(true);
        Check(FrameFenceReady() && *reinterpret_cast<int32_t*>(defragVar) == 1 &&
                  MatchesAt(game.base + 0x26B9B08, ParsePattern("41 B0 01 90")),
              "fence in effect: parallel translate on with buffer defragmentation running");
        SetRdgUseFrameFence(false);
        SetRdgParallelTranslateEnabled(true);
        Check(*reinterpret_cast<int32_t*>(defragVar) == 0 && FrameFenceReady(),
              "RdgFrameFence=0 while translating: defrag paused first, the fence stays until the pause is in effect");
        Sleep(300);
        SetRdgParallelTranslateEnabled(true);
        Check(!FrameFenceReady() && *reinterpret_cast<int32_t*>(defragVar) == 0 &&
                  MatchesAt(game.base + 0x26B9B08, ParsePattern("41 B0 01 90")),
              "... then the fence goes; translate stays on with defrag paused (0.15 behaviour)");
        SetRdgUseFrameFence(true);
        SetRdgParallelTranslateEnabled(false);
        Sleep(1100);
        SetRdgParallelTranslateEnabled(false);
        Check(!FrameFenceReady() && *reinterpret_cast<int32_t*>(defragVar) == 1 &&
                  MatchesAt(game.base + 0x26B9B08, ParsePattern("41 0F 95 C0")),
              "translate off: fence off and defrag back a second later");
    }

    // RdgParallelTranslate, descriptor heap guard: the sub-allocated view heap's GetTotalSize (vtable slot 5) is
    // wrapped; a heap without a block gets one through its own OpenCommandList (slot 3).
    Check(InstallDescriptorHeapGuard(), "descriptor heap guard installed");
    void** heapVtable = nullptr;
    size_t blockOffset = 0;
    void* guardHook = nullptr;
    Check(DescriptorHeapGuardInfo(&heapVtable, &blockOffset, &guardHook), "descriptor heap guard info");
    Check(heapVtable == reinterpret_cast<void**>(game.base + 0x7259348), "sub-allocated view heap vtable at exe+0x7259348");
    Check(blockOffset == 0x28 && blockOffset == offsetof(FakeHeap, block), "CurrentBlock at +0x28");
    Check(heapVtable && heapVtable[5] == guardHook, "GetTotalSize slot points at the guard");
    Check(InstallDescriptorHeapGuard(), "second installation finds the guard in place");
    {
        void* fakeVtable[8] = {};
        fakeVtable[3] = reinterpret_cast<void*>(&FakeOpenCommandList);
        FakeHeap heap{fakeVtable, {}, nullptr};
        auto getTotalSize = reinterpret_cast<uint32_t (*)(void*)>(guardHook);
        const uint32_t first = getTotalSize(&heap);
        Check(first == 2000 && g_fakeOpens == 1, "heap without a block: block allocated, size 2000 (was 0 = the crash)");
        const uint32_t second = getTotalSize(&heap);
        Check(second == 2000 && g_fakeOpens == 1 && DescriptorHeapGuardHits() == 1, "heap with a block: passed through");
    }
    SetRdgParallelTranslateAllowed(false);
    SetRdgParallelTranslateEnabled(true);
    Check(MatchesAt(game.base + 0x26B9B08, ParsePattern("41 0F 95 C0")), "without the guard parallel translate stays off");
    SetRdgParallelTranslateAllowed(true);

    // RdgSubmitBatch, submit batching: hooks, the engine's merge test, and the batching logic against fakes.
    Check(InstallRdgSubmitBatching(false), "submit batching installed");
    RdgSubmitInfo info{};
    Check(RdgSubmitBatchingInfo(&info), "submit batching info");
    Check(info.submitLists == game.base + 0x254EDF0, "RHISubmitCommandLists at exe+0x254EDF0");
    Check(info.submitCommands == game.base + 0x2551A40, "SubmitCommands at exe+0x2551A40");
    Check(info.submitPayloads == game.base + 0x25520F0, "SubmitPayloads at exe+0x25520F0");
    Check(info.vtableSlot == reinterpret_cast<void**>(game.base + 0x725FD58 + 0x418), "vtable slot exe+0x725FD58 + 0x418");
    Check(info.rhiVtable == reinterpret_cast<void**>(game.base + 0x725FD58), "FD3D12DynamicRHI vtable at exe+0x725FD58");
    Check(info.gDynamicRhi == reinterpret_cast<void**>(game.base + 0x9206108), "GDynamicRHI at exe+0x9206108");
    Check(info.queueOffset == 8 && info.numChecks == 11, "merge test: Queue at +8, 11 field checks");
    {
        const uint32_t offsets[11] = {0x08, 0x18, 0x30, 0x50, 0x60, 0x78, 0x80, 0x90, 0xB8, 0xC8, 0xD8};
        const uint8_t widths[11] = {4, 4, 4, 4, 1, 8, 8, 1, 4, 4, 4};
        bool same = info.numChecks == 11 && info.checkOnQueue[0];
        for (int i = 0; same && i < 11; ++i)
            same = info.checkOffset[i] == offsets[i] && info.checkWidth[i] == widths[i] && (i == 0 || !info.checkOnQueue[i]);
        Check(same, "merge test fields as in build 19115871");
    }
    Check(MatchesAt(game.base + 0x25520F0, ParsePattern("48 B8 ?? ?? ?? ?? ?? ?? ?? ?? FF E0 90 48 63 42 08")), "SubmitPayloads detoured");
    void* hookListsPtr = nullptr;
    void* hookPayloadsPtr = nullptr;
    RdgSubmitBatchingTestHooks(reinterpret_cast<void*>(&FakeSubmitLists), reinterpret_cast<void*>(&FakeSubmitPayloads),
                               &hookListsPtr, &hookPayloadsPtr);
    Check(info.vtableSlot && *info.vtableSlot == hookListsPtr, "RHISubmitCommandLists slot points at the hook");
    {
        auto hookLists = reinterpret_cast<void (*)(void*, const FakeView*, bool)>(hookListsPtr);
        auto hookPayloads = reinterpret_cast<void (*)(void*, const FakeView*)>(hookPayloadsPtr);
        static FakeQueue direct{0, 0}, asyncCompute{0, 1};
        static FakePayload payloads[16];
        static FakePayload* payloadPtrs[16];
        static FakeList lists[16];
        for (int i = 0; i < 16; ++i) {
            payloads[i] = {};
            payloads[i].queue = &direct;
            payloadPtrs[i] = &payloads[i];
            lists[i] = {&payloadPtrs[i], 1, 1};
        }
        int rhi = 0;
        auto submit = [&](int i, bool flush = false) {
            void* item = &lists[i];
            FakeView v{&item, 1};
            hookLists(&rhi, &v, flush);
        };
        auto is = [](const SubmitCall& c, std::initializer_list<int> want, bool flush) {
            if (c.payloads || c.flush != flush || c.lists.size() != want.size()) return false;
            size_t k = 0;
            for (int i : want)
                if (c.lists[k++] != &lists[i]) return false;
            return true;
        };

        submit(0);  // batching still off: straight through
        auto calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {0}, false), "batching off: passed straight through");

        SetRdgSubmitBatch(4, 20000);
        submit(0); submit(1); submit(2);
        Check(TakeCalls().empty(), "three lists held back");
        submit(3);
        calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {0, 1, 2, 3}, false), "fourth list: one submission of four, in order");

        submit(4); submit(5);
        FakeView none{nullptr, 0};
        hookPayloads(&rhi, &none);
        calls = TakeCalls();
        Check(calls.size() == 2 && is(calls[0], {4, 5}, false) && calls[1].payloads, "direct SubmitPayloads: held lists go first");

        payloads[7].rest[0x18 - 0x10] = 1;  // SyncPointsToWait.Num != 0: the engine would not merge it
        submit(6); submit(7);
        calls = TakeCalls();
        Check(calls.size() == 2 && is(calls[0], {6}, false) && is(calls[1], {7}, false), "unmergeable list: batch first, then alone");
        payloads[8].queue = &asyncCompute;
        submit(8);
        calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {8}, false), "async compute list: submitted alone");

        submit(9); submit(10, true);
        calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {9, 10}, true), "flush resources: one submission with the flush");
        hookLists(&rhi, &none, true);
        calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {}, true), "flush resources with nothing held: forwarded");

        SetRdgSubmitBatch(4, 2000);
        submit(11);
        Sleep(100);
        calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {11}, false), "a held list is submitted after the maximum age");

        SetRdgSubmitBatch(4, 20000);
        submit(12);
        SetRdgSubmitBatch(0, 20000);
        calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {12}, false), "switching off submits what was held");
        submit(13);
        calls = TakeCalls();
        Check(calls.size() == 1 && is(calls[0], {13}, false), "off again: straight through");
    }

    // ClothConfigCache: the cloth config copy in FClothSimulationProxy::Tick_GameThread's InitializeConfigs loop.
    // beforeGameCode = false: the call must be switched with one atomic write (as when loaded late).
    Check(ApplyClothConfigFix(false), "cloth config cache installed with an atomic call switch");
    ClothConfigInfo cloth{};
    Check(ClothConfigFixInfo(&cloth), "cloth config cache info");
    if (Installed(cloth.hook != nullptr && cloth.stub != nullptr, "ClothConfigCache (cloth config cache)")) {
        Check(cloth.callSite == game.base + 0x53CEEFE, "InitializeConfigs call at exe+0x53CEEFE");
        Check(cloth.initialize == game.base + 0x53B50A0, "FClothingSimulationConfig::Initialize at exe+0x53B50A0");
        Check(cloth.inputOffset == 0x920 && cloth.facadesOffset == 0x930, "component: PropertyCollections +0x920, facades +0x930");
        Check(cloth.configCollections == 0x08 && cloth.configProperties == 0x18, "config: PropertyCollections +0x08, Properties +0x18");
        if (cloth.stub) {
            int32_t rel;
            memcpy(&rel, cloth.callSite + 1, sizeof rel);
            void* jumpTo = nullptr;
            memcpy(&jumpTo, cloth.stub + 6, sizeof jumpTo);
            Check(cloth.callSite[0] == 0xE8 && cloth.callSite + 5 + rel == cloth.stub, "the call goes to the stub");
            Check(MatchesAt(cloth.callSite + 1, ParsePattern("9D 61")), "low 16 bits of the call unchanged (only two bytes switched)");
            Check(MatchesAt(cloth.stub, ParsePattern("FF 25 00 00 00 00")) && jumpTo == cloth.hook, "stub jumps to the hook");
        } else {
            Check(false, "call stub allocated");
        }
        Check(ApplyClothConfigFix(false), "second application finds the call already switched");
        {
            static FakeLod in[2], out[2];
            for (int i = 0; i < 2; ++i) {
                FillLod(in[i], i);
                FillLod(out[i], i);
            }
            FakeShared inShared[2] = {{&in[0].collection, nullptr}, {&in[1].collection, nullptr}};
            void* inFacades[2] = {&in[0].facade, &in[1].facade};
            FakeComponentArrays component{{inShared, 2, 2}, {inFacades, 2, 2}};
            FakeShared outShared[2] = {{&out[0].collection, nullptr}, {&out[1].collection, nullptr}};
            void* outFacades[2] = {&out[0].facade, &out[1].facade};
            FakeConfig config{0, {outShared, 2, 2}, {outFacades, 2, 2}};
            ClothConfigTestOriginal(reinterpret_cast<void*>(&FakeInitialize));
            auto hook = reinterpret_cast<void (*)(void*, const void*)>(cloth.hook);
            // true = skipped; false = the original was called with the same arguments. Each case is also
            // decided in verify mode (without and with the comparison's warm-up): they must agree.
            auto skipped = [&]() {
                ClothConfigTestDecide(&config, &component, 2);
                g_fakeInitCalls = 0;
                hook(&config, &component);
                if (g_fakeInitCalls == 1 && (g_fakeInitArgs[0] != &config || g_fakeInitArgs[1] != &component)) {
                    Check(false, "original called with the hook's arguments");
                }
                return g_fakeInitCalls == 0;
            };
            // Every "copied" case below leaves the fake config equal to its input again (as the real copy does).
            const ClothConfigStats s0 = GetClothConfigStats();
            Check(skipped(), "cloth: identical copy -> skipped");

            out[1].flags[2] |= 0x80;  // the copy still has Dirty, the component cleared it (ClearDirtyFlags)
            Check(!skipped(), "cloth: flags differ (Dirty cleared on the component) -> copied");
            Check(out[1].flags[2] == 1 && skipped(), "cloth: next frame, nothing changed -> skipped");
            in[1].flags[2] |= 0x80;   // dirty on both sides, otherwise identical: a copy would change nothing
            out[1].flags[2] |= 0x80;
            Check(skipped(), "cloth: identical dirty flags -> skipped");
            in[1].flags[2] = out[1].flags[2] = 1;

            in[0].low[4] += 1.0f;
            Check(!skipped(), "cloth: a LowValue differs -> copied");
            out[1].high[8] = -0.0f;   // +0 vs -0 compare equal as floats, but are different bits
            in[1].high[8] = 0.0f;
            Check(!skipped(), "cloth: HighValue +0 vs -0 -> copied (byte-exact)");
            in[1].keys[0] = {L"DampingCoefficienT", 19, 19};
            Check(!skipped(), "cloth: a key differs (same length) -> copied");
            out[0].strings[2] = {L"MaxDistanceMap2", 16, 16};
            Check(!skipped(), "cloth: a string value differs in length -> copied");
            // The byte compare works in 16-byte, then 8-byte, then 1-byte steps: a difference in each part.
            in[0].keys[0] = {L"dampingCoefficient", 19, 19};  // byte 0 (first 16-byte block)
            Check(!skipped(), "cloth: a key differs in its first byte -> copied");
            in[0].keys[1] = {L"WindVeloXity", 13, 13};        // bytes 16-17 (the 8-byte step of 26 bytes)
            Check(!skipped(), "cloth: a key differs in the 8-byte step -> copied");
            in[1].low[8] += 0.25f;                           // bytes 32-35 of 36 (the 1-byte tail)
            Check(!skipped(), "cloth: the last LowValue differs -> copied");
            Check(skipped(), "cloth: after the copies -> skipped again");

            // Strings longer than one cache line (the warm-up prefetches every line of them): equal, then the last character differs.
            static wchar_t kLongA[] = L"AVeryLongPropertyKeyThatSpansSeveralCacheLinesOfSixtyFourBytes_A";
            static wchar_t kLongB[] = L"AVeryLongPropertyKeyThatSpansSeveralCacheLinesOfSixtyFourBytes_A";
            static wchar_t kLongC[] = L"AVeryLongPropertyKeyThatSpansSeveralCacheLinesOfSixtyFourBytes_B";
            const int32_t longLen = static_cast<int32_t>(wcslen(kLongA) + 1);
            in[1].strings[1] = {kLongA, longLen, longLen};
            out[1].strings[1] = {kLongB, longLen, longLen};
            Check(skipped(), "cloth: equal string value of 130 bytes (separate buffers) -> skipped");
            in[1].strings[1] = {kLongC, longLen, longLen};
            Check(!skipped(), "cloth: a 130-byte string value differs in its last character -> copied");
            Check(skipped(), "cloth: after that copy -> skipped again");

            // The warm-up reads every LOD before the comparison does; a bad pointer in a LOD the comparison never
            // reaches (LOD 0 already differs in size) faults only the warm-up, which is ignored: same decision.
            {
                const ClothConfigStats w0 = GetClothConfigStats();
                void* const saved = outFacades[1];
                outFacades[1] = reinterpret_cast<void*>(uintptr_t{0x10});
                in[0].facade.key.num = in[0].facade.low.num = in[0].facade.high.num = in[0].facade.str.num = in[0].facade.flags.num = 2;
                const int cold = ClothConfigTestDecide(&config, &component, 0);
                const int warm = ClothConfigTestDecide(&config, &component, 1);
                const ClothConfigStats w1 = GetClothConfigStats();
                Check(cold != 0 && warm == cold && w1.warmFaults - w0.warmFaults == 1 && w1.faults == w0.faults,
                      "cloth: warm-up fault beyond the first difference -> ignored, same decision, fix stays on");
                in[0].facade.key.num = in[0].facade.low.num = in[0].facade.high.num = in[0].facade.str.num = in[0].facade.flags.num = 3;
                outFacades[1] = saved;
            }

            config.collections.num = config.properties.num = 1;
            Check(!skipped(), "cloth: config has fewer LODs -> copied");
            out[0].facade.key.num = out[0].facade.low.num = out[0].facade.high.num = out[0].facade.str.num = out[0].facade.flags.num = 2;
            Check(!skipped(), "cloth: config has fewer properties -> copied");
            in[0].facade.collection = &out[0].collection;
            Check(!skipped(), "cloth: a facade views another collection -> copied");
            in[0].facade.collection = &in[0].collection;
            out[1].facade.flags.num = 2;
            Check(!skipped(), "cloth: inconsistent view sizes -> copied");
            component.facades.num = 1;
            Check(!skipped(), "cloth: component facades not parallel to its collections -> copied");
            component.facades.num = 2;

            SetClothConfigCacheEnabled(false);
            Check(!skipped(), "cloth: switched off -> copied");
            SetClothConfigCacheEnabled(true);
            Check(skipped(), "cloth: switched on again -> skipped");

            FakeComponentArrays none{{nullptr, 0, 0}, {nullptr, 0, 0}};
            FakeConfig empty{0, {nullptr, 0, 0}, {nullptr, 0, 0}};
            g_fakeInitCalls = 0;
            hook(&empty, &none);
            Check(g_fakeInitCalls == 0, "cloth: no LODs on either side -> skipped");

            const ClothConfigStats s1 = GetClothConfigStats();
            Check(s1.calls - s0.calls == 23 && s1.skipped - s0.skipped == 8 && s1.flags - s0.flags == 1 &&
                      s1.values - s0.values == 8 && s1.shape - s0.shape == 2 && s1.unverified - s0.unverified == 3 &&
                      s1.disabled - s0.disabled == 1 && s1.faults == 0 && s1.checked - s0.checked == 11 && s1.checkFailed == 0,
                  "cloth: statistics count every outcome");
            Check(s1.verified - s0.verified == 22 && s1.verifyDiffs == 0, "cloth: every case decided alike without and with the warm-up");
            LogClothConfigStats();

            // A "copy" that does not reproduce its input breaks what the skip relies on: the fix must switch off.
            g_fakeInitCopies = false;
            in[0].low[0] += 1.0f;
            Check(!skipped(), "cloth: copy that leaves a difference -> copied");
            in[0].low[0] -= 1.0f;  // equal again, but the fix is now off for good
            Check(!skipped(), "cloth: after a failed re-check the fix stays off -> copied");
            const ClothConfigStats s2 = GetClothConfigStats();
            Check(s2.checkFailed == 1 && s2.disabled - s1.disabled == 1, "cloth: failed re-check counted, fix off");
            LogClothConfigStats();
        }
    }

    // The fade-to-game hook PrepareCollisionBehindLoadingScreen runs from. Sites of build 19115871, then the pass-through with fakes.
    Check(InstallFadeHook(), "fade hook: installed");
    {
        FadeHookSites u{};
        Check(FadeHookInfo(&u), "fade hook: info");
        if (Installed(u.execSlot != nullptr && u.execHook != nullptr && u.execIfSafeHook != nullptr,
                      "the fade-to-game hook (PrepareCollisionBehindLoadingScreen)")) {
            Check(u.fadeToGame == game.base + 0x4877E80, "fade hook: fade-to-game at exe+0x4877E80");
            Check(u.unfreeze == game.base + 0x4881640, "fade hook: game's unfreeze timer body at exe+0x4881640");
            Check(u.exec == game.base + 0x4881680 && u.execIfSafe == game.base + 0x4881830, "fade hook: delegate thunks exe+0x4881680 / exe+0x4881830");
            Check(u.flagOffset == 0x200 && u.flag2Offset == 0x1F1 && u.timerOffset == 0x240 && u.captureOffset == 0x18,
                  "fade hook: offsets +0x200 / +0x1F1 / gate timer +0x240 / capture +0x18");
            Check(u.modeOffset == 0x3C && u.stackNumOffset == 0xB0, "fade hook: freeze subsystem mode +0x3C, freeze stack Num +0xB0");
            Check(u.getWorldSlot == 0x188, "fade hook: GetWorld vtable slot +0x188");
            Check(u.execSlot && *u.execSlot == u.execHook && u.execIfSafeSlot == u.execSlot + 1 && *u.execIfSafeSlot == u.execIfSafeHook,
                  "fade hook: both delegate slots point at the hooks");
            Check(InstallFadeHook(), "fade hook: second installation is a no-op");

            // With PrepareCollisionBehindLoadingScreen off the hook only calls the game's fade-to-game (fake thunks count the calls).
            static int fades = 0;
            static uint8_t fakeLcd[0x400];
            static uint8_t fakeFreeze[0x100];
            struct FakeDelegate { uint8_t pad[0x18]; uint8_t* lcd; };
            FadeHookTestFunctions(
                reinterpret_cast<void*>(+[](void*) { ++fades; }),
                reinterpret_cast<void*>(+[](void*) -> bool { ++fades; return true; }),
                reinterpret_cast<void*>(+[](void*) -> void* { return fakeFreeze; }));
            auto exec = reinterpret_cast<void (*)(void*)>(u.execHook);
            auto execSafe = reinterpret_cast<bool (*)(void*)>(u.execIfSafeHook);
            FakeDelegate d{{}, fakeLcd};
            fakeLcd[0x200] = 1;
            *reinterpret_cast<uint64_t*>(fakeLcd + 0x240) = 0x1234;
            *reinterpret_cast<int32_t*>(fakeFreeze + 0x3C) = 1;
            SetCollisionPrepareEnabled(false);
            exec(&d);
            Check(fades == 1 && fakeLcd[0x200] == 1, "fade hook: Execute -> the game's fade-to-game, freeze flag untouched");
            Check(execSafe(&d) && fades == 2, "fade hook: ExecuteIfSafe -> the game's fade-to-game, result passed back");
            d.lcd = nullptr;
            exec(&d);
            Check(fades == 3, "fade hook: no VLevelChangeData -> the game's fade-to-game");
        }
    }

    // PrepareCollisionBehindLoadingScreen: hand a transition's new area to physics behind the loading screen. Sites of build 19115871, then the
    // decision with fakes (through the fade-to-game hook installed above).
    Check(FindCollisionPrepare(), "collision: game code found");
    {
        CollisionPrepareFixInfo c{};
        Check(CollisionPrepareInfo(&c), "collision: info");
        const bool fadeHooked = [] { FadeHookSites f{}; return FadeHookInfo(&f) && f.execHook && f.execIfSafeHook; }();
        if (Installed(c.startFrame != nullptr && fadeHooked, "PrepareCollisionBehindLoadingScreen (collision, needs the fade hook)")) {
            Check(c.dispatch == game.base + 0x1883DB0, "collision: AdvanceAndDispatch_External at exe+0x1883DB0");
            Check(c.copy == game.base + 0x1A9FF90, "collision: CopySolverAccelerationStructure at exe+0x1A9FF90");
            Check(c.recycle == game.base + 0xDD03A0, "collision: graph event recycle at exe+0xDD03A0");
            Check(c.startFrame == game.base + 0x1AA6510 && c.endFrame == game.base + 0x1AA1110, "collision: FChaosScene StartFrame / EndFrame");
            Check(c.doWork == game.base + 0x1A28590 && c.cisa == game.base + 0x192CC60,
                  "collision: DoWork exe+0x1A28590, zero-dt ComputeIntermediateSpatialAcceleration exe+0x192CC60");
            Check(c.setupPhysics == game.base + 0x3593670 && c.freezeApply == game.base + 0x4958988,
                  "collision: SetupPhysicsTickFunctions exe+0x3593670, freeze apply exe+0x4958988");
            Check(c.simEnable == game.base + 0x9144F00 && c.forceDisableAsync == reinterpret_cast<const int32_t*>(game.base + 0x9142FD0),
                  "collision: P.Chaos.Simulation.Enable ref exe+0x9144F00, ForceDisableAsyncPhysics exe+0x9142FD0");
            Check(c.sceneSolver == 0x38 && c.asyncDt == 0x210 && c.rewindCallback == 0x1B8 && c.rewindData == 0x1B0 &&
                      c.shuttingDown == 0x208 && c.evolution == 0x930,
                  "collision: scene solver +0x38; solver AsyncDt +0x210, rewind +0x1B8/+0x1B0, shutting down +0x208, evolution +0x930");
            Check(c.worldPhysicsByte == 0x13C && c.worldPhysicsBit == 2 && c.worldScene == 0x228,
                  "collision: world physics switch +0x13C bit 2 (= the freeze's), PhysicsScene +0x228");
            Check(c.refCount == 0x38 && c.completeShift == 26, "collision: graph event refcount +0x38, completion bit 26");

            // Fakes laid out with the offsets above.
            static uint8_t world[0x400], scene[0x100], solver[0x400], freeze[0x100], lcd[0x400];
            alignas(16) static uint64_t ev[16];
            static void* lcdVtable[0x60];
            static int32_t simEnable = 1, forceDisable = 0;
            static int dispatches = 0, copies = 0, recycles = 0, fades = 0;
            static void* dispatchedSolver = nullptr;
            static double dispatchedDt = -1;
            static void* copiedScene = nullptr;
            static bool swapSceneDuringFade = false;
            struct FakeDelegate { uint8_t pad[0x18]; uint8_t* lcd; };
            lcdVtable[0x188 / 8] = reinterpret_cast<void*>(+[](void*) -> void* { return world; });
            auto fakeFade = [] {
                ++fades;
                if (swapSceneDuringFade) *reinterpret_cast<void**>(world + 0x228) = nullptr;
            };
            static void (*fadeFn)() = fakeFade;
            FadeHookTestFunctions(
                reinterpret_cast<void*>(+[](void*) { fadeFn(); }),
                reinterpret_cast<void*>(+[](void*) -> bool { fadeFn(); return true; }),
                reinterpret_cast<void*>(+[](void*) -> void* { return freeze; }));
            CollisionPrepareTestFunctions(
                reinterpret_cast<void*>(+[](void* s, void** out, double dt) -> void** {
                    ++dispatches;
                    dispatchedSolver = s;
                    dispatchedDt = dt;
                    InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<uint8_t*>(ev) + 0x38));
                    *out = ev;
                    return out;
                }),
                reinterpret_cast<void*>(+[](void* s) { ++copies; copiedScene = s; }),
                reinterpret_cast<void*>(+[](void*) { ++recycles; }),
                &simEnable, &forceDisable);
            auto exec = reinterpret_cast<void (*)(void*)>([] { FadeHookSites u{}; FadeHookInfo(&u); return u.execHook; }());
            auto execSafe = reinterpret_cast<bool (*)(void*)>([] { FadeHookSites u{}; FadeHookInfo(&u); return u.execIfSafeHook; }());
            FakeDelegate d{{}, lcd};
            // A frozen door/gate/fast-travel transition: loading mode, gate timer, freeze mode 1 without named freezes,
            // physics off, a synchronous solver without rewind; the solver's work finishes during the fade-to-game.
            auto reset = [&](bool complete = true) {
                memset(world, 0, sizeof world);
                memset(scene, 0, sizeof scene);
                memset(solver, 0, sizeof solver);
                memset(lcd, 0, sizeof lcd);
                memset(ev, 0, sizeof ev);
                *reinterpret_cast<void***>(lcd) = lcdVtable;
                lcd[0x200] = 1;
                lcd[0x1F1] = 1;
                *reinterpret_cast<uint64_t*>(lcd + 0x240) = 0x1234;
                *reinterpret_cast<int32_t*>(freeze + 0x3C) = 1;
                *reinterpret_cast<int32_t*>(freeze + 0xB0) = 0;
                *reinterpret_cast<void**>(world + 0x228) = scene;
                *reinterpret_cast<void**>(scene + 0x38) = solver;
                *reinterpret_cast<double*>(solver + 0x210) = -1.0;
                ev[0] = complete ? (1ull << 26) : 0;
                *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ev) + 0x38) = 1;  // the solver's own reference
                simEnable = 1;
                forceDisable = 0;
                swapSceneDuringFade = false;
                dispatches = copies = recycles = fades = 0;
                dispatchedSolver = nullptr;
                dispatchedDt = -1;
                copiedScene = nullptr;
            };
            auto refs = [] { return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ev) + 0x38); };
            SetCollisionPrepareEnabled(false);
            reset();
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: switched off -> fade-in as before, nothing dispatched");
            SetCollisionPrepareEnabled(true);
            Check(CollisionPrepareEnabled(), "collision: switched on");
            reset();
            exec(&d);
            Check(dispatches == 1 && dispatchedSolver == solver && dispatchedDt == 0.0 && fades == 1,
                  "collision: frozen transition -> zero-time solver tick, then the game's fade-to-game");
            Check(copies == 1 && copiedScene == scene && refs() == 1 && recycles == 0,
                  "collision: finished -> scene queries updated, our event reference released");
            Check(lcd[0x200] == 1, "collision: the transition's freeze flag untouched");
            reset(false);
            Check(execSafe(&d) && dispatches == 1 && fades == 1 && copies == 0 && refs() == 1,
                  "collision: still running after the fade-to-game -> no scene-query update, reference released (ExecuteIfSafe)");
            reset();
            swapSceneDuringFade = true;
            exec(&d);
            Check(dispatches == 1 && fades == 1 && copies == 0, "collision: physics scene gone meanwhile -> nothing updated");
            reset();
            world[0x13C] |= 4;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: physics already on -> left to the game");
            reset();
            *reinterpret_cast<void**>(world + 0x228) = nullptr;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: no physics scene -> left to the game");
            reset();
            *reinterpret_cast<void**>(scene + 0x38) = nullptr;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: no solver -> left to the game");
            reset();
            *reinterpret_cast<double*>(solver + 0x210) = 1.0 / 60;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: asynchronous physics (fixed step) -> left to the game");
            reset();
            *reinterpret_cast<double*>(solver + 0x210) = 1.0 / 60;
            forceDisable = 1;
            exec(&d);
            Check(dispatches == 1, "collision: async step configured but p.ForceDisableAsyncPhysics -> synchronous -> prepared");
            reset();
            *reinterpret_cast<void**>(solver + 0x1B0) = solver;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: rewind data -> left to the game");
            reset();
            *reinterpret_cast<void**>(solver + 0x1B8) = solver;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: rewind callback -> left to the game");
            reset();
            solver[0x208] = 1;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: solver shutting down -> left to the game");
            reset();
            simEnable = 0;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: P.Chaos.Simulation.Enable 0 -> left to the game");
            reset();
            lcd[0x200] = 0;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: transition not in loading mode (interiors) -> left to the game");
            reset();
            *reinterpret_cast<uint64_t*>(lcd + 0x240) = 0;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: no fade gate timer -> left to the game");
            reset();
            *reinterpret_cast<int32_t*>(freeze + 0xB0) = 1;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: a menu freeze on the stack (main menu) -> left to the game");
            reset();
            *reinterpret_cast<int32_t*>(freeze + 0x3C) = 0;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: freeze subsystem not in loading mode -> left to the game");
            reset();
            d.lcd = nullptr;
            exec(&d);
            Check(dispatches == 0 && fades == 1, "collision: no VLevelChangeData -> left to the game");
            d.lcd = lcd;
            LogCollisionPrepareStats();
            SetCollisionPrepareEnabled(false);
        }
    }

    // NameLookupCrashGuard: crash guard for the old engine's name table (editor ID -> form).
    {
        Check(ApplyEditorIdLookupGuard(), "name table crash guard installed");
        EditorIdGuardInfo eg{};
        Check(EditorIdLookupGuardInfo(&eg), "name table guard info");
        if (Installed(eg.vtableSlot != nullptr && eg.hook != nullptr, "NameLookupCrashGuard (name table guard)")) {
            Check(eg.map == game.base + 0x8FAB580, "name table at exe+0x8FAB580");
            Check(eg.vtableSlot == reinterpret_cast<void**>(game.base + 0x861EB18) + 2, "compare slot = vtable exe+0x861EB18 slot 2");
            Check(eg.original == game.base + 0x660FDE0, "the game's compare at exe+0x660FDE0");
            Check(eg.stricmpImport == reinterpret_cast<void* const*>(game.base + 0x6D98228), "_stricmp import slot exe+0x6D98228");
            Check(eg.vtableSlot && *eg.vtableSlot == eg.hook, "compare slot points at the guard");
            Check(ApplyEditorIdLookupGuard(), "second installation finds the guard in place");

            // The mapped exe's imports are not resolved: let the guard call this CRT's _stricmp instead.
            static void* const crtStricmp = reinterpret_cast<void*>(&_stricmp);
            EditorIdLookupGuardTestImport(&crtStricmp);
            using CompareFn = bool (*)(void*, const char*, const char*);
            const auto cmp = reinterpret_cast<CompareFn>(*eg.vtableSlot);
            Check(cmp(eg.map, "Tamriel", "tamriel") && cmp(eg.map, "CStoneMedium", "CSTONEMEDIUM"), "same names (any case) match");
            Check(!cmp(eg.map, "Tamriel", "Weye") && !cmp(eg.map, "Tamriel", "Tamriel2") && !cmp(eg.map, "Tamriel", ""),
                  "different names do not match");

            // Freed keys: one on a page given back to Windows, one whose string runs into such a page.
            auto* region = static_cast<char*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            DWORD old = 0;
            Check(region && VirtualFree(region + 0x1000, 0x1000, MEM_DECOMMIT), "test pages set up");
            memcpy(region + 0x1000 - 4, "Tamr", 4);  // no terminator before the released page
            const uint64_t faults0 = eg.faults;
            Check(!cmp(eg.map, "Tamriel", region + 0x1000), "key on a released page: no match, no crash");
            Check(!cmp(eg.map, "Tamriel", region + 0x1000 - 4), "key running into a released page: no match, no crash");
            EditorIdLookupGuardInfo(&eg);
            Check(eg.faults == faults0 + 2, "both faults counted");
            LogEditorIdLookupGuardStats();

            // The filter handles only a read fault inside the stored key; anything else crashes as before.
            EXCEPTION_RECORD r{};
            r.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
            r.NumberParameters = 2;
            const char* key = region + 0x1000 - 4;
            r.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(region + 0x1000);
            Check(EditorIdGuardFilter(&r, key) == EXCEPTION_EXECUTE_HANDLER, "filter: read fault inside the key -> handled");
            r.ExceptionInformation[1] = 0x10;
            Check(EditorIdGuardFilter(&r, key) == EXCEPTION_CONTINUE_SEARCH, "filter: read fault elsewhere (bad name pointer) -> not handled");
            r.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(key) + 0x3000;
            Check(EditorIdGuardFilter(&r, key) == EXCEPTION_CONTINUE_SEARCH, "filter: read fault far past the key -> not handled");
            r.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(region + 0x1000);
            r.ExceptionInformation[0] = 1;
            Check(EditorIdGuardFilter(&r, key) == EXCEPTION_CONTINUE_SEARCH, "filter: write fault -> not handled");
            r.ExceptionInformation[0] = 0;
            r.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
            Check(EditorIdGuardFilter(&r, key) == EXCEPTION_CONTINUE_SEARCH, "filter: other exception -> not handled");
            (void)old;
            VirtualFree(region, 0, MEM_RELEASE);
        }
    }

    // TrapCrashGuard: crash guard for the old engine's trap listener (an actor destroyed while still in a trap's list).
    {
        Check(ApplyTrapTargetGuard(), "trap listener crash guard installed");
        TrapGuardInfo tg{};
        Check(TrapTargetGuardInfo(&tg), "trap guard info");
        if (Installed(tg.slot != nullptr && tg.hook != nullptr, "TrapCrashGuard (trap listener guard)")) {
            Check(tg.update == game.base + 0x66D67E0, "TESTrapListener update at exe+0x66D67E0");
            Check(tg.remove == game.base + 0x66D6560, "actor removal at exe+0x66D6560");
            Check(tg.slot == reinterpret_cast<void**>(game.base + 0x8633E80) + 5, "update slot = vtable exe+0x8633E80 slot 5");
            Check(tg.nodes == 0x28 && tg.actors == 0x20 && tg.target == 0x18 && tg.next == 0x8 && tg.nodeNext == 0x8,
                  "layout: nodes +0x28, actors +0x20, actor +0x18, next +0x8, next node +0x8");
            Check(tg.hitSlot == 0x250, "trap-hit slot +0x250");
            Check(*tg.slot == tg.hook, "update slot points at the guard");
            Check(ApplyTrapTargetGuard(), "second installation finds the guard in place");

            // The liveness check: the first qword must be the vtable of a class derived from Actor (RTTI).
            auto object = [&](uint64_t vtableRva) {
                static uintptr_t storage[16][4];
                static int used = 0;
                uintptr_t* o = storage[used++ % 16];
                o[0] = reinterpret_cast<uintptr_t>(game.base + vtableRva);
                return static_cast<void*>(o);
            };
            Check(TrapTargetGuardTestIsActor(object(0x8624380)), "a Creature is an actor");
            Check(TrapTargetGuardTestIsActor(object(0x8649520)), "a Character is an actor");
            Check(TrapTargetGuardTestIsActor(object(0x861CBA0)), "the PlayerCharacter is an actor");
            Check(TrapTargetGuardTestIsActor(object(0x8624380)), "a Creature again (cached answer)");
            Check(!TrapTargetGuardTestIsActor(object(0x86157E0)), "a plain TESObjectREFR is not an actor");
            Check(!TrapTargetGuardTestIsActor(object(0x861BC80)), "a MobileObject is not an actor");
            Check(!TrapTargetGuardTestIsActor(object(0x8633E80)), "the trap listener itself is not an actor");
            static uintptr_t freedBlock[4] = {0x00000001E3140380ull};  // the 2026-10-12 crash: Unreal's free-list header
            Check(!TrapTargetGuardTestIsActor(freedBlock), "a freed block (FMallocBinned2 header) is not an actor");
            static uintptr_t heapLink[4];
            heapLink[0] = reinterpret_cast<uintptr_t>(&heapLink[2]);
            Check(!TrapTargetGuardTestIsActor(heapLink), "a heap link is not an actor");
            auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS));
            Check(page && !TrapTargetGuardTestIsActor(page), "memory given back to Windows: not an actor, no crash");

            // The update through the guard, with the game's update and removal replaced by fakes.
            struct Entry { uint32_t flags, pad; Entry* next; void* unused; void* actor; uint8_t rest[0x40]; };
            struct Node { uint64_t a; Node* next; void* trapCollidable; void* trapRef; Entry* actors; };
            struct Listener { uint8_t head[0x28]; Node* nodes; void* phantoms; };
            static Listener listener{};
            static Node node1{}, node2{};
            static Entry e1{}, e2{}, e3{}, e4{};
            static uintptr_t trapRef[4] = {0, 0, 0x0001ABCD};  // form ID at +0x10
            static int updates = 0, removals = 0;
            static float lastDt = 0;
            static void* removedActors[8];
            struct Fakes {
                static void Update(void* l, float dt)
                {
                    if (l == &listener) ++updates;
                    lastDt = dt;
                }
                // As the game's: unlink the first entry of this actor; drop the node from the listener when empty.
                static void Remove(void* l, void* n, void* actor)
                {
                    auto* L = static_cast<Listener*>(l);
                    auto* N = static_cast<Node*>(n);
                    for (Entry** p = &N->actors; *p; p = &(*p)->next)
                        if ((*p)->actor == actor) { *p = (*p)->next; break; }
                    if (removals < 8) removedActors[removals] = actor;
                    ++removals;
                    if (!N->actors) {
                        for (Node** p = &L->nodes; *p; p = &(*p)->next)
                            if (*p == N) { *p = N->next; break; }
                        N->next = nullptr;  // as if freed: the guard must have read the next node already
                    }
                }
            };
            void* const creature = object(0x8624380);
            void* const character = object(0x8649520);
            // node1: a live creature, the freed block, the released page; node2: only a dead entry (the node goes).
            e1 = {}; e1.actor = creature; e1.next = &e2;
            e2 = {}; e2.actor = freedBlock; e2.next = &e3;
            e3 = {}; e3.actor = page; e3.next = nullptr;
            e4 = {}; e4.actor = nullptr; e4.next = nullptr;
            node1 = {}; node1.trapRef = trapRef; node1.actors = &e1; node1.next = &node2;
            node2 = {}; node2.trapRef = trapRef; node2.actors = &e4; node2.next = nullptr;
            listener = {}; listener.nodes = &node1;
            TrapTargetGuardTestFunctions(reinterpret_cast<void*>(&Fakes::Update), reinterpret_cast<void*>(&Fakes::Remove));
            using UpdateFn = void (*)(void*, float);
            const auto guarded = reinterpret_cast<UpdateFn>(*tg.slot);
            SetTrapTargetGuardEnabled(false);
            guarded(&listener, 0.0125f);
            Check(updates == 1 && removals == 0 && lastDt == 0.0125f, "switched off: the game's update only, nothing removed");
            SetTrapTargetGuardEnabled(true);
            guarded(&listener, 0.016f);
            Check(updates == 2 && lastDt == 0.016f, "the game's update runs after the check, with the same time step");
            Check(removals == 3 && removedActors[0] == freedBlock && removedActors[1] == page && removedActors[2] == nullptr,
                  "the freed actor, the released page and the empty actor removed, in list order");
            Check(listener.nodes == &node1 && node1.actors == &e1 && !e1.next, "the live creature stays in its trap's list");
            Check(!node1.next, "the emptied trap node is dropped (the guard read the next node before the removal)");
            e1.next = &e2; e2.next = nullptr; e2.actor = character;
            guarded(&listener, 0.016f);
            Check(removals == 3 && updates == 3, "live actors only: nothing removed");
            TrapTargetGuardInfo(&tg);
            Check(tg.removed == 3 && tg.checked == 6, "removals and checks counted (switched off: nothing checked)");
            LogTrapTargetGuardStats();
            if (page) VirtualFree(page, 0, MEM_RELEASE);
        }
    }

    // BowSneakAttackFix: a player's arrow hits the old engine's hit function once, the old engine's own impact with the
    // Unreal side's sneak multiplier instead of its fixed 1.0.
    {
        Check(ApplyBowSneakAttackFix(), "bow sneak fix installed");
        BowSneakInfo bi{};
        Check(BowSneakAttackFixInfo(&bi), "bow sneak fix info");
        if (Installed(bi.stub != nullptr, "BowSneakAttackFix (bow sneak attacks)")) {
            Check(bi.queueCall == game.base + 0x65574B0, "the arrow hit handler's call at exe+0x65574B0");
            Check(bi.impactCall == game.base + 0x6665A43, "the old engine's impact call at exe+0x6665A43");
            Check(bi.thunk == game.base + 0x65A5200, "the hit thunk at exe+0x65A5200");
            Check(bi.player == reinterpret_cast<void* const*>(game.base + 0x941F708), "the player pointer at exe+0x941F708");
            Check(bi.queueCall[0] == 0xE8 && RipTarget(bi.queueCall, 1, 5) == bi.stub, "handler's call goes to the stub");
            Check(bi.impactCall[0] == 0xE8 && RipTarget(bi.impactCall, 1, 5) == bi.stub + 16 && bi.impactCall[5] == 0x66 &&
                      bi.impactCall[6] == 0x90,
                  "impact's call [r10+868h] is now call stub; nop");
            Check(*reinterpret_cast<void* const*>(bi.stub + 6) == bi.queueHook &&
                      *reinterpret_cast<void* const*>(bi.stub + 22) == bi.impactHook,
                  "stubs jump to the two hooks");
            Check(ApplyBowSneakAttackFix(), "second installation finds the fix in place");

            // The hooks with fakes: a fake player and target (vtables with the hit slot), arrows, a clock.
            using HitFn = uint64_t (*)(void*, uint64_t, void*, void*, uint64_t, float, uint64_t);
            static int thunkCalls = 0, slotCalls = 0;
            static float lastMult = 0;
            static uint64_t clockMs = 1000;
            struct Fakes {
                static uint64_t Thunk(void*, uint64_t, void*, void*, uint64_t, float mult, uint64_t)
                {
                    ++thunkCalls;
                    lastMult = mult;
                    return 1;
                }
                static uint64_t Slot(void*, uint64_t, void*, void*, uint64_t, float mult, uint64_t)
                {
                    ++slotCalls;
                    lastMult = mult;
                    return 2;
                }
                static uint64_t Clock() { return clockMs; }
            };
            static void* actorVtable[0x868 / 8 + 1] = {};
            actorVtable[0x868 / 8] = reinterpret_cast<void*>(&Fakes::Slot);
            static void* arrowVtable[1] = {};
            static void* magicVtable[1] = {};
            struct Obj { void** vtable; };
            static Obj player{actorVtable}, npc{actorVtable}, target{actorVtable};
            static Obj arrowA{arrowVtable}, arrowB{arrowVtable}, arrowC{arrowVtable}, magic{magicVtable};
            static void* playerPtr = &player;
            BowSneakAttackFixTestSetup(reinterpret_cast<void*>(&Fakes::Thunk), &playerPtr, &Fakes::Clock);
            const auto queue = reinterpret_cast<HitFn>(bi.queueHook);
            const auto impact = reinterpret_cast<HitFn>(bi.impactHook);
            Check(impact(&player, 0, &arrowA, &target, 0, 1.0f, 0) == 2 && slotCalls == 1 && lastMult == 1.0f,
                  "old engine's hit before any Unreal-side value: made at 1.0 (stock)");
            clockMs += 1000;
            Check(queue(&player, 0, &arrowB, &target, 3, 2.0f, 0) == 1 && thunkCalls == 1 && lastMult == 2.0f,
                  "Unreal side's hit: made with its multiplier");
            clockMs += 2000;
            Check(impact(&player, 0, &arrowC, &target, 0, 1.0f, 0) == 2 && slotCalls == 2 && lastMult == 2.0f,
                  "old engine's hit first: made with the Unreal side's multiplier (2.0, not 1.0)");
            clockMs += 8;
            Check(queue(&player, 0, &arrowC, &target, 3, 2.0f, 0) == 0 && thunkCalls == 1,
                  "the same arrow's Unreal-side hit 8 ms later: not made (one hit per arrow)");
            clockMs += 2000;
            Check(queue(&player, 0, &arrowA, &target, 3, 3.0f, 0) == 1 && thunkCalls == 2 && lastMult == 3.0f,
                  "Unreal side first (multiplier now 3.0): made");
            Check(impact(&player, 0, &arrowA, &target, 0, 1.0f, 0) == 0 && slotCalls == 2,
                  "the same arrow's old-engine hit: not made");
            clockMs += 240;
            Check(impact(&player, 0, &arrowA, &target, 0, 1.0f, 0) == 0 && slotCalls == 2,
                  "the same arrow and target 240 ms later: still not made");
            clockMs += 20;
            Check(impact(&player, 0, &arrowA, &target, 0, 1.0f, 0) == 2 && slotCalls == 3 && lastMult == 3.0f,
                  "the same arrow and target after 250 ms: a new hit, with the latest multiplier");
            Check(impact(&player, 0, &magic, &target, 0, 1.0f, 0) == 2 && slotCalls == 4 && lastMult == 1.0f,
                  "another projectile class: its 1.0 kept");
            Check(impact(&npc, 0, &arrowB, &target, 0, 1.0f, 0) == 2 && slotCalls == 5 && lastMult == 1.0f &&
                      queue(&npc, 0, &arrowB, &target, 3, 2.0f, 0) == 1 && thunkCalls == 3,
                  "another attacker: both hits made as in the stock game");
            SetBowSneakAttackFixEnabled(false);
            Check(impact(&player, 0, &arrowB, &target, 0, 1.0f, 0) == 2 && slotCalls == 6 && lastMult == 1.0f &&
                      queue(&player, 0, &arrowB, &target, 3, 2.0f, 0) == 1 && thunkCalls == 4,
                  "switched off: both hits, the old engine's at 1.0, as in the stock game");
            SetBowSneakAttackFixEnabled(true);
            BowSneakAttackFixInfo(&bi);
            Check(bi.skipped == 3 && bi.corrected == 2 && bi.oneOnly == 2, "second hits (one of them 240 ms later), corrected and 1.0 hits counted");
            LogBowSneakAttackFixStats();
            BowSneakAttackFixTestSetup(nullptr, nullptr, nullptr);
        }
    }

    // StuckEffectLookFix (game bug #48): the record / pass rules on fakes, then the exec functions on the mapped exe and the
    // entry detour of SetOverlayMaterial's exec on the private copy.
    {
        const int logic = StuckEffectLookTestLogic();
        if (logic) printf("  StuckEffectLookFix: check %d failed\n", logic);
        Check(logic == 0, "StuckEffectLookFix: leftover on a quiver cleared (and its static-mesh flags), nothing while the effect shows, "
                          "the sword's own look put back, a foreign base gives none, other setters and unseen changes left alone, "
                          "a dismounted rider and a gone component, off and destroyed meshes untouched, the horse's look kept "
                          "only while the rider is attached, an ended look skipped for a deeper one, a gone top component, "
                          "history overflow, table full, the hook's off / off-game-thread / in-pass gates, the name and "
                          "offset lookup (and a missing name: does nothing)");
        StuckEffectLookInfo si{};
        const bool found = FindStuckEffectLook(&si);
        Check(found, "StuckEffectLookFix: SetOverlayMaterial / SetVisibleInRayTracing exec functions, FName, GUObjectArray found");
        if (found && game.timeDateStamp == 0xF19077A4u) {
            Check(si.exec - game.base == 0x3054490 && si.rtExec - game.base == 0x2D89C10,
                  "StuckEffectLookFix: the exec functions at exe+0x3054490 / exe+0x2D89C10 (build 19115871)");
            Check(si.overlayOffset == 0x5B0 && si.precacheSlot == 0x640, "StuckEffectLookFix: OverlayMaterial +0x5B0, PrecachePSOs slot +0x640");
        }
        if (Installed(found && InstallStuckEffectLook(true), "StuckEffectLookFix (stuck effect look)")) {
            Check(si.exec[0] == 0xE9, "StuckEffectLookFix: SetOverlayMaterial's exec starts with the detour's jmp");
            Check(InstallStuckEffectLook(true), "StuckEffectLookFix: a second install finds it in place");
            SetStuckEffectLookFix(false);
        }
    }

    // WorldSoundPositionFix (GitHub #52): the bank walk on a synthetic bank laid out like the game's (our own bytes: a bank
    // header, a HIRC chunk with listed and unlisted objects), then the 8 exported Wwise bank loaders on the mapped exe and
    // their entry detours on the private copy, and one hook call with a fake original (writable and read-only bank).
    {
        std::vector<uint8_t> hirc;
        auto u8 = [&](uint8_t v) { hirc.push_back(v); };
        auto u32 = [&](uint32_t v) {
            for (int k = 0; k < 4; ++k) hirc.push_back(static_cast<uint8_t>(v >> (8 * k)));
        };
        size_t bitsAt[8] = {};
        int nobj = 0;
        uint32_t plugin = 0x00040001;  // a sound object's source plugin (type 2 = a source plugin with a parameter block)
        uint32_t paramLen = 0;
        // one HIRC object: type, size, id, [sound source], node base up to uBits3d, then `tail` filler bytes
        auto object = [&](uint8_t type, uint32_t id, bool sound, int nfx, uint32_t parent, uint32_t att, uint8_t posBits, uint8_t bits3d,
                          int tail) -> size_t {
            u8(type);
            const size_t sizeAt = hirc.size();
            u32(0);
            u32(id);
            if (sound) {
                u32(plugin);
                u8(0);
                u32(0x1234);
                u32(100);
                u8(0);
                if ((plugin & 0xF) == 2) {
                    u32(paramLen);
                    for (uint32_t k = 0; k < paramLen; ++k) u8(0x55);
                }
            }
            u8(nfx ? 1 : 0);
            u8(static_cast<uint8_t>(nfx));
            if (nfx) {
                u8(0);
                for (int k = 0; k < nfx; ++k) {
                    u8(static_cast<uint8_t>(k));
                    u32(0x29DCEA45);
                    u8(0);
                }
            }
            u8(0);  // metadata
            u8(0);
            u32(0);  // bus
            u32(parent);
            u8(0);  // priority bits
            u8(2);  // props: volume, attenuation
            u8(0x00);
            u8(0x55);
            u32(0xC0C00000);
            u32(att);
            u8(0);  // ranges
            u8(posBits);
            const size_t at = hirc.size();
            u8(bits3d);
            for (int k = 0; k < tail; ++k) u8(static_cast<uint8_t>(0xA0 + k));
            const uint32_t osz = static_cast<uint32_t>(hirc.size() - sizeAt - 4);
            memcpy(hirc.data() + sizeAt, &osz, 4);
            ++nobj;
            return at;
        };
        u32(0);                                                                                 // object count, set below
        bitsAt[0] = object(5, 0x0BCA8316, false, 0, 0x0E0B72D9, 0x15942315, 0x03, 0x80, 40);  // NPC torch: 3D + attenuation
        bitsAt[1] = object(5, 0x2BE59E41, false, 0, 0x1E32EB8D, 0x15942315, 0x03, 0x80, 40);  // the player's 1st-person torch: unlisted
        bitsAt[2] = object(5, 0x121578BC, false, 1, 0x210871A2, 0x3B8BC7B9, 0x03, 0x88, 30);  // deer footsteps, with an effect slot
        bitsAt[3] = object(2, 0x17C5C5E5, true, 0, 0x3DC3B884, 0x2922A303, 0x03, 0x08, 20);   // a trap sound object
        bitsAt[4] = object(5, 0x39635892, false, 0, 0x11111111, 0x03FE78AC, 0x03, 0x08, 20);  // ogre: another parent -> differs
        bitsAt[5] = object(5, 0x0330BC32, false, 0, 0x07F032AF, 0x3B8BC7B9, 0x03, 0x0A, 20);  // spider: already 3D -> left
        bitsAt[6] = object(5, 0x0DD98FF7, false, 0, 0x38976342, 0x2922A303, 0x03, 0x08, 20);  // a trap id with the wrong type -> differs
        const uint32_t count = static_cast<uint32_t>(nobj);
        memcpy(hirc.data(), &count, 4);
        auto bank = [&](uint32_t version) {
            std::vector<uint8_t> b = {'B', 'K', 'H', 'D', 8, 0, 0, 0};
            for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(version >> (8 * k)));
            for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(0x5CD943EDu >> (8 * k)));
            const uint32_t n = static_cast<uint32_t>(hirc.size());
            b.insert(b.end(), {'H', 'I', 'R', 'C'});
            for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(n >> (8 * k)));
            b.insert(b.end(), hirc.begin(), hirc.end());
            return b;
        };
        const size_t base = 16 + 8;  // BKHD chunk, HIRC header
        std::vector<uint8_t> b = bank(150);
        const std::vector<uint8_t> orig = b;
        WorldSoundBankResult r{};
        WorldSoundScanBank(b.data(), b.size(), &r, nullptr);
        Check(r.toPatch == 3 && r.already == 1 && r.mismatch == 2 && b == orig,
              "WorldSoundPositionFix: a read-only scan finds 3 containers to change, 1 already 3D, 2 that differ, and writes nothing");
        WorldSoundScanBank(b.data(), b.size(), &r, b.data());
        std::vector<uint8_t> want = orig;
        want[base + bitsAt[0]] = 0x8A;
        want[base + bitsAt[2]] = 0x8A;
        want[base + bitsAt[3]] = 0x0A;
        Check(r.patched == 3 && b == want,
              "WorldSoundPositionFix: exactly the three bytes change (NPC torch 0x80 -> 0x8A, deer 0x88 -> 0x8A, trap 0x08 -> 0x0A); "
              "the player's torch, the differing ogre and wrong-type trap and every other byte stay");
        WorldSoundScanBank(b.data(), b.size(), &r, b.data());
        Check(r.patched == 0 && r.already == 4 && b == want, "WorldSoundPositionFix: a second pass over the same bytes changes nothing");
        std::vector<uint8_t> other = bank(145);
        const std::vector<uint8_t> otherOrig = other;
        WorldSoundScanBank(other.data(), other.size(), &r, other.data());
        Check(r.otherVersion && r.patched == 0 && other == otherOrig, "WorldSoundPositionFix: another bank version is left unchanged");
        bool cutOk = true;
        for (size_t cut = 0; cut < orig.size(); cut += 3) {  // truncated banks (heap copies of exact size): only listed flag bytes change
            std::vector<uint8_t> t(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(cut));
            WorldSoundScanBank(t.data(), t.size(), &r, t.data());
            for (size_t k = 0; k < t.size(); ++k)
                if (t[k] != orig[k] && k != base + bitsAt[0] && k != base + bitsAt[2] && k != base + bitsAt[3]) cutOk = false;
        }
        Check(cutOk, "WorldSoundPositionFix: truncated banks: no byte outside the listed containers' flag bytes changes");
        // refusals (review gh52build S2): another AttenuationID, no override bit, a 3D position type (automation data would
        // follow), a flags byte neither the shipped nor the new value, and an object whose size runs past the chunk; plus a
        // listed sound object with a source plugin's parameter block (its byte is still found and changed)
        hirc.clear();
        nobj = 0;
        u32(0);
        size_t refusedAt[5] = {};
        refusedAt[0] = object(5, 0x288241B8, false, 0, 0x037F1AC2, 0x37517387, 0x03, 0x88, 20);  // trap gears: other attenuation
        refusedAt[1] = object(5, 0x0B3518D2, false, 0, 0x037F1AC2, 0x37517387, 0x02, 0x88, 20);  // hall spikes: no override bit
        refusedAt[2] = object(5, 0x3BCE953C, false, 0, 0x037F1AC2, 0x37517387, 0x23, 0x88, 20);  // mine: 3D position type
        refusedAt[3] = object(5, 0x2A329A40, false, 0, 0x037F1AC2, 0x2922A303, 0x03, 0x81, 20);  // root worm: 0x81
        plugin = 0x00B80002;
        paramLen = 7;
        const size_t pluginAt = object(2, 0x1EF4C97C, true, 0, 0x1C57DBBD, 0x2922A303, 0x03, 0x08, 20);  // iron scrape: plugin block
        plugin = 0x00040001;
        u8(5);  // an object whose size field runs past the chunk: the walk stops there
        u32(0xFFFFFFFFu);
        u32(0x25B900EA);
        u8(0x08);
        ++nobj;
        const uint32_t count2 = static_cast<uint32_t>(nobj);
        memcpy(hirc.data(), &count2, 4);
        std::vector<uint8_t> rb = bank(150);
        const std::vector<uint8_t> rbOrig = rb;
        WorldSoundScanBank(rb.data(), rb.size(), &r, rb.data());
        std::vector<uint8_t> rbWant = rbOrig;
        rbWant[base + pluginAt] = 0x0A;
        Check(r.patched == 1 && r.mismatch == 4 && r.mismatchMask == ((1u << 5) | (1u << 6) | (1u << 9) | (1u << 10)) && rb == rbWant,
              "WorldSoundPositionFix: refused (another attenuation, no override, a 3D position type, flags 0x81) and an object size "
              "past the chunk leave every byte; a sound with a source-plugin parameter block is still changed (0x08 -> 0x0A)");
        (void)refusedAt;
        Check(kNumWorldSoundTargets == 15 && kWorldSoundTargets[0].id == 0x0BCA8316 && kWorldSoundTargets[0].newBits == 0x8A,
              "WorldSoundPositionFix: 15 containers listed, the NPC torch first (0x80 -> 0x8A)");

        if (Installed(InstallWorldSoundPosition(true), "WorldSoundPositionFix (sounds without direction)")) {
            uint8_t* const* fns = nullptr;
            int n = 0;
            WorldSoundPositionInfo(&fns, &n);
            bool jumps = n == 8;
            for (int k = 0; k < n; ++k) jumps = jumps && fns[k][0] == 0xE9;
            Check(jumps, "WorldSoundPositionFix: the 8 exported LoadBankMemoryView / Copy start with the detour's jmp");
            if (game.timeDateStamp == 0xF19077A4u)
                Check(fns[0] - game.base == 0x62AF210 && fns[4] - game.base == 0x62AED90,
                      "WorldSoundPositionFix: LoadBankMemoryView / Copy (3 arguments) at exe+0x62AF210 / exe+0x62AED90 (build 19115871)");
            Check(InstallWorldSoundPosition(true), "WorldSoundPositionFix: a second install finds it in place");
            static int fakeCalls;
            static const void* fakeData;
            fakeCalls = 0;
            LoadFn_t fake = [](uint64_t d, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> uint64_t {
                ++fakeCalls;
                fakeData = reinterpret_cast<const void*>(d);
                return 1;
            };
            std::vector<uint8_t> h = orig;
            WorldSoundPositionTestCall(1, h.data(), static_cast<uint32_t>(h.size()), fake);
            Check(fakeCalls == 1 && fakeData == h.data() && h == want, "WorldSoundPositionFix: the hook changes a writable bank, then loads it");
            uint8_t* const ro = static_cast<uint8_t*>(VirtualAlloc(nullptr, orig.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            DWORD oldp = 0;
            if (ro) {
                memcpy(ro, orig.data(), orig.size());
                VirtualProtect(ro, orig.size(), PAGE_READONLY, &oldp);
                WorldSoundPositionTestCall(6, ro, static_cast<uint32_t>(orig.size()), fake);
                Check(fakeCalls == 2 && memcmp(ro, orig.data(), orig.size()) == 0,
                      "WorldSoundPositionFix: a read-only bank is loaded unchanged (no write, no fault)");
                VirtualFree(ro, 0, MEM_RELEASE);
            }
            SetWorldSoundPositionFix(false);
        }
    }

    // ArrowHitSoundFix (GitHub #52): an NPC's arrow hit's thud is posted once, on the arrow: the shooter's weapon's posts are
    // left out when the hit's thud is already on the arrow, else moved to the arrow; the arrow's own post is not repeated after
    // a moved one.
    {
        Check(ApplyArrowHitSoundFix(), "arrow hit sound fix installed");
        ArrowHitSoundInfo ai{};
        Check(ArrowHitSoundFixInfo(&ai), "arrow hit sound fix info");
        if (Installed(ai.stub != nullptr, "ArrowHitSoundFix (NPC arrow hits heard at the shooter)")) {
            if (game.timeDateStamp == 0xF19077A4u) {
                Check(ai.bowCall == game.base + 0x485DB08 && ai.arrowCall == game.base + 0x4860CE6,
                      "ArrowHitSoundFix: the handler's weapon post at exe+0x485DB08, the arrow's own post at exe+0x4860CE6 (build 19115871)");
                Check(ai.post == game.base + 0x480DE20 && ai.constant == game.base + 0x9303E90 &&
                          ai.setSwitches == game.base + 0x486F6B0,
                      "ArrowHitSoundFix: both call the post helper exe+0x480DE20 (constant exe+0x9303E90) and the switch setter exe+0x486F6B0");
                Check(ai.weakGet == game.base + 0x1126290 && ai.arrowClass == game.base + 0x4618F70 && ai.playerFlag == 0xB20 &&
                          ai.compOffset == 0x790 && ai.ownerOffset == 0xA8,
                      "ArrowHitSoundFix: FWeakObjectPtr::Get exe+0x1126290, the arrow class exe+0x4618F70, Player flag +0xB20, hit component +0x790, owner +0xA8");
            }
            Check(ai.bowCall[0] == 0xE8 && RipTarget(ai.bowCall, 1, 5) == ai.stub && ai.arrowCall[0] == 0xE8 &&
                      RipTarget(ai.arrowCall, 1, 5) == ai.stub + 32,
                  "ArrowHitSoundFix: both calls go to their stubs");
            const uint8_t bowMov[6] = {0x49, 0x89, 0xF8, 0x49, 0x89, 0xD9}, arrowMov[6] = {0x49, 0x89, 0xE8, 0x49, 0x89, 0xF9};
            Check(memcmp(ai.stub, bowMov, 6) == 0 && memcmp(ai.stub + 32, arrowMov, 6) == 0 &&
                      *reinterpret_cast<void* const*>(ai.stub + 12) == ai.bowHook &&
                      *reinterpret_cast<void* const*>(ai.stub + 44) == ai.arrowHook,
                  "ArrowHitSoundFix: stubs hand over (record, attacker) / (component, shooter) and jump to the two hooks");
            Check(ApplyArrowHitSoundFix(), "ArrowHitSoundFix: a second installation finds the fix in place");

            // The hooks with fakes: objects (flags +0x08, class +0x10), classes (bases +0x30, depth +0x38), a weak pointer that
            // holds the object, a post helper and a switch setter that count, a clock.
            using PostFn = uint64_t (*)(void*, void*, void*, void*, uint64_t, uint64_t);
            static int posts = 0, switches = 0, failNext = 0;
            static void* lastActor = nullptr;
            static void* swShooter = nullptr;
            static void* swComp = nullptr;
            static void* swActor = nullptr;
            static bool argsOk = true;
            static void* constant = nullptr;
            static uint64_t clockMs = 5000;
            alignas(8) static uint8_t arrowCls[0x40] = {}, arrowObjCls[0x40] = {}, otherCls[0x40] = {};
            static const uint8_t* bases[2] = {};
            struct Fakes {
                static uint64_t Post(void*, void* actor, uint64_t one, void* c, uint64_t a5, uint64_t a6)
                {
                    ++posts;
                    lastActor = actor;
                    argsOk = argsOk && one == 1 && c == constant && a5 == 0 && a6 == 1;
                    if (failNext) {
                        --failNext;
                        return 0;
                    }
                    return 7;
                }
                static void Switches(void* shooter, void* comp, uint64_t index, void* actor)
                {
                    ++switches;
                    swShooter = shooter;
                    swComp = comp;
                    swActor = actor;
                    argsOk = argsOk && index == 0;
                }
                static void* Weak(const void* p) { return *static_cast<void* const*>(p); }
                static const uint8_t* Class() { return arrowCls; }
                static uint64_t Clock() { return clockMs; }
            };
            constant = ai.constant;
            bases[0] = otherCls + 0x30;
            bases[1] = arrowCls + 0x30;
            const int32_t one = 1, zero = 0;
            memcpy(arrowCls + 0x38, &one, 4);
            memcpy(arrowObjCls + 0x38, &one, 4);
            const uint8_t* const* basesPtr = bases;
            memcpy(arrowObjCls + 0x30, &basesPtr, 8);
            memcpy(otherCls + 0x38, &zero, 4);
            memcpy(otherCls + 0x30, &basesPtr, 8);  // depth 0: bases[0] is itself
            struct Obj { alignas(8) uint8_t b[0xC00]; };
            static Obj npc{}, player{}, wolf{}, bow{}, arrowA{}, arrowB{}, arrowC{}, deer{}, eventA{}, eventB{};
            alignas(8) static uint8_t compDeer[0xB0] = {}, compWolf[0xB0] = {};
            auto setClass = [](Obj& o, const uint8_t* cls) { memcpy(o.b + 0x10, &cls, 8); };
            setClass(arrowA, arrowObjCls);
            setClass(arrowB, arrowObjCls);
            setClass(arrowC, arrowObjCls);
            setClass(bow, otherCls);
            setClass(deer, otherCls);
            player.b[ai.playerFlag] = 1;
            const void* d = &deer;
            const void* w = &wolf;
            memcpy(compDeer + 0xA8, &d, 8);
            memcpy(compWolf + 0xA8, &w, 8);
            const void* cd = compDeer;
            memcpy(deer.b + ai.compOffset, &cd, 8);
            alignas(8) static uint8_t rec[0x30] = {};
            auto makeRec = [](const void* target, const void* arrow, uint8_t arrowFlag, uint8_t spellFlag) {
                memset(rec, 0, sizeof rec);
                memcpy(rec + 0x08, &target, 8);
                rec[0x1C] = arrowFlag;
                rec[0x1D] = spellFlag;
                memcpy(rec + 0x20, &arrow, 8);
            };
            ArrowHitSoundFixTestSetup(reinterpret_cast<void*>(&Fakes::Post), reinterpret_cast<void*>(&Fakes::Switches),
                                      reinterpret_cast<void*>(&Fakes::Weak), reinterpret_cast<void*>(&Fakes::Class), &Fakes::Clock);
            SetArrowHitSoundFixEnabled(true);
            const auto bowHook = reinterpret_cast<PostFn>(ai.bowHook);
            const auto arrowHook = reinterpret_cast<PostFn>(ai.arrowHook);
            ArrowHitSoundFixInfo(&ai);
            const uint64_t s0 = ai.skipped, m0 = ai.moved, k0 = ai.kept, p0 = ai.player, a0 = ai.arrowSkipped;
            // a: the Unreal side first (the arrow's own post), then the handler: the weapon's post left out, twice
            makeRec(&deer, &arrowA, 1, 0);
            Check(arrowHook(&eventA, &arrowA, compDeer, &npc, 0, 1) == 7 && posts == 1 && lastActor == &arrowA,
                  "ArrowHitSoundFix: the arrow's own post is made unchanged");
            clockMs += 6;
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 0 && posts == 1 && switches == 0,
                  "ArrowHitSoundFix: the shooter's weapon's post 6 ms later is left out");
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 0 && posts == 1,
                  "ArrowHitSoundFix: the other engine's handler call for the same hit: left out too");
            // b: the handler first (the old engine's hit): the post moves to the arrow with the hit's switches; the arrow's own
            // post that follows is not made again, nor the second handler call's
            clockMs += 2000;
            makeRec(&deer, &arrowB, 1, 0);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 2 && lastActor == &arrowB && switches == 1 &&
                      swShooter == &npc && swComp == compDeer && swActor == &arrowB,
                  "ArrowHitSoundFix: handler first: the hit's switches set on the arrow (as the arrow's own impact does), posted on the arrow");
            Check(arrowHook(&eventA, &arrowB, compDeer, &npc, 0, 1) == 0 && posts == 2,
                  "ArrowHitSoundFix: the arrow's own post after a moved one: not made again");
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 0 && posts == 2 && switches == 1,
                  "ArrowHitSoundFix: the second handler call: left out");
            // c: the moved post fails: the weapon's post is made (a hit is never silent)
            clockMs += 2000;
            makeRec(&deer, &arrowC, 1, 0);
            failNext = 1;
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 4 && lastActor == &bow && switches == 2,
                  "ArrowHitSoundFix: the moved post gives no playing id: the game's post on the weapon");
            // d: the player's shot, melee, a spell record: the game's post on the weapon
            makeRec(&deer, &arrowA, 1, 0);
            Check(bowHook(&eventA, &bow, rec, &player, 0, 1) == 7 && posts == 5 && lastActor == &bow,
                  "ArrowHitSoundFix: the player's arrow: posted on the weapon (stock)");
            makeRec(&deer, nullptr, 0, 0);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 6 && lastActor == &bow, "ArrowHitSoundFix: a melee hit: stock");
            makeRec(&deer, &arrowA, 1, 1);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 7 && lastActor == &bow,
                  "ArrowHitSoundFix: a record with the spell flag: stock");
            // e: a dying arrow, no target: the game's post on the weapon
            makeRec(&deer, &arrowA, 1, 0);
            const uint32_t dying = 0x20000000;
            memcpy(arrowA.b + 0x08, &dying, 4);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 8 && lastActor == &bow, "ArrowHitSoundFix: a dying arrow: stock");
            memset(arrowA.b + 0x08, 0, 4);
            makeRec(nullptr, &arrowA, 1, 0);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 9 && lastActor == &bow, "ArrowHitSoundFix: no target: stock");
            // f: past the 1 s window, another hit character, event or shooter: the handler moves its post to the arrow
            clockMs += 2000;
            makeRec(&deer, &arrowA, 1, 0);
            arrowHook(&eventA, &arrowA, compDeer, &npc, 0, 1);
            clockMs += 1000;
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 11 && lastActor == &arrowA,
                  "ArrowHitSoundFix: 1 s after the arrow's post: a new thud, on the arrow");
            clockMs += 2000;
            arrowHook(&eventA, &arrowA, compWolf, &npc, 0, 1);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 13 && lastActor == &arrowA,
                  "ArrowHitSoundFix: the arrow's post was on another character: the handler's moved to the arrow");
            clockMs += 2000;
            arrowHook(&eventB, &arrowA, compDeer, &npc, 0, 1);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 15, "ArrowHitSoundFix: another event: moved");
            clockMs += 2000;
            arrowHook(&eventA, &arrowA, compDeer, &wolf, 0, 1);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 17, "ArrowHitSoundFix: another shooter: moved");
            // g: the arrow's post on a non-arrow actor: made unchanged, not remembered
            clockMs += 2000;
            Check(arrowHook(&eventA, &bow, compDeer, &npc, 0, 1) == 7 && posts == 18 && lastActor == &bow,
                  "ArrowHitSoundFix: the arrow's post on a non-arrow: made unchanged");
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 19 && lastActor == &arrowA,
                  "ArrowHitSoundFix: ... and not taken as the hit's sound");
            // h: switched off: both made as in the game; on again: what was posted while off is not remembered
            clockMs += 2000;
            SetArrowHitSoundFixEnabled(false);
            Check(arrowHook(&eventA, &arrowA, compDeer, &npc, 0, 1) == 7 && bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 &&
                      posts == 21 && lastActor == &bow,
                  "ArrowHitSoundFix: switched off: both posts as in the game");
            SetArrowHitSoundFixEnabled(true);
            Check(bowHook(&eventA, &bow, rec, &npc, 0, 1) == 7 && posts == 22 && lastActor == &arrowA,
                  "ArrowHitSoundFix: switched on again: the arrow post made while off is not remembered");
            // i: the arrow's own posts are only left out after a moved one (the player's are never)
            clockMs += 2000;
            Check(arrowHook(&eventA, &arrowB, compDeer, &player, 0, 1) == 7 && arrowHook(&eventA, &arrowB, compDeer, &player, 0, 1) == 7 &&
                      posts == 24,
                  "ArrowHitSoundFix: arrow posts not after a moved one: all made");
            Check(argsOk && switches == 8, "ArrowHitSoundFix: every post got r8 = 1, the game's constant and its two bytes back; 8 switch setups");
            ArrowHitSoundFixInfo(&ai);
            Check(ai.skipped - s0 == 3 && ai.moved - m0 == 7 && ai.kept - k0 == 3 && ai.player - p0 == 1 && ai.arrowSkipped - a0 == 1,
                  "ArrowHitSoundFix: counted 3 left out, 7 moved, 3 left on the weapon, 1 player shot, 1 arrow post not repeated");
            LogArrowHitSoundFixStats();
            ArrowHitSoundFixTestSetup(nullptr, nullptr, nullptr, nullptr, nullptr);
        }
    }

    // AutosaveFollowsMenu: the level-up's and the scripts' autosaves are not made while all four of the menu's autosave
    // options are off.
    {
        Check(ApplyAutosaveFollowsMenu(), "autosave fix installed");
        AutosaveMenuInfo ai{};
        Check(AutosaveFollowsMenuInfo(&ai), "autosave fix info");
        if (Installed(ai.stub != nullptr, "AutosaveFollowsMenu (autosaves with the menu's options off)")) {
            Check(ai.levelUpCall == game.base + 0x65FF287, "the level-up's autosave call at exe+0x65FF287");
            Check(ai.scriptedCall == game.base + 0x6A07684, "the scripts' autosave call at exe+0x6A07684");
            Check(ai.autosave == game.base + 0x6703BC0, "the autosave at exe+0x6703BC0");
            Check(ai.switches[0] == game.base + 0x941DAD8 && ai.switches[1] == game.base + 0x941DB28 &&
                      ai.switches[2] == game.base + 0x941DA80 && ai.switches[3] == game.base + 0x941DA68,
                  "the menu's settings at exe+0x941DAD8 / DB28 / DA80 / DA68 (interior/exterior, travel, rest, wait)");
            Check(ai.levelUpCall[0] == 0xE8 && RipTarget(ai.levelUpCall, 1, 5) == ai.stub, "level-up's call goes to the stub");
            Check(ai.scriptedCall[0] == 0xE8 && RipTarget(ai.scriptedCall, 1, 5) == ai.stub + 16,
                  "scripted autosave's call goes to the stub + 16");
            Check(*reinterpret_cast<void* const*>(ai.stub + 6) == ai.levelUpHook &&
                      *reinterpret_cast<void* const*>(ai.stub + 22) == ai.scriptedHook,
                  "stubs jump to the two hooks");
            Check(ApplyAutosaveFollowsMenu(), "second installation finds the fix in place");

            // The hooks with a fake autosave and fake switches.
            using SaveFn = void (*)(void*);
            static int saves = 0;
            static void* lastManager = nullptr;
            struct Fakes {
                static void Autosave(void* manager)
                {
                    ++saves;
                    lastManager = manager;
                }
            };
            static uint8_t sw[4] = {};
            static const uint8_t* swp[4] = {&sw[0], &sw[1], &sw[2], &sw[3]};
            AutosaveFollowsMenuTestSetup(reinterpret_cast<void*>(&Fakes::Autosave), swp);
            const auto levelUp = reinterpret_cast<SaveFn>(ai.levelUpHook);
            const auto scripted = reinterpret_cast<SaveFn>(ai.scriptedHook);
            int manager = 0;
            levelUp(&manager);
            scripted(&manager);
            Check(saves == 0, "all four menu options off: no level-up or scripted autosave");
            for (int i = 0; i < 4; ++i) {
                sw[i] = 1;
                levelUp(&manager);
                scripted(&manager);
                sw[i] = 0;
            }
            Check(saves == 8 && lastManager == &manager, "any one option on: both autosaves made, with the game's argument");
            SetAutosaveFollowsMenuEnabled(false);
            levelUp(&manager);
            scripted(&manager);
            Check(saves == 10, "switched off: both autosaves made as in the stock game");
            SetAutosaveFollowsMenuEnabled(true);
            AutosaveFollowsMenuInfo(&ai);
            Check(ai.skippedLevelUp == 1 && ai.skippedScripted == 1 && ai.made == 10, "skipped and made autosaves counted");
            LogAutosaveFollowsMenuStats();
            AutosaveFollowsMenuTestSetup(nullptr, nullptr);
        }
    }

    // HavokPickWarningGuard: the old engine's "Havok picks" warning box (a modal MessageBoxA on the OblivionThread) held back.
    {
        // A changed handler first (the pointer at exe+0x8FB7CC8 aimed at another function): the guard refuses and leaves the
        // pointer alone. (The "pattern not found" path needs a different exe: the self-test on another build shows it.)
        {
            auto** slot = reinterpret_cast<void**>(game.base + 0x8FB7CC8);
            void* const saved = *slot;
            void* const elsewhere = game.base + 0x699E850;  // another caller of the handler, not the handler
            *slot = elsewhere;
            Check(!ApplyHavokPickWarningGuard(), "a changed handler: guard not installed");
            Check(*slot == elsewhere, "a changed handler: the pointer is left alone");
            HavokPickGuardInfo none{};
            Check(!HavokPickWarningGuardInfo(&none), "a changed handler: no guard to report");
            *slot = saved;
        }
        Check(ApplyHavokPickWarningGuard(), "pick warning guard installed");
        HavokPickGuardInfo pg{};
        Check(HavokPickWarningGuardInfo(&pg), "pick warning guard info");
        if (Installed(pg.slot != nullptr && pg.hook != nullptr, "HavokPickWarningGuard (pick warning box)")) {
            Check(pg.slot == reinterpret_cast<void**>(game.base + 0x8FB7CC8), "warning handler pointer at exe+0x8FB7CC8");
            Check(pg.original == game.base + 0x691B460, "the game's handler (MessageBoxA) at exe+0x691B460");
            Check(pg.callReturn == game.base + 0x65E10F5, "the pick warning's call returns to exe+0x65E10F5");
            Check(pg.picks == reinterpret_cast<const uint32_t*>(game.base + 0x941F9A8) &&
                      pg.max == reinterpret_cast<const uint32_t*>(game.base + 0x8FAAB98),
                  "pick counter at exe+0x941F9A8, iMaxPicks at exe+0x8FAAB98");
            Check(*pg.max == 40, "iMaxPicks default 40");
            Check(*pg.slot == pg.hook, "handler pointer points at the guard");
            Check(ApplyHavokPickWarningGuard(), "second installation finds the guard in place");

            // The wrapper with the game's handler replaced by a fake: held back only from the pick warning's call site.
            static int boxes = 0;
            static const char* lastText = nullptr;
            static uint64_t lastA3 = 0;
            struct Fakes {
                static uint64_t Handler(const char*, const char* text, uint64_t a3)
                {
                    ++boxes;
                    lastText = text;
                    lastA3 = a3;
                    return 7;
                }
            };
            using HandlerFn = uint64_t (*)(const char*, const char*, uint64_t);
            const auto hook = reinterpret_cast<HandlerFn>(*pg.slot);
            HavokPickWarningGuardTestFunctions(reinterpret_cast<void*>(&Fakes::Handler), pg.callReturn);
            Check(hook("WARNING", "42 Havok picks (6 path, 36 LOS, 0 ViewCaster) used this frame.", 0) == 0 && boxes == 0,
                  "from the pick warning: held back, returns 0 like the game's handler");
            HavokPickWarningGuardTestFunctions(nullptr, game.base + 0x699EA03);  // another caller of the handler
            Check(hook("WARNING", "something else", 5) == 7 && boxes == 1 && lastText && strcmp(lastText, "something else") == 0 &&
                      lastA3 == 5,
                  "another caller: the game's handler, same arguments and result");
            SetHavokPickWarningGuardEnabled(false);
            HavokPickWarningGuardTestFunctions(nullptr, pg.callReturn);
            Check(hook("WARNING", "41 Havok picks (0 path, 41 LOS, 0 ViewCaster) used this frame.", 0) == 7 && boxes == 2,
                  "switched off: the box as in the stock game");
            SetHavokPickWarningGuardEnabled(true);
            Check(hook("WARNING", "43 Havok picks (1 path, 42 LOS, 0 ViewCaster) used this frame.", 0) == 0 && boxes == 2,
                  "switched on again: held back");
            HavokPickWarningGuardInfo(&pg);
            Check(pg.heldBack == 2 && pg.forwarded == 2, "held back and forwarded counted");
            LogHavokPickWarningGuardStats();
            HavokPickWarningGuardTestFunctions(nullptr, nullptr);
        }
    }

    // DisplayModeCache: GetMonitorsInfo's display mode lists (graphics settings page).
    {
        Check(ApplyDisplayModeCache(false) == 1, "display mode lists: GetMonitorsInfo found, import slot hooked");
        DisplayModeCacheInfoOut di{};
        Check(DisplayModeCacheInfo(&di), "display mode lists: info");
        if (Installed(di.slot != nullptr && di.hook != nullptr, "DisplayModeCache (display mode lists)")) {
            Check(di.slot == reinterpret_cast<void**>(game.base + 0x6D975C0), "EnumDisplaySettingsW import slot at exe+0x6D975C0");
            Check(di.edidRef == game.base + 0x117331B, "GetMonitorsInfo's \"EDID\" reference at exe+0x117331B");
            Check(di.numSites == 4 && di.returns[0] == game.base + 0x1172F73 && di.returns[1] == game.base + 0x1173000 &&
                      di.returns[2] == game.base + 0x1173560 && di.returns[3] == game.base + 0x11735A5,
                  "its four EnumDisplaySettingsW calls (return addresses exe+0x1172F73/0x1173000/0x1173560/0x11735A5)");
            Check(*di.slot == di.hook, "the import slot points at the hook");
            Check(ApplyDisplayModeCache(false) == 1, "second installation finds the hook in place");
            const auto hook = reinterpret_cast<EnumDisplaySettingsFn>(di.hook);
            const EnumDisplaySettingsFn direct = &FakeEnumDisplaySettings;
            int calls = 0;
            auto same = [&](bool zero, uint8_t fill) {
                const auto a = EnumerateModes(hook, zero, fill, &calls);
                int directCalls = 0;
                return a == EnumerateModes(direct, zero, fill, &directCalls) && !a.empty();
            };

            // a Windows that writes past dmSize: never answered from a copy
            DisplayModeCacheTestSetup(reinterpret_cast<void*>(&FakeEnumDisplaySettings), true);
            g_fd = {};
            g_fd.writeBeyond = true;
            Check(same(false, 0xAA) && calls == 2 + 683 + 1, "writes past dmSize: the check fails, every read goes to Windows");
            Check(same(false, 0xAA) && calls == 683 + 1, "... and again (no copy)");

            DisplayModeCacheTestSetup(reinterpret_cast<void*>(&FakeEnumDisplaySettings), true);
            g_fd = {};
            Check(same(false, 0xAA) && calls == 2 + 683 + 1 + 1, "first read: from Windows (2 write checks, 684 reads, current mode)");
            Check(same(false, 0xAA) && calls <= 8, "second read: the copy, checked against Windows in <= 8 calls, same bytes");
            Check(same(true, 0x00) && calls <= 8, "UE's loop (buffer zeroed, dmSize 0 before every call): same bytes from the copy");
            Check(same(false, 0x5C) && calls <= 8, "another buffer fill: the bytes past dmSize stay the caller's");
            g_fd.count = 684;
            Check(same(false, 0xAA) && calls > 684, "a mode more: noticed, read from Windows");
            Check(same(false, 0xAA) && calls <= 8, "... and the new list is kept");
            g_fd.count = 682;
            Check(same(false, 0xAA) && calls > 682, "a mode less: noticed, read from Windows");
            g_fd.curW = 2560;
            g_fd.curH = 1440;
            Check(same(false, 0xAA) && calls > 682, "another current mode: noticed, read from Windows");
            g_fd.changedIndex = 682 / 2;
            Check(same(false, 0xAA) && calls > 682, "a mode in the middle changed: noticed, read from Windows");
            g_fd.changedIndex = 0;
            Check(same(false, 0xAA) && calls > 682, "mode 0 changed: noticed, read from Windows");
            Check(same(false, 0xAA) && calls <= 8, "unchanged again: from the copy");
            // a read that stops half-way, then a full one
            EnumerateModes(hook, false, 0xAA, &calls, L"\\\\.\\DISPLAY1", 10);
            Check(same(false, 0xAA) && calls <= 8, "a read stopped after 10 modes, then a full one: from the copy, same bytes");
            // calls that are not a sequential read from mode 0 go to Windows
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            int before = g_fd.calls;
            Check(hook(L"\\\\.\\DISPLAY1", 5, &dm) && g_fd.calls == before + 1, "mode 5 without mode 0 first: from Windows");
            before = g_fd.calls;
            Check(hook(L"\\\\.\\DISPLAY1", ENUM_CURRENT_SETTINGS, &dm) && g_fd.calls == before + 1 && dm.dmPelsWidth == 2560,
                  "current mode: from Windows");
            Check(EnumerateModes(hook, false, 0, &calls, L"\\\\.\\DISPLAY2").empty() && calls == 1, "another device without modes: from Windows");
            before = g_fd.calls;
            Check(hook(nullptr, 0, &dm) == FALSE && g_fd.calls == before + 1, "no device name: from Windows");
            // switched off: every read from Windows; on again: the first read refills
            SetDisplayModeCacheEnabled(false);
            Check(same(false, 0xAA) && calls == 682 + 1, "switched off: every read from Windows");
            SetDisplayModeCacheEnabled(true);
            Check(same(false, 0xAA) && calls == 682 + 1 + 1, "switched on: the first read refills the copy");
            Check(same(false, 0xAA) && calls <= 8, "... then from the copy");
            // only GetMonitorsInfo's calls are answered from the copy
            DisplayModeCacheTestSetup(reinterpret_cast<void*>(&FakeEnumDisplaySettings), false);
            Check(same(false, 0xAA) && calls == 682 + 1, "other callers (not GetMonitorsInfo): every read from Windows");
            Check(same(false, 0xAA) && calls == 682 + 1, "... always");
            LogDisplayModeCacheStats();
        }
    }

    // MenuScreenshotFilter: the menu screenshot's same-size ImageResize (2x2 box filter) in SSE2, compared with the game's own code.
    {
        Check(ApplyImageResizeFix(true), "image resize: call site found and redirected");
        uint8_t *call = nullptr, *orig = nullptr, *stub = nullptr;
        void* hook = nullptr;
        Check(ImageResizeFixInfo(&call, &orig, &stub, &hook), "image resize: info");
        if (Installed(call != nullptr && stub != nullptr && hook != nullptr, "MenuScreenshotFilter (image resize)")) {
            Check(call == game.base + 0x32549A3, "ImageResize(TArray) calls the TArrayView version at exe+0x32549A3");
            Check(orig == game.base + 0x32549E0, "ImageResize(TArrayView) at exe+0x32549E0");
            int32_t rel = 0;
            memcpy(&rel, call + 1, 4);
            Check(call[0] == 0xE8 && call + 5 + rel == stub, "the call goes to the stub");
            void* jumpTo = nullptr;
            memcpy(&jumpTo, stub + 6, sizeof jumpTo);
            Check(MatchesAt(stub, ParsePattern("FF 25 00 00 00 00")) && jumpTo == hook, "stub jumps to the hook");
            Check(MatchesAt(call + 1, ParsePattern("38 00")), "low 16 bits of the call unchanged (only two bytes switched)");
            Check(ApplyImageResizeFix(true), "second application finds the call already switched");

            struct View { uint32_t* data; int32_t num; };
            using ViewResize = void (*)(int32_t, int32_t, const View*, int32_t, int32_t, const View*, bool, bool);
            const auto gameResize = reinterpret_cast<ViewResize>(orig);
            const auto hooked = reinterpret_cast<ViewResize>(hook);
            uint32_t seed = 12345;
            auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
            const int sizes[][2] = {{1, 1}, {1, 2}, {2, 1}, {2, 2}, {3, 3}, {4, 1}, {1, 4}, {4, 4}, {5, 5}, {6, 2}, {7, 3}, {3, 7},
                                    {8, 8}, {9, 9}, {17, 5}, {33, 17}, {64, 48}, {127, 65}, {640, 360}, {1279, 719}};
            bool allSame = true, hookSame = true;
            int compared = 0;
            for (const auto& sz : sizes) {
                const int w = sz[0], h = sz[1];
                for (int pattern = 0; pattern < 3; ++pattern) {
                    std::vector<uint32_t> src(static_cast<size_t>(w) * h);
                    for (auto& c : src) c = pattern == 0 ? rnd() : pattern == 1 ? 0xFFFFFFFFu : (rnd() & 0x01010101u) * 0xFF;
                    for (const bool opaque : {true, false}) {
                        std::vector<uint32_t> a(src.size(), 0xDEADBEEFu), b(src.size(), 0x12345678u), c(src.size(), 0x0BADF00Du);
                        View sv{src.data(), static_cast<int32_t>(src.size())}, av{a.data(), static_cast<int32_t>(a.size())};
                        View cv{c.data(), static_cast<int32_t>(c.size())};
                        gameResize(w, h, &sv, w, h, &av, false, opaque);
                        BoxFilter2x2(w, h, src.data(), b.data(), opaque);
                        hooked(w, h, &sv, w, h, &cv, false, opaque);
                        if (a != b) allSame = false;
                        if (a != c) hookSame = false;
                        ++compared;
                    }
                }
            }
            char what[160];
            sprintf_s(what, "the fast filter gives the game's bytes (%d images: 20 sizes 1x1..1279x719, random/white/0-255 noise, alpha forced or kept)", compared);
            Check(allSame, what);
            Check(hookSame, "the hook gives the game's bytes for the same images");
            // other calls go to the game's code: a real resize (different size), linear space, overlapping arrays
            {
                std::vector<uint32_t> src(64 * 48);
                for (auto& c : src) c = rnd();
                std::vector<uint32_t> a(32 * 24, 1), b(32 * 24, 2);
                View sv{src.data(), static_cast<int32_t>(src.size())}, av{a.data(), 32 * 24}, bv{b.data(), 32 * 24};
                gameResize(64, 48, &sv, 32, 24, &av, false, true);
                hooked(64, 48, &sv, 32, 24, &bv, false, true);
                Check(a == b, "a real resize (64x48 -> 32x24): left to the game's code, same bytes");
                std::vector<uint32_t> inplace(src), inplace2(src);
                View iv{inplace.data(), static_cast<int32_t>(inplace.size())}, iv2{inplace2.data(), static_cast<int32_t>(inplace2.size())};
                gameResize(64, 48, &iv, 64, 48, &iv, false, true);
                hooked(64, 48, &iv2, 64, 48, &iv2, false, true);
                Check(inplace == inplace2, "source and destination the same array: left to the game's code, same bytes");
            }
            // speed at 3840x2160 (informational): the game's loop vs the fast filter
            {
                const int w = 3840, h = 2160;
                std::vector<uint32_t> src(static_cast<size_t>(w) * h), a(src.size()), b(src.size());
                for (auto& c : src) c = rnd();
                View sv{src.data(), static_cast<int32_t>(src.size())}, av{a.data(), static_cast<int32_t>(a.size())};
                LARGE_INTEGER f, t0, t1, t2;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t0);
                gameResize(w, h, &sv, w, h, &av, false, true);
                QueryPerformanceCounter(&t1);
                BoxFilter2x2(w, h, src.data(), b.data(), true);
                QueryPerformanceCounter(&t2);
                char msg[160];
                sprintf_s(msg, "3840x2160: same bytes; the game's loop %.1f ms, the fast filter %.1f ms",
                          1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart),
                          1000.0 * static_cast<double>(t2.QuadPart - t1.QuadPart) / static_cast<double>(f.QuadPart));
                Check(a == b, msg);
            }
            LogImageResizeStats();
        }
    }

    // ScreenshotReadbackConvert: the screenshot readback's R10G10B10A2 conversion (selftest_readback.h).
    rbc::Run(game);
    // ScreenshotParallel: the screenshot's copies on helper threads (selftest_parallel.h).
    spar::Run(game);
    // OsPageCache: the OS page cache (selftest_oscache.h).
    osc::Run(game);
    // PsoPrecompileFast: the start-up PSO precompile's pace (selftest_pso.h).
    pso::Run(game);
    // Diagnostic HitchLedger (selftest_ledger.h): before BulkIndexInsert's test (its lab probe times the same tick-group entry).
    hl::Run(game);
    // Diagnostic DoorDiag (selftest_doordiag.h): its sites, located only.
    dd::Run(game);
    // StreamBehindLoadingScreen (selftest_streamload.h): its sites, located only (after the fade hook test).
    sbl::Run(game);
    // Diagnostic TickSplit (selftest_ticksplit.h): rides on the ledger's tick-group hook.
    tsp::Run(game);
    // MonitorNativeFromWindows / FullscreenAllResolutions (selftest_display_native.h).
    dnat::Run(game);
    // SeparableSkinAbove4K (selftest_skinsss.h).
    skinsss::Run(game);
    // BodyRemovalSpread (selftest_bodyspread.h): discovery only on the mapped exe (nothing installed) and its logic with fakes.
    BodySpreadRun(game);
    // StreamOutEndPlaySlice / ResetLoadersNoFlush (selftest_epicport.h): discovery on the mapped exe (nothing patched), hooks with fakes.
    EpicPortRun(game);
    // StartupBarFinish (selftest_barfinish.h): discovery on the mapped exe (nothing patched), the hook with a fake step.
    BarFinishRun(game);
    // TopicSort / PluginPrefetch (selftest_gapfix.h): the game's sort run from the mapped exe against ours; the prefetch's path and list rules.
    GapFixRun(game);
    // GcPassAfterBusy (selftest_gchold.h): discovery on the mapped exe (nothing patched), the hold with a fake engine.
    GcHoldRun(game);
    // GcPrepAhead (selftest_gcprep.h): discovery on the mapped exe (nothing patched; altered bytes refused), the step with fakes.
    GcPrepRun(game);
    // GcMarkPriority (selftest_gcmark.h): discovery on the mapped exe (nothing patched), fake code, the raise with fake thunks.
    GcMarkRun(game);
    // StreamRegisterGranularity (selftest_streamgran.h): the cvar's bound int on the mapped exe and the set rules.
    StreamGranRun();
    // NavQueryPriority (selftest_navprio.h): the cvar's name on the mapped exe and the set rules with a fake console variable.
    NavPrioRun();
    // PhysicsSubstepCap (selftest_physcap.h): fake object list, class, properties and FName constructor; Engine.ini lines.
    PhysCapRun();
    // IndirectArgBufferPool (selftest_indirectargs.h): the cvar's int on the mapped exe and the start-up rules (put back to 0).
    IndirectArgsRun();
    // ParallelFileReads (selftest_filereads.h): the engine's reader, its slot, Pop and ServiceNotify on the mapped exe (nothing
    // patched); the replacement run against stand-ins for the engine's objects and a real temporary file.
    FileReadsRun(game);
    // LumenCardsNarrow (selftest_lumennarrow.h): discovery, refusal and patch on a copy, the stubs, the decision with fakes.
    LumenNarrowRun(game);
    // LumenScanCull (selftest_lumenscan.h): sites (nothing installed), the stub, and the cull run against the mapped exe's scan body
    // (the proof and an independent stock run as oracles; a forged wrong skip must be caught).
    lscan::Run(game);
    // RepathParallel (selftest_repath.h): discovery on the mapped exe (nothing installed), the key and hooks with fakes.
    RepathRun(game);
    // Start-up install order (selftest_installorder.h): core/main.cpp's Init installs each dependent key after its prerequisite.
    InstallOrderRun();

    // PairReleaseOnce / lab PairRaceProbe (selftest_pairrace.h): rewrites the two early releases (no later test reads them).
    PairRaceRun(game);
    // OtFrameTrim (selftest_otframe.h): rewrites the OT frame-start sleep's call and the node timer (no later test reads them).
    OtFrameTrimRun(game);
    // LooseMissCache (selftest_loosemiss.h): rewrites the file finder's two _access calls (no later test reads them).
    LooseMissRun(game);
    // PowerAttackAnimKeep (selftest_poweranim.h): rewrites 8 bytes of the animation release (no later test reads them).
    PowerAnimRun(game);
    // FlowOverlapsOnce (selftest_flowonce.h): patches the adoption's entry and one call; then fakes (no later test reads them).
    FlowOverlapsRun(game);
    // AnimTeardownParallel (lab prototype): dying NPC animation data's interpolators on helper threads (selftest_animpar.h).
    apar::Run(game);
    // CellBufferPacedEvict (lab prototype): spare cell buffer slots, overflow torn down one cell per OT frame (selftest_cellbuf.h); after
    // the ledger's test, which detours the same grid-change entry. RowPreload first: it reads CellBufferPacedEvict's real
    // sites, which the cell buffer test replaces with fakes.
    rpl::RowPreloadSelfTest(game);
    cbuf::Run(game);
    // HdrMenuRows: the engine helpers it calls (selftest_hdrmenu.h).
    hdrm::Run(game);
    // HdrGamma22 (selftest_hdrgamma.h).
    hdrg::Run(game);
    // HdrMenuBackground (selftest_hdrmenubg.h).
    hmbg::Run(game);
    // HdrSaveThumbnail (selftest_hdrthumb.h).
    hthumb::Run(game);

    // VramCounterFix (selftest_vramcounter.h).
    vramc::Run(game);
    // GpuPassMarkers (discovery only, nothing installed) and the GPU crash report's new parts with fakes (selftest_hangcapture.h).
    hangcap::Run(game);
    // ShaderScreenAfterDriverChange (selftest_shaderscreen.h).
    shs::Run(game);

    // RestoreFsrAfterAltTab: FSR back on when a menu that was up at focus gain closes (the game's own focus-gained branch, run again).
    {
        Check(ApplyFocusRestoreFix(true), "focus restore: focus handler and menu-visibility function found and detoured");
        FocusRestoreInfoOut fr{};
        Check(FocusRestoreInfo(&fr), "focus restore: info");
        if (Installed(fr.handler != nullptr && fr.setter != nullptr && fr.fsrCvarSlot != nullptr && fr.altarFsrCvarSlot != nullptr,
                      "RestoreFsrAfterAltTab (focus restore)")) {
            Check(fr.handler == game.base + 0x495EE00, "focus handler at exe+0x495EE00");
            Check(fr.setter == game.base + 0x4729660, "menu-visibility function at exe+0x4729660");
            Check(fr.fsrCvarSlot == reinterpret_cast<void**>(game.base + 0x930AAC8), "the handler's static r.FidelityFX.FSR3.Enabled at exe+0x930AAC8");
            Check(fr.altarFsrCvarSlot == reinterpret_cast<void**>(game.base + 0x930A268), "Altar.FSR3.Enabled at exe+0x930A268");
            Check(fr.getBoolOffset == 0x88 && fr.menuFlagOffset == 0xE2, "GetBool at vtable+0x88, bIsMenuVisible at +0xE2 (handler and setter agree)");
            const auto entryJumpsTo = [](uint8_t* fn, void* hook) {
                if (fn[0] != 0xE9) return false;
                int32_t rel = 0;
                memcpy(&rel, fn + 1, 4);
                uint8_t* stub = fn + 5 + rel;
                void* target = nullptr;
                memcpy(&target, stub + 6, sizeof target);
                return MatchesAt(stub, ParsePattern("FF 25 00 00 00 00")) && target == hook;
            };
            Check(entryJumpsTo(fr.handler, fr.handlerHook), "handler entry: jmp to a stub that jumps to the hook");
            Check(entryJumpsTo(fr.setter, fr.setterHook), "setter entry: jmp to a stub that jumps to the hook");
            Check(MatchesAt(fr.handler + 5, ParsePattern("57 48 83 EC 20")) && MatchesAt(fr.setter + 5, ParsePattern("48 89 6C 24 10")),
                  "only the first instruction was replaced");
            const auto trampolineOk = [](uint8_t* t, uint8_t* fn) {
                void* back = nullptr;
                memcpy(&back, t + 11, sizeof back);
                return MatchesAt(t, ParsePattern("48 89 5C 24 08 FF 25 00 00 00 00")) && back == fn + 5;
            };
            Check(trampolineOk(fr.handlerTrampoline, fr.handler) && trampolineOk(fr.setterTrampoline, fr.setter),
                  "trampolines: mov [rsp+8],rbx, then back to +5");
            Check(ApplyFocusRestoreFix(true), "second application finds it installed");

            // behaviour, with fakes for the game's two functions, the two console variables and the freeze subsystem
            Check(fr.getFlagsOffset == 0x18 && fr.setOffset == 0x80, "GetFlags at vtable+0x18, Set at vtable+0x80 (gain and loss branches agree)");
            Check(fr.one && fr.one == reinterpret_cast<const wchar_t*>(game.base + 0x6E0F864) && fr.one[0] == L'1' && fr.one[1] == 0,
                  "the handler's own L\"1\" at exe+0x6E0F864");
            Check(fr.freezeAvailable, "freeze subsystem available (named freezes = no menu holds the world)");
            g_fakeCvarVtable[0x88 / sizeof(void*)] = reinterpret_cast<void*>(&FakeGetBool);
            g_fakeCvarVtable[0x18 / sizeof(void*)] = reinterpret_cast<void*>(&FakeGetFlags);
            g_fakeCvarVtable[0x80 / sizeof(void*)] = reinterpret_cast<void*>(&FakeSet);
            *fr.fsrCvarSlot = &g_fakeFsr;
            *fr.altarFsrCvarSlot = &g_fakeAltarFsr;
            FadeHookTestFunctions(nullptr, nullptr, reinterpret_cast<void*>(+[](void*) -> void* { return g_fakeFreezeThere ? g_fakeFreeze : nullptr; }));
            FocusRestoreTestOriginals(reinterpret_cast<void*>(&FakeFocusHandler), reinterpret_cast<void*>(&FakeMenuSetter));
            using Handler = uint64_t (*)(void*, uint8_t);
            using Setter = uint64_t (*)(void*, uint8_t, void*, void*, uint64_t);
            const auto focus = [&](bool active) { reinterpret_cast<Handler>(fr.handlerHook)(g_fakeManager, active ? 1 : 0); };
            const auto menu = [&](bool visible) { reinterpret_cast<Setter>(fr.setterHook)(g_fakeUi, visible ? 1 : 0, nullptr, nullptr, 6); };
            const auto tick = [&] { reinterpret_cast<TIMERPROC>(fr.timerProc)(nullptr, WM_TIMER, 0, 0); };
            const auto st = [&] { FocusRestoreInfoOut i{}; FocusRestoreInfo(&i); return i; };
            const auto freezes = [&](int32_t n) { *reinterpret_cast<int32_t*>(g_fakeFreeze + 0xB0) = n; };
            const auto staleFlag = [&](bool set) { g_fakeUi[g_fakeMenuFlag] = set ? 1 : 0; };   // the game leaves it set after most menus
            g_fakeFsr.value = g_fakeAltarFsr.value = true;
            freezes(0);
            staleFlag(false);
            focus(false);
            Check(!g_fakeFsr.value && st().pending, "focus lost: the game switches FSR off; a restore is pending");
            focus(true);
            Check(g_fakeFsr.value && !st().pending && g_fakeFsr.sets == 0 && !st().timerRunning,
                  "focus regained, menu flag clear: the game switches it on; nothing left pending, no timer");
            // the reported case (2026-10-07): a player menu was opened and closed before; the game left its menu flag set
            staleFlag(true);
            focus(false);
            focus(true);
            Check(g_fakeFsr.value && st().restoredDirect == 1 && g_fakeFsr.sets == 1 && g_fakeFsr.lastSetBy == 0x0A000000 &&
                      !st().pending && !st().timerRunning,
                  "alt-tab in play, menu flag left set, no menu open: FSR on at once, Set(\"1\", GetFlags() & set-by mask) as the handler does");
            // a menu open at the gain (it holds a named freeze), closed later without clearing the flag (inventory, map, wait)
            freezes(1);
            focus(false);
            focus(true);
            Check(!g_fakeFsr.value && st().pending && st().timerRunning, "regained with a menu open: FSR stays off (the game's behaviour), timer runs");
            tick();
            tick();
            Check(!g_fakeFsr.value && st().pending && st().timerChecks == 2, "timer checks while the menu is open: nothing");
            menu(true);
            Check(!g_fakeFsr.value && st().pending, "a menu shown again (nested): still off");
            freezes(0);
            tick();
            Check(g_fakeFsr.value && st().restoredDirect == 2 && !st().pending && !st().timerRunning,
                  "menu closed (flag left set): the next timer check switches FSR on and stops the timer");
            // pause menu: the game clears its flag when it closes -> the game's own focus-gain branch, at once
            staleFlag(true);
            freezes(1);
            focus(false);
            focus(true);
            const int before = g_fakeHandlerCalls;
            freezes(0);
            menu(false);
            Check(g_fakeFsr.value && st().restoreCalls == 1 && st().restored == 1 && g_fakeHandlerCalls == before + 1 &&
                      g_fakeHandlerThis == g_fakeManager && !st().timerRunning,
                  "pause menu closed (flag cleared): the game's handler runs once more with 'gained' on its own object, FSR on");
            menu(false);
            tick();
            Check(st().restoreCalls == 1 && st().restoredDirect == 2, "further menu closes and ticks call nothing");
            // the player applied the upscaler again in the menu
            menu(true);
            freezes(1);
            focus(false);
            focus(true);
            g_fakeFsr.value = true;
            tick();
            Check(!st().pending && !st().timerRunning && st().restoreCalls == 1 && st().restoredDirect == 2,
                  "FSR switched on by the menu meanwhile: nothing called, nothing pending");
            // another upscaler chosen in the menu: the player's choice wins
            focus(false);
            focus(true);
            g_fakeAltarFsr.value = false;
            freezes(0);
            tick();
            Check(!g_fakeFsr.value && !st().pending && !st().timerRunning && st().restoredDirect == 2 && st().restoreCalls == 1,
                  "another upscaler chosen in the menu: left off, nothing pending");
            g_fakeAltarFsr.value = true;
            g_fakeFsr.value = true;
            // menu closed while the window is in the background
            menu(true);
            freezes(1);
            focus(false);
            freezes(0);
            menu(false);
            Check(!g_fakeFsr.value && !st().timerRunning && st().restoreCalls == 1,
                  "menu closed while the window is in the background: nothing (FSR stays off there)");
            focus(true);
            Check(g_fakeFsr.value && !st().pending, "then regained without a menu: the game's own switch-on");
            // freeze subsystem not readable: only the pause/main menu path (the older behaviour)
            staleFlag(true);
            g_fakeFreezeThere = false;
            focus(false);
            focus(true);
            tick();
            Check(!g_fakeFsr.value && st().pending, "freeze subsystem not readable: waits (no guess)");
            g_fakeFreezeThere = true;
            tick();
            Check(g_fakeFsr.value && !st().pending && !st().timerRunning, "readable again: restored by the next check");
            // switched off live: the game's behaviour
            SetFocusRestoreEnabled(false);
            focus(false);
            focus(true);
            tick();
            menu(false);
            Check(!g_fakeFsr.value && !st().timerRunning, "switched off: the game's behaviour (FSR stays off, no timer)");
            SetFocusRestoreEnabled(true);
            staleFlag(false);
            focus(false);
            focus(true);
            Check(g_fakeFsr.value, "switched on again; the next alt-tab as before");
            *fr.fsrCvarSlot = nullptr;
            *fr.altarFsrCvarSlot = nullptr;
            LogFocusRestoreStats();
        }
    }

    // MenuOpenFlashFix (issue #5, game bug): occlusion culling off for the player menu's open frame (RequestCapture ->
    // the same frame's ProcessScreenShots). After RestoreFsrAfterAltTab's test (the console-variable offsets come from its search).
    {
        Check(InstallMenuOpenFlashFix(true), "menu-open flash: RequestCapture, ProcessScreenShots and r.AllowOcclusionQueries found");
        MenuOpenFlashInfoOut mf{};
        if (Installed(MenuOpenFlashInfo(&mf), "MenuOpenFlashFix (menu-open flash)")) {
            Check(mf.request == game.base + 0x492A120, "RequestCapture at exe+0x492A120");
            Check(mf.jmpSite == game.base + 0x4620C55, "the exec thunk's jmp at exe+0x4620C55");
            Check(mf.processShots == game.base + 0x3222CE0, "ProcessScreenShots at exe+0x3222CE0");
            Check(mf.numShotSlots == 3 && mf.shotSlots[0] == reinterpret_cast<void**>(game.base + 0x767AA68) &&
                      mf.shotSlots[1] == reinterpret_cast<void**>(game.base + 0x7868C18) &&
                      mf.shotSlots[2] == reinterpret_cast<void**>(game.base + 0x7B1B2F8),
                  "its three vtable slots exe+0x767AA68, 0x7868C18, 0x7B1B2F8");
            bool slotsHooked = true;
            for (int i = 0; i < mf.numShotSlots; ++i) slotsHooked &= *mf.shotSlots[i] == mf.shotsHook;
            Check(slotsHooked, "every ProcessScreenShots slot now holds the hook");
            Check(mf.cvarSlot == reinterpret_cast<void**>(game.base + 0x91D4C80), "r.AllowOcclusionQueries' IConsoleVariable* at exe+0x91D4C80");
            Check(mf.cvarRef == reinterpret_cast<const bool*>(game.base + 0x8DD2395), "the bool it binds at exe+0x8DD2395");
            const auto stubTo = [](uint8_t* stub, void* hook) {
                void* target = nullptr;
                memcpy(&target, stub + 6, sizeof target);
                return MatchesAt(stub, ParsePattern("FF 25 00 00 00 00")) && target == hook;
            };
            Check(mf.jmpSite[0] == 0xE9 && stubTo(RipTarget(mf.jmpSite, 1, 5), mf.requestHook), "the thunk jumps to a stub to the request hook");
            Check(InstallMenuOpenFlashFix(true), "second installation finds it installed");

            // behaviour with a fake console variable (GetFlags / Set / GetBool / GetInt / GetFloat) and fake originals
            static FakeCvar occ{g_fakeCvarVtable, true, 0x04000020, 0, 0};  // set by the system settings ini, render-thread safe
            static void* occSlot = nullptr;
            static int requests = 0, shotsCalls = 0;
            static bool occAtRequest = true;
            static int fakeIntBias = 0;
            g_fakeCvarVtable[0x88 / sizeof(void*)] = reinterpret_cast<void*>(&FakeGetBool);
            g_fakeCvarVtable[0x18 / sizeof(void*)] = reinterpret_cast<void*>(&FakeGetFlags);
            g_fakeCvarVtable[0x80 / sizeof(void*)] = reinterpret_cast<void*>(&FakeSet);
            g_fakeCvarVtable[0x90 / sizeof(void*)] =
                reinterpret_cast<void*>(+[](void* c) -> int32_t { return (static_cast<FakeCvar*>(c)->value ? 1 : 0) + fakeIntBias; });
            g_fakeCvarVtable[0x98 / sizeof(void*)] =
                reinterpret_cast<void*>(+[](void* c) -> float { return static_cast<FakeCvar*>(c)->value ? 1.0f : 0.0f; });
            const auto setup = [&](uint32_t setOff) {
                occSlot = &occ;
                MenuOpenFlashTestSetup(&occSlot, &occ.value, reinterpret_cast<void*>(+[](void*) { ++requests; occAtRequest = occ.value; }),
                                       reinterpret_cast<void*>(+[](void*, void*) -> bool { ++shotsCalls; return true; }), setOff, 0x88);
            };
            using Request = void (*)(void*);
            using Shots = bool (*)(void*, void*);
            const auto request = [&] { reinterpret_cast<Request>(mf.requestHook)(nullptr); };
            const auto shots = [&] { return reinterpret_cast<Shots>(mf.shotsHook)(nullptr, nullptr); };
            const auto st = [&] { MenuOpenFlashInfoOut i{}; MenuOpenFlashInfo(&i); return i; };
            setup(0x80);
            SetMenuOpenFlashFix(true);
            request();
            Check(requests == 1 && occ.value && occ.sets == 0, "no ProcessScreenShots seen yet (restore point not live): nothing switched");
            Check(shots() && shotsCalls == 1, "ProcessScreenShots: the game's result passed through");
            request();
            Check(requests == 2 && !occ.value && !occAtRequest && occ.lastSetBy == 0x04000000 && occ.sets == 1 && st().armed,
                  "request: occlusion off before the game's request, with the variable's own set-by priority");
            shots();
            Check(occ.value && occ.sets == 2 && !st().armed, "that frame's ProcessScreenShots: occlusion back on");
            request();
            request();
            Check(!occ.value && occ.sets == 5 && st().armed, "two requests in a row: the second restores first, then switches off again");
            shots();
            Check(occ.value && occ.sets == 6, "... and the next ProcessScreenShots switches it back on");
            occ.value = false;
            request();
            shots();
            Check(!occ.value && occ.sets == 6, "occlusion already off (player or ini): left alone");
            occ.value = true;
            HANDLE th = CreateThread(nullptr, 0, [](void* p) -> DWORD { reinterpret_cast<void (*)(void*)>(p)(nullptr); return 0; },
                                     mf.requestHook, 0, nullptr);
            if (th) {
                WaitForSingleObject(th, INFINITE);
                CloseHandle(th);
            }
            Check(occ.value && occ.sets == 6 && !st().armed, "a request off the game thread: nothing switched (the game's call still runs)");
            SetMenuOpenFlashFix(false);
            shots();
            request();
            Check(occ.value && occ.sets == 6, "switched off: the game's behaviour");
            // the interface checks: a wrong GetInt, a Set that does nothing, flags without ECVF_RenderThreadSafe
            SetMenuOpenFlashFix(true);
            setup(0x80);
            fakeIntBias = 5;
            shots();
            request();
            Check(occ.value && occ.sets == 6 && st().broken, "GetInt disagrees with the bound bool: off for the session, nothing set");
            fakeIntBias = 0;
            g_fakeCvarVtable[0xA0 / sizeof(void*)] = reinterpret_cast<void*>(+[](void*, const wchar_t*, int32_t) {});
            setup(0xA0);  // a "Set" slot that does nothing
            shots();
            request();
            Check(occ.value && st().broken && !st().armed, "Set has no effect: off for the session, not armed");
            setup(0x80);
            occ.flags = 0x04000000;
            shots();
            request();
            Check(occ.value && occ.sets == 6 && st().broken, "no ECVF_RenderThreadSafe in the flags: off for the session");
            occ.flags = 0x04000020;
            setup(0x80);
            occSlot = nullptr;
            shots();
            request();
            Check(occ.value && !st().broken, "variable not registered yet: nothing switched, the game's call still runs");
            // a stale restore point: ProcessScreenShots not seen for over 1 s (e.g. a viewport class that overrides the slot)
            setup(0x80);
            shots();
            MenuOpenFlashTestAgeRestorePoint(1100);
            request();
            Check(occ.value && !st().armed && !st().broken, "restore point stale (no ProcessScreenShots for over 1 s): nothing switched");
            shots();
            request();
            Check(!occ.value && st().armed, "... live again after the next ProcessScreenShots: switched off for the open frame");
            shots();
            Check(occ.value && !st().armed && !st().broken, "... and back on at that frame's ProcessScreenShots");
            // the Broken() path: three restores in a row that do not take: off for the session, the variable left as it
            // is, and the log line names that value
            setup(0x80);
            shots();
            request();
            Check(!occ.value && st().armed, "restore failures: armed (occlusion off for the open frame)");
            g_fakeCvarVtable[0x80 / sizeof(void*)] = reinterpret_cast<void*>(+[](void*, const wchar_t*, int32_t) {});
            shots();
            shots();
            Check(!occ.value && st().armed && !st().broken, "two restores that did not take: still armed, retried at the next frame");
            shots();
            Check(!occ.value && !st().armed && st().broken && strstr(MenuOpenFlashLastBrokenLog(), "left at 0") != nullptr,
                  "the third: off for the session, the variable left at 0 and named in the log line");
            g_fakeCvarVtable[0x80 / sizeof(void*)] = reinterpret_cast<void*>(&FakeSet);
            const int setsBefore = occ.sets;
            shots();
            request();
            Check(!occ.value && occ.sets == setsBefore && !st().armed, "... nothing switched or restored after that (left as it is)");
            occ.value = true;
            // a failed interface check names the value too (1: the game's)
            setup(0x80);
            fakeIntBias = 5;
            shots();
            request();
            fakeIntBias = 0;
            Check(occ.value && st().broken && strstr(MenuOpenFlashLastBrokenLog(), "left at 1") != nullptr,
                  "interface check failed: the log line names the value left (1, the game's)");
            SetMenuOpenFlashFix(false);
            LogMenuOpenFlashStats();
        }
    }

    // LumenKeepArrayCapacity and UploadMemoryKeep (stream churn): the five shrinking calls in FLumenSceneData::ResetAndConsolidate and the three
    // upload heap clean-up calls in FD3D12Adapter::EndFrame (build 19115871), redirected to stubs; hooks tried on fakes.
    if (Installed(ApplyLumenKeepCapacityFix(true), "lumen keep-capacity fix applied") &&
        Installed(ApplyUploadMemoryKeepFix(true), "upload memory keep fix applied")) {
        StreamChurnFixInfo sc{};
        Check(StreamChurnInfo(&sc), "stream churn info available");
        const uint32_t lumenCalls[5] = {0x1F8BBA6, 0x1F8BC1A, 0x1F8BC8E, 0x1F8BCF4, 0x1F8BD61};
        const uint32_t lumenTargets[5] = {0x1F87810, 0xDD1D70, 0x1F8C3A0, 0x1F87750, 0xFD9590};
        bool sitesOk = true, redirected = true;
        for (int i = 0; i < 5; ++i) {
            sitesOk &= sc.lumenCall[i] == game.base + lumenCalls[i] && sc.lumenOriginal[i] == game.base + lumenTargets[i] &&
                       sc.lumenIsRemoveAt[i] == (i == 0 || i == 3);
            redirected &= RipTarget(sc.lumenCall[i], 1, 5) != sc.lumenOriginal[i];
        }
        Check(sitesOk, "lumen: 5 shrinking calls at the expected sites (RemoveAt x2, ResizeShrink x3)");
        Check(redirected, "lumen: all 5 calls redirected");
        Check(sc.uploadCall[0] == game.base + 0x24FFE01 && sc.uploadCall[1] == game.base + 0x24FFE21 &&
                  sc.uploadCall[2] == game.base + 0x24FFE3F && sc.buddyClean == game.base + 0x24F3C50 &&
                  sc.poolClean == game.base + 0x251B2A0,
              "upload: EndFrame clean-up calls and their targets found");
        Check(RipTarget(sc.uploadCall[0], 1, 5) == RipTarget(sc.uploadCall[2], 1, 5) &&
                  RipTarget(sc.uploadCall[0], 1, 5) != sc.buddyClean && RipTarget(sc.uploadCall[1], 1, 5) != sc.poolClean,
              "upload: 3 calls redirected (the two buddy clean-ups share one stub)");
        Check(sc.minPages && sc.minPagesOriginal == 5, "upload: d3d12.FastAllocator.MinPagesToRetain found (5)");

        StreamChurnTestOriginals(reinterpret_cast<void*>(&FakeRemoveAt), reinterpret_cast<void*>(&FakeShrink),
                                 reinterpret_cast<void*>(&FakeBuddyClean), reinterpret_cast<void*>(&FakePoolClean),
                                 reinterpret_cast<void*>(&FakeResizeGrow));
        Check(sc.lumenGrowCall == game.base + 0x1F97902 && sc.lumenGrowOriginal == game.base + 0xE79870 &&
                  RipTarget(sc.lumenGrowCall, 1, 5) != sc.lumenGrowOriginal,
              "lumen: PrimitiveGroups grow call found (exe+0x1F97902 -> exe+0xE79870) and redirected");
        using RemoveAtFn = void (*)(void*, int32_t, int32_t, bool);
        using ShrinkFn = void (*)(void*);
        using BuddyFn = void (*)(void*, uint64_t);
        using PoolFn = void (*)(void*, uint64_t, bool);
        SetLumenKeepCapacityEnabled(true);
        struct FakeArray { void* data; int32_t num, max; } fakeGroups{nullptr, 10, 4096}, fakeHeightfields{nullptr, 3, 64};
        reinterpret_cast<RemoveAtFn>(sc.removeAtHook0)(&fakeGroups, 10, 5, true);
        reinterpret_cast<ShrinkFn>(sc.shrinkHook1)(&fakeHeightfields);
        Check(g_fakeRemoveAtCalls == 1 && !g_fakeRemoveAtShrink && g_fakeShrinkCalls == 0,
              "lumen on: RemoveAt called without shrinking, ResizeShrink skipped");
        SetLumenKeepCapacityEnabled(false);
        reinterpret_cast<RemoveAtFn>(sc.removeAtHook0)(&fakeGroups, 10, 5, true);
        reinterpret_cast<ShrinkFn>(sc.shrinkHook1)(&fakeHeightfields);
        Check(g_fakeRemoveAtCalls == 2 && g_fakeRemoveAtShrink && g_fakeShrinkCalls == 1, "lumen off: both as the game calls them");
        using GrowFn = void (*)(void*, int32_t);
        FakeTArray grow{nullptr, 1001, 1000};
        reinterpret_cast<GrowFn>(sc.growHook)(&grow, 1000);
        Check(grow.num == 1001 && grow.max == 1001 + 3 * 1001 / 8 + 16 && g_fakeGrowSawNum == 1001,
              "lumen off: PrimitiveGroups grows as the game grows it");
        SetLumenKeepCapacityEnabled(true);
        grow = {nullptr, 1001, 1000};
        reinterpret_cast<GrowFn>(sc.growHook)(&grow, 1000);
        Check(grow.num == 1001 && grow.max >= 1990 && grow.max <= 2020 && g_fakeGrowOldNum == 1000,
              "lumen on: PrimitiveGroups grows to ~2x its capacity, Num and the copied count unchanged");
        // the session's peak is now at least 1001; a new small scene that reaches an eighth of a big peak jumps to it
        FakeTArray big{nullptr, 400000, 399999};
        reinterpret_cast<GrowFn>(sc.growHook)(&big, 399999);  // peak 400000
        FakeTArray fresh{nullptr, 60000, 59999};
        reinterpret_cast<GrowFn>(sc.growHook)(&fresh, 59999);
        Check(fresh.num == 60000 && fresh.max >= 490000 && fresh.max <= 510000,
              "lumen on: a new scene at 1/8 of the session's peak grows straight to the peak + 1/4");
        // LumenGroupsPrior (default 2,600,000: 1/32 = 81,250). The session's peak is set to 400,000 here (as the cases above
        // leave it), so without the prior a grow from 1/8 of it goes to the peak + 1/4 (500,000). The commit room and the
        // available commit come from the self-test override.
        {
            constexpr int64_t kPrior = 2600000;
            constexpr int kSpareMB = 2048;  // 2 x MemoryReliefCommitMB
            constexpr int64_t kBlockMB = (kPrior * 80 + (1 << 20) - 1) >> 20;  // 199 (198.4 MiB, rounded up)
            const int64_t needMB = kBlockMB + kSpareMB;
            LumenGrowTestSetPeak(400000);
            SetLumenGroupsPrior(kPrior, kSpareMB);
            LumenPriorTestCommitRoom(100000, -1);
            auto growTo = [&](int32_t num) {
                FakeTArray arr{nullptr, num, num - 1};
                reinterpret_cast<GrowFn>(sc.growHook)(&arr, num - 1);
                return arr;
            };
            auto viaRule = [](const FakeTArray& t) { return t.max >= 490000 && t.max <= 510000; };   // peak + 1/4
            auto viaPrior = [](const FakeTArray& t) { return t.max >= 2590000 && t.max <= 2610000; };
            const FakeTArray under = growTo(81249);  // one under 1/32: the policy above
            const FakeTArray at = growTo(81250);     // exactly 1/32: straight to the prior
            const bool boundary = viaRule(under) && viaPrior(at) && at.num == 81250 && g_fakeGrowOldNum == 81249;
            Check(boundary, "LumenGroupsPrior: one entry under 1/32 the grow policy as before, at exactly 1/32 straight to the "
                            "prior (Num and the copied count unchanged)");
            if (!boundary) printf("    under %d at %d\n", under.max, at.max);
            LumenPriorTestCommitRoom(needMB - 2, -1);  // commit room under the block + 2 GB: doubled as before
            const FakeTArray tight = growTo(81251);
            LumenPriorTestCommitRoom(needMB, -1);
            const FakeTArray roomy = growTo(81251);
            Check(viaRule(tight) && viaPrior(roomy), "LumenGroupsPrior: skipped while the commit room is under the block + 2 GB, "
                                                     "taken from there");
            if (!(viaRule(tight) && viaPrior(roomy))) printf("    tight %d roomy %d\n", tight.max, roomy.max);
            LumenPriorTestCommitRoom(100000, kBlockMB - 2);  // room through page file growth only: Windows would grow it mid-copy
            const FakeTArray growth = growTo(81251);
            LumenPriorTestCommitRoom(100000, kBlockMB);
            const FakeTArray avail = growTo(81251);
            Check(viaRule(growth) && viaPrior(avail), "LumenGroupsPrior: skipped while the available commit (without page file "
                                                      "growth) is under the block, taken from there");
            if (!(viaRule(growth) && viaPrior(avail))) printf("    growth %d avail %d\n", growth.max, avail.max);
            LumenPriorTestCommitRoom(-2, -1);  // no memory sample yet
            Check(viaRule(growTo(81251)), "LumenGroupsPrior: skipped while the commit room is not known yet");
            SetLumenGroupsPrior(kPrior, kSpareMB);  // logs the last decision (as the monitor thread does once a second)
            LumenPriorTestCommitRoom(100000, -1);
            SetLumenGroupsPrior(0, kSpareMB);
            Check(viaRule(growTo(81251)), "LumenGroupsPrior=0: the grow policy as before (0.12.0-1.2.4)");
            SetLumenGroupsPrior(kPrior, kSpareMB);
            SetLumenKeepCapacityEnabled(false);  // the ini, MemoryRelief's commit level or a shared-memory GPU
            const FakeTArray stock = growTo(81251);
            Check(stock.max == 81251 + 3 * 81251 / 8 + 16, "LumenGroupsPrior: with LumenKeepArrayCapacity off (or under MemoryRelief) "
                                                         "the game's own grow");
            SetLumenKeepCapacityEnabled(true);
            const FakeTArray late = growTo(2600001);  // a grow past the prior: doubles (never a smaller block than before)
            Check(late.num == 2600001 && late.max >= 5150000 && late.max <= 5250000,
                  "LumenGroupsPrior: a grow past the prior doubles as before");
            if (!(late.max >= 5150000 && late.max <= 5250000)) printf("    late %d\n", late.max);
            SetLumenGroupsPrior(0, kSpareMB);
            LumenPriorTestCommitRoom(-1, -1);
            LumenGrowTestSetPeak(400000);  // as before this block
        }

        SetUploadMemoryKeep(true, 3600, 64, 0);
        reinterpret_cast<BuddyFn>(sc.buddyHook)(nullptr, 20);
        reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        Check(g_fakeBuddyLag == 3600 && g_fakePoolLag == 3600 && *sc.minPages == 64, "upload on: lag 3600 frames, 64 pages");
        SetUploadMemoryKeep(true, 0, 64, 0);
        reinterpret_cast<BuddyFn>(sc.buddyHook)(nullptr, 20);
        Check(g_fakeBuddyLag == 0xFFFFFFFFu, "upload on, 0 frames: kept for the session");
        SetUploadMemoryKeep(true, 3600, 64, 0);
        reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 0, true);
        Check(g_fakePoolLag == 0 && g_fakePoolForce, "upload on: a forced clean-up passes through");
        SetUploadMemoryKeep(false, 3600, 64, 0);
        reinterpret_cast<BuddyFn>(sc.buddyHook)(nullptr, 20);
        reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        Check(g_fakeBuddyLag == 20 && g_fakePoolLag == 20 && *sc.minPages == 5, "upload off: the game's 20 frames and 5 pages");
        *sc.minPages = 7;  // someone else (Engine.ini) set it while the fix is off: left alone
        SetUploadMemoryKeep(false, 3600, 64, 0);
        Check(*sc.minPages == 7, "upload off: a value set by someone else is left alone");
        SetUploadMemoryKeep(true, 3600, 64, 0);
        Check(*sc.minPages == 64, "upload on again: 64 pages");
        // Switched off after keeping, the lag ramps down to the game's 20 over the ramp (oldest pools first).
        SetUploadMemoryKeep(true, 0, 64, 900);
        for (int i = 0; i < 100; ++i) reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        SetUploadMemoryKeep(false, 0, 64, 900);
        reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        const uint64_t rampStart = g_fakePoolLag;
        reinterpret_cast<BuddyFn>(sc.buddyHook)(nullptr, 20);
        Check(rampStart > 3600 && rampStart != 0xFFFFFFFFu && g_fakeBuddyLag == rampStart,
              "upload off after keeping: the lag starts above the session's age (nothing released yet)");
        for (int i = 0; i < 449; ++i) reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        Check(g_fakePoolLag < rampStart * 6 / 10 && g_fakePoolLag > rampStart * 4 / 10, "upload ramp: half way after 450 of 900 frames");
        for (int i = 0; i < 460; ++i) reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        Check(g_fakePoolLag == 20 && LogContains("[upload] kept upload memory handed back over"),
              "upload ramp: the game's 20 frames after 900 frames, end logged");
        SetUploadMemoryKeep(true, 0, 64, 900);
        for (int i = 0; i < 10; ++i) reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        SetUploadMemoryKeep(false, 0, 64, 900);
        for (int i = 0; i < 10; ++i) reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        SetUploadMemoryKeep(true, 0, 64, 900);
        reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        Check(g_fakePoolLag == 0xFFFFFFFFu, "upload ramp: switched on again mid-ramp: kept for the session at once");
        SetUploadMemoryKeep(false, 0, 64, 0);
        reinterpret_cast<PoolFn>(sc.poolHook)(nullptr, 20, false);
        Check(g_fakePoolLag == 20, "upload: UploadReleaseRampFrames=0 hands everything back at once, as the game");
        SetUploadMemoryKeep(true, 3600, 64, 0);
        LogStreamChurnStats();
    }

    // NpcCreationPacing: NPC creation pacing. Sites first (build 19115871), then the scheduler against a simulated pairing queue.
    if (Installed(FindNpcPacing(), "npc pacing sites found")) {
        NpcPacingInfoOut pi{};
        Check(NpcPacingInfo(&pi), "npc pacing info available");
        auto at = [&](const void* p, uint32_t rva) { return static_cast<const uint8_t*>(p) == game.base + rva; };
        Check(at(pi.runSession, 0x4591010) && at(pi.runSessionSlot, 0x7B027C0), "pacing: RunSession exe+0x4591010, slot exe+0x7B027C0");
        Check(at(pi.handlerEnd, 0x4590CC0) && at(pi.handlerEndSlot, 0x7B02788), "pacing: Handler_End exe+0x4590CC0, slot exe+0x7B02788");
        Check(at(pi.sessionVtable, 0x7B02700) && at(pi.findSession, 0x4593930), "pacing: session vtable exe+0x7B02700, FindSession exe+0x4593930");
        Check(at(pi.createExec, 0x657C8D0) && at(pi.createSlot, 0x8616B88), "pacing: create command exe+0x657C8D0, slot exe+0x8616B88");
        Check(at(pi.genericExec, 0x4919D40) && pi.numGenericSlots == 3, "pacing: generic command exe+0x4919D40, 3 slots");
        Check(at(pi.preTickReturn, 0x4899E17) && at(pi.postTickReturn, 0x4899B2D), "pacing: pre-tick call returns to exe+0x4899E17, post-tick to exe+0x4899B2D");
        Check(at(pi.tes, 0x941F748), "pacing: TES global exe+0x941F748");
        Check(at(pi.delegateVtable[0], 0x7D76998) && at(pi.delegateExecIfSafe[0], 0x4852870) && pi.delegateSize[0] == 0x28 &&
                  !pi.delegateCopyCtor[0], "pacing: appearance delegate vtable exe+0x7D76998, size 0x28, plain copy");
        Check(at(pi.delegateVtable[1], 0x7D7F190) && at(pi.delegateExecIfSafe[1], 0x48B7A30) && pi.delegateSize[1] == 0x30 &&
                  !pi.delegateCopyCtor[1], "pacing: equipment delegate vtable exe+0x7D7F190, size 0x30, plain copy");
        Check(at(pi.delegateVtable[2], 0x7D72370) && at(pi.delegateExecIfSafe[2], 0x47FBC10) && pi.delegateSize[2] == 0x90 &&
                  at(pi.delegateCopyCtor[2], 0x47EB6E0), "pacing: hair delegate vtable exe+0x7D72370, size 0x90, copy constructor exe+0x47EB6E0");
        Check(at(pi.weakGet, 0x1126290), "pacing: FWeakObjectPtr::Get exe+0x1126290");
        Check(pi.hairActorWeak == 0x20,
              "pacing: PacingHairActor: the hair lambda tests and Gets the capture's first weak pointer (its mesh component), +0x20");
        Check(at(pi.getWorld, 0x37E3350) && at(pi.gEngine, 0x927E7D8) && at(pi.getPc, 0x31ABE90) && at(pi.getPawn, 0x357D290),
              "pacing: GetCurrentPlayWorld, GEngine, GetPlayerController, GetPawnOrSpectator");
        Check(pi.rootOffset == 0x1A0 && pi.locationOffset == 0x280, "pacing: RootComponent +0x1A0, location +0x280");
        Check(at(pi.maxTickRate, 0x37FD1A0) && pi.numMaxTickSlots == 2, "pacing: UpdateTimeAndHandleMaxTickRate exe+0x37FD1A0, 2 slots");
        Check(at(pi.flowReturn[0], 0x4881C55) && at(pi.flowReturn[1], 0x4881E74) && at(pi.chooseWriter, 0x4592810),
              "pacing: level flow RunSession calls return to exe+0x4881C55 / exe+0x4881E74, writer session exe+0x4592810");
        // FrozenClothStretchFix: actual mapped-EXE call discovery + trampoline ABI, then engine-facing flag semantics.
        Check(!InstallFrozenClothStretchFix(false), "clothlod: refuses late installation");
        Check(InstallFrozenClothStretchFix(true), "clothlod: uniquely verifies and installs the LOD call");
        if (FrozenClothTestCallSite()) {
            auto* site = static_cast<uint8_t*>(FrozenClothTestCallSite());
            Check(site == game.base + 0x53CF40B, "clothlod: only WriteSimulationData's zero-time update is patched");
            static uint8_t component[0x950]{}, proxy[0xA0]{}, solver[0x90]{}, testCloths[3][0x50]{};
            static uint8_t* items[3]{testCloths[0],testCloths[1],testCloths[2]};
            static int calls = 0, seenReset[3]{}, seenTeleport[3]{};
            static float seenDt = 0;
            struct A { uint8_t** data; int32_t num,max; };
            *reinterpret_cast<void**>(proxy+0x58)=component;
            *reinterpret_cast<void**>(proxy+0x78)=solver;
            *reinterpret_cast<void**>(component+0x940)=proxy;
            *reinterpret_cast<A*>(proxy+0x90)={items,3,3};
            *reinterpret_cast<A*>(solver+0x70)={items,3,3};
            component[0x918]=0x0D;
            auto fake = +[](uint8_t* s,float dt) {
                ++calls; seenDt=dt;
                for(int i=0;i<3;++i) {
                    seenReset[i]=testCloths[i][0x44]; seenTeleport[i]=testCloths[i][0x45];
                    testCloths[i][0x44]=testCloths[i][0x45]=0; // engine consumes both, reset has precedence
                }
                (void)s;
            };
            FrozenClothTestOriginal(reinterpret_cast<void*>(fake));
            SetFrozenClothStretchFix(false);
            FrozenClothTestCall(solver,0,proxy);
            Check(calls==1 && !seenTeleport[0], "clothlod: disabled passes through exactly once");
            SetFrozenClothStretchFix(true);
            testCloths[1][0x44]=1; testCloths[2][0x45]=1;
            FrozenClothTestCall(solver,0,proxy);
            Check(calls==2 && seenTeleport[0]==1 && seenReset[1]==1 && !seenTeleport[1] && seenTeleport[2]==1,
                  "clothlod: handles multiple cloths, preserves reset and existing teleport");
            Check(!testCloths[0][0x45] && !testCloths[1][0x44] && !testCloths[2][0x45], "clothlod: leaves flag consumption to engine");
            component[0x918]=5;
            FrozenClothTestCall(solver,0,proxy);
            Check(calls==3 && !seenTeleport[0], "clothlod: parent-pose bit alone is not suspension; running cloth untouched");
            component[0x918]=13;
            FrozenClothTestCall(solver,0.016f,proxy);
            Check(calls==4 && seenDt==0.016f && !seenTeleport[0], "clothlod: positive physics delta untouched");
            *reinterpret_cast<void**>(proxy)=solver;
            FrozenClothTestCall(solver,0,proxy);
            Check(!seenTeleport[0], "clothlod: never mutates during an outstanding task");
            *reinterpret_cast<void**>(proxy)=nullptr;
            items[2]=nullptr;
            FrozenClothTestCall(solver,0,proxy);
            Check(!seenTeleport[0], "clothlod: validates every cloth before first mutation");
            items[2]=testCloths[2];
            reinterpret_cast<A*>(solver+0x70)->num=2;
            FrozenClothTestCall(solver,0,proxy);
            Check(!seenTeleport[0], "clothlod: mismatched ownership falls back");
            reinterpret_cast<A*>(solver+0x70)->num=3;
            // Invoke the installed stub with the caller's real r14 convention, rcx solver and xmm1 delta.
            uint8_t code[]{0x41,0x56,0x48,0x83,0xEC,0x20,0x4D,0x8B,0xF0,0x48,0xB8,
                0,0,0,0,0,0,0,0,0xFF,0xD0,0x48,0x83,0xC4,0x20,0x41,0x5E,0xC3};
            void* stub=RipTarget(site,1,5); memcpy(code+11,&stub,8);
            void* thunk=VirtualAlloc(nullptr,sizeof code,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if(thunk) {
                memcpy(thunk,code,sizeof code); DWORD old;
                const bool executable=VirtualProtect(thunk,sizeof code,PAGE_EXECUTE_READ,&old)!=0;
                Check(executable,"clothlod: ABI test thunk executable");
                if(executable) {
                    FlushInstructionCache(GetCurrentProcess(),thunk,sizeof code);
                    reinterpret_cast<void(*)(uint8_t*,float,uint8_t*)>(thunk)(solver,0,proxy);
                    Check(seenTeleport[0]==1 && seenTeleport[1]==1 && seenTeleport[2]==1 && seenDt==0,
                          "clothlod: actual trampoline forwards solver, delta and caller proxy correctly");
                }
                VirtualFree(thunk,0,MEM_RELEASE);
            } else Check(false,"clothlod: ABI test allocation");
            SetFrozenClothStretchFix(false);
        }
        // NpcFadeInNoRebuild: the fade-in value setter (exe+0x3044B00), redirected at start-up; the hook against fakes
        Check(!InstallNpcFadeInNoRebuild(false), "fade: refuses to install after game code started");
        Check(InstallNpcFadeInNoRebuild(true) && game.base[0x3044B00] == 0xE9, "fade: the float setter exe+0x3044B00 now jumps to the hook");
        {
            NpcFadeInInfoOut fi{};
            NpcFadeInInfo(&fi);
            Check(fi.floatOff == 0x318 && fi.boolOff == 0x316 && fi.proxyFloatOff == 0x80 && fi.proxyBoolOff == 0x7D,
                  "fade: component +0x318 / +0x316, proxy +0x80 / +0x7D");
            Check(fi.markDirty == game.base + 0x2FE7980 && fi.getWorld == game.base + 0x2FE58E0 && fi.sceneOff == 0x1E0 && fi.updateSlot == 0x58,
                  "fade: MarkRenderStateDirty exe+0x2FE7980, GetWorld exe+0x2FE58E0, world+0x1E0 scene, UpdateCustomPrimitiveData +0x58");
            static int dirties = 0, updates = 0;
            alignas(16) static uint8_t comp[0x800] = {}, proxy[0x100] = {}, world[0x200] = {};
            static void* sceneVt[16] = {};
            static void* scene[1] = {sceneVt};
            sceneVt[0x58 / 8] = reinterpret_cast<void*>(static_cast<void (*)(void*, uint8_t*)>([](void*, uint8_t*) { ++updates; }));
            *reinterpret_cast<void**>(world + 0x1E0) = scene;
            *reinterpret_cast<uint8_t**>(comp + 0x600) = proxy;
            auto mark = static_cast<void (*)(uint8_t*)>([](uint8_t*) { ++dirties; });
            auto getWorld = static_cast<uint8_t* (*)(uint8_t*)>([](uint8_t*) -> uint8_t* { return world; });
            NpcFadeInTestSetup(reinterpret_cast<void*>(mark), reinterpret_cast<void*>(getWorld), 0x600);
            comp[0x8C] = 3; comp[0x316] = 1; proxy[0x7D] = 1;
            *reinterpret_cast<float*>(comp + 0x318) = 0.5f;
            SetNpcFadeInNoRebuild(false);
            NpcFadeInTestCall(comp, 0.5f);
            Check(dirties == 0 && updates == 0, "fade: an unchanged value does nothing");
            NpcFadeInTestCall(comp, 0.4f);
            Check(dirties == 1 && updates == 0 && *reinterpret_cast<float*>(comp + 0x318) == 0.4f, "fade off: a changed value recreates (the game's behaviour)");
            SetNpcFadeInNoRebuild(true);
            dirties = 0;
            NpcFadeInTestCall(comp, 0.3f);
            Check(dirties == 0 && updates == 1 && *reinterpret_cast<float*>(proxy + 0x80) == 0.3f && *reinterpret_cast<float*>(comp + 0x318) == 0.3f,
                  "fade on: a changed value goes to the proxy and the GPU scene, no recreate");
            proxy[0x7D] = 0;
            NpcFadeInTestCall(comp, 0.2f);
            Check(dirties == 1 && updates == 1, "fade on: a proxy built with the other bool value recreates");
            proxy[0x7D] = 1; comp[0x8C] = 0x83; dirties = 0;
            NpcFadeInTestCall(comp, 0.1f);
            Check(dirties == 1 && updates == 1, "fade on: a render state already marked dirty goes the game's way");
            comp[0x8C] = 1; dirties = 0;
            NpcFadeInTestCall(comp, 0.05f);
            Check(dirties == 1 && updates == 1, "fade on: no render state: the game's way");
            comp[0x8C] = 3; *reinterpret_cast<uint8_t**>(comp + 0x600) = nullptr; dirties = 0;
            NpcFadeInTestCall(comp, 0.0f);
            Check(dirties == 1 && updates == 1, "fade on: no scene proxy: the game's way");
            SetNpcFadeInNoRebuild(false);
        }
        // PhysAnimNoEmptyLock (public since 1.2.3): UpdateTargetActors' entry (exe+0x354EA80) redirected; the trampoline runs
        // the game's prologue; the hook against a fake game function
        Check(!InstallPhysAnimNoEmptyLock(false), "physanim: refuses to install after game code started");
        Check(InstallPhysAnimNoEmptyLock(true) && game.base[0x354EA80] == 0xE9, "physanim: UpdateTargetActors exe+0x354EA80 now jumps to the hook");
        {
            static int gameCalls = 0;
            static uint8_t lastTeleport = 0xFF;
            alignas(16) static uint8_t comp[0x200] = {};
            auto fakeGame = [](uint8_t*, uint8_t tt) { ++gameCalls; lastTeleport = tt; };
            void* fg = reinterpret_cast<void*>(static_cast<void (*)(uint8_t*, uint8_t)>(fakeGame));
            int32_t* const num = reinterpret_cast<int32_t*>(comp + 0xE0);  // DriveData.Num in build 19115871
            SetPhysAnimNoEmptyLock(false);
            *num = 0; gameCalls = 0;
            PhysAnimTestCall(comp, 0, fg);
            Check(gameCalls == 1 && lastTeleport == 0, "physanim off: no driven body still runs the game's code");
            SetPhysAnimNoEmptyLock(true);
            gameCalls = 0;
            PhysAnimTestCall(comp, 0, fg);
            PhysAnimTestCall(comp, 1, fg);
            Check(gameCalls == 0, "physanim on: no driven body skips the game's code (tick and teleport)");
            *num = 3;
            PhysAnimTestCall(comp, 1, fg);
            Check(gameCalls == 1 && lastTeleport == 1, "physanim on: driven bodies run the game's code with its teleport type");
            *num = -1; gameCalls = 0;
            PhysAnimTestCall(comp, 0, fg);
            Check(gameCalls == 0, "physanim on: a negative count is no driven body");
            SetPhysAnimNoEmptyLock(false);
            *num = 0;
            PhysAnimTestCall(comp, 0, fg);
            Check(gameCalls == 1, "physanim off again: the game's code");
            // a mesh with a pending parallel animation task: the game's code (GetBoneSpaceTransforms completes it there)
            Check(PhysAnimTestOffset(0) == 0xE0 && PhysAnimTestOffset(1) == 0xC0 && PhysAnimTestOffset(2) == 0xDC0,
                  "physanim: offsets read from the code (DriveData.Num +0xE0, mesh +0xC0, pending task +0xDC0)");
            alignas(16) static uint8_t mesh[0x1000] = {};
            *reinterpret_cast<uint8_t**>(comp + 0xC0) = mesh;
            SetPhysAnimNoEmptyLock(true);
            *num = 0; gameCalls = 0;
            PhysAnimTestCall(comp, 0, fg);
            Check(gameCalls == 0, "physanim on: a mesh without a pending task, no driven body: skipped");
            *reinterpret_cast<void**>(mesh + 0xDC0) = mesh;  // any non-null task
            PhysAnimTestCall(comp, 1, fg);
            Check(gameCalls == 1 && lastTeleport == 1, "physanim on: a pending parallel animation task runs the game's code");
            *reinterpret_cast<void**>(mesh + 0xDC0) = nullptr;
            *reinterpret_cast<uint8_t**>(comp + 0xC0) = nullptr;
            SetPhysAnimNoEmptyLock(false);
            // the trampoline: the game's 9-byte prologue, then jmp [rip+0] to UpdateTargetActors + 9
            const uint8_t* const tr = PhysAnimTestTrampoline();
            static const uint8_t prologue[9] = {0x88, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xEC, 0x60};
            Check(tr && memcmp(tr, prologue, 9) == 0 && tr[9] == 0xFF && tr[10] == 0x25 &&
                      *reinterpret_cast<const int32_t*>(tr + 11) == 0 &&
                      *reinterpret_cast<uint8_t* const*>(tr + 15) == game.base + 0x354EA80 + 9,
                  "physanim: trampoline = the original prologue + an absolute jump to UpdateTargetActors + 9");
            LogPhysAnimNoEmptyLockStats();
        }
        // ChaosMaxPayloadSize (public since 1.2.3, sqprobe): p.MaxPayloadSize's bound value found once on the mapped exe (its registration and
        // a read in the code), the game's 100000.0f; set on both start-up routes, only over the game's value
        {
            float* const v = FindChaosMaxPayloadVar();
            Check(v && *v == 100000.0f, "chaosmps: p.MaxPayloadSize's value found (registration + a read in the code), the game's 100000");
            if (v) {
                Check(!SetChaosMaxPayloadSize(0, true) && !SetChaosMaxPayloadSize(50000, true) && *v == 100000.0f,
                      "chaosmps: 0 and values up to 100000 leave the game's value");
                Check(SetChaosMaxPayloadSize(10000000, true) && *v == 10000000.0f, "chaosmps: 10000000 set (version.dll route)");
                Check(!SetChaosMaxPayloadSize(20000000, true) && *v == 10000000.0f,
                      "chaosmps: a value that is no longer the game's 100000 is left alone");
                LogChaosMaxPayloadSizeLive();
                *v = 100000.0f;  // .data of the mapped image is writable, as in the game
                Check(SetChaosMaxPayloadSize(10000000, false) && *v == 10000000.0f, "chaosmps: 10000000 set (OBSE64 route)");
                *v = 100000.0f;
                LogChaosMaxPayloadSizeLive();
            }
        }
        // PerceptionLosPrefetch: the perception round, its line-of-sight call site and the helper found and checked
        // (discovery only; no writes); then the batch and the serve with a fake helper and fake pawns
        {
            if (!g_perclos.captured) PerclosCapture(game);
            const uint32_t rr = g_perclos.rr, rc = g_perclos.rc, rh = g_perclos.rh, rf = g_perclos.rf;
            const char* why = g_perclos.why;
            if (why) printf("      perclos: %s\n", why);
            Check(!why, "perclos: the perception round, its line-of-sight call site and the helper found and checked");
            Check(rr == 0x485FAF4, "perclos: the perception round's list and settings calls at exe+0x485FAF4");
            Check(rc == 0x485FCDC, "perclos: the line-of-sight call site at exe+0x485FCDC");
            Check(rh == 0x485A170, "perclos: the line-of-sight helper at exe+0x485A170");
            Check(rf == 0x485A900, "perclos: the field-of-view filter (its getter, acos call and end checked) at exe+0x485A900");

            // fake class {chain at +0x30 -> [cls+0x30 at depth 1], depth +0x38 = 1}; pawns of 0x800 bytes: class +0x10,
            // paired +0x7B8 (-> +0x120 non-zero), root +0x1A0 (-> location doubles at +0x280)
            alignas(16) static uint8_t cls[0x100] = {}, other[0x100] = {}, paired[0x200] = {};
            alignas(16) static uint8_t roots[6][0x300] = {};
            alignas(16) static uint8_t pawns[6][0x800] = {};
            static const uint8_t* chain[2];
            chain[0] = nullptr;
            chain[1] = cls + 0x30;
            const void* chainPtr = chain;
            memcpy(cls + 0x30, &chainPtr, 8);
            const int depth = 1, otherDepth = 1;
            memcpy(cls + 0x38, &depth, 4);
            static const uint8_t* otherChain[2] = {nullptr, other + 0x30};
            const void* otherChainPtr = otherChain;
            memcpy(other + 0x30, &otherChainPtr, 8);
            memcpy(other + 0x38, &otherDepth, 4);
            const uint64_t one = 1;
            memcpy(paired + 0x120, &one, 8);
            // pawn 0 = the NPC at the origin; 1..3 in range (100, 200, 300 units); 4 far (5000); 5 of another class
            const double xs[6] = {0, 100, 200, 300, 5000, 50};
            for (int i = 0; i < 6; ++i) {
                const uint8_t* c = i == 5 ? other : cls;
                memcpy(pawns[i] + 0x10, &c, 8);
                const uint8_t* pp = paired;
                memcpy(pawns[i] + 0x7B8, &pp, 8);
                const uint8_t* rp = roots[i];
                memcpy(pawns[i] + 0x1A0, &rp, 8);
                memcpy(roots[i] + 0x280, &xs[i], 8);
            }
            static uint8_t* listData[6];
            for (int i = 0; i < 6; ++i) listData[i] = pawns[i];
            alignas(16) static uint8_t list[16] = {};
            const void* ld = listData;
            const int ln = 6;
            memcpy(list, &ld, 8);
            memcpy(list + 8, &ln, 4);
            // range: a = 10, b = 50, constant 1: (a*1*b) * (a*b) = 250000 = 500 units squared
            alignas(16) static uint8_t settings[0x500] = {};
            const float ra = 10.0f, rb = 50.0f;
            memcpy(settings + 0x4E8, &ra, 4);
            memcpy(settings + 0x4EC, &rb, 4);
            static std::atomic<int> helperCalls{0};
            static std::atomic<int> salt{0};
            auto fakeHelper = [](uint8_t*, uint8_t* o, uint8_t* out, int tries, uint8_t flag) -> uint8_t {
                helperCalls.fetch_add(1);
                *out = static_cast<uint8_t>(((reinterpret_cast<uintptr_t>(o) >> 11) & 1) + static_cast<uint8_t>(salt.load()));
                return static_cast<uint8_t>(tries == 3 && flag == 1);
            };
            auto fakeClass = []() -> const uint8_t* { return cls; };
            void* fh = reinterpret_cast<void*>(static_cast<uint8_t (*)(uint8_t*, uint8_t*, uint8_t*, int, uint8_t)>(fakeHelper));
            void* fc = reinterpret_cast<void*>(static_cast<const uint8_t* (*)()>(fakeClass));
            uint64_t s0, v0, d0, f0, j0, s1, v1, d1, f1, j1;
            // on, 2 helper threads: pawns 1..3 precomputed (the NPC itself, the far pawn and the other class are not)
            PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 1, 2, 2);
            PerceptionLosTestCounts(&s0, &v0, &d0, &f0, &j0);
            helperCalls = 0;
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            PerceptionLosTestCounts(&s1, &v1, &d1, &f1, &j1);
            Check(j1 == j0 + 3 && helperCalls == 3, "perclos on: the three pawns in range of the NPC's class are precomputed");
            // the game's loop asks pawns 2, 1, 3 (any order) then the far pawn: three served, the far one runs the helper
            bool same = true;
            for (int k : {2, 1, 3}) {
                uint8_t out = 3, ref = 3;
                const uint8_t r = PerceptionLosTestCall(pawns[0], pawns[k], &out, 3, 1);
                const uint8_t rref = fakeHelper(pawns[0], pawns[k], &ref, 3, 1);
                same = same && r == rref && out == ref;
            }
            uint8_t outFar = 3;
            PerceptionLosTestCall(pawns[0], pawns[4], &outFar, 3, 1);
            PerceptionLosTestCounts(&s1, &v1, &d1, &f1, &j1);
            Check(same && s1 == s0 + 3 && f1 == f0 + 1 && helperCalls == 3 + 3 + 1,
                  "perclos on: precomputed answers served bit for bit, a pawn not precomputed runs the game's helper");
            // a second ask of the same pawn, other arguments, another NPC: the game's helper
            uint8_t o2 = 3, o3 = 3, o4 = 3, o5 = 2;
            PerceptionLosTestCall(pawns[0], pawns[1], &o2, 3, 1);
            PerceptionLosTestCall(pawns[0], pawns[2], &o3, 2, 1);
            PerceptionLosTestCall(pawns[1], pawns[3], &o4, 3, 1);
            PerceptionLosTestCall(pawns[0], pawns[3], &o5, 3, 1);
            PerceptionLosTestCounts(&s1, &v1, &d1, &f1, &j1);
            Check(s1 == s0 + 3 && f1 == f0 + 5, "perclos on: a repeat, other tries, another NPC or another result byte run the game's helper");
            // too few pawns in range for a batch: nothing precomputed, every call runs the helper
            PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 1, 4, 2);
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            uint8_t o6 = 3;
            PerceptionLosTestCall(pawns[0], pawns[1], &o6, 3, 1);
            uint64_t j2, s2, f2;
            PerceptionLosTestCounts(&s2, nullptr, nullptr, &f2, &j2);
            Check(j2 == j1 && s2 == s1 && f2 == f1 + 1, "perclos: below PerceptionLosPrefetchMin no batch, the game's helper runs");
            // our review of PR #55: the lightweight-instance guard. A subsystem listing a manager leaves the round to the game
            // (nothing precomputed, the call runs the helper); an empty list or no subsystem batches as before.
            {
                const uint32_t eyeRva = g_perclos.eye, lwiRva = g_perclos.lwi;
                Check(eyeRva == 0x4858700, "perclos guard: the NPC eye point (vtable +0xA98) at exe+0x4858700");
                Check(lwiRva == 0x925FD50, "perclos guard: the lightweight-instance subsystem's static at exe+0x925FD50 (getter, fetch "
                                           "and manager lookup checked)");
                alignas(16) static uint8_t fakeSub[0x20] = {};
                static uint8_t* fakeStatic = nullptr;
                fakeStatic = fakeSub;
                const int32_t managers = 1;
                memcpy(fakeSub + 8, &managers, 4);
                PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 1, 2, 2);
                PerceptionLosTestSetLwi(&fakeStatic);
                const uint64_t lw0 = PerceptionLosTestLwiRounds();
                uint64_t ja, sa, fa, jb, sb, fb;
                PerceptionLosTestCounts(&sa, nullptr, nullptr, &fa, &ja);
                PerceptionLosTestPrefetch(pawns[0], list, settings);
                uint8_t og = 3;
                PerceptionLosTestCall(pawns[0], pawns[1], &og, 3, 1);
                PerceptionLosTestCounts(&sb, nullptr, nullptr, &fb, &jb);
                Check(jb == ja && sb == sa && fb == fa + 1 && PerceptionLosTestLwiRounds() == lw0 + 1,
                      "perclos guard: with a lightweight-instance manager listed the round runs as in the game");
                const int32_t none = 0;
                memcpy(fakeSub + 8, &none, 4);
                PerceptionLosTestPrefetch(pawns[0], list, settings);
                PerceptionLosTestCounts(&sb, nullptr, nullptr, &fb, &jb);
                fakeStatic = nullptr;
                PerceptionLosTestPrefetch(pawns[0], list, settings);  // no subsystem yet: batched
                uint64_t jc;
                PerceptionLosTestCounts(nullptr, nullptr, nullptr, nullptr, &jc);
                Check(jb == ja + 3 && jc == jb + 3 && PerceptionLosTestLwiRounds() == lw0 + 1,
                      "perclos guard: with no manager (or no subsystem yet) the round is batched");
                PerceptionLosTestSetLwi(nullptr);
                // the eye-point check: an NPC whose vtable +0xA98 is not the checked eye point runs its round as in the game
                {
                    alignas(16) static uint8_t eyeVt[0xB00] = {};
                    static uint8_t okEye = 1, otherEye = 2;  // distinct objects (not foldable constants)
                    const void* pe = &otherEye;
                    memcpy(eyeVt + 0xA98, &pe, 8);
                    const void* vtp = eyeVt;
                    uint8_t savedVt[8];
                    memcpy(savedVt, pawns[0], 8);
                    memcpy(pawns[0], &vtp, 8);
                    PerceptionLosTestSetEye(&okEye);
                    const uint64_t oe0 = PerceptionLosTestOtherEyeRounds();
                    uint64_t je0, fe0, je1, fe1, se0, se1;
                    PerceptionLosTestCounts(&se0, nullptr, nullptr, &fe0, &je0);
                    PerceptionLosTestPrefetch(pawns[0], list, settings);
                    uint8_t oeye = 3;
                    PerceptionLosTestCall(pawns[0], pawns[1], &oeye, 3, 1);
                    PerceptionLosTestCounts(&se1, nullptr, nullptr, &fe1, &je1);
                    Check(je1 == je0 && se1 == se0 && fe1 == fe0 + 1 && PerceptionLosTestOtherEyeRounds() == oe0 + 1,
                          "perclos eye check: an NPC with another eye point (vtable +0xA98) runs its round as in the game");
                    pe = &okEye;
                    memcpy(eyeVt + 0xA98, &pe, 8);
                    PerceptionLosTestPrefetch(pawns[0], list, settings);
                    PerceptionLosTestCounts(nullptr, nullptr, nullptr, nullptr, &je1);
                    Check(je1 == je0 + 3 && PerceptionLosTestOtherEyeRounds() == oe0 + 1,
                          "perclos eye check: the checked eye point is batched");
                    PerceptionLosTestSetEye(nullptr);
                    memcpy(pawns[0], savedVt, 8);
                }
                // leave the open batch closed (the prefetches above used the non-overlapped path)
                PerceptionLosTestPrefetch(nullptr, nullptr, nullptr);
                PerceptionLosTestCounts(&s2, nullptr, nullptr, &f2, &j2);
            }
            // verify: the game's helper runs, its answer is kept and compared (the salt makes pawn 2 differ)
            PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 2, 2, 2);
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            salt = 0;
            uint8_t ov1 = 3;
            const uint8_t rv1 = PerceptionLosTestCall(pawns[0], pawns[1], &ov1, 3, 1);
            salt = 4;
            uint8_t ov2 = 3;
            PerceptionLosTestCall(pawns[0], pawns[2], &ov2, 3, 1);
            salt = 0;
            uint64_t s3, v3, d3;
            PerceptionLosTestCounts(&s3, &v3, &d3, nullptr, nullptr);
            Check(rv1 == 1 && s3 == s2 && v3 == v1 + 2 && d3 == d1 + 1 && ov2 >= 4,
                  "perclos verify: the game's helper runs, its answer is kept, a different answer is counted");
            // off: nothing precomputed, every call runs the helper
            PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 0, 2, 2);
            uint64_t j4;
            PerceptionLosTestCounts(nullptr, nullptr, nullptr, nullptr, &j4);
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            uint8_t o7 = 3;
            PerceptionLosTestCall(pawns[0], pawns[1], &o7, 3, 1);
            uint64_t s5, j5;
            PerceptionLosTestCounts(&s5, nullptr, nullptr, nullptr, &j5);
            Check(j5 == j4 && s5 == s3, "perclos off: nothing precomputed or served");
            // the field-of-view filter: when the NPC's vtable slot +0xA90 holds it, a pawn out of view (here pawn 2) is not
            // precomputed, and the loop's helper call for it (if any) runs the game's helper; another filter: all precomputed
            alignas(16) static uint8_t fakeVt[0xB00] = {};
            auto fakeFov = [](uint8_t*, uint8_t* o) -> uint8_t { return o != pawns[2]; };
            void* ff = reinterpret_cast<void*>(static_cast<uint8_t (*)(uint8_t*, uint8_t*)>(fakeFov));
            memcpy(fakeVt + 0xA90, &ff, 8);
            const uint8_t* fvt = fakeVt;
            memcpy(pawns[0], &fvt, 8);
            PerceptionLosTestSetup(fh, fc, ff, 1.0f, 1, 2, 2);
            uint64_t j6, j7, s6, s7, f6, f7;
            PerceptionLosTestCounts(&s6, nullptr, nullptr, &f6, &j6);
            helperCalls = 0;
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            uint8_t o8 = 3, o9 = 3;
            PerceptionLosTestCall(pawns[0], pawns[1], &o8, 3, 1);
            PerceptionLosTestCall(pawns[0], pawns[2], &o9, 3, 1);
            PerceptionLosTestCounts(&s7, nullptr, nullptr, &f7, &j7);
            Check(j7 == j6 + 2 && s7 == s6 + 1 && f7 == f6 + 1 && helperCalls == 3,
                  "perclos field of view: a pawn out of view is not precomputed (its call runs the game's helper)");
            static uint8_t otherFilter = 0;
            PerceptionLosTestSetup(fh, fc, &otherFilter, 1.0f, 1, 2, 2);
            PerceptionLosTestCounts(nullptr, nullptr, nullptr, nullptr, &j6);
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            PerceptionLosTestCounts(nullptr, nullptr, nullptr, nullptr, &j7);
            uint64_t f8, f9;
            PerceptionLosTestCounts(nullptr, nullptr, nullptr, &f8, nullptr);
            uint8_t o10 = 3;
            PerceptionLosTestCall(pawns[0], pawns[1], &o10, 3, 1);
            PerceptionLosTestCounts(nullptr, nullptr, nullptr, &f9, nullptr);
            Check(j7 == j6 && f9 == f8 + 1,
                  "perclos field of view: a round with another filter is not precomputed (its calls run the game's helper)");
            memset(pawns[0], 0, 8);
            // the overlap: the batch is dispatched, the loop's calls wait only for their own answer, the round-end join closes it
            Check(g_perclos.join == 0x485FDF0, "perclos: the round-end join's call site (the loop's exit) at exe+0x485FDF0");
            PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 1, 2, 2);
            PerceptionLosTestSetOverlap(true);
            uint64_t s10, s11, j10, j11;
            PerceptionLosTestCounts(&s10, nullptr, nullptr, nullptr, &j10);
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            bool sameOv = true;
            for (int k : {1, 2, 3}) {
                uint8_t out = 3, ref = 3;
                const uint8_t r = PerceptionLosTestCall(pawns[0], pawns[k], &out, 3, 1);
                const uint8_t rref = fakeHelper(pawns[0], pawns[k], &ref, 3, 1);
                sameOv = sameOv && r == rref && out == ref;
            }
            const bool open1 = PerceptionLosTestJoin(), open2 = PerceptionLosTestJoin();
            PerceptionLosTestCounts(&s11, nullptr, nullptr, nullptr, &j11);
            Check(sameOv && open1 && !open2 && s11 == s10 + 3 && j11 == j10 + 3,
                  "perclos overlap: answers served bit for bit while the batch runs, the round-end join closes it once");
            // a round whose join never came: the next round's start closes the open batch first
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            PerceptionLosTestPrefetch(pawns[0], list, settings);
            const bool open3 = PerceptionLosTestJoin(), open4 = PerceptionLosTestJoin();
            Check(open3 && !open4, "perclos overlap: no batch outlives the next round's start");
            // the ignore-list cache: the helper's two NPC gathers (exe+0x485A260 attached, exe+0x485A2AF child) get a copy of
            // the lists gathered once at the round's start; the other pawn's gathers and any gather after the join run the
            // game's function; verify (mode 2) runs it and compares
            {
                uint32_t ga = 0, gc = 0;
                ga = g_perclos.ga;
                gc = g_perclos.gc;
                Check(ga == 0x485A260 && gc == 0x485A2AF, "perclos cache: the helper's NPC gathers at exe+0x485A260 and exe+0x485A2AF");
                struct Arr { void** data; int32_t num, max; };
                static int attCalls = 0, childCalls = 0;
                auto fakeMalloc = [](size_t n, uint32_t) -> void* { return malloc(n); };
                auto put = [](Arr* out, std::initializer_list<void*> items) {
                    const int n = out->num + static_cast<int>(items.size());
                    out->data = static_cast<void**>(realloc(out->data, static_cast<size_t>(n) * sizeof(void*)));
                    for (void* it : items) out->data[out->num++] = it;
                    out->max = n;
                };
                static void (*s_put)(Arr*, std::initializer_list<void*>) = nullptr;
                s_put = put;
                auto fakeAtt = [](uint8_t* a, void* o, uint8_t) {
                    ++attCalls;
                    s_put(static_cast<Arr*>(o), {a + 1, a + 2});
                };
                auto fakeChild = [](uint8_t* a, void* o, uint8_t, uint8_t) {
                    ++childCalls;
                    static_cast<Arr*>(o)->num = 0;
                    s_put(static_cast<Arr*>(o), {a + 3});
                };
                void* fa = reinterpret_cast<void*>(static_cast<void (*)(uint8_t*, void*, uint8_t)>(fakeAtt));
                void* fch = reinterpret_cast<void*>(static_cast<void (*)(uint8_t*, void*, uint8_t, uint8_t)>(fakeChild));
                void* fm = reinterpret_cast<void*>(static_cast<void* (*)(size_t, uint32_t)>(fakeMalloc));
                PerceptionLosTestSetCache(fa, fch, fm, true);
                PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 1, 2, 2);
                uint64_t cs0, cv0, cd0, cs1, cv1, cd1;
                PerceptionLosTestCacheCounts(&cs0, &cv0, &cd0);
                attCalls = childCalls = 0;
                PerceptionLosTestPrefetch(pawns[0], list, settings);
                const bool built = attCalls == 1 && childCalls == 1;
                Arr a1{}, c1{}, a2{}, a3{};
                PerceptionLosTestGather(pawns[0], &a1, false);
                PerceptionLosTestGather(pawns[0], &c1, true);
                PerceptionLosTestGather(pawns[1], &a2, false);
                PerceptionLosTestCacheCounts(&cs1, &cv1, &cd1);
                Check(built && attCalls == 2 && childCalls == 1 && cs1 == cs0 + 2 && a1.num == 2 && a1.data[0] == pawns[0] + 1 &&
                          a1.data[1] == pawns[0] + 2 && c1.num == 1 && c1.data[0] == pawns[0] + 3 && a2.num == 2 && a2.data[0] == pawns[1] + 1,
                      "perclos cache: the NPC's gathers are copies of the round's lists, the other pawn's run the game's function");
                PerceptionLosTestJoin();
                PerceptionLosTestGather(pawns[0], &a3, false);
                Check(attCalls == 3 && a3.num == 2, "perclos cache: after the round's join the NPC's gather runs the game's function");
                // verify: the game's function runs and is compared (same lists: no difference)
                PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 2, 2, 2);
                PerceptionLosTestPrefetch(pawns[0], list, settings);
                Arr a4{};
                PerceptionLosTestGather(pawns[0], &a4, false);
                uint64_t cs2, cv2, cd2;
                PerceptionLosTestCacheCounts(&cs2, &cv2, &cd2);
                PerceptionLosTestJoin();
                Check(attCalls == 5 && cv2 == cv1 + 1 && cd2 == cd1 && cs2 == cs1 && a4.num == 2,
                      "perclos cache verify: the game's gather runs and matches the round's copy");
                for (Arr* x : {&a1, &c1, &a2, &a3, &a4}) free(x->data);
                PerceptionLosTestSetCache(nullptr, nullptr, nullptr, false);
            }
            // cross-round (experimental): NPCs 0 and 1 run a round each frame, 60 ms apart; in the third frame NPC 0's round
            // also precomputes NPC 1 (expected in that frame), whose round then serves from that batch; the next frame starts anew
            {
                static uint64_t fr = 1;
                auto frameFn = []() -> uint64_t { return fr; };
                PerceptionLosTestSetup(fh, fc, nullptr, 1.0f, 1, 1, 2);
                PerceptionLosTestSetCross(true, static_cast<uint64_t (*)()>(frameFn));
                uint64_t cn0, cr0, cs0x, cn1, cr1, cs1x, cn2, cr2, cs2x;
                PerceptionLosTestCrossCounts(&cn0, &cr0, &cs0x);
                for (fr = 1; fr <= 2; ++fr) {
                    PerceptionLosTestPrefetch(pawns[0], list, settings);
                    PerceptionLosTestPrefetch(pawns[1], list, settings);
                    Sleep(60);
                }
                PerceptionLosTestCrossCounts(&cn1, &cr1, &cs1x);
                fr = 3;
                PerceptionLosTestPrefetch(pawns[0], list, settings);
                PerceptionLosTestPrefetch(pawns[1], list, settings);
                uint8_t ox = 3, rx = 3;
                const uint8_t r = PerceptionLosTestCall(pawns[1], pawns[2], &ox, 3, 1);
                const uint8_t rr2 = fakeHelper(pawns[1], pawns[2], &rx, 3, 1);
                PerceptionLosTestCrossCounts(&cn2, &cr2, &cs2x);
                Check(cn2 >= cn1 + 1 && cr2 == cr1 + 1 && cs2x == cs1x + 1 && r == rr2 && ox == rx,
                      "perclos cross-round: an NPC expected later in the frame is precomputed and its round served from that batch");
                fr = 4;
                PerceptionLosTestPrefetch(pawns[1], list, settings);
                uint64_t cn3, cr3, cs3x;
                PerceptionLosTestCrossCounts(&cn3, &cr3, &cs3x);
                Check(cr3 == cr2, "perclos cross-round: a new frame does not take an older frame's batch");
                PerceptionLosTestSetCross(false, nullptr);
            }
            PerceptionLosTestSetOverlap(false);
        }
        // RigTraceReuse (public since 1.2.3, corral2): the sphere-trace rig unit and UControlRig::Execute found once by pattern
        // (discovery only; no writes); the lab's second world-trace unit (RigTraceReuse2)
        Check(RigTraceUnitDiscover(game) == 0x4B83E90, "rigtrace: the sphere-trace rig unit at exe+0x4B83E90");
        Check(RigTraceExecuteDiscover(game) == 0x4AB6250, "rigtrace: UControlRig::Execute at exe+0x4AB6250");
        {
            // RigTraceReuse with a fake unit and a fake Execute: a repeat with identical inputs inside one execution reuses the
            // first result (bit for bit); a different input, another execution, a call outside any execution, or the key
            // off: the game's unit runs
            static int unitCalls = 0;
            alignas(16) static uint8_t ctx[0x100] = {};
            *reinterpret_cast<uint64_t*>(ctx + 0xE0) = 0x1234;  // world
            auto fakeUnit = [](uint8_t*, const double* s, const double*, const uint8_t*, float r, uint8_t* hit, double* loc, double* nrm) {
                ++unitCalls;
                *hit = 1;
                loc[0] = s[0] + unitCalls; loc[1] = s[1]; loc[2] = r;
                nrm[0] = 0; nrm[1] = 0; nrm[2] = 1;
            };
            void* fu = reinterpret_cast<void*>(static_cast<void (*)(uint8_t*, const double*, const double*, const uint8_t*, float, uint8_t*, double*, double*)>(fakeUnit));
            static void* s_fu = nullptr;
            s_fu = fu;
            static double outA[2][3], outB[2][3];
            static uint8_t hitA = 0, hitB = 0;
            static int mode = 0;
            auto fakeExec = [](void*, void*, void*, void*) -> uint64_t {
                const double st[3] = {1, 2, 3}, en[3] = {1, 2, -100}, st2[3] = {1, 2, 4};
                const uint8_t ch = 3;
                RigTraceTestCall(ctx, st, en, &ch, 10.0f, &hitA, outA[0], outA[1], s_fu);
                if (mode == 0) RigTraceTestCall(ctx, st, en, &ch, 10.0f, &hitB, outB[0], outB[1], s_fu);
                else RigTraceTestCall(ctx, st2, en, &ch, 10.0f, &hitB, outB[0], outB[1], s_fu);
                return 1;
            };
            void* fe = reinterpret_cast<void*>(static_cast<uint64_t (*)(void*, void*, void*, void*)>(fakeExec));
            RigTraceTestSetReuse(true);
            unitCalls = 0; mode = 0;
            const uint64_t r0 = RigTraceTestReused();
            Check(RigTraceTestExecute(fe, nullptr) == 1 && unitCalls == 1 && RigTraceTestReused() == r0 + 1 && hitB == 1 &&
                  memcmp(outA, outB, sizeof outA) == 0, "rigtrace on: an identical repeat in one execution reuses the first result");
            Check(RigTraceTestExecute(fe, nullptr) == 1 && unitCalls == 2 && RigTraceTestReused() == r0 + 2,
                  "rigtrace on: the next execution runs its first trace again");
            mode = 1;
            RigTraceTestExecute(fe, nullptr);
            Check(unitCalls == 4 && RigTraceTestReused() == r0 + 2, "rigtrace on: a different start runs the game's unit");
            const double st[3] = {1, 2, 3}, en[3] = {1, 2, -100};
            const uint8_t ch = 3;
            RigTraceTestCall(ctx, st, en, &ch, 10.0f, &hitA, outA[0], outA[1], fu);
            RigTraceTestCall(ctx, st, en, &ch, 10.0f, &hitB, outB[0], outB[1], fu);
            Check(unitCalls == 6 && RigTraceTestReused() == r0 + 2, "rigtrace on: outside an execution every trace runs");
            RigTraceTestSetReuse(false);
            mode = 0;
            RigTraceTestExecute(fe, nullptr);
            Check(unitCalls == 8 && RigTraceTestReused() == r0 + 2, "rigtrace off: every trace runs");
            // verify mode: the repeat runs the game's unit and is compared (the fake unit's location differs per call)
            RigTraceTestSetMode(2);
            uint64_t d0 = 0, d1 = 0;
            const uint64_t v0 = RigTraceTestVerified(&d0);
            unitCalls = 0; mode = 0;
            RigTraceTestExecute(fe, nullptr);
            Check(unitCalls == 2 && RigTraceTestReused() == r0 + 2 && RigTraceTestVerified(&d1) == v0 + 1 && d1 == d0 + 1 &&
                  outB[0][0] == 3.0, "rigtrace verify: the repeat runs the game's unit, is compared and keeps the game's result");
            // key fields: one mismatch each (context, rig-to-world transform, radius, channel, world), then the identical repeat
            RigTraceTestSetReuse(true);
            static int kmode = 0;
            alignas(16) static uint8_t ctx2[0x100] = {};
            auto keyExec = [](void*, void*, void*, void*) -> uint64_t {
                const double st[3] = {5, 6, 7}, en[3] = {5, 6, -50};
                uint8_t ch = 2;
                float r = 8.0f;
                double l[3], n[3];
                uint8_t h = 0;
                RigTraceTestCall(ctx, st, en, &ch, r, &h, l, n, s_fu);
                if (kmode == 0) {
                    memcpy(ctx2, ctx, sizeof ctx2);
                    RigTraceTestCall(ctx2, st, en, &ch, r, &h, l, n, s_fu);  // another context, same contents
                } else if (kmode == 1) {
                    ctx[0x90] ^= 1;  // the transform's translation
                    RigTraceTestCall(ctx, st, en, &ch, r, &h, l, n, s_fu);
                    ctx[0x90] ^= 1;
                } else if (kmode == 2) {
                    RigTraceTestCall(ctx, st, en, &ch, 8.5f, &h, l, n, s_fu);
                } else if (kmode == 3) {
                    const uint8_t ch2 = 1;
                    RigTraceTestCall(ctx, st, en, &ch2, r, &h, l, n, s_fu);
                } else if (kmode == 4) {
                    *reinterpret_cast<uint64_t*>(ctx + 0xE0) = 0x5678;
                    RigTraceTestCall(ctx, st, en, &ch, r, &h, l, n, s_fu);
                    *reinterpret_cast<uint64_t*>(ctx + 0xE0) = 0x1234;
                } else {
                    RigTraceTestCall(ctx, st, en, &ch, r, &h, l, n, s_fu);
                }
                return 1;
            };
            void* ke = reinterpret_cast<void*>(static_cast<uint64_t (*)(void*, void*, void*, void*)>(keyExec));
            const char* const keyNames[5] = {"rigtrace on: another context runs the game's unit",
                                             "rigtrace on: another rig-to-world transform runs the game's unit",
                                             "rigtrace on: another radius runs the game's unit",
                                             "rigtrace on: another channel runs the game's unit",
                                             "rigtrace on: another world runs the game's unit"};
            for (int k = 0; k < 5; ++k) {
                kmode = k; unitCalls = 0;
                const uint64_t rk = RigTraceTestReused();
                RigTraceTestExecute(ke, nullptr);
                Check(unitCalls == 2 && RigTraceTestReused() == rk, keyNames[k]);
            }
            kmode = 5; unitCalls = 0;
            const uint64_t rk5 = RigTraceTestReused();
            RigTraceTestExecute(ke, nullptr);
            Check(unitCalls == 1 && RigTraceTestReused() == rk5 + 1, "rigtrace on: the identical repeat after them is reused");
            // nested Execute: a repeat in the outer run after an inner Execute returned is not reused
            static void* s_inner = nullptr;
            s_inner = fe;
            auto outerExec = [](void*, void*, void*, void*) -> uint64_t {
                const double st[3] = {9, 9, 9}, en[3] = {9, 9, -9};
                const uint8_t ch = 3;
                double l[3], n[3];
                uint8_t h = 0;
                RigTraceTestCall(ctx, st, en, &ch, 4.0f, &h, l, n, s_fu);
                static void* const self = nullptr;
                RigTraceTestExecute(s_inner, self);  // the inner run (its own two traces)
                RigTraceTestCall(ctx, st, en, &ch, 4.0f, &h, l, n, s_fu);
                return 1;
            };
            void* oe = reinterpret_cast<void*>(static_cast<uint64_t (*)(void*, void*, void*, void*)>(outerExec));
            mode = 0; unitCalls = 0;
            const uint64_t rn = RigTraceTestReused();
            RigTraceTestExecute(oe, nullptr);
            Check(unitCalls == 3 && RigTraceTestReused() == rn + 1,
                  "rigtrace on: after a nested Execute the outer run's repeat runs the game's unit (inner repeat reused)");
            RigTraceTestSetReuse(false);
        }

        // the scheduler, with fakes for every game function it calls
        g_pcSemVtable[2] = reinterpret_cast<void*>(&FakeSemUnlock);
        *reinterpret_cast<void**>(g_pcSession + 0x90) = g_pcSemObj;
        g_pcCreateHook = pi.createHook;
        g_pcGenericHook = pi.genericHook;
        NpcPacingTestFakes f{};
        f.clock = &PcClock;
        f.player = &PcPlayer;
        f.guards = 1;
        f.run = reinterpret_cast<void*>(&FakeRunSession);
        f.end = reinterpret_cast<void*>(&FakeEndHook);
        f.findSession = reinterpret_cast<void*>(&FakeFindSession);
        f.create = reinterpret_cast<void*>(&FakeCmdExec);
        f.generic = reinterpret_cast<void*>(&FakeCmdExec);
        f.weakGet = reinterpret_cast<void*>(&FakeWeakGet);
        f.chooseWriter = reinterpret_cast<void*>(&FakeChooseWriter);
        for (int i = 0; i < 3; ++i) {
            f.execIfSafe[i] = reinterpret_cast<void*>(&FakeDelExec);
            f.dtor[i] = reinterpret_cast<void*>(&FakeDelDtor);
            f.sizeOverride[i] = 0x30;
        }
        f.rootOffset = 0x1A0;
        f.locationOffset = 0x280;
        NpcPacingTestSetup(f);
        SetNpcPacing(true, 4, 4096, 15, false);  // 4 ms, 4096 game units (5851 UE units), 15 frames
        using RunHook = uint8_t (*)(void*, int32_t);
        using EndHook = void (*)(void*, void*, uint64_t, uint64_t);
        using DelHook = bool (*)(void*);
        auto run = reinterpret_cast<RunHook>(pi.runHook);
        auto end = reinterpret_cast<EndHook>(pi.endHook);
        static uint8_t fakeMgr[0x100] = {};
        int maxHeavyPerFrame = 0, maxOwed = 0;
        auto gtFrame = [&]() {
            const size_t before = g_pcRan.size();
            run(fakeMgr, -1);
            end(fakeMgr, nullptr, 2, 0);
            int heavy = 0;
            for (size_t i = before; i < g_pcRan.size(); ++i) heavy += g_pcRan[i] < 1000;
            maxHeavyPerFrame = std::max(maxHeavyPerFrame, heavy);
            const NpcPacingTestState st = NpcPacingTestRead();
            maxOwed = std::max(maxOwed, static_cast<int>(st.S - st.E));
        };
        const float distant = 20000.0f, nearby = 1000.0f;
        // frame 1: warm-up, a 7 ms generic command (learned as heavy), near
        PcPost(kFakeGeneric, 1, 7000, 0, 0x77);
        PcPost(kFakeEnd, 0, 0);
        gtFrame();
        // frame 2: the burst: far creates (10 ms), the heavy generic (7 ms) after a far create, a near create
        PcPost(kFakeCreate, 2, 10000, distant);
        PcPost(kFakeCreate, 3, 10000, distant);
        PcPost(kFakeGeneric, 4, 7000, 0, 0x77);
        PcPost(kFakeGeneric, 1001, 100, 0, 0x99);  // a cheap command of another kind
        PcPost(kFakeCreate, 5, 10000, distant);
        PcPost(kFakeCreate, 1006, 10000, nearby);  // near: runs whatever the budget (not counted as a far unit)
        PcPost(kFakeEnd, 0, 0);
        gtFrame();
        Check(g_pcRan.size() == 2 && g_pcRan[1] == 2, "pacing: burst frame runs only its first unit");
        for (int k = 0; k < 12; ++k) {
            PcPost(kFakeGeneric, 2000 + k, 100, 0, 0x99);
            PcPost(kFakeEnd, 0, 0);
            gtFrame();
        }
        const std::vector<int> want = {1, 2, 3, 4, 1001, 5, 1006};
        Check(g_pcRan.size() >= 7 && std::vector<int>(g_pcRan.begin(), g_pcRan.begin() + 7) == want, "pacing: units run in queue order");
        Check(maxHeavyPerFrame == 1, "pacing: at most one unit per frame (4 ms budget, 7-10 ms units)");
        Check(!g_pcDeadlock, "pacing: never waits for an OT frame that is not posted yet");
        NpcPacingTestState st = NpcPacingTestRead();
        Check(st.S == st.E && g_pcHead == g_pcTail && g_pcSem == 0, "pacing: caught up afterwards, semaphore balanced");
        Check(g_pcRan.size() == 7 + 12, "pacing: every command ran exactly once");
        Check(maxOwed <= 3, "pacing: lag of a few frames only");

        // lag bound: 10 far creates in one OT frame, max lag 2 frames: forced after that
        SetNpcPacing(true, 4, 4096, 2, false);
        const size_t r0 = g_pcRan.size();
        for (int k = 0; k < 10; ++k) PcPost(kFakeCreate, 100 + k, 10000, distant);
        PcPost(kFakeEnd, 0, 0);
        int framesToClear = 0;
        while (framesToClear < 30) {
            gtFrame();
            ++framesToClear;
            if (g_pcRan.size() >= r0 + 10) break;
            PcPost(kFakeEnd, 0, 0);
        }
        printf("      (lag bound: %d frames to clear)\n", framesToClear);
        Check(framesToClear == 6, "pacing: lag bound: budget grows with the lag, the rest runs after 2 x 2 frames of lag");
        Check(!g_pcDeadlock, "pacing: no deadlock with the lag bound");
        for (int k = 0; k < 3; ++k) { PcPost(kFakeEnd, 0, 0); gtFrame(); }
        st = NpcPacingTestRead();
        Check(st.S == st.E && g_pcSem == 0, "pacing: caught up after the lag bound");

        // switched off with a backlog: everything runs in the next frame
        SetNpcPacing(true, 4, 4096, 15, false);
        const size_t r1 = g_pcRan.size();
        for (int k = 0; k < 4; ++k) PcPost(kFakeCreate, 200 + k, 10000, distant);
        PcPost(kFakeEnd, 0, 0);
        gtFrame();
        PcPost(kFakeEnd, 0, 0);
        gtFrame();
        Check(g_pcRan.size() == r1 + 2, "pacing: backlog kept while on");
        SetNpcPacing(false, 4, 4096, 15, false);
        PcPost(kFakeEnd, 0, 0);
        gtFrame();
        st = NpcPacingTestRead();
        Check(g_pcRan.size() == r1 + 4 && st.S == st.E && !g_pcDeadlock, "pacing: switched off: the backlog runs at once");

        // another session (travel) runs: the owed part of the main queue runs first
        SetNpcPacing(true, 4, 4096, 15, false);
        const size_t r2 = g_pcRan.size();
        for (int k = 0; k < 3; ++k) PcPost(kFakeCreate, 300 + k, 10000, distant);
        PcPost(kFakeEnd, 0, 0);
        gtFrame();
        Check(g_pcRan.size() == r2 + 1, "pacing: travel test backlog");
        run(fakeMgr, 7);  // the travel session, before next frame's main queue
        st = NpcPacingTestRead();
        Check(g_pcRan.size() == r2 + 3 && st.S == st.E && g_pcOtherSessionRuns == 1 && !g_pcDeadlock,
              "pacing: travel session: owed main queue first, then the travel session");
        PcPost(kFakeEnd, 0, 0);
        gtFrame();

        // (c) a level flow while the main queue owes an OT frame: the flow waits, and the frame's reverse end command
        // with it, so the OT posts nothing new; the creates run one per frame, then the flow, then the end command.
        // The OT is modelled exactly: its next frame is posted only after a reverse end command.
        {
            const size_t r3 = g_pcRan.size();
            const int ends0 = g_pcRevEnds;
            int maxCreatesPerFrame = 0, waits = 0, framesToFlow = 0;
            for (int k = 0; k < 3; ++k) PcPost(kFakeCreate, 400 + k, 10000, distant);
            PcPost(kFakeEnd, 0, 0);
            bool flowDone = false, heldOk = true;
            while (!flowDone && framesToFlow < 10) {
                const size_t b = g_pcRan.size();
                const int e0 = g_pcRevEnds;
                g_pcWriter[0x4E] = 1;  // Begin
                run(fakeMgr, -1);
                const uint8_t fr = run(fakeMgr, 1001);  // the flow runner, at the start of the post-tick
                if (fr == 3) ++waits;
                else flowDone = true;
                end(fakeMgr, nullptr, 2, 0);
                heldOk &= flowDone ? g_pcRevEnds == e0 + 1 : (g_pcRevEnds == e0 && g_pcWriter[0x4E] == 0);
                int creates = 0;
                for (size_t i = b; i < g_pcRan.size(); ++i) creates += g_pcRan[i] >= 400 && g_pcRan[i] < 500;
                maxCreatesPerFrame = std::max(maxCreatesPerFrame, creates);
                ++framesToFlow;
            }
            Check(flowDone && waits == 2 && framesToFlow == 3 && maxCreatesPerFrame == 1 && heldOk,
                  "pacing: level flow waits 2 frames (end command held, open flag cleared), one create per frame");
            Check(g_pcRan.size() == r3 + 4 && std::vector<int>(g_pcRan.begin() + r3, g_pcRan.end()) == std::vector<int>({400, 401, 402, 1001}),
                  "pacing: level flow runs after the whole OT frame, as in the game");
            Check(g_pcRevEnds == ends0 + 1 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E &&
                      NpcPacingTestRead().heldFrames == 0,
                  "pacing: one reverse end command for the three frames, never read past the OT's posted end");
            // the OT's next frame comes after that end command and runs normally
            PcPost(kFakeGeneric, 2100, 100, 0, 0x99);
            PcPost(kFakeEnd, 0, 0);
            gtFrame();
            Check(g_pcRan.back() == 2100 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: the OT's next frame follows the flow");
            // Six far creates: the queue gets through them one per frame, so the flow keeps waiting past 3 frames
            // and runs after the sixth (before: 3 frames, then the last three drained in one frame)
            const auto flowFrames = [&](int creates, int base, int32_t flowKey, int* frames, size_t* lastFrameRan, int* maxPerFrame) {
                for (int k = 0; k < creates; ++k) PcPost(kFakeCreate, base + k, 10000, distant);
                PcPost(kFakeEnd, 0, 0);
                bool done = false;
                *frames = 0;
                *maxPerFrame = 0;
                while (!done && *frames < 20) {
                    const size_t b = g_pcRan.size();
                    g_pcWriter[0x4E] = 1;
                    run(fakeMgr, -1);
                    done = run(fakeMgr, flowKey) != 3;
                    end(fakeMgr, nullptr, 2, 0);
                    *lastFrameRan = g_pcRan.size() - b;
                    int c = 0;
                    for (size_t i = b; i < g_pcRan.size(); ++i) c += g_pcRan[i] >= base && g_pcRan[i] < base + creates;
                    *maxPerFrame = std::max(*maxPerFrame, c);
                    ++*frames;
                }
                return done;
            };
            const size_t r4 = g_pcRan.size();
            int f2 = 0, perFrame = 0;
            size_t lastFrameRan = 0;
            flowDone = flowFrames(6, 410, 1002, &f2, &lastFrameRan, &perFrame);
            Check(flowDone && f2 == 6 && lastFrameRan == 2 && perFrame == 1 && g_pcRan.size() == r4 + 7 &&
                      g_pcRan.back() == 1002 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: level flow waits while the queue gets through its OT frame (6 creates, one per frame, no drain)");
            PcPost(kFakeEnd, 0, 0);
            gtFrame();
            // the hard bound: ten far creates: the flow waits 8 frames, then the last create drains with the 9th
            const size_t r4b = g_pcRan.size();
            flowDone = flowFrames(10, 430, 1004, &f2, &lastFrameRan, &perFrame);
            Check(flowDone && f2 == 9 && lastFrameRan == 3 && perFrame == 2 && g_pcRan.size() == r4b + 11 &&
                      g_pcRan.back() == 1004 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: level flow waits at most 8 frames, then the queue drains and the flow runs");
            PcPost(kFakeEnd, 0, 0);
            gtFrame();
            // PacingFlowWaitFrames=3: the older behaviour (3 frames, then the last three creates drain in one frame)
            SetNpcPacing(true, 4, 4096, 15, false, true, 3);
            const size_t r4c = g_pcRan.size();
            flowDone = flowFrames(6, 450, 1005, &f2, &lastFrameRan, &perFrame);
            Check(flowDone && f2 == 4 && lastFrameRan == 4 && perFrame == 3 && g_pcRan.size() == r4c + 7 &&
                      g_pcRan.back() == 1005 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: PacingFlowWaitFrames=3: 3 frames, then the queue drains (as 0.17)");
            SetNpcPacing(true, 4, 4096, 15, false);
            PcPost(kFakeEnd, 0, 0);
            gtFrame();
            // PacingFlowProgress (drainfix 2026-11-12), (1): two OT frames of far creates (6 + 4); the first frame runs one
            // and leaves the rest owed, so the flow waits with an older OT frame owed (lag 1) while one create runs per
            // frame. Key 0: that is no progress, so after 3 held frames the drain runs the rest of the first OT frame and
            // the whole second one in one frame; key 1: one create per frame, the flow runs after the last.
            const auto lagFlow = [&](bool on, int base, int32_t flowKey, int* frames, int* maxPerFrame) {
                SetNpcPacingFlowProgress(on);
                for (int k = 0; k < 6; ++k) PcPost(kFakeCreate, base + k, 10000, distant);
                PcPost(kFakeEnd, 0, 0);
                for (int k = 0; k < 4; ++k) PcPost(kFakeCreate, base + 6 + k, 10000, distant);
                PcPost(kFakeEnd, 0, 0);
                const size_t b0 = g_pcRan.size();
                g_pcWriter[0x4E] = 1;
                run(fakeMgr, -1);  // frame 1: one create, the rest owed; no flow yet
                end(fakeMgr, nullptr, 2, 0);
                *maxPerFrame = static_cast<int>(g_pcRan.size() - b0);
                bool done = false;
                *frames = 1;
                while (!done && *frames < 25) {
                    const size_t b = g_pcRan.size();
                    g_pcWriter[0x4E] = 1;
                    run(fakeMgr, -1);
                    done = run(fakeMgr, flowKey) != 3;
                    end(fakeMgr, nullptr, 2, 0);
                    int c = 0;
                    for (size_t i = b; i < g_pcRan.size(); ++i) c += g_pcRan[i] >= base && g_pcRan[i] < base + 10;
                    *maxPerFrame = std::max(*maxPerFrame, c);
                    ++*frames;
                }
                SetNpcPacingFlowProgress(false);
                return done;
            };
            {
                int fOff = 0, pOff = 0, fOn = 0, pOn = 0;
                const size_t q0 = g_pcRan.size();
                const bool dOff = lagFlow(false, 470, 1006, &fOff, &pOff);
                const bool okOff = dOff && g_pcRan.size() == q0 + 11 && g_pcRan.back() == 1006 && !g_pcDeadlock &&
                                   NpcPacingTestRead().S == NpcPacingTestRead().E;
                PcPost(kFakeEnd, 0, 0);
                gtFrame();
                const size_t q1 = g_pcRan.size();
                const bool dOn = lagFlow(true, 490, 1007, &fOn, &pOn);
                std::vector<int> wantOn;
                for (int k = 0; k < 10; ++k) wantOn.push_back(490 + k);
                wantOn.push_back(1007);
                const bool okOn = dOn && std::vector<int>(g_pcRan.begin() + q1, g_pcRan.end()) == wantOn && !g_pcDeadlock &&
                                  NpcPacingTestRead().S == NpcPacingTestRead().E && NpcPacingTestRead().heldFrames == 0;
                printf("      (PacingFlowProgress, older frame owed: key 0 %d frames, max %d creates per frame; key 1 %d frames, max %d)\n",
                       fOff, pOff, fOn, pOn);
                Check(okOff && pOff >= 3, "pacing: PacingFlowProgress=0: with an older OT frame owed the flow drains after 3 frames (as before)");
                Check(okOn && pOn == 1 && fOn == 10,
                      "pacing: PacingFlowProgress=1: creates with an older OT frame owed count as progress: one per frame, then the flow");
                PcPost(kFakeEnd, 0, 0);
                gtFrame();
            }
            // PacingFlowProgress (2): the queue's head create waits for its loads (CreatePreload, faked: 4 offers) while a
            // flow waits. Key 0: no progress, the drain at 3 held frames runs it with its load in flight; key 1: the flow
            // keeps waiting, the create runs in the frame's own run once loaded, then the flow.
            const auto loadFlow = [&](bool on, int id, int32_t flowKey, int* frames) {
                SetNpcPacingFlowProgress(on);
                CreatePreloadTestFake(4);
                PcPost(kFakeCreate, id, 10000, distant);
                PcPost(kFakeEnd, 0, 0);
                bool done = false;
                *frames = 0;
                while (!done && *frames < 20) {
                    g_pcWriter[0x4E] = 1;
                    run(fakeMgr, -1);
                    done = run(fakeMgr, flowKey) != 3;
                    end(fakeMgr, nullptr, 2, 0);
                    ++*frames;
                }
                SetNpcPacingFlowProgress(false);
                const bool ok = done && g_pcRan.size() >= 2 && g_pcRan[g_pcRan.size() - 2] == id && g_pcRan.back() == flowKey;
                CreatePreloadTestFake(0);
                return ok;
            };
            {
                int fOff = 0, fOn = 0;
                const bool okOff = loadFlow(false, 520, 1008, &fOff);
                PcPost(kFakeEnd, 0, 0);
                gtFrame();
                const bool okOn = loadFlow(true, 521, 1009, &fOn);
                printf("      (PacingFlowProgress, head create loading 4 frames: key 0 flow after %d frames, key 1 after %d)\n", fOff, fOn);
                Check(okOff && fOff == 4 && !g_pcDeadlock,
                      "pacing: PacingFlowProgress=0: a create waiting for its loads is no progress: drained at 3 held frames (as before)");
                Check(okOn && fOn == 5 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                      "pacing: PacingFlowProgress=1: the flow waits while the head create loads; it runs in the frame's own run, then the flow");
                PcPost(kFakeEnd, 0, 0);
                gtFrame();
            }
            // CreatePreloadEarly: a far create held back for the budget asks for its loads at once (key 1), not with 0
            {
                SetCreatePreloadEarly(false);
                CreatePreloadTestFake(0);
                for (int k = 0; k < 2; ++k) PcPost(kFakeCreate, 530 + k, 10000, distant);
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);  // 530 runs, 531 is held back for the budget
                end(fakeMgr, nullptr, 2, 0);
                const int asksOff = CreatePreloadTestFake(-1);
                SetCreatePreloadEarly(true);
                for (int k = 0; k < 2; ++k) PcPost(kFakeCreate, 532 + k, 10000, distant);
                PcPost(kFakeEnd, 0, 0);
                int asksOn = 0;
                for (int fr = 0; fr < 4; ++fr) {  // 531; 532 (533 held back: asks); 533; the owed ends
                    if (fr > 0) PcPost(kFakeEnd, 0, 0);
                    run(fakeMgr, -1);
                    end(fakeMgr, nullptr, 2, 0);
                    asksOn += CreatePreloadTestFake(-1);
                }
                SetCreatePreloadEarly(false);
                Check(asksOff == 0 && asksOn >= 1 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                      "pacing: CreatePreloadEarly: a far create held back for the budget requests its loads (key 1 only)");
            }
            // switched off (PacingFlowWait=0): the flow drains the queue at once, as in 0.13.x
            SetNpcPacing(true, 4, 4096, 15, false, false);
            const size_t r5 = g_pcRan.size();
            const int e5 = g_pcRevEnds;
            for (int k = 0; k < 2; ++k) PcPost(kFakeCreate, 420 + k, 10000, distant);
            PcPost(kFakeEnd, 0, 0);
            run(fakeMgr, -1);
            const uint8_t fr = run(fakeMgr, 1003);
            end(fakeMgr, nullptr, 2, 0);
            Check(fr == 4 && g_pcRan.size() == r5 + 3 && g_pcRan.back() == 1003 && g_pcRevEnds == e5 + 1,
                  "pacing: PacingFlowWait=0: the flow drains the queue in its frame");
            SetNpcPacing(true, 4, 4096, 15, false);
            PcPost(kFakeEnd, 0, 0);
            gtFrame();
        }

        // delegates: five far units of one actor in one frame, then a near one of another actor
        PcActor(0, 30000.0);
        PcActor(1, 500.0);
        static FakeDelInst inst[6]{};
        for (int k = 0; k < 6; ++k) {
            inst[k].vtable = reinterpret_cast<void*>(0x1234);
            inst[k].weak = k < 5 ? static_cast<void*>(&g_pcActors[0]) : static_cast<void*>(&g_pcActors[1]);
            inst[k].id = 50 + k;
            inst[k].costUs = 6000;
        }
        auto del = reinterpret_cast<DelHook>(pi.execIfSafeHook[0]);
        PcPost(kFakeEnd, 0, 0);
        run(fakeMgr, -1);  // frame start
        bool allTrue = true;
        for (int k = 0; k < 5; ++k) allTrue &= del(&inst[k]);
        Check(g_pcDelRan.size() == 1 && NpcPacingTestRead().fifo == 4 && allTrue, "pacing: delegates: first runs, four wait");
        allTrue &= del(&inst[5]);
        Check(g_pcDelRan.size() == 2 && g_pcDelRan[1] == 55 && allTrue, "pacing: a near delegate of another actor runs at once");
        end(fakeMgr, nullptr, 2, 0);
        int frames = 0;
        bool onePerFrame = true;
        while (NpcPacingTestRead().fifo && frames < 20) {
            PcPost(kFakeEnd, 0, 0);
            const size_t d0 = g_pcDelRan.size();
            run(fakeMgr, -1);
            end(fakeMgr, nullptr, 2, 0);
            onePerFrame &= g_pcDelRan.size() - d0 == 1;
            ++frames;
        }
        Check(onePerFrame && frames == 4, "pacing: one deferred delegate per frame");
        Check(g_pcDelRan == std::vector<int>({50, 55, 51, 52, 53, 54}) && g_pcDtors == 4,
              "pacing: deferred delegates replayed in order, each copy destroyed once");
        Check(!g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E, "pacing: queue in step after the delegate test");

        // a distant unit replayed from the backlog completes two more delegates of its actor at once (an appearance
        // apply whose hair loads were already done): they wait like it, one per frame, not all in its frame
        static FakeDelInst nest[5]{};
        for (int k = 0; k < 5; ++k) {
            nest[k].vtable = reinterpret_cast<void*>(0x1234);
            nest[k].weak = &g_pcActors[0];
            nest[k].id = 60 + k;
            nest[k].costUs = 6000;
        }
        g_pcNestParent = 62;
        g_pcNested[0] = &nest[3];
        g_pcNested[1] = &nest[4];
        g_pcNestHook = del;
        g_pcDelRan.clear();
        PcPost(kFakeEnd, 0, 0);
        run(fakeMgr, -1);
        for (int k = 0; k < 3; ++k) del(&nest[k]);  // 60 runs, 61 and 62 wait
        end(fakeMgr, nullptr, 2, 0);
        frames = 0;
        onePerFrame = true;
        while (NpcPacingTestRead().fifo && frames < 20) {
            PcPost(kFakeEnd, 0, 0);
            const size_t d0 = g_pcDelRan.size();
            run(fakeMgr, -1);
            end(fakeMgr, nullptr, 2, 0);
            onePerFrame &= g_pcDelRan.size() - d0 == 1;
            ++frames;
        }
        g_pcNestParent = -1;
        Check(g_pcDelRan == std::vector<int>({60, 61, 62, 63, 64}) && onePerFrame && frames == 4,
              "pacing: delegates completed inside a distant unit wait like it (one unit per frame)");
        Check(!g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E, "pacing: queue in step after the nested test");

        // (c) with older delegates waiting, a held frame runs the main queue first (Silver Road, a test build: the
        // held frames replayed a 17 ms equipment unit each, the create never fit, and the flow drained after 3 frames)
        {
            static FakeDelInst wait[3]{};
            for (int k = 0; k < 3; ++k) {
                wait[k].vtable = reinterpret_cast<void*>(0x1234);
                wait[k].weak = &g_pcActors[0];
                wait[k].id = 70 + k;
                wait[k].costUs = 6000;
            }
            PcPost(kFakeEnd, 0, 0);
            g_pcWriter[0x4E] = 1;
            run(fakeMgr, -1);
            for (int k = 0; k < 3; ++k) del(&wait[k]);  // 70 runs, 71 and 72 wait
            end(fakeMgr, nullptr, 2, 0);
            for (int k = 0; k < 2; ++k) PcPost(kFakeCreate, 430 + k, 10000, distant);
            PcPost(kFakeEnd, 0, 0);
            g_pcSeq.clear();
            bool flowDone = false;
            int fr = 0;
            while (!flowDone && fr < 6) {
                g_pcWriter[0x4E] = 1;
                run(fakeMgr, -1);   // frame A: 71 (older) first, 430 waits; held frames: the main queue first
                flowDone = run(fakeMgr, 1004) != 3;
                end(fakeMgr, nullptr, 2, 0);
                ++fr;
            }
            printf("      (held-frame order: fr %d, flow %d, seq", fr, flowDone);
            for (int v : g_pcSeq) printf(" %d", v);
            puts(")");
            Check(flowDone && fr == 3 && g_pcSeq.size() >= 4 &&
                      std::vector<int>(g_pcSeq.begin(), g_pcSeq.begin() + 4) == std::vector<int>({71, 430, 431, 1004}),
                  "pacing: while a level flow waits, the main queue runs before older delegates (no drain)");
            for (int k = 0; k < 4 && NpcPacingTestRead().fifo; ++k) { PcPost(kFakeEnd, 0, 0); gtFrame(); }
            Check(NpcPacingTestRead().fifo == 0 && !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: the delegates run afterwards, queue in step");
        }

        // PacingOwnerWalk (mkusm, PR #25): three delegates whose owner is a smaller object (equipment, hair) with an invalid word
        // at the actor's root-component offset, its outer a far actor. Off: the fault ends the walk, the units count as near
        // and all run in their frame; on: the walk goes on to the outer, so they wait like that actor's units
        {
            static FakeActor owActor{}, owRoot{}, owEquip{};
            *reinterpret_cast<void**>(owActor.bytes + 0x1A0) = &owRoot;
            *reinterpret_cast<void**>(owRoot.bytes + 0x20) = &owActor;
            *reinterpret_cast<double*>(owRoot.bytes + 0x280) = 30000.0;
            *reinterpret_cast<uintptr_t*>(owEquip.bytes + 0x1A0) = 0x10;  // no pointer: reading its outer faults
            *reinterpret_cast<void**>(owEquip.bytes + 0x20) = &owActor;
            static FakeDelInst ow[3]{};
            auto ownerWalk = [&](bool on, int idBase) {
                SetNpcPacingOwnerWalk(on);
                for (int k = 0; k < 3; ++k) {
                    ow[k] = FakeDelInst{};
                    ow[k].vtable = reinterpret_cast<void*>(0x1234);
                    ow[k].weak = &owEquip;
                    ow[k].id = idBase + k;
                    ow[k].costUs = 6000;
                }
                g_pcDelRan.clear();
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);
                for (int k = 0; k < 3; ++k) del(&ow[k]);
                const size_t inFrame = g_pcDelRan.size();
                end(fakeMgr, nullptr, 2, 0);
                for (int k = 0; k < 12 && NpcPacingTestRead().fifo; ++k) {
                    PcPost(kFakeEnd, 0, 0);
                    run(fakeMgr, -1);
                    end(fakeMgr, nullptr, 2, 0);
                }
                return inFrame;
            };
            const size_t ranOff = ownerWalk(false, 80);
            Check(ranOff == 3 && g_pcDelRan == std::vector<int>({80, 81, 82}),
                  "pacing: PacingOwnerWalk=0: an owner that faults at the root-component offset ends the walk: near, all run at once");
            const size_t ranOn = ownerWalk(true, 90);
            Check(ranOn == 1 && g_pcDelRan == std::vector<int>({90, 91, 92}) && NpcPacingTestRead().fifo == 0 && !g_pcDeadlock &&
                      NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: PacingOwnerWalk=1: the walk goes on to its outer, a far actor: the first runs, the others wait, in order");
        }

        // PacingHairActor (npcunits 2026-11-12): two appearance units of a far unseen actor, then four hair units whose
        // owner is the hair asset (no actor in its outers) and whose capture holds the hair mesh component (an invalid word
        // at the root-component offset, its outer the actor). Off: the hair units cannot be resolved, queue as near behind
        // the second appearance and all run in its frame (the Imperial City Waterfront's 14 hair units in one frame); on:
        // they resolve to the far actor and wait under the budget, in order
        {
            static FakeActor haActor{}, haRoot{}, haComp{}, haAsset{};
            *reinterpret_cast<void**>(haActor.bytes + 0x1A0) = &haRoot;
            *reinterpret_cast<void**>(haRoot.bytes + 0x20) = &haActor;
            *reinterpret_cast<double*>(haRoot.bytes + 0x280) = 30000.0;
            *reinterpret_cast<uintptr_t*>(haComp.bytes + 0x1A0) = 0x10;  // no pointer: reading it as a root faults
            *reinterpret_cast<void**>(haComp.bytes + 0x20) = &haActor;
            struct FakeHairInst { void* vtable; uint64_t pad; uint64_t handle; void* weak; int id; int pad2; uint64_t costUs; void* comp; };
            static_assert(offsetof(FakeHairInst, comp) == 0x30, "fake hair delegate layout");
            NpcPacingTestFakes fh = f;
            fh.sizeOverride[2] = sizeof(FakeHairInst);
            fh.hairActorWeak = 0x30;
            NpcPacingTestSetup(fh);
            auto hairHook = reinterpret_cast<DelHook>(pi.execIfSafeHook[2]);
            static FakeDelInst ha[2]{};
            static FakeHairInst hh[4]{};
            auto hairRun = [&](bool on, int idBase, size_t* inSecondFrame, size_t* maxLater, int* framesUsed) {
                SetNpcPacingHairActor(on);
                for (int k = 0; k < 2; ++k) {
                    ha[k] = FakeDelInst{};
                    ha[k].vtable = reinterpret_cast<void*>(0x1234);
                    ha[k].weak = &haActor;
                    ha[k].id = idBase + k;
                    ha[k].costUs = 6000;
                }
                for (int k = 0; k < 4; ++k) {
                    hh[k] = FakeHairInst{};
                    hh[k].vtable = reinterpret_cast<void*>(0x1234);
                    hh[k].weak = &haAsset;
                    hh[k].id = idBase + 2 + k;
                    hh[k].costUs = 1500;
                    hh[k].comp = &haComp;
                }
                g_pcDelRan.clear();
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);
                del(&ha[0]);
                del(&ha[1]);
                for (int k = 0; k < 4; ++k) hairHook(&hh[k]);
                end(fakeMgr, nullptr, 2, 0);
                *inSecondFrame = 0;
                *maxLater = 0;
                *framesUsed = 0;
                for (int k = 0; k < 12 && NpcPacingTestRead().fifo; ++k) {
                    const size_t d0 = g_pcDelRan.size();
                    PcPost(kFakeEnd, 0, 0);
                    run(fakeMgr, -1);
                    end(fakeMgr, nullptr, 2, 0);
                    const size_t n = g_pcDelRan.size() - d0;
                    if (k == 0) *inSecondFrame = n;
                    else *maxLater = std::max(*maxLater, n);
                    ++*framesUsed;
                }
            };
            size_t second = 0, later = 0;
            int haFrames = 0;
            hairRun(false, 110, &second, &later, &haFrames);
            Check(second == 5 && g_pcDelRan == std::vector<int>({110, 111, 112, 113, 114, 115}) && NpcPacingTestRead().fifo == 0,
                  "pacing: PacingHairActor=0: hair units owned by their asset count as near: all four run in the second "
                  "appearance's frame");
            hairRun(true, 120, &second, &later, &haFrames);
            printf("      (PacingHairActor=1: second frame %zu units, later frames at most %zu, %d frames)\n", second, later, haFrames);
            Check(second == 1 && later >= 1 && later <= 3 && haFrames >= 3 &&
                      g_pcDelRan == std::vector<int>({120, 121, 122, 123, 124, 125}) && NpcPacingTestRead().fifo == 0 &&
                      !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: PacingHairActor=1: hair units resolve to their far actor through the mesh component and wait under "
                  "the budget, in order");
            // rev94a N4: the hair walk is guarded on its own, also with PacingOwnerWalk=0 (the component's root word faults)
            SetNpcPacingOwnerWalk(false);
            hairRun(true, 130, &second, &later, &haFrames);
            SetNpcPacingOwnerWalk(true);
            Check(second == 1 && later >= 1 && later <= 3 && haFrames >= 3 &&
                      g_pcDelRan == std::vector<int>({130, 131, 132, 133, 134, 135}) && NpcPacingTestRead().fifo == 0 &&
                      !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: PacingHairActor=1 with PacingOwnerWalk=0: the hair walk is still guarded and resolves the far actor");
            SetNpcPacingHairActor(false);
            NpcPacingTestFakes restore = f;  // rev94a N4: the fake hair offset (0x30) does not stay set for later tests
            restore.hairActorWeak = pi.hairActorWeak;
            NpcPacingTestSetup(restore);
        }

        // PacingBaseGate (basegate 2026-11-12): frames are cap sleep (NpcPacingTestFrameEdge) -> RunSession(-1) -> the actor
        // tick (the clock moves by the frame's base) -> the post-actor tick (end) -> delegate arrivals -> 1.5 ms to the next
        // sleep. 8 ms far appearance units arriving on 20 ms frames: off, each runs in its frame (29.5 ms frames); on (25 ms),
        // they wait for frames with room and run there one per frame, in order; at the lag bound they run one per frame, not
        // all at once; a near unit never waits; the queue runs at once when the frame may not defer; live off = today's rule
        {
            PcActor(0, 30000.0);
            PcActor(1, 500.0);
            NpcPacingTestFakes gf = f;
            gf.hairActorWeak = pi.hairActorWeak;
            NpcPacingTestSetup(gf);
            SetNpcPacing(true, 4, 4096, 15, false);
            SetNpcPacingGateBounds(30, 0, 100, 0);  // gate2: the old tests' bound (2 x 15 frames), calm frames off
            static FakeDelInst gd[64]{};
            int gdNext = 0;
            auto newUnit = [&](int id, int actor, uint64_t costUs) {
                FakeDelInst& d = gd[gdNext++ % 64];
                d = FakeDelInst{};
                d.vtable = reinterpret_cast<void*>(0x1234);
                d.weak = &g_pcActors[actor];
                d.id = id;
                d.costUs = costUs;
                return &d;
            };
            struct GateFrame { uint64_t busy; size_t ran; uint64_t tp; };  // tp: the clock at the post-actor tick
            // one frame: base = the actor tick's time, arrivals = units completing after the post-actor tick
            auto frame = [&](uint64_t baseUs, std::initializer_list<FakeDelInst*> arrivals) {
                NpcPacingTestFrameEdge();
                const uint64_t t0 = g_pcClock;
                const size_t d0 = g_pcDelRan.size();
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);
                g_pcClock += baseUs;
                const uint64_t tp = g_pcClock;
                end(fakeMgr, nullptr, 2, 0);
                for (FakeDelInst* a : arrivals) del(a);
                g_pcClock += 1500;
                return GateFrame{g_pcClock - t0, g_pcDelRan.size() - d0, tp};
            };
            for (int k = 0; k < 6; ++k) frame(4000, {});  // the rest after the post-actor tick is learnt (1.5 ms)
            frame(4000, {newUnit(140, 0, 8000)});         // a unit's size is learnt (8 ms)
            auto heavyThenLight = [&](int on, int idBase, uint64_t* maxBusy, int* slow, std::vector<size_t>* lightRuns) {
                SetNpcPacingBaseGate(on);
                g_pcDelRan.clear();
                *maxBusy = 0;
                *slow = 0;
                lightRuns->clear();
                for (int k = 0; k < 4; ++k) {
                    const GateFrame r = frame(20000, {newUnit(idBase + k, 0, 8000)});
                    *maxBusy = std::max(*maxBusy, r.busy);
                    *slow += r.busy > 25000;
                }
                for (int k = 0; k < 6; ++k) {
                    const GateFrame r = frame(4000, {});
                    *maxBusy = std::max(*maxBusy, r.busy);
                    *slow += r.busy > 25000;
                    lightRuns->push_back(r.ran);
                }
            };
            uint64_t busyOff = 0, busyOn = 0;
            int slowOff = 0, slowOn = 0;
            std::vector<size_t> lightOff, lightOn;
            heavyThenLight(0, 150, &busyOff, &slowOff, &lightOff);
            Check(slowOff == 4 && busyOff >= 29000 && g_pcDelRan == std::vector<int>({150, 151, 152, 153}),
                  "pacing: PacingBaseGate=0: far units arriving on 20 ms frames run there (four frames over 25 ms)");
            heavyThenLight(25, 160, &busyOn, &slowOn, &lightOn);
            printf("      (PacingBaseGate=25: max frame %.1f ms, units in the light frames %zu %zu %zu %zu %zu %zu)\n",
                   busyOn / 1000.0, lightOn[0], lightOn[1], lightOn[2], lightOn[3], lightOn[4], lightOn[5]);
            Check(slowOn == 0 && busyOn <= 25000 && g_pcDelRan == std::vector<int>({160, 161, 162, 163}) &&
                      lightOn == std::vector<size_t>({1, 1, 1, 1, 0, 0}) && NpcPacingTestRead().fifo == 0,
                  "pacing: PacingBaseGate=25: they wait for frames with room and run there one per frame, in order; no frame "
                  "over 25 ms");
            // the lag bound: three far units on one 20 ms frame, then 40 more 20 ms frames: they run at ages 28-30, one
            // per frame (bound 30 frames), never two in one frame
            {
                g_pcDelRan.clear();
                const uint64_t f0 = NpcPacingTestRead().frame + 1;
                std::vector<uint64_t> ranAt;
                size_t maxPer = 0;
                frame(20000, {newUnit(170, 0, 8000), newUnit(171, 0, 8000), newUnit(172, 0, 8000)});
                for (int k = 0; k < 40 && NpcPacingTestRead().fifo; ++k) {
                    const GateFrame r = frame(20000, {});
                    maxPer = std::max(maxPer, r.ran);
                    for (size_t i = 0; i < r.ran; ++i) ranAt.push_back(NpcPacingTestRead().frame - f0);
                }
                printf("      (PacingBaseGate=25 at the bound: ages at run");
                for (uint64_t a : ranAt) printf(" %llu", static_cast<unsigned long long>(a));
                puts(")");
                Check(ranAt == std::vector<uint64_t>({28, 29, 30}) && maxPer == 1 && g_pcDelRan == std::vector<int>({170, 171, 172}),
                      "pacing: PacingBaseGate=25: at the lag bound held units run one per frame, by age 30");
            }
            // gatefix D1: 35 far units arriving in one frame (a queue longer than the bound): the gate never runs more
            // units in a frame than today's rule does on the same stream (the old lookahead ran 35 - 29 = 6 at once)
            {
                uint64_t burstAge = 0;  // rev94d: the oldest age a unit ran at (frames since the burst's frame)
                auto burst = [&](int on, int idBase, std::vector<size_t>* per) {
                    SetNpcPacingBaseGate(on);
                    g_pcDelRan.clear();
                    per->clear();
                    std::vector<FakeDelInst*> arr;
                    for (int k = 0; k < 35; ++k) arr.push_back(newUnit(idBase + k, 0, 8000));
                    // one frame, all 35 arriving after its post-actor tick
                    NpcPacingTestFrameEdge();
                    const size_t d0 = g_pcDelRan.size();
                    PcPost(kFakeEnd, 0, 0);
                    run(fakeMgr, -1);
                    g_pcClock += 20000;
                    end(fakeMgr, nullptr, 2, 0);
                    for (FakeDelInst* a : arr) del(a);
                    g_pcClock += 1500;
                    per->push_back(g_pcDelRan.size() - d0);
                    const uint64_t f0 = NpcPacingTestRead().frame;
                    burstAge = 0;
                    for (int k = 0; k < 80 && NpcPacingTestRead().fifo; ++k) {
                        const GateFrame r = frame(20000, {});
                        per->push_back(r.ran);
                        if (r.ran) burstAge = std::max<uint64_t>(burstAge, NpcPacingTestRead().frame - f0);
                    }
                    std::vector<int> want;
                    for (int k = 0; k < 35; ++k) want.push_back(idBase + k);
                    return g_pcDelRan == want && NpcPacingTestRead().fifo == 0;
                };
                std::vector<size_t> perOff, perOn;
                const bool orderOff = burst(0, 300, &perOff);
                const bool orderOn = burst(25, 400, &perOn);
                const uint64_t ageOn = burstAge;
                const size_t maxOff = *std::max_element(perOff.begin(), perOff.end());
                const size_t maxOn = *std::max_element(perOn.begin(), perOn.end());
                printf("      (35 at once: max units per frame off %zu, on %zu; frames to drain off %zu, on %zu)\n", maxOff, maxOn,
                       perOff.size(), perOn.size());
                printf("      (35 at once: oldest age at run %llu frames, bound 30)\n", static_cast<unsigned long long>(ageOn));
                Check(orderOff && orderOn && maxOn <= maxOff && ageOn <= 30,
                      "pacing: PacingBaseGate=25: 35 units arriving in one frame never run more per frame than today's rule, "
                      "and none older than the bound (30)");
            }
            // a near unit of another actor arriving behind a held far one runs at once; then the frame may not defer
            // (menus, loading): the held one runs at once
            {
                g_pcDelRan.clear();
                frame(20000, {newUnit(180, 0, 8000)});
                const GateFrame r = frame(20000, {newUnit(181, 1, 8000)});
                Check(r.ran == 1 && g_pcDelRan == std::vector<int>({181}) && NpcPacingTestRead().fifo == 1,
                      "pacing: PacingBaseGate=25: a near unit never waits behind a held far one");
                NpcPacingTestFakes nf = gf;
                nf.guards = 0;
                NpcPacingTestSetup(nf);
                const GateFrame gq2 = frame(20000, {});
                NpcPacingTestSetup(gf);
                Check(gq2.ran == 1 && g_pcDelRan == std::vector<int>({181, 180}) && NpcPacingTestRead().fifo == 0,
                      "pacing: PacingBaseGate=25: a frame that may not defer (menu, loading) runs the held units at once");
            }
            // a held far unit whose actor comes into view (rendered, not fading) runs at once, on a 20 ms frame too
            {
                g_pcDelRan.clear();
                frame(20000, {newUnit(185, 0, 8000)});
                const GateFrame held = frame(20000, {});
                NpcPacingTestRendered([](const void* a) { return a == static_cast<const void*>(&g_pcActors[0]); });
                const GateFrame shown = frame(20000, {});
                NpcPacingTestRendered(nullptr);
                Check(held.ran == 0 && shown.ran == 1 && g_pcDelRan == std::vector<int>({185}) && NpcPacingTestRead().fifo == 0,
                      "pacing: PacingBaseGate=25: a held unit whose actor comes into view runs at once");
            }
            // live off: a held unit runs by today's rule in the next frame
            {
                g_pcDelRan.clear();
                frame(20000, {newUnit(190, 0, 8000)});
                const GateFrame gq1 = frame(20000, {});
                SetNpcPacingBaseGate(0);
                const GateFrame gq2 = frame(20000, {});
                Check(gq1.ran == 0 && gq2.ran == 1 && g_pcDelRan == std::vector<int>({190}) && NpcPacingTestRead().fifo == 0 &&
                          !g_pcDeadlock && NpcPacingTestRead().S == NpcPacingTestRead().E,
                      "pacing: PacingBaseGate live 25 -> 0: the held unit runs by today's rule");
            }
            // PacingHairActor's fade rate (gatefix; stampfix / rev96a: at most 2 units and 16 ms of estimates a frame, due
            // before dither 3): (1) eight far units, then one of a far character whose fade-in is at 3.6 (0.15 s before it
            // shows; the fake fade drops 4 per second of each frame's time), arriving in one frame, gate off. Today's budget
            // runs about one 8 ms unit per frame, so that character's unit ran at frame ~8, after it began to show; with the
            // fade rate the queue up to it is done before 3.0, in order, at most 2 per frame. (2) 12 units of 10 ms before
            // a character at 3.5: the time cap leaves one rule unit a frame (2 without it). (3) 16 units of 4 ms before a
            // character at 3.5: at most 2 a frame (4 with the old cap of 4).
            {
                const FakeActor saveA2 = g_pcActors[2], saveR2 = g_pcRoots[2];
                PcActor(2, 30000.0);
                static float s_fade = 3.6f;  // drops 4 per second of the fake clock's frame time
                NpcPacingTestFadeValue([](const void* a) { return a == static_cast<const void*>(&g_pcActors[2]) ? s_fade : -1.0f; });
                auto fadeRun = [&](bool hair, int idBase, int nUnits, uint64_t costUs, float fade0) {
                    SetNpcPacingHairActor(hair);
                    g_pcDelRan.clear();
                    s_fade = fade0;
                    std::vector<FakeDelInst*> arr;
                    for (int k = 0; k < nUnits; ++k) arr.push_back(newUnit(idBase + k, 0, costUs));
                    arr.push_back(newUnit(idBase + nUnits, 2, costUs));
                    NpcPacingTestFrameEdge();
                    PcPost(kFakeEnd, 0, 0);
                    run(fakeMgr, -1);
                    g_pcClock += 20000;
                    end(fakeMgr, nullptr, 2, 0);
                    for (FakeDelInst* a : arr) del(a);
                    g_pcClock += 1500;
                    int at = -1;
                    float fadeAt = -1.0f;
                    size_t maxPer = 0;
                    for (int k = 0; k < 60 && NpcPacingTestRead().fifo; ++k) {
                        const float before = s_fade;
                        const GateFrame r = frame(20000, {});
                        s_fade -= 4.0f * static_cast<float>(r.busy) / 1e6f;
                        maxPer = std::max(maxPer, r.ran);
                        for (size_t i = 0; i < g_pcDelRan.size(); ++i)
                            if (g_pcDelRan[i] == idBase + nUnits && at < 0) { at = k + 1; fadeAt = before; }
                    }
                    std::vector<int> want;
                    for (int k = 0; k <= nUnits; ++k) want.push_back(idBase + k);
                    printf("      (fade rate %s, %d x %llu us at %.2f: the fading character's unit ran %d frames after arriving, at "
                           "dither %.2f; max units per frame %zu)\n", hair ? "on" : "off", nUnits, (unsigned long long)costUs,
                           fade0, at, fadeAt, maxPer);
                    return std::make_tuple(at, maxPer, g_pcDelRan == want && fadeAt >= 3.0f);
                };
                SetNpcPacingBaseGate(0);
                const auto [atOff, maxOff, orderOff] = fadeRun(false, 700, 8, 8000, 3.6f);
                const auto [atOn, maxOn, orderOn] = fadeRun(true, 720, 8, 8000, 3.6f);
                Check(!orderOff && orderOn && atOn >= 1 && maxOn <= 2,
                      "pacing: PacingHairActor: a fading far character's queued unit (and those before it) is done before "
                      "it shows, in order, at most 2 per frame");
                const auto [atBig, maxBig, orderBig] = fadeRun(true, 740, 12, 10000, 3.5f);
                (void)atBig;
                (void)orderBig;
                Check(maxBig == 1, "pacing: PacingHairActor's fade rate: 10 ms units, one a frame (the 16 ms cap)");
                const auto [atSmall, maxSmall, orderSmall] = fadeRun(true, 760, 16, 4000, 3.5f);
                (void)atSmall;
                (void)orderSmall;
                Check(maxSmall == 2, "pacing: PacingHairActor's fade rate: 4 ms units, at most 2 a frame (the 2-unit cap)");
                NpcPacingTestFadeValue(nullptr);
                SetNpcPacingHairActor(false);
                SetNpcPacingBaseGate(0);
                g_pcActors[2] = saveA2;
                g_pcRoots[2] = saveR2;
            }
            // gate2 (2026-11-13): the gate decides per queued unit and has its own hard bound (PacingGateBoundFrames). Actors
            // 0 and 2 are 300 m away in their spawn fade-in at the game's floor (dither 4.0), actor 3 300 m away, not fading.
            {
                const FakeActor saveA2 = g_pcActors[2], saveR2 = g_pcRoots[2], saveA3 = g_pcActors[3], saveR3 = g_pcRoots[3];
                PcActor(2, 30000.0);
                PcActor(3, 30000.0);
                static float s_f0 = 4.0f, s_f2 = 4.0f;
                NpcPacingTestFadeValue([](const void* a) {
                    return a == static_cast<const void*>(&g_pcActors[0]) ? s_f0 : a == static_cast<const void*>(&g_pcActors[2]) ? s_f2 : -1.0f;
                });
                SetNpcPacingHairActor(true);
                SetNpcPacingBaseGate(25);
                SetNpcPacingGateBounds(90, 0, 100, 1000);  // public: the fade hold cannot be switched on
                auto ranIdx = [](int id) {
                    for (size_t i = 0; i < g_pcDelRan.size(); ++i)
                        if (g_pcDelRan[i] == id) return static_cast<int>(i);
                    return -1;
                };
                // (1) the flaw: four units of actor 0 (held at the floor) and one of actor 2 behind them, all on 20 ms frames;
                // actor 2's fade-in then runs on (3.6: 0.15 s before it shows). Its unit runs within two frames, ahead of
                // actor 0's, and actor 0's stay held (the old gate dropped itself for the whole call once any queued actor's
                // fade had left 5, and today's lag-scaled budget ran the floor-held units onto the busy frames)
                g_pcDelRan.clear();
                s_f0 = 4.0f;
                s_f2 = 4.0f;
                const uint64_t q0 = NpcPacingTestRead().frame + 1;
                frame(20000, {newUnit(800, 0, 8000), newUnit(801, 0, 8000), newUnit(802, 0, 8000), newUnit(803, 0, 8000),
                              newUnit(804, 2, 8000)});
                const size_t heldFirst = g_pcDelRan.size();
                s_f2 = 3.6f;
                int soonAt = -1;
                for (int k = 0; k < 10; ++k) {
                    frame(20000, {});
                    s_f2 -= 0.08f;  // 4 per second of 20 ms frames
                    if (soonAt < 0 && ranIdx(804) >= 0) soonAt = k + 1;
                }
                printf("      (gate2: floor-held units ran in the first frame %zu, in the next 10 frames %zu; the fading actor's unit "
                       "after %d frames)\n", heldFirst, g_pcDelRan.size(), soonAt);
                Check(heldFirst == 0 && g_pcDelRan == std::vector<int>({804}) && soonAt >= 1 && soonAt <= 2,
                      "pacing: PacingBaseGate per unit: a far unit whose actor is about to show runs within two frames, ahead of "
                      "another actor's held units, without releasing them (gate2: the flaw)");
                // (1b) lab fade hold (PacingGateFadeHoldMs; off by default, public builds never): actor 0 has held units, so
                // its fade-in stops at the floor (4): a step from 4.3 is kept, from 4.05 cut to land on 4.0, none at 4.0; a
                // value under the game's floor 4 is raised to 4 (no visible change); a fade already past the floor (3.8)
                // and actor 2 (its unit ran) go on as the game's. Public: every step is the game's.
                {
                    float val = 4.0f;
                    Check(NpcPacingTestFadeHold(&g_pcActors[0], &val, 0.0f, 4.0f, 0.02f) == 0.02f && val == 4.0f,
                          "pacing: PacingBaseGate: no fade hold in the public build (rev96e B1)");
                }
                // (2) a held unit of actor 3 whose actor comes within PacingGateNearM (80 m: still far for pacing) runs at once;
                // actor 0's stay held
                frame(20000, {newUnit(810, 3, 8000)});
                const size_t before3 = g_pcDelRan.size();
                PcActor(3, 8000.0);
                frame(20000, {});
                Check(before3 == 1 && g_pcDelRan == std::vector<int>({804, 810}),
                      "pacing: PacingBaseGate per unit: an actor nearer than PacingGateNearM is not held; the far held units stay "
                      "held");
                // (3) the bound: actor 0's units run by age 90 (frames since queued), one per frame, in order; none older
                std::vector<uint64_t> ages;
                size_t maxPer = 0;
                for (int k = 0; k < 120 && NpcPacingTestRead().fifo; ++k) {
                    const GateFrame r = frame(20000, {});
                    maxPer = std::max(maxPer, r.ran);
                    for (size_t i = 0; i < r.ran; ++i) ages.push_back(NpcPacingTestRead().frame - q0);
                }
                printf("      (gate2 bound 90: ages at run");
                for (uint64_t a : ages) printf(" %llu", static_cast<unsigned long long>(a));
                puts(")");
                const bool inBound = !ages.empty() && *std::max_element(ages.begin(), ages.end()) <= 90 &&
                                     *std::min_element(ages.begin(), ages.end()) >= 85;
                Check(inBound && maxPer == 1 && g_pcDelRan == std::vector<int>({804, 810, 800, 801, 802, 803}) &&
                          NpcPacingTestRead().fifo == 0,
                      "pacing: PacingBaseGate per unit: held units wait on busy frames up to PacingGateBoundFrames (90) and run "
                      "by it, one per frame, oldest first; no unit older than the bound");
                {
                    float val = 4.0f;
                    Check(NpcPacingTestFadeHold(&g_pcActors[0], &val, 0.0f, 4.0f, 0.02f) == 0.02f,
                          "pacing: PacingBaseGate fade hold: once the held units ran (here at the bound) the fade goes on");
                }
                PcActor(3, 30000.0);
                // (4) calm frames: held 8 ms units do not fit a 16 ms frame (16 + 1.5 + 8 > 25); PacingGateCalmMs 20 lets
                // one a frame run there (16 + 1.5 < 20); with it off they wait
                auto calmRun = [&](int calmMs, int idBase) {
                    SetNpcPacingGateBounds(90, calmMs, 100, 0);
                    g_pcDelRan.clear();
                    frame(20000, {newUnit(idBase, 0, 8000), newUnit(idBase + 1, 0, 8000), newUnit(idBase + 2, 0, 8000)});
                    std::vector<size_t> per;
                    for (int k = 0; k < 4; ++k) per.push_back(frame(16000, {}).ran);
                    return per;
                };
                const std::vector<size_t> calmOff = calmRun(0, 820);
                for (int k = 0; k < 100 && NpcPacingTestRead().fifo; ++k) frame(20000, {});
                const std::vector<size_t> calmOn = calmRun(20, 830);
                printf("      (gate2 calm frames: off %zu %zu %zu %zu, on %zu %zu %zu %zu)\n", calmOff[0], calmOff[1], calmOff[2],
                       calmOff[3], calmOn[0], calmOn[1], calmOn[2], calmOn[3]);
                Check(calmOff == std::vector<size_t>({0, 0, 0, 0}) && calmOn == std::vector<size_t>({1, 1, 1, 0}) &&
                          g_pcDelRan == std::vector<int>({830, 831, 832}),
                      "pacing: PacingGateCalmMs: a held unit too big for the target runs on a calm frame, one a frame, in order");
                // (5) arrival (no queue): a unit of an actor past the fade's floor (3.6) runs at once on a 20 ms frame; one at
                // the floor (4.0) is held (gatefix D2 as the per-unit test)
                SetNpcPacingGateBounds(90, 0, 100, 0);
                g_pcDelRan.clear();
                s_f2 = 3.6f;
                const GateFrame soonArr = frame(20000, {newUnit(840, 2, 8000)});
                s_f2 = 4.0f;
                const GateFrame floorArr = frame(20000, {newUnit(841, 2, 8000)});
                Check(soonArr.ran == 1 && floorArr.ran == 0 && NpcPacingTestRead().fifo == 1,
                      "pacing: PacingBaseGate: an arriving unit of an actor past the fade's floor runs at once; at the floor it is "
                      "held");
                s_f2 = 3.6f;  // ... and runs once its actor leaves the floor
                frame(20000, {});
                Check(g_pcDelRan == std::vector<int>({840, 841}) && NpcPacingTestRead().fifo == 0,
                      "pacing: PacingBaseGate: a held unit runs once its actor leaves the fade's floor");
                // (6) gatefix L2: a key-0 unit (its actor not found) queued behind held units: everything before it runs
                // first, by today's rule, in order; it does not wait for the bound
                s_f2 = 4.0f;
                g_pcDelRan.clear();
                FakeDelInst* k0 = newUnit(852, 0, 8000);
                k0->weak = nullptr;  // its weak pointer resolves to nothing
                frame(20000, {newUnit(850, 0, 8000), newUnit(851, 0, 8000), k0});
                int k0At = -1;
                for (int k = 0; k < 20 && NpcPacingTestRead().fifo; ++k) {
                    frame(20000, {});
                    if (k0At < 0 && ranIdx(852) >= 0) k0At = k + 1;
                }
                Check(g_pcDelRan == std::vector<int>({850, 851, 852}) && k0At >= 1 && k0At <= 6,
                      "pacing: PacingBaseGate: a key-0 unit runs by today's rule with everything queued before it (gatefix L2)");
                // (7) PacingGateShownHoldMs's due rule on the gate's own queue (rev96g F10, port102). Actor 0 stays at the
                // shown level 5 (held: what the hold shows), its own fade value (the game's) falls 4 per second on 20 ms
                // frames from 5; its 8 ms units never fit (20 + 1.5 + 8 > 25). Key off: they wait for the bound (90 frames,
                // long after the fade's end at frame ~63). On (End 1.0): all run before the game's value reaches 1.0 (the
                // fade's last 0.25 s), at most one a frame, in order, none as due now; the actor leaves the hold list in the
                // frame its last unit ran (F9). 7b: a unit queued while the actor was near (not gated) runs before its later
                // gated units (F1), also counted in the spread (the follow-up)
                {
                    static float s_gv0 = 5.0f;
                    NpcPacingTestGameValue([](const void* a) {
                        return a == static_cast<const void*>(&g_pcActors[0]) ? s_gv0 : -1.0f;
                    });
                    SetNpcPacingGateBounds(90, 0, 100, 0);
                    s_f0 = 5.0f;
                    // fix102: longAt >= 0: that frame takes 250 ms (the fade value drops 1.0 in it); markAt >= 0: an equipment
                    // unit of actor 0 runs before its appearance in that frame (its hold open from then; touch: kept live)
                    auto dueRun = [&](int ms, int idBase, bool nearFirst, std::vector<float>* at, size_t* most, bool* heldAfter,
                                      int longAt = -1, int markAt = -1, bool touch = true, int units = 4,
                                      std::vector<int>* ranFrame = nullptr, int openAt = -2) {
                        if (openAt == -2) openAt = markAt;
                        ShownHoldTestSetup(nullptr, markAt >= 0 ? &PcClock : nullptr, markAt >= 0 ? 1 : -1, ms, 10);
                        g_pcDelRan.clear();
                        at->clear();
                        *most = 0;
                        s_gv0 = 5.0f;
                        if (nearFirst) {
                            PcActor(0, 500.0);
                            frame(20000, {newUnit(idBase + 9, 0, 8000)});
                            PcActor(0, 30000.0);
                        }
                        if (units == 4)
                            frame(20000, {newUnit(idBase, 0, 8000), newUnit(idBase + 1, 0, 8000), newUnit(idBase + 2, 0, 8000),
                                          newUnit(idBase + 3, 0, 8000)});
                        else
                            frame(20000, {newUnit(idBase, 0, 8000)});
                        if (ranFrame) ranFrame->clear();
                        const uintptr_t a0 = reinterpret_cast<uintptr_t>(&g_pcActors[0]);
                        for (int k = 0; k < 120 && NpcPacingTestRead().fifo; ++k) {
                            const bool isLong = k == longAt;
                            s_gv0 = std::max(0.0f, s_gv0 - (isLong ? 1.0f : 0.08f));
                            if (openAt >= 0 && (k == openAt || (k > openAt && touch))) ShownHoldTestHold(0x5000, a0, true);
                            if (k == markAt) ShownHoldNoteEquipFirst(a0);
                            const GateFrame r = frame(isLong ? 248500 : 20000, {});
                            *most = std::max(*most, r.ran);
                            for (size_t i = 0; i < r.ran; ++i) {
                                at->push_back(s_gv0);
                                if (ranFrame) ranFrame->push_back(k);
                            }
                        }
                        *heldAfter = NpcGateHoldsActor(reinterpret_cast<uintptr_t>(&g_pcActors[0]));
                    };
                    std::vector<float> atOff, atOn, atNear;
                    size_t mostOff = 0, mostOn = 0, mostNear = 0;
                    bool heldOff = true, heldOn = true, heldNear = true;
                    dueRun(0, 860, false, &atOff, &mostOff, &heldOff);
                    const std::vector<int> ranOff = g_pcDelRan;
                    dueRun(1000, 870, false, &atOn, &mostOn, &heldOn);
                    const std::vector<int> ranOn = g_pcDelRan;
                    dueRun(1000, 880, true, &atNear, &mostNear, &heldNear);
                    const std::vector<int> ranNear = g_pcDelRan;
                    auto minAt = [](const std::vector<float>& v) { return v.empty() ? -1.0f : *std::min_element(v.begin(), v.end()); };
                    printf("      (shownhold due rule: off %zu units, lowest game value at a run %.2f; on %zu units, lowest %.2f, most "
                           "a frame %zu; near-first %zu units, lowest %.2f)\n",
                           atOff.size(), minAt(atOff), atOn.size(), minAt(atOn), mostOn, atNear.size(), minAt(atNear));
                    Check(ranOff == std::vector<int>({860, 861, 862, 863}) && minAt(atOff) == 0.0f,
                          "pacing + shownhold: key off: the held units wait for the bound, past the fade's end (the gate alone)");
                    Check(ranOn == std::vector<int>({870, 871, 872, 873}) && minAt(atOn) > 1.0f && mostOn == 1 && !heldOn,
                          "pacing + shownhold (rev96g F10): the due rule runs a held far character's units before the last 0.25 s of "
                          "its fade, one a frame, in order; it leaves the hold list with its last unit (F9)");
                    Check(ranNear.size() == 5 && ranNear[0] == 889 &&
                              std::vector<int>(ranNear.begin() + 1, ranNear.end()) == std::vector<int>({880, 881, 882, 883}) &&
                              minAt(atNear) > 1.0f && !heldNear,
                          "pacing + shownhold (F1): a unit queued while the character was near runs before its later gated units, "
                          "all before the fade's last 0.25 s");
                    // fix102 (rev102a S2 / Q3): a 250 ms frame. (a) before the spread starts (value 2.68 -> 1.68): the period
                    // ring takes its second longest frame, so the units still go one a frame, none as due now; (b) into the
                    // last 0.25 s (1.96 -> 0.96): due now runs the rest in the next replay, still before the fade's end
                    std::vector<float> atL1, atL2;
                    size_t mostL1 = 0, mostL2 = 0;
                    bool heldL1 = true, heldL2 = true;
                    std::vector<int> frL1;
                    uint64_t dueL1[2] = {};
                    ShownHoldTestDueCounts(dueL1);
                    dueRun(1000, 900, false, &atL1, &mostL1, &heldL1, 29, -1, true, 4, &frL1);
                    ShownHoldTestDueCounts(dueL1);
                    const std::vector<int> ranL1 = g_pcDelRan;
                    dueRun(1000, 910, false, &atL2, &mostL2, &heldL2, 38);
                    const std::vector<int> ranL2 = g_pcDelRan;
                    printf("      (shownhold due rule with a 250 ms frame: before the spread most a frame %zu, lowest value %.2f, "
                           "frames %d %d %d %d, due units soft %llu now %llu; into the last 0.25 s most a frame %zu, lowest value "
                           "%.2f)\n",
                           mostL1, minAt(atL1), frL1.size() > 0 ? frL1[0] : -1, frL1.size() > 1 ? frL1[1] : -1,
                           frL1.size() > 2 ? frL1[2] : -1, frL1.size() > 3 ? frL1[3] : -1, (unsigned long long)dueL1[0],
                           (unsigned long long)dueL1[1], mostL2, minAt(atL2));
                    // (today's rule may add units the due rule has released after the long frame: not counted against it)
                    Check(ranL1 == std::vector<int>({900, 901, 902, 903}) && minAt(atL1) > 1.0f && !heldL1 &&
                              ranL2 == std::vector<int>({910, 911, 912, 913}) && minAt(atL2) > 0.0f && !heldL2,
                          "pacing + shownhold (fix102): a 250 ms frame before the spread: the units still run in order before "
                          "the fade's last 0.25 s; one into it: due now runs the rest before the fade's end, in order");
                    // fix102 (rev102a S2): the period ring: one long frame of 8 is not the spread's period, two are
                    {
                        for (int k = 0; k < 9; ++k) frame(20000, {});
                        const float p0 = NpcPacingTestGatePeriodS();
                        frame(100000, {}), frame(20000, {});
                        const float p1 = NpcPacingTestGatePeriodS();
                        frame(100000, {}), frame(20000, {});
                        const float p2 = NpcPacingTestGatePeriodS();
                        for (int k = 0; k < 8; ++k) frame(20000, {});
                        const float p3 = NpcPacingTestGatePeriodS();
                        printf("      (due spread period: %.4f, one 101.5 ms frame %.4f, two %.4f, 8 frames later %.4f s)\n", p0, p1,
                               p2, p3);
                        Check(std::fabs(p0 - 0.0215f) < 0.001f && std::fabs(p1 - (0.1f + 7 * 0.0215f) / 8) < 0.001f &&
                                  std::fabs(p2 - 0.1f) < 0.001f && std::fabs(p3 - 0.0215f) < 0.001f,
                              "pacing + shownhold (fix102 S2): the due spread's period: one 100 ms hitch of the last 8 frames counts "
                              "as their mean, two in full, and they leave the ring after 8 frames");
                    }
                    // fix102 (rev102a M1 / M2): equipment ran before the appearance in frame 5 (value 4.52, ~0.9 s before the
                    // units would be due). Before: all units soft-due at once at rate 2. Now: a deadline 0.1 s later in the
                    // spread: 4 units one a frame from then, done within 0.225 s; 1 unit: the room rule keeps it 3 frames,
                    // then it runs (~0.1 s after the mark); a hold not called for 0.1 s (its actor gone, opened at frame 0)
                    // is not marked by a later equipment unit at its address: the units run as unmarked
                    std::vector<float> atU4, atU1, atUs, atUn;
                    std::vector<int> frU4, frU1, frUs, frUn;
                    size_t mostU4 = 0, mostU1 = 0, mostUs = 0, mostUn = 0;
                    bool heldU = true;
                    dueRun(1000, 920, false, &atU4, &mostU4, &heldU, -1, 5, true, 4, &frU4);
                    const std::vector<int> ranU4 = g_pcDelRan;
                    dueRun(1000, 930, false, &atU1, &mostU1, &heldU, -1, 5, true, 1, &frU1);
                    dueRun(1000, 940, false, &atUs, &mostUs, &heldU, -1, 6, false, 1, &frUs, 0);
                    dueRun(1000, 950, false, &atUn, &mostUn, &heldU, -1, -1, true, 1, &frUn);
                    auto first = [](const std::vector<int>& v) { return v.empty() ? -1 : v.front(); };
                    auto last = [](const std::vector<int>& v) { return v.empty() ? -1 : v.back(); };
                    printf("      (shownhold equipment first at frame 5: 4 units ran frames %d-%d, most a frame %zu; 1 unit at frame %d; "
                           "stale hold: frame %d; unmarked: frame %d)\n",
                           first(frU4), last(frU4), mostU4, first(frU1), first(frUs), first(frUn));
                    Check(ranU4 == std::vector<int>({920, 921, 922, 923}) && mostU4 == 1 && first(frU4) >= 5 && last(frU4) <= 15 &&
                              frU1.size() == 1 && first(frU1) >= 8 && first(frU1) <= 11,
                          "pacing + shownhold (fix102 M1): equipment ran before the appearance: the actor's units get a 0.1 s "
                          "deadline in the spread (one a frame, not all at once; a single unit waits for room about 0.1 s)");
                    Check(frUs.size() == 1 && frUn.size() == 1 && first(frUs) == first(frUn) && first(frUn) > 20,
                          "pacing + shownhold (fix102 M2): a hold not called for 0.1 s (its actor gone) is not marked by an "
                          "equipment unit at its address: those units run as unmarked ones");
                    ShownHoldTestSetup(nullptr, nullptr, -1, 0, 10);
                    ShownHoldTestSetup(nullptr, nullptr, -1, 0, 10);
                    NpcPacingTestGameValue(nullptr);
                    s_f0 = 4.0f;
                }
                NpcPacingTestFadeValue(nullptr);
                SetNpcPacingHairActor(false);
                SetNpcPacingBaseGate(0);
                SetNpcPacingGateBounds(30, 0, 100, 0);
                g_pcActors[2] = saveA2;
                g_pcRoots[2] = saveR2;
                g_pcActors[3] = saveA3;
                g_pcRoots[3] = saveR3;
            }
            NpcPacingTestSetup(gf);
        }

        // PacingLeanFlowDrain: a level flow reaches its bound (3 frames: the queue owes an older OT frame) and the owed
        // main queue drains before it; off: the waiting delegates drain with it (as before), on: they stay queued and
        // replay one per frame afterwards, in order
        {
            static FakeDelInst dl[8]{};
            auto boundDrain = [&](int idBase, int* delInDrain, std::vector<int>* delOrder) {
                for (int k = 0; k < 8; ++k) {
                    dl[k] = FakeDelInst{};
                    dl[k].vtable = reinterpret_cast<void*>(0x1234);
                    dl[k].weak = &g_pcActors[0];
                    dl[k].id = idBase + k;
                    dl[k].costUs = 6000;
                }
                const size_t d0 = g_pcDelRan.size();
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);
                for (int k = 0; k < 8; ++k) del(&dl[k]);  // the first runs, seven wait
                end(fakeMgr, nullptr, 2, 0);
                for (int k = 0; k < 4; ++k) PcPost(kFakeCreate, idBase + 20 + k, 10000, distant);
                PcPost(kFakeEnd, 0, 0);
                gtFrame();
                PcPost(kFakeEnd, 0, 0);  // the OT's next frame: the queue owes an older one, so the flow's bound is 3
                bool done = false;
                int fr = 0;
                *delInDrain = -1;
                while (!done && fr < 8) {
                    const size_t b = g_pcDelRan.size();
                    g_pcWriter[0x4E] = 1;
                    run(fakeMgr, -1);
                    done = run(fakeMgr, 1007) != 3;
                    end(fakeMgr, nullptr, 2, 0);
                    if (done) *delInDrain = static_cast<int>(g_pcDelRan.size() - b);
                    ++fr;
                }
                for (int k = 0; k < 12 && NpcPacingTestRead().fifo; ++k) { PcPost(kFakeEnd, 0, 0); gtFrame(); }
                delOrder->assign(g_pcDelRan.begin() + static_cast<std::ptrdiff_t>(d0), g_pcDelRan.end());
                return done;
            };
            std::vector<int> wantOn, orderOff, orderOn;
            int inDrainOff = 0, inDrainOn = 0;
            SetNpcPacingLeanFlowDrain(false);
            const bool doneOff = boundDrain(600, &inDrainOff, &orderOff);
            SetNpcPacingLeanFlowDrain(true);
            const bool doneOn = boundDrain(700, &inDrainOn, &orderOn);
            printf("      (flow bound drain: delegates in the drain frame: off %d, on %d)\n", inDrainOff, inDrainOn);
            for (int k = 0; k < 8; ++k) wantOn.push_back(700 + k);
            Check(doneOff && inDrainOff >= 3, "pacing: flow bound drain, PacingLeanFlowDrain=0: the waiting delegates drain with the queue");
            Check(doneOn && inDrainOn == 0 && orderOn == wantOn && NpcPacingTestRead().fifo == 0 && !g_pcDeadlock &&
                      NpcPacingTestRead().S == NpcPacingTestRead().E,
                  "pacing: flow bound drain, PacingLeanFlowDrain=1: the main queue drains, the delegates stay queued and run after, in order");
        }
        // PacingFadeAware (public since 1.2.3, npcjitter): the fade component's layout from its tick; a far actor that the
        // rendered test counts as seen but that is still invisible in its spawn fade-in (dither >= 3) waits like an unseen
        // one, and the re-check releases it once its dither falls below 3; key 0 and a far actor not fading: as 1.2.2
        {
            uint32_t fo[5] = {};
            void* fcls = nullptr;
            const bool ff = NpcPacingFadeInfo(fo, &fcls);
            Check(ff && fo[0] == 0xD8 && fo[1] == 0xBC && fo[2] == 0xA8 && fo[3] == 0x7A0 && fo[4] == 0xCC && at(fcls, 0x46B19E0),
                  "pacing: spawn fade-in tick: state +0xD8, value +0xBC, owner +0xA8, actor's component +0x7A0, floor +0xCC, class exe+0x46B19E0");
            if (ff) {
                static uint8_t pClass[0x80], pObjClass[0x80], pActor[0x800], pRoot[0x300], pComp[0x100];
                static void* pChain[4];
                memset(pActor, 0, sizeof pActor);
                memset(pRoot, 0, sizeof pRoot);
                memset(pComp, 0, sizeof pComp);
                *reinterpret_cast<int32_t*>(pClass + 0x38) = 2;
                *reinterpret_cast<int32_t*>(pObjClass + 0x38) = 3;
                *reinterpret_cast<void**>(pObjClass + 0x30) = pChain;
                pChain[2] = pClass + 0x30;
                *reinterpret_cast<void**>(pActor + 0x10) = pObjClass;
                *reinterpret_cast<void**>(pActor + 0x1A0) = pRoot;
                *reinterpret_cast<void**>(pRoot + 0x20) = pActor;
                *reinterpret_cast<double*>(pRoot + 0x280) = 30000.0;  // far
                *reinterpret_cast<void**>(pActor + 0x7A0) = pComp;
                *reinterpret_cast<void**>(pComp + 0xA8) = pActor;
                *reinterpret_cast<int32_t*>(pComp + 0xD8) = 1;     // FadeIn
                *reinterpret_cast<float*>(pComp + 0xBC) = 4.0f;    // dither 4: invisible
                struct C { static const uint8_t* Cls() { return pClass; } };
                NpcPacingTestFade(&C::Cls);
                NpcPacingTestRendered(+[](const void* a) { return a == static_cast<const void*>(pActor); });  // "seen"
                SetNpcPacingFadeAware(1);
                static FakeDelInst pd[3]{};
                for (int k = 0; k < 3; ++k) {
                    pd[k].vtable = reinterpret_cast<void*>(0x1234);
                    pd[k].weak = k == 0 ? static_cast<void*>(&g_pcActors[0]) : static_cast<void*>(pActor);
                    pd[k].id = 100 + k;
                    pd[k].costUs = 6000;
                }
                PcActor(0, 30000.0);
                g_pcDelRan.clear();
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);
                for (int k = 0; k < 3; ++k) del(&pd[k]);  // 100 (far, unseen) runs; 101, 102 (far, seen, invisible) wait
                Check(g_pcDelRan == std::vector<int>({100}) && NpcPacingTestRead().fifo == 2,
                      "pacing: far actor counted as seen but invisible in its fade-in waits like an unseen one (PacingFadeAware=1)");
                end(fakeMgr, nullptr, 2, 0);
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);  // 101 runs first; 102 still fading: waits
                Check(g_pcDelRan == std::vector<int>({100, 101}) && NpcPacingTestRead().fifo == 1, "pacing: one per frame while fading");
                *reinterpret_cast<float*>(pComp + 0xBC) = 2.0f;  // below 3: becoming visible
                end(fakeMgr, nullptr, 2, 0);   // the post-tick replay re-checks 102: seen and visible, runs now
                Check(g_pcDelRan == std::vector<int>({100, 101, 102}) && NpcPacingTestRead().fifo == 0,
                      "pacing: the re-check releases it once its dither falls below 3");
                // key 0 (1.2.2): the same seen, invisible actor runs at once
                *reinterpret_cast<float*>(pComp + 0xBC) = 4.0f;
                SetNpcPacingFadeAware(0);
                g_pcDelRan.clear();
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);
                for (int k = 0; k < 3; ++k) del(&pd[k]);
                end(fakeMgr, nullptr, 2, 0);
                Check(g_pcDelRan == std::vector<int>({100, 101, 102}) && NpcPacingTestRead().fifo == 0,
                      "pacing: PacingFadeAware=0: a seen actor runs at once whatever its fade (as 1.2.2)");
                // key 1, the actor seen and not fading in (fade over): runs at once
                SetNpcPacingFadeAware(1);
                *reinterpret_cast<int32_t*>(pComp + 0xD8) = 0;
                g_pcDelRan.clear();
                PcPost(kFakeEnd, 0, 0);
                run(fakeMgr, -1);
                for (int k = 0; k < 3; ++k) del(&pd[k]);
                end(fakeMgr, nullptr, 2, 0);
                Check(g_pcDelRan == std::vector<int>({100, 101, 102}) && NpcPacingTestRead().fifo == 0 && !g_pcDeadlock &&
                          NpcPacingTestRead().S == NpcPacingTestRead().E,
                      "pacing: PacingFadeAware=1, a seen actor whose fade-in is over runs at once; queue in step");
                // FadeInUnseen (stampfix, Epic's idea from UE 5.5.0): the fade tick's caller jump, the mesh list, the static
                // mesh class and LastRenderTime from the game's code; then the after-tick step on fakes (rev96a's age rule):
                // a far character in its fade-in whose skinned meshes were not drawn for 200 ms gets their render time just
                // past the engine's 1 s window; a mesh drawn lately, one already outside the window, static meshes and
                // LastRenderTimeOnScreen are left alone; key 0, near characters, FadeOut and a finished fade likewise
                {
                    uint32_t jr = 0, br = 0, lo = 0, cl = 0, sr = 0;
                    const bool fu = FadeInUnseenInfo(&jr, &br, &lo, &cl, &sr);
                    Check(fu && jr == 0x48D18FE && br == 0x48D5A40 && lo == 0xE0 && cl == 0x384 && sr == 0x30BF250,
                          "FadeInUnseen: fade tick caller's jump exe+0x48D18FE -> body exe+0x48D5A40, mesh list +0xE0, "
                          "LastRenderTime +0x384, static mesh class exe+0x30BF250");
                    if (fu) {
                        static uint8_t mBody[0x400], mHead[0x400], mStatic[0x400], smClass[0x80], smObjClass[0x80], skObjClass[0x80];
                        static void* smChain[8];
                        static void* skChain[8];
                        static uint8_t* mList[3];
                        *reinterpret_cast<int32_t*>(smClass + 0x38) = 4;
                        *reinterpret_cast<int32_t*>(smObjClass + 0x38) = 4;
                        *reinterpret_cast<void**>(smObjClass + 0x30) = smChain;
                        smChain[4] = smClass + 0x30;
                        *reinterpret_cast<int32_t*>(skObjClass + 0x38) = 5;
                        *reinterpret_cast<void**>(skObjClass + 0x30) = skChain;
                        skChain[4] = skObjClass + 0x30;
                        *reinterpret_cast<void**>(mStatic + 0x10) = smObjClass;
                        *reinterpret_cast<void**>(mBody + 0x10) = skObjClass;
                        *reinterpret_cast<void**>(mHead + 0x10) = skObjClass;
                        mList[0] = mBody;
                        mList[1] = mStatic;
                        mList[2] = mHead;
                        *reinterpret_cast<void**>(pComp + lo) = mList;
                        *reinterpret_cast<int32_t*>(pComp + lo + 8) = 3;
                        float& lb = *reinterpret_cast<float*>(mBody + cl);
                        float& lh = *reinterpret_cast<float*>(mHead + cl);
                        float& ls = *reinterpret_cast<float*>(mStatic + cl);
                        float& onB = *reinterpret_cast<float*>(mBody + cl + 4);
                        *reinterpret_cast<int32_t*>(pComp + 0xD8) = 1;
                        *reinterpret_cast<float*>(pComp + 0xBC) = 4.5f;
                        *reinterpret_cast<double*>(pRoot + 0x280) = 30000.0;  // far
                        struct U {
                            static double Now() { return 100.0; }
                            static const uint8_t* Sm() { return smClass; }
                        };
                        const float expired = static_cast<float>(100.0 - 1.0) - 0.001f;
                        FadeInUnseenTestSlack(0.0);  // the last two frames' time: none (rev96d A2 below)
                        // drawn 16 ms ago (in view): untouched at any dither
                        lb = lh = 99.984f;
                        ls = onB = 99.5f;
                        int r = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm);
                        const bool drawn = r == 0 && lb == 99.984f && lh == 99.984f;
                        *reinterpret_cast<float*>(pComp + 0xBC) = 1.0f;
                        r = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm);
                        const bool drawnLow = r == 0 && lb == 99.984f;
                        Check(drawn && drawnLow, "FadeInUnseen: a far fading-in body drawn 16 ms ago keeps its render time (dither 4.5 and 1.0)");
                        // not drawn for 300 / 500 ms (occluded after its first frames): expired; the static mesh and the
                        // on-screen time are kept; one already outside the 1 s window is not touched
                        lb = 99.7f;
                        lh = 99.5f;
                        r = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm);
                        const bool exp = r == 2 && lb == expired && lh == expired && ls == 99.5f && onB == 99.5f;
                        lb = 98.0f;
                        lh = -1000.0f;
                        r = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm);
                        const bool outside = r == 0 && lb == 98.0f && lh == -1000.0f;
                        Check(exp && outside, "FadeInUnseen: a far fading-in body not drawn for 300-500 ms leaves the 1 s window; "
                                              "static meshes, the on-screen time and old times kept");
                        // rev96d A6 / A2: the window's edge (200 ms): 180 ms kept, 250 ms expired; after a 250 ms hitch (the
                        // last two frames took 0.25 s) a body last stamped 300 ms ago is still a drawn one and is kept,
                        // 500 ms is not
                        lb = lh = 99.82f;
                        const bool edgeKept = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm) == 0 && lb == 99.82f;
                        lb = lh = 99.75f;
                        const bool edgeExp = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm) == 2 && lb == expired;
                        FadeInUnseenTestSlack(0.25);
                        lb = lh = 99.70f;
                        const bool hitchKept = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm) == 0 && lb == 99.70f;
                        lb = lh = 99.5f;
                        const bool hitchExp = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm) == 2 && lb == expired;
                        FadeInUnseenTestSlack(0.0);
                        Check(edgeKept && edgeExp && hitchKept && hitchExp,
                              "FadeInUnseen: 180 ms kept, 250 ms dropped; after a 250 ms hitch 300 ms kept, 500 ms dropped (rev96d A2)");
                        lb = lh = 99.7f;
                        r = FadeInUnseenTestTick(pComp, 0, &U::Now, &U::Sm);
                        const bool off = r == 0 && lb == 99.7f;
                        *reinterpret_cast<double*>(pRoot + 0x280) = 0.0;  // near
                        r = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm);
                        const bool nearKept = r == 0 && lb == 99.7f;
                        *reinterpret_cast<double*>(pRoot + 0x280) = 30000.0;
                        *reinterpret_cast<int32_t*>(pComp + 0xD8) = 2;  // FadeOut
                        r = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm);
                        const bool outKept = r == 0 && lb == 99.7f;
                        *reinterpret_cast<int32_t*>(pComp + 0xD8) = 0;  // fade over
                        r = FadeInUnseenTestTick(pComp, 1, &U::Now, &U::Sm);
                        const bool doneKept = r == 0 && lb == 99.7f;
                        Check(off && nearKept && outKept && doneKept,
                              "FadeInUnseen: key 0, a near character, FadeOut and a finished fade are left alone");
                        *reinterpret_cast<void**>(pComp + lo) = nullptr;
                        *reinterpret_cast<int32_t*>(pComp + lo + 8) = 0;
                        *reinterpret_cast<int32_t*>(pComp + 0xD8) = 0;
                        FadeInUnseenTestSlack(-1.0);
                    }
                }
                SetNpcPacingFadeAware(0);
                NpcPacingTestRendered(nullptr);
                NpcPacingTestFade(reinterpret_cast<const uint8_t* (*)()>(fcls));
            }
        }

        // ClothBuildDefer's owner lookup: three delegates whose owner is like an equipment unit's (the character's body
        // pairing component: no pointer at the actor's root-component offset), its outer a far actor. Pacing's walk ends at
        // that owner (PacingOwnerWalk=0 for this check): the units count as near and all run in their frame. Asked from inside
        // them, NpcPacingDistantUnit and NpcPacingStillDistant walk on to the far actor, so a robe built there may wait
        SetNpcPacingOwnerWalk(false);
        {
            static FakeActor owActor{}, owRoot{}, owEquip{};
            *reinterpret_cast<void**>(owActor.bytes + 0x1A0) = &owRoot;
            *reinterpret_cast<void**>(owRoot.bytes + 0x20) = &owActor;
            *reinterpret_cast<double*>(owRoot.bytes + 0x280) = 30000.0;
            *reinterpret_cast<uintptr_t*>(owEquip.bytes + 0x1A0) = 0x10;  // no pointer: reading its outer faults
            *reinterpret_cast<void**>(owEquip.bytes + 0x20) = &owActor;
            static FakeDelInst ow[3]{};
            for (int k = 0; k < 3; ++k) {
                ow[k] = FakeDelInst{};
                ow[k].vtable = reinterpret_cast<void*>(0x1234);
                ow[k].weak = &owEquip;
                ow[k].id = 90 + k;
                ow[k].costUs = 6000;
            }
            static int askedFar, askedStill;
            static uint64_t askedOwner;
            askedFar = askedStill = 0;
            askedOwner = 0;
            g_pcInUnit = [](int) {
                uint64_t owner = 0, frame = 0;
                if (NpcPacingDistantUnit(&owner, &frame)) {
                    ++askedFar;
                    askedOwner = owner;
                }
                if (NpcPacingStillDistant(reinterpret_cast<uint64_t>(&owEquip))) ++askedStill;
            };
            g_pcDelRan.clear();
            PcPost(kFakeEnd, 0, 0);
            run(fakeMgr, -1);
            for (int k = 0; k < 3; ++k) del(&ow[k]);
            const size_t inFrame = g_pcDelRan.size();
            end(fakeMgr, nullptr, 2, 0);
            g_pcInUnit = nullptr;
            Check(inFrame == 3 && g_pcDelRan == std::vector<int>({90, 91, 92}) && NpcPacingTestRead().fifo == 0 && !g_pcDeadlock,
                  "pacing: an owner that faults at the root-component offset ends pacing's walk: near, all run at once (as on main)");
            Check(askedFar == 3 && askedOwner == reinterpret_cast<uint64_t>(&owEquip) && askedStill == 3,
                  "pacing (ClothBuildDefer): asked inside those units, the guarded walk reaches the far actor (a robe built there may wait)");
        }
        SetNpcPacingOwnerWalk(true);
        SetNpcPacing(false, 4, 4096, 15, false);
        LogNpcPacingStats();
    }


    // NavmeshBuildNoBusyWait: the navmesh tile task's start, Flags byte exe+0x41FD583 (0 = None); 1 = DoNotRunInsideBusyWait.
    if (Installed(FindNavmeshBuildNoBusyWait(), "navmesh tile task start found")) {
        uint8_t* site = NavmeshBuildNoBusyWaitSite();
        Check(site == game.base + 0x41FD583 && *site == 0 && site[-1] == 3 && site[-5] == 0x66,
              "navmesh: flags byte exe+0x41FD583 (mov word [rdi+28h], 0x0003)");
        SetNavmeshBuildNoBusyWait(true);
        Check(*site == 1 && site[-1] == 3, "navmesh on: flags = DoNotRunInsideBusyWait, priority unchanged");
        SetNavmeshBuildNoBusyWait(false);
        Check(*site == 0, "navmesh off: flags = None again");
        Check(FindNavmeshBuildNoBusyWait(), "navmesh: found again after a toggle");
    }

    // BusyWaitFlagCheck: the busy-wait put-backs (build 19115871: four TryExecuteTaskFrom<..., true> instantiations, the global
    // queue's at exe+0xDB2684), each "test dil,dil; jne <movzx eax,dil>": the jne byte 0x85 becomes jno 0x81 and back.
    if (Installed(FindBusyWaitFlagCheck(), "busy-wait put-backs found")) {
        uint8_t* bwSites[8] = {};
        const size_t n = BusyWaitFlagCheckSites(bwSites, 8);
        Check(n == 4, "busywait: four put-back sites");
        bool stock = n > 0, global = false;
        for (size_t i = 0; i < n && i < 8; ++i) {
            stock = stock && bwSites[i][0] == 0x85 && bwSites[i][-1] == 0x0F && bwSites[i][-2] == 0xFF && bwSites[i][-3] == 0x84;
            global = global || bwSites[i] == game.base + 0xDB2685;
        }
        Check(stock && global, "busywait: each site is test dil,dil; jne (0F 85), one at exe+0xDB2684");
        SetBusyWaitFlagCheck(true);
        bool on = true;
        for (size_t i = 0; i < n && i < 8; ++i) on = on && bwSites[i][0] == 0x81 && bwSites[i][-1] == 0x0F;
        Check(on, "busywait on: every site is jno (always taken after TEST), same target");
        SetBusyWaitFlagCheck(false);
        bool off = true;
        for (size_t i = 0; i < n && i < 8; ++i) off = off && bwSites[i][0] == 0x85;
        Check(off, "busywait off: every site is jne again");
        Check(FindBusyWaitFlagCheck(), "busywait: found again after a toggle");
    }

    // CreatureAnimPreload: QueuedCreature's queue step (build 19115871: exe+0x67EBF8A, call QueueModelAnims exe+0x67E9A00 at
    // exe+0x67EBFB5); flag7's immediate exe+0x67EBFAA 0 -> 1 and back, flag6's (exe+0x67EBFAF) untouched.
    if (Installed(FindCreatureAnimPreload(), "creature queue step found")) {
        uint8_t* site = CreatureAnimPreloadSite();
        Check(site == game.base + 0x67EBFAA && *site == 0 && site[-1] == 0x30 && site[-4] == 0xC6 && site[5] == 1,
              "creatureanim: flag7 byte exe+0x67EBFAA (mov byte [rsp+30h], 0), flag6 = 1 next to it");
        Check(site + 11 + 5 + *reinterpret_cast<int32_t*>(site + 12) == game.base + 0x67E9A00,
              "creatureanim: the call after it goes to QueueModelAnims exe+0x67E9A00");
        SetCreatureAnimPreload(true);
        Check(*site == 1 && site[5] == 1, "creatureanim on: flag7 = 1, flag6 unchanged");
        SetCreatureAnimPreload(false);
        Check(*site == 0, "creatureanim off: flag7 = 0 again");
        const char* caWhy = "";
        SetCreatureAnimPreload(true);
        Check(CreatureAnimPreloadLocate(game, &caWhy) == nullptr, "creatureanim: a site whose byte is already 1 is refused (a game build's own)");
        SetCreatureAnimPreload(false);
        Check(CreatureAnimPreloadLocate(game, &caWhy) == site, "creatureanim: located again after on / off (byte 0)");
    }
    {
        // Fakes: the locate step accepts only one site whose call reaches the prologue with the flag test after it.
        std::vector<uint8_t> img(0x2000, 0xCC);
        const Pattern site = ParsePattern(
            "8B 43 10 4C 8B CB 48 8B 0D ?? ?? ?? ?? 48 8B D7 48 C1 F8 10 44 0F B6 C0 48 8B 43 28 C6 44 24 30 ?? "
            "C6 44 24 28 01 48 89 44 24 20 E8 ?? ?? ?? ??");
        const Pattern pro = ParsePattern("40 55 53 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 FE FF FF 48 81 EC A0 02 00 00");
        const Pattern dec = ParsePattern("40 38 B5 10 02 00 00 0F 84 11 22 33 44 40 38 B5 08 02 00 00 0F 84");
        auto put = [&](size_t at, const Pattern& p) {
            for (size_t i = 0; i < p.size(); ++i) img[at + i] = p.care[i] ? p.bytes[i] : 0;
        };
        auto call = [&](size_t at, size_t fn) {
            const int32_t rel = static_cast<int32_t>(fn) - static_cast<int32_t>(at + 43 + 5);
            memcpy(&img[at + 44], &rel, 4);
        };
        auto locate = [&]() {
            GameModule fake{};
            fake.base = img.data();
            fake.text = {img.data(), img.data() + img.size()};
            const char* why = "";
            return CreatureAnimPreloadLocate(fake, &why);
        };
        put(0x100, site), call(0x100, 0x800), put(0x800, pro), put(0x800 + 0x250, dec);
        Check(locate() == img.data() + 0x100 + 32, "creatureanim fake: one site, call to the prologue, flag test: found");
        img[0x100 + 32] = 1;
        Check(locate() == nullptr, "creatureanim fake: flag byte already 1 (the game's own): refused");
        img[0x100 + 32] = 2;
        Check(locate() == nullptr, "creatureanim fake: flag byte 2: refused");
        img[0x100 + 32] = 0;
        img[0x800 + 0x250] = 0x41;
        Check(locate() == nullptr, "creatureanim fake: no flag test in the target: refused");
        img[0x800 + 0x250] = 0x40;
        img[0x801] = 0x56;
        Check(locate() == nullptr, "creatureanim fake: call target is another function: refused");
        img[0x801] = 0x55;
        put(0x400, site), call(0x400, 0x800);
        Check(locate() == nullptr, "creatureanim fake: two sites: refused");
    }

    // CellEvictSplit (flightrest / evfix 2026-11-13; public 1.5.0): the cell 3D removal and what is read from it, build
    // 19115871; a module without the shapes is refused, and the key cannot be switched on before it was found
    {
        std::vector<uint8_t> none(0x4000, 0xCC);
        GameModule fake{};
        fake.base = none.data();
        fake.text = {none.data(), none.data() + none.size()};
        uint32_t fo[4] = {}, fg[3] = {};
        Check(!CellEvictSplitFind(fake, fo) && !CellEvictSplitFindGates(fake, fg),
              "evictsplit: a module without the 3D removal / Set3D block / hit-effect walk is refused (the key stays off)");
        alignas(16) static uint8_t cesCell[0x200];
        SetCellEvictSplit(2);  // not installed: forced to 0
        Check(!CellEvictSplitStep(cesCell) && !CellEvictSplitTookFrame(),
              "evictsplit: not found / not installed: CellEvictSplit=2 stays off, the paced unload is the stock one");
        SetCellEvictSplit(0);
        uint32_t ces[4] = {};
        const bool cesFound = CellEvictSplitFind(game, ces);
        Check(cesFound && ces[0] == 0x6658290 && ces[1] == 0x694FA70 && ces[2] == 0x694FC80 && ces[3] == 0x941F708,
              "evictsplit: 3D removal exe+0x6658290, lock 0x694FA70, unlock 0x694FC80, player 0x941F708");
        // review rev96f B1 gates (evfix): Set3D's cell-state block 0x658B9AF and the hit-effect walk 0x6741C50
        uint32_t gates[3] = {};
        const bool gatesFound = CellEvictSplitFindGates(game, gates);
        Check(gatesFound && gates[0] == 0x94807F4 && gates[1] == 0x8FAFCB0 && gates[2] == 0x94918C8,
              "evictsplit gates: arrow count 0x94807F4, ProcessLists 0x8FAFCB0, MagicHitEffect RTTI 0x94918C8");
        // fix102 (rev102b S3): the refusal branches, on a copy of the game's code from Set3D's block (0x658B9AF) to past the
        // ProcessLists getter (0x674B050): the copy is found as the game is; a wrong second lock lea, a notify call to
        // another place, a getter of another shape or out of the code each leave the key off
        if (cesFound && gatesFound) {
            constexpr uint32_t lo = 0x658B000, hi = 0x674C000;
            // static: the pattern-search check at the end repeats every search of the self-test on its range
            static std::vector<uint8_t> img;
            img.assign(game.base + lo, game.base + hi);
            GameModule cp{};
            cp.base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(img.data()) - lo);
            cp.text = {img.data(), img.data() + img.size()};
            auto at = [&](uint32_t rva) { return img.data() + (rva - lo); };
            uint32_t co[4] = {}, cg[3] = {};
            const bool copyOk = CellEvictSplitFind(cp, co) && CellEvictSplitFindGates(cp, cg) &&
                                memcmp(co, ces, sizeof co) == 0 && memcmp(cg, gates, sizeof cg) == 0;
            auto bump = [&](uint32_t rva, int32_t by) {
                int32_t v;
                memcpy(&v, at(rva), 4);
                v += by;
                memcpy(at(rva), &v, 4);
            };
            bump(0x6658290 + 0x58 + 3, 8);  // the unlock's lock object lea no longer equals the lock's
            const bool lockRefused = !CellEvictSplitFind(cp, co);
            bump(0x6658290 + 0x58 + 3, -8);
            bump(0x658B9AF + 0x1D + 1, 16);  // Set3D's notify call goes elsewhere
            const bool notifyRefused = !CellEvictSplitFindGates(cp, cg);
            bump(0x658B9AF + 0x1D + 1, -16);
            uint8_t* const getter = RipTarget(at(0x658B9AF + 0x12), 1, 5);
            const bool getterInCopy = getter >= img.data() && getter + 8 <= img.data() + img.size();
            bool shapeRefused = false, rangeRefused = false;
            if (getterInCopy) {
                const uint8_t ret = getter[7];
                getter[7] = 0x90;  // not `mov rax,[rip+X]; ret`
                shapeRefused = !CellEvictSplitFindGates(cp, cg);
                getter[7] = ret;
                bump(0x658B9AF + 0x12 + 1, 0x01000000);  // the getter call out of the code
                rangeRefused = !CellEvictSplitFindGates(cp, cg);
                bump(0x658B9AF + 0x12 + 1, -0x01000000);
            }
            const bool restored = CellEvictSplitFind(cp, co) && CellEvictSplitFindGates(cp, cg);
            Check(copyOk && lockRefused && notifyRefused && getterInCopy && shapeRefused && rangeRefused && restored,
                  "evictsplit (fix102 S3): a copy of the code is found as the game; a different lock object, a notify call "
                  "elsewhere, a getter of another shape or outside the code each refuse (the key stays off)");
        }
        // fix102 (rev102b M1): the split decision on fake objects (every guard of rev96f B1 / F1)
        {
            using namespace ces;
            CellEvictSplitFakes fk{&Lock, &Unlock, &depth, &player, &arrows, &plPtr, rttiHit};
            CellEvictSplitTestFakes(&fk);
            uint64_t cnt[8] = {};
            CellEvictSplitTestCounts(cnt);
            auto step = [&](int per) {
                SetCellEvictSplit(per);
                const bool r = CellEvictSplitStep(cell);
                SetCellEvictSplit(0);
                return r;
            };
            bool balanced = true;  // the lock is released on every return
            auto stepB = [&](int per) {
                const bool r = step(per);
                balanced = balanced && depth == 0;
                return r;
            };
            // per 0: nothing, not even the lock
            Build("nnn");
            const bool off = !stepB(0) && calls.empty() && locks == 0;
            // B1.1: arrows live: the stock unload, no call
            Build("nnn");
            arrows = 1;
            const bool arrowOff = !stepB(2) && calls.empty();
            CellEvictSplitTestCounts(cnt);
            const bool arrowCounted = cnt[2] == 1 && cnt[6] == 1;
            // B1.3: one eligible actor and one hit target: the stock unload, no call
            Build("nn");
            AddHit(1);
            const bool oneEligible = !stepB(2) && calls.empty();
            // B1.2: a hit target is never cleared ahead; the others are, in list order
            Build("nnn");
            AddHit(1);
            const bool h1 = stepB(2);
            const bool h2 = !stepB(2);
            const bool hitKept = h1 && h2 && calls == std::vector<int>({0, 2});
            // N a step, in list order, statics and the player never, then the stock unload once fewer than 2 are left
            Build("nsncsnpns");
            const bool o1 = stepB(2), o2 = stepB(2), o3 = !stepB(2);
            const bool order = o1 && o2 && o3 && calls == std::vector<int>({0, 2, 3, 5}) ;
            // F1: a cell whose actors keep their 3D: at most ceil(4 / 2) + 1 = 3 steps, then the stock unload
            Build("nnnn");
            keep3d = true;
            CellEvictSplitTestCounts(cnt);
            int trueSteps = 0;
            for (int k = 0; k < 6 && stepB(2); ++k) ++trueSteps;
            CellEvictSplitTestCounts(cnt);
            const bool limit = trueSteps == 3 && cnt[4] == 1 && calls.size() == 6;
            // B1.1 mid-step: the first Set3D creates an arrow: the step ends after it, the next call is the stock unload
            Build("nnnn");
            arrowAfter = 1;
            const bool m1 = stepB(2), m2 = !stepB(2);
            const bool midArrow = m1 && m2 && calls == std::vector<int>({0});
            // a step that clears nothing (its one actor became a hit target between the walk and the call): the stock
            // unload, counted as an empty step
            Build("nn");
            hitOnBaseOf = 1, hitTarget = 0;
            CellEvictSplitTestCounts(cnt);
            const bool e1 = !stepB(1);
            CellEvictSplitTestCounts(cnt);
            const bool empty = e1 && calls.empty() && cnt[5] == 1;
            // a creature counts as an actor; one actor without 3D does not
            Build("cxn");
            const bool crea1 = stepB(2), crea2 = !stepB(2);
            const bool creature = crea1 && crea2 && calls == std::vector<int>({0, 2});
            const bool underLock = unlocked == 0 && unlockedCalls == 0;
            CellEvictSplitTestFakes(nullptr);
            Check(off && arrowOff && arrowCounted && oneEligible,
                  "evictsplit (fix102 M1): key 0: no call, no lock; arrows live: the stock unload; one eligible actor beside a "
                  "hit target: the stock unload (no actor cleared ahead)");
            Check(hitKept && order && creature,
                  "evictsplit (fix102 M1): N actors a step in the game's list order (creatures too), never the player, a static "
                  "or a hit-effect target; fewer than two left: the stock unload");
            Check(limit && midArrow && empty,
                  "evictsplit (fix102 M1): at most ceil(n / N) + 1 steps a cell; an arrow created mid-step ends the step and "
                  "the split; a step that clears nothing is the stock unload");
            Check(underLock && balanced,
                  "evictsplit (fix102 S1): the cell's list is walked and every actor cleared under the game's lock, released "
                  "on every return");
        }
    }
    // PacingGateShownHoldMs (hold2 2026-11-13; public 1.5.0): the fade applier and the fade component's layout of build 19115871,
    // then the decision on simulated spawn fade-ins (60 fps, 4 per second): what reaches the meshes is never below the
    // writer's own value, the component and the actor are never written, a held far character stays at the max until its
    // units ran / the bound / the fade's clock, then never rises, at 5 / value_release times the fade's speed (holdfix F6:
    // 2x for the cases below released at 2.5, 3.3x at End 1.0), and the fade's end call (state 0) passes unchanged.
    {
        uint32_t sh[8] = {};
        const bool shFound = ShownHoldFind(sh);
        Check(shFound && sh[0] == 0x48CD850 && sh[1] == 0xD8 && sh[2] == 0xBC && sh[3] == 0xC0 && sh[4] == 0xD0 &&
                  sh[5] == 0xCC && sh[6] == 0xA8 && sh[7] == 0x7A0,
              "shownhold: applier exe+0x48CD850; fade component state +0xD8, value +0xBC, rate +0xC0, max +0xD0, floor +0xCC, "
              "owner +0xA8; the actor's fade component +0x7A0");
        if (shFound) {
            alignas(16) static uint8_t shComp[0x200], shActor[0x800], shOther[0x200];
            static uint64_t shNow;
            static int shTick, shHeldFrom, shHeldUntil, shEnd = 20;
            shEnd = 20;  // set at run time: the static initialiser was seen as 0 on the first spawns of the lab build (check 3 then ran with end 0)
            auto putF = [](uint8_t* p, uint32_t off, float v) { memcpy(p + off, &v, 4); };
            auto putI = [](uint8_t* p, uint32_t off, int32_t v) { memcpy(p + off, &v, 4); };
            auto putP = [](uint8_t* p, uint32_t off, const void* v) { memcpy(p + off, &v, 8); };
            struct ShRun {
                std::vector<float> arg, shown;  // every call in order (ticks and the floor setter's)
                std::vector<char> isTick;
                bool untouched = true;
                float endArg = -1.0f, endShown = -2.0f, levelHeld = -1.0f, levelEnd = -1.0f;
                uint64_t c[6] = {};
            };
            // one spawn: SetFadeState (value = max 5), then the tick at 60 fps until the fade ends (its end call: state 0).
            // floor: the game's floor; foreign >= 0: the floor setter's own call (floor alone) after every 3rd tick
            auto spawn = [&](int ms, int gt, int heldFrom, int heldUntil, float floor, float foreign, bool reset, bool ownComp) {
                ShRun r;
                shHeldFrom = heldFrom, shHeldUntil = heldUntil;
                if (reset)
                    ShownHoldTestSetup(+[](uintptr_t) { return shTick >= shHeldFrom && shTick < shHeldUntil; },
                                       +[]() { return shNow; }, gt, ms, shEnd);
                putP(shComp, 0xA8, shActor);
                putP(shActor, 0x7A0, ownComp ? shComp : shOther);
                putI(shComp, 0xD8, 1);
                putF(shComp, 0xC0, 4.0f), putF(shComp, 0xD0, 5.0f), putF(shComp, 0xCC, floor);
                float v = 5.0f;
                for (shTick = 0; shTick < 200; ++shTick) {
                    shNow += 16667;
                    v -= 4.0f / 60.0f;
                    const bool end = v < 0.0f;
                    if (end) v = 0.0f, putI(shComp, 0xD8, 0);
                    putF(shComp, 0xBC, v);
                    alignas(16) static uint8_t c0[0x200], a0[0x800];
                    memcpy(c0, shComp, sizeof c0), memcpy(a0, shActor, sizeof a0);
                    const float arg = std::max(floor, v);
                    const float shown = ShownHoldTestCall(shComp, shActor, arg);
                    if (memcmp(c0, shComp, sizeof c0) || memcmp(a0, shActor, sizeof a0)) r.untouched = false;
                    if (end) {
                        r.endArg = arg, r.endShown = shown, r.levelEnd = ShownHoldLevel(shComp);
                        break;
                    }
                    if (shTick == 10) r.levelHeld = ShownHoldLevel(shComp);
                    r.arg.push_back(arg), r.shown.push_back(shown), r.isTick.push_back(1);
                    if (foreign >= 0.0f && shTick % 3 == 1) {
                        const float fs = ShownHoldTestCall(shComp, shActor, foreign);
                        if (memcmp(c0, shComp, sizeof c0) || memcmp(a0, shActor, sizeof a0)) r.untouched = false;
                        r.arg.push_back(foreign), r.shown.push_back(fs), r.isTick.push_back(0);
                    }
                }
                ShownHoldTestCounts(r.c);
                return r;
            };
            auto never = [](const ShRun& r) {  // never more visible than the game: shown >= the writer's value, every call
                for (size_t i = 0; i < r.arg.size(); ++i)
                    if (r.shown[i] < r.arg[i]) return false;
                return r.endShown >= r.endArg;
            };
            auto same = [](const ShRun& r) {  // the game's value everywhere
                for (size_t i = 0; i < r.arg.size(); ++i)
                    if (r.shown[i] != r.arg[i]) return false;
                return r.endShown == r.endArg;
            };
            // ticks at the max after the hold opened; the opening jump; the largest tick-to-tick rise after it and fall
            struct ShShape {
                int atMax = 0;
                float openJump = 0, rise = 0, fall = 0;
            };
            auto shape = [](const ShRun& r) {
                ShShape s;
                float prev = -1.0f;
                bool open = false;
                for (size_t i = 0; i < r.arg.size(); ++i) {
                    if (!r.isTick[i]) continue;
                    const float x = r.shown[i];
                    if (!open && x >= 5.0f) open = true, s.openJump = prev >= 0.0f ? x - prev : 0.0f;
                    else if (open && prev >= 0.0f) s.rise = std::max(s.rise, x - prev), s.fall = std::max(s.fall, prev - x);
                    if (open && x >= 5.0f) ++s.atMax;
                    prev = x;
                }
                return s;
            };
            const float stockStep = 4.0f / 60.0f;
            // (1) units run after 20 ticks; far floor 4 with the floor setter writing 2.9 in between (rev96e F3)
            const ShRun r1 = spawn(500, 1, 1, 20, 4.0f, 2.9f, true, true);
            const ShShape s1 = shape(r1);
            Check(r1.untouched && never(r1) && r1.c[0] == 1 && r1.c[2] == 1 && r1.c[5] == 0 && s1.atMax == 20 && s1.rise == 0.0f &&
                      s1.openJump <= 0.25f && r1.endShown == r1.endArg && r1.levelHeld == 5.0f && r1.levelEnd == 0.0f,
                  "shownhold: units ran after 19 held ticks (20 at the max: the release tick shows the max): the meshes stay at 5 meanwhile (also against the floor setter's 2.9), "
                  "then fade without rising; the fade's end call passes unchanged; nothing written");
            // (2) held for ever, bound 500 ms: released by the bound (~30 ticks), at most twice the stock speed afterwards
            const ShRun r2 = spawn(500, 1, 1, 1000, 0.0f, -1.0f, true, true);
            const ShShape s2 = shape(r2);
            Check(r2.untouched && never(r2) && r2.c[0] == 1 && r2.c[3] == 1 && s2.atMax >= 30 && s2.atMax <= 31 && s2.rise == 0.0f &&
                      s2.fall <= 2.1f * stockStep && r2.endShown == r2.endArg,
                  "shownhold: held for ever: released by PacingGateShownHoldMs (500 ms), then at most about twice the fade's "
                  "speed, reaching the game's value at its end");
            // (3) end 2.0 (released at 2.5 = end + the soft window), bound 1000 ms: the fade's own clock releases it at 0.625 s
            const ShRun r3 = spawn(1000, 1, 1, 1000, 0.0f, -1.0f, true, true);
            const ShShape s3 = shape(r3);
            Check(r3.untouched && never(r3) && r3.c[0] == 1 && r3.c[4] == 1 && s3.atMax >= 36 && s3.atMax <= 37 && s3.rise == 0.0f &&
                      s3.fall <= 2.1f * stockStep && r3.endShown == r3.endArg,
                  "shownhold: PacingGateShownHoldEnd 2.0, a bound past it: released by the fade's clock at value 2.5; the end "
                  "unchanged");
            // (3b) PacingGateShownHoldEnd 0.75 (released at 1.25), bound 2000 ms: at 0.94 s, then at most about 4x
            shEnd = 7;
            const ShRun r3b = spawn(2000, 1, 1, 1000, 0.0f, -1.0f, true, true);
            shEnd = 20;
            const ShShape s3b = shape(r3b);
            printf("      (shownhold end 0.7: %d ticks at the max, largest fall %.3f per tick)\n", s3b.atMax, s3b.fall);
            Check(r3b.untouched && never(r3b) && r3b.c[0] == 1 && r3b.c[4] == 1 && s3b.atMax >= 55 && s3b.atMax <= 57 &&
                      s3b.rise == 0.0f && s3b.fall <= 4.6f * stockStep && r3b.endShown == r3b.endArg,
                  "shownhold: PacingGateShownHoldEnd 0.7: released by the fade's clock at value 1.2, then at most about 4x the "
                  "fade's speed; the end unchanged");
            // (4) not held / (5) found too late (value 4.6) / (6) key 0 / (7) not the game thread / (8) not the actor's own
            const ShRun r4 = spawn(500, 1, 0, 0, 4.0f, 2.9f, true, true);
            const ShRun r5 = spawn(500, 1, 5, 1000, 0.0f, -1.0f, true, true);
            const ShRun r6 = spawn(0, 1, 1, 1000, 0.0f, -1.0f, true, true);
            const ShRun r7 = spawn(500, 0, 1, 1000, 0.0f, -1.0f, true, true);
            const ShRun r8 = spawn(500, 1, 1, 1000, 0.0f, -1.0f, true, false);
            Check(same(r4) && r4.c[0] == 0 && same(r5) && r5.c[0] == 0 && r5.c[1] == 1 && same(r6) && same(r7) && same(r8) &&
                      r4.untouched && r5.untouched && r8.untouched,
                  "shownhold: the game's value when the gate holds nothing, when first held past the fade's first quarter "
                  "unit (not hidden again), with the key 0, off the game thread, or for another actor's fade component");
            // (9) one hold per spawn: a second spawn on the same component (after the end) is held again; within one spawn a
            // hold that was released is not opened again although the units are held again
            const ShRun r9a = spawn(500, 1, 1, 20, 0.0f, -1.0f, true, true);
            const ShRun r9 = spawn(500, 1, 1, 20, 0.0f, -1.0f, false, true);
            shHeldFrom = 1, shHeldUntil = 1000;
            const ShRun r10 = [&] {
                ShownHoldTestSetup(+[](uintptr_t) { return (shTick >= 1 && shTick < 10) || shTick >= 15; }, +[]() { return shNow; },
                                   1, 500, 20);
                return spawn(500, 1, 1, 1000, 0.0f, -1.0f, false, true);
            }();
            const ShShape s10 = shape(r10);
            ShownHoldTestQueued(+[](uintptr_t) { return true; });
            ShownHoldTestAiEarly();
            const ShRun r11 = spawn(500, 1, 1, 1000, 0.0f, -1.0f, true, true);
            const uint64_t early11 = ShownHoldTestAiEarly();
            ShownHoldTestQueued(+[](uintptr_t) { return false; });
            const ShRun r12 = spawn(500, 1, 1, 1000, 0.0f, -1.0f, true, true);
            const uint64_t early12 = ShownHoldTestAiEarly();
            ShownHoldTestQueued(nullptr);
            Check(never(r11) && early11 == 1 && early12 == 0,
                  "shownhold: a held fade that ends with the actor's units still queued is counted (its AI starts before it is "
                  "built); none when they ran");
            Check(r9a.c[0] == 1 && never(r9) && r9.c[0] == 1 && never(r10) && r10.c[0] == 1 && r10.c[2] == 1 && s10.atMax == 10 && s10.rise == 0.0f,
                  "shownhold: a new spawn is held again; within one spawn a released hold never opens again");
            // holdfix F3: a fade that is never held (the gate holds nothing) and ends with units queued is counted too
            ShownHoldTestQueued(+[](uintptr_t) { return true; });
            ShownHoldTestAiEarly();
            const ShRun r13 = spawn(500, 1, 0, 0, 0.0f, -1.0f, true, true);
            const uint64_t early13 = ShownHoldTestAiEarly();
            ShownHoldTestQueued(nullptr);
            Check(same(r13) && r13.c[0] == 0 && early13 == 1,
                  "shownhold (holdfix F3): a fade never held that ends with the actor's units queued is counted (AI before built)");
            // fix102 (rev102a M1 / M2 / S3a): the equipment-first mark and the urgent list on the hook's own slots (fake
            // owners whose UObject InternalIndex at +0xC can change: another object at the same address)
            {
                alignas(16) static uint8_t own[8][0x40];
                auto setIdx = [](uint8_t* o, int32_t v) { memcpy(o + 0xC, &v, 4); };
                for (int i = 0; i < 8; ++i) setIdx(own[i], 100 + i);
                auto O = [](int i) { return reinterpret_cast<uintptr_t>(own[i]); };
                auto C = [](int i) { return static_cast<uintptr_t>(0x7000 + i * 0x10); };
                ShownHoldTestSetup(+[](uintptr_t) { return true; }, +[]() { return shNow; }, 1, 1000, 10);
                auto urgentLeft = [&](int i) {  // the seconds left of owner i in the urgent list, -1: not listed
                    uintptr_t out[64];
                    float left[64];
                    const int n = ShownHoldUrgentOwners(out, left, 64);
                    for (int k = 0; k < n; ++k)
                        if (out[k] == O(i)) return left[k];
                    return -1.0f;
                };
                auto touch = [&](std::initializer_list<int> live) {
                    for (int i : live) ShownHoldTestHold(C(i), O(i), true);
                };
                shNow += 1000000;
                touch({0});
                const float u0none = urgentLeft(0);
                ShownHoldNoteEquipFirst(O(0));
                const float u0a = urgentLeft(0);
                shNow += 50000, touch({0});
                const float u0b = urgentLeft(0);
                shNow += 60000, touch({0});
                const float u0c = urgentLeft(0);
                Check(u0none < 0.0f && std::fabs(u0a - 0.1f) < 1e-4f && std::fabs(u0b - 0.05f) < 1e-4f && u0c == 0.0f,
                      "shownhold (fix102 M1): an equipment unit run during an open hold gives the actor a deadline 0.1 s later "
                      "(the urgent list's time left falls with the clock, 0 past it)");
                // M2: a marked hold not called for 0.1 s is not listed; another object at the owner's address is not listed
                touch({1, 2});
                ShownHoldNoteEquipFirst(O(1));
                ShownHoldNoteEquipFirst(O(2));
                const bool both = urgentLeft(1) > 0.0f && urgentLeft(2) > 0.0f;
                shNow += 120000, touch({0, 2});
                const float u1stale = urgentLeft(1), u2live = urgentLeft(2);
                setIdx(own[2], 999);  // a new object at owner 2's address
                const float u2reused = urgentLeft(2);
                setIdx(own[2], 102);
                Check(both && u1stale < 0.0f && u2live >= 0.0f && u2reused < 0.0f,
                      "shownhold (fix102 M2): a marked hold not called for 0.1 s (its actor destroyed) or with another object "
                      "at its owner's address is not urgent");
                // M2: the sweep frees held slots not called for 2 s (not only when the table is full)
                const int activeBefore = ShownHoldTestHold(0, 0, false);
                for (int k = 0; k < 10; ++k) shNow += 250000, touch({0}), urgentLeft(0);
                const int activeAfter = ShownHoldTestHold(0, 0, false);
                Check(activeBefore == 3 && activeAfter == 1,
                      "shownhold (fix102 M2): held slots of actors gone for 2 s are swept (the hook's active count falls)");
                // S3a: equipment that ran before the hold opened marks it when it opens (deadline from that unit); an old note,
                // or one for another object at the address, does not
                ShownHoldNoteEquipFirst(O(4));
                shNow += 30000, touch({0, 4});
                const float u4 = urgentLeft(4);
                ShownHoldNoteEquipFirst(O(5));
                setIdx(own[5], 555);
                shNow += 10000, touch({0, 4, 5});
                const float u5 = urgentLeft(5);
                ShownHoldNoteEquipFirst(O(6));
                shNow += 600000, touch({0, 4, 5, 6});
                const float u6 = urgentLeft(6);
                printf("      (shownhold equipment-first: left %.3f / %.3f / %.3f s; before the open %.3f, reused %.3f, old %.3f)\n",
                       u0a, u0b, u0c, u4, u5, u6);
                Check(std::fabs(u4 - 0.07f) < 1e-4f && u5 < 0.0f && u6 < 0.0f,
                      "shownhold (fix102 S3a): equipment that ran just before the hold opened marks it at the open (deadline "
                      "from that unit); a note older than 0.5 s or for another object at the address does not");
                ShownHoldTestSetup(nullptr, nullptr, -1, 0, 10);
            }
        }
        // holdfix F2 (2026-11-13): the due rule's deadline spread (ShownHoldDueSpread) and the runner's order (earliest
        // deadline first; every due-now unit; one soft-due unit a frame) on simulated frames. chars characters with units
        // held units each that never fit (only the due rule runs them), spawned gap frames apart at fade value v0; the fade
        // falls 4 per second; PacingGateShownHoldEnd 1.0 (due now at value 1, 0.25 s before the end), soft window 0.125 s.
        // A unit still queued at its character's fade end = AI before built; most units in one frame = the burst.
        {
            struct DueSim {
                int maxFrame = 0, leftAtEnd = 0, dueNow = 0, maxRate = 0;
            };
            // jump > 0: every jump-th frame takes 3x the period (rev96g delta: the model had constant periods only); the
            // spread is given the larger of the second longest and the mean of the last 8 periods, as GatePeriodS does
            // (fix102 S2), and the frame's
            // rate is the highest the spread asked for in that frame (fix102 S1, the runner's g_dueRate)
            auto sim = [](float fps, int chars, int units, int gap, float v0, int jump = 0) {
                DueSim r;
                const float base = 1.0f / fps, endV = 1.0f, softS = 0.5f / 4.0f;
                std::vector<float> v(chars, v0);
                std::vector<int> left(chars, units);
                std::vector<float> ring(8, base);
                for (int f = 0; f < 4000; ++f) {
                    const float period = jump > 0 && f % jump == jump - 1 ? 3.0f * base : base;
                    float seen1 = 0.0f, seen = 0.0f, mean = 0.0f;  // the spread's period: max(second longest, mean) of 8
                    for (float x : ring) {
                        mean += x / 8.0f;
                        if (x > seen1) seen = seen1, seen1 = x;
                        else if (x > seen) seen = x;
                    }
                    seen = std::max(seen, mean);
                    bool live = false;
                    for (int c = 0; c < chars; ++c) {
                        if (f < c * gap || v[c] <= 0.0f) {
                            live = live || f < c * gap;
                            continue;
                        }
                        v[c] -= 4.0f * period;
                        if (v[c] <= 0.0f) {  // the fade's end (its AI switches on)
                            r.leftAtEnd += left[c];
                            left[c] = 0;
                            continue;
                        }
                        live = true;
                    }
                    ring.insert(ring.begin(), period);
                    ring.pop_back();
                    if (!live) break;
                    int ran = 0, softRan = 0, frameRate = 1;
                    for (int guard = 0; guard < 256; ++guard) {
                        std::vector<float> t;
                        std::vector<int> who;
                        std::vector<uint8_t> cls;  // 0 not due, 1 soft, 2 now
                        std::vector<float> st;
                        std::vector<size_t> sidx;
                        for (int c = 0; c < chars; ++c) {
                            if (f < c * gap || v[c] <= 0.0f) continue;
                            for (int u = 0; u < left[c]; ++u) {
                                const bool now = v[c] <= endV;
                                t.push_back(now ? 0.0f : (v[c] - endV) / 4.0f);
                                who.push_back(c);
                                cls.push_back(now ? 2 : 0);
                                if (!now) st.push_back(t.back()), sidx.push_back(t.size() - 1);
                            }
                        }
                        int rate = 1;
                        if (!st.empty()) {
                            std::vector<uint8_t> dd(st.size());
                            rate = ShownHoldDueSpread(st.data(), st.size(), seen, softS, dd.data());
                            r.maxRate = std::max(r.maxRate, rate);
                            for (size_t k = 0; k < st.size(); ++k)
                                if (dd[k]) cls[sidx[k]] = 1;
                        }
                        frameRate = std::max(frameRate, rate);
                        size_t pick = t.size();
                        for (size_t k = 0; k < t.size(); ++k) {
                            const bool ok = cls[k] == 2 || (cls[k] == 1 && softRan < frameRate);
                            if (ok && (pick == t.size() || t[k] < t[pick])) pick = k;
                        }
                        if (pick == t.size()) break;
                        if (cls[pick] == 2) ++r.dueNow;
                        else ++softRan;
                        --left[who[pick]];
                        ++ran;
                    }
                    r.maxFrame = std::max(r.maxFrame, ran);
                }
                return r;
            };
            bool allClear = true, oneAFrame = true;
            for (float fps : {60.0f, 45.0f, 30.0f, 20.0f})
                for (int k : {1, 4, 10}) {
                    const DueSim one = sim(fps, 1, k, 0, 5.0f);   // one character with k held units
                    const DueSim many = sim(fps, k, 1, 0, 5.0f);  // k characters with one unit each, spawned together
                    const DueSim step = sim(fps, k, 2, 1, 5.0f);  // k characters with two units, a frame apart
                    printf("      (due spread %2.0f fps, %2d units: most a frame %d / %d / %d, due-now runs %d / %d / %d)\n", fps,
                           k, one.maxFrame, many.maxFrame, step.maxFrame, one.dueNow, many.dueNow, step.dueNow);
                    allClear = allClear && one.leftAtEnd == 0 && many.leftAtEnd == 0 && step.leftAtEnd == 0;
                    // port102: at 20 fps the spread's rate may take two a frame (10 characters a frame apart, 2 units each)
                    const int most = fps < 25.0f ? 2 : 1;
                    oneAFrame = oneAFrame && one.maxFrame <= most && many.maxFrame <= most && step.maxFrame <= most &&
                                one.dueNow == 0 && many.dueNow == 0 && step.dueNow == 0;
                }
            // found late (value 1.2: 0.05 s before due now) with 10 units at 30 fps: the hard backstop (due now) clears them
            const DueSim late = sim(30.0f, 1, 10, 0, 1.2f);
            printf("      (due spread late 30 fps 10 units: most a frame %d, due-now runs %d)\n", late.maxFrame, late.dueNow);
            Check(allClear && late.leftAtEnd == 0,
                  "shownhold (holdfix F2): at 60 / 45 / 30 / 20 fps with 1, 4 and 10 held units (one or many characters) no fade "
                  "ends with units queued; a late find is cleared by due now");
            Check(oneAFrame,
                  "shownhold (holdfix F2): from the fade's start the deadline spread runs at most one due unit a frame (two at "
                  "20 fps) and never needs due now (60 / 45 / 30 / 20 fps, 1-10 units)");
            // rev96g F2 (port102): more units than one a frame can clear (20 fps, 24 units of one character: the frames
            // before its due now hold about 16 at 1.25 periods each) and the late find: the spread raises its rate instead
            // of leaving the rest to one due-now frame (the late find before: 1 soft, then 9 in one due-now frame)
            const DueSim many20 = sim(20.0f, 1, 24, 0, 5.0f);
            const DueSim late4 = sim(30.0f, 1, 10, 0, 1.2f);
            printf("      (due spread 20 fps 24 units: most a frame %d, rate %d, due-now runs %d; late 30 fps 10 units: most a "
                   "frame %d, rate %d, due-now runs %d)\n",
                   many20.maxFrame, many20.maxRate, many20.dueNow, late4.maxFrame, late4.maxRate, late4.dueNow);
            Check(many20.leftAtEnd == 0 && many20.maxRate == 2 && many20.maxFrame <= 2 && many20.dueNow == 0 &&
                      late4.leftAtEnd == 0 && late4.maxRate == 4 && late4.maxFrame <= 6,
                  "shownhold (rev96g F2): when one a frame cannot meet the deadlines the spread runs the least rate that can "
                  "(at most 4 a frame) instead of one due-now heap (late find: 4 + 6 instead of 1 + 9); due now stays the "
                  "backstop past that");
            // fix102 (rev102a S1): when a rate of at most 4 suffices, no unit is left to due now: 4 units 10 ms before their
            // due now at 60 fps run together (rate 4; before: 2, then 2 as due now); 6 units 70 ms before it at 20 fps run
            // 3 + 3 (rate 3; before: 2 + 2, then 2 as due now)
            const DueSim fit60 = sim(60.0f, 1, 4, 0, 1.0f + 4.0f * (0.01f + 1.0f / 60.0f));
            const DueSim fit20 = sim(20.0f, 1, 6, 0, 1.0f + 4.0f * (0.07f + 1.0f / 20.0f));
            printf("      (due spread, the frame's rate latched: 60 fps 4 units 10 ms before due now: most a frame %d, due-now %d; "
                   "20 fps 6 units 70 ms before: most a frame %d, due-now %d)\n",
                   fit60.maxFrame, fit60.dueNow, fit20.maxFrame, fit20.dueNow);
            Check(fit60.leftAtEnd == 0 && fit60.dueNow == 0 && fit60.maxFrame == 4 && fit20.leftAtEnd == 0 && fit20.dueNow == 0 &&
                      fit20.maxFrame == 3,
                  "shownhold (fix102 S1): the spread's rate holds for the whole frame: when a rate of 4 or less can meet the "
                  "deadlines no unit is left to due now");
            // fix102 (rev102a S2): 10 units 0.6 s before due now at 60 fps after one 100 ms hitch: with the period now (the
            // mean of 8, 27 ms) none is due yet (rate 1); with the hitch as the period (the longest of 8, before) all 10
            // were soft-due at once at rate 2
            {
                std::vector<float> t10(10, 0.6f);
                std::vector<uint8_t> d10(10);
                const int rTrue = ShownHoldDueSpread(t10.data(), 10, (0.1f + 7.0f / 60.0f) / 8.0f, 0.125f, d10.data());
                const int nTrue = static_cast<int>(std::count(d10.begin(), d10.end(), uint8_t(1)));
                const int rHitch = ShownHoldDueSpread(t10.data(), 10, 0.1f, 0.125f, d10.data());
                const int nHitch = static_cast<int>(std::count(d10.begin(), d10.end(), uint8_t(1)));
                Check(rTrue == 1 && nTrue == 0 && rHitch == 2 && nHitch == 10,
                      "shownhold (fix102 S2): 10 units 0.6 s before due now at 60 fps after one 100 ms hitch: none due yet "
                      "with the period now (the mean of 8; with the hitch as the period: all 10 at rate 2)");
            }
            // rev96g delta: frames of changing length (every 5th / 9th frame 3x as long) with 4 and 10 units at 45 / 30 fps
            bool jumpClear = true;
            int jumpMost = 0;
            for (float fps : {45.0f, 30.0f})
                for (int k : {4, 10})
                    for (int jump : {5, 9}) {
                        const DueSim j = sim(fps, 1, k, 0, 5.0f, jump), jm = sim(fps, k, 1, 1, 5.0f, jump);
                        jumpClear = jumpClear && j.leftAtEnd == 0 && jm.leftAtEnd == 0;
                        jumpMost = std::max({jumpMost, j.maxFrame, jm.maxFrame});
                    }
            printf("      (due spread with long frames: most a frame %d)\n", jumpMost);
            Check(jumpClear && jumpMost <= 2,  // fix102: the second longest alone gave 3 here
                  "shownhold (rev96g delta): with frames of changing length (3x every 5th / 9th) no fade ends with units "
                  "queued and no frame runs more than two due units");
        }
        ShownHoldTestSetup(nullptr, nullptr, -1, 0, 10);
        ShownHoldTestQueued(nullptr);
    }

    // GpuSceneKeepCapacity: the GPU scene buffers (build 19115871): sites, then the thunk end to end (a caller that sets edi and rsi as
    // UpdateBufferState does) and the policy through both hooks, on fakes.
    if (Installed(ApplyGpuSceneKeepCapacityFix(true), "gpu scene keep-capacity fix applied")) {
        GpuSceneFixInfo gs{};
        Check(GpuSceneInfo(&gs), "gpuscene: info available");
        Check(gs.strideCall[0] == game.base + 0x1DD8913 && gs.strideCall[1] == game.base + 0x1DD8E9E &&
                  gs.strideFn == game.base + 0x329BD00,
              "gpuscene: instance reserve calls exe+0x1DD8913 / exe+0x1DD8E9E -> GetDataStrideInFloat4s exe+0x329BD00");
        Check(gs.resizeCall[0] == game.base + 0x1DD88C0 && gs.resizeCall[1] == game.base + 0x1DD8E51 &&
                  gs.resizeCall[2] == game.base + 0x1DD899E && gs.resizeCall[3] == game.base + 0x1DD8F29 &&
                  gs.resizeFn == game.base + 0x270CB10,
              "gpuscene: PrimitiveData and InstancePayloadData resize calls (x2 each) -> exe+0x270CB10");
        bool redirected = RipTarget(gs.strideCall[0], 1, 5) == RipTarget(gs.strideCall[1], 1, 5);
        for (uint8_t* c : gs.strideCall) redirected &= RipTarget(c, 1, 5) != gs.strideFn;
        for (uint8_t* c : gs.resizeCall) redirected &= RipTarget(c, 1, 5) != gs.resizeFn;
        Check(redirected, "gpuscene: all 6 calls redirected (the two reserve calls share the thunk's stub)");
        GpuSceneTestOriginals(reinterpret_cast<void*>(&FakeStride), reinterpret_cast<void*>(&FakeResizeStructured));
        SetGpuSceneKeepCapacityEnabled(true);
        // push rdi; push rsi; mov edi,ecx; mov rsi,rdx; sub rsp,28h; call r8; mov eax,edi; add rsp,28h; pop rsi; pop rdi; ret
        static const uint8_t kCaller[] = {0x57, 0x56, 0x89, 0xCF, 0x48, 0x89, 0xD6, 0x48, 0x83, 0xEC, 0x28, 0x41, 0xFF,
                                          0xD0, 0x89, 0xF8, 0x48, 0x83, 0xC4, 0x28, 0x5E, 0x5F, 0xC3};
        void* const mem = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        Check(mem != nullptr, "gpuscene: test caller allocated");
        if (mem) {
            memcpy(mem, kCaller, sizeof kCaller);
            using CallerFn = uint32_t (*)(uint32_t need, uint8_t* gpuScene, void* target);
            const auto reserve = [&](uint32_t need, uint8_t* scene) {
                return reinterpret_cast<CallerFn>(mem)(need, scene, RipTarget(gs.strideCall[0], 1, 5));
            };
            alignas(16) uint8_t scene[0x400] = {};
            void*& instanceBuffer = *reinterpret_cast<void**>(scene + 0x30);
            Check(reserve(1u << 20, scene) == 1u << 20, "gpuscene: a new instance buffer gets what the game asks for");
            instanceBuffer = scene;  // it exists now
            Check(reserve(1u << 21, scene) == 1u << 21, "gpuscene: growing passes through");
            Check(reserve(1u << 20, scene) == 1u << 21, "gpuscene on: one step down keeps the capacity");
            Check(reserve(1u << 20, scene) == 1u << 21, "gpuscene on: and keeps it on the next frames");
            Check(reserve(1u << 19, scene) == 1u << 19, "gpuscene on: a quarter of the capacity shrinks it");
            SetGpuSceneKeepCapacityEnabled(false);
            Check(reserve(1u << 21, scene) == 1u << 21 && reserve(1u << 20, scene) == 1u << 20,
                  "gpuscene off: the instance buffer resizes as the game asks");
            SetGpuSceneKeepCapacityEnabled(true);
            Check(reserve(1u << 21, scene) == 1u << 21 && reserve(1u << 20, scene) == 1u << 21,
                  "gpuscene on again: kept after a grow");
            VirtualFree(mem, 0, MEM_RELEASE);
        }
        using ResizeFn = void* (*)(void*, void**, uint32_t, const wchar_t*);
        const auto resize = reinterpret_cast<ResizeFn>(gs.resizeHook);
        void* primitives = nullptr;
        resize(nullptr, &primitives, 64u << 20, gs.name[1]);
        Check(g_fakeResizeBytes == 64u << 20, "gpuscene: a new primitive buffer gets what the game asks for");
        primitives = &primitives;
        resize(nullptr, &primitives, 128u << 20, gs.name[1]);
        const bool grew = g_fakeResizeBytes == 128u << 20;
        resize(nullptr, &primitives, 64u << 20, gs.name[1]);
        Check(grew && g_fakeResizeBytes == 128u << 20, "gpuscene on: primitive buffer grows, then keeps its capacity one step down");
        void* other = &other;
        resize(nullptr, &other, 1u << 20, L"GPUScene.LightmapData");
        resize(nullptr, &other, 1u << 16, L"GPUScene.LightmapData");
        Check(g_fakeResizeBytes == 1u << 16, "gpuscene: other buffers on that function pass through unchanged");
        SetGpuSceneKeepCapacityEnabled(false);
        resize(nullptr, &primitives, 64u << 20, gs.name[1]);
        Check(g_fakeResizeBytes == 64u << 20, "gpuscene off: the primitive buffer shrinks as the game asks");
        SetGpuSceneKeepCapacityEnabled(true);
        resize(nullptr, &primitives, 96u << 20, gs.name[1]);
        Check(g_fakeResizeBytes == 128u << 20, "gpuscene on: a grow past 1/8 of the session's largest goes straight to it");
        void* payload = &payload;
        resize(nullptr, &payload, 4096, gs.name[2]);
        resize(nullptr, &payload, 16384, gs.name[2]);
        resize(nullptr, &payload, 4096, gs.name[2]);
        Check(g_fakeResizeBytes == 16384, "gpuscene on: a small buffer (the payload's per-frame flip) is never shrunk");
        LogStreamChurnStats();
    }

    // CommandAllocatorTrim: the command allocator pools (build 19115871): the three functions, the shared queue layout, the lock-free
    // list helpers, the detours, and the stand-in's Reset through the game's own reset helper.
    if (Installed(FindAllocatorTrim(), "allocator pools found")) {
        AllocatorTrimInfoOut at{};
        Check(AllocatorTrimInfo(&at), "allocators: info available");
        Check(at.obtain == game.base + 0x2527730 && at.release == game.base + 0x252E2E0 && at.ctor == game.base + 0x24ED9D0,
              "allocators: obtain exe+0x2527730, release exe+0x252E2E0, constructor exe+0x24ED9D0");
        Check(at.pop == game.base + 0xDE3320 && at.freeLink == game.base + 0xDE12C0 && at.linkBlocks == game.base + 0x8FFF980,
              "allocators: lock-free pop exe+0xDE3320, link free exe+0xDE12C0, link blocks exe+0x8FFF980");
        Check(at.queuesData == 0x1400 && at.queuesInline == 0xB00 && at.pool == 0x240,
              "allocators: queues at +0x1400 (inline +0xB00), pool +0x240");
        Check(at.finalize == game.base + 0x24F8570 && at.allocOff == 0xB0 && at.queueTypeOff == 0x10 && at.defaultFlagOff == 0x14,
              "allocators: Finalize exe+0x24F8570 (allocator +0xB0, queue type +0x10, default-context flag +0x14)");
        Check(InstallAllocatorTrim(true), "allocators: detours placed");
        Check(at.obtain[0] == 0xE9 && at.release[0] == 0xE9 && at.ctor[0] == 0xE9 && at.finalize[0] == 0xE9,
              "allocators: the four entries jump to the hooks");
        SetImmediateAllocatorLists(16);
        SetImmediateAllocatorLists(0);
        // The game's reset helper on a wrapper holding the stand-in: [this+0x10]->Reset (vtable +0x40) must succeed.
        using ResetFn = void (*)(void* allocator);
        void* wrapper[3] = {nullptr, nullptr, AllocatorTrimTestStub()};
        reinterpret_cast<ResetFn>(game.base + 0x2505EC0)(wrapper);
        Check(true, "allocators: the game's reset helper accepts the stand-in");
        SetAllocatorTrim(true, 20);
        AllocatorTrimTick();  // no device seen: does nothing
        SetAllocatorTrim(false, 20);
        LogAllocatorTrimStats();
        {
            // The give-back decides every wrapper's place before any goes back (an obtain between the passes must not
            // make it reset or return a wrapper the engine uses), and an exception keeps the trim off (1.1.3).
            const char* why = AllocatorTrimSelfTest();
            const char* const whyPregrow = CmdPregrowSelfTest();
            Check(whyPregrow == nullptr, whyPregrow ? whyPregrow : "CmdMemPregrow: pool handling (fake pool: each wrapper back once, engine running, failures, stops, quit, cap) and gates");
            Check(why == nullptr, why ? why : "allocators: give-back safe against an obtain between the passes; off after an exception stays off; copy floor kept outside the over-budget mode");
        }
    }

    // PhysicsTreeTimeSlicing: p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce (1000 in build 19115871).
    if (Installed(FindChaosTimeSlicing(), "chaos time-slicing variable found")) {
        StreamChurnFixInfo sc{};
        StreamChurnInfo(&sc);
        Check(sc.chaosForce && sc.chaosForceOriginal == 1000, "chaos: variable found (1000)");
        SetChaosTimeSlicing(true, 40000, 1000, false);
        Check(*sc.chaosForce == 100000, "chaos on: 100000");
        SetChaosTimeSlicing(false, 40000, 1000, false);
        Check(*sc.chaosForce == 1000, "chaos off: the game's 1000");
        *sc.chaosForce = 500;  // set by someone else while off: left alone
        SetChaosTimeSlicing(false, 40000, 1000, false);
        Check(*sc.chaosForce == 500, "chaos off: a value set by someone else is left alone");
        *sc.chaosForce = 1000;
        // Slice sizes: p.IterationsPerTimeSlice (set by FBroadPhaseConfig's constructor: 0 in the mapped file) and
        // p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy (100000).
        Check(sc.chaosIterations && sc.chaosCopyBytes, "chaos: slice size variables found");
        if (sc.chaosIterations && sc.chaosCopyBytes) {
            printf("      slice variables at exe+0x%X / exe+0x%X, values %ld / %ld\n",
                   static_cast<unsigned>(reinterpret_cast<const volatile uint8_t*>(sc.chaosIterations) - game.base),
                   static_cast<unsigned>(reinterpret_cast<const volatile uint8_t*>(sc.chaosCopyBytes) - game.base),
                   *sc.chaosIterations, *sc.chaosCopyBytes);
            Check(*sc.chaosCopyBytes == 100000, "chaos: copy slice 100000 bytes in the file");
            const LONG fileIterations = *sc.chaosIterations;
            *sc.chaosIterations = 0;  // before the engine's constructor: not touched yet
            SetChaosTimeSlicing(true, 40000, 1000, false);
            Check(*sc.chaosIterations == 0 && *sc.chaosCopyBytes == 1000000, "chaos on before set-up: iterations wait, copy 1,000,000");
            *sc.chaosIterations = 4000;  // the constructor ran
            SetChaosTimeSlicing(true, 40000, 1000, false);
            Check(*sc.chaosIterations == 40000, "chaos on: iterations 40000 once set up");
            *sc.chaosIterations = 4000;  // someone put it back: set again
            SetChaosTimeSlicing(true, 40000, 1000, false);
            Check(*sc.chaosIterations == 40000, "chaos on: set again after a change");
            SetChaosTimeSlicing(true, 1000, 50, false);  // smaller than the game's: the game's values
            Check(*sc.chaosIterations == 4000 && *sc.chaosCopyBytes == 100000, "chaos: slices never below the game's");
            SetChaosTimeSlicing(true, 40000, 1000, false);
            SetChaosTimeSlicing(false, 40000, 1000, false);
            Check(*sc.chaosIterations == 4000 && *sc.chaosCopyBytes == 100000, "chaos off: the game's slice sizes back");
            *sc.chaosIterations = fileIterations;
        }
    }

    // PhysicsFlushParallel: FlushAsyncAccelerationQueue exe+0x1931660 and its game-thread copy block (build 19115871), then the
    // split against fakes (a stand-in for the game's function that calls the copy's slot and sets its timestamp).
    if (Installed(FindPhysicsFlush(), "parallel flush: FlushAsyncAccelerationQueue found")) {
        PhysicsFlushInfoOut pf{};
        PhysicsFlushInfo(&pf);
        Check(pf.flush == game.base + 0x1931660, "parallel flush: function exe+0x1931660");
        Check(pf.data == 0x380 && pf.num == 0x388 && pf.internalCopy == 0x2E0 && pf.externalCopy == 0x2E8 && pf.stamp == 0x358,
              "parallel flush: queue +0x380/+0x388, copies +0x2E0/+0x2E8, timestamp +0x358");
        Check(pf.removeSlot == 0x80 && pf.updateSlot == 0x88, "parallel flush: remove/update slots +0x80/+0x88");
        Check(pf.hasBounds == 0x188 && pf.bounds == 0x170 && pf.gtParticle == 0xE0 && pf.unique == 0x50,
              "parallel flush: particle arrays +0x188/+0x170/+0xE0/+0x50");
        Check(pf.dynamicTreeFlag == game.base + 0x8DC0494, "parallel flush: dynamic-tree flag exe+0x8DC0494");
        Check(pf.query == game.base + 0x16F11F0 && pf.sim == game.base + 0x16F1260, "parallel flush: filter helpers");
        Check(pf.step == game.base + 0x192CC60, "physics step: ComputeIntermediateSpatialAcceleration exe+0x192CC60 (calls the flush)");
        PhysicsFlushTestSetup(reinterpret_cast<void*>(&pfl::FakeOriginal), reinterpret_cast<void*>(&pfl::FakeQuery),
                              reinterpret_cast<void*>(&pfl::FakeSim), &pfl::g_dynamicFlag);
        SetPhysicsFlushParallel(true, 2);
        pfl::Reset();
        PhysicsFlushTestCall(pfl::g_evolution);
        Check(pfl::g_originalCalls == 1 && pfl::g_externalSeenByOriginal != &pfl::g_external,
              "parallel flush split: the game's function ran once, its game-thread copy calls went to the stand-in");
        Check(*reinterpret_cast<void**>(pfl::g_evolution + 0x2E8) == &pfl::g_external, "parallel flush: copy pointer restored");
        Check(*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&pfl::g_external) + 0xC) == 77,
              "parallel flush: the copy got the step's timestamp");
        const bool order = pfl::g_calls.size() == 3 && pfl::g_calls[0].update && pfl::g_calls[1].update && !pfl::g_calls[2].update;
        Check(order, "parallel flush: add, update, remove in queue order; the dynamic-tree bucket skipped");
        if (order) {
            const pfl::Call& a = pfl::g_calls[0];
            const pfl::Call& u = pfl::g_calls[1];
            const pfl::Call& r = pfl::g_calls[2];
            bool filters = true;
            for (int i = 0; i < 16; ++i)
                filters = filters && a.h.query[i] == 0xA0 && a.h.sim[i] == 0xB0 && u.h.query[i] == 0xA1 && u.h.sim[i] == 0xB1;
            Check(a.h.gt == pfl::g_gt[0] && a.h.particle == &pfl::g_particles[0] && a.h.unique == 101 && a.h.canPreFilter == 1 &&
                      a.bounds == &pfl::g_bounds[0][0] && a.hasBounds == 1 && a.index == 0,
                  "parallel flush: add with the particle's handle, bounds, bounds flag and index");
            Check(u.h.gt == pfl::g_gt[1] && u.h.unique == 102 && u.bounds == &pfl::g_bounds[1][0] && u.hasBounds == 0 &&
                      u.index == 0x10,
                  "parallel flush: update with the second particle's data (no bounds)");
            Check(filters, "parallel flush: query and sim filters from the game's helpers");
            Check(r.handleOrOp == pfl::g_ops + 3 * 0x48 && r.index == 0, "parallel flush: remove with the op as the handle");
            Check(a.tid != pfl::g_originalTid, "parallel flush: the copy's updates ran on the helper thread");
        }
        pfl::g_dynamicFlag = 0;  // dynamic trees off: every op goes to the copy
        pfl::Reset();
        PhysicsFlushTestCall(pfl::g_evolution);
        Check(pfl::g_calls.size() == 4, "parallel flush: dynamic trees off -> the bucket-1 op too");
        pfl::g_dynamicFlag = 1;
        SetPhysicsFlushParallel(true, 5);  // 4 ops < 5: the game's function alone
        pfl::Reset();
        PhysicsFlushTestCall(pfl::g_evolution);
        Check(pfl::g_originalCalls == 1 && pfl::g_externalSeenByOriginal == &pfl::g_external && pfl::g_calls.size() == 1 &&
                  pfl::g_calls[0].handleOrOp == nullptr,
              "parallel flush: small queue -> the game's function with the real copy");
        SetPhysicsFlushParallel(false, 2);
        pfl::Reset();
        PhysicsFlushTestCall(pfl::g_evolution);
        Check(pfl::g_originalCalls == 1 && pfl::g_externalSeenByOriginal == &pfl::g_external, "parallel flush off: the game's function");
        SetPhysicsFlushParallel(false, 2);
        // PhysicsTreeTimeSlicing's adaptive copy slice through the step hook (fake step): the game's value while quiet, ours from the
        // 8th step of a rebuild cycle or with 2048 changes waiting, the game's again after a swap; off -> the game's.
        StreamChurnFixInfo sc2{};
        StreamChurnInfo(&sc2);
        if (sc2.chaosCopyBytes) {
            pfl::g_copyVarForFake = sc2.chaosCopyBytes;
            PhysicsStepTestSetup(reinterpret_cast<void*>(&pfl::FakeCisa));
            SetPhysicsFlushParallel(true, 1024);
            SetChaosTimeSlicing(true, 40000, 1000, true);
            pfl::Reset();
            *reinterpret_cast<int32_t*>(pfl::g_evolution + 0x388) = 300;  // quiet: a few hundred changes
            PhysicsFlushTestCall(pfl::g_evolution);  // a swap: cycle starts
            *reinterpret_cast<int32_t*>(pfl::g_evolution + 0x388) = 300;
            LONG seen[10];
            for (LONG& s : seen) { PhysicsStepTestCall(pfl::g_evolution, 0); s = pfl::g_copySeen; }
            Check(seen[0] == 100000 && seen[7] == 100000 && seen[8] == 1000000 && seen[9] == 1000000,
                  "adaptive copy: the game's 100000 for 8 steps, 1,000,000 from the 9th");
            PhysicsFlushTestCall(pfl::g_evolution);  // swap
            *reinterpret_cast<int32_t*>(pfl::g_evolution + 0x388) = 300;
            PhysicsStepTestCall(pfl::g_evolution, 0);
            Check(pfl::g_copySeen == 100000, "adaptive copy: after a swap the game's value again");
            *reinterpret_cast<int32_t*>(pfl::g_evolution + 0x388) = 3000;  // a streamed area waits
            PhysicsStepTestCall(pfl::g_evolution, 0);
            Check(pfl::g_copySeen == 1000000, "adaptive copy: 3000 changes waiting -> 1,000,000 at once");
            Check(pfl::g_cisaCalls == 12, "adaptive copy: every step ran the game's function");
            SetChaosTimeSlicing(false, 40000, 1000, true);
            Check(*sc2.chaosCopyBytes == 100000, "adaptive copy off: the game's value");
            PhysicsStepTestCall(pfl::g_evolution, 0);
            Check(pfl::g_copySeen == 100000, "adaptive copy off: the step leaves it alone");
            pfl::g_copyVarForFake = nullptr;
        }
    }

    // InstanceClearReserve: ClearInstances' call to FInstanceUpdateCmdBuffer::Reset (exe+0x31FA7CE, build 19115871), then the hook against
    // fakes: the game's Reset runs, then the list is reserved for min(render instances, instances) commands.
    if (Installed(FindInstanceClearReserve(), "instance clear: ClearInstances' reset found")) {
        InstanceClearInfoOut ic{};
        InstanceClearInfo(&ic);
        Check(ic.site == game.base + 0x31FA7C7 && ic.reset == game.base + 0x325A7B0 && ic.resize == game.base + 0xDE6200,
              "instance clear: site exe+0x31FA7C7, Reset exe+0x325A7B0, allocation exe+0xDE6200");
        Check(ic.cmdOff == 0x758 && ic.renderOff == 0x6D0 && ic.bufferOff == 0xC0 && ic.countOff == 0x40 && ic.numOff == 0x668,
              "instance clear: offsets +0x758, +0x6D0 -> +0xC0 -> +0x40, +0x668");
        Check(ic.elemSize == 0x150 && ic.alignment == 0x10, "instance clear: 336-byte commands, 16-byte alignment");
        InstanceClearTestSetup(reinterpret_cast<void*>(&icr::FakeReset), reinterpret_cast<void*>(&icr::FakeResize));
        SetInstanceClearReserve(true);
        uint8_t* const cmd = icr::g_component + 0x758;
        icr::Setup(40000, 30000, true);
        InstanceClearTestCall(cmd);
        Check(icr::g_resets == 1 && icr::g_resizes == 1 && icr::g_prev == 0 && icr::g_newMax == 30000 && icr::g_elem == 0x150 &&
                  icr::g_align == 0x10 && *reinterpret_cast<int32_t*>(cmd + 0xC) == 30000,
              "instance clear: Reset, then room for min(40000, 30000) = 30000 commands");
        icr::Setup(20000, 50000, true);
        InstanceClearTestCall(cmd);
        Check(icr::g_newMax == 20000, "instance clear: the render instance count when smaller");
        icr::Setup(100, 100, true);
        InstanceClearTestCall(cmd);
        Check(icr::g_resets == 1 && icr::g_resizes == 0, "instance clear: small clears left to the growth policy");
        icr::Setup(40000, 30000, false);
        InstanceClearTestCall(cmd);
        Check(icr::g_resets == 1 && icr::g_resizes == 0, "instance clear: no render data -> no loop, nothing reserved");
        SetInstanceClearReserve(false);
        icr::Setup(40000, 30000, true);
        InstanceClearTestCall(cmd);
        Check(icr::g_resets == 1 && icr::g_resizes == 0, "instance clear off: the game's Reset only");
    }

    clr::ClothRegSelfTest(game);  // ClothRegisterLite (lab: also ClothRegisterSkip, ClothRegisterVerify)
    cdf::ClothDeferSelfTest(game);  // ClothBuildDefer (its hooks run inside ClothRegisterLite's, as in the game)

    // ParallelStaticBodies: InitStaticBodies' call of CreateShapesAndActors (build 19115871), then the split against a fake.
    if (Installed(FindParallelStaticBodies(), "static bodies: InitStaticBodies' call found")) {
        using namespace sbt;
        StaticBodiesInfoOut si{};
        ParallelStaticBodiesInfo(&si);
        Check(si.call == game.base + 0x34ECCBC && si.create == game.base + 0x34DAE60 && si.handleOff == 0x158 && si.staticOff == 0x31,
              "static bodies: call exe+0x34ECCBC -> CreateShapesAndActors exe+0x34DAE60, actor handle +0x158, static flag +0x31");
        Check(InstallParallelStaticBodies(true, 3, 64), "static bodies: installed on the mapped exe (3 helper threads)");
        Check(RipTarget(si.call, 1, 5)[0] == 0xFF, "static bodies: the call goes through the hook's jump");
        ParallelStaticBodiesTestSetup(reinterpret_cast<void*>(&FakeCreate), 0x158, 0x31, 3, 64);
        using HookFn = bool (*)(void*);
        const auto hook = reinterpret_cast<HookFn>(si.hook);
        SetParallelStaticBodies(true, 64);
        Run(2000);
        bool ok = hook(g_helper);
        bool all = true;
        std::vector<DWORD> tids;
        for (int i = 0; i < 2000; ++i) {
            all = all && g_seq[i] >= 0 && *reinterpret_cast<void**>(g_bodies[i] + 0x158) == g_bodies[i];
            if (std::find(tids.begin(), tids.end(), g_tid[i]) == tids.end()) tids.push_back(g_tid[i]);
        }
        Check(ok && all && g_order.load() == 2000 && g_bad.load() == 0, "static bodies: every body created once, with its own transform");
        Check(g_seq[1999] == 0 && g_tid[1999] == GetCurrentThreadId(), "static bodies: the game's first body (the last) alone, first, on the calling thread");
        Check(tids.size() >= 2 && g_maxView < 2000 && g_b.num == 2000 && g_t.num == 2000,
              "static bodies: the rest in slices on several threads; the arrays left as they were");
        printf("      (%zu threads, %d calls, largest slice %d)\n", tids.size(), g_sbCalls.load(), g_maxView);
        Run(2000, true, 700);
        hook(g_helper);
        Check(g_sbCalls.load() == 1 && g_maxView == 2000, "static bodies: a body that already has an actor: the game's call as before");
        Run(40);
        hook(g_helper);
        Check(g_sbCalls.load() == 1 && g_maxView == 40, "static bodies: fewer than 64 bodies: the game's call as before");
        Run(500, false);
        hook(g_helper);
        Check(g_sbCalls.load() == 1 && g_maxView == 500, "static bodies: not the static helper: the game's call as before");
        Run(500, true, -1, 499);
        hook(g_helper);
        Check(g_sbCalls.load() == 1, "static bodies: transforms do not match the bodies: the game's call as before");
        SetParallelStaticBodies(false, 64);
        Run(500);
        hook(g_helper);
        Check(g_sbCalls.load() == 1 && g_maxView == 500, "static bodies off: the game's call as before");
        SetParallelStaticBodies(true, 64);
        Run(65);
        hook(g_helper);
        Check(g_order.load() == 65 && g_bad.load() == 0 && g_seq[64] == 0, "static bodies: 65 bodies (just over the threshold) all created");
        // Stress: back-to-back jobs of varying size with the helper threads yielding at random points (the race where a
        // late worker carried a slice into the next job): every body exactly once per job, nothing running after return.
        ParallelStaticBodiesTestJitter(true);
        g_work = 50;
        uint32_t rnd = 12345;
        int badJobs = 0, lateRuns = 0;
        for (int j = 0; j < 4000; ++j) {
            rnd = rnd * 1664525u + 1013904223u;
            const int n = 64 + static_cast<int>(rnd % 900);
            Run(n);
            hook(g_helper);
            bool once = g_order.load() == n && g_bad.load() == 0 && g_inFake.load() == 0;
            for (int i = 0; i < n && once; ++i) once = g_seq[i] >= 0 && *reinterpret_cast<void**>(g_bodies[i] + 0x158) == g_bodies[i];
            if (!once) ++badJobs;
            const int after = g_order.load();
            if ((j & 63) == 0) { SwitchToThread(); Sleep(0); }
            if (g_order.load() != after || g_inFake.load() != 0) ++lateRuns;
        }
        ParallelStaticBodiesTestJitter(false);
        g_work = 20000;
        printf("      (stress: 4000 jobs, %d with a body missed or doubled, %d with work after the hook returned)\n", badJobs, lateRuns);
        Check(badJobs == 0 && lateRuns == 0, "static bodies: 4000 back-to-back jobs with yielding helpers: every body once, nothing after return");
        // Late workers: they sleep 300 ms after each wake. The job must not wait for them (the calling thread makes every
        // slice itself), and when they come they must find nothing left to claim.
        ParallelStaticBodiesTestHold(300);
        g_work = 50;
        Run(2000);
        const ULONGLONG h0 = GetTickCount64();
        ok = hook(g_helper);
        const ULONGLONG heldMs = GetTickCount64() - h0;
        bool here = g_order.load() == 2000 && g_bad.load() == 0;
        for (int i = 0; i < 2000 && here; ++i) here = g_seq[i] >= 0 && g_tid[i] == GetCurrentThreadId();
        Sleep(400);
        const bool nothingLate = g_order.load() == 2000 && g_inFake.load() == 0;
        ParallelStaticBodiesTestHold(0);
        Sleep(50);
        Run(2000);
        const bool okAfter = hook(g_helper) && g_order.load() == 2000 && g_bad.load() == 0;
        g_work = 20000;
        printf("      (late workers: the job took %llu ms)\n", static_cast<unsigned long long>(heldMs));
        Check(ok && here && heldMs < 150 && nothingLate && okAfter,
              "static bodies: workers that wake 300 ms late: the job ends without them, they find nothing, the next job is whole");
        SetParallelStaticBodies(false, 64);
    }

    // PipelinedSceneAdd: InitBodies' call of AddActorsToScene and its call of AddActorToSolver (build 19115871), then the hook
    // against fakes: registrations on the calling thread, inserts on the helper, both in array order, the lock held.
    if (Installed(FindPipelinedSceneAdd(), "scene add: AddActorsToScene and AddActorToSolver found")) {
        uint8_t* saSites[5] = {};
        uint32_t offs[3] = {};
        PipelinedSceneAddInfo(saSites, offs);
        Check(saSites[0] - game.base == 0x34D02A1 && saSites[1] - game.base == 0x1A9C8A0 && saSites[2] - game.base == 0x1A9C974 &&
                  saSites[3] - game.base == 0x1A2EEB0 && saSites[4] - game.base == 0x9142320 && offs[0] == 0x38 && offs[1] == 0x200 &&
                  offs[2] == 0x28,
              "scene add: call exe+0x34D02A1 -> exe+0x1A9C8A0, inner call exe+0x1A9C974 -> exe+0x1A2EEB0, flag exe+0x9142320, +0x38/+0x200/+0x28");
        InitializeSRWLock(&pst::g_lock.srw);
        pst::g_lock.tls = TlsAlloc();
        *reinterpret_cast<uint8_t**>(pst::g_scene + 0x38) = pst::g_solver;
        *reinterpret_cast<uint8_t**>(pst::g_scene + 0x28) = pst::g_solver;  // any non-null query structure
        *reinterpret_cast<pst::FakeLock**>(pst::g_solver + 0x200) = &pst::g_lock;
        pst::g_stub = reinterpret_cast<pst::StubFn>(PipelinedSceneAddTestSetup(
            reinterpret_cast<void*>(&pst::FakeAdd), reinterpret_cast<void*>(&pst::FakeToSolver), &pst::g_flag, 0x38, 0x200, 0x28, 64));
        SetPipelinedSceneAdd(true, 64);
        // A reader that tries the lock while a job's inserts run: it must never get it.
        pst::g_probeRun = true;
        HANDLE probe = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            while (pst::g_probeRun.load()) {
                if (pst::g_inJob.load() && TryAcquireSRWLockShared(&pst::g_lock.srw)) {
                    if (pst::g_inJob.load()) pst::g_probeHits.fetch_add(1);
                    ReleaseSRWLockShared(&pst::g_lock.srw);
                }
                YieldProcessor();
            }
            return 0;
        }, nullptr, 0, nullptr);
        Check(pst::Job(1000, true), "scene add: 1000 bodies registered here in order, inserted by the helper in order, each after its registration");
        Check(pst::Job(10, false), "scene add: fewer than 64 bodies: the game's call on this thread as before");
        pst::g_flag = 0;
        Check(pst::Job(500, false), "scene add: the game without the scene lock: as before");
        pst::g_flag = 1;
        SetPipelinedSceneAdd(false, 64);
        Check(pst::Job(500, false), "scene add off: as before");
        SetPipelinedSceneAdd(true, 64);
        pst::g_probeHits = 0;  // from here on every job holds the lock (the flag-0 job above did not)
        int badJobs = 0;
        uint32_t rnd = 12345;
        for (int j = 0; j < 400; ++j) {
            rnd = rnd * 1664525u + 1013904223u;
            if (!pst::Job(64 + static_cast<int>(rnd % 900), true)) ++badJobs;
        }
        pst::g_probeRun = false;
        if (probe) { WaitForSingleObject(probe, INFINITE); CloseHandle(probe); }
        printf("      (stress: 400 jobs, %d bad; a reader got the lock during a job %d times)\n", badJobs, pst::g_probeHits.load());
        Check(badJobs == 0 && pst::g_probeHits.load() == 0, "scene add: 400 jobs: every body in order, no reader inside a job");
        SetPipelinedSceneAdd(false, 64);
        PipelinedBodiesSelfTest(game);  // PipelinedBodyCreation (needs 28b's fakes set up above)
    }

    // DirtyTreeQuickInsert: greedy dirty-tree sibling choice. Sites on the mapped exe, then a fake tree (TAABBTree layout of this build):
    // root 0 -> internal 1 (leaves 3, 4) and internal 2 (leaves 5, 6); four leaves around x = 0, 100, 1000, 1100.
    {
        Check(InstallDirtyTreeQuickInsert(true), "dirty-tree quick insert: InsertLeaf, its two dirty-tree calls, FindBestSibling and the leaf capacity found");
        alignas(16) static uint8_t tree[0x100], nodes[7][0x70], leaves[4][0x48];
        memset(tree, 0, sizeof tree); memset(nodes, 0, sizeof nodes); memset(leaves, 0, sizeof leaves);
        *reinterpret_cast<uint8_t**>(tree + 0x18) = &nodes[0][0];
        *reinterpret_cast<int32_t*>(tree + 0x20) = 7;
        *reinterpret_cast<uint8_t**>(tree + 0x28) = &leaves[0][0];
        *reinterpret_cast<int32_t*>(tree + 0x54) = 0;
        auto setBox = [](uint8_t* node, int c, double x0, double x1) {
            const double b[6] = {x0, 0, 0, x1, 10, 10};
            memcpy(node + c * 0x30, b, sizeof b);
        };
        auto setKids = [](uint8_t* node, int a, int b) { memcpy(node + 0x60, &a, 4); memcpy(node + 0x64, &b, 4); };
        const double lx[4] = {0, 100, 1000, 1100};
        for (int i = 0; i < 4; ++i) {
            uint8_t* n = nodes[3 + i];
            setBox(n, 0, lx[i], lx[i] + 10);
            setKids(n, i, -1);
            n[0x6C] = 1;
            *reinterpret_cast<int32_t*>(leaves[i] + 0x38) = (i == 1) ? 8 : 3;  // leaf 1 full at capacity 8
        }
        setBox(nodes[1], 0, 0, 10); setBox(nodes[1], 1, 100, 110); setKids(nodes[1], 3, 4);
        setBox(nodes[2], 0, 1000, 1010); setBox(nodes[2], 1, 1100, 1110); setKids(nodes[2], 5, 6);
        setBox(nodes[0], 0, 0, 110); setBox(nodes[0], 1, 1000, 1110); setKids(nodes[0], 1, 2);
        auto pick = [&](double x0, double x1, bool& add) {
            const double b[6] = {x0, 1, 1, x1, 9, 9};
            return QuickFindBestSibling(tree, b, &add, 8);
        };
        bool add = false;
        int r = pick(2, 8, add);
        Check(r == 3 && add, "dirty-tree quick insert: a box inside leaf 0 goes into that leaf (room left)");
        r = pick(102, 108, add);
        Check(r == 4 && !add, "dirty-tree quick insert: next to the full leaf 1: paired with it, not added");
        r = pick(1102, 1108, add);
        Check(r == 6 && add, "dirty-tree quick insert: the other subtree is found");
        r = pick(-50000, 50000, add);
        Check(r == 0 && !add, "dirty-tree quick insert: a huge box pairs with the root");
        bool allValid = true;
        uint32_t rnd = 7;
        for (int k = 0; k < 2000; ++k) {
            rnd = rnd * 1664525u + 1013904223u;
            const double x = static_cast<double>(rnd % 3000) - 500.0, w = static_cast<double>((rnd >> 12) % 400);
            r = pick(x, x + w, add);
            allValid = allValid && r >= 0 && r < 7 && (!add || (nodes[r][0x6C] & 1));
        }
        Check(allValid, "dirty-tree quick insert: 2000 random boxes: always a node of the tree, added only to a leaf with room");
        SetDirtyTreeQuickInsert(false);
    }

    // PhysicsPushParallel (before BulkIndexInsert, whose lab timer detours ForEachProxy's entry): sites, then the push-loop model.
    PushSelfTest(game);

    // BulkIndexInsert: its sites on the mapped exe, then the tree model (selftest_bulkinsert.h).
    Check(InstallBulkIndexInsert(true), "BulkIndexInsert: the physics step's swap, FlushInternalAccelerationQueue and its switch found, detour placed");
    BulkInsertSelfTest();
    ShrinkSelfTest(game);  // PhysicsQueueShrinkOnce (rides on BulkIndexInsert's catch-up detour)
    svb::SaveBytesSelfTest(game);   // SaveBytesBulk / SaveThumbnailAsync
    srid::SaveRefIdSelfTest(game);  // SaveRefIdIndex
    rsu::RigSetupSelfTest(game);    // RigVmHashMemo / RigLogicMapCache
    rtm::RigTmplSelfTest(game);     // RigImportTemplate
    fpm::FarPawnSelfTest(game);     // FarPawnMoveHalfRate / FarHorseMove
    cpl::CreatePreloadSelfTest(game);  // CreatePreload
    cop::CellObjectPreloadSelfTest(game);  // CellObjectPreload
    cah::CreateAheadSelfTest(game);    // CreateAhead
    ipl::ImpactPreloadSelfTest(game);  // ImpactPreload
    nbl::NpcBodiesSelfTest(game);   // NpcBodiesOneLock
    rts::RtSceneSelfTest(game);     // RtSceneBufferKeep
    pkp::PoolKeepSelfTest(game);    // GpuPoolKeepMB
    BufferFloorRun(game);           // InstanceBufferFloor / lab SceneBufferFloorMiB (after GpuSceneKeepCapacity's and RtSceneBufferKeep's tests)
    nsb::NaniteSelfTest(game);      // NaniteInitialRootPages (lab: NaniteStreamProbe / Nanite*MinMB)
    wtr::WaterInfoSelfTest(game);   // WaterCaptureFsrSkip / NoRhiFlush / NoOcclusionWait / FastMeshScan (lab: WaterInfoProbe)
    WaterInfoSkipRun(game);         // WaterInfoSkip (selftest_waterinfo_skip.h): after the install above (its game-thread detour)
    WaterSpreadRun(game);           // WaterInfoSpread (selftest_waterspread.h): discovery only (nothing installed), logic with fakes
    MapSpreadRun(game);             // MapPopupSpread (selftest_mapspread.h): discovery only (nothing installed), logic with fakes
    GpuShotRun(game);               // GpuMenuShot (selftest_gpushot.h): discovery only, the shader's arithmetic, the shader on this GPU
    ThumbLeftoverRun(game);         // SaveThumbLeftoverFix (selftest_thumbleft.h)
    fpc::FpsCapSelfTest(game);      // FrameRateLimitAtStart
    nlf::NavLinkSelfTest(game);     // NavLinkFast
    objrep::ObjectReplaceSelfTest(game);  // ObjectReplaceCrashGuard
    tw::TaskWaitSelfTest(game);           // TaskWaitDeadlockGuard
    dbt::DestroyBatchSelfTest(game);      // PhysicsDestroyBatch

    // ParallelStaticBodies and PhysicsFlushParallel's helpers: FMallocBinned2::SetupTLSCachesOnCurrentThread (exe+0xE0D650, slot exe+0x900B928, build 19115871),
    // then the live switch against a fake set-up on a thread of its own.
    if (Installed(FindHelperThreadCaches(), "helper caches: the allocator's thread-cache set-up found")) {
        HelperCachesInfoOut hi{};
        HelperThreadCachesInfo(&hi);
        Check(hi.setup - game.base == 0xE0D650 && reinterpret_cast<uint8_t*>(hi.slot) - game.base == 0x900B928 && hi.refs >= 4,
              "helper caches: set-up exe+0xE0D650, slot exe+0x900B928, the slot read nearby");
        htc::g_slot = TlsAlloc();
        uint32_t fakeSlot = htc::g_slot;
        HelperThreadCachesTestSetup(reinterpret_cast<void*>(&htc::FakeSetup), &fakeSlot);
        struct Step { bool on; void* expect; int setups; };
        static const Step steps[] = {{false, nullptr, 0}, {true, &htc::g_marker, 1}, {true, &htc::g_marker, 1},
                                     {false, nullptr, 1}, {true, &htc::g_marker, 1}, {false, nullptr, 1}};
        bool ok = true;
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            bool& good = *static_cast<bool*>(p);
            for (const Step& st : steps) {
                SetHelperThreadCaches(st.on);
                HelperThreadCache();
                good = good && TlsGetValue(htc::g_slot) == st.expect && htc::g_setups.load() == st.setups;
            }
            return 0;
        }, &ok, 0, nullptr);
        if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
        Check(t && ok, "helper caches: set up once on first use, taken away and given back by the switch");
        fakeSlot = 0;
        SetHelperThreadCaches(true);
        const int before = htc::g_setups.load();
        HelperThreadCache();
        Check(htc::g_setups.load() == before, "helper caches: nothing done before the game has a cache slot");
        SetHelperThreadCaches(false);
        HelperThreadCachesTestSetup(nullptr, nullptr);
        TlsFree(htc::g_slot);
    }

    // FrameGenAsyncCompute: the registration of r.FidelityFX.FI.AllowAsyncWorkloads (exe+0xAA06C1, build 19115871; its value pointer at
    // exe+0x92BB250). Nothing runs here: in the mapped exe the registration never ran, so the pointer is still empty.
    if (Installed(FindFrameGenAsyncCompute(), "frame generation async: AllowAsyncWorkloads registration found"))
        Check(FrameGenAsyncComputeValueForTest() == nullptr, "frame generation async: value pointer empty before registration");

    // GpuSceneParallelUpload: the registration of r.GPUScene.ParallelUpdate (exe+0x940D3B, build 19115871; value pointer at exe+0x91648C8).
    // In the mapped exe the registration never ran, so the pointer is empty; then the switch on a fake value pair: on sets
    // the stock 0 to 1 (both copies), off puts back only the fix's own 1, a value set by someone else is left alone.
    if (Installed(FindGpuSceneParallelUpload(), "GPU scene parallel upload: r.GPUScene.ParallelUpdate registration found")) {
        int32_t* volatile* const slot = GpuSceneParallelUploadSlotForTest();
        Check(*slot == nullptr, "GPU scene parallel upload: value pointer empty before registration");
        SetGpuSceneParallelUpload(true);  // nothing registered: nothing written, no crash
        Check(*slot == nullptr, "GPU scene parallel upload: unregistered value left alone");
    }
    {
        int32_t pair[2] = {0, 0};
        int32_t* volatile fakeSlot = nullptr;
        GpuSceneParallelUploadTestSetup(&fakeSlot);
        SetGpuSceneParallelUpload(true);
        Check(pair[0] == 0, "GPU scene parallel upload: nothing written before registration");
        fakeSlot = pair;
        SetGpuSceneParallelUpload(false);
        Check(pair[0] == 0 && pair[1] == 0, "GPU scene parallel upload: off leaves the stock 0");
        SetGpuSceneParallelUpload(true);
        Check(pair[0] == 1 && pair[1] == 1, "GPU scene parallel upload: on sets both copies to 1");
        SetGpuSceneParallelUpload(true);
        Check(pair[0] == 1 && pair[1] == 1, "GPU scene parallel upload: on twice stays 1");
        SetGpuSceneParallelUpload(false);
        Check(pair[0] == 0 && pair[1] == 0, "GPU scene parallel upload: off puts back 0");
        pair[0] = pair[1] = 2;  // the player's own value
        SetGpuSceneParallelUpload(true);
        Check(pair[0] == 2 && pair[1] == 2, "GPU scene parallel upload: a player's value left alone (on)");
        SetGpuSceneParallelUpload(false);
        Check(pair[0] == 2 && pair[1] == 2, "GPU scene parallel upload: a player's value left alone (off)");
        GpuSceneParallelUploadTestSetup(&fakeSlot);
        pair[0] = pair[1] = 0;
        SetGpuSceneParallelUpload(true);
        pair[0] = pair[1] = 0;  // the player sets 0 in the console after the fix
        SetGpuSceneParallelUpload(true);
        Check(pair[0] == 0 && pair[1] == 0, "GPU scene parallel upload: a later change by the player is kept");
        SetGpuSceneParallelUpload(true);
        Check(pair[0] == 0 && pair[1] == 0, "GPU scene parallel upload: ... and stays kept");
        GpuSceneParallelUploadTestSetup(nullptr);
    }

    // FadeVtFlushSkip : the fade-in's r.VT.Flush (exe+0x48774E0, build 19115871), its timer delegate's
    // Execute / ExecuteIfSafe thunks (exe+0x4881630 / exe+0x48817D0) and their .rdata slots (exe+0x7D7BD00 / +0x7D7BD08);
    // then the stubs on fake functions: off passes through to the game's thunk, on skips it (and ExecuteIfSafe returns true).
    {
        FadeVtFlushInfo fi{};
        if (Installed(FindFadeVtFlush(&fi), "fade VT flush: r.VT.Flush function, timer delegate thunks and slots found")) {
            auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
            Check(rva(fi.flush) == 0x48774E0 && rva(fi.exec) == 0x4881630 && rva(fi.execIfSafe) == 0x48817D0,
                  "fade VT flush: flush at exe+0x48774E0, thunks at exe+0x4881630 / exe+0x48817D0");
            Check(rva(fi.execSlot) == 0x7D7BD00 && rva(fi.execIfSafeSlot) == 0x7D7BD08,
                  "fade VT flush: delegate slots at exe+0x7D7BD00 / +0x7D7BD08");
        }
        static int calls = 0;
        struct Fake {
            static void Exec(void*) { ++calls; }
            static bool ExecIfSafe(void*) { ++calls; return true; }
        };
        void* slots[2] = {nullptr, nullptr};
        FadeVtFlushTestSetup(&slots[0], &slots[1], reinterpret_cast<void*>(&Fake::Exec), reinterpret_cast<void*>(&Fake::ExecIfSafe), true);
        FadeVtFlushTestCall(nullptr, false);
        FadeVtFlushTestCall(nullptr, true);
        Check(calls == 2 && FadeVtFlushSkippedForTest() == 0, "fade VT flush: off passes both calls through to the game");
        SetFadeVtFlushSkip(true);
        FadeVtFlushTestCall(nullptr, false);
        FadeVtFlushTestCall(nullptr, true);
        Check(calls == 2 && FadeVtFlushSkippedForTest() == 2, "fade VT flush: on skips both calls");
        SetFadeVtFlushSkip(false);
        FadeVtFlushTestCall(nullptr, false);
        Check(calls == 3, "fade VT flush: off again passes through");
        FadeVtFlushTestSetup(nullptr, nullptr, nullptr, nullptr, false);
    }

    // VtStreamingUploads : the registration of r.VT.MaxUploadsPerFrame.Streaming (exe+0xA69B5B, build 19115871).
    // Empty value pointer in the mapped exe; then the key on a fake value pair: N written over the game's 4 (both copies),
    // written again when the game's settings put a value back, 0 puts back the game's value only if the value is still N.
    if (Installed(FindVtStreamingUploads(), "VT streaming uploads: r.VT.MaxUploadsPerFrame.Streaming registration found")) {
        int32_t* volatile* const slot = VtStreamingUploadsSlotForTest();
        Check(*slot == nullptr, "VT streaming uploads: value pointer empty before registration");
        SetVtStreamingUploads(64);  // nothing registered: nothing written, no crash
        Check(*slot == nullptr, "VT streaming uploads: unregistered value left alone");
    }
    {
        int32_t pair[2] = {4, 4};
        int32_t* volatile fakeSlot = nullptr;
        VtStreamingUploadsTestSetup(&fakeSlot);
        SetVtStreamingUploads(64);
        Check(pair[0] == 4, "VT streaming uploads: nothing written before registration");
        fakeSlot = pair;
        SetVtStreamingUploads(0);
        Check(pair[0] == 4 && pair[1] == 4, "VT streaming uploads: 0 leaves the game's 4");
        SetVtStreamingUploads(64);
        Check(pair[0] == 64 && pair[1] == 64, "VT streaming uploads: 64 sets both copies");
        SetVtStreamingUploads(64);
        Check(pair[0] == 64 && pair[1] == 64, "VT streaming uploads: 64 twice stays 64");
        pair[0] = pair[1] = 4;  // the game's graphics settings applied again
        SetVtStreamingUploads(64);
        Check(pair[0] == 64 && pair[1] == 64, "VT streaming uploads: set again after the game's settings put 4 back");
        SetVtStreamingUploads(32);
        Check(pair[0] == 32 && pair[1] == 32, "VT streaming uploads: a changed key value applied live");
        SetVtStreamingUploads(0);
        Check(pair[0] == 4 && pair[1] == 4, "VT streaming uploads: 0 puts back the game's 4");
        SetVtStreamingUploads(0);
        Check(pair[0] == 4 && pair[1] == 4, "VT streaming uploads: 0 twice leaves it");
        SetVtStreamingUploads(64);
        pair[0] = pair[1] = 16;  // changed after the fix by something else, then the key goes off
        SetVtStreamingUploads(0);
        Check(pair[0] == 16 && pair[1] == 16, "VT streaming uploads: 0 leaves a value that is no longer the fix's");
        SetVtStreamingUploads(100000);
        Check(pair[0] == 1024 && pair[1] == 1024, "VT streaming uploads: key capped at 1024");
        VtStreamingUploadsTestSetup(nullptr);
    }

    // FrameGenCrashGuard: AMD FidelityFX's command list pool, the game's own amd_fidelityfx_dx12.dll loaded here, on a WARP device. The
    // guard installs from the DLL load notification; with the queue held by a fence wait and all 8 lists of a type taken,
    // the library's function would read through a null slot (the 10-08 / 10-13 crashes); the guard waits for the queue.
    {
        std::wstring lib = exe;
        lib = lib.substr(0, lib.find_last_of(L"\\/") + 1) + L"amd_fidelityfx_dx12.dll";
        Check(!WatchFrameGenLibrary(false), "frame generation guard: waits for the library to load");
        const HMODULE ffx = LoadLibraryW(lib.c_str());
        const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        const auto createDevice =
            d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
        const auto createFactory =
            dxgi ? reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
        IDXGIFactory4* factory = nullptr;
        IDXGIAdapter* warp = nullptr;
        ID3D12Device* device = nullptr;
        if (createFactory && createDevice && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) && factory &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp)
            createDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        const FsrLibraryInfo fsr = DescribeFsrLibrary(ffx);
        if (ffx)
            printf("      (amd_fidelityfx_dx12.dll next to the exe: %s, %u.%u.%u.%u)\n", fsr.name, fsr.version[0], fsr.version[1],
                   fsr.version[2], fsr.version[3]);
        // the two libraries the guard knows (the same pool, compiled differently): function, calls, the unchecked read
        struct GuardSites { const char* what; uint32_t acquire; std::vector<uint32_t> calls; uint32_t fault; };
        const GuardSites g310{"the game's FSR 3.1.0", 0x1D7C0, {0x1C3FD, 0x1CAE9, 0x1D4D2, 0x1EAED, 0x1ED66, 0x20264}, 0x1D9BF};
        const GuardSites g314{"AMD's FSR 3.1.4", 0x1FF70, {0x1E8CD, 0x1EF78, 0x1F21F, 0x1FC62, 0x21341, 0x215C6, 0x22D6D}, 0x2016A};
        const GuardSites* gsites = fsr.kind == FsrLibrary::Game310 ? &g310 : fsr.kind == FsrLibrary::Sdk114 ? &g314 : nullptr;
        if (fsr.version[0] == 1 && fsr.version[2] == 1)  // a 1.0.1.x build: AMD's 3.1.4 (1.0.1.41314)
            Check(fsr.kind == FsrLibrary::Sdk114 && fsr.pacingKey && fsr.providerQuery,
                  "frame generation library: AMD's FSR 3.1.4 DLL recognised (pacing tuning and provider query available)");
        if (fsr.kind == FsrLibrary::Game310)
            Check(fsr.version[0] == 1 && fsr.version[2] == 0 && fsr.version[3] == 36604 && !fsr.pacingKey && !fsr.providerQuery,
                  "frame generation library: the game's own FSR 3.1.0 recognised (no pacing tuning, no provider query)");
        if (ffx && !gsites) {
            // the SDK 2.x loader (32 lists per type) or an unknown build: the guard must stay out
            FrameGenGuardInfo fi{};
            Check(!FrameGenCrashGuardInfo(&fi) || !fi.acquire, "frame generation guard: not installed with another FSR library");
            printf("SKIP  frame generation guard's pool checks: they need FSR 3.1.0 or 3.1.4 next to the exe\n");
        } else if (!ffx || !device) {
            printf("SKIP  frame generation guard: %s\n", !ffx ? "amd_fidelityfx_dx12.dll not found next to the exe" : "no WARP D3D12 device");
        } else {
            char what[160];
            FrameGenGuardInfo fi{};
            snprintf(what, sizeof what, "frame generation guard (%s): installed at the library's load, pool function +0x%X, %zu calls",
                     gsites->what, gsites->acquire, gsites->calls.size());
            Check(FrameGenCrashGuardInfo(&fi) && fi.module == reinterpret_cast<uint8_t*>(ffx) &&
                      fi.acquire == reinterpret_cast<uint8_t*>(ffx) + gsites->acquire &&
                      fi.calls == static_cast<int>(gsites->calls.size()),
                  what);
            bool redirected = true;
            for (const uint32_t rva : gsites->calls) {
                const uint8_t* call = reinterpret_cast<uint8_t*>(ffx) + rva;
                const uint8_t* to = RipTarget(call, 1, 5);
                redirected = redirected && call[0] == 0xE8 && to != fi.acquire && to[0] == 0xFF && to[1] == 0x25;
            }
            Check(redirected, "frame generation guard: each call goes through the guard's jump");
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* queue = nullptr;
            ID3D12Fence* hold = nullptr;
            device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold));
            queue->Wait(hold, 1);  // the queue stops here until the CPU signals hold
            // the pool: its CRITICAL_SECTION, then 8 slots per queue type of {queue, allocator, list, fence, value}
            alignas(16) static uint8_t pool[40 * (1 + 8 * 8)] = {};
            InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(pool));
            static const wchar_t name[] = L"SelfTestFrameGen";
            uint8_t* slots[9] = {};
            bool distinct = true;
            for (int i = 0; i < 8; ++i) {
                slots[i] = static_cast<uint8_t*>(FrameGenCrashGuardTestAcquire(pool, queue, name));
                for (int j = 0; j < i; ++j) distinct = distinct && slots[j] != slots[i];
                if (!slots[i]) { distinct = false; break; }
                // what the library's caller does once the list is recorded: signal the slot's fence on the queue
                ID3D12Fence* f = nullptr;
                uint64_t v = 0;
                memcpy(&f, slots[i] + 0x18, 8);
                memcpy(&v, slots[i] + 0x20, 8);
                queue->Signal(f, v);
            }
            Check(distinct && FrameGenCrashGuardInfo(&fi) && fi.waits == 0,
                  "frame generation guard: 8 lists taken from the pool without waiting");
            HANDLE release = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                Sleep(200);
                static_cast<ID3D12Fence*>(p)->Signal(1);  // the queue runs again: its 8 signals complete
                return 0;
            }, hold, 0, nullptr);
            const ULONGLONG t0 = GetTickCount64();
            slots[8] = static_cast<uint8_t*>(FrameGenCrashGuardTestAcquire(pool, queue, name));
            const ULONGLONG waited = GetTickCount64() - t0;
            Check(slots[8] && FrameGenCrashGuardInfo(&fi) && fi.waits == 1 && fi.gaveUp == 0 && waited >= 150 && waited < 2000,
                  "frame generation guard: all 8 on the held queue, the 9th waited for it (the library alone reads a null slot)");
            printf("      (waited %llu ms)\n", static_cast<unsigned long long>(waited));
            if (release) { WaitForSingleObject(release, INFINITE); CloseHandle(release); }
            SetFrameGenCrashGuardEnabled(false);
            Check(FrameGenCrashGuardTestAcquire(pool, queue, name) != nullptr && FrameGenCrashGuardInfo(&fi) && fi.waits == 1,
                  "frame generation guard off: the library's function as before (a list was free)");
            // and the crash itself: a second pool, the guard off, the queue held again
            alignas(16) static uint8_t pool2[40 * (1 + 8 * 8)] = {};
            InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(pool2));
            ID3D12Fence* hold2 = nullptr;
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold2));
            queue->Wait(hold2, 1);
            for (int i = 0; i < 8; ++i) {
                uint8_t* s2 = static_cast<uint8_t*>(FrameGenCrashGuardTestAcquire(pool2, queue, name));
                ID3D12Fence* f = nullptr;
                uint64_t v = 0;
                memcpy(&f, s2 + 0x18, 8);
                memcpy(&v, s2 + 0x20, 8);
                queue->Signal(f, v);
            }
            uintptr_t faultAt = 0, faultAddress = 0;
            const bool faulted = FrameGenLibraryFaults(pool2, queue, name, &faultAt, &faultAddress);
            snprintf(what, sizeof what, "frame generation guard off: the 9th list reads 0x8 at amd_fidelityfx_dx12.dll+0x%X (%s; "
                     "the game's crash)", gsites->fault, gsites->what);
            Check(faulted && faultAt == reinterpret_cast<uintptr_t>(ffx) + gsites->fault && faultAddress == 8, what);
            hold2->Signal(1);
            // pool2's lock stays with this thread (the library faulted holding it); its lists are released below
            SetFrameGenCrashGuardEnabled(true);
            // release what the pools created
            ID3D12Fence* idle = nullptr;  // wait until the queue is idle before releasing its lists
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle));
            queue->Signal(idle, 1);
            for (int n = 0; n < 2000 && idle->GetCompletedValue() < 1; ++n) Sleep(1);
            idle->Release();
            for (uint8_t* p : {pool, pool2})
                for (int t = 0; t < 8 * 8; ++t)
                    for (int k = 1; k <= 3; ++k) {
                        IUnknown* u = nullptr;
                        memcpy(&u, p + 40 * (1 + t) + 8 * k, 8);
                        if (u) u->Release();
                    }
            DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(pool));
            hold2->Release();
            hold->Release();
            queue->Release();
            device->Release();
        }
        if (warp) warp->Release();
        if (factory) factory->Release();
    }

    // FrameGenPacingTuning: AMD's frame pacing tuning set on the frame generation swap chain context (fakes, no library).
    {
        using namespace fgp;
        void* ctx = nullptr;
        Check(FrameGenPacingTestRun(reinterpret_cast<void*>(&FakeCreate), reinterpret_cast<void*>(&FakeDestroy),
                                    reinterpret_cast<void*>(&FakeConfigure), false, 1, &ctx) &&
                  ctx == reinterpret_cast<void*>(0x1234) && calls == 0 && FrameGenPacingTestApplied() == 0,
              "frame generation pacing: a library without the pacing key (the game's own FSR 3.1.0) is never configured");
        FrameGenPacingTestDestroy(ctx);
        Check(FrameGenPacingTestContext() == nullptr, "frame generation pacing: ffxDestroyContext forgets the context");
        Check(FrameGenPacingTestRun(reinterpret_cast<void*>(&FakeCreate), reinterpret_cast<void*>(&FakeDestroy),
                                    reinterpret_cast<void*>(&FakeConfigure), true, 0, &ctx) && calls == 0,
              "frame generation pacing: mode 0 leaves the swap chain as the game creates it");
        FrameGenPacingTestSwitch(1);
        Check(calls == 1 && type == 0x00030008 && key == 2 && ctxSeen == reinterpret_cast<void*>(0x1234) && Near(margin, 0.01f) &&
                  Near(variance, 0.3f) && !spin && spinTime == 2 && !waitObj && FrameGenPacingTestApplied() == 1,
              "frame generation pacing: switched to 1 live = key-value desc 0x30008, key 2, Set B (0.01 ms, 0.3, no hybrid spin, 2, spin on fence)");
        FrameGenPacingTestSwitch(2);
        Check(calls == 2 && Near(margin, 0.75f) && Near(variance, 0.1f), "frame generation pacing: 2 = Set A (0.75 ms, 0.1)");
        FrameGenPacingTestSwitch(0);
        Check(calls == 3 && Near(margin, 0.1f) && Near(variance, 0.1f) && FrameGenPacingTestApplied() == 0,
              "frame generation pacing: back to 0 live = AMD's defaults (0.1 ms, 0.1), the game's pacing");
        FrameGenPacingTestDestroy(reinterpret_cast<void*>(0x1234));
        FrameGenPacingTestSwitch(1);
        Check(calls == 3, "frame generation pacing: no context, nothing configured");
        FrameGenPacingTestSwitch(0);
        const std::string prov = FrameGenPacingTestProvider(reinterpret_cast<void*>(&FakeCreate), reinterpret_cast<void*>(&FakeQuery),
                                                            0x00020001);
        Check(prov == "4.0.1 (driver)" && queryType == 6 && queryCtx == reinterpret_cast<void*>(0x1234),
              "frame generation provider: a new frame generation context is asked (query desc 6) and a driver provider named");
        const std::string failed = FrameGenPacingTestProvider(reinterpret_cast<void*>(&FakeCreate),
                                                              reinterpret_cast<void*>(&FakeQueryFails), 0x00020001);
        queryType = 0;
        const std::string other = FrameGenPacingTestProvider(reinterpret_cast<void*>(&FakeCreate), reinterpret_cast<void*>(&FakeQuery),
                                                             0x00020002);
        Check(failed == "?" && queryType == 0 && other == "?",
              "frame generation provider: no answer = '?', other context types are not asked");

        // By provider (1.2.2): with 1, Adrenalin's driver swap chain (provider id bit 31) keeps its own pacing; 3 = Set B
        // on every swap chain (1.2.1's 1); 2 = Set A everywhere. AMD's DLL swap chains get Set B with 1 as before.
        const auto run = [&](int mode) {
            return FrameGenPacingTestRun(reinterpret_cast<void*>(&FakeCreate), reinterpret_cast<void*>(&FakeDestroy),
                                         reinterpret_cast<void*>(&FakeConfigure), true, mode, &ctx);
        };
        calls = 0;
        FrameGenPacingTestQuery(reinterpret_cast<void*>(&FakeQuery));  // the driver's provider
        Check(run(1) && calls == 0 && FrameGenPacingTestApplied() == 0,
              "frame generation pacing by provider: 1 on Adrenalin's driver swap chain (id bit 31) = its own pacing, not configured");
        FrameGenPacingTestSwitch(3);
        Check(calls == 1 && Near(margin, 0.01f) && Near(variance, 0.3f) && FrameGenPacingTestApplied() == 1,
              "frame generation pacing by provider: 3 = Set B on the driver's swap chain too");
        FrameGenPacingTestSwitch(1);
        Check(calls == 2 && Near(margin, 0.1f) && Near(variance, 0.1f) && FrameGenPacingTestApplied() == 0,
              "frame generation pacing by provider: back to 1 live on the driver's swap chain = AMD's defaults (its own pacing)");
        FrameGenPacingTestSwitch(2);
        Check(calls == 3 && Near(margin, 0.75f) && Near(variance, 0.1f) && FrameGenPacingTestApplied() == 2,
              "frame generation pacing by provider: 2 = Set A on the driver's swap chain too");
        FrameGenPacingTestDestroy(ctx);
        FrameGenPacingTestSwitch(0);
        FrameGenPacingTestQuery(reinterpret_cast<void*>(&FakeQueryDll));  // a provider inside AMD's DLL
        Check(run(1) && calls == 4 && Near(margin, 0.01f) && Near(variance, 0.3f) && FrameGenPacingTestApplied() == 1,
              "frame generation pacing by provider: 1 on AMD's DLL swap chain (id bit 31 clear) = Set B");
        FrameGenPacingTestSwitch(3);
        Check(calls == 4 && FrameGenPacingTestApplied() == 1,
              "frame generation pacing by provider: 3 on AMD's DLL swap chain = the same Set B, not configured again");
        FrameGenPacingTestDestroy(ctx);
        FrameGenPacingTestSwitch(0);
        FrameGenPacingTestQuery(reinterpret_cast<void*>(&FakeQueryFails));  // no answer: not the driver's
        Check(run(1) && calls == 5 && FrameGenPacingTestApplied() == 1,
              "frame generation pacing by provider: no provider answer = AMD's DLL swap chain, Set B");
        FrameGenPacingTestDestroy(ctx);
        FrameGenPacingTestSwitch(0);
        FrameGenPacingTestQuery(nullptr);

        // The call order AMD's ML frame generation requires: per frameID configure -> prepare -> generate. Frames 1-3
        // pipelined as the game does (the generation of frame N at present, after frame N+1's configure and prepare).
        static int passed = 0;
        struct Fake {
            static uint32_t Configure(void**, const void*) { ++passed; return 0; }
            static uint32_t Dispatch(void**, const void*) { ++passed; return 0; }
        };
        FrameGenOrderTestReset(reinterpret_cast<void*>(&Fake::Configure), reinterpret_cast<void*>(&Fake::Dispatch));
        alignas(8) uint8_t cfg[144] = {}, prep[64] = {}, gen[312] = {}, kvDesc[48] = {};
        const auto setType = [](uint8_t* d, uint64_t t) { memcpy(d, &t, 8); };
        setType(cfg, 0x00020002);
        setType(prep, 0x00020004);
        setType(gen, 0x00020003);
        setType(kvDesc, 0x00030008);
        const auto C = [&](uint64_t id) { memcpy(cfg + 136, &id, 8); FrameGenOrderTestCall(true, cfg); };
        const auto P = [&](uint64_t id) { memcpy(prep + 16, &id, 8); FrameGenOrderTestCall(false, prep); };
        const auto G = [&](uint64_t id) { memcpy(gen + 304, &id, 8); FrameGenOrderTestCall(false, gen); };
        C(1); P(1); C(2); P(2); G(1); G(2); C(3); P(3);
        FrameGenOrderTestCall(true, kvDesc);  // a swap chain key-value configure: passed on, not counted
        G(3);
        FrameGenOrderStats os = FrameGenOrderTestStats();
        Check(os.dispatches == 3 && os.inOrder == 3 && os.configures == 3 && os.prepares == 3 && os.idJumps == 0 && passed == 10,
              "frame generation order: pipelined frames 1-3 in AMD's order, every call passed on");
        P(5); G(5);        // frameID jump, no configure
        G(9);              // no prepare
        P(10); C(10); G(10);  // configure after prepare
        os = FrameGenOrderTestStats();
        Check(os.dispatches == 6 && os.inOrder == 3 && os.noConfigure == 1 && os.noPrepare == 1 && os.outOfOrder == 1 &&
                  os.idJumps == 2 && passed == 16,
              "frame generation order: missing configure, missing prepare, configure after prepare and frameID jumps counted");
    }

    // FrameGenCameraInfo: the camera desc linked to the frame generation prepare desc (build 19115871: the pNext store at
    // exe+0x40987D2, desc at [rbp+210h]; WorldToMetersScale +0xB98 -> ViewLocation +0xB30, ViewMatrices +0x350). The
    // stub is installed in the mapped exe and run through a caller that sets rbp/r10/rax as the game's code does.
    if (Installed(FindFrameGenCameraSite(), "frame generation camera: prepare desc site found")) {
        using namespace fgc;
        FrameGenCameraInfoOut o{};
        Check(FrameGenCameraInfoTestInfo(&o) && o.site == game.base + 0x40987D2 && o.descDisp == 0x210 && o.scale == 0xB98 &&
                  o.location == 0xB30 && o.matrices == 0x350,
              "frame generation camera: site exe+0x40987D2, desc [rbp+0x210], view layout 0xB98 / 0xB30 / 0x350");
        Check(ApplyFrameGenCameraInfo(true) && FrameGenCameraInfoTestInfo(&o) && o.stub && o.site[0] == 0xE8 &&
                  RipTarget(o.site, 1, 5) == o.stub && o.site[5] == 0x66 && o.site[6] == 0x90,
              "frame generation camera: the pNext store now calls the stub (call + 2-byte nop)");
        BuildView(o);
        ResetDesc();
        FrameGenCameraInfoTestAttach(g_view, g_desc);
        const Camera* cam = Linked();
        Check(cam && cam->header.type == 0x0002000A && cam->header.pNext == nullptr && Same(cam->position, g_location, 0.01) &&
                  Same(cam->right, g_right, 1e-6) && Same(cam->up, g_up, 1e-6) && Same(cam->forward, g_forward, 1e-6),
              "frame generation camera: a valid view links position, right, up and forward (ViewMatrix columns 0/1/2)");
        // A view whose InvViewMatrix position disagrees with ViewLocation (another layout): nothing linked.
        double moved[3] = {g_location[0] + 50, g_location[1], g_location[2]};
        Put(o.matrices + 4 * 0x80 + 12 * 8, moved, 3);
        ResetDesc();
        uint64_t rejected = o.rejected;
        FrameGenCameraInfoTestAttach(g_view, g_desc);
        Check(!Linked() && FrameGenCameraInfoTestInfo(&o) && o.rejected == rejected + 1,
              "frame generation camera: matrices that disagree with ViewLocation -> nothing linked");
        // Not a rotation: nothing linked.
        BuildView(o);
        const double stretched = 1.1;
        Put(o.matrices + 3 * 0x80, &stretched, 1);
        ResetDesc();
        FrameGenCameraInfoTestAttach(g_view, g_desc);
        Check(!Linked(), "frame generation camera: a ViewMatrix that is not a rotation -> nothing linked");
        BuildView(o);
        SetFrameGenCameraInfoEnabled(false);
        ResetDesc();
        FrameGenCameraInfoTestAttach(g_view, g_desc);
        Check(!Linked(), "frame generation camera off: nothing linked");
        SetFrameGenCameraInfoEnabled(true);
        // The stub itself, as called from the game's code.
        void* const callerMem = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (callerMem) {
            memcpy(callerMem, kCaller, sizeof kCaller);
            FlushInstructionCache(GetCurrentProcess(), callerMem, sizeof kCaller);
            ResetDesc();
            const uint64_t marker = 0xDEADBEEF;
            memcpy(g_desc + 8, &marker, sizeof marker);  // the stub's own store must clear it first
            const uint64_t back = reinterpret_cast<CallerFn>(callerMem)(g_desc - o.descDisp, g_view, o.stub);
            cam = Linked();
            Check(back == reinterpret_cast<uint64_t>(g_view) && cam && cam->header.type == 0x0002000A &&
                      Same(cam->forward, g_forward, 1e-6),
                  "frame generation camera: the stub stores pNext, links the camera, keeps rax and r10");
            Put(o.matrices + 4 * 0x80 + 12 * 8, moved, 3);
            ResetDesc();
            memcpy(g_desc + 8, &marker, sizeof marker);
            const uint64_t back2 = reinterpret_cast<CallerFn>(callerMem)(g_desc - o.descDisp, g_view, o.stub);
            Check(back2 == reinterpret_cast<uint64_t>(g_view) && !Linked(),
                  "frame generation camera: the stub with an invalid view leaves pNext cleared (the game's store)");
            VirtualFree(callerMem, 0, MEM_RELEASE);
        } else {
            Check(false, "frame generation camera: memory for the stub caller");
        }
        // The ffxDispatch counter on the library next to the exe (calls passed to a fake, not to AMD's code).
        std::wstring lib = exe;
        lib = lib.substr(0, lib.find_last_of(L"\\/") + 1) + L"amd_fidelityfx_dx12.dll";
        const HMODULE ffx = LoadLibraryW(lib.c_str());
        if (!ffx) {
            printf("SKIP  frame generation camera counter: amd_fidelityfx_dx12.dll not found next to the exe\n");
        } else {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(ffx) +
                                                                        reinterpret_cast<const IMAGE_DOS_HEADER*>(ffx)->e_lfanew);
            const FARPROC before = GetProcAddress(ffx, "ffxDispatch");
            void* const original = FrameGenCameraInfoTestDispatch(ffx, nt->OptionalHeader.SizeOfImage, reinterpret_cast<void*>(&FakeDispatch));
            const auto exported = reinterpret_cast<int32_t (*)(void*, const void*)>(GetProcAddress(ffx, "ffxDispatch"));
            Check(original == reinterpret_cast<void*>(before) && exported && reinterpret_cast<FARPROC>(exported) != before,
                  "frame generation camera: ffxDispatch export redirected to the counter");
            if (original && exported) {
                FrameGenCameraInfoTestInfo(&o);
                const uint64_t prepares = o.prepares, withOurs = o.withOurs;
                ResetDesc();
                exported(nullptr, g_desc);  // no camera
                BuildView(o);
                ResetDesc();
                FrameGenCameraInfoTestAttach(g_view, g_desc);
                exported(nullptr, g_desc);  // ours
                const uint64_t otherType = 0x00020003;  // a frame generation dispatch: not counted
                exported(nullptr, &otherType);
                Check(g_dispatchCalls == 3 && g_dispatchDesc == &otherType && FrameGenCameraInfoTestInfo(&o) &&
                          o.prepares == prepares + 2 && o.withOurs == withOurs + 1,
                      "frame generation camera: the counter passes every call on and counts prepares with our camera");
                FrameGenCameraInfoTestDispatch(ffx, 0, original);
            }
        }
    }

    // GPU crash report: the device is found among a crashing thread's stack words, and the DRED part of the report is
    // read, on a WARP device with DRED set up as the game does (markers only, with their names).
    {
        Check(InstallGpuCrashReport(), "crash report: exception handler installed");
        const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        const auto getInterface =
            d3d12 ? reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress(d3d12, "D3D12GetInterface")) : nullptr;
        const auto createDevice =
            d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
        const auto createFactory =
            dxgi ? reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
        static const GUID kDredClsid = {0x4a75bbc4, 0x9ff4, 0x4ad8, {0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2}};
#ifdef __ID3D12DeviceRemovedExtendedDataSettings2_INTERFACE_DEFINED__
        ID3D12DeviceRemovedExtendedDataSettings2* dred = nullptr;
        if (getInterface && SUCCEEDED(getInterface(kDredClsid, IID_PPV_ARGS(&dred))) && dred) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->UseMarkersOnlyAutoBreadcrumbs(TRUE);
            dred->Release();
        }
#else
        // Windows SDKs before 10.0.26100 don't declare DRED settings 2 (markers-only breadcrumbs).
        printf("SKIP  crash report: Windows SDK without ID3D12DeviceRemovedExtendedDataSettings2, DRED set-up not tested\n");
        (void)getInterface;
        (void)kDredClsid;
#endif
        IDXGIFactory4* factory = nullptr;
        IDXGIAdapter* warp = nullptr;
        ID3D12Device* device = nullptr;
        if (createFactory && createDevice && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) && factory &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp)
            createDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (!device) {
            printf("SKIP  crash report: no WARP D3D12 device on this machine, device search and DRED not tested\n");
        } else {
            // A queue that waits for a fence nobody signals, holding one list with nested markers.
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* queue = nullptr;
            ID3D12CommandAllocator* alloc = nullptr;
            ID3D12GraphicsCommandList* list = nullptr;
            ID3D12Fence* fence = nullptr;
            device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            static const wchar_t outer[] = L"SelfTestOuter", inner[] = L"SelfTestInner";
            list->BeginEvent(0, outer, sizeof outer);
            list->BeginEvent(0, inner, sizeof inner);
            list->EndEvent();
            list->EndEvent();
            list->Close();
            queue->Wait(fence, 1);
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);

            void* heap = malloc(64);
            memset(heap, 0x11, 64);
            uintptr_t words[8] = {0, 1, 0x1234, reinterpret_cast<uintptr_t>(heap), reinterpret_cast<uintptr_t>(&words[0]),
                                  reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), reinterpret_cast<uintptr_t>(queue), 0};
            auto* found = static_cast<ID3D12Device*>(GpuCrashReportTestFind(words, 8));
            Check(found == device, "crash report: the device found through a queue among stack words");
            if (found) found->Release();
            words[6] = reinterpret_cast<uintptr_t>(device);
            found = static_cast<ID3D12Device*>(GpuCrashReportTestFind(words, 8));
            Check(found == device, "crash report: the device itself found among stack words");
            if (found) found->Release();
            words[6] = 0;
            Check(GpuCrashReportTestFind(words, 8) == nullptr, "crash report: no D3D12 object among the words, no device");
            free(heap);

            ID3D12Device5* d5 = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d5))) && d5) {
                d5->RemoveDevice();
                d5->Release();
            }
            Check(GpuCrashReportTestDred(device), "crash report: DRED read after the device was removed, no fault");
            Check(LogContains("[gpucrash] device removed reason 0x887A"), "crash report: removed reason logged");
            Check(LogContains("[gpucrash] DRED command lists:") || LogContains("[gpucrash] DRED breadcrumbs not available"),
                  "crash report: DRED breadcrumbs summarised");
            printf("      (breadcrumbs %s on WARP)\n", LogContains("[gpucrash] DRED command lists:") ? "available" : "not available");
            fence->Release();
            list->Release();
            alloc->Release();
            queue->Release();
            device->Release();
        }
        if (warp) warp->Release();
        if (factory) factory->Release();
    }

    printf("\nSharedMemoryGpu: the GPU's memory kind from fake D3D12 answers, and what it turns to the game's way\n");
    {
        // A fake ID3D12Device: only CheckFeatureSupport (vtable slot 13) answers; every other slot is never called.
        struct FakeArch {
            void** vtbl;
            HRESULT arch1, arch0;  // the answers for D3D12_FEATURE_ARCHITECTURE1 / _ARCHITECTURE
            BOOL uma, coherent;
            int calls;
        };
        struct Fns {
            static HRESULT STDMETHODCALLTYPE Check(FakeArch* self, D3D12_FEATURE f, void* data, UINT size)
            {
                ++self->calls;
                if (f == D3D12_FEATURE_ARCHITECTURE1 && size == sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1)) {
                    if (FAILED(self->arch1)) return self->arch1;
                    auto* a = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1*>(data);
                    a->TileBasedRenderer = FALSE;
                    a->UMA = self->uma;
                    a->CacheCoherentUMA = self->coherent;
                    a->IsolatedMMU = TRUE;
                    return S_OK;
                }
                if (f == D3D12_FEATURE_ARCHITECTURE && size == sizeof(D3D12_FEATURE_DATA_ARCHITECTURE)) {
                    if (FAILED(self->arch0)) return self->arch0;
                    auto* a = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE*>(data);
                    a->TileBasedRenderer = FALSE;
                    a->UMA = self->uma;
                    a->CacheCoherentUMA = self->coherent;
                    return S_OK;
                }
                return E_INVALIDARG;
            }
            static void Trap() { __debugbreak(); }
        };
        void* vtbl[14];
        for (auto& v : vtbl) v = reinterpret_cast<void*>(&Fns::Trap);
        vtbl[13] = reinterpret_cast<void*>(&Fns::Check);
        auto kind = [&](HRESULT a1, HRESULT a0, BOOL uma, BOOL coherent, bool* coh, int* calls) {
            FakeArch f{vtbl, a1, a0, uma, coherent, 0};
            const GpuMemoryKind k = QueryGpuMemoryKind(&f, coh);
            *calls = f.calls;
            return k;
        };
        bool coh = true;
        int calls = 0;
        Check(kind(S_OK, S_OK, TRUE, TRUE, &coh, &calls) == GpuMemoryKind::Shared && coh && calls == 1,
              "SharedMemoryGpu: ARCHITECTURE1 UMA + cache-coherent -> shared (one query)");
        Check(kind(S_OK, S_OK, FALSE, FALSE, &coh, &calls) == GpuMemoryKind::Dedicated && !coh && calls == 1,
              "SharedMemoryGpu: ARCHITECTURE1 not UMA -> dedicated");
        Check(kind(E_INVALIDARG, S_OK, TRUE, FALSE, &coh, &calls) == GpuMemoryKind::Shared && !coh && calls == 2,
              "SharedMemoryGpu: an older runtime (no ARCHITECTURE1) -> ARCHITECTURE answers UMA -> shared");
        Check(kind(E_INVALIDARG, S_OK, FALSE, FALSE, &coh, &calls) == GpuMemoryKind::Dedicated && calls == 2,
              "SharedMemoryGpu: ARCHITECTURE alone, not UMA -> dedicated");
        Check(kind(E_FAIL, E_FAIL, TRUE, TRUE, &coh, &calls) == GpuMemoryKind::Unknown && !coh,
              "SharedMemoryGpu: no answer -> unknown (treated as a GPU with its own memory)");
        Check(QueryGpuMemoryKind(nullptr, &coh) == GpuMemoryKind::Unknown, "SharedMemoryGpu: no device -> unknown");
        Check(GameGpuMemoryKind() == GpuMemoryKind::Pending, "SharedMemoryGpu: nothing asked in the self-test -> pending");

        // The rule: 0 never, 1 always, 2 (default) only on a UMA GPU, on Windows itself, with under 20 GB of RAM.
        const GpuMemoryKind kinds[] = {GpuMemoryKind::Pending, GpuMemoryKind::Unknown, GpuMemoryKind::Dedicated, GpuMemoryKind::Shared};
        const uint64_t rams[] = {0, 11 * 1024 + 700, 16 * 1024, 20 * 1024 - 1, 20 * 1024, 24 * 1024, 32 * 1024, 128 * 1024};
        bool rule = Config().sharedMemoryGpu == 2 && kSharedGpuMaxRamMB == 20 * 1024;
        for (GpuMemoryKind k : kinds)
            for (bool wine : {false, true})
                for (uint64_t ram : rams) {
                    const bool lowRam = ram && ram < 20 * 1024;
                    rule = rule && !SharedMemoryGpuWanted(0, k, wine, ram) && SharedMemoryGpuWanted(1, k, wine, ram) &&
                           SharedMemoryGpuWanted(2, k, wine, ram) == (k == GpuMemoryKind::Shared && !wine && lowRam) &&
                           SharedMemoryGpuPossible(2, wine, ram) == (!wine && lowRam) && !SharedMemoryGpuPossible(0, wine, ram) &&
                           SharedMemoryGpuPossible(1, wine, ram);
                }
        Check(rule, "SharedMemoryGpu: default 2; 0 never, 1 always, 2 only UMA + not Wine + RAM known and under 20 GB");
        Check(SharedMemoryGpuWanted(2, GpuMemoryKind::Shared, false, 16 * 1024) &&
                  !SharedMemoryGpuWanted(2, GpuMemoryKind::Shared, false, 24 * 1024) &&
                  !SharedMemoryGpuWanted(2, GpuMemoryKind::Shared, false, 32 * 1024) &&
                  !SharedMemoryGpuWanted(2, GpuMemoryKind::Shared, true, 16 * 1024) &&
                  !SharedMemoryGpuWanted(2, GpuMemoryKind::Dedicated, false, 16 * 1024),
              "SharedMemoryGpu=2: a 16 GB Windows handheld yes; a 24 GB Ally X, a 32 GB APU, Proton/CrossOver (Wine), a graphics card no");
        const SharedGpuHost host = SharedGpuHostFacts();
        Check(!host.wine && host.ramMB >= 1024, "SharedMemoryGpu: this PC is Windows itself (no wine_get_version) and its RAM is known");
        Check(!SharedMemoryGpuWaitForAnswer(0) && !SharedMemoryGpuWaitForAnswer(1) &&
                  SharedMemoryGpuWaitForAnswer(2) == (host.ramMB < kSharedGpuMaxRamMB),
              "RdgParallelTranslate=2 waits for the GPU's answer only where SharedMemoryGpu could apply (not on a 20 GB+ PC)");

        // What it turns off (with Windows roomy, nothing else asks for relief).
        MemoryRelief r;
        MemoryReliefTestSharedGpu(GpuMemoryKind::Shared, 2, false, 16 * 1024, &r);
        Check(r.shared && r.upload && r.lumen && !r.translate && !r.osCache,
              "SharedMemoryGpu=2 on a 16 GB UMA Windows PC: UploadMemoryKeep and LumenKeepArrayCapacity as stock, relief.shared set");
        MemoryReliefTestSharedGpu(GpuMemoryKind::Shared, 2, true, 16 * 1024, &r);
        Check(!r.shared && !r.upload && !r.lumen, "SharedMemoryGpu=2 under Wine (Steam Deck, CrossOver): nothing changes");
        MemoryReliefTestSharedGpu(GpuMemoryKind::Shared, 2, false, 64 * 1024, &r);
        Check(!r.shared && !r.upload && !r.lumen, "SharedMemoryGpu=2 on a 64 GB UMA PC: nothing changes");
        MemoryReliefTestSharedGpu(GpuMemoryKind::Dedicated, 2, false, 16 * 1024, &r);
        Check(!r.shared && !r.upload && !r.lumen, "SharedMemoryGpu=2 on a GPU with its own memory: nothing changes");
        MemoryReliefTestSharedGpu(GpuMemoryKind::Unknown, 2, false, 16 * 1024, &r);
        Check(!r.shared && !r.upload && !r.lumen, "SharedMemoryGpu=2, kind not known: nothing changes");
        MemoryReliefTestSharedGpu(GpuMemoryKind::Dedicated, 1, true, 64 * 1024, &r);
        Check(r.shared && r.upload && r.lumen, "SharedMemoryGpu=1 (the desktop test switch): the shared-memory path anywhere");
        MemoryReliefTestSharedGpu(GpuMemoryKind::Shared, 0, false, 16 * 1024, &r);
        Check(!r.shared && !r.upload && !r.lumen, "SharedMemoryGpu=0 on a UMA GPU: the fixes as configured");
        Check(GameGpuMemoryKind() == GpuMemoryKind::Pending, "SharedMemoryGpu: the test put the live kind back");

        // CommandAllocatorTrim's tight level: the old rule unchanged without a shared GPU, always tight with one.
        const uint64_t G = 1ull << 30, H = 2 * G;
        bool tightRule = !AllocatorTrimTightStep(false, 4 * G, 16 * G, H, false) &&    // roomy
                         AllocatorTrimTightStep(false, 15 * G, 16 * G, H, false) &&    // within 2 GB
                         AllocatorTrimTightStep(true, 14 * G - (256ull << 20), 16 * G, H, false) &&  // hysteresis: stays tight
                         !AllocatorTrimTightStep(true, 13 * G, 16 * G, H, false) &&    // 512 MB below the line: not tight
                         !AllocatorTrimTightStep(false, 4 * G, 0, H, false) &&         // unknown budget: as before
                         AllocatorTrimTightStep(false, 4 * G, 16 * G, 0, false) &&     // headroom 0: always
                         AllocatorTrimTightStep(false, 1 * G, 16 * G, H, true) &&      // shared GPU: always
                         AllocatorTrimTightStep(false, 1 * G, 0, H, true);
        Check(tightRule, "CommandAllocatorTrim: tight as before on a GPU with its own memory, always on a shared-memory GPU");
    }

    printf("\nfix 16 video-memory relief without the translate stage (0.17.0 default)\n");
    {
        using E = VramReliefEvent;
        MemoryRelief relief;
        E ev = E::None;
        const uint64_t B = 10000;
        MemoryReliefTestAllowTranslate(false);
        MemoryReliefTestVramStep(true, 0, 0, true, &relief);
        auto steps = [&](int n, uint64_t vram) {
            for (int i = 1; i <= n; ++i) {
                ev = MemoryReliefTestVramStep(false, vram, B, true, &relief);
                if (ev != E::None) return i;
            }
            return 0;
        };
        Check(steps(15, 10500) == 15 && ev == E::Engaged && relief.upload && !relief.translate, "VRAM (no stage 2): stage 1 after 15 s");
        Check(steps(30, 10400) == 30 && ev == E::NotHelping && !relief.upload && !relief.translate,
              "VRAM (no stage 2): a failed stage 1 backs off, parallel translate never paused");
        Check(steps(10 * 60 - 1, 10500) == 0 && !relief.upload, "VRAM (no stage 2): 10 min back-off");
        Check(steps(1, 10500) == 1 && ev == E::Engaged && !relief.translate, "VRAM (no stage 2): retry is stage 1 again");
        Check(steps(20, 10300) == 0 && steps(10, 9900) == 10 && ev == E::Helped && relief.upload && !relief.translate,
              "VRAM (no stage 2): a helping stage 1 holds");
        Check(steps(15, 10200) == 15 && ev == E::NoLongerHelping && !relief.upload && !relief.translate,
              "VRAM (no stage 2): renewed pressure gives up instead of escalating");
        MemoryReliefTestAllowTranslate(true);
    }

    printf("\nfix 16 video-memory stages (VramRelief, 0.16.1; stage 2 with VramReliefPauseTranslate=1)\n");
    {
        using E = VramReliefEvent;
        MemoryRelief relief;
        E ev = E::None;
        const uint64_t B = 10000;
        auto reset = [&] { MemoryReliefTestVramStep(true, 0, 0, true, &relief); };
        auto off = [&] { return !relief.upload && !relief.translate; };
        auto upload = [&] { return relief.upload && !relief.translate; };
        auto both = [&] { return relief.upload && relief.translate; };
        // Return the step of the first event (1-based), 0 = none.
        auto steps = [&](int n, uint64_t vram, uint64_t budget, bool enabled = true) {
            for (int i = 1; i <= n; ++i) {
                ev = MemoryReliefTestVramStep(false, vram, budget, enabled, &relief);
                if (ev != E::None) return i;
            }
            return 0;
        };
        reset();
        Check(steps(14, 10500, B) == 0 && off(), "VRAM: arrival peaks do not trigger relief");
        Check(steps(30, 9000, B) == 0 && off(), "VRAM: pressure timer resets below budget");
        Check(steps(15, 10500, B) == 15 && ev == E::Engaged && upload(), "VRAM: stage 1 releases only upload memory after 15 s");
        Check(steps(29, 10400, B) == 0 && upload(), "VRAM: translation runs throughout stage 1 trial");
        Check(steps(1, 10400, B) == 1 && ev == E::Escalated && both(), "VRAM: stage 1 failure starts stage 2 with both overrides");
        Check(steps(29, 10400, B) == 0 && both(), "VRAM: stage 2 gets its own full trial");
        Check(steps(1, 10400, B) == 1 && ev == E::NotHelping && off(), "VRAM: both stages fail, both overrides cleared");
        for (int minutes : {10, 20, 40, 40}) {
            Check(steps(minutes * 60 - 1, 10500, B) == 0 && off(), "VRAM: failed episode backs off (10/20/40/40 min)");
            Check(steps(1, 10500, B) == 1 && ev == E::Engaged && upload(), "VRAM: retry starts at cheaper stage");
            Check(steps(30, 10500, B) == 30 && ev == E::Escalated && both(), "VRAM: retry escalates");
            Check(steps(30, 10500, B) == 30 && ev == E::NotHelping && off(), "VRAM: retry fails cleanly");
        }
        reset();
        steps(15, 10300, B);
        Check(steps(20, 10300, B) == 0 && upload(), "VRAM: first 20 s allow upload ramp to settle");
        Check(steps(10, B, B) == 10 && ev == E::Helped && upload(), "VRAM: at budget in last 10 s passes stage 1");
        Check(steps(120, 9800, B) == 0 && upload(), "VRAM: stage 1 holds without restoration headroom");
        Check(steps(59, B - 512, B) == 0 && upload() && steps(1, B - 512, B) == 1 && ev == E::Cleared && off(),
              "VRAM: stage 1 restores after 60 s with exactly 512 MB headroom");
        Check(steps(15, 10300, B) == 15 && ev == E::Engaged, "VRAM: successful episode has no back-off");
        steps(30, 9900, B);
        Check(steps(14, 10200, B) == 0 && upload() && steps(1, 10200, B) == 1 && ev == E::Escalated && both(),
              "VRAM: renewed pressure after stage 1 success escalates");
        Check(steps(20, 10200, B) == 0 && steps(10, 9800, B) == 10 && ev == E::Helped && both(),
              "VRAM: stage 2 success keeps both overrides");
        Check(steps(120, 9400, B) == 0 && both(), "VRAM: 512 MB headroom cannot restore two fixes");
        Check(steps(59, B - 1024, B) == 0 && steps(1, B - 1024, B) == 1 && ev == E::Cleared && off(),
              "VRAM: stage 2 restores both after 60 s with 1024 MB headroom");
        reset();
        steps(15, 10300, B);
        steps(30, 10300, B);
        steps(30, 9900, B);
        Check(steps(15, 10200, B) == 15 && ev == E::NoLongerHelping && off(),
              "VRAM: renewed pressure in stage 2 clears both overrides");
        Check(steps(599, 10200, B) == 0 && off() && steps(1, 10200, B) == 1 && ev == E::Engaged,
              "VRAM: failed hold backs off 10 min");
        for (int stage : {1, 2}) {
            reset();
            steps(15, 10300, B);
            if (stage == 2) steps(30, 10300, B);
            Check(steps(27, 9000, B) == 0 && steps(3, 10100, B) == 3 &&
                  ev == (stage == 1 ? E::Escalated : E::NotHelping), "VRAM: 3 over-budget samples fail the trial");
            reset();
            steps(15, 10300, B);
            if (stage == 2) steps(30, 10300, B);
            Check(steps(28, 9000, B) == 0 && steps(2, 10100, B) == 2 && ev == E::Helped,
                  "VRAM: 2 over-budget samples pass the trial");
        }
        // Cancellation in both probes and holds; neither a missing budget nor disabled relief proves success.
        for (int stage : {1, 2}) for (bool hold : {false, true}) {
            for (bool enabled : {false, true}) {
                reset();
                steps(15, 10300, B);
                if (stage == 2) steps(30, 10300, B);
                if (hold) steps(30, 9900, B);
                Check(steps(1, 10300, enabled ? 0 : B, enabled) == 1 && off() &&
                      ev == (enabled ? E::BudgetUnknown : E::SwitchedOff), "VRAM: cancellation clears both outputs in every state");
                Check(steps(100, 10300, enabled ? 0 : B, enabled) == 0 && off(), "VRAM: no relief without enabled, valid budget");
                Check(steps(14, 10300, B) == 0 && off() && steps(1, 10300, B) == 1 && ev == E::Engaged,
                      "VRAM: fresh pressure required after cancellation");
            }
        }
        reset();
        Check(steps(100, 12000, 0) == 0 && off(), "VRAM: unknown budget never engages");
    }
    {
        // Commit relief: 3 s under MemoryReliefCommitMB (1024), or at once under half of it.
        for (int i = 0; i < 61; ++i) MemoryReliefTestCommitStep(4096);  // a clear start (60 s with twice the threshold)
        const bool a = !MemoryReliefTestCommitStep(900) && !MemoryReliefTestCommitStep(900) && MemoryReliefTestCommitStep(900);
        for (int i = 0; i < 61; ++i) MemoryReliefTestCommitStep(4096);
        const bool cleared = !MemoryReliefTestCommitStep(4096);
        const bool b = MemoryReliefTestCommitStep(400);
        for (int i = 0; i < 61; ++i) MemoryReliefTestCommitStep(4096);
        Check(a && cleared && b && !MemoryReliefTestCommitStep(4096),
              "commit relief: on after 3 s under 1024 MB, at once under 512 MB, off after a minute with twice the room");
    }
    // ViewRenameGuard, the view rename guard (UE-217692): the game's entries are detoured; the originals are replaced by fakes.
    {
        using namespace vrg;
        Check(FindViewRenameGuard(), "view rename guard: FD3D12View::CreateView and OnAllocationMoved found");
        ViewRenameGuardInfoData vi;
        Check(ViewRenameGuardInfo(&vi) && vi.createView == game.base + 0x255CDC0 && vi.onAllocationMoved == game.base + 0x2549E80,
              "view rename guard: CreateView at exe+0x255CDC0, OnAllocationMoved at exe+0x2549E80");
        {
            // FD3D12UnorderedAccessView_RHI's FD3D12View vtable (exe+0x7263138, stored at +0x28 by its constructor):
            // slot 0 = the ResourceRenamed thunk to the _RHI CreateView (mov rax,[rcx-28h]; add rcx,-28h; jmp [rax+10h]),
            // slot 2 = the UAV UpdateDescriptor the guard uses to recognise UAVs.
            void* const* uavVtable = reinterpret_cast<void* const*>(game.base + 0x7263138);
            Check(vi.uavUpdateDescriptor == game.base + 0x2567B50 && uavVtable[2] == vi.uavUpdateDescriptor &&
                      MatchesAt(static_cast<const uint8_t*>(uavVtable[0]), ParsePattern("48 8B 41 D8 48 83 C1 D8 48 FF 60 10")),
                  "view rename guard: UAV UpdateDescriptor at exe+0x2567B50 = slot 2 of the UAV_RHI view vtable; slot 0 re-creates the view");
        }
        if (Installed(InstallViewRenameGuard(1), "view rename guard installed")) {
            Check(game.base[0x255CDC0] == 0xE9 && game.base[0x2549E80] == 0xE9, "view rename guard: both entries jump to the hooks");
            ViewRenameGuardTestSetOriginals(reinterpret_cast<void*>(&FakeCreateView), reinterpret_cast<void*>(&FakeMoved));
            g_entry = reinterpret_cast<CreateFn>(game.base + 0x255CDC0);
            const auto moved = reinterpret_cast<bool (*)(uint8_t*, void*)>(game.base + 0x2549E80);
            InitializeCriticalSection(Lock());
            *reinterpret_cast<void***>(g_view) = g_vtable;
            const auto reset = [&] { memset(g_view + 8, 0, sizeof g_view - 8); g_creates = g_renamed = 0; g_lockHeld = false; };
            uint8_t* const infoA[4] = {g_base, g_location, g_resA, nullptr};
            uint8_t* const infoB[4] = {g_base, g_location, g_resB, nullptr};
            uint8_t* const infoManual[4] = {nullptr, g_location, g_resB, nullptr};

            reset();
            Field(g_location, 0x10) = g_resA;
            g_entry(g_view, infoA, 0);
            Check(g_creates == 1 && g_lockHeld && g_renamed == 0 && Field(g_view, 0x20) == g_resA && !Held(),
                  "view rename guard: a first registration runs under the listener lock; nothing to repair");
            reset();
            Field(g_location, 0x10) = g_resB;  // the creator read resA, the buffer moved to resB before it registered
            g_entry(g_view, infoA, 0);
            Check(g_creates == 2 && g_renamed == 1 && g_lockHeldInRenamed && Field(g_view, 0x20) == g_resB && !Held(),
                  "view rename guard: a stale view is repaired by its own ResourceRenamed under the lock (nested CreateView passes through)");
            g_creates = g_renamed = 0;
            g_entry(g_view, infoB, 0);
            Check(g_creates == 1 && !g_lockHeld && g_renamed == 0, "view rename guard: re-creating a registered view (rename loop) passes through");
            reset();
            g_entry(g_view, infoManual, 0);
            Check(g_creates == 1 && !g_lockHeld && g_renamed == 0, "view rename guard: a view without a base resource passes through");
            reset();
            Field(g_location, 0x10) = g_resA;
            Check(moved(g_location, nullptr) && g_moves == 1 && Field(g_location, 0x10) == g_resB,
                  "view rename guard: OnAllocationMoved runs the original through the hook");
            g_entry(g_view, infoB, 0);  // not stale, but the location moved just now (a torn read is possible)
            Check(g_creates == 2 && g_renamed == 1 && Field(g_view, 0x20) == g_resB,
                  "view rename guard: a view of a location moved within the last second is re-created once");
            Check(ViewRenameGuardRepairing(), "view rename guard: repairing (RdgParallelTranslate's frame fence may be used)");
            SetViewRenameGuardMode(2);
            reset();
            Field(g_location, 0x10) = g_resA;
            g_entry(g_view, infoB, 0);
            Check(g_creates == 1 && g_lockHeld && g_renamed == 0 && Field(g_view, 0x20) == g_resB,
                  "view rename guard: count-only mode checks under the lock and does not repair");
            SetViewRenameGuardMode(0);
            reset();
            g_entry(g_view, infoB, 0);
            Check(g_creates == 1 && !g_lockHeld && g_renamed == 0 && !ViewRenameGuardRepairing(),
                  "view rename guard: mode 0 passes everything through (and the frame fence is not used)");
            Check(ViewRenameGuardInfo(&vi) && vi.stale == 2 && vi.repairs == 2 && vi.recentOnly == 1 && vi.moves == 1,
                  "view rename guard: counters (2 stale, 1 moved-recently, 2 repairs, 1 move)");
            SetViewRenameGuardMode(1);
            // Counter UAVs (repair would create a counter buffer under the listener lock): left as stock in mode 1,
            // repaired in mode 3; a UAV without a counter is repaired in mode 1.
            g_uavVtable[2] = vi.uavUpdateDescriptor;
            ViewRenameGuardInfo(&vi);
            const uint64_t leftBefore = vi.counterLeft, repairsBefore = vi.repairs;
            alignas(16) static uint8_t fakeCounter[0x40];
            for (int m : {1, 3}) {
                SetViewRenameGuardMode(m);
                reset();
                *reinterpret_cast<void***>(g_view) = g_uavVtable;
                Field(g_view, 0x88) = fakeCounter;
                Field(g_location, 0x10) = g_resB;
                g_entry(g_view, infoA, 0);  // stale counter UAV
                Check(m == 1 ? g_renamed == 0 && Field(g_view, 0x20) == g_resA : g_renamed == 1 && Field(g_view, 0x20) == g_resB,
                      m == 1 ? "view rename guard: mode 1 leaves a stale counter UAV as in the stock game"
                             : "view rename guard: mode 3 repairs a stale counter UAV");
            }
            SetViewRenameGuardMode(1);
            reset();
            *reinterpret_cast<void***>(g_view) = g_uavVtable;  // UAV without a counter (+0x88 null)
            g_entry(g_view, infoA, 0);
            Check(g_renamed == 1 && Field(g_view, 0x20) == g_resB, "view rename guard: mode 1 repairs a UAV without a counter");
            ViewRenameGuardInfo(&vi);
            Check(vi.counterLeft == leftBefore + 1 && vi.repairs == repairsBefore + 2,
                  "view rename guard: counter UAVs left as stock are counted separately");
            *reinterpret_cast<void***>(g_view) = g_vtable;
            DeleteCriticalSection(Lock());
        }
    }
    rtb::RtBatchSelfTest(game);     // RayTracingBuildBatch (last: it patches a call other searches might read)
    // obseparity review S2: no fix's search or install met a [hook] case on the clean exe (only the hook test's own)
    Check(ForeignHooksMet() == hooks_test::g_hooksAfterRun, "hooks: zero [hook] lines from the other fixes' searches and writes the self-test runs, on the clean exe");
    scantest::Report();
    LogFlush();
    printf("\n--- selftest.log ---\n");
    if (FILE* f = _wfsopen(L"selftest.log", L"r", _SH_DENYNO)) {
        char line[1024];
        while (fgets(line, sizeof line, f)) fputs(line, stdout);
        fclose(f);
    }
    printf("\n%s (%d failure(s))\n", g_failures ? "SELFTEST FAILED" : "SELFTEST OK", g_failures);
    return g_failures ? 1 : 0;
}
