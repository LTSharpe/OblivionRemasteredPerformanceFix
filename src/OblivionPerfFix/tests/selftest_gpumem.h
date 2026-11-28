// GpuMemoryRecord self-test (worker pubdiagD, 2026-11-28; S-082 / #54), included by selftest.cpp.
// The exe's two device-creation import slots are found in the mapped game exe; on a WARP device made through the
// GetProcAddress route's wrapper (the same code the game's device goes through), a committed buffer, a committed texture, a
// heap with a placed buffer and a reserved buffer are recorded; names, frees, the heap's VA from the placed buffer, and the
// report's look-ups (inside a freed buffer, inside a heap, between two ranges with a VA-less object made between them) are
// checked against the log; the bookkeeping cost per call is measured; every patched slot is put back.
#pragma once

static ID3D12Device* g_gmDev = nullptr;
static uint64_t g_gmFreedVa = 0;

static void GpuMemRun()
{
    printf("\nGpuMemoryRecord: import routes, WARP device recording, frees, names, heap VA, report look-ups, cost\n");
    bool delay = false, gpa = false;
    GpuMemoryRecordTestRoutes(Game().base, &delay, &gpa);
    Check(delay, "GpuMemoryRecord: the exe's d3d12.dll delay import #101 (D3D12CreateDevice) found");
    Check(gpa, "GpuMemoryRecord: the exe's KERNEL32 GetProcAddress import found");
    Check(GpuMemoryRecordTestInstall(), "GpuMemoryRecord: table allocated");

    const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    const auto realCreate = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    const auto createFactory = dxgi ? reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device* dev = nullptr;
    const auto wrapped =
        realCreate ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GpuMemoryRecordTestWrap(reinterpret_cast<void*>(realCreate))) : nullptr;
    if (createFactory && wrapped && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) && factory &&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp)
        wrapped(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (!dev) {
        printf("SKIP  GpuMemoryRecord: no WARP D3D12 device on this machine, recording not tested\n");
    } else {
        Check(GpuMemoryRecordTestAttached(), "GpuMemoryRecord: the device made through the wrapper is recorded (vtable patched)");
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC buf{};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = 1u << 20;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = 256;
        tex.Height = 256;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex.SampleDesc.Count = 1;
        ID3D12Resource *a = nullptr, *b = nullptr, *t = nullptr, *placed = nullptr, *reserved = nullptr;
        ID3D12Heap* heap = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&a));
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b));
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = 4u << 20;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        dev->CreateHeap(&hd, IID_PPV_ARGS(&heap));
        if (heap) dev->CreatePlacedResource(heap, 1u << 20, &buf, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&placed));
        dev->CreateReservedResource(&buf, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reserved));
        Check(a && b && t && heap && placed, "GpuMemoryRecord: WARP made the buffers, texture, heap and placed buffer");
        if (a && b && t && heap && placed) {
            b->SetName(L"SelfTest freed buffer");
            uint64_t va = 0, size = 0;
            char name[48] = "";
            Check(GpuMemoryRecordTestFind(a, &va, &size, nullptr, 0) == 1 && va == a->GetGPUVirtualAddress() && size == (1u << 20),
                  "GpuMemoryRecord: committed buffer recorded alive with its VA and size");
            Check(GpuMemoryRecordTestFind(t, &va, &size, nullptr, 0) == 1 && va == 0 && size >= 256 * 256 * 4,
                  "GpuMemoryRecord: committed texture recorded (no VA, its allocation size)");
            Check(GpuMemoryRecordTestFind(heap, &va, &size, nullptr, 0) == 1 && size == (4u << 20) &&
                      va == placed->GetGPUVirtualAddress() - (1u << 20),
                  "GpuMemoryRecord: heap recorded, its VA learned from the placed buffer (buffer VA - offset)");
            Check(GpuMemoryRecordTestFind(placed, nullptr, nullptr, nullptr, 0) == 0, "GpuMemoryRecord: placed resources get no entry");
            Check(!reserved || GpuMemoryRecordTestFind(reserved, &va, nullptr, nullptr, 0) == 1, "GpuMemoryRecord: reserved buffer recorded");
            const uint64_t freedVa = b->GetGPUVirtualAddress();
            g_gmFreedVa = freedVa;
            const void* bKey = b;
            Check(b->Release() == 0, "GpuMemoryRecord: the named buffer's last Release returns 0 through the hook");
            Check(GpuMemoryRecordTestFind(bKey, &va, nullptr, name, sizeof name) == 2 && va == freedVa && strcmp(name, "SelfTest freed buffer") == 0,
                  "GpuMemoryRecord: the released buffer is kept as freed, with its VA and SetName's name");
            // the report: a fault inside the freed buffer (as #54's address would be), then inside the heap
            Log("[gpucrash] --- self-test sample: fault inside the freed buffer ---");
            GpuMemoryRecordReport(freedVa + 0x1230);
            Check(LogContains("was inside (offset 0x1230): #") && LogContains("\"SelfTest freed buffer\"") && LogContains("FREED at"),
                  "GpuMemoryRecord report: the fault VA named as inside the freed buffer (kind, size, name, made / freed time and frames)");
            Check(LogContains("freed in the last 10 s: 1 recorded"), "GpuMemoryRecord report: the free listed among the last 10 s");
            const uint64_t heapVa = placed->GetGPUVirtualAddress() - (1u << 20);
            Log("[gpucrash] --- self-test sample: fault inside the heap ---");
            GpuMemoryRecordReport(heapVa + (3u << 20));
            Check(LogContains("heap 4.00 MB (default"), "GpuMemoryRecord report: a fault inside the heap names the heap");
        }
        // between two ranges: fake objects at fixed VAs (WARP's VAs need not rise)
        static int fa, ft, fb;
        GpuMemoryRecordTestAdd(&fa, 0x7E0000000000ull, 1u << 20, true, false);
        GpuMemoryRecordTestAdd(&ft, 0, 512u << 10, false, true);
        GpuMemoryRecordTestAdd(&fb, 0x7E0000200000ull, 1u << 20, true, false);
        Log("[gpucrash] --- self-test sample: fault between two recorded ranges ---");
        GpuMemoryRecordReport(0x7E0000150000ull);
        Check(LogContains("VA 0x7E0000150000 is in no recorded range") && LogContains("nearest recorded range below (ends 0x50000 before the VA)") &&
                  LogContains("nearest recorded range above (starts 0xB0000 after the VA)"),
              "GpuMemoryRecord report: an address in no range gets its nearest neighbours below and above");
        Check(LogContains("the gap there is 0x100000 bytes") && LogContains("committed texture2D 1024x1024"),
              "GpuMemoryRecord report: the freed VA-less texture made between the neighbours is listed as a likely owner");
        if (placed) placed->Release();
        if (reserved) reserved->Release();
        if (heap) heap->Release();
        if (t) t->Release();
        if (a) a->Release();
        g_gmDev = dev;  // kept (recorded) for GpuMemRun2
    }
    if (warp) warp->Release();
    if (factory) factory->Release();
}

