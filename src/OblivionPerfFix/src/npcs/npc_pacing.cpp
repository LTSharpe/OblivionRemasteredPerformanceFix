// NpcCreationPacing: spread the creation of far-away NPCs over frames.
//
// When the old engine (the OblivionThread, "OT") attaches a new row of exterior cells it creates every actor in them at
// once: for each NPC a "create actor" command goes through Altar's pairing queue to the game thread (GT), which spawns
// the Unreal actor (components, first anim instance, construction script, AI controller) and runs a second command for
// the head and headwear; one frame later Altar's asset-load completion delegates swap in the real body/head meshes
// (second anim instance), add each piece of clothing (child actors) and the hair. Per NPC that is 20-40 ms of GT work
// in pieces ("units") of 1-15 ms, and the GT runs all of them in the frame they arrive: 3-6 frames of 45-110 ms when a
// row with a few NPCs attaches (2026-10-07/08: the Stendarr route, Sheogorath's shrine 11k units away).
//
// The fix gives those units a per-frame GT time budget. A unit that does not fit waits for a later frame, but only if
// its actor is far from the player (PacingMinDistance); near actors, travel, loading screens, menus and anything the
// guards do not recognise run exactly as before. Nothing is skipped, everything still runs in the same order:
//
// (a) The pairing queue (OT -> GT, "Replication" session -1) is processed by the GT in OnWorldPreActorTick through the
//     pairing manager's RunSession (manager vtable +0x70), which executes commands until the OT's end-of-frame command.
//     To defer a create command, its Execute (create command vtable +8) gives back the semaphore count the reader
//     took, clears the session's run flag and returns 0: the read position stays on the command and RunSession returns
//     as after an end command. The next frame starts with that command. The whole queue waits behind it (FIFO), so
//     every later command for any reference keeps its order. The GT then owes end commands: in frame g it may only wait
//     for the OT's ends up to number g (the OT's frame g+1 needs the GT's reverse end of frame g), so it counts
//     RunSession(-1) calls (S) and ends executed (E) and only continues while E < S. Before any other session runs on
//     the GT (the travel session) the owed part of the queue is run first, so their relative order is the stock one.
//     A heavy generic pairing command (the head/BP command after a create; learned by its cost) is treated the same
//     way when a distant create preceded it in the same OT frame.
// (b) Altar's three completion delegates (appearance apply, equipment slot, hair) are run by UE's
//     FStreamableDelegateDelayHelper, all due ones in one frame. Their ExecuteIfSafe / Execute (vtable slots) copy a
//     deferred delegate instance (the capture is copied with the type's own copy constructor; the weak pointer is
//     re-checked by the original when it finally runs) into one global FIFO, replayed at the start of the next frames
//     and in OnWorldPostActorTick (the pairing manager's end-of-frame call, vtable +0x38, from that handler).
//     A delegate's actor is its owner (the weak pointer at +0x18) or the first actor among the owner's outers (up to
//     four objects). An equipment or hair delegate's owner is a smaller object whose word at the actor's root-component
//     offset can be an invalid pointer: the fault reading it ends the walk, so the unit counts as near and runs
//     whatever the budget, right after its actor's appearance unit.
//     A hair delegate's owner is the hair-piece asset (outer: its package), so no walk from it reaches the character:
//     every hair unit counted as near ("unknown"), and with a backlog it queued as a near unit behind the far ones and
//     then ran whatever the budget once it reached the head (14 hair units in one frame after a train of far appearance
//     units, 24.8 ms, Imperial City Waterfront; npcunits 2026-11-12). PacingHairActor resolves a hair unit through the
//     weak pointer its lambda calls SetSkeletalMesh on (the hair mesh component, whose outer is the character), checked
//     at start-up in the lambda's code; the walk is the guarded one (a component is no actor).
//
// (c) (PacingFlowWait) Altar runs a "flow" session for each newly loaded level's pre-placed actors at the start
//     of OnWorldPostActorTick. In the game the main queue is always complete by then (the GT waited for the OT's end
//     command), so a flow follows the whole OT frame. When pacing has left that frame's tail for later, running the
//     flow first needs that tail at once ("drain": 2-3 creates in one frame, 17-27 ms, at the Silver Road's flow
//     spots). Instead the flow waits: its RunSession returns 3 ("not finished, run again"), which the flow runner
//     treats as "nothing happened", and the GT's end command for the reverse session (GT -> OT) is held back for this
//     frame, so the OT stays in its RunSession(-2) after its frame g, running the GT's commands as they come. Next frame
//     the GT continues the main queue (up to the OT's end command g, the only one it may wait for: the OT's frame g+1
//     needs the held-back end; it goes before older waiting delegates), the flows run when it is complete (waiting
//     delegates are Unreal-side load completions, which the main queue overtakes every frame too: they stay in their
//     queue instead of being forced through first), and the end command goes out. The order of every
//     command between the two threads is the game's: main queue frame g, flows (all of frame g, the OT is waiting),
//     reverse commands, OT frame g+1. The OT sees one long GT frame, as at a stock frame with the same work in it. The
//     session's open flag is cleared as the end command would, so the next frame's Begin works as usual. At most
//     kMaxFlowWait frames in a row; then the queue drains as before; up to kMaxFlowWaitProgress frames while
//     the main queue is getting through the flow's own OT frame (a create or head/BP unit ran in each held frame and
//     nothing older is owed): the drain at the 3-frame bound ran that frame's last creates together with the paced one
//     (two creature creates in one frame, 29-30 ms at 4K, Echo Mine -> Brotch Camp, in half the runs). With
//     PacingLeanFlowDrain the drain at the bound runs the main queue only: the waiting delegates stay queued, as when
//     the flow runs on a complete queue (14-18 delegates drained with the queue there, 30-40 ms, Imperial City flight).
//     PacingFlowProgress (drainfix 2026-11-12) widens "progress" for the 8-frame bound: a create or head/BP unit that ran
//     while older OT frames are owed too (before: only with nothing older owed), and a frame in which the queue's head
//     create waited for its asset loads (CreatePreload). Without it, a flow at the Waterfront drained after 3 frames: the
//     head create's load still in flight (24-28 ms synchronous load in the drain, 2 of 6 runs), or two creates of a train
//     with older frames owed in one frame (15-24 ms).
//
// Budget: PacingBudgetMs of unit time per frame (the first unit of a frame always runs). Lag bound: a unit or an OT
// frame older than PacingMaxLagFrames runs whatever the budget. Switched off (ini, re-read every second), travel,
// a loading screen, a menu or any failed check: the backlog is run at once and nothing is deferred.
//
// Every site is found by byte pattern or as the only .rdata slot holding a matched function; offsets are read from
// the matched code where they vary; the fix stays off with a log line if anything does not match.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>

