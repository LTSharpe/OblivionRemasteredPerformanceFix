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
    uintptr_t key;    // its actor (0 = unknown)
    bool distant;
    bool fadeHeld;    // PacingFadeAware: waits because its actor is still invisible in its spawn fade-in
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
    std::atomic<uint64_t> guard[5]{};  // frames not deferring: session, travel, menu/loading, ring, player
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
    std::atomic<bool> leanFlowDrain{true};  // PacingLeanFlowDrain: a level flow at its wait bound drains the main queue only

    // game-thread state
    DWORD gtId = 0;
    uint64_t frame = 0;      // GT frames seen (RunSession(-1) from OnWorldPreActorTick)
    uint64_t S = 0, E = 0;   // RunSession(-1) calls, end commands executed
    bool allowDefer = false; // this frame: enabled, guards passed
    bool inMainRun = false;  // inside our RunSession(-1)
    bool inFrame = false;    // between our pre-tick run and the post-tick end
    bool deferredNow = false;
    bool lastCreateFar = false;  // a distant create command ran earlier in this OT frame
    int depth = 0;           // inside a unit
    bool unitDistant = false;  // the running unit's actor may wait (what a delegate completing inside it inherits)
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
    uint64_t budgetThisFrame = 4000;
    uint64_t frameStartUs = 0;
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
    return v;
}

bool FadeHidden(const uint8_t* actor)
{
    return FadeLevel(actor) * 10.0f >= static_cast<float>(g.fadeMinTenths.load(std::memory_order_relaxed));
}


bool FarFrom(double x, double y, double z)
{
    if (!Player()) return false;
    const double dx = x - g.px, dy = y - g.py, dz = z - g.pz;
    const double d = g.minDistance.load(std::memory_order_relaxed);
    return dx * dx + dy * dy + dz * dz > d * d;
}

