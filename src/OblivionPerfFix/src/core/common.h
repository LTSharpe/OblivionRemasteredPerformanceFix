// Shared declarations: the version string, the ini settings (Config, with each fix's default), the pattern
// search and memory-write helpers (mem.cpp), and the entry points each fix file exports to main.cpp.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define OPF_VERSION "1.6.0"
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
void LogGoAsync(uint8_t* exeBase);  // end of Init: a background thread writes the lines from here on; exeBase: guard the exe's TerminateProcess import
void LogFlush();                    // everything queued so far is in the file when this returns (crash dumps, self-test)
void LogCrashMode();                // a crash: flush, and every later line is written by its caller at once
void LogBeforeExit();               // the game ends now (TerminateProcess): the same as LogCrashMode
void LogProcessExit(bool otherThreadsEnded);  // DLL_PROCESS_DETACH; true (ExitProcess): no locks, writes the rest of an ended write too
void LogTestHoldWriter(bool hold);  // self-test: the writer thread takes nothing while held
void LogTestSetCapacity(size_t bytes);  // self-test: a smaller queue (0 = the default)
bool LogTestAsync();                // self-test: lines go through the writer thread
bool LogTestEndedMidWrite(size_t written);  // self-test: the queue becomes a write that ExitProcess cut after `written` bytes

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
    int pairReleaseOnce = 1;           // PairReleaseOnce (game bug fix, on; findings crash4590f5c): guards/pair_race.cpp, a pairing
                                       // session's buffer ids released once (the destructor's), not twice; start-up only
    bool bowSneakAttackFix = true;     // BowSneakAttackFix (game bug fix, on; user 2026-10-31): one hit per player arrow, the old engine's with the Unreal side's sneak
                                       // multiplier (game bug: bow sneak attacks at 1x or hit twice)
    bool stuckEffectLookFix = true;   // StuckEffectLookFix (game bug fix #48; default off until it has been tested in play, then on; stuckfx2
                                       // 2026-11-20): guards/stuck_effect_look.cpp, a status effect's overlay look left on weapons,
                                       // the quiver or a dismounted rider after the effect ended is taken off (or the look under it
                                       // that is still showing put back)
    bool worldSoundPositionFix = true;  // WorldSoundPositionFix (game bug fix GitHub #52; default off until it has been tested in play;
                                       // gh52build 2026-11-20): guards/world_sound_position.cpp, 15 sound containers the game's data
                                       // leaves without 3D panning (NPC torch lighting also without attenuation) get it at bank load
    bool arrowHitSoundFix = true;     // ArrowHitSoundFix (game bug fix GitHub #52; default off until tested in
                                       // play): guards/arrow_hit_sound.cpp, an NPC's arrow hit is heard once, at
                                       // the hit (posted on the arrow, not on the shooter's weapon); player shots stock
    bool saveThumbLeftoverFix = true;  // SaveThumbLeftoverFix (game bug fix, on; user 2026-11-09, shot130q): saves/save_thumb_leftover.cpp,
                                       // a save's thumbnail binding on the menus' screenshot service removed once its callback ran
    bool autosaveFollowsMenu = false;  // AutosaveFollowsMenu (game bug fix, OFF by default, user 2026-10-31: it removes the level-up
                                       // and quest autosaves while all four menu options are off): no level-up or scripted
                                       // autosave while every autosave option in the menu is off (game bug)
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
    bool monitorNativeFromWindows = true;   // MonitorNativeFromWindows (game bug, on by default): a monitor's native
                                            // resolution from Windows' preferred mode, not the EDID's first timing (start-up only)
    bool fullscreenAllResolutions = false;  // FullscreenAllResolutions (OFF by default, user 2026-10-31: adds modes of other shapes and
                                            // above native; not needed for the reported bug): every mode of the monitor
                                            // in the fullscreen resolution list (start-up only)
    bool separableSkinAbove4K = true;   // SeparableSkinAbove4K (game bug fix, on; user 2026-10-31): r.SSS.Burley.Quality 0 while the
                                         // render is wider than 3840 (the magenta skin specks on ultrawides at DLAA); live
    bool screenshotFilterFast = true;  // MenuScreenshotFilter: the menu's same-size ImageResize of its screenshot, same bytes in SSE2
    int screenshotReadbackConvert = 1; // ScreenshotReadbackConvert: the screenshot readback's R10G10B10A2 -> FColor conversion in SSE2 (identical
                                       // bytes; start-up install, then live): 0 off, 1 on, 2 on + verify against the game's loop
    int screenshotParallel = 1;        // ScreenshotParallel: the menu screenshot's big copies (and MenuScreenshotFilter and ScreenshotReadbackConvert's loops) on helper threads,
                                       // ImageResize's zero fill skipped (same bytes; start-up install, then live): 0 off, 1 on
    bool restoreFsrAfterAltTab = true; // RestoreFsrAfterAltTab: FSR (and frame generation) back on when a menu up at focus gain closes
    bool lumenKeepArrayCapacity = true; // LumenKeepArrayCapacity: Lumen scene arrays keep their capacity instead of shrink/grow copies
    int lumenGroupsPrior = 2600000;     // LumenGroupsPrior (entries of 80 B; 0 = off): PrimitiveGroups grows straight to it from 1/32 of it (live)
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
    int bodyRemovalSpread = 1;         // BodyRemovalSpread: physics/body_spread.cpp, a streamed-out level's instance bodies
                                       // terminated over <= MaxFrames frames at the post-actor tick (equivalent, user
                                       // 2026-11-07); start-up install when != 0 (needs NpcCreationPacing), then live 0/1
    int bodyRemovalSpreadPerFrame = 500;   // not in the shipped ini: at least this many bodies a frame
    int bodyRemovalSpreadMaxFrames = 30;   // not in the shipped ini: no body waits longer (public 2..120)
    int bodyRemovalSpreadMin = 1;          // not in the shipped ini: components with fewer bodies stay stock
    int bodyRemovalSpreadNearM = 50;       // not in the shipped ini: components nearer the player (m, bounds box) stay stock
    int instanceBufferFloor = 1;       // InstanceBufferFloor: render/buffer_floor.cpp, the GPU scene instance buffer made 192 MiB at the
                                       // session's first need of 2^21 slots (16 GB-class cards: budget >= 14000 MiB, 1 GiB free;
                                       // decided once a session; needs GpuSceneKeepCapacity); start-up only
    int streamRegisterGranularity = 1; // StreamRegisterGranularity: streaming/stream_granularity.cpp, 1..9 sets
                                       // s.LevelStreamingComponentsRegistrationGranularity (game 10) outside loading
                                       // screens (equivalent, user 2026-11-07); AddToWorld hook at start-up (key on), then live
    int streamInBudget = 1;            // StreamInBudget (gatefix 2026-11-12): streaming/stream_granularity.cpp, outside loading
                                       // screens s.AdaptiveAddToWorld.AddToWorldTimeSliceMax / s.AsyncLoadingTimeLimit (game 4 / 5 ms)
                                       // set to 1 / 3 (key 1) or 2 / 3 (key 2); shares StreamRegisterGranularity's AddToWorld
                                       // hook (installed at start-up when either key is on); live 0..2
    int physicsSubstepCap = 1;         // PhysicsSubstepCap (physkey 2026-11-20): physics/physics_substep_cap.cpp, 1 = the physics
                                       // settings' MaxSubstepDeltaTime 0.02 -> 0.03 and MaxSubsteps 6 -> 4 (as the two Engine.ini lines;
                                       // not bit for bit); set on the game thread at AddToWorld (shares the StreamRegisterGranularity hook); live
    int navQueryPriority = 1;          // NavQueryPriority (navprio 2026-11-18): npcs/nav_query_priority.cpp, 1 = Epic's
                                       // TaskGraph.TaskPriorities.NavTriggerAsyncQueries set to "nnn" (game "bnn"); set on the game thread at
                                       // AddToWorld (shares the StreamRegisterGranularity hook: installed when any of the three keys is on); live
    int lumenCardsNarrow = 1;          // LumenCardsNarrow: render/lumen_cards_narrow.cpp, the Lumen mapping upload sends only the
                                       // instances whose mesh cards changed (identical); start-up install when != 0, then live 0/1
                                       // (lab: 2 = with proof, needs 2 at start-up)
    int lumenScanCull = 1;             // LumenScanCull: render/lumen_scan_cull.cpp, Lumen's CPU scan of its primitive groups reads only the
                                       // blocks of 64 groups that can emit a request (identical; Epic's UE 5.6.0 idea); on since 1.4.0;
                                       // start-up install when != 0, then live 0 / 1 / 2 (2 = proof)
    int waterInfoSkip = 1;             // WaterInfoSkip: render/water_info_skip.cpp, a water zone's water-info redraw skipped when its
                                       // inputs equal its last redraw's (equivalent); start-up install when != 0 (with the water-info
                                       // render's game-thread detour), then live 0/1 (lab: 2 proof)
    int waterInfoSpread = 3;           // WaterInfoSpread: render/water_spread.cpp, water-info updates spread (bit 1) and merged (bit 2)
                                       // within max(4 frames, 250 ms) (equivalent); start-up install when != 0, then live 0-3
                                       // (lab: bit 4 logs each request)
    int waterInfoSpreadNearM = -1;     // WaterInfoSpreadNearM: zones this close (m) are never deferred (0 = the player's own, -1 none)
    int waterInfoSpreadPerFrame = 1;   // WaterInfoSpreadPerFrame / MaxFrames / MergeMs (not in the shipped ini): its limits
    int waterInfoSpreadMaxFrames = 4;
    int waterInfoSpreadMergeMs = 250;
    int waterInfoAfterCrossing = 500;    // WaterInfoAfterCrossing: render/water_spread.cpp, a water zone's water-info request held while
                                       // levels are added / removed, at most this many ms from its first ask (0 = off; 500 suggested;
                                       // equivalent); installs WaterInfoSpread's call site when on, then live
    int waterInfoAfterCrossingQuietMs = 150;  // ...QuietMs (not in the shipped ini): a stream step within this time = still crossing
    int repathParallel = 1;            // RepathParallel: npcs/repath_parallel.cpp, a TickActor's queued path re-searches' A* on
                                       // helper threads, served only on identical inputs (identical); start-up install when != 0,
                                       // then live 0/1 (lab: 2 proof); off while ParallelHelpersAllowed is false
    int repathParallelMin = 2;         // not in the shipped ini: queued re-searches needed for a batch (live)
    int repathParallelThreads = 4;     // not in the shipped ini: helper threads, 0..8 (start-up)
    int repathEarly = 0;               // RepathEarly: npcs/repath_parallel.cpp, a navigation change's queued repaths start their A* on
                                       // RepathParallel's helpers at the change (navmesh writers fenced), served at TickActor only on
                                       // identical inputs; start-up install when != 0 (needs RepathParallel), then live 0/1
    int capSleepSkip = 0;              // CapSleepSkip: npcs/npc_pacing.cpp, the frame-rate cap's wait before a frame known to carry a
                                       // GC pass (1) or a cell removal step (2) is shortened by up to CapSleepSkipMaxMs (NOT exact cap
                                       // for that frame); start-up install when != 0 (needs NpcCreationPacing, GcPassAfterBusy), live
    int capSleepSkipMaxMs = 8;         // not in the shipped ini: the most the wait is shortened (ms, live)
    int capSleepSkipAddBack = 1;       // lab only (the public build always gives back): 1 the cut goes back into FApp::DeltaTime, 0 the
                                       // cut frame keeps its real, shorter step (the game clock stays with the real clock); live
    int repathDefer = 1;               // RepathDefer: npcs/repath_parallel.cpp, a TickActor's queued repaths start on RepathParallel's
                                       // helpers; what is not done after the grace is taken out of the queue and given back when done
                                       // (equivalent, not identical: start / end within the tolerance); start-up install, then live 0/1
    int repathDeferGraceUs = 500;      // not in the shipped ini: the game thread's wait for the helpers first (us, live)
    int repathDeferMaxFrames = 6;      // not in the shipped ini: frames a request may stay out; then the game thread waits (live)
    int repathDeferTolerance = 50;     // not in the shipped ini: start / end point distance (uu) a late result is served across (live)
    int farPawnMoveHalfRate = 1;       // FarPawnMoveHalfRate: npcs/far_pawn_move.cpp, a calm pawn >= 100 m away moves at FarPawnMoveHz
                                       // with the summed step (NOT identical: small steps far away): 1 at walking pace, 2 any speed,
                                       // 0 off; start-up install when this or FarHorseMove != 0 (needs NpcCreationPacing), then live
    int farPawnMoveHz = 30;            // FarPawnMoveHz: its movement rate, 1..59 (60 = every frame) (live)
    int farHorseMove = 2;              // FarHorseMove: 0 off, 1 far riderless calm horses, 2 also far horses with a calm NPC rider (live)
    int farPawnMoveMinDistance = 10000;  // not in the shipped ini: FarPawnMove's distance in UE units (>= 1000; start-up)
    int farPawnMoveMaxStepMs = 400;      // not in the shipped ini: the longest carried step, 20..1000 ms (start-up)
    int createPreload = 1;             // CreatePreload: npcs/create_preload.cpp, a far create whose form asset or actor class is not
                                       // loaded waits (pacing's deferral, bounded) while it loads asynchronously (equivalent);
                                       // start-up install when != 0 (needs NpcCreationPacing), then live 0/1
    int createPreloadNear = 12;        // CreatePreloadNear: with CreatePreload=1, a near create may also wait, at most this many
                                       // frames (0..30), while its spot is > 75 deg off the camera's forward (frame generation's
                                       // camera: no wait without frame generation); 0 = off; live
    int otFrameTrim = 3;               // OtFrameTrim: npcs/ot_frame_trim.cpp, bit 1 the OT frame-start path-builder sleep wakes on
                                       // the path thread's completion (equivalent), bit 2 the node profiler timer off (identical by
                                       // audit); start-up install when != 0, then live 0..3
    int looseMissCache = 1;            // LooseMissCache: npcs/loose_miss_cache.cpp, a missing loose voice file's failed check is
                                       // answered from memory (identical); start-up install when != 0, then live 0/1
    int flowOverlapsOnce = 1;          // FlowOverlapsOnce (otflows 2026-10-26, public tree flowport 2026-11-16, S-007):
                                       // streaming/flow_overlaps_once.cpp, a pre-placed actor's overlap updates inside the
                                       // level-flow adoption run once per component at its end; start-up install when != 0
                                       // (lab: always), then live 0/1
    int powerAttackAnimKeep = 1;       // PowerAttackAnimKeep (humananim 2026-11-15): npcs/power_attack_anim_keep.cpp, a power-attack
                                       // animation file whose last holder lets go stays in the old engine's animation map at
                                       // count 0 instead of being destroyed; start-up install when != 0, then live 0/1
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
    int perceptionLosPrefetch = 1;       // PerceptionLosPrefetch: npcs/perception_los_prefetch.cpp, a perception round's line-of-sight
                                         // checks precomputed on helper threads and served in the game's order (equivalent): 1 on,
                                         // 2 verify, 0 off (default off until reviewed); installed only if not 0 at start-up
                                         // (version.dll route), then live; off while ParallelHelpersAllowed is false
    int perceptionLosPrefetchMin = 4;    // not in the shipped ini: precomputed checks needed for a batch (live)
    int perceptionLosPrefetchThreads = 4;  // not in the shipped ini: helper threads, 0..8 (start-up)
    bool perceptionLosPrefetchOverlap = true;   // not in the shipped ini: the loop starts before the batch is done, a round-end
                                                // join waits for the rest (live; 2026-11-14 A-B-B-A x2: -24 % per round in
                                                // the battle, -10 % in the crowd, 0 differences in 1.1 M verified checks)
    bool perceptionLosIgnoreCache = false;      // lab builds only (no measured gain, 2026-11-16): the NPC's two ignore-list gathers once per batched round
                                                // (the helper's NPC calls get a copy; needs the round-end join; live)
    bool perceptionLosCrossRound = false;       // lab builds only (our review of PR #55: answers served across other game work of the
                                                // frame are not covered by the equivalence argument), experimental: the first round of a
                                                // frame also precomputes the NPCs expected later in that frame (live; needs
                                                // NpcCreationPacing's frame count)
    int rigImportTemplate = 1;           // RigImportTemplate: npcs/rig_import_template.cpp, the first bone import of a new rig is a copy
                                         // of a stored identical import (equivalent), RemoveElement guard; 1 on, 0 off (lab: 2 verify);
                                         // installed only if not 0 at start-up (version.dll route), then live
    int chaosMaxPayloadSize = 10000000;  // ChaosMaxPayloadSize: physics/chaos_payload.cpp, p.MaxPayloadSize raised (cm) at start-up so
                                         // the ~730 bodies over 1 km stay in the Chaos trees (equivalent); only over the game's 100000;
                                         // an Engine.ini value wins; 0 = the game's value; both routes; start-up only
    int osPageCache = 1;                 // OsPageCache: blocks the allocator releases to Windows are kept (exact size, bounded) and
                                         // handed out again for its own cache misses (identical; start-up install, then live)
    int osPageCacheAsyncRelease = 1;     // OsPageCacheAsyncRelease: the blocks OsPageCache hands back to Windows are released by one
                                         // background thread instead of the game's (identical; live, needs OsPageCache installed)
    int osPageCacheMB = 64;              // not in the shipped ini: OsPageCache's cap (MB, 4-1024)
    int osPageCacheTrimSeconds = 5;      // not in the shipped ini: OsPageCache releases blocks unused this long (s)
    int animTeardownParallel = 1;        // AnimTeardownParallel: dying NPC animation data's interpolators
                                         // destroyed on helper threads, joined before the actor's clean-up goes on (start-up
                                         // install when not 0, then live): 0 off, 1 on
    int cellBufferPacedEvict = 1;        // CellBufferPacedEvict: spare exterior cell buffer slots
                                         // (start-up install when not 0) and the overflow torn down one cell per OT frame
                                         // after a crossing (live): 0 in the crossing frame (as the game), 1 paced
    int cellBufferEvictDelayMs = 1500;   // CellBufferEvictDelayMs (otunload; public 1.3.0): the paced drain starts this many ms after
                                         // a crossing (live, 0..3000; 0 = right after it); needs CellBufferPacedEvict
    int cellEvictSplit = 2;              // CellEvictSplit (flightrest / evfix; public 1.5.0, 0 until the release flips it): a paced
                                         // unload's actors lose their 3D N per OT frame (0-8; 2 when on) before the cell's
                                         // teardown (live; needs CellBufferPacedEvict)
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
    int indirectArgBufferPool = 1;     // IndirectArgBufferPool (poolargs 2026-11-14; off until played): d3d12.AllowPoolAllocateIndirectArgBuffers
                                       // 1 (Epic's UE 5.5 default): indirect-arg buffers from the D3D12 buffer pool, not committed one by
                                       // one; placement only; start-up only, version.dll route only
    int parallelFileReads = 8;         // ParallelFileReads (vtfix 2026-11-14; off until played): up to N reads of a game container
                                       // file in flight instead of the engine's one at a time (streaming/parallel_file_reads.cpp);
                                       // 0 / 1 = the engine's reader; 2..16; live once installed at start-up; identical data
    int vtStreamingUploads = 0;        // VtStreamingUploads (off by default): r.VT.MaxUploadsPerFrame.Streaming set to
                                       // this (streamed virtual texture pages uploaded per frame; the game's 4; 0 = the game's
                                       // value; live)
    bool hitchLedger = false;          // Diagnostic HitchLedger: slow game-thread frames' stage times to
                                       // OblivionPerfFix-ledger.csv (installed only if on at start-up, then switched live)
    int hitchLedgerMs = 25;            // ... a frame counts as slow from this period (ms, the cap's sleep included)
    bool doorDiag = false;             // diagnostic DoorDiag (not in the shipped ini): door/loading-screen timeline,
                                       // stream-all timing, render-target pool misses/flushes (installed only if on at start-up)
    bool doorDiagEarlyStream = false;  // ... experiment: stream-all once when the loading screen idles (live)
    bool fadeVtFlushSkip = false;      // FadeVtFlushSkip (off by default): the r.VT.Flush the game runs 1.5 s into a
                                       // transition's fade-in skipped (installed only if on at start-up, then live)
    bool streamBehindLoading = true;   // StreamBehindLoadingScreen: a new area's textures stream while its loading screen is still
                                       // up, so the fade-in is not held for them (installed only if on at start-up, then live)
    int unfreezeBehindLoading = 0;     // prototype UnfreezeBehindLoadingScreen (trade-off, off by default): after an
                                       // exit, ms the world runs behind the loading screen before the fade-in, as after an
                                       // interior entry (0 = off; live; needs StreamBehindLoadingScreen)
    int unfreezeBehindLoadingFastTravel = 0;  // the same after a fast travel, in ms (0 = UnfreezeBehindLoadingScreen's
                                              // value; on only while UnfreezeBehindLoadingScreen is on; live)
    int gpuMenuShot = 1;               // GpuMenuShot (gpushot; public 1.3.0): menus/gpu_menu_shot.cpp, the menu backdrop's screenshot made
                                       // on the GPU; start-up install when != 0, then live 0/1; 3 = compare the next 4 with the
                                       // game's own picture (slow; lab: 2 = compute only + compare)
    int mapPopupSpread = 5;            // MapPopupSpread (mapspread; public 1.3.0): menus/map_spread.cpp, the map's icon lists cleared
                                       // unseen are not built (1) and an equal rebuild keeps the icons (4); start-up install when
                                       // != 0 (needs NpcCreationPacing), then live 0, 1, 4, 5
    int drawBehindLoading = 3;         // DrawBehindLoadingScreen (loadreveal; public 1.3.0): the still-frozen world is drawn for
                                       // this many frames behind the opaque loading screen from StreamBehindLoadingScreen's
                                       // start (no unfreeze, no gate hold; 0 off, 2..10; live; needs StreamBehindLoadingScreen,
                                       // PrepareCollisionBehindLoadingScreen's fade hook and NpcCreationPacing's frame hook)
    bool hdrMenuRows = false;          // HdrMenuRows (off by default): an HDR section on the PC Graphics page (HDR,
                                       // peak, paper white, black floor, mid-tone; start-up only; needs NpcCreationPacing)
    bool hudAspectRow = false;          // HudAspectRow (off by default): a "HUD Aspect Ratio" row (4:3 / 16:9 /
                                        // 21:9 / 32:9 / Wide) in the Graphics page's General section (start-up only)
    bool dlssRayReconstruction = false;  // DlssRayReconstruction (off by default, trade-off): a "DLSS - Ray
                                         // Reconstruction" row in the Graphics page's Advanced section (start-up only)
    bool hdrGamma22 = false;           // HdrGamma22 (TRADE-OFF, off by default: a look change, user 2026-10-31; HDR only): the HDR picture's darks as an SDR
                                       // monitor shows them (gamma 2.2), the curve refitted in memory (live; needs NpcCreationPacing)
    bool hdrMenuBackground = false;    // HdrMenuBackground (TRADE-OFF, off by default): the player menu's background
                                       // in HDR10 shown at the world's nits, not its PQ values as colour (start-up install, live)
    bool hdrSaveThumbnail = false;     // HdrSaveThumbnail (TRADE-OFF, off by default): a save's thumbnail made in
                                       // HDR10 converted the same way (start-up install, live)
    int hdrGamma22White = 95;         // not in the shipped ini: HdrGamma22's reference white = this / 10 x the Mid-Tone (live)
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
    bool pacingOwnerWalk = true;       // PacingOwnerWalk (mkusm, first version of public PR #25): a non-actor in a delegate's owner
                                       // chain (equipment, hair) does not end the walk to its actor, so a far unseen character's
                                       // equipment / hair steps are paced like its other steps (live)
    bool pacingHairActor = true;      // PacingHairActor (npcunits 2026-11-12): a hair step is paced by its character (found through
                                       // the hair mesh component it sets), not counted as near because its owner is the hair asset
    bool pacingFlowProgress = true;   // PacingFlowProgress (drainfix 2026-11-12): a waiting level flow's 8-frame bound also counts
                                       // creates run with older OT frames owed and a head create waiting for its loads (live)
    int fadeInUnseen = 1;              // FadeInUnseen (stampfix 2026-11-13; Epic's idea, UE 5.5.0 8dd2cb4237): a far character in its
                                       // spawn fade-in whose meshes the renderer has not drawn for 200 ms no longer counts as
                                       // rendered, so it stops animating then instead of 1 s after its first frames' stamps
                                       // (start-up install, then live; needs NpcCreationPacing)
    int fadeInUnseenWindowMs = 200;    // not in the shipped ini (lab): FadeInUnseenWindowMs (50-900)
    int pacingBaseGate = 25;            // PacingBaseGate (basegate 2026-11-12): a far, unseen NPC's appearance / equipment / hair
                                       // step runs after the actor tick only if the frame stays within a target (0 = off, 1 = 25 ms,
                                       // 18-50 = that target in ms; live)
    int pacingGateBoundFrames = 45;    // PacingGateBoundFrames (gate2 2026-11-13): a unit PacingBaseGate holds runs by this age
                                       // (frames since it was queued) at the latest (10-180; live)
    int pacingGateFadeHoldMs = 0;      // lab PacingGateFadeHoldMs: the fade hold (0 = off; lab builds only; live)
    int pacingGateShownHoldMs = 1000;     // PacingGateShownHoldMs (hold2; public 1.5.0, 0 until the release flips it; 1000 when on):
                                       // visual-only hold of a held far character's spawn fade-in (0 = off, 1-2000 ms bound;
                                       // hook installed at start-up when > 0, not on the OBSE64 route; then live)
    int pacingGateShownHoldEnd = 10;   // PacingGateShownHoldEnd: tenths of the game's fade value (5 at the start, 0 at its end,
                                       // 4 per second) at which a held character's units are due now (soft from +0.5); the
                                       // visual hold ends at +0.5 at the latest (5-25; live)
    int pacingGateCalmMs = 17;         // PacingGateCalmMs: a held unit may also run on a frame whose own time stays below this
                                       // (0 = off, 10-30; live)
    int pacingGateNearM = 100;         // PacingGateNearM: PacingBaseGate never holds an actor nearer than this (m, 0-500; live)
    bool createPreloadEarly = true;   // CreatePreloadEarly (drainfix 2026-11-12): a far create held back by the pacing budget
                                       // requests its loads at once (live; needs CreatePreload)
    int impactPreload = 1;             // ImpactPreload (impactload 2026-11-12; 2 = log first uses only): the hit-effect classes of the game's impact table
                                       // (every surface) and its blood effect class are loaded in the background before the
                                       // first fight and kept (start-up install, then live; needs NpcCreationPacing)
    int createAhead = 1;               // CreateAhead (aheadport 2026-11-16; public, 0 until a release flips it): the SpawnActor
                                       // half of a character create is done seconds earlier in frames with frame-cap slack and
                                       // taken by the game's own create (start-up install, then live; needs NpcCreationPacing,
                                       // CreatePreload; RowPreload for the references of cells ahead)
    int createAheadSlackMs = 4;        // CreateAheadSlackMs: a build only follows a frame that slept at least this long in the
                                       // frame cap (1-14; live)
    int createAheadPool = 24;           // CreateAheadPool (aheadwide 2026-11-18): 0 = at most 12 actors held, spares while < 8;
                                       // 12-32 = at most N held, spares while < N - 4, a spare of a base form idle 20 s makes
                                       // room for a base form in use (live)
    int createAheadLevelled = 1;       // CreateAheadLevelled (rollahead 2026-11-18, S-005): 1 = the game's levelled spawns already
                                       // made (run-time references, ids FF......) in the cells RowPreload walks ahead are built
                                       // ahead as references, as placed characters are (live)
    int cellObjectPreload = 1;         // CellObjectPreload (preload 2026-11-18, S-008; public, 0 until a release flips it): the
                                       // form asset and actor class of every object in a cell RowPreload loaded ahead are
                                       // requested in the background (start-up install, then live; needs CreatePreload, RowPreload)
    int rowPreload = 1;                // RowPreload (rowpreload 2026-11-12): the next row of exterior cells the game asks for is loaded
                                       // ahead, one per calm old-engine frame, and parked in the cell buffer (the 2006 game's
                                       // preload; start-up install, then live; needs CellBufferPacedEvict and NpcCreationPacing)
    int streamOutEndPlaySlice = 4;     // StreamOutEndPlaySlice (epicport 2026-11-12; Epic UE 5.6.0 dfeb9ecce7): a streamed-out
                                       // level's actors get EndPlay in batches of this many actors inside RemoveFromWorld's own
                                       // time limit instead of all in the finishing call (0 = stock; start-up install, then live)
    int startupBarFinish = 2;          // StartupBarFinish (loader106 2026-11-16, S-058; 0 until the release flips it): the old
                                       // engine's loading-bar finish loop ends at its first step: 1 = the first loading menu of
                                       // the process (start-up), 2 = every loading menu (start-up install when != 0, then live)
    int pluginPrefetch = 1;            // PluginPrefetch (gapfix 2026-11-16, S-058; 0 until the release flips it): a low-priority
                                       // thread reads the active plugin files once at start-up into the file cache (start-up)
    int topicSort = 1;                 // TopicSort (gapfix 2026-11-16, S-058; 0 until the release flips it): the old engine's
                                       // dialogue-topic list sort as a stable merge sort, same comparison, identical order
                                       // (start-up install when != 0, then live; lab: 2 = verify against the game's sort)
    bool resetLoadersNoFlush = true;  // ResetLoadersNoFlush (epicport 2026-11-12; Epic UE 5.4.0 0d790f0ba5): ResetLoaders of a
                                       // package without a loader linker returns without FlushAsyncLoading (start-up, then live)
    int gcMarkPriority = 1;            // GcMarkPriority (gc104 2026-11-15, gcprobe's design): a task worker below the level runs
                                       // its part of a GC pass's mark at normal (1) or above normal (2) priority, then goes back
                                       // (start-up install when != 0, then live; memory/gc_mark_priority.cpp)
    int gcPrepAhead = 1;               // GcPrepAhead (gcprepbuild 2026-11-19, S-041): pending streamed-out levels prepared for
                                       // the next GC pass (the engine's PrepareStreamedOutLevelForGC) in calm frames of the last
                                       // second before it, oldest first (start-up install, then live; memory/gc_prep_ahead.cpp)
    int gcPrepAheadMinAgeMs = 3000;    // not in the shipped ini: a level must have been pending this long
    int gcPrepAheadMaxPerFrame = 2;    // not in the shipped ini: levels a frame
    int gcPrepAheadBudgetUs = 500;     // not in the shipped ini: no further level once a frame's preparations took this long
    int gcPrepAheadWindowMs = 1000;    // not in the shipped ini: how long before the predicted pass
    int gcPassAfterBusy = 1;           // GcPassAfterBusy (gchold 2026-11-14): a due timer GC pass waits through busy frames
                                       // (cell added / removed, character steps run, frame >= 13 ms at the call) with the
                                       // engine's own one-frame delay, up to a bound (0 off, 1 = 500 ms, 100-1000 ms; start-up
                                       // install, then live; memory/gc_pass_after_busy.cpp)
    bool creatureAnimPreload = true;  // CreatureAnimPreload (kfbuild 2026-11-13): the creature queue step always queues the
                                       // skeleton folder's animation files, as summons do (one byte; live)
    int npcDeferAnimInit = 0;          // [Fixes] NpcDeferAnimInit (lab, deferanim 2026-11-13): far characters' units before the post-actor tick
                                       // init their animation with the reference pose, first pose by the engine's update or at
                                       // the post-actor tick (1; 2 = also the deferred layer links); start-up install, then live
    bool pacingLeanFlowDrain = true;  // PacingLeanFlowDrain: a level flow at its wait bound drains the owed main queue only;
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
    int sharedMemoryGpu = 2;           // SharedMemoryGpu (memory_monitor.cpp): on a GPU that uses system RAM as its memory (D3D12 UMA:
                                       // integrated GPUs, APUs, handhelds) the memory-keeping fixes act as the game; 2 = when the
                                       // game's GPU is one, on Windows itself (not Wine/Proton/CrossOver) with under 20 GB of RAM
                                       // installed; 1 = always (test on any PC), 0 = never; live
    bool navmeshBuildNoBusyWait = true; // NavmeshBuildNoBusyWait: navmesh tile builds never run inside another task's busy wait
    bool busyWaitFlagCheck = true;      // BusyWaitFlagCheck: a busy-waiting worker checks every task it picks up (UE 5.3 checks only the
                                        // first): tasks that must not run inside busy waits (NavmeshBuildNoBusyWait's tiles) never do; live
    bool gpuSceneKeepCapacity = true;  // GpuSceneKeepCapacity: GPU scene buffers keep their capacity instead of shrink/grow re-creations
    bool commandAllocatorTrim = true;  // CommandAllocatorTrim: D3D12 command allocator pools shrink back to recent need
    int cmdMemPregrow = 1;             // CmdMemPregrow: direct allocators pre-grown before the first world: 0 off, 1 automatic size, else the size (start-up)
    int cmdMemGiveBackHeadroomMB = 512; // not in the shipped ini: CmdMemPregrow gives back what the session has not used, once, when within this much of the budget for 3 s; 0 = never (start-up)
    int allocatorTrimWindowSeconds = 20; // not in the shipped ini: the window whose peak of allocators in flight is kept
    int allocatorTrimHeadroomMB = 2048; // not in the shipped ini: CommandAllocatorTrim trims the copy pool only within this much of the budget (0 = always)
    int gpuPoolKeepIniMB = 0;          // GpuPoolKeepMB as written in the ini (before the 0..4096 clamp; for the start-up log line)
    int gpuPoolKeepMB = 384;             // GpuPoolKeepMB: memory/gpu_pool_keep.cpp, empty D3D12 texture/buffer pools kept up to this
                                       // many MB instead of destroyed 20 frames after going empty (start-up install when > 0, then
                                       // live; 0 later = given back one per allocator per frame). 0 until the release
    int allocatorTrimCopyFloor = 64;    // AllocatorTrimCopyFloor (live): CommandAllocatorTrim gives back no copy allocator below this many, counting those in use; it makes none (0 = no floor; not over budget). 0 until the release, which sets 64
    int immediateAllocatorLists = 0;   // not in the shipped ini: CommandAllocatorTrim part 2, lists the immediate context records per allocator
    bool logFile = true;               // [General] LogFile: 0 = no OblivionPerfFix.log except a GPU crash report (start-up only; lab builds ignore it)
    bool gpuCrashReport = true;        // [General] GpuCrashReport: DRED + memory report in the log at a GPU crash
    int gpuPassMarkers = 0;            // [General] GpuPassMarkers: 1 pass names in DRED, 2 AMD markers (RGD/RGP), 3 both (installed at start-up when not 0, then live)
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
uint8_t* AltarCommandSetter();  // Altar's command setter, bool (const {FString name, FString value}*, bool force); nullptr if not found
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
bool InstallWaterCapture(bool beforeGameCode, bool gtDetour);  // start-up only; gtDetour = UpdateWaterInfoRendering too (WaterInfoSkip, lab WaterInfoProbe)
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
bool InstallRayTracingBuildBatch(bool beforeGameCode);   // start-up only
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
// Whether this exe is a build we know. An unknown one gets one plain log line; nothing is switched off because of it
// (each fix checks its own code).
bool NoteGameBuild(const GameModule& game);
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
// ---- mem.cpp: other mods' hooks (obseparity, 2026-11-21) ----
// The OBSE64 route scans after other injected code ran (UE4SS, OBSE64 itself, other plugins), so a function may already
// start with another mod's jump. Forms recognised: E9 rel32, EB rel8, FF 25 [rip+d32] (an import thunk, whose slot lies
// in the module's own image with d32 != 0, is not counted), mov r64,imm64 + jmp r64 (also r8-r15), push imm32 + ret and
// push imm32 + mov [rsp+4],imm32 + ret.
struct JumpInfo {
    size_t length = 0;               // bytes of the jump instruction(s); 0 = not a jump
    const uint8_t* target = nullptr;  // its target (one hop)
    bool importThunk = false;         // FF 25 whose slot lies in the jump's own module image (d32 != 0): the module's own code
};
JumpInfo DecodeJump(const uint8_t* at);
// Follows jumps from `at` (at most 6 hops, readable memory only); the address where they end.
const uint8_t* FollowJumps(const uint8_t* at);
// "dwmapi.dll+0x1234", "anonymous executable memory", "unmapped memory" (for log lines).
void DescribeAddress(const void* p, char* out, size_t size);
// A foreign hook at `at`: a jump (not an import thunk) whose chain ends outside the module that holds `at`. finalTarget
// gets where it ends.
bool ForeignJumpAt(const uint8_t* at, const uint8_t** finalTarget = nullptr);
// Look-ups for functions we only CALL: FindAll; if it finds nothing, candidates whose first bytes are another mod's jump
// and whose remaining pattern bytes (from the end of the jump, at an instruction boundary within 15 bytes after it) match.
// readFrom = the lowest pattern offset the caller reads bytes at (a candidate whose overwritten bytes reach past it is not
// taken). One "[hook]" log line per hooked function found. Never use it for a function whose entry bytes we patch.
std::vector<uint8_t*> FindAllCallable(const Section& s, const Pattern& p, const char* tag, size_t readFrom = SIZE_MAX);
bool MatchesAtCallable(const uint8_t* at, const Pattern& p, const char* tag, size_t readFrom = SIZE_MAX);
// Patch sites: the description of a foreign hook whose jump covers any of [at, at + size) (a jump starting at most 13 bytes
// before), or nullptr when there is none. WriteCode refuses such a write with one "[hook]" line and returns false.
const char* ForeignHookOverlap(const void* at, size_t size, char* why, size_t whySize);
uint64_t ForeignHooksMet();  // [hook] lines so far (call-only look-ups through a hook + refused writes)
// A slot of the game's tables (not its import table) whose current value is another mod's code, or nullptr (WritePointer).
const char* ForeignPointerSlot(void* const* slot, char* why, size_t whySize);
// Our own code memory: AllocNear registers its regions; a stub allocated another way calls RegisterOwnCode (returns p).
void* RegisterOwnCode(void* p);
bool IsOwnCode(const void* p);
void UnregisterOwnCode(const void* base);
BOOL FreeOwnCode(void* base);  // VirtualFree(base, 0, MEM_RELEASE) after taking it out of the registry
// Code ranges we wrote: writing there again (rollback, live switch) is never refused (WriteCode etc. record them).
void RecordOwnWrite(const void* at, size_t size);
bool OverlapsOwnWrite(const void* at, size_t size);
void ForgetOwnWritesForTest();
bool OsPageCacheTestPatchThunk(uint8_t* thunk, const uint8_t* stub);  // memory/os_page_cache.cpp, self-test  // self-test: refusal checks at a site it has written before
// VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, old) for the fixes that write code themselves (one compare-exchange):
// refuses (FALSE, one [hook] line) when another mod's jump covers or precedes the site as ForeignHookOverlap finds.
BOOL CodeSiteProtect(void* at, size_t size, DWORD* old);
// Code bytes that lie within one aligned 8-byte word, written with one compare-exchange (another thread sees either the old
// or the new bytes): for code other threads may run while we patch (before WinMain). False if they do not fit one word.
bool WriteCodeAtomicFits(const void* at, size_t size);
bool WriteCodeAtomic(void* at, std::span<const uint8_t> bytes);
uint64_t CodeWriteRetries();  // WriteCode copies retried because the page's protection changed under them
// ---- mem.cpp: load stage (obseparity, 2026-11-21) ----
// Where in the game's start OblivionPerfFix was initialised. InDllMain: as version.dll, from DllMain, before any game code
// (every start-up-only fix may install). BeforeWinMain: as an OBSE64 plugin, which OBSE64 calls on the main thread before
// WinMain. What is checked at run time is weaker: called on the process's first thread and no engine thread started in the
// exe yet (UE's crash-reporting thread from static init excepted). An OBSE64 that called plugins inside WinMain but before
// the engine starts its threads would pass too (review S5); the per-key audit assumed before WinMain. Late: anything else;
// the start-up-only fixes stay off then.
enum class LoadStage { InDllMain, BeforeWinMain, Late };
LoadStage CurrentLoadStage();
void SetLoadStage(LoadStage stage);  // Init
const char* LoadStageName(LoadStage stage);
// BeforeWinMain needs both: called on the process's first thread (its start address is the exe's entry point) and no other
// thread started in the exe yet (UE's engine threads, the task graph and the RHI threads all start in the exe).
LoadStage ClassifyLoadStage(bool onMainThread, int exeThreads);
struct ThreadCensus {
    bool ok = false;              // the snapshot worked
    bool currentIsEntry = false;  // the calling thread started at `entry`
    int threads = 0;              // all threads of the process
    int exeThreads = 0;           // threads started in [exeBase, exeBase + exeSize) other than at `entry`
    int crashThreads = 0;         // of those, UE's crash-reporting thread (started by the exe's static set-up; not counted)
    struct ExeThread { uint32_t rva; int64_t createdMs; bool crash; };  // start (exe offset), created ms after the process
    std::vector<ExeThread> exeStarts;
    std::vector<std::pair<std::string, int>> others;  // the other threads by the module they started in
};
ThreadCensus TakeThreadCensus(const uint8_t* exeBase, size_t exeSize, const uint8_t* entry);
// UE 5.3's FCrashReportingThread proc (WindowsPlatformCrashContext.cpp: the global GCrashReportingThread is constructed in
// static init and CreateThread()s CrashReportingThreadProc, which loops on WaitForMultipleObjects(IE_MAX, ..., false, 500)):
// recognised by that call (mov r9d,500 then a call through the exe's WaitForMultipleObjects import within its first bytes).
bool IsCrashReportingThreadStart(const uint8_t* exeBase, const uint8_t* start);

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
    uint64_t warmFaults;            // faults in the comparison's warm-up (ignored; the comparison then decides as before)
    uint64_t verified, verifyDiffs; // lab ClothConfigWarm=2: decisions compared without and with the warm-up, disagreements
};
// beforeGameCode: loaded from DllMain. Otherwise the call is only switched if that is one atomic write.
bool ApplyClothConfigFix(bool beforeGameCode);
void SetClothConfigCacheEnabled(bool enabled);  // switchable live; off = copy every frame as before
void LogClothConfigStats();
bool ClothConfigFixInfo(ClothConfigInfo* out);
ClothConfigStats GetClothConfigStats();
void ClothConfigTestOriginal(void* fakeInitialize);  // self-test only: replaces the original Initialize
// The comparison's warm-up (prefetch pass): 1 = on (shipped); lab ClothConfigWarm: 0 = off, 2 = verify. Live.
void SetClothConfigWarm(int mode);
int ClothConfigTestDecide(void* config, const void* propertyCollections, int warm);  // self-test only: 0 = would skip
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
int GcPassAfterBusyNextUnits(uint64_t elapsedUs);  // gc_pass_after_busy.cpp: units the coming frame carries (1 GC pass, 2 removal)
double* GcPassAfterBusyDeltaTime();                // gc_pass_after_busy.cpp: FApp::DeltaTime (null when not installed)
bool InstallCapSleepSkip(bool beforeGameCode, int key);  // npc_pacing.cpp (CapSleepSkip): start-up, after InstallNpcPacing
void SetCapSleepSkip(int bits, int maxMs, int addBack);  // monitor thread, each second (live once installed; addBack: lab only)
void LogCapSleepSkipStats();                             // stats interval (no line when nothing ran)
bool CapSleepSkipTestSite(const GameModule& game);                             // self-test: the engine's GetMaxTickRate call it patches
int CapSleepSkipTest(int which);                         // self-test: rate hook and decision on fakes (1 = as expected)

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
uint32_t FadeToGameCalls(bool* hooked);       // fade_hook.cpp: fade-to-game calls seen so far (CmdMemPregrow, DrawBehindLoadingScreen)

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
uint8_t* FindObjectArrayFromDtor();                    // GUObjectArray from ~UObjectBase (no patching; nullptr if not exactly one match)
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