namespace opf {
namespace {

// ---- code patterns (addresses of build 19115871 in the comments) ----

// Pairing manager RunSession(key) (exe+0x4591010, manager vtable +0x70): lea rbp,[rcx+80h] (session table);
// call ChooseWriter; call FindSession(table,key) (+0x2C); mov r12d,[rax+50h]; ... mov word [rbx+4Ch],1;
// call [rax+28h] (step); ... cmp byte [rbx+4Ch],0 ... cmp byte [rbx+4Dh],0
constexpr char kRunSession[] =
    "48 89 5C 24 18 55 56 41 54 41 56 41 57 48 83 EC 20 48 8D A9 80 00 00 00 44 8B F2 48 8B CD E8 ?? ?? ?? ?? "
    "41 8B D6 48 8B CD 48 8B F0 E8 ?? ?? ?? ?? 45 33 FF 48 8B D8 44 8B 60 50 45 85 E4 75 ?? 48 8D 4C 24 50 4C 8B F8 "
    "FF 15 ?? ?? ?? ?? 0F 57 C0 F2 48 0F 2A 44 24 50 F2 0F 59 05 ?? ?? ?? ?? F2 0F 58 05 ?? ?? ?? ?? F2 0F 11 83 88 00 00 00 "
    "48 89 7C 24 58 66 66 66 0F 1F 84 00 00 00 00 00 66 C7 43 4C 01 00 48 8B 03 48 8B CB FF 50 28 8B 4B 50 85 C9 75 08 "
    "3C 02 74 ?? 3C 03 74 ?? 80 7B 4C 00 75 E2 40 B7 04 85 C9 74 ?? 80 7B 4D 00 75 CF";
constexpr size_t kRunSessionFindCall = 0x2C;
// Handler_End (exe+0x4590CC0, manager vtable +0x38): writes the end command, whose function (exe+0x4595F90) is
// "mov byte [rdx+4Ch],0; ret" (clears the session's run flag).
constexpr char kHandlerEnd[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 4C 8B F1 48 8B F2 48 83 E9 80 E8 ?? ?? ?? ?? "
    "4D 8B 06 49 8B CE 48 8B F8 41 FF 90 90 00 00 00 4D 8B 06 49 8B CE 41 FF 50 48 4D 8B 06 BA 30 00 00 00 49 8B CE "
    "41 FF 90 C8 00 00 00 4D 8B 16 48 8D 2D ?? ?? ?? ?? 4C 8B CD 48 C7 44 24 20 02 00 00 00 4C 8B C0 BA 30 00 00 00 49 8B CE "
    "48 8B D8 41 FF 92 D8 00 00 00 48 89 6B 08 48 8D 05 ?? ?? ?? ?? 48 89 03 4C 8B C5 48 C7 43 10 00 00 00 00 48 8D 05";
constexpr size_t kHandlerEndFnLea = 0x99;  // lea rax,[end function]
constexpr char kEndFunction[] = "C6 42 4C 00 C3";
// Session (type B, "Replication") step (exe+0x4590F80, session vtable +0x28): ++[+58h]; semaphore [+90h] Lock; read
// pointer [+18h], end [+20h]; switch to buffer (word [+8] + 1) % pool[+98h].count[+18h]; Execute; [+18h] += size.
constexpr char kStep[] =
    "40 57 48 83 EC 20 FF 41 58 48 8B F9 48 8B 89 90 00 00 00 48 8B 01 FF 10 48 8B 47 20 48 8B 4F 18 48 83 E8 08 48 3B C8 "
    "77 06 48 83 39 00 75 41 48 89 5C 24 30 48 8D 5F 68 48 8B CB FF 15 ?? ?? ?? ?? 48 8B 8F 98 00 00 00 33 D2 0F BF 47 08 "
    "4C 8B 07 FF C0 F7 71 18 48 8B CF 41 FF 50 48 48 85 DB 74 09 48 8B CB FF 15 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B 4F 18 "
    "48 8B 01 FF 50 08 48 01 47 18 32 C0 48 83 C4 20 5F C3";
// The session's write-side buffer switch (exe+0x4596BD0, session vtable +0x10): movsx eax, word [rcx+28h] (writer index)
constexpr char kWriteSwitch[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 4C 8B 81 98 00 00 00 48 8D 59 68 0F BF 41 28 33 D2 FF C0";
constexpr uint32_t kSessReadIdx = 0x08, kSessReadPtr = 0x18, kSessWriteIdx = 0x28, kSessRun = 0x4C, kSessSem = 0x90,
                   kSessPool = 0x98, kPoolCount = 0x18;

// Create-actor command Execute (exe+0x657C8D0, vtable exe+0x8616B80 +8): skips a cancelled entry, else calls
// [cmd+18h](entry [+10h], ..., floats [+30h] .. [+60h]); size 0x80. The position is [+60h] x, [+58h] y, [+50h] z in old
// engine units (checked live against the player: UE x = x * 1.428571, UE y = -y * 1.428571).
constexpr char kCreateExec[] =
    "48 89 5C 24 08 57 48 81 EC 80 00 00 00 48 8B F9 48 8B 49 10 48 85 C9 74 09 E8 ?? ?? ?? ?? 84 C0 75 7F 0F B6 47 20 "
    "F3 0F 10 47 30 F3 0F 10 4F 38 44 8B 4F 68 44 8B 47 70 8B 57 78 48 8B 4F 10 88 44 24 70 0F B6 47 21 88 44 24 68 "
    "8B 47 24 89 44 24 60 48 8B 47 28 48 89 44 24 58 F3 0F 11 44 24 50 F3 0F 10 47 40 F3 0F 11 4C 24 48 F3 0F 10 4F 48 "
    "F3 0F 11 44 24 40 F3 0F 10 47 50 F3 0F 11 4C 24 38 F3 0F 10 4F 58 F3 0F 11 44 24 30 F3 0F 10 47 60 F3 0F 11 4C 24 28 "
    "F3 0F 11 44 24 20 FF 57 18 48 8B 4F 10 48 8B 5F 28 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 9C 24 90 00 00 00 "
    "48 8D 05 ?? ?? ?? ?? 48 89 07 B8 80 00 00 00";
constexpr uint32_t kCreatePosX = 0x60, kCreatePosY = 0x58, kCreatePosZ = 0x50;
// Generic pairing command Execute (exe+0x4919D40; vtables exe+0x8616A90, 0x86193A0, 0x86195D0): [cmd+18h](entry,
// int [+24h], byte [+20h]); size 0x28.
constexpr char kGenericExec[] =
    "40 53 48 83 EC 20 48 8B D9 48 8B 49 10 48 85 C9 74 09 E8 ?? ?? ?? ?? 84 C0 75 0F 44 0F B6 43 20 8B 53 24 48 8B 4B 10 "
    "FF 53 18 48 8B 4B 10 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03 B8 28 00 00 00 48 83 C4 20 5B C3";
constexpr uint32_t kCmdFn = 0x18;

// OnWorldPreActorTick handler (exe+0x4899B50): the per-frame RunSession(-1) call (exe+0x4899E09):
//   mov rax,[rsi]; mov edx,-1; mov rcx,rsi; call [rax+70h]; mov r8,[rip+..]
constexpr char kPreTickRunCall[] = "48 8B 06 BA FF FF FF FF 48 8B CE FF 50 70 4C 8B 05";
constexpr size_t kPreTickRunReturn = 14;
// OnWorldPostActorTick handler (exe+0x4899AF0): ...; call GetManager; mov rdx,[rip+..]; mov r8d,2; mov rcx,rax;
// mov r9,[rax]; call [r9+38h] (Handler_End of the reverse session)
constexpr char kPostTick[] =
    "48 83 EC 38 E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B 15 ?? ?? ?? ?? 41 B8 02 00 00 00 48 8B C8 4C 8B 08 41 FF 51 38";
constexpr size_t kPostTickEndReturn = 0x3D;
constexpr size_t kPostTickFlowCall = 4;  // its first call: the flow runner (exe+0x4881920)
// The flow runner's two RunSession calls (exe+0x4881C52, 0x4881E71), each with the type-A time budget off around it:
//   mgr->SetBudget(0) [+98h]; result = mgr->RunSession([entry+40h]) [+70h]; [global] = result; mgr->SetBudget(1)
constexpr char kFlowRun[] =
    "49 8B 04 24 33 D2 49 8B CC FF 90 98 00 00 00 49 8B 04 24 49 8B CC 8B 53 40 FF 50 70 88 05 ?? ?? ?? ?? B2 01 "
    "49 8B 04 24 49 8B CC FF 90 98 00 00 00";
constexpr size_t kFlowRunReturn = 0x1C, kFlowRunnerSpan = 0x800;
constexpr uint8_t kFlowNotFinished = 3;  // RunSession's "timed out": the flow runner does nothing, next frame runs it again
constexpr int kMaxFlowWait = 3;          // frames in a row a flow may wait before the queue drains as before
constexpr int kMaxFlowWaitProgress = 8;  // ... while each held frame ran a unit of the flow's own OT frame (the
                                         // hidden PacingFlowWaitFrames, 3 = as before)
// Handler_End: "sub rcx,-80h; call ChooseWriter" (the calling thread's writer session) at +0x20
constexpr size_t kHandlerEndWriterCall = 0x24;
constexpr uint32_t kSessOpen = 0x4E;     // a writer session's open flag: Begin sets it, Handler_End clears it

// Change_Level_With_Fade (exe+0x66ACC19): TES->[+11Bh] = 1; TES->[+11Ah] = 1 (fade travel pending); TES->[+120h] = 1
constexpr char kTesTravel[] =
    "48 8B 05 ?? ?? ?? ?? C6 80 1B 01 00 00 01 48 8B 05 ?? ?? ?? ?? C6 80 1A 01 00 00 01 48 8B 05 ?? ?? ?? ?? C6 80 20 01 00 00 01";
constexpr uint32_t kTesTravelPending = 0x11A;

// Completion delegates (TWeakBaseFunctorDelegateInstance): ExecuteIfSafe = IsValid(owner weak [+18h]) then the lambda.
// Appearance apply (exe+0x4852870, slot exe+0x7D769F8): two weak pointers, Get [+20h] -> actor vtable +0AA8h (Refresh)
constexpr char kAppearanceExec[] =
    "40 53 48 83 EC 20 48 8B D9 48 83 C1 18 E8 ?? ?? ?? ?? 84 C0 74 55 48 89 74 24 30 48 8D 73 20 48 8B CE E8 ?? ?? ?? ?? "
    "84 C0 74 33 48 8B CE 48 89 7C 24 38 E8 ?? ?? ?? ?? 48 8B CE 48 8B F8 48 8B 10 48 8B 9A A8 0A 00 00 E8 ?? ?? ?? ?? "
    "48 8B CF 0F B6 90 9A 0E 00 00 FF D3";
constexpr size_t kAppearanceIsValid = 0x0D, kAppearanceGet = 0x33;
// Equipment slot (exe+0x48B7A30, slot exe+0x7D7F1F0) and hair (exe+0x47FBC10, slot exe+0x7D723D0) share this shape:
//   IsValid([rcx+18h]) ? (lambda(rcx+20h), true) : false
constexpr char kLambdaExec[] =
    "40 53 48 83 EC 20 48 8B D9 48 83 C1 18 E8 ?? ?? ?? ?? 84 C0 74 11 48 8D 4B 20 E8 ?? ?? ?? ?? B0 01 48 83 C4 20 5B C3 "
    "32 C0 48 83 C4 20 5B C3";
constexpr size_t kLambdaIsValid = 0x0D, kLambdaCall = 0x1A;
// ... whose lambdas are: equipment (exe+0x48AAAC0) looks the slot up in the owner's slot map and jumps on;
constexpr char kEquipmentLambda[] =
    "4C 8B 09 44 0F B6 51 08 41 8B 81 ?? ?? 00 00 41 3B 81 ?? ?? 00 00 74 ?? 41 8B 89 ?? ?? 00 00 4D 8D 81 ?? ?? 00 00 "
    "49 8B 50 08 FF C9 49 23 CA 48 85 D2 4C 0F 45 C2 41 8B 04 88 83 F8 FF";
// hair (exe+0x47EFF60) sets the hair mesh: IsValid(capture weak), IsValid([+60h]), Get -> vtable +0A68h(mesh, true)
constexpr char kHairLambda[] =
    "40 56 48 83 EC 50 48 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 4C 89 74 24 38 4C 8D 76 60 49 8B CE E8 ?? ?? ?? ?? "
    "84 C0 0F 84 ?? ?? ?? ?? 48 89 5C 24 78 48 8B CE 48 89 6C 24 48 48 89 7C 24 40 4C 89 7C 24 30 E8 ?? ?? ?? ?? "
    "48 8B F8 48 8B 08 48 8B 99 68 0A 00 00 48 8D 4E 10 E8 ?? ?? ?? ?? 41 B0 01 48 8B D0 48 8B CF FF D3";
// ... its first call is IsValid on the capture's first weak pointer, the Get at +0x46 reads the same pointer (rcx = rsi =
// the capture): the object whose SetSkeletalMesh it calls, the hair mesh component (PacingHairActor)
constexpr size_t kHairLambdaIsValid = 0x09, kHairLambdaGet = 0x46;
// Delegate vtable slots: +0 scalar deleting destructor, +0x50 CreateCopy, +0x58 Execute, +0x60 ExecuteIfSafe.
constexpr size_t kSlotDtor = 0, kSlotCreateCopy = 10, kSlotExecute = 11, kSlotExecuteIfSafe = 12;
// A destructor with nothing to destroy: push rbx; ...; test dl,1; je; mov edx,size; call free; ...
constexpr char kTrivialDtor[] = "40 53 48 83 EC 20 48 8B D9 F6 C2 01 74 0A BA ?? 00 00 00 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3";
constexpr size_t kTrivialDtorSize = 0x0F;
// A destructor that frees members ([+70h], [+48h] in the hair's), then "test dil,1; je; mov edx,size; mov rcx,rbx; call free"
constexpr char kMemberDtorStart[] = "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 8B FA";
constexpr char kMemberDtorFree[] = "40 F6 C7 01 74 0D BA ?? 00 00 00 48 8B CB E8";
// CreateCopy: "mov rax,[rbp+10h]; mov [rcx+10h],rax; lea rax,[vtable]; mov [rcx],rax; mov rax,[rbp+18h]; mov [rcx+18h],rax"
// then either the capture's copy constructor "add rcx,20h; call ctor" (hair) or a plain copy.
constexpr char kCopyHeader[] = "48 8B 45 10 48 89 41 10 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 45 18 48 89 41 18";
constexpr char kCopyCtorCall[] = "48 83 C1 20 E8";
constexpr size_t kCaptureOffset = 0x20, kOwnerWeakOffset = 0x18;
constexpr size_t kMaxInstance = 0xA0;

// The player's position: UGameplayStatics::GetPlayerPawn's exec thunk calls GetPlayerController(ctx, idx) then
// GetPawnOrSpectator: "call A; test rax,rax; je; mov rcx,rax; call B; mov [rdi],rax"
constexpr char kGetPawnTail[] = "48 8B 4C 24 40 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 89 07";
// GetPlayerController (exe+0x31ABE90): ...; mov r8d,1; mov rdx,rcx; mov rcx,[GEngine]; call GetWorldFromContextObject
constexpr char kGetPlayerController[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 8B EA 41 B8 01 00 00 00 "
    "48 8B D1 48 8B 0D ?? ?? ?? ?? E8";
constexpr size_t kGEngineMov = 0x29;
// GetPawnOrSpectator (exe+0x357D290): Pawn ? Pawn : spectator
constexpr char kGetPawn[] = "48 8B 81 ?? ?? 00 00 48 85 C0 75 07 48 8B 81 ?? ?? 00 00 C3";
// UEngine::GetCurrentPlayWorld(PossiblePlayWorld) (exe+0x37E3350): first game world in the world list
constexpr char kGetCurrentPlayWorld[] =
    "48 89 5C 24 08 48 8B 81 ?? ?? 00 00 33 DB 48 63 89 ?? ?? 00 00 44 8B D3 48 8D 0C C8 48 3B C1 74 ?? 4C 8B 00 48 85 D2 "
    "74 ?? 4D 8B 88 ?? ?? 00 00 4C 3B CA 74 ?? 4D 85 D2 75 ?? 41 80 38 01 75 ?? 4D 8B 90 ?? ?? 00 00 48 83 C0 08 48 3B C1 75 D1";
// AActor::K2_GetActorLocation exec thunk (exe+0x2DD5130): RootComponent [actor+1A0h], location doubles [root+280h], [+290h]
constexpr char kActorLocation[] =
    "48 8B 81 ?? ?? 00 00 48 85 C0 74 ?? 0F 10 80 ?? ?? 00 00 0F 11 04 24 0F 10 80 ?? ?? 00 00 F2 0F 11 44 24 10";
// AActor::WasRecentlyRendered's exec thunk (exe+0x2DD7970), inlined: world = GetWorld (call), DeltaTimeSeconds
// [world+6E4h], GetLastRenderTime (actor vtable +6B8h), TimeSeconds (double) [world+6C0h]
constexpr char kRecentlyRendered[] =
    "48 8B CE E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B 16 48 8B CE 0F 29 74 24 30 F3 0F 10 B0 ?? ?? ?? ?? "
    "F3 0F 58 35 ?? ?? ?? ?? FF 92 ?? ?? ?? ?? F2 0F 10 93 ?? ?? ?? ??";
constexpr double kRenderedWithinS = 0.5;   // an actor drawn within this many seconds counts as seen
constexpr uint32_t kUObjectOuter = 0x20;   // UObjectBase::OuterPrivate (checked live: root component's outer == pawn)
constexpr double kOldToUe = 1.428571;      // UE units per old engine unit

// UEngine::UpdateTimeAndHandleMaxTickRate (exe+0x37FD1A0, virtual; UEngine and UGameEngine vtables): where the game
// thread sleeps to hold the frame rate cap. Its duration is the frame's slack (PacingUseSlack).
constexpr char kMaxTickRate[] =
    "48 89 4C 24 08 55 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B F1 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 "
    "48 8B D6 FF 90 ?? ?? ?? ?? 84 C0";
// ... which asks the engine for the cap: UpdateRunningAverageDeltaTime(delta, true) (vtable +328h), then
// GetMaxTickRate(delta, true) (vtable +310h, 0 = uncapped)
constexpr char kMaxTickCalls[] =
    "48 8B 06 41 B0 01 0F 28 CF 48 8B CE FF 90 ?? ?? ?? ?? 48 8B 06 41 B0 01 0F 28 CF 48 8B CE FF 90 ?? ?? ?? ?? F6 86";
constexpr size_t kMaxTickRateSlot = 32;
constexpr uint64_t kSlackMarginUs = 1500;  // kept free below the cap


// PacingFadeAware (npcslice 2026-10-20, public since 1.2.3 after npcjitter): UVCharacterFadeInOutComponent's tick
// (exe+0x48D5A40): every spawned NPC
// dither-fades in (value 5 -> 0 at 4/s; findings 2026-10-18 "fade value"). From the matched code: the state (+0xD8,
// 1 = FadeIn), the value (+0xBC), the owner actor (+0xA8), the owner class check (StaticClass call; UE 5.3
// FStructBaseChain: class +0x30 array / +0x38 depth, object class at +0x10) and the actor's fade component (+0x7A0)
// whose floor (+0xCC) the tick applies as max(floor, value).
// Lab PacingNearFadeFrames (nearfade 2026-11-01, user decision "ok that they build while they fade-in"): the same test for
// NEAR actors, stricter (dither >= 4.0, kNearFadeMin) and bounded: their appearance / equipment / hair units may wait under
// the budget for at most the key's frames (1-3), run at once once the dither is below 4.0, and run as near units (nested
// completions, ClothBuildDefer, AnimSkipPlaceholder see a near actor). The create and the head/BP pairing command are not
// touched (the pairing queue keeps its stock timing for near actors).
constexpr char kFadeTick[] =
    "48 89 5C 24 10 57 48 83 EC 20 48 8B D9 8B 89 ?? ?? 00 00 83 E9 01 0F 84 ?? ?? ?? ?? 83 F9 01 0F 85 ?? ?? ?? ?? "
    "F3 0F 59 8B ?? ?? 00 00 F3 0F 58 8B ?? ?? 00 00 0F 2F 8B ?? ?? 00 00 F3 0F 11 8B ?? ?? 00 00 72 ?? 48 8D 4B 30 "
    "C7 83 ?? ?? 00 00 00 00 00 00 33 D2 E8 ?? ?? ?? ?? 48 8B BB ?? ?? 00 00 48 85 FF 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B 57 10 4C 8D 40 30 48 63 40 38 3B 42 38 0F 8F ?? ?? ?? ?? 48 8B C8 48 8B 42 30 4C 39 04 C8 0F 85 ?? ?? ?? ?? "
    "48 8B 87 ?? ?? 00 00 41 B0 01 48 8B D7 48 8B CB F3 0F 10 98 ?? ?? 00 00";
constexpr size_t kFadeStateAt = 15, kFadeValueAt = 49, kFadeOwnerAt = 94, kFadeClassCallAt = 107, kFadeCompAt = 153,
                 kFadeFloorAt = 170;

// ---- types ----
using RunSessionFn = uint8_t (*)(void* manager, int32_t key);
using HandlerEndFn = void (*)(void* manager, void* tag, uint64_t a3, uint64_t a4);
using FindSessionFn = void* (*)(void* table, int32_t key);
using ChooseWriterFn = uint8_t* (*)(void* table);
using CmdExecFn = uint64_t (*)(void* cmd);
using SemFn = void (*)(void* sem);
using ExecIfSafeFn = bool (*)(void* inst);
using ExecFn = void (*)(void* inst);
using DtorFn = void* (*)(void* inst, uint32_t flags);
using CopyCtorFn = void* (*)(void* dst, const void* src);
using WeakGetFn = void* (*)(const void* weak);
using GetWorldFn = void* (*)(void* engine, void* possible);
using GetPcFn = void* (*)(void* context, int32_t index);
using GetPawnFn = void* (*)(void* pc);
using MaxTickRateFn = void (*)(void* engine);
using GetMaxTickRateFn = float (*)(void* engine, float delta, bool allowSmoothing);
using ActorWorldFn = void* (*)(const void* actor);
using LastRenderFn = float (*)(const void* actor);
using StaticClassFn = const uint8_t* (*)();

enum UnitType { kCreate, kGeneric, kAppearance, kEquipment, kHair, kNumTypes };
const char* const kTypeName[kNumTypes] = {"create", "head/BP", "appearance", "equipment", "hair"};
constexpr int kNumDelegates = 3;  // kAppearance + i

struct DelegateType {
    void** vtable = nullptr;
    ExecIfSafeFn execIfSafe = nullptr;  // original slot values
    ExecFn execute = nullptr;
    DtorFn dtor = nullptr;
    CopyCtorFn copyCtor = nullptr;      // nullptr: the capture is copied as bytes (trivially destructible)
    size_t size = 0;
};

struct Entry {
    alignas(16) uint8_t inst[kMaxInstance];
    int type;
    uint64_t since;   // frame it was deferred in
    uint64_t sinceUs; // ... and its time (PacingBaseGate's bound, gate2)
    uintptr_t key;    // its actor (0 = unknown)
    bool distant;
    bool fadeHeld;    // PacingFadeAware: waits because its actor is still invisible in its spawn fade-in
    uint64_t gateFrom;  // PacingBaseGate: the frame the gate first held it where the budget alone would have run it (0 = never)
};
constexpr size_t kFifoCap = 1024;

struct Stats {
    std::atomic<uint64_t> run[kNumTypes]{}, deferred[kNumTypes]{}, frames{}, deferFrames{}, drains{}, forced{};
    std::atomic<uint64_t> maxUnitsPerFrame{}, maxLag{}, maxOwed{}, maxFifo{}, maxRing{}, longestUs{}, maxFrameUnitUs{};
    std::atomic<int> longestType{-1};
    std::atomic<uint64_t> slackFrames{}, maxBudgetUs{}, frameLogs{};
    std::atomic<uint64_t> flowWaits{}, heldEnds{}, flowDrains{};  // (c): flow calls that waited, frames whose end waited, bound reached
    std::atomic<uint64_t> flowLongWaits{};  // (c) waits past kMaxFlowWait while the queue progressed
    std::atomic<uint64_t> leanDrains{}, leanKept{};  // PacingLeanFlowDrain: bound drains of the main queue only, delegates kept
    std::atomic<uint64_t> flowLoadHeld{}, flowLagHeld{};  // PacingFlowProgress: held frames past 3 for a create's loads / with older OT frames owed
    std::atomic<uint64_t> guard[5]{};  // frames not deferring: session, travel, menu/loading, ring, player
    // PacingBaseGate (per stats interval): units held, how long, how they were released; frames with a fit release and
    // how far their real busy time ended from the prediction
    std::atomic<uint64_t> gateHeld{}, gateRan{}, gateWaitSum{}, gateWaitMax{}, gateWaitHist[6]{}, gateLate{}, gateForced{},
        gateFallback{}, gateDrain{}, fadeRate{}, gateFitFrames{}, gateOver{}, gateUnderSum{}, gateOverSum{}, gateErrMax{};
    // gate2: held units released on a calm frame, released because their actor may no longer wait (near, seen, about to
    // show), released by a key-0 unit queued behind them; the oldest age (frames since queued) a held unit ran at
    std::atomic<uint64_t> gateCalm{}, gateTurned{}, gateKey0{}, gateAgeMax{}, gateFit{}, gateAgeMaxUs{};
    std::atomic<uint64_t> gateFloorTicks{}, gateFloorActors{}, gateFloorMax{};  // gate2 fade hold: ticks held, actors, longest (frames)
};

void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

struct State {
    bool found = false, installed = false;
    // pairing
    uint8_t* runSession = nullptr;
    uint8_t* handlerEnd = nullptr;
    void** runSessionSlot = nullptr;
    void** handlerEndSlot = nullptr;
    FindSessionFn findSession = nullptr;
    void** sessionVtable = nullptr;
    uint8_t* createExec = nullptr;
    void** createSlot = nullptr;
    uint8_t* genericExec = nullptr;
    void** genericSlots[8] = {};
    int numGenericSlots = 0;
    uint8_t* preTickReturn = nullptr;
    uint8_t* postTickReturn = nullptr;
    uint8_t* flowReturn[2] = {};   // the flow runner's RunSession calls (none found: flows drain the queue as before)
    ChooseWriterFn chooseWriter = nullptr;
    uint8_t** tes = nullptr;
    RunSessionFn origRun = nullptr;
    HandlerEndFn origEnd = nullptr;
    CmdExecFn origCreate = nullptr;
    CmdExecFn origGeneric = nullptr;
    // delegates
    DelegateType del[kNumDelegates];
    WeakGetFn weakGet = nullptr;
    // player position
    GetWorldFn getWorld = nullptr;
    void** gEngine = nullptr;
    GetPcFn getPc = nullptr;
    GetPawnFn getPawn = nullptr;
    uint32_t rootOffset = 0, locationOffset = 0;
    ActorWorldFn actorWorld = nullptr;
    uint32_t deltaOffset = 0, timeOffset = 0, lastRenderSlot = 0;
    int delegateLogs = 0;
    bool freezeKnown = false;
    MaxTickRateFn origMaxTick = nullptr;
    void** maxTickSlots[4] = {};
    int numMaxTickSlots = 0;
    uint32_t getMaxTickRateSlot = 0;

    // settings (written by the monitor thread)
    std::atomic<bool> enabled{false};
    std::atomic<int> budgetUs{4000};
    std::atomic<double> minDistance{4096 * kOldToUe};
    std::atomic<int> maxLag{15};
    std::atomic<bool> useSlack{true};
    std::atomic<bool> flowWait{true};
    std::atomic<int> flowWaitFrames{kMaxFlowWaitProgress};
    std::atomic<bool> ownerWalk{true};  // PacingOwnerWalk (mkusm, PR #25): a non-actor in a delegate's owner chain does not end the walk
    std::atomic<bool> leanFlowDrain{true};  // PacingLeanFlowDrain: a level flow at its wait bound drains the main queue only
    std::atomic<bool> hairActor{false};     // PacingHairActor: a hair unit resolves its character through its mesh component
    uint32_t hairActorWeak = 0;             // ... the component's weak pointer in a hair delegate instance (0 = not found)
    std::atomic<bool> flowProgress{false};  // PacingFlowProgress: a waiting flow's progress also counts with older OT frames owed
                                            // and while the head create waits for its loads (CreatePreload)
    uint64_t preloadWaitFrame = 0;          // game thread: the last frame whose head create CreatePreload deferred
    // PacingBaseGate (basegate 2026-11-12): a far, unseen (or fade-hidden) NPC's delegate unit runs after the actor tick
    // only if the frame's time so far + the predicted rest + the unit's size stays within the target
    std::atomic<int> gateUs{0};             // the target (us); 0 = off
    uint64_t gateS3Us = 0, gateS3UnitUs = 0, gateS3Frame = ~0ull;  // the post-actor tick of frame gateS3Frame: time, unit time so far
    uint32_t gateRest[5] = {};              // the last frames' time from the post-actor tick to the cap's sleep, without units
    int gateRestN = 0, gateRestAt = 0;
    uint32_t gateLast[kNumTypes][8] = {};   // the last eight unit times per type (the size estimate: their upper quartile)
    int gateLastN[kNumTypes] = {}, gateLastAt[kNumTypes] = {};
    uint64_t gatePredUs = 0, gatePredFrame = ~0ull;  // the last fit release's predicted busy time, and its frame
    int gateFrameHolds = 0, gateFrameRuns = 0, gateFrameLate = 0, gateFrameDrain = 0, gateFrameFallback = 0;  // this frame (lab line)
    uint64_t gateDrainFrame = ~0ull;        // the frame a unit last ran toward a soon-visible actor (one per frame)
    uint64_t gateLateFrame = ~0ull;         // the frame units last ran for the lag bound (gatefix D1) ...
    uint64_t gateLateDone = 0;              // ... and how many in that frame
    bool gateOffLogged = false;
    // gate2 (2026-11-13): the gate decides per queued unit; its hard bound, calm-frame threshold and near distance (live)
    std::atomic<int> gateBound{45};         // PacingGateBoundFrames: a held unit runs by this age (frames since it was queued)
    std::atomic<int> gateHoldUs{0};         // PacingGateFadeHoldMs: the fade hold (lab, 0 = off; rev96e B1)
    std::atomic<int> gateCalmUs{17000};     // PacingGateCalmMs: a held unit may run on a frame whose own time stays below it (0 = off)
    std::atomic<int> gateNearCm{10000};     // PacingGateNearM: an actor nearer than this is not held by the gate (game units)
    uint64_t gateHeavyFrame = ~0ull;        // the frame a held unit last ran for room (fit or calm): one heavy unit a frame
    int gateFrameCalm = 0, gateFrameU = 0, gateFrameKey0 = 0;  // this frame (lab line): calm runs, runs of units not held, key-0 drains
    uint64_t gateFrameOldest = 0;           // this frame (lab line): the oldest held unit's age
    // gate2 fade hold: the actors with a held appearance / equipment unit (from the last GateClassify), the frame each was
    // first held, the frame the list was made; their spawn fade-in waits at its floor (dither 4) until those units ran
    static constexpr int kGateHoldMax = 64;
    uintptr_t gateHoldKey[kGateHoldMax] = {};
    uint64_t gateHoldSince[kGateHoldMax] = {};
    int gateHoldN = 0;
    uint64_t gateHoldFrame = ~0ull;
    int gateFrameFloor = 0;                 // this frame (lab line): fade ticks held at the floor
    uint64_t gateFrameFloorOldest = 0;      // ... and the longest such hold (frames)

    // game-thread state
    DWORD gtId = 0;
    uint64_t frame = 0;      // GT frames seen (RunSession(-1) from OnWorldPreActorTick)
    uint64_t S = 0, E = 0;   // RunSession(-1) calls, end commands executed
    bool allowDefer = false; // this frame: enabled, guards passed
    bool inMainRun = false;  // inside our RunSession(-1)
    bool inFrame = false;    // between our pre-tick run and the post-tick end
    bool deferredNow = false;
    bool aheadDeferred = false;  // CreateAhead: a command was deferred since the last frame end
    bool lastCreateFar = false;  // a distant create command ran earlier in this OT frame
    int depth = 0;           // inside a unit
    bool unitDistant = false;  // the running unit's actor may wait (what a delegate completing inside it inherits)
    const uint8_t* unitWeak = nullptr;  // the running delegate unit's owner (weak object pointer; ClothBuildDefer)
    uintptr_t unitKey = 0;              // the running unit's actor key (PacingHairActor: what a nested completion is queued with)
    void* session = nullptr;
    int units = 0;
    uint64_t spentUs = 0;    // unit time + catch-up time this frame
    uint64_t unitUs = 0;     // unit time this frame
    // HitchLedger (hitch_ledger.cpp): cumulative unit time / count, pairing wait and flow time (us)
    uint64_t unitCumUs = 0, unitCumN = 0, pairCumUs = 0, flowCumUs = 0;
    uint64_t ledgerFrame = 0;  // g.frame at the last ledger frame end
    double est[kNumTypes] = {10000, 7000, 8000, 4000, 2000};  // EMA of unit time (us)
    // player position, cached per frame
    uint64_t playerFrame = ~0ull;
    bool playerKnown = false, outerChecked = false, outerOk = false;
    double px = 0, py = 0, pz = 0;
    uintptr_t heavyFns[16] = {};
    int numHeavy = 0;
    int createLogs = 0;
    int drainLogs = 0;
    // (c) flows waiting for the main queue
    bool holdEnd = false;     // this frame's post-tick: a flow waited, the reverse end command waits too
    bool endHeld = false;     // the last frame's reverse end command was held: no new OT frame can be posted (S stays)
    int heldFrames = 0;       // frames in a row whose end command was held
    int flowLogs = 0;
    // frame slack (from the frame-rate cap's sleep)
    uint64_t lastPaceEnd = 0;
    double busyUs = 0;        // decaying max of the GT's busy time per frame without our units
    double capPeriodUs = 0;   // 1 / the engine's max tick rate (0 = uncapped)
    // PacingFadeAware (public since 1.2.3; lab modes 2 / 3 and PacingFadeMin)
    std::atomic<int> fadeAware{0};        // 1: far + rendered actors still invisible in their fade-in may wait; lab 2: near ones too
    std::atomic<int> fadeMinTenths{30};   // ... while max(floor, value) >= this / 10 (5 = invisible, 2 "mostly gone"); lab key only
    StaticClassFn fadeClass = nullptr;
    uint32_t fadeState = 0, fadeValue = 0, fadeOwner = 0, fadeComp = 0, fadeFloor = 0;
    bool fadeFound = false;
    bool lastFadeHeld = false;           // set by ResolveDelegate
    std::atomic<uint64_t> fadeHeldUnits{0}, fadeReleased{0};  // per stats interval: units held as fade-hidden, heads released
                                                              // by the re-check (game thread; read and reset by the monitor)
    bool (*testRendered)(const void*) = nullptr;   // self-test: fake "recently rendered"
    float (*testFade)(const void*) = nullptr;      // self-test: fake spawn fade-in dither (FadeLevel)
    float (*testGameValue)(const void*) = nullptr; // self-test: fake own fade value (GateGameValue; the shown hold's due rule)
    uint64_t fadeRateFrame = ~0ull, fadeRateDone = 0;  // PacingHairActor fade rate (fadefix): units run for it this frame
    uint64_t fadeRateUs = 0;                       // ... and their estimated time (stampfix: the frame's time cap)
    uint64_t lastPeriodUs = 0;                     // the last frame's period (cap exit to cap exit) without our units
    uint64_t budgetThisFrame = 4000;
    uint64_t frameStartUs = 0;
    uint64_t frameStart1Us = 0, frameStart2Us = 0;  // FadeInUnseen (rev96d A2): the two frame starts before this one
    uint64_t periodRing[8] = {};  // PacingGateShownHoldMs (fix102, rev102a S2): the last 8 frame periods (us), every frame
    int periodNext = 0;
    int frameTypes[kNumTypes] = {};
    uint64_t frameFirstUs = 0;
    int frameFlags = 0;       // 1 drained, 2 lag bound, 4 near/seen units ran past the budget, 8 a flow waited
    // FIFO (ring)
    Entry* fifo = nullptr;
    size_t head = 0, count = 0;
    // test hooks
    bool testAnyCaller = false;   // also: sessions with a key >= kTestFlowKey are flows
    uint64_t (*testClock)() = nullptr;
    bool (*testPlayer)(double*, double*, double*) = nullptr;
    int testGuards = -1;     // -1: real guards; 0/1 forced
};
State g;
Stats s;

uint64_t NowUs()
{
    if (g.testClock) return g.testClock();
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return QpcToMicroseconds(static_cast<uint64_t>(t.QuadPart), static_cast<uint64_t>(freq.QuadPart));
}

uint32_t Disp32(const uint8_t* at) { uint32_t v; memcpy(&v, at, 4); return v; }

// The only .rdata qword holding this code address (a vtable slot), or nullptr.
void** FindSlot(const GameModule& game, const void* target)
{
    void** found = nullptr;
    int hits = 0;
    const uintptr_t want = reinterpret_cast<uintptr_t>(target);
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uintptr_t*>(p) == want) { found = reinterpret_cast<void**>(p); ++hits; }
    return hits == 1 ? found : nullptr;
}

int FindSlots(const GameModule& game, const void* target, void*** out, int max)
{
    int n = 0;
    const uintptr_t want = reinterpret_cast<uintptr_t>(target);
    for (uint8_t* p = game.rdata.begin; p + 8 <= game.rdata.end; p += 8)
        if (*reinterpret_cast<const uintptr_t*>(p) == want) {
            if (n < max) out[n] = reinterpret_cast<void**>(p);
            ++n;
        }
    return n;
}

uint8_t* Unique(const GameModule& game, const char* pattern, const char* what)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    if (hits.size() != 1) {
        Log("[pacing] %s not recognised (%zu matches): fix not available", what, hits.size());
        return nullptr;
    }
    return hits[0];
}

// The exec thunks registered for a native UFunction name ({const char* name, thunk} pairs in .rdata).
std::vector<uint8_t*> NativeThunks(const GameModule& game, const char* name)
{
    std::vector<uint8_t*> out;
    const size_t len = strlen(name) + 1;
    for (uint8_t* p = game.rdata.begin + 1; p + len <= game.rdata.end; ++p) {
        if (p[-1] != 0 || memcmp(p, name, len) != 0) continue;
        const uintptr_t str = reinterpret_cast<uintptr_t>(p);
        for (uint8_t* q = game.rdata.begin; q + 16 <= game.rdata.end; q += 8) {
            if (*reinterpret_cast<const uintptr_t*>(q) != str) continue;
            uint8_t* thunk = *reinterpret_cast<uint8_t**>(q + 8);
            if (game.text.Contains(thunk) && game.text.Contains(thunk + 0x100)) out.push_back(thunk);
        }
    }
    return out;
}

// First match of the pattern within [from, from+span), or nullptr.
uint8_t* FindNear(uint8_t* from, size_t span, const char* pattern)
{
    const Pattern p = ParsePattern(pattern);
    for (size_t i = 0; i + p.size() <= span; ++i)
        if (MatchesAt(from + i, p)) return from + i;
    return nullptr;
}

bool FindPairing(const GameModule& game)
{
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    uint8_t* run = Unique(game, kRunSession, "pairing RunSession");
    uint8_t* end = Unique(game, kHandlerEnd, "pairing Handler_End");
    uint8_t* step = Unique(game, kStep, "pairing session step");
    uint8_t* writeSwitch = Unique(game, kWriteSwitch, "pairing write switch");
    uint8_t* create = Unique(game, kCreateExec, "create-actor command");
    uint8_t* generic = Unique(game, kGenericExec, "generic pairing command");
    uint8_t* preRun = Unique(game, kPreTickRunCall, "pre-actor-tick RunSession(-1) call");
    uint8_t* post = Unique(game, kPostTick, "post-actor-tick handler");
    // the fade-travel flag is set in a few places; all must name the same global
    uint8_t* tes = nullptr;
    for (uint8_t* hit : FindAll(game.text, ParsePattern(kTesTravel))) {
        uint8_t* glob = RipTarget(hit, 3, 7);
        if (RipTarget(hit + 14, 3, 7) != glob || RipTarget(hit + 28, 3, 7) != glob || (tes && RipTarget(tes, 3, 7) != glob)) {
            tes = nullptr;
            break;
        }
        if (!tes) tes = hit;
    }
    if (!tes) Log("[pacing] travel flag not recognised: fix not available");
    if (!run || !end || !step || !writeSwitch || !create || !generic || !preRun || !post || !tes) return false;
    if (!MatchesAt(RipTarget(end + kHandlerEndFnLea, 3, 7), ParsePattern(kEndFunction))) {
        Log("[pacing] end command function not recognised: fix not available");
        return false;
    }
    // the manager vtable: RunSession at +0x70 and Handler_End at +0x38 of the same table
    g.runSessionSlot = FindSlot(game, run);
    g.handlerEndSlot = FindSlot(game, end);
    if (!g.runSessionSlot || !g.handlerEndSlot || g.runSessionSlot - g.handlerEndSlot != (0x70 - 0x38) / 8) {
        Log("[pacing] pairing manager vtable not found: fix not available");
        return false;
    }
    // the type-B session vtable: step at +0x28, write switch at +0x10
    void** stepSlot = FindSlot(game, step);
    if (!stepSlot || stepSlot[-3] != writeSwitch) {
        Log("[pacing] pairing session vtable not found: fix not available");
        return false;
    }
    g.sessionVtable = stepSlot - 5;
    g.createSlot = FindSlot(game, create);
    if (!g.createSlot) {
        Log("[pacing] create-actor command vtable not found: fix not available");
        return false;
    }
    g.numGenericSlots = FindSlots(game, generic, g.genericSlots, 8);
    if (g.numGenericSlots < 1 || g.numGenericSlots > 8) {
        Log("[pacing] generic pairing command vtables not found (%d): fix not available", g.numGenericSlots);
        return false;
    }
    // both commands check cancellation and release their entry through the same functions
    if (RipTarget(create + 0x19, 1, 5) != RipTarget(generic + 0x12, 1, 5)) {
        Log("[pacing] create and generic commands differ in their entry check: fix not available");
        return false;
    }
    g.runSession = run;
    g.handlerEnd = end;
    g.findSession = reinterpret_cast<FindSessionFn>(RipTarget(run + kRunSessionFindCall, 1, 5));
    g.createExec = create;
    g.genericExec = generic;
    g.preTickReturn = preRun + kPreTickRunReturn;
    g.postTickReturn = post + kPostTickEndReturn;
    g.tes = reinterpret_cast<uint8_t**>(RipTarget(tes, 3, 7));
    if (RipTarget(tes + 14, 3, 7) != reinterpret_cast<uint8_t*>(g.tes) || !game.rdata.Contains(g.runSessionSlot)) {
        Log("[pacing] travel flag global not recognised: fix not available");
        return false;
    }
    g.origRun = reinterpret_cast<RunSessionFn>(run);
    g.origEnd = reinterpret_cast<HandlerEndFn>(end);
    g.origCreate = reinterpret_cast<CmdExecFn>(create);
    g.origGeneric = reinterpret_cast<CmdExecFn>(generic);
    // (c) optional: the flow runner (the post-tick handler's first call) and its two RunSession calls, storing their
    // result in one global; and the writer-session function Handler_End uses. Without them flows drain as before.
    g.flowReturn[0] = g.flowReturn[1] = nullptr;
    g.chooseWriter = nullptr;
    uint8_t* runner = post[kPostTickFlowCall] == 0xE8 ? RipTarget(post + kPostTickFlowCall, 1, 5) : nullptr;
    if (runner && game.text.Contains(runner) && game.text.Contains(runner + kFlowRunnerSpan)) {
        const Pattern fp = ParsePattern(kFlowRun);
        uint8_t* calls[3] = {};
        int n = 0;
        for (size_t i = 0; i + fp.size() <= kFlowRunnerSpan; ++i)
            if (MatchesAt(runner + i, fp)) {
                if (n < 3) calls[n] = runner + i;
                ++n;
            }
        uint8_t* writer = RipTarget(end + kHandlerEndWriterCall, 1, 5);
        if (n == 2 && RipTarget(calls[0] + kFlowRunReturn, 2, 6) == RipTarget(calls[1] + kFlowRunReturn, 2, 6) &&
            end[kHandlerEndWriterCall] == 0xE8 && game.text.Contains(writer)) {
            g.flowReturn[0] = calls[0] + kFlowRunReturn;
            g.flowReturn[1] = calls[1] + kFlowRunReturn;
            g.chooseWriter = reinterpret_cast<ChooseWriterFn>(writer);
            Log("[pacing] level flows: runner exe+0x%X, RunSession calls return to exe+0x%X / exe+0x%X, writer session exe+0x%X",
                rva(runner), rva(g.flowReturn[0]), rva(g.flowReturn[1]), rva(writer));
        } else Log("[pacing] level flow runner not recognised (%d calls): flows drain the queue as before", n);
    } else Log("[pacing] level flow runner not found: flows drain the queue as before");
    Log("[pacing] pairing: RunSession exe+0x%X (slot exe+0x%X), Handler_End exe+0x%X, session vtable exe+0x%X, "
        "create command exe+0x%X (slot exe+0x%X), generic command exe+0x%X (%d slots), pre-tick call exe+0x%X, "
        "post-tick exe+0x%X, TES exe+0x%X",
        rva(run), rva(g.runSessionSlot), rva(end), rva(g.sessionVtable), rva(create), rva(g.createSlot), rva(generic),
        g.numGenericSlots, rva(preRun), rva(post), rva(g.tes));
    return true;
}

bool FindDelegate(const GameModule& game, uint8_t* execIfSafe, DelegateType& d, const char* name)
{
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    void** slot = FindSlot(game, execIfSafe);
    if (!slot) {
        Log("[pacing] %s delegate vtable not found: fix not available", name);
        return false;
    }
    void** vt = slot - kSlotExecuteIfSafe;
    uint8_t* dtor = static_cast<uint8_t*>(vt[kSlotDtor]);
    uint8_t* copy = static_cast<uint8_t*>(vt[kSlotCreateCopy]);
    uint8_t* exec = static_cast<uint8_t*>(vt[kSlotExecute]);
    if (!game.text.Contains(dtor) || !game.text.Contains(copy) || !game.text.Contains(exec)) {
        Log("[pacing] %s delegate vtable exe+0x%X not recognised: fix not available", name, rva(vt));
        return false;
    }
    // CreateCopy must write this vtable (so the slots belong to this delegate type)
    uint8_t* header = FindNear(copy, 0x90, kCopyHeader);
    if (!header || RipTarget(header + 8, 3, 7) != reinterpret_cast<uint8_t*>(vt)) {
        Log("[pacing] %s delegate CreateCopy not recognised: fix not available", name);
        return false;
    }
    uint8_t* ctorCall = header + 26;
    const bool hasCtor = MatchesAt(ctorCall, ParsePattern(kCopyCtorCall));
    if (MatchesAt(dtor, ParsePattern(kTrivialDtor)) && !hasCtor) {
        d.size = dtor[kTrivialDtorSize];
        d.copyCtor = nullptr;
    } else if (MatchesAt(dtor, ParsePattern(kMemberDtorStart)) && hasCtor) {
        uint8_t* fr = FindNear(dtor, 0x40, kMemberDtorFree);
        if (!fr) {
            Log("[pacing] %s delegate destructor not recognised: fix not available", name);
            return false;
        }
        d.size = Disp32(fr + 7);
        d.copyCtor = reinterpret_cast<CopyCtorFn>(RipTarget(ctorCall + 4, 1, 5));
    } else {
        Log("[pacing] %s delegate layout not recognised (destructor exe+0x%X): fix not available", name, rva(dtor));
        return false;
    }
    if (d.size < kCaptureOffset || d.size > kMaxInstance) {
        Log("[pacing] %s delegate size 0x%zX unexpected: fix not available", name, d.size);
        return false;
    }
    d.vtable = vt;
    d.execIfSafe = reinterpret_cast<ExecIfSafeFn>(execIfSafe);
    d.execute = reinterpret_cast<ExecFn>(exec);
    d.dtor = reinterpret_cast<DtorFn>(dtor);
    Log("[pacing] %s delegate: vtable exe+0x%X, ExecuteIfSafe exe+0x%X, size 0x%zX, capture %s", name, rva(vt),
        rva(execIfSafe), d.size, d.copyCtor ? "copied by its constructor" : "copied as bytes");
    return true;
}

bool FindDelegates(const GameModule& game)
{
    uint8_t* app = Unique(game, kAppearanceExec, "appearance delegate");
    if (!app) return false;
    uint8_t* const isValid = RipTarget(app + kAppearanceIsValid, 1, 5);
    g.weakGet = reinterpret_cast<WeakGetFn>(RipTarget(app + kAppearanceGet, 1, 5));
    uint8_t *equip = nullptr, *hair = nullptr;
    int ne = 0, nh = 0;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kLambdaExec))) {
        if (RipTarget(p + kLambdaIsValid, 1, 5) != isValid) continue;
        uint8_t* lambda = RipTarget(p + kLambdaCall, 1, 5);
        if (!game.text.Contains(lambda)) continue;
        if (MatchesAt(lambda, ParsePattern(kEquipmentLambda))) { equip = p; ++ne; }
        if (MatchesAt(lambda, ParsePattern(kHairLambda))) { hair = p; ++nh; }
    }
    if (ne != 1 || nh != 1) {
        Log("[pacing] equipment / hair delegates not recognised (%d / %d matches): fix not available", ne, nh);
        return false;
    }
    {   // PacingHairActor: the hair lambda tests and reads the capture's first weak pointer with the same IsValid / Get as
        // the appearance delegate, and calls SetSkeletalMesh on that object (pattern: "mov rcx,rsi; call Get; ... call
        // [vtable+0A68h]"); otherwise hair units stay unresolved (counted as near), as before
        const uint8_t* const hl = RipTarget(hair + kLambdaCall, 1, 5);
        if (RipTarget(hl + kHairLambdaIsValid, 1, 5) == isValid &&
            RipTarget(hl + kHairLambdaGet, 1, 5) == reinterpret_cast<uint8_t*>(g.weakGet))
            g.hairActorWeak = static_cast<uint32_t>(kCaptureOffset);
        else
            Log("[pacing] hair delegate's mesh component not recognised: PacingHairActor not available");
    }
    return FindDelegate(game, app, g.del[0], "appearance") && FindDelegate(game, equip, g.del[1], "equipment") &&
           FindDelegate(game, hair, g.del[2], "hair");
}

