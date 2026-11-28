// WaterRebuildSkip (worker waterskip, 2026-11-25; S-039 player-menu close; findings "waterprobe", "waterskip").
//
// What it removes: every player-menu open and close (inventory, pause, magic, map) sets one of the Water plugin's three
// visibility console variables (r.Water.Enabled / r.Water.WaterMesh.Enabled / r.Water.WaterMesh.EnableRendering), whose
// change callback UWaterSubsystem::NotifyWaterVisibilityChangedInternal (exe+0x42DBC70, UE 5.3 WaterSubsystem.cpp) calls
// UWaterBodyComponent::UpdateComponentVisibility(true) (exe+0x42EBC80) for every water body (57 in Tamriel). UE 5.3's
// UpdateComponentVisibility marks the body's water zone for a mesh rebuild "in case its visibility has changed" without
// comparing anything, so the first UWaterSubsystem::Tick after the close runs UWaterMeshComponent::RebuildWaterMesh
// (exe+0x42E1BD0, the quadtree build) for all 8 Tamriel zones: 9.2 ms on the game thread (8.6-11.4) at each close, plus a
// new scene proxy per zone (MarkRenderStateDirty at its end). Lab probe (findings "waterprobe"): the rebuild's inputs were
// unchanged in 96 of 96 such rebuilds and the tree it made was identical (an uninitialised bit and padding aside).
//
// Epic fixed this in UE 5.5 (CL 34234412, "Fixed UWaterBodyComponent::UpdateComponentVisibility updating the water info
// texture whenever it was called"; 5.5.4, 5.6.1, 5.7.4 and ue5-main have it): the body keeps a transient
// bIsRenderedByWaterMeshAndVisible and marks its zone only when that changes. This key applies the same idea in a narrower,
// stricter form, on the one path that S-039 measures: only the marks that the visibility-callback loop itself makes are
// considered, and such a mark is taken back only when the zone's mesh inputs are exactly those of its last real rebuild.
//
// Why the callback's mark carries no information (UE 5.3 source): RebuildWaterMesh reads, per body, ShouldRender()
// (USceneComponent: the component's and its owner's visibility flags, SceneComponent.cpp) and ShouldGenerateWaterMeshTile()
// (bAlwaysGenerateWaterMeshTiles, the body type, WaterMeshOverride, WaterMaterial); neither reads the three console
// variables. Those variables only (a) switch the body's own standard renderable components, which UpdateComponentVisibility
// still does as in the game, and (b) gate UWaterMeshComponent::Update (bIsEnabled) and CreateSceneProxy. So the mark only
// makes the next tick pick up whatever input changed since the zone's last rebuild without a mark of its own.
//
// The rule (all must hold, per zone, else the game's mark stays):
//   1. the zone's mesh flag bNeedsRebuild (mesh +0x6E1) was clear before the loop and set after it (only this loop set it;
//      a mark from any other writer, before or during the loop, is never touched);
//   2. the mesh still has its scene proxy (UPrimitiveComponent::SceneProxy +0x578 != null). The menu itself switches
//      r.Water.WaterMesh.Enabled off at the open and on at the close (lab run c2), and the water tick runs in the menu, so
//      at the close the mesh's bIsEnabled is 0. In UE 5.3 (and 5.4-5.7, ue5-main) that flip touches no render state:
//      Update only sets bIsEnabled and returns, the proxy stays and skips drawing while the variable is 0 (it reads it on
//      the render thread every frame). A proxy is lost only if the render state is recreated while disabled
//      (CreateSceneProxy then returns none): the pointer is null and the game's rebuild (with its MarkRenderStateDirty)
//      must bring it back, so the mark stays. A proxy that exists was made while enabled from the tree of the last
//      rebuild (rule 3 checks the tree is that one; every rebuild ends with MarkRenderStateDirty);
//   3. the inputs the fix checks equal those recorded at the zone's last real rebuild: the mesh's transform, TileSize,
//      ExtentInTiles and the three cached LOD values, the zone's body list (order and identity), and per body its
//      transform, Bounds, visibility bytes, bAlwaysGenerateWaterMeshTiles, WaterMeshOverride, WaterMaterial, WaterMID,
//      body type and (rivers) its two transition MIDs, its owner, the owner's hidden bit, the owner's spline component and
//      its Bounds; and the mesh's quadtree storage (WaterQuadTree +0x5F8, its arrays' pointers and counts) is the one that
//      rebuild left. Not checked (review revwaterskip S2-1): spline points and collision shapes beyond their bounds,
//      MaxWaveHeight, OverlapMaterialPriority, the ocean height / flood, the far-mesh settings. In the game's normal play
//      these change only together with a rebuild request of their own (MarkForRebuild, Add/RemoveWaterBodyComponent,
//      UpdateComponentVisibility from OnVisibilityChanged / OnHiddenInGameChanged / SetActorHiddenInGame), which rule 1
//      leaves alone; in 5.3 shipping a change without such a request (UpdateAll does nothing for non-dynamic bodies) would
//      be picked up by the game only at the next mark (in practice the next menu), with the key at the next real rebuild.
//      So the key is equivalent, not identical.
// Then the flag is put back to clear. The rebuild's other effects are the same without it: the tree and the zone's
// UsedMaterials stay as they are (the same inputs give the same tree, the lab proof below); its SetDynamicParametersOnMID
// calls set the water materials' parameters from the current state, which the water-info update that the same loop
// still marks (unchanged, as the game) sets again later in that frame (WaterViewExtension: UpdateMaterialInstances for every
// body of a zone it renders); PrecachePSOs repeats a request already made. MarkRenderStateDirty would recreate the
// scene proxy (re-adding the primitive to the scene); the key keeps the old proxy rather than recreating it, built from the
// same tree, LOD and tessellation settings and materials: the same water, without the one-frame re-add (S3-4).
//
// Ini [Fixes] WaterRebuildSkip: 0 = off (not installed), 1 = on; lab 2 = verify (the game's mark stays, and the rebuild it
// causes is compared with the zone's previous one: the masked tree hash; differences counted). Installed at start-up, then
// live; a change of the value forgets every zone's recorded inputs.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {

// UWaterSubsystem::NotifyWaterVisibilityChangedInternal (build 19115871: exe+0x42DBC70): the whole function: for each
// UWaterBodyComponent* of the manager's array (this +0x40, num +0x48): vcall +0xA90 (UpdateComponentVisibility) with dl = 1.
constexpr char kNotify[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 59 40 48 63 41 48 48 8D 3C C3 48 3B DF 74 21 0F 1F 44 00 00 48 8B 0B 48 85 C9 74 0B "
    "48 8B 01 B2 01 FF 90 90 0A 00 00 48 83 C3 08 48 3B DF 75 E4 48 8B 5C 24 30 48 83 C4 20 5F C3";
constexpr uint8_t kNotifyPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};  // mov [rsp+8],rbx