// indirect_args_pool.cpp (IndirectArgBufferPool: d3d12.AllowPoolAllocateIndirectArgBuffers 1 before the engine registers it)
bool FindIndirectArgBufferPool();          // no patching; false = unavailable (logged once)
void SetIndirectArgBufferPool(int key, bool beforeGameCode);  // start-up, once (Init): 1 written while the int holds the stock 0
void IndirectArgBufferPoolTick(int key);   // once a second: later ini changes only logged; a player's Engine.ini value reported
bool IndirectArgBufferPoolApplied();
int32_t* IndirectArgBufferPoolVar();       // the variable's int (nullptr if not found)
uint64_t* IndirectArgBufferPoolVtableSlot();  // its auto-object's vtable pointer (0 until registered; nullptr if not found)
void IndirectArgBufferPoolTestReset();     // self-test only

// parallel_file_reads.cpp (ParallelFileReads: several reads of a game container file in flight; the engine's reader's vtable slot)
struct ParallelFileReadsInfo {
    uint8_t* startRequests = nullptr;  // FGenericFileIoStoreImpl::StartRequests
    void** slot = nullptr;             // its vtable slot
    uint8_t* pop = nullptr;            // FFileIoStoreRequestQueue::Pop (the call the original makes)
    uint8_t* serviceNotify = nullptr;  // the generic backend's ServiceNotify (two slots after StartRequests)
};
bool FindParallelFileReads(const GameModule& game, ParallelFileReadsInfo* info);  // no patching
bool InstallParallelFileReads(int reads);  // start-up, either route: both slots replaced when reads >= 2
void SetParallelFileReads(int reads);      // live, once a second (no-op unless installed)
void LogParallelFileReadsStats();          // one line when blocks were read since the last
bool ParallelFileReadsTestStart(void* impl, void* queue, int reads, void* pop, void* original);  // self-test only
void ParallelFileReadsTestNotify();        // self-test only: our wake event set (what a ServiceNotify does)
bool ParallelFileReadsTestNotifyHook(void* impl, void* original);  // self-test only: the wrapped ServiceNotify itself
void ParallelFileReadsTestHooks(int failCompletions, int seekPenalty, bool anyFile);  // self-test only (-2 = ask Windows)
int ParallelFileReadsTestBusy();           // self-test only: reads in flight
int ParallelFileReadsTestSeekPenalty(void* file);  // self-test only: the real question to Windows (1 hard disk, 0 not, -1 unknown)
void ParallelFileReadsTestReset();         // self-test only
void ParallelFileReadsTestCounts(uint64_t* reads, uint64_t* inFlightMost, uint64_t* retried, uint64_t* failed, uint64_t* engineReads, int* state,
                                 uint64_t* seekQueries = nullptr);

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
// bow_sneak.cpp (BowSneakAttackFix: a player's arrow hits the old engine's hit function once, the old engine's own
// impact with the Unreal side's sneak multiplier instead of a fixed 1.0)
struct BowSneakInfo {
    uint8_t* queueCall;   // the Arrow_hit_Reverse_Pairing handler's call (now to the stub)
    uint8_t* impactCall;  // the old engine's projectile impact's call [r10+868h] (now call stub; nop)
    uint8_t* stub;
    void* thunk;          // the hit thunk the handler calls
    void* const* player;  // the old engine's player pointer
    void* queueHook;
    void* impactHook;
    uint64_t skipped, corrected, oneOnly;
};
bool ApplyBowSneakAttackFix();                     // start-up (two calls on the OblivionThread's code)
void SetBowSneakAttackFixEnabled(bool enabled);    // switchable live; off = both hits as in the stock game
void LogBowSneakAttackFixStats();
bool BowSneakAttackFixInfo(BowSneakInfo* out);
void BowSneakAttackFixTestSetup(void* fakeThunk, void* const* fakePlayer, uint64_t (*clock)());  // self-test only
// stuck_effect_look.cpp (StuckEffectLookFix, game bug #48): the stock status-effect blueprint puts its overlay material (a MID)
// on the owner's meshes and attached actors, weapons included, but its removal skips weapons (and cannot reach a rider that
// has dismounted): those keep the look. Every blueprint call of MeshComponent.SetOverlayMaterial is seen (its exec
// function, by name and shape); at the frame end a mesh whose overlay is still a status-effect MID whose blueprint shows no
// effect any more gets the newest look under it that is still showing, else none (as the stock removal does to the rest).
struct StuckEffectLookInfo {
    uint8_t* exec;           // MeshComponent.SetOverlayMaterial's exec function (detoured)
    uint8_t* rtExec;         // PrimitiveComponent.SetVisibleInRayTracing's exec function
    uint8_t* rtSetter;       // UPrimitiveComponent::SetVisibleInRayTracing (called from its exec)
    uint8_t* markDirty;      // UActorComponent::MarkRenderStateDirty (called from the inlined overlay setter)
    uint32_t overlayOffset;  // UMeshComponent::OverlayMaterial
    uint32_t precacheSlot;   // the vtable slot the setter calls (PrecachePSOs)
    uint8_t* objectArray;    // GUObjectArray
    uint8_t* fnameCtor;      // FName(const TCHAR*, EFindName)
};
bool FindStuckEffectLook(StuckEffectLookInfo* out);  // locate only; false = unavailable (logged once)
bool InstallStuckEffectLook(bool beforeGameCode);    // start-up (an entry detour of the exec function); needs NpcCreationPacing's frame hook
void SetStuckEffectLookFix(bool on);                 // live
void StuckEffectLookFrameEnd();                      // game thread, once per frame (NpcCreationPacing's frame hook)
void LogStuckEffectLookStats();
int StuckEffectLookTestLogic();                      // self-test: the record / sweep rules on fake objects (0 = pass, else the failed check)
// guards/world_sound_position.cpp (WorldSoundPositionFix, GitHub #52, a game bug fix; default off until it has been tested
// in play): while Wwise loads a SoundBank (the exported AK::SoundEngine::LoadBankMemoryView / Copy, 8 overloads, entry detours),
// listed sound containers whose positioning the game's data sets to "no 3D panning" (the NPC torch-lighting sound also without
// attenuation) get the 3D flags byte of their correctly set parents; each checked by id, type, parent, attenuation and old value
struct WorldSoundTarget {
    uint32_t id;           // the container's Wwise short id
    uint8_t type;          // HIRC object type (2 sound, 5 random/sequence container)
    uint32_t parent;       // its DirectParentID
    uint32_t attenuation;  // its AttenuationID property (the distance curve it keeps)
    uint8_t oldBits, newBits;  // uBits3d as the game ships it, and as set
    const char* what;
};
extern const WorldSoundTarget kWorldSoundTargets[];
extern const size_t kNumWorldSoundTargets;
struct WorldSoundBankResult {
    uint32_t toPatch = 0, patched = 0, already = 0, mismatch = 0;  // listed objects: to change / changed / already set / differ
    uint32_t touchedMask = 0, mismatchMask = 0, alreadyMask = 0;     // bit per kWorldSoundTargets entry
    bool otherVersion = false;                                       // BKHD names another bank version: nothing read further
};
void WorldSoundScanBank(const uint8_t* data, size_t size, WorldSoundBankResult* res, uint8_t* out);  // out = data to write, or null
bool InstallWorldSoundPosition(bool beforeGameCode);  // installed at start-up (either route), entry detours of the 8 exports
void SetWorldSoundPositionFix(bool on);               // live switch: affects banks loaded afterwards (loaded ones keep their state)
void LogWorldSoundPositionStats();
bool WorldSoundPositionInfo(uint8_t* const** fns, int* n);  // self-test: the 8 hooked exports (true = installed)
using LoadFn_t = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
void WorldSoundPositionTestCall(int which, void* data, uint32_t size, LoadFn_t fake);  // self-test: one hook with a fake original
// guards/arrow_hit_sound.cpp (ArrowHitSoundFix, GitHub #52, a game bug fix; default off until tested in play): an NPC's
// arrow hit posts its impact sound on the shooter's weapon (once per engine that registers the hit) besides the arrow's own
// post at the hit; with the fix each NPC arrow hit's thud is posted once, on the arrow. Player shots unchanged.
struct ArrowHitSoundInfo {
    uint8_t* bowCall;    // the hit handler's weapon post (now to the stub)
    uint8_t* arrowCall;  // the arrow's own impact post (now to the stub + 32)
    uint8_t* stub;
    void* post;          // the post helper both calls reach
    void* setSwitches;   // the impact type / material switch setter both reach
    void* constant;      // their r9 argument
    void* weakGet;       // FWeakObjectPtr::Get
    void* arrowClass;    // the arrow's StaticClass
    uint32_t playerFlag; // the pawn's "is the player" byte (the Player sound switch)
    uint32_t compOffset; // the pawn's hit component (impact type and material)
    uint32_t ownerOffset; // a component's owner (as the switch setter reads it)
    void* bowHook;
    void* arrowHook;
    uint64_t skipped, moved, kept, player, arrowSkipped;
};
bool ApplyArrowHitSoundFix();                   // start-up (two calls in the game thread's hit code)
void SetArrowHitSoundFixEnabled(bool enabled);  // live; off = both posts as in the stock game
void LogArrowHitSoundFixStats();
bool ArrowHitSoundFixInfo(ArrowHitSoundInfo* out);
void ArrowHitSoundFixTestSetup(void* fakePost, void* fakeSwitches, void* fakeWeakGet, void* fakeClass, uint64_t (*clock)());  // self-test only
// autosave_menu.cpp (AutosaveFollowsMenu: the level-up's and the scripts' autosaves are not made while all four of the
// menu's autosave options are off)
struct AutosaveMenuInfo {
    uint8_t* levelUpCall;          // the level-up's call of the autosave (now to the stub)
    uint8_t* scriptedCall;         // the scripts' Autosave command's call (now to the stub + 16)
    uint8_t* stub;
    void* autosave;                // the old engine's autosave function
    const uint8_t* switches[4];    // bSaveOnInteriorExteriorSwitch, bSaveOnTravel, bSaveOnRest, bSaveOnWait
    void* levelUpHook;
    void* scriptedHook;
    uint64_t skippedLevelUp, skippedScripted, made;
};
// saves/save_thumb_leftover.cpp (SaveThumbLeftoverFix, shot130q; a game bug fix, also without the mod)
struct SaveThumbLeftoverSites { uint8_t* serviceBind; uint8_t* addRawCall; uint8_t* callback; uint8_t* removeCall; uint8_t* engineDelegate; uint8_t* rawExec; };
const char* SaveThumbLeftoverDiscover(const GameModule& game, SaveThumbLeftoverSites* out);  // nullptr = all found (self-test too)
bool InstallSaveThumbLeftoverFix(bool beforeGameCode);  // start-up: two calls redirected
void SetSaveThumbLeftoverFix(bool on);                  // live once installed
void LogSaveThumbLeftoverStats();
uint8_t* SaveThumbLeftoverResolve(uint8_t* p);          // the game's Remove when p is our redirect (SaveThumbnailAsync's check)
uint8_t* SaveThumbLeftoverFind(const uint8_t* delegate, uint64_t handle);  // the bound instance with that handle, or nullptr
bool SaveThumbLeftoverShouldRemove(bool on, uint64_t handle, uint64_t boundHandle, DWORD boundThread, DWORD thread, bool foundRawExec);
void SaveThumbLeftoverTestSetup(void* addRaw, void* remove, const uint8_t* rawExec, bool on);  // self-test only (fakes)
uint64_t* SaveThumbLeftoverTestAddRaw(void* delegate, uint64_t* out, void* object, void* fn);  // self-test: the AddRaw hook
bool SaveThumbLeftoverTestRemove(void* delegate, uint64_t handle);                            // self-test: the Remove hook
uint64_t SaveThumbLeftoverTestNoted();                                                       // self-test: the noted handle
bool ApplyAutosaveFollowsMenu();                   // start-up (two calls on the OblivionThread's code)
void SetAutosaveFollowsMenuEnabled(bool enabled);  // switchable live; off = both autosaves as in the stock game
void LogAutosaveFollowsMenuStats();
bool AutosaveFollowsMenuInfo(AutosaveMenuInfo* out);
void AutosaveFollowsMenuTestSetup(void* fakeAutosave, const uint8_t* const* fakeSwitches);  // self-test only (nullptr: restore)
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