bool FindPlayer(const GameModule& game)
{
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    uint8_t* tail = nullptr;
    for (uint8_t* t : NativeThunks(game, "GetPlayerPawn"))
        if (uint8_t* at = FindNear(t, 0xE0, kGetPawnTail)) { tail = at; break; }
    uint8_t* loc = nullptr;
    for (uint8_t* t : NativeThunks(game, "K2_GetActorLocation"))
        if (uint8_t* at = FindNear(t, 0x40, kActorLocation)) { loc = at; break; }
    uint8_t* world = Unique(game, kGetCurrentPlayWorld, "GetCurrentPlayWorld");
    uint8_t* rendered = nullptr;
    for (uint8_t* t : NativeThunks(game, "WasRecentlyRendered"))
        if (uint8_t* at = FindNear(t, 0x120, kRecentlyRendered)) { rendered = at; break; }
    if (!rendered) {
        Log("[pacing] WasRecentlyRendered not recognised: fix not available");
        return false;
    }
    g.actorWorld = reinterpret_cast<ActorWorldFn>(RipTarget(rendered + 3, 1, 5));
    g.deltaOffset = Disp32(rendered + 31);
    g.lastRenderSlot = Disp32(rendered + 45);
    g.timeOffset = Disp32(rendered + 53);
    if (g.deltaOffset > 0x2000 || g.timeOffset > 0x2000 || g.lastRenderSlot > 0x2000 || g.lastRenderSlot % 8 ||
        !game.text.Contains(reinterpret_cast<uint8_t*>(g.actorWorld))) {
        Log("[pacing] WasRecentlyRendered layout not recognised: fix not available");
        return false;
    }
    if (!tail || !loc || !world) {
        Log("[pacing] player position functions not found (GetPlayerPawn %d, K2_GetActorLocation %d): fix not available",
            tail != nullptr, loc != nullptr);
        return false;
    }
    uint8_t* pc = RipTarget(tail + 5, 1, 5);
    uint8_t* pawn = RipTarget(tail + 18, 1, 5);
    if (!MatchesAt(pc, ParsePattern(kGetPlayerController)) || !MatchesAt(pawn, ParsePattern(kGetPawn))) {
        Log("[pacing] GetPlayerController / GetPawnOrSpectator not recognised: fix not available");
        return false;
    }
    g.gEngine = reinterpret_cast<void**>(RipTarget(pc + kGEngineMov, 3, 7));
    g.rootOffset = Disp32(loc + 3);
    g.locationOffset = Disp32(loc + 15);
    if (Disp32(loc + 26) != g.locationOffset + 0x10 || g.rootOffset > 0x1000 || g.locationOffset > 0x1000) {
        Log("[pacing] actor location layout not recognised: fix not available");
        return false;
    }
    g.getWorld = reinterpret_cast<GetWorldFn>(world);
    g.getPc = reinterpret_cast<GetPcFn>(pc);
    g.getPawn = reinterpret_cast<GetPawnFn>(pawn);
    Log("[pacing] player: GEngine exe+0x%X, GetCurrentPlayWorld exe+0x%X, GetPlayerController exe+0x%X, "
        "GetPawnOrSpectator exe+0x%X, RootComponent +0x%X, location +0x%X; rendered: world exe+0x%X, delta +0x%X, "
        "time +0x%X, GetLastRenderTime slot +0x%X",
        rva(g.gEngine), rva(world), rva(pc), rva(pawn), g.rootOffset, g.locationOffset,
        rva(reinterpret_cast<void*>(g.actorWorld)), g.deltaOffset, g.timeOffset, g.lastRenderSlot);
    return true;
}

// PacingFadeAware: the fade component's layout from its tick (optional; without it the key does nothing and the pacing
// works as in 1.2.2).
bool FindFade(const GameModule& game)
{
    const auto hits = FindAll(game.text, ParsePattern(kFadeTick));
    if (hits.size() != 1) {
        Log("[pacing] spawn fade-in component tick not recognised (%zu matches): PacingFadeAware not available (the rest of "
            "the pacing works as before)", hits.size());
        return false;
    }
    uint8_t* const t = hits[0];
    g.fadeState = Disp32(t + kFadeStateAt);
    g.fadeValue = Disp32(t + kFadeValueAt);
    g.fadeOwner = Disp32(t + kFadeOwnerAt);
    g.fadeComp = Disp32(t + kFadeCompAt);
    g.fadeFloor = Disp32(t + kFadeFloorAt);
    uint8_t* const cls = RipTarget(t + kFadeClassCallAt, 1, 5);
    if (g.fadeState > 0x400 || g.fadeValue > 0x400 || g.fadeOwner > 0x400 || g.fadeFloor > 0x400 || g.fadeComp > 0x4000 ||
        g.fadeComp % 8 || g.fadeOwner % 8 || !game.text.Contains(cls)) {
        Log("[pacing] spawn fade-in component layout not recognised: PacingFadeAware not available (the rest of the pacing "
            "works as before)");
        return false;
    }
    g.fadeClass = reinterpret_cast<StaticClassFn>(cls);
    g.fadeFound = true;
    Log("[pacing] spawn fade-in component: tick exe+0x%X, state +0x%X, value +0x%X, owner +0x%X, floor +0x%X, actor's "
        "component +0x%X, owner class exe+0x%X",
        static_cast<unsigned>(t - game.base), g.fadeState, g.fadeValue, g.fadeOwner, g.fadeFloor, g.fadeComp,
        static_cast<unsigned>(cls - game.base));
    return true;
}

// occlvis (2026-10-23): what drew an actor. UE 5.3 FPrimitiveSceneInfo::UpdateComponentLastRenderTime sets the
// component's LastRenderTime and the owner actor's (what WasRecentlyRendered reads) from three places: the views'
// visibility after frustum and occlusion culling (SceneVisibility.cpp ~1812, also LastRenderTimeOnScreen), every
// shadow-caster gathering (ShadowSetup.cpp ~1839 / ~2057, not on screen) and ray-traced dynamic geometry within
// r.RayTracing.DynamicGeometryLastRenderTimeUpdateDistance of the view (DeferredShadingRenderer.cpp ~1400, on screen).
// UPrimitiveComponent::WasRecentlyRendered(float) (exe+0x304DDE0): world [+0B0h] (or GetWorld), DeltaTimeSeconds
// [world+6E4h], TimeSeconds [world+6C0h], LastRenderTime [comp+384h]; the constructor sets +384h and +388h to -1000.
constexpr char kCompRendered[] =
    "40 55 48 83 EC 30 48 8B 81 ?? ?? 00 00 48 8B E9 0F 29 74 24 20 0F 28 F1 48 85 C0 75 ?? E8 ?? ?? ?? ?? 48 85 C0 "
    "0F 84 ?? ?? ?? ?? F3 0F 10 88 ?? ?? 00 00 F3 0F 58 0D ?? ?? ?? ?? F2 0F 10 90 ?? ?? 00 00 48 8B 05 ?? ?? ?? ?? "
    "F3 0F 10 85 ?? ?? 00 00";
constexpr size_t kCompTimeAt = 63, kCompLastAt = 78;


// ---- positions (game thread) ----

// [root + location] of an actor, if obj is one: its root component's outer is the actor itself.
bool ActorLocation(const uint8_t* obj, double* x, double* y, double* z)
{
    const uint8_t* root = *reinterpret_cast<const uint8_t* const*>(obj + g.rootOffset);
    if (!root || *reinterpret_cast<const uint8_t* const*>(root + kUObjectOuter) != obj) return false;
    const double* l = reinterpret_cast<const double*>(root + g.locationOffset);
    *x = l[0];
    *y = l[1];
    *z = l[2];
    return std::isfinite(*x) && std::isfinite(*y) && std::isfinite(*z);
}

