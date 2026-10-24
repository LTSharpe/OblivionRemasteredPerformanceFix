// NaniteInitialRootPages self-test (worker tlasfreeze), included by selftest.cpp: the three Nanite streaming-buffer resizes
// in ProcessNewResources and r.Nanite.Streaming.NumInitialRootPages (checked against ProcessNewResources' read) on the mapped
// exe; the value set on both routes, values up to 2048 and a value that is not the game's 2048 left alone, the cap at 8192.
// Lab builds also: NaniteStreamProbe / Nanite*MinMB (the patch, and the hook with a fake resize: the game's size passes
// through unchanged; a minimum raises a smaller size (16-byte aligned) and never lowers a larger one).
#pragma once

namespace nsb {


inline void NaniteSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    (void)game;
    if (!Installed(FindNaniteStreamBuffers(), "nanite: ProcessNewResources' three streaming-buffer resizes found")) return;
    int32_t* const rootPages = FindNaniteRootPagesVar();
    if (!Installed(rootPages && *rootPages == 2048, "nanite: r.Nanite.Streaming.NumInitialRootPages found (2048 in the exe)")) return;
    Check(!SetNaniteInitialRootPages(2048, true) && !SetNaniteInitialRootPages(1000, true) && *rootPages == 2048,
          "nanite: NaniteInitialRootPages up to 2048 leaves the game's 2048");
    Check(SetNaniteInitialRootPages(3072, true) && *rootPages == 3072, "nanite: NaniteInitialRootPages sets 3072 (version.dll route)");
    Check(!SetNaniteInitialRootPages(4096, true) && *rootPages == 3072, "nanite: a value that is not the game's 2048 is left alone");
    *rootPages = 2048;
    Check(SetNaniteInitialRootPages(3072, false) && *rootPages == 3072, "nanite: NaniteInitialRootPages sets 3072 on the OBSE64 route too");
    *rootPages = 2048;
    Check(SetNaniteInitialRootPages(100000, true) && *rootPages == 8192, "nanite: NaniteInitialRootPages is capped at 8192");
    *rootPages = 2048;
}

}  // namespace nsb