// menus/display_native.cpp (MonitorNativeFromWindows, FullscreenAllResolutions; on by default): a
// monitor's native resolution from Windows' preferred mode instead of its EDID's first timing; the fullscreen list
// without the native-aspect filter. Start-up only.
struct DisplayNativeInfoOut {
    uint8_t* callSite;         // RebuildDisplayMetrics' call to GetMonitorsInfo
    uint8_t* getMonitorsInfo;
    uint8_t* filterJump;       // the fullscreen filter's jnz
};
bool FindDisplayNative(DisplayNativeInfoOut* out);  // locate only (true if either site is found)
bool InstallMonitorNativeFromWindows();
bool InstallFullscreenAllResolutions();
void LogDisplayNativeStats();

// SeparableSkinAbove4K (trade-off): separable skin scattering while the render is wider than 4K (skin_separable.cpp).
void SetSeparableSkinAbove4K(bool on);            // once per second from the monitor; live
void LogSeparableSkinAbove4KStats();
void SeparableSkinTestSetup(int32_t* data, const float* percent, const int* windowWidth);  // self-test only: fakes
void** SeparableSkinConsoleManager();             // &GConsoleManager as found from the game's lookups (null: not found)

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
// memory_monitor.cpp: the last sample's commit room (available commit + page file growth room) and available commit alone
// (bytes); false = no sample yet.
bool LatestCommitRoom(uint64_t* room, uint64_t* available);
// shader_screen.cpp (ShaderScreenAfterDriverChange, game bug): the start-up's Save_Settings check (which decides the
// game's first-launch shader screen) answers "no save" once after the graphics card or driver changed
bool ApplyShaderScreenAfterDriverChange(bool beforeGameCode, const wchar_t* dir);  // start-up only; dir: the DLL's folder
bool ShaderScreenInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook);
void ShaderScreenTestSetup(void* original, bool (*gpu)(char*, size_t), const wchar_t* recordPath);  // self-test
void ShaderScreenPrecompileFinished();  // pso_precompile.cpp: the start-up precompile ended (records a pending card/driver)
int ShaderScreenShownUnrecorded();      // self-test: the "shown N" count this start-up wrote (-1: the record not writable)
// menu_hdr_background.cpp (HdrMenuBackground, TRADE-OFF, off by default): in HDR10 the menu's background
// screenshot holds sRGB-encoded PQ values the UI shows as colour; remapped to sRGB at the same nits
bool ApplyHdrMenuBackground(bool beforeGameCode);  // redirects the handler's one ImageResize call (start-up)
void SetHdrMenuBackground(bool on);                // live
void LogHdrMenuBackgroundStats();
bool HdrMenuBackgroundInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook, void*** manager);
void HdrMenuBackgroundTable(float uiNits, uint8_t (&table)[256]);  // screenshot byte (sRGB-encoded PQ) -> sRGB at the same nits
bool HdrMenuBackgroundShotTable(uint8_t (&table)[256]);  // GpuMenuShot: the table the handler would apply now (false: none)
void HdrMenuBackgroundTestSetup(void* original, const int32_t* hdr, const int32_t* device, const float* uiNits);  // self-test
// (same file) HdrSaveThumbnail (TRADE-OFF, off by default): the save's screenshot callback's pixel copy
// through a wrapper that converts the holder's bytes with the same table in HDR10
bool ApplyHdrSaveThumbnail(bool beforeGameCode);   // redirects the callback's one copy call (start-up)
void SetHdrSaveThumbnail(bool on);                 // live
void LogHdrSaveThumbnailStats();
uint8_t* HdrSaveThumbnailResolve(uint8_t* p);      // the game's copy when p is the stub, else p (SaveThumbnailAsync's check)
bool HdrSaveThumbnailInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook);
void HdrSaveThumbnailTestSetup(void* original);    // self-test: the copy the wrapper calls
void** HdrConsoleManager();                        // &GConsoleManager (menu_hdr_background.cpp's lookup), or null; monitor thread
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
// OsPageCacheAsyncRelease (os_page_cache.cpp): the cache's releases to Windows from one background thread
void SetOsPageCacheAsyncRelease(int mode);  // live (1-second tick, before SetOsPageCache, which applies it); needs OsPageCache installed
void OsPageCacheAsyncPause();               // a crash is being written: the thread stops, releases are made at once (any thread)
void OsPageCacheMemoryLow(bool low);        // memory_monitor.cpp, every sample: RAM or commit under 2x the relief thresholds
struct OsPageCacheAsyncStatsOut {
    uint64_t queued, released, drained, fullBlocks, fullBytes, busy, paused, drainBusy, maxBlocks, maxBytes, offWait, low, threadUs;
    uint32_t queuedNow;
};
BOOL OsPageCacheTestRelease(void* ptr, size_t bytes);  // self-test: a game thread's release through the queue
void OsPageCacheTestHold(bool hold);                   // self-test: the release thread takes nothing while held
void OsPageCacheTestResume();
bool OsPageCacheTestWaitIdle(uint32_t ms);
void OsPageCacheTestLockQueue(bool lock);
bool OsPageCacheTestAsyncOn();
void OsPageCacheTestAsyncStats(OsPageCacheAsyncStatsOut* out);
struct OsPageCacheInfoOut {
    uint8_t* allocFn;
    uint8_t* freeFn;
    const uint8_t* cacheRet[2];
};
bool OsPageCacheInfo(OsPageCacheInfoOut* out);
// pso_precompile.cpp (PsoPrecompileFast: the start-up PSO precompile at UE's Fast pace outside play)
bool InstallPsoPrecompileFast(bool beforeGameCode);  // SetBatchMode's entry detour (start-up)
void SetPsoPrecompileFast(bool enabled);             // live (the game thread's timer applies it at its next check)
// Rule R1 (user decision 2026-11-11, GitHub issue 51): the faster pace only while TaskWaitDeadlockGuard is installed and on
// (its mode 1); without it (the OBSE64 route, TaskWaitDeadlockGuard=0 or unavailable, mode 0 or 2) the game's own pace.
bool PsoPaceInstall(bool keyOn, int taskWaitGuardMode);  // start-up: detour SetBatchMode at all (key on, guard installed: mode >= 0)
bool PsoPaceAllowed(bool keyOn, int taskWaitGuardMode);  // the faster pace now (key on, guard in mode 1)
void ApplyPsoPrecompileFast(bool keyOn, int taskWaitGuardMode);  // live: SetPsoPrecompileFast(PsoPaceAllowed(..)), a log line
                                                                 // when the guard's mode turns the faster pace off or on again
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
void SetCellBufferEvictDelay(int ms);                    // CellBufferEvictDelayMs (live): the paced drain starts ms after a crossing
uint64_t CellBufferEvictDelayFrames();                   // OT frames a drain waited for that delay
void LogCellBufferPacedEvictStats();
// npcs/cell_evict_split.cpp (CellEvictSplit, flightrest / evfix 2026-11-13; public 1.5.0): a paced unload's actors' 3D first
struct GameModule;
bool CellEvictSplitFind(const GameModule& game, uint32_t out[4]);  // self-test: 3D removal, lock, unlock, player (RVAs)
bool CellEvictSplitFindGates(const GameModule& game, uint32_t out[3]);  // self-test (rev96f B1): arrow count, ProcessLists, MagicHitEffect RTTI
bool InstallCellEvictSplit();                          // start-up (pattern only, no code written); false: the key stays off
void SetCellEvictSplit(int perStep);                   // live: actors cleared per OT frame (0-8), 0 = off; 0 when not found
bool CellEvictSplitStep(uint8_t* cell);                // OT, cell_buffer.cpp's paced Trim: true = a step ran, the cell stays
bool CellEvictSplitTookFrame();                        // OT, cell_buffer.cpp: a step ran in this OT frame (clears it)
void CellEvictSplitForget();                           // OT, cell_buffer.cpp (fix102 N1): a non-paced Trim ran (budget reset)
// self-test (fix102, rev102b M1): fake lock / unlock / player / arrow count / ProcessLists / MagicHitEffect RTTI so that
// CellEvictSplitStep's decision runs on fake objects; nullptr restores the found (or not found) state
struct CellEvictSplitFakes {
    void (*lock)(void* lockObj, void* cell);
    void (*unlock)(void* lockObj, void* cell);
    void* lockObj;
    void** player;
    const volatile int32_t* arrows;
    uint8_t* const* processLists;
    const uint8_t* hitRtti;
};
void CellEvictSplitTestFakes(const CellEvictSplitFakes* f);
void CellEvictSplitTestCounts(uint64_t out[8]);  // self-test: steps, cleared, arrow skips, hit excluded, limit, empty, fallbacks, faults (zeroed)
void LogCellEvictSplitStats();
// settings/hdr_menu.cpp (HdrMenuRows, off by default): an HDR section on the game's PC Graphics page
// (a new settings table and category row, in memory; game thread): HDR, peak, paper white, black floor, mid-tone
struct HdrMenuInfoOut {
    uint8_t* fromString;   // FText::FromString(const FString&)
    uint8_t* fnameCtor;    // FName(const TCHAR*, EFindName)
    uint8_t* malloc;       // FMemory::Malloc
    uint8_t* free;         // FMemory::Free (checked: the same GMalloc)
    uint8_t* gmalloc;
    uint8_t* objectArray;  // GUObjectArray
    uint8_t* setter;       // Altar's command setter (the HDR variables synced to the shown state once)
    uint8_t* addOrUpdate;  // UAltarGameUserSetting::AddOrUpdateSettingsToSave
    uint8_t* saveWriter;   // the settings save's writer (Save_Settings slot)
    const uint8_t* saveSlot;  // its slot name FString
    uint8_t* spawnObject;  // UGameplayStatics::SpawnObject (NewObject for the HDR section's table)
};
bool FindHdrMenu(HdrMenuInfoOut* out);   // locate only; false = unavailable (logged)
bool InstallHdrMenu(bool hdrRows, bool rayReconstruction, bool hudAspect, bool beforeGameCode);  // start-up, the parts that are on (needs NpcCreationPacing's frame hook)
uint8_t* FindProcessEvent();                       // HudAspectRow: UObject::ProcessEvent by its prologue, or null
uint8_t* FindFNameCtor();                          // FName(const TCHAR*, EFindName) by its pattern, or null (StuckEffectLookFix)
std::wstring GameUserFolder();                     // the game's own "...\Documents\My Games\Oblivion Remastered\" (via local AppData), or empty
bool HudAspectIntRegistrationFound();              // HudAspectRow: the console manager's int registration as expected (self-test)
float HudPadding(float w, float h, float scale, float aspect);  // HudAspectRow: Slate padding per side for a centred box (-1: no viewport)
constexpr float kHdrBlackFloorMin = -8.f, kHdrBlackFloorMax = -1.f;  // HdrMenuRows: Black Floor's range (log10 nits, whole steps)
bool HdrBlackFloorFromSaved(double saved, float* out);  // a saved Black Floor in range, rounded to a whole step; false outside it
bool RrLockInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook);  // self-test: the upscaler lock's lookup call (installs it)
void RrLockTestSetup(void* original);    // self-test: the lookup the wrapper calls
void HdrMenuFrameEnd();                  // game thread, once per frame (NpcCreationPacing's frame hook)
void HdrMenuMonitorTick();               // monitor thread, every second (its first tick reads Save_Settings.sav)
// render/hdr_gamma.cpp (HdrGamma22, TRADE-OFF, off by default): the HDR tone curve's darks re-decoded as
// gamma 2.2 (an SDR monitor's) instead of sRGB, by refitting the curve's spline in its parameter block (no code patched)
struct HdrGammaInfoOut {
    uint8_t* getter;   // the tone-curve block's getter (every pass copies the block through it)
    uint8_t* block;    // the block (20 floats, .data)
    uint8_t* builder;  // the tone-scale parameter build (its 4.8 / 1.55 mid-point stores)
};
bool FindHdrGamma(HdrGammaInfoOut* out);  // locate only
bool InstallHdrGamma();                   // start-up (needs NpcCreationPacing's frame hook); nothing written while off
void SetHdrGamma22(bool on, int whiteX10);  // monitor thread (live); W = whiteX10 / 10 x the Mid-Tone
void HdrGammaFrameEnd();                  // game thread, once per frame (NpcCreationPacing's frame hook)
void HdrGammaMonitorTick();               // monitor thread, every second: fits a pending curve
void LogHdrGammaStats();
bool HdrGammaSelfFit(const float* block, double factor, double* worst, float* out);  // self-test: the fit on a recorded block
double HdrGammaEval(const float* block, double x);  // self-test: the curve's output (nits) at input x

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
struct CellBufferCounts { uint64_t trimmed, paced, immediate, blocked, crossings, maxOverflow, crossTail, insertTorn; };
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
bool InstallRigTraceReuse(bool beforeGameCode, const wchar_t* iniPath);  // start-up only
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
// perception_los_prefetch.cpp (PerceptionLosPrefetch, default off): a perception round's line-of-sight checks precomputed
// on helper threads at the round's start, served to the game's loop in its own order (equivalent)
bool InstallPerceptionLosPrefetch(bool beforeGameCode, int threads, bool ignoreCache);  // start-up only
void SetPerceptionLosPrefetch(int mode, int minJobs, bool helpersAllowed, bool overlap, bool ignoreCache, bool crossRound);  // live
void PerceptionLosTestSetCross(bool on, uint64_t (*frameFn)());                   // self-test only
void PerceptionLosTestCrossCounts(uint64_t* npcs, uint64_t* rounds, uint64_t* served);  // self-test only
void PerceptionLosTestGatherSites(const GameModule& game, uint32_t* att, uint32_t* child);  // self-test: the NPC gathers' RVAs
void PerceptionLosTestSetCache(void* fakeAttached, void* fakeChild, void* fakeMalloc, bool on);  // self-test only
void PerceptionLosTestGather(uint8_t* actor, void* out, bool child);                           // self-test only
void PerceptionLosTestCacheCounts(uint64_t* served, uint64_t* verified, uint64_t* diff);        // self-test only
void PerceptionLosTestSetOverlap(bool on);  // self-test only: the overlap with a stand-in round-end hook
bool PerceptionLosTestJoin();               // self-test only: the round-end join (true = a batch was open)
uint32_t PerceptionLosTestJoinSite(const GameModule& game);  // self-test: the round-end join's call site RVA (0 = not found)
void LogPerceptionLosPrefetchStats();
void PerceptionLosTestGuardSites(const GameModule& game, uint32_t* eye, uint32_t* lwiStatic);  // self-test: RVAs (0 = not found)
void PerceptionLosTestSetLwi(void* fakeStatic);  // self-test only: a stand-in subsystem static (nullptr = none)
uint64_t PerceptionLosTestLwiRounds();           // self-test only: rounds left to the game by the guard
void PerceptionLosTestSetEye(const void* fakeEye);  // self-test only: a stand-in eye point (nullptr = not checked)
uint64_t PerceptionLosTestOtherEyeRounds();      // self-test only: rounds left to the game by the eye check
const char* PerceptionLosTestDiscover(const GameModule& game, uint32_t* round, uint32_t* callSite, uint32_t* helper,
                                      uint32_t* fov);  // self-test: nullptr = found (fov 0 = the field-of-view filter not found)
