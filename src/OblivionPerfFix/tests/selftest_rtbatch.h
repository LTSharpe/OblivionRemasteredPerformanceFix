// RayTracingBuildBatch self-test, included by selftest.cpp: CreateRayTracingGeometry's build request (exe+0x266FD26 ->
// exe+0x26C47C0, build 19115871) goes to the gate, whose trampoline region also holds the request function's queue path
// (its prologue, then a jump past its "build at once" test); FDeferredShadingSceneRenderer::Render's vtable slot
// (exe+0x70EEB50) holds the wrapper that builds the queued requests first. Then the decision: only InitRHI's Normal build
// requests of geometries nobody updates in place, on the render thread outside a render, with the game's budget (<= 0),
// are queued. Run last: the patched call must not disturb another fix's pattern search. First the refused path: with one
// byte of RequestBuildAccelerationStructure changed in this mapped copy the installation stops at its checks, writes
// nothing and logs one line.
#pragma once

namespace rtb {

inline void RtBatchSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    Check(!InstallRayTracingBuildBatch(false), "ray tracing build batch: refuses to install after game code started");
    uint8_t* const call = game.base + 0x266FD26;      // CreateRayTracingGeometry's build request call
    uint8_t* const request = game.base + 0x26C47C0;   // RequestBuildAccelerationStructure
    void** const slot = reinterpret_cast<void**>(game.base + 0x70EEB50);   // FDeferredShadingSceneRenderer::Render's slot
    uint8_t callBefore[5];
    memcpy(callBefore, call, sizeof callBefore);
    void* const slotBefore = *slot;
    const uint8_t first[1] = {request[0]}, changed[1] = {static_cast<uint8_t>(request[0] ^ 0x01)};
    const bool setUp = WriteCode(request, changed);
    Check(setUp, "ray tracing build batch: test set-up, one byte of RequestBuildAccelerationStructure changed");
    if (setUp) {
        Check(!InstallRayTracingBuildBatch(true) && memcmp(call, callBefore, sizeof callBefore) == 0 && *slot == slotBefore &&
                  LogContains("[rtbatch] unexpected RequestBuildAccelerationStructure: RayTracingBuildBatch not installed"),
              "ray tracing build batch: a changed RequestBuildAccelerationStructure: not installed, nothing written, one log line");
        WriteCode(request, first);
    }
    if (Installed(InstallRayTracingBuildBatch(true), "RayTracingBuildBatch (new meshes' ray-tracing builds as one batch)")) {
        RayTracingBuildBatchInfoOut rtb;
        if (Installed(RayTracingBuildBatchInfo(&rtb), "ray tracing build batch: info")) {
            Check(rtb.callSite == call && rtb.request == request && rtb.slot == slot && rtb.render == slotBefore,
                  "ray tracing build batch: found at exe+0x266FD26 (the request call), exe+0x26C47C0 and Render's slot exe+0x70EEB50");
            Check(rtb.immediate == game.base + 0x9206608,
                  "ray tracing build batch: the immediate command list exe+0x9206608 (the \"build at once\" path's, for the early batches)");
            int32_t rc;
            memcpy(&rc, rtb.callSite + 1, 4);
            Check(rtb.callSite[0] == 0xE8 && rtb.callSite + 5 + rc == rtb.region, "ray tracing build batch: the build request goes to the gate");
            const uint8_t queue[] = {0x40, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF1, 0xFF, 0x25, 0, 0, 0, 0};
            uint8_t* resume = nullptr;
            memcpy(&resume, rtb.region + 31, 8);
            Check(rtb.region[0] == 0xFF && rtb.region[1] == 0x25 && memcmp(rtb.region + 16, queue, sizeof queue) == 0 &&
                      resume == rtb.request + 0x1F,
                  "ray tracing build batch: the queue path is the request function's prologue, then its code after the \"build at once\" test");
            Check(*rtb.slot == rtb.hook && rtb.render != rtb.hook, "ray tracing build batch: Render's vtable slot holds the wrapper");
        }
    }
    // The queue as the manager keeps it (RequestBuild's queue path): entries of 0x18 bytes with the geometry at +8,
    // allocation bits inline at +0x38 or behind the pointer at +0x48, count +0x30, free count +0x5C; a geometry's RHI
    // geometry (+0x90) holds its build scratch size (+0x18)
    {
        alignas(16) static uint8_t rhi[3][0x20]{}, geo[3][0xA0]{}, entries[3 * 0x18]{}, manager[0x60]{};
        const uint64_t sizes[3] = {10, 20, 40};
        for (int i = 0; i < 3; ++i) {
            const uint8_t* r = rhi[i];
            const uint8_t* g = geo[i];
            memcpy(rhi[i] + 0x18, &sizes[i], 8);
            memcpy(geo[i] + 0x90, &r, 8);
            memcpy(entries + i * 0x18 + 8, &g, 8);
        }
        const uint8_t* e = entries;
        const int32_t num = 3, oneFree = 1, allFree = 3;
        const uint32_t inlineBits = 0x5;   // entries 0 and 2 queued, 1 free
        static uint32_t heapBits = 0x2;    // only entry 1
        const uint32_t* hb = &heapBits;
        memcpy(manager + 0x28, &e, 8);
        memcpy(manager + 0x30, &num, 4);
        memcpy(manager + 0x38, &inlineBits, 4);
        memcpy(manager + 0x5C, &oneFree, 4);
        int32_t n = -1;
        uint64_t s = 0;
        Check(RayTracingBuildBatchQueueForTest(manager, &n, &s) && n == 2 && s == 50,
              "ray tracing build batch: the engine's queue counted from its allocation bits (inline), the queued builds' scratch summed");
        memcpy(manager + 0x48, &hb, 8);
        Check(RayTracingBuildBatchQueueForTest(manager, &n, &s) && n == 1 && s == 20,
              "ray tracing build batch: the engine's queue with its allocation bits on the heap");
        memcpy(manager + 0x5C, &allFree, 4);
        Check(RayTracingBuildBatchQueueForTest(manager, &n, &s) && n == 0 && s == 0,
              "ray tracing build batch: an empty queue (every request dropped) counts nothing");
    }
    Check(!RayTracingBuildBatchFlushFirst(60ull << 20, 5, 3ull << 20) && RayTracingBuildBatchFlushFirst(62ull << 20, 5, 3ull << 20) &&
              !RayTracingBuildBatchFlushFirst(100ull << 20, 0, 1),
          "ray tracing build batch: a request that would take the queue past 64 MiB of scratch builds it first; an empty queue never");
    Check(RayTracingBuildBatchDecide(true, 2, 0, true, true, false, -1, 0, 0),
          "ray tracing build batch: InitRHI's Normal build of a static geometry on the render thread is queued");
    Check(!RayTracingBuildBatchDecide(false, 2, 0, true, true, false, -1, 0, 0), "ray tracing build batch: off builds at once");
    Check(!RayTracingBuildBatchDecide(true, 2, 0, false, true, false, -1, 0, 0) &&
              !RayTracingBuildBatchDecide(true, 0, 0, true, true, false, -1, 0, 0) &&
              !RayTracingBuildBatchDecide(true, 2, 1, true, true, false, -1, 0, 0),
          "ray tracing build batch: other callers, priority Immediate and update builds go to the game's function");
    Check(!RayTracingBuildBatchDecide(true, 2, 0, true, false, false, -1, 0, 0) &&
              !RayTracingBuildBatchDecide(true, 2, 0, true, true, true, -1, 0, 0),
          "ray tracing build batch: other threads and requests during a render build at once");
    Check(!RayTracingBuildBatchDecide(true, 2, 0, true, true, false, -1, 1, 0) &&
              !RayTracingBuildBatchDecide(true, 2, 0, true, true, false, -1, 0, 1),
          "ray tracing build batch: fast-build or updatable geometries build at once");
    Check(!RayTracingBuildBatchDecide(true, 2, 0, true, true, false, 50000, 0, 0),
          "ray tracing build batch: with a build budget set (> 0) the game queues by itself");
}

}  // namespace rtb
