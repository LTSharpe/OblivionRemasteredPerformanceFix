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
std::atomic<bool> g_flushHooked{false};      // PhysicsFlushParallel installed
std::atomic<bool> g_flushTried{false};       // PhysicsFlushParallel looked for while the game runs (switched on after start-up)
std::atomic<bool> g_clearHooked{false};      // InstanceClearReserve installed
std::atomic<bool> g_gpuSceneHooked{false};   // GpuSceneKeepCapacity installed
std::atomic<bool> g_allocTrimHooked{false};  // CommandAllocatorTrim installed
std::atomic<bool> g_navLinkHooked{false};    // NavLinkFast installed
std::atomic<bool> g_saveBytesHooked{false};  // SaveBytesBulk / SaveThumbnailAsync (lab: and SaveStallTimers) installed
std::atomic<bool> g_saveRefIdHooked{false};  // SaveRefIdIndex installed

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

bool TranslateWanted(const Config& cfg)
{
    if (cfg.rdgParallelTranslate != 2) return cfg.rdgParallelTranslate != 0;
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

DWORD WINAPI MonitorThread(LPVOID)
{
    for (unsigned seconds = 1;; ++seconds) {
        Sleep(1000);
        const Config cfg = LoadConfig(g_iniPath.c_str());
        // MemoryRelief: while Windows is short of memory, the fixes that keep memory on purpose give it back.
        MemoryRelief relief;
        UpdateMemory(cfg, &relief);
        SetAtmosphereDirtyFilterEnabled(cfg.atmosphereDirtyFilter);
        SetAtmosphereLightFastPathEnabled(cfg.atmosphereLightFastPath);
        SetAtmosphereLightCloudPathEnabled(cfg.atmosphereLightCloudPath);
        if (g_viewGuardHooked) SetViewRenameGuardMode(ViewGuardMode(cfg));
        if (FrameFenceWanted(cfg) && !FrameFenceAvailable() && !g_frameFenceTried.exchange(true) && FindFrameFence())
            InstallFrameFence(false);  // switched on while the game runs (the detour is a single atomic write)
        SetRdgUseFrameFence(FrameFenceWanted(cfg));
        SetRdgParallelTranslateEnabled(TranslateWanted(cfg) && !relief.translate);
        // Submissions are only batched while parallel translate produces many of them.
        SetRdgSubmitBatch(RdgParallelTranslateActive() ? cfg.rdgSubmitBatch : 0, cfg.rdgSubmitMaxAgeUs);
        CheckRdgSubmitBatching();
        SetClothConfigCacheEnabled(cfg.clothConfigCache);
        if (g_frozenClothHooked) SetFrozenClothStretchFix(cfg.frozenClothStretchFix);
        if (g_fadeInHooked) SetNpcFadeInNoRebuild(cfg.npcFadeInNoRebuild);
        if (g_collisionHooked) SetCollisionPrepareEnabled(cfg.prepareCollisionBehindLoadingScreen);
        SetEditorIdLookupGuardEnabled(cfg.editorIdLookupGuard);
        SetTrapTargetGuardEnabled(cfg.trapCrashGuard);
        SetHavokPickWarningGuardEnabled(cfg.havokPickWarningGuard);
        SetFrameGenCrashGuardEnabled(cfg.frameGenCrashGuard);
        SetObjectReplaceGuardMode(cfg.objectReplaceGuard);
        SetTaskWaitGuardMode(cfg.taskWaitDeadlockGuard);  // no-op unless installed at start-up
        PsoCompileThreadPoolTick(cfg.psoCompileThreadPool, cfg.psoCompileThreadPoolPriority);  // start-up only: later changes are only logged
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
        SetReadbackConvertMode(cfg.screenshotReadbackConvert);  // ScreenshotReadbackConvert: no-op unless installed at start-up
        SetScreenshotParallel(cfg.screenshotParallel);          // ScreenshotParallel: no-op unless installed at start-up
        if (g_focusHooked) SetFocusRestoreEnabled(cfg.restoreFsrAfterAltTab);
        if (g_lumenHooked) SetLumenKeepCapacityEnabled(cfg.lumenKeepArrayCapacity && !relief.lumen);
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
        SetHitchLedger(cfg.hitchLedger, cfg.hitchLedgerMs);  // diagnostic: no-op unless installed at start-up
        HitchLedgerDrain();                                  // ... its slow-frame records into OblivionPerfFix-ledger.csv
        if (g_clothRegHooked) SetClothRegisterLite(cfg.clothRegisterLite);  // ClothRegisterLite: live once installed
        if (g_bodiesHooked) SetParallelStaticBodies(ParallelHelpersAllowed(cfg) && cfg.parallelStaticBodies, cfg.parallelStaticBodiesMin);
        SetHelperThreadCaches(cfg.helperThreadCaches);  // no-op when the set-up was not found
        if (g_sceneAddHooked) SetPipelinedSceneAdd(ParallelHelpersAllowed(cfg) && cfg.pipelinedSceneAdd, cfg.pipelinedSceneAddMin);
        SetPipelinedBodyCreation(ParallelHelpersAllowed(cfg) && cfg.pipelinedBodyCreation);  // PipelinedBodyCreation: no-op unless found with ParallelStaticBodies installed
        SetGpuSceneParallelUpload(cfg.gpuSceneParallelUpload);  // GpuSceneParallelUpload: a console variable, set once registered
        SetDirtyTreeQuickInsert(cfg.dirtyTreeQuickInsert);  // no-op unless installed at start-up
        SetBulkIndexInsert(cfg.bulkIndexInsert, cfg.bulkIndexInsertMin, cfg.physicsInternalFlushParallel);  // BulkIndexInsert: no-op unless installed
        SetPhysicsQueueShrinkOnce(cfg.physicsQueueShrinkOnce);  // PhysicsQueueShrinkOnce: no-op unless installed with BulkIndexInsert
        if (g_destroyBatch) SetPhysicsDestroyBatch(cfg.physicsDestroyBatch, cfg.physicsDestroyBatchMin);  // PhysicsDestroyBatch: live once installed
        if (g_rigSetup) SetRigSetup(cfg.rigVmHashMemo, cfg.rigLogicMapCache);  // RigVmHashMemo / RigLogicMapCache: live once installed
        if (g_npcBodies) SetNpcBodiesOneLock(cfg.npcBodiesOneLock);  // NpcBodiesOneLock: live once installed
        SetRtSceneKeep(cfg.rtSceneBufferKeep);  // RtSceneBufferKeep: live once installed (no-op otherwise)
        SetOsPageCache(cfg.osPageCache, cfg.osPageCacheMB, cfg.osPageCacheTrimSeconds, relief.osCache);  // OsPageCache: no-op unless installed
        SetPsoPrecompileFast(cfg.psoPrecompileFast);  // PsoPrecompileFast: no-op unless installed at start-up
        SetPsoPrecompileFastInGameMenus(cfg.psoPrecompileFastInGameMenus);
        SetPsoPrecompileFastBudget(static_cast<float>(cfg.psoPrecompileFastBudgetMs < 1 ? 1 : cfg.psoPrecompileFastBudgetMs),
                                   cfg.psoPrecompileColdQueue < 0 ? 0 : cfg.psoPrecompileColdQueue);
        PsoPrecompileWatchDriver();  // the cold-cache guard's driver-thread reading (no-op unless installed and precompiling)
        SetAnimTeardownParallel(ParallelHelpersAllowed(cfg) ? cfg.animTeardownParallel : 0);    // AnimTeardownParallel: no-op unless installed at start-up
        SetCellBufferPacedEvict(cfg.cellBufferPacedEvict);    // CellBufferPacedEvict: no-op unless installed at start-up
        SetPhysicsPushParallel(ParallelHelpersAllowed(cfg) ? cfg.physicsPushParallel : 0, cfg.physicsPushMin);  // PhysicsPushParallel: no-op unless installed
        if (g_navmeshFound) SetNavmeshBuildNoBusyWait(cfg.navmeshBuildNoBusyWait);
        if (g_busyWaitFound) SetBusyWaitFlagCheck(cfg.busyWaitFlagCheck);
        if (g_gpuSceneHooked) SetGpuSceneKeepCapacityEnabled(cfg.gpuSceneKeepCapacity);
        if (g_allocTrimHooked) {
            SetAllocatorTrim(cfg.commandAllocatorTrim, cfg.allocatorTrimWindowSeconds);
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
            if (g_rigSetup) LogRigSetupStats();
            if (g_npcBodies) LogNpcBodiesOneLockStats();
            LogRtSceneKeepStats();
            LogWaterCaptureStats();
            if (g_navLinkHooked) LogNavLinkFastStats();
            LogEditorIdLookupGuardStats();
            LogTrapTargetGuardStats();
            LogHavokPickWarningGuardStats();
            LogFrameGenCrashGuardStats();
            LogObjectReplaceGuardStats();
            LogTaskWaitGuardStats();
            LogFrameGenAsyncComputeState();
            LogFrameGenCameraInfoStats();
            LogFrameGenPacingStats();
            LogDisplayModeCacheStats();
            LogImageResizeStats();
            LogReadbackConvertStats();
            LogScreenshotParallelStats();
            LogOsPageCacheStats();
            LogPsoPrecompileStats();
            LogAnimTeardownParallelStats();
            LogCellBufferPacedEvictStats();
            LogFocusRestoreStats();
            LogStreamChurnStats();
            LogAllocatorTrimStats();
            LogNpcPacingStats();
            LogClothRegisterStats();
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
            LogViewRenameGuardStats();
        }
    }
}

void Init(const char* loadedVia, bool proxyOk, bool beforeGameCode)
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
    Log("OblivionPerfFix " OPF_VERSION " loaded as %s in %s", loadedVia,
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
    // Diagnostics only: an exception handler that writes UE's GPU crashes (DRED and memory) to the log.
    if (cfg.gpuCrashReport) InstallGpuCrashReport();
    else if (LogEnabled()) Log("[gpucrash] GPU crash report disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    const bool waterProbe = false;
    // WaterCaptureFsrSkip / NoRhiFlush / NoOcclusionWait / FastMeshScan: the water-info render's entry and the four sites,
    // written only before game code runs, when any key is on (lab: or WaterInfoProbe); then live.
    if (waterProbe || cfg.waterCaptureFsrSkip || cfg.waterCaptureNoRhiFlush || cfg.waterCaptureNoOcclusionWait || cfg.waterCaptureFastMeshScan) {
        if (InstallWaterCapture(beforeGameCode, waterProbe)) {
            SetWaterCaptureFsrSkip(cfg.waterCaptureFsrSkip);
            SetWaterCaptureNoRhiFlush(cfg.waterCaptureNoRhiFlush);
            SetWaterCaptureNoOcclusionWait(cfg.waterCaptureNoOcclusionWait);
            SetWaterCaptureFastMeshScan(cfg.waterCaptureFastMeshScan);
        }
    } else Log("[water] water-info capture keys off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ViewRenameGuard: the view rename guard (UE-217692). Placed before the game's code runs (two entry detours), then switched
    // live by the ini; RdgParallelTranslate's frame fence is only used while it repairs.
    if (const int mode = ViewGuardMode(cfg); mode > 0) {
        if (!beforeGameCode) Log("[view] view rename guard: can only be installed at start-up (version.dll), not installed");
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
    // HavokPickWarningGuard (a freeze guard, not a speed-up): installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.havokPickWarningGuard) ApplyHavokPickWarningGuard();
    else Log("[picks] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // FrameGenCrashGuard (a crash guard in AMD's FidelityFX library, not a speed-up): installed when the game loads the library, if
    // enabled at startup; afterwards the ini switches it live.
    if (cfg.frameGenCrashGuard) WatchFrameGenLibrary(beforeGameCode);
    else Log("[fg] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // ObjectReplaceCrashGuard (a crash guard for a game bug, not a speed-up): installed only if enabled at startup; afterwards the ini
    // switches it live (0 = the game's way: the fatal error).
    if (cfg.objectReplaceGuard && !beforeGameCode) Log("[replace] crash guard: can only be installed at start-up (version.dll), not installed");
    else if (cfg.objectReplaceGuard) {
        SetObjectReplaceGuardMode(cfg.objectReplaceGuard);
        ApplyObjectReplaceGuard();
    } else Log("[replace] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // TaskWaitDeadlockGuard (a guard against a game deadlock, not a speed-up): one vtable slot of the task graph, written only
    // before game code runs (the task graph starts early); afterwards the ini switches the mode live (0 = the game's wait).
    // On the OBSE64 route the task graph already runs when the plugin loads: the guard (and with it PsoCompileThreadPool) stays
    // off there, the game's own wait is kept, and the two log lines say so (guardreview G10).
    if (cfg.taskWaitDeadlockGuard && !beforeGameCode)
        Log("[taskwait] TaskWaitDeadlockGuard not installed: it can only be installed at start-up (as version.dll); loaded as an "
            "OBSE64 plugin the game's own wait is kept, and PsoCompileThreadPool is not applied either");
    else if (cfg.taskWaitDeadlockGuard) InstallTaskWaitGuard(cfg.taskWaitDeadlockGuard);
    else Log("[taskwait] TaskWaitDeadlockGuard off (takes a restart to switch on)");
    // PsoCompileThreadPool: two console variables located always (no patching); the pool size is written once, here, and only
    // with the guard above installed in mode 1 (UE treats the size as constant; the pool alone runs into the deadlock).
    if (FindPsoCompileThreadPool()) SetPsoCompileThreadPool(cfg.psoCompileThreadPool, cfg.psoCompileThreadPoolPriority);
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
    // ScreenshotReadbackConvert: installed only if enabled at startup; afterwards the ini switches it live (0 / 1 / 2 = verify).
    if (cfg.screenshotReadbackConvert) {
        if (ApplyReadbackConvertFix(beforeGameCode)) SetReadbackConvertMode(cfg.screenshotReadbackConvert);
    } else Log("[readback] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // OsPageCache: installed only if enabled at startup (the lab trace installs it too); afterwards the ini switches it live.
    const int osTrace = 0;
    if (cfg.osPageCache || osTrace) {
        if (InstallOsPageCache(beforeGameCode, Directory(g_iniPath).c_str(), osTrace))
            SetOsPageCache(cfg.osPageCache, cfg.osPageCacheMB, cfg.osPageCacheTrimSeconds, false);
    } else Log("[oscache] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // PsoPrecompileFast: SetBatchMode's entry detour, installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.psoPrecompileFast) {
        SetPsoPrecompileFastBudget(static_cast<float>(cfg.psoPrecompileFastBudgetMs < 1 ? 1 : cfg.psoPrecompileFastBudgetMs),
                                   cfg.psoPrecompileColdQueue < 0 ? 0 : cfg.psoPrecompileColdQueue);
        if (InstallPsoPrecompileFast(beforeGameCode)) SetPsoPrecompileFast(true);
    } else Log("[pso] off in OblivionPerfFix.ini (takes a restart to switch on)");
    // ScreenshotParallel: installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.screenshotParallel) {
        if (ApplyScreenshotParallelFix(beforeGameCode)) SetScreenshotParallel(cfg.screenshotParallel);
    } else Log("[shotpar] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // RestoreFsrAfterAltTab (a bug fix, not a speed-up): installed only if enabled at startup; afterwards the ini switches it live.
    if (cfg.restoreFsrAfterAltTab) g_focusHooked = ApplyFocusRestoreFix(beforeGameCode);
    else Log("[focus] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // LumenKeepArrayCapacity and UploadMemoryKeep (traversal stutter): installed only if enabled at startup; afterwards the ini switches them live.
    if (cfg.lumenKeepArrayCapacity) {
        g_lumenHooked = ApplyLumenKeepCapacityFix(beforeGameCode);
        if (g_lumenHooked) SetLumenKeepCapacityEnabled(true);
    } else Log("[lumen] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    if (cfg.uploadMemoryKeep) {
        g_uploadHooked = ApplyUploadMemoryKeepFix(beforeGameCode);
        if (g_uploadHooked) SetUploadMemoryKeep(true, cfg.uploadMemoryKeepFrames, cfg.uploadPagesToKeep, cfg.uploadReleaseRampFrames);
    } else Log("[upload] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // GpuSceneKeepCapacity: installed always (the policy logs the game's own resizes too); the ini switches the keeping live.
    g_gpuSceneHooked = ApplyGpuSceneKeepCapacityFix(beforeGameCode);
    if (g_gpuSceneHooked) SetGpuSceneKeepCapacityEnabled(cfg.gpuSceneKeepCapacity);
    // CommandAllocatorTrim: installed always (it counts the allocators in flight); the ini switches the trimming live.
    g_allocTrimHooked = InstallAllocatorTrim(beforeGameCode);
    if (g_allocTrimHooked) {
        SetAllocatorTrim(cfg.commandAllocatorTrim, cfg.allocatorTrimWindowSeconds);
        SetImmediateAllocatorLists(cfg.commandAllocatorTrim ? cfg.immediateAllocatorLists : 0);
    }
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
    } else Log("[pacing] disabled in OblivionPerfFix.ini (takes a restart to switch on)");
    // ClothRegisterLite: installed only if != 0 at startup and only as version.dll (the cloth component's native function table is
    // read when the engine registers the class); afterwards the ini switches it live. The lab's ClothRegisterSkip uses the same
    // hooks: either key installs them.
    const bool clothHooks = cfg.clothRegisterLite != 0;
    if (clothHooks) {
        g_clothRegHooked = InstallClothRegisterSkip(beforeGameCode);
        if (g_clothRegHooked) SetClothRegisterLite(cfg.clothRegisterLite);
    } else Log("[clothreg] off in OblivionPerfFix.ini (takes a restart to switch on)");
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
    // Diagnostic HitchLedger: installed only if on at startup (the tick-group entry detour, before BulkIndexInsert, whose lab
    // probe times the same entry; the frame-end sync call; read-only counters); afterwards the ini switches recording live.
    if (cfg.hitchLedger) {
        if (InstallHitchLedger(beforeGameCode, dir.c_str(), g_pacingHooked)) SetHitchLedger(true, cfg.hitchLedgerMs);
    }
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
    const bool saveBytesWanted = cfg.saveBytesBulk != 0, saveThumbWanted = cfg.saveThumbnailAsync != 0,
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
    // NaniteInitialRootPages: r.Nanite.Streaming.NumInitialRootPages before the ini and the renderer (start-up only; the OBSE64
    // route's start-up also runs before WinMain).
    if (cfg.naniteInitialRootPages) SetNaniteInitialRootPages(cfg.naniteInitialRootPages, beforeGameCode);
    else Log("[nanite] NaniteInitialRootPages=0: the game's 2048 root pages");
    // FrameRateLimitAtStart (game bug): frame generation's switch, detoured only if != 0 at start-up and while it is still off
    // (acts once, at its first switch on).
    if (cfg.frameRateLimitAtStart) InstallFrameCapStart(beforeGameCode, cfg.frameRateLimitAtStart);
    else Log("[fpscap] FrameRateLimitAtStart off in OblivionPerfFix.ini (takes a restart to switch on)");
    // HitchLedger's OblivionThread grid-change timer: last (the lab's CellRowDiag times the same entry and wins there).
    if (cfg.hitchLedger) InstallHitchLedgerGrid(beforeGameCode);
    // CellBufferPacedEvict: the TES constructor's buffer clamp, the grid change's entry (chained after CellRowDiag's and
    // HitchLedger's detours of the same entry, so last) and the ini writer, only if not 0 at startup; then live.
    if (cfg.cellBufferPacedEvict) {
        if (InstallCellBufferPacedEvict(beforeGameCode)) SetCellBufferPacedEvict(cfg.cellBufferPacedEvict);
    }
    // PhysicsDestroyBatch: three call sites in the step's DestroyPendingProxies path, only if != 0 at start-up (then
    // live); before the lab's PhysWaitProbe, whose entry detours would fail this code's entry check.
    if (cfg.physicsDestroyBatch) {
        if (InstallPhysicsDestroyBatch(beforeGameCode, cfg.parallelMinProcessors)) {
            g_destroyBatch = true;
            SetPhysicsDestroyBatch(cfg.physicsDestroyBatch, cfg.physicsDestroyBatchMin);
        }
    } else Log("[destroybatch] off in OblivionPerfFix.ini (takes a restart to switch on)");

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
    }
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
    0x010201,  // plugin version 1.2.1
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
    opf::Init("OBSE64 plugin", true, false);
    return true;
}
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        opf::g_self = module;
        DisableThreadLibraryCalls(module);
        if (_wcsicmp(opf::FileName(opf::ModulePath(module)).c_str(), L"version.dll") == 0) {
            const bool proxyOk = opf::ResolveVersionProxy();
            // Runs before the game's own code: the patches are plain code/vtable writes with no
            // dependency on game state, so they are safe to apply here.
            opf::Init("version.dll proxy", proxyOk, true);
        }
    }
    return TRUE;
}