// Part 2, after CrashReport's self-test: the GPU crash record (code 0x8000) in OblivionPerfFix-crash.txt, from the WARP device
// (removed) with the freed buffer's VA as the page fault: the paste summary names the reason, the VA, what was there and the
// DRED lists; the full report follows the stack. The file stays in gpudrill\ (the drill report without the game).
static void GpuMemRun2()
{
    printf("\nGpuMemoryRecord: a GPU crash record in OblivionPerfFix-crash.txt (WARP, the freed buffer's VA as the page fault)\n");
    if (!g_gmDev || !g_gmFreedVa) {
        printf("SKIP  GpuMemoryRecord: no WARP device from part 1\n");
        GpuMemoryRecordTestReset();
        return;
    }
    ID3D12Device5* d5 = nullptr;
    if (SUCCEEDED(g_gmDev->QueryInterface(IID_PPV_ARGS(&d5))) && d5) {
        d5->RemoveDevice();
        d5->Release();
    }
    Check(GpuCrashReportTestRecord(g_gmDev, g_gmFreedVa + 0x1230), "GPU crash record: the handler's report read DRED on the removed device");
    CreateDirectoryW(L"gpudrill", nullptr);
    Check(CrashReportTestGpuRecord(L"gpudrill"), "GPU crash record: written to gpudrill\\OblivionPerfFix-crash.txt");
    static char buf[1 << 19];
    buf[0] = 0;
    if (FILE* f = _wfsopen(L"gpudrill\\OblivionPerfFix-crash.txt", L"rb", _SH_DENYNO)) {
        buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
        fclose(f);
    }
    const char* end = strstr(buf, "---- end of summary ----");
    const size_t summary = end ? static_cast<size_t>(end - buf) : 0;
    char va[48];
    snprintf(va, sizeof va, "gpu page fault at VA 0x%llX", static_cast<unsigned long long>(g_gmFreedVa + 0x1230));
    auto inSummary = [&](const char* t) { const char* h = strstr(buf, t); return h && end && h < end; };
    Check(inSummary("what: GPU crash: ") && inSummary("gpu: device removed 0x887A") && inSummary(va), "GPU crash summary: the removed reason and the fault VA in hex");
    Check(inSummary("gpu memory there (GpuMemoryRecord): freed ") && inSummary("\"SelfTest freed buffer\"") && inSummary("committed buffer"),
          "GPU crash summary: what was there (freed N s before, size, kind, name, made by)");
    Check(inSummary("gpu DRED's own lists there:") && inSummary("GPU crash report: device removed 0x887A"), "GPU crash summary: DRED's lists and the decoded ring event");
    Check(inSummary("GPU: ") || inSummary("upscaler / frame generation:"), "GPU crash summary: the GPU and upscaler lines");
    Check(summary > 0 && summary < 4096, "GPU crash summary: within ~4 KB");
    Check(strstr(buf, "GPU crash report (the same lines as in OblivionPerfFix.log):") && strstr(buf, "was inside (offset 0x1230)"),
          "GPU crash record: the full GPU report (the record's look-up) below the summary");
    printf("      (summary %zu bytes; file gpudrill\\OblivionPerfFix-crash.txt; breadcrumbs %s on WARP)\n", summary,
           strstr(buf, "gpu work (DRED)") ? "summarised" : "not available");
    // cost: the bookkeeping alone
    double createNs = 0, missNs = 0, hitNs = 0;
    GpuMemoryRecordTestBench(20000, &createNs, &missNs, &hitNs);
    printf("      (bookkeeping per recorded creation %.0f ns, per final Release of an unrecorded object %.0f ns, of a recorded one %.0f ns)\n",
           createNs, missNs, hitNs);
    Check(createNs < 5000 && missNs < 500, "GpuMemoryRecord: bookkeeping under 5 us per creation and 0.5 us per unrecorded Release");
    GpuMemoryRecordTestReset();
    g_gmDev->Release();
    g_gmDev = nullptr;
}