// UWaterBodyComponent::UpdateComponentVisibility (exe+0x42EBC80), entry to the two flag stores; not hooked, the layout pins:
// ShouldGenerateWaterMeshTile inlined (bAlwaysGenerate +0xDB8, GetWaterBodyType vcall +0xA28 != Transition, WaterMeshOverride
// +0xDB0, WaterMaterial +0xCE0), bVisible (+0x1A0 bit 0x20), bHiddenInGame (+0x1A1 bit 8), GetWaterZone(+0xE10), then
// zone->WaterMesh (+0x2B8)->bNeedsRebuild (+0x6E1) = 1 and zone->bNeedsWaterInfoRebuild (+0x300) = 1.
constexpr char kUcv[] =
    "40 57 41 54 48 83 EC 48 48 83 B9 B0 00 00 00 00 44 0F B6 E2 48 8B F9 75 0E E8 ?? ?? ?? ?? 48 85 C0 0F 84 69 01 00 00 80 "
    "BF B8 0D 00 00 00 48 8B 05 ?? ?? ?? ?? 48 89 5C 24 68 48 89 74 24 40 4C 89 7C 24 30 8B 18 75 28 48 8B 07 48 8B CF FF 90 "
    "28 0A 00 00 3C 03 74 14 48 83 BF B0 0D 00 00 00 75 0A 48 83 BF E0 0C 00 00 00 75 04 32 C0 EB 02 B0 01 48 89 6C 24 70 4C "
    "89 74 24 38 85 DB 74 12 84 C0 75 0E F6 87 A0 01 00 00 20 74 05 40 B5 01 EB 0B 40 32 ED 85 DB 74 12 84 C0 75 0E F6 87 A1 "
    "01 00 00 08 75 05 45 32 F6 EB 03 41 B6 01 48 8B 07 48 8D 54 24 20 48 8B CF FF 90 18 0A 00 00 48 8B 74 24 20 48 63 44 24 "
    "28 4C 8D 3C C6 49 3B F7 74 2F 48 8B 1E 41 B0 01 48 8B CB 40 0F B6 D5 E8 ?? ?? ?? ?? 41 B0 01 41 0F B6 D6 48 8B CB E8 ?? "
    "?? ?? ?? 48 83 C6 08 49 3B F7 75 D6 48 8B 74 24 20 4C 8B 7C 24 30 4C 8B 74 24 38 48 8B 6C 24 70 48 85 F6 74 08 48 8B CE "
    "E8 ?? ?? ?? ?? 45 84 E4 74 64 48 8D 8F 10 0E 00 00 E8 ?? ?? ?? ?? 48 8B F0 48 85 C0 74 50 48 8B 17 48 8B CF 33 DB FF 92 "
    "E8 09 00 00 48 8B 17 B9 04 00 00 00 84 C0 0F 45 D9 48 8B CF FF 92 F0 09 00 00 84 C0 74 03 83 CB 02 8B C3 C1 E8 02 84 C0 "
    "74 0E 48 8B 86 B8 02 00 00 C6 80 E1 06 00 00 01 D1 EB F6 C3 01 74 07 C6 86 00 03 00 00 01";

// UWaterMeshComponent::RebuildWaterMesh(float TileSize, const FIntPoint& Extent) (exe+0x42E1BD0): entry through the grid
// snap of GetComponentLocation (ComponentToWorld translation +0x280, +0x290) to WaterQuadTree.InitTree (this +0x5F8).
constexpr char kRebuild[] =
    "48 8B C4 48 89 58 20 55 56 57 41 56 41 57 48 8D 68 A1 48 81 EC 00 01 00 00 0F 10 A1 80 02 00 00 0F 57 D2 0F 29 70 C8 0F "
    "57 DB 0F 10 B1 90 02 00 00 48 8B D9 0F 29 78 B8 F2 0F 10 3D ?? ?? ?? ?? 44 0F 29 40 A8 44 0F 28 C1 F3 41 0F 5A D0 44 0F "
    "29 48 98 66 0F 2E D3 44 0F 29 50 88 0F 11 64 24 20 F2 0F 11 74 24 30 75 08 F2 0F 10 6C 24 28 EB 1B 0F 28 C2 F2 0F 59 C7 "
    "F2 0F 58 44 24 28 F2 0F 5E C2 66 0F 3A 09 E8 01 F2 0F 59 EA 66 0F 2E D3 0F 11 64 24 20 F2 0F 11 74 24 30 75 08 F2 0F 10 "
    "64 24 20 EB 1B 0F 28 C2 F2 0F 59 C7 F2 0F 58 44 24 20 F2 0F 5E C2 66 0F 3A 09 E0 01 F2 0F 59 E2 66 41 0F 6E 40 04 48 8D "
    "54 24 38 66 41 0F 6E 08 48 81 C1 F8 05 00 00";
// mov rax,rsp + mov [rax+0x20],rbx: the 7 bytes moved into the trampoline (no rip-relative operand).
constexpr uint8_t kRebuildPrologue[7] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20};

// UWaterMeshComponent::Update inlined in UWaterSubsystem::Tick (exe+0x42E814B): bNeedsRebuild (+0x6E1) test, bIsEnabled
// (+0x6E2) = 1, the three cached LOD values (+0x6E4/+0x6E8/+0x6EC), ExtentInTiles (+0x5F0/+0x5F4), TileSize (+0x5EC), the
// RebuildWaterMesh call (checked to reach kRebuild), PrecachePSOs (vcall +0x640), bNeedsRebuild = 0, bIsEnabled = 0.
constexpr char kTickPin[] =
    "80 BB E1 06 00 00 00 C6 83 E2 06 00 00 01 48 8B 05 ?? ?? ?? ?? 8B 08 48 8B 05 ?? ?? ?? ?? 8B 10 48 8B 05 ?? ?? ?? ?? F3 0F "
    "10 00 75 35 48 8B 05 ?? ?? ?? ?? 83 38 00 75 29 48 8B 05 ?? ?? ?? ?? 83 38 00 75 1D 3B 8B E4 06 00 00 75 15 3B 93 E8 06 00 "
    "00 75 0D 0F 2E 83 EC 06 00 00 0F 84 AA 00 00 00 66 0F 6E C9 F3 0F 11 83 EC 06 00 00 0F 28 C7 0F 5B C9 89 8B E4 06 00 00 89 "
    "93 E8 06 00 00 E8 ?? ?? ?? ?? 66 0F 6E 8B F4 05 00 00 4C 8D 45 8F 0F 28 E0 0F 28 D8 0F 5B C9 0F 28 D6 F3 0F 58 DC F3 0F 59 "
    "D9 0F 28 CE F3 0F 5C D3 F3 0F 2D CA 0F 28 D0 66 0F 6E 83 F0 05 00 00 0F 5B C0 D1 F9 F7 D9 89 4D 93 48 8B CB F3 0F 58 D4 F3 "
    "0F 59 D0 F3 0F 5C CA F3 0F 2D C1 F3 0F 10 8B EC 05 00 00 D1 F8 F3 0F 5E CC F7 D8 89 45 8F E8 ?? ?? ?? ?? 48 8B 03 48 8B CB "
    "FF 90 40 06 00 00 C6 83 E1 06 00 00 00 EB 07 C6 83 E2 06 00 00 00";
constexpr size_t kTickPinRebuildCall = 0xEB;  // offset of the RebuildWaterMesh call in kTickPin (exe+0x42E8236)

// AWaterZone::AddWaterBodyComponent (exe+0x42BC5F0): the zone's TArray<TWeakObjectPtr<UWaterBodyComponent>> at +0x2A0.
constexpr char kAddPin[] =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 4C 8B F1 48 8D B1 A0 02 00 00 33 FF 48 8D 4C 24 50";
// FWeakObjectPtr::Get (exe+0x1126290), called only.
constexpr char kWeakGet[] =
    "44 8B 49 04 45 85 C9 74 44 8B 01 85 C0 78 3E 3B 05 ?? ?? ?? ?? 7D 36 8B C8 0F B7 C0 48 C1 E9 10 48 8D 14 40 48 8B 05 ?? ?? "
    "?? ?? 48 8B 0C C8 4C 8D 04 D1";

