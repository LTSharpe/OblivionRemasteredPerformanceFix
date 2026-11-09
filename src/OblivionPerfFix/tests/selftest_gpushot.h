// GpuMenuShot self-test (worker gpushot; public since 1.3.0, port130k), included by selftest.cpp:
//  - every site is found by pattern on the mapped exe (RHICopyTexture and its barrier / residency calls, its command-list
//    offsets (G-S3), its one context vtable slot, the state-cache flags, UTexture::SetResource, the backdrop handler and its
//    CreateTexture2DFromImage call); on build 19115871 at the known addresses;
//  - the shader's integer arithmetic (gpu_menu_shot.cpp's CPU twin) equals fix 40's conversion + fix 10's 2x2 filter on random
//    pictures of many sizes (edges: 1x1, one row, one column, odd sizes);
//  - the real compute shader on this PC's GPU (D3D12, a few small dispatches) gives the same bytes as the CPU twin, with the
//    game's SDR tables and with random tables (the HDR route uses other tables through the same code). Skipped without D3D12.
#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

namespace gpushot_test {

inline uint32_t Rnd(uint32_t& s) { s = s * 1664525u + 1013904223u; return s ^ (s >> 13); }

inline void SdrTables(std::vector<uint32_t>& lut)
{
    lut.assign(3328, 0);
    for (uint32_t i = 0; i < 256; ++i) lut[3072 + i] = i;  // identity byte table
    for (uint32_t v = 0; v < 1024; ++v) {
        const uint32_t px[3] = {v << 20, v << 10, v};
        uint32_t c[3] = {};
        ConvertR10G10B10A2ToColor(3, 1, reinterpret_cast<const uint8_t*>(px), 12, c);
        lut[v] = c[0] & 0xFF;
        lut[1024 + v] = (c[1] >> 8) & 0xFF;
        lut[2048 + v] = (c[2] >> 16) & 0xFF;
    }
}

// The real shader on a D3D12 device: returns -1 when no device (skipped), else the number of mismatching pictures.
inline int GpuRun(const std::vector<uint32_t>& lutSdr)
{
    HMODULE d3d = LoadLibraryW(L"d3d12.dll");
    if (!d3d) return -1;
    using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(d3d, "D3D12CreateDevice"));
    ID3D12Device* dev = nullptr;
    if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return -1;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    int bad = 0;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        dev->Release();
        return -1;
    }
    size_t blobSize = 0;
    const unsigned char* blob = GpuMenuShotShaderBlob(&blobSize);
    if (FAILED(dev->CreateRootSignature(0, blob, blobSize, IID_PPV_ARGS(&rs)))) { printf("    root signature failed\n"); return 99; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS = {blob, blobSize};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { printf("    pipeline failed\n"); return 99; }
    auto buffer = [&](D3D12_HEAP_TYPE heap, uint64_t bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r));
        return r;
    };
    const int sizes[][2] = {{1, 1}, {1, 7}, {9, 1}, {2, 2}, {3, 5}, {64, 64}, {257, 129}, {333, 77}, {1280, 720}};
    uint32_t seed = 777;
    UINT64 fv = 0;
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<uint32_t> lut = lutSdr;
        if (pass == 1)
            for (auto& v : lut) v = Rnd(seed) & 0xFF;
        if (pass == 2) {  // HdrMenuBackground's byte table (UI white 200 nits) after the SDR tables
            uint8_t t[256];
            HdrMenuBackgroundTable(200.0f, t);
            for (int i = 0; i < 256; ++i) lut[3072 + i] = t[i];
        }
        for (const auto& sz : sizes) {
            const int w = sz[0], h = sz[1];
            const uint32_t pitch = (static_cast<uint32_t>(w) * 4 + 255) & ~255u;
            std::vector<uint32_t> src(static_cast<size_t>(w) * h), ref(src.size());
            for (auto& p : src) p = Rnd(seed);
            GpuMenuShotTestReference(src.data(), w, h, lut.data(), ref.data());
            ID3D12Resource* up = buffer(D3D12_HEAP_TYPE_UPLOAD, 3328 * 4 + static_cast<uint64_t>(pitch) * h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            ID3D12Resource* out = buffer(D3D12_HEAP_TYPE_DEFAULT, static_cast<uint64_t>(pitch) * h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            ID3D12Resource* rb = buffer(D3D12_HEAP_TYPE_READBACK, static_cast<uint64_t>(pitch) * h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!up || !out || !rb) { ++bad; continue; }
            uint8_t* m = nullptr;
            up->Map(0, nullptr, reinterpret_cast<void**>(&m));
            memcpy(m, lut.data(), 3328 * 4);
            for (int y = 0; y < h; ++y) memcpy(m + 3328 * 4 + static_cast<size_t>(y) * pitch, src.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w) * 4);
            up->Unmap(0, nullptr);
            alloc->Reset();
            if (!cl) dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));
            else cl->Reset(alloc, nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = out;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            cl->ResourceBarrier(1, &b);
            const uint32_t consts[4] = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), pitch / 4, pitch / 4};
            cl->SetComputeRootSignature(rs);
            cl->SetPipelineState(pso);
            cl->SetComputeRoot32BitConstants(0, 4, consts, 0);
            cl->SetComputeRootShaderResourceView(1, up->GetGPUVirtualAddress());
            cl->SetComputeRootShaderResourceView(2, up->GetGPUVirtualAddress() + 3328 * 4);
            cl->SetComputeRootUnorderedAccessView(3, out->GetGPUVirtualAddress());
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &b);
            cl->CopyBufferRegion(rb, 0, out, 0, static_cast<uint64_t>(pitch) * h);
            cl->Close();
            ID3D12CommandList* lists[] = {cl};
            q->ExecuteCommandLists(1, lists);
            q->Signal(fence, ++fv);
            while (fence->GetCompletedValue() < fv) Sleep(1);
            uint8_t* r = nullptr;
            rb->Map(0, nullptr, reinterpret_cast<void**>(&r));
            bool same = r != nullptr;
            for (int y = 0; same && y < h; ++y)
                same = memcmp(r + static_cast<size_t>(y) * pitch, ref.data() + static_cast<size_t>(y) * w, static_cast<size_t>(w) * 4) == 0;
            rb->Unmap(0, nullptr);
            if (!same) {
                ++bad;
                printf("    GPU shader differs at %dx%d (%s tables)\n", w, h, pass == 0 ? "SDR" : pass == 1 ? "random" : "SDR + HDR background");
            }
            up->Release();
            out->Release();
            rb->Release();
        }
    }
    if (cl) cl->Release();
    pso->Release();
    rs->Release();
    fence->Release();
    alloc->Release();
    q->Release();
    dev->Release();
    return bad;
}

}  // namespace gpushot_test