// ActorLocation for any object of an owner chain: a smaller object (an equipment or hair delegate's owner, e.g. the
// character's body pairing component) has no root component at that offset, and the word read there can be an invalid
// pointer. That object is no actor; the walk goes on with its outer (ClothBuildDefer's owner lookup; pacing's own walk with
// PacingOwnerWalk, mkusm's code from the first version of public PR #25). With PacingOwnerWalk=0 pacing's own walk
// (ResolveDelegate) ends there: the unit counts as near (runs at once).
bool ActorLocationGuarded(const uint8_t* obj, double* x, double* y, double* z)
{
    __try {
        return ActorLocation(obj, x, y, z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPlayer(double* x, double* y, double* z)
{
    __try {
        void* engine = *g.gEngine;
        if (!engine) return false;
        void* world = g.getWorld(engine, nullptr);
        if (!world) return false;
        void* pc = g.getPc(world, 0);
        if (!pc) return false;
        const uint8_t* pawn = static_cast<const uint8_t*>(g.getPawn(pc));
        if (!pawn) return false;
        const uint8_t* root = *reinterpret_cast<const uint8_t* const*>(pawn + g.rootOffset);
        if (!root) return false;
        if (!g.outerChecked) {
            g.outerChecked = true;
            g.outerOk = *reinterpret_cast<const uint8_t* const*>(root + kUObjectOuter) == pawn;
            Log("[pacing] object layout check (the player's root component belongs to the player): %s",
                g.outerOk ? "ok" : "failed, delegates are never deferred");
        }
        const double* l = reinterpret_cast<const double*>(root + g.locationOffset);
        *x = l[0];
        *y = l[1];
        *z = l[2];
        return std::isfinite(*x) && std::isfinite(*y) && std::isfinite(*z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Player()
{
    if (g.playerFrame != g.frame) {
        g.playerFrame = g.frame;
        g.playerKnown = g.testPlayer ? g.testPlayer(&g.px, &g.py, &g.pz) : ReadPlayer(&g.px, &g.py, &g.pz);
    }
    return g.playerKnown;
}

// AActor::WasRecentlyRendered(kRenderedWithinS), as the engine computes it (game thread, inside ResolveDelegate's guard).
bool RecentlyRendered(const uint8_t* actor)
{
    if (g.testRendered) return g.testRendered(actor);
    if (!g.actorWorld) return false;
    const uint8_t* world = static_cast<const uint8_t*>(g.actorWorld(actor));
    if (!world) return false;
    const float delta = *reinterpret_cast<const float*>(world + g.deltaOffset);
    const double now = *reinterpret_cast<const double*>(world + g.timeOffset);
    void* const* vt = *reinterpret_cast<void* const* const*>(actor);
    const float last = reinterpret_cast<LastRenderFn>(vt[g.lastRenderSlot / 8])(actor);
    return now - static_cast<double>(last) <= std::max(static_cast<double>(delta), kRenderedWithinS);
}

// PacingFadeAware: the actor is in its spawn fade-in and still (almost) invisible: FadeIn and max(floor, value) at or
// above 3.0 (lab: PacingFadeMin / 10), read as the component's tick reads it (owner class check first). Dither 5 is fully
// invisible; >= 3 is about the first 0.75 s of the 1.25 s fade (findings 2026-10-26 npcjitter). Game thread, caller's guard.
float FadeLevel(const uint8_t* actor)  // the actor's spawn fade-in dither, -1 = not fading in / not a faded character
{
    if (g.testFade) return actor ? g.testFade(actor) : -1.0f;
    if (!g.fadeFound || !actor) return -1.0f;
    const uint8_t* const cls = g.fadeClass();
    const uint8_t* const oc = *reinterpret_cast<const uint8_t* const*>(actor + 0x10);
    if (!cls || !oc) return -1.0f;
    const int32_t idx = *reinterpret_cast<const int32_t*>(cls + 0x38);
    if (idx < 0 || idx > *reinterpret_cast<const int32_t*>(oc + 0x38)) return -1.0f;
    const uint8_t* const* chain = *reinterpret_cast<const uint8_t* const* const*>(oc + 0x30);
    if (!chain || chain[idx] != cls + 0x30) return -1.0f;
    const uint8_t* const comp = *reinterpret_cast<const uint8_t* const*>(actor + g.fadeComp);
    if (!comp || *reinterpret_cast<const uint8_t* const*>(comp + g.fadeOwner) != actor) return -1.0f;
    if (*reinterpret_cast<const int32_t*>(comp + g.fadeState) != 1) return -1.0f;
    const float v = std::max(*reinterpret_cast<const float*>(comp + g.fadeValue), *reinterpret_cast<const float*>(comp + g.fadeFloor));
    // PacingGateShownHoldMs (hold2): the level shown on the meshes (inert at 0: the game's value)
    return ShownHoldOn() ? std::max(v, ShownHoldLevel(comp)) : v;
}

bool FadeHidden(const uint8_t* actor)
{
    return FadeLevel(actor) * 10.0f >= static_cast<float>(g.fadeMinTenths.load(std::memory_order_relaxed));
}


// The weak pointer a delegate unit's actor is found through: its owner (+0x18), or for a hair unit with PacingHairActor
// the hair mesh component its lambda sets the mesh on (the owner is the hair asset). hair: set when the component is used
// (its walk must be the guarded one: a component's word at the root-component offset is no root component).
const uint8_t* OwnerWeakOf(int type, const uint8_t* inst, bool* viaComponent = nullptr)
{
    const bool comp = type == kHair && g.hairActorWeak && g.hairActor.load(std::memory_order_relaxed);
    if (viaComponent) *viaComponent = comp;
    return inst + (comp ? g.hairActorWeak : kOwnerWeakOffset);
}

bool FarFrom(double x, double y, double z)
{
    if (!Player()) return false;
    const double dx = x - g.px, dy = y - g.py, dz = z - g.pz;
    const double d = g.minDistance.load(std::memory_order_relaxed);
    return dx * dx + dy * dy + dz * dz > d * d;
}

// A delegate's owner (the weak pointer at +0x18) -> the actor it belongs to (itself or an outer); distant = farther than
// the minimum distance and not rendered recently (PacingFadeAware: or still invisible in its spawn fade-in). Unknown
// counts as near (runs as before). guarded: an object that faults at the root-component offset is skipped instead of
// ending the walk (ClothBuildDefer's questions; also pacing's own walk with PacingOwnerWalk). log: pacing's own
// call (the first 16 resolutions are logged; lab: its counters).
bool ResolveOwner(const uint8_t* ownerWeak, uintptr_t* key, bool* distant, bool log, bool guarded)
{
    *key = 0;
    *distant = false;
    g.lastFadeHeld = false;
    if (!g.testPlayer && (!g.outerOk || !g.weakGet)) return false;
    __try {
        const uint8_t* obj = static_cast<const uint8_t*>(g.weakGet(ownerWeak));
        const bool walk = guarded || g.ownerWalk.load(std::memory_order_relaxed);
        for (int i = 0; obj && i < 4; ++i) {
            double x, y, z;
            if (walk ? ActorLocationGuarded(obj, &x, &y, &z) : ActorLocation(obj, &x, &y, &z)) {
                *key = reinterpret_cast<uintptr_t>(obj);
                const bool far1 = FarFrom(x, y, z);
                bool seen = far1 && RecentlyRendered(obj);
                *distant = far1 && !seen;
                {   // PacingFadeAware: far and rendered, but still invisible in its spawn fade-in: unseen, so it may wait
                    // like a hidden far NPC (the frustum/occlusion "rendered" test counts primitives that are drawn fully
                    // dithered away; npcjitter, Namira's Shrine). Lab: 2 = near ones too, 3 = equipment/hair only.
                    const int fa = g.fadeAware.load(std::memory_order_relaxed);
                    bool fadeWait = fa && !*distant && far1 && FadeHidden(obj);
                    if (fadeWait) {
                        *distant = true;
                        g.lastFadeHeld = true;
                    }
                }
                if (log && g.delegateLogs < 16) {
                    ++g.delegateLogs;
                    Log("[pacing] delegate unit: actor at (%.0f, %.0f, %.0f), %.0f from the player, %s: %s", x, y, z,
                        std::sqrt((x - g.px) * (x - g.px) + (y - g.py) * (y - g.py)), seen ? "rendered" : "not rendered",
                        *distant ? "may wait" : "runs now");
                }
                return true;
            }
            obj = *reinterpret_cast<const uint8_t* const*>(obj + kUObjectOuter);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool ResolveDelegate(const uint8_t* inst, int type, uintptr_t* key, bool* distant)
{
    bool comp = false;
    const uint8_t* const weak = OwnerWeakOf(type, inst, &comp);
    return ResolveOwner(weak, key, distant, true, comp);
}

// ClothBuildDefer's questions (asked from inside a unit, or at the frame start): the same decision as pacing's with the
// guarded walk, leaving what pacing's own resolution set for its caller as it was (lastFadeHeld; lab: the decision bits
// and the type it resolves for). Port of PR #31 onto the lab (lab-only state is restored too).
bool ResolveOwnerQuiet(const uint8_t* ownerWeak, uintptr_t* key, bool* distant)
{
    const bool fadeHeld = g.lastFadeHeld;
    const bool r = ResolveOwner(ownerWeak, key, distant, false, true);
    g.lastFadeHeld = fadeHeld;
    return r;
}

// ---- guards (game thread) ----

int RingDistance(const uint8_t* sess)
{
    const uint8_t* pool = *reinterpret_cast<const uint8_t* const*>(sess + kSessPool);
    const int n = *reinterpret_cast<const int32_t*>(pool + kPoolCount);
    if (n <= 0) return 1 << 20;
    const int w = *reinterpret_cast<const int16_t*>(sess + kSessWriteIdx);
    const int r = *reinterpret_cast<const int16_t*>(sess + kSessReadIdx);
    return ((w - r) % n + n) % n;
}

bool GuardsPass(const uint8_t* sess)
{
    if (g.testGuards >= 0) return g.testGuards != 0;
    int why = -1;
    int32_t named = -1, mode = -1;
    const uint8_t* tes = *g.tes;
    if (!sess || *reinterpret_cast<void* const*>(sess) != g.sessionVtable) why = 0;
    else if (!tes || tes[kTesTravelPending]) why = 1;
    else if (!g.freezeKnown || !ReadMenuFreeze(&named, &mode) || named != 0 || mode != 0) why = 2;
    else {
        const int ring = RingDistance(sess);
        AtomicMax(s.maxRing, static_cast<uint64_t>(ring));
        if (ring > 8) why = 3;
        else if (!Player()) why = 4;
    }
    if (why >= 0) {
        s.guard[why].fetch_add(1, std::memory_order_relaxed);
        static const char* const kWhy[5] = {"session", "travel", "menu/loading", "ring", "player"};
        if ((g.count || g.E + 1 < g.S) && s.frameLogs.fetch_add(1, std::memory_order_relaxed) < 40)
            Log("[pacing] frame %llu: not deferring (%s, freeze %d/%d) with a backlog of %zu delegates, %llu OT frames: runs now",
                static_cast<unsigned long long>(g.frame), kWhy[why], named, mode, g.count,
                static_cast<unsigned long long>(g.S - g.E - 1));
    }
    return why < 0;
}

// ---- units ----

void Account(int type, uint64_t us)
{
    if (g.units == 0) g.frameFirstUs = us;
    g.frameTypes[type]++;
    g.units++;
    g.unitUs += us;
    g.spentUs += us;
    g.unitCumUs += us;
    ++g.unitCumN;
    g.est[type] = g.est[type] * 0.75 + static_cast<double>(us) * 0.25;
    g.gateLast[type][g.gateLastAt[type]] = static_cast<uint32_t>(std::min<uint64_t>(us, 0xFFFFFFFFull));  // PacingBaseGate
    g.gateLastAt[type] = (g.gateLastAt[type] + 1) % 8;
    if (g.gateLastN[type] < 8) ++g.gateLastN[type];
    s.run[type].fetch_add(1, std::memory_order_relaxed);
    if (us > s.longestUs.load(std::memory_order_relaxed)) {
        AtomicMax(s.longestUs, us);
        s.longestType = type;
    }
}

#define LAB_UNIT(t, us, a, p, l) ((void)0)

// Whether a distant unit of this type must wait: the first unit of a frame always runs, then the budget decides.
// The budget grows with the lag of the work waiting (lag = frames it has waited): x1 up to half of PacingMaxLagFrames,
// then +1 per quarter of it (x3 at PacingMaxLagFrames), so a long backlog catches up a few units per frame instead
// of all at once; beyond twice PacingMaxLagFrames everything runs (PairForced / forced replay).
uint64_t LagBudget(uint64_t lag)
{
    const uint64_t maxLag = static_cast<uint64_t>(g.maxLag.load(std::memory_order_relaxed));
    const uint64_t half = maxLag / 2, quarter = maxLag / 4 ? maxLag / 4 : 1;
    const uint64_t scale = 1 + (lag > half ? (lag - half + quarter - 1) / quarter : 0);
    return g.budgetThisFrame * scale;
}

bool OverBudget(int type, uint64_t lag = 0)
{
    if (g.units == 0) return false;
    return g.spentUs + static_cast<uint64_t>(g.est[type]) > LagBudget(lag);
}

uint64_t PairLag()  // OT frames the head of the queue is behind
{
    return g.S > g.E + 1 ? g.S - g.E - 1 : 0;
}

bool PairForced() { return PairLag() > 2 * static_cast<uint64_t>(g.maxLag.load(std::memory_order_relaxed)); }

Entry& Front() { return g.fifo[g.head]; }

uint64_t FifoAge() { return g.count ? g.frame - Front().since : 0; }

bool FifoHasKey(uintptr_t key)
{
    if (!key) return true;  // unknown: keep the order
    for (size_t i = 0; i < g.count; ++i)
        if (g.fifo[(g.head + i) % kFifoCap].key == key) return true;
    return false;
}

bool ResolveDelegate(const uint8_t* inst, int type, uintptr_t* key, bool* distant);
// PacingFadeAware: whether a waiting entry may still wait (its actor far and unseen, or still fade-hidden); an actor that
// cannot be resolved any more runs (its ExecuteIfSafe re-checks the weak pointer).
bool StillMayWait(const Entry& e)
{
    uintptr_t key = 0;
    bool distant = false;
    const bool resolved = ResolveDelegate(e.inst, e.type, &key, &distant);
    if (!resolved) return false;
    if (!distant && e.fadeHeld) g.fadeReleased.fetch_add(1, std::memory_order_relaxed);
    return distant;
}



// ---- PacingBaseGate (basegate 2026-11-12; per-unit decision and hard bound: gate2 2026-11-13) ----
// The gate decides for each queued unit, not for the whole queue:
// - A "held" unit (W) is an appearance / equipment / hair unit queued as a far one whose actor is, now: found, still far and
//   unseen by pacing's own test (or still hidden in its spawn fade-in: PacingFadeAware), at least PacingGateNearM away, and,
//   if it is fading in, still at the fade's floor or above (dither >= kGateHiddenMin; the game holds far actors at 4.0).
//   Held units keep their FIFO order among themselves; the oldest runs only after the world's actor tick (the post-actor-
//   tick replay, and delegate arrivals after it), when the frame has room: the game thread's time since the frame started
//   (the end of the cap's sleep) + the predicted rest of the frame (median of the last five frames' time from the post-actor
//   tick to the cap's sleep, without units) + the unit's size (upper quartile of the last eight of its type) stays within
//   the target (PacingBaseGate), or (one a frame) when the frame's own time + its predicted rest stays below
//   PacingGateCalmMs whatever the unit's size ("calm" frames; a unit too big to fit anywhere waits for one of those
//   instead of the bound). At most one unit of 4 ms or more a frame runs for room. No lag-scaled catch-up for held units.
//   Hard bound: once waiting longer would let a held unit pass PacingGateBoundFrames at one unit per frame, the frame runs
//   the fewest held units (oldest first) that keep every held unit within the bound at an even rate (one per frame for
//   fewer held units than the bound; gatefix D1's rate rule over the held units' positions): no held unit runs older than
//   the bound, and never as a heap.
// - Every other queued unit (U: a near or seen actor's, one about to show, an unresolved one, or one queued as near) runs
//   as without the gate (today's budget rule, PacingHairActor's fade rate over these units only), in queue order among
//   themselves and AHEAD of other actors' held units (an actor's own units keep their order: once one of its units may no
//   longer wait, all of its queued units count as U). Overtaking another actor's queued unit is what a near unit's arrival
//   already does without the gate; units of different actors do not depend on each other.
// - A key-0 unit (its actor not found) may belong to any actor: everything queued before it runs first, by today's rule,
//   as before the change (gatefix L2), counted.
// The old version dropped the gate for the whole call as soon as any queued actor had begun its fade-in (dither below 5:
// every far actor at the floor 4.0) or a near unit was queued, so the far hidden units ran by the lag-scaled budget onto
// busy frames (Imperial City Waterfront: 8-14 ms appearances on 20-26 ms frames after 7-10 frames, two drops per flight).
// Creates and the head/BP commands are not gated.
constexpr float kGateHiddenMin = 3.95f;  // the fade floor 4.0 = (1 - 0.2) x 5 in floats, which may read 3.9999998

bool GateActive()
{
    return g.gateUs.load(std::memory_order_relaxed) > 0 && g.allowDefer && g.lastPaceEnd &&
           (g.origMaxTick || g.testClock) && g.enabled.load(std::memory_order_relaxed);
}

bool Gated(const Entry& e)
{
    return e.distant;
}

// After this frame's post-actor tick and before the next cap sleep (delegate arrivals in the world-less tickables)
bool GateKnown() { return g.gateS3Frame == g.frame && !g.inFrame && g.gateS3Us >= g.lastPaceEnd; }

uint64_t GateRest()
{
    if (!g.gateRestN) return 4000;
    uint32_t v[5];
    const int n = g.gateRestN;
    for (int i = 0; i < n; ++i) v[i] = g.gateRest[i];
    std::sort(v, v + n);
    return v[n / 2];
}

uint64_t GateSize(int type)
{
    const int n = g.gateLastN[type];
    if (!n) return static_cast<uint64_t>(g.est[type]);
    uint32_t v[8];
    for (int i = 0; i < n; ++i) v[i] = g.gateLast[type][i];
    std::sort(v, v + n);
    return v[std::min(n - 1, 3 * n / 4)];
}

// Whether a unit of this type, run now, keeps the frame within the target (and the prediction kept for the check)
bool GateFits(int type)
{
    const uint64_t now = NowUs();
    if (now < g.lastPaceEnd) return false;
    const uint64_t pred = (now - g.lastPaceEnd) + GateRest() + GateSize(type);
    if (pred > static_cast<uint64_t>(g.gateUs.load(std::memory_order_relaxed))) return false;
    g.gatePredUs = pred;
    g.gatePredFrame = g.frame;
    return true;
}

// The dither of a fading actor (-1: not fading in, or not readable)
float GateFade(uintptr_t key)
{
    if (!key) return -1.0f;
    __try {
        return FadeLevel(reinterpret_cast<const uint8_t*>(key));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

// PacingGateShownHoldMs (hold2): the actor's own spawn fade-in value as the game has it (not the floor, not the shown
// level); -1 = not in its spawn fade-in. Its end (value 0) switches the character's AI controller on (rev96e B1).
// holdfix F7 (rev96g): given as the value it would have at the stock rate of 4 per second (value x 4 / rate: 4 x the
// seconds to the fade's end), so that the due thresholds are times, not values that assume the rate
constexpr float kStockFadeRate = 4.0f;
float GateGameValue(uintptr_t key)
{
    if (!key) return -1.0f;
    __try {
        const uint8_t* const actor = reinterpret_cast<const uint8_t*>(key);
        if (g.testGameValue) return g.testGameValue(actor);
        const float level = FadeLevel(actor);  // the class, own-component and state-1 checks
        if (level < 0.0f || g.testFade) return level;
        const uint8_t* const comp = *reinterpret_cast<const uint8_t* const*>(actor + g.fadeComp);
        const float v = *reinterpret_cast<const float*>(comp + g.fadeValue);
        const float rate = *reinterpret_cast<const float*>(comp + g.fadeValue + 4);  // +0xC0 after the value +0xBC (GateFadeDt)
        return rate >= 0.5f && rate <= 20.0f ? v * kStockFadeRate / rate : v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

// PacingGateShownHoldMs (rev96g follow-up, PacingHairActor's fade rate): how much faster than the game's fade the shown
// level of this actor falls (1: as the game's; > 1 while a released hold converges, hRel / vRel)
float GateShownSpeed(uintptr_t key)
{
    if (!key || !ShownHoldOn() || g.testFade) return 1.0f;
    __try {
        const uint8_t* const comp = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<const uint8_t*>(key) + g.fadeComp);
        return comp ? ShownHoldSpeed(comp) : 1.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
}

// The actor is at least PacingGateNearM from the player (false when its position or the player's is not known)
bool GateFar(uintptr_t key)
{
    if (!key || !Player()) return false;
    double x = 0, y = 0, z = 0;
    __try {
        if (!ActorLocationGuarded(reinterpret_cast<const uint8_t*>(key), &x, &y, &z)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const double dx = x - g.px, dy = y - g.py, dz = z - g.pz;
    const double d = static_cast<double>(g.gateNearCm.load(std::memory_order_relaxed));
    return dx * dx + dy * dy + dz * dz >= d * d;
}

// Whether a unit of this actor may be held by the gate: found, still far and unseen (or fade-hidden) by pacing's own test,
// at least PacingGateNearM away, and not past the fade's floor (a fading actor below kGateHiddenMin shows within a quarter
// second: the fade drops 4 per second and counts as visible below 3)
bool GateMayHold(bool resolved, uintptr_t key, bool distant)
{
    if (!resolved || !key || !distant) return false;
    const float fade = GateFade(key);
    if (fade >= 0.0f && fade < kGateHiddenMin) return false;
    return GateFar(key);
}

// The queue's classes for the gate: kGateHeld (W) or one of the U kinds; at most kGateScan entries are classified (the
// rest count as held: they cannot run before the classified ones, and the bound counts their ages)
enum : uint8_t { kGateHeld = 0, kGateRuns = 1, kGateKey0 = 2 };
// PacingGateShownHoldMs (hold2): a gated unit whose character's own fade value is near the fade's end (its end switches
// the AI controller on, rev96e B1): soft (a few a frame, the spread's rate) or now (all, this replay)
enum : uint8_t { kGateDue = 3, kGateDueNow = 4 };
constexpr float kGateDueSoft = 0.5f;  // the soft window above PacingGateShownHoldEnd (fade value; 0.125 s at 4 per second)
uint64_t g_dueFrame = ~0ull;          // the frame the soft-due units below ran in
int g_dueRan = 0;                     // soft-due units run in g_dueFrame
int g_dueRate = 1;                    // fix102 (rev102a S1): the highest spread rate seen in g_dueFrame (the frame's rate)
constexpr size_t kGateScan = 256;
struct GateClasses {
    uint8_t c[kGateScan];
    size_t n = 0;  // classified entries (min(count, kGateScan))
    float t[kGateScan];  // PacingGateShownHoldMs (holdfix F2): seconds until due now (0: due now; set while the key is on)
    int rate = 1;        // PacingGateShownHoldMs (rev96g F2, port102): soft-due units a frame the spread needs (1-4)
};
// PacingGateShownHoldMs (holdfix F2): the frame period (s) for the due spread, from the last 8 frame periods (rev96g F2
// follow-up). fix102 (rev102a S2): the larger of the second longest and the mean of them (each sample capped at 100 ms),
// not the longest: one hitch (a cell crossing's 50-100 ms frame, usually followed by the spawns it loaded) no longer
// makes the spread treat the next 8 frames as hitches (10 units 0.6 s from their deadline at 60 fps: soft-due at once at
// rate 2 before; with the mean of 27 ms none is due yet), while two long frames of 8 (a slow stretch) count in full and
// a steady pattern of long frames (3x every 5th frame) raises the mean (self-test sim: at most two due units a frame, as
// with the longest; the second longest alone let three through). The spread's 1.25 margin covers a somewhat longer next
// frame; a hitch past it is what due now is for. 1/60 when not known.
float GatePeriodS()
{
    uint64_t m1 = 0, m2 = 0, sum = 0;  // the longest, the second longest, the sum (capped samples)
    int n = 0;
    for (uint64_t d : g.periodRing) {
        if (!d) continue;
        d = std::min<uint64_t>(d, 100000);
        sum += d, ++n;
        if (d > m1) m2 = m1, m1 = d;
        else if (d > m2) m2 = d;
    }
    if (!n) return 1.0f / 60.0f;
    const uint64_t m = std::max(n > 1 ? m2 : m1, sum / static_cast<uint64_t>(n));
    return std::clamp(static_cast<float>(m) / 1e6f, 0.004f, 0.1f);
}

void GateClassify(GateClasses& gc)
{
    gc.n = std::min<size_t>(g.count, kGateScan);
    uintptr_t keys[kGateScan];
    uintptr_t rkeys[kGateScan];  // the resolved actor (0: not resolved / not gated): only those are read for the due rule
    for (size_t i = 0; i < gc.n; ++i) {
        const Entry& q = g.fifo[(g.head + i) % kFifoCap];
        keys[i] = q.key;
        rkeys[i] = 0;
        bool hold = false;
        if (Gated(q) && q.key) {
            uintptr_t key = 0;
            bool distant = false;
            const bool resolved = ResolveDelegate(q.inst, q.type, &key, &distant);
            hold = GateMayHold(resolved, key, distant);
            keys[i] = q.key;
            if (resolved) rkeys[i] = key;
        }
        gc.c[i] = hold ? kGateHeld : kGateRuns;
    }
    // an actor's units go together, in their order: once one of them may no longer wait, all of them are U
    for (size_t i = 0; i < gc.n; ++i) {
        if (gc.c[i] == kGateHeld || !keys[i]) continue;
        for (size_t j = 0; j < gc.n; ++j)
            if (gc.c[j] == kGateHeld && keys[j] == keys[i]) gc.c[j] = kGateRuns;
    }
    // a key-0 unit (its actor not found) may belong to any queued actor: everything before it runs first (gatefix L2)
    size_t last0 = 0;
    for (size_t i = 0; i < gc.n; ++i)
        if (!keys[i]) last0 = i + 1;
    for (size_t i = 0; i + 1 < last0; ++i)
        if (gc.c[i] == kGateHeld) gc.c[i] = kGateKey0;
    // PacingGateShownHoldMs (hold2): a gated unit of a character whose own spawn fade value (the game's, not the shown
    // level) is within the soft window above PacingGateShownHoldEnd is soft-due, at or below it due now (all)
    // holdfix F2 (rev96g): soft-due also early enough that the spread's rate a frame (1, more only when one a frame cannot
    // meet the deadlines) clears all of them before they are due now (deadline order over every character,
    // ShownHoldDueSpread); due now stays the hard backstop
    if (ShownHoldOn()) {
        const float endV = ShownHoldEndValue();
        // holdfix F4 (port102; fix102 = rev102a M1): actors whose open hold saw an equipment unit run before their
        // appearance: until their deadline (0.1 s after that unit) their candidate units count in the spread with that
        // deadline plus the soft window as their time to due now, so the room rule can still run them first and the spread
        // starts them early enough (one a frame unless more are needed); past it they are soft-due (deadline 0, ahead of
        // the spread's units; one more a frame while there are any). Every live hold is listed (at most the hook's 64
        // slots; rev102a S3c: 16 before).
        uintptr_t urgent[64];
        float urgentLeft[64];
        const int nu = ShownHoldUrgentOwners(urgent, urgentLeft, 64);
        float tt[kGateScan];
        size_t idx[kGateScan];
        uint8_t dd[kGateScan];
        bool cand[kGateScan];
        for (size_t i = 0; i < gc.n; ++i) {
            gc.t[i] = 1e9f;
            cand[i] = false;
            // fix102 (rev102a S5): a held unit in the key-0 class (queued before a key-0 unit) is in the due rule too: it
            // runs ahead of that key-0 unit either way (gatefix L2's order is kept)
            if (!rkeys[i]) continue;
            const float v = GateGameValue(rkeys[i]);  // at the stock rate (holdfix F7)
            if (v < 0.0f) continue;
            if (v <= endV) {
                gc.c[i] = kGateDueNow;
                gc.t[i] = 0.0f;
                continue;
            }
            gc.t[i] = (v - endV) / kStockFadeRate;
            cand[i] = true;
        }
        bool anyUrgent = false;
        for (size_t i = 0; nu && i < gc.n; ++i) {
            if (!keys[i] || gc.c[i] == kGateDueNow) continue;
            int u = -1;
            for (int k = 0; k < nu && u < 0; ++k)
                if (urgent[k] == keys[i]) u = k;
            if (u < 0) continue;
            if (urgentLeft[u] > 0.0f) {  // before the deadline: in the spread at it (rev102a M1)
                if (cand[i]) gc.t[i] = std::min(gc.t[i], urgentLeft[u] + kGateDueSoft / kStockFadeRate);
                continue;
            }
            gc.c[i] = kGateDue;
            gc.t[i] = 0.0f;
            cand[i] = false;
            anyUrgent = true;
        }
        // rev96g delta follow-up (port102): the earlier queued units of a candidate's actor that are not candidates
        // themselves (not gated or not resolved: F1 below gives them the candidate's class) count in the spread too, at the
        // earliest deadline of the actor's later candidates, so that the spread starts early enough for them as well
        float carryT[kGateScan];
        {
            uintptr_t ck[kGateScan];
            float cmin[kGateScan];
            size_t nk = 0;
            for (size_t i = gc.n; i-- > 0;) {
                carryT[i] = -1.0f;
                if (cand[i]) {
                    size_t k = 0;
                    while (k < nk && ck[k] != keys[i]) ++k;
                    if (k == nk) ck[nk] = keys[i], cmin[nk++] = gc.t[i];
                    else cmin[k] = std::min(cmin[k], gc.t[i]);
                    continue;
                }
                if (!keys[i] || gc.c[i] == kGateDueNow || gc.c[i] == kGateDue) continue;
                for (size_t k = 0; k < nk; ++k)
                    if (ck[k] == keys[i]) carryT[i] = cmin[k];
            }
        }
        size_t nc = 0;
        for (size_t i = 0; i < gc.n; ++i) {  // queue order: ties in the spread keep it
            if (cand[i]) idx[nc] = i, tt[nc++] = gc.t[i];
            else if (carryT[i] >= 0.0f) idx[nc] = i, tt[nc++] = carryT[i];
        }
        gc.rate = 1;
        if (nc) {
            gc.rate = ShownHoldDueSpread(tt, nc, GatePeriodS(), kGateDueSoft / kStockFadeRate, dd);
            for (size_t k = 0; k < nc; ++k)
                if (dd[k]) gc.c[idx[k]] = kGateDue, gc.t[idx[k]] = tt[k];
        }
        if (anyUrgent) ++gc.rate;
        // holdfix F1 (rev96g): an actor's units keep their order: every earlier entry of a due unit's actor (one that is not
        // gated or not resolved, e.g. a near unit queued for it) gets the same class and deadline, so that it is not overtaken
        for (size_t i = gc.n; i-- > 0;) {
            if ((gc.c[i] != kGateDue && gc.c[i] != kGateDueNow) || !keys[i]) continue;
            for (size_t j = 0; j < i; ++j) {
                if (keys[j] != keys[i] || gc.c[j] == kGateDueNow) continue;
                if (gc.c[j] == kGateDue && gc.t[j] <= gc.t[i]) continue;
                gc.c[j] = gc.c[i];
                gc.t[j] = gc.t[i];
            }
        }
    }
    // held units are counted as held by the gate from the frame they were first held
    for (size_t i = 0; i < gc.n; ++i) {
        Entry& q = g.fifo[(g.head + i) % kFifoCap];
        if (gc.c[i] == kGateHeld && !q.gateFrom) {
            q.gateFrom = g.frame;
            s.gateHeld.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // the fade hold's list: actors with a held appearance or equipment unit (the frame each was first held is kept)
    uintptr_t nk[State::kGateHoldMax];
    uint64_t ns[State::kGateHoldMax];
    int nn = 0;
    // PacingGateShownHoldMs (hold2) reads the same list (the lab's fade hold PacingGateFadeHoldMs too; 0 in public builds)
    const bool holdOn = g.gateHoldUs.load(std::memory_order_relaxed) > 0 || ShownHoldOn();
    // holdfix F2: with the shown hold on, a soft-due unit (being spread out) keeps its character in the list, so that the
    // body stays fully dithered while its units run; the hold's own clock (End + 0.5 in time) still ends it
    const bool listDue = ShownHoldOn();
    for (size_t i = 0; holdOn && i < gc.n && nn < State::kGateHoldMax; ++i) {
        const Entry& q = g.fifo[(g.head + i) % kFifoCap];
        // fix102 (rev102a S5): a held unit in the key-0 class keeps its character listed too (before: it dropped out, and the
        // hold ended as "units ran" with the unit still queued)
        const bool dueListed = listDue && (gc.c[i] == kGateDue || gc.c[i] == kGateKey0) && q.type != kHair && q.key;
        if (!dueListed && (gc.c[i] != kGateHeld || q.type == kHair || !q.key)) continue;
        bool dup = false;
        for (int k = 0; k < nn && !dup; ++k) dup = nk[k] == q.key;
        if (dup) continue;
        uint64_t since = 0;
        for (int k = 0; k < g.gateHoldN; ++k)
            if (g.gateHoldKey[k] == q.key) since = g.gateHoldSince[k];
        if (!since) {  // newly held (rev96e F6: counted once, not once per classify call)
            since = NowUs();
            s.gateFloorActors.fetch_add(1, std::memory_order_relaxed);
        }
        nk[nn] = q.key;
        ns[nn++] = since;
    }
    memcpy(g.gateHoldKey, nk, sizeof(uintptr_t) * nn);
    memcpy(g.gateHoldSince, ns, sizeof(uint64_t) * nn);
    g.gateHoldN = nn;
    g.gateHoldFrame = g.frame;
}

// gate2 fade hold: the time step the fade tick of this actor may take. While the actor has a held appearance or equipment
// unit (the list above, made this frame or the last), at most lab PacingGateFadeHoldMs since it was first held, and its
// spawn fade-in is still at the floor or above (dither >= kGateHiddenMin), the fade stops at 4 (the game's own floor for
// far characters): the step goes down to 4 and no further; a value below 4 held up by the game's floor is set to the
// shown level (at most 4) so that it fades from there once released. Afterwards the fade runs as the game's (4 per second
// from where it stands): its start moves, not its curve. *value is the component's value (may be raised), floor the
// game's floor, rate its rate per second.
float GateFadeStep(uintptr_t actor, float* value, float floor, float rate, float dt)
{
    if (!actor || !g.gateHoldN || g.gateHoldFrame + 1 < g.frame || !GateActive()) return dt;
    int k = 0;
    while (k < g.gateHoldN && g.gateHoldKey[k] != actor) ++k;
    if (k == g.gateHoldN) return dt;
    const uint64_t heldUs = NowUs() - g.gateHoldSince[k];
    if (heldUs > static_cast<uint64_t>(std::max(1, g.gateHoldUs.load(std::memory_order_relaxed)))) return dt;
    const uint64_t held = heldUs / 1000;  // ms (lab line, stats)
    const float shown = std::max(*value, floor);
    if (shown < kGateHiddenMin) return dt;  // already past the floor: not held (its units run as U)
    constexpr float kHold = 4.0f;
    if (rate < 0.5f || rate > 20.0f) rate = 4.0f;
    ++g.gateFrameFloor;
    g.gateFrameFloorOldest = std::max(g.gateFrameFloorOldest, held);
    s.gateFloorTicks.fetch_add(1, std::memory_order_relaxed);
    AtomicMax(s.gateFloorMax, held);
    if (*value > kHold) return std::min(dt, (*value - kHold) / rate);
    *value = std::max(*value, std::min(shown, kHold));
    return 0.0f;
}

}  // namespace
// PacingGateShownHoldMs (hold2): the actor has a held appearance or equipment unit in the gate's list (made this frame or
// the last, the gate active). rev96g F9 (port102): the list can be a frame old, so the actor must also still have a
// non-hair unit queued (a hold now ends in the frame its last held unit ran, not one frame later). Game thread.
bool NpcGateHoldsActor(uintptr_t actor)
{
    if (!actor || !g.gateHoldN || g.gateHoldFrame + 1 < g.frame || !GateActive()) return false;
    bool listed = false;
    for (int k = 0; k < g.gateHoldN && !listed; ++k) listed = g.gateHoldKey[k] == actor;
    if (!listed) return false;
    for (size_t i = 0; i < g.count; ++i) {
        const Entry& e = g.fifo[(g.head + i) % kFifoCap];
        if (e.key == actor && e.type != kHair) return true;
    }
    return false;
}
// PacingGateShownHoldMs (hold2, holdfix F3): a unit of this actor is still queued (any type). Game thread.
bool NpcGateActorQueued(uintptr_t actor)
{
    if (!actor) return false;
    for (size_t i = 0; i < g.count; ++i)
        if (g.fifo[(g.head + i) % kFifoCap].key == actor) return true;
    return false;
}
// PacingGateShownHoldMs (build97, rev96g delta follow-up): a unit of this actor that the gate holds (Gated) is still
// queued; the "AI before built" count's measure (the hold list misses due-now and hair units). Game thread.
bool NpcGateActorGated(uintptr_t actor)
{
    if (!actor) return false;
    for (size_t i = 0; i < g.count; ++i) {
        const Entry& e = g.fifo[(g.head + i) % kFifoCap];
        if (e.key == actor && Gated(e)) return true;
    }
    return false;
}
// PacingGateShownHoldMs (holdfix F8, rev96g): pacing's own fade component layout (state, value, owner, floor, the
// actor's component), for the hook's cross-check; false when pacing has not found it
bool NpcPacingFadeLayout(uint32_t out[5])
{
    if (!g.fadeFound) return false;
    out[0] = g.fadeState, out[1] = g.fadeValue, out[2] = g.fadeOwner, out[3] = g.fadeFloor, out[4] = g.fadeComp;
    return true;
}
// PacingGateShownHoldMs (holdfix F2, rev96g; rate: port102): the deadline spread. In deadline order (ties: queue order)
// at r units a frame the m-th unit (0-based) needs ceil((m + 1) / r) frames; it and every unit before it in that order are
// soft-due when its time left is at most that many frame periods x 1.25 (margin for frames longer than the period) + the
// soft window. r is 1 unless one a frame cannot make some unit's deadline (its due now) any more with that margin
// (found late, or more units than
// frames left): then the least r that can, at most kSpreadMaxRate (rev96g F2: below ~45 fps the rest otherwise all ran as
// due now in one frame; past that cap they still do: due now stays the backstop). Returns r. Pure (self-test).
int ShownHoldDueSpread(const float* t, size_t n, float periodS, float softS, uint8_t* due)
{
    constexpr int kSpreadMaxRate = 4;
    n = std::min<size_t>(n, kGateScan);
    size_t ord[kGateScan];
    for (size_t i = 0; i < n; ++i) {
        ord[i] = i;
        due[i] = 0;
    }
    for (size_t i = 1; i < n; ++i) {  // stable insertion sort by t (no allocation on the game thread; n is small)
        const size_t x = ord[i];
        size_t j = i;
        for (; j > 0 && t[ord[j - 1]] > t[x]; --j) ord[j] = ord[j - 1];
        ord[j] = x;
    }
    const float slot = periodS * 1.25f;
    int r = 1;
    for (size_t m = 0; m < n && slot > 0.0f; ++m) {  // the frames left before its due now, with the due rule's margin
        const float left = std::max(0.0f, t[ord[m]]);
        const size_t frames = static_cast<size_t>(std::min(left / slot, 1e6f)) + 1;  // this frame included
        const size_t need = (m + 1 + frames - 1) / frames;
        r = std::max<int>(r, static_cast<int>(std::min<size_t>(need, kSpreadMaxRate)));
    }
    size_t upto = 0;  // units [0, upto) of the order are due
    for (size_t m = 0; m < n; ++m) {
        const size_t frames = (m + 1 + static_cast<size_t>(r) - 1) / static_cast<size_t>(r);
        if (t[ord[m]] <= slot * static_cast<float>(frames) + softS) upto = m + 1;
    }
    for (size_t m = 0; m < upto; ++m) due[ord[m]] = 1;
    return r;
}
namespace {

// The fade tick's time step for this component (FadeTickHook, game thread): the fade hold above for the actor's own spawn
// fade-in (state 1), else dt
float GateFadeDt(uint8_t* comp, float dt)
{
    __try {
        if (!g.fadeFound || *reinterpret_cast<const int32_t*>(comp + g.fadeState) != 1) return dt;
        const uint8_t* const actor = *reinterpret_cast<const uint8_t* const*>(comp + g.fadeOwner);
        if (!actor || *reinterpret_cast<uint8_t* const*>(actor + g.fadeComp) != comp) return dt;
        float* const value = reinterpret_cast<float*>(comp + g.fadeValue);
        const float floor = *reinterpret_cast<const float*>(comp + g.fadeFloor);
        const float rate = *reinterpret_cast<const float*>(comp + g.fadeValue + 4);  // +0xC0 after the value +0xBC
        return GateFadeStep(reinterpret_cast<uintptr_t>(actor), value, floor, rate, dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return dt;
    }
}

// Moves the queue's entry i to the head (the entries before it move one place back, their order kept). Entries are moved
// bit by bit: the delegate copies are Unreal objects, which Unreal's own containers relocate the same way.
void GateToFront(size_t i)
{
    if (!i || i >= g.count) return;
    alignas(16) uint8_t tmp[sizeof(Entry)];
    memcpy(tmp, &g.fifo[(g.head + i) % kFifoCap], sizeof(Entry));
    for (size_t k = i; k > 0; --k)
        memcpy(static_cast<void*>(&g.fifo[(g.head + k) % kFifoCap]), &g.fifo[(g.head + k - 1) % kFifoCap], sizeof(Entry));
    memcpy(static_cast<void*>(&g.fifo[g.head]), tmp, sizeof(Entry));
}

// Room for a held unit of this type now (post-actor tick only): it fits the target, or (one a frame) the frame is calm.
// At most one unit of 4 ms or more a frame runs for room. *calm: it runs as the frame's calm run.
bool GateRoom(int type, bool* calm)
{
    *calm = false;
    const bool heavy = GateSize(type) >= 4000;
    if (g.gateHeavyFrame == g.frame && heavy) return false;
    if (OverBudget(type)) return false;  // today's budget, without its lag-scaled catch-up
    if (GateFits(type)) {
        if (heavy) g.gateHeavyFrame = g.frame;
        return true;
    }
    const int calmUs = g.gateCalmUs.load(std::memory_order_relaxed);
    if (calmUs <= 0 || g.gateHeavyFrame == g.frame) return false;
    const uint64_t now = NowUs();
    if (now < g.lastPaceEnd || (now - g.lastPaceEnd) + GateRest() >= static_cast<uint64_t>(calmUs)) return false;
    g.gateHeavyFrame = g.frame;
    *calm = true;
    return true;
}

void GateUnitRan(const Entry& e)
{
    if (!e.gateFrom) return;
    const uint64_t w = g.frame - e.gateFrom;
    const uint64_t age = g.frame - e.since;
    const uint64_t now = NowUs();
    const uint64_t ageUs = now > e.sinceUs ? now - e.sinceUs : 0;
    AtomicMax(s.gateAgeMaxUs, ageUs);
    s.gateRan.fetch_add(1, std::memory_order_relaxed);
    s.gateWaitSum.fetch_add(age, std::memory_order_relaxed);
    AtomicMax(s.gateWaitMax, w);
    AtomicMax(s.gateAgeMax, age);
    s.gateWaitHist[age <= 5 ? 0 : age <= 15 ? 1 : age <= 30 ? 2 : age <= 60 ? 3 : age <= 90 ? 4 : 5].fetch_add(1, std::memory_order_relaxed);
    if (age > static_cast<uint64_t>(std::max(1, g.gateBound.load(std::memory_order_relaxed))))
        s.gateForced.fetch_add(1, std::memory_order_relaxed);
}


// At the cap's sleep (t0 = its start = the end of the frame's busy time): the rest after the post-actor tick for the
// prediction, and the check of the frame's prediction against its real busy time
void GateFrameEdge(uint64_t t0)
{
    if (g.gateS3Frame == g.frame && g.lastPaceEnd && g.gateS3Us >= g.lastPaceEnd && t0 >= g.gateS3Us) {
        const uint64_t after = g.unitUs >= g.gateS3UnitUs ? g.unitUs - g.gateS3UnitUs : 0;
        const uint64_t span = t0 - g.gateS3Us;
        const uint64_t rest = span > after ? span - after : 0;
        g.gateRest[g.gateRestAt] = static_cast<uint32_t>(std::min<uint64_t>(rest, 1000000));
        g.gateRestAt = (g.gateRestAt + 1) % 5;
        if (g.gateRestN < 5) ++g.gateRestN;
    }
    const bool fit = g.gatePredFrame == g.frame && g.lastPaceEnd && t0 >= g.lastPaceEnd;
    const uint64_t busy = g.lastPaceEnd && t0 >= g.lastPaceEnd ? t0 - g.lastPaceEnd : 0;
    if (fit) {
        s.gateFitFrames.fetch_add(1, std::memory_order_relaxed);
        if (busy > g.gatePredUs) {
            s.gateOverSum.fetch_add(busy - g.gatePredUs, std::memory_order_relaxed);
            AtomicMax(s.gateErrMax, busy - g.gatePredUs);
            if (busy > static_cast<uint64_t>(g.gateUs.load(std::memory_order_relaxed)) + 1000)
                s.gateOver.fetch_add(1, std::memory_order_relaxed);
        } else
            s.gateUnderSum.fetch_add(g.gatePredUs - busy, std::memory_order_relaxed);
    }
    g.gatePredFrame = ~0ull;
    g.gateFrameHolds = g.gateFrameRuns = g.gateFrameLate = g.gateFrameDrain = g.gateFrameFallback = 0;
    g.gateFrameCalm = g.gateFrameU = g.gateFrameKey0 = 0;
    g.gateFrameOldest = 0;
    g.gateFrameFloor = 0;
    g.gateFrameFloorOldest = 0;
}

// Runs a unit (depth, and the distance and actor key a nested completion inherits) and returns its time in us.
template <class F>
uint64_t TimeUnit(int type, bool distant, const uint8_t* ownerWeak, uintptr_t key, F&& body)
{
    const bool outer = g.unitDistant;
    const uint8_t* const outerWeak = g.unitWeak;
    const uintptr_t outerKey = g.unitKey;
    g.unitDistant = distant;
    g.unitWeak = ownerWeak;
    g.unitKey = key;
    ++g.depth;
    const uint64_t t0 = NowUs();
    body();
    const uint64_t us = NowUs() - t0;
    (void)type;
    --g.depth;
    g.unitDistant = outer;
    g.unitWeak = outerWeak;
    g.unitKey = outerKey;
    return us;
}

void RunEntry()
{
    Entry& e = Front();
    const DelegateType& d = g.del[e.type - kAppearance];
    const bool runDistant = e.distant;
    const uint64_t us = TimeUnit(e.type, runDistant, OwnerWeakOf(e.type, e.inst), e.key, [&] {
        d.execIfSafe(e.inst);
        d.dtor(e.inst, 0);
    });
    LAB_UNIT(e.type, us, reinterpret_cast<const uint8_t*>(e.key), nullptr, g.frame - e.since);
    GateUnitRan(e);  // PacingBaseGate stats
    g.head = (g.head + 1) % kFifoCap;
    --g.count;
    Account(e.type, us);
}

void ReplayAll()
{
    g.gateHoldN = 0;  // gate2 fade hold: nothing waits any more
    if (g.count) {
        s.drains.fetch_add(1, std::memory_order_relaxed);
        g.frameFlags |= 1;
    }
    while (g.count) RunEntry();
}

// PacingHairActor's fade rate (worker gatefix, 2026-11-12 evening): a queued unit whose character's spawn fade-in has left its
// start value 5 is due before the fade reaches 3 (where it counts as visible; 4 per second): the units this frame must run so
// that every such unit (and, FIFO, everything queued before it) is done in time at an even rate from now on: with b = the
// last frame's period without our units, W_j = the estimated time of units 0..j (each type's recent upper quartile) and
// T_j = the time until it shows ((dither - 3) / 4 s), n units per frame finish unit j in time when
// (j + 1) / n frames of b + n x unit size fit in T_j, i.e. n >= (j + 1) b / (T_j - W_j); when T_j - W_j < 2 b, unit j and
// all before it run now. Already showing (below 3): over two frames. Units queued behind a fading character's hair otherwise ran at today's budget (about one 8-10 ms unit per
// frame) and a 25-deep queue let the hair land after the character began to show (hidden 4K Waterfront runs, gate off).
// At most this many units a frame (8-10 ms each): a fading character that cannot be finished in time within it shows a
// little early rather than costing a heap frame.
// stampfix (2026-11-13, reviews rev94d / rev96a): the first version allowed 4 units a frame (32-40 ms of units behind a deep
// queue, for several frames: a sudden drop on its own). Now at most 2 units and at most kFadeRateMaxUs of their estimated
// time a frame (the first unit always; the need is cut to what fits, so a queue of 9-10 ms units goes one a frame and may
// finish after the character shows); still due before dither 3 (kFadeDue), where the character starts to show.
constexpr uint64_t kFadeRateMax = 2;
constexpr uint64_t kFadeRateMaxUs = 16000;
constexpr float kFadeDue = 3.0f;
// gate2: with PacingBaseGate on, the units the gate holds (gc: kGateHeld) are left out (they are not ahead of a fading
// character's units: those run ahead of them), and the result counts the remaining units in queue order.
uint64_t FadeRateNeed(const GateClasses* gc = nullptr)
{
    const uint64_t period = std::min<uint64_t>(std::max<uint64_t>(g.lastPeriodUs, 8000), 100000);
    uint64_t need = 0, work = 0;
    size_t pos = 0;  // the unit's place among the units counted
    for (size_t idx = 0; idx < g.count; ++idx) {
        if (gc && (idx >= gc->n || gc->c[idx] == kGateHeld)) continue;
        const size_t i = pos++;
        const Entry& q = g.fifo[(g.head + idx) % kFifoCap];
        work += GateSize(q.type);
        if (!q.distant) continue;  // a near / seen unit is not held by the budget beyond today's rule
        uintptr_t key = 0;
        bool distant = false;
        ResolveDelegate(q.inst, q.type, &key, &distant);
        const float fade = GateFade(key);
        if (fade < 0.0f || fade >= 5.0f) continue;
        if (fade < kFadeDue) {  // already showing: over two frames, not as one heap
            need = std::max<uint64_t>(need, (i + 2) / 2);
            continue;
        }
        // PacingGateShownHoldMs (rev96g follow-up, port102): a released hold's shown level falls GateShownSpeed times faster
        // than the game's fade (1 when the key is off)
        const uint64_t left = static_cast<uint64_t>((fade - kFadeDue) / 4.0f * 1e6f / GateShownSpeed(key));
        if (left < work + 2 * period) {
            need = std::max<uint64_t>(need, i + 1);
            continue;
        }
        const uint64_t room = left - work;
        need = std::max<uint64_t>(need, ((i + 1) * period + room - 1) / room);
    }
    // a bounded cost per frame, never a heap: at most kFadeRateMax units and what fits in kFadeRateMaxUs (estimates, the
    // first unit always)
    need = std::min<uint64_t>(need, kFadeRateMax);
    uint64_t fit = 0, sum = 0;
    for (size_t i = 0; i < g.count && fit < need; ++i) {
        if (gc && (i >= gc->n || gc->c[i] == kGateHeld)) continue;
        sum += GateSize(g.fifo[(g.head + i) % kFifoCap].type);
        if (fit && sum > kFadeRateMaxUs) break;
        ++fit;
    }
    return fit;
}

// post: the post-actor-tick replay (the frame's own time is known there: PacingBaseGate may release gated units)
// Today's rule for one queued unit (the head without PacingBaseGate; a unit the gate does not hold with it): true = it runs
// now. A distant unit over the frame's (lag-scaled) budget waits, unless PacingFadeAware's re-check finds its actor seen
// (or near) and no longer fade-hidden, i.e. it would no longer have been allowed to wait.
bool TodayRuns(const Entry& e, uint64_t age, bool forced)
{
    if (forced || !e.distant || !OverBudget(e.type, age)) return true;
    bool recheck = g.fadeAware.load(std::memory_order_relaxed) != 0;
    const bool release = recheck && !StillMayWait(e);
    return release;
}

void GateReplay(bool post);

void ReplayBudget(bool post)
{
    const bool gateOn = GateActive();  // PacingBaseGate: decided per unit below (gate2), PacingHairActor's fade rate included
    if (!gateOn && g.hairActor.load(std::memory_order_relaxed)) {
        // PacingHairActor: a fading character's queued units are done before it shows (see FadeRateNeed), counted over the
        // frame's replays; FIFO order unchanged
        if (g.fadeRateFrame != g.frame) {
            g.fadeRateFrame = g.frame;
            g.fadeRateDone = 0;
            g.fadeRateUs = 0;
        }
        const uint64_t need = g.count ? FadeRateNeed() : 0;
        while (g.count && g.fadeRateDone < need) {
            // the frame's time cap (estimates; the first unit always runs)
            const uint64_t next = GateSize(g.fifo[g.head].type);
            if (g.fadeRateDone && g.fadeRateUs + next > kFadeRateMaxUs) break;
            g.fadeRateUs += next;
            ++g.fadeRateDone;
            s.fadeRate.fetch_add(1, std::memory_order_relaxed);
            RunEntry();
        }
    }
    const uint64_t maxLag = static_cast<uint64_t>(g.maxLag.load(std::memory_order_relaxed));
    if (gateOn) {
        GateReplay(post);
        return;
    }
    while (g.count) {
        Entry& e = Front();
        const uint64_t age = g.frame - e.since;
        const bool forced = age > 2 * maxLag;
        if (!TodayRuns(e, age, forced)) break;
        if (forced) {
            s.forced.fetch_add(1, std::memory_order_relaxed);
            g.frameFlags |= 2;
        }
        RunEntry();
    }
}

// PacingBaseGate's replay (gate2): the units the gate does not hold run first, by today's rule, in their queue order and
// ahead of the held ones (PacingHairActor's fade rate over them first); then the oldest held unit runs for the bound, or
// when the frame has room (post-actor tick only), else the held units wait. See the gate's comment above GateFade.
void GateReplay(bool post)
{
    const uint64_t maxLag = static_cast<uint64_t>(g.maxLag.load(std::memory_order_relaxed));
    const uint64_t bound = static_cast<uint64_t>(std::max(1, g.gateBound.load(std::memory_order_relaxed)));
    if (g.gateLateFrame != g.frame) {
        g.gateLateFrame = g.frame;
        g.gateLateDone = 0;
    }
    GateClasses gc;
    // runs the queue's entry i now (it moves to the head first); why: the lab unit line's release reason
    auto runAt = [&](size_t i, char why) {
        const Entry& q = g.fifo[(g.head + i) % kFifoCap];
        if (q.gateFrom && (why == 'u' || why == 'r')) s.gateTurned.fetch_add(1, std::memory_order_relaxed);
        if (why == 'k') {
            ++g.gateFrameKey0;
            s.gateKey0.fetch_add(1, std::memory_order_relaxed);
        }
        // PacingGateShownHoldMs (holdfix F4): an equipment unit runs while the same actor's appearance is still queued: its
        // open hold is marked, and from then on that actor's queued units are soft-due at once (GateClassify, port102), so
        // the body is built within a frame or two instead of up to ~0.9 s later, its weapon showing on a hidden body
        if (q.type == kEquipment && q.key && ShownHoldOn()) {
            bool appQueued = false;
            for (size_t j = 0; j < g.count && !appQueued; ++j) {
                const Entry& o = g.fifo[(g.head + j) % kFifoCap];
                appQueued = j != i && o.key == q.key && o.type == kAppearance;
            }
            if (appQueued) ShownHoldNoteEquipFirst(q.key);
        }
        GateToFront(i);
        RunEntry();
    };
    if (g.hairActor.load(std::memory_order_relaxed)) {
        // PacingHairActor: a fading character's queued units are done before it shows (FadeRateNeed over the units the
        // gate does not hold, which run ahead of the held ones), counted over the frame's replays
        if (g.fadeRateFrame != g.frame) {
            g.fadeRateFrame = g.frame;
            g.fadeRateDone = 0;
            g.fadeRateUs = 0;
        }
        GateClassify(gc);
        const uint64_t need = g.count ? FadeRateNeed(&gc) : 0;
        while (g.count && g.fadeRateDone < need) {
            GateClassify(gc);
            size_t i = 0;
            while (i < gc.n && gc.c[i] == kGateHeld) ++i;
            if (i >= gc.n) break;
            // the frame's time cap (estimates; the first unit always runs)
            const uint64_t next = GateSize(g.fifo[(g.head + i) % kFifoCap].type);
            if (g.fadeRateDone && g.fadeRateUs + next > kFadeRateMaxUs) break;
            g.fadeRateUs += next;
            ++g.fadeRateDone;
            s.fadeRate.fetch_add(1, std::memory_order_relaxed);
            runAt(i, gc.c[i] == kGateKey0 ? 'k' : 'r');
        }
    }
    // PacingGateShownHoldMs (hold2): units due before their character's own fade ends run first, ahead of other actors'
    // units (an actor's own order kept: its gated units are due together): every due-now unit, and soft-due units up to
    // the spread's rate a frame (1 unless one a frame cannot meet the deadlines, at most 4: rev96g F2, port102)
    // holdfix F2: the earliest deadline first (due now has deadline 0; ties: queue order, so an actor's units, which share
    // one deadline after F1, keep their order); the most due units of one replay is logged
    if (ShownHoldOn()) {
        if (g_dueFrame != g.frame) {
            g_dueFrame = g.frame;
            g_dueRan = 0;
            g_dueRate = 1;
        }
        int ranDue = 0;
        for (int guard = 0; g.count && guard < 64; ++guard) {
            GateClassify(gc);
            // fix102 (rev102a S1): the frame's rate is the highest the spread asked for in this frame: recomputed on the
            // shortened queue after each run it shrinks, and against the frame's running count only about half ran
            g_dueRate = std::max(g_dueRate, gc.rate);
            size_t i = gc.n;
            for (size_t k = 0; k < gc.n; ++k) {
                const bool ok = gc.c[k] == kGateDueNow || (gc.c[k] == kGateDue && g_dueRan < g_dueRate);
                if (ok && (i == gc.n || gc.t[k] < gc.t[i])) i = k;
            }
            if (i >= gc.n) break;
            const bool now = gc.c[i] == kGateDueNow;
            if (!now) ++g_dueRan;
            ShownHoldCountDue(now);
            ++ranDue;
            runAt(i, now ? 'D' : 'd');
        }
        ShownHoldCountDueReplay(ranDue);
    }
    bool todayHeld = false;  // a unit not held by the gate waits for today's budget: so do those behind it
    for (int guard = 0; g.count && guard < 4096; ++guard) {
        GateClassify(gc);
        // 1. the units the gate does not hold: today's rule, in queue order
        size_t u = 0;
        while (u < gc.n && gc.c[u] == kGateHeld) ++u;
        if (!todayHeld && u < gc.n) {
            const Entry& e = g.fifo[(g.head + u) % kFifoCap];
            const uint64_t age = g.frame - e.since;
            const bool forced = age > 2 * maxLag;
            if (TodayRuns(e, age, forced)) {
                if (forced) {
                    s.forced.fetch_add(1, std::memory_order_relaxed);
                    g.frameFlags |= 2;
                }
                ++g.gateFrameU;
                runAt(u, gc.c[u] == kGateKey0 ? 'k' : 'u');
                continue;
            }
            todayHeld = true;
        }
        // 2. the held units, oldest first: the bound (gatefix D1's rate rule over the held units' places), then room
        size_t w[kGateScan];
        size_t nw = 0;
        for (size_t i = 0; i < gc.n; ++i)
            if (gc.c[i] == kGateHeld) w[nw++] = i;
        for (size_t i = gc.n; i < g.count && nw < kGateScan; ++i) w[nw++] = i;  // beyond the scan: held, for the bound
        if (!nw) break;
        // places counted from the frame's start (units already run for the bound this frame included): recomputing from
        // the shortened queue after each run would ask one unit too few and let the last one reach bound + 1 (gatefix's
        // D1 relied on today's forced rule for that one; held units have no other bound)
        // A unit already past the bound (only after the bound was lowered live) needs one unit this frame: such units
        // run one per frame, never all at once (rev96e note on a live change). (A bound in time was tried: with frames of
        // changing length it let a unit pass by a frame or ran two in one frame; the bound stays in frames, rev96e F9.)
        const uint64_t doneB = g.gateLateDone;
        uint64_t need = 0;
        for (size_t j = 0; j < nw; ++j) {
            const uint64_t a = g.frame - g.fifo[(g.head + w[j]) % kFifoCap].since;
            if (a > bound) {
                need = std::max<uint64_t>(need, 1);
                continue;
            }
            const uint64_t left = bound + 1 - a;  // frames left for it, this one included
            if (j + doneB + 1 < left) continue;    // still within reach at one unit a frame from the next frame on
            need = std::max<uint64_t>(need, (j + doneB + 1 + left - 1) / left);
        }
        const Entry& h = g.fifo[(g.head + w[0]) % kFifoCap];
        const uint64_t hAge = g.frame - h.since;
        g.gateFrameOldest = std::max(g.gateFrameOldest, hAge);
        if (g.gateLateDone < need) {
            ++g.gateLateDone;
            ++g.gateFrameLate;
            s.gateLate.fetch_add(1, std::memory_order_relaxed);
            runAt(w[0], 'b');
            continue;
        }
        bool calm = false;
        if (post && w[0] < gc.n && GateRoom(h.type, &calm)) {
            if (calm) {
                ++g.gateFrameCalm;
                s.gateCalm.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++g.gateFrameRuns;
                s.gateFit.fetch_add(1, std::memory_order_relaxed);
            }
            runAt(w[0], calm ? 'c' : 'f');
            continue;
        }
        g.gateFrameHolds = std::max(g.gateFrameHolds, static_cast<int>(nw));  // the held units this frame
        break;
    }
    if (!g.count) g.gateHoldN = 0;  // the fade hold: nothing held any more
}

bool Enqueue(int type, const uint8_t* inst, uintptr_t key, bool distant, bool fadeHeld = false)
{
    if (g.count == kFifoCap) return false;
    const DelegateType& d = g.del[type - kAppearance];
    Entry& e = g.fifo[(g.head + g.count) % kFifoCap];
    memset(e.inst, 0, sizeof e.inst);
    memcpy(e.inst, inst, kCaptureOffset);        // vtable, handle, owner weak pointer
    memset(e.inst + 8, 0, 8);
    if (d.copyCtor) d.copyCtor(e.inst + kCaptureOffset, inst + kCaptureOffset);
    else memcpy(e.inst + kCaptureOffset, inst + kCaptureOffset, d.size - kCaptureOffset);
    e.type = type;
    e.since = g.frame;
    e.sinceUs = NowUs();
    e.key = key;
    e.distant = distant;
    e.fadeHeld = fadeHeld;
    e.gateFrom = 0;
    if (fadeHeld) g.fadeHeldUnits.fetch_add(1, std::memory_order_relaxed);
    ++g.count;
    s.deferred[type].fetch_add(1, std::memory_order_relaxed);
    AtomicMax(s.maxFifo, g.count);
    return true;
}

bool OnGameThread() { return g.gtId && GetCurrentThreadId() == g.gtId; }

// Delegate units may wait while the world ticks frames that defer: the completion delegates run in the engine's
// world-less tickables, after the world's post-actor tick, so "this frame" = since the last frame start (if recent).
bool FrameOpen() { return g.allowDefer && NowUs() - g.frameStartUs < 100000; }

// Common path of the delegate hooks: true = handled here (deferred or run), *result = what to return.
bool DelegateUnit(int type, void* inst, bool* result)
{
    const DelegateType& d = g.del[type - kAppearance];
    if (!OnGameThread()) return false;
    uint8_t* const p = static_cast<uint8_t*>(inst);
    if (g.depth > 0) {  // inside another unit (e.g. a load that completed at once): part of it, unless order says wait
        // Queued behind the backlog, it waits like the unit it came from: as near (runs at the next replay whatever
        // the budget) only if that unit's actor was near or seen. An earlier version always counted as near, so a
        // distant appearance unit replayed at its turn pulled the hair/equipment it completed into the same frame
        // (Silver Road, 4K: 59 ms appearance + 3 units = 115 ms in one frame).
        // PacingHairActor (rev94a L1): queued with the running unit's actor key, so a later unit of the same actor (now
        // matched to it, e.g. a hair step) waits behind it instead of overtaking it; key 0 (as in 1.4.0) with the key off.
        const uintptr_t nestedKey = g.hairActor.load(std::memory_order_relaxed) ? g.unitKey : 0;
        if (g.count && FrameOpen() && Enqueue(type, p, nestedKey, g.unitDistant)) { *result = true; return true; }
        return false;
    }
    bool distant = false;
    bool nearFade = false;  // lab PacingNearFadeFrames: near and fade-hidden (runs as a near unit)
    uintptr_t unitKey = 0;
    if (!FrameOpen()) {
        if (g.count && g.allowDefer && g.drainLogs < 12) {
            ++g.drainLogs;
            Log("[pacing] delegate %.0f ms after the frame start (outside the frame window): backlog of %zu runs now",
                (NowUs() - g.frameStartUs) / 1000.0, g.count);
        }
        ReplayAll();  // not deferring now: whatever waits runs first, in order
    } else {
        uintptr_t key;
        ResolveDelegate(p, type, &key, &distant);
        unitKey = key;
        // With a backlog, a distant unit (or one of an actor that has queued units) queues behind it; a near unit of
        // another actor runs now. Without one, a distant unit waits only when the frame's budget is spent.
        bool wait = g.count ? (distant || FifoHasKey(key)) : (distant && OverBudget(type));
        // PacingBaseGate: with no backlog, a gated unit the budget would run now runs only after this frame's actor tick
        // and if the frame stays within the target (an arrival before the post-actor tick queues and is replayed there)
        bool gateHold = false;
        const bool labGateable = true;
        // (only a unit the gate may hold: far, unseen or fade-hidden, not past the fade's floor; gate2)
        if (!wait && !g.count && distant && labGateable && GateActive() && GateMayHold(true, key, distant)) {
            bool calm = false;
            if (GateKnown() && GateRoom(type, &calm)) {
                ++(calm ? g.gateFrameCalm : g.gateFrameRuns);
                (calm ? s.gateCalm : s.gateFit).fetch_add(1, std::memory_order_relaxed);
            } else {
                wait = true;
                gateHold = true;
            }
        }
        const bool fadeHeld = g.lastFadeHeld;
        if (wait) {
            if (Enqueue(type, p, key, distant, fadeHeld)) {
                if (gateHold) {  // PacingBaseGate: held although the budget would have run it
                    g.fifo[(g.head + g.count - 1) % kFifoCap].gateFrom = g.frame;
                    s.gateHeld.fetch_add(1, std::memory_order_relaxed);
                    ++g.gateFrameHolds;
                }
                *result = true;
                return true;
            }
            ReplayAll();  // FIFO full: keep the order
        }
    }
    if (OverBudget(type)) g.frameFlags |= 4;
    const uint64_t us = TimeUnit(type, distant && !nearFade, OwnerWeakOf(type, p), unitKey, [&] { *result = d.execIfSafe(inst); });
    Account(type, us);
    LAB_UNIT(type, us, reinterpret_cast<const uint8_t*>(unitKey), nullptr, 0);
    (void)unitKey;
    return true;
}

template <int kType>
bool ExecIfSafeHook(void* inst)
{
    bool r;
    if (DelegateUnit(kType, inst, &r)) return r;
    return g.del[kType - kAppearance].execIfSafe(inst);
}

template <int kType>
void ExecuteHook(void* inst)
{
    bool r;
    if (DelegateUnit(kType, inst, &r)) return;
    g.del[kType - kAppearance].execute(inst);
}

// Defers the command the session's reader is on: the read position stays, the reader's semaphore count is given back,
// and the run loop ends as after an end command.
uint64_t Defer(uint8_t* sess, int type)
{
    void* sem = *reinterpret_cast<void**>(sess + kSessSem);
    reinterpret_cast<SemFn>((*reinterpret_cast<void***>(sem))[2])(sem);  // FSemaphore::Unlock
    sess[kSessRun] = 0;
    g.deferredNow = true;
    g.aheadDeferred = true;
    s.deferred[type].fetch_add(1, std::memory_order_relaxed);
    return 0;
}

bool IsMainCommand(void* cmd)
{
    return OnGameThread() && g.inMainRun && g.depth == 0 && g.session &&
           *reinterpret_cast<void**>(static_cast<uint8_t*>(g.session) + kSessReadPtr) == cmd;
}

uint64_t CreateExecHook(void* cmd)
{
    if (!IsMainCommand(cmd)) return g.origCreate(cmd);
    const uint8_t* c = static_cast<const uint8_t*>(cmd);
    const float x = *reinterpret_cast<const float*>(c + kCreatePosX);
    const float y = *reinterpret_cast<const float*>(c + kCreatePosY);
    const float z = *reinterpret_cast<const float*>(c + kCreatePosZ);
    const bool distant = FarFrom(x * kOldToUe, -y * kOldToUe, z * kOldToUe);
    if (g.createLogs < 12) {
        ++g.createLogs;
        Log("[pacing] create command at UE (%.0f, %.0f, %.0f), player %s(%.0f, %.0f, %.0f): %s", x * kOldToUe, -y * kOldToUe,
            z * kOldToUe, g.playerKnown ? "" : "unknown ", g.px, g.py, g.pz, distant ? "distant" : "near");
    }
    if (g.allowDefer && distant && !PairForced() && OverBudget(kCreate, PairLag())) {
        // CreatePreloadEarly: what it will load is requested now, not only once the budget lets it run
        CreatePreloadOnDefer(cmd);
        return Defer(static_cast<uint8_t*>(g.session), kCreate);
    }
    // CreatePreload (bigcreate; public 1.3.0): a far create whose form asset / class is not loaded waits while it loads
    // asynchronously (CreatePreloadNear: also a near one, a few frames at most, while its spot is out of the camera's view)
    const bool nearWait = !distant && g.allowDefer && !PairForced() &&
                          CreatePreloadNearMayWait(x * kOldToUe, -y * kOldToUe, z * kOldToUe);
    if (CreatePreloadBefore(cmd, (g.allowDefer && distant && !PairForced()) || nearWait, PairLag(),
                            static_cast<uint64_t>(g.maxLag.load(std::memory_order_relaxed)), nearWait)) {
        g.preloadWaitFrame = g.frame;  // PacingFlowProgress: the queue's head waits for its loads this frame
        return Defer(static_cast<uint8_t*>(g.session), kCreate);
    }
    if (PairForced() && distant) s.forced.fetch_add(1, std::memory_order_relaxed);
    g.lastCreateFar = distant;
    if (OverBudget(kCreate)) g.frameFlags |= distant ? 2 : 4;
    uint64_t r = 0;
    // CreateAhead (create_ahead.cpp): the command's reference and base form ids for the spawner's SpawnActor call (no-op
    // unless installed)
    CreateAheadCreateBegin(*reinterpret_cast<const uint32_t*>(c + 0x78), *reinterpret_cast<const uint32_t*>(c + 0x70));
    const uint64_t us = TimeUnit(kCreate, distant, nullptr, 0, [&] { r = g.origCreate(cmd); });
    CreateAheadCreateEnd();
    Account(kCreate, us);
    return r;
}

bool IsHeavy(uintptr_t fn)
{
    for (int i = 0; i < g.numHeavy; ++i)
        if (g.heavyFns[i] == fn) return true;
    return false;
}

uint64_t GenericExecHook(void* cmd)
{
    if (!IsMainCommand(cmd)) return g.origGeneric(cmd);
    const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(cmd) + kCmdFn);
    const bool heavy = IsHeavy(fn);
    if (heavy && g.allowDefer && g.lastCreateFar && !PairForced() && OverBudget(kGeneric, PairLag()))
        return Defer(static_cast<uint8_t*>(g.session), kGeneric);
    uint64_t r = 0;
    const uint64_t us = TimeUnit(kGeneric, heavy && g.lastCreateFar, nullptr, 0, [&] { r = g.origGeneric(cmd); });
    if (heavy) {
        Account(kGeneric, us);
        LAB_UNIT(kGeneric, us, nullptr, nullptr, PairLag());
    } else if (us >= 2000 && g.numHeavy < 16) {
        g.heavyFns[g.numHeavy++] = fn;
        Log("[pacing] generic pairing command exe+0x%llX took %.1f ms: treated as an NPC unit from now on",
            static_cast<unsigned long long>(fn - reinterpret_cast<uintptr_t>(Game().base)), us / 1000.0);
    }
    return r;
}

void EndFrameStats()
{
    // frames with more than a frame's worth of units: logged (at most 40 a minute) to see what the floor is made of
    if (g.unitUs > 16000 && s.frameLogs.fetch_add(1, std::memory_order_relaxed) < 40)
        Log("[pacing] frame %llu: %.1f ms of units (%d: create %d, head/BP %d, appearance %d, equipment %d, hair %d), first "
            "%.1f ms, budget %.1f ms, lag %llu/%llu frames%s%s%s%s",
            static_cast<unsigned long long>(g.frame), g.unitUs / 1000.0, g.units, g.frameTypes[0], g.frameTypes[1],
            g.frameTypes[2], g.frameTypes[3], g.frameTypes[4], g.frameFirstUs / 1000.0, g.budgetThisFrame / 1000.0,
            static_cast<unsigned long long>(PairLag()), static_cast<unsigned long long>(FifoAge()),
            g.frameFlags & 1 ? ", drained" : "", g.frameFlags & 2 ? ", lag bound" : "", g.frameFlags & 4 ? ", near/seen units" : "",
            g.frameFlags & 8 ? ", a level flow waits" : "");
    MapPopupSpreadFrameEnd(g.frame);  // MapPopupSpread (public 1.3.0): safety flush at the next frame start (counted late)
    s.frames.fetch_add(1, std::memory_order_relaxed);
    AtomicMax(s.maxUnitsPerFrame, static_cast<uint64_t>(g.units));
    AtomicMax(s.maxFrameUnitUs, g.unitUs);
    AtomicMax(s.maxOwed, g.S - g.E);
    AtomicMax(s.maxLag, std::max(PairLag(), FifoAge()));
}

// The frame's unit budget: PacingBudgetMs, or, while a frame-rate cap holds the game thread (PacingUseSlack), the
// time the cap would otherwise sleep: capped frame period - the game thread's own busy time - a margin.
uint64_t FrameBudget()
{
    const uint64_t fixed = static_cast<uint64_t>(g.budgetUs.load(std::memory_order_relaxed));
    // only under a frame-rate cap (the engine's own GetMaxTickRate, read every frame)
    if (!g.useSlack.load(std::memory_order_relaxed) || g.capPeriodUs <= 0) return fixed;
    const double slack = g.capPeriodUs - g.busyUs - static_cast<double>(kSlackMarginUs);
    if (slack <= static_cast<double>(fixed)) return fixed;
    s.slackFrames.fetch_add(1, std::memory_order_relaxed);
    const uint64_t b = static_cast<uint64_t>(slack);
    AtomicMax(s.maxBudgetUs, b);
    return b;
}


// Times the frame-rate cap's sleep (game thread, once per frame, before the world ticks).

// CapSleepSkip (worker repathdefer 2026-11-19; lever 7 of the stutterfree plan; S-041, S-045, S-062): the frame-rate
// cap's wait before a frame the game is known to make long is shortened by up to CapSleepSkipMaxMs. A present gap is the cap's
// wait before a frame + the frame's game-thread work + the render thread's lag growth (stutterfree, 14 of 14 marks): a 14 ms
// garbage-collection pass after 8.4 ms of waiting made a 32 ms gap at Mortal Camp, a cell-removal frame after 4.9 ms a 37 ms
// one. Known at this point (the end of the frame before, in MaxTickRateHook, before the engine's wait): GcPassAfterBusy's
// reading of the GC timer (the pass runs in the coming frame when the timer, plus the time this frame already took, is over
// the interval and nothing the engine tests or our hold uses stops it: GcPassAfterBusyNextUnits bit 1) and a cell's
// RemoveFromWorld step in the frame that just ended (StreamOutEndPlaySlice's note: the removal goes on in the coming frame,
// bit 2). How: the engine's one GetMaxTickRate call inside UpdateTimeAndHandleMaxTickRate (a 6-byte `call [rax+slot]`)
// becomes `call [rip+slot]` into CapRateHook, which calls the same virtual and, for this one call only, returns the rate of
// a period shorter by the cut: the engine's WaitTime = 1/rate - DeltaRealTime is then shorter by the cut (0 below 0).
// Identical simulation (user 2026-11-19: "Ok if identical"): right after the engine's function returns, FApp::DeltaTime
// (written there as CurrentTime - LastRealTime, read by nothing in between) gets back the wait that was cut (the engine's
// own WaitTime minus the one it waited), so the frame is simulated with the delta of a frame that waited as the game does;
// the engine's LastRealTime is the frame's real start, so the next frame's delta is its real length, as in the game. Only
// the moment the frame starts (and is shown) moves; the sleep's own overshoot stays, as in the game, nondeterministic. No
// added latency (the frame's input is read earlier). At most one shortened wait in kCapSkipEvery frames; never in menus,
// loading screens, or uncapped.
constexpr uint64_t kCapSkipEvery = 4;
constexpr size_t kCapCallAt = 0x1E;   // kMaxTickCalls: the second `call [rax+disp32]` (GetMaxTickRate)
uint8_t* g_capCallSite = nullptr;
std::atomic<int> g_capSkipBits{0};    // live: 1 GC pass due, 2 cell removal going on (0 off)
std::atomic<int> g_capSkipMaxUs{8000};
std::atomic<int> g_capSkipAddBack{1};  // lab CapSleepSkipAddBack (public: always 1): 1 the cut goes back into the delta, 0 not
bool g_capSkipInstalled = false;
double g_capSkipCutS = 0;             // game thread: > 0 for the next GetMaxTickRate call inside the cap's function only
double g_capDeltaAddS = 0;            // game thread: the wait that call cut (the engine's WaitTime minus the shortened one)
uint64_t g_capSkipLastFrame = 0;
bool g_capSkipCheck = false;          // the frame running now started early: its length is recorded at the next wait
int g_capSkipWhy = 0;
double g_capSkipCutUs = 0;
int (*g_capUnitsFn)(uint64_t) = &GcPassAfterBusyNextUnits;
bool (*g_capMenuFn)(int32_t*, int32_t*) = &ReadMenuFreeze;
// skips / long20 / short20 by reason ([0] GC pass, [1] cell removal; review C3); noWait: cut frames that took a whole cap
// period or more (the next frame does not wait); caseB: of those, the frame waited >= 1 ms for the render thread (FFrameEndSync,
// HitchLedger's sync column): the head start was partly absorbed and the next step is up to that much (at most the cut) longer
// than the game's (review C1), caseBUs = sum of min(sync wait, cut); syncUnknown: noWait frames while the ledger did not record;
// notGiven: cuts not given back to the delta (lab CapSleepSkipAddBack=0).
struct CapSkipStats {
    std::atomic<uint64_t> skips[2]{}, cutUs{}, long20[2]{}, short20[2]{}, maxAfterUs{}, calls{}, addedUs{}, noDelta{}, noWait{}, caseB{},
        caseBUs{}, syncUnknown{}, notGiven{};
} cs;
int g_capSkipLines = 0;
double g_capLabAdded = 0, g_capLabDelta = 0;  // lab line: the last cut frame's restored part and its delta time

// UpdateTimeAndHandleMaxTickRate's GetMaxTickRate call (game thread; the patched call site).
float CapRateHook(void* engine, float delta, bool allowSmoothing)
{
    void* const* const vt = *static_cast<void* const* const*>(engine);
    const float rate = reinterpret_cast<GetMaxTickRateFn>(vt[g.getMaxTickRateSlot / 8])(engine, delta, allowSmoothing);
    const double cut = g_capSkipCutS;
    g_capSkipCutS = 0;
    if (cut <= 0 || !(rate > 1.0f && rate < 1000.0f)) return rate;
    cs.calls.fetch_add(1, std::memory_order_relaxed);
    const double period = 1.0 / static_cast<double>(rate) - cut;
    const float shorter = period > 1e-4 ? static_cast<float>(1.0 / period) : 0.0f;
    // the engine's WaitTime = max(1/MaxTickRate - DeltaRealTime, 0) (float), with its rate and with ours
    const double d = static_cast<double>(delta);
    const double stockWait = std::max(1.0 / static_cast<double>(rate) - d, 0.0);
    const double ourWait = shorter > 0.0f ? std::max(1.0 / static_cast<double>(shorter) - d, 0.0) : 0.0;
    g_capDeltaAddS = stockWait - ourWait;
    return shorter;
}

// After the engine's function (game thread): the cut wait goes back into FApp::DeltaTime (`dt`), so the frame's simulated
// delta is the one of a frame that waited. Returns the seconds added (0: nothing was cut).
double CapSkipRestoreDelta(double* dt)
{
    const double add = g_capDeltaAddS;
    g_capDeltaAddS = 0;
    if (add <= 0) return 0;
    if (g_capSkipAddBack.load(std::memory_order_relaxed) == 0) {  // lab CapSleepSkipAddBack=0: the frame keeps its real, shorter step
        cs.notGiven.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (!dt) {  // no FApp::DeltaTime known: the cut frame keeps its shorter delta (counted; the install requires it)
        cs.noDelta.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    *dt += add;
    cs.addedUs.fetch_add(static_cast<uint64_t>(add * 1e6), std::memory_order_relaxed);
    return add;
}

// MaxTickRateHook's entry, game thread: is the coming frame known to carry a unit? Then the next wait is shortened.
void CapSkipDecide(uint64_t entryUs)
{
    if (g_capSkipCheck) {  // the frame that just ended started early: how long it took (its work, entry to the last wait's end)
        g_capSkipCheck = false;
        const uint64_t work = g.lastPaceEnd && entryUs > g.lastPaceEnd ? entryUs - g.lastPaceEnd : 0;
        const int r = g_capSkipWhy & 1 ? 0 : 1;
        (work >= 20000 ? cs.long20[r] : cs.short20[r]).fetch_add(1, std::memory_order_relaxed);
        AtomicMax(cs.maxAfterUs, work);
        // review C1 (case B): a cut frame as long as the period or longer is not followed by a wait, so the next frame's delta is
        // the cut frame's real length; if the frame waited for the render thread, part of its head start was absorbed there
        const uint64_t sync = LedgerSyncUsNow();
        const bool noWait = g.capPeriodUs > 0 && static_cast<double>(work) >= g.capPeriodUs;
        bool caseB = false;
        if (noWait) {
            cs.noWait.fetch_add(1, std::memory_order_relaxed);
            if (sync == ~0ull) cs.syncUnknown.fetch_add(1, std::memory_order_relaxed);
            else if (sync >= 1000) {
                caseB = true;
                cs.caseB.fetch_add(1, std::memory_order_relaxed);
                cs.caseBUs.fetch_add(static_cast<uint64_t>(std::min(static_cast<double>(sync), g_capSkipCutUs)), std::memory_order_relaxed);
            }
        }
    }
    const int bits = g_capSkipBits.load(std::memory_order_relaxed);
    if (!bits || !g_capSkipInstalled || g.capPeriodUs <= 0 || !g.lastPaceEnd || entryUs <= g.lastPaceEnd) return;
    if (g_capSkipLastFrame && g.frame < g_capSkipLastFrame + kCapSkipEvery) return;
    const double elapsed = static_cast<double>(entryUs - g.lastPaceEnd);
    const double wait = g.capPeriodUs - elapsed;
    if (wait < 1000.0) return;  // nothing worth shortening
    const uint8_t* const tes = g.tes ? *g.tes : nullptr;
    if (!tes || tes[kTesTravelPending]) return;
    int32_t named = 0, mode = 0;
    if (!g_capMenuFn || !g_capMenuFn(&named, &mode) || named != 0 || mode != 0) return;
    // review C6: the coming frame's delta is at least the cap period with the add-back (a waited frame's), at least the period
    // minus the cut without it (lab CapSleepSkipAddBack=0); 0.2 ms below either, so the prediction stays on the safe side
    const double maxCut = static_cast<double>(g_capSkipMaxUs.load(std::memory_order_relaxed));
    const double stepUs = g_capSkipAddBack.load(std::memory_order_relaxed) ? g.capPeriodUs : g.capPeriodUs - std::min(wait, maxCut);
    const uint64_t predictUs = static_cast<uint64_t>(std::max(elapsed, stepUs - 200.0));
    const int units = g_capUnitsFn ? g_capUnitsFn(predictUs) & bits : 0;
    if (!units) return;
    const double cut = std::min(wait, maxCut);
    g_capSkipCutS = cut / 1e6;
    g_capSkipCutUs = cut;
    g_capSkipWhy = units;
    g_capSkipLastFrame = g.frame;
    g_capSkipCheck = true;
    cs.skips[units & 1 ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
    cs.cutUs.fetch_add(static_cast<uint64_t>(cut), std::memory_order_relaxed);
}

void MaxTickRateHook(void* engine)
{
    LedgerSleepBegin();  // HitchLedger: the cap's sleep starts (no-op unless recording, game thread only)
    const uint64_t t0 = NowUs();
    if (g_capSkipInstalled && OnGameThread()) CapSkipDecide(t0);  // CapSleepSkip: the wait below may be shortened (one call)
    g.origMaxTick(engine);
    g_capSkipCutS = 0;  // used by the engine's GetMaxTickRate call or not: never carried to another call
    if (g_capDeltaAddS > 0) {  // CapSleepSkip: the cut frame's delta as the game's
        double* const dt = GcPassAfterBusyDeltaTime();
        g_capLabAdded = CapSkipRestoreDelta(dt);
        g_capLabDelta = dt ? *dt : 0;
    }
    const uint64_t t1 = NowUs();
    if (!OnGameThread()) return;
    HdrMenuFrameEnd();   // HdrMenuRows / HudAspectRow / DlssRayReconstruction (no-op unless installed)
    HdrGammaFrameEnd();  // HdrGamma22 (no-op unless installed; writes only while on)
    StuckEffectLookFrameEnd();  // StuckEffectLookFix: leftover status-effect looks (no-op unless installed)
    CmdMemPregrowFrame();  // CmdMemPregrow: its pass waits for the first loading screen (reads the freeze state only while it waits)
    {
        LedgerPacing lp;
        lp.unitCumUs = g.unitCumUs;
        lp.unitCumN = g.unitCumN;
        lp.pairCumUs = g.pairCumUs;
        lp.flowCumUs = g.flowCumUs;
        lp.capPeriodUs = g.capPeriodUs;
        const uint8_t* const tes = g.tes ? *g.tes : nullptr;
        lp.travel = tes && tes[kTesTravelPending];
        LedgerFrameEnd(lp);  // the frame since the last exit (no-op unless installed)
        DoorDiagFrameEnd();  // diagnostic DoorDiag's loading-screen timeline (no-op unless installed)
        StreamBehindLoadingFrameEnd();  // StreamBehindLoadingScreen (no-op unless installed and on)
    }
    BodySpreadFrame();     // BodyRemovalSpread (physics/body_spread.cpp): no-op unless installed
    ImpactPreloadFrame();  // ImpactPreload (loading/impact_preload.cpp): no-op unless installed and on
    RowPreloadGtFrame();   // RowPreload (streaming/row_preload.cpp): the menu / loading freeze for the OT (no-op unless installed)
    const uint64_t sleep = t1 - t0, period = g.lastPaceEnd ? t1 - g.lastPaceEnd : 0;
    {  // CreateAhead (create_ahead.cpp): one SpawnActor half after a frame with slack, while nothing is held (no-op unless on)
        const uint8_t* const tesNow = g.tes ? *g.tes : nullptr;
        CreateAheadFrame(sleep, g.count != 0 || g.aheadDeferred, tesNow && tesNow[kTesTravelPending]);
        g.aheadDeferred = false;
        // CellObjectPreload (streaming/cell_preload.cpp): the files of objects in cells loaded ahead (no-op unless on)
        CellObjectPreloadFrame(tesNow && tesNow[kTesTravelPending]);
    }
    GateFrameEdge(t0);  // PacingBaseGate: the frame's rest after the post-actor tick, its prediction check
    g.lastPeriodUs = period > g.unitUs ? period - g.unitUs : 0;  // PacingHairActor fade rate: the frame without our units
    g.lastPaceEnd = t1;
    if (!period || period > 200000) return;
    const uint64_t busyAll = period > sleep ? period - sleep : 0;
    const double busy = static_cast<double>(busyAll > g.spentUs ? busyAll - g.spentUs : 0);  // without our units
    g.busyUs = busy > g.busyUs ? busy : g.busyUs * 0.9 + busy * 0.1;
    float cap = 0;
    __try {
        void* const* vt = *static_cast<void* const* const*>(engine);
        cap = reinterpret_cast<GetMaxTickRateFn>(vt[g.getMaxTickRateSlot / 8])(engine, static_cast<float>(period / 1e6), true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cap = 0;
    }
    g.capPeriodUs = cap > 1.0f && cap < 1000.0f ? 1e6 / cap : 0;
}

// Runs the owed part of the pairing queue (and the deferred delegates) at once, without deferring.
void Drain(void* manager, bool delegates = true)
{
    if (g.E >= g.S && (!g.count || !delegates)) return;
    s.drains.fetch_add(1, std::memory_order_relaxed);
    g.frameFlags |= 1;
    if (delegates) ReplayAll();
    const bool saved = g.allowDefer;
    g.allowDefer = false;
    g.inMainRun = true;
    g.session = g.findSession(static_cast<uint8_t*>(manager) + 0x80, -1);
    while (g.E < g.S) {
        g.deferredNow = false;
        g.origRun(manager, -1);
        ++g.E;
        g.lastCreateFar = false;
    }
    g.inMainRun = false;
    g.allowDefer = saved;
}

// The frame's RunSession(-1): units under the budget, catch-up of owed OT frames while budget is left.
uint8_t FrameRun(void* manager)
{
    LedgerPreActorTick();  // HitchLedger: OnWorldPreActorTick (no-op unless recording)
    if (g.frame) EndFrameStats();  // the previous frame, including the delegate units after its post-tick
    for (int& n : g.frameTypes) n = 0;
    g.frameFlags = 0;
    ++g.frame;
    // The OT can post its end command for frame S only after the GT's reverse end command of frame S-1; a held one (c)
    // means no new OT frame since.
    if (g.endHeld) g.endHeld = false;
    else ++g.S;
    g.units = 0;
    g.spentUs = 0;
    g.unitUs = 0;
    g.budgetThisFrame = FrameBudget();
    g.session = g.findSession(static_cast<uint8_t*>(manager) + 0x80, -1);
    const bool enabled = g.enabled.load(std::memory_order_relaxed);
    g.allowDefer = enabled && GuardsPass(static_cast<const uint8_t*>(g.session));
    g.inFrame = true;
    g.frameStart2Us = g.frameStart1Us;  // FadeInUnseen (rev96d A2)
    g.frameStart1Us = g.frameStartUs;
    g.frameStartUs = NowUs();
    // PacingGateShownHoldMs (fix102, rev102a S2): the due spread's period ring gets a sample every frame (before: only on
    // frames with candidates, so an old long frame could stay in it)
    if (g.frameStart1Us && g.frameStartUs > g.frameStart1Us) {
        g.periodRing[g.periodNext] = g.frameStartUs - g.frameStart1Us;
        g.periodNext = (g.periodNext + 1) % 8;
    }
    // The older work first; but while a level flow waits (c), the main queue first: the flow and the OT wait for it.
    if (!g.allowDefer) ReplayAll();
    else if (g.count && g.heldFrames == 0 && FifoAge() >= PairLag()) ReplayBudget(false);
    g.inMainRun = true;
    MapPopupSpreadPairing(true);  // MapPopupSpread: map icon lists may be kept until this run ends
    uint8_t r = 4;
    bool first = true;
    const uint64_t pair0 = NowUs(), pairUnits0 = g.unitCumUs;
    for (;;) {
        g.deferredNow = false;
        const uint64_t t0 = NowUs(), u0 = g.unitUs;
        r = g.origRun(manager, -1);
        if (!first) {
            const uint64_t all = NowUs() - t0, unit = g.unitUs - u0;
            g.spentUs += all > unit ? all - unit : 0;
        }
        first = false;
        if (g.deferredNow) break;
        ++g.E;
        g.lastCreateFar = false;
        if (g.E >= g.S) break;
        if (!g.allowDefer || PairForced() || g.spentUs < LagBudget(PairLag())) continue;
        break;  // budget spent: the rest of the queue waits at an OT frame boundary
    }
    {   // HitchLedger: the pairing calls' time without their units (the wait for the OT frame and the catch-up's commands)
        const uint64_t all = NowUs() - pair0, units = g.unitCumUs - pairUnits0;
        g.pairCumUs += all > units ? all - units : 0;
    }
    g.inMainRun = false;
    MapPopupSpreadPairing(false);  // MapPopupSpread: kept map icon lists run now
    if (g.deferredNow || g.E < g.S) s.deferFrames.fetch_add(1, std::memory_order_relaxed);
    // ClothBuildDefer (cloth_defer.cpp): the cloth simulation builds that wait, in what the frame's budget has left;
    // they count as units (the ledger's NPC units; the delegate units after the post-tick no longer run as the frame's
    // first unit whatever the budget)
    int clothBuilds = 0;
    const uint64_t clothUs = ClothDeferFrame(g.frame, !g.allowDefer, g.spentUs, g.budgetThisFrame, g.units > 0, &clothBuilds);
    if (clothBuilds) {
        g.spentUs += clothUs;
        g.unitUs += clothUs;
        g.unitCumUs += clothUs;
        g.unitCumN += static_cast<uint64_t>(clothBuilds);
        g.units += clothBuilds;
    }
    return r;
}

constexpr int32_t kTestFlowKey = 1000;  // self-test: sessions with a key from here on are flows

// (c) A level flow called by the flow runner while the main queue still owes OT frames waits for the next frame, and
// the frame's reverse end command with it (HandlerEndHook): true = return "not finished" without running it.
bool IsFlow(const void* ret, int32_t key)
{
    return g.testAnyCaller ? key >= kTestFlowKey : (ret == g.flowReturn[0] || ret == g.flowReturn[1]);
}

bool FlowWaitActive()
{
    return g.chooseWriter && g.inFrame && g.allowDefer && g.enabled.load(std::memory_order_relaxed) &&
           g.flowWait.load(std::memory_order_relaxed);
}

bool FlowWaits(const void* ret, int32_t key)
{
    if (!IsFlow(ret, key) || g.E >= g.S || !FlowWaitActive()) return false;
    // past kMaxFlowWait only while the main queue is getting through the flow's own OT frame, a unit per frame.
    // PacingFlowProgress (drainfix 2026-11-12): also while it gets through older OT frames owed (a create or head/BP unit
    // ran this frame), and while the queue's head create waits for its asset loads (CreatePreload deferred it this frame):
    // at the 3-frame bound the drain ran the rest of the owed queue at once, two creates in one frame (15-24 ms, Imperial
    // City Waterfront), or ran the head create with its load still in flight: 24-28 ms of synchronous load in the frame.
    const bool fp = g.flowProgress.load(std::memory_order_relaxed);
    const bool ran = g.frameTypes[kCreate] + g.frameTypes[kGeneric] > 0;
    const bool loading = fp && g.preloadWaitFrame == g.frame;
    const bool progressing = (ran && (PairLag() == 0 || fp)) || loading;
    const int bound = progressing ? std::max(kMaxFlowWait, g.flowWaitFrames.load(std::memory_order_relaxed)) : kMaxFlowWait;
    if (g.heldFrames >= bound) {  // waited long enough: the queue drains as before
        if (!g.holdEnd) s.flowDrains.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g.holdEnd && g.heldFrames >= kMaxFlowWait) {  // PacingFlowProgress: held past 3 only because of the key
        if (loading && !ran) s.flowLoadHeld.fetch_add(1, std::memory_order_relaxed);
        else if (ran && PairLag() > 0) s.flowLagHeld.fetch_add(1, std::memory_order_relaxed);
    }
    if (!g.holdEnd && g.flowLogs < 12) {
        ++g.flowLogs;
        Log("[pacing] frame %llu: level flow %d waits for the main queue (%llu OT frames owed, %d frames held so far)",
            static_cast<unsigned long long>(g.frame), key, static_cast<unsigned long long>(g.S - g.E), g.heldFrames);
    }
    if (!g.holdEnd && g.heldFrames >= kMaxFlowWait) s.flowLongWaits.fetch_add(1, std::memory_order_relaxed);
    g.holdEnd = true;
    g.frameFlags |= 8;
    s.flowWaits.fetch_add(1, std::memory_order_relaxed);
    return true;
}


uint8_t RunSessionHook(void* manager, int32_t key)
{
    const void* ret = _ReturnAddress();
    if (key == -1 && (ret == g.preTickReturn || g.testAnyCaller) && g.depth == 0 && !g.inMainRun) {
        g.gtId = GetCurrentThreadId();
        return FrameRun(manager);
    }
    if (OnGameThread() && g.depth == 0 && !g.inMainRun) {
        if (key != -1 && FlowWaits(ret, key)) return kFlowNotFinished;
        // (c) a flow with the main queue complete runs now: the delegates waiting in our queue are Unreal-side load
        // completions, which the main queue's commands overtake every frame as well; they keep their own order.
        if (key != -1 && g.E >= g.S && IsFlow(ret, key) && FlowWaitActive()) {
            const uint64_t t0 = NowUs(), units0 = g.unitCumUs;
            const uint8_t r = g.origRun(manager, key);
            const uint64_t all = NowUs() - t0, units = g.unitCumUs - units0;  // HitchLedger
            g.flowCumUs += all > units ? all - units : 0;
            return r;
        }
        if (key != -1) {
            // another session (travel, or a cell's pre-placed actor flow) keeps its stock order against the main queue.
            // PacingLeanFlowDrain: a level flow at its wait bound needs only the main queue before it; the waiting delegates
            // (Unreal-side load completions, which in the game run after the frame's flows) stay queued, as when a flow
            // runs on a complete queue above. (2026-10-20, Imperial City flight: one flow at its bound drained 14-18
            // delegates with the owed queue, 17-20 units and 30-40 ms in one frame, in every run.)
            const bool lean = IsFlow(ret, key) && FlowWaitActive() && g.leanFlowDrain.load(std::memory_order_relaxed);
            if ((g.E < g.S || (g.count && !lean)) && g.drainLogs < 12) {
                ++g.drainLogs;
                Log("[pacing] session %d run by exe+0x%llX: owed queue (%llu OT frames, %zu delegates%s) runs first", key,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(Game().base)),
                    static_cast<unsigned long long>(g.S - g.E), g.count, lean ? ", which stay queued" : "");
            }
            if (lean) {
                if (g.E < g.S) {
                    s.leanDrains.fetch_add(1, std::memory_order_relaxed);
                    s.leanKept.fetch_add(g.count, std::memory_order_relaxed);
                }
                Drain(manager, false);
            } else
            Drain(manager);
        } else {
            // an unexpected RunSession(-1) on the game thread: it consumes an end command like ours
            const uint8_t r = g.origRun(manager, key);
            ++g.E;
            return r;
        }
    }
    return g.origRun(manager, key);
}

void HandlerEndHook(void* manager, void* tag, uint64_t a3, uint64_t a4)
{
    const void* ret = _ReturnAddress();
    if (ret == g.postTickReturn) LedgerPostActorTick();  // HitchLedger: OnWorldPostActorTick (no-op unless recording)
    if (ret == g.postTickReturn && OnGameThread()) MapPopupSpreadPostTick();  // MapPopupSpread: safety flush (counted late)
    if (ret == g.postTickReturn && OnGameThread()) BodySpreadPostTick();  // BodyRemovalSpread: its drain point
    if ((ret == g.postTickReturn || g.testAnyCaller) && OnGameThread() && g.depth == 0 && !g.inMainRun && g.inFrame) {
        g.gateS3Us = NowUs();  // PacingBaseGate: the post-actor tick (the rest of the frame is measured from here)
        g.gateS3UnitUs = g.unitUs;
        g.gateS3Frame = g.frame;
        if (!g.allowDefer || !g.enabled.load(std::memory_order_relaxed)) ReplayAll();
        else ReplayBudget(true);
        g.inFrame = false;
        if (g.holdEnd) {
            // (c) a flow waits: so does this frame's end command. The OT keeps running our reverse commands as they come
            // and starts its next frame when the flows have run. The open flag is cleared as Handler_End would.
            g.holdEnd = false;
            g.endHeld = true;
            ++g.heldFrames;
            s.heldEnds.fetch_add(1, std::memory_order_relaxed);
            g.chooseWriter(static_cast<uint8_t*>(manager) + 0x80)[kSessOpen] = 0;
            return;
        }
        g.heldFrames = 0;
    }
    g.origEnd(manager, tag, a3, a4);
}

ExecIfSafeFn const kExecIfSafeHooks[kNumDelegates] = {&ExecIfSafeHook<kAppearance>, &ExecIfSafeHook<kEquipment>,
                                                      &ExecIfSafeHook<kHair>};
ExecFn const kExecuteHooks[kNumDelegates] = {&ExecuteHook<kAppearance>, &ExecuteHook<kEquipment>, &ExecuteHook<kHair>};


}  // namespace

// CmdMemPregrow self-test: MaxTickRateHook must call CmdMemPregrowFrame (its code is searched for the call).
const void* NpcPacingMaxTickHookCode() { return reinterpret_cast<const void*>(&MaxTickRateHook); }


bool FindNpcPacing()
{
    if (g.found) return true;
    const GameModule& game = Game();
    if (!FindPairing(game) || !FindDelegates(game) || !FindPlayer(game)) return false;
    // optional: the frame-rate cap's sleep (without it the fixed budget is used)
    const auto paces = FindAll(game.text, ParsePattern(kMaxTickRate));
    if (paces.size() == 1) {
        g.numMaxTickSlots = FindSlots(game, paces[0], g.maxTickSlots, 4);
        uint8_t* calls = FindNear(paces[0], 0x400, kMaxTickCalls);
        if (calls) g.getMaxTickRateSlot = Disp32(calls + kMaxTickRateSlot);
        g_capCallSite = calls ? calls + kCapCallAt : nullptr;  // CapSleepSkip
        if (g.numMaxTickSlots < 1 || g.numMaxTickSlots > 4 || !calls || g.getMaxTickRateSlot % 8 || g.getMaxTickRateSlot > 0x2000)
            g.numMaxTickSlots = 0;
        else g.origMaxTick = reinterpret_cast<MaxTickRateFn>(paces[0]);
    }
    if (g.origMaxTick)
        Log("[pacing] frame-rate cap: UpdateTimeAndHandleMaxTickRate exe+0x%X (%d slots), GetMaxTickRate vtable +0x%X",
            static_cast<unsigned>(paces[0] - game.base), g.numMaxTickSlots, g.getMaxTickRateSlot);
    else Log("[pacing] frame-rate cap sleep not found: PacingUseSlack has no effect");
    FindFade(game);  // optional (PacingFadeAware)
    g.freezeKnown = FindFreezeSubsystem();
    if (!g.freezeKnown) {
        Log("[pacing] menu/loading freeze state not available: fix not available");
        return false;
    }
    g.found = true;
    return true;
}

bool InstallNpcPacing()
{
    if (g.installed) return true;
    if (!FindNpcPacing()) return false;
    if (!g.fifo) g.fifo = static_cast<Entry*>(VirtualAlloc(nullptr, sizeof(Entry) * kFifoCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g.fifo) return false;
    bool ok = true;
    ok &= WritePointer(&g.del[0].vtable[kSlotExecuteIfSafe], reinterpret_cast<void*>(kExecIfSafeHooks[0]));
    ok &= WritePointer(&g.del[1].vtable[kSlotExecuteIfSafe], reinterpret_cast<void*>(kExecIfSafeHooks[1]));
    ok &= WritePointer(&g.del[2].vtable[kSlotExecuteIfSafe], reinterpret_cast<void*>(kExecIfSafeHooks[2]));
    for (int i = 0; i < kNumDelegates; ++i)
        ok &= WritePointer(&g.del[i].vtable[kSlotExecute], reinterpret_cast<void*>(kExecuteHooks[i]));
    ok &= WritePointer(g.createSlot, reinterpret_cast<void*>(&CreateExecHook));
    for (int i = 0; i < g.numGenericSlots; ++i) ok &= WritePointer(g.genericSlots[i], reinterpret_cast<void*>(&GenericExecHook));
    ok &= WritePointer(g.handlerEndSlot, reinterpret_cast<void*>(&HandlerEndHook));
    for (int i = 0; i < g.numMaxTickSlots; ++i) ok &= WritePointer(g.maxTickSlots[i], reinterpret_cast<void*>(&MaxTickRateHook));
    ok &= WritePointer(g.runSessionSlot, reinterpret_cast<void*>(&RunSessionHook));  // last: everything else is ready
    if (!ok) Log("[pacing] warning: a vtable write failed");
    g.installed = true;
    Log("[pacing] NPC creation pacing installed (%d slots)", 9 + g.numGenericSlots + g.numMaxTickSlots);
    return true;
}

void SetNpcPacing(bool enabled, int budgetMs, int minDistance, int maxLagFrames, bool useSlack, bool flowWait, int flowWaitFrames)
{
    g.useSlack = useSlack;
    g.flowWait = flowWait;
    g.flowWaitFrames = std::clamp(flowWaitFrames, kMaxFlowWait, 30);
    const bool was = g.enabled.exchange(enabled && g.installed);
    g.budgetUs = std::clamp(budgetMs, 0, 100) * 1000;
    g.minDistance = std::clamp(minDistance, 0, 1000000) * kOldToUe;
    g.maxLag = std::clamp(maxLagFrames, 1, 600);
    if (was != g.enabled.load()) Log("[pacing] NPC creation pacing %s", g.enabled.load() ? "on" : "off (backlog runs at once)");
}

void SetNpcPacingOwnerWalk(bool on)
{
    if (g.ownerWalk.exchange(on) != on)
        Log("[pacing] a delegate owner's non-actor objects %s (PacingOwnerWalk %d)", on ? "are skipped" : "end the walk", on);
}

void SetNpcPacingHairActor(bool on)
{
    if (g.hairActor.exchange(on) != on)
        Log("[pacing] hair units %s (PacingHairActor %d%s)", on ? "are paced by their character" : "count as near (as before)", on,
            on && !g.hairActorWeak ? "; not available: the hair delegate's mesh component was not recognised" : "");
}

void SetNpcPacingFlowProgress(bool on)
{
    if (g.flowProgress.exchange(on) != on)
        Log("[pacing] a waiting level flow counts %s (PacingFlowProgress %d)",
            on ? "creates run with older frames owed and a create waiting for its loads as progress"
               : "only creates of its own frame as progress (as before)", on);
}

void SetNpcPacingBaseGate(int value)
{
    const int us = value <= 0 ? 0 : value == 1 ? 25000 : std::clamp(value, 18, 50) * 1000;
    if (g.gateUs.exchange(us) != us)
        Log("[pacing] far NPC steps after the actor tick %s (PacingBaseGate %d%s)",
            us ? "run only while the frame stays within the target" : "run under the budget alone (as before)", us / 1000,
            us && !g.origMaxTick && !g.testClock ? " ms; not available: the frame-rate cap's sleep was not found" : us ? " ms" : "");
}

// self-test: the fade hold's step for an actor (GateFadeStep)
float NpcPacingTestFadeHold(const void* actor, float* value, float floor, float rate, float dt)
{
    return GateFadeStep(reinterpret_cast<uintptr_t>(actor), value, floor, rate, dt);
}

void SetNpcPacingGateBounds(int boundFrames, int calmMs, int nearM, int fadeHoldMs)
{
    const int b = std::clamp(boundFrames, 10, 180);
    const int c = calmMs <= 0 ? 0 : std::clamp(calmMs, 10, 30) * 1000;
    const int n = std::clamp(nearM, 0, 500) * 100;
    const int h = 0;  // the fade hold is lab-only (rev96e B1)
    (void)fadeHoldMs;
    const bool changed = g.gateBound.exchange(b) != b;
    const bool changedC = g.gateCalmUs.exchange(c) != c;
    const bool changedN = g.gateNearCm.exchange(n) != n;
    const bool changedH = g.gateHoldUs.exchange(h) != h;
    if (!h) g.gateHoldN = 0;
    if (changed || changedC || changedN || changedH)
        Log("[pacing] PacingBaseGate: a held far unit runs within %d frames at the latest (PacingGateBoundFrames), on a frame "
            "whose own time stays below %d ms %s(PacingGateCalmMs), and never holds an actor nearer than %d m (PacingGateNearM); "
            "fade hold %d ms (lab PacingGateFadeHoldMs%s)", b, c / 1000, c ? "" : "never: off ", n / 100, h / 1000,
            h ? "" : ": off");
}

void NpcPacingTestFrameEdge()
{
    const uint64_t t = NowUs();
    GateFrameEdge(t);
    g.lastPeriodUs = g.lastPaceEnd && t - g.lastPaceEnd > g.unitUs ? t - g.lastPaceEnd - g.unitUs : 0;  // as MaxTickRateHook
    g.lastPaceEnd = t;
}

void SetNpcPacingLeanFlowDrain(bool on)
{
    if (g.leanFlowDrain.exchange(on) != on)
        Log("[pacing] a level flow at its wait bound drains %s (PacingLeanFlowDrain %d)",
            on ? "the main queue only, the waiting delegates stay queued" : "the main queue and the waiting delegates", on);
}

void SetNpcPacingFadeAware(int mode)
{
    const int fa = std::clamp(mode, 0, 1);
    const int was = g.fadeAware.exchange(g.fadeFound ? fa : 0);
    if (was != g.fadeAware.load())
        Log("[pacing] PacingFadeAware %d%s", g.fadeAware.load(),
            fa && !g.fadeFound ? " [spawn fade-in component not found: off]"
            : fa              ? " (a far NPC still invisible in its spawn fade-in counts as unseen)"
                              : " (only the rendered test, as 1.2.2)");
}





// ---- FadeInUnseen (worker stampfix, 2026-11-13; the idea is Epic's, UE 5.5.0, commit 8dd2cb4237 / CL 36133226) ----
// The engine's behaviour (UE 5.3): a primitive without an occlusion history (a new one) draws as visible until its first
// occlusion result arrives, and every drawn primitive gets the frame's LastRenderTime (SceneVisibility.cpp ComputeRelevance ->
// FPrimitiveSceneInfo::UpdateComponentLastRenderTime). A skinned mesh with OnlyTickPoseWhenRendered and the animation budget
// allocator's ShouldComponentTick tick and evaluate its pose while that time is within 1.0 s of the world time. The game's
// behaviour: every spawned NPC fades in by dither over 1.25 s. Together: a crowd created 120-150 m away behind buildings is
// "rendered" in its first frames (no occlusion result yet) and then ticks and evaluates its animation at full rate for one
// second although nobody sees it (worker animbudget, census 2026-11-12: parallel evaluations 4 -> 15 a frame, +5-6 ms per
// frame for ~1.05 s at the IC Waterfront). Epic changed the stamp in UE 5.5.0 (8dd2cb4237, CL 36133226, 2024-09-09; 5.5.4
// SceneVisibility.cpp:2014-2019): the component's render time is updated only once its occlusion state is known, so that
// components pending an occlusion result are not counted as rendered. Our version of that idea, without touching the render
// thread: after the fade component's tick of a far character (PacingMinDistance, as PacingFadeAware) still in its spawn
// fade-in (state 1), each of its non-static meshes whose LastRenderTime (not LastRenderTimeOnScreen, which texture
// streaming reads) is older than FadeInUnseenWindowMs (default 200 ms) but still inside the engine's 1 s window gets it
// set to just past that window: a body the renderer keeps stamping (in view, or through a shadow or ray-traced stamp: its
// stamp is at most a frame or two old) is never touched and animates every frame at every dither level, exactly as
// without the key, even across a 150 ms hitch frame; a body stamped only in its first frames (occluded once the first
// occlusion result came) stops 200 ms after its last stamp instead of 1 s after. No state is kept: the renderer raises the
// time again whenever it draws the body. (rev96a: the first version reset the time while dither >= 4 and opened a window
// below it; that relied on dither 4-5 being invisible and opened the window a frame late, so it was replaced by this
// age rule, which needs no visibility threshold.) Equivalence: an unseen new far
// character's animation (notifies, montages) then starts when it is first seen, as for every unseen character in the game
// after its first second, and as in UE 5.5+; not identical to 5.3. Static meshes (weapons, shields) keep their stamps.
// Hook: the fade component's TickComponent override (exe+0x48D18C0) ends in a tail jump to the tick body (exe+0x48D5A40);
// that jump's rel32 is pointed at a stub to FadeTickHook, which calls the body and then reads the fresh mesh list the
// body's refresh (exe+0x48CD850) leaves in the component (+0xE0 data / +0xE8 count; refreshed on every FadeIn tick).
// Start-up install only (the rel32 is written before game code runs); the key is live afterwards (0 = the hook only calls
// the body).
constexpr char kFadeTickCaller[] =
    "40 53 48 83 EC 30 0F 29 74 24 20 48 8B D9 0F 28 F1 E8 ?? ?? ?? ?? 80 BB ?? ?? 00 00 00 48 8B CB 74 0F 0F 28 74 24 20 "
    "48 83 C4 30 5B E9 ?? ?? ?? ?? 0F 28 CE 0F 28 74 24 20 48 83 C4 30 5B E9";
constexpr size_t kFadeTickJmpAt = 62;  // the tail jump to the tick body
// the refresh (exe+0x48CD850), 0x30 into it: lea rsi,[rcx+list]; xor edi,edi; ...; mov [rsi+8],edi; cmp [rsi+0Ch],edi
constexpr char kFadeListHead[] = "48 8D B1 ?? ?? 00 00 33 FF 0F 28 FB 45 0F B6 E0 4C 8B EA 89 7E 08 39 7E 0C";
// its per-mesh loop: SetDoDither, SetDitherScale, then IsA(UStaticMeshComponent) (the getter's call at +24)
constexpr char kFadeListLoop[] =
    "48 8B CB E8 ?? ?? ?? ?? 0F 28 CF 48 8B CB E8 ?? ?? ?? ?? 48 85 DB 74 ?? E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 30 48 63 40 "
    "38 3B 42 38 7F";
constexpr size_t kFadeStaticClassAt = 24;

using FadeTickFn = void (*)(uint8_t* comp, float dt);
struct FadeUnseen {
    bool found = false, installed = false;
    uint8_t* jmp = nullptr;            // the caller's tail jump (E9 rel32)
    FadeTickFn body = nullptr;         // the tick body (the jump's target)
    uint32_t list = 0;                 // the component's mesh list (TArray data; count +8)
    uint32_t compLast = 0;             // UPrimitiveComponent::LastRenderTime
    StaticClassFn staticMesh = nullptr;
    std::atomic<int> mode{0};          // FadeInUnseen: 0 off, 1 far characters (lab 2: any distance)
    std::atomic<int> windowMs{200};    // lab FadeInUnseenWindowMs
    double (*testNow)() = nullptr;     // self-test: fake world time
    double testSlack = -1.0;           // self-test: the last two frames' time (s; < 0 = from the frame clock)
    // per stats interval (written on the game thread, read and zeroed by the stats thread)
    std::atomic<uint64_t> ticks{0}, farTicks{0}, clamped{0}, skippedStatic{0}, failed{0};
} u;

bool IsA(const uint8_t* obj, const uint8_t* cls)
{
    if (!obj || !cls) return false;
    const uint8_t* const oc = *reinterpret_cast<const uint8_t* const*>(obj + 0x10);
    if (!oc) return false;
    const int32_t idx = *reinterpret_cast<const int32_t*>(cls + 0x38);
    if (idx < 0 || idx > *reinterpret_cast<const int32_t*>(oc + 0x38)) return false;
    const uint8_t* const* chain = *reinterpret_cast<const uint8_t* const* const*>(oc + 0x30);
    return chain && chain[idx] == cls + 0x30;
}

double WorldNow(const uint8_t* actor, bool* ok)
{
    *ok = false;
    if (u.testNow) {
        *ok = true;
        return u.testNow();
    }
    if (!g.actorWorld) return 0.0;
    const uint8_t* world = static_cast<const uint8_t*>(g.actorWorld(actor));
    if (!world) return 0.0;
    *ok = true;
    return *reinterpret_cast<const double*>(world + g.timeOffset);
}

// After the fade component's tick (game thread). Returns the meshes whose render time it expired (self-test).
int FadeUnseenAfterTick(uint8_t* comp)
{
    const int mode = u.mode.load(std::memory_order_relaxed);
    if (!mode || !comp) return 0;
    int changed = 0;
    __try {
        // only a spawn fade-in still running after this tick (state 1): the body refreshed the mesh list in it
        if (*reinterpret_cast<const int32_t*>(comp + g.fadeState) != 1) return 0;
        const uint8_t* const actor = *reinterpret_cast<const uint8_t* const*>(comp + g.fadeOwner);
        if (!actor || *reinterpret_cast<uint8_t* const*>(actor + g.fadeComp) != comp) return 0;  // the actor's own fade
        uint8_t* const* list = *reinterpret_cast<uint8_t* const* const*>(comp + u.list);
        const int32_t n = *reinterpret_cast<const int32_t*>(comp + u.list + 8);
        if (!list || n <= 0 || n > 64) return 0;
        u.ticks.fetch_add(1, std::memory_order_relaxed);
        if (mode == 1) {
            double x, y, z;
            if (!ActorLocationGuarded(actor, &x, &y, &z) || !FarFrom(x, y, z)) return 0;
        }
        u.farTicks.fetch_add(1, std::memory_order_relaxed);
        bool timeOk = false;
        const double now = WorldNow(actor, &timeOk);
        if (!timeOk) return 0;
        // rev96d A2: the freshest stamp the game thread can see is one or two frames old, so a drawn body's age reaches this
        // frame's and the previous frame's time: the window counts from there (real time, which world time never exceeds:
        // world deltas are the real ones, dilated or clamped). A body is dropped only once it has not been drawn for the
        // window beyond that, also across a hitch frame of 0.2-0.4 s.
        double slack = u.testSlack;
        if (slack < 0.0)
            slack = g.frameStart2Us && g.frameStartUs > g.frameStart2Us
                        ? std::min(1.0, static_cast<double>(g.frameStartUs - g.frameStart2Us) / 1e6)
                        : 1.0;  // not known yet: drop nothing
        const double window = u.windowMs.load(std::memory_order_relaxed) / 1000.0 + slack;
        const float expired = static_cast<float>(now - 1.0) - 0.001f;  // just outside the engine's 1 s window
        for (int32_t i = 0; i < n; ++i) {
            uint8_t* const m = list[i];
            if (!m) continue;
            if (u.staticMesh && IsA(m, u.staticMesh())) {
                u.skippedStatic.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            float* const last = reinterpret_cast<float*>(m + u.compLast);
            const double age = now - static_cast<double>(*last);
            if (age > window && *last > expired) {  // not drawn for the window (+ two frames), still inside the engine's 1 s
                *last = expired;
                ++changed;
            }
        }
        if (changed) u.clamped.fetch_add(changed, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        u.failed.fetch_add(1, std::memory_order_relaxed);
    }
    return changed;
}


void FadeTickHook(uint8_t* comp, float dt)
{
    if (g.gateHoldN && g.gateHoldUs.load(std::memory_order_relaxed) && OnGameThread()) {
        dt = GateFadeDt(comp, dt);  // lab PacingGateFadeHoldMs (gate2; off by default, rev96e B1)
    }
    u.body(comp, dt);
    if (u.mode.load(std::memory_order_relaxed) && OnGameThread()) FadeUnseenAfterTick(comp);
}

bool FindFadeUnseen()
{
    if (u.found) return true;
    const GameModule& game = Game();
    if (!g.fadeFound) {  // (the world time function: FindPlayer, required by the pacing itself)
        Log("[fadeunseen] the spawn fade-in component was not found: FadeInUnseen not available");
        return false;
    }
    const auto ticks = FindAll(game.text, ParsePattern(kFadeTick));
    const auto callers = FindAll(game.text, ParsePattern(kFadeTickCaller));
    const auto rendered = FindAll(game.text, ParsePattern(kCompRendered));
    if (ticks.size() != 1 || callers.size() != 1 || rendered.size() != 1) {
        Log("[fadeunseen] code not recognised (tick %zu, caller %zu, render time %zu matches): FadeInUnseen not available",
            ticks.size(), callers.size(), rendered.size());
        return false;
    }
    uint8_t* const jmp = callers[0] + kFadeTickJmpAt;
    if (RipTarget(jmp, 1, 5) != ticks[0]) {
        Log("[fadeunseen] the fade tick's caller does not jump to the tick body: FadeInUnseen not available");
        return false;
    }
    // the refresh: the tick body's call whose target has the list head 0x28-0x48 in
    uint8_t* refresh = nullptr;
    uint8_t* head = nullptr;
    for (uint8_t* p = ticks[0]; p < ticks[0] + 0x200 && !refresh; ++p) {
        if (*p != 0xE8) continue;
        uint8_t* const t = RipTarget(p, 1, 5);
        if (!game.text.Contains(t) || !game.text.Contains(t + 0x80)) continue;
        if (uint8_t* h = FindNear(t + 0x28, 0x20 + 25, kFadeListHead)) {
            refresh = t;
            head = h;
        }
    }
    uint8_t* const loop = refresh ? FindNear(refresh, 0x800, kFadeListLoop) : nullptr;
    uint8_t* const sm = loop ? RipTarget(loop + kFadeStaticClassAt, 1, 5) : nullptr;
    const uint32_t list = head ? Disp32(head + 3) : 0;
    const uint32_t last = Disp32(rendered[0] + kCompLastAt);
    if (!head || !loop || !game.text.Contains(sm) || !list || list > 0x400 || list % 8 || last > 0x1000 || last % 4 ||
        Disp32(rendered[0] + kCompTimeAt) != g.timeOffset) {
        Log("[fadeunseen] the fade component's mesh list or the render time layout not recognised: FadeInUnseen not available");
        return false;
    }
    u.jmp = jmp;
    u.body = reinterpret_cast<FadeTickFn>(ticks[0]);
    u.list = list;
    u.compLast = last;
    u.staticMesh = reinterpret_cast<StaticClassFn>(sm);
    u.found = true;
    Log("[fadeunseen] fade tick caller's jump exe+0x%X -> body exe+0x%X; mesh list +0x%X (refresh exe+0x%X), static mesh class "
        "exe+0x%X, LastRenderTime +0x%X",
        static_cast<unsigned>(jmp - game.base), static_cast<unsigned>(ticks[0] - game.base), list,
        static_cast<unsigned>(refresh - game.base), static_cast<unsigned>(sm - game.base), last);
    return true;
}

bool InstallFadeInUnseen(bool beforeGameCode)
{
    if (u.installed) return true;
    if (!g.installed) {
        Log("[fadeunseen] FadeInUnseen: needs NpcCreationPacing, not installed");
        return false;
    }
    if (!FindFadeUnseen()) return false;
    if (!beforeGameCode) {  // the rel32 is not written atomically: only before the game's code runs
        Log("[fadeunseen] FadeInUnseen: start-up install only, not installed");
        return false;
    }
    uint8_t* const stub = AllocNearGame(16);
    if (!stub) {
        Log("[fadeunseen] no memory near the game's code: FadeInUnseen not installed");
        return false;
    }
    uint8_t code[14] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; dq FadeTickHook
    const uint64_t hook = reinterpret_cast<uint64_t>(&FadeTickHook);
    memcpy(code + 6, &hook, 8);
    memcpy(stub, code, sizeof code);
    DWORD old;
    VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    const int32_t rel = static_cast<int32_t>(stub - (u.jmp + 5));
    uint8_t insn[5] = {0xE9};
    memcpy(insn + 1, &rel, 4);
    if (!WriteCode(u.jmp, insn)) {
        Log("[fadeunseen] code write failed: FadeInUnseen not installed");
        return false;
    }
    u.installed = true;
    Log("[fadeunseen] fade tick hook installed (FadeInUnseen; lab: PacingBaseGate's fade hold)");
    return true;
}

void SetFadeInUnseen(int mode, int windowMs)
{
    const int m = std::clamp(mode, 0, 1);
    (void)windowMs;
    const int was = u.mode.exchange(u.installed ? m : 0);
    if (was != u.mode.load())
        Log("[fadeunseen] FadeInUnseen %d%s", u.mode.load(),
            u.mode.load() ? ": a far character in its spawn fade-in not drawn for the window no longer counts as rendered"
                          : " (as the game)");
}

void LogFadeInUnseenStats()
{
    if (!u.installed) return;
    const uint64_t t = u.ticks.exchange(0), f = u.farTicks.exchange(0), c = u.clamped.exchange(0);
    const uint64_t st = u.skippedStatic.exchange(0), x = u.failed.exchange(0);
    if (!(t || c || x)) return;
    Log("[fadeunseen] %llu fade-in ticks (%llu of far characters): %llu mesh render times expired after %d ms not drawn, %llu "
        "static meshes left alone, %llu faults",
        (unsigned long long)t, (unsigned long long)f, (unsigned long long)c, u.windowMs.load(), (unsigned long long)st,
        (unsigned long long)x);
}

// self-test
bool FadeInUnseenInfo(uint32_t* jmpRva, uint32_t* bodyRva, uint32_t* list, uint32_t* compLast, uint32_t* staticClassRva)
{
    if (!FindFadeUnseen()) return false;
    const uint8_t* base = Game().base;
    *jmpRva = static_cast<uint32_t>(u.jmp - base);
    *bodyRva = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(u.body) - base);
    *list = u.list;
    *compLast = u.compLast;
    *staticClassRva = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(u.staticMesh) - base);
    return true;
}
void FadeInUnseenTestSlack(double seconds) { u.testSlack = seconds; }
int FadeInUnseenTestTick(uint8_t* comp, int mode, double (*now)(), const uint8_t* (*staticMesh)())
{
    const int savedMode = u.mode.load();
    const StaticClassFn savedSm = u.staticMesh;
    u.mode = mode;
    u.testNow = now;
    u.staticMesh = staticMesh;
    const int r = FadeUnseenAfterTick(comp);
    u.mode = savedMode;
    u.testNow = nullptr;
    u.staticMesh = savedSm;
    return r;
}

void LogNpcPacingStats()
{
    if (!g.installed) return;
    {   // PacingFadeAware (per stats interval)
        const uint64_t held = g.fadeHeldUnits.exchange(0, std::memory_order_relaxed);
        const uint64_t released = g.fadeReleased.exchange(0, std::memory_order_relaxed);
        if (held || released)
            Log("[pacing] PacingFadeAware: %llu units of far NPCs still invisible in their spawn fade-in waited; %llu released by "
                "the re-check", static_cast<unsigned long long>(held), static_cast<unsigned long long>(released));
    }
    LogFadeInUnseenStats();  // FadeInUnseen
    LogShownHoldStats();  // PacingGateShownHoldMs (hold2; no line while nothing happened)
    uint64_t run[kNumTypes], def[kNumTypes], runAll = 0, defAll = 0;
    for (int i = 0; i < kNumTypes; ++i) {
        run[i] = s.run[i].exchange(0);
        def[i] = s.deferred[i].exchange(0);
        runAll += run[i];
        defAll += def[i];
    }
    const uint64_t frames = s.frames.exchange(0);
    if (!runAll && !defAll) return;
    const int lt = s.longestType.exchange(-1);
    Log("[pacing] %llu frames: units run %llu (create %llu, head/BP %llu, appearance %llu, equipment %llu, hair %llu), "
        "deferred %llu (%llu/%llu/%llu/%llu/%llu) in %llu frames, max %llu units and %.1f ms of units per frame, "
        "max lag %llu frames (owed OT frames %llu), max backlog %llu delegates / ring %llu, longest unit %.1f ms (%s), "
        "slack budget in %llu frames (max %.1f ms), forced %llu, drains %llu, level flows waited %llu times in %llu frames "
        "(%llu of them past 3 while the queue progressed; bound %llu), frames not deferring (session/travel/menu/ring/player) "
        "%llu/%llu/%llu/%llu/%llu",
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(runAll),
        static_cast<unsigned long long>(run[0]), static_cast<unsigned long long>(run[1]), static_cast<unsigned long long>(run[2]),
        static_cast<unsigned long long>(run[3]), static_cast<unsigned long long>(run[4]), static_cast<unsigned long long>(defAll),
        static_cast<unsigned long long>(def[0]), static_cast<unsigned long long>(def[1]), static_cast<unsigned long long>(def[2]),
        static_cast<unsigned long long>(def[3]), static_cast<unsigned long long>(def[4]),
        static_cast<unsigned long long>(s.deferFrames.exchange(0)), static_cast<unsigned long long>(s.maxUnitsPerFrame.exchange(0)),
        s.maxFrameUnitUs.exchange(0) / 1000.0, static_cast<unsigned long long>(s.maxLag.exchange(0)),
        static_cast<unsigned long long>(s.maxOwed.exchange(0)), static_cast<unsigned long long>(s.maxFifo.exchange(0)),
        static_cast<unsigned long long>(s.maxRing.exchange(0)), s.longestUs.exchange(0) / 1000.0,
        lt >= 0 ? kTypeName[lt] : "-", static_cast<unsigned long long>(s.slackFrames.exchange(0)),
        s.maxBudgetUs.exchange(0) / 1000.0, static_cast<unsigned long long>(s.forced.exchange(0)),
        static_cast<unsigned long long>(s.drains.exchange(0) + 0 * s.frameLogs.exchange(0)),
        static_cast<unsigned long long>(s.flowWaits.exchange(0)), static_cast<unsigned long long>(s.heldEnds.exchange(0)),
        static_cast<unsigned long long>(s.flowLongWaits.exchange(0)), static_cast<unsigned long long>(s.flowDrains.exchange(0)), static_cast<unsigned long long>(s.guard[0].exchange(0)),
        static_cast<unsigned long long>(s.guard[1].exchange(0)), static_cast<unsigned long long>(s.guard[2].exchange(0)),
        static_cast<unsigned long long>(s.guard[3].exchange(0)), static_cast<unsigned long long>(s.guard[4].exchange(0)));
    {   // PacingLeanFlowDrain
        const uint64_t ld = s.leanDrains.exchange(0), lk = s.leanKept.exchange(0);
        if (ld)
            Log("[pacing] PacingLeanFlowDrain: %llu level flows at their wait bound drained the main queue only; %llu waiting "
                "delegates stayed queued", static_cast<unsigned long long>(ld), static_cast<unsigned long long>(lk));
    }
    {   // PacingFlowProgress
        const uint64_t fl = s.flowLoadHeld.exchange(0), fg = s.flowLagHeld.exchange(0);
        if (fl || fg)
            Log("[pacing] PacingFlowProgress: level flows held past 3 frames %llu times while the queue's head create waited for "
                "its loads, %llu times while creates ran with older OT frames owed", static_cast<unsigned long long>(fl),
                static_cast<unsigned long long>(fg));
    }
    {   // PacingHairActor's fade rate
        const uint64_t fr = s.fadeRate.exchange(0);
        if (fr) Log("[pacing] PacingHairActor: %llu units ran ahead of the budget so that fading characters are built before "
                    "they show (at most 2 and 16 ms a frame)", static_cast<unsigned long long>(fr));
    }
    {   // PacingBaseGate (gate2: per unit, hard bound)
        const uint64_t held = s.gateHeld.exchange(0), ran = s.gateRan.exchange(0), sum = s.gateWaitSum.exchange(0);
        const uint64_t late = s.gateLate.exchange(0), fitRuns = s.gateFit.exchange(0), calm = s.gateCalm.exchange(0);
        const uint64_t turned = s.gateTurned.exchange(0), key0 = s.gateKey0.exchange(0), ageMax = s.gateAgeMax.exchange(0);
        const uint64_t fit = s.gateFitFrames.exchange(0);
        const uint64_t floorTicks = s.gateFloorTicks.exchange(0), floorActors = s.gateFloorActors.exchange(0);
        const uint64_t floorMax = s.gateFloorMax.exchange(0);
        s.gateDrain.exchange(0);
        s.gateFallback.exchange(0);
        const uint64_t over = s.gateOver.exchange(0), oSum = s.gateOverSum.exchange(0), uSum = s.gateUnderSum.exchange(0);
        const uint64_t eMax = s.gateErrMax.exchange(0), forced = s.gateForced.exchange(0);
        s.gateWaitMax.exchange(0);
        uint64_t h[6];
        for (int i = 0; i < 6; ++i) h[i] = s.gateWaitHist[i].exchange(0);
        if (held || ran || late || fitRuns || calm)
            Log("[pacing] PacingBaseGate %d ms (bound %d frames, calm below %.1f ms, near %d m): %llu far hidden units held, "
                "%llu held units ran at an age of %.1f frames on average (<= 5: %llu, 6-15: %llu, 16-30: %llu, 31-60: %llu, "
                "61-90: %llu, > 90: %llu; oldest %llu, past the bound %llu): %llu for room, %llu on a calm frame, %llu for the "
                "bound, %llu once their actor could no longer wait, %llu before a key-0 unit; %llu frames with a fitting run "
                "(busy above the target by > 1 ms: %llu, over the prediction %.2f ms per frame, max %.1f, under %.2f)",
                g.gateUs.load() / 1000, g.gateBound.load(), g.gateCalmUs.load() / 1000.0, g.gateNearCm.load() / 100,
                static_cast<unsigned long long>(held), static_cast<unsigned long long>(ran),
                ran ? static_cast<double>(sum) / ran : 0.0, static_cast<unsigned long long>(h[0]),
                static_cast<unsigned long long>(h[1]), static_cast<unsigned long long>(h[2]), static_cast<unsigned long long>(h[3]),
                static_cast<unsigned long long>(h[4]), static_cast<unsigned long long>(h[5]), static_cast<unsigned long long>(ageMax),
                static_cast<unsigned long long>(forced), static_cast<unsigned long long>(fitRuns),
                static_cast<unsigned long long>(calm), static_cast<unsigned long long>(late), static_cast<unsigned long long>(turned),
                static_cast<unsigned long long>(key0), static_cast<unsigned long long>(fit), static_cast<unsigned long long>(over),
                fit ? oSum / 1000.0 / fit : 0.0, eMax / 1000.0, fit ? uSum / 1000.0 / fit : 0.0);
        if (floorTicks)
            Log("[pacing] PacingBaseGate: %llu far characters' fade-ins waited at the floor (dither 4) for their held steps, "
                "%llu fade ticks, the longest %llu frames", static_cast<unsigned long long>(floorActors),
                static_cast<unsigned long long>(floorTicks), static_cast<unsigned long long>(floorMax));
    }
}

bool NpcPacingDistantUnit(uint64_t* ownerWeak, uint64_t* frame)
{
    if (!g.installed || !OnGameThread() || g.depth == 0 || !g.unitWeak) return false;
    if (!g.enabled.load(std::memory_order_relaxed) || !FrameOpen()) return false;
    if (!g.unitDistant) {
        // pacing's walk ran the unit as near; an equipment unit's owner is the character's body pairing component, where
        // that walk ends: ask again with the guarded walk, which reaches the character (2026-10-27, npcburst: 1 robe
        // build held without it, 9 with it)
        uintptr_t key;
        bool distant;
        if (!ResolveOwnerQuiet(g.unitWeak, &key, &distant) || !distant) return false;
    }
    memcpy(ownerWeak, g.unitWeak, sizeof *ownerWeak);
    *frame = g.frame;
    return *ownerWeak != 0;
}


bool NpcPacingStillDistant(uint64_t ownerWeak)
{
    if (!g.installed || !OnGameThread() || !g.allowDefer || !g.enabled.load(std::memory_order_relaxed)) return false;
    uintptr_t key;
    bool distant;
    return ResolveOwnerQuiet(reinterpret_cast<const uint8_t*>(&ownerWeak), &key, &distant) && distant;
}

bool NpcPacingOnGameThread() { return OnGameThread(); }  // MapPopupSpread (port130k): the pairing hooks' game thread

bool NpcPacingPlayerPos(double* x, double* y, double* z)
{
    if (!g.installed || !OnGameThread() || !Player()) return false;
    *x = g.px;
    *y = g.py;
    *z = g.pz;
    return true;
}


// The last position the game thread saw (any thread; for the GPU crash report: the values may be a frame old or mixed).
bool NpcPacingLastPlayerPos(double* x, double* y, double* z)
{
    if (!g.installed || (g.px == 0 && g.py == 0 && g.pz == 0)) return false;
    *x = g.px;
    *y = g.py;
    *z = g.pz;
    return true;
}

bool NpcPacingInfo(NpcPacingInfoOut* out)
{
    if (!g.found || !out) return false;
    out->runSession = g.runSession;
    out->handlerEnd = g.handlerEnd;
    out->runSessionSlot = g.runSessionSlot;
    out->handlerEndSlot = g.handlerEndSlot;
    out->sessionVtable = g.sessionVtable;
    out->createExec = g.createExec;
    out->createSlot = g.createSlot;
    out->genericExec = g.genericExec;
    out->numGenericSlots = g.numGenericSlots;
    out->preTickReturn = g.preTickReturn;
    out->postTickReturn = g.postTickReturn;
    out->tes = reinterpret_cast<void*>(g.tes);
    out->findSession = reinterpret_cast<void*>(g.findSession);
    for (int i = 0; i < 3; ++i) {
        out->delegateVtable[i] = g.del[i].vtable;
        out->delegateExecIfSafe[i] = reinterpret_cast<void*>(g.del[i].execIfSafe);
        out->delegateSize[i] = g.del[i].size;
        out->delegateCopyCtor[i] = reinterpret_cast<void*>(g.del[i].copyCtor);
        out->execIfSafeHook[i] = reinterpret_cast<void*>(kExecIfSafeHooks[i]);
    }
    out->weakGet = reinterpret_cast<void*>(g.weakGet);
    out->getWorld = reinterpret_cast<void*>(g.getWorld);
    out->gEngine = g.gEngine;
    out->getPc = reinterpret_cast<void*>(g.getPc);
    out->getPawn = reinterpret_cast<void*>(g.getPawn);
    out->rootOffset = g.rootOffset;
    out->locationOffset = g.locationOffset;
    out->runHook = reinterpret_cast<void*>(&RunSessionHook);
    out->endHook = reinterpret_cast<void*>(&HandlerEndHook);
    out->createHook = reinterpret_cast<void*>(&CreateExecHook);
    out->genericHook = reinterpret_cast<void*>(&GenericExecHook);
    out->maxTickRate = reinterpret_cast<void*>(g.origMaxTick);
    out->flowReturn[0] = g.flowReturn[0];
    out->flowReturn[1] = g.flowReturn[1];
    out->chooseWriter = reinterpret_cast<void*>(g.chooseWriter);
    out->numMaxTickSlots = g.numMaxTickSlots;
    out->hairActorWeak = g.hairActorWeak;
    return true;
}

void NpcPacingTestSetup(const NpcPacingTestFakes& f)
{
    g.testAnyCaller = true;
    g.testClock = f.clock;
    g.testPlayer = f.player;
    g.testGuards = f.guards;
    if (f.run) g.origRun = reinterpret_cast<RunSessionFn>(f.run);
    if (f.end) g.origEnd = reinterpret_cast<HandlerEndFn>(f.end);
    if (f.findSession) g.findSession = reinterpret_cast<FindSessionFn>(f.findSession);
    if (f.create) g.origCreate = reinterpret_cast<CmdExecFn>(f.create);
    if (f.generic) g.origGeneric = reinterpret_cast<CmdExecFn>(f.generic);
    for (int i = 0; i < 3; ++i) {
        if (f.execIfSafe[i]) g.del[i].execIfSafe = reinterpret_cast<ExecIfSafeFn>(f.execIfSafe[i]);
        if (f.dtor[i]) g.del[i].dtor = reinterpret_cast<DtorFn>(f.dtor[i]);
        if (f.sizeOverride[i]) { g.del[i].size = f.sizeOverride[i]; g.del[i].copyCtor = nullptr; }
    }
    if (f.weakGet) g.weakGet = reinterpret_cast<WeakGetFn>(f.weakGet);
    if (f.chooseWriter) g.chooseWriter = reinterpret_cast<ChooseWriterFn>(f.chooseWriter);
    if (f.rootOffset) { g.rootOffset = f.rootOffset; g.locationOffset = f.locationOffset; }
    if (f.hairActorWeak) g.hairActorWeak = f.hairActorWeak;
    g.actorWorld = nullptr;  // fakes are never rendered
    g.outerOk = true;
    g.installed = true;
    if (!g.fifo) g.fifo = static_cast<Entry*>(VirtualAlloc(nullptr, sizeof(Entry) * kFifoCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

bool NpcPacingFadeInfo(uint32_t offsets[5], void** staticClass)
{
    if (!g.fadeFound) return false;
    offsets[0] = g.fadeState;
    offsets[1] = g.fadeValue;
    offsets[2] = g.fadeOwner;
    offsets[3] = g.fadeComp;
    offsets[4] = g.fadeFloor;
    *staticClass = reinterpret_cast<void*>(g.fadeClass);
    return true;
}

void NpcPacingTestFade(const uint8_t* (*staticClass)()) { g.fadeClass = staticClass; }

void NpcPacingTestRendered(bool (*rendered)(const void* actor)) { g.testRendered = rendered; }
void NpcPacingTestFadeValue(float (*fade)(const void* actor)) { g.testFade = fade; }
void NpcPacingTestGameValue(float (*value)(const void* actor)) { g.testGameValue = value; }
float NpcPacingTestGatePeriodS() { return GatePeriodS(); }

uint64_t NpcPacingFrame() { return g.frame; }  // RigTraceReuse's stats line
// GcPassAfterBusy (memory/gc_pass_after_busy.cpp), game thread: the frame's own time so far (since the frame-rate cap's
// sleep point) and the time of the character steps run in this frame so far; false until the first frame edge
bool NpcPacingFrameState(uint64_t* elapsedUs, uint64_t* unitUs)
{
    if (!g.installed || !g.lastPaceEnd) return false;
    const uint64_t now = NowUs();
    *elapsedUs = now > g.lastPaceEnd ? now - g.lastPaceEnd : 0;
    *unitUs = g.unitUs;
    return true;
}

NpcPacingTestState NpcPacingTestRead()
{
    NpcPacingTestState t{};
    t.S = g.S;
    t.E = g.E;
    t.frame = g.frame;
    t.fifo = g.count;
    t.units = g.units;
    t.inFrame = g.inFrame;
    t.heldFrames = g.heldFrames;
    return t;
}


// ---- CapSleepSkip ----
// Start-up only (a 6-byte code patch of the engine's GetMaxTickRate call inside UpdateTimeAndHandleMaxTickRate); needs
// NpcCreationPacing's cap hook (MaxTickRateHook decides) and GcPassAfterBusy's readings (its predictor).
bool InstallCapSleepSkip(bool beforeGameCode, int key)
{
    if (!key) { Log("[capskip] CapSleepSkip off in OblivionPerfFix.ini (takes a restart to switch on)"); return false; }
    if (!beforeGameCode) { Log("[capskip] CapSleepSkip: only installed when the game starts (version.dll route), not installed"); return false; }
    if (!g.installed || !g.origMaxTick || !g_capCallSite) {
        Log("[capskip] CapSleepSkip needs NpcCreationPacing's frame-rate cap hook: not installed");
        return false;
    }
    if (!GcPassAfterBusyDeltaTime()) {
        Log("[capskip] CapSleepSkip needs GcPassAfterBusy's readings (the GC timer, FApp::DeltaTime): not installed");
        return false;
    }
    uint8_t* const site = g_capCallSite;
    if (site[0] != 0xFF || site[1] != 0x90 || Disp32(site + 2) != g.getMaxTickRateSlot) {
        Log("[capskip] CapSleepSkip: the engine's GetMaxTickRate call does not match: not installed");
        return false;
    }
    uint8_t* const mem = AllocNearGame(16);
    if (!mem) { Log("[capskip] CapSleepSkip: no memory near the game's code: not installed"); return false; }
    void* const hook = reinterpret_cast<void*>(&CapRateHook);
    memcpy(mem, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(mem, 16, PAGE_READONLY, &old);
    const int64_t d = mem - (site + 6);
    if (d != static_cast<int32_t>(d)) { Log("[capskip] CapSleepSkip: slot out of reach: not installed"); return false; }
    uint8_t insn[6] = {0xFF, 0x15};
    const int32_t rel = static_cast<int32_t>(d);
    memcpy(insn + 2, &rel, 4);
    if (!WriteCode(site, std::span<const uint8_t>(insn, 6))) { Log("[capskip] CapSleepSkip: could not patch the call: not installed"); return false; }
    g_capSkipInstalled = true;
    const GameModule& game = Game();
    Log("[capskip] CapSleepSkip installed: GetMaxTickRate call exe+0x%X (vtable +0x%X) goes through our hook; live",
        static_cast<unsigned>(site - game.base), g.getMaxTickRateSlot);
    return true;
}

// Monitor thread, every second (live once installed): bits 1 GC pass, 2 cell removal; the most the wait is shortened (ms).
void SetCapSleepSkip(int bits, int maxMs, int addBack)
{
    if (!g_capSkipInstalled) return;
    const int b = bits < 0 ? 0 : bits & 3;
    const int us = (maxMs < 1 ? 1 : maxMs > 16 ? 16 : maxMs) * 1000;
    (void)addBack;
    const int ab = 1;  // public: the cut always goes back into the delta
    if (b != g_capSkipBits.load() || us != g_capSkipMaxUs.load() || ab != g_capSkipAddBack.load()) {
        Log("[capskip] CapSleepSkip %d -> %d, at most %d ms%s", g_capSkipBits.load(), b, us / 1000,
            ab ? "" : " (lab CapSleepSkipAddBack=0: the cut is not given back to the frame's delta)");
        g_capSkipBits = b;
        g_capSkipMaxUs = us;
        g_capSkipAddBack = ab;
    }
}

void LogCapSleepSkipStats()
{
    if (!g_capSkipInstalled) return;
    const uint64_t gc = cs.skips[0].exchange(0), rem = cs.skips[1].exchange(0);
    if (!gc && !rem) return;
    const uint64_t cut = cs.cutUs.exchange(0), lg = cs.long20[0].exchange(0), sg = cs.short20[0].exchange(0), lr = cs.long20[1].exchange(0),
                   sr = cs.short20[1].exchange(0), mx = cs.maxAfterUs.exchange(0), calls = cs.calls.exchange(0), added = cs.addedUs.exchange(0),
                   noDelta = cs.noDelta.exchange(0), nw = cs.noWait.exchange(0), cb = cs.caseB.exchange(0), cbUs = cs.caseBUs.exchange(0),
                   su = cs.syncUnknown.exchange(0), ng = cs.notGiven.exchange(0);
    using U = unsigned long long;
    Log("[capskip] stats: the cap's wait shortened before %llu frames (GC pass due %llu: then >= 20 ms %llu, less %llu; cell removal %llu: "
        ">= 20 ms %llu, less %llu; longest %.1f ms), %.1f ms in all, %llu reached the engine's wait; %.1f ms given back to the frames' "
        "delta time (the game clock's lead on the real clock grew by that)%s%s; no wait after the cut frame %llu, of which it waited "
        "for the render thread %llu (%.1f ms: the next frame's step that much longer than the game's), not known %llu",
        static_cast<U>(gc + rem), static_cast<U>(gc), static_cast<U>(lg), static_cast<U>(sg), static_cast<U>(rem), static_cast<U>(lr),
        static_cast<U>(sr), mx / 1000.0, cut / 1000.0, static_cast<U>(calls), added / 1000.0, noDelta ? ", SOME NOT" : "",
        ng ? " (lab CapSleepSkipAddBack=0: not given back)" : "", static_cast<U>(nw), static_cast<U>(cb), cbUs / 1000.0, static_cast<U>(su));
}

// Self-tests. Site: the GetMaxTickRate call CapSleepSkip patches is `call [rax+slot]` with NpcCreationPacing's slot.
bool CapSleepSkipTestSite(const GameModule& game)
{
    const auto paces = FindAll(game.text, ParsePattern(kMaxTickRate));
    uint8_t* const calls = paces.size() == 1 ? FindNear(paces[0], 0x400, kMaxTickCalls) : nullptr;
    if (!calls) return false;
    const uint8_t* const site = calls + kCapCallAt;
    const uint32_t slot = Disp32(calls + kMaxTickRateSlot);
    return site[0] == 0xFF && site[1] == 0x90 && Disp32(site + 2) == slot && slot > 0 && slot % 8 == 0;
}

namespace {
float g_fakeRate = 60.0f;
float FakeGetMaxTickRate(void*, float, bool) { return g_fakeRate; }
int g_fakeUnits = 0;
uint64_t g_fakeUnitsArg = 0;
int FakeUnits(uint64_t us) { g_fakeUnitsArg = us; return g_fakeUnits; }
bool FakeMenu(int32_t* n, int32_t* m) { *n = 0; *m = 0; return true; }
}  // namespace

// The rate hook and the decision on fakes. `which`: 0 no cut: the rate passes through; 1 a cut of 8 ms on 60 fps: the rate of a
// 8.67 ms period, once (the next call passes through); 2 a cut longer than the period: 0 (no wait); 3 decision: a GC pass due
// after 8 ms of work at 60 fps: cut 8 ms (the bound) and counted; 4 the next frame (within kCapSkipEvery): no cut; 5 nothing
// due: no cut; 6 a frame that already took 16 ms: no cut. 1 = as expected.
int CapSleepSkipTest(int which)
{
    static void* fakeVt[0x400 / 8] = {};
    void* engine[1] = {fakeVt};
    const uint32_t savedSlot = g.getMaxTickRateSlot;
    if (!g.getMaxTickRateSlot) g.getMaxTickRateSlot = 0x310;
    fakeVt[g.getMaxTickRateSlot / 8] = reinterpret_cast<void*>(&FakeGetMaxTickRate);
    g_fakeRate = 60.0f;
    bool ok = false;
    if (which == 7) {  // lab CapSleepSkipAddBack=0: the cut wait is not given back, counted
        const int savedAb = g_capSkipAddBack.load();
        g_capSkipAddBack = 0;
        g_capSkipCutS = 0.008;
        g_capDeltaAddS = 0;
        const float r = CapRateHook(engine, 0.008f, true);
        const uint64_t ng0 = cs.notGiven.load();
        double dt = 0.0087;
        const double back = CapSkipRestoreDelta(&dt);
        ok = r < 120.0f && r > 110.0f && back == 0 && dt == 0.0087 && g_capDeltaAddS == 0 && cs.notGiven.load() == ng0 + 1;
        g_capSkipAddBack = savedAb;
    } else if (which <= 2) {
        g_capSkipCutS = which == 0 ? 0 : which == 1 ? 0.008 : 0.02;
        g_capDeltaAddS = 0;
        const float r = CapRateHook(engine, 0.008f, true);  // 8 ms of the frame gone: the game would wait 8.67 ms
        const double add = g_capDeltaAddS;
        const float r2 = CapRateHook(engine, 0.008f, true);
        // the delta given back after the engine's call: the cut part of the wait (8 ms; the whole 8.67 ms wait when cut to 0)
        double dt = 0.0087;  // what the engine wrote for the shortened frame (DeltaRealTime + its shorter wait)
        const double back = CapSkipRestoreDelta(&dt);
        if (which == 0) ok = r == 60.0f && add == 0 && back == 0 && dt == 0.0087;
        else if (which == 1)
            ok = std::fabs(r - static_cast<float>(1.0 / (1.0 / 60.0 - 0.008))) < 0.01f && r2 == 60.0f && std::fabs(add - 0.008) < 2e-5 &&
                 std::fabs(dt - (0.0087 + add)) < 1e-12 && g_capDeltaAddS == 0;
        else ok = r == 0.0f && r2 == 60.0f && std::fabs(add - (1.0 / 60.0 - static_cast<double>(0.008f))) < 2e-6 && std::fabs(back - add) < 1e-12;
    } else {
        const bool savedInst = g_capSkipInstalled;
        const int savedBits = g_capSkipBits.load(), savedMax = g_capSkipMaxUs.load();
        const double savedCap = g.capPeriodUs;
        const uint64_t savedEnd = g.lastPaceEnd, savedFrame = g.frame, savedLast = g_capSkipLastFrame;
        const auto savedUnits = g_capUnitsFn;
        const auto savedMenu = g_capMenuFn;
        uint8_t** const savedTes = g.tes;
        static uint8_t tes[0x200] = {};
        static uint8_t* tesPtr = tes;
        g.tes = &tesPtr;
        g_capSkipInstalled = true;
        g_capSkipBits = 3;
        g_capSkipMaxUs = 8000;
        g.capPeriodUs = 16667;
        g_capUnitsFn = &FakeUnits;
        g_capMenuFn = &FakeMenu;
        g_fakeUnits = which == 5 ? 0 : 1;
        g.frame = 1000;
        g_capSkipLastFrame = which == 4 ? 998 : 0;
        g.lastPaceEnd = 50000;
        g_capSkipCutS = 0;
        g_capSkipCheck = false;
        const uint64_t gc0 = cs.skips[0].load();
        CapSkipDecide(50000 + (which == 6 ? 16000 : 8000));
        // 3: 8 ms of work; the predictor is asked with the cap period less 0.2 ms (the coming frame's delta with the add-back)
        if (which == 3)
            ok = std::fabs(g_capSkipCutS - 0.008) < 1e-9 && cs.skips[0].load() == gc0 + 1 && g_capSkipLastFrame == 1000 &&
                 g_fakeUnitsArg == 16467;
        else ok = g_capSkipCutS == 0 && cs.skips[0].load() == gc0;
        g_capSkipCutS = 0;
        g_capSkipCheck = false;
        g.tes = savedTes;
        g_capUnitsFn = savedUnits;
        g_capMenuFn = savedMenu;
        g.capPeriodUs = savedCap;
        g.lastPaceEnd = savedEnd;
        g.frame = savedFrame;
        g_capSkipLastFrame = savedLast;
        g_capSkipBits = savedBits;
        g_capSkipMaxUs = savedMax;
        g_capSkipInstalled = savedInst;
    }
    g.getMaxTickRateSlot = savedSlot;
    return ok ? 1 : 0;
}

}  // namespace opf