// (review revwaterskip S2-1) more of what the rebuild's per-body lambda reads, pinned:
// the lambda (exe+0x42B4D87, through the TFunctionRef thunk exe+0x42C0750): the owner (+0xA8) AWaterBody's spline component
// (+0x2A0) and its Bounds (USceneComponent +0x108..+0x130), GetWaterBodyType (vcall +0xA28), ShouldRender (call +0x107),
// ShouldGenerateWaterMeshTile (call +0x117), the water body's Z (+0x290), GetWaterMaterialInstance (vcall +0xB48) -> WaterMID (+0xDC8).
constexpr char kLambda[] =
    "4C 39 B8 A0 02 00 00 0F 84 C4 0E 00 00 48 8B 9E A8 00 00 00 48 85 DB 74 34 E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 30 48 63 40 "
    "38 3B 42 38 7F 0D 48 8B C8 48 8B 42 30 4C 39 04 C8 74 03 49 8B DF 48 85 DB 74 09 48 8B 83 A0 02 00 00 EB 03 49 8B C7 F2 0F "
    "10 80 28 01 00 00 F2 0F 10 98 10 01 00 00 F2 0F 10 90 08 01 00 00 0F 28 E3 F2 0F 10 88 20 01 00 00 F2 0F 58 E0 45 0F 10 46 "
    "08 0F 28 FA F2 0F 5C D8 41 0F 10 46 18 F2 0F 5C D1 41 0F 28 E8 0F 28 F0 66 41 0F 15 E8 66 0F 15 F0 F2 0F 58 F9 66 0F 2F D0 "
    "0F 87 27 0E 00 00 66 44 0F 2F C7 0F 87 1C 0E 00 00 66 0F 2F DE 0F 87 12 0E 00 00 66 0F 2F EC 0F 87 08 0E 00 00 B8 00 80 FF "
    "FF 4C 89 7D 88 66 89 45 98 0F 57 C0 B8 FF FF FF FF F2 0F 11 45 90 66 89 45 A0 48 8B CE 48 8B 06 F3 0F 7F 44 24 78 C7 45 9C "
    "FF FF FF FF FF 90 28 0A 00 00 48 8B CE 44 0F B6 E8 E8 ?? ?? ?? ?? 84 C0 0F 84 BD 0D 00 00 48 8B CE E8 ?? ?? ?? ?? 84 C0 0F "
    "84 AD 0D 00 00 41 80 FD 02 74 43 45 38 7E 30 74 3D 45 84 ED 75 1D 48 8B 07 48 8D 55 C0 45 33 C9 45 33 C0 48 8B CF FF 90 28 "
    "06 00 00 F2 0F 10 40 28 EB 0C 0F 10 86 90 02 00 00 F2 0F 11 45 08 F2 0F 5A C0 41 0F 2F 46 34 0F 86 64 0D 00 00 48 8B 06 48 "
    "8B CE FF 90 48 0B 00 00 48 8B 9E C8 0D 00 00";
