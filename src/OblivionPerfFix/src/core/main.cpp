// Entry points. The same DLL works two ways:
//  - as version.dll next to the game exe (a proxy: exports forward to System32\version.dll),
//    initialised from DllMain before the game's code runs;
//  - as an OBSE64 plugin (OBSE\Plugins\OblivionPerfFix.dll), initialised from OBSEPlugin_Load.
// If both are installed, only the first one to load applies the fixes.
#include "common.h"

#include <atomic>
#include <cwchar>
#include <string>

namespace opf {
namespace {

HMODULE g_self = nullptr;

std::wstring ModulePath(HMODULE module)
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring FileName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Directory(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring g_iniPath;
std::atomic<bool> g_frozenClothHooked{false};  // FrozenClothStretchFix installed at startup
std::atomic<bool> g_fadeInHooked{false};       // NpcFadeInNoRebuild installed at startup
std::atomic<bool> g_destroyBatch{false};       // PhysicsDestroyBatch installed at startup
std::atomic<bool> g_collisionHooked{false};  // PrepareCollisionBehindLoadingScreen found and its fade-to-game hook installed at startup
std::atomic<bool> g_displayHooked{false};    // DisplayModeCache installed
std::atomic<int> g_displayRetries{0};        // DisplayModeCache: the import slot was not filled yet at startup: tries left
std::atomic<bool> g_resizeHooked{false};     // MenuScreenshotFilter installed
std::atomic<bool> g_frameFenceTried{false};
std::atomic<bool> g_viewGuardHooked{false};  // ViewRenameGuard installed at start-up  // RdgParallelTranslate's frame fence (RdgFrameFence) looked for while the game runs
std::atomic<bool> g_focusHooked{false};      // RestoreFsrAfterAltTab installed
std::atomic<bool> g_lumenHooked{false};      // LumenKeepArrayCapacity installed
std::atomic<bool> g_poolKeepHooked{false};   // GpuPoolKeepMB installed
std::atomic<bool> g_uploadHooked{false};     // UploadMemoryKeep installed
std::atomic<bool> g_chaosFound{false};       // PhysicsTreeTimeSlicing's console variable found
std::atomic<bool> g_pacingHooked{false};     // NpcCreationPacing installed
std::atomic<bool> g_clothRegHooked{false};   // ClothRegisterLite's hooks installed (lab: also ClothRegisterSkip's)
std::atomic<bool> g_npcBodies{false};        // NpcBodiesOneLock installed
std::atomic<bool> g_rigSetup{false};         // RigVmHashMemo / RigLogicMapCache installed
std::atomic<bool> g_bodiesHooked{false};     // ParallelStaticBodies installed
std::atomic<bool> g_sceneAddHooked{false};   // PipelinedSceneAdd installed
std::atomic<bool> g_navmeshFound{false};     // NavmeshBuildNoBusyWait's site found
std::atomic<bool> g_busyWaitFound{false};    // BusyWaitFlagCheck's sites found
std::atomic<bool> g_creatureAnimFound{false}; // CreatureAnimPreload's flag byte found
std::atomic<bool> g_flushHooked{false};      // PhysicsFlushParallel installed
std::atomic<bool> g_flushTried{false};       // PhysicsFlushParallel looked for while the game runs (switched on after start-up)
std::atomic<bool> g_clearHooked{false};      // InstanceClearReserve installed
std::atomic<bool> g_gpuSceneHooked{false};   // GpuSceneKeepCapacity installed
std::atomic<bool> g_allocTrimHooked{false};  // CommandAllocatorTrim installed
std::atomic<bool> g_navLinkHooked{false};    // NavLinkFast installed
std::atomic<bool> g_lumenNarrowHooked{false}; // LumenCardsNarrow installed
std::atomic<bool> g_lumenScanHooked{false};   // LumenScanCull installed
std::atomic<bool> g_saveBytesHooked{false};  // SaveBytesBulk / SaveThumbnailAsync (lab: and SaveStallTimers) installed
std::atomic<bool> g_saveRefIdHooked{false};  // SaveRefIdIndex installed
std::atomic<bool> g_physAnimHooked{false};   // PhysAnimNoEmptyLock installed (start-up)

// Re-reads the ini every second so AtmosphereDirtyFilter, AtmosphereLightFastPath, AtmosphereLightCloudPath, RdgParallelTranslate,
// RdgSubmitBatch, ClothConfigCache, FrozenClothStretchFix, NpcFadeInNoRebuild, PrepareCollisionBehindLoadingScreen, NameLookupCrashGuard, TrapCrashGuard, HavokPickWarningGuard,
// FrameGenCrashGuard, FrameGenCameraInfo, DisplayModeCache,
// MenuScreenshotFilter, RestoreFsrAfterAltTab, LumenKeepArrayCapacity, UploadMemoryKeep, PhysicsTreeTimeSlicing (with its
// hidden PhysicsSliceIterations, PhysicsSliceCopyKB, PhysicsSliceAdaptive), PhysicsFlushParallel (hidden PhysicsFlushMinChanges), InstanceClearReserve and
// NpcCreationPacing (with its hidden PacingBudgetMs, PacingMinDistance, PacingMaxLagFrames, PacingUseSlack, PacingFlowWait,
// PacingFlowWaitFrames),
// NavmeshBuildNoBusyWait, GpuSceneKeepCapacity and MemoryRelief
// can be switched while the game runs, samples memory for MemoryRelief and writes the periodic statistics.
// RdgParallelTranslate's switch: 1 on, 0 off, 2 automatic: on unless the PC's biggest GPU has less than 9.5 GB of video memory. On such
// cards (8 GB) the game is over its video memory budget at 1080p/1440p High even without the fix, and parallel
// translation's command memory (0.4-1 GB) pushed it further over while its CPU gain did not show (2026-10-12, an
// ~8.2 GB budget: the road flight's frames over 50 ms 13-15 with it, 5-6 without; Rumare 37 vs 46 fps at 1440p).
// ViewRenameGuard's mode from the ini: the public key is 0 / 1; the lab build also takes 2 (count only) and 3 (repair all).
int ViewGuardMode(const Config& cfg)
{
    return cfg.viewRenameGuard != 0 ? 1 : 0;
}

// RdgParallelTranslate's frame fence lets buffer defragmentation run alongside parallel translate only while ViewRenameGuard closes
// the view-creation race that defragmentation brings (otherwise the defragmentation stays paused, as before). The lab
// build may use it without the guard (tests).
bool FrameFenceWanted(const Config& cfg)
{
    return cfg.rdgFrameFence && ViewRenameGuardRepairing();
}

// The helper-thread fixes measured on 16 logical processors only: off below ParallelMinProcessors usable ones (the
// process affinity counts, so Process Lasso or CCD parking lower it).
bool ParallelHelpersAllowed(const Config& cfg) { return UsableLogicalProcessors() >= static_cast<unsigned>(cfg.parallelMinProcessors); }

// The automatic mode is also off where SharedMemoryGpu applies (relief.shared). Only where it could apply (automatic, Windows
// itself, under 20 GB of RAM) it waits for the GPU's answer, at most a minute from start-up; elsewhere (PCs with 20 GB
// and more, Wine) nothing waits.
bool TranslateWanted(const Config& cfg, bool sharedGpu, unsigned seconds)
{
    if (cfg.rdgParallelTranslate != 2) return cfg.rdgParallelTranslate != 0;
    if (sharedGpu) {
        static bool loggedShared = false;
        if (!loggedShared) Log("[rdg] parallel translate automatic: off (the GPU uses system RAM: SharedMemoryGpu)");
        loggedShared = true;
        return false;
    }
    if (SharedMemoryGpuWaitForAnswer(cfg.sharedMemoryGpu) && seconds < 60) return false;
    const char* name = "";
    uint64_t mb = LargestGpuMemoryMB(&name);
    static int logged = -1;
    const unsigned processors = UsableLogicalProcessors();
    const bool on = processors >= 8 && (mb == 0 || mb >= 9728);  // unknown: on
    if (logged != static_cast<int>(on)) {
        logged = on;
        Log("[rdg] parallel translate automatic: %s, %.1f GB of video memory, %u usable logical processors (needs 8): %s",
            mb ? name : "no GPU found", mb / 1024.0, processors,
            on ? "on" : "off (needs 8 logical processors and 9.5 GB video memory when known; RdgParallelTranslate=1 forces it on)");
    }
    return on;
}

// LumenGroupsPrior's commit margin: twice MemoryReliefCommitMB (where MemoryRelief gives memory back again), kept within
// 1-64 GB so an odd hidden value can neither overflow nor take the margin away.
int LumenPriorSpareMB(const Config& cfg)
{
    const int mb = cfg.memoryReliefCommitMB < 512 ? 512 : cfg.memoryReliefCommitMB > 32768 ? 32768 : cfg.memoryReliefCommitMB;
    return 2 * mb;
}

DWORD WINAPI MonitorThread(LPVOID)
{
    for (unsigned seconds = 1;; ++seconds) {
        Sleep(1000);
        const Config cfg = LoadConfig(g_iniPath.c_str());
        // MemoryRelief: while Windows is short of memory, the fixes that keep memory on purpose give it back.
        MemoryRelief relief;
        UpdateMemory(cfg, &relief);
        {
            // GPU crash report extras: pass markers once the game's device exists (lab: video memory per process)
            uint64_t vram = 0, budget = 0;
            const bool deviceReady = LatestGameVram(&vram, &budget);
            GpuPassMarkersTick(cfg.gpuPassMarkers, deviceReady);
        }
        SetAtmosphereDirtyFilterEnabled(cfg.atmosphereDirtyFilter);
        SetAtmosphereLightFastPathEnabled(cfg.atmosphereLightFastPath);
        SetAtmosphereLightCloudPathEnabled(cfg.atmosphereLightCloudPath);
        if (g_viewGuardHooked) SetViewRenameGuardMode(ViewGuardMode(cfg));
        if (FrameFenceWanted(cfg) && !FrameFenceAvailable() && !g_frameFenceTried.exchange(true) && FindFrameFence())
            InstallFrameFence(false);  // switched on while the game runs (the detour is a single atomic write)
        SetRdgUseFrameFence(FrameFenceWanted(cfg));
        SetRdgParallelTranslateEnabled(TranslateWanted(cfg, relief.shared, seconds) && !relief.translate);
        // Submissions are only batched while parallel translate produces many of them.
        SetRdgSubmitBatch(RdgParallelTranslateActive() ? cfg.rdgSubmitBatch : 0, cfg.rdgSubmitMaxAgeUs);
        CheckRdgSubmitBatching();
        SetClothConfigCacheEnabled(cfg.clothConfigCache);
        if (g_frozenClothHooked) SetFrozenClothStretchFix(cfg.frozenClothStretchFix);
        if (g_fadeInHooked) SetNpcFadeInNoRebuild(cfg.npcFadeInNoRebuild);
        if (g_collisionHooked) SetCollisionPrepareEnabled(cfg.prepareCollisionBehindLoadingScreen);
        SetEditorIdLookupGuardEnabled(cfg.editorIdLookupGuard);
        SetTrapTargetGuardEnabled(cfg.trapCrashGuard);
        SetBowSneakAttackFixEnabled(cfg.bowSneakAttackFix);  // BowSneakAttackFix: no-op unless installed at start-up
        SetStuckEffectLookFix(cfg.stuckEffectLookFix);  // StuckEffectLookFix: no-op unless installed at start-up
        SetWorldSoundPositionFix(cfg.worldSoundPositionFix);  // WorldSoundPositionFix: no-op unless installed at start-up
        SetArrowHitSoundFixEnabled(cfg.arrowHitSoundFix);  // ArrowHitSoundFix: no-op unless installed at start-up
        SetAutosaveFollowsMenuEnabled(cfg.autosaveFollowsMenu);  // AutosaveFollowsMenu: no-op unless installed at start-up
        SetSaveThumbLeftoverFix(cfg.saveThumbLeftoverFix);  // SaveThumbLeftoverFix: no-op unless installed at start-up
        SetHavokPickWarningGuardEnabled(cfg.havokPickWarningGuard);
        SetFrameGenCrashGuardEnabled(cfg.frameGenCrashGuard);
        SetObjectReplaceGuardMode(cfg.objectReplaceGuard);
        SetTaskWaitGuardMode(cfg.taskWaitDeadlockGuard);  // no-op unless installed at start-up
        PsoCompileThreadPoolTick(cfg.psoCompileThreadPool, cfg.psoCompileThreadPoolPriority);  // start-up only: later changes are only logged
        IndirectArgBufferPoolTick(cfg.indirectArgBufferPool);  // start-up only: later changes are only logged
        SetParallelFileReads(cfg.parallelFileReads);  // ParallelFileReads: live once installed at start-up
        TaskWaitGuardTick();
        SetFrameGenCameraInfoEnabled(cfg.frameGenCameraInfo);
        SetFrameGenPacingTuning(cfg.frameGenPacingTuning);
        if (g_displayRetries > 0) {
            const int r = ApplyDisplayModeCache(true);
            if (r > 0) g_displayHooked = true;
            if (r >= 0 || --g_displayRetries == 0) g_displayRetries = 0;
        }
        if (g_displayHooked) SetDisplayModeCacheEnabled(cfg.displayModeCache);
        if (g_resizeHooked) SetImageResizeFixEnabled(cfg.screenshotFilterFast);
        SetVramCounterFix(cfg.vramCounterFix);                  // VramCounterFix: no-op unless installed
        SetReadbackConvertMode(cfg.screenshotReadbackConvert);  // ScreenshotReadbackConvert: no-op unless installed at start-up
        SetScreenshotParallel(cfg.screenshotParallel);          // ScreenshotParallel: no-op unless installed at start-up
        if (g_focusHooked) SetFocusRestoreEnabled(cfg.restoreFsrAfterAltTab);
        SetHdrMenuBackground(cfg.hdrMenuBackground);            // HdrMenuBackground: no-op unless installed
        SetHdrSaveThumbnail(cfg.hdrSaveThumbnail);              // HdrSaveThumbnail: no-op unless installed
        SetMenuOpenFlashFix(cfg.menuOpenFlashFix);  // MenuOpenFlashFix: no-op unless installed at start-up
        if (g_physAnimHooked) SetPhysAnimNoEmptyLock(cfg.physAnimNoEmptyLock);
        SetRigTraceReuse(cfg.rigTraceReuse);  // RigTraceReuse: no-op unless installed at start-up
        SetRigImportTemplate(cfg.rigImportTemplate);  // RigImportTemplate: no-op unless installed at start-up
        SetPerceptionLosPrefetch(cfg.perceptionLosPrefetch, cfg.perceptionLosPrefetchMin, ParallelHelpersAllowed(cfg), cfg.perceptionLosPrefetchOverlap, cfg.perceptionLosIgnoreCache, cfg.perceptionLosCrossRound);  // no-op unless installed
        SetFarPawnMove(cfg.farPawnMoveHalfRate, cfg.farPawnMoveHz, cfg.farHorseMove);  // FarPawnMove: no effect unless installed
        SetCreatePreload(cfg.createPreload);          // CreatePreload: no effect unless installed at start-up
        SetCreatePreloadNear(cfg.createPreloadNear);
        SetCreatePreloadEarly(cfg.createPreloadEarly);  // CreatePreloadEarly: live
        SetImpactPreload(cfg.impactPreload);          // ImpactPreload: no effect unless installed at start-up
        SetRowPreload(cfg.rowPreload);                // RowPreload: no effect unless installed at start-up
        SetCreateAhead(cfg.createAhead, cfg.createAheadSlackMs);  // CreateAhead: no effect unless installed at start-up
        SetCreateAheadPool(cfg.createAheadPool);      // CreateAheadPool: live
        SetCreateAheadLevelled(cfg.createAheadLevelled);  // CreateAheadLevelled: live
        SetCellObjectPreload(cfg.cellObjectPreload);  // CellObjectPreload: no effect unless installed at start-up
        SetOtFrameTrim(cfg.otFrameTrim);              // OtFrameTrim: no effect unless installed at start-up
        SetLooseMissCache(cfg.looseMissCache);        // LooseMissCache: no effect unless installed at start-up
        SetPowerAttackAnimKeep(cfg.powerAttackAnimKeep);  // PowerAttackAnimKeep: no effect unless installed at start-up
        SetFlowOverlapsOnce(cfg.flowOverlapsOnce);    // FlowOverlapsOnce: no effect unless installed at start-up
        if (g_lumenHooked) SetLumenKeepCapacityEnabled(cfg.lumenKeepArrayCapacity && !relief.lumen);
        if (g_lumenHooked) SetLumenGroupsPrior(cfg.lumenGroupsPrior, LumenPriorSpareMB(cfg));  // also logs its decisions
        if (g_uploadHooked)
            SetUploadMemoryKeep(cfg.uploadMemoryKeep && !relief.upload, cfg.uploadMemoryKeepFrames, cfg.uploadPagesToKeep,
                               cfg.uploadReleaseRampFrames);
        if ((cfg.physicsFlushParallel || cfg.physicsTreeTimeSlicing) && !g_flushHooked && !g_flushTried.exchange(true))
            g_flushHooked = InstallPhysicsFlush(false);  // switched on while the game runs (each detour is one atomic write)
        if (g_chaosFound)
            SetChaosTimeSlicing(cfg.physicsTreeTimeSlicing, cfg.physicsSliceIterations, cfg.physicsSliceCopyKB, cfg.physicsSliceAdaptive);
        if (g_flushHooked) SetPhysicsFlushParallel(cfg.physicsFlushParallel, cfg.physicsFlushMinChanges);
        if (g_clearHooked) SetInstanceClearReserve(cfg.instanceClearReserve);
        if (g_pacingHooked)
            SetNpcPacing(cfg.npcCreationPacing, cfg.pacingBudgetMs, cfg.pacingMinDistance, cfg.pacingMaxLagFrames, cfg.pacingUseSlack,
                         cfg.pacingFlowWait, cfg.pacingFlowWaitFrames);
        if (g_pacingHooked) SetNpcPacingFadeAware(cfg.pacingFadeAware);
        if (g_pacingHooked) SetNpcPacingOwnerWalk(cfg.pacingOwnerWalk);  // PacingOwnerWalk (mkusm, PR #25): live
        if (g_pacingHooked) SetNpcPacingHairActor(cfg.pacingHairActor);  // PacingHairActor: live
        if (g_pacingHooked) SetNpcPacingLeanFlowDrain(cfg.pacingLeanFlowDrain);  // PacingLeanFlowDrain: live
        if (g_pacingHooked) SetNpcPacingFlowProgress(cfg.pacingFlowProgress);  // PacingFlowProgress: live
        if (g_pacingHooked) SetNpcPacingGateBounds(cfg.pacingGateBoundFrames, cfg.pacingGateCalmMs, cfg.pacingGateNearM, cfg.pacingGateFadeHoldMs);
        if (g_pacingHooked) SetNpcPacingBaseGate(cfg.pacingBaseGate);  // PacingBaseGate: live
        if (g_pacingHooked) SetFadeInUnseen(cfg.fadeInUnseen, cfg.fadeInUnseenWindowMs);  // FadeInUnseen: live
        if (g_pacingHooked) SetShownHold(cfg.pacingGateShownHoldMs, cfg.pacingGateShownHoldEnd);  // PacingGateShownHoldMs/End: live (0 when not installed)
        SetHitchLedger(cfg.hitchLedger, cfg.hitchLedgerMs);  // diagnostic: no-op unless installed at start-up
        SetTickSplit(cfg.hitchLedger && cfg.hitchLedgerTickSplit);  // diagnostic: no-op unless installed at start-up
        SetDoorDiagEarlyStream(cfg.doorDiagEarlyStream);     // diagnostic DoorDiag's experiment (no-op unless installed)
        SetFadeVtFlushSkip(cfg.fadeVtFlushSkip);             // FadeVtFlushSkip (no-op unless installed)
        SetStreamBehindLoading(cfg.streamBehindLoading);     // StreamBehindLoadingScreen: no-op unless installed at start-up
        SetUnfreezeBehindLoading(cfg.unfreezeBehindLoading); // prototype UnfreezeBehindLoadingScreen (trade-off; 0 = off)
        SetUnfreezeBehindLoadingFastTravel(cfg.unfreezeBehindLoadingFastTravel);  // ... its own hold after a fast travel (0 = the same)
        SetDrawBehindLoading(cfg.drawBehindLoading);  // DrawBehindLoadingScreen (loadreveal; public 1.3.0): live
        SetMapPopupSpread(cfg.mapPopupSpread);        // MapPopupSpread (mapspread; public 1.3.0): live once installed
        SetGpuMenuShot(cfg.gpuMenuShot, cfg.hdrMenuBackground);  // GpuMenuShot (gpushot; public 1.3.0): live once installed
        HitchLedgerDrain();                                  // ... its slow-frame records into OblivionPerfFix-ledger.csv
        if (g_clothRegHooked) SetClothRegisterLite(cfg.clothRegisterLite);  // ClothRegisterLite: live once installed
        SetClothBuildDefer(cfg.clothBuildDefer, cfg.clothDeferQuietFrames);  // ClothBuildDefer: live once installed (no-op otherwise)
        if (g_bodiesHooked) SetParallelStaticBodies(ParallelHelpersAllowed(cfg) && cfg.parallelStaticBodies, cfg.parallelStaticBodiesMin);
        SetHelperThreadCaches(cfg.helperThreadCaches);  // no-op when the set-up was not found
        if (g_sceneAddHooked) SetPipelinedSceneAdd(ParallelHelpersAllowed(cfg) && cfg.pipelinedSceneAdd, cfg.pipelinedSceneAddMin);
        SetPipelinedBodyCreation(ParallelHelpersAllowed(cfg) && cfg.pipelinedBodyCreation);  // PipelinedBodyCreation: no-op unless found with ParallelStaticBodies installed
        SetGpuSceneParallelUpload(cfg.gpuSceneParallelUpload);  // GpuSceneParallelUpload: a console variable, set once registered
        SetSeparableSkinAbove4K(cfg.separableSkinAbove4K);    // SeparableSkinAbove4K: r.SSS.Burley.Quality 0 while wider than 4K (game bug fix)
        SetVtStreamingUploads(cfg.vtStreamingUploads);          // VtStreamingUploads: a console variable (0 = the game's)
        SetDirtyTreeQuickInsert(cfg.dirtyTreeQuickInsert);  // no-op unless installed at start-up
        SetBulkIndexInsert(cfg.bulkIndexInsert, cfg.bulkIndexInsertMin, cfg.physicsInternalFlushParallel);  // BulkIndexInsert: no-op unless installed
        SetPhysicsQueueShrinkOnce(cfg.physicsQueueShrinkOnce);  // PhysicsQueueShrinkOnce: no-op unless installed with BulkIndexInsert
        if (g_destroyBatch) SetPhysicsDestroyBatch(cfg.physicsDestroyBatch, cfg.physicsDestroyBatchMin);  // PhysicsDestroyBatch: live once installed
        SetBodyRemovalSpread(cfg.bodyRemovalSpread, cfg.bodyRemovalSpreadPerFrame, cfg.bodyRemovalSpreadMaxFrames,
                             cfg.bodyRemovalSpreadMin, cfg.bodyRemovalSpreadNearM);  // BodyRemovalSpread: no-op unless installed
        SetStreamOutEndPlaySlice(cfg.streamOutEndPlaySlice);  // StreamOutEndPlaySlice: no-op unless installed at start-up
        SetResetLoadersNoFlush(cfg.resetLoadersNoFlush);      // ResetLoadersNoFlush: no-op unless installed at start-up
        SetStartupBarFinish(cfg.startupBarFinish);            // StartupBarFinish: no-op unless installed at start-up
        SetTopicSort(cfg.topicSort);                          // TopicSort: no-op unless installed at start-up
        SetGcMarkPriority(cfg.gcMarkPriority);                // GcMarkPriority (live once installed)
        SetGcPassAfterBusy(cfg.gcPassAfterBusy, relief.osCache);  // GcPassAfterBusy (live once installed; RAM/commit short = never hold)
        SetGcPrepAhead(cfg.gcPrepAhead, cfg.gcPrepAheadMinAgeMs, cfg.gcPrepAheadMaxPerFrame, cfg.gcPrepAheadBudgetUs,
                       cfg.gcPrepAheadWindowMs);  // GcPrepAhead (live once installed)
        SetStreamRegisterGranularity(cfg.streamRegisterGranularity);  // the key (live); applied on the game thread at AddToWorld
        SetStreamInBudget(cfg.streamInBudget);  // StreamInBudget (live); applied on the game thread at AddToWorld
        SetNavQueryPriority(cfg.navQueryPriority);  // NavQueryPriority (live); applied on the game thread at AddToWorld
        SetPhysicsSubstepCap(cfg.physicsSubstepCap);  // PhysicsSubstepCap (live); applied on the game thread at AddToWorld
        if (g_lumenNarrowHooked) SetLumenCardsNarrow(cfg.lumenCardsNarrow);  // LumenCardsNarrow: live once installed (render thread forgets on a change)
        if (g_lumenScanHooked) SetLumenScanCull(cfg.lumenScanCull);  // LumenScanCull: live once installed (any change marks every block dirty)
        SetWaterInfoSkip(cfg.waterInfoSkip);  // WaterInfoSkip: live once installed (no-op otherwise)
        SetWaterInfoSpread(cfg.waterInfoSpread, cfg.waterInfoSpreadNearM, cfg.waterInfoSpreadPerFrame, cfg.waterInfoSpreadMaxFrames,
                           cfg.waterInfoSpreadMergeMs);  // WaterInfoSpread: live once installed (no-op otherwise)
        SetWaterInfoAfterCrossing(cfg.waterInfoAfterCrossing, cfg.waterInfoAfterCrossingQuietMs);  // WaterInfoAfterCrossing: live
        SetRepathParallel(cfg.repathParallel, cfg.repathParallelMin, ParallelHelpersAllowed(cfg));  // RepathParallel: live once installed
        SetRepathEarly(cfg.repathEarly);  // RepathEarly: live once installed (no-op otherwise)
        SetCapSleepSkip(cfg.capSleepSkip, cfg.capSleepSkipMaxMs, cfg.capSleepSkipAddBack);  // CapSleepSkip: live once installed (no-op otherwise)
        SetRepathDefer(cfg.repathDefer, cfg.repathDeferGraceUs, cfg.repathDeferMaxFrames,
                       cfg.repathDeferTolerance);  // RepathDefer: live once installed (no-op otherwise)
        if (g_rigSetup) SetRigSetup(cfg.rigVmHashMemo, cfg.rigLogicMapCache);  // RigVmHashMemo / RigLogicMapCache: live once installed
        if (g_npcBodies) SetNpcBodiesOneLock(cfg.npcBodiesOneLock);  // NpcBodiesOneLock: live once installed
        SetRtSceneKeep(relief.shared && cfg.rtSceneBufferKeep ? 2 : cfg.rtSceneBufferKeep);  // RtSceneBufferKeep: live once installed (no-op otherwise); 2 = the game's way
        SetRayTracingBuildBatch(cfg.rayTracingBuildBatch);  // RayTracingBuildBatch: live once installed (no-op otherwise)
        SetOsPageCacheAsyncRelease(cfg.osPageCacheAsyncRelease);  // OsPageCacheAsyncRelease: applied by SetOsPageCache below
        SetOsPageCache(relief.shared ? 0 : cfg.osPageCache, cfg.osPageCacheMB, cfg.osPageCacheTrimSeconds, relief.osCache);  // OsPageCache: no-op unless installed
        // PsoPrecompileFast: no-op unless installed at start-up; faster only while TaskWaitDeadlockGuard is in mode 1 (rule R1,
        // its mode was set above in this same pass)
        ApplyPsoPrecompileFast(cfg.psoPrecompileFast, TaskWaitGuardMode());
        SetPsoPrecompileFastInGameMenus(cfg.psoPrecompileFastInGameMenus);
        SetPsoPrecompileFastBudget(static_cast<float>(cfg.psoPrecompileFastBudgetMs < 1 ? 1 : cfg.psoPrecompileFastBudgetMs),
                                   cfg.psoPrecompileColdQueue < 0 ? 0 : cfg.psoPrecompileColdQueue);
        PsoPrecompileWatchDriver();  // the cold-cache guard's driver-thread reading (no-op unless installed and precompiling)
        SetAnimTeardownParallel(ParallelHelpersAllowed(cfg) ? cfg.animTeardownParallel : 0);    // AnimTeardownParallel: no-op unless installed at start-up
        SetCellBufferPacedEvict(cfg.cellBufferPacedEvict);    // CellBufferPacedEvict: no-op unless installed at start-up
        SetCellBufferEvictDelay(cfg.cellBufferEvictDelayMs);  // CellBufferEvictDelayMs (otunload; public 1.3.0): live
        SetCellEvictSplit(cfg.cellEvictSplit);                // CellEvictSplit (public 1.5.0): live once found (0 when not)
        HdrMenuMonitorTick();                                 // HdrMenuRows: reads Save_Settings.sav once (no-op unless installed)
        SetHdrGamma22(cfg.hdrGamma22, cfg.hdrGamma22White);   // HdrGamma22 (trade-off): live (no-op unless installed)
        HdrGammaMonitorTick();                                // ... fits a curve the game thread asked for
        SetPhysicsPushParallel(ParallelHelpersAllowed(cfg) ? cfg.physicsPushParallel : 0, cfg.physicsPushMin);  // PhysicsPushParallel: no-op unless installed
        if (g_navmeshFound) SetNavmeshBuildNoBusyWait(cfg.navmeshBuildNoBusyWait);
        if (g_busyWaitFound) SetBusyWaitFlagCheck(cfg.busyWaitFlagCheck);
        if (g_creatureAnimFound) SetCreatureAnimPreload(cfg.creatureAnimPreload);
        if (g_gpuSceneHooked) SetGpuSceneKeepCapacityEnabled(cfg.gpuSceneKeepCapacity && !relief.shared);
        if (g_poolKeepHooked) SetGpuPoolKeep(GpuPoolKeepBudgetNow(cfg.gpuPoolKeepMB, relief.upload));  // GpuPoolKeepMB: live; MemoryRelief / VRAM tight = give back
        if (g_allocTrimHooked) {
            SetAllocatorTrim(cfg.commandAllocatorTrim, cfg.allocatorTrimWindowSeconds);
            SetAllocatorTrimCopyFloor(cfg.allocatorTrimCopyFloor);
            SetImmediateAllocatorLists(cfg.commandAllocatorTrim ? cfg.immediateAllocatorLists : 0);
            AllocatorTrimTick();
        }
        // WaterCapture* and NavLinkFast: live once installed (no-ops otherwise)
        SetWaterCaptureFsrSkip(cfg.waterCaptureFsrSkip);
        SetWaterCaptureNoRhiFlush(cfg.waterCaptureNoRhiFlush);
        SetWaterCaptureNoOcclusionWait(cfg.waterCaptureNoOcclusionWait);
        SetWaterCaptureFastMeshScan(cfg.waterCaptureFastMeshScan);
        if (g_navLinkHooked) SetNavLinkFast(cfg.navLinkFast);
        // SaveBytesBulk / SaveThumbnailAsync / SaveRefIdIndex: live once installed (lab: with SaveStallTimers)
        const bool saveTimers = false;
        if (g_saveBytesHooked) SetSaveBytes(cfg.saveBytesBulk, saveTimers, cfg.saveThumbnailAsync);
        if (g_saveRefIdHooked) SetSaveRefIdIndex(cfg.saveRefIdIndex, saveTimers);
        if (cfg.statsIntervalSeconds > 0 && seconds % cfg.statsIntervalSeconds == 0) {
            LogAtmosphereStats();
            LogRdgSubmitStats();
            LogFrameFenceStats();
            LogClothConfigStats();
            if (g_frozenClothHooked) LogFrozenClothStats();
            if (g_fadeInHooked) LogNpcFadeInStats();
            LogCollisionPrepareStats();
            if (g_destroyBatch) LogPhysicsDestroyBatchStats();
            LogBodyRemovalSpreadStats();  // no-op unless installed (the lab logs every 5 s instead)
            LogStreamOutEndPlaySliceStats();  // StreamOutEndPlaySlice (no line when no level was removed)
            LogResetLoadersNoFlushStats();    // ResetLoadersNoFlush (no line when not called)
            LogStartupBarFinishStats();       // StartupBarFinish (no line when no finish loop ran)
            LogTopicSortStats();              // TopicSort (no line when no topic list was sorted)
            LogParallelFileReadsStats();      // ParallelFileReads (no line when nothing was read since the last)
            LogGcPassAfterBusyStats();        // GcPassAfterBusy (no line when no timer pass fell due)
            LogGcPrepAheadStats();            // GcPrepAhead (no line when nothing happened)
            LogCapSleepSkipStats();           // CapSleepSkip (no line when no wait was shortened)
            LogGcMarkPriorityStats();         // GcMarkPriority (no line when no GC pass ran)
            StreamGranLogStats();         // StreamRegisterGranularity (no-op unless active)
            if (g_lumenNarrowHooked) LogLumenCardsNarrowStats();  // LumenCardsNarrow (no line when nothing ran)
            if (g_lumenScanHooked) LogLumenScanCullStats(false);  // LumenScanCull (no line when nothing ran; MISMATCH lines of the proof)
            LogRepathParallelStats();     // RepathParallel (no line when nothing ran)
            LogPairReleaseOnceStats();    // PairReleaseOnce (no line unless the count changed)
            if (g_rigSetup) LogRigSetupStats();
            if (g_npcBodies) LogNpcBodiesOneLockStats();
            LogRtSceneKeepStats();
            LogGpuPoolKeepStats();        // GpuPoolKeepMB (no line when nothing was kept)
            LogRayTracingBuildBatchStats();
            LogWaterCaptureStats();
            LogWaterInfoSkipStats();  // WaterInfoSkip (no line unless the count changed)
            LogWaterInfoSpreadStats();  // WaterInfoSpread (no line unless the count changed)
            LogMapPopupSpreadStats();   // MapPopupSpread (no line unless a map list was seen since the last one)
            LogGpuMenuShotStats();      // GpuMenuShot (no line unless something happened)
            if (g_navLinkHooked) LogNavLinkFastStats();
            LogEditorIdLookupGuardStats();
            LogTrapTargetGuardStats();
            LogBowSneakAttackFixStats();
            LogStuckEffectLookStats();
            LogWorldSoundPositionStats();
            LogArrowHitSoundFixStats();
            LogAutosaveFollowsMenuStats();
            LogSaveThumbLeftoverStats();
            LogHavokPickWarningGuardStats();
            LogFrameGenCrashGuardStats();
            LogObjectReplaceGuardStats();
            LogTaskWaitGuardStats();
            LogFrameGenAsyncComputeState();
            LogFrameGenCameraInfoStats();
            LogFrameGenPacingStats();
            LogHdrGammaStats();       // HdrGamma22 (no-op unless it wrote)
            LogHdrMenuBackgroundStats();  // HdrMenuBackground (no-op unless it converted)
            LogHdrSaveThumbnailStats();   // HdrSaveThumbnail (no-op unless a save's picture passed)
            LogDisplayModeCacheStats();
            LogDisplayNativeStats();  // MonitorNativeFromWindows (no-op unless installed)
            LogImageResizeStats();
            LogVramCounterFixStats();  // VramCounterFix (no-op unless the counter was read)
            LogReadbackConvertStats();
            LogScreenshotParallelStats();
            LogOsPageCacheStats();
            LogPsoPrecompileStats();
            LogAnimTeardownParallelStats();
            LogCellBufferPacedEvictStats();
            LogSeparableSkinAbove4KStats();
            LogVtStreamingUploadsStats();
            LogFocusRestoreStats();
            LogMenuOpenFlashStats();
            if (g_physAnimHooked) LogPhysAnimNoEmptyLockStats();
            LogRigTraceReuseStats();
            LogRigImportTemplateStats();
            LogCellEvictSplitStats();  // CellEvictSplit (a line when steps ran)
            LogPerceptionLosPrefetchStats();  // PerceptionLosPrefetch (no line when no round ran)
            LogFarPawnMoveStats();  // FarPawnMoveHalfRate / FarHorseMove (no line when nothing qualified)
            LogCreatePreloadStats();  // CreatePreload (no line when nothing waited)
            LogImpactPreloadStats();  // ImpactPreload (a line when its state or count changed)
            LogRowPreloadStats();     // RowPreload (a line when its counts changed)
            LogCreateAheadStats();    // CreateAhead (a line when its counts changed)
            LogCellObjectPreloadStats();  // CellObjectPreload (a line when its counts changed)
            LogOtFrameTrimStats();    // OtFrameTrim (no line when the frame-start sleep did not run)
            LogLooseMissCacheStats(); // LooseMissCache (no line when nothing was answered from memory)
            LogPowerAttackAnimKeepStats();  // PowerAttackAnimKeep (no line when no file was left loaded)
            LogFlowOverlapsOnceStats();     // FlowOverlapsOnce (no line when no adoption batched)
            LogChaosMaxPayloadSizeLive();
            LogStreamChurnStats();
            LogAllocatorTrimStats();
            LogNpcPacingStats();
            LogClothRegisterStats();
            LogClothBuildDeferStats();
            LogParallelStaticBodiesStats();
            LogPipelinedSceneAddStats();
            LogPipelinedBodyCreationStats();
            LogBusyWaitStats();
            LogDirtyTreeQuickInsertStats();
            LogBulkIndexInsertStats();
            LogPhysicsPushStats();
            LogPhysicsFlushStats();
            LogInstanceClearStats();
            LogMemoryStats();
            LogGpuPassMarkersStats();
            LogViewRenameGuardStats();
            LogDoorDiagStats();
            LogFadeVtFlushStats();
            LogStreamBehindLoadingStats();
        }
    }
}

void Init(const char* loadedVia, bool proxyOk, LoadStage requested)
{
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;

    // version.dll may also be loaded by helper processes (crash reporter etc.): only patch the game. The Steam build is
    // the tested one; the Xbox / PC Game Pass build (Binaries\WinGDK) is compiled separately and supported
    // experimentally: every fix checks its own code and stays off where it does not match.
    const std::wstring exeName = FileName(ModulePath(nullptr));
    const bool steamExe = _wcsicmp(exeName.c_str(), L"OblivionRemastered-Win64-Shipping.exe") == 0;
    const bool gamePassExe = _wcsicmp(exeName.c_str(), L"OblivionRemastered-WinGDK-Shipping.exe") == 0;
    if (!steamExe && !gamePassExe) return;

    // One instance per process, even when installed both as version.dll and as an OBSE64 plugin.
    wchar_t mutexName[64];
    swprintf_s(mutexName, L"Local\\OblivionPerfFix.%lu", GetCurrentProcessId());
    if (CreateMutexW(nullptr, FALSE, mutexName) && GetLastError() == ERROR_ALREADY_EXISTS) return;

    const std::wstring dir = Directory(ModulePath(g_self));
    g_iniPath = dir + L"\\OblivionPerfFix.ini";
    const Config cfg = LoadConfig(g_iniPath.c_str());  // before the log opens: [General] LogFile decides whether it does
    LogOpen((dir + L"\\OblivionPerfFix.log").c_str(), 2, cfg.logFile);  // the last two sessions stay as .1.log and .2.log
    Log("OblivionPerfFix " OPF_VERSION OPF_RELEASE_SUFFIX " loaded as %s in %s", loadedVia,
        steamExe ? "the Steam build (OblivionRemastered-Win64-Shipping.exe)"
                 : "the Xbox / PC Game Pass build (OblivionRemastered-WinGDK-Shipping.exe, experimental)");
    if (!proxyOk) Log("warning: could not resolve all System32\\version.dll exports");

    LARGE_INTEGER initStart;
    QueryPerformanceCounter(&initStart);

    GameModule game;
    if (!InitGameModule(game)) {
        Log("unexpected game module layout: nothing patched");
        return;
    }
    Log("game exe: TimeDateStamp 0x%08X, SizeOfImage 0x%X", game.timeDateStamp, game.sizeOfImage);
    NoteGameBuild(game);
    // The load stage (obseparity): version.dll = in DllMain; the OBSE64 route is checked here, before our own threads start:
    // before WinMain only on the process's first thread with no thread started in the exe yet (engine, task graph, RHI).
    LoadStage stage = requested;
    if (requested != LoadStage::InDllMain) {
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            game.base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game.base)->e_lfanew);
        const ThreadCensus c = TakeThreadCensus(game.base, game.sizeOfImage, game.base + nt->OptionalHeader.AddressOfEntryPoint);
        stage = c.ok ? ClassifyLoadStage(c.currentIsEntry, c.exeThreads) : LoadStage::Late;
        std::string others;
        for (const auto& o : c.others) others += (others.empty() ? "" : ", ") + std::to_string(o.second) + " " + o.first;
        Log("[stage] load stage: %s; %s, %d other engine thread(s) started in the exe, %d thread(s) in all (others by start module: %s)",
            LoadStageName(stage), !c.ok ? "thread snapshot failed" : c.currentIsEntry ? "called on the main thread" : "NOT called on the main thread",
            c.exeThreads, c.threads, others.empty() ? "none" : others.c_str());
        for (const auto& t : c.exeStarts)
            Log(t.rva ? "[stage] thread started in the exe at exe+0x%X, %lld ms after the process%s" : "[stage] a thread whose start could not be read (%X), %lld ms after the process%s: counted", t.rva, static_cast<long long>(t.createdMs),
                t.crash ? ": the engine's crash-reporting thread (made by the exe's static set-up; not counted)" : "");
    } else {
        Log("[stage] load stage: %s", LoadStageName(stage));
    }
    SetLoadStage(stage);
    // Start-up-only fixes install in DllMain and before WinMain (our audit of every start-up gate, 2026-11-21: every target they
    // patch first runs in PreInit or later; the two exceptions, IndirectArgBufferPool's already registered variable and
    // OsPageCache's live allocator thunks, check CurrentLoadStage() themselves). A late load keeps them off.
    const bool beforeGameCode = stage == LoadStage::InDllMain || stage == LoadStage::BeforeWinMain;
    if (!beforeGameCode)
        Log("[stage] loaded late: the start-up-only fixes stay off (each says so below; installed as version.dll they work)");
    // PluginPrefetch (gapfix; S-058): a low-priority thread reads the active plugin files once into the file cache (it runs
    // after DllMain returns, long before the old engine loads them). First, so it has the longest head start.
    StartPluginPrefetch(cfg.pluginPrefetch);
    // Diagnostics only: an exception handler that writes UE's GPU crashes (DRED and memory) to the log.
    if (cfg.gpuCrashReport) InstallGpuCrashReport();
    else if (LogEnabled()) Log("[gpucrash] GPU crash report disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    SetGpuCrashIniPath(g_iniPath.c_str());
    {
        // GpuPassMarkers: render pass names for DRED / AMD's tools (two call sites; the variable is set once the device exists)
        wchar_t ctl[MAX_PATH] = L"";
        if (cfg.gpuPassMarkers || ctl[0]) InstallGpuPassMarkers(cfg.gpuPassMarkers ? cfg.gpuPassMarkers : 1, ctl);
    }
    const bool waterProbe = false;
    // WaterInfoSkip (and the lab's WaterInfoProbe) need the water-info render's game-thread detour.
    const bool waterGt = waterProbe || cfg.waterInfoSkip != 0;
    // WaterCaptureFsrSkip / NoRhiFlush / NoOcclusionWait / FastMeshScan: the water-info render's entry and the four sites,
    // written only before game code runs, when any key is on (lab: or WaterInfoProbe); then live.
    if (waterGt || cfg.waterCaptureFsrSkip || cfg.waterCaptureNoRhiFlush || cfg.waterCaptureNoOcclusionWait || cfg.waterCaptureFastMeshScan) {
        if (InstallWaterCapture(beforeGameCode, waterGt)) {
            // WaterInfoSkip (waterinfo; public 1.3.0): checks its layout reads in the detoured UpdateWaterInfoRendering; then live.
            InstallWaterInfoSkip(beforeGameCode, cfg.waterInfoSkip, WaterInfoGtSite(), g_iniPath.c_str());
            SetWaterCaptureFsrSkip(cfg.waterCaptureFsrSkip);
            SetWaterCaptureNoRhiFlush(cfg.waterCaptureNoRhiFlush);
            SetWaterCaptureNoOcclusionWait(cfg.waterCaptureNoOcclusionWait);
            SetWaterCaptureFastMeshScan(cfg.waterCaptureFastMeshScan);
        }
    } else Log("[water] water-info capture keys off in OblivionPerfFix.ini (takes a restart to switch on)");
    // WaterInfoSpread (waterspread; public 1.3.0, port130k): water-info updates spread over frames / merged (its own call site in
    // UWaterSubsystem::Tick, independent of the capture keys); start-up only, then live.
    // WaterInfoAfterCrossing (waterlate, S-003) shares that call site: set first, so the site is installed when only it is on.
    SetWaterInfoAfterCrossing(cfg.waterInfoAfterCrossing, cfg.waterInfoAfterCrossingQuietMs);
    InstallWaterInfoSpread(beforeGameCode, cfg.waterInfoSpread, cfg.waterInfoSpreadNearM, cfg.waterInfoSpreadPerFrame,
                           cfg.waterInfoSpreadMaxFrames, cfg.waterInfoSpreadMergeMs);
    // ViewRenameGuard: the view rename guard (UE-217692). Placed before the game's code runs (two entry detours), then switched
    // live by the ini; RdgParallelTranslate's frame fence is only used while it repairs.
    if (const int mode = ViewGuardMode(cfg); mode > 0) {
        if (!beforeGameCode) Log("[view] view rename guard: can only be installed at start-up (loaded late here), not installed");
        else if (InstallViewRenameGuard(mode)) {
            g_viewGuardHooked = true;
        }
    } else Log("[view] view rename guard disabled in OblivionPerfFix.ini (switch it on before starting the game)");

    if (cfg.boneUploadStores) ApplyBoneUploadFix();
    else Log("[bone] disabled in OblivionPerfFix.ini");
    // Installed only if enabled at startup; afterwards the ini switches it on and off live.
    if (cfg.atmosphereDirtyFilter) {
        ApplyAtmosphereDirtyFix();
        SetAtmosphereLightFastPathEnabled(cfg.atmosphereLightFastPath);
        SetAtmosphereLightCloudPathEnabled(cfg.atmosphereLightCloudPath);
    } else Log("[atm] disabled in OblivionPerfFix.ini");
    // The site is always located so the ini can switch the fix live.
    if (FindRdgParallelTranslateSite()) {
        // Parallel translate needs the descriptor heap guard (an early build crashed without it).
        if (!InstallDescriptorHeapGuard()) SetRdgParallelTranslateAllowed(false);
        // ... and, the buffer-pool defragmentation pause (translate_defrag_guard.cpp).
        if (!FindPoolDefragSwitch()) SetRdgParallelTranslateAllowed(false);
        // RdgFrameFence: with the frame fence, buffer defragmentation keeps running
        // while parallel translate is on, only while ViewRenameGuard is installed and on. The detour is only placed when wanted.
        if (FrameFenceWanted(cfg) && FindFrameFence()) InstallFrameFence(beforeGameCode);
        else if (cfg.rdgFrameFence && !FrameFenceWanted(cfg))
            Log("[rdg] frame fence not used: ViewRenameGuard (ViewRenameGuard) is off or not installed, so buffer defragmentation "
                "stays paused while translating");
        SetRdgUseFrameFence(FrameFenceWanted(cfg));
        // The submission hooks can only be installed now; afterwards the ini switches batching live.
        if (cfg.rdgSubmitBatch <= 0) Log("[rdg] submit batching disabled in OblivionPerfFix.ini (takes a restart to enable)");
        else if (!InstallRdgSubmitBatching(!beforeGameCode)) Log("[rdg] submit batching not available: parallel translate keeps its GPU cost");
        // Automatic mode (2) waits for the monitor thread's first look at the GPU (dxgi.dll cannot be loaded here).
        SetRdgParallelTranslateEnabled(cfg.rdgParallelTranslate == 1);
        SetRdgSubmitBatch(RdgParallelTranslateActive() ? cfg.rdgSubmitBatch : 0, cfg.rdgSubmitMaxAgeUs);
    }
    // Installed only if enabled at startup; afterwards the ini switches it on and off live.
    if (cfg.clothConfigCache) ApplyClothConfigFix(beforeGameCode);
    else Log("[cloth] disabled in OblivionPerfFix.ini");
    // FrozenClothStretchFix (a game bug fix) and NpcFadeInNoRebuild: one call / one function entry each, written only
    // before game code runs, and only if enabled at startup; afterwards the ini switches them on and off live.
    if (cfg.frozenClothStretchFix) {
        g_frozenClothHooked = InstallFrozenClothStretchFix(beforeGameCode);
        if (g_frozenClothHooked) SetFrozenClothStretchFix(true);
    } else Log("[frozencloth] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    if (cfg.npcFadeInNoRebuild) {
        g_fadeInHooked = InstallNpcFadeInNoRebuild(beforeGameCode);
        if (g_fadeInHooked) SetNpcFadeInNoRebuild(true);
    } else Log("[fadein] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // PrepareCollisionBehindLoadingScreen runs from the fade-to-game hook (two vtable slots), installed only if the fix is enabled at startup and its
    // game code is found; afterwards the ini switches it on and off live.
    if (cfg.prepareCollisionBehindLoadingScreen) {
        if (FindCollisionPrepare() && InstallFadeHook()) {
            g_collisionHooked = true;
            SetCollisionPrepareEnabled(true);
        }
    } else Log("[collision] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // NameLookupCrashGuard (a crash guard, not a speed-up): installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.editorIdLookupGuard) ApplyEditorIdLookupGuard();
    else Log("[edid] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // TrapCrashGuard (a crash guard, not a speed-up): installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.trapCrashGuard) ApplyTrapTargetGuard();
    else Log("[trap] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // BowSneakAttackFix (a game bug fix that changes damage back to the intended value): two calls in the OblivionThread's
    // code, written only before game code runs and only if on at start-up; afterwards the ini switches it live.
    if (cfg.bowSneakAttackFix) {
        if (!beforeGameCode) Log("[bowsneak] can only be installed at start-up (loaded late here), not installed");
        else if (ApplyBowSneakAttackFix()) SetBowSneakAttackFixEnabled(true);
    } else Log("[bowsneak] BowSneakAttackFix off in OblivionPerfFix.ini (takes a restart to switch on)");
    // AutosaveFollowsMenu (a game bug fix: no autosave the menu's options say not to make): two calls in the
    // OblivionThread's code, written only before game code runs and only if on at start-up; afterwards the ini switches it live.
    if (cfg.autosaveFollowsMenu) {
        if (!beforeGameCode) Log("[autosave] can only be installed at start-up (loaded late here), not installed");
        else if (ApplyAutosaveFollowsMenu()) SetAutosaveFollowsMenuEnabled(true);
    } else Log("[autosave] AutosaveFollowsMenu off in OblivionPerfFix.ini (takes a restart to switch on)");
    // SaveThumbLeftoverFix (a game bug fix: a save made in a menu leaves its thumbnail binding on the menus' screenshot
    // service): two calls redirected, only before game code runs and only if on at start-up; afterwards the ini switches it live.
    if (cfg.saveThumbLeftoverFix) {
        if (InstallSaveThumbLeftoverFix(beforeGameCode)) SetSaveThumbLeftoverFix(true);
    } else Log("[thumbleft] SaveThumbLeftoverFix off in OblivionPerfFix.ini (takes a restart to switch on)");
    // MonitorNativeFromWindows / FullscreenAllResolutions: one call redirected / one jump changed, only if on at
    // startup (before the game first reads its display metrics on the version.dll route).
    if (cfg.monitorNativeFromWindows) InstallMonitorNativeFromWindows();
    else Log("[display] MonitorNativeFromWindows off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (cfg.fullscreenAllResolutions) InstallFullscreenAllResolutions();
    else Log("[display] FullscreenAllResolutions off in OblivionPerfFix.ini (takes a restart to switch on)");
    // HavokPickWarningGuard (a freeze guard, not a speed-up): installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.havokPickWarningGuard) ApplyHavokPickWarningGuard();
    else Log("[picks] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // FrameGenCrashGuard (a crash guard in AMD's FidelityFX library, not a speed-up): installed when the game loads the library, if
    // enabled at startup; afterwards the ini switches it live.
    if (cfg.frameGenCrashGuard) WatchFrameGenLibrary(beforeGameCode);
    else Log("[fg] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // ObjectReplaceCrashGuard (a crash guard for a game bug, not a speed-up): installed only if enabled at startup; afterwards the ini
    // switches it live (0 = the game's way: the fatal error).
    if (cfg.objectReplaceGuard && !beforeGameCode) Log("[replace] crash guard: can only be installed at start-up (loaded late here), not installed");
    else if (cfg.objectReplaceGuard) {
        SetObjectReplaceGuardMode(cfg.objectReplaceGuard);
        ApplyObjectReplaceGuard();
    } else Log("[replace] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // TaskWaitDeadlockGuard (a guard against a game deadlock, not a speed-up): one vtable slot of the task graph, written only
    // before game code runs (the task graph starts early); afterwards the ini switches the mode live (0 = the game's wait).
    // OBSE64 calls the plugin before WinMain (from its hook of the CRT's command-line call, findings promote2 / boottriage): the
    // task graph starts later (FEngineLoop::PreInit), so the guard installs there as in DllMain (obseparity); only a late load
    // (the stage check above) keeps the game's own wait, and then PsoCompileThreadPool is not applied either.
    if (cfg.taskWaitDeadlockGuard && !beforeGameCode)
        Log("[taskwait] TaskWaitDeadlockGuard not installed: it can only be installed at start-up; loaded late, the game's own "
            "wait is kept, and PsoCompileThreadPool is not applied either");
    else if (cfg.taskWaitDeadlockGuard) InstallTaskWaitGuard(cfg.taskWaitDeadlockGuard);
    else Log("[taskwait] TaskWaitDeadlockGuard off (takes a restart to switch on)");
    // PsoCompileThreadPool: two console variables located always (no patching); the pool size is written once, here, and only
    // with the guard above installed in mode 1 (UE treats the size as constant; the pool alone runs into the deadlock).
    if (FindPsoCompileThreadPool()) SetPsoCompileThreadPool(cfg.psoCompileThreadPool, cfg.psoCompileThreadPoolPriority);
    // IndirectArgBufferPool: one console variable's int, written here (before the exe's static initialisers register it, long
    // before the RHI starts); version.dll route only.
    SetIndirectArgBufferPool(cfg.indirectArgBufferPool, beforeGameCode);
    // ParallelFileReads: the engine's container reader (one vtable slot, and its ServiceNotify) replaced when the key is >= 2 at
    // start-up; either route (the reader thread fetches the slot at every call). Live afterwards.
    InstallParallelFileReads(cfg.parallelFileReads);
    // FrameGenAsyncCompute: a console variable the game registers during its start-up, set by a short-lived thread as soon as it exists
    // (before the renderer and frame generation start). Start-up only.
    if (cfg.frameGenAsyncCompute) ApplyFrameGenAsyncCompute(beforeGameCode);
    else Log("[fgasync] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // FrameGenCameraInfo (optional, for AMD's ML frame generation: SDK 2.x DLLs, or the driver's with AMD's 3.1.4 DLL): installed at
    // start-up only if enabled; afterwards the ini switches it live.
    if (cfg.frameGenCameraInfo) ApplyFrameGenCameraInfo(beforeGameCode);
    else Log("[fgcam] off in OblivionPerfFix.ini (optional; takes a restart to switch on)");
    // FrameGenPacingTuning (optional, for AMD's 3.1.4 or SDK 2.x DLLs): watches the library's load at start-up whatever the setting (the hook
    // remembers the swap chain context and logs each context's provider), so the ini can switch the tuning live; 0 leaves the
    // game's pacing untouched.
    ApplyFrameGenPacingTuning(beforeGameCode, cfg.frameGenPacingTuning);
    // DisplayModeCache: installed only if enabled at startup (one import-slot write, safe at any time); afterwards the ini
    // switches it live. If the loader has not filled the slot yet, the monitor thread installs it a little later.
    if (cfg.displayModeCache) {
        const int r = ApplyDisplayModeCache(true);
        if (r > 0) g_displayHooked = true;
        else if (r < 0) g_displayRetries = 30;
    } else Log("[display] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // MenuScreenshotFilter: installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.screenshotFilterFast) g_resizeHooked = ApplyImageResizeFix(beforeGameCode);
    else Log("[resize] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // VramCounterFix (game bug): the "Show VRAM" counter's two query calls redirected, only if on at start-up (as the
    // other fixes: 0 at start-up leaves the game's code untouched); afterwards the ini switches it live.
    if (cfg.vramCounterFix) {
        if (ApplyVramCounterFix(beforeGameCode)) SetVramCounterFix(true);
    } else Log("[vram] VramCounterFix off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ScreenshotReadbackConvert: installed only if enabled at startup; afterwards the ini switches it live (0 / 1 / 2 = verify).
    if (cfg.screenshotReadbackConvert) {
        if (ApplyReadbackConvertFix(beforeGameCode)) SetReadbackConvertMode(cfg.screenshotReadbackConvert);
    } else Log("[readback] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // OsPageCache: installed only if enabled at startup (the lab trace installs it too); afterwards the ini switches it live.
    const int osTrace = 0;
    if (cfg.osPageCache || osTrace) {
        if (InstallOsPageCache(beforeGameCode, Directory(g_iniPath).c_str(), osTrace))
        {
            SetOsPageCacheAsyncRelease(cfg.osPageCacheAsyncRelease);
            SetOsPageCache(cfg.osPageCache, cfg.osPageCacheMB, cfg.osPageCacheTrimSeconds, false);
        }
    } else Log("[oscache] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // PsoPrecompileFast: SetBatchMode's entry detour, installed only if enabled at startup and only with TaskWaitDeadlockGuard
    // installed above (rule R1, user decision 2026-11-11, issue 51: without the guard the game's own pace); afterwards the
    // ini switches it live, and the guard's mode decides too (faster only in mode 1).
    if (cfg.psoPrecompileFast && !PsoPaceInstall(true, TaskWaitGuardMode()))
        Log("[pso] PsoPrecompileFast not installed: the game's own shader precompile pace, because TaskWaitDeadlockGuard is not "
            "installed (%s) and the faster pace without it can run into the game's task-wait freeze (issue 51)",
            !beforeGameCode ? "loaded late, after the game's threads started" : !cfg.taskWaitDeadlockGuard ? "TaskWaitDeadlockGuard=0"
                                                                             : "unavailable, see the [taskwait] line");
    else if (cfg.psoPrecompileFast) {
        SetPsoPrecompileFastBudget(static_cast<float>(cfg.psoPrecompileFastBudgetMs < 1 ? 1 : cfg.psoPrecompileFastBudgetMs),
                                   cfg.psoPrecompileColdQueue < 0 ? 0 : cfg.psoPrecompileColdQueue);
        if (InstallPsoPrecompileFast(beforeGameCode)) ApplyPsoPrecompileFast(true, TaskWaitGuardMode());
    } else Log("[pso] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ScreenshotParallel: installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.screenshotParallel) {
        if (ApplyScreenshotParallelFix(beforeGameCode)) SetScreenshotParallel(cfg.screenshotParallel);
    } else Log("[shotpar] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // RestoreFsrAfterAltTab (a bug fix, not a speed-up): installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.restoreFsrAfterAltTab) g_focusHooked = ApplyFocusRestoreFix(beforeGameCode);
    else Log("[focus] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // MenuOpenFlashFix (issue #5, game bug): installed only if enabled at start-up (after RestoreFsrAfterAltTab, whose
    // console-variable offsets it reuses); afterwards the ini switches it live. A late load logs it as not installed.
    if (cfg.menuOpenFlashFix) {
        if (InstallMenuOpenFlashFix(beforeGameCode)) SetMenuOpenFlashFix(true);
    } else Log("[menuflash] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // LumenKeepArrayCapacity and UploadMemoryKeep (traversal stutter): installed only if enabled at startup; afterwards the ini switches them live.
    if (cfg.lumenKeepArrayCapacity) {
        g_lumenHooked = ApplyLumenKeepCapacityFix(beforeGameCode);
        if (g_lumenHooked) {
            SetLumenKeepCapacityEnabled(true);
            SetLumenGroupsPrior(cfg.lumenGroupsPrior, LumenPriorSpareMB(cfg));
        }
    } else Log("[lumen] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    if (cfg.uploadMemoryKeep) {
        g_uploadHooked = ApplyUploadMemoryKeepFix(beforeGameCode);
        if (g_uploadHooked) SetUploadMemoryKeep(true, cfg.uploadMemoryKeepFrames, cfg.uploadPagesToKeep, cfg.uploadReleaseRampFrames);
    } else Log("[upload] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // GpuPoolKeepMB: the texture and buffer pools' 20-frame clean-up calls, only if > 0 at start-up; then the budget is live.
    if (cfg.gpuPoolKeepIniMB != cfg.gpuPoolKeepMB)
        Log("[poolkeep] GpuPoolKeepMB=%d in OblivionPerfFix.ini is outside 0..4096: using %d", cfg.gpuPoolKeepIniMB, cfg.gpuPoolKeepMB);
    if (cfg.gpuPoolKeepMB > 0) {
        g_poolKeepHooked = InstallGpuPoolKeep(beforeGameCode);
        if (g_poolKeepHooked) SetGpuPoolKeep(cfg.gpuPoolKeepMB);
    } else Log("[poolkeep] GpuPoolKeepMB=0 in OblivionPerfFix.ini (takes a restart to switch on)");
    // GpuSceneKeepCapacity: installed always (the policy logs the game's own resizes too); the ini switches the keeping live.
    g_gpuSceneHooked = ApplyGpuSceneKeepCapacityFix(beforeGameCode);
    if (g_gpuSceneHooked) SetGpuSceneKeepCapacityEnabled(cfg.gpuSceneKeepCapacity);
    // CommandAllocatorTrim: installed always (it counts the allocators in flight); the ini switches the trimming live.
    g_allocTrimHooked = InstallAllocatorTrim(beforeGameCode);
    if (g_allocTrimHooked) {
        SetAllocatorTrim(cfg.commandAllocatorTrim, cfg.allocatorTrimWindowSeconds);
        SetAllocatorTrimCopyFloor(cfg.allocatorTrimCopyFloor);
        SetImmediateAllocatorLists(cfg.commandAllocatorTrim ? cfg.immediateAllocatorLists : 0);
    }
    // CmdMemPregrow: uses those hooks' pool access; runs later, from their once-a-second tick (the key: start-up only).
    SetCmdMemPregrow(cfg.cmdMemPregrow, g_allocTrimHooked, beforeGameCode, cfg.cmdMemGiveBackHeadroomMB, g_iniPath.c_str());
    // PhysicsFlushParallel and PhysicsTreeTimeSlicing's adaptive copy slice: two entry detours (the flush and the physics step), placed if either fix is
    // enabled at startup (or when switched on later); the ini switches both live.
    if (cfg.physicsFlushParallel || cfg.physicsTreeTimeSlicing) {
        g_flushTried = true;
        g_flushHooked = InstallPhysicsFlush(beforeGameCode);
        if (g_flushHooked) SetPhysicsFlushParallel(cfg.physicsFlushParallel, cfg.physicsFlushMinChanges);
    }
    // InstanceClearReserve: one call redirected if enabled at startup (four bytes, written only before the game's code runs); live after.
    if (cfg.instanceClearReserve) {
        g_clearHooked = InstallInstanceClearReserve(beforeGameCode);
        if (g_clearHooked) SetInstanceClearReserve(true);
    } else Log("[instances] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // PhysicsTreeTimeSlicing: console variables, located always, set while the ini switch is on (the copy slice through the step hook).
    if (FindChaosTimeSlicing()) {
        g_chaosFound = true;
        SetChaosTimeSlicing(cfg.physicsTreeTimeSlicing, cfg.physicsSliceIterations, cfg.physicsSliceCopyKB, cfg.physicsSliceAdaptive);
    }

    // NpcCreationPacing: installed only if enabled at startup (vtable slots); afterwards the ini switches it live (switching it off
    // runs the backlog at once).
    if (cfg.npcCreationPacing) {
        g_pacingHooked = InstallNpcPacing();
        if (g_pacingHooked)
            SetNpcPacing(true, cfg.pacingBudgetMs, cfg.pacingMinDistance, cfg.pacingMaxLagFrames, cfg.pacingUseSlack, cfg.pacingFlowWait,
                         cfg.pacingFlowWaitFrames);
        if (g_pacingHooked) SetNpcPacingFadeAware(cfg.pacingFadeAware);
        if (g_pacingHooked) SetNpcPacingOwnerWalk(cfg.pacingOwnerWalk);  // PacingOwnerWalk (mkusm, PR #25): live
        if (g_pacingHooked) SetNpcPacingHairActor(cfg.pacingHairActor);  // PacingHairActor: live
        if (g_pacingHooked) SetNpcPacingLeanFlowDrain(cfg.pacingLeanFlowDrain);  // PacingLeanFlowDrain: live
        if (g_pacingHooked) SetNpcPacingFlowProgress(cfg.pacingFlowProgress);  // PacingFlowProgress: live
        if (g_pacingHooked) SetNpcPacingGateBounds(cfg.pacingGateBoundFrames, cfg.pacingGateCalmMs, cfg.pacingGateNearM, cfg.pacingGateFadeHoldMs);
        if (g_pacingHooked) SetNpcPacingBaseGate(cfg.pacingBaseGate);  // PacingBaseGate: live
        // FadeInUnseen: one rel32 in the fade tick's caller, written at start-up only when != 0; then live. The same hook
        // carries the lab fade hold of PacingBaseGate (PacingGateFadeHoldMs, gate2; rev96e F10: installed for it only when
        // the key is set at start-up).
        const bool fadeHook = cfg.fadeInUnseen != 0;
        if (g_pacingHooked && fadeHook && InstallFadeInUnseen(beforeGameCode))
            SetFadeInUnseen(cfg.fadeInUnseen, cfg.fadeInUnseenWindowMs);
        // PacingGateShownHoldMs (hold2; public 1.5.0): the fade applier's entry, start-up only when > 0 (not after a late load:
        // the log says so), then live
        if (g_pacingHooked && cfg.pacingGateShownHoldMs > 0) {
            if (InstallShownHold(beforeGameCode)) SetShownHold(cfg.pacingGateShownHoldMs, cfg.pacingGateShownHoldEnd);
        } else if (g_pacingHooked) {
            Log("[shownhold] PacingGateShownHoldMs off in OblivionPerfFix.ini (takes a restart to switch on)");
        }
    } else Log("[pacing] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // ClothRegisterLite: installed only if != 0 at startup and only as version.dll (the cloth component's native function table is
    // read when the engine registers the class); afterwards the ini switches it live. The lab's ClothRegisterSkip uses the same
    // hooks: either key installs them.
    const bool clothHooks = cfg.clothRegisterLite != 0;
    if (clothHooks) {
        g_clothRegHooked = InstallClothRegisterSkip(beforeGameCode);
        if (g_clothRegHooked) SetClothRegisterLite(cfg.clothRegisterLite);
    } else Log("[clothreg] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ClothBuildDefer: RecreateClothSimulationProxy's entry, OnUnregister's vtable slot (in front of ClothRegisterLite's hook,
    // installed above) and the two Blueprint readers, only if on at start-up (version.dll route); then live. It defers
    // only while NpcCreationPacing runs.
    if (cfg.clothBuildDefer) {
        if (InstallClothBuildDefer(beforeGameCode)) SetClothBuildDefer(true, cfg.clothDeferQuietFrames);
    } else Log("[clothdefer] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ParallelStaticBodies: one call redirected and its helper threads started, only if enabled at startup (as version.dll); afterwards
    // the ini switches it live.
    if (cfg.parallelStaticBodies && ParallelHelpersAllowed(cfg)) {
        g_bodiesHooked = InstallParallelStaticBodies(beforeGameCode, cfg.parallelStaticBodiesThreads, cfg.parallelStaticBodiesMin);
        if (g_bodiesHooked) SetParallelStaticBodies(true, cfg.parallelStaticBodiesMin);
    } else if (cfg.parallelStaticBodies)
        Log("[bodies] ParallelStaticBodies with PipelinedSceneAdd and PipelinedBodyCreation kept off: %u usable logical processors (needs %d, ParallelMinProcessors)", UsableLogicalProcessors(), cfg.parallelMinProcessors);
    else Log("[bodies] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ParallelStaticBodies and PhysicsFlushParallel's helper threads: the game allocator's per-thread caches (located always; the ini switches it live).
    if (FindHelperThreadCaches()) SetHelperThreadCaches(cfg.helperThreadCaches);
    // DirtyTreeQuickInsert: three calls redirected, only if enabled at startup; afterwards the ini switches it live.
    if (cfg.dirtyTreeQuickInsert) {
        if (InstallDirtyTreeQuickInsert(beforeGameCode)) SetDirtyTreeQuickInsert(true);
    } else Log("[bodies] dirty-tree quick insert off in OblivionPerfFix.ini (takes a restart to switch on)");
    // PhysicsPushParallel: five calls redirected and its helper threads started, only if on at startup; afterwards the ini switches it
    // live (0 off, 1 batch + parallel, 2 batch only). Before BulkIndexInsert,
    // whose lab timer detours ForEachProxy's entry (PhysicsPushParallel checks that entry and calls it through the call site).
    if (cfg.physicsPushParallel && ParallelHelpersAllowed(cfg)) {
        if (InstallPhysicsPush(beforeGameCode, cfg.physicsPushThreads, cfg.physicsPushMin)) {
            SetPhysicsPushParallel(ParallelHelpersAllowed(cfg) ? cfg.physicsPushParallel : 0, cfg.physicsPushMin);
        }
    } else if (cfg.physicsPushParallel)
        Log("[push] PhysicsPushParallel kept off: %u usable logical processors (needs %d, ParallelMinProcessors)", UsableLogicalProcessors(), cfg.parallelMinProcessors);
    else Log("[push] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ShaderScreenAfterDriverChange (game bug): installed only if on at start-up (the decision is made once, early).
    if (cfg.shaderScreenAfterDriverChange) ApplyShaderScreenAfterDriverChange(beforeGameCode, dir.c_str());
    else Log("[shaderscreen] ShaderScreenAfterDriverChange off in OblivionPerfFix.ini (takes a restart to switch on)");
    // Diagnostic HitchLedger: installed only if on at startup (the tick-group entry detour, before BulkIndexInsert, whose lab
    // probe times the same entry; the frame-end sync call; read-only counters); afterwards the ini switches recording live.
    if (cfg.hitchLedger) {
        if (InstallHitchLedger(beforeGameCode, dir.c_str(), g_pacingHooked)) SetHitchLedger(true, cfg.hitchLedgerMs);
        // Diagnostic TickSplit (HitchLedgerTickSplit): the tick task's ExecuteTick call, the tickables' Tick call and
        // UWorld::Tick's calls, installed only if on at start-up (then the ini switches it live; off = nothing installed).
        const bool tickHooks = cfg.hitchLedgerTickSplit;
        if (tickHooks && InstallTickSplit(beforeGameCode, dir.c_str())) SetTickSplit(cfg.hitchLedgerTickSplit);
    }
    // Diagnostic DoorDiag: installed only if on at startup (two entry detours, one vtable slot); its timeline needs
    // NpcCreationPacing's frame hook.
    if (cfg.doorDiag) {
        if (!g_pacingHooked) Log("[doordiag] NpcCreationPacing's frame hook is not installed: no loading-screen timeline");
        if (InstallDoorDiag(beforeGameCode)) SetDoorDiagEarlyStream(cfg.doorDiagEarlyStream);
    }
    // FadeVtFlushSkip (off by default): installed only if on at startup (two .rdata slots of the fade-in's flush timer
    // delegate); afterwards the ini switches it live.
    if (cfg.fadeVtFlushSkip) {
        if (InstallFadeVtFlushSkip()) SetFadeVtFlushSkip(true);
    }
    // StreamBehindLoadingScreen: the gate-arming detour, only if on at startup (it also needs the fade-to-game hook's search and
    // NpcCreationPacing's frame hook); afterwards the ini switches it live.
    if (cfg.streamBehindLoading) {
        if (!g_pacingHooked) Log("[streamload] NpcCreationPacing's frame hook is not installed: not installed");
        else if (InstallStreamBehindLoading(beforeGameCode, cfg.unfreezeBehindLoading != 0)) {
            SetStreamBehindLoading(true);
            SetDrawBehindLoading(cfg.drawBehindLoading);  // DrawBehindLoadingScreen: from the first transition (then live)
        }
    } else Log("[streamload] StreamBehindLoadingScreen off in OblivionPerfFix.ini (takes a restart to switch on)");
    // AnimTeardownParallel: three calls redirected, only if not 0 at startup; afterwards the ini switches it live.
    if (cfg.animTeardownParallel && ParallelHelpersAllowed(cfg)) {
        if (InstallAnimTeardownParallel(beforeGameCode)) SetAnimTeardownParallel(ParallelHelpersAllowed(cfg) ? cfg.animTeardownParallel : 0);
    } else if (cfg.animTeardownParallel)
        Log("[anim] AnimTeardownParallel kept off: %u usable logical processors (needs %d, ParallelMinProcessors)", UsableLogicalProcessors(), cfg.parallelMinProcessors);
    // BulkIndexInsert (and 31b): DirtyTreeQuickInsert's three call sites (installed here also when DirtyTreeQuickInsert is off) and entry detours, only if
    // either key is on at startup; afterwards the ini switches both live.
    // PhysicsQueueShrinkOnce rides on the same catch-up detour (installed with it when its key is on at startup).
    if (cfg.bulkIndexInsert || cfg.physicsInternalFlushParallel || cfg.physicsQueueShrinkOnce
    ) {
        if (InstallBulkIndexInsert(beforeGameCode)) {
            SetBulkIndexInsert(cfg.bulkIndexInsert, cfg.bulkIndexInsertMin, cfg.physicsInternalFlushParallel);
            SetPhysicsQueueShrinkOnce(cfg.physicsQueueShrinkOnce);
        }
    } else Log("[bulk] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // PipelinedSceneAdd: installed with ParallelStaticBodies (start-up), then the ini switches it live.
    if (cfg.parallelStaticBodies && ParallelHelpersAllowed(cfg)) {
        g_sceneAddHooked = InstallPipelinedSceneAdd(beforeGameCode, cfg.pipelinedSceneAddMin);
        if (g_sceneAddHooked) SetPipelinedSceneAdd(ParallelHelpersAllowed(cfg) && cfg.pipelinedSceneAdd, cfg.pipelinedSceneAddMin);
        // PipelinedBodyCreation: rides on ParallelStaticBodies's (and 28b's) hooks; located with them, then the ini switches it live.
        if (g_bodiesHooked && FindPipelinedBodyCreation()) SetPipelinedBodyCreation(ParallelHelpersAllowed(cfg) && cfg.pipelinedBodyCreation);
    }
    // NavmeshBuildNoBusyWait: one byte, located always; the ini switches it live.
    if (FindNavmeshBuildNoBusyWait()) {
        g_navmeshFound = true;
        SetNavmeshBuildNoBusyWait(cfg.navmeshBuildNoBusyWait);
    }
    // BusyWaitFlagCheck: one byte per busy-wait put-back, located always; the ini switches it live.
    if (FindBusyWaitFlagCheck()) {
        g_busyWaitFound = true;
        SetBusyWaitFlagCheck(cfg.busyWaitFlagCheck);
    }
    // CreatureAnimPreload: one byte in the creature queue step, located always; the ini switches it live.
    if (FindCreatureAnimPreload()) {
        g_creatureAnimFound = true;
        SetCreatureAnimPreload(cfg.creatureAnimPreload);
    }
    // NavLinkFast: findConnectingPolys' and addTile's connectExtLinks calls, written only before game code runs and only when
    // the key is not 0 at start-up (then live: 1 fast, 0 the game's search, timed; lab: 2 fast + compared, 3 = 0).
    if (cfg.navLinkFast != 0) {
        g_navLinkHooked = InstallNavLinkFast(beforeGameCode, cfg.navLinkFast < 1 || cfg.navLinkFast > 3 ? 3 : cfg.navLinkFast);
        if (g_navLinkHooked) SetNavLinkFast(cfg.navLinkFast);
    } else Log("[navlink] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // SaveBytesBulk / SaveThumbnailAsync: SaveGameToMemory's call sites and FArrayProperty::SerializeItem's vtable slots, and
    // ProcessScreenShots' read-back call, written only before game code runs and only for a key that is on at start-up
    // (lab builds: always, with SaveStallTimers, so all can be switched live). SaveRefIdIndex: SaveGame's entry and two
    // lookup blocks, the same way. Then live.
    // CreateAhead needs SaveGameToMemory's hook (its save-start drop) whatever SaveBytesBulk says (0 = byte by byte, as stock)
    const bool saveBytesWanted = cfg.saveBytesBulk != 0 || cfg.createAhead != 0, saveThumbWanted = cfg.saveThumbnailAsync != 0,
               saveRefIdWanted = cfg.saveRefIdIndex != 0;
    const bool saveTimers = false;
    if (saveBytesWanted || saveThumbWanted) {
        g_saveBytesHooked = InstallSaveBytes(beforeGameCode, saveBytesWanted, saveThumbWanted);
        if (g_saveBytesHooked) SetSaveBytes(cfg.saveBytesBulk, saveTimers, cfg.saveThumbnailAsync);
    }
    if (!saveBytesWanted) Log("[savestall] SaveBytesBulk off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (!saveThumbWanted) Log("[savethumb] SaveThumbnailAsync off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (saveRefIdWanted) {
        g_saveRefIdHooked = InstallSaveRefIdIndex(beforeGameCode);
        if (g_saveRefIdHooked) SetSaveRefIdIndex(cfg.saveRefIdIndex, saveTimers);
    } else Log("[saverefid] SaveRefIdIndex off in OblivionPerfFix.ini (takes a restart to switch on)");
    // RigVmHashMemo / RigLogicMapCache: ComputeVMHash's entry and InitializeRigLogic's map calls, written only before game
    // code runs, when either key is on (lab builds: always, so both can be switched on live from 0); then live. Before the
    // lab's AnimInitProbe, whose entry detours of the map functions would fail this code's byte checks.
    const bool rigWanted = cfg.rigVmHashMemo || cfg.rigLogicMapCache, bodiesWanted = cfg.npcBodiesOneLock != 0;
    if (rigWanted) g_rigSetup = InstallRigSetup(beforeGameCode);
    else Log("[rigsetup] RigVmHashMemo and RigLogicMapCache off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (g_rigSetup) SetRigSetup(cfg.rigVmHashMemo, cfg.rigLogicMapCache);
    // NpcBodiesOneLock: InitArticulated's call of InstantiatePhysicsAsset_Internal, redirected only before game code runs,
    // when the key is on (lab builds: always); then live. The lab's SceneLockWaitProbe (start-up only) below.
    if (bodiesWanted) g_npcBodies = InstallNpcBodiesOneLock(beforeGameCode);
    else Log("[npcbodies] off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (g_npcBodies) SetNpcBodiesOneLock(cfg.npcBodiesOneLock);
    // RtSceneBufferKeep: the TLAS buffer's size check and its creations, only if != 0 at start-up; then live.
    if (cfg.rtSceneBufferKeep && InstallRtSceneKeep(beforeGameCode)) SetRtSceneKeep(cfg.rtSceneBufferKeep);
    else if (!cfg.rtSceneBufferKeep) Log("[rtscene] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // NaniteInitialRootPages: r.Nanite.Streaming.NumInitialRootPages before the ini and the renderer (start-up only; on the OBSE64
    // route the plugin's start-up also runs before WinMain).
    if (cfg.naniteInitialRootPages) SetNaniteInitialRootPages(cfg.naniteInitialRootPages, beforeGameCode);
    else Log("[nanite] NaniteInitialRootPages=0: the game's 2048 root pages");
    // HdrMenuBackground (optional tweak): the menu screenshot handler's ImageResize call redirected only if on at start-up
    // (the wrapper passes through while off or not HDR10); afterwards the ini switches it live.
    if (!cfg.hdrMenuBackground) Log("[hdrbg] HdrMenuBackground off in OblivionPerfFix.ini (takes a restart to switch on)");
    else if (ApplyHdrMenuBackground(beforeGameCode)) SetHdrMenuBackground(true);
    // HdrSaveThumbnail (optional tweak): the save's screenshot callback's copy redirected only if on at start-up (the
    // wrapper only copies while off or not HDR10); afterwards the ini switches it live.
    if (!cfg.hdrSaveThumbnail) Log("[hdrthumb] HdrSaveThumbnail off in OblivionPerfFix.ini (takes a restart to switch on)");
    else if (ApplyHdrSaveThumbnail(beforeGameCode)) SetHdrSaveThumbnail(true);
    // HdrMenuRows, HudAspectRow and DlssRayReconstruction: before FrameRateLimitAtStart, whose detour of frame generation's
    // switch hides the pattern Altar's command setter is found by. No code patched; the Graphics page's rows are made from
    // NpcCreationPacing's frame hook once the settings tables exist. Only if on at startup.
    // StuckEffectLookFix (game bug #48: a status effect's look left on weapons, the quiver or a rider): an entry detour of
    // MeshComponent.SetOverlayMaterial's exec function, only if on at start-up (it needs NpcCreationPacing's frame hook for
    // its once-a-frame pass); afterwards the ini switches it live.
    if (cfg.stuckEffectLookFix) {
        if (!g_pacingHooked) Log("[stuckfx] NpcCreationPacing's frame hook is not installed: StuckEffectLookFix not installed");
        else if (InstallStuckEffectLook(beforeGameCode)) SetStuckEffectLookFix(true);
    } else Log("[stuckfx] StuckEffectLookFix off in OblivionPerfFix.ini (takes a restart to switch on)");
    // WorldSoundPositionFix (GitHub #52: NPC torch lighting heard at full volume anywhere; creature footsteps and traps without
    // direction): entry detours of the Wwise bank loaders, only if on at start-up (banks load from the start); then live.
    if (cfg.worldSoundPositionFix) {
        if (InstallWorldSoundPosition(beforeGameCode)) SetWorldSoundPositionFix(true);
    } else Log("[worldpos] WorldSoundPositionFix off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ArrowHitSoundFix (GitHub #52: an NPC's arrow hit heard twice, once at the shooter's bow): two calls in the hit code,
    // only if on at start-up; then live.
    if (cfg.arrowHitSoundFix) {
        if (!beforeGameCode) Log("[arrowhit] can only be installed at start-up (loaded late here), not installed");
        else if (ApplyArrowHitSoundFix()) SetArrowHitSoundFixEnabled(true);
    } else Log("[arrowhit] ArrowHitSoundFix off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (cfg.hdrMenuRows || cfg.dlssRayReconstruction || cfg.hudAspectRow) {
        if (!g_pacingHooked) Log("[hdrmenu] NpcCreationPacing's frame hook is not installed: HdrMenuRows / DlssRayReconstruction / HudAspectRow not installed");
        else InstallHdrMenu(cfg.hdrMenuRows, cfg.dlssRayReconstruction, cfg.hudAspectRow, beforeGameCode);
    }
    if (!cfg.hdrMenuRows) Log("[hdrmenu] HdrMenuRows off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (!cfg.dlssRayReconstruction) Log("[rrrow] DlssRayReconstruction off in OblivionPerfFix.ini (takes a restart to switch on)");
    if (!cfg.hudAspectRow) Log("[hudaspect] HudAspectRow off in OblivionPerfFix.ini (takes a restart to switch on)");
    // HdrGamma22 (trade-off): no code patched; the HDR tone curve's block is found here and refitted from
    // NpcCreationPacing's frame hook while the ini switch is on (live, also when off at startup).
    if (!g_pacingHooked) Log("[hdrgamma] NpcCreationPacing's frame hook is not installed: HdrGamma22 not installed");
    else if (InstallHdrGamma()) SetHdrGamma22(cfg.hdrGamma22, cfg.hdrGamma22White);
    // FrameRateLimitAtStart (game bug): frame generation's switch, detoured only if != 0 at start-up and while it is still off
    // (acts once, at its first switch on).
    if (cfg.frameRateLimitAtStart) InstallFrameCapStart(beforeGameCode, cfg.frameRateLimitAtStart);
    else Log("[fpscap] FrameRateLimitAtStart off in OblivionPerfFix.ini (takes a restart to switch on)");
    const uint8_t* labAddHook = nullptr;  // lab: BodyAddProbe's AddToWorld hook, shared by StreamRegisterGranularity
    // RepathParallel (repathfix; public 1.3.0): a TickActor's queued path re-searches' A* on helper threads, served only on
    // identical inputs; start-up install when != 0, then live. Lab: RepathProbe rides on its detours (installed for it too).
    InstallRepathParallel(beforeGameCode, cfg.repathParallel, cfg.repathParallelMin, cfg.repathParallelThreads, ParallelHelpersAllowed(cfg),
                          false, cfg.repathEarly, cfg.repathDefer);
    // MapPopupSpread (mapspread; public 1.3.0): the map menu's icon build (its native tables are patched before the classes
    // register them, so start-up only; needs NpcCreationPacing's pairing hooks); then live. After the lab's menu probes.
    InstallMapPopupSpread(beforeGameCode, cfg.mapPopupSpread, g_pacingHooked);
    // GpuMenuShot (gpushot; public 1.3.0): the menu backdrop's screenshot on the GPU; after SaveThumbnailAsync's read-back hooks
    // and MenuScreenshotFilter's resize hook (it needs both, and MenuOpenFlashFix's per-frame hook); start-up when != 0, then live.
    if (cfg.gpuMenuShot) {
        if (InstallGpuMenuShot()) SetGpuMenuShot(cfg.gpuMenuShot, cfg.hdrMenuBackground);
    } else Log("[gpushot] GpuMenuShot off in OblivionPerfFix.ini (takes a restart to switch on)");
    // StreamRegisterGranularity: an AddToWorld entry detour (found from the console variable's only read), only if != 0 at
    // start-up, then live 0..9 (after the lab's BodyAddProbe: shares its hook when both are on).
    // StreamInBudget (gatefix 2026-11-12) and NavQueryPriority (navprio 2026-11-18: sets its console variable on the game
    // thread at AddToWorld) share that hook: installed when any of the three keys is on.
    // PhysicsSubstepCap (physkey 2026-11-20: writes the physics settings object on the game thread at AddToWorld) too; its
    // two pattern searches run whenever the hook is installed (13 ms measured), before anything patches the FName
    // constructor, so switching the key on live works.
    if (cfg.streamRegisterGranularity || cfg.streamInBudget || cfg.navQueryPriority || cfg.physicsSubstepCap) {
        PhysicsSubstepCapFindAtStart();
        InstallStreamRegisterGranularity(beforeGameCode, labAddHook);
    }
    else Log("[streamgran] StreamRegisterGranularity, StreamInBudget, NavQueryPriority and PhysicsSubstepCap off in OblivionPerfFix.ini (takes a restart to switch on)");
    // HitchLedger's OblivionThread grid-change timer: last (the lab's CellRowDiag times the same entry and wins there).
    if (cfg.hitchLedger) InstallHitchLedgerGrid(beforeGameCode);
    // CellBufferPacedEvict: the TES constructor's buffer clamp, the grid change's entry (chained after CellRowDiag's and
    // HitchLedger's detours of the same entry, so last) and the ini writer, only if not 0 at startup; then live.
    if (cfg.cellBufferPacedEvict) {
        if (InstallCellBufferPacedEvict(beforeGameCode)) {
            SetCellBufferPacedEvict(cfg.cellBufferPacedEvict);
            SetCellBufferEvictDelay(cfg.cellBufferEvictDelayMs);  // CellBufferEvictDelayMs: from the first crossing (then live)
            // CellEvictSplit (public 1.5.0): finds only (no code written); runs inside the paced unload, so a late load,
            // where CellBufferPacedEvict stays off, never splits
            if (InstallCellEvictSplit()) SetCellEvictSplit(cfg.cellEvictSplit);
        } else if (cfg.cellEvictSplit) {
            Log("[evictsplit] CellEvictSplit=%d not available: it needs CellBufferPacedEvict, which is not installed",
                cfg.cellEvictSplit);
        }
    } else if (cfg.cellEvictSplit) {
        Log("[evictsplit] CellEvictSplit=%d not available: it needs CellBufferPacedEvict (0 in OblivionPerfFix.ini)",
            cfg.cellEvictSplit);
    }
    // PhysicsDestroyBatch: three call sites in the step's DestroyPendingProxies path, only if != 0 at start-up (then
    // live); before the lab's PhysWaitProbe, whose entry detours would fail this code's entry check.
    if (cfg.physicsDestroyBatch) {
        if (InstallPhysicsDestroyBatch(beforeGameCode, cfg.parallelMinProcessors)) {
            g_destroyBatch = true;
            SetPhysicsDestroyBatch(cfg.physicsDestroyBatch, cfg.physicsDestroyBatchMin);
        }
    } else Log("[destroybatch] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // RayTracingBuildBatch: CreateRayTracingGeometry's build request and Render's vtable slot, only if on at start-up
    // (a late load logs why not); then live.
    if (cfg.rayTracingBuildBatch) {
        if (InstallRayTracingBuildBatch(beforeGameCode)) SetRayTracingBuildBatch(true);
    } else Log("[rtbatch] RayTracingBuildBatch off in OblivionPerfFix.ini (takes a restart to switch on)");
    // PhysAnimNoEmptyLock and RigTraceReuse (the Brindle Home corral's horses): entry detours written only before the game's
    // code runs (a late load logs them as not installed), installed only if on at start-up, then
    // live.
    const bool physAnimInstall = cfg.physAnimNoEmptyLock, rigTraceInstall = cfg.rigTraceReuse != 0;
    if (physAnimInstall) {
        g_physAnimHooked = InstallPhysAnimNoEmptyLock(beforeGameCode);
        if (g_physAnimHooked) SetPhysAnimNoEmptyLock(cfg.physAnimNoEmptyLock);
    } else Log("[physanim] PhysAnimNoEmptyLock disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    if (rigTraceInstall) {
        if (InstallRigTraceReuse(beforeGameCode, g_iniPath.c_str())) SetRigTraceReuse(cfg.rigTraceReuse);
    } else Log("[rigtrace] RigTraceReuse disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // PerceptionLosPrefetch (default off): two call redirections in the NPC perception round, written only before the
    // game's code runs (version.dll route), installed only if not 0 at start-up, then live.
    if (cfg.perceptionLosPrefetch != 0) {
        if (InstallPerceptionLosPrefetch(beforeGameCode, cfg.perceptionLosPrefetchThreads, cfg.perceptionLosIgnoreCache))
            SetPerceptionLosPrefetch(cfg.perceptionLosPrefetch, cfg.perceptionLosPrefetchMin, ParallelHelpersAllowed(cfg), cfg.perceptionLosPrefetchOverlap, cfg.perceptionLosIgnoreCache, cfg.perceptionLosCrossRound);
    } else Log("[perclos] PerceptionLosPrefetch off in OblivionPerfFix.ini (takes a restart to switch on)");
    // RigImportTemplate (Altar's Control Rig: the first bone import of a new rig): the RemoveElement guard, then entry detours
    // of the node's CacheBones and ImportBones; written only before the game's code runs (version.dll route), installed only if
    // on at start-up, then live.
    bool rigTmplHooked = false;
    const bool rigTmplInstall = cfg.rigImportTemplate != 0;
    if (rigTmplInstall) {
        rigTmplHooked = InstallRigImportTemplate(beforeGameCode);
        if (rigTmplHooked) SetRigImportTemplate(cfg.rigImportTemplate);
    } else if (!cfg.rigImportTemplate) Log("[rigtmpl] RigImportTemplate off in OblivionPerfFix.ini (takes a restart to switch on)");
    (void)rigTmplHooked;
    // ChaosMaxPayloadSize: p.MaxPayloadSize before the engine reads its settings and before any physics scene (start-up,
    // both routes; only over the game's 100000; an Engine.ini value applied later wins).
    SetChaosMaxPayloadSize(cfg.chaosMaxPayloadSize, beforeGameCode);
    // FlowOverlapsOnce (otflows; public tree flowport): a pre-placed actor's overlap updates inside the level flow's adoption
    // run once per component at its end (the adoption's entry and one call in OnComponentCollisionSettingsChanged). Before
    // the lab's stream zones (whose adoption zone then stays off). Start-up install when != 0 (lab: always), then live.
    InstallFlowOverlapsOnce(beforeGameCode, cfg.flowOverlapsOnce);
    // FarPawnMoveHalfRate / FarHorseMove (farmove; public 1.3.0): calm far pawns' and horses' movement tick at FarPawnMoveHz with
    // the summed step (not identical); slot 127 of both movement components' vtables, start-up install when either key != 0
    // (needs NpcCreationPacing), then live. After the lab's HorseMoveProbe (same horse slot: horses then not covered).
    if (cfg.farPawnMoveHalfRate || cfg.farHorseMove) {
        if (g_pacingHooked)
            InstallFarPawnMove(beforeGameCode, cfg.farPawnMoveHalfRate, cfg.farPawnMoveHz, cfg.farHorseMove, cfg.farPawnMoveMinDistance,
                               cfg.farPawnMoveMaxStepMs);
        else Log("[farmove] FarPawnMoveHalfRate: needs NpcCreationPacing, not installed");
    } else Log("[farmove] FarPawnMoveHalfRate and FarHorseMove off in OblivionPerfFix.ini (takes a restart to switch on)");
    // CreatePreload (bigcreate; public 1.3.0): deferrable creates wait while their form asset / class loads asynchronously;
    // the form-asset lookup's entry detour, start-up install when != 0 (needs NpcCreationPacing: asked from its create hook).
    if (cfg.createPreload) {
        if (g_pacingHooked && InstallCreatePreload(beforeGameCode)) {
            SetCreatePreload(cfg.createPreload);
            SetCreatePreloadNear(cfg.createPreloadNear);
            SetCreatePreloadEarly(cfg.createPreloadEarly);
        } else if (!g_pacingHooked) Log("[createpreload] CreatePreload: needs NpcCreationPacing, not installed");
    } else Log("[createpreload] CreatePreload off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ImpactPreload (impactload): the impact table's hit-effect classes and the blood effect class loaded in the background
    // and kept; start-up install when != 0 (runs from NpcCreationPacing's frame-end hook), then live 0/1.
    if (cfg.impactPreload) {
        if (g_pacingHooked && InstallImpactPreload(beforeGameCode)) SetImpactPreload(cfg.impactPreload);
        else if (!g_pacingHooked) Log("[impactpreload] ImpactPreload: needs NpcCreationPacing, not installed");
    } else Log("[impactpreload] ImpactPreload off in OblivionPerfFix.ini (takes a restart to switch on)");
    // RowPreload (rowpreload): the grid change's three cell-request calls redirected; runs from CellBufferPacedEvict's grid hook
    // (installed above) and reads the menu state from NpcCreationPacing's frame-end hook; start-up when != 0, then live 0/1.
    if (cfg.rowPreload) {
        if (!g_pacingHooked) Log("[rowpreload] RowPreload: needs NpcCreationPacing, not installed");
        else if (!CellBufferPacedEvictInstalled()) Log("[rowpreload] RowPreload: needs CellBufferPacedEvict, not installed");
        else if (InstallRowPreload(beforeGameCode)) SetRowPreload(cfg.rowPreload);
    } else Log("[rowpreload] RowPreload off in OblivionPerfFix.ini (takes a restart to switch on)");
    // CellObjectPreload (preload, S-008): no patch of its own; RowPreload feeds it the cells loaded ahead (old-engine thread),
    // NpcCreationPacing's frame-end hook runs its requests, CreatePreload's readers find what is missing; start-up when != 0,
    // then live 0/1.
    if (cfg.cellObjectPreload) {
        if (!g_pacingHooked) Log("[cellpreload] CellObjectPreload: needs NpcCreationPacing, not installed");
        else if (InstallCellObjectPreload(beforeGameCode, CreatePreloadInstalled() && cfg.createPreload != 0, RowPreloadInstalled()))
            SetCellObjectPreload(cfg.cellObjectPreload);
    }
    // CreateAhead (aheadport): the spawner's SpawnActor call redirected; runs from NpcCreationPacing's create and frame-end
    // hooks, asks CreatePreload for classes, RowPreload feeds it the cells loaded ahead; start-up when != 0, then live 0/1.
    if (cfg.createAhead) {
        if (!g_pacingHooked) Log("[createahead] CreateAhead: needs NpcCreationPacing, not installed");
        else if (!cfg.createPreload) Log("[createahead] CreateAhead: needs CreatePreload, not installed");
        else if (!g_saveBytesHooked) Log("[createahead] CreateAhead: needs the save-start hook (SaveGameToMemory's call sites), not installed");
        else {
            SetCreateAheadPool(cfg.createAheadPool);  // before the install, so its log line names the pool size (rev-pool N1)
            SetCreateAheadLevelled(cfg.createAheadLevelled);
            if (InstallCreateAhead(beforeGameCode)) SetCreateAhead(cfg.createAhead, cfg.createAheadSlackMs);
        }
    }
    // OtFrameTrim (fightcost; public 1.3.0): the OT frame-start sleep's call and the node profiler timer's entry; start-up
    // install when != 0, then live 0..3.
    if (cfg.otFrameTrim) InstallOtFrameTrim(beforeGameCode, cfg.otFrameTrim);
    else Log("[otframe] OtFrameTrim off in OblivionPerfFix.ini (takes a restart to switch on)");
    // LooseMissCache (fightot2; public 1.3.0): the file finder's two _access calls; start-up install when != 0, then live.
    if (cfg.looseMissCache) InstallLooseMissCache(beforeGameCode, cfg.looseMissCache);
    else Log("[loosemiss] LooseMissCache off in OblivionPerfFix.ini (takes a restart to switch on)");
    // LumenCardsNarrow (lumenfixa; public 1.3.0): the Lumen mapping upload narrowed to the changed instances (identical); start-up
    // install when != 0, then live. Before the lab's LumenCardsProbe: it reads the call the probe redirects.
    if (InstallLumenCardsNarrow(beforeGameCode, cfg.lumenCardsNarrow)) g_lumenNarrowHooked = true;
    // LumenScanCull (lumenscan; public since 1.4.0): Lumen's CPU primitive scan reads only the blocks of groups that can emit a
    // request (identical; the idea is Epic's, UE 5.6.0); after LumenCardsNarrow (shares its lists' reset hook; without it, a
    // reset-only patch at the same site); start-up install when != 0, then live 0 / 1 / 2.
    if (InstallLumenScanCull(beforeGameCode, cfg.lumenScanCull)) g_lumenScanHooked = true;
    // PowerAttackAnimKeep (humananim): the old engine's animation release leaves a power-attack file in its map at count 0
    // when the last holder lets go (8 bytes in the release's tail); start-up install when != 0 (lab: always), then live.
    InstallPowerAttackAnimKeep(beforeGameCode, cfg.powerAttackAnimKeep);
    // GcPassAfterBusy (gchold): a due timer GC pass waits through busy frames with the engine's own one-frame delay, up to
    // a bound; one 6-byte call on ConditionalCollectGarbage's timer path. Start-up when != 0 (lab: always), then live.
    // Before the lab's SpikeStages, which puts a jump on the function's entry (rev98c M1; the discovery also accepts that jump).
    // GcPrepAhead (gcprepbuild) runs from the same hook, so it is installed when either key is on (for GcPrepAhead only
    // once its own code and the GC lock were found: no hook with nothing to do; review N2).
    InstallGcPassAfterBusy(beforeGameCode, cfg.gcPassAfterBusy, cfg.gcPrepAhead != 0 && beforeGameCode && GcPrepAheadFindable());
    // GcPrepAhead (gcprepbuild, S-041): the engine's PrepareStreamedOutLevelForGC for the oldest pending levels in calm frames
    // of the last second before a GC pass, from GcPassAfterBusy's hook (no code patched of its own). Start-up when != 0 (lab:
    // always), then live.
    InstallGcPrepAhead(beforeGameCode, cfg.gcPrepAhead);
    // CapSleepSkip (repathdefer): the cap's wait before a frame known to carry a GC pass or a cell-removal step is shortened;
    // one 6-byte call in UpdateTimeAndHandleMaxTickRate, written at start-up only when != 0; then live. After NpcCreationPacing
    // (its cap hook decides) and GcPassAfterBusy (its readings: the GC timer, FApp::DeltaTime) are installed (repathdefertest:
    // installed before GcPassAfterBusy it always found it missing and was never installed).
    if (g_pacingHooked && InstallCapSleepSkip(beforeGameCode, cfg.capSleepSkip)) SetCapSleepSkip(cfg.capSleepSkip, cfg.capSleepSkipMaxMs, cfg.capSleepSkipAddBack);
    // GcMarkPriority (gc104): ProcessAsync's two ProcessSync pointers (4-byte lea displacements) go through our wrapper, which
    // raises a below-normal task worker for its mark part. Start-up when != 0 (lab: always, the lab GcMarkProbe watches it).
    InstallGcMarkPriority(beforeGameCode, cfg.gcMarkPriority);
    // BodyRemovalSpread: a streamed-out level's instance bodies terminated over frames; three call sites and the physics
    // scene's deleting-destructor slot, installed at start-up when != 0 (lab: always) and NpcCreationPacing's frame hooks
    // are in (it drains at their post-actor tick), then live. After the lab's probes (as in the lab, where it was first run).
    InstallBodyRemovalSpread(beforeGameCode, cfg.bodyRemovalSpread, cfg.bodyRemovalSpreadPerFrame, cfg.bodyRemovalSpreadMaxFrames,
                             cfg.bodyRemovalSpreadMin, cfg.bodyRemovalSpreadNearM, g_pacingHooked, g_iniPath.c_str());
    // StreamOutEndPlaySlice (epicport; Epic UE 5.6.0 dfeb9ecce7): a streamed-out level's EndPlay in batches inside
    // RemoveFromWorld's time limit; chained after BodyRemovalSpread on the same call, so installed after it. Start-up when
    // != 0 (lab: always), then live.
    InstallStreamOutEndPlaySlice(beforeGameCode, cfg.streamOutEndPlaySlice);
    // ResetLoadersNoFlush (epicport; Epic UE 5.4.0 0d790f0ba5): ResetLoaders of a package without a loader linker does not
    // wait for every load in flight (the bow-draw rename). Start-up when on (lab: always, counting), then live.
    InstallResetLoadersNoFlush(beforeGameCode, cfg.resetLoadersNoFlush);
    // StartupBarFinish (loader106; S-058): the old engine's loading-bar finish loops (LoadingMenu's destructor, the load's
    // finish) through a stub at their step call; key 1 ends the first loading menu's loop (start-up) at its first step.
    // Start-up when != 0 (lab: always, timing the loops), then live.
    InstallStartupBarFinish(beforeGameCode, cfg.startupBarFinish);
    // TopicSort (gapfix; S-058): the old engine's dialogue-topic list sort (a bubble sort) runs as a stable merge sort with
    // the game's own _stricmp (identical order). Start-up when != 0 (lab: always, timing the sorts), then live.
    InstallTopicSort(beforeGameCode, cfg.topicSort);
    // InstanceBufferFloor: the GPU scene instance buffer at 192 MiB at its first large need (inside GpuSceneKeepCapacity's reserve;
    // no patch of its own; the lab build also patches lab SceneBufferFloorMiB and times the buffer's creations); start-up only.
    InstallBufferFloor(beforeGameCode, cfg.instanceBufferFloor, g_gpuSceneHooked, g_iniPath.c_str());
    // PairReleaseOnce (game bug fix): RunSession's and the drain's early release of a pairing session's buffer ids go to
    // counting stubs (each id released once, by the destructor); start-up only. Lab: also PairRaceProbe (own key).
    InstallPairRace(beforeGameCode, cfg.pairReleaseOnce, g_iniPath.c_str());

    {
        // Start-up cost: the whole Init and its pattern searches (OBSE route: the game's main thread waits for it).
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const ScanStats scan = GetScanStats();
        FILETIME created, exited, kernel, user, wall;
        unsigned long long sinceStart = 0;  // ms since the process was created
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            GetSystemTimePreciseAsFileTime(&wall);
            const auto u64 = [](const FILETIME& f) { return static_cast<unsigned long long>(f.dwHighDateTime) << 32 | f.dwLowDateTime; };
            if (u64(wall) > u64(created)) sinceStart = (u64(wall) - u64(created)) / 10000;
        }
        Log("[startup] fixes set up in %llu ms (%llu ms after the process started): %llu pattern searches over %llu MB took %llu ms%s",
            QpcToMicroseconds(static_cast<uint64_t>(now.QuadPart - initStart.QuadPart), static_cast<uint64_t>(freq.QuadPart)) / 1000,
            sinceStart, scan.calls, scan.bytes >> 20, scan.microseconds / 1000,
            scan.mismatches ? " (ScanVerify: MISMATCHES, see above)" : "");
        if (const uint64_t hooks = ForeignHooksMet())
            Log("[hook] %llu other mods' hook(s) met while setting up (the [hook] lines above)", static_cast<unsigned long long>(hooks));
        if (const uint64_t retries = CodeWriteRetries())
            Log("[hook] %llu code write(s) retried: the page's protection changed under them (another mod patching at the same time)", static_cast<unsigned long long>(retries));
    }
    // From here on a background thread writes the log (before: the thread that logged, often the game thread, waited for
    // each line's disk write); start-up above stays synchronous. Also guards the exe's TerminateProcess import.
    LogGoAsync(game.base);
    if (HANDLE t = CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace
}  // namespace opf

// ---- OBSE64 plugin interface (declared here to avoid depending on OBSE64 headers) ----
struct OBSEInterface {
    uint32_t obse64Version;
    uint32_t runtimeVersion;
    uint32_t editorVersion;
    uint32_t isEditor;
    // further members unused
};

struct OBSEPluginVersionData {
    enum { kVersion = 1 };
    enum { kAddressIndependence_Signatures = 1 << 0 };
    enum { kStructureIndependence_NoStructs = 1 << 0 };
    uint32_t dataVersion;
    uint32_t pluginVersion;
    char name[256];
    char author[256];
    uint32_t addressIndependence;
    uint32_t structureIndependence;
    uint32_t compatibleVersions[16];
    uint32_t seVersionRequired;
    uint32_t reservedNonBreaking;
    uint32_t reservedBreaking;
    uint8_t reserved[512];
};

extern "C" {
__declspec(dllexport) OBSEPluginVersionData OBSEPlugin_Version = {
    OBSEPluginVersionData::kVersion,
    0x010600,  // plugin version 1.6.0
    "OblivionPerfFix",
    "OblivionPerfFix",
    // Addresses come from byte-pattern scans and we use no OBSE game structures,
    // so the plugin is version independent: OBSE64 loads it on any game version.
    OBSEPluginVersionData::kAddressIndependence_Signatures,
    OBSEPluginVersionData::kStructureIndependence_NoStructs,
    {0},
    0,
    0,
    0,
    {},
};

__declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse)
{
    if (obse && obse->isEditor) return false;
    opf::Init("OBSE64 plugin", true, opf::LoadStage::BeforeWinMain);  // checked in Init
    return true;
}
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH) {
        // the game's normal end (ExitProcess, reserved != null: every other thread has ended): the log's last lines
        opf::LogProcessExit(reserved != nullptr);
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        opf::g_self = module;
        DisableThreadLibraryCalls(module);
        if (_wcsicmp(opf::FileName(opf::ModulePath(module)).c_str(), L"version.dll") == 0) {
            const bool proxyOk = opf::ResolveVersionProxy();
            // Runs before the game's own code: the patches are plain code/vtable writes with no
            // dependency on game state, so they are safe to apply here.
            opf::Init("version.dll proxy", proxyOk, opf::LoadStage::InDllMain);
        }
    }
    return TRUE;
}
