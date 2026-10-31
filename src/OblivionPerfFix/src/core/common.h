// Shared declarations: the version string, the ini settings (Config, with each fix's default), the pattern
// search and memory-write helpers (mem.cpp), and the entry points each fix file exports to main.cpp.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#define OPF_VERSION "1.2.3"
// The release label after the version (the tag is v<OPF_VERSION><OPF_RELEASE_SUFFIX>): in the log's first line and the DLL's
// version resource (build.ps1 reads both lines; CI checks the tag against OPF_VERSION).
#define OPF_RELEASE_SUFFIX "-beta"

namespace opf {

// Process affinity, QPC conversion (frequency must be nonzero), and optional thread names.
unsigned UsableLogicalProcessors();
uint64_t QpcToMicroseconds(uint64_t ticks, uint64_t frequency);
void DescribeThread(HANDLE thread, const wchar_t* name);

// Waits on a thread that may not be running (a helper preempted on a busy core): spin for about 50 us, then block until
// the word changes, so this processor goes idle and the scheduler can run the awaited thread on it. SwitchToThread
// would only yield to threads already queued on this processor. The writer calls WakeWord after its store; the wake
// costs a call only while a waiter sleeps. Blocking is WaitOnAddress (looked up at run time; Sleep(0) polling without).
struct WaitGate {
    std::atomic<int> sleepers{0};
};
bool SpinPhase(int& spin, uint64_t& deadline);                      // true while the spin phase lasts (pauses once)
void BlockOnWord(volatile void* word, const void* seen, size_t size);  // returns when *word may differ from *seen
void WakeWordAll(volatile void* word);
template <class T> void WakeWord(std::atomic<T>& word, WaitGate& gate)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);  // the caller's store before the sleeper check
    if (gate.sleepers.load(std::memory_order_relaxed) > 0) WakeWordAll(&word);
}
// Returns once done(word's value) holds; the value that satisfied it is returned.
template <class T, class Done> T WaitWord(std::atomic<T>& word, WaitGate& gate, Done done)
{
    int spin = 0;
    uint64_t deadline = 0;
    for (;;) {
        T v = word.load(std::memory_order_acquire);
        if (done(v)) return v;
        if (SpinPhase(spin, deadline)) continue;
        gate.sleepers.fetch_add(1, std::memory_order_seq_cst);
        v = word.load(std::memory_order_seq_cst);  // after the sleeper count: a writer that missed it is seen here
        if (done(v)) {
            gate.sleepers.fetch_sub(1, std::memory_order_relaxed);
            return v;
        }
        BlockOnWord(&word, &v, sizeof v);
        gate.sleepers.fetch_sub(1, std::memory_order_relaxed);
    }
}

// ---- log.cpp ----
void LogOpen(const wchar_t* path, int keepPrevious = 0, bool enabled = true);  // keepPrevious: earlier logs kept as <name>.1.log, .2.log, ...; enabled=false ([General] LogFile=0): nothing is written or renamed until a [gpucrash] line arrives
bool LogEnabled();  // false with LogFile=0
void LogClose();    // self-test only
void Log(const char* fmt, ...);

