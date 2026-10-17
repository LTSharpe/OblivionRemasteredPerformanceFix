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
    c.prepareCollisionBehindLoadingScreen =
        get(L"Fixes", L"PrepareCollisionBehindLoadingScreen", c.prepareCollisionBehindLoadingScreen) != 0;
    c.editorIdLookupGuard = get(L"Fixes", L"NameLookupCrashGuard", c.editorIdLookupGuard) != 0;
    c.trapCrashGuard = get(L"Fixes", L"TrapCrashGuard", c.trapCrashGuard) != 0;
    c.frameGenCrashGuard = get(L"Fixes", L"FrameGenCrashGuard", c.frameGenCrashGuard) != 0;
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
    c.osPageCache = get(L"Fixes", L"OsPageCache", c.osPageCache);
    c.osPageCacheMB = get(L"Fixes", L"OsPageCacheMB", c.osPageCacheMB);
    c.osPageCacheTrimSeconds = get(L"Fixes", L"OsPageCacheTrimSeconds", c.osPageCacheTrimSeconds);
    c.psoPrecompileFast = get(L"Fixes", L"PsoPrecompileFast", c.psoPrecompileFast) != 0;
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
    c.gpuCrashReport = get(L"General", L"GpuCrashReport", c.gpuCrashReport) != 0;
    c.statsIntervalSeconds = get(L"General", L"StatsIntervalSeconds", c.statsIntervalSeconds);
    return c;
}

}  // namespace opf