void PerceptionLosTestSetup(void* fakeHelper, void* fakeClass, void* fakeFov, float rangeConst, int mode, int minJobs, int threads);  // self-test only
void PerceptionLosTestPrefetch(uint8_t* self, const uint8_t* list, const uint8_t* settings);                          // self-test only
uint8_t PerceptionLosTestCall(uint8_t* self, uint8_t* other, uint8_t* out, int tries, uint8_t flag);                 // self-test only
void PerceptionLosTestCounts(uint64_t* served, uint64_t* verified, uint64_t* diff, uint64_t* fallback, uint64_t* jobs);  // self-test only
// rig_import_template.cpp (RigImportTemplate, public since 1.3.0): the first bone import of a new rig is a copy of a stored
// identical import (equivalent); RemoveElement's DestroyElement call guarded so a copied block is never freed under live elements
bool InstallRigImportTemplate(bool beforeGameCode);  // start-up only; the guard first
void SetRigImportTemplate(int mode);                 // live, once installed: 0 off, 1 on (lab: 2 verify); stays off after the cap
void LogRigImportTemplateStats();                    // one line when a counter changed
bool RigImportTemplateDiscover(const GameModule& game, const char** missing, const char** detail);  // self-test: patterns, checks, offsets
struct RigTmplTestCounts {
    long long removesOnCopied = 0, blocksKept = 0, elemsKept = 0, stockOwnerRemoves = 0, copies = 0, marksFull = 0;
    int mode = 0;
    size_t marks = 0;
};
void RigTmplTestBegin(void* fakeDestroy, void* fakeCopy, size_t maxMarks);  // self-test only: fresh state, fakes for the game
void RigTmplTestEnd();                                                      // self-test only
void RigTmplTestDestroy(uint8_t* hier, uint8_t** pElem);  // self-test only: the guard (RemoveElement's DestroyElement call)
bool RigTmplTestCopy(uint8_t* hier, uint8_t* tmpl);       // self-test only: mark + copy (false: table full, no copy)
void RigTmplTestNewImport(const uint8_t* hier);           // self-test only: Altar's first import on an empty hierarchy (forget its mark)
RigTmplTestCounts RigTmplTestGet();                       // self-test only
// npcs/far_pawn_move.cpp (FarPawnMoveHalfRate / FarPawnMoveHz / FarHorseMove, public since 1.3.0): calm far pawns' and horses'
// movement tick at FarPawnMoveHz with the skipped frames' steps summed (not identical: small steps far away)
struct FarPawnStep {
    float acc = 0.f;   // carried time (s)
    float thr = -1.f;  // the next threshold of carried time; -1 = not qualifying last time
    int n = 0;         // frames skipped since the last tick
};
// One movement tick's decision (pure): true = tick now with *step; false = skip this frame (its time carried).
bool FarPawnDecide(FarPawnStep& s, float dt, float period, float maxStep, bool qualifies, bool rootMotion, float phase, float* step);
struct FarPawnMoveDiscovery {
    void** pawnVtable = nullptr;   // VPairedPawnMovementComponent's
    void** horseVtable = nullptr;  // VHorseMovementComponent's (nullptr: horses not covered)
    uint8_t* tick = nullptr;       // UCharacterMovementComponent::TickComponent (slot 127 of both)
    uint8_t* getProc = nullptr;    // Altar's GetCurrentProcedureType(pawn)
    uint8_t* isRootMotion = nullptr;  // ACharacter::IsPlayingRootMotion
    uint8_t* getAttachParent = nullptr;  // USceneComponent's execGetAttachParent (pins AttachParent's offset; FP-S1)
    const char* missing = nullptr;
};
bool FarPawnMoveDiscover(const GameModule& game, uint32_t rootOffset, FarPawnMoveDiscovery* out);  // self-test (no writes)
bool InstallFarPawnMove(bool beforeGameCode, int mode, int hz, int horseMode, int minDistance, int maxStepMs);  // start-up only (needs pacing)
void SetFarPawnMove(int mode, int hz, int horseMode);  // live, once installed
void LogFarPawnMoveStats();                            // one line when anything qualified
struct FarPawnMoveTestFakes {
    uint32_t rootOffset, locationOffset;
    void* getProc;       // uint8_t (const void* pawn)
    void* isRootMotion;  // bool (const void* character)
    bool (*playerPos)(double*, double*, double*);
    void* origTick;      // void (void* comp, float dt, int tickType, void* tf)
};
void FarPawnMoveTestSetup(const FarPawnMoveTestFakes& f);       // self-test only
void FarPawnMoveTestTick(void* comp, float dt, bool horse);      // self-test only: the hook's path
// npcs/create_ahead.cpp (CreateAhead, aheadport 2026-11-16): the SpawnActor half of character creates built ahead in frames
// with frame-cap slack and adopted at the spawner's SpawnActor call (equivalent)
struct CreateAheadSites {
    uint8_t *spawnCall, *spawnFn, *finishCall, *finishFn, *destroyFn, *destroyThunk, *walk, *pairedScale;
    uint8_t *preSpawnCall, *spawnedCall, *broadcastFn;  // UWorld::SpawnActor's two broadcasts and their target
    uint8_t *destroyedCall, *removedCall;               // UWorld::DestroyActor's two broadcasts (same target)
    uint32_t destroySlot, spawnedDelegate, preSpawnDelegate, destroyedDelegate, removedDelegate;
    uint32_t creationTimeOff, worldTimeOff;             // AActor::CreationTime, UWorld::TimeSeconds (PostSpawnInitialize's store)
    uint8_t* pawnBox;                                   // the pawn index's box of a pawn (its inlined GetActorLocation)
    uint32_t rootOff, locOff;                           // AActor::RootComponent, the root's ComponentToWorld translation
};
bool CreateAheadDiscover(const GameModule& game, CreateAheadSites* out, const char** missing);  // self-test (no writes)
bool InstallCreateAhead(bool beforeGameCode);       // start-up only (needs NpcCreationPacing, CreatePreload)
void SetCreateAhead(int mode, int slackMs);         // live: 0 off (held actors destroyed), 1 on
void SetCreateAheadPool(int pool);                  // live: CreateAheadPool (0 = 12 held, no eviction; 12-32 = cap + idle-spare eviction)
void SetCreateAheadLevelled(int levelled);          // live: CreateAheadLevelled (1 = run-time references in the cells ahead built ahead)
void CreateAheadCell(const uint8_t* cell);          // row_preload.cpp, old-engine thread: a cell loaded ahead
void CreateAheadCreateBegin(uint32_t ref, uint32_t base);  // npc_pacing.cpp CreateExecHook, before the game's create
bool CreateAheadCreateEnd();                        // ... after it; true = the create took an actor built ahead
void CreateAheadFrame(uint64_t sleepUs, bool busy, bool travel);  // npc_pacing.cpp frame-end hook, after the cap's wait
void CreateAheadDropAll(int why);                   // game thread: 0 a save starts, 1 a level change
void LogCreateAheadStats();                         // one line when something changed
void* CreatePreloadClassFor(uint32_t form, bool request);  // create_preload.cpp, GT: a base form's actor class, null while not resident
// streaming/cell_preload.cpp (CellObjectPreload, preload 2026-11-18, S-008): the objects of cells RowPreload loads ahead get
// their form asset and actor class packages requested in the background (create_preload.cpp's readers and request)
bool CreatePreloadAheadDiscover(const GameModule& game, const char** missing);  // StaticLoadObject's still-loading flag test
int CreatePreloadAheadStep(uint32_t form, uint32_t ask, uint32_t* requested);   // GT (ask bit 0 form, 1 class): -1 not ready, 0 nothing, 1 form loading, 2 class loading, 3 loaded
bool CreatePreloadInstalled();                                                  // create_preload.cpp
bool RowPreloadInstalled();                                                     // row_preload.cpp
bool CellRefWalkFound(const GameModule& game, const char** missing);            // create_ahead.cpp: the cell reference walk's layout
bool CellObjectPreloadDiscover(const GameModule& game, const char** missing);   // self-test (no writes)
bool InstallCellObjectPreload(bool beforeGameCode, bool createPreloadOn, bool rowPreloadOn);  // start-up only
void SetCellObjectPreload(int mode);                // live: 0 off (queues emptied), 1 on
void CellObjectPreloadCell(const uint8_t* cell);    // row_preload.cpp, old-engine thread: a cell loaded ahead
void CellObjectPreloadFrame(bool travel);           // npc_pacing.cpp frame-end hook, game thread
void LogCellObjectPreloadStats();                   // one line when something changed
struct CellObjectPreloadTestCounts { int queued, looks, formReqs, classReqs, gaveUp, resident, pending, todo, otQueue, walkFaults, statSkip; };
void CellObjectPreloadTestSetup(int (*step)(uint32_t, uint32_t, uint32_t*), uint64_t (*now)());  // self-test (null = clear)
CellObjectPreloadTestCounts CellObjectPreloadTestRead();
struct CreateAheadFakes {  // self-test: the game's functions and objects replaced
    uint8_t* (*spawn)(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t* params);
    void* (*classOf)(uint32_t base, bool request);
    uint8_t* objArray;      // a fake GUObjectArray
    size_t destroySlot;     // the fake actors' vtable slot
    const double* clockS;   // seconds
    const int* freeze;      // != 0: a menu / loading screen
    const double* player;   // x, y, z (UE units)
    void (*broadcast)(void* delegate, uint8_t* actor);  // the two broadcasts at an adoption (world + 1, world + 2)
    uint32_t creationTimeOff, worldTimeOff;             // in the fake actor / world (0: not stamped)
    const int* purge;                                   // != 0: a garbage collection's purge is pending (null: never)
    uint32_t rootOff, locOff;                           // in the fake actor / its root (0: the announced place is not set)
};
void CreateAheadTestSetup(const CreateAheadFakes* f);  // self-test only (nullptr = clear)
uint8_t* CreateAheadTestSpawnCall(uint8_t* world, uint8_t* cls, const uint8_t* xf, const uint8_t* params);  // the redirected call
int CreateAheadTestParams(const uint8_t* tmpl, const uint8_t* p);   // -1 template not usable, 0 equal, else offset + 1
bool CreateAheadTestScale(const double* s, double built);           // the scale rule
struct CreateAheadTestCounts { int held, built, adopted, spareAdopted, rejected, rejScale, rejPlace, dropped, gone, queued, heldBack, destroyQuiet, mode, idleEvicted, runtimeNoted, runtimeTook, runtimeLate, rejBase, runtimeNever, runtimeNeverBuilt, dropOff, rejClass, purgeWait, classRenewed, classLost, offsetSkipped; };
void CreateAheadTestBroadcast(void* delegate, uint8_t* actor);      // self-test: a redirected broadcast call
void CreateAheadTestFault();                                        // self-test: as a build's fault
CreateAheadTestCounts CreateAheadTestRead();                        // self-test only
// npcs/create_preload.cpp (CreatePreload / CreatePreloadNear, public since 1.3.0): a deferrable create waits while its form
// asset / class package loads asynchronously (equivalent)
bool InstallCreatePreload(bool beforeGameCode);  // start-up only (needs NpcCreationPacing)
void SetCreatePreload(int mode);                 // live: 0 off, 1 on
void SetCreatePreloadNear(int frames);           // live: 0..30
void SetCreatePreloadEarly(bool on);             // CreatePreloadEarly (live; drainfix 2026-11-12)
void CreatePreloadOnDefer(const void* cmd);      // npc_pacing.cpp, GT: a far create deferred for the budget (requests its loads)
int CreatePreloadTestFake(int waits);            // self-test (fakes): next n deferrable creates wait; returns OnDefer calls seen
bool CreatePreloadBefore(const void* cmd, bool mayWait, uint64_t lag, uint64_t maxLag, bool nearCreate);  // npc_pacing.cpp, GT
bool CreatePreloadNearMayWait(double x, double y, double z);  // npc_pacing.cpp, GT: a near create's spot is out of view
void LogCreatePreloadStats();                                 // one line when it waited or requested
bool CreatePreloadDiscover(const GameModule& game, const uint8_t* createExec, const char** missing);  // self-test (no writes)
const void* CreatePreloadTestCached(const void* holder, uint32_t form);                 // self-test: map 1 reader
int CreatePreloadTestPath(const void* holder, uint32_t form, wchar_t* buf, int cap);    // self-test: map 2 reader (a copy)
int CreatePreloadTestGetter(const uint8_t* fn);                                         // self-test: a class path getter's offset
int CreatePreloadTestHeads(const void* cmd, uint64_t frame, bool deferAgain);           // self-test: frames waited (null = clear)
struct SoftLoadApi {  // FSoftObjectPath::ResolveObject, ::ToString, FMemory::Free, LoadPackageAsync(FString) (create_preload.cpp's)
    uint8_t *resolve, *toString, *free, *loadAsync;
};
bool FindSoftLoadApi(const GameModule& game, SoftLoadApi* out);  // the same patterns as CreatePreload's; false = not found once
// loading/impact_preload.cpp (ImpactPreload, impactload 2026-11-12): the impact table's hit-effect classes (all surfaces) and the
// blood effect class are requested in the background as soon as the game's settings exist (at the main menu) and kept loaded,
// so the first hit of each surface no longer loads its effect synchronously (identical: the same classes, only loaded earlier)
bool InstallImpactPreload(bool beforeGameCode);  // start-up only (needs NpcCreationPacing's frame hook)
void SetImpactPreload(int mode);                 // live: 0 off, 1 preload, 2 log first uses only (lab watch)
void ImpactPreloadFrame();                       // npc_pacing.cpp, GT: every frame end
void LogImpactPreloadStats();                    // one line when something changed
struct ImpactPreloadSites {
    uint8_t *rowLookup, *classGetter, *pawnSparkLoad, *rowRead, *bloodRead;
    uint8_t **classGlobal;     // the settings class (StaticClass cache)
    uint8_t *rowList;          // TArray<row*> (data, num)
    int32_t *objNum;           // GUObjectArray NumElements
    uint8_t ***objChunks;      // GUObjectArray chunk table
    uint32_t cdo, table, rowClass, blood;  // 0x110, 0x240, 0x80, 0x78
};
bool ImpactPreloadDiscover(const GameModule& game, ImpactPreloadSites* out, const char** missing);  // self-test (no writes)
struct ImpactPreloadFakes {  // self-test: the game's functions and objects replaced
    const void* (*resolve)(const void* softPath);
    void (*request)(const void* softPath);
    void (*initRows)(void* arg);
    uint8_t** classGlobal;
    uint8_t* rowList;
    int32_t* objNum;
    uint8_t*** objChunks;
};
void ImpactPreloadTestSetup(const ImpactPreloadFakes* f);  // self-test only (nullptr = clear)
struct ImpactPreloadTestState { int state, requested, rooted, resident, rows, failed; };
ImpactPreloadTestState ImpactPreloadTestRead();           // self-test only
// streaming/row_preload.cpp (RowPreload, rowpreload 2026-11-12): the next row of exterior cells the game asks for is loaded
// ahead with the game's own cell load and parked in the cell buffer, one cell per calm old-engine frame (the 2006 game's
// background preload, which the remaster asks for but never runs); the crossing then only attaches them
bool InstallRowPreload(bool beforeGameCode);      // start-up only (needs CellBufferPacedEvict installed, NpcCreationPacing's frame hook)
void SetRowPreload(int mode);                     // live: 0 off (notes cleared), 1 on
void RowPreloadGtFrame();                         // npc_pacing.cpp, GT: every frame end (menu / loading freeze)
void RowPreloadGridCall(uint8_t* tes, bool shifted);                     // cell_buffer.cpp, OT: after every grid-change call
void RowPreloadOtStep(uint8_t* tes, bool busy, uint32_t tailFree);       // cell_buffer.cpp, OT: the per-frame call, no shift
uint32_t CellBufferTailFree(const uint8_t* tes);  // cell_buffer.cpp: free spare slots (0 when not installed / not made yet)
bool CellBufferPacedEvictInstalled();             // cell_buffer.cpp
void LogRowPreloadStats();                        // one line when something changed
struct RowPreloadSites {
    uint8_t *stub, *calls[3], *lookup, *isInterior, *inExtBuffer, *load, *insert, *loadingTest, *background, *tesSlot, *flagsSlot,
        *grids;
};
bool RowPreloadDiscover(const GameModule& game, RowPreloadSites* out, const char** missing);  // self-test (no writes; needs
                                                                                              // FindCellBufferPacedEvict first)