static void GpuShotRun(const GameModule& game)
{
    {
        GpuMenuShotSites s{};
        const char* why = GpuMenuShotDiscover(game, &s);
        Check(why == nullptr, "GpuMenuShot: every site found by pattern on the mapped exe (incl. RHICopyTexture's list offsets)");
        if (why) printf("    missing: %s\n", why);
        else Check(*s.copySlot == s.copyTexture && s.createTexCall == s.handler + 0xA4, "GpuMenuShot: the vtable slot holds RHICopyTexture");
        if (!why && game.timeDateStamp == 0xF19077A4u)
            Check(s.copyTexture - game.base == 0x254BF50 && s.handler - game.base == 0x4924EF0 && s.service - game.base == 0x48C2AF0,
                  "GpuMenuShot: build 19115871's RHICopyTexture exe+0x254BF50, handler exe+0x4924EF0, service exe+0x48C2AF0");
        // Leftover save-thumbnail bindings (shot130q): the save's callback (removes itself from the engine delegate the
        // service binds to) and the save function's idle check, by pattern.
        if (!why) {
            Check(s.leftoverWhy == nullptr && s.saveCallback && s.saveFunction,
                  "GpuMenuShot: the save's thumbnail callback (through its event check) and the save function's idle check found");
            if (s.leftoverWhy) printf("    missing: %s\n", s.leftoverWhy);
            if (!s.leftoverWhy && game.timeDateStamp == 0xF19077A4u)
                Check(s.saveCallback - game.base == 0x48AF8A0 && s.saveFunction - game.base == 0x48CBAE0,
                      "GpuMenuShot: build 19115871's save callback exe+0x48AF8A0, save function exe+0x48CBAE0");
        }
    }
    // The leftover rule: held out only when its call would just rewrite the idle save system's holder.
    {
        GpuMenuShotLeftoverView ok{};
        ok.callback = ok.rawExec = ok.holder = ok.saveSystem = ok.eventSlot = ok.eventDone = true;
        ok.state = 4;
        ok.multiplier = 1.0f;
        Check(GpuMenuShotLeftoverRule(ok) == nullptr, "GpuMenuShot leftover: idle save system, event done, no high-res setup -> held out");
        GpuMenuShotLeftoverView v = ok;
        int refused = 0, cases = 0;
        auto expectRefused = [&](GpuMenuShotLeftoverView x) { ++cases; if (GpuMenuShotLeftoverRule(x) != nullptr) ++refused; };
        v = ok; v.callback = false; expectRefused(v);          // another function: the game's way
        v = ok; v.rawExec = false; expectRefused(v);           // another delegate kind
        v = ok; v.holder = false; expectRefused(v);
        v = ok; v.saveSystem = false; expectRefused(v);        // the save system gone
        for (int st : {0, 1, 2, 3, 5, -1}) { v = ok; v.state = st; expectRefused(v); }  // a save in progress
        v = ok; v.eventSlot = false; expectRefused(v);
        v = ok; v.eventDone = false; expectRefused(v);         // its call would trigger a save's thumbnail event
        v = ok; v.handleOnEngine = true; expectRefused(v);     // its Remove would unbind something on the engine delegate
        v = ok; v.resX = 3840; expectRefused(v);               // its call would reset a high-resolution screenshot's setup
        v = ok; v.resY = 2160; expectRefused(v);
        v = ok; v.multiplier = 2.0f; expectRefused(v);
        Check(refused == cases, "GpuMenuShot leftover: refused (the game's way) for a save in progress, a pending event, another "
                                "function or delegate kind, a handle on the engine delegate or a high-res setup");
    }
    // The leftover helpers on fake memory (rev130s nits): the event test, the engine-delegate handle match, the held set's
    // limit and the function-slot swap with its restore.
    {
        alignas(8) uint8_t ev[0x60] = {};
        const bool nullDone = GpuMenuShotEventDone(nullptr);
        const bool clearPending = !GpuMenuShotEventDone(ev);
        *reinterpret_cast<uint64_t*>(ev + 0x58) = uint64_t{1} << 26;
        const bool setDone = GpuMenuShotEventDone(ev);
        *reinterpret_cast<uint64_t*>(ev + 0x58) = ~(uint64_t{1} << 26);
        const bool otherBitsPending = !GpuMenuShotEventDone(ev);
        Check(nullDone && clearPending && setDone && otherBitsPending,
              "GpuMenuShot leftover: event done = no event or bit 26 of +0x58 (the callback's own test)");

        alignas(8) uint8_t a[0x28] = {}, b[0x28] = {}, c[0x28] = {};
        *reinterpret_cast<uint64_t*>(a + 0x10) = 101;
        *reinterpret_cast<uint64_t*>(b + 0x10) = 202;
        *reinterpret_cast<uint64_t*>(c + 0x10) = 303;
        struct Entry { void* inst; int32_t size, pad; };
        Entry list[4] = {{a, 0x28, 0}, {b, 0, 0}, {nullptr, 0x28, 0}, {c, 0x28, 0}};  // b unbound (size 0), one empty slot
        struct Delegate { Entry* data; int32_t num, max; } del{list, 4, 4};
        const auto* const d = reinterpret_cast<const uint8_t*>(&del);
        Check(GpuMenuShotHandleOnDelegate(d, 101) && GpuMenuShotHandleOnDelegate(d, 303) && !GpuMenuShotHandleOnDelegate(d, 202) &&
                  !GpuMenuShotHandleOnDelegate(d, 404) && !GpuMenuShotHandleOnDelegate(d, 0) && !GpuMenuShotHandleOnDelegate(nullptr, 101),
              "GpuMenuShot leftover: a handle on the engine delegate is found only among its bound entries (handle 0 never)");

        alignas(8) static uint8_t insts[GpuMenuShotHeld::kMax + 1][0x28];
        GpuMenuShotHeld h{};
        bool allAdded = true;
        for (int i = 0; i < GpuMenuShotHeld::kMax; ++i) {
            *reinterpret_cast<uintptr_t*>(insts[i] + 0x20) = 0x1000 + i;  // the "function" slot
            allAdded = GpuMenuShotHeldAdd(h, insts[i]) && allAdded;
        }
        const bool full = !GpuMenuShotHeldAdd(h, insts[GpuMenuShotHeld::kMax]) && h.n == GpuMenuShotHeld::kMax;
        Check(allAdded && full, "GpuMenuShot leftover: 16 held at most; a 17th is refused (the takeover then declines, logged once)");
        GpuMenuShotHeldSwap(h, true);
        bool swapped = true;
        const uintptr_t stub = *reinterpret_cast<uintptr_t*>(insts[0] + 0x20);
        for (int i = 0; i < h.n; ++i) swapped = swapped && *reinterpret_cast<uintptr_t*>(insts[i] + 0x20) == stub && stub >= 0x10000;
        GpuMenuShotHeldSwap(h, false);
        bool restored = true;
        for (int i = 0; i < h.n; ++i) restored = restored && *reinterpret_cast<uintptr_t*>(insts[i] + 0x20) == 0x1000u + i;
        GpuMenuShotHeldSwap(h, false);  // a second restore changes nothing
        for (int i = 0; i < h.n; ++i) restored = restored && *reinterpret_cast<uintptr_t*>(insts[i] + 0x20) == 0x1000u + i;
        GpuMenuShotHeld none{};
        GpuMenuShotHeldSwap(none, true);
        GpuMenuShotHeldSwap(none, false);
        Check(swapped && restored, "GpuMenuShot leftover: the held bindings' function slots point to the no-op for the call, then "
                                   "hold their own functions again (a second restore and an empty set change nothing)");
    }
    std::vector<uint32_t> lut;
    gpushot_test::SdrTables(lut);
    // The CPU twin against fix 40's conversion (+ alpha 255, as ProcessScreenShots' loop) and fix 10's filter.
    {
        const int sizes[][2] = {{1, 1}, {1, 5}, {6, 1}, {2, 2}, {3, 3}, {17, 9}, {64, 31}, {255, 254}, {640, 360}};
        uint32_t seed = 4242;
        int bad = 0;
        for (const auto& sz : sizes) {
            const int w = sz[0], h = sz[1];
            std::vector<uint32_t> src(static_cast<size_t>(w) * h), conv(src.size()), a(src.size()), b(src.size());
            for (auto& p : src) p = gpushot_test::Rnd(seed);
            GpuMenuShotTestReference(src.data(), w, h, lut.data(), a.data());
            ConvertR10G10B10A2ToColor(static_cast<uint32_t>(w), static_cast<uint32_t>(h), reinterpret_cast<const uint8_t*>(src.data()),
                                      static_cast<uint32_t>(w) * 4, conv.data());
            for (auto& c : conv) c |= 0xFF000000u;
            BoxFilter2x2(w, h, conv.data(), b.data(), true);
            if (a != b) ++bad;
        }
        Check(bad == 0, "GpuMenuShot: the shader's arithmetic (CPU twin) equals fix 40 + fix 10 on random pictures");
    }
    Check(GpuMenuShotShaderSize() > 64 && GpuMenuShotShaderBlob(nullptr) != nullptr, "GpuMenuShot: the compute shader is compiled in");
    const int gpu = gpushot_test::GpuRun(lut);
    if (gpu < 0) printf("  (GpuMenuShot: no D3D12 device here: the GPU shader check skipped)\n");
    else Check(gpu == 0, "GpuMenuShot: the compute shader on this GPU gives the CPU twin's bytes (SDR, random, HdrMenuBackground tables)");
}