// ---- config.cpp ----
struct Config {
    bool boneUploadStores = true;      // BoneUploadStores
    bool atmosphereDirtyFilter = true; // AtmosphereDirtyFilter
    bool atmosphereLightFastPath = true; // AtmosphereLightFastPath (light fast path; needs AtmosphereDirtyFilter)
    bool atmosphereLightCloudPath = true; // AtmosphereLightFastPath: the sun's/moon's cloud values into their render proxy
    int rdgParallelTranslate = 2;      // RdgParallelTranslate: translate all RDG pass sets on worker threads (1 on, 0 off, 2 auto = on unless
                                       // the GPU has less than 9.5 GB: there video memory is the limit)
    bool rdgFrameFence = true;         // not in the shipped ini: RdgParallelTranslate orders each frame's start before its translates, so
                                       // buffer defrag can run with it (-0.2 GB of video memory;
                                       // used only while ViewRenameGuard is installed and on)
    int viewRenameGuard = 1;           // ViewRenameGuard: view creation vs pool defragmentation (UE-217692), installed at
                                       // start-up; 0 off, 1 on (lab: 2 count only, 3 repair counter UAVs too)
    int rdgSubmitBatch = 32;           // RdgSubmitBatch: command lists per GPU submission while parallel translate is on (0 = off)
    int rdgSubmitMaxAgeUs = 1000;      // not in the shipped ini: longest time a command list is held back
    bool clothConfigCache = true;      // ClothConfigCache: skip the per-frame cloth config copy when nothing changed
    bool frozenClothStretchFix = true; // FrozenClothStretchFix: a frozen cloth's LOD change keeps its pose (game bug: stretched robes)
    bool npcFadeInNoRebuild = true;    // NpcFadeInNoRebuild: a character's fade-in dither value reaches the GPU without a render-state rebuild
    bool prepareCollisionBehindLoadingScreen = true;   // PrepareCollisionBehindLoadingScreen: hand a transition's new area to physics while still frozen (A-B clean, 2026-10-05)
    bool editorIdLookupGuard = true;   // NameLookupCrashGuard: name lookups skip freed name-table entries instead of crashing (game bug)
    bool trapCrashGuard = true;        // TrapCrashGuard: traps drop actors that no longer exist instead of crashing (game bug)
    bool havokPickWarningGuard = true; // HavokPickWarningGuard: the old engine's "Havok picks" warning box no longer stops the game (game bug)
    bool frameGenCrashGuard = true;    // FrameGenCrashGuard: FSR 3 frame generation waits for a free command list instead of crashing (AMD library)
    int objectReplaceGuard = 1;        // ObjectReplaceCrashGuard: an in-place object re-creation whose destroy kept its slot releases it instead of
                                       // the UObjectArray.cpp:234 fatal (game bug; start-up install, then live): 0 off, 1 on (lab: 2 log only)
    int psoCompileThreadPool = -1;     // PsoCompileThreadPool (default on since 1.2.0, user 2026-10-24): the PSO precompile and precache compile on UE's PSO thread pool, not the task
                                       // workers (game bug: with a cold driver cache the loading froze behind them; START-UP ONLY, and only
                                       // applied with TaskWaitDeadlockGuard=1 installed): 0 the game's (task workers), N threads,
                                       // -1 logical processors - 2 (at least 1)
    int psoCompileThreadPoolPriority = -1; // not in the shipped ini: the pool threads' EThreadPriority (-1 = UE's below normal, kept; 0 normal = the
                                           // task workers': with a cold cache the loading then competes with the pool, 21 s stalls instead of 4 s)
    int taskWaitDeadlockGuard = 1;     // TaskWaitDeadlockGuard (default on since 1.2.0): a task worker waiting for tasks is also woken by the reserve worker covering it,
                                       // not only by a trigger task that may never get a thread (game bug: the cold-cache deadlock; start-up
                                       // install, then live): 0 off, 1 on, 2 measure only (the game's wake-up timing, through the guard's own
                                       // wait; only 0 is the game's code). PsoCompileThreadPool keeps it at 1.
    bool frameGenAsyncCompute = true;  // FrameGenAsyncCompute: r.FidelityFX.FI.AllowAsyncWorkloads 1 at start-up (frame generation on its own GPU queue)
    bool frameGenCameraInfo = true;   // FrameGenCameraInfo: the camera desc for AMD's ML frame generation (SDK 2.x DLLs)
    int frameGenPacingTuning = 1;      // FrameGenPacingTuning: AMD's FG present pacing tuning (FSR 3.1.4 or SDK 2.x DLLs): 0 game's, 1 Set B but the driver's own swap chain left alone, 2 Set A, 3 Set B everywhere (1)
    bool displayModeCache = true;      // DisplayModeCache: GetMonitorsInfo reads each monitor's mode list once (checked each time)
    bool screenshotFilterFast = true;  // MenuScreenshotFilter: the menu's same-size ImageResize of its screenshot, same bytes in SSE2
    int screenshotReadbackConvert = 1; // ScreenshotReadbackConvert: the screenshot readback's R10G10B10A2 -> FColor conversion in SSE2 (identical
                                       // bytes; start-up install, then live): 0 off, 1 on, 2 on + verify against the game's loop
    int screenshotParallel = 1;        // ScreenshotParallel: the menu screenshot's big copies (and MenuScreenshotFilter and ScreenshotReadbackConvert's loops) on helper threads,
                                       // ImageResize's zero fill skipped (same bytes; start-up install, then live): 0 off, 1 on
    bool restoreFsrAfterAltTab = true; // RestoreFsrAfterAltTab: FSR (and frame generation) back on when a menu up at focus gain closes
    bool lumenKeepArrayCapacity = true; // LumenKeepArrayCapacity: Lumen scene arrays keep their capacity instead of shrink/grow copies
    bool uploadMemoryKeep = true;      // UploadMemoryKeep: keep empty D3D12 upload pools/pages between streaming bursts
    int uploadMemoryKeepFrames = 0;    // not in the shipped ini: frames empty upload pools are kept; 0 = the whole session (game: 20)
    int uploadPagesToKeep = 24;        // not in the shipped ini: fast-allocator pages kept (4 MB each; game: 5)
    int uploadReleaseRampFrames = 900; // not in the shipped ini: UploadMemoryKeep switched off: kept pools handed back over this many frames (0 = at once)
    bool physicsTreeTimeSlicing = true; // PhysicsTreeTimeSlicing: Chaos rebuilds its trees time-sliced also after big streaming updates
    int physicsSliceIterations = 40000; // not in the shipped ini: PhysicsTreeTimeSlicing's rebuild slice, elements per frame (game: 4000)
    int physicsSliceCopyKB = 1000;     // not in the shipped ini: PhysicsTreeTimeSlicing's copy slice, KB per frame (game: 100)
    bool physicsSliceAdaptive = true;  // not in the shipped ini: PhysicsTreeTimeSlicing's copy slice only while a rebuild has a backlog
    bool physicsFlushParallel = true;  // PhysicsFlushParallel: the catch-up after a physics tree rebuild runs on two threads
    int physicsFlushMinChanges = 1024; // not in the shipped ini: PhysicsFlushParallel only for at least this many queued changes
    bool instanceClearReserve = true;  // InstanceClearReserve: clearing a big instanced mesh reserves its hide commands at once
    bool parallelStaticBodies = true;  // ParallelStaticBodies: instanced meshes' static bodies created on several threads (start-up
                                       // install, then live)
    int parallelStaticBodiesMin = 64;  // not in the shipped ini: ParallelStaticBodies only for components with at least this many bodies
    int parallelStaticBodiesThreads = 0; // not in the shipped ini: ParallelStaticBodies's helper threads (0 = logical processors / 4, max 8)
    int parallelMinProcessors = 8;     // not in the shipped ini: ParallelStaticBodies (with PipelinedSceneAdd/BodyCreation), PhysicsPushParallel and
                                       // AnimTeardownParallel stay off below this many usable logical processors (re-checked live;
                                       // PhysicsDestroyBatch's helper: checked once at start-up)
    bool pipelinedSceneAdd = true;     // PipelinedSceneAdd: InitStaticBodies' scene add, registration and inserts side by side (
                                       // start-up install with ParallelStaticBodies, then live)
    int pipelinedSceneAddMin = 64;     // not in the shipped ini
    bool dirtyTreeQuickInsert = true;  // DirtyTreeQuickInsert: greedy sibling choice for dirty-tree inserts of the collision index (
                                       // start-up install, then live; "equivalent" class:, user decision)
    bool helperThreadCaches = true;    // ParallelStaticBodies/22 helpers: the game allocator's per-thread caches on our helper threads
                                       // (live)
    bool bulkIndexInsert = true;       // BulkIndexInsert: a flush's dirty-tree inserts filed as one subtree, built at its end
                                       // (start-up install, then live; "equivalent" class:, user decision)
    int bulkIndexInsertMin = 256;      // not in the shipped ini: BulkIndexInsert only for passes of at least this many changes
    bool physicsInternalFlushParallel = true; // PhysicsInternalFlushParallel: a swap step's FlushInternal beside FlushAsync (identical; live)
    bool physicsQueueShrinkOnce = true;  // PhysicsQueueShrinkOnce: the game thread's catch-up shrinks its pending queue once, not per
                                         // removal (identical contents; installed with BulkIndexInsert at start-up, then live)
    int physicsDestroyBatch = 3;       // PhysicsDestroyBatch: physics/destroy_batch.cpp, the step's destruction of removed bodies:
                                       // 1 proxies deleted by a helper, 2 one UpdateViews, 3 both, 4 installed but off; 0 = not
                                       // installed (start-up), then live
    int physicsDestroyBatchMin = 256;  // not in the shipped ini: PhysicsDestroyBatch acts only from this many pending bodies
    int npcBodiesOneLock = 1;          // NpcBodiesOneLock: npcs/npc_bodies.cpp, a character's physics bodies and constraints
                                       // (InitArticulated's InstantiatePhysicsAsset_Internal) made under one hold of the scene's
                                       // write lock (start-up install, then live): 0 off, 1 on
    int clothRegisterLite = 3;         // ClothRegisterLite: npcs/cloth_register.cpp, every cloth build of an EquipBodyPart call made
                                       // without its constraint rules, the one left at the call's end completed in place (start-up
                                       // install, then live): 0 off, 3 on (lab: 1 learned, 2 the registration build only; the
                                       // public build reads any value != 0 as 3)
    int rigVmHashMemo = 1;             // RigVmHashMemo: npcs/rig_setup.cpp, URigVM::ComputeVMHash memoised on its full inputs
                                       // (identical; start-up install, then live): 0 off, 1 on (lab: 2 verify; public reads 2 as 1)
    int rigLogicMapCache = 1;          // RigLogicMapCache: npcs/rig_setup.cpp, the head rig's DNA name -> hierarchy index maps
                                       // replayed per DNA content (identical; start-up install, then live): 0 off, 1 on (lab: 2 verify)
    int rtSceneBufferKeep = 1;         // RtSceneBufferKeep: streaming/rt_scene_keep.cpp, the ray tracing scene's TLAS buffer is not
                                       // re-created smaller (1), or installed but the game's way, creations timed (2); start-up
                                       // install when != 0, then live 1/2
    bool rayTracingBuildBatch = true;  // RayTracingBuildBatch: render/rt_build_batch.cpp, new meshes' ray-tracing builds as one
                                       // batch at the start of the frame's render instead of one at a time (same builds, same
                                       // frame; start-up install on the version.dll route, then live)
    bool waterCaptureFsrSkip = true;   // WaterCaptureFsrSkip: render/water_info.cpp, no FSR3 pre-alpha copy in water-info renderers
    bool waterCaptureNoRhiFlush = true;  // WaterCaptureNoRhiFlush: water-info renderers skip the RHI-thread flush
    bool waterCaptureNoOcclusionWait = true;  // WaterCaptureNoOcclusionWait: water-info renderers skip the occlusion-fence wait
    int waterCaptureFastMeshScan = 1;  // WaterCaptureFastMeshScan: FEnableWaterInfoMeshesScoped scans the packed component ids
                                       // (1; lab: 2 = and checks against the stock scan; the public build reads 2 as 1).
                                       // The four water keys: installed at start-up (version.dll route) when any is on, then live
    int naniteInitialRootPages = 3072; // NaniteInitialRootPages: streaming/nanite_stream_buffers.cpp, r.Nanite.Streaming.
                                       // NumInitialRootPages set at start-up (0 or up to 2048 = the game's 2048; 2049-8192):
                                       // ClusterPageData starts with that many root pages (3072: +32 MiB of video memory)
    int frameRateLimitAtStart = 1;     // FrameRateLimitAtStart: framegen/frame_cap_start.cpp (game bug): with frame generation on at
                                       // start-up, the saved Frame Rate Limit applied once after the game's switch zeroed it, as the
                                       // Graphics page does; since 1.2.2 only after FSR's switch on AMD/Intel GPUs (1; lab: 2 =
                                       // only logged; the public build reads 2 as 1); start-up install when != 0, 0 off
    int navLinkFast = 1;               // NavLinkFast: streaming/nav_link_fast.cpp, a streamed navmesh tile's border links with less
                                       // searching (calls that cannot make a link left out; the search looks only at the
                                       // neighbour's polygons with a border edge on that side; same links): 1 on (lab: 2 on +
                                       // checked against the game's, 3 installed but the game's calls and search, timed; the
                                       // public build reads 2 and 3 as 1); 0 = not installed (start-up), then live (0 at run
                                       // time = the game's calls and search)
    int saveBytesBulk = 1;             // SaveBytesBulk: saves/save_bytes.cpp, a save's byte arrays (the save list's thumbnails) written
                                       // in one piece (same bytes): 1 on (lab: 2 on + every save also written the game's way and
                                       // compared; the public build reads 2 as 1); 0 = not installed (start-up), then live
    int saveRefIdIndex = 1;            // SaveRefIdIndex: saves/save_refid.cpp, the old engine save's refID lookups through a hash
                                       // index (same refIDs): 1 on (lab: 2 on + compared with the linear search; public reads 2 as 1);
                                       // 0 = not installed (start-up), then live
    int saveThumbnailAsync = 1;        // SaveThumbnailAsync: saves/save_thumb.cpp, the save's thumbnail read back without the game
                                       // thread waiting (same picture): 1 on (lab: 2 on + compared with the game's read-back; public
                                       // reads 2 as 1); 0 = not installed (start-up), then live
    bool menuOpenFlashFix = true;        // MenuOpenFlashFix (issue #5, game bug): occlusion culling off for the player menu's open
                                         // frame, so the next frame (the menu's background "box") is not culled (installed only if on
                                         // at start-up, version.dll route; then live; the OBSE64 route logs it as not installed)
    bool physAnimNoEmptyLock = true;     // PhysAnimNoEmptyLock: physics/physanim_lock.cpp, a physical animation component that drives no
                                         // bodies skips UpdateTargetActors' scene write lock (identical; installed only if on at start-up,
                                         // version.dll route; then live)
    int rigTraceReuse = 1;               // RigTraceReuse: npcs/rig_trace_reuse.cpp, a Control Rig sphere trace repeated with identical
                                         // inputs inside one rig execution reuses its result (equivalent): 1 on, 2 verify (runs and
                                         // compares), 0 off; installed only if not 0 at start-up (version.dll route), then live
    int chaosMaxPayloadSize = 10000000;  // ChaosMaxPayloadSize: physics/chaos_payload.cpp, p.MaxPayloadSize raised (cm) at start-up so
                                         // the ~730 bodies over 1 km stay in the Chaos trees (equivalent); only over the game's 100000;
                                         // an Engine.ini value wins; 0 = the game's value; both routes; start-up only
    int osPageCache = 1;                 // OsPageCache: blocks the allocator releases to Windows are kept (exact size, bounded) and
                                         // handed out again for its own cache misses (identical; start-up install, then live)
    int osPageCacheMB = 64;              // not in the shipped ini: OsPageCache's cap (MB, 4-1024)
    int osPageCacheTrimSeconds = 5;      // not in the shipped ini: OsPageCache releases blocks unused this long (s)
    int animTeardownParallel = 1;        // AnimTeardownParallel: dying NPC animation data's interpolators
                                         // destroyed on helper threads, joined before the actor's clean-up goes on (start-up
                                         // install when not 0, then live): 0 off, 1 on
    int cellBufferPacedEvict = 1;        // CellBufferPacedEvict: spare exterior cell buffer slots
                                         // (start-up install when not 0) and the overflow torn down one cell per OT frame
                                         // after a crossing (live): 0 in the crossing frame (as the game), 1 paced
    bool psoPrecompileFast = true;       // PsoPrecompileFast: the start-up PSO precompile at UE's Fast pace outside play (identical: timing
                                         // only; start-up install, then live)
    int psoPrecompileFastBudgetMs = 4;   // PsoPrecompileFastBudgetMs: render-thread ms per frame at the faster pace (16 = UE's Fast,
                                         // the pace before 1.1.10; live)
    int psoPrecompileColdQueue = 1000;   // PsoPrecompileColdQueue: once this many pipelines wait (a cold driver cache), the game's own
                                         // pace for the rest of the precompile (0 = no cold-cache guard at all: no queue,
                                         // driver-thread or 60 s limit; live)
    bool psoPrecompileFastInGameMenus = false;  // PsoPrecompileFastInGameMenus: also Fast in menus once play has started
                                         // (inventory, map, dialogue, pause); 0 = the game's pace there (live)
    int physicsPushParallel = 1;       // PhysicsPushParallel: the physics side of the push of new static bodies: 0 off, 1 batch creation +
                                       // shapes set up on helper threads, 2 batch creation only, 3 = 1 + verify (start-up install, then live)
    int physicsPushMin = 32;           // not in the shipped ini: PhysicsPushParallel's helpers only for runs of at least this many proxies
    int physicsPushThreads = 0;        // not in the shipped ini: PhysicsPushParallel's helper threads (0 = logical processors / 4, max 8)
    bool pipelinedBodyCreation = true;  // PipelinedBodyCreation: one instanced mesh's body creation, registration and inserts side by side
                                        // (needs ParallelStaticBodies at start-up; live; identical)
    bool gpuSceneParallelUpload = true;  // GpuSceneParallelUpload: r.GPUScene.ParallelUpdate 1 (the GPU scene's instance upload of a
                                         // streamed area on all workers; identical; live)
    bool hitchLedger = false;          // Diagnostic HitchLedger: slow game-thread frames' stage times to
                                       // OblivionPerfFix-ledger.csv (installed only if on at start-up, then switched live)
    int hitchLedgerMs = 25;            // ... a frame counts as slow from this period (ms, the cap's sleep included)
    bool hitchLedgerTickSplit = false; // HitchLedgerTickSplit (diagnostic, public since 1.2.3): diagnostics/tick_split.cpp, the
                                       // game thread's tick time by kind, class and group to OblivionPerfFix-ticksplit.csv
                                       // (needs HitchLedger=1; installed only if on at start-up, then live)
    bool vramCounterFix = true;        // VramCounterFix: the Graphics page's "Show VRAM" counter shows the game's video
                                       // memory use and budget instead of 0/0 (game bug; start-up install, live)
    bool shaderScreenAfterDriverChange = true;   // ShaderScreenAfterDriverChange (game bug): the game's first-launch
                                                 // shader screen at start-up after a driver/card change (start-up only)
    bool npcCreationPacing = true;    // NpcCreationPacing: far-away NPC creation spread over frames 
    int pacingBudgetMs = 4;           // not in the shipped ini: NPC unit time per frame before far units wait
    int pacingMinDistance = 4096;      // not in the shipped ini: only actors farther than this (game units) wait
    int pacingMaxLagFrames = 15;       // not in the shipped ini: work older than this many frames runs regardless
    bool clothBuildDefer = true;       // ClothBuildDefer: npcs/cloth_defer.cpp, a far, unseen NPC's first cloth simulation
                                       // build waits a few frames, repeated requests merge (needs NpcCreationPacing; timing
                                       // change; start-up install on the version.dll route, then live)
    int clothDeferQuietFrames = 6;     // not in the shipped ini: ... frames without a new request before it is built
    bool pacingUseSlack = true;        // not in the shipped ini: under a frame-rate cap, use the cap's sleep as budget
    bool pacingFlowWait = true;        // not in the shipped ini: a level flow waits for the main queue instead of draining it
    bool pacingLeanFlowDrain = true;   // PacingLeanFlowDrain: a level flow at its wait bound drains the owed main queue only;
                                       // the waiting delegates stay queued (live; PR #25)
    int pacingFlowWaitFrames = 8;      // not in the shipped ini: ... up to this many frames while the queue progresses (3 = 0.17)
    int pacingFadeAware = 1;           // not in the shipped ini: PacingFadeAware (npcjitter): 1 = a far NPC still fully invisible in
                                       // its spawn fade-in (dither >= 3) counts as unseen, so its appearance / equipment / hair may
                                       // wait like a hidden far NPC's (live); 0 = only fix 15's "rendered" test (1.2.2). Lab: 2 = near
                                       // ones too; 3 (fadenear) = far-seen and near, equipment and hair only, after the appearance
    bool memoryRelief = true;          // MemoryRelief: when Windows runs short of memory, LumenKeepArrayCapacity and UploadMemoryKeep (and 4 for VRAM) give theirs back
    int memoryReliefRamMB = 1024;      // not in the shipped ini: UploadMemoryKeep gives back while available RAM is under this (3 s)
    bool vramRelief = false;           // not in the shipped ini: MemoryRelief's video-memory stages (RAM/commit relief stays)
    bool vramReliefPauseTranslate = false; // not in the shipped ini: MemoryRelief's old stage 2 (pause parallel translate for VRAM)
    int memoryReliefCommitMB = 1024;   // not in the shipped ini: LumenKeepArrayCapacity and UploadMemoryKeep while available commit + page file growth room is under this
    bool navmeshBuildNoBusyWait = true; // NavmeshBuildNoBusyWait: navmesh tile builds never run inside another task's busy wait
    bool busyWaitFlagCheck = true;      // BusyWaitFlagCheck: a busy-waiting worker checks every task it picks up (UE 5.3 checks only the
                                        // first): tasks that must not run inside busy waits (NavmeshBuildNoBusyWait's tiles) never do; live
    bool gpuSceneKeepCapacity = true;  // GpuSceneKeepCapacity: GPU scene buffers keep their capacity instead of shrink/grow re-creations
    bool commandAllocatorTrim = true;  // CommandAllocatorTrim: D3D12 command allocator pools shrink back to recent need
    int allocatorTrimWindowSeconds = 20; // not in the shipped ini: the window whose peak of allocators in flight is kept
    int allocatorTrimHeadroomMB = 2048; // not in the shipped ini: CommandAllocatorTrim trims the copy pool only within this much of the budget (0 = always)
    int immediateAllocatorLists = 0;   // not in the shipped ini: CommandAllocatorTrim part 2, lists the immediate context records per allocator
    bool logFile = true;               // [General] LogFile: 0 = no OblivionPerfFix.log except a GPU crash report (start-up only; lab builds ignore it)
    bool gpuCrashReport = true;        // [General] GpuCrashReport: DRED + memory report in the log at a GPU crash
    int statsIntervalSeconds = 60;     // 0 = no periodic stats in the log
};
Config LoadConfig(const wchar_t* iniPath);

// ---- streaming/nav_link_fast.cpp (NavLinkFast) ----
bool InstallNavLinkFast(bool beforeGameCode, int mode);  // findConnectingPolys' and connectExtLinks' calls redirected (start-up only)
void SetNavLinkFast(int mode);                           // live, once installed: 1 fast, else the game's (lab: 2 checked)
void LogNavLinkFastStats();
int NavLinkFastTestFind(int mode, void* nav, const double* va, const double* vb, void* fromTile, int fromPolyIdx, uint8_t* tile, int side,
                        void* cons);  // self-test only
int NavLinkFastTestOriginal(void* nav, const double* va, const double* vb, void* fromTile, int fromPolyIdx, uint8_t* tile, int side,
                            void* cons);  // self-test only
int NavLinkFastTestSkip(const uint8_t* tile, const uint8_t* target, int side);  // self-test only: 0 = the call is made
uint8_t* NavLinkFastTestSite(int which);   // self-test only: -1 the findConnectingPolys call, 0-3 addTile's connectExtLinks calls
// ---- framegen/frame_cap_start.cpp (FrameRateLimitAtStart, game bug) ----
bool FindFrameCapStart();
bool InstallFrameCapStart(bool beforeGameCode, int mode);  // start-up only (version.dll and OBSE64 routes, while T's state is off)
bool FrameCapStartTestPatched();                           // self-test: T's entry jump and trampoline
bool FrameCapStartTestRun(void* fakeOrig, void* fakeGetter, void* fakeHash, void* fakeFind, void* fakeSet, const void* name,
                          const uint8_t* state, int menu, int mode, bool on, bool reset, int caller, const uint8_t* reflex,
                          int32_t* const* valueSlot, const uint32_t* vendor);  // self-test: one hook call with fakes (caller 1 = FSR's sink)