struct RowPreloadFakes {  // self-test: the game's functions and objects replaced
    void* stub;         // void(manager, world, x, y)
    void* lookup;       // uint8_t*(world, x, y)
    void* isInterior;   // uint8_t(cell)
    void* inExtBuffer;  // uint8_t(tes, cell)
    void* load;         // uint8_t*(world, x, y)
    void* insert;       // void(tes, cell)
    void* loadingTest;  // uint8_t(flags)
    const uint8_t* background;
    uint64_t* tesSlot;
    uint64_t* flagsSlot;
    const uint32_t* grids;
};
void RowPreloadTestSetup(const RowPreloadFakes* f);  // self-test only (nullptr = clear)
void RowPreloadTestGt(int state);                    // self-test only: 1 calm, 2 frozen, 0 unknown
void RowPreloadTestRequest(uint64_t manager, uint64_t world, int32_t x, int32_t y);  // self-test only: the redirected call
struct RowPreloadTestCounts {
    uint64_t requests, dupes, loaded, already, stale, noCell, attached, hits, unused, skipBusy, skipSettle, skipFrozen, skipTail,
        skipUnsafe, cleared;
    uint32_t pending;
};
RowPreloadTestCounts RowPreloadTestRead();  // self-test only
// npcs/ot_frame_trim.cpp (OtFrameTrim, public since 1.3.0): the OT frame's path-builder sleep wakes early (bit 1, equivalent)
// and the node profiler timer returns 0 (bit 2, identical by audit)
bool InstallOtFrameTrim(bool beforeGameCode, int mode);  // start-up only
void SetOtFrameTrim(int mode);                           // live 0..3, once installed
void LogOtFrameTrimStats();                              // one line when the frame-start sleep ran with bit 1
struct OtFrameTrimDiscovery {
    int consumers, requests, polls, lists, timers;  // pattern matches (1 each)
    uint8_t* consumer;      // exe+0x68D4090 frame-start deferred sleep
    uint8_t* timer;         // exe+0x694E170 QPC wrapper
    void** sleepSlot;       // the Sleep import slot
    void** pathObj;         // exe+0x948C200 PathBuilder object pointer
    int32_t* accumulator;   // exe+0x948D0A4 deferred sleep ms
};
bool OtFrameTrimDiscover(const GameModule& game, OtFrameTrimDiscovery* out);  // self-test (no writes)
bool OtFrameTrimTestInstall();                                                // self-test: install on Game()
void OtFrameTrimTestStats(uint64_t* sleeps, uint64_t* early, uint64_t* idle, uint64_t* full);
// npcs/loose_miss_cache.cpp (LooseMissCache, public since 1.3.0): the file finder's failed loose voice-file checks remembered
bool InstallLooseMissCache(bool beforeGameCode, int mode);  // start-up only
void SetLooseMissCache(int mode);                           // live 0/1, once installed
void LogLooseMissCacheStats();                              // one line when it answered from memory
struct LooseMissDiscovery {
    int finds;          // pattern matches (1)
    uint8_t* find;      // exe+0x68D0270 file finder's find
    void** slot;        // the _access import slot
};
bool LooseMissDiscover(const GameModule& game, LooseMissDiscovery* out);  // self-test (no writes)
bool LooseMissTestInstall();
bool LooseMissTestVoicePath(const char* path);  // the cached family (voice paths)
void LooseMissTestStats(uint64_t* cached, uint64_t* stored);
// npcs/power_attack_anim_keep.cpp (PowerAttackAnimKeep): the animation release leaves a power-attack entry in the map at count 0
bool InstallPowerAttackAnimKeep(bool beforeGameCode, int mode);  // start-up only; the lab build installs at 0 too
void SetPowerAttackAnimKeep(int mode);                            // live 0/1, once installed
void LogPowerAttackAnimKeepStats();                               // one line when files were left loaded
struct PowerAttackAnimKeepDiscovery {
    int releases, purges;  // pattern matches (1 each)
    uint8_t* site;         // exe+0x67EC8E3: the release's re-read of the count (8 bytes)
    uint8_t* purge;        // exe+0x67E74E6: the loading purge's rule for a count-0 entry
    uint8_t* getter;       // exe+0x67E2580: the group of a sequence
    uint8_t* destroy;      // exe+0x67E51F0: an entry's destruction
};
bool PowerAttackAnimKeepDiscover(const GameModule& game, PowerAttackAnimKeepDiscovery* out);  // self-test (no writes)
bool PowerAttackAnimKeepTestInstall();
uint32_t PowerAttackAnimKeepTestCount(const uint8_t* entry);  // the hook, called directly
uint64_t PowerAttackAnimKeepKept();
// streaming/flow_overlaps_once.cpp (FlowOverlapsOnce): overlap updates inside a pre-placed actor's adoption run once at its end
bool InstallFlowOverlapsOnce(bool beforeGameCode, int mode);  // start-up only; the lab build installs at 0 too
void SetFlowOverlapsOnce(int mode);                            // live 0/1, once installed
void LogFlowOverlapsOnceStats();                               // one line when adoptions batched since the last one
struct FlowOverlapsDiscovery {
    int adopts, changes, begins;  // pattern matches (1 each)
    uint8_t* adopt;               // exe+0x472EFB0: the level flow's pre-placed actor adoption
    uint8_t* site;                // exe+0x358850F: OnComponentCollisionSettingsChanged's call of UpdateOverlaps
    uint8_t* update;              // exe+0x3092010: USceneComponent::UpdateOverlaps
    uint32_t overlapList;         // UPrimitiveComponent::OverlappingComponents
};
using FlowOverlapsUpdateFn = uint64_t (*)(void* comp, const void* pending, uint64_t doNotifies, const void* atEnd);
using FlowOverlapsAdoptFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
bool FlowOverlapsDiscover(const GameModule& game, FlowOverlapsDiscovery* out);  // self-test (no writes)
bool FlowOverlapsTestInstall();
void FlowOverlapsTestSetFakes(FlowOverlapsAdoptFn adopt, FlowOverlapsUpdateFn update);  // self-test: fake adoption / update
uint64_t FlowOverlapsTestAdopt(uint64_t a1);    // the adoption hook, called directly
uint64_t FlowOverlapsTestRequest(void* comp);   // the patched call, called directly (UpdateOverlaps(comp, nullptr, true, nullptr))
void FlowOverlapsTestCounts(uint64_t* deferred, uint64_t* inPlace, uint64_t* flushed, uint64_t* skipped);
uint32_t FlowOverlapsListOffset();
const uint8_t* FlowOverlapsTestTrampoline();  // self-test: the adoption's first 6 bytes + jmp [rip] back to +6
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
// LumenGroupsPrior (part of LumenKeepArrayCapacity): PrimitiveGroups grows go straight to this many entries once a scene
// holds 1/32 of it, while Windows' commit room (available + page file growth, the memory monitor's last sample) stays at
// least minCommitRoomMB above the block; 0 entries = fix 12's doubling alone. Live.
void SetLumenGroupsPrior(int64_t entries, int minCommitRoomMB);
// Self-test only: pretend commit room and available commit (MB; free -1 = same as room); room -1 = the monitor's sample,
// -2 = not known. And set the grow policy's session peak (entries).
void LumenPriorTestCommitRoom(int64_t roomMB, int64_t freeMB);
void LumenGrowTestSetPeak(int32_t entries);
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
// ---- gpu_pool_keep.cpp (GpuPoolKeepMB): empty D3D12 texture and buffer pools kept up to a budget ----
bool InstallGpuPoolKeep(bool beforeGameCode);  // the 20-frame CleanUpAllocations call sites (start-up only)
void SetGpuPoolKeep(int mb);                    // live budget; 0 = give kept pools back (one per allocator per frame)
void LogGpuPoolKeepStats();
// the budget to apply now: 0 under MemoryRelief or while video memory + the budget is over the DXGI budget (512 MB hysteresis)
int GpuPoolKeepBudgetNow(int mb, bool relief);  // monitor thread, once a second (LatestGameVram)
int GpuPoolKeepBudgetStep(int mb, bool relief, uint64_t vram, uint64_t vramBudget);  // the same step on given values (self-test)
bool GpuPoolKeepTestDefrag(const GameModule& game, uint32_t* entry, uint32_t* step);  // self-test only: Defrag's +24h / +60h checks
bool GpuPoolKeepTestLockImport(const GameModule& game, uint32_t fnRva);  // self-test only: the lock's import is EnterCriticalSection
uint64_t GpuPoolKeepTestCursorMoves();         // self-test only
bool GpuPoolKeepTestFailed();                  // self-test only: the frame-fence read failed (key off)
int GpuPoolKeepTestDiscover(const GameModule& game, uint32_t* fn, uint32_t* sites, int max);  // self-test only
uint64_t GpuPoolKeepTestDecide(const uint64_t* ages, const uint64_t* sizes, int n, uint64_t avail, uint64_t* keptBytes,
                               int* keptCount);  // self-test only: the frame lag passed to the game's clean-up