constexpr size_t kLambdaShouldRenderCall = 0x107;
// USceneComponent::ShouldRender (exe+0x308C6C0): the owner (+0xA8) is hidden when its byte +0x58 has bit 7 (AActor::bHidden),
// the component's bVisible is +0x1A0 bit 0x20. Checked to be the function the lambda calls.
constexpr char kShouldRender[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B B1 B0 00 00 00 48 8B D9 48 8B B9 A8 00 00 00 48 85 F6 75 08 E8 ?? ?? ?? "
    "?? 48 8B F0 48 85 FF 74 19 48 8B CF E8 ?? ?? ?? ?? 48 85 C0 74 0C 48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 5E 48 85 F6 74 11 48 8B "
    "CE E8 ?? ?? ?? ?? 84 C0 74 05 40 B6 01 EB 03 40 32 F6 48 8B 03 48 8B CB FF 90 30 05 00 00 84 C0 74 0F 48 85 FF 74 06 80 7F "
    "58 00 7C 04 B0 01 EB 02 32 C0 40 84 F6 74 1F 84 C0 74 1B F6 83 A0 01 00 00 20 74 12 B0 01 48 8B 5C 24 30 48 8B 74 24 38 48 "
    "83 C4 20 5F C3";
// UWaterBodyRiverComponent::CreateOrUpdateLakeTransitionMID / ...OceanTransitionMID (exe+0x42C4030, +0x42C40B0, adjacent):
// LakeTransitionMaterial +0x1580 -> LakeTransitionMID +0x1588, OceanTransitionMaterial +0x1590 -> OceanTransitionMID +0x1598.
constexpr char kRiverMids[] =
    "40 57 48 83 EC 20 48 83 B9 B0 00 00 00 00 48 8B F9 75 0A E8 ?? ?? ?? ?? 48 85 C0 74 57 48 89 5C 24 38 48 8D 15 ?? ?? ?? ?? "
    "48 8B 9F 80 15 00 00 48 8D 4C 24 30 41 B8 01 00 00 00 E8 ?? ?? ?? ?? 48 8B 8F 88 15 00 00 41 B9 40 00 10 02 4C 8B C3 48 8B "
    "10 E8 ?? ?? ?? ?? 48 89 87 88 15 00 00 48 8B D0 4C 8B 07 48 8B CF 41 FF 90 78 0A 00 00 48 8B 5C 24 38 48 83 C4 20 5F C3 CC "
    "CC CC CC CC CC 40 57 48 83 EC 20 48 83 B9 B0 00 00 00 00 48 8B F9 75 0A E8 ?? ?? ?? ?? 48 85 C0 74 57 48 89 5C 24 38 48 8D "
    "15 ?? ?? ?? ?? 48 8B 9F 90 15 00 00 48 8D 4C 24 30 41 B8 01 00 00 00 E8 ?? ?? ?? ?? 48 8B 8F 98 15 00 00 41 B9 40 00 10 02 "
    "4C 8B C3 48 8B 10 E8 ?? ?? ?? ?? 48 89 87 98 15 00 00 48 8B D0 4C 8B 07 48 8B CF 41 FF 90 78 0A 00 00 48 8B 5C 24 38 48 83 "
    "C4 20 5F C3";

// WaterViewExtension's inlined UWaterMeshComponent::PushTessellatedWaterMeshBoundsToPoxy (exe+0x42E667D): the anchor is
// zone bEnableLocalOnlyTessellation (+0x2E0), WaterMesh (+0x2B8), TileSize (+0x5EC); within 0x180 bytes after it the
// `mov rbx,[rbx+disp32]` whose rbx is tested within 0x60 bytes (`test rbx,rbx; je`) is `if (SceneProxy)` before the
// proxy's OnTessellatedWaterMeshBoundsChanged_GameThread: disp32 = UPrimitiveComponent::SceneProxy (0x578 on 19115871).
constexpr char kViewExtAnchor[] = "80 B9 E0 02 00 00 00 0F 84 ?? ?? ?? ?? 48 8B 99 B8 02 00 00 F3 0F 10 83 EC 05 00 00";
// UWaterMeshComponent::CreateSceneProxy (exe+0x42C4670): `if (!bIsEnabled (+0x6E2)) return nullptr; return new
// FWaterMeshSceneProxy(this)`; detoured (review revwaterskip2 S2-1) to give a taken-back mark back when it returns null.
constexpr char kCreateProxy[] =
    "40 53 48 83 EC 20 80 B9 E2 06 00 00 00 48 8B D9 74 1F B9 00 04 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 10 48 8B D3 48 8B C8 48 83 "
    "C4 20 5B E9 ?? ?? ?? ?? 33 C0 48 83 C4 20 5B C3";
constexpr uint8_t kCreatePrologue[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};  // push rbx; sub rsp,0x20

// the layout (pinned above; UActorComponent::OwnerPrivate +0xA8 is checked at run time: zone->WaterMesh->Owner == zone)
constexpr size_t kBodyMid = 0xDC8, kBounds = 0x108, kBoundsLen = 0x38, kOwnerSpline = 0x2A0, kOwnerHidden = 0x58;
constexpr size_t kRiverLakeMid = 0x1588, kRiverOceanMid = 0x1598, kTypeSlot = 0xA28;
constexpr uint8_t kRiverType = 0;  // EWaterBodyType::River (the lambda's "test r13b; jne" branch to the transition MIDs)
constexpr size_t kSubsysBodies = 0x40, kSubsysBodiesNum = 0x48;
constexpr size_t kBodyZoneWeak = 0xE10, kBodyVis = 0x1A0, kBodyAlways = 0xDB8, kBodyOverride = 0xDB0, kBodyMaterial = 0xCE0;
constexpr size_t kXform = 0x260, kXformLen = 0x60;  // FTransform ComponentToWorld (rotation, translation +0x20, scale)
constexpr size_t kZoneBodies = 0x2A0, kZoneMesh = 0x2B8, kOwner = 0xA8;
size_t g_sceneProxy = 0x578;  // UPrimitiveComponent::SceneProxy (read from the view extension's code at install)
constexpr size_t kMeshFlag = 0x6E1, kMeshEnabled = 0x6E2, kMeshTile = 0x5EC, kMeshTileLen = 12, kMeshCvars = 0x6E4;
constexpr size_t kTree = 0x5F8, kTreeLen = 0x68;  // FWaterQuadTree through its WaterBodyRenderData array's count
constexpr int kMaxBodies = 512;

using NotifyFn = void (*)(uint8_t* subsystem, void* cvar);
using RebuildFn = void (*)(uint8_t* mesh, float tile, const int32_t* extent);
using CreateProxyFn = void* (*)(uint8_t* mesh);
using WeakGetFn = uint8_t* (*)(const void* weak);

NotifyFn g_oNotify = nullptr;
RebuildFn g_oRebuild = nullptr;
CreateProxyFn g_oCreate = nullptr;
WeakGetFn g_weak = nullptr;
bool g_installed = false, g_searched = false;
std::atomic<int> g_mode{0};

struct Entry {
    uint8_t* mesh = nullptr;
    uint8_t* zone = nullptr;
    uint64_t inputs = 0, tree = 0, used = 0;
    double lastMs = 0;
    bool takenBack = false;  // a mark of this mesh was taken back since its last real rebuild
};
constexpr int kEntries = 32;
Entry g_entries[kEntries];
uint64_t g_useClock = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

std::atomic<uint64_t> g_loops{0}, g_marks{0}, g_kept{0}, g_changed{0}, g_noRecord{0}, g_disabled{0}, g_rebuilds{0},
    g_rebuildTicks{0}, g_savedTicks{0}, g_verified{0}, g_verifyDiff{0}, g_armed{0}, g_givenBack{0}, g_lostProxy{0};
uint64_t g_logged = 0;
int g_changedLogged = 0;

constexpr uint64_t kFnvInit = 1469598103934665603ull;
uint64_t Fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
bool ReadBytes(const uint8_t* p, size_t off, void* dst, size_t n)
{
    if (!p) return false;
    __try { memcpy(dst, p + off, n); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
template <typename T> bool Read(const uint8_t* p, size_t off, T* v) { return ReadBytes(p, off, v, sizeof *v); }
uint8_t* Weak(const uint8_t* weakAt)
{
    if (!weakAt || !g_weak) return nullptr;
    __try { return g_weak(weakAt); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// UWaterBodyComponent::GetWaterBodyType (vcall +0xA28: a constant per class, no side effects; UCV and the rebuild call it)
using TypeFn = uint8_t (*)(const uint8_t* body);
bool BodyType(const uint8_t* body, int* out)
{
    __try {
        const auto vt = *reinterpret_cast<TypeFn* const*>(body);
        *out = vt[kTypeSlot / sizeof(void*)](body);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
double TicksToMs(uint64_t t)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(t) / static_cast<double>(f.QuadPart);
}

// The zone of a mesh component (its owner), checked both ways; nullptr when the layout does not hold.
uint8_t* ZoneOf(uint8_t* mesh)
{
    uint8_t* zone = nullptr;
    uint8_t* back = nullptr;
    if (!Read(mesh, kOwner, &zone) || !zone || !Read(zone, kZoneMesh, &back) || back != mesh) return nullptr;
    return zone;
}

// The rebuild inputs this key watches (rule 3), as one hash; false when anything cannot be read or does not hold together.
bool Inputs(uint8_t* mesh, uint8_t* zone, uint64_t* out)
{
    uint8_t buf[kXformLen];
    uint64_t h = kFnvInit;
    if (!ReadBytes(mesh, kXform, buf, kXformLen)) return false;
    h = Fnv(h, buf, kXformLen);
    if (!ReadBytes(mesh, kMeshTile, buf, kMeshTileLen)) return false;
    h = Fnv(h, buf, kMeshTileLen);
    if (!ReadBytes(mesh, kMeshCvars, buf, 12)) return false;
    h = Fnv(h, buf, 12);
    uint8_t* arr = nullptr;
    int32_t num = -1;
    if (!Read(zone, kZoneBodies, &arr) || !Read(zone, kZoneBodies + 8, &num) || num < 0 || num > kMaxBodies) return false;
    if (num && !arr) return false;
    h = Fnv(h, &num, sizeof num);
    for (int32_t i = 0; i < num; ++i) {
        const uint8_t* const weak = arr + static_cast<size_t>(i) * 8;
        uint8_t w[8];
        if (!ReadBytes(weak, 0, w, 8)) return false;
        h = Fnv(h, w, 8);
        uint8_t* const body = Weak(weak);
        h = Fnv(h, &body, sizeof body);
        if (!body) continue;  // a stale entry: the rebuild skips it too (ForEachWaterBodyComponent: valid ones only)
        if (Weak(body + kBodyZoneWeak) != zone) return false;  // the body's own zone must be this zone
        if (!ReadBytes(body, kXform, buf, kXformLen)) return false;
        h = Fnv(h, buf, kXformLen);
        uint8_t s[4];
        uint64_t p[2];
        if (!ReadBytes(body, kBodyVis, s, 3) || !Read(body, kBodyAlways, &s[3]) || !Read(body, kBodyOverride, &p[0]) ||
            !Read(body, kBodyMaterial, &p[1]))
            return false;
        h = Fnv(h, s, sizeof s);
        h = Fnv(h, p, sizeof p);
        // S2-1: the MID the tree stores, the body's bounds, the owner and its hidden bit, the spline and its bounds, and for a
        // river its two transition MIDs (read only when GetWaterBodyType, a constant per class, says River)
        uint64_t mid = 0;
        uint8_t* owner = nullptr;
        if (!Read(body, kBodyMid, &mid) || !ReadBytes(body, kBounds, buf, kBoundsLen) || !Read(body, kOwner, &owner)) return false;
        h = Fnv(h, &mid, sizeof mid);
        h = Fnv(h, buf, kBoundsLen);
        h = Fnv(h, &owner, sizeof owner);
        if (owner) {
            uint8_t hidden = 0;
            uint8_t* spline = nullptr;
            if (!Read(owner, kOwnerHidden, &hidden) || !Read(owner, kOwnerSpline, &spline)) return false;
            h = Fnv(h, &hidden, 1);
            h = Fnv(h, &spline, sizeof spline);
            if (spline) {
                if (!ReadBytes(spline, kBounds, buf, kBoundsLen)) return false;
                h = Fnv(h, buf, kBoundsLen);
            }
        }
        int type = -1;
        if (!BodyType(body, &type)) return false;
        h = Fnv(h, &type, sizeof type);
        if (type == kRiverType) {
            uint64_t rm[2];
            if (!Read(body, kRiverLakeMid, &rm[0]) || !Read(body, kRiverOceanMid, &rm[1])) return false;
            h = Fnv(h, rm, sizeof rm);
        }
    }
    *out = h;
    return true;
}
bool TreeHash(uint8_t* mesh, uint64_t* out)
{
    uint8_t t[kTreeLen];
    if (!ReadBytes(mesh, kTree, t, kTreeLen)) return false;
    *out = Fnv(kFnvInit, t, kTreeLen);
    return true;
}

Entry* Find(uint8_t* mesh)  // under g_lock
{
    for (Entry& e : g_entries) if (e.mesh == mesh) return &e;
    return nullptr;
}
Entry* FindOrAdd(uint8_t* mesh)  // under g_lock (exclusive): the least recently used slot is reused
{
    if (Entry* e = Find(mesh)) return e;
    Entry* best = &g_entries[0];
    for (Entry& e : g_entries) {
        if (!e.mesh) { best = &e; break; }
        if (e.used < best->used) best = &e;
    }
    *best = Entry{};
    best->mesh = mesh;
    return best;
}


void HookRebuild(uint8_t* mesh, float tile, const int32_t* extent)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!mode) { g_oRebuild(mesh, tile, extent); return; }
    uint8_t* const zone = ZoneOf(mesh);
    uint64_t inputs = 0;
    const bool haveInputs = zone && Inputs(mesh, zone, &inputs);
    const uint64_t t0 = Ticks();
    g_oRebuild(mesh, tile, extent);
    const uint64_t dt = Ticks() - t0;
    g_rebuilds.fetch_add(1, std::memory_order_relaxed);
    g_rebuildTicks.fetch_add(dt, std::memory_order_relaxed);
    uint64_t tree = 0;
    const bool haveTree = TreeHash(mesh, &tree);
    AcquireSRWLockExclusive(&g_lock);
    Entry* const e = FindOrAdd(mesh);
    e->zone = haveInputs && haveTree ? zone : nullptr;  // nullptr: no record, the next callback mark stays
    e->inputs = inputs;
    e->tree = tree;
    e->lastMs = TicksToMs(dt);
    e->takenBack = false;
    e->used = ++g_useClock;
    ReleaseSRWLockExclusive(&g_lock);
}

struct Seen {
    uint8_t* zone;
    uint8_t* mesh;
    uint8_t before;
};
constexpr int kMaxZones = 32;

void HookNotify(uint8_t* subsystem, void* cvar)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!mode) { g_oNotify(subsystem, cvar); return; }
    // before: every zone the loop will mark (the zones of the subsystem's bodies) and its mesh flag
    Seen seen[kMaxZones];
    int nz = 0;
    uint8_t** arr = nullptr;
    int32_t num = 0;
    if (Read(subsystem, kSubsysBodies, &arr) && Read(subsystem, kSubsysBodiesNum, &num) && arr && num > 0 && num <= 4096) {
        for (int32_t i = 0; i < num; ++i) {
            uint8_t* body = nullptr;
            if (!Read(reinterpret_cast<uint8_t*>(arr), static_cast<size_t>(i) * 8, &body) || !body) continue;
            uint8_t* const zone = Weak(body + kBodyZoneWeak);
            if (!zone) continue;
            bool known = false;
            for (int k = 0; k < nz; ++k) if (seen[k].zone == zone) { known = true; break; }
            if (known || nz == kMaxZones) continue;
            uint8_t* mesh = nullptr;
            uint8_t flag = 1;
            if (!Read(zone, kZoneMesh, &mesh) || !mesh || ZoneOf(mesh) != zone || !Read(mesh, kMeshFlag, &flag)) continue;
            seen[nz++] = Seen{zone, mesh, flag};
            // review revwaterskip2: an enabled mesh with no mark and no scene proxy shows no water until its next real mark
            uint8_t en = 0;
            void* px = nullptr;
            if (flag == 0 && Read(mesh, kMeshEnabled, &en) && en == 1 && Read(mesh, g_sceneProxy, &px) && !px)
                g_lostProxy.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_oNotify(subsystem, cvar);
    g_loops.fetch_add(1, std::memory_order_relaxed);
    for (int k = 0; k < nz; ++k) {
        const Seen& s = seen[k];
        uint8_t after = 0;
        void* proxy = nullptr;
        if (s.before != 0 || !Read(s.mesh, kMeshFlag, &after) || after != 1) continue;  // rule 1
        g_marks.fetch_add(1, std::memory_order_relaxed);
        if (!Read(s.mesh, g_sceneProxy, &proxy) || !proxy) { g_disabled.fetch_add(1, std::memory_order_relaxed); continue; }  // rule 2
        uint64_t inputs = 0, tree = 0;
        const bool now = Inputs(s.mesh, s.zone, &inputs) && TreeHash(s.mesh, &tree);
        AcquireSRWLockExclusive(&g_lock);
        Entry* const e = Find(s.mesh);
        const bool recorded = e && e->zone == s.zone;
        const bool same = now && recorded && e->inputs == inputs && e->tree == tree;  // rule 3
        double lastMs = 0;
        if (same) {
            lastMs = e->lastMs;
            e->used = ++g_useClock;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (!same) {
            (recorded ? g_changed : g_noRecord).fetch_add(1, std::memory_order_relaxed);
            if (recorded && g_changedLogged < 20) {
                ++g_changedLogged;
                Log("[waterskip] zone mesh %p: inputs differ from its last rebuild (%s): the game's rebuild runs", static_cast<void*>(s.mesh),
                    now ? "changed" : "not readable");
            }
            continue;
        }
        AcquireSRWLockExclusive(&g_lock);
        if (Entry* const e2 = Find(s.mesh)) e2->takenBack = true;  // before the write: CreateSceneProxy may run on another thread
        ReleaseSRWLockExclusive(&g_lock);
        uint8_t zero = 0;
        __try { *(s.mesh + kMeshFlag) = zero; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        g_kept.fetch_add(1, std::memory_order_relaxed);
        // a menu's open and close each take back one mark; the game rebuilt once per close: half the marks, at the last ms
        g_savedTicks.fetch_add(static_cast<uint64_t>(lastMs * 500.0), std::memory_order_relaxed);
    }
}

// UWaterMeshComponent::CreateSceneProxy: when it returns no proxy for a mesh whose mark we took back since its last real
// rebuild (its render state recreated while r.Water.WaterMesh.Enabled was 0), the mark is given back: the next enabled tick
// rebuilds and its MarkRenderStateDirty makes the proxy, as in the game (review revwaterskip2 S2-1). May run on any thread.
void* HookCreateProxy(uint8_t* mesh)
{
    void* const proxy = g_oCreate(mesh);
    if (proxy || !g_mode.load(std::memory_order_relaxed)) return proxy;
    bool give = false;
    AcquireSRWLockExclusive(&g_lock);
    if (Entry* const e = Find(mesh); e && e->takenBack) { e->takenBack = false; give = true; }
    ReleaseSRWLockExclusive(&g_lock);
    if (give) {
        uint8_t one = 1;
        __try { *(mesh + kMeshFlag) = one; } __except (EXCEPTION_EXECUTE_HANDLER) { return proxy; }
        if (g_givenBack.fetch_add(1, std::memory_order_relaxed) < 20)
            Log("[waterskip] zone mesh %p lost its scene proxy after a taken-back mark: mark given back (the game's rebuild runs)",
                static_cast<void*>(mesh));
    }
    return proxy;
}

bool Fail(const char* why)
{
    Log("[waterskip] WaterRebuildSkip not installed: %s", why);
    return false;
}

// Entry detour as string_table_memo.cpp: region+0 = trampoline (the first whole instructions, jmp [rip] back), region+32 =
// jmp [rip] to the hook; fn's first 5 bytes become a jmp to region+32 (one aligned qword, compare-exchange).
bool Detour(uint8_t* fn, const uint8_t* prologue, size_t len, void* hook, uint8_t* region, bool beforeGameCode)
{
    if (len < 5 || len > 16 || memcmp(fn, prologue, len) != 0) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + len;
    memcpy(region, prologue, len);
    memcpy(region + len, jmpAbs, sizeof jmpAbs);
    memcpy(region + len + 6, &resume, sizeof resume);
    memcpy(region + 32, jmpAbs, sizeof jmpAbs);
    memcpy(region + 38, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 64);
    const int64_t rel = (region + 32) - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t jmp[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, sizeof rel32);
    const uintptr_t at = reinterpret_cast<uintptr_t>(fn);
    if ((at & 7) + sizeof jmp <= 8) {
        auto* q = reinterpret_cast<volatile LONG64*>(at & ~uintptr_t{7});
        if (!CodeSiteProtect(const_cast<LONG64*>(q), 8, &old)) return false;
        const LONG64 was = *q;
        LONG64 now = was;
        memcpy(reinterpret_cast<uint8_t*>(&now) + (at & 7), jmp, sizeof jmp);
        const bool ok = InterlockedCompareExchange64(q, now, was) == was;
        VirtualProtect(const_cast<LONG64*>(q), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(q), 8);
        if (ok) RecordOwnWrite(fn, sizeof jmp);
        return ok;
    }
    return beforeGameCode && WriteCode(fn, std::span<const uint8_t>(jmp, sizeof jmp));
}

uint8_t* One(const Section& s, const char* pattern)
{
    const auto hits = FindAll(s, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}

}  // namespace

bool WaterRebuildSkipFind(const GameModule& game, WaterRebuildSkipSites* out)
{
    *out = WaterRebuildSkipSites{};
    out->notify = One(game.text, kNotify);
    out->ucv = One(game.text, kUcv);
    out->rebuild = One(game.text, kRebuild);
    out->tickPin = One(game.text, kTickPin);
    out->addPin = One(game.text, kAddPin);
    const auto weak = FindAllCallable(game.text, ParsePattern(kWeakGet), "waterskip");
    out->weakGet = weak.size() == 1 ? weak[0] : nullptr;
    out->lambda = One(game.text, kLambda);
    out->shouldRender = One(game.text, kShouldRender);
    out->riverMids = One(game.text, kRiverMids);
    out->viewExtPin = One(game.text, kViewExtAnchor);
    out->sceneProxyOff = 0;
    if (out->viewExtPin) {
        uint8_t* const a = out->viewExtPin;
        for (size_t k = 0x1C; k < 0x180 && a + k + 0x68 < game.text.end && !out->sceneProxyOff; ++k) {
            if (a[k] != 0x48 || a[k + 1] != 0x8B || a[k + 2] != 0x9B) continue;  // mov rbx,[rbx+disp32]
            for (size_t m = k + 7; m < k + 0x60; ++m)
                if (a[m] == 0x48 && a[m + 1] == 0x85 && a[m + 2] == 0xDB && a[m + 3] == 0x0F && a[m + 4] == 0x84) {  // test rbx,rbx; je
                    uint32_t d;
                    memcpy(&d, a + k + 3, 4);
                    if (d >= 0x200 && d < 0x1000 && (d & 7) == 0) out->sceneProxyOff = d;
                    break;
                }
        }
    }
    out->createProxy = One(game.text, kCreateProxy);
    out->lambdaCallsShouldRender = out->lambda && out->shouldRender &&
                                   RipTarget(out->lambda + kLambdaShouldRenderCall, 1, 5) == out->shouldRender;
    if (out->tickPin) out->tickCallsRebuild = out->rebuild && RipTarget(out->tickPin + kTickPinRebuildCall, 1, 5) == out->rebuild;
    if (!out->notify) out->why = "NotifyWaterVisibilityChangedInternal not found (byte pattern; another game version?)";
    else if (!out->ucv) out->why = "UpdateComponentVisibility's layout not found (byte pattern)";
    else if (!out->rebuild) out->why = "RebuildWaterMesh not found (byte pattern)";
    else if (!out->tickPin || !out->tickCallsRebuild) out->why = "UWaterMeshComponent::Update's layout not found in the water tick (byte pattern)";
    else if (!out->addPin) out->why = "AddWaterBodyComponent's layout not found (byte pattern)";
    else if (!out->weakGet) out->why = "FWeakObjectPtr::Get not found (byte pattern)";
    else if (!out->lambda || !out->lambdaCallsShouldRender) out->why = "the rebuild's per-body layout not found (byte pattern)";
    else if (!out->riverMids) out->why = "the river transition materials' layout not found (byte pattern)";
    else if (!out->viewExtPin || !out->sceneProxyOff) out->why = "the water mesh's scene proxy field not found (byte pattern)";
    else if (!out->createProxy || memcmp(out->createProxy, kCreatePrologue, sizeof kCreatePrologue) != 0)
        out->why = "UWaterMeshComponent::CreateSceneProxy not found (byte pattern)";
    else if (memcmp(out->notify, kNotifyPrologue, sizeof kNotifyPrologue) != 0 || memcmp(out->rebuild, kRebuildPrologue, sizeof kRebuildPrologue) != 0)
        out->why = "a hooked function's first instructions changed";
    return out->why == nullptr;
}

bool InstallWaterRebuildSkip(bool beforeGameCode)
{
    if (g_installed) return true;
    if (g_searched) return false;
    g_searched = true;
    const GameModule& game = Game();
    WaterRebuildSkipSites s;
    if (!WaterRebuildSkipFind(game, &s)) return Fail(s.why);
    // one allocation (its own page) per trampoline: Detour makes its region's page read-execute, so a second trampoline in the
    // same page could no longer be written (lab run c1 2026-11-25: the second Detour's memcpy faulted in DllMain)
    uint8_t* const region = AllocNearGame(64);
    uint8_t* const region2 = AllocNearGame(64);
    uint8_t* const region3 = AllocNearGame(64);
    if (!region || !region2 || !region3) return Fail("no free memory near the game's code");
    g_sceneProxy = s.sceneProxyOff;
    g_oCreate = reinterpret_cast<CreateProxyFn>(region3);
    g_weak = reinterpret_cast<WeakGetFn>(s.weakGet);
    g_oNotify = reinterpret_cast<NotifyFn>(region);
    g_oRebuild = reinterpret_cast<RebuildFn>(region2);
    // the rebuild's detour first: the callback's decisions rely on the rebuilds being recorded
    if (!Detour(s.rebuild, kRebuildPrologue, sizeof kRebuildPrologue, reinterpret_cast<void*>(&HookRebuild), region2, beforeGameCode))
        return Fail("RebuildWaterMesh's entry could not be detoured (another mod hooks it?)");
    // then the proxy safety net (it only acts on meshes whose mark we took back)
    if (!Detour(s.createProxy, kCreatePrologue, sizeof kCreatePrologue, reinterpret_cast<void*>(&HookCreateProxy), region3, beforeGameCode)) {
        g_mode = 0;
        return Fail("CreateSceneProxy's entry could not be detoured (another mod hooks it?)");
    }
    if (!Detour(s.notify, kNotifyPrologue, sizeof kNotifyPrologue, reinterpret_cast<void*>(&HookNotify), region, beforeGameCode)) {
        // the rebuild detour stays (it only records while the key is on; the callback hook never runs): off
        g_mode = 0;
        return Fail("NotifyWaterVisibilityChangedInternal's entry could not be detoured (another mod hooks it?)");
    }
    g_installed = true;
    Log("[waterskip] installed: NotifyWaterVisibilityChangedInternal exe+0x%llX and RebuildWaterMesh exe+0x%llX detoured (a water "
        "zone's mesh rebuild asked by the visibility callback is skipped when its inputs are those of its last rebuild)",
        static_cast<unsigned long long>(s.notify - game.base), static_cast<unsigned long long>(s.rebuild - game.base));
    return true;
}

void SetWaterRebuildSkip(int mode)
{
    if (!g_installed) return;
    if (mode > 1) mode = 1;
    if (mode < 0 || mode > 2) mode = 1;
    if (g_mode.load() == mode) return;
    AcquireSRWLockExclusive(&g_lock);
    for (Entry& e : g_entries) e = Entry{};  // nothing was recorded while off; a new mode starts from the next rebuilds
    g_mode.store(mode);
    ReleaseSRWLockExclusive(&g_lock);
    Log("[waterskip] WaterRebuildSkip %d", mode);
}

void LogWaterRebuildSkipStats()
{
    if (!g_installed) return;
    const uint64_t loops = g_loops.load(), rebuilds = g_rebuilds.load(), verified = g_verified.load();
    const uint64_t sum = loops + rebuilds + verified;
    if (sum == g_logged) return;
    g_logged = sum;
    const double avg = rebuilds ? TicksToMs(g_rebuildTicks.load()) / static_cast<double>(rebuilds) : 0.0;
    Log("[waterskip] stats: %llu visibility callbacks, %llu zone marks made by them: %llu taken back (inputs as at the zone's last "
        "rebuild), %llu kept (%llu inputs changed, %llu no record yet, %llu no scene proxy); %llu real rebuilds (%.2f ms each); "
        "about %.1f ms of rebuilds saved; %llu marks given back (proxy lost), %llu enabled meshes seen without mark or proxy; "
        "verify %llu compared, %llu different (%llu armed)",
        static_cast<unsigned long long>(loops), static_cast<unsigned long long>(g_marks.load()),
        static_cast<unsigned long long>(g_kept.load()),
        static_cast<unsigned long long>(g_changed.load() + g_noRecord.load() + g_disabled.load()),
        static_cast<unsigned long long>(g_changed.load()), static_cast<unsigned long long>(g_noRecord.load()),
        static_cast<unsigned long long>(g_disabled.load()), static_cast<unsigned long long>(rebuilds), avg,
        static_cast<double>(g_savedTicks.load()) / 1000.0, static_cast<unsigned long long>(g_givenBack.load()),
        static_cast<unsigned long long>(g_lostProxy.load()), static_cast<unsigned long long>(verified),
        static_cast<unsigned long long>(g_verifyDiff.load()), static_cast<unsigned long long>(g_armed.load()));
}

// ---- self-test: the decision logic on fake objects (nothing installed) ----
namespace {
struct FakeWorld {
    alignas(16) uint8_t subsys[0x60];
    alignas(16) uint8_t zone[0x320];
    alignas(16) uint8_t mesh[0x700];
    alignas(16) uint8_t body[2][0x15A0];  // body 1 is a river (transition MIDs at +0x1588 / +0x1598)
    alignas(16) uint8_t owner[2][0x2B0];
    alignas(16) uint8_t spline[2][0x140];
    uint64_t zoneWeak[2];     // zone list (TWeakObjectPtr = 8 bytes; the fake holds an index)
    uint8_t* bodies[2];       // the subsystem's array
};
FakeWorld* g_fw = nullptr;
uint8_t* FakeWeak(const void* weak)
{
    uint64_t v;
    memcpy(&v, weak, 8);
    if (v == 1) return g_fw->body[0];
    if (v == 2) return g_fw->body[1];
    if (v == 9) return g_fw->zone;
    return nullptr;
}
int g_fakeNotifyCalls = 0, g_fakeRebuildCalls = 0;
void FakeNotify(uint8_t*, void*)  // the game's loop: every body marks its zone
{
    ++g_fakeNotifyCalls;
    g_fw->mesh[kMeshFlag] = 1;
    g_fw->zone[0x300] = 1;
}
void FakeRebuild(uint8_t* mesh, float, const int32_t*)
{
    ++g_fakeRebuildCalls;  // the same inputs make the same tree: the fake leaves it as it is
    (void)mesh;
}
void Put(uint8_t* p, size_t off, const void* v, size_t n) { memcpy(p + off, v, n); }
void* FakeCreateNull(uint8_t*) { return nullptr; }
void* FakeCreateSome(uint8_t*) { return reinterpret_cast<void*>(0x1000); }
uint8_t FakeLake(const uint8_t*) { return 1; }
uint8_t FakeRiver(const uint8_t*) { return kRiverType; }
void* g_lakeVt[kTypeSlot / 8 + 1];
void* g_riverVt[kTypeSlot / 8 + 1];
}  // namespace

int WaterRebuildSkipTestLogic()
{
    if (g_installed) return 0x80000000;  // never on a live detour
    int fail = 0;
    static FakeWorld fw;
    memset(&fw, 0, sizeof fw);
    g_fw = &fw;
    g_weak = &FakeWeak;
    g_oNotify = &FakeNotify;
    g_oRebuild = &FakeRebuild;
    for (Entry& e : g_entries) e = Entry{};
    g_kept = g_changed = g_noRecord = g_disabled = g_marks = g_loops = g_givenBack = g_lostProxy = 0;
    g_sceneProxy = 0x578;
    // wiring: subsystem -> 2 bodies -> zone (weak 9) -> mesh (owner = zone); zone list = weak 1, 2
    fw.bodies[0] = fw.body[0];
    fw.bodies[1] = fw.body[1];
    uint8_t* const barr = reinterpret_cast<uint8_t*>(fw.bodies);
    const int32_t two = 2;
    Put(fw.subsys, kSubsysBodies, &barr, 8);
    Put(fw.subsys, kSubsysBodiesNum, &two, 4);
    const uint64_t nine = 9;
    for (auto& b : fw.body) Put(b, kBodyZoneWeak, &nine, 8);
    fw.zoneWeak[0] = 1;
    fw.zoneWeak[1] = 2;
    uint8_t* const zarr = reinterpret_cast<uint8_t*>(fw.zoneWeak);
    Put(fw.zone, kZoneBodies, &zarr, 8);
    Put(fw.zone, kZoneBodies + 8, &two, 4);
    uint8_t* const meshP = fw.mesh;
    uint8_t* const zoneP = fw.zone;
    Put(fw.zone, kZoneMesh, &meshP, 8);
    Put(fw.mesh, kOwner, &zoneP, 8);
    fw.mesh[kMeshEnabled] = 1;
    const uint64_t fakeProxy = 0xABCDEF0;
    Put(fw.mesh, g_sceneProxy, &fakeProxy, 8);
    g_lakeVt[kTypeSlot / 8] = reinterpret_cast<void*>(&FakeLake);
    g_riverVt[kTypeSlot / 8] = reinterpret_cast<void*>(&FakeRiver);
    void** const lakeVt = g_lakeVt;
    void** const riverVt = g_riverVt;
    Put(fw.body[0], 0, &lakeVt, 8);
    Put(fw.body[1], 0, &riverVt, 8);
    for (int i = 0; i < 2; ++i) {
        uint8_t* const o = fw.owner[i];
        uint8_t* const sp = fw.spline[i];
        Put(fw.body[i], kOwner, &o, 8);
        Put(fw.owner[i], kOwnerSpline, &sp, 8);
    }
    const uint64_t mat = 0x1234;
    Put(fw.body[0], kBodyMaterial, &mat, 8);
    const int32_t ext[2] = {8, 8};
    auto notify = [&]() { HookNotify(fw.subsys, nullptr); };
    auto tick = [&]() {  // UWaterMeshComponent::Update: rebuild when marked
        if (fw.mesh[kMeshFlag] && fw.mesh[kMeshEnabled]) { HookRebuild(fw.mesh, 1.0f, ext); fw.mesh[kMeshFlag] = 0; }
    };
    // off: the game's mark stays, the tick rebuilds
    g_mode = 0;
    notify();
    if (fw.mesh[kMeshFlag] != 1) fail |= 1;
    tick();
    // on: no record yet -> the mark stays, the rebuild is recorded
    g_mode = 1;
    fw.mesh[kMeshFlag] = 0;
    notify();
    if (fw.mesh[kMeshFlag] != 1 || g_noRecord.load() != 1) fail |= 2;
    g_fakeRebuildCalls = 0;
    tick();
    if (g_fakeRebuildCalls != 1) fail |= 2;
    // on, inputs unchanged: the callback's mark is taken back (open and close), the info mark stays, no rebuild
    fw.zone[0x300] = 0;
    notify();
    notify();
    if (fw.mesh[kMeshFlag] != 0 || fw.zone[0x300] != 1 || g_kept.load() != 2 || g_fakeNotifyCalls < 4) fail |= 4;
    g_fakeRebuildCalls = 0;
    tick();
    if (g_fakeRebuildCalls != 0) fail |= 4;
    // a mark made before the loop (another writer): never touched
    fw.mesh[kMeshFlag] = 1;
    notify();
    if (fw.mesh[kMeshFlag] != 1) fail |= 8;
    tick();
    // a body's visibility byte, its material, its transform, the mesh's tile size changed: the mark stays
    struct Change { uint8_t* p; size_t off; };
    const Change changes[] = {{fw.body[1], kBodyVis + 1}, {fw.body[0], kBodyMaterial}, {fw.body[1], kXform + 0x20},
                              {fw.mesh, kMeshTile}, {fw.mesh, kXform + 0x28}, {fw.body[0], kBodyAlways}, {fw.body[1], kBodyOverride},
                              {fw.body[0], kBodyMid}, {fw.body[1], kBounds + 0x10}, {fw.owner[0], kOwnerHidden},
                              {fw.spline[1], kBounds + 0x20}, {fw.body[1], kRiverLakeMid}, {fw.body[1], kRiverOceanMid}};
    for (const Change& c : changes) {
        c.p[c.off] ^= 0x5A;
        notify();
        if (fw.mesh[kMeshFlag] != 1) fail |= 16;
        tick();  // the rebuild records the new inputs
        notify();
        if (fw.mesh[kMeshFlag] != 0) fail |= 16;  // and the next callback mark is taken back again
    }
    // a lake's bytes at the river offsets are never read: changing them keeps the take-back
    fw.body[0][kRiverLakeMid] ^= 0x33;
    notify();
    if (fw.mesh[kMeshFlag] != 0) fail |= 1024;
    // the tree storage changed behind the record (another rebuild path): the mark stays
    fw.mesh[kTree + 0x40] ^= 1;
    notify();
    if (fw.mesh[kMeshFlag] != 1) fail |= 32;
    tick();
    // the mesh is disabled (ticked while r.Water.WaterMesh.Enabled was 0) but keeps its proxy: taken back (the menu's case)
    fw.mesh[kMeshEnabled] = 0;
    notify();
    if (fw.mesh[kMeshFlag] != 0) fail |= 64;
    // the proxy is gone (render state recreated while disabled): the mark stays
    const uint64_t noProxy = 0;
    Put(fw.mesh, g_sceneProxy, &noProxy, 8);
    notify();
    if (fw.mesh[kMeshFlag] != 1 || g_disabled.load() != 1) fail |= 64;
    fw.mesh[kMeshEnabled] = 1;
    tick();
    Put(fw.mesh, g_sceneProxy, &fakeProxy, 8);
    // a body that names another zone, a body list that cannot be read: the mark stays
    const uint64_t other = 7;
    Put(fw.body[1], kBodyZoneWeak, &other, 8);  // body 1 now resolves to no zone: zone list check fails
    notify();
    if (fw.mesh[kMeshFlag] != 1) fail |= 128;
    tick();
    Put(fw.body[1], kBodyZoneWeak, &nine, 8);
    // that rebuild could not read its inputs: no record, so the next callback mark stays too
    notify();
    if (fw.mesh[kMeshFlag] != 1) fail |= 256;
    tick();
    notify();  // recorded again by that rebuild: taken back
    if (fw.mesh[kMeshFlag] != 0) fail |= 512;
    // review revwaterskip2: a taken-back mesh whose proxy creation returns null gets its mark back; a proxy made: nothing
    g_oCreate = &FakeCreateNull;
    if (HookCreateProxy(fw.mesh) != nullptr || fw.mesh[kMeshFlag] != 1 || g_givenBack.load() != 1) fail |= 2048;
    fw.mesh[kMeshFlag] = 0;
    HookCreateProxy(fw.mesh);  // given back once only
    if (fw.mesh[kMeshFlag] != 0 || g_givenBack.load() != 1) fail |= 2048;
    fw.mesh[kMeshFlag] = 1;
    tick();
    notify();  // taken back again
    g_oCreate = &FakeCreateSome;
    if (HookCreateProxy(fw.mesh) == nullptr || fw.mesh[kMeshFlag] != 0 || g_givenBack.load() != 1) fail |= 4096;
    // an enabled mesh with no mark and no proxy is counted
    const uint64_t noProxy2 = 0;
    Put(fw.mesh, g_sceneProxy, &noProxy2, 8);
    notify();
    if (g_lostProxy.load() != 1) fail |= 8192;
    g_mode = 0;
    for (Entry& e : g_entries) e = Entry{};
    g_weak = nullptr;
    g_oNotify = nullptr;
    g_oRebuild = nullptr;
    g_oCreate = nullptr;
    g_fw = nullptr;
    return fail;
}

}  // namespace opf
