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
#include <string>
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
static bool FakeDelExec(void* inst)
{
    const uint8_t* p = static_cast<const uint8_t*>(inst);
    const int id = *reinterpret_cast<const int*>(p + 0x20);
    g_pcDelRan.push_back(id);
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
}  // namespace fgp

static void Check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
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
#include "selftest_npcbodies.h"     // NpcBodiesOneLock: sites and the one-hold wrapper with a fake scene lock
#include "selftest_rtscene.h"       // RtSceneBufferKeep: sites, patch and the stub's decisions
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
#include "selftest_animpar.h"      // AnimTeardownParallel's shapes and the helper queue with fakes
#include "selftest_cellbuf.h"      // CellBufferPacedEvict's shapes, the clamp stub and the buffer LRU with and without the fix
#include "selftest_ledger.h"       // diagnostic HitchLedger: sites, record/ring/csv format, cost per frame
#include "selftest_objreplace.h"   // ObjectReplaceCrashGuard: the in-place destroy site and the stub with a fake GUObjectArray
#include "selftest_taskwait.h"     // TaskWaitDeadlockGuard: the task graph's worker wait, the wake-up protocol against fakes
#include "selftest_scan.h"         // FindAll: the fast pattern search against the reference search

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
        DeleteFileW(l0); DeleteFileW(l1); DeleteFileW(l2); DeleteFileW(ini);
        RemoveDirectoryW(L"logfile");
    }
    LogOpen(L"selftest.log");

    GameModule game;
    Check(InitGameModule(game, module), "game module sections found");
    scantest::Begin();  // every search below is checked against the reference search
    scantest::RandomTests(game);

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
            // true = skipped; false = the original was called with the same arguments
            auto skipped = [&]() {
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
            Check(s1.calls - s0.calls == 20 && s1.skipped - s0.skipped == 6 && s1.flags - s0.flags == 1 &&
                      s1.values - s0.values == 7 && s1.shape - s0.shape == 2 && s1.unverified - s0.unverified == 3 &&
                      s1.disabled - s0.disabled == 1 && s1.faults == 0 && s1.checked - s0.checked == 10 && s1.checkFailed == 0,
                  "cloth: statistics count every outcome");
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

    // HavokPickWarningGuard: the old engine's "Havok picks" warning box (a modal MessageBoxA on the OblivionThread) held back.
    {
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
    // AnimTeardownParallel (lab prototype): dying NPC animation data's interpolators on helper threads (selftest_animpar.h).
    apar::Run(game);
    // CellBufferPacedEvict (lab prototype): spare cell buffer slots, overflow torn down one cell per OT frame (selftest_cellbuf.h); after
    // the ledger's test, which detours the same grid-change entry.
    cbuf::Run(game);

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
            Check(why == nullptr, why ? why : "allocators: give-back safe against an obtain between the passes; off after an exception stays off");
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
    nbl::NpcBodiesSelfTest(game);   // NpcBodiesOneLock
    rts::RtSceneSelfTest(game);     // RtSceneBufferKeep
    nsb::NaniteSelfTest(game);      // NaniteInitialRootPages (lab: NaniteStreamProbe / Nanite*MinMB)
    wtr::WaterInfoSelfTest(game);   // WaterCaptureFsrSkip / NoRhiFlush / NoOcclusionWait / FastMeshScan (lab: WaterInfoProbe)
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
        ID3D12DeviceRemovedExtendedDataSettings2* dred = nullptr;
        if (getInterface && SUCCEEDED(getInterface(kDredClsid, IID_PPV_ARGS(&dred))) && dred) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->UseMarkersOnlyAutoBreadcrumbs(TRUE);
            dred->Release();
        }
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
    scantest::Report();
    printf("\n--- selftest.log ---\n");
    if (FILE* f = _wfsopen(L"selftest.log", L"r", _SH_DENYNO)) {
        char line[1024];
        while (fgets(line, sizeof line, f)) fputs(line, stdout);
        fclose(f);
    }
    printf("\n%s (%d failure(s))\n", g_failures ? "SELFTEST FAILED" : "SELFTEST OK", g_failures);
    return g_failures ? 1 : 0;
}