const uint32_t* FrameCapStartTestVendor();                 // self-test: GRHIVendorId as found through T's vendor check
// ---- streaming/nanite_stream_buffers.cpp (NaniteInitialRootPages; the probe and minimums are lab-only) ----
bool FindNaniteStreamBuffers();                   // ProcessNewResources' three resizes (the root-page variable is checked against it)
int32_t* FindNaniteRootPagesVar();                // r.Nanite.Streaming.NumInitialRootPages' value (nullptr if not found)
bool SetNaniteInitialRootPages(int pages, bool beforeGameCode);  // NaniteInitialRootPages (start-up only, both routes)
// ---- render/water_info.cpp (WaterCaptureFsrSkip / NoRhiFlush / NoOcclusionWait / FastMeshScan) ----
bool InstallWaterCapture(bool beforeGameCode, bool probe);  // start-up only (version.dll route); probe = lab WaterInfoProbe's game-thread part
void SetWaterCaptureFsrSkip(bool on);
void SetWaterCaptureNoRhiFlush(bool on);
void SetWaterCaptureNoOcclusionWait(bool on);
void SetWaterCaptureFastMeshScan(int mode);
void LogWaterCaptureStats();
const char* WaterInfoTestScan();
bool WaterInfoTestOccTrampoline();
bool WaterInfoTestPatched();
const char* WaterInfoTestHooks();
bool WaterInfoTestTrampoline();
bool WaterInfoTestMismatch();
// ---- render/rt_build_batch.cpp (RayTracingBuildBatch) ----
// new meshes' ray-tracing builds (InitRHI's build requests on the render thread) queued in the engine's geometry manager
// instead of built one at a time, then all built as one batch at the start of the next scene render (Render's vtable
// slot wrapped). Same builds, same frame.
bool InstallRayTracingBuildBatch(bool beforeGameCode);   // start-up only (version.dll route)
void SetRayTracingBuildBatch(bool on);                   // live
void LogRayTracingBuildBatchStats();
bool RayTracingBuildBatchDecide(bool on, int32_t priority, int32_t mode, bool fromInitRhi, bool renderThread, bool inRender,
                                int32_t budget, uint8_t fastBuild, uint8_t allowUpdate);   // true = queue (pure; self-test)
struct RayTracingBuildBatchInfoOut {
    uint8_t* callSite = nullptr;   // CreateRayTracingGeometry's call, now to the gate
    uint8_t* region = nullptr;     // [0] jmp to the gate, [16] the request function's queue path
    void** slot = nullptr;         // Render's vtable slot, now the wrapper
    void* hook = nullptr;          // the wrapper
    uint8_t* request = nullptr;    // the game's RequestBuildAccelerationStructure
    void* render = nullptr;        // the game's Render
    void* immediate = nullptr;     // the immediate command list (the early batches')
};
bool RayTracingBuildBatchInfo(RayTracingBuildBatchInfoOut* out);   // self-test
bool RayTracingBuildBatchFlushFirst(uint64_t queuedScratch, int32_t queued, uint64_t scratch);  // the cap's decision (pure)
bool RayTracingBuildBatchQueueForTest(const void* manager, int32_t* count, uint64_t* scratch);  // self-test: the queue reading


// ---- view_rename_guard.cpp (ViewRenameGuard: closes UE-217692, view creation vs pool defragmentation) ----
struct ViewRenameGuardInfoData {
    uint8_t* createView = nullptr;         // FD3D12View::CreateView
    uint8_t* onAllocationMoved = nullptr;  // FD3D12ResourceLocation::OnAllocationMoved
    uint64_t stale = 0, recentOnly = 0, repairs = 0, guarded = 0, moves = 0;
    uint8_t* uavUpdateDescriptor = nullptr;  // FD3D12UnorderedAccessView::UpdateDescriptor (identifies UAVs, slot 2)
    uint64_t counterLeft = 0;                // counter UAVs not repaired in mode 1
};
bool FindViewRenameGuard();
bool InstallViewRenameGuard(int mode);  // start-up only; 1 repair (not counter UAVs), 2 count only, 3 repair all
void SetViewRenameGuardMode(int mode);  // 0 = pass through (switchable while the game runs)
bool ViewRenameGuardRepairing();        // installed and repairing (mode 1 or 3): RdgParallelTranslate's frame fence may be used
void LogViewRenameGuardStats();
bool ViewRenameGuardInfo(ViewRenameGuardInfoData* out);
void ViewRenameGuardTestSetOriginals(void* createView, void* onAllocationMoved);  // self-test: fakes instead of the game

// ---- mem.cpp ----
struct Section {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
    bool Contains(const void* p) const { return p >= begin && p < end; }
};
struct GameModule {
    uint8_t* base = nullptr;
    Section text, rdata, data;
    uint32_t timeDateStamp = 0;
    uint32_t sizeOfImage = 0;
};
// module = nullptr: the process exe. Tests pass a mapped copy of the game exe.
bool InitGameModule(GameModule& out, HMODULE module = nullptr);
bool ParseModule(GameModule& out, HMODULE module);  // another module's sections (Game unchanged)
const GameModule& Game();

// Byte pattern such as "C5 FC 28 1D ?? ?? ?? ?? 66 90".
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> care;  // 1 = byte must match
    size_t size() const { return bytes.size(); }
};
Pattern ParsePattern(std::string_view text);
std::vector<uint8_t*> FindAll(const Section& s, const Pattern& p);
// The original search (memchr for the first byte): the same results as FindAll, slower.
std::vector<uint8_t*> FindAllReference(const Section& s, const Pattern& p);
// Start-up cost of the pattern searches (FindAll calls, their time, bytes searched, ScanVerify mismatches).
struct ScanStats {
    uint64_t calls, microseconds, bytes, mismatches;
};
ScanStats GetScanStats();
// 0 = fast search; 1 = also run FindAllReference on every search, log differences and use its result; 2 = only the
// reference search (the original start-up, for timing comparisons).
void SetScanMode(int mode);
using ScanObserver = void (*)(const Section& s, const Pattern& p, const std::vector<uint8_t*>& hits, uint64_t ticks);
void SetScanObserver(ScanObserver observe);  // self-test: sees every search
bool MatchesAt(const uint8_t* at, const Pattern& p);
// Absolute target of a RIP-relative operand: disp32 at insn+dispOffset, instruction length insnLen.
uint8_t* RipTarget(const uint8_t* insn, size_t dispOffset, size_t insnLen);
uint8_t* PrimaryFunction(const GameModule& game, const uint8_t* at);  // physics/bulk_insert.cpp: the .pdata function holding a code address (nullptr if none)
bool WriteCode(void* at, std::span<const uint8_t> bytes);
bool WritePointer(void** at, void* value);
void** FindImportSlot(uint8_t* base, const char* dll, const char* function);  // the import slot of dll!function
uint8_t* AllocNearGame(size_t size);  // read-write memory within rel32 reach of the game's code (nullptr if none)
uint8_t* AllocNear(const uint8_t* module, size_t sizeOfImage, size_t size);  // ... of another module's code

// ---- proxy.cpp ----
bool ResolveVersionProxy();

// ---- fixes ----
bool ApplyBoneUploadFix();      // bone_upload.cpp
bool ApplyAtmosphereDirtyFix(); // atmosphere.cpp
void SetAtmosphereDirtyFilterEnabled(bool enabled);  // atmosphere.cpp, switchable live
void SetAtmosphereLightFastPathEnabled(bool enabled);  // atmosphere.cpp, switchable live
bool AtmosphereLightFastPathInfo(uint8_t** ucab, int32_t* mobility, std::vector<uint32_t>* fields, size_t* thunks);
void SetAtmosphereLightCloudPathEnabled(bool enabled);  // atmosphere.cpp, switchable live
struct AtmosphereCloudPathInfoOut {
    uint8_t* setter = nullptr;  // FDirectionalLightSceneProxy::SetAtmosphereRelatedProperties
    void** vtable = nullptr;    // the proxy's vtable (the first of numVtables)
    int numVtables = 0, slot = -1;
    bool hooked = false;
    uint32_t compSurface = 0, compScale = 0, proxySurface = 0, proxyScale = 0;
    int32_t sceneProxy = -1;  // ULightComponent::SceneProxy, once found
    uint64_t posted = 0, applied = 0, late = 0;
    uint64_t inStep = 0, outOfStep = 0;  // before a change: the proxy held the light's previous values / other values
    uint64_t duringRebuild = 0, rebuilt = 0;  // checks skipped while a rebuild replaces the proxy; such rebuilds
    int pending = 0;
    struct Field { const char* name; uint32_t comp, size, dst; bool inLightInfo; };
    std::vector<Field> fields;  // what the path writes: component offset -> proxy (or its FLightSceneInfo) offset
    uint32_t lightInfo = 0;     // FLightSceneProxy::LightSceneInfo
    uint32_t component = 0;     // FLightSceneProxy::LightComponent (the sky-light check; 0: sky-only fields only)
    uint64_t notSky = 0;        // changes on directional lights the renderer did not draw as sky lights (rebuilt)
};
bool AtmosphereCloudPathInfo(AtmosphereCloudPathInfoOut* out);
// self-test: the game thread's part for a component (before = its bytes before the Set), without the class checks
bool AtmosphereCloudTestPost(const uint8_t* before, const uint8_t* component, int32_t size);
void AtmosphereCloudTestForget(const uint8_t* component, int32_t size);
bool AtmosphereSkyChoiceTest(const uint8_t* component, int32_t size);  // self-test: the sky light choice check
int32_t AtmosphereSunIndexOffset();
void AtmosphereCloudTestAge(uint64_t us);
void LogAtmosphereStats();      // atmosphere.cpp
bool FindRdgParallelTranslateSite();               // parallel_translate.cpp
void SetRdgParallelTranslateEnabled(bool enabled); // parallel_translate.cpp, switchable live
uint8_t* RdgParallelTranslateSite();
bool RdgParallelTranslateActive();
void SetRdgParallelTranslateAllowed(bool allowed);  // false: the fix stays off whatever the ini says
// memory_monitor.cpp: the largest dedicated video memory of the PC's hardware GPUs in MB (0 = not known yet; asked once, from
// the monitor thread: loads dxgi.dll), and its name.
uint64_t LargestGpuMemoryMB(const char** name);
// translate_defrag_guard.cpp: D3D12 buffer-pool defragmentation paused while parallel translate is on
bool FindPoolDefragSwitch();
void SetPoolDefragPaused(bool paused);           // live; paused again if the engine's settings switch it back on
bool PoolDefragPausedFor(int ms);                // paused, and for at least ms milliseconds
bool PoolDefragSwitchFound();
void PoolDefragTestAddresses(uint8_t** defrag, uint8_t** maxCopy);
bool InstallDescriptorHeapGuard();               // parallel_translate.cpp; safe at any time (one vtable slot)
bool DescriptorHeapGuardInfo(void*** vtable, size_t* blockOffset, void** hook);
uint64_t DescriptorHeapGuardHits();

// submit_batch.cpp
struct RdgSubmitInfo {
    uint8_t* submitLists;
    uint8_t* submitCommands;
    uint8_t* submitPayloads;
    void** vtableSlot;
    void** rhiVtable;
    void** gDynamicRhi;
    uint32_t queueOffset;
    int numChecks;
    uint32_t checkOffset[16];
    uint8_t checkWidth[16];
    bool checkOnQueue[16];
};
// Must run before the game's renderer starts (it detours a function). rhiMayBeRunning: we were not
// loaded from DllMain, so only install if the exe shows that no RHI exists yet.
bool InstallRdgSubmitBatching(bool rhiMayBeRunning);
void SetRdgSubmitBatch(int maxLists, int maxAgeUs);  // switchable live; 0 = pass everything through
void CheckRdgSubmitBatching();                   // once a second
void LogRdgSubmitStats();
bool RdgSubmitBatchingInfo(RdgSubmitInfo* out);
void RdgSubmitBatchingTestHooks(void* fakeSubmitLists, void* fakeSubmitPayloads, void** hookLists, void** hookPayloads);

// cloth_config.cpp
struct ClothConfigInfo {
    uint8_t* callSite;    // the call in FClothSimulationProxy::InitializeConfigs' loop
    uint8_t* initialize;  // FClothingSimulationConfig::Initialize (the original target)
    uint8_t* stub;        // jmp to the hook, within rel32 reach of the call
    void* hook;
    uint32_t inputOffset, facadesOffset;           // UChaosClothComponent: PropertyCollections, CollectionPropertyFacades
    uint32_t configCollections, configProperties;  // FClothingSimulationConfig: PropertyCollections, Properties
};
struct ClothConfigStats {
    uint64_t calls, skipped, flags, values, shape, unverified, disabled, faults;
    uint64_t checked, checkFailed;  // copies re-compared afterwards: equal / not equal (the fix then switches off)
};
// beforeGameCode: loaded from DllMain. Otherwise the call is only switched if that is one atomic write.
bool ApplyClothConfigFix(bool beforeGameCode);
void SetClothConfigCacheEnabled(bool enabled);  // switchable live; off = copy every frame as before
void LogClothConfigStats();
bool ClothConfigFixInfo(ClothConfigInfo* out);
ClothConfigStats GetClothConfigStats();
void ClothConfigTestOriginal(void* fakeInitialize);  // self-test only: replaces the original Initialize
// frozen_cloth_lod.cpp (FrozenClothStretchFix): a suspended cloth's zero-time LOD update uses the engine's per-cloth teleport
bool InstallFrozenClothStretchFix(bool beforeGameCode);  // one call redirected (only while the game starts)
void SetFrozenClothStretchFix(bool enabled);             // live, once installed
void LogFrozenClothStats();
void* FrozenClothTestCallSite();                         // self-test only
void FrozenClothTestOriginal(void* fn);                  // self-test only
void FrozenClothTestCall(uint8_t* solver, float dt, uint8_t* proxy);  // self-test only
// npc_fade_in.cpp (NpcFadeInNoRebuild): Altar's per-primitive dither value (characters fading in) updates the GPU scene
// instead of recreating the render state
struct NpcFadeInInfoOut {
    uint8_t *setter, *markDirty, *getWorld;
    uint32_t floatOff, boolOff, sceneOff, updateSlot, proxyBoolOff, proxyFloatOff;
};
bool InstallNpcFadeInNoRebuild(bool beforeGameCode);  // the float setter's entry redirected (only while the game starts)
void SetNpcFadeInNoRebuild(bool enabled);             // live, once installed
void LogNpcFadeInStats();
bool NpcFadeInInfo(NpcFadeInInfoOut* out);
void NpcFadeInTestSetup(void* fakeMarkDirty, void* fakeGetWorld, int32_t proxyOff);  // self-test only
void NpcFadeInTestCall(uint8_t* comp, float value);                                   // self-test only

// fade_hook.cpp (the end of a transition's loading screen; PrepareCollisionBehindLoadingScreen runs from it)
struct FadeHookSites {
    uint8_t* fadeToGame;     // VLevelChangeData fade-to-game (lambda body)
    uint8_t* unfreeze;       // the game's unfreeze timer body (read for offsets, never called)
    uint8_t* exec;           // fade-to-game delegate Execute / ExecuteIfSafe (original slot values)
    uint8_t* execIfSafe;
    void** execSlot;         // their vtable slots
    void** execIfSafeSlot;
    uint32_t flagOffset, flag2Offset, timerOffset, captureOffset, modeOffset, stackNumOffset;
    uint32_t getWorldSlot;   // UObject::GetWorld vtable offset used on the VLevelChangeData
    void* execHook;
    void* execIfSafeHook;
};
bool InstallFadeHook();                       // safe at any time (two vtable slots)
bool FadeHookInfo(FadeHookSites* out);
void FadeHookTestFunctions(void* fakeExec, void* fakeExecIfSafe, void* fakeGetFreeze);  // self-test only
bool FindFreezeSubsystem();                   // locates the freeze subsystem's getter and layout (start-up, no hook)
bool ReadMenuFreeze(int32_t* namedFreezes, int32_t* mode);  // game thread; false if not available

