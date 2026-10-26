// Reads OblivionPerfFix.ini (next to the DLL) into a Config. A missing key keeps its default from common.h.
// main.cpp calls this at start-up and then every second (MonitorThread), so most fixes can be switched live.
#include "common.h"

namespace opf {

Config LoadConfig(const wchar_t* iniPath)
{
    Config c;
    auto get = [&](const wchar_t* section, const wchar_t* key, int def) {
        return static_cast<int>(GetPrivateProfileIntW(section, key, def, iniPath));
    };
    c.boneUploadStores = get(L"Fixes", L"BoneUploadStores", c.boneUploadStores) != 0;
    c.atmosphereDirtyFilter = get(L"Fixes", L"AtmosphereDirtyFilter", c.atmosphereDirtyFilter) != 0;
    c.atmosphereLightFastPath = get(L"Fixes", L"AtmosphereLightFastPath", c.atmosphereLightFastPath) != 0;
    c.atmosphereLightCloudPath = get(L"Fixes", L"AtmosphereLightCloudPath", c.atmosphereLightCloudPath) != 0;
    c.rdgParallelTranslate = get(L"Fixes", L"RdgParallelTranslate", c.rdgParallelTranslate);
    c.rdgFrameFence = get(L"Fixes", L"RdgFrameFence", c.rdgFrameFence) != 0;
    c.viewRenameGuard = get(L"Fixes", L"ViewRenameGuard", c.viewRenameGuard);
    c.rdgSubmitBatch = get(L"Fixes", L"RdgSubmitBatch", c.rdgSubmitBatch);
    c.rdgSubmitMaxAgeUs = get(L"Fixes", L"RdgSubmitMaxAgeUs", c.rdgSubmitMaxAgeUs);
    c.clothConfigCache = get(L"Fixes", L"ClothConfigCache", c.clothConfigCache) != 0;
    c.frozenClothStretchFix = get(L"Fixes", L"FrozenClothStretchFix", c.frozenClothStretchFix) != 0;
    c.npcFadeInNoRebuild = get(L"Fixes", L"NpcFadeInNoRebuild", c.npcFadeInNoRebuild) != 0;
    c.prepareCollisionBehindLoadingScreen =
        get(L"Fixes", L"PrepareCollisionBehindLoadingScreen", c.prepareCollisionBehindLoadingScreen) != 0;
    c.editorIdLookupGuard = get(L"Fixes", L"NameLookupCrashGuard", c.editorIdLookupGuard) != 0;
    c.trapCrashGuard = get(L"Fixes", L"TrapCrashGuard", c.trapCrashGuard) != 0;
    c.frameGenCrashGuard = get(L"Fixes", L"FrameGenCrashGuard", c.frameGenCrashGuard) != 0;
    c.objectReplaceGuard = get(L"Fixes", L"ObjectReplaceCrashGuard", c.objectReplaceGuard);
    c.taskWaitDeadlockGuard = get(L"Fixes", L"TaskWaitDeadlockGuard", c.taskWaitDeadlockGuard);
    c.psoCompileThreadPool = get(L"Fixes", L"PsoCompileThreadPool", c.psoCompileThreadPool);
    c.psoCompileThreadPoolPriority = get(L"Fixes", L"PsoCompileThreadPoolPriority", c.psoCompileThreadPoolPriority);
    c.frameGenAsyncCompute = get(L"Fixes", L"FrameGenAsyncCompute", c.frameGenAsyncCompute) != 0;
    c.frameGenCameraInfo = get(L"Fixes", L"FrameGenCameraInfo", c.frameGenCameraInfo) != 0;
    c.frameGenPacingTuning = get(L"Fixes", L"FrameGenPacingTuning", c.frameGenPacingTuning);
    c.displayModeCache = get(L"Fixes", L"DisplayModeCache", c.displayModeCache) != 0;
    c.screenshotFilterFast = get(L"Fixes", L"MenuScreenshotFilter", c.screenshotFilterFast) != 0;
    c.screenshotReadbackConvert = get(L"Fixes", L"ScreenshotReadbackConvert", c.screenshotReadbackConvert);
    c.screenshotParallel = get(L"Fixes", L"ScreenshotParallel", c.screenshotParallel);
    c.restoreFsrAfterAltTab = get(L"Fixes", L"RestoreFsrAfterAltTab", c.restoreFsrAfterAltTab) != 0;
    c.lumenKeepArrayCapacity = get(L"Fixes", L"LumenKeepArrayCapacity", c.lumenKeepArrayCapacity) != 0;
    c.uploadMemoryKeep = get(L"Fixes", L"UploadMemoryKeep", c.uploadMemoryKeep) != 0;
    c.uploadMemoryKeepFrames = get(L"Fixes", L"UploadMemoryKeepFrames", c.uploadMemoryKeepFrames);
    c.uploadPagesToKeep = get(L"Fixes", L"UploadPagesToKeep", c.uploadPagesToKeep);
    c.uploadReleaseRampFrames = get(L"Fixes", L"UploadReleaseRampFrames", c.uploadReleaseRampFrames);
    c.physicsTreeTimeSlicing = get(L"Fixes", L"PhysicsTreeTimeSlicing", c.physicsTreeTimeSlicing) != 0;
    c.physicsSliceIterations = get(L"Fixes", L"PhysicsSliceIterations", c.physicsSliceIterations);
    c.physicsSliceCopyKB = get(L"Fixes", L"PhysicsSliceCopyKB", c.physicsSliceCopyKB);
    c.physicsSliceAdaptive = get(L"Fixes", L"PhysicsSliceAdaptive", c.physicsSliceAdaptive) != 0;
    c.physicsFlushParallel = get(L"Fixes", L"PhysicsFlushParallel", c.physicsFlushParallel) != 0;
    c.physicsFlushMinChanges = get(L"Fixes", L"PhysicsFlushMinChanges", c.physicsFlushMinChanges);
    c.instanceClearReserve = get(L"Fixes", L"InstanceClearReserve", c.instanceClearReserve) != 0;
    c.parallelStaticBodies = get(L"Fixes", L"ParallelStaticBodies", c.parallelStaticBodies) != 0;
    c.parallelStaticBodiesMin = get(L"Fixes", L"ParallelStaticBodiesMin", c.parallelStaticBodiesMin);
    c.parallelStaticBodiesThreads = get(L"Fixes", L"ParallelStaticBodiesThreads", c.parallelStaticBodiesThreads);
    c.parallelMinProcessors = get(L"Fixes", L"ParallelMinProcessors", c.parallelMinProcessors);
    c.pipelinedSceneAdd = get(L"Fixes", L"PipelinedSceneAdd", c.pipelinedSceneAdd) != 0;
    c.pipelinedSceneAddMin = get(L"Fixes", L"PipelinedSceneAddMin", c.pipelinedSceneAddMin);
    c.dirtyTreeQuickInsert = get(L"Fixes", L"DirtyTreeQuickInsert", c.dirtyTreeQuickInsert) != 0;
    c.helperThreadCaches = get(L"Fixes", L"HelperThreadCaches", c.helperThreadCaches) != 0;
    c.bulkIndexInsert = get(L"Fixes", L"BulkIndexInsert", c.bulkIndexInsert) != 0;
    c.bulkIndexInsertMin = get(L"Fixes", L"BulkIndexInsertMin", c.bulkIndexInsertMin);
    c.physicsInternalFlushParallel = get(L"Fixes", L"PhysicsInternalFlushParallel", c.physicsInternalFlushParallel) != 0;
    c.physicsQueueShrinkOnce = get(L"Fixes", L"PhysicsQueueShrinkOnce", c.physicsQueueShrinkOnce) != 0;
    c.physicsDestroyBatch = get(L"Fixes", L"PhysicsDestroyBatch", c.physicsDestroyBatch);
    c.physicsDestroyBatchMin = get(L"Fixes", L"PhysicsDestroyBatchMin", c.physicsDestroyBatchMin);
    c.npcBodiesOneLock = get(L"Fixes", L"NpcBodiesOneLock", c.npcBodiesOneLock);
    c.clothRegisterLite = get(L"Fixes", L"ClothRegisterLite", c.clothRegisterLite);
    c.rigVmHashMemo = get(L"Fixes", L"RigVmHashMemo", c.rigVmHashMemo);
    c.rigLogicMapCache = get(L"Fixes", L"RigLogicMapCache", c.rigLogicMapCache);
    c.rtSceneBufferKeep = get(L"Fixes", L"RtSceneBufferKeep", c.rtSceneBufferKeep);
    c.rayTracingBuildBatch = get(L"Fixes", L"RayTracingBuildBatch", c.rayTracingBuildBatch) != 0;
    c.waterCaptureFsrSkip = get(L"Fixes", L"WaterCaptureFsrSkip", c.waterCaptureFsrSkip) != 0;
    c.waterCaptureNoRhiFlush = get(L"Fixes", L"WaterCaptureNoRhiFlush", c.waterCaptureNoRhiFlush) != 0;
    c.waterCaptureNoOcclusionWait = get(L"Fixes", L"WaterCaptureNoOcclusionWait", c.waterCaptureNoOcclusionWait) != 0;
    c.waterCaptureFastMeshScan = get(L"Fixes", L"WaterCaptureFastMeshScan", c.waterCaptureFastMeshScan);
    c.naniteInitialRootPages = get(L"Fixes", L"NaniteInitialRootPages", c.naniteInitialRootPages);
    c.frameRateLimitAtStart = get(L"Fixes", L"FrameRateLimitAtStart", c.frameRateLimitAtStart);
    c.navLinkFast = get(L"Fixes", L"NavLinkFast", c.navLinkFast);
    c.saveBytesBulk = get(L"Fixes", L"SaveBytesBulk", c.saveBytesBulk);
    c.saveRefIdIndex = get(L"Fixes", L"SaveRefIdIndex", c.saveRefIdIndex);
    c.saveThumbnailAsync = get(L"Fixes", L"SaveThumbnailAsync", c.saveThumbnailAsync);
    // The public build has one ClothRegisterLite mode (3) and no verify / check / log-only modes (lab 2 and NavLinkFast's 3):
    // other values read as on.
    if (c.clothRegisterLite) c.clothRegisterLite = 3;
    if (c.rigVmHashMemo) c.rigVmHashMemo = 1;
    if (c.rigLogicMapCache) c.rigLogicMapCache = 1;
    if (c.rtSceneBufferKeep != 0 && c.rtSceneBufferKeep != 2) c.rtSceneBufferKeep = 1;
    if (c.waterCaptureFastMeshScan) c.waterCaptureFastMeshScan = 1;
    if (c.frameRateLimitAtStart) c.frameRateLimitAtStart = 1;
    if (c.navLinkFast) c.navLinkFast = 1;
    if (c.saveBytesBulk) c.saveBytesBulk = 1;
    if (c.saveRefIdIndex) c.saveRefIdIndex = 1;
    if (c.saveThumbnailAsync) c.saveThumbnailAsync = 1;
    if (c.taskWaitDeadlockGuard) c.taskWaitDeadlockGuard = 1;          // lab 2 (measure only) reads as on
    if (c.psoCompileThreadPool < -1) c.psoCompileThreadPool = -1;      // 0 the game's, N threads, -1 automatic
    c.osPageCache = get(L"Fixes", L"OsPageCache", c.osPageCache);
    c.osPageCacheMB = get(L"Fixes", L"OsPageCacheMB", c.osPageCacheMB);
    c.osPageCacheTrimSeconds = get(L"Fixes", L"OsPageCacheTrimSeconds", c.osPageCacheTrimSeconds);
    c.psoPrecompileFast = get(L"Fixes", L"PsoPrecompileFast", c.psoPrecompileFast) != 0;
    c.psoPrecompileFastInGameMenus = get(L"Fixes", L"PsoPrecompileFastInGameMenus", c.psoPrecompileFastInGameMenus) != 0;
    c.psoPrecompileFastBudgetMs = get(L"Fixes", L"PsoPrecompileFastBudgetMs", c.psoPrecompileFastBudgetMs);
    c.psoPrecompileColdQueue = get(L"Fixes", L"PsoPrecompileColdQueue", c.psoPrecompileColdQueue);
    c.animTeardownParallel = get(L"Fixes", L"AnimTeardownParallel", c.animTeardownParallel);
    c.cellBufferPacedEvict = get(L"Fixes", L"CellBufferPacedEvict", c.cellBufferPacedEvict);
    c.physicsPushParallel = get(L"Fixes", L"PhysicsPushParallel", c.physicsPushParallel);
    c.physicsPushMin = get(L"Fixes", L"PhysicsPushMin", c.physicsPushMin);
    c.physicsPushThreads = get(L"Fixes", L"PhysicsPushThreads", c.physicsPushThreads);
    c.pipelinedBodyCreation = get(L"Fixes", L"PipelinedBodyCreation", c.pipelinedBodyCreation) != 0;
    c.gpuSceneParallelUpload = get(L"Fixes", L"GpuSceneParallelUpload", c.gpuSceneParallelUpload) != 0;
    c.hitchLedger = get(L"Fixes", L"HitchLedger", c.hitchLedger) != 0;
    c.hitchLedgerMs = get(L"Fixes", L"HitchLedgerMs", c.hitchLedgerMs);
    c.npcCreationPacing = get(L"Fixes", L"NpcCreationPacing", c.npcCreationPacing) != 0;
    c.pacingBudgetMs = get(L"Fixes", L"PacingBudgetMs", c.pacingBudgetMs);
    c.pacingMinDistance = get(L"Fixes", L"PacingMinDistance", c.pacingMinDistance);
    c.pacingMaxLagFrames = get(L"Fixes", L"PacingMaxLagFrames", c.pacingMaxLagFrames);
    c.pacingUseSlack = get(L"Fixes", L"PacingUseSlack", c.pacingUseSlack) != 0;
    c.pacingFlowWait = get(L"Fixes", L"PacingFlowWait", c.pacingFlowWait) != 0;
    c.pacingFlowWaitFrames = get(L"Fixes", L"PacingFlowWaitFrames", c.pacingFlowWaitFrames);
    c.memoryRelief = get(L"Fixes", L"MemoryRelief", c.memoryRelief) != 0;
    c.memoryReliefRamMB = get(L"Fixes", L"MemoryReliefRamMB", c.memoryReliefRamMB);
    c.memoryReliefCommitMB = get(L"Fixes", L"MemoryReliefCommitMB", c.memoryReliefCommitMB);
    c.vramRelief = get(L"Fixes", L"VramRelief", c.vramRelief) != 0;
    c.vramReliefPauseTranslate = get(L"Fixes", L"VramReliefPauseTranslate", c.vramReliefPauseTranslate) != 0;
    c.navmeshBuildNoBusyWait = get(L"Fixes", L"NavmeshBuildNoBusyWait", c.navmeshBuildNoBusyWait) != 0;
    c.busyWaitFlagCheck = get(L"Fixes", L"BusyWaitFlagCheck", c.busyWaitFlagCheck) != 0;
    c.gpuSceneKeepCapacity = get(L"Fixes", L"GpuSceneKeepCapacity", c.gpuSceneKeepCapacity) != 0;
    c.commandAllocatorTrim = get(L"Fixes", L"CommandAllocatorTrim", c.commandAllocatorTrim) != 0;
    c.allocatorTrimWindowSeconds = get(L"Fixes", L"AllocatorTrimWindowSeconds", c.allocatorTrimWindowSeconds);
    c.immediateAllocatorLists = get(L"Fixes", L"ImmediateAllocatorLists", c.immediateAllocatorLists);
    c.allocatorTrimHeadroomMB = get(L"Fixes", L"AllocatorTrimHeadroomMB", c.allocatorTrimHeadroomMB);
    c.logFile = get(L"General", L"LogFile", c.logFile) != 0;
    c.gpuCrashReport = get(L"General", L"GpuCrashReport", c.gpuCrashReport) != 0;
    c.statsIntervalSeconds = get(L"General", L"StatsIntervalSeconds", c.statsIntervalSeconds);
    return c;
}

}  // namespace opf