void GpuPoolKeepTestCall(void* allocator, uint64_t lag, bool force, void* fakeOrig);  // self-test only
void GpuPoolKeepTestReset();                    // self-test only
uint64_t GpuPoolKeepTestKept();                 // self-test only
uint64_t GpuPoolKeepTestReused();               // self-test only
bool GpuPoolKeepTestInstalled();                // self-test only
void SetAllocatorTrimCopyFloor(int floor);            // copy pool kept at >= floor outside over-budget mode; 0 = off; live
void SetImmediateAllocatorLists(int lists);           // part 2, switchable live; 0/1 = off
void SetAllocatorTrimPressure(bool pressure, bool tight);  // over budget -> copy pool, short window; tight -> copy pool
void AllocatorTrimTick();                             // monitor thread, once a second
void LogAllocatorTrimStats();
bool AllocatorTrimInfo(AllocatorTrimInfoOut* out);
void* AllocatorTrimTestStub();                        // the stand-in allocator object (self-test; CmdMemPregrow never records on it)
// CmdMemPregrow (memory/cmd_pregrow.cpp): the direct pool brought to its working size with its command memory, before the first world
void SetCmdMemPregrow(int requested, bool poolsFound, bool beforeGameCode, int giveBackHeadroomMB, const wchar_t* iniPath);  // start-up: 0 off, 1 automatic size, else the size
void CmdMemPregrowTick(void* device, uint64_t deviceAgeMs);  // allocator_trim.cpp, once a second
void CmdMemPregrowFrame();  // game thread, once per frame (NpcCreationPacing's frame hook): sees the first loading screen for the pass
const void* NpcPacingMaxTickHookCode();  // npc_pacing.cpp: MaxTickRateHook (CmdMemPregrow self-test)
void CmdMemGiveBackTick(void* device);   // allocator_trim.cpp, once a second, on the trim's own thread: the one trim of the direct pool
struct CmdMemTrimOut {
    int swapped = 0, freeReal = 0;
    double releaseMs = 0, longestOneMs = 0;
};
int CmdMemTrimDirect(void* device, int64_t keepFree, int maxSwap, CmdMemTrimOut* out);  // allocator_trim.cpp: CommandAllocatorTrim's pass on the direct pool
void CmdMemDirectStats(int64_t* real, int64_t* inFlight, int64_t* peakEver);  // allocator_trim.cpp
bool AllocatorTrimFailed();              // allocator_trim.cpp: an exception in a trim pass: the pools are not to be touched again
const char* CmdPregrowSelfTest();                     // self-test of the pool handling and the gates (nullptr = ok)
void* CmdMemPopDirect(void* device);     // allocator_trim.cpp: a free direct allocator wrapper off the pool, nullptr if none
void* CmdMemObtainDirect(void* device);  // allocator_trim.cpp: the engine's obtain, uncounted
void CmdMemReleaseDirect(void* device, void* wrapper);  // allocator_trim.cpp: the engine's release (Reset, back to the pool), uncounted
void* CmdMemD3dOf(void* wrapper);        // allocator_trim.cpp: the wrapper's D3D12 allocator
int64_t CmdMemDirectCreated();           // allocator_trim.cpp: direct allocators the engine has made
uint64_t CmdMemLastObtainMs();           // allocator_trim.cpp: GetTickCount64 of the last obtain of any pool
uint64_t CmdMemDeviceAgeMs();            // allocator_trim.cpp
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
// creature_anim_preload.cpp (CreatureAnimPreload: a returning creature's missing animation files load in the background)
bool FindCreatureAnimPreload();                    // locate the creature queue step's flag byte (no patching); false = unavailable (logged)
void SetCreatureAnimPreload(bool enabled);         // one byte, switchable live
uint8_t* CreatureAnimPreloadSite();                // the flag byte (self-test)
uint8_t* CreatureAnimPreloadLocate(const GameModule& game, const char** why);  // locate only, any image (self-test fakes)

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
    uint32_t hairActorWeak;  // PacingHairActor: the hair delegate's mesh-component weak pointer in its instance (0 = not found)
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
    uint32_t hairActorWeak;  // PacingHairActor: the fake hair instance's component weak pointer (0 = keep the found one)
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
bool InstallFadeInUnseen(bool beforeGameCode);  // FadeInUnseen: start-up only (one rel32 in the fade tick's caller); needs pacing installed
void SetFadeInUnseen(int mode, int windowMs);  // live (public 0/1; lab 2 = any distance, window)
bool FadeInUnseenInfo(uint32_t* jmpRva, uint32_t* bodyRva, uint32_t* list, uint32_t* compLast, uint32_t* staticClassRva);  // self-test
void FadeInUnseenTestSlack(double seconds);  // self-test: the last two frames' time (< 0: the frame clock)
int FadeInUnseenTestTick(uint8_t* comp, int mode, double (*now)(), const uint8_t* (*staticMesh)());  // self-test: the after-tick step
bool NpcPacingFadeInfo(uint32_t offsets[5], void** staticClass);  // self-test: state, value, owner, actor's component, floor
void NpcPacingTestFade(const uint8_t* (*staticClass)());           // self-test: a fake owner class
void NpcPacingTestRendered(bool (*rendered)(const void* actor));   // self-test: fake "recently rendered" (nullptr = fakes never are)
void NpcPacingTestFadeValue(float (*fade)(const void* actor));   // self-test: fake spawn fade-in dither (nullptr = the real one)
void NpcPacingTestGameValue(float (*value)(const void* actor));  // self-test: fake own fade value of the due rule (nullptr = real)
void SetNpcPacing(bool enabled, int budgetMs, int minDistance, int maxLagFrames, bool useSlack, bool flowWait = true,
                  int flowWaitFrames = 8);  // switchable live
void SetNpcPacingOwnerWalk(bool on);  // PacingOwnerWalk (live; mkusm, PR #25)
void SetNpcPacingHairActor(bool on);  // PacingHairActor (live; npcunits 2026-11-12)
void SetNpcPacingLeanFlowDrain(bool on);  // PacingLeanFlowDrain (live)
void SetNpcPacingFlowProgress(bool on);   // PacingFlowProgress (live; drainfix 2026-11-12)
float NpcPacingTestFadeHold(const void* actor, float* value, float floor, float rate, float dt);  // self-test (gate2 fade hold)
void SetNpcPacingGateBounds(int boundFrames, int calmMs, int nearM, int fadeHoldMs);  // PacingGateBoundFrames / CalmMs / NearM, lab FadeHoldMs (live; gate2)
void SetNpcPacingBaseGate(int value);     // PacingBaseGate (live; basegate 2026-11-12): 0 off, 1 = 25 ms, 18-50 = the target in ms
// PacingGateShownHoldMs (hold2 2026-11-13, npcs/shown_hold.cpp; public 1.5.0): the fade applier's entry hook
bool NpcGateHoldsActor(uintptr_t actor);  // game thread: the actor has held appearance / equipment units (gate's list, still queued)
bool NpcGateActorQueued(uintptr_t actor);  // game thread: a unit of the actor is still queued
bool NpcGateActorGated(uintptr_t actor);   // game thread: a unit of the actor that the gate holds is still queued (build97)
float ShownHoldEndValue();                 // PacingGateShownHoldEnd as a fade value
void ShownHoldCountDue(bool now);          // the gate ran a due unit (now / soft)
void ShownHoldCountDueReplay(int n);       // holdfix F2: due units run in one gate replay (the most is logged)
void ShownHoldNoteEquipFirst(uintptr_t owner);  // holdfix F4: equipment ran while the appearance is still queued
// holdfix F2: the deadline spread of the gate's due units (pure, npc_pacing.cpp): t[i] = the seconds until unit i is due
// now (its character's fade value reaches PacingGateShownHoldEnd), periodS the frame period, softS the soft window (s):
// due[i] = 1 when it must start running one a frame now so that one unit a frame clears all of them in time
// returns the soft-due units a frame needed (1 unless one a frame cannot meet the deadlines; at most 4; port102)
int ShownHoldDueSpread(const float* t, size_t n, float periodS, float softS, uint8_t* due);
bool NpcPacingFadeLayout(uint32_t out[5]);  // holdfix F8: pacing's fade layout (state, value, owner, floor, actor's comp)
bool InstallShownHold(bool beforeGameCode);  // start-up only
void SetShownHold(int ms, int endTenths);  // live: 0 off, else the hold's bound in ms; PacingGateShownHoldEnd
bool ShownHoldOn();
float ShownHoldLevel(const uint8_t* comp);  // game thread: the dither held on this fade component's meshes (0: none)
float ShownHoldSpeed(const uint8_t* comp);  // game thread: how much faster than the game's fade its shown level falls (1: as the game's)
// game thread (holdfix F4; fix102): live open holds whose equipment ran before the appearance, with the seconds left to
// their deadline (0.1 s after that unit; 0 = past it)
int ShownHoldUrgentOwners(uintptr_t* out, float* leftS, int max);
void LogShownHoldStats();
bool ShownHoldFind(uint32_t out[8]);      // self-test: applier RVA, state, value, rate, max, floor, owner, actor's component
void ShownHoldTestSetup(bool (*held)(uintptr_t), uint64_t (*now)(), int gt, int ms, int endTenths);  // self-test
float ShownHoldTestCall(uint8_t* comp, uint8_t* owner, float scale);  // self-test: what the applier would be handed
void ShownHoldTestCounts(uint64_t out[6]);  // self-test: opened, late, released by units / bound / clock, shown below
void ShownHoldTestQueued(bool (*queued)(uintptr_t));  // self-test: the actor still has queued units (AI-early count)
uint64_t ShownHoldTestAiEarly();          // self-test: fades that ended with the actor's units still queued
void ShownHoldTestDueCounts(uint64_t out[2]);  // self-test: due units run soft / now (zeroed)
int ShownHoldTestHold(uintptr_t comp, uintptr_t owner, bool touch);  // self-test (fix102): open / keep live a hold; slots held
float NpcPacingTestGatePeriodS();          // self-test (fix102 S2): the due spread's frame period now
void NpcPacingTestFrameEdge();            // self-test only: the frame-rate cap's sleep point (frame end / start) with no sleep
void LogNpcPacingStats();
bool NpcPacingInfo(NpcPacingInfoOut* out);
void NpcPacingTestSetup(const NpcPacingTestFakes& fakes);  // self-test only
NpcPacingTestState NpcPacingTestRead();                    // self-test only
bool NpcPacingPlayerPos(double* x, double* y, double* z);  // game thread: the player's position (UE units), if known
bool NpcPacingOnGameThread();                              // this is NpcCreationPacing's game thread (known after its first frame)
bool NpcPacingLastPlayerPos(double* x, double* y, double* z);  // any thread: the last one the game thread saw (crash report)
// ClothBuildDefer (game thread): inside a unit of a far, unseen actor in a frame that defers: true, that unit's owner
// (its delegate's weak object pointer) and the pacing frame; and later, whether that owner is still far and unseen. Both
// walk past an owner chain's non-actors (an equipment unit's body pairing component), which pacing's own walk does not
bool NpcPacingDistantUnit(uint64_t* ownerWeak, uint64_t* frame);
bool NpcPacingStillDistant(uint64_t ownerWeak);
// NpcDeferAnimInit (deferanim): the running unit runs distant and BEFORE this frame's post-actor tick (frame-start replay
// or pairing run), on the game thread; *actorKey = its actor (0 = unknown)
bool NpcPacingEarlyDistantUnit(uintptr_t* actorKey);
// ---- NpcDeferAnimInit (defer_anim_init.cpp; experimental, lab builds only, as NpcPacingEarlyDistantUnit) ----
bool InstallNpcDeferAnimInit(bool beforeGameCode, int mode);  // start-up only; then live
void SetNpcDeferAnimInit(int mode);                           // live: 0 pass-through, 1 reference pose at init, 2 + deferred layer links
void NpcDeferAnimInitPostActorTick();                         // NpcCreationPacing's post-actor-tick handler (game thread)
void NpcDeferAnimInitFrameStart();                            // NpcCreationPacing's frame start (safety net, counted)
void LogNpcDeferAnimInitStats();                              // one line when a counter changed
bool NpcDeferAnimInitDiscover(const GameModule& game, const char** missing, uint32_t out[11]);  // self-test
void DeferAnimTestBegin(const uint32_t offsets[11], bool (*eligible)(uintptr_t*), void (*evaluate)(uint8_t*), bool (*handle)(uint8_t*),
                        void (*origInitAnim)(uint8_t*, uint64_t), void (*origOverlay)(uint8_t*, void*, void*, uint64_t));
void DeferAnimTestEnd();
void DeferAnimTestMode(int mode);
void DeferAnimTestInitAnim(uint8_t* comp, uint64_t force);
void DeferAnimTestOverlay(uint8_t* inst, uint64_t defer);
size_t DeferAnimTestPending();
void DeferAnimTestCounts(uint64_t out[6]);
void SetRigImportTemplateGameThreadOnly(bool on);  // rig_import_template.cpp: imports off the game thread go to the game (NpcDeferAnimInit 2)
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
bool ClothDeferTestBoundsSites(void** calcBounds, void** markTransform, void** clothVtable);  // self-test only
void ClothDeferTestSetBoundsFakes(void* calcBounds, void* markTransform);  // self-test only
void SetClothDeferBoundsLab(bool on);                                     // lab ClothDeferBounds (0 = the bounds refresh left out); live
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
uint64_t LedgerSyncUsNow();                        // game thread: the FFrameEndSync wait of the frame being recorded (~0 when not recording)
bool LedgerIsGameThread();                         // installed and on the game thread (its id from the first frame end)
const volatile uint32_t* FindGpuFrameTime();       // GGPUFrameTime (FPlatformTime cycles), nullptr if not found
bool LedgerFrameStats(uint32_t* frames, uint32_t* maxUs);  // game-thread frames and the longest since the last call (monitor thread)
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
// fade_vt_flush.cpp (FadeVtFlushSkip, off by default): the r.VT.Flush the game runs 1.5 s into a door's fade-in.
struct FadeVtFlushInfo {
    uint8_t* flush = nullptr;
    uint8_t* exec = nullptr;
    uint8_t* execIfSafe = nullptr;
    void** execSlot = nullptr;
    void** execIfSafeSlot = nullptr;
};
bool FindFadeVtFlush(FadeVtFlushInfo* out);        // locate only (no patching); false = unavailable (logged)
bool InstallFadeVtFlushSkip();                     // start-up only (two .rdata slots)
void SetFadeVtFlushSkip(bool on);                  // monitor thread, live
void LogFadeVtFlushStats();
void FadeVtFlushTestSetup(void** execSlot, void** execIfSafeSlot, void* origExec, void* origExecIfSafe, bool installed);  // self-test
void FadeVtFlushTestCall(void* delegateInstance, bool ifSafe);                                                         // self-test
uint32_t FadeVtFlushSkippedForTest();                                                                                  // self-test
// door_diag.cpp (diagnostic DoorDiag): door / loading-screen timeline, stream-all timing, render-target pool
// misses and flushes, and the DoorDiagEarlyStream experiment.
struct DoorDiagInfoOut {
    uint8_t* pool = nullptr;
    int32_t* flushFlag = nullptr;      // r.bFlushRenderTargetsOnWorldCleanup's int
    uint8_t* freeAll = nullptr;
    uint8_t* findFree = nullptr;
    uint8_t* elemSize = nullptr;
    uint8_t* streamAll = nullptr;
    void** streamAllSlot = nullptr;
};
bool FindDoorDiag(DoorDiagInfoOut* out);           // locate only (no patching); false = unavailable (logged)
bool InstallDoorDiag(bool beforeGameCode);         // start-up only
void SetDoorDiagEarlyStream(bool on);              // monitor thread, live
void DoorDiagFrameEnd();                           // game thread, once per frame (NpcCreationPacing's frame hook)
void LogDoorDiagStats();
// stream_behind_loading.cpp (StreamBehindLoadingScreen): a new area's textures stream while its loading screen is still up
// (the stored pool size set back + the streaming manager's non-blocking update, once the gate is armed and loading is idle).
struct StreamBehindLoadingInfo {
    uint8_t* gateBody = nullptr;
    uint8_t* arm = nullptr;
    void* platformGet = nullptr;  // IPlatformFeaturesModule::Get, from the settings save's writer (save loads: their save read)
    void** cvarStatic = nullptr;
    void** singleton = nullptr;
    void** asyncLoading = nullptr;
    uint8_t* travelPending = nullptr;
    uint8_t* update = nullptr;
    uint32_t poolOffset = 0, getWorldSlot = 0;
    void* armHook = nullptr;
    // the world drawing for UnfreezeBehindLoadingScreen (optional: nullptr / 0 when not found)
    void** execSlot = nullptr;
    void** execIfSafeSlot = nullptr;
    uint8_t* gameViewport = nullptr;
    uint32_t drawFlagOffset = 0;
    uint8_t* setLoadingFreeze = nullptr;  // prototype UnfreezeBehindLoadingScreen
};
bool FindStreamBehindLoading(StreamBehindLoadingInfo* out);  // locate only; false = unavailable (logged)
bool InstallStreamBehindLoading(bool beforeGameCode, bool tradeOffs);  // one entry detour (the gate-arming function); needs the fade hook
                                                            // found; tradeOffs: also UnfreezeBehindLoadingScreen's redirections