// collision_prepare.cpp (PrepareCollisionBehindLoadingScreen; called from the fade-to-game hook in fade_hook.cpp)
struct CollisionPrepareFixInfo {
    uint8_t *dispatch, *copy, *recycle, *startFrame, *endFrame, *doWork, *cisa, *setupPhysics, *freezeApply;
    void* simEnable;
    const int32_t* forceDisableAsync;
    uint32_t sceneSolver, asyncDt, rewindCallback, rewindData, shuttingDown, evolution;
    uint32_t worldPhysicsByte, worldScene, refCount;
    uint8_t worldPhysicsBit, completeShift;
};
bool FindCollisionPrepare();                  // locate everything (no patching); false = fix unavailable
void SetCollisionPrepareEnabled(bool enabled);  // switchable live (needs the fade-to-game hook installed)
bool CollisionPrepareEnabled();
bool PrepareCollisionBegin(void* world);      // game thread, before the game's fade-to-game; true = physics work dispatched
void PrepareCollisionEnd();                   // game thread, after the game's fade-to-game
void LogCollisionPrepareStats();
bool CollisionPrepareInfo(CollisionPrepareFixInfo* out);
void CollisionPrepareTestFunctions(void* fakeDispatch, void* fakeCopy, void* fakeRecycle, const int32_t* fakeSimEnable,
                                   const int32_t* fakeForceDisableAsync);  // self-test only

// name_lookup_guard.cpp (NameLookupCrashGuard: crash guard for the old engine's name table)
struct EditorIdGuardInfo {
    uint8_t* map;                 // the NiTStringMap (name -> form)
    void** vtableSlot;            // its key-compare slot (now the guard)
    uint8_t* original;            // the game's compare: return _stricmp(a, b) == 0
    void* const* stricmpImport;   // the game's _stricmp import slot, called by the guard
    void* hook;
    uint64_t faults;              // compares that faulted on a freed key and were treated as "no match"
};
bool ApplyEditorIdLookupGuard();             // safe at any time (one vtable slot)
void SetEditorIdLookupGuardEnabled(bool enabled);  // switchable live; off = the game's compare unguarded
void LogEditorIdLookupGuardStats();
bool EditorIdLookupGuardInfo(EditorIdGuardInfo* out);
int EditorIdGuardFilter(const EXCEPTION_RECORD* rec, const char* key);  // the guard's exception filter
void EditorIdLookupGuardTestImport(void* const* fakeImport);  // self-test only: the import slot to call

// object_replace_guard.cpp (ObjectReplaceCrashGuard: StaticAllocateObject's replace-in-place destroy that keeps the slot)
struct ObjectReplaceGuardInfoOut {
    uint8_t* site;                  // the matched destroy sequence (lea rcx,[cs]; EnterCriticalSection; ...)
    uint8_t* call;                  // the patched 8 bytes (call stub; nop dword)
    uint8_t* stub;
    uint8_t* objectArray;           // GUObjectArray
    int32_t oldIndexOffset;         // OldIndex's place in StaticAllocateObject's frame ([rsp+off])
    const uint8_t* baseVtable;      // UObjectBase's vtable (written by ~UObjectBase)
    const uint8_t* objInitialized;  // GObjInitialized
    const uint8_t* criticalError;   // GIsCriticalError
    uint64_t replaced, kept, released;
};
bool ApplyObjectReplaceGuard();            // start-up (one call site in StaticAllocateObject)
void SetObjectReplaceGuardMode(int mode);  // live: 1 on; lab 2 = log only
void LogObjectReplaceGuardStats();
bool ObjectReplaceGuardInfo(ObjectReplaceGuardInfoOut* out);
bool ObjectReplaceGuardTestPatched();                  // self-test only
void ObjectReplaceGuardTestArray(uint8_t* fakeArray);  // self-test only: a fake GUObjectArray

// pso_pool.cpp (PsoCompileThreadPool: r.pso.PrecompileThreadPoolSize set while the game starts)
bool FindPsoCompileThreadPool();           // no patching; false = unavailable (logged)
void SetPsoCompileThreadPool(int threads, int priority); // start-up, once, after InstallTaskWaitGuard: 0 = the game's, N, -1 = logical - 2;
                                                          // applied only with the guard installed in mode 1 (else logged);
                                                          // priority: the pool threads' EThreadPriority (-1 = UE's below normal)
void PsoCompileThreadPoolTick(int threads, int priority); // once a second: later ini changes are ignored (logged once), Engine.ini check
bool PsoCompileThreadPoolApplied();        // the pool size we wrote is in force (the guard must then stay in mode 1)
void PsoCompileThreadPoolTestReset();      // self-test only
bool PsoCompileThreadPoolInfo(int32_t** size, int32_t** percent, int32_t** priority = nullptr);

// task_wait_guard.cpp (TaskWaitDeadlockGuard: the task graph's worker wait, woken by its reserve worker too)
struct TaskWaitGuardInfoOut {
    uint8_t* wait;                  // FTaskGraphCompatibilityImplementation::WaitUntilTasksComplete
    void** slot;                    // its vtable slot
    uint8_t* doReserve;             // FReserveScheduler::DoReserveWorkUntil
    uint8_t* trigger;               // TriggerEventWhenTasksComplete
    uint8_t* reserveScheduler;      // FReserveScheduler singleton
    uint8_t* scheduler;             // LowLevelTasks::FScheduler singleton
    const uint32_t* tlsIndex;       // the exe's TLS index (FSchedulerTls)
    uint32_t flagOffset;            // bReserveWorkersEnabled in the task graph object
    uint32_t triggerThread;         // the trigger task's thread (ENamedThreads value the game passes)
};
struct TaskWaitGuardCounters {
    uint64_t rescues = 0, lateTriggers = 0;
    int64_t orphans = 0;
};
bool FindTaskWaitGuard();                  // no patching; false = unavailable (logged)
bool InstallTaskWaitGuard(int mode);       // one vtable slot (start-up; afterwards the mode is live)
void SetTaskWaitGuardMode(int mode);       // 0 the game's wait, 1 on, 2 measure only (ignored while PsoCompileThreadPool runs: it needs 1)
int TaskWaitGuardMode();                   // the mode now; -1 = not installed
void TaskWaitGuardTick();                  // once a second: logs waiters left without a trigger task for 1 s or more
void LogTaskWaitGuardStats();
bool TaskWaitGuardInfo(TaskWaitGuardInfoOut* out);
void TaskWaitGuardTestSet(void* doReserve, void* trigger, uint8_t* reserveScheduler, uint32_t triggerThread);  // self-test only
bool TaskWaitGuardTestWait(void* self, const void* tasks, int mode);  // self-test only: the worker branch with the fakes
TaskWaitGuardCounters TaskWaitGuardTestCounters();                    // self-test only (resets the counters it returns)
bool TaskWaitGuardTestConditionCheck(uint8_t** call);                  // self-test only: the stock-condition layout check again (call: the stock Call)
int64_t TaskWaitGuardTestLiveWaits();                                 // self-test only: wait objects alive (0 when every wait is over)


// trap_guard.cpp (TrapCrashGuard: crash guard for the old engine's trap listener, a destroyed actor still in a trap's list)
struct TrapGuardInfo {
    uint8_t* update;            // TESTrapListener's update (the original)
    uint8_t* remove;            // the game's removal of one actor from a trap node
    void** slot;                // the listener vtable's update slot (now the guard)
    void* hook;
    const uint8_t* actorType;   // RTTI type descriptor of Actor
    uint32_t nodes, actors, target, next, nodeNext, hitSlot;
    uint64_t checked, removed;
};
bool ApplyTrapTargetGuard();                       // safe at any time (one vtable slot)
void SetTrapTargetGuardEnabled(bool enabled);      // switchable live; off = the game's update unguarded
void LogTrapTargetGuardStats();
// havok_pick_warning.cpp (HavokPickWarningGuard: the old engine's "Havok picks" warning box, a modal MessageBoxA on the OblivionThread
// that stops the game, is held back; the pick cap iMaxPicks stays)
struct HavokPickGuardInfo {
    void** slot;                // the warning handler pointer in .data (now the guard)
    void* original;             // the game's handler (MessageBoxA)
    void* hook;
    const uint8_t* callReturn;  // the pick warning's return address: the one caller held back
    const uint32_t* picks;      // the per-frame pick counter
    const uint32_t* max;        // iMaxPicks
    uint64_t heldBack, forwarded;
};
bool ApplyHavokPickWarningGuard();                    // safe at any time (one pointer)
void SetHavokPickWarningGuardEnabled(bool enabled);   // switchable live; off = the box as in the stock game
void LogHavokPickWarningGuardStats();
bool HavokPickWarningGuardInfo(HavokPickGuardInfo* out);
void HavokPickWarningGuardTestFunctions(void* fakeHandler, const void* returnAddress);  // self-test only
bool TrapTargetGuardInfo(TrapGuardInfo* out);
void TrapTargetGuardTestFunctions(void* fakeUpdate, void* fakeRemove);  // self-test only
bool TrapTargetGuardTestIsActor(const void* object);                   // self-test only: the guard's liveness check
// FrameGenCrashGuard (framegen_guard.cpp): AMD FidelityFX frame generation's command list pool waits for a free list instead of
// crashing. Installed when amd_fidelityfx_dx12.dll loads (or at once if it is loaded before the game's code runs).
bool WatchFrameGenLibrary(bool beforeGameCode);
void SetFrameGenCrashGuardEnabled(bool enabled);  // switchable live
void LogFrameGenCrashGuardStats();
struct FrameGenGuardInfo {
    uint8_t* module = nullptr;   // amd_fidelityfx_dx12.dll
    uint8_t* acquire = nullptr;  // its command list pool function
    int calls = 0;               // calls redirected
    uint64_t waits = 0, gaveUp = 0;
};
bool FrameGenCrashGuardInfo(FrameGenGuardInfo* out);
void* FrameGenCrashGuardTestAcquire(void* pool, void* queue, const void* name);  // self-test: the wrapper
// FrameGenAsyncCompute (framegen_async.cpp): frame generation's GPU work on its own queue (r.FidelityFX.FI.AllowAsyncWorkloads 1),
// set as soon as the game registers the variable, before the renderer starts. Start-up only.
bool FindFrameGenAsyncCompute();                       // locate the registration (no change); false = unavailable (logged)
bool ApplyFrameGenAsyncCompute(bool beforeGameCode);   // the thread that sets it once registered
void LogFrameGenAsyncComputeState();                   // once: whether something set it back later
int32_t* FrameGenAsyncComputeValueForTest();           // self-test: the registered value (nullptr before registration)
// FrameGenCameraInfo (framegen_camera.cpp, optional): the camera desc AMD's ML frame generation needs (position, up/right/forward)
// linked to the game's frame generation prepare call. Installed at start-up only; afterwards the ini switches it live.
bool FindFrameGenCameraSite();                          // locate the site and the view layout (no change)
bool ApplyFrameGenCameraInfo(bool beforeGameCode);      // the stub, the call to it, the ffxDispatch counter
void SetFrameGenCameraInfoEnabled(bool enabled);        // switchable live
void LogFrameGenCameraInfoStats();
struct FrameGenCameraInfoOut {
    uint8_t* site = nullptr;  // the game's pNext store (now the call to the stub)
    uint8_t* stub = nullptr;
    int32_t descDisp = 0;     // the prepare desc at [rbp+descDisp]
    uint32_t scale = 0, location = 0, matrices = 0;  // FSceneView offsets
    uint64_t attached = 0, rejected = 0, prepares = 0, withCamera = 0, withOurs = 0;
};
bool FrameGenCameraInfoTestInfo(FrameGenCameraInfoOut* out);
bool FrameGenLastCamera(float pos[3], float fwd[3], uint64_t maxAgeMs);  // the last FG view's camera (FrameGenCameraInfo)
void FrameGenCameraInfoTestAttach(const void* view, void* desc);  // self-test: what the stub calls
// self-test: redirects the module's ffxDispatch export to the counter (once) and swaps in `original` as the function
// it passes calls to; returns the previous one (nullptr if the export could not be redirected)
void* FrameGenCameraInfoTestDispatch(void* module, uint32_t sizeOfImage, void* original);
// ---- framegen_pacing.cpp (FrameGenPacingTuning) ----
bool ApplyFrameGenPacingTuning(bool beforeGameCode, int mode);  // watches AMD's library load (start-up only)
void SetFrameGenPacingTuning(int mode);                          // switchable live (re-configures the swap chain)
void LogFrameGenPacingStats();
bool FrameGenPacingTestRun(void* fakeCreate, void* fakeDestroy, void* fakeConfigure, bool supported, int mode, void** outContext);
void FrameGenPacingTestSwitch(int mode);
void FrameGenPacingTestQuery(void* fakeQuery);  // self-test: ffxQuery for the next contexts (nullptr = none)
void FrameGenPacingTestDestroy(void* context);
int FrameGenPacingTestApplied();
const char* FrameGenPacingTestProvider(void* fakeCreate, void* fakeQuery, uint64_t type);
// The frame generation call order (configure -> prepare -> generate per frameID) seen at AMD's exports (log only)
struct FrameGenOrderStats {
    uint64_t configures = 0, prepares = 0, dispatches = 0;          // calls seen
    uint64_t inOrder = 0, noPrepare = 0, noConfigure = 0, outOfOrder = 0;  // per generation dispatch
    uint64_t idJumps = 0;                                           // prepare frameIDs that did not step by one
};
// self-test: one call through the redirected ffxConfigure / ffxDispatch (passed on to the fakes); stats since reset
void FrameGenOrderTestReset(void* fakeConfigure, void* fakeDispatch);
uint32_t FrameGenOrderTestCall(bool configure, void* desc);
FrameGenOrderStats FrameGenOrderTestStats();
void* FrameGenPacingTestContext();
// Which AMD FidelityFX library the game loaded as amd_fidelityfx_dx12.dll, from its version resource. The
// game ships FSR 3.1.0 (1.0.0.36604); players may drop in AMD's FSR 3.1.4 DLL (1.0.1.41314, SDK 1.1.4) or the SDK 2.x
// loader (2.x) with its component DLLs.
enum class FsrLibrary { None, Game310, Sdk11Other, Sdk114, Sdk2 };
struct FsrLibraryInfo {
    FsrLibrary kind = FsrLibrary::None;
    uint16_t version[4] = {};
    bool pacingKey = false;      // the swap chain takes FRAMEPACINGTUNING (SDK 1.1.3+; known for 3.1.4 and 2.x)
    bool providerQuery = false;  // ffxQuery answers FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION (SDK 1.1.4+)
    char name[96] = {};
};
FsrLibraryInfo DescribeFsrLibrary(const void* module);  // module nullptr = the loaded amd_fidelityfx_dx12.dll

