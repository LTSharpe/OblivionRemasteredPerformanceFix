// RepathParallel self-test, included by selftest.cpp: every function, call target, vtable and layout pin found by pattern on the
// mapped exe (nothing installed; build 19115871's RVAs checked when it is that build), the input key on fake queries, the
// nav-edit counter and the link marker with fake originals (npcs/repath_parallel.cpp).
#pragma once

static void RepathRun(const GameModule& game)
{
    uint32_t r[21] = {};
    const char* why = "";
    const int ok = RepathParallelTestDiscover(game, r, &why);
    Check(ok == 1, "RepathParallel: every function, call target, vtable and layout pin found by pattern on the mapped exe");
    if (ok != 1) printf("    %s\n", why);
    if (ok == 1 && game.timeDateStamp == 0xF19077A4u) {
        static const uint32_t want[21] = {0x41B8970, 0x41CA0C0, 0x41D6490, 0x4175E90, 0x1126290, 0x416B770, 0x416B900, 0x417CED0, 0x41D6240, 0x41976C0,
                                          0xE26500,  0xE32300,  0x2DC0350, 0x41919F0, 0x2D65460, 0x1115A40, 0x41CA4A0, 0x79E7EE0, 0x4186270, 0x4186340, 0x79E4198};
        int bad = 0;
        for (int i = 0; i < 21; ++i)
            if (r[i] != want[i]) { ++bad; printf("    target %d: 0x%X, build 19115871 has 0x%X\n", i, r[i], want[i]); }
        Check(bad == 0, "RepathParallel: the 21 targets are build 19115871's (rev130d's table, the link filter and query filter vtables, the nav-link updates)");
    }
    Check(RepathParallelTestKey(), "RepathParallel: the input key compares every findPath input; the helpers' link filter keys as stock; a filter of another class is never keyed");
    Check(RepathParallelTestHooks(), "RepathParallel: nav-link updates bump the nav counter and call the original; the helpers' isLinkAllowed passes "
                                     "the stock answer and records each link once with its answer; a changing answer or too many links is not servable");
    // repath130l: a search that asked custom links is served only when every link it asked answers the same at serve time.
    Check(RepathParallelTestLinkServe(0) == 1, "RepathParallel: a search whose custom links answer the same at serve time is served");
    Check(RepathParallelTestLinkServe(1) == 1, "RepathParallel: a search one of whose custom links flipped between precompute and serve is declined (stock search)");
    Check(RepathParallelTestLinkServe(2) == 1, "RepathParallel: a flip of a custom link the search never asked does not decline it");
    Check(RepathParallelTestLinkServe(3) == 1, "RepathParallel: a search that asked no custom link is served whatever the links do");
    Check(RepathParallelTestLinkServe(4) == 1, "RepathParallel: a navmesh edit between precompute and serve declines it");
    // RepathEarly (repathearly): the navmesh writers it fences, the early copies, serve / decline, the fence.
    uint32_t er[5] = {};
    const char* ewhy = "";
    const int eok = RepathEarlyTestDiscover(game, er, &ewhy);
    Check(eok == 1, "RepathEarly: dtNavMesh::addTile / removeTile, dtFreeNavMesh (the free of impl +8; its null path a plain ret), "
                    "ReplaceAreaInTileBounds (its area write) found by pattern at function starts, with their detour entries");
    if (eok != 1) printf("    %s\n", ewhy);
    if (eok == 1 && game.timeDateStamp == 0xF19077A4u) {
        static const uint32_t ewant[5] = {0x416E640, 0x4184B40, 0x41743A0, 0x41DB690, 0x41DBB70};
        int bad = 0;
        for (int i = 0; i < 5; ++i)
            if (er[i] != ewant[i]) { ++bad; printf("    early target %d: 0x%X, build 19115871 has 0x%X\n", i, er[i], ewant[i]); }
        Check(bad == 0, "RepathEarly: the 5 targets are build 19115871's (addTile, removeTile, dtFreeNavMesh, ReleaseDetourNavMesh, "
                        "ReplaceAreaInTileBounds)");
    }
    Check(RepathEarlyTestCopies(), "RepathEarly: the filter copy passes through until its own job is stopped (flag after the copy); a custom-link "
                                   "question stops only its own search, without a game call; both copies key as the stock filters");
    Check(RepathEarlyTestServe(0) == 1, "RepathEarly: an early search with every input the same and the navmesh unchanged is served");
    Check(RepathEarlyTestServe(1) == 1, "RepathEarly: a navmesh edit since the early start declines it (stock search)");
    Check(RepathEarlyTestServe(2) == 1, "RepathEarly: another end point declines it");
    Check(RepathEarlyTestServe(3) == 1, "RepathEarly: a search that asked a custom link is never served");
    Check(RepathEarlyTestServe(4) == 1, "RepathEarly: a TickActor not offered the early batch never serves from it");
    Check(RepathEarlyTestFence(0) == 1, "RepathEarly: a writer on another navmesh only moves the nav counter");
    Check(RepathEarlyTestFence(1) == 1, "RepathEarly: a writer on the early batch's navmesh aborts it and cancels its unclaimed searches first");
    Check(RepathEarlyTestFence(2) == 1, "RepathEarly: the fence waits for a claimed search and cancels only the unclaimed ones");
    // repathfix (rev-repathearly S1-S3, N9)
    Check(RepathEarlyTestTick(0) == 1, "RepathEarly: at TickActor an early search no queued path matches is stopped, not waited for; the matched one is");
    Check(RepathEarlyTestTick(1) == 1, "RepathEarly: at TickActor a nav counter that moved stops the whole early batch before the wait");
    Check(RepathEarlyTestTick(2) == 1, "RepathEarly: at TickActor RepathEarly switched off live stops the whole early batch before the wait");
    Check(RepathEarlyTestTick(3) == 1, "RepathEarly: at TickActor RepathParallel switched off live stops the whole early batch before the wait");
    Check(RepathEarlyTestCovers(0) == 1, "RepathEarly: a queued path with a searched early search's inputs is covered (not searched again)");
    Check(RepathEarlyTestCovers(1) == 1, "RepathEarly: another start point is not covered");
    Check(RepathEarlyTestCovers(2) == 1, "RepathEarly: a filter byte changed since the early start is not covered");
    Check(RepathEarlyTestCovers(3) == 1, "RepathEarly: an early search that was not searched covers nothing");
    Check(RepathEarlyTestCovers(4) == 1, "RepathEarly: another filter object with the same bytes is not covered");
    Check(RepathEarlyTestWriters(0) == 1, "RepathEarly: ReplaceAreaInTileBounds moves the nav counter and calls the original with its result");
    Check(RepathEarlyTestWriters(1) == 1, "RepathEarly: an early batch is not started while a navmesh writer runs (no hand-off to the helpers)");
    Check(RepathEarlyTestWriters(2) == 1, "RepathEarly: an early batch with no writer running is handed to the helpers and stays live");
    // RepathDefer (repathdefer): the stock loop's request handling, the tolerant key, take-out / put-back, the kept batch.
    uint32_t drva = 0;
    const char* dwhy = "";
    const int dok = RepathDeferTestDiscover(game, &drva, &dwhy);
    Check(dok == 1, "RepathDefer: TickActor's RepathRequests.Add (ResizeGrow, element {object, controller, reason} of 0x18) and its "
                    "skip of an element with no controller found by pattern");
    if (dok != 1) printf("    %s\n", dwhy);
    if (dok == 1 && game.timeDateStamp == 0xF19077A4u) {
        if (drva != 0xD82700) printf("    ResizeGrow: 0x%X, build 19115871 has 0xD82700\n", drva);
        Check(drva == 0xD82700, "RepathDefer: RepathRequests' ResizeGrow is build 19115871's");
    }
    Check(RepathDeferTestKey(0) == 1, "RepathDefer: a start point 30 uu from the searched one is within the tolerance (50)");
    Check(RepathDeferTestKey(1) == 1, "RepathDefer: a start point 80 uu away is not");
    Check(RepathDeferTestKey(2) == 1, "RepathDefer: an end point 40 uu away is within the tolerance");
    Check(RepathDeferTestKey(3) == 1, "RepathDefer: another start poly is never within the tolerance");
    Check(RepathDeferTestKey(4) == 1, "RepathDefer: another filter byte is never within the tolerance");
    Check(RepathDeferTestKey(5) == 1, "RepathDefer: identical inputs are within the tolerance (moved 0)");
    Check(RepathDeferTestQueue(0) == 1, "RepathDefer: a path queued once is taken out (its element emptied, the queue's size unchanged)");
    Check(RepathDeferTestQueue(1) == 1, "RepathDefer: a path queued twice is left to the stock loop");
    Check(RepathDeferTestQueue(2) == 1, "RepathDefer: requests put back go to the front of the queue in their order");
    Check(RepathDeferTestQueue(3) == 1, "RepathDefer: a full queue grows with the stock Add's steps before the put-back");
    Check(RepathDeferTestResume(0) == 1, "RepathDefer: a request whose search still runs stays out");
    Check(RepathDeferTestResume(1) == 1, "RepathDefer: a finished search's request is put back and offered; the batch ends after the TickActor");
    Check(RepathDeferTestResume(2) == 1, "RepathDefer: a navmesh change since the start stops the search and puts the request back unsearched");
    Check(RepathDeferTestResume(3) == 1, "RepathDefer: at the bound the game thread waits for the running search, then puts it back and offers it");
    Check(RepathDeferTestResume(4) == 1, "RepathDefer: a batch whose navigation data is gone is dropped and its weak reference released");
    Check(RepathDeferTestHandBack(0) == 1, "RepathDefer (S-067): every search ended in the grace and one was stopped by a nav link: the start goes back to RepathParallel (nothing taken out or offered, batch ended)");
    Check(RepathDeferTestHandBack(1) == 1, "RepathDefer (S-067): one search still running: the start is kept (the running one is deferred)");
    Check(RepathDeferTestHandBack(2) == 1, "RepathDefer (S-067): every search ended and searched: the start is kept (offered)");
    Check(RepathDeferTestHandBack(3) == 1, "RepathDefer (S-067): RepathParallel's batch skips a queued path with an offered deferred result, counts the others, skips emptied elements");
    Check(RepathDeferTestBurst(0) == 1, "RepathDefer (S-067): a burst of 5 searches all stopped by nav links in the grace is handed back whole (queue untouched) and RepathParallel's batch is allowed for it");
    Check(RepathDeferTestBurst(1) == 1, "RepathDefer (S-067): a burst of 5 long searches without links still running after the grace is taken out whole (held), RepathParallel stays off");
    Check(RepathDeferTestBurst(2) == 1, "RepathDefer (S-067): 4 link stops + 1 running: the running one is held, the 4 are left to the stock loop and counted");
    Check(RepathDeferTestBurst(3) == 1, "RepathDefer (S-067): 4 link stops + 1 running whose request cannot be taken out: handed back after the take-out loop (queue untouched, counts taken back, batch collected)");
    Check(RepathDeferTestBeside() == 1, "RepathDefer (S-067): RepathParallel's batch beside an offered deferred result: the deferred batch collected first, the offered path not searched again, its call served from the offer and the others from the batch, each once");
    // CapSleepSkip (repathdefer): the GetMaxTickRate call it patches, its rate hook and its decision.
    Check(CapSleepSkipTestSite(game), "CapSleepSkip: UpdateTimeAndHandleMaxTickRate's GetMaxTickRate call is `call [rax+slot]` with NpcCreationPacing's slot");
    Check(CapSleepSkipTest(0) == 1, "CapSleepSkip: with no cut the engine's rate passes through");
    Check(CapSleepSkipTest(1) == 1, "CapSleepSkip: a cut of 8 ms at 60 fps gives the rate of an 8.67 ms period, for one call only, and the 8 ms go back into the frame's delta time");
    Check(CapSleepSkipTest(2) == 1, "CapSleepSkip: a cut longer than the period gives 0 (no wait), for one call only, and the whole wait goes back into the delta time");
    Check(CapSleepSkipTest(3) == 1, "CapSleepSkip: a GC pass due after 8 ms of work at 60 fps shortens the wait by the bound (8 ms), counted; the predictor is asked with the cap period less 0.2 ms");
    Check(CapSleepSkipTest(4) == 1, "CapSleepSkip: not again within 4 frames");
    Check(CapSleepSkipTest(5) == 1, "CapSleepSkip: nothing due, no cut");
    Check(CapSleepSkipTest(6) == 1, "CapSleepSkip: a frame that left under 1 ms of wait is not cut");
    Check(CapSleepSkipTest(7) == 1, "CapSleepSkip: lab CapSleepSkipAddBack=0 cuts the wait and gives nothing back to the delta time, counted");
}
