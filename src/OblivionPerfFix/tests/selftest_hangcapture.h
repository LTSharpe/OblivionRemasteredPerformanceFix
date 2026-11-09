// GPU hang capture self-test (worker hangcapture, 2026-11-03), included by selftest.cpp: GpuPassMarkers' sites on the mapped
// exe (build 19115871) and its PIX3 encoding and hooks with fakes (a real WARP command list for the D3D12 calls); the
// crash report's new parts with fakes: the per-queue DRED summary from a fake breadcrumb chain,
// the context / history / ini lines from fake samples. Nothing here reads another program.
#pragma once

namespace hangcap {

int g_push = 0, g_pop = 0;
int FakePush(void*, void*, const char*) { ++g_push; return 3; }
int FakePop(void*, void*) { ++g_pop; return 4; }

void Markers(const GameModule& game)
{
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t emit = 0, push = 0, pop = 0;
        const bool ok = GpuPassMarkersTestDiscover(game, &emit, &push, &pop);
        Check(ok && emit == 0x91F9E78 && push == 0x2501170 && pop == 0x2500E7B,
              "GpuPassMarkers: D3D12.EmitRgpFrameMarkers and RHIPushEvent / RHIPopEvent's AGS calls found on the mapped exe");
        if (!ok || emit != 0x91F9E78 || push != 0x2501170 || pop != 0x2500E7B)
            printf("    found %d: variable exe+0x%X, push call exe+0x%X, pop call exe+0x%X\n", ok, emit, push, pop);
    }
    // PIX3 blob: event info, colour, string info, the name as UTF-16 with its zero in 8-byte words
    uint64_t b[27] = {};
    UINT size = GpuPassMarkersTestEncode(b, 27, "Abc", false);
    Check(size == 32 && b[0] == (0x002ull << 10) && b[2] == (8ull << 55) && !wcscmp(reinterpret_cast<const wchar_t*>(b + 3), L"Abc"),
          "GpuPassMarkers: PIX3 begin-event blob for a 3-letter name (4 words)");
    size = GpuPassMarkersTestEncode(b, 27, "Abcd", true);
    Check(size == 40 && b[0] == (0x008ull << 10) && b[4] == 0 && !wcscmp(reinterpret_cast<const wchar_t*>(b + 3), L"Abcd"),
          "GpuPassMarkers: a 4-letter name takes a fifth word for its zero (marker type)");
    char longName[300];
    memset(longName, 'x', sizeof longName - 1);
    longName[sizeof longName - 1] = 0;
    memset(b, 0xCC, sizeof b);
    size = GpuPassMarkersTestEncode(b, 27, longName, false);
    Check(size == 27 * 8 && reinterpret_cast<const wchar_t*>(b + 3)[95] == 0 && reinterpret_cast<const wchar_t*>(b + 3)[94] == L'x',
          "GpuPassMarkers: a long name is cut at 95 characters inside the buffer");
    // hooks: mode 0 nothing, mode 2 AGS only (no D3D12 call: a null list is never touched)
    g_push = g_pop = 0;
    Check(GpuPassMarkersTestHooks(0, reinterpret_cast<void*>(&FakePush), reinterpret_cast<void*>(&FakePop), nullptr, "X") == 0 &&
              g_push == 0 && g_pop == 0,
          "GpuPassMarkers: mode 0 calls neither D3D12 nor AGS");
    Check(GpuPassMarkersTestHooks(2, reinterpret_cast<void*>(&FakePush), reinterpret_cast<void*>(&FakePop), nullptr, "X") == 7 &&
              g_push == 1 && g_pop == 1,
          "GpuPassMarkers: mode 2 passes push and pop on to AGS");
    // modes 1 and 3 on a real (WARP) command list: BeginEvent / EndEvent go through
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
    const auto createDevice = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    const auto createFactory = dxgi ? reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device* device = nullptr;
    if (createFactory && createDevice && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) && factory &&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp)
        createDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (!device) printf("SKIP  GpuPassMarkers: no WARP device, modes 1/3 not run on a command list\n");
    else {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (alloc) device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
        if (list) {
            g_push = g_pop = 0;
            const int r1 = GpuPassMarkersTestHooks(1, reinterpret_cast<void*>(&FakePush), reinterpret_cast<void*>(&FakePop), list, "Lumen");
            const int r3 = GpuPassMarkersTestHooks(3, reinterpret_cast<void*>(&FakePush), reinterpret_cast<void*>(&FakePop), list, longName);
            Check(r1 == 0 && r3 == 7 && g_push == 1 && g_pop == 1 && SUCCEEDED(list->Close()),
                  "GpuPassMarkers: modes 1 and 3 record BeginEvent / EndEvent on a command list (3 also AGS); the list closes");
            list->Release();
        }
        if (alloc) alloc->Release();
        device->Release();
    }
    if (warp) warp->Release();
    if (factory) factory->Release();
}