// display_modes.cpp (DisplayModeCache: the graphics settings page re-reads every monitor's display mode list 8 times)
struct DisplayModeCacheInfoOut {
    void** slot;            // the exe's import slot for USER32!EnumDisplaySettingsW (now the hook)
    uint8_t* edidRef;       // GetMonitorsInfo's reference to L"EDID"
    size_t numSites;        // its EnumDisplaySettingsW calls
    uint8_t* returns[8];    // their return addresses
    void* hook;
    uint64_t reads, fromCopy, fromWindows, modesAnswered;
};
// 1 = installed, 0 = not available (logged), -1 = the import slot is not filled yet (requireBound): call again later.
// Safe at any time (one pointer write).
int ApplyDisplayModeCache(bool requireBound);
void SetDisplayModeCacheEnabled(bool enabled);  // switchable live; off = every read goes to Windows (copies dropped)
void LogDisplayModeCacheStats();
bool DisplayModeCacheInfo(DisplayModeCacheInfoOut* out);
void* DisplayModeCacheTestSetup(void* fakeReal, bool anyCaller);  // self-test only: fake EnumDisplaySettingsW, any caller; returns the hook

// image_resize.cpp (MenuScreenshotFilter: the menu's screenshot goes through a same-size FImageUtils::ImageResize, pixel by pixel)
bool ApplyImageResizeFix(bool beforeGameCode);  // redirects one call (two bytes, atomic when they share a qword)
void SetImageResizeFixEnabled(bool enabled);    // switchable live; off = the game's own loop
void LogImageResizeStats();
bool ImageResizeFixInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook);
// vram_counter.cpp (VramCounterFix): the "Show VRAM" counter's query reads two adapter fields the shipping build
// never updates (0/0); with both 0, filled from the monitor thread's DXGI sample (memory_monitor.cpp)
bool ApplyVramCounterFix(bool beforeGameCode);  // redirects the query's two calls (start-up)
void SetVramCounterFix(bool on);                // live
void LogVramCounterFixStats();
struct VramCounterInfo {
    uint8_t* query;    // the game's query (budget MB, used MB)
    uint8_t* calls[2]; // the overlay's call and GetUsedVRAMMB's exec thunk's call
    uint8_t* stub;
    void* hook;
    uint64_t filled, gameValues, unknown;
};
bool VramCounterFixInfo(VramCounterInfo* out);
void VramCounterFixTestSetup(void* original, bool (*latest)(uint64_t*, uint64_t*));  // self-test
bool LatestGameVram(uint64_t* used, uint64_t* budget);  // memory_monitor.cpp: last DXGI sample (bytes); false = none yet
// shader_screen.cpp (ShaderScreenAfterDriverChange, game bug): the start-up's Save_Settings check (which decides the
// game's first-launch shader screen) answers "no save" once after the graphics card or driver changed
bool ApplyShaderScreenAfterDriverChange(bool beforeGameCode, const wchar_t* dir);  // start-up only; dir: the DLL's folder
bool ShaderScreenInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook);
void ShaderScreenTestSetup(void* original, bool (*gpu)(char*, size_t), const wchar_t* recordPath);  // self-test
void ShaderScreenPrecompileFinished();  // pso_precompile.cpp: the start-up precompile ended (records a pending card/driver)
// UE's ImageResize for equal sizes (2x2 box filter, truncated, alpha 255 if forceOpaque) on 4-byte FColor pixels
void BoxFilter2x2(int32_t w, int32_t h, const uint32_t* src, uint32_t* dst, bool forceOpaque);

// readback_convert.cpp (ScreenshotReadbackConvert: the menu screenshot's readback converts R10G10B10A2 to FColor one pixel at a time)
bool ApplyReadbackConvertFix(bool beforeGameCode);  // redirects the one call in RHIReadSurfaceData (start-up only)
void SetReadbackConvertMode(int mode);              // live: 0 the game's loop, 1 fast, 2 fast + verify
void LogReadbackConvertStats();
bool ReadbackConvertFixInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook);
uint32_t ConvertR10G10B10A2Pixel(uint32_t p);  // FColor::MakeRequantizeFrom1010102 as UE's conversion uses it
void ConvertR10G10B10A2ToColor(uint32_t width, uint32_t height, const uint8_t* in, uint32_t srcPitch, uint32_t* out);

// screenshot_parallel.cpp (ScreenshotParallel: the menu screenshot's copies into fresh 33 MB arrays, one thread each)
bool InstallOsPageCache(bool beforeGameCode, const wchar_t* traceDir, int trace);  // os_page_cache.cpp: the OS thunks' detours (start-up)
void SetOsPageCache(int mode, int capMB, int trimSeconds, bool relief);  // OsPageCache, live (1-second tick): also trims unused blocks
void LogOsPageCacheStats();
struct OsPageCacheStatsOut {
    uint64_t hits, misses, kept, passed, evictCap, evictSize, trimmed, flushed, allocFails, held;
    uint32_t blocks;
};
// self-test: the cache core with fake OS functions
void OsPageCacheTestInit(void* (*osAlloc)(SIZE_T), BOOL (*osFree)(void*), uint64_t (*nowMs)());
void* OsPageCacheTestAlloc(size_t size, bool cachePath);
BOOL OsPageCacheTestFree(void* ptr, size_t size);
void OsPageCacheTestStats(OsPageCacheStatsOut* out);
struct OsPageCacheInfoOut {
    uint8_t* allocFn;
    uint8_t* freeFn;
    const uint8_t* cacheRet[2];
};
bool OsPageCacheInfo(OsPageCacheInfoOut* out);
// pso_precompile.cpp (PsoPrecompileFast: the start-up PSO precompile at UE's Fast pace outside play)
bool InstallPsoPrecompileFast(bool beforeGameCode);  // SetBatchMode's entry detour (start-up)
void SetPsoPrecompileFast(bool enabled);             // live (the game thread's timer applies it at its next check)
void SetPsoPrecompileFastInGameMenus(bool enabled);  // live: Fast also in menus after play started (default: the game's pace)
void SetPsoPrecompileScreenHold(bool on);  // ShaderScreenAfterDriverChange: UE's Fast, no cold guard, until the main menu
bool PsoPrecompileScreenHold();
void LogPsoPrecompileStats();
struct PsoPrecompileInfoOut {
    uint8_t* setBatchMode;
    uint8_t* trampoline;
    void** singleton;          // static FShaderPipelineCache*
    uint32_t flagOffset;       // bHasShaderPipelineTask
    uint32_t* batchSize;
    float* batchTime;
    void* hook;
    void* timerProc;
    int requested, applied, inPlay;
    bool timerRunning;
    uint64_t switchesFast, handBacks, replaced, fastMs, doneMs;
    bool bounded;              // the faster pace runs with the bounded render-thread budget (not UE's Fast values)
    bool cold;                 // this precompile was found slow (cold driver cache): the game's own pace until it ends
    int64_t* active;           // TotalActiveTasks (null: not found)
    const char* coldWhy;       // which cold-cache guard tripped ("" while none)
    int driverThreads;         // the driver's compile threads held now (monitor thread)
    int driverSearches;        // searches for them in this precompile
    bool driverGaveUp;         // none found in three searches: no driver-thread guard for this precompile
    double driverSearchMs;     // the last search's duration
};
bool PsoPrecompileInfo(PsoPrecompileInfoOut* out);
void PsoPrecompileTestOriginal(void* fakeSetBatchMode);  // self-test: a fake instead of the game's SetBatchMode, state reset
void PsoPrecompileTestPlayMs(uint32_t ms);  // self-test: how long play must be seen before "play has started" (default 3000)
void PsoPrecompileTestTiming(uint32_t fastMaxMs, uint32_t driverRetryMs);  // self-test: the 60 s limit, the 10 s search interval
void SetPsoPrecompileFastBudget(float ms, int coldQueue);  // live: render-thread ms per frame at the faster pace; the queue that
                                                           // means a cold driver cache (0: no cold-cache guard at all)
void PsoPrecompileTestStatics(uint32_t* batchSize, float* batchTime, int64_t* active);  // self-test: fakes for the statics
void PsoPrecompileWatchDriver();               // monitor thread, once a second: the driver's compile threads (cold-cache guard)
void PsoPrecompileTestDriverBusy(double cores);  // self-test: one second's reading of the driver's compile threads
int PsoPrecompileTestOwnThreads(uint32_t* mainThread);  // self-test: this process's threads (NtQuerySystemInformation) and the first one
// anim_teardown.cpp (AnimTeardownParallel, lab prototype: dying NPC animation data's interpolators on helper threads; off by default)
bool FindAnimTeardownParallel();                         // locate and verify everything (no patching)
bool InstallAnimTeardownParallel(bool beforeGameCode);   // start-up only (three calls redirected)
void SetAnimTeardownParallel(int mode);                  // live: 0 off, 1 on
void LogAnimTeardownParallelStats();
struct AnimTeardownTest {  // self-test: fakes instead of the game's functions and classes
    void* iterator; void* free; void* blockDtor; void* animData; uintptr_t vt[8]; uint8_t* keyTable; uint8_t* empty;
};
bool AnimTeardownParallelTestSetup(const AnimTeardownTest& t, int helpers);
void AnimTeardownParallelTestIter(void* arr, uint64_t size, uint64_t count, void* dtor);  // the patched calls, as stubs
void AnimTeardownParallelTestFree(void* p, uint64_t size);
uint64_t AnimTeardownParallelTestAnimData(uint64_t ad);
struct AnimTeardownCounts { uint64_t actors, arrays, blocks, byHelper, byOwner, fallback, inlined; };
AnimTeardownCounts AnimTeardownParallelTestCounts();
bool AnimTeardownParallelTestKeyTables(uint8_t* table);  // KeyTablesOk on a given 18-slot table (after Find)
// cell_buffer.cpp (CellBufferPacedEvict CellBufferPacedEvict, lab prototype): spare exterior cell buffer slots, overflow evicted one
// cell per OT frame after a crossing
bool FindCellBufferPacedEvict();                          // locate and verify everything (no patching)
bool InstallCellBufferPacedEvict(bool beforeGameCode);   // start-up only (before the TES constructor; last: chains detours)
void SetCellBufferPacedEvict(int mode);                  // live: 0 crossing-frame teardown (as the game), 1 paced
void LogCellBufferPacedEvictStats();
bool BuildCellBufferClampStub(uint8_t* mem, const void* grids, const void* bound, const void* back);  // mem: 2 pages
struct CellBufferInfoOut {
    uint8_t *clamp, *grids, *bound, *insert, *teardown, *dataHandler, *grid, *site1Ret, *site2Ret, *tesSlot, *flagsSlot,
        *writeSetting;
};
bool CellBufferTestInfo(CellBufferInfoOut* out);
struct CellBufferTest {
    void* grid;      // uint64_t(tes, pos, flag, r9): returns the shift flag
    void* teardown;  // uint64_t(dataHandler, cell)
    uint64_t* dataHandler;
    uint32_t* bound;
    volatile uint32_t* base;
    volatile uint32_t* cap;
    volatile uint8_t* done;
    uint64_t* flagsSlot;
    void* site1Ret;
    void* site2Ret;
};
void CellBufferTestSetup(const CellBufferTest& t);
uint64_t CellBufferTestGridHook(const void* caller, uint64_t tes, uint64_t pos, uint64_t flag);
struct CellBufferCounts { uint64_t trimmed, paced, immediate, blocked, crossings, maxOverflow; };
CellBufferCounts CellBufferTestCounts();
bool ApplyScreenshotParallelFix(bool beforeGameCode);  // patches the copy calls, the readback row loop, the zero fill (start-up)
void SetScreenshotParallel(int mode);                  // live: 0 the game's way, 1 helpers + zero fill skipped
bool ScreenshotParallelOn();
void LogScreenshotParallelStats();
// MenuScreenshotFilter and ScreenshotReadbackConvert: fn over row bands of an image on the helpers when ScreenshotParallel is on (else all rows on the caller)
bool ScreenshotParallelRows(uint32_t rows, size_t bytes, void (*fn)(const void* ctx, uint32_t y0, uint32_t y1), const void* ctx);
void* BigMemcpyHook(void* dst, const void* src, size_t bytes);
void RowCopyHook(uint8_t* dst, const uint8_t* src, uint64_t bytesPerLine, uint32_t pitch, uint64_t rows);
struct ScreenshotParallelSites {
    int copySites, helpers;
    uint8_t *rowSite, *rowStub, *memsetSite, *memsetStub;
    const uint64_t* zeroSkips;
};
bool ScreenshotParallelInfo(ScreenshotParallelSites* out);
void BoxFilter2x2Rows(int32_t w, int32_t h, const uint32_t* src, uint32_t* dst, bool forceOpaque, int32_t y0, int32_t y1);

// focus_restore.cpp (RestoreFsrAfterAltTab: after an alt-tab, FSR and frame generation stay off until the upscaler is applied again)
struct FocusRestoreInfoOut {
    uint8_t* handler;          // Altar's focus handler (now jumps to handlerHook)
    uint8_t* setter;           // VAltarUISubsystem's menu-visibility function (now jumps to setterHook)
    void** fsrCvarSlot;        // the handler's static IConsoleVariable* r.FidelityFX.FSR3.Enabled
    void** altarFsrCvarSlot;   // IConsoleVariable* Altar.FSR3.Enabled
    uint32_t getBoolOffset;    // IConsoleVariable::GetBool vtable offset
    uint32_t getFlagsOffset;   // IConsoleVariable::GetFlags vtable offset (the handler keeps the set-by priority)
    uint32_t setOffset;        // IConsoleVariable::Set(const TCHAR*, EConsoleVariableFlags) vtable offset
    const wchar_t* one;        // the handler's own L"1"
    uint32_t menuFlagOffset;   // VAltarUISubsystem::bIsMenuVisible
    bool freezeAvailable;      // the freeze subsystem was found (fade_hook.cpp)
    void* handlerHook;
    void* setterHook;
    void* timerProc;           // the game-thread timer's callback (TIMERPROC)
    uint8_t* handlerTrampoline;
    uint8_t* setterTrampoline;
    uint64_t restored, restoreCalls, restoredDirect, timerChecks;
    bool pending, timerRunning;
};
bool ApplyFocusRestoreFix(bool beforeGameCode);  // two function entries, each one atomic 8-byte write
void SetFocusRestoreEnabled(bool enabled);       // switchable live; off = the game's behaviour
void LogFocusRestoreStats();
bool FocusRestoreInfo(FocusRestoreInfoOut* out);
void FocusRestoreTestOriginals(void* fakeHandler, void* fakeSetter);  // self-test only: fakes for the game's two functions