void SetStreamBehindLoading(bool on);                       // monitor thread, live
void SetUnfreezeBehindLoading(int ms);                      // monitor thread, live (0 = off); prototype, trade-off
void SetUnfreezeBehindLoadingFastTravel(int ms);            // ... its hold for fast travels (0 = UnfreezeBehindLoadingScreen's); live
void SetDrawBehindLoading(int mode);                        // DrawBehindLoadingScreen (0 = off, 2..10 frames); monitor thread, live
bool DrawBehindLoadingSelfTest(char* err, size_t n);         // self-test on a fake viewport field and transition (stream_behind_loading.cpp)
void StreamBehindLoadingFrameEnd();                         // game thread, once per frame (NpcCreationPacing's frame hook)
void LogStreamBehindLoadingStats();
// an exec function (UFunction thunk) by its registered name, checked against a byte pattern (stream_behind_loading.cpp);
// nullptr unless the name, its native-table pair and the match are each exactly one
uint8_t* FindExecByName(const char* name, const char* shape);
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
    bool shared = false;     // SharedMemoryGpu: the game's GPU uses system RAM (or the ini says to act so): every keep above as
                             // stock, also GpuSceneKeepCapacity and RtSceneBufferKeep, OsPageCache off, RdgParallelTranslate=2 off
};
void UpdateMemory(const Config& cfg, MemoryRelief* relief);
// SharedMemoryGpu: what the game's GPU is, asked once on the monitor thread after the game's device exists (the adapter on
// which the game uses the most video memory; D3D12CreateDevice on it returns the game's own device, a per-adapter
// singleton; D3D12_FEATURE_ARCHITECTURE1, else D3D12_FEATURE_ARCHITECTURE). Pending until asked.
enum class GpuMemoryKind { Pending, Unknown, Dedicated, Shared };
GpuMemoryKind GameGpuMemoryKind();
GpuMemoryKind QueryGpuMemoryKind(void* d3d12Device, bool* cacheCoherent);  // the feature query alone (self-test: a fake device)
// The PC around the GPU, asked once: running under Wine (Proton, CrossOver: ntdll exports wine_get_version) and the RAM in
// MB (installed, from the firmware's table, which counts a GPU carve-out; else what Windows sees; 0 = not known).
struct SharedGpuHost {
    bool wine = false;
    uint64_t ramMB = 0;
    bool ramInstalled = false;  // ramMB is the installed RAM (GetPhysicallyInstalledSystemMemory), not Windows' total
};
SharedGpuHost SharedGpuHostFacts();
constexpr uint64_t kSharedGpuMaxRamMB = 20 * 1024;  // automatic mode only under this: 16 GB devices, not 24 GB+
// SharedMemoryGpu's rule: 0 never, 1 always, 2 (automatic) when the GPU is UMA, not under Wine, RAM known and under 20 GB.
bool SharedMemoryGpuWanted(int mode, GpuMemoryKind kind, bool wine, uint64_t ramMB);
// Could the automatic rule still apply while the GPU's answer is pending (mode 2, not Wine, RAM under 20 GB)?
bool SharedMemoryGpuPossible(int mode, bool wine, uint64_t ramMB);
bool SharedMemoryGpuWaitForAnswer(int mode);  // live: possible and the answer still pending (RdgParallelTranslate=2 waits)
// CommandAllocatorTrim's "tight" level, one second: within headroom of the budget (512 MB hysteresis), always with headroom 0
// or a shared-memory GPU (there the budget is a share of system RAM and does not see the CPU's use of it); unknown budget: as before.
bool AllocatorTrimTightStep(bool tight, uint64_t vram, uint64_t budget, uint64_t headroom, bool shared);
void MemoryReliefTestSharedGpu(GpuMemoryKind kind, int mode, bool wine, uint64_t ramMB, MemoryRelief* relief);  // self-test: the relief a detected kind gives
void LogMemoryStats();
void* GpuCrashReportTestFind(const void* words, size_t count);  // self-test: the device found among words (AddRef'd)
// MemoryRelief's video-memory level (VramRelief): what one step (one second) did. Self-test: one step of a test instance,
// reset = start a fresh one; relief = VRAM overrides after the step (RAM/commit handled independently).
enum class VramReliefEvent { None, Engaged, Escalated, Helped, NotHelping, NoLongerHelping, Cleared, SwitchedOff, BudgetUnknown };
VramReliefEvent MemoryReliefTestVramStep(bool reset, uint64_t vramMB, uint64_t budgetMB, bool enabled, MemoryRelief* relief);
void MemoryReliefTestAllowTranslate(bool allow);  // self-test: VramReliefPauseTranslate for the test state machine
bool MemoryReliefTestCommitStep(uint64_t commitRoomMB);  // self-test: one second of RAM/commit relief (the live state); commit relief on?
bool GpuCrashReportTestDred(void* device);                      // self-test: the DRED part of the report
// diagnostics/gpu_markers.cpp ([General] GpuPassMarkers): render pass names for DRED (bit 1) and AMD's markers (bit 2)
struct GameModule;
bool InstallGpuPassMarkers(int mode, const wchar_t* ctlPath);  // start-up (either route) when the key is not 0; ctlPath: lab control file
void GpuPassMarkersTick(int mode, bool deviceReady);           // monitor thread, once a second (live); deviceReady: the game's GPU in use
int GpuPassMarkersMode();                                       // the mode in effect (-1 = not installed / not applied yet)
void LogGpuPassMarkersStats();                                  // lab: pushes per second (with the statistics)
UINT GpuPassMarkersTestEncode(uint64_t* out, size_t words, const char* name, bool marker);  // self-test: the PIX3 blob
bool GpuPassMarkersTestDiscover(const GameModule& game, uint32_t* emitRva, uint32_t* pushRva, uint32_t* popRva);  // self-test
int GpuPassMarkersTestHooks(int mode, void* fakePush, void* fakePop, void* list, const char* name);  // self-test: the hooks
// memory_monitor.cpp: the GPU crash report's parts that the self-test feeds with fakes
void GpuCrashReportTestBreadcrumbs(const void* headNode1);  // D3D12_AUTO_BREADCRUMB_NODE1 chain -> the per-queue lines
void GpuCrashReportTestContext();                           // the context lines (uptime, position, keys, process VRAM, ring)
void SetGpuCrashIniPath(const wchar_t* path);               // the ini whose [Fixes]/[General] keys the report lists (start-up)
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
    void* calcBoundsOrig;  // the game's CalcBounds behind calcBoundsHook (ClothBuildDefer's bounds refresh)
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
// VtStreamingUploads : r.VT.MaxUploadsPerFrame.Streaming set to N (vt_streaming_uploads.cpp).
bool FindVtStreamingUploads();                     // the console variable's registration and value pointer (no patching)
void SetVtStreamingUploads(int uploads);           // once per second from the monitor; 0 = the game's value; live
void LogVtStreamingUploadsStats();
void VtStreamingUploadsTestSetup(int32_t* volatile* slot);  // self-test only: a fake value pointer slot
int32_t* volatile* VtStreamingUploadsSlotForTest();         // self-test only
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
// streaming/stream_granularity.cpp (StreamRegisterGranularity): smaller registration steps of streamed-in levels
int32_t* FindStreamRegisterGranularityVar();      // the cvar's bound int (also on the mapped exe)
uint8_t* FindAddToWorld(bool checkPrologue);      // UWorld::AddToWorld (the function of the int's only read)
bool InstallStreamRegisterGranularity(bool beforeGameCode, const uint8_t* probeAdd);  // start-up, key on: the AddToWorld hook
void StreamGranBeforeAdd(bool considerTimeLimit); // game thread, every AddToWorld call: sets the value it should see
void SetStreamRegisterGranularity(int value);     // monitor thread, every second: StreamRegisterGranularity (live)
void StreamGranLogStats();                        // stats interval (no-op unless active)
void StreamGranTestSetup(void* gran, void* limit);  // self-test: fake console variables
void SetStreamInBudget(int value);                // StreamInBudget (live; 0 off, 1 = slice 1 / async 3 ms, 2 = 2 / 3 ms)
void StreamBudgetTestSetup(void* slice, void* async);  // self-test: fake console variables
// npcs/nav_query_priority.cpp (NavQueryPriority): the navigation query task at normal priority
bool StreamGranHookActive();                     // the AddToWorld hook is in place (shared by StreamRegisterGranularity, StreamInBudget, NavQueryPriority)
void NavQueryPriorityOnGameThread();             // game thread, every AddToWorld call (StreamGranBeforeAdd)
void SetNavQueryPriority(int value);             // monitor thread, every second: the key (live; 1 on, else off)
bool NavQueryPriorityNameInGame();               // self-test: the name found once in .rdata and referenced from code
void NavQueryPriorityTestSetup(void* fakeCvar);  // self-test: a fake console variable
unsigned NavQueryPrioritySets();                 // self-test
// physics/physics_substep_cap.cpp (PhysicsSubstepCap): the physics settings' MaxSubstepDeltaTime 0.03 / MaxSubsteps 4
void PhysicsSubstepCapOnGameThread();            // game thread, every AddToWorld call (StreamGranBeforeAdd)
void PhysicsSubstepCapFindAtStart();             // start-up, key on: FName's constructor and GUObjectArray by pattern
void SetPhysicsSubstepCap(int value);            // monitor thread, every second: the key (live; 1 on, else off)
void PhysicsSubstepCapTestSetup(uint8_t* fakeArray, void* fakeFname, const wchar_t* fakeIni);  // self-test: fakes
unsigned PhysicsSubstepCapSets();                // self-test
bool PhysicsSubstepCapIniLines(const char* text);  // self-test: the Engine.ini check on a text
int NavQueryPriorityNameState();                 // self-test: 0 the name not checked yet, 1 found, 2 not (monitor thread checks it)
// physics/body_spread.cpp (BodyRemovalSpread): a streamed-out level's instance bodies terminated over frames
bool InstallBodyRemovalSpread(bool beforeGameCode, int key, int perFrame, int maxFrames, int min, int nearM, bool frameHooks,
                              const wchar_t* iniPath);  // start-up only (key != 0; lab: always); needs NpcCreationPacing's hooks
void SetBodyRemovalSpread(int key, int perFrame, int maxFrames, int min, int nearM);  // monitor thread, each second (live)
void LogBodyRemovalSpreadStats();                       // stats interval (nothing when nothing happened)
const uint8_t* BodyRemovalSpreadUnregStub();            // the stub RemoveFromWorld's unregister call was patched to (or nullptr)
// streaming/endplay_slice.cpp (StreamOutEndPlaySlice; Epic UE 5.6.0 dfeb9ecce7): a streamed-out level's EndPlay in batches
bool InstallStreamOutEndPlaySlice(bool beforeGameCode, int key);  // start-up only (key != 0; lab: always), after BodyRemovalSpread
void SetStreamOutEndPlaySlice(int key);                 // monitor thread (live once installed)
void LogStreamOutEndPlaySliceStats();                   // stats interval (nothing when no level was removed)
bool StreamOutEndPlaySliceHook(void* level, int32_t n); // the hook (self-test calls it with fakes)
bool StreamOutEndPlaySliceTestDiscover(const GameModule& game, uint32_t rva[4], uint32_t* dataOff, uint32_t* numOff);
void StreamOutEndPlaySliceTestSetup(void* next, void* route, uint32_t dataOff, uint32_t numOff, int gran);
void StreamOutEndPlaySliceTestAge(double ms);           // self-test: the pending level's last call that much earlier
// memory/gc_pass_after_busy.cpp (GcPassAfterBusy): a due timer GC pass waits through busy frames (engine's one-frame delay)
bool InstallGcPassAfterBusy(bool beforeGameCode, int key, bool sharedUse = false);  // start-up only (key != 0 or GcPrepAhead on; lab: always), then live
bool GcPassAfterBusyInstalled();                            // the timer-call hook is in place (GcPrepAhead runs from it)
bool GcTimerAsyncLoading();                                 // game thread: the engine's IsAsyncLoading (GcPrepAhead's gate)
int GcTimerBusy();                                          // game thread: GcPassAfterBusy's busy signals now (1 add, 2 remove, 4 units, 8 time)
void SetGcPassAfterBusy(int key, bool memoryShort);         // monitor thread, every second (MemoryRelief's RAM/commit signal)
void LogGcPassAfterBusyStats();                             // stats interval (nothing when no timer pass fell due)
uint8_t* FindIncrementalPurgePending(const GameModule& game);  // UE's IsIncrementalPurgePending (CreateAhead; no writes)
void GcPassAfterBusyNoteStream(int kind);                   // game thread: 0 a level's AddToWorld step, 1 its RemoveFromWorld step
void GcPassAfterBusyNoteAdd(int64_t qpcTicks);              // game thread: an AddToWorld call's own time (noted when it did work)
bool GcPassAfterBusyWatching();                             // installed (or under self-test): the AddToWorld hook times its calls
float GcPassAfterBusyInterval(void* engine, bool playersConnected);  // the hook (self-test calls it with fakes)
struct GcPassAfterBusyFakes {
    uint32_t slot, tslOff, flagOff;
    bool (*purgePending)();
    const double* deltaTime;
    const uint64_t* frameCounter;
    const int32_t* asyncVar;
    bool (**asyncFn)();
    uint64_t (*now)();
    bool (*frame)(uint64_t* elapsedUs, uint64_t* unitUs);
    bool (*menu)(int32_t* namedFreezes, int32_t* mode);
};
bool GcPassAfterBusyTestDiscover(const GameModule& game, uint32_t rva[7], uint32_t off[3]);  // self-test: on the mapped exe
bool GcPassAfterBusyTestFindOn(const GameModule& game);       // self-test: discovery on altered bytes
bool GcPassAfterBusyTestEntry(const uint8_t* entry);          // self-test: the entry check (game's prologue or a hook's jump)
const char* GcPassAfterBusyTestLayout(uint32_t full, uint32_t tsl, uint32_t slot, uint32_t flagCmp, uint32_t flagClear);  // nullptr = ok
bool GcPassAfterBusyTestCall(const uint8_t* site, const uint8_t* slot, uint8_t out[6]);  // self-test: the 6 bytes written
void GcPassAfterBusyTestSetup(const GcPassAfterBusyFakes& k);  // self-test: fakes, state and counters reset
void GcPassAfterBusyTestTeardown();                     // self-test: nothing installed, fakes cleared (after GcHoldRun)
void GcPassAfterBusyTestCounts(uint64_t out[16]);
// memory/gc_prep_ahead.cpp (GcPrepAhead): streamed-out levels prepared for the next GC pass in calm frames just before it
struct GcTimerFrame {
    float tsl;          // UEngine::TimeSinceLastPendingKillPurge, this frame's DeltaTime already added
    float interval;     // GetTimeBetweenGarbageCollectionPasses()
    bool purgePending;  // IsIncrementalPurgePending()
};
bool GcPrepAheadFindable();                                 // start-up: its code and the GC lock found (InstallGcPassAfterBusy's sharedUse)
bool InstallGcPrepAhead(bool beforeGameCode, int key);     // start-up only (key != 0; lab: always; needs GcPassAfterBusy's hook), then live
void SetGcPrepAhead(int key, int minAgeMs, int maxPerFrame, int budgetUs, int windowMs);  // monitor thread, every second
void LogGcPrepAheadStats();                                // stats interval (nothing when nothing happened)
bool GcPrepAheadActive();                                  // game thread: the hook should call the step
void GcPrepAheadFrame(const GcTimerFrame& f);              // game thread: the step (GcPassAfterBusy's hook, once per frame)
void GcPrepAheadNoteAdd(const void* level);                // game thread: AddToWorld's level (the "wanted again" counter)
struct GcPrepAheadFakes {
    int32_t* num;
    uint64_t** data;
    int32_t* prepared;
    void* (*get)(const void* weakPtr);
    void (*prep)(void* level);
    const int32_t* softCount;
    const uint8_t* lowMemory;
    void* const* engine;
    uint32_t worldList, worldNum, ctxWorld, inTickOff;
    uint8_t inTickMask;
    uint64_t (*now)();
    bool (*frame)(uint64_t* elapsedUs, uint64_t* unitUs);
    bool (*menu)(int32_t* namedFreezes, int32_t* mode);
    bool (*tryLock)();  // the engine's GC lock without blocking (true = held by us)
    void (*unlock)();
};
bool GcPrepAheadTestDiscover(const GameModule& game, uint32_t rva[16], uint32_t off[5], const char** why);  // self-test: mapped exe
bool GcPrepAheadTestTryLock(void* const* singleton, void (*release)(), bool unlockAfter);  // self-test: the real TryGCLock on a fake lock object
bool GcPrepAheadTestFindOn(const GameModule& game);        // self-test: discovery on altered bytes
void GcPrepAheadTestSetup(const GcPrepAheadFakes& k);      // self-test: fakes, state and counters reset (installed)
void GcPrepAheadTestTeardown();
void GcPrepAheadTestCounts(uint64_t out[32]);
bool GcPrepAheadTestRemove(uint64_t key);                  // self-test: remove-by-value on the fake list
// memory/gc_mark_priority.cpp (GcMarkPriority): a task worker runs its part of a GC pass's mark at least at normal priority
bool InstallGcMarkPriority(bool beforeGameCode, int key);  // start-up only (key != 0; lab: always), then live
void SetGcMarkPriority(int key);                           // monitor thread, every second: 0 off, 1 normal, 2 above normal
bool GcMarkPriorityInstalled();
void LogGcMarkPriorityStats();                             // stats interval (nothing when no pass ran)
bool GcMarkPriorityTestFind(const GameModule& game, uint32_t rva[5]);  // self-test: lea x2, thunk x2, ProcessAsync (RVAs)
const char* GcMarkPriorityTestPatch(const GameModule& game);  // self-test: find + patch a fake module (nullptr = done)
void* GcMarkPriorityTestWrapper(int i);                        // self-test: Wrap<i>
void GcMarkPriorityTestSetup(void* orig0, void* orig1, int key);  // self-test: fake thunks, the level, counters reset
void GcMarkPriorityTestCounts(uint64_t out[3]);                // self-test: parts run, raised, raise failed
// loading/startup_bar_finish.cpp (StartupBarFinish, loader106; S-058): the loading-bar finish loops end at their first step
bool InstallStartupBarFinish(bool beforeGameCode, int key);  // start-up only (key != 0; lab: always, timing the loops)
void SetStartupBarFinish(int key);                    // monitor thread (live once installed, from the next loop)
void LogStartupBarFinishStats();                      // stats interval (nothing when no loop ran)
void StartupBarFinishStep(uint64_t arg, uint8_t* data, uint64_t site);  // the hook (self-test calls it with a fake step)
uint64_t StartupBarFinishDtor(uint64_t menu, uint64_t flags);  // the destructor-call hook (spends key 1; self-test with a fake)
bool StartupBarFinishTestDiscover(const GameModule& game, uint32_t rva[5]);
void StartupBarFinishTestSetup(void* step, void* dtor, int mode, bool firstClosed);
bool StartupBarFinishTestFirstClosed();
// loading/topic_sort.cpp (TopicSort, gapfix; S-058): the dialogue-topic list sort (exe+0x684EC60) as a stable merge sort
bool InstallTopicSort(bool beforeGameCode, int key);  // start-up only (key != 0; lab: always, timing the sorts)
void SetTopicSort(int key);                          // monitor thread (live once installed)
void LogTopicSortStats();                            // stats interval (nothing when no sort ran)
void TopicSortEntry(void* list);                     // the entry's target (self-test calls it through the patched entry)
bool TopicSortTestDiscover(const GameModule& game, uint32_t rva[5], int32_t* listOffset);
bool TopicSortTestInstall(const GameModule& game, int mode);  // self-test: patches the mapped exe's entry
void TopicSortTestMode(int mode);
bool TopicSortTestSort(void* list, int (*cmp)(const char*, const char*));  // our sort alone on a fake list
// loading/plugin_prefetch.cpp (PluginPrefetch, gapfix; S-058): the active plugin files read once at start-up
void StartPluginPrefetch(int key);  // start-up (both routes); starts a thread that exits when done
void PluginPrefetchParse(const char* text, size_t len, std::vector<std::wstring>* out);  // Plugins.txt -> plain names
std::wstring PluginPrefetchDataDir(const std::wstring& exePath, const wchar_t* literal);   // "" when it does not fit
bool PluginPrefetchFindLiteral(const GameModule& game);  // the game's L"Dev/ObvData/Data/Plugins.txt" in .rdata
// loading/reset_loaders.cpp (ResetLoadersNoFlush; Epic UE 5.4.0 0d790f0ba5): no flush for a package without a linker
bool InstallResetLoadersNoFlush(bool beforeGameCode, bool key);  // start-up only (key on; lab: always)
void SetResetLoadersNoFlush(bool on);                   // monitor thread (live once installed)
void LogResetLoadersNoFlushStats();                     // stats interval (nothing when not called)
void ResetLoadersNoFlushHook(void* obj);                // the hook (self-test calls it with fakes)
bool ResetLoadersNoFlushTestDiscover(const GameModule& game, uint32_t rva[5], uint32_t* linkerOff);
void ResetLoadersNoFlushTestSetup(void* game, void* getPackage, void* findLinker, void* isAsyncPtr, bool on);
void ResetLoadersNoFlushTestCounts(uint64_t* calls, uint64_t* skipped, uint64_t* linker);
// guards/pair_race.cpp (PairReleaseOnce, game bug fix; lab also PairRaceProbe): each pairing buffer released once
bool InstallPairRace(bool beforeGameCode, int fixKey, const wchar_t* iniPath);  // start-up only (lab: PairRaceProbe's own key)
void LogPairReleaseOnceStats();   // stats interval: the skipped early releases, when the count changed
struct PairRaceDiscovery {
    uint8_t* release, *acquire, *first, *dtor, *drain, *erase, *runSession;  // pool Release, Acquire (lab); early release; destructor; manager +8; erase; RunSession
    void** releaseSlot, **acquireSlot;  // the pool vtable (lab)
    uint8_t* runCall;                   // RunSession's call of the early release (exe+0x45910F0)
    uint8_t* firstRet, *dtorRet, *drainRet;  // where the pool Release returns to at the three teardown sites
    uint8_t* stepLoop, *preTickRet, *flowRet;  // exe+0x489C880 and its two callers' return addresses (lab; may stay null)
};
bool PairRaceDiscover(const GameModule& game, PairRaceDiscovery* out, const char** missing);  // self-test (no writes)
int PairReleaseOnceTestInstall(); // self-test: the fix's writes and stubs on the mapped exe; 0 = passed
// render/water_info_skip.cpp (WaterInfoSkip): a water zone's same-input water-info redraw skipped (equivalent)
bool InstallWaterInfoSkip(bool beforeGameCode, int key, uint8_t* gtFn, const wchar_t* iniPath);  // start-up, after InstallWaterCapture
bool WaterInfoSkipInstalled();
void SetWaterInfoSkip(int mode);                     // live: 0 off, 1 skip same-input redraws (lab: 2 proof)
bool WaterInfoSkipGt(const uint8_t* ctx);            // game thread, before UpdateWaterInfoRendering: true = skip the call
void LogWaterInfoSkipStats();
uint8_t* WaterInfoGtSite();                          // water_info.cpp: UpdateWaterInfoRendering when its game-thread detour is in
int WaterInfoSkipTestLogic();                        // self-test: 0 = all checks passed
const char* WaterInfoSkipTestPins(const GameModule& game, uint8_t* fn);  // self-test: nullptr = every layout read matches
// render/water_spread.cpp (WaterInfoSpread): water-info updates spread over frames / merged (equivalent; port130k)
bool InstallWaterInfoSpread(bool beforeGameCode, int mode, int nearM, int perFrame, int maxFrames, int mergeMs);  // start-up only
void SetWaterInfoSpread(int mode, int nearM, int perFrame, int maxFrames, int mergeMs);  // live once installed
void LogWaterInfoSpreadStats();
// WaterInfoAfterCrossing (waterlate, S-003): a zone's water-info request held while levels are added / removed, bounded
void SetWaterInfoAfterCrossing(int afterMs, int quietMs);  // before InstallWaterInfoSpread (installs the site when on), then live
void WaterCrossingNoteStream();                     // game thread: a level add / remove step (StreamOutEndPlaySlice's hook)
bool WaterCrossingWatching();                       // installed and on: the AddToWorld hook times its calls
void WaterCrossingNoteAdd(int64_t qpcTicks);        // StreamRegisterGranularity's AddToWorld hook: the call's own time (QPC ticks)
int StreamLoadingModeNow();                         // stream_granularity.cpp, game thread: Altar's loading mode 1 / 0, -1 unknown
int WaterSpreadTestLocalTess(const GameModule& game);  // self-test: bEnableLocalOnlyTessellation's offset as read from the code (0 = none)
int WaterSpreadTestCrossingBits();                  // self-test: failed checks of WaterSpreadTestLogic's cases 21-26 (0 = none)
int WaterSpreadTestDiscover(const GameModule& game, uint32_t* flag, uint32_t* tex, uint32_t* extents, uint32_t* groundZ);  // self-test
int WaterSpreadTestLogic();                                                           // self-test: 0 = all checks passed
// menus/map_spread.cpp (MapPopupSpread): the map menu's icon build (drop lists cleared unseen, reuse equal rebuilds; port130k)
bool InstallMapPopupSpread(bool beforeGameCode, int mode, bool pacingHooked);  // start-up only (native tables before registration)
void SetMapPopupSpread(int mode);                 // live once installed: 0, 1 drop, 4 reuse, 5 both
void MapPopupSpreadFrameEnd(uint64_t frame);      // GT, NpcCreationPacing's frame end
void MapPopupSpreadPairing(bool begin);           // GT, NpcCreationPacing's FrameRun: its main pairing run begins / ended
void MapPopupSpreadPostTick();                    // GT, NpcCreationPacing's post-actor-tick end call
void LogMapPopupSpreadStats();
struct MapSpreadFound {
    int tableCounts[3];
    uint8_t* loop, *clear, *size, *hover, *copy, *rehash, *free, *thunkReturn, *filterTable, *iconTable;
    uint32_t peSlot;
    uint8_t* mapTable;       // {name, exec thunk} pairs of the map widget class
    bool mapAudited;         // the map widget's table is the audited one (only its icon natives are wrapped; mapshrink)
    bool wrapped[3][256];    // per table (map widget, icon filter, icon), per entry: gets a notification stub
    int codeSites, inlinePatches;   // code sites written at install (inline E9 entries + call sites; mapfix: 11 / 9)
    uint8_t* inlineAt[16];          // where the inline E9 patches go (none at UpdateOrCreateMapIcons' exec thunk: mapfix)
    uint8_t* iconsThunk;            // UpdateOrCreateMapIcons' exec thunk (from the native table)
};
const char* MapSpreadTestDiscover(const GameModule& game, MapSpreadFound* out);  // self-test: nullptr = every site found (no writes)
int MapSpreadTestLogic();                                                        // self-test: 0 = all checks passed
// menus/gpu_menu_shot.cpp (GpuMenuShot, worker gpushot; public since 1.3.0, port130k): the menu backdrop made on the GPU
struct GpuMenuShotSites {
    uint8_t* copyTexture; void** copySlot; uint8_t* handler; uint8_t* createTexCall; uint8_t* createTex;
    uint8_t* transition; uint8_t* addBarrier; uint8_t* flush; uint8_t* residency; uint8_t* service;
    uint8_t* saveCallback; uint8_t* saveFunction; const char* leftoverWhy;  // leftover save bindings (nullptr why = handled)
};
const char* GpuMenuShotDiscover(const GameModule& game, GpuMenuShotSites* out);  // nullptr = all sites found (self-test too)
bool InstallGpuMenuShot();                                   // start-up (after SaveThumbnailAsync's and the resize hooks)
void SetGpuMenuShot(int mode, bool hdrMenuBackground);       // live 0/1 (3 = the compare test; lab: 2)
int GpuMenuShotMode();
uint8_t* GpuMenuShotHandlerFn();
bool GpuMenuShotSkipResize(int32_t srcW, int32_t srcH, int32_t dstW, int32_t dstH, bool linear);  // image_resize.cpp
int GpuMenuShotPrepare(int32_t x0, int32_t y0, int32_t w, int32_t h, bool hdr, const char** why); // GT: job + 1, or 0
uint8_t* GpuMenuShotServiceFn();                // Altar's screenshot service (the engine delegate's listener)
const uint8_t* GpuMenuShotServiceDelegate();    // the engine delegate its code unbinds from (must be save_thumb's)
bool GpuMenuShotIsAdjustingExec(const uint8_t* exec);
// gh54 (GitHub issue #54): setups the takeover is not made on (self-tested rules). Layer rule: a reason, kGpuMenuShotLayerScan
// (a proxy DLL name outside the Windows system folder: its image is searched for OptiScaler's name) or nullptr.
extern const char* const kGpuMenuShotLayerScan;
const char* GpuMenuShotLayerRule(const wchar_t* moduleName, bool fromSystemFolder);
const char* GpuMenuShotSetupRule(uint32_t vendorId, const char* layer);  // nullptr = the takeover may run
bool GpuMenuShotImageHolds(const uint8_t* base, size_t size, const char* text);  // a loaded module's image holds the text
// Leftover save-thumbnail bindings on the screenshot service (shot130q; the game's behaviour after a save made in a menu):
struct GpuMenuShotLeftoverView {
    bool callback, rawExec, holder, saveSystem; int32_t state; bool eventSlot, eventDone, handleOnEngine;
    int32_t resX, resY; float multiplier;
};
const char* GpuMenuShotLeftoverRule(const GpuMenuShotLeftoverView& v);  // nullptr = may be held out (self-test)
bool GpuMenuShotLeftover(const uint8_t* fn, uint8_t* holder, bool rawExec, const char** why);  // GT: may it be held out?
bool GpuMenuShotEventDone(const void* ev);                                   // the callback's own test (null = done)
bool GpuMenuShotHandleOnDelegate(const uint8_t* delegate, uint64_t handle);  // what the callback's Remove would find
struct GpuMenuShotHeld {                       // leftovers held out of one call (their raw delegate's function swapped)
    static constexpr int kMax = 16;
    int n = 0; uint8_t* inst[kMax] = {}; void* fn[kMax] = {};
};
bool GpuMenuShotHeldAdd(GpuMenuShotHeld& h, uint8_t* inst);   // false = full
void GpuMenuShotHeldSwap(GpuMenuShotHeld& h, bool hold);      // hold: function slots -> no-op; false: restored
void GpuMenuShotNoteLeftovers(int job, const GpuMenuShotHeld& h);  // GT: held out of the placeholder call
void GpuMenuShotBeginCapture(int job);         // GT: around the service delegate's call with an empty picture
int GpuMenuShotEndCapture();                    // -> textures made
uint32_t GpuMenuShotServiceArm(int job, uint64_t weak, int32_t adj, void* weakGet, int32_t w, int32_t h);  // -> generation
void GpuMenuShotSetHandlers(int job, const uint64_t* weak, const int32_t* adj, int n);  // GT: the checked backdrop handlers (fallback)
void GpuMenuShotServicePixels(uint32_t gen, uint32_t* pixels, int32_t w, int32_t h);  // helper thread: the read-back picture (ownership passes)
void GpuMenuShotGameThreadFrame();              // GT, every frame (menu_open_flash.cpp's ProcessScreenShots hook)
bool GpuMenuShotSkipping();                     // GT: inside the service call with a placeholder (menu_hdr_background.cpp)
void* GpuMenuShotMarker(int job);
void* GpuMenuShotProbeMarker();
bool GpuMenuShotIsMarker(const void* outData);
void GpuMenuShotRenderThread(void** ctx, void* cmdList);
bool GpuMenuShotVerify(int job, const uint32_t* ref, int32_t w, int32_t h);
void GpuMenuShotTaken(int job, uint64_t gtTicks);
void GpuMenuShotDeclined(const char* why);
void LogGpuMenuShotStats();
void GpuMenuShotTestReference(const uint32_t* r10, int32_t w, int32_t h, const uint32_t* lut, uint32_t* out);  // self-test (lut: 3072 + 256 entries)
size_t GpuMenuShotShaderSize();
const unsigned char* GpuMenuShotShaderBlob(size_t* size);  // self-test: the compiled compute shader (DXBC with its root signature)
struct SaveThumbRhi {
    void* readbackCtor; void* readbackVtable; void* executeAndReset; void* initContexts; void** rhiGlobal; bool hdrRoute;
};
bool SaveThumbInternals(SaveThumbRhi* out);  // saves/save_thumb.cpp: its render-thread helpers, once installed (GpuMenuShot)
// npcs/repath_parallel.cpp (RepathParallel): a TickActor's queued re-searches' A* on helper threads, served on identical inputs
bool InstallRepathParallel(bool beforeGameCode, int key, int minQueued, int threads, bool helpersAllowed, bool labProbe,
                           int earlyKey, int deferKey);  // start-up only; earlyKey = RepathEarly, deferKey = RepathDefer