// A fake breadcrumb node.
struct Node {
    D3D12_AUTO_BREADCRUMB_NODE1 n{};
    UINT last = 0;
    D3D12_AUTO_BREADCRUMB_OP ops[16];
    D3D12_DRED_BREADCRUMB_CONTEXT ctx[8];
};
void Make(Node& x, const wchar_t* queue, const void* queueKey, const D3D12_AUTO_BREADCRUMB_OP* ops, UINT count, UINT last,
          const wchar_t* const* names, Node* next)
{
    x.n.pCommandQueueDebugNameW = queue;
    x.n.pCommandQueue = static_cast<ID3D12CommandQueue*>(const_cast<void*>(queueKey));
    x.n.pCommandListDebugNameW = L"list";
    memcpy(x.ops, ops, count * sizeof ops[0]);
    x.n.BreadcrumbCount = count;
    x.last = last;
    x.n.pLastBreadcrumbValue = &x.last;
    x.n.pCommandHistory = x.ops;
    UINT c = 0;
    for (UINT i = 0; i < count && names; ++i)
        if (names[i] && c < 8) x.ctx[c].BreadcrumbIndex = i, x.ctx[c].pContextString = names[i], ++c;
    x.n.BreadcrumbContextsCount = c;
    x.n.pBreadcrumbContexts = x.ctx;
    x.n.pNext = next ? &next->n : nullptr;
}

void Report()
{
    using O = D3D12_AUTO_BREADCRUMB_OP;
    const O L = D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST, B = D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT, E = D3D12_AUTO_BREADCRUMB_OP_ENDEVENT,
            D = D3D12_AUTO_BREADCRUMB_OP_DISPATCH, R = D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS;
    static int q3d, qcs;
    static Node a, b, c, d, e;
    const O oa[] = {L, B, B, E, E};
    const wchar_t* na[] = {nullptr, L"Frame", L"Shadows", nullptr, nullptr};
    const O ob[] = {L, B, B, B, D, E, R, E, E};
    const wchar_t* nb[] = {nullptr, L"Frame", L"Lumen", L"ScreenProbeGather", nullptr, nullptr, nullptr, nullptr, nullptr};
    const O oc[] = {L, B, D, E};
    const wchar_t* nc[] = {nullptr, L"Translucency", nullptr, nullptr};
    const O od[] = {L, B, D, E};
    const wchar_t* nd[] = {nullptr, L"AsyncLumen", nullptr, nullptr};
    const O oe[] = {L, B, D, E};
    const wchar_t* ne[] = {nullptr, L"AsyncNext", nullptr, nullptr};
    Make(a, L"3D Queue (GPU 0)", &q3d, oa, 5, 5, na, &b);
    Make(b, L"3D Queue (GPU 0)", &q3d, ob, 9, 4, nb, &c);
    Make(c, L"3D Queue (GPU 0)", &q3d, oc, 4, 0, nc, &d);
    Make(d, L"Compute Queue (GPU 0)", &qcs, od, 4, 4, nd, &e);
    Make(e, L"Compute Queue (GPU 0)", &qcs, oe, 4, 0, ne, nullptr);
    GpuCrashReportTestBreadcrumbs(&a.n);
    Check(LogContains("[gpucrash]   3D Queue (GPU 0), list 2 of 5 (list): 4 of 9 operations done; open: Frame > Lumen > ScreenProbeGather; "
                      "next: dispatch, end, dispatch rays, end, end"),
          "crash report: the list the GPU stopped in, with its open passes and the next operations");
    Check(LogContains("[gpucrash]   Compute Queue (GPU 0): list 4 of 5 finished (its end: begin AsyncLumen, dispatch, end), list 5 never "
                      "started (its start: begin AsyncNext, dispatch, end)"),
          "crash report: where a finished list meets a never-started one on the same queue");
    Check(LogContains("[gpucrash] DRED 3D Queue (GPU 0): 3 command lists, 1 finished, 1 stopped part-way, 1 never started") &&
              LogContains("[gpucrash] DRED Compute Queue (GPU 0): 2 command lists, 1 finished, 0 stopped part-way, 1 never started") &&
              LogContains("[gpucrash] DRED command lists: 1 stopped part-way (1 shown above), 2 not started, 2 finished"),
          "crash report: per-queue DRED counts and the total line");

    // context, history and ini lines (fake samples, a small ini of our own)
    wchar_t ini[MAX_PATH];
    GetFullPathNameW(L"selftest-gpucrash.ini", MAX_PATH, ini, nullptr);
    FILE* f = nullptr;
    if (!_wfopen_s(&f, ini, L"w") && f) {
        fputs("[Fixes]\nAlpha=1\n; a comment\nBeta=2\n[General]\nGpuPassMarkers=1\n", f);
        fclose(f);
    }
    SetGpuCrashIniPath(ini);
    GpuCrashReportTestContext();
    Check(LogContains("[gpucrash] context: game process up") && LogContains("GpuPassMarkers not installed"),
          "crash report: the context line (uptime, position, marker mode)");
    Check(LogContains("frames 60 (longest 33.0 ms), GPU 14.0 ms") && LogContains("VRAM 12.00 of 11.00 GB (109%)"),
          "crash report: one line per second of the last minute (VRAM against budget, RAM, commit, frames)");
    Check(LogContains("[gpucrash] ini 1: [Fixes] Alpha=1 Beta=2 | [General] GpuPassMarkers=1"),
          "crash report: the ini keys in effect (comments left out)");
    // The public report never names another program (video memory per process is a lab-only feature).
    Check(!LogContains("by process"),
          "crash report: no per-process video memory line (public build)");
    DeleteFileW(ini);
}

void Run(const GameModule& game)
{
    Markers(game);
    Report();
}

}  // namespace hangcap