// physanim_lock.cpp (PhysAnimNoEmptyLock, public since 1.2.3): a physical animation component with no driven body and no
// pending parallel animation task skips UpdateTargetActors' scene write lock (the game's code otherwise; identical)
bool InstallPhysAnimNoEmptyLock(bool beforeGameCode);  // the function's entry redirected (only while the game starts)
void SetPhysAnimNoEmptyLock(bool enabled);              // live, once installed
void LogPhysAnimNoEmptyLockStats();
void PhysAnimTestCall(uint8_t* self, uint8_t teleport, void* fakeGame);  // self-test only
const uint8_t* PhysAnimTestTrampoline();   // self-test only: the trampoline (prologue + absolute jump back)
uint32_t PhysAnimTestOffset(int which);     // self-test only: 0 DriveData.Num, 1 mesh, 2 pending task
// chaos_payload.cpp (ChaosMaxPayloadSize, public since 1.2.3): p.MaxPayloadSize raised at start-up so that the ~730 large
// elements stay in the Chaos acceleration trees instead of the GlobalPayloads list every scene query walks (equivalent)
float* FindChaosMaxPayloadVar();                      // p.MaxPayloadSize's bound value (nullptr if not found)
bool SetChaosMaxPayloadSize(int cm, bool beforeGameCode);  // start-up, both routes; only over the game's 100000
void LogChaosMaxPayloadSizeLive();                    // periodic report: the live value once, then on change
// rig_trace_reuse.cpp (RigTraceReuse, public since 1.2.3): a Control Rig sphere
// trace repeated with bit-identical inputs inside one UControlRig::Execute reuses that run's earlier result (equivalent)
bool InstallRigTraceReuse(bool beforeGameCode, const wchar_t* iniPath);  // start-up only (version.dll route)
void SetRigTraceReuse(int mode);                                         // live, once installed: 0 off, 1 reuse, 2 verify
void LogRigTraceReuseStats();
uint32_t RigTraceUnitDiscover(const GameModule& game);     // self-test: the unit's RVA (0 = not found); no writes
uint32_t RigTraceExecuteDiscover(const GameModule& game);  // self-test: UControlRig::Execute's RVA (0 = not found)
void RigTraceTestCall(uint8_t* ctx, const double* start, const double* end, const uint8_t* channel, float radius, uint8_t* hit,
                      double* loc, double* nrm, void* fakeUnit);  // self-test only
uint64_t RigTraceTestExecute(void* fakeExec, void* self);         // self-test only
void RigTraceTestSetReuse(bool on);                               // self-test only
uint64_t RigTraceTestReused();                                    // self-test only
void RigTraceTestSetMode(int mode);                               // self-test only: 0 off, 1 reuse, 2 verify
uint64_t RigTraceTestVerified(uint64_t* diff);                    // self-test only: verified repeats (and how many differed)
uint64_t NpcPacingFrame();  // npc_pacing.cpp: its frame counter (counts while NpcCreationPacing's frame hook runs)
// tick_split.cpp (diagnostic TickSplit, [Fixes] HitchLedgerTickSplit, public since 1.2.3, needs HitchLedger=1): the game
// thread's tick time by tick-function kind, ticked class and tick group into OblivionPerfFix-ticksplit.csv.
struct TickSplitFrame {        // from LedgerFrameEnd, every frame
    bool recording, slow;
    uint64_t frame, endUs;
    uint32_t periodUs, actorUs, pairUs;
    uint32_t tgUs[8];
};
struct TickSplitInfoOut {
    bool installed;
    uint8_t *exec, *tickable, *setup, *coll, *latent, *timer, *tickObj, *stream, *region, *tNative, *tDynamic, *tCallback;
    void *execHook, *tickableHook;
};
bool InstallTickSplit(bool beforeGameCode, const wchar_t* dir);  // start-up only, after InstallHitchLedger
void SetTickSplit(bool on);                                      // monitor thread, live
void TickSplitGroup(int group, bool entering);                   // game thread: the ledger's tick-group hook
void TickSplitPreActor();                                        // game thread: OnWorldPreActorTick
void TickSplitFrameEnd(const TickSplitFrame& fr);                // game thread: every frame end
void TickSplitWindowEnd();                                       // game thread: hands a 2 s window to the monitor thread
void TickSplitDrain();                                           // monitor thread: windows and slow-frame rows -> csv
bool TickSplitInfo(TickSplitInfoOut* out);
void TickSplitTestSetup(uint64_t (*clock)(), const wchar_t* path);  // self-test only
void TickSplitTestClose();                                       // self-test only
// menu_open_flash.cpp (MenuOpenFlashFix, issue #5, game bug: the bright wrong frame when the player menu opens; occlusion
// culling off for the menu-open frame; start-up install after ApplyFocusRestoreFix, then live)
struct MenuOpenFlashInfoOut {
    uint8_t* jmpSite;     // RequestCapture's exec thunk jmp (now to requestHook)
    void* request;        // VBackgroundWidgetBase::RequestCapture
    void* processShots;   // UGameViewportClient::ProcessScreenShots (the restore point, every frame)
    void** shotSlots[4];  // its vtable slots (now shotsHook)
    int numShotSlots;
    void** cvarSlot;      // IConsoleVariable* r.AllowOcclusionQueries
    const bool* cvarRef;  // the bool it binds (GOcclusionCullEnabled)
    void* requestHook;
    void* shotsHook;
    bool armed, broken;
};
bool InstallMenuOpenFlashFix(bool beforeGameCode);
void SetMenuOpenFlashFix(bool on);
void LogMenuOpenFlashStats();
bool MenuOpenFlashInfo(MenuOpenFlashInfoOut* out);
// self-test only: fakes for the variable (slot, bound bool), RequestCapture and ProcessScreenShots; the caller is the game thread
void MenuOpenFlashTestSetup(void** fakeCvarSlot, const bool* fakeRef, void* fakeRequest, void* fakeShots, uint32_t setOffset,
                            uint32_t getBoolOffset);
void MenuOpenFlashTestAgeRestorePoint(int64_t ms);  // self-test only: the last ProcessScreenShots seen this much earlier
const char* MenuOpenFlashLastBrokenLog();           // self-test only: the last "fix off for this session" line ("" = none)


// stream_churn.cpp (LumenKeepArrayCapacity and UploadMemoryKeep: memory churn while the world streams)
struct StreamChurnFixInfo {
    uint8_t* lumenCall[5];        // ResetAndConsolidate's shrinking calls
    uint8_t* lumenOriginal[5];    // RemoveAt / ResizeShrink
    bool lumenIsRemoveAt[5];
    uint8_t* uploadCall[3];       // EndFrame: small block, big block, fast constant clean-up
    uint8_t* buddyClean;          // FD3D12MultiBuddyAllocator::CleanUpAllocations
    uint8_t* poolClean;           // FD3D12PoolAllocator::CleanUpAllocations
    volatile LONG* minPages;      // d3d12.FastAllocator.MinPagesToRetain
    LONG minPagesOriginal;
    volatile LONG* chaosForce;    // p.Chaos.AccelerationStructureTimeSlicingMaxQueueSizeBeforeForce (PhysicsTreeTimeSlicing)
    LONG chaosForceOriginal;
    volatile LONG* chaosIterations;  // p.IterationsPerTimeSlice (PhysicsTreeTimeSlicing; nullptr if not found)
    LONG chaosIterationsOriginal;
    volatile LONG* chaosCopyBytes;   // p.Chaos.AccelerationStructureTimeSlicingMaxBytesCopy (PhysicsTreeTimeSlicing)
    LONG chaosCopyBytesOriginal;
    void *removeAtHook0, *shrinkHook1, *buddyHook, *poolHook;
    uint8_t* lumenGrowCall;       // PrimitiveGroups grow (nullptr if not found)
    uint8_t* lumenGrowOriginal;   // TArray<80-byte>::ResizeGrow
    void* growHook;
};
// GpuSceneKeepCapacity (GPU scene buffers keep their capacity)
struct GpuSceneFixInfo {
    uint8_t* strideCall[2];       // the instance reserve's first GetDataStrideInFloat4s call in each UpdateBufferState
    uint8_t* strideFn;            // FInstanceSceneShaderData::GetDataStrideInFloat4s
    uint8_t* resizeCall[4];       // ResizeStructuredBufferIfNeeded calls: PrimitiveData x2, InstancePayloadData x2
    uint8_t* resizeFn;            // ResizeStructuredBufferIfNeeded
    uint8_t* thunk;               // the reserve thunk (edi = policy(edi, rsi), then GetDataStrideInFloat4s)
    void* resizeHook;             // void* (void* graphBuilder, void** external, uint32_t numBytes, const wchar_t* name)
    const wchar_t* name[3];       // L"GPUScene.InstanceSceneData", L"GPUScene.PrimitiveData", L"GPUScene.InstancePayloadData"
};
bool ApplyGpuSceneKeepCapacityFix(bool beforeGameCode);
void SetGpuSceneKeepCapacityEnabled(bool enabled);  // switchable live
bool GpuSceneInfo(GpuSceneFixInfo* out);
void GpuSceneTestOriginals(void* stride, void* resize);  // self-test only: fakes for the game's two functions
// beforeGameCode: loaded from DllMain. Otherwise calls are only switched where that is one atomic write.
bool ApplyLumenKeepCapacityFix(bool beforeGameCode);
void SetLumenKeepCapacityEnabled(bool enabled);     // switchable live
bool ApplyUploadMemoryKeepFix(bool beforeGameCode);
void SetUploadMemoryKeep(bool enabled, int keepFrames, int keepPages, int releaseRampFrames);  // switchable live
bool FindChaosTimeSlicing();                      // PhysicsTreeTimeSlicing: locate the console variables (no patching)
void SetChaosTimeSlicing(bool enabled, int sliceIterations, int sliceCopyKB, bool adaptiveCopy);  // switchable live
void LogStreamChurnStats();

// ---- allocator_trim.cpp (CommandAllocatorTrim) ----
struct AllocatorTrimInfoOut {
    uint8_t *obtain, *release, *ctor, *pop, *freeLink, *linkBlocks;
    uint32_t queuesData, queuesInline, pool;
    uint8_t* finalize;
    uint32_t allocOff, queueTypeOff, defaultFlagOff;
};
bool FindAllocatorTrim();
bool InstallAllocatorTrim(bool beforeGameCode);       // three entry detours (counting is always on once installed)
void SetAllocatorTrim(bool enabled, int windowSeconds);  // switchable live
void SetImmediateAllocatorLists(int lists);           // part 2, switchable live; 0/1 = off
void SetAllocatorTrimPressure(bool pressure, bool tight);  // over budget -> every pool, short window; tight -> copy pool
void AllocatorTrimTick();                             // monitor thread, once a second
void LogAllocatorTrimStats();
bool AllocatorTrimInfo(AllocatorTrimInfoOut* out);
void* AllocatorTrimTestStub();                        // self-test only: the stand-in allocator object
const char* AllocatorTrimSelfTest();                  // self-test of the give-back (obtain between passes) and the latch
bool StreamChurnInfo(StreamChurnFixInfo* out);
void StreamChurnTestOriginals(void* removeAt0, void* shrink1, void* buddy, void* pool, void* grow);  // self-test only

// physics_flush.cpp (PhysicsFlushParallel: the catch-up of collision changes after a physics tree rebuild on two threads)
struct PhysicsFlushInfoOut {
    uint8_t* flush;               // FPBDRigidsEvolutionBase::FlushAsyncAccelerationQueue
    uint32_t data, num, internalCopy, externalCopy, stamp;  // evolution offsets
    uint32_t removeSlot, updateSlot;                        // collection vtable slots
    uint32_t hasBounds, bounds, gtParticle, unique;         // particle container offsets
    const uint8_t* dynamicTreeFlag;                         // p.Chaos.AccelerationStructureUseDynamicTree
    const uint8_t *query, *sim;                             // the handle's filter helpers
    const uint8_t* step;                                    // ComputeIntermediateSpatialAcceleration (nullptr: not found)
};
bool FindPhysicsFlush();                          // locate the function (no patching); false = unavailable (logged)
bool InstallPhysicsFlush(bool beforeGameCode);    // entry detours (flush + physics step) and the helper thread, once
bool PhysicsStepHooked();                         // the step detour is in place (PhysicsTreeTimeSlicing's adaptive copy slice)
void SetPhysicsCopyAdaptive(volatile LONG* var, LONG normal, LONG boost, bool on);  // PhysicsTreeTimeSlicing: the step hook owns var
void SetPhysicsFlushParallel(bool enabled, int minChanges);  // switchable live
void LogPhysicsFlushStats();
bool PhysicsFlushInfo(PhysicsFlushInfoOut* out);
void PhysicsFlushTestSetup(void* fakeOriginal, void* fakeQuery, void* fakeSim, const int32_t* fakeFlag);  // self-test only
void PhysicsFlushTestCall(uint8_t* evolution);                                                         // self-test only
void PhysicsStepTestSetup(void* fakeStep);                                                             // self-test only
void PhysicsStepTestCall(uint8_t* evolution, uint8_t block);                                           // self-test only

// instance_clear.cpp (InstanceClearReserve: clearing a big instanced mesh reserves its hide commands at once)
struct InstanceClearInfoOut {
    uint8_t *site, *reset, *resize;
    uint32_t cmdOff, renderOff, bufferOff, numOff;
    uint8_t countOff;
    uint32_t elemSize, alignment;
};
bool FindInstanceClearReserve();                  // locate the call (no patching); false = unavailable (logged)
bool InstallInstanceClearReserve(bool beforeGameCode);  // the call redirected (only while the game starts)
void SetInstanceClearReserve(bool enabled);       // switchable live
void LogInstanceClearStats();
bool InstanceClearInfo(InstanceClearInfoOut* out);
void InstanceClearTestSetup(void* fakeReset, void* fakeResize);  // self-test only
void InstanceClearTestCall(uint8_t* cmdBuffer);                  // self-test only