// The delegate's owner (weak pointer at +0x18) -> the actor it belongs to (itself or an outer); distant = farther than
// the minimum distance. Unknown counts as near (runs as before).
bool ResolveDelegate(const uint8_t* inst, uintptr_t* key, bool* distant)
{
    *key = 0;
    *distant = false;
    g.lastFadeHeld = false;
    if (!g.testPlayer && (!g.outerOk || !g.weakGet)) return false;
    __try {
        const uint8_t* obj = static_cast<const uint8_t*>(g.weakGet(inst + kOwnerWeakOffset));
        for (int i = 0; obj && i < 4; ++i) {
            double x, y, z;
            if (ActorLocation(obj, &x, &y, &z)) {
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
                if (g.delegateLogs < 16) {
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

bool ResolveDelegate(const uint8_t* inst, uintptr_t* key, bool* distant);
// PacingFadeAware: whether a waiting entry may still wait (its actor far and unseen, or still fade-hidden); an actor that
// cannot be resolved any more runs (its ExecuteIfSafe re-checks the weak pointer).
bool StillMayWait(const Entry& e)
{
    uintptr_t key = 0;
    bool distant = false;
    const bool resolved = ResolveDelegate(e.inst, &key, &distant);
    if (!resolved) return false;
    if (!distant && e.fadeHeld) g.fadeReleased.fetch_add(1, std::memory_order_relaxed);
    return distant;
}


// Runs a unit (depth and the distance a nested completion inherits) and returns its time in us.
template <class F>
uint64_t TimeUnit(int type, bool distant, F&& body)
{
    const bool outer = g.unitDistant;
    g.unitDistant = distant;
    ++g.depth;
    const uint64_t t0 = NowUs();
    body();
    const uint64_t us = NowUs() - t0;
    (void)type;
    --g.depth;
    g.unitDistant = outer;
    return us;
}

void RunEntry()
{
    Entry& e = Front();
    const DelegateType& d = g.del[e.type - kAppearance];
    const uint64_t us = TimeUnit(e.type, e.distant, [&] {
        d.execIfSafe(e.inst);
        d.dtor(e.inst, 0);
    });
    LAB_UNIT(e.type, us, reinterpret_cast<const uint8_t*>(e.key), nullptr, g.frame - e.since);
    g.head = (g.head + 1) % kFifoCap;
    --g.count;
    Account(e.type, us);
}

void ReplayAll()
{
    if (g.count) {
        s.drains.fetch_add(1, std::memory_order_relaxed);
        g.frameFlags |= 1;
    }
    while (g.count) RunEntry();
}

void ReplayBudget()
{
    const uint64_t maxLag = static_cast<uint64_t>(g.maxLag.load(std::memory_order_relaxed));
    while (g.count) {
        const Entry& e = Front();
        const uint64_t age = g.frame - e.since;
        const bool forced = age > 2 * maxLag;
        if (!forced && e.distant && OverBudget(e.type, age)) {
            // PacingFadeAware: the head is re-checked; it runs now once its actor is seen (or near) and no longer
            // fade-hidden, i.e. once it would no longer have been allowed to wait.
            bool recheck = g.fadeAware.load(std::memory_order_relaxed) != 0;
            const bool release = recheck && !StillMayWait(e);
            if (!release) break;
        }
        if (forced) {
            s.forced.fetch_add(1, std::memory_order_relaxed);
            g.frameFlags |= 2;
        }
        RunEntry();
    }
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
    e.key = key;
    e.distant = distant;
    e.fadeHeld = fadeHeld;
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
        if (g.count && FrameOpen() && Enqueue(type, p, 0, g.unitDistant)) { *result = true; return true; }
        return false;
    }
    bool distant = false;
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
        ResolveDelegate(p, &key, &distant);
        unitKey = key;
        // With a backlog, a distant unit (or one of an actor that has queued units) queues behind it; a near unit of
        // another actor runs now. Without one, a distant unit waits only when the frame's budget is spent.
        const bool wait = g.count ? (distant || FifoHasKey(key)) : (distant && OverBudget(type));
        const bool fadeHeld = g.lastFadeHeld;
        if (wait) {
            if (Enqueue(type, p, key, distant, fadeHeld)) {
                *result = true;
                return true;
            }
            ReplayAll();  // FIFO full: keep the order
        }
    }
    if (OverBudget(type)) g.frameFlags |= 4;
    const uint64_t us = TimeUnit(type, distant, [&] { *result = d.execIfSafe(inst); });
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
    if (g.allowDefer && distant && !PairForced() && OverBudget(kCreate, PairLag()))
        return Defer(static_cast<uint8_t*>(g.session), kCreate);
    if (PairForced() && distant) s.forced.fetch_add(1, std::memory_order_relaxed);
    g.lastCreateFar = distant;
    if (OverBudget(kCreate)) g.frameFlags |= distant ? 2 : 4;
    uint64_t r = 0;
    const uint64_t us = TimeUnit(kCreate, distant, [&] { r = g.origCreate(cmd); });
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
    const uint64_t us = TimeUnit(kGeneric, heavy && g.lastCreateFar, [&] { r = g.origGeneric(cmd); });
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
void MaxTickRateHook(void* engine)
{
    LedgerSleepBegin();  // HitchLedger: the cap's sleep starts (no-op unless recording, game thread only)
    const uint64_t t0 = NowUs();
    g.origMaxTick(engine);
    const uint64_t t1 = NowUs();
    if (!OnGameThread()) return;
    HdrMenuFrameEnd();   // HdrMenuRows / HudAspectRow / DlssRayReconstruction (no-op unless installed)
    HdrGammaFrameEnd();  // HdrGamma22 (no-op unless installed; writes only while on)
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
    }
    const uint64_t sleep = t1 - t0, period = g.lastPaceEnd ? t1 - g.lastPaceEnd : 0;
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
    g.frameStartUs = NowUs();
    // The older work first; but while a level flow waits (c), the main queue first: the flow and the OT wait for it.
    if (!g.allowDefer) ReplayAll();
    else if (g.count && g.heldFrames == 0 && FifoAge() >= PairLag()) ReplayBudget();
    g.inMainRun = true;
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
    if (g.deferredNow || g.E < g.S) s.deferFrames.fetch_add(1, std::memory_order_relaxed);
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
    // past kMaxFlowWait only while the main queue is getting through the flow's own OT frame, a unit per frame
    const bool progressing = g.frameTypes[kCreate] + g.frameTypes[kGeneric] > 0 && PairLag() == 0;
    const int bound = progressing ? std::max(kMaxFlowWait, g.flowWaitFrames.load(std::memory_order_relaxed)) : kMaxFlowWait;
    if (g.heldFrames >= bound) {  // waited long enough: the queue drains as before
        if (!g.holdEnd) s.flowDrains.fetch_add(1, std::memory_order_relaxed);
        return false;
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
    if ((ret == g.postTickReturn || g.testAnyCaller) && OnGameThread() && g.depth == 0 && !g.inMainRun && g.inFrame) {
        if (!g.allowDefer || !g.enabled.load(std::memory_order_relaxed)) ReplayAll();
        else ReplayBudget();
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
}

bool NpcPacingPlayerPos(double* x, double* y, double* z)
{
    if (!g.installed || !OnGameThread() || !Player()) return false;
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

uint64_t NpcPacingFrame() { return g.frame; }  // RigTraceReuse's stats line

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


}  // namespace opf
