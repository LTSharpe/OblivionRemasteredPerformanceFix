// WaterCaptureFsrSkip / WaterCaptureNoRhiFlush / WaterCaptureNoOcclusionWait / WaterCaptureFastMeshScan self-test (workers
// bridgelake, waterrdg and waterscan; lab builds also the WaterInfoProbe parts), included by selftest.cpp: the sites found
// on the mapped exe (through their single callers;
// WaitOcclusionTests by its unique body), the patches after the start-up install, the hook decisions with fakes for every
// key combination (skip only inside the water-info render, flags cleared after), the EndFlushResourcesRHI and
// WaitOcclusionTests trampolines, the fast mesh scan against the per-proxy test, and a mismatch (no byte written, a log
// line, off).
#pragma once

namespace wtr {

inline void WaterInfoSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    (void)game;
    Check(WaterInfoTestMismatch(), "water info: a mismatching entry or a missing site writes nothing and stays off");
    Check(!InstallWaterCapture(false, true), "water info: refuses to install after game code started");
    if (!Installed(InstallWaterCapture(true, true), "water info: installs before game code (sites found through their callers)")) return;
    Check(WaterInfoTestPatched(), "water info: render-thread entry detoured (lab: and the game-thread one), FSR3 FX slot, EndFlushResourcesRHI, WaitOcclusionTests and FEnableWaterInfoMeshesScoped hooked");
    const char* why = WaterInfoTestHooks();
    Check(why == nullptr, why ? why : "water info: hooks forward outside the water-info render, skip inside it with the keys on, flags cleared after");
    Check(WaterInfoTestTrampoline(), "water info: EndFlushResourcesRHI trampoline runs on a fake builder with +0x780 = 0");
    Check(WaterInfoTestOccTrampoline(), "water info: WaitOcclusionTests trampoline returns at once without an RHI thread");
    const char* scan = WaterInfoTestScan();
    Check(scan == nullptr, scan ? scan : "water info: fast mesh scan equals the per-proxy test on fake sets; FScene getters found; hook paths (fast, stock fallback; lab: check) on a fake scene");
}

}  // namespace wtr