void SetRepathEarly(int key);                                          // monitor thread, each second (live once installed)
void SetRepathDefer(int key, int graceUs, int maxFrames, int toleranceUu);  // monitor thread, each second (live once installed)
int RepathDeferTestDiscover(const GameModule& game, uint32_t* rva, const char** why);  // self-test: 1 = found (ResizeGrow's RVA)
int RepathDeferTestKey(int which);     // self-test: the tolerant key (1 = as expected)
int RepathDeferTestQueue(int which);   // self-test: a request taken out of / put back into a fake RepathRequests (1 = as expected)
int RepathDeferTestResume(int which);  // self-test: the deferred batch at later TickActors on fakes (1 = as expected)
int RepathDeferTestHandBack(int which);  // self-test: S-067 hand-back to RepathParallel and the offered-path cover (1 = as expected)
int RepathDeferTestBurst(int which);     // self-test: S-067 five-search burst through DeferStart's grace / hand-back / take-out (1 = as expected)
int RepathDeferTestBeside();         // self-test: S-067 TickHook path: RepathParallel's batch beside an offer-only deferred batch (1 = as expected)
void SetRepathParallel(int mode, int minQueued, bool helpersAllowed);  // monitor thread, each second (live once installed)
void LogRepathParallelStats();                                         // stats interval (no line when nothing ran)
int RepathParallelTestDiscover(const GameModule& game, uint32_t* rvas, const char** why);  // self-test: 1 = found (21 RVAs)
bool RepathParallelTestKey();    // self-test: the input key on fake queries
bool RepathParallelTestHooks();  // self-test: the nav-edit counter and the link recorder with fake originals
int RepathParallelTestLinkServe(int which);  // self-test: serve / decline of a search that asked custom links (1 = as expected)
int RepathEarlyTestDiscover(const GameModule& game, uint32_t* rvas, const char** why);  // self-test: 1 = found (5 RVAs)
bool RepathEarlyTestCopies();         // self-test: the early filter / link filter copies (abort, drop, keys)
int RepathEarlyTestServe(int which);  // self-test: serve / decline of an early search (1 = as expected)
int RepathEarlyTestFence(int which);  // self-test: a navmesh writer's fence on an early batch (1 = as expected)
int RepathEarlyTestTick(int which);     // self-test: TickActor stops what it cannot serve before it waits (1 = as expected)
int RepathEarlyTestCovers(int which);   // self-test: EarlyCovers (1 = as expected)
int RepathEarlyTestWriters(int which);  // self-test: ReplaceAreaInTileBounds counter, a writer at the publish (1 = as expected)
// render/lumen_cards_narrow.cpp (LumenCardsNarrow): the Lumen mapping upload sends only the instances whose group changed (identical)
struct LumenCardsOffsets { uint32_t list, groupList, groupsData, bits, bitsNum, sceneData, primNumInst, primOffset, primGroupsNum, primIndices; };
bool LumenMeshCardsDiscover(const GameModule& game, uint8_t** call, uint8_t** fn, LumenCardsOffsets* o, const char** why);  // also the lab probe's
bool InstallLumenCardsNarrow(bool beforeGameCode, int key);  // start-up only (key != 0), then live
void SetLumenCardsNarrow(int value);                         // monitor thread, each second (live once installed)
void LogLumenCardsNarrowStats();                             // stats interval (no line when nothing ran)
int LumenNarrowTestDiscover(const GameModule& game, uint32_t* rvas, uint32_t* offs);  // self-test: 1 = found (no writes)
int LumenNarrowTestMismatch(const GameModule& game);  // self-test: on a private copy; 0 = passed
int LumenNarrowTestStub();                            // self-test: the stub executed; 0 = passed
int LumenNarrowTestLogic();                           // self-test: the decision with fakes; 0 = passed
int LumenNarrowTestUnwind(const GameModule& game);    // self-test: the stubs' unwind data, walked; 0 = passed
int LumenNarrowTestResetSite(const GameModule& game, uint32_t* foundRva);  // self-test: the lists' reset, refusal and patch on a copy; 0 = passed
// An observer called at the lists' reset (render thread, before they are emptied), through LumenCardsNarrow's reset patch, or a
// reset-only patch when that fix is not installed (start-up only). False (with why) = no reset hook.
bool LumenListsResetObserve(void (*observer)(const void* lumenSceneData), const char** why);
// render/lumen_scan_cull.cpp (LumenScanCull, lumenscan; public since 1.4.0): Lumen's CPU primitive scan reads only the blocks of groups that can emit a request
struct LumenScanBlock {   // one per 64 group indices (36 bytes): what the block's last scan saw
    float lo[3], hi[3];   // box over its candidates (MeshCardsIndex -1, valid mesh cards)
    float maxExt;         // the largest GetExtent().GetMax() of its near candidates (not far field)
    float maxExtFar;      // ... of its far-field candidates
    uint8_t flags;        // 1 empty, 2 emissive, 4 cards, 8 merged, 16 far-field candidates, 32 bad box, 64 added, 128 near candidates
    uint8_t pad[3];
};
bool InstallLumenScanCull(bool beforeGameCode, int key);  // start-up only (key != 0), then live 0 / 1 / 2
void SetLumenScanCull(int value);                         // monitor thread (live once installed)
void LogLumenScanCullStats(bool detailed);               // per stats interval (no line when nothing ran); lab: detailed = every 10 s (LumenScanCullStats=1)
LumenScanBlock* LumenScanBlocks(int32_t* count);          // the summaries (for a later GPU-side use)
void LumenScanCullTask(void* task);                       // the hooks (the self-test calls them directly)
void LumenScanCullPrep(void* lumenSceneData);
void LumenScanCullPost();
void LumenScanCullListsReset(const void* lumenSceneData);
int LumenScanCullTestDiscover(const GameModule& game, uint32_t rva[4], uint32_t* pgOff, uint32_t* viewSpecOff);  // self-test: 1 = found (no writes)
bool LumenScanCullTestInit(void* body, uint32_t pgOff, uint32_t listOff, uint32_t viewSpecOff);  // self-test: state with the given body, no patch
void LumenScanCullTestLegacyGap(bool on);  // self-test: the first version's mode handling (review finding 1)
void LumenScanCullTestMode(int mode);
void LumenScanCullTestCounts(uint64_t* scanned, uint64_t* skipped, uint64_t* verified, uint64_t* mismatches);
void LumenScanCullTestDone();
uint8_t* LumenScanCullTestStub(const void* prep, const void* original, const void* post);
void LumenScanCullTestFreeStub(uint8_t* mem);
// render/buffer_floor.cpp (InstanceBufferFloor; lab also SceneBufferFloorMiB): the GPU scene instance buffer at 192 MiB at its first large need
bool InstallBufferFloor(bool beforeGameCode, int instanceKey, bool gpuSceneKeep, const wchar_t* iniPath);  // start-up only
uint32_t BufferFloorGrow(int kind, uint32_t use, uint32_t need);        // GpuSceneKeep: the size a growth gets
bool BufferFloorHold(int kind, uint32_t need, uint32_t capacity);       // GpuSceneKeep: keep the capacity at this need
struct BufferFloorChoice { bool on; uint32_t instanceSlots; uint64_t sceneBytes; const char* why; };
BufferFloorChoice BufferFloorChoose(double budgetMB, double usedMB, int instanceKey, int sceneKey);  // the gate and the tiers
void BufferFloorTestKeys(int instanceKey, int sceneKey, double budgetMB, double usedMB);             // self-test: keys and a fake budget
void BufferFloorTestVram(double budgetMB, double usedMB);                                           // self-test: a later fake sample
void BodySpreadFrame();                                 // GT, every frame start (NpcCreationPacing's cap hook)
void BodySpreadPostTick();                              // GT, every post-actor tick (NpcCreationPacing)
int BodySpreadTestDiscover(const GameModule& game, uint32_t* arr, uint32_t* handle, uint32_t* weak);  // self-test: 1 = found
int BodySpreadTestLogic();                                                                           // self-test: 0 = passed
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