// navmesh_wait.cpp (NavmeshBuildNoBusyWait: navmesh tile builds never run inside another task's busy wait)
bool FindNavmeshBuildNoBusyWait();                // locate the tile task's start (no patching); false = unavailable (logged)
void SetNavmeshBuildNoBusyWait(bool enabled);     // one byte, switchable live
uint8_t* NavmeshBuildNoBusyWaitSite();            // the flags byte (self-test)
// BusyWaitFlagCheck: the busy-wait put-back returns instead of running the next task unchecked (busywait_check.cpp).
bool FindBusyWaitFlagCheck();                      // the four put-back sites (no patching); false = unavailable (logged)
void SetBusyWaitFlagCheck(bool enabled);           // one byte per site, switchable live
size_t BusyWaitFlagCheckSites(uint8_t** out, size_t max);  // the patched bytes (self-test)
void LogBusyWaitStats();                           // lab: navmesh tile builds inside busy waits (no-op in public builds)

// npc_pacing.cpp (NpcCreationPacing: far-away NPC creation spread over frames)
struct NpcPacingInfoOut {
    uint8_t *runSession, *handlerEnd, *createExec, *genericExec, *preTickReturn, *postTickReturn;
    void **runSessionSlot, **handlerEndSlot, **sessionVtable, **createSlot, **gEngine;
    int numGenericSlots;
    void *tes, *findSession, *weakGet, *getWorld, *getPc, *getPawn;
    void** delegateVtable[3];
    void* delegateExecIfSafe[3];
    size_t delegateSize[3];
    void* delegateCopyCtor[3];
    void* execIfSafeHook[3];
    uint32_t rootOffset, locationOffset;
    void *runHook, *endHook, *createHook, *genericHook;
    void* maxTickRate;
    int numMaxTickSlots;
    uint8_t* flowReturn[2];  // (c) the level flow runner's RunSession calls
    void* chooseWriter;
};
struct NpcPacingTestFakes {
    uint64_t (*clock)();
    bool (*player)(double*, double*, double*);
    int guards;  // -1 real, 0 fail, 1 pass
    void *run, *end, *findSession, *create, *generic, *weakGet, *chooseWriter;
    void* execIfSafe[3];
    void* dtor[3];
    size_t sizeOverride[3];
    uint32_t rootOffset, locationOffset;
};
struct NpcPacingTestState {
    uint64_t S, E, frame;
    size_t fifo;
    int units;
    bool inFrame;
    int heldFrames;
};
bool FindNpcPacing();                 // locate everything (no patching); false = fix unavailable (logged)
bool InstallNpcPacing();              // vtable slot writes, safe at any time
void SetNpcPacingFadeAware(int mode);  // PacingFadeAware (live; public 0/1, lab also 2/3); no effect if the fade tick was not found
bool NpcPacingFadeInfo(uint32_t offsets[5], void** staticClass);  // self-test: state, value, owner, actor's component, floor
void NpcPacingTestFade(const uint8_t* (*staticClass)());           // self-test: a fake owner class
void NpcPacingTestRendered(bool (*rendered)(const void* actor));   // self-test: fake "recently rendered" (nullptr = fakes never are)
void SetNpcPacing(bool enabled, int budgetMs, int minDistance, int maxLagFrames, bool useSlack, bool flowWait = true,
                  int flowWaitFrames = 8);  // switchable live
void SetNpcPacingLeanFlowDrain(bool on);  // PacingLeanFlowDrain (live)
void LogNpcPacingStats();
bool NpcPacingInfo(NpcPacingInfoOut* out);
void NpcPacingTestSetup(const NpcPacingTestFakes& fakes);  // self-test only
NpcPacingTestState NpcPacingTestRead();                    // self-test only
bool NpcPacingPlayerPos(double* x, double* y, double* z);  // game thread: the player's position (UE units), if known
// ClothBuildDefer (game thread): inside a unit of a far, unseen actor in a frame that defers: true, that unit's owner
// (its delegate's weak object pointer) and the pacing frame; and later, whether that owner is still far and unseen. Both
// walk past an owner chain's non-actors (an equipment unit's body pairing component), which pacing's own walk does not
bool NpcPacingDistantUnit(uint64_t* ownerWeak, uint64_t* frame);
bool NpcPacingStillDistant(uint64_t ownerWeak);
// ---- npcs/cloth_defer.cpp (ClothBuildDefer) ----
// RecreateClothSimulationProxy asked for inside a far, unseen actor's NpcCreationPacing unit while the component has no
// simulation yet waits (later requests merge into it) and is made in a later frame, within the pacing budget
bool InstallClothBuildDefer(bool beforeGameCode);   // start-up only (version.dll route; after ClothRegisterLite)
void SetClothBuildDefer(bool on, int quietFrames);  // live
void LogClothBuildDeferStats();
uint64_t ClothDeferFrame(uint64_t frame, bool flushAll, uint64_t spentUs, uint64_t budgetUs, bool unitsRan,
                         int* built = nullptr);  // NpcCreationPacing's frame start (game thread): the builds due now; their time (us)
void* ClothDeferTestRecreate();                                           // self-test only: the detoured entry
void ClothDeferTestSetFakes(void* recreate, void* unregister, void* suspended);  // self-test only; this thread plays the game thread
void ClothDeferTestSetDistant(int mode, uint64_t frame);                  // self-test only: -1 = NpcCreationPacing decides, 0/1 forced
bool ClothDeferTestSites(void*** unregisterSlot, void** suspendedCall, void** enabledThunk);  // self-test only
void* ClothDeferTestUnregisterNext();                                     // self-test only: what OnUnregister's hook calls
void ClothDeferTestCallRecreate(uint8_t* component);                      // self-test only
void ClothDeferTestCallUnregister(uint8_t* component);                    // self-test only
bool ClothDeferTestCallSuspended(uint8_t* component);                     // self-test only
size_t ClothDeferTestPending();                                           // self-test only
// hitch_ledger.cpp (diagnostic HitchLedger): per slow game-thread frame, its stage times into
// OblivionPerfFix-ledger.csv (docs/stutter-ledger.md). NpcCreationPacing's frame hooks feed it.
struct LedgerPacing {          // NpcCreationPacing's cumulative game-thread counters at a frame end (us)
    uint64_t unitCumUs = 0, unitCumN = 0;  // NPC units (NpcCreationPacing's unit time)
    uint64_t pairCumUs = 0;    // the pre-actor-tick RunSession(-1) calls without their units: the wait for the old engine
    uint64_t flowCumUs = 0;    // old-engine level flows run by the game thread (without units)
    double capPeriodUs = 0;    // the engine's frame-rate cap (0 = none)
    bool travel = false;       // the old engine's fade travel pending
};
bool InstallHitchLedger(bool beforeGameCode, const wchar_t* dir, bool pacingInstalled);  // start-up only
bool InstallHitchLedgerGrid(bool beforeGameCode);  // start-up, last (after the lab's CellRowDiag)
void SetHitchLedger(bool on, int thresholdMs);     // monitor thread, live
void HitchLedgerDrain();                           // monitor thread, once a second: ring -> csv
void LedgerPreActorTick();                         // game thread (NpcCreationPacing's hooks and the fade hook)
void LedgerPostActorTick();
void LedgerSleepBegin();
void LedgerFrameEnd(const LedgerPacing& p);
void LedgerFadeToGame(uint64_t us);
uint64_t LedgerClockUs();                          // the ledger's clock (QPC in us; 0 when not installed)
bool LedgerIsGameThread();                         // installed and on the game thread (its id from the first frame end)
const volatile uint32_t* FindGpuFrameTime();       // GGPUFrameTime (FPlatformTime cycles), nullptr if not found
uint64_t StaticBodiesGtUs();                       // static_bodies.cpp: cumulative game-thread us in body creation / scene add
struct HitchLedgerInfoOut {
    bool installed;
    uint8_t *tg, *syncCall, *sync, *grid;
    int64_t *psoTasks, *psoTime;
    uint8_t** psoSingleton;
    uint32_t psoFlag;
    uint32_t* gpu;
    void *tgHook, *syncHook, *gridHook;
    size_t recordSize;
};
bool HitchLedgerInfo(HitchLedgerInfoOut* out);
void HitchLedgerTestSetup(uint64_t (*clock)(), void* fakeTg, void* fakeSync, const wchar_t* path);  // self-test only
void HitchLedgerTestGridAdd(uint64_t us);          // self-test only: an OT grid change of this length
void HitchLedgerTestClose();                       // self-test only: closes the csv
// translate_frame_fence.cpp (RdgParallelTranslate guard): an RHI thread fence after every FRHICommandListImmediate::BeginFrame, as
// UE 5.5 orders its BeginFrame, so the buffer defragmentation can run while parallel translate is on.
bool FindFrameFence();                         // locate BeginFrame, RHIThreadFence, the event release (no patching)
bool InstallFrameFence(bool beforeGameCode);   // detour BeginFrame's entry (safe while the game runs)
bool FrameFenceAvailable();
void SetFrameFence(bool on);                   // monitor thread
bool FrameFenceReady();                        // on, and the first fence has run: every BeginFrame since is fenced
void LogFrameFenceStats();
struct FrameFenceInfoOut {
    uint8_t *beginFrame, *threadFence, *recycle, *commandVtable, *rhiThreadFlag;
    uint32_t refCount;
    uint8_t completeShift;
};
bool FrameFenceInfo(FrameFenceInfoOut* out);
void FrameFenceTestFakes(void* fakeThreadFence, void* fakeRecycle);  // self-test only
void SetRdgUseFrameFence(bool use);            // parallel_translate.cpp: [Fixes] RdgFrameFence
// memory_monitor.cpp (the GPU crash report and MemoryRelief MemoryRelief)
bool InstallGpuCrashReport();         // a vectored exception handler (UE's GPU-crash exception 0x8000); safe at any time
// Once a second: samples memory and decides MemoryRelief; RAM (12/13), VRAM (13 first, then also 4).
struct MemoryRelief {
    bool lumen = false;      // LumenKeepArrayCapacity as stock (commit short)
    bool upload = false;     // UploadMemoryKeep as stock (RAM/commit short or either VRAM stage)
    bool translate = false;  // RdgParallelTranslate paused (VRAM stage 2 only)
    bool osCache = false;    // OsPageCache as stock (RAM or commit short)
};
void UpdateMemory(const Config& cfg, MemoryRelief* relief);
void LogMemoryStats();
void* GpuCrashReportTestFind(const void* words, size_t count);  // self-test: the device found among words (AddRef'd)
// MemoryRelief's video-memory level (VramRelief): what one step (one second) did. Self-test: one step of a test instance,
// reset = start a fresh one; relief = VRAM overrides after the step (RAM/commit handled independently).
enum class VramReliefEvent { None, Engaged, Escalated, Helped, NotHelping, NoLongerHelping, Cleared, SwitchedOff, BudgetUnknown };
VramReliefEvent MemoryReliefTestVramStep(bool reset, uint64_t vramMB, uint64_t budgetMB, bool enabled, MemoryRelief* relief);
void MemoryReliefTestAllowTranslate(bool allow);  // self-test: VramReliefPauseTranslate for the test state machine
bool MemoryReliefTestCommitStep(uint64_t commitRoomMB);  // self-test: one second of RAM/commit relief (the live state); commit relief on?
bool GpuCrashReportTestDred(void* device);                      // self-test: the DRED part of the report
// saves/save_bytes.cpp (SaveBytesBulk; lab: SaveStallTimers): byte arrays inside SaveGameToMemory written in one piece
// start-up only: bulk = SaveGameToMemory's calls and FArrayProperty::SerializeItem's slots, thumb = the screenshot read-back call
bool InstallSaveBytes(bool beforeGameCode, bool bulk, bool thumb);
void SetSaveBytes(int bulk, bool timers, int thumbAsync);  // live, once installed (bulk / thumbAsync: 0 off, 1 on; lab 2 on + verify)
bool SaveCallSiteValid(const GameModule& game, const uint8_t* call);  // an E8 hit inside a .pdata function, 84 C0 within 8 bytes after
// saves/save_thumb.cpp (SaveThumbnailAsync): the save's thumbnail read back without the game / render thread waiting
bool InstallSaveThumbAsync(uint8_t* readBackCall, void* getViewportScreenShot);  // start-up, from InstallSaveBytes
void SetSaveThumbAsync(int mode, bool timers);
bool SaveThumbTakeOver(void* viewport, const void* rect, bool hdr = false);  // game thread, in place of the read-back (hdr: the HDR route's); true = taken over
bool SaveThumbHdrInfo(uint8_t** readCall, void** toFColorSrgb);           // self-test: the HDR route's redirected read call and the quantiser (false: not installed)
void SaveThumbTestHdrConvert(const uint32_t* r10, uint32_t n, uint32_t* out);  // self-test: the HDR route's tables on R10G10B10A2 pixels
void LogSaveThumbStats();
int SaveThumbTestDeliverOrder(const bool* save, int n, int* order);  // self-test only: the helper's delivery order
// saves/save_refid.cpp (SaveRefIdIndex; lab: the SaveGame timer): the old engine save's formID -> refID lookups through a hash index
bool InstallSaveRefIdIndex(bool beforeGameCode);  // start-up only (SaveGame entry, two lookup blocks)
void SetSaveRefIdIndex(int mode, bool timers);    // live: 0 linear (the game's), 1 index, 2 index + verify
void LogSaveRefIdStats();
void SaveRefIdTestSetup(void* append, int mode);                       // self-test only
uint64_t SaveRefIdTestCall(void* saveLoad, uint32_t id, bool inSave);  // self-test only
void SaveRefIdTestNewSave();                                            // self-test only
void LogSaveBytesStats();
void SaveBytesTestSetup(void* arrayOrig, void* getArchive, void* byteSerialize, bool bulk);  // self-test only
void SaveBytesTestCall(void* self, void* slot, void* value, bool inSave);                    // self-test only

