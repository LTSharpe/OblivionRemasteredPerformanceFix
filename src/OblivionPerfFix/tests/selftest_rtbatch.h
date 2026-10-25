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