// ---- cloth_register.cpp (ClothRegisterLite; lab: ClothRegisterSkip, ClothRegisterVerify) ----
struct ClothRegisterInfoOut {
    uint8_t* recreate; uint8_t* onRegisterCall; uint8_t* setAssetCall;
    void** execRecreateSlot; void** execSuspendedSlot; void** execEnabledSlot;
    void* clothVtable; void** requiresSyncSlot; void** calcBoundsSlot;
    uint8_t* equip; void** equipSlots[4]; int numEquipSlots; uint8_t* equipCall;
    uint32_t proxyOff, registeredOff;
    void* onRegisterHook; void* setAssetHook; void* execRecreateHook; void* execSuspendedHook; void* execEnabledHook;
    void* requiresSyncHook; void* calcBoundsHook; void* equipHook;
    uint8_t* addRulesCall; void* addRulesHook;  // ClothRegisterLite (npcspawn)
    uint8_t* ccUpdateCall; uint8_t* ccEnable; void* ccUpdateHook;  // identical completion (clothfinish)
    void** onUnregisterSlot; void* onUnregisterHook;
};
struct ClothRegisterStats {
    uint64_t registers, skipped, learning, kept, rebuiltAfterSkip, lateBuilds, fallbacks, readsSync, readsSuspended, readsEnabled;
    uint64_t liteBuilds, liteLearning, liteReplaced, liteFallbacks, rulesLeftOut;  // ClothRegisterLite
    uint64_t completed, completeFail, twins, verified, mismatches;              // identical completion / verify (clothfinish)
};
bool FindClothRegisterSkip();                        // locate every site (no patching); false = unavailable (logged)
bool InstallClothRegisterSkip(bool beforeGameCode);  // start-up only (version.dll): the native table is read at class registration;
                                                     // installs the hooks of ClothRegisterLite and of the lab's ClothRegisterSkip
void SetClothRegisterLite(int mode);                 // ClothRegisterLite: 0 off, 3 all lite + completion (lab: 1 all builds of a
                                                     // call but the last lite, learned; 2 only the registration build); live
bool ClothRegisterLiteAvailable();
void** ClothRegisterUnregisterSlot();               // for ClothBuildDefer: OnUnregister's slot while this file's hook holds it
void ClothRegisterLiteTestSetup(void* addRules, void* ccEnable, void* ccUpdate);  // self-test only (after ClothRegisterTestSetup)
void LogClothRegisterStats();
bool ClothRegisterInfo(ClothRegisterInfoOut* out);
ClothRegisterStats GetClothRegisterStats();
void ClothRegisterTestSetup(void* recreate, void* execRecreate, void* execSuspended, void* execEnabled, void* requiresSync,
                            void* calcBounds, void* equip, void* clothVtable, uint32_t proxyOff, uint32_t registeredOff);  // self-test only

// ---- static_bodies.cpp (ParallelStaticBodies) ----
struct StaticBodiesInfoOut { uint8_t* call; uint8_t* create; uint32_t handleOff, staticOff; void* hook; int threads; };
bool FindParallelStaticBodies();                     // locate the call (no patching); false = unavailable (logged)
bool InstallParallelStaticBodies(bool beforeGameCode, int threads, int minBodies);  // start-up only (one call redirected)
void SetParallelStaticBodies(bool enabled, int minBodies);  // switchable live
void LogParallelStaticBodiesStats();
bool ParallelStaticBodiesInfo(StaticBodiesInfoOut* out);
void ParallelStaticBodiesTestJitter(bool on);  // self-test only: helper threads yield at random points
void ParallelStaticBodiesTestHold(int ms);     // self-test only: helper threads sleep this long after each wake
void ParallelStaticBodiesTestSetup(void* fakeCreate, uint32_t handleOff, uint32_t staticOff, int threads, int minBodies);  // self-test only
// The game allocator's per-thread caches on our helper threads (ParallelStaticBodies and PhysicsFlushParallel).
struct HelperCachesInfoOut { uint8_t* setup; uint32_t* slot; int refs; };
bool FindHelperThreadCaches();                       // locate FMallocBinned2's per-thread cache set-up (no patching)
void SetHelperThreadCaches(bool enabled);            // switchable live; takes effect at each helper's next job
void HelperThreadCache();                            // on a helper thread, between jobs: give it / take away its cache
bool HelperThreadCachesInfo(HelperCachesInfoOut* out);
void HelperThreadCachesTestSetup(void* fakeSetup, uint32_t* fakeSlot);  // self-test only
// PipelinedSceneAdd: InitStaticBodies' scene add with registration and query-structure inserts side by side.
bool FindPipelinedSceneAdd();
bool InstallPipelinedSceneAdd(bool beforeGameCode, int minBodies);  // start-up only (two calls redirected)
void SetPipelinedSceneAdd(bool enabled, int minBodies);            // switchable live
void LogPipelinedSceneAddStats();
void* PipelinedSceneAddTestSetup(void* add, void* toSolver, const volatile uint8_t* flag, uint32_t solverOff, uint32_t lockOff,
                                 uint32_t collOff, int minBodies);  // self-test only: returns the stub for the inner call
void* PipelinedSceneAddTestHook();                                   // self-test only: the hook of the outer call
bool PipelinedSceneAddInfo(uint8_t** sites, uint32_t* offsets);  // sites: call A, AddActorsToScene, call B, AddActorToSolver, flag
// PipelinedBodyCreation: one component's body creation, registration and inserts side by side (ParallelStaticBodies's and
// 28b's hooks; no patch of its own).
bool FindPipelinedBodyCreation();                  // ExecuteWrite, InitBodies' lambda, its body flag (no patching)
void SetPipelinedBodyCreation(bool enabled);       // switchable live (no-op unless found and ParallelStaticBodies installed)
void LogPipelinedBodyCreationStats();
bool PipelinedBodyCreationInfo(uint8_t** sites, uint32_t* offsets);  // sites: ExecuteWrite, lambda; offsets: flag off, bit
void PipelinedBodyCreationTestSetup(void* lambda, void* execWrite, uint32_t postOff, uint8_t postBit);  // self-test only
// GpuSceneParallelUpload: r.GPUScene.ParallelUpdate set to 1 (gpuscene_upload.cpp).
bool FindGpuSceneParallelUpload();                 // the console variable's registration and value pointer (no patching)
void SetGpuSceneParallelUpload(bool enabled);      // once per second from the monitor; switchable live
void GpuSceneParallelUploadTestSetup(int32_t* volatile* slot);  // self-test only: a fake value pointer slot
int32_t* volatile* GpuSceneParallelUploadSlotForTest();         // self-test only
// DirtyTreeQuickInsert: greedy sibling choice for dirty-element-tree inserts ("equivalent" class).
bool InstallDirtyTreeQuickInsert(bool beforeGameCode);  // start-up only (three calls redirected)
void SetDirtyTreeQuickInsert(bool enabled);             // switchable live
void LogDirtyTreeQuickInsertStats();
int32_t QuickFindBestSibling(uint8_t* tree, const double* bounds, bool* addToLeaf, int32_t leafCapacity);  // self-test
// ... and what BulkIndexInsert uses of DirtyTreeQuickInsert's three call sites (static_bodies.cpp)
using DirtyInsertHookFn = int32_t* (*)(uint8_t* tree, int32_t* out, void* payload, const double* bounds);
bool DirtyTreeSitesInstalled();
void SetDirtyInsertHook(DirtyInsertHookFn hook);  // null: DirtyTreeQuickInsert's path
int32_t* DirtyInsertDefault(uint8_t* tree, int32_t* out, void* payload, const double* bounds);  // DirtyTreeQuickInsert's path for one insert
int32_t* InsertLeafForcedSibling(uint8_t* tree, int32_t* out, void* payload, const double* bounds, int32_t sibling);
void DirtyTreeTestSetup(void* insertLeaf, void* fbs, const int32_t* leafCap);                // self-test only
int32_t* DirtyTreeTestInsert(uint8_t* tree, int32_t* out, void* payload, const double* bounds);  // self-test only
int32_t DirtyTreeTestFbs(uint8_t* tree, const double* bounds, bool* addToLeaf);                  // self-test only

// ---- bulk_insert.cpp (BulkIndexInsert): batched dirty-tree inserts, FlushInternal beside FlushAsync ----
bool InstallBulkIndexInsert(bool beforeGameCode);  // start-up only (DirtyTreeQuickInsert's sites + entry detours)
void SetBulkIndexInsert(bool bulk, int minOps, bool internalParallel);  // switchable live
void LogBulkIndexInsertStats();
void BulkFlushAsyncBegin(uint8_t* evolution);  // PhysicsFlushParallel's FlushAsync hook, at its start (physics step's thread)
void BulkFlushAsyncEnd();                      // ... at its end
void BulkBatchBegin(int ops);                  // a pass on this thread files `ops` changes (batched if on and enough)
void BulkBatchEnd();                           // ... done: the batched trees get their final shape
struct BulkTestStats { uint64_t passes, inserts, rebuilt, broken, fallback; };
BulkTestStats BulkIndexInsertTestStats();       // self-test
void BulkIndexInsertTestSetup(bool bulk, int minOps);  // self-test: DirtyTreeQuickInsert's sites are models (DirtyTreeTestSetup)
void BulkParallelTestSetup(void* fakeFlushInternal, uint32_t numOffset, const int32_t* cvar, bool on);  // self-test
void BulkTestFlushInternal(uint8_t* evolution);        // self-test: FlushInternal's detour
void SetPhysicsQueueShrinkOnce(bool on);               // PhysicsQueueShrinkOnce, live (no-op unless installed with BulkIndexInsert)
struct ShrinkTestInfo { bool installed; uint8_t* site; void* hook; void* real; };
ShrinkTestInfo BulkShrinkTestInfo();                    // self-test: PhysicsQueueShrinkOnce's call site and functions
void BulkShrinkTestSetup(void* fakeExtOriginal, void* fakeShrink, bool on);  // self-test: fakes for the catch-up and shrink
void BulkTestCatchUp(uint8_t* evolution, void** structure, uint8_t* queue);  // self-test: the catch-up's detour
// ---- destroy_batch.cpp (PhysicsDestroyBatch): the step's destruction of removed bodies, same physics state ----
bool InstallPhysicsDestroyBatch(bool beforeGameCode, int minProcessors);  // start-up only; the helper from minProcessors on
void SetPhysicsDestroyBatch(int mode, int minPending);                    // live once installed (0/4 off, 1, 2, 3)
void LogPhysicsDestroyBatchStats();
bool PhysicsDestroyBatchTestSites(const GameModule& game);  // self-test
const char* PhysicsDestroyBatchTestRun(int mode, int n, int gcNum, int cuNum);  // self-test: the wrapper with fakes
const char* PhysicsDestroyBatchTestConcurrent(int callers, int rounds, int n);    // self-test: concurrent solvers
const char* PhysicsDestroyBatchTestPaths();  // self-test: inline paths, no helper, sleeping helper, stall guard
// ---- npc_bodies.cpp (NpcBodiesOneLock): a character's physics bodies and constraints under one hold of the scene lock ----
bool InstallNpcBodiesOneLock(bool beforeGameCode);  // start-up only (one call redirected)
void SetNpcBodiesOneLock(int mode);                 // live once installed: 0 off, 1 on
void LogNpcBodiesOneLockStats();
void NpcBodiesTestSetup(void* fakeIpa, uint32_t solverOff, uint32_t lockOff);  // self-test
void* NpcBodiesTestHook();
bool NpcBodiesTestFind(void** site, void** fn, uint32_t* solverOff, uint32_t* lockOff);
// ---- rig_setup.cpp (RigVmHashMemo, RigLogicMapCache): identical shortcuts in an NPC's Control Rig set-up ----
bool InstallRigSetup(bool beforeGameCode);  // start-up only
void SetRigSetup(int vmHashMemo, int mapCache);  // live once installed: 0 off, 1 on, 2 verify
void LogRigSetupStats();
uint8_t* RigSetupTestSites(int which);  // self-test: 0 = ComputeVMHash
struct RigSetupTestFns {
    void *vmOrig, *staticClass, *memoryHash, *fnameCtor, *nameHash, *resizeTo, *grow32, *grow16, *empty16, *free;
    void* mapOrig[4];  // joints, controls, morph targets, mask multipliers
};
void RigSetupTestSetup(const RigSetupTestFns& f);  // also clears both caches
void* RigSetupTestNameHook();                      // the FName capture hook the fake map functions call
uint32_t RigSetupTestVm(const void* vm);
void RigSetupTestMap(int kind, void* data, void* hierarchy);
void RigSetupTestCounts(uint64_t* out);  // 23 counters, see rig_setup.cpp
int RigSetupTestReaderClasses(int state);  // reader classes in a state (1 usable, 0 unchecked, -1 refused)
bool RigSetupTestUnwrapShape(const uint8_t* fn);  // the IDNAReader::Unwrap code-shape check
// ---- rt_scene_keep.cpp (RtSceneBufferKeep): the ray tracing scene's TLAS buffer is not re-created smaller ----
bool FindRtSceneKeep();
bool InstallRtSceneKeep(bool beforeGameCode);  // start-up only
void SetRtSceneKeep(int mode);                 // live once installed: 1 keep, 0/2 the game's way
void LogRtSceneKeepStats();
bool RtSceneLogCreate(int site, uint32_t size, uint32_t was, uint64_t us);  // whether a buffer creation gets a log line (0 = TLAS)
int RtSceneKeepTestDecide(uint64_t need, uint64_t size);  // self-test only: 1 keep, 0 re-create, -1 not installed
bool RtSceneKeepTestPatched();                    // self-test only
// ---- physics_push.cpp (PhysicsPushParallel, PhysicsPushParallel): batch creation and parallel shape set-up of new statics ----
bool FindPhysicsPush();                                                 // locate the sites (no patching)
bool InstallPhysicsPush(bool beforeGameCode, int threads, int minParallel);  // start-up only (five calls redirected)
void SetPhysicsPushParallel(int mode, int minParallel);                 // switchable live: 0 off, 1 batch + parallel, 2 batch
void LogPhysicsPushStats();
struct PhysicsPushInfoOut {
    uint8_t *pushCall, *createCall, *viewsCall, *snfdCall, *uwsCall;
    void *forEach, *create, *views, *snfd, *spt, *grow, *free, *uws;
    uint32_t offsets[8];  // buckets, numBuckets, frame, ignore, type, particles, rewind, evolution
};
bool PhysicsPushInfo(PhysicsPushInfoOut* out);
struct PhysicsPushTestFns { void *forEach, *create, *views, *snfd, *spt, *grow, *free, *uws, *jobUws; };
struct PhysicsPushTestHooks { void *forEach, *create, *views, *snfd, *uws; };
PhysicsPushTestHooks PhysicsPushTestSetup(const PhysicsPushTestFns& fns, int threads, int minParallel);  // self-test only
void PhysicsPushTestHold(int ms);  // self-test only: helper threads sleep this long after each wake
void PhysicsPushTestMode(int mode, bool jitter
);      // self-test only
uint64_t PhysicsPushTestAnomalies();                                    // self-test only

}  // namespace opf
