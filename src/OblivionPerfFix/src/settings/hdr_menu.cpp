// HdrMenuRows (off by default): an HDR section in the game's own PC Graphics settings
// page, between "General" and "Quality": HDR On / Off, Peak Brightness, Paper White, Black Floor, Mid-Tone.
//
// The settings pages are data:
// - The page table's Graphics page points to a category table (DT_Modern_Setting_GraphicsTable).
// - That table's Categories array has one row per section: {Name text (the banner), Settings -> a VModernSettingTable}.
//   Graphics has General (DT_Modern_Settings_GraphicsGeneral), Quality and Upscaler.
// - Each VModernSettingTable holds a Settings array of ModernSettingTableRow: name and description texts, a widget type
//   (1 steps, 2 slider), a platform bit mask whose bit 0 is PC, step commands or a slider's console variable and range.
//
// The variables the rows drive (measured live, captures/probe/20261027-hdr-semantics.txt and 20261027-cvarpairs-*):
// - Altar.GraphicsOptions.HDR (0 / 1) and Altar.GraphicsOptions.HDRWhitePoint (nits) run the game's own HDR switch.
//   The settings object's bUseHDRDisplayOutput and HDRDisplayOutputNits, r.HDR.EnableHDROutput, the output device and
//   its MaxLuminance follow.
// - r.HDR.UI.Luminance is the brightness of menus, subtitles and the HUD in HDR (nits; the game's 300).
// - r.HDR.Display.MinLuminanceLog10 is the black floor (log10 nits; -4 = 0.0001 nits, the engine's darkest).
// - r.HDR.Display.MidLuminance is the scene's mid-grey in nits (15): the overall scene brightness in HDR.
// - The game's Brightness option changes nothing with HDR on (picture difference within the no-change controls).
//
// Once the tables exist, this does the following on the game thread (NpcCreationPacing's frame hook). It walks
// GUObjectArray (once in one frame when the names first exist, later 16384 objects a frame) until it has the General
// table, the Graphics category table and the settings object. Then:
// - It creates a new VModernSettingTable with the engine's own NewObject (UGameplayStatics::SpawnObject), its outer the
//   category table.
// - It fills that table with five rows, each a full copy of one of the game's own PC rows ("Screen Space Reflections"
//   for the step row, "Brightness Intensity" for the sliders) made through reflection: texts counted once more, strings
//   and arrays copied, so every row owns what it holds. Then each row gets its name, description, command or variable
//   and range.
// - It inserts a category row "HDR" (a copy of General's, pointing to the new table) after General, in a new array.
// - It gives the four HDR sliders the game's HDROptions row sub-type and General's Brightness Intensity row SDROptions:
//   the settings widgets then grey the sliders out while HDR is off and Brightness while it is on, and switch live
//   (the game's own lock; seen in probe games).
// Apart from Brightness's sub-type, the General table is left as the game made it.
//
// The rows' values are the state when they are made: the menu's saved options (Save_Settings.sav) where they have the
// entry, else the settings object (HDR on, nits) and the defaults (paper white 200, black floor -4, mid-tone 15). The
// rows' own defaults (DefaultIndex / DefaultValue): HDR Off, peak 1000, paper white 200, black floor -4, mid-tone 15.
// Showing the page changes nothing. The menu's own widgets, its Apply and its saved options do the rest; the game
// applies the saved options at every launch. Once per session it also sets the two Altar variables to that state
// (SyncCvars), r.HDR.UI.Luminance to 200 when no paper white is saved (the engine's is 300), and puts the saved HDR
// options back into the saved options (KeepSavedOptions): see there.
//
// Every field is found by name in the engine's reflection data, and each lookup checks the property's name, type and
// size. FField, FProperty and UStruct offsets are this UE 5.3 build's; UE4SS measures the same (run/ue4ss-logs). The
// template rows are checked by their variables. Any difference means nothing is written. Texts come from the engine's
// FText::FromString (it copies our string), memory from FMemory::Malloc. The replaced texts, strings and the old
// category array stay allocated, a few hundred bytes once. The category table is checked every few seconds and the
// section made again if it was reloaded. The new table is kept by the category row's object reference; this is
// checked (the category table must be an object the garbage collector scans).
//
// What changes: a fourth section on the PC Graphics page, and Brightness Intensity greyed out while HDR is on (it does
// nothing then). Nothing else changes until the player uses the rows, apart
// from the launch peak (with HDR on, the tone mapping uses the saved peak brightness from the start; stock: 1000 nits
// until the value is changed) and paper white 200 instead of the engine's 300 when none is saved. Off by default (a menu addition, not
// a speed-up). Installed only if on at start-up; needs NpcCreationPacing's frame hook.
//
// DlssRayReconstruction (off by default, trade-off: the picture changes when the player
// picks On) uses the same machinery for one row in the Graphics page's Advanced section (DT_Modern_Settings_GraphicsUpscaler):
// "DLSS - Ray Reconstruction", Off / On, right after "DLSS - Frame Generation". The row is a full copy of the Frame
// Generation row (its sub-type Upscaler and its platform flag included, so the widgets treat it as they treat the other
// DLSS rows), its two steps running r.NGX.DLSS.DenoiserMode 0 / 1: the game's DLSS plugin's Ray Reconstruction switch
// (measured active 2026-10-27). The table's rows go into a new array with the copy inserted (the old
// array is left allocated). The shown state is the saved option (else Off), applied once and put back into the saved
// options as for the HDR rows. Only when NVIDIA's Ray Reconstruction library is present (nvngx_dlssd.dll in the game's
// DLSS plugin folder; the driver's downloaded dlssd model alone was never tested, so it does not count).
// Greying: the settings widgets enable an upscaler row (exe+0x495BF70 in build 19115871) only when the upscaler its
// command belongs to is the current one, looked up in a fixed table of the game's commands (exe+0x495AFD0: Altar.DLSS.*
// -> DLSS, Altar.FSR3.* -> FSR, ...; a row whose command is not in it is left as it is). That one call (exe+0x495BF8C)
// goes through a wrapper that asks the game's table first and answers DLSS only for r.NGX.DLSS.DenoiserMode when the
// table has no entry, so the game greys and enables the row with the other DLSS rows (seen in a probe game). The other
// caller of the table (exe+0x4756410) is left alone.
//
// HudAspectRow (off by default): a "HUD Aspect Ratio" row in the Graphics page's General
// section, after Frame Rate Limit and before Show FPS: Wide (the default, the game's HUD), 4:3, 16:9, 21:9, 32:9. The
// row is a copy of Show FPS (sub-type, platform flag) with five steps built from its first step, each running
// OPF.HudAspect 0..4: a console variable this registers with the console manager's int RegisterConsoleVariable (slot
// +0x18, the call the game's own int variables use, checked at the registration of r.VT.MaxUploadsPerFrame.Streaming). The HUD is
// WBP_ModernHud_PrimaryLayout_C, the widget in the primary game layout's GameStack (menus, loading screens and fades are
// in the layout's other stacks, seen in probe games). Its root overlay holds the vignette (WBP_Modern_Hud_Vignette_C:
// the full-screen effects such as low health), subtitles, the main HUD, the reticle, quick keys and notifications.
// Every 30 frames on the game thread each part except the vignette gets left and right padding on its overlay slot (on
// top of its own, kept), so those parts lay out in a centred box of the chosen aspect while the vignette stays full
// screen (Slate units: the viewport size divided by its scale, both from UWidgetLayoutLibrary with the HUD's owning
// player as world context); a box wider than the screen means no padding (Wide). Engine functions are
// called through UObject::ProcessEvent (found by its prologue, exe+0x10F3EA0; its object-flag and native-flag checks
// as UE 5.3's), each UFunction found by name on its class and checked by parameter count and size. The saved option is
// shown and applied once and kept as for the HDR rows. Nothing changes until the player picks an aspect other than
// Wide. Installed only if on at start-up; needs NpcCreationPacing's frame hook.
//
// Build 19115871: Conv_StringToText's exec function exe+0x2D48FD0 -> exe+0x32903A0 -> FText::FromString(const FString&)
// exe+0xE56680; FName(const TCHAR*, EFindName) exe+0xF3B8B0; FMemory::Malloc exe+0xE32F40 and FMemory::Free
// exe+0xE26500 (GMalloc exe+0x9032560, slots +0x28 / +0x48); GUObjectArray exe+0x9112A60 (from ~UObjectBase, as
// ObjectReplaceCrashGuard); Altar's command setter exe+0x48F7F10 (as FrameRateLimitAtStart finds it);
// UGameplayStatics::SpawnObject exe+0x31C1860 (its exec function exe+0x31DCAE0 by name).
#include "common.h"

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace opf {
namespace {

// Conv_StringToText's exec function (exe+0x2D48FD0): its FString parameter read into a local, then (+0x77) the call
// to FText::FromString's wrapper, then the result swapped into the return value (16 bytes, then the 4-byte flags).
//   ... 48 89 7B 20  mov [rbx+20h],rdi / E8 <rel32> call wrapper / 0F 10 0E movups xmm1,[rsi] / 0F 10 00 / 0F 11 06 /
//   0F 11 08 / 48 8B 5C 24 38 / 8B 40 10 mov eax,[rax+10h] / 89 46 10 mov [rsi+10h],eax
constexpr char kStringToTextExec[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 33 FF 49 8B F0 48 89 7C 24 20 48 8B DA 48 89 7C 24 28 E8 ?? ?? ?? ?? "
    "48 8B CB 48 39 7B 20 74 ?? 48 8B 53 18 4C 8D 44 24 20 E8 ?? ?? ?? ?? EB ?? 4C 8B 83 88 00 00 00 48 8D 54 24 20 "
    "49 8B 40 18 48 89 83 88 00 00 00 E8 ?? ?? ?? ?? 48 8B 43 20 48 8D 54 24 20 48 85 C0 48 8D 4C 24 30 40 0F 95 C7 "
    "48 03 F8 48 89 7B 20 E8 ?? ?? ?? ?? 0F 10 0E 0F 10 00 0F 11 06 0F 11 08 48 8B 5C 24 38 8B 40 10 89 46 10";
constexpr size_t kExecCall = 0x77;
// The wrapper (exe+0x32903A0): push rbx / sub rsp,20h / mov rbx,rcx / call FromString / mov rax,rbx / ... / ret
constexpr char kWrapper[] = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3";
constexpr size_t kWrapperCall = 9;
// FText::FromString(const FString&) (exe+0xE56680): rcx = the result, rdx = the string; Num = [rdx+8], a string of
// at most one character (empty) gives the shared empty text, else the characters are copied into a new string.
constexpr char kFromString[] =
    "48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 60 33 C0 48 89 74 24 78 48 63 72 08 48 8B F9 89 44 24 70 83 FE 01 7F";
// FName::FName(const TCHAR*, EFindName) (exe+0xF3B8B0): rcx = the name, rdx = the characters, r8d = find type; the
// length and wide-character scan, then the number split and the pool lookup.
constexpr char kFNameCtor[] =
    "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 89 54 24 20 33 C9 41 8B F8 4C 8B D2 44 8B C9 48 85 D2 74 ?? 0F B7 02 "
    "66 85 C0 74 ?? 66 0F 1F 44 00 00 48 83 C2 02 0F B7 C0 44 0B C8 0F B7 02 66 85 C0 75 ?? 41 F7 C1 80 FF FF FF "
    "0F 95 44 24 2C";
// FMemory::Malloc(Count, Alignment) (exe+0xE32F40): GMalloc created if missing, then a tail jump to its slot +0x28.
constexpr char kMalloc[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
    "48 8B 01 44 8B C3 48 8B D7 48 8B 5C 24 30 48 83 C4 20 5F 48 FF 60 28";
constexpr size_t kMallocG1 = 15, kMallocG2 = 32;
// FMemory::Free(Original) (exe+0xE26500): null check, GMalloc created if missing, its slot +0x48.
constexpr char kFree[] =
    "48 85 C9 74 ?? 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
    "48 8B 01 48 8B D3 FF 50 48 48 83 C4 20 5B C3";
constexpr size_t kFreeG1 = 13, kFreeG2 = 30;

// UAltarGameUserSetting::AddOrUpdateSettingsToSave's exec function (exe+0x45BCBA0, found through the engine's native
// function table by its name): its FString parameter read into a local, then (+0xB8) the call to the function itself.
constexpr char kAddOrUpdateExec[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B DA 48 8B F1 33 FF 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 30 48 89 7C 24 20 "
    "E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B CB 48 8B 44 24 30 48 89 44 24 20 8B 44 24 38 89 44 24 28 "
    "8B 44 24 3C 89 44 24 2C 48 89 7B 38 48 89 7B 40 48 39 7B 20 74 10 48 8B 53 18 4C 8D 44 24 20 E8 ?? ?? ?? ?? EB 1C "
    "4C 8B 83 88 00 00 00 48 8D 54 24 20 49 8B 40 18 48 89 83 88 00 00 00 E8 ?? ?? ?? ?? 48 8B 43 38 48 8D 54 24 20 48 85 C0 "
    "48 8B CE 48 0F 45 D0 48 8B 43 20 48 85 C0 40 0F 95 C7 48 03 F8 48 89 7B 20 E8";
constexpr size_t kAddOrUpdateCall = 0xB8;
// AddOrUpdateSettingsToSave(const FString& Command) (exe+0x4757FA0): "<console variable> <value>" split, then the value
// set or added in SavedSettings (the TMap at +0x1C8, as GetOrAddValueFromSavedSettings); nothing written to disk.
constexpr char kAddOrUpdate[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B F9 48 8D 4C 24 30 E8 ?? ?? ?? ?? 8B 4C 24 38 48 8D 35 ?? ?? ?? ?? "
    "48 81 C7 ?? ?? ?? ??";
constexpr size_t kAddOrUpdateMap = 0x2A;
// The settings save's writer (exe+0x476B290, this = the settings object): the object serialised into bytes, then
// (+0x111) the save system's SaveGame(false, the "Save_Settings" slot FString's characters, user 0, the bytes):
//   39 35 [n] cmp [slot.Num],esi / ... / 4C 8D 05 lea r8,[L""] / 4C 0F 45 05 [slot] cmovnz r8,[slot.Data] / ...
//   FF 50 28 call [rax+28h]
constexpr char kSaveWriter[] =
    "48 89 5C 24 18 48 89 7C 24 20 55 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 48 8B D9 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B C8 48 8B 10 FF 52 40 48 8B F8 48 85 C0 0F 84";
constexpr char kSaveWriterSlot[] =
    "39 35 ?? ?? ?? ?? 48 8D 4C 24 30 48 8B 07 4C 8D 05 ?? ?? ?? ?? 4C 0F 45 05 ?? ?? ?? ?? 45 33 C9 48 89 4C 24 20 33 D2 "
    "48 8B CF FF 50 28";
constexpr size_t kSaveWriterSlotAt = 0x111, kSaveWriterSlotLoad = 0x15;
// UGameplayStatics::SpawnObject's exec function (exe+0x31DCAE0, by its name in the native table): its two object
// parameters (class, outer) read, then (+0xB1) the call to SpawnObject(class, outer) and the result stored:
//   48 03 F8 / 48 89 7B 20 / E8 <rel32> call SpawnObject / 48 8B 5C 24 30 / 48 89 06 mov [rsi],rax
constexpr char kSpawnObjectExec[] =
    "48 89 5C 24 08 48 89 74 24 20 57 48 83 EC 20 33 FF 49 8B F0 48 89 7C 24 40 48 8B DA E8 ?? ?? ?? ?? 48 8B CB "
    "48 39 7B 20 74 10 48 8B 53 18 4C 8D 44 24 40 E8 ?? ?? ?? ?? EB 1C 4C 8B 83 88 00 00 00 48 8D 54 24 40 49 8B 40 18 "
    "48 89 83 88 00 00 00 E8 ?? ?? ?? ?? 48 89 7C 24 38 E8 ?? ?? ?? ?? 48 8B CB 48 39 7B 20 74 10 48 8B 53 18 4C 8D 44 24 38 "
    "E8 ?? ?? ?? ?? EB 1C 4C 8B 83 88 00 00 00 48 8D 54 24 38 49 8B 40 18 48 89 83 88 00 00 00 E8 ?? ?? ?? ?? 48 8B 43 20 "
    "48 8B 54 24 38 48 85 C0 48 8B 4C 24 40 40 0F 95 C7 48 03 F8 48 89 7B 20 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 89 06";
constexpr size_t kSpawnObjectCall = 0xB1;
constexpr size_t kSpawnObjectScan = 0x300;  // its NewObject message lies within this distance of its entry
constexpr wchar_t kNewObjectMessage[] = L"NewObject with empty name";

// UE 5.3 reflection as this build has it (each use below checks a name, a type or a size)
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18;   // UObjectBase
constexpr size_t kStructSuper = 0x40, kStructChildProps = 0x50;                           // UStruct
constexpr size_t kFieldClass = 0x08, kFieldNext = 0x18, kFieldName = 0x20;                 // FField (FFieldClass: name at +0)
constexpr size_t kPropArrayDim = 0x30, kPropElementSize = 0x34, kPropOffset = 0x44;        // FProperty
constexpr size_t kStructPropStruct = 0x70;                                                 // FStructProperty
constexpr size_t kArrayPropInner = 0x78;                                                   // FArrayProperty
constexpr size_t kBoolByteOffset = 0x71, kBoolFieldMask = 0x73;                            // FBoolProperty
constexpr size_t kArrayLastNonGc = 0x04, kArrayChunks = 0x10, kArrayNum = 0x24;            // FUObjectArray
constexpr size_t kSoftObjectSize = 0x28, kSoftSubPath = 0x18;  // FSoftObjectPtr: weak ptr, {asset path, SubPathString}
constexpr uint32_t kChunkItems = 64 * 1024;
constexpr uint32_t kRfClassDefaultObject = 0x10;
constexpr uint32_t kScanPerFrame = 16384;

struct Item {  // FUObjectItem, 24 bytes
    uint8_t* object;
    int32_t flags, clusterRoot, serial, pad;
};
struct NameId {  // FName: comparison index, number
    uint32_t index = 0, number = 0;
    bool operator==(const NameId& o) const { return index == o.index && number == o.number; }
    bool valid() const { return index != 0; }
};
struct FStr {  // FString / TArray<TCHAR>
    wchar_t* data;
    int32_t num, max;
};
struct TArr {
    uint8_t* data;
    int32_t num, max;
};

struct CmdPair {  // what Altar's command setter reads: the console variable's name and the value
    FStr name, value;
};
using SetFn = bool (*)(const CmdPair* pair, bool force);
using AddOrUpdateFn = void (*)(void* settings, const FStr* command);
using SaveWriterFn = void (*)(void* settings);
using SpawnFn = uint8_t* (*)(uint8_t* cls, uint8_t* outer);
using FromStringFn = void* (*)(void* text, const FStr* str);
using FNameFn = NameId* (*)(NameId* out, const wchar_t* name, int findType);
using MallocFn = void* (*)(size_t count, uint32_t alignment);

constexpr int kToggleTemplate = 15, kSliderTemplate = 10;  // 0-based rows of General: Screen Space Reflections (PC), Brightness
constexpr int32_t kAllPlatforms = 255;
constexpr size_t kMaxStride = 0x400;
constexpr int kHdrRows = 5;
// EModernSettingWidgetSubType (UE4SS, build 19115871): the settings widgets lock rows by it. HDROptions rows are disabled
// while HDR is off, SDROptions rows while it is on, and re-enabled live when it changes (seen in probe games). Checked
// through two of General's own rows: Display Resolution (Resolution) and V-Sync (VSync).
constexpr uint8_t kSubNone = 0, kSubResolution = 1, kSubHdrOptions = 6, kSubSdrOptions = 8, kSubVSync = 17;
constexpr int kResolutionRow = 2, kVSyncRow = 5;  // 0-based rows of General

struct Names {
    // property classes
    NameId arrayProp, structProp, textProp, strProp, intProp, floatProp, enumProp, boolProp, objectProp;
    NameId byteProp, doubleProp, nameProp, classProp, int64Prop, uint32Prop, int8Prop, int16Prop, uint16Prop, uint64Prop;
    NameId weakObjectProp, softObjectProp, softClassProp;
    // objects
    NameId tableClass, tableName, catClass, catName, gusClass, upName;
    // fields
    NameId settings, categories, name, description, descImage, descObject, type, subType, platformFlag, stepProps, sliderProps;
    NameId index, defaultIndex, steps, command, consoleCommand, label;
    NameId value, defaultValue, cvar, maxFrac, visFrac, scroll, minValue, maxValue, stepSize, jumpSize;
    NameId hdrOn, hdrNits;
};

struct Layout {  // byte offsets found by name
    const uint8_t* rowStruct = nullptr;
    uint32_t settings = 0, stride = 0;
    uint32_t name = 0, description = 0, descImage = 0, descObject = 0, type = 0, subType = 0, platformFlag = 0, steps = 0, slider = 0;
    uint32_t stepIndex = 0, stepDefault = 0, stepArray = 0, cmdStride = 0, cmdCommand = 0;
    const uint8_t* stepStruct = nullptr;  // a step's struct (ModernSettingSingleCommand)
    uint32_t stepLabel = UINT32_MAX;      // its Label text (UINT32_MAX: none)
    uint32_t value = 0, defaultValue = 0, cvar = 0, maxFrac = 0, visFrac = 0, scroll = 0;
    uint32_t scMin = 0, scMax = 0, scStep = 0, scJump = 0;
    uint32_t hdrOn = 0, hdrNits = 0;
    uint8_t hdrOnMask = 0;
    // the category table
    const uint8_t* catStruct = nullptr;
    uint32_t categories = 0, catStride = 0, catName = 0, catSettings = 0;
};

struct State {
    bool found = false, installed = false, failed = false;
    bool hdrWanted = false, rrWanted = false;  // the HDR section, the Ray Reconstruction row
    bool hudWanted = false, hudFailed = false; // the HUD aspect row
    uint8_t* hudPatched = nullptr;             // the General table last given the HUD row
    int32_t hudPatchedIndex = -1;
    int hudPatches = 0;
    Layout hudLayout;
    bool rrFailed = false;
    FromStringFn fromString = nullptr;
    FNameFn fname = nullptr;
    MallocFn malloc = nullptr;
    SetFn set = nullptr;
    AddOrUpdateFn addOrUpdate = nullptr;
    SaveWriterFn saveWriter = nullptr;
    SpawnFn spawn = nullptr;
    const FStr* saveSlot = nullptr;  // the writer's slot name FString ("Save_Settings" once the statics have run)
    uint8_t* fromStringAt = nullptr;
    uint8_t* fnameAt = nullptr;
    uint8_t* mallocAt = nullptr;
    uint8_t* freeAt = nullptr;
    uint8_t* gmalloc = nullptr;
    uint8_t* objectArray = nullptr;
    bool namesReady = false;
    bool firstPass = false;       // the first walk after the names exist: whole, in one frame
    Names n;
    uint32_t cursor = 0;          // the incremental walk's next GUObjectArray index
    uint8_t* table = nullptr;     // found in the current walk: General
    uint8_t* catTable = nullptr;  // ... the Graphics category table
    uint8_t* gus = nullptr;
    uint8_t* patched = nullptr;   // the category table last given the HDR section
    int32_t patchedIndex = -1;
    uint8_t* hdrTable = nullptr;  // the HDR section's table
    uint8_t* upTable = nullptr;   // found in the current walk: the upscaler (Advanced) table
    uint8_t* rrPatched = nullptr; // the upscaler table last given the Ray Reconstruction row
    int32_t rrPatchedIndex = -1;
    int rrPatches = 0;
    Layout rrLayout;              // the row layout found when the row was made
    uint32_t frames = 0;
    int patches = 0;
    std::string save;             // Save_Settings.sav as it was soon after the DLL loaded (before the game writes it back)
    std::atomic<bool> saveRead{false};  // set by the monitor thread once `save` is filled (read by the game thread after)
    uint32_t syncAt = 0;          // the frame of SyncCvars' second step (0 = none)
    int32_t syncNits = 0;         // ... the WhitePoint it sets (0 = none)
    bool syncOff = false;         // ... and whether it sets HDR 0
} s;

bool Fail(const char* why)
{
    Log("[hdrmenu] %s: HdrMenuRows not installed", why);
    return false;
}

void Disable(const char* why)
{
    Log("[hdrmenu] %s: HdrMenuRows disabled, nothing written", why);
    s.failed = true;
}

template <class T> T At(const uint8_t* p, size_t off) { T v; memcpy(&v, p + off, sizeof v); return v; }
uint8_t* Ptr(const uint8_t* p, size_t off) { return At<uint8_t*>(p, off); }

NameId MakeName(const wchar_t* text)
{
    NameId id;
    s.fname(&id, text, 0);  // FNAME_Find: a name not in the pool yet gives None
    return id;
}

bool MakeNames()
{
    Names& n = s.n;
    struct { NameId* id; const wchar_t* text; } all[] = {
        {&n.arrayProp, L"ArrayProperty"}, {&n.structProp, L"StructProperty"}, {&n.textProp, L"TextProperty"},
        {&n.strProp, L"StrProperty"}, {&n.intProp, L"IntProperty"}, {&n.floatProp, L"FloatProperty"},
        {&n.enumProp, L"EnumProperty"}, {&n.boolProp, L"BoolProperty"}, {&n.objectProp, L"ObjectProperty"},
        {&n.byteProp, L"ByteProperty"}, {&n.doubleProp, L"DoubleProperty"}, {&n.nameProp, L"NameProperty"},
        {&n.classProp, L"ClassProperty"}, {&n.int64Prop, L"Int64Property"}, {&n.uint32Prop, L"UInt32Property"},
        {&n.int8Prop, L"Int8Property"}, {&n.int16Prop, L"Int16Property"}, {&n.uint16Prop, L"UInt16Property"},
        {&n.uint64Prop, L"UInt64Property"}, {&n.weakObjectProp, L"WeakObjectProperty"},
        {&n.softObjectProp, L"SoftObjectProperty"}, {&n.softClassProp, L"SoftClassProperty"},
        {&n.tableClass, L"VModernSettingTable"}, {&n.tableName, L"DT_Modern_Settings_GraphicsGeneral"},
        {&n.catClass, L"VModernSettingCategoryTable"}, {&n.catName, L"DT_Modern_Setting_GraphicsTable"},
        {&n.gusClass, L"AltarGameUserSetting"}, {&n.upName, L"DT_Modern_Settings_GraphicsUpscaler"},
        {&n.settings, L"Settings"}, {&n.categories, L"Categories"}, {&n.name, L"Name"}, {&n.description, L"Description"},
        {&n.descImage, L"DescriptionImage"}, {&n.descObject, L"DescriptionObject"},
        {&n.type, L"Type"}, {&n.subType, L"SubType"}, {&n.platformFlag, L"PlatformFlag"}, {&n.stepProps, L"StepProperties"},
        {&n.sliderProps, L"SliderProperties"}, {&n.index, L"Index"}, {&n.defaultIndex, L"DefaultIndex"}, {&n.steps, L"Steps"},
        {&n.command, L"Command"}, {&n.consoleCommand, L"ConsoleCommand"}, {&n.label, L"Label"}, {&n.value, L"Value"},
        {&n.defaultValue, L"DefaultValue"}, {&n.cvar, L"CVAR"}, {&n.maxFrac, L"MaximumFractionalDigits"},
        {&n.visFrac, L"VisualMaximumLabelFractionalDigits"}, {&n.scroll, L"ScrollbarValueProperties"},
        {&n.minValue, L"MinValue"}, {&n.maxValue, L"MaxValue"}, {&n.stepSize, L"StepSize"}, {&n.jumpSize, L"JumpSize"},
        {&n.hdrOn, L"bUseHDRDisplayOutput"}, {&n.hdrNits, L"HDRDisplayOutputNits"},
    };
    for (auto& e : all) {
        *e.id = MakeName(e.text);
        if (!e.id->valid()) return false;  // not in the name pool yet (its asset not loaded): try again later
    }
    return true;
}

NameId ObjName(const uint8_t* obj) { return At<NameId>(obj, kObjName); }
NameId ClassName(const uint8_t* obj)
{
    const uint8_t* const cls = Ptr(obj, kObjClass);
    return cls ? ObjName(cls) : NameId{};
}
NameId FieldClass(const uint8_t* prop)
{
    const uint8_t* const fc = Ptr(prop, kFieldClass);
    return fc ? At<NameId>(fc, 0) : NameId{};
}

// The property `name` of type `type` declared by `st` (or, with `super`, one of its parents); its expected size.
const uint8_t* FindProp(const uint8_t* st, const NameId& name, const NameId& type, uint32_t size, bool super = false)
{
    for (int depth = 0; st && depth < 16; ++depth, st = super ? Ptr(st, kStructSuper) : nullptr) {
        for (const uint8_t* f = Ptr(st, kStructChildProps); f; f = Ptr(f, kFieldNext)) {
            if (!(At<NameId>(f, kFieldName) == name)) continue;
            if (!(FieldClass(f) == type)) return nullptr;
            if (size && At<int32_t>(f, kPropElementSize) != static_cast<int32_t>(size)) return nullptr;
            return f;
        }
    }
    return nullptr;
}

uint32_t Off(const uint8_t* prop) { return static_cast<uint32_t>(At<int32_t>(prop, kPropOffset)); }
uint32_t Size(const uint8_t* prop) { return static_cast<uint32_t>(At<int32_t>(prop, kPropElementSize)); }
const uint8_t* InnerStruct(const uint8_t* structProp) { return Ptr(structProp, kStructPropStruct); }

// --- copies that own what they hold -------------------------------------------------------------------------------

bool OwnStruct(const uint8_t* st, uint8_t* dst);

// A string's characters copied into a new buffer (an empty string keeps no buffer).
bool OwnString(uint8_t* field)
{
    FStr str;
    memcpy(&str, field, sizeof str);
    if (str.num <= 0 || !str.data) {
        str = FStr{nullptr, 0, 0};
    } else {
        auto* const copy = static_cast<wchar_t*>(s.malloc(static_cast<size_t>(str.num) * sizeof(wchar_t), 0));
        if (!copy) return false;
        memcpy(copy, str.data, static_cast<size_t>(str.num) * sizeof(wchar_t));
        str.data = copy;
        str.max = str.num;
    }
    memcpy(field, &str, sizeof str);
    return true;
}

// The value at `dst`, a byte copy of another value of property `prop`, made its own: texts counted once more
// (TSharedRef: the reference count at controller+8), strings and arrays copied, structs walked. False for a property
// type this does not know; the caller then writes nothing more.
bool OwnValue(const uint8_t* prop, uint8_t* dst)
{
    const Names& n = s.n;
    const NameId c = FieldClass(prop);
    if (c == n.intProp || c == n.floatProp || c == n.boolProp || c == n.byteProp || c == n.enumProp || c == n.nameProp ||
        c == n.objectProp || c == n.classProp || c == n.doubleProp || c == n.int64Prop || c == n.uint32Prop || c == n.int8Prop ||
        c == n.int16Prop || c == n.uint16Prop || c == n.uint64Prop || c == n.weakObjectProp)
        return true;  // plain bytes
    if (c == n.textProp) {
        if (Size(prop) != 0x18) return false;
        uint8_t* const controller = Ptr(dst, 8);
        if (controller) _InterlockedIncrement(reinterpret_cast<volatile long*>(controller + 8));
        return true;
    }
    if (c == n.strProp) return Size(prop) == 0x10 && OwnString(dst);
    if (c == n.softObjectProp || c == n.softClassProp) return Size(prop) == kSoftObjectSize && OwnString(dst + kSoftSubPath);
    if (c == n.structProp) return OwnStruct(InnerStruct(prop), dst);
    if (c == n.arrayProp) {
        const uint8_t* const inner = Ptr(prop, kArrayPropInner);
        if (!inner || Size(prop) != 0x10) return false;
        const uint32_t esz = Size(inner);
        TArr a;
        memcpy(&a, dst, sizeof a);
        if (a.num <= 0 || !a.data) {
            a = TArr{nullptr, 0, 0};
        } else {
            auto* const copy = static_cast<uint8_t*>(s.malloc(static_cast<size_t>(a.num) * esz, 0));
            if (!copy) return false;
            memcpy(copy, a.data, static_cast<size_t>(a.num) * esz);
            for (int32_t i = 0; i < a.num; ++i)
                if (!OwnValue(inner, copy + static_cast<size_t>(i) * esz)) return false;
            a.data = copy;
            a.max = a.num;
        }
        memcpy(dst, &a, sizeof a);
        return true;
    }
    return false;
}

// Every property of struct `st` (its parents too) at `dst` made its own.
bool OwnStruct(const uint8_t* st, uint8_t* dst)
{
    for (int depth = 0; st && depth < 16; ++depth, st = Ptr(st, kStructSuper)) {
        for (const uint8_t* f = Ptr(st, kStructChildProps); f; f = Ptr(f, kFieldNext)) {
            const int32_t dim = At<int32_t>(f, kPropArrayDim);
            if (dim < 1 || dim > 64) return false;
            for (int32_t i = 0; i < dim; ++i)
                if (!OwnValue(f, dst + Off(f) + static_cast<size_t>(i) * Size(f))) {
                    Log("[hdrmenu] a copied row has a property this cannot copy (%u bytes at +0x%X)", Size(f), Off(f));
                    return false;
                }
        }
    }
    return true;
}

bool CopyStruct(const uint8_t* st, uint32_t size, uint8_t* dst, const uint8_t* src)
{
    memcpy(dst, src, size);
    return OwnStruct(st, dst);
}

// --- layouts -------------------------------------------------------------------------------------------------------

// Every offset the rows and the section need, by name; false (logged) if anything is missing or differs.
bool ResolveLayout(const uint8_t* table, const uint8_t* catTable, const uint8_t* gus, Layout& l)
{
    const Names& n = s.n;
    const uint8_t* const tableClass = Ptr(table, kObjClass);
    const uint8_t* const settings = FindProp(tableClass, n.settings, n.arrayProp, 0x10, true);
    if (!settings) return Log("[hdrmenu] the table class has no Settings array"), false;
    const uint8_t* const inner = Ptr(settings, kArrayPropInner);
    if (!inner || !(FieldClass(inner) == n.structProp)) return Log("[hdrmenu] Settings is not an array of structs"), false;
    l.rowStruct = InnerStruct(inner);
    l.settings = Off(settings);
    l.stride = Size(inner);
    if (!l.rowStruct || l.stride < 0x40 || l.stride > kMaxStride) return Log("[hdrmenu] the row struct's size %u is unexpected", l.stride), false;
    const uint8_t* const row = l.rowStruct;
    const uint8_t *pName = FindProp(row, n.name, n.textProp, 0x18), *pDesc = FindProp(row, n.description, n.textProp, 0x18),
                  *pType = FindProp(row, n.type, n.enumProp, 1), *pFlag = FindProp(row, n.platformFlag, n.intProp, 4),
                  *pSteps = FindProp(row, n.stepProps, n.structProp, 0), *pSlider = FindProp(row, n.sliderProps, n.structProp, 0);
    const uint8_t *pImage = FindProp(row, n.descImage, n.softObjectProp, kSoftObjectSize), *pObject = FindProp(row, n.descObject, n.classProp, 8);
    const uint8_t* const pSubType = FindProp(row, n.subType, n.enumProp, 1);
    if (!pName || !pDesc || !pType || !pFlag || !pSteps || !pSlider || !pImage || !pObject || !pSubType)
        return Log("[hdrmenu] a row field is missing or of another type"), false;
    l.name = Off(pName); l.description = Off(pDesc); l.type = Off(pType); l.subType = Off(pSubType); l.platformFlag = Off(pFlag);
    l.descImage = Off(pImage); l.descObject = Off(pObject);
    l.steps = Off(pSteps); l.slider = Off(pSlider);
    const uint8_t* const steps = InnerStruct(pSteps);
    const uint8_t *pIndex = FindProp(steps, n.index, n.intProp, 4), *pDefIndex = FindProp(steps, n.defaultIndex, n.intProp, 4),
                  *pArray = FindProp(steps, n.steps, n.arrayProp, 0x10);
    if (!pIndex || !pDefIndex || !pArray) return Log("[hdrmenu] a step field is missing or of another type"), false;
    l.stepIndex = Off(pIndex); l.stepDefault = Off(pDefIndex); l.stepArray = Off(pArray);
    const uint8_t* const cmdInner = Ptr(pArray, kArrayPropInner);
    if (!cmdInner || !(FieldClass(cmdInner) == n.structProp)) return Log("[hdrmenu] Steps is not an array of structs"), false;
    l.cmdStride = Size(cmdInner);
    const uint8_t* const pCommand = FindProp(InnerStruct(cmdInner), n.command, n.structProp, 0);
    const uint8_t* const pConsole = pCommand ? FindProp(InnerStruct(pCommand), n.consoleCommand, n.strProp, 0x10) : nullptr;
    if (!pConsole || l.cmdStride < 0x20 || l.cmdStride > 0x100) return Log("[hdrmenu] a step's console command is missing"), false;
    l.cmdCommand = Off(pCommand) + Off(pConsole);
    l.stepStruct = InnerStruct(cmdInner);
    if (const uint8_t* const pLabel = FindProp(l.stepStruct, n.label, n.textProp, 0x18))
        if (Off(pLabel) + 0x18 <= l.cmdStride) l.stepLabel = Off(pLabel);
    const uint8_t* const slider = InnerStruct(pSlider);
    const uint8_t *pValue = FindProp(slider, n.value, n.floatProp, 4), *pDefault = FindProp(slider, n.defaultValue, n.floatProp, 4),
                  *pCvar = FindProp(slider, n.cvar, n.strProp, 0x10), *pFrac = FindProp(slider, n.maxFrac, n.intProp, 4),
                  *pVis = FindProp(slider, n.visFrac, n.intProp, 4), *pScroll = FindProp(slider, n.scroll, n.structProp, 0);
    if (!pValue || !pDefault || !pCvar || !pFrac || !pVis || !pScroll) return Log("[hdrmenu] a slider field is missing or of another type"), false;
    l.value = Off(pValue); l.defaultValue = Off(pDefault); l.cvar = Off(pCvar); l.maxFrac = Off(pFrac); l.visFrac = Off(pVis);
    l.scroll = Off(pScroll);
    const uint8_t* const scroll = InnerStruct(pScroll);
    const uint8_t *pMin = FindProp(scroll, n.minValue, n.floatProp, 4), *pMax = FindProp(scroll, n.maxValue, n.floatProp, 4),
                  *pStep = FindProp(scroll, n.stepSize, n.floatProp, 4), *pJump = FindProp(scroll, n.jumpSize, n.floatProp, 4);
    if (!pMin || !pMax || !pStep || !pJump) return Log("[hdrmenu] a scroll bar field is missing or of another type"), false;
    l.scMin = Off(pMin); l.scMax = Off(pMax); l.scStep = Off(pStep); l.scJump = Off(pJump);
    // every field inside its row / struct
    const uint32_t ends[] = {l.name + 0x18, l.description + 0x18, l.descImage + kSoftObjectSize, l.descObject + 8, l.type + 1,
                             l.subType + 1, l.platformFlag + 4, l.steps + l.stepArray + 0x10,
                             l.slider + l.scroll + l.scJump + 4, l.slider + l.cvar + 0x10};
    for (uint32_t e : ends)
        if (e > l.stride) return Log("[hdrmenu] a row field lies outside the row (%u > %u)", e, l.stride), false;
    if (l.cmdCommand + 0x10 > l.cmdStride) return Log("[hdrmenu] the console command lies outside its step"), false;
    // the category table: Categories, an array of {Name text, Settings -> table}
    const uint8_t* const cats = FindProp(Ptr(catTable, kObjClass), n.categories, n.arrayProp, 0x10, true);
    const uint8_t* const catInner = cats ? Ptr(cats, kArrayPropInner) : nullptr;
    if (!catInner || !(FieldClass(catInner) == n.structProp)) return Log("[hdrmenu] the category table has no Categories array of structs"), false;
    l.catStruct = InnerStruct(catInner);
    l.categories = Off(cats);
    l.catStride = Size(catInner);
    const uint8_t* const pCatName = FindProp(l.catStruct, n.name, n.textProp, 0x18);
    const uint8_t* const pCatSettings = FindProp(l.catStruct, n.settings, n.objectProp, 8);
    if (!pCatName || !pCatSettings || l.catStride < 0x20 || l.catStride > 0x100) return Log("[hdrmenu] a category field is missing or of another type"), false;
    l.catName = Off(pCatName);
    l.catSettings = Off(pCatSettings);
    if (l.catName + 0x18 > l.catStride || l.catSettings + 8 > l.catStride) return Log("[hdrmenu] a category field lies outside its row"), false;
    // the settings object's HDR switch and nits (UGameUserSettings)
    const uint8_t* const gusClass = Ptr(gus, kObjClass);
    const uint8_t* const pOn = FindProp(gusClass, n.hdrOn, n.boolProp, 0, true);
    const uint8_t* const pNits = FindProp(gusClass, n.hdrNits, n.intProp, 4, true);
    if (!pOn || !pNits) return Log("[hdrmenu] the settings object's HDR fields are missing"), false;
    l.hdrOn = Off(pOn) + At<uint8_t>(pOn, kBoolByteOffset);
    l.hdrOnMask = At<uint8_t>(pOn, kBoolFieldMask);
    l.hdrNits = Off(pNits);
    if (!l.hdrOnMask) return Log("[hdrmenu] the HDR switch's bit mask is 0"), false;
    return true;
}

// --- writing rows --------------------------------------------------------------------------------------------------

void SetText(uint8_t* field, const wchar_t* text)
{
    const int32_t len = static_cast<int32_t>(wcslen(text)) + 1;
    const FStr str{const_cast<wchar_t*>(text), len, len};
    alignas(8) uint8_t made[0x18] = {};
    s.fromString(made, &str);   // the engine's copy; ours stays ours
    memcpy(field, made, sizeof made);  // the old text is left as it was (not released)
}

// The string's characters replaced: in place when they fit, else in a new FMemory::Malloc buffer (the old one kept).
wchar_t* SetString(uint8_t* field, const wchar_t* text)
{
    FStr str;
    memcpy(&str, field, sizeof str);
    const int32_t len = static_cast<int32_t>(wcslen(text)) + 1;
    if (!str.data || str.max < len) {
        str.data = static_cast<wchar_t*>(s.malloc(static_cast<size_t>(len) * sizeof(wchar_t), 0));
        if (!str.data) return nullptr;
        str.max = len;
    }
    memcpy(str.data, text, static_cast<size_t>(len) * sizeof(wchar_t));
    str.num = len;
    memcpy(field, &str, sizeof str);
    return str.data;
}

bool StrIs(const uint8_t* field, const wchar_t* text)
{
    FStr str;
    memcpy(&str, field, sizeof str);
    return str.data && str.num == static_cast<int32_t>(wcslen(text)) + 1 && wcscmp(str.data, text) == 0;
}

struct SliderSpec {
    const wchar_t* name;
    const wchar_t* description;
    const wchar_t* cvar;
    float value, def, mn, mx, step, jump;  // value: shown (the current state); def: the row's default
    int32_t fractionDigits;
    bool calibrationPicture;  // keep the template's (Brightness's) picture of the faint symbol beside the description
};

void SetSlider(uint8_t* row, const Layout& l, const SliderSpec& v)
{
    SetText(row + l.name, v.name);
    SetText(row + l.description, v.description);
    uint8_t* const sl = row + l.slider;
    SetString(sl + l.cvar, v.cvar);
    memcpy(sl + l.value, &v.value, 4);
    memcpy(sl + l.defaultValue, &v.def, 4);
    memcpy(sl + l.maxFrac, &v.fractionDigits, 4);
    memcpy(sl + l.visFrac, &v.fractionDigits, 4);
    uint8_t* const sc = sl + l.scroll;
    memcpy(sc + l.scMin, &v.mn, 4);
    memcpy(sc + l.scMax, &v.mx, 4);
    memcpy(sc + l.scStep, &v.step, 4);
    memcpy(sc + l.scJump, &v.jump, 4);
    // no step commands, as the game's own sliders: a slider row with steps runs a step instead of its console variable
    // (seen live). The template (a game slider) has none; Num 0 makes sure.
    TArr steps;
    memcpy(&steps, row + l.steps + l.stepArray, sizeof steps);
    steps.num = 0;
    memcpy(row + l.steps + l.stepArray, &steps, sizeof steps);
    const int32_t zero = 0;
    memcpy(row + l.steps + l.stepIndex, &zero, 4);
    memcpy(row + l.steps + l.stepDefault, &zero, 4);
    row[l.type] = 2;  // EModernSettingWidgetType: slider
    memcpy(row + l.platformFlag, &kAllPlatforms, 4);
    if (!v.calibrationPicture) {  // no picture beside the description (the copied strings stay allocated)
        memset(row + l.descImage, 0, kSoftObjectSize);
        memset(row + l.descObject, 0, 8);
    }
}

// --- engine functions found by name ---------------------------------------------------------------------------------

// An exec function by its registered name. The engine's native function tables are {const char* name, function} pairs:
// the name's one copy in .rdata (a whole string), the pairs that point to it and to code (.rdata or .data, 8-byte
// aligned; the function's reflection parameters also point to the name and then to code), and of their functions the
// one that matches `shape`. nullptr unless each is exactly one.
uint8_t* FindNativeByName(const char* name, const char* shape)
{
    const Pattern pattern = ParsePattern(shape);
    const GameModule& game = Game();
    const size_t len = strlen(name) + 1;
    const uint8_t* str = nullptr;
    int copies = 0;
    for (const uint8_t* p = game.rdata.begin + 1; p + len <= game.rdata.end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, name[0], static_cast<size_t>(game.rdata.end - p)));
        if (!p || p + len > game.rdata.end) break;
        if (p[-1] == 0 && memcmp(p, name, len) == 0) {
            str = p;
            ++copies;
        }
    }
    if (copies != 1) return nullptr;
    uint8_t* fn = nullptr;
    int pairs = 0;
    for (const Section* sec : {&game.rdata, &game.data}) {
        for (const uint8_t* q = sec->begin; q + 16 <= sec->end; q += 8) {
            if (At<const uint8_t*>(q, 0) != str || !game.text.Contains(Ptr(q, 8)) || !MatchesAt(Ptr(q, 8), pattern)) continue;
            fn = Ptr(q, 8);
            ++pairs;
        }
    }
    return pairs == 1 ? fn : nullptr;
}

// SpawnObject's body names NewObject in its error message (a lea rdx,[message] within its first bytes).
bool MentionsNewObject(const uint8_t* fn)
{
    const GameModule& game = Game();
    for (const uint8_t* p = fn; p + 7 <= fn + kSpawnObjectScan; ++p) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;
        const auto* const str = reinterpret_cast<const wchar_t*>(RipTarget(p, 3, 7));
        if ((game.rdata.Contains(str) || game.data.Contains(str)) &&
            wcsncmp(str, kNewObjectMessage, wcslen(kNewObjectMessage)) == 0)
            return true;
    }
    return false;
}

// --- saved options -------------------------------------------------------------------------------------------------

bool Saved(const std::string& file, const char* name, double* out);

// The HDR options of the menu's saved options (see ReadSettingsSave) put back into the settings object's SavedSettings
// with the game's own AddOrUpdateSettingsToSave, then the settings save written once with the game's own writer. The
// game keeps, and writes back at launch, only the options of rows that exist when it loads the file; the rows are made
// later, so without this the HDR options the player applied would be gone after one more launch. The file then holds
// what the game itself writes when the player applies settings with these rows on the page. Nothing happens when the
// file had none of them.
constexpr const char* kSavedKeys[] = {"Altar.GraphicsOptions.HDR", "Altar.GraphicsOptions.HDRWhitePoint", "r.HDR.UI.Luminance",
                                      "r.HDR.Display.MinLuminanceLog10", "r.HDR.Display.MidLuminance"};
constexpr const char* kRrKey = "r.NGX.DLSS.DenoiserMode";

void KeepSavedOptions(uint8_t* gus, const char* const* keys, size_t count, const char* what)
{
    if (!s.saveRead.load(std::memory_order_acquire) || s.save.empty() || !s.addOrUpdate || !s.saveWriter) return;
    // the slot name as the writer will use it (initialised by the exe's static constructors long before this)
    const FStr* const slot = s.saveSlot;
    if (!slot->data || slot->num != 14 || wcscmp(slot->data, L"Save_Settings") != 0) {
        Log("[hdrmenu] the settings save's slot name is not \"Save_Settings\": saved %s options not kept", what);
        return;
    }
    int kept = 0;
    wchar_t line[96];
    for (size_t k = 0; k < count; ++k) {
        const char* const key = keys[k];
        double d = 0;
        if (!Saved(s.save, key, &d)) continue;
        const int n = swprintf(line, 96, L"%hs %g", key, d);
        if (n <= 0) continue;
        const FStr cmd{line, n + 1, n + 1};
        s.addOrUpdate(gus, &cmd);
        ++kept;
    }
    if (!kept) return;
    s.saveWriter(gus);
    Log("[hdrmenu] %d saved %s option(s) put back into the saved options and the settings save written (the game drops "
        "options of rows it does not have yet when it loads the file)", kept, what);
}

// "<name> <value>" through Altar's command setter, as the menu's rows and the saved settings run them (the sinks follow)
void RunSetter(const wchar_t* name, const wchar_t* value)
{
    const int32_t n = static_cast<int32_t>(wcslen(name)) + 1, v = static_cast<int32_t>(wcslen(value)) + 1;
    const CmdPair pair{{const_cast<wchar_t*>(name), n, n}, {const_cast<wchar_t*>(value), v, v}};
    s.set(&pair, false);
}

// At launch the game sets HDR from its settings object (GameUserSettings.ini) but leaves Altar.GraphicsOptions.HDR at
// 0 and Altar.GraphicsOptions.HDRWhitePoint at 1000 when the saved options have no entry: the tonemapper's MaxLuminance
// then stays 1000 whatever nits are saved (two probe games, captures/probe/20261027-hdr-semantics.txt), the row's first
// Off would change nothing (0 -> 0) and On would use 1000 nits. Both variables are set to the state the rows show, in
// the order measured live: when HDR is on, HDR 1 (the switch, with the current WhitePoint) and then the nits
// (MaxLuminance and the object's nits follow); when it is off, the nits (stored, nothing switched on), and if the
// output is on anyway (the object says on, the saved options off), HDR 1 and later 0 (0 alone changes nothing).
// The setter does nothing when a variable already holds the value, and two commands for one variable in the same frame
// leave only the last one in effect (seen live). With saved options the game's launch has already set both variables,
// and then the ini's nits switched the output on again (seen: WhitePoint 1000, MaxLuminance 1000, but the object's
// nits 1300 and the 2000-nit output device). So when the object's nits differ from the shown nits, the WhitePoint goes
// to a neighbouring value now and to the shown nits kSyncFrames frames later (FinishSync), as does the HDR 0 above.
constexpr uint32_t kSyncFrames = 10;

void SyncCvars(bool on, int32_t nits, bool outputOn, int32_t objectNits)
{
    wchar_t v[16];
    const bool nudge = objectNits != nits, offAgain = !on && outputOn;
    swprintf(v, 16, L"%d", nudge ? (nits < 4000 ? nits + 50 : nits - 50) : nits);
    if (on || offAgain) RunSetter(L"Altar.GraphicsOptions.HDR", L"1");
    RunSetter(L"Altar.GraphicsOptions.HDRWhitePoint", v);
    if (nudge || offAgain) {
        s.syncNits = nudge ? nits : 0;
        s.syncOff = offAgain;
        s.syncAt = s.frames + kSyncFrames;
    }
}

// The second step of SyncCvars, kSyncFrames frames later (game thread).
void FinishSync()
{
    wchar_t v[16];
    if (s.syncNits) {
        swprintf(v, 16, L"%d", s.syncNits);
        RunSetter(L"Altar.GraphicsOptions.HDRWhitePoint", v);
    }
    if (s.syncOff) RunSetter(L"Altar.GraphicsOptions.HDR", L"0");
    s.syncAt = 0;
    s.syncNits = 0;
    s.syncOff = false;
}

// The menu's saved options: Documents\My Games\Oblivion Remastered\Saved\SaveGames\Save_Settings.sav, the file the game
// loads and applies at every launch. In memory the game keeps only the options of rows that exist when it loads the
// file (seen live: the HDR keys were in the file and applied at launch, but not in the settings object's map once this
// made its rows; it also writes the file back at launch without them), so the file is read (only read) on the monitor
// thread's first tick, about 20 s before the game writes it. Its options are FString pairs, each an int32 length
// counting the terminator and then the characters (ASCII here): the console variable's name, then its value.
std::string ReadSettingsSave()
{
    using GetFolderFn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*);
    using FreeFn = void(WINAPI*)(void*);
    static const GUID kDocuments = {0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};  // FOLDERID_Documents
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    const auto getFolder = shell ? reinterpret_cast<GetFolderFn>(GetProcAddress(shell, "SHGetKnownFolderPath")) : nullptr;
    const auto taskFree = ole ? reinterpret_cast<FreeFn>(GetProcAddress(ole, "CoTaskMemFree")) : nullptr;
    std::wstring path;
    PWSTR docs = nullptr;
    if (getFolder && taskFree && SUCCEEDED(getFolder(kDocuments, 0, nullptr, &docs)) && docs)
        path = std::wstring(docs) + L"\\My Games\\Oblivion Remastered\\Saved\\SaveGames\\Save_Settings.sav";
    if (docs && taskFree) taskFree(docs);
    std::string data;
    if (path.empty()) return data;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (4 << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(f);
    return data;
}

// The saved option `name` as a number; false if the file has none or it is not a number.
bool Saved(const std::string& file, const char* name, double* out)
{
    const int32_t len = static_cast<int32_t>(strlen(name)) + 1;
    std::string key(reinterpret_cast<const char*>(&len), 4);
    key.append(name, static_cast<size_t>(len));  // with its terminator: "...HDR" does not match "...HDRWhitePoint"
    const size_t at = file.find(key);
    if (at == std::string::npos || at + key.size() + 4 > file.size()) return false;
    int32_t n = 0;
    memcpy(&n, file.data() + at + key.size(), 4);
    if (n < 2 || n > 32 || at + key.size() + 4 + static_cast<size_t>(n) > file.size()) return false;
    const std::string value(file.data() + at + key.size() + 4, static_cast<size_t>(n - 1));
    char* end = nullptr;
    const double d = strtod(value.c_str(), &end);
    if (end == value.c_str() || *end) return false;
    *out = d;
    return true;
}

// --- the section ---------------------------------------------------------------------------------------------------

const Item* ItemOf(int32_t index)
{
    if (index < 0 || index >= At<int32_t>(s.objectArray, kArrayNum)) return nullptr;
    Item** const chunks = reinterpret_cast<Item**>(Ptr(s.objectArray, kArrayChunks));
    Item* const chunk = chunks ? chunks[static_cast<uint32_t>(index) / kChunkItems] : nullptr;
    return chunk ? chunk + static_cast<uint32_t>(index) % kChunkItems : nullptr;
}

// The category table's rows; the index of the one pointing to `table` (-1 if none).
int FindCategory(const uint8_t* catTable, const Layout& l, const uint8_t* table, TArr* rows)
{
    memcpy(rows, catTable + l.categories, sizeof *rows);
    if (!rows->data || rows->num <= 0 || rows->num > 64) return -1;
    for (int32_t i = 0; i < rows->num; ++i)
        if (Ptr(rows->data + static_cast<size_t>(i) * l.catStride, l.catSettings) == table) return i;
    return -1;
}

void Patch(uint8_t* table, uint8_t* catTable, uint8_t* gus)
{
    Layout l;
    if (!ResolveLayout(table, catTable, gus, l)) return Disable("the settings menu's data differs from build 19115871");
    TArr cats;
    const int general = FindCategory(catTable, l, table, &cats);
    if (general < 0) return Disable("the Graphics categories do not contain the General table");
    if (s.hdrTable && FindCategory(catTable, l, s.hdrTable, &cats) >= 0) {  // already there (found again by a new walk)
        s.patched = catTable;
        s.patchedIndex = At<int32_t>(catTable, kObjIndex);
        return;
    }
    // the new table must stay alive through the category row's reference: the category table must be an object the
    // garbage collector scans (above the never-collected pool), not one it only keeps
    const int32_t catIndex = At<int32_t>(catTable, kObjIndex), lastNonGc = At<int32_t>(s.objectArray, kArrayLastNonGc);
    if (catIndex <= lastNonGc) {
        Log("[hdrmenu] the Graphics category table (object %d) is in the never-collected pool (up to %d)", catIndex, lastNonGc);
        return Disable("a new HDR table could not be kept alive by it");
    }
    TArr rows;
    memcpy(&rows, table + l.settings, sizeof rows);
    if (!rows.data || rows.num <= kToggleTemplate || rows.num > 64) return Disable("the General table has fewer rows than expected");
    // the templates by their commands (General's rows 16 and 11 in build 19115871; the HUD aspect row, when made first,
    // moves them down by one)
    static const wchar_t* const kSsrCmds[2] = {L"Altar.GraphicsOptions.ScreenSpaceReflection 0", L"Altar.GraphicsOptions.ScreenSpaceReflection 1"};
    int toggleAt = kToggleTemplate, sliderAt = kSliderTemplate;
    for (int32_t i = 0; i < rows.num; ++i) {
        const uint8_t* const r = rows.data + static_cast<size_t>(i) * l.stride;
        if (r[l.type] == 2 && (At<int32_t>(r, l.platformFlag) & 1) && StrIs(r + l.slider + l.cvar, L"Altar.GraphicsOptions.Brightness")) sliderAt = i;
    }
    for (int32_t i = 0; i < rows.num; ++i) {
        const uint8_t* const r = rows.data + static_cast<size_t>(i) * l.stride;
        TArr st;
        memcpy(&st, r + l.steps + l.stepArray, sizeof st);
        if (r[l.type] == 1 && (At<int32_t>(r, l.platformFlag) & 1) && st.num == 2 && st.data && StrIs(st.data + l.cmdCommand, kSsrCmds[0]) &&
            StrIs(st.data + l.cmdStride + l.cmdCommand, kSsrCmds[1]))
            toggleAt = i;
    }
    const uint8_t* const toggleTpl = rows.data + static_cast<size_t>(toggleAt) * l.stride;
    uint8_t* const sliderTpl = rows.data + static_cast<size_t>(sliderAt) * l.stride;  // Brightness Intensity (its sub-type set below)
    TArr tplSteps;
    memcpy(&tplSteps, toggleTpl + l.steps + l.stepArray, sizeof tplSteps);
    if (toggleTpl[l.type] != 1 || !(At<int32_t>(toggleTpl, l.platformFlag) & 1) || tplSteps.num != 2 || !tplSteps.data ||
        !StrIs(tplSteps.data + l.cmdCommand, L"Altar.GraphicsOptions.ScreenSpaceReflection 0") ||
        !StrIs(tplSteps.data + l.cmdStride + l.cmdCommand, L"Altar.GraphicsOptions.ScreenSpaceReflection 1"))
        return Disable("General has no PC Screen Space Reflections row as in build 19115871");
    if (sliderTpl[l.type] != 2 || !(At<int32_t>(sliderTpl, l.platformFlag) & 1) ||
        !StrIs(sliderTpl + l.slider + l.cvar, L"Altar.GraphicsOptions.Brightness"))
        return Disable("General has no Brightness Intensity slider as in build 19115871");

    // The state the rows show and the variables get: the menu's saved options when they have the entry, else the settings
    // object and the engine's defaults. With GameUserSettings.ini read-only (as some players keep it) the object reloads the
    // ini's values at every launch and never learns a menu change, so the saved options win (seen live: saved 1000 nits,
    // object 1300).
    const bool outputOn = (gus[l.hdrOn] & l.hdrOnMask) != 0;
    bool on = outputOn;
    int32_t nits = At<int32_t>(gus, l.hdrNits);
    // defaults (the rows' DefaultValue / DefaultIndex): HDR Off, peak 1000, paper white 200, black floor -4, mid-tone 15
    constexpr float kPeakDefault = 1000.f, kPaperDefault = 200.f, kBlackDefault = -4.f, kMidDefault = 15.f;
    float paper = kPaperDefault, black = kBlackDefault, mid = kMidDefault;
    double d = 0;
    static const std::string kNone;
    const std::string& save = s.saveRead.load(std::memory_order_acquire) ? s.save : kNone;
    const bool savedOn = Saved(save, "Altar.GraphicsOptions.HDR", &d);
    if (savedOn) on = d != 0;
    const bool savedNits = Saved(save, "Altar.GraphicsOptions.HDRWhitePoint", &d) && d >= 400 && d <= 4000;
    if (savedNits) nits = static_cast<int32_t>(d);
    if (nits < 400 || nits > 4000) nits = 1000;
    const bool savedPaper = Saved(save, "r.HDR.UI.Luminance", &d) && d >= 80 && d <= 400;
    if (savedPaper) paper = static_cast<float>(d);
    if (Saved(save, "r.HDR.Display.MinLuminanceLog10", &d) && d >= -4 && d <= -1) black = static_cast<float>(static_cast<int>(d - 0.5));
    if (Saved(save, "r.HDR.Display.MidLuminance", &d) && d >= 5 && d <= 50) mid = static_cast<float>(d);

    // the five rows, each a full copy of a game row, then given its own contents
    const size_t bytes = static_cast<size_t>(l.stride) * kHdrRows;
    auto* const hdrRows = static_cast<uint8_t*>(s.malloc(bytes, 0));
    if (!hdrRows) return Disable("no memory for the HDR rows");
    for (int i = 0; i < kHdrRows; ++i)
        if (!CopyStruct(l.rowStruct, l.stride, hdrRows + static_cast<size_t>(i) * l.stride, i == 0 ? toggleTpl : sliderTpl))
            return Disable("a template row could not be copied");
    uint8_t* const hdr = hdrRows;
    SetText(hdr + l.name, L"HDR");
    SetText(hdr + l.description, L"Toggles HDR output on or off. Requires HDR to be on in Windows display settings.");
    TArr hdrSteps;
    memcpy(&hdrSteps, hdr + l.steps + l.stepArray, sizeof hdrSteps);  // the copy's own two steps (labels Off / On)
    if (!SetString(hdrSteps.data + l.cmdCommand, L"Altar.GraphicsOptions.HDR 0") ||
        !SetString(hdrSteps.data + l.cmdStride + l.cmdCommand, L"Altar.GraphicsOptions.HDR 1"))
        return Disable("no memory for a step's command");
    const int32_t idx = on ? 1 : 0, offIdx = 0;  // shown: the current state; default: Off (the player switches HDR on)
    memcpy(hdr + l.steps + l.stepIndex, &idx, 4);
    memcpy(hdr + l.steps + l.stepDefault, &offIdx, 4);
    memcpy(hdr + l.platformFlag, &kAllPlatforms, 4);
    const SliderSpec sliders[kHdrRows - 1] = {
        {L"Peak Brightness", L"Sets the brightest highlight in nits. Match it to your display's peak brightness.",
         L"Altar.GraphicsOptions.HDRWhitePoint", static_cast<float>(nits), kPeakDefault, 400.f, 4000.f, 50.f, 100.f, 0, false},
        {L"Paper White", L"Adjusts the brightness of menus, subtitles and the HUD in nits.", L"r.HDR.UI.Luminance", paper,
         kPaperDefault, 80.f, 400.f, 10.f, 20.f, 0, false},
        {L"Black Floor", L"Sets the darkest black. -4 is recommended for OLED, -2 for LED/LCD.", L"r.HDR.Display.MinLuminanceLog10",
         black, kBlackDefault, -4.f, -1.f, 1.f, 1.f, 0, true},  // whole steps: the slider label rounds (-3.5 showed as -3)
        {L"Mid-Tone", L"Adjusts the overall brightness of the scene in nits.", L"r.HDR.Display.MidLuminance", mid, kMidDefault, 5.f,
         50.f, 1.f, 5.f, 0, false},
    };
    for (int i = 1; i < kHdrRows; ++i) SetSlider(hdrRows + static_cast<size_t>(i) * l.stride, l, sliders[i - 1]);

    // the HDR table: a new VModernSettingTable (the General table's class) holding the rows
    uint8_t* const hdrTable = s.spawn(Ptr(table, kObjClass), catTable);
    if (!hdrTable || Ptr(hdrTable, kObjClass) != Ptr(table, kObjClass)) return Disable("the engine did not create the HDR table");
    const TArr hdrArray{hdrRows, kHdrRows, kHdrRows};
    memcpy(hdrTable + l.settings, &hdrArray, sizeof hdrArray);  // the new table's own array is empty: nothing to release

    // the category row "HDR" after General, in a new array (the old one is left allocated)
    auto* const newCats = static_cast<uint8_t*>(s.malloc(static_cast<size_t>(cats.num + 1) * l.catStride, 0));
    if (!newCats) return Disable("no memory for the categories");
    const size_t head = static_cast<size_t>(general + 1) * l.catStride;
    memcpy(newCats, cats.data, head);
    uint8_t* const hdrCat = newCats + head;
    if (!CopyStruct(l.catStruct, l.catStride, hdrCat, cats.data + static_cast<size_t>(general) * l.catStride))
        return Disable("the General category could not be copied");
    SetText(hdrCat + l.catName, L"HDR");
    memcpy(hdrCat + l.catSettings, &hdrTable, 8);
    memcpy(hdrCat + l.catStride, cats.data + head, static_cast<size_t>(cats.num - general - 1) * l.catStride);
    const TArr newArray{newCats, cats.num + 1, cats.num + 1};
    memcpy(catTable + l.categories, &newArray, sizeof newArray);

    // Locks, by the game's own row sub-types (the widgets made from now on follow them; nothing else reads them as far
    // as seen): the four HDR sliders HDROptions (greyed while HDR is off), Brightness Intensity SDROptions (greyed while
    // HDR is on; measured: it has no effect then). Only when General's own rows carry the sub-type values of build
    // 19115871 and Brightness has none (or ours, on a reloaded table).
    const char* locks = "rows lock with HDR";
    const uint8_t* const resRow = rows.data + static_cast<size_t>(kResolutionRow) * l.stride;
    const uint8_t* const vsyncRow = rows.data + static_cast<size_t>(kVSyncRow) * l.stride;
    if (rows.num > kVSyncRow && resRow[l.subType] == kSubResolution && vsyncRow[l.subType] == kSubVSync &&
        (sliderTpl[l.subType] == kSubNone || sliderTpl[l.subType] == kSubSdrOptions)) {
        for (int i = 1; i < kHdrRows; ++i) hdrRows[static_cast<size_t>(i) * l.stride + l.subType] = kSubHdrOptions;
        hdr[l.subType] = kSubNone;  // the HDR switch itself: never locked (the game's HDR sub-type switched HDR off)
        sliderTpl[l.subType] = kSubSdrOptions;
    } else {
        locks = "rows do not lock (General's sub-types are not build 19115871's)";
    }

    s.hdrTable = hdrTable;
    s.patched = catTable;
    s.patchedIndex = catIndex;
    ++s.patches;
    if (s.patches == 1) {  // once per session (a reloaded table finds them set)
        SyncCvars(on, nits, outputOn, At<int32_t>(gus, l.hdrNits));
        if (!savedPaper) RunSetter(L"r.HDR.UI.Luminance", L"200");  // the row's default instead of the engine's 300
        KeepSavedOptions(gus, kSavedKeys, sizeof kSavedKeys / sizeof *kSavedKeys, "HDR");
    }
    Log("[hdrmenu] HDR section made after General (category %d of %d): HDR %s (%s), peak %d nits (%s), paper white %.0f, black "
        "floor %.1f, mid-tone %.0f; %s; table object %d, row stride 0x%X, category stride 0x%X (patch #%d)%s",
        general + 2, cats.num + 1, on ? "On" : "Off", savedOn ? "saved options" : "settings object", nits,
        savedNits ? "saved options" : "settings object", paper, black, mid, locks, At<int32_t>(hdrTable, kObjIndex), l.stride,
        l.catStride, s.patches, s.patches == 1 ? "; HDR variables set to that state" : "");
}

// The category table we patched is still there and still has the section (a reload makes a new one from the asset).
bool StillPatched()
{
    if (!s.patched) return false;
    const Item* const item = ItemOf(s.patchedIndex);
    if (!item || item->object != s.patched || !(ObjName(s.patched) == s.n.catName)) return false;
    Layout l;  // only the category offsets are needed: found again cheaply from the class
    const uint8_t* const cats = FindProp(Ptr(s.patched, kObjClass), s.n.categories, s.n.arrayProp, 0x10, true);
    const uint8_t* const catInner = cats ? Ptr(cats, kArrayPropInner) : nullptr;
    const uint8_t* const pCatSettings = catInner ? FindProp(InnerStruct(catInner), s.n.settings, s.n.objectProp, 8) : nullptr;
    if (!pCatSettings) return false;
    l.categories = Off(cats);
    l.catStride = Size(catInner);
    l.catSettings = Off(pCatSettings);
    TArr rows;
    return FindCategory(s.patched, l, s.hdrTable, &rows) >= 0;
}

// --- the Ray Reconstruction row --------------------------------------------------------------------------------------

constexpr wchar_t kRrName[] = L"DLSS - Ray Reconstruction";
constexpr wchar_t kRrDescription[] = L"Uses AI to improve ray-traced lighting and reflections for a sharper, more stable image.";
constexpr wchar_t kRrOff[] = L"r.NGX.DLSS.DenoiserMode 0";
constexpr wchar_t kRrOn[] = L"r.NGX.DLSS.DenoiserMode 1";
constexpr const wchar_t* kFgCmds[3] = {L"Altar.DLSS.FG.Enabled 0", L"Altar.DLSS.FG.Enabled 1", L"Altar.DLSS.FG.Enabled 2"};
constexpr const wchar_t* kRrCmds[2] = {kRrOff, kRrOn};

void DisableRr(const char* why)
{
    Log("[rrrow] %s: DlssRayReconstruction disabled, nothing written", why);
    s.rrFailed = true;
}

// The index of the table's step row whose first `count` steps run `cmds`, -1 if none.
int FindStepRow(const TArr& rows, const Layout& l, const wchar_t* const* cmds, int count)
{
    for (int32_t i = 0; i < rows.num; ++i) {
        const uint8_t* const row = rows.data + static_cast<size_t>(i) * l.stride;
        TArr steps;
        memcpy(&steps, row + l.steps + l.stepArray, sizeof steps);
        if (row[l.type] != 1 || steps.num < count || !steps.data) continue;
        bool all = true;
        for (int k = 0; k < count && all; ++k) all = StrIs(steps.data + static_cast<size_t>(k) * l.cmdStride + l.cmdCommand, cmds[k]);
        if (all) return i;
    }
    return -1;
}

void PatchRr(uint8_t* table, uint8_t* catTable, uint8_t* gus)
{
    Layout l;
    if (!ResolveLayout(table, catTable, gus, l)) return DisableRr("the settings menu's data differs from build 19115871");
    TArr rows;
    memcpy(&rows, table + l.settings, sizeof rows);
    if (!rows.data || rows.num <= 0 || rows.num > 64) return DisableRr("the upscaler table's rows are not as expected");
    if (FindStepRow(rows, l, kRrCmds, 2) >= 0) {  // already there (found again by a new walk)
        s.rrPatched = table;
        s.rrPatchedIndex = At<int32_t>(table, kObjIndex);
        s.rrLayout = l;
        return;
    }
    const int fg = FindStepRow(rows, l, kFgCmds, 3);
    if (fg < 0) return DisableRr("the upscaler table has no \"DLSS - Frame Generation\" row of build 19115871");
    const uint8_t* const fgRow = rows.data + static_cast<size_t>(fg) * l.stride;
    if (!(At<int32_t>(fgRow, l.platformFlag) & 1)) return DisableRr("the Frame Generation row is not a PC row");

    // the shown state: the saved option, else Off
    double d = 0;
    static const std::string kNone;
    const std::string& save = s.saveRead.load(std::memory_order_acquire) ? s.save : kNone;
    const bool saved = Saved(save, kRrKey, &d);
    const int32_t idx = saved && d != 0 ? 1 : 0, offIdx = 0;

    // the new rows: those up to Frame Generation, the copy, the rest (the old array is left allocated)
    const size_t stride = l.stride;
    auto* const newRows = static_cast<uint8_t*>(s.malloc(static_cast<size_t>(rows.num + 1) * stride, 0));
    if (!newRows) return DisableRr("no memory for the upscaler rows");
    const size_t head = static_cast<size_t>(fg + 1) * stride;
    memcpy(newRows, rows.data, head);
    uint8_t* const rr = newRows + head;
    if (!CopyStruct(l.rowStruct, l.stride, rr, fgRow)) return DisableRr("the Frame Generation row could not be copied");
    memcpy(rr + stride, rows.data + head, static_cast<size_t>(rows.num - fg - 1) * stride);
    SetText(rr + l.name, kRrName);
    SetText(rr + l.description, kRrDescription);
    TArr steps;  // the copy's own steps (Off / On / Automatic): the first two kept, Automatic dropped (left allocated)
    memcpy(&steps, rr + l.steps + l.stepArray, sizeof steps);
    if (!SetString(steps.data + l.cmdCommand, kRrOff) || !SetString(steps.data + l.cmdStride + l.cmdCommand, kRrOn))
        return DisableRr("no memory for a step's command");
    steps.num = 2;
    memcpy(rr + l.steps + l.stepArray, &steps, sizeof steps);
    memcpy(rr + l.steps + l.stepIndex, &idx, 4);
    memcpy(rr + l.steps + l.stepDefault, &offIdx, 4);
    const TArr newArray{newRows, rows.num + 1, rows.num + 1};
    memcpy(table + l.settings, &newArray, sizeof newArray);

    s.rrPatched = table;
    s.rrPatchedIndex = At<int32_t>(table, kObjIndex);
    s.rrLayout = l;
    ++s.rrPatches;
    if (s.rrPatches == 1 && saved) {  // once per session: the saved state applied and kept
        RunSetter(L"r.NGX.DLSS.DenoiserMode", idx ? L"1" : L"0");
        KeepSavedOptions(gus, &kRrKey, 1, "Ray Reconstruction");
    }
    Log("[rrrow] \"DLSS - Ray Reconstruction\" made after \"DLSS - Frame Generation\" (row %d of %d): %s (%s); sub-type %u, "
        "platform flag %d as Frame Generation's; table object %d (patch #%d)",
        fg + 2, rows.num + 1, idx ? "On" : "Off", saved ? "saved option" : "no saved option", rr[l.subType],
        At<int32_t>(rr, l.platformFlag), s.rrPatchedIndex, s.rrPatches);
}

// The upscaler table we patched is still there and still has the row (a reload makes a new one from the asset).
bool RrStillPatched()
{
    if (!s.rrPatched) return false;
    const Item* const item = ItemOf(s.rrPatchedIndex);
    if (!item || item->object != s.rrPatched || !(ObjName(s.rrPatched) == s.n.upName)) return false;
    TArr rows;
    memcpy(&rows, s.rrPatched + s.rrLayout.settings, sizeof rows);
    return rows.data && rows.num > 0 && rows.num <= 65 && FindStepRow(rows, s.rrLayout, kRrCmds, 2) >= 0;
}

// The settings widget's upscaler-row lock (exe+0x495BF70): rcx = the menu's settings object (its current upscaler at
// +0xD8), rdx = the widget, r8 = the row's console variable (FString). At +0x1C it looks the variable up:
//   48 89 6C 24 18 / 48 89 74 24 20 / 41 56 / 48 83 EC 30 / 4C 8B F2 mov r14,rdx / 49 8B F0 mov rsi,r8 /
//   49 8B D0 mov rdx,r8 / 48 8B E9 mov rbp,rcx / E8 <rel32> call lookup / 48 85 C0 test rax,rax / 0F 84 jz (no entry:
//   the widget left as it is) / 44 0F B6 8D D8 00 00 00 movzx r9d,byte [rbp+0D8h] / 44 3A 08 cmp r9b,[rax]
constexpr char kLockFn[] =
    "48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 30 4C 8B F2 49 8B F0 49 8B D0 48 8B E9 E8 ?? ?? ?? ?? 48 85 C0 "
    "0F 84 ?? ?? ?? ?? 44 0F B6 8D D8 00 00 00 44 3A 08";
constexpr size_t kLockCall = 0x1C;
// The lookup (exe+0x495AFD0): a function-static TMap<FString, upscaler> made once, then the entry's value or null.
constexpr char kLockLookup[] = "48 89 5C 24 08 48 89 7C 24 18 55 48 8D AC 24 30 FF FF FF 48 81 EC D0 01 00 00";
constexpr uint8_t kUpscalerDlss = 1;  // the table's value for Altar.DLSS.* (Altar.UpscalingMethod 1)

using LookupFn = const uint8_t* (*)(void* settings, const FStr* cvar);
struct LockState {
    uint8_t* call = nullptr;
    uint8_t* stub = nullptr;
    LookupFn original = nullptr;
    std::atomic<uint64_t> answered{0};
} g_lock;

const uint8_t* LockLookupWrapper(void* settings, const FStr* cvar)
{
    const uint8_t* const r = g_lock.original(settings, cvar);
    static const int32_t keyLen = static_cast<int32_t>(strlen(kRrKey)) + 1;  // with the terminator, as FString's Num
    if (r || !cvar || !cvar->data || cvar->num != keyLen) return r;
    for (int32_t i = 0; i < cvar->num; ++i)
        if (cvar->data[i] != static_cast<wchar_t>(kRrKey[i])) return r;
    static const uint8_t dlss = kUpscalerDlss;
    g_lock.answered.fetch_add(1, std::memory_order_relaxed);
    return &dlss;
}

bool ApplyRrLock(bool beforeGameCode)
{
    if (g_lock.call) return true;
    const GameModule& game = Game();
    const auto hits = FindAll(game.text, ParsePattern(kLockFn));
    if (hits.size() != 1) {
        Log("[rrrow] the settings widgets' upscaler lock not found (%zu matches): the row is not greyed with the other DLSS rows", hits.size());
        return false;
    }
    uint8_t* const call = hits[0] + kLockCall;
    uint8_t* const target = RipTarget(call, 1, 5);
    if (!game.text.Contains(target) || !MatchesAt(target, ParsePattern(kLockLookup))) {
        Log("[rrrow] the lock's lookup at exe+0x%llX differs: the row is not greyed with the other DLSS rows",
            static_cast<unsigned long long>(call - game.base));
        return false;
    }
    uint8_t* const region = AllocNearGame(0x1000);
    if (!region) {
        Log("[rrrow] no free memory near the game's code: the row is not greyed with the other DLSS rows");
        return false;
    }
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    void* const hook = reinterpret_cast<void*>(&LockLookupWrapper);
    memcpy(region, jmp, sizeof jmp);
    memcpy(region + sizeof jmp, &hook, sizeof hook);
    DWORD old;
    VirtualProtect(region, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 14);
    const int64_t rel = region - (call + 5);
    // the rel32 straddles two qwords here, so it is written only before the game's code runs (the version.dll route);
    // a 4-byte write while the game runs is never done
    bool ok = false;
    if (rel == static_cast<int32_t>(rel)) {
        uint8_t insn[5] = {0xE8};
        const int32_t rel32 = static_cast<int32_t>(rel);
        memcpy(insn + 1, &rel32, 4);
        const uintptr_t lo = reinterpret_cast<uintptr_t>(call) + 1, qword = lo & ~uintptr_t{7};
        if (lo + 3 < qword + 8) {
            auto* q = reinterpret_cast<volatile LONG64*>(qword);
            if (VirtualProtect(reinterpret_cast<void*>(qword), 8, PAGE_EXECUTE_READWRITE, &old)) {
                const LONG64 before = *q;
                LONG64 after = before;
                memcpy(reinterpret_cast<uint8_t*>(&after) + (lo - qword), insn + 1, 4);
                ok = InterlockedCompareExchange64(q, after, before) == before;
                VirtualProtect(reinterpret_cast<void*>(qword), 8, old, &old);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(qword), 8);
            }
        } else if (beforeGameCode) {
            ok = WriteCode(call, insn);
        }
    }
    if (!ok) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[rrrow] the lock's call at exe+0x%llX could not be switched%s: the row is not greyed with the other DLSS rows",
            static_cast<unsigned long long>(call - game.base), beforeGameCode ? "" : " while the game runs");
        return false;
    }
    g_lock.original = reinterpret_cast<LookupFn>(target);
    g_lock.call = call;
    g_lock.stub = region;
    Log("[rrrow] the settings widgets' upscaler lock (exe+0x%llX) looks rows up through the wrapper at exe+0x%llX: the row "
        "is greyed with the other DLSS rows", static_cast<unsigned long long>(hits[0] - game.base),
        static_cast<unsigned long long>(call - game.base));
    return true;
}

// --- the HUD aspect row ----------------------------------------------------------------------------------------------

constexpr wchar_t kHudName[] = L"HUD Aspect Ratio";
constexpr wchar_t kHudDescription[] = L"Places the HUD within a 4:3, 16:9, 21:9 or 32:9 area in the middle of the screen. Wide uses the whole screen.";
constexpr const char* kHudKey = "OPF.HudAspect";
constexpr wchar_t kHudVar[] = L"OPF.HudAspect";
constexpr wchar_t kHudHelp[] = L"(OblivionPerfFix HudAspectRow): the HUD's area. 0 = Wide (the whole screen), 1 = 4:3, 2 = 16:9, 3 = 21:9, 4 = 32:9.";
constexpr int kHudSteps = 5;
// the row's steps in the order shown, and the value each runs
constexpr const wchar_t* kHudLabels[kHudSteps] = {L"Wide", L"4:3", L"16:9", L"21:9", L"32:9"};
constexpr const wchar_t* kHudCmds[kHudSteps] = {L"OPF.HudAspect 0", L"OPF.HudAspect 1", L"OPF.HudAspect 2", L"OPF.HudAspect 3", L"OPF.HudAspect 4"};
constexpr int32_t kHudValues[kHudSteps] = {0, 1, 2, 3, 4};
constexpr int32_t kHudWideStep = 0;
constexpr const wchar_t* kShowFpsCmds[2] = {L"Altar.GraphicsOptions.ShowFPS 0", L"Altar.GraphicsOptions.ShowFPS 1"};

bool RegisterHudVar();  // below: OPF.HudAspect registered (the saved option is applied through it)

void DisableHud(const char* why)
{
    Log("[hudaspect] %s: HudAspectRow disabled, nothing written", why);
    s.hudFailed = true;
}

void PatchHud(uint8_t* table, uint8_t* catTable, uint8_t* gus)
{
    Layout l;
    if (!ResolveLayout(table, catTable, gus, l)) return DisableHud("the settings menu's data differs from build 19115871");
    if (l.stepLabel == UINT32_MAX) return DisableHud("a step has no Label text");
    TArr rows;
    memcpy(&rows, table + l.settings, sizeof rows);
    if (!rows.data || rows.num <= 0 || rows.num > 64) return DisableHud("the General table's rows are not as expected");
    if (FindStepRow(rows, l, kHudCmds, kHudSteps) >= 0) {  // already there (found again by a new walk)
        s.hudPatched = table;
        s.hudPatchedIndex = At<int32_t>(table, kObjIndex);
        s.hudLayout = l;
        return;
    }
    const int fps = FindStepRow(rows, l, kShowFpsCmds, 2);
    if (fps < 0) return DisableHud("the General table has no \"Show FPS\" row of build 19115871");
    const uint8_t* const fpsRow = rows.data + static_cast<size_t>(fps) * l.stride;
    if (!(At<int32_t>(fpsRow, l.platformFlag) & 1)) return DisableHud("the Show FPS row is not a PC row");
    TArr fpsSteps;
    memcpy(&fpsSteps, fpsRow + l.steps + l.stepArray, sizeof fpsSteps);
    if (!fpsSteps.data || fpsSteps.num < 1) return DisableHud("the Show FPS row has no steps");

    // the shown state: the saved option, else Wide
    double d = 0;
    static const std::string kNone;
    const std::string& save = s.saveRead.load(std::memory_order_acquire) ? s.save : kNone;
    const bool saved = Saved(save, kHudKey, &d);
    int32_t idx = kHudWideStep;
    if (saved)
        for (int k = 0; k < kHudSteps; ++k)
            if (kHudValues[k] == static_cast<int32_t>(d)) idx = k;
    const int32_t defIdx = kHudWideStep;

    // the five steps, each a copy of Show FPS's first step with its own label and command
    auto* const steps = static_cast<uint8_t*>(s.malloc(static_cast<size_t>(kHudSteps) * l.cmdStride, 0));
    if (!steps) return DisableHud("no memory for the steps");
    for (int k = 0; k < kHudSteps; ++k) {
        uint8_t* const st = steps + static_cast<size_t>(k) * l.cmdStride;
        if (!CopyStruct(l.stepStruct, l.cmdStride, st, fpsSteps.data)) return DisableHud("a step could not be copied");
        SetText(st + l.stepLabel, kHudLabels[k]);
        if (!SetString(st + l.cmdCommand, kHudCmds[k])) return DisableHud("no memory for a step's command");
    }
    // the new rows: those before Show FPS, the copy, Show FPS and the rest (the old array is left allocated)
    const size_t stride = l.stride;
    auto* const newRows = static_cast<uint8_t*>(s.malloc(static_cast<size_t>(rows.num + 1) * stride, 0));
    if (!newRows) return DisableHud("no memory for the General rows");
    const size_t head = static_cast<size_t>(fps) * stride;
    memcpy(newRows, rows.data, head);
    uint8_t* const hud = newRows + head;
    if (!CopyStruct(l.rowStruct, l.stride, hud, fpsRow)) return DisableHud("the Show FPS row could not be copied");
    memcpy(hud + stride, rows.data + head, static_cast<size_t>(rows.num - fps) * stride);
    SetText(hud + l.name, kHudName);
    SetText(hud + l.description, kHudDescription);
    const TArr stepArray{steps, kHudSteps, kHudSteps};  // the copy's own two steps are left allocated
    memcpy(hud + l.steps + l.stepArray, &stepArray, sizeof stepArray);
    memcpy(hud + l.steps + l.stepIndex, &idx, 4);
    memcpy(hud + l.steps + l.stepDefault, &defIdx, 4);
    const TArr newArray{newRows, rows.num + 1, rows.num + 1};
    memcpy(table + l.settings, &newArray, sizeof newArray);

    s.hudPatched = table;
    s.hudPatchedIndex = At<int32_t>(table, kObjIndex);
    s.hudLayout = l;
    ++s.hudPatches;
    if (s.hudPatches == 1 && saved) {  // once per session: the saved state applied (the variable registered first) and kept
        if (!RegisterHudVar()) Log("[hudaspect] OPF.HudAspect not registered yet: the saved choice shows in the menu but is not applied until picked again");
        wchar_t v[8];
        swprintf(v, 8, L"%d", kHudValues[idx]);
        RunSetter(kHudVar, v);
        KeepSavedOptions(gus, &kHudKey, 1, "HUD aspect");
    }
    Log("[hudaspect] \"HUD Aspect Ratio\" made before \"Show FPS\" (row %d of %d): %ls (%s); table object %d (patch #%d)",
        fps + 1, rows.num + 1, kHudLabels[idx], saved ? "saved option" : "no saved option", s.hudPatchedIndex, s.hudPatches);
}

// The General table we gave the row is still there and still has it (a reload makes a new one from the asset).
bool HudStillPatched()
{
    if (!s.hudPatched) return false;
    const Item* const item = ItemOf(s.hudPatchedIndex);
    if (!item || item->object != s.hudPatched || !(ObjName(s.hudPatched) == s.n.tableName)) return false;
    TArr rows;
    memcpy(&rows, s.hudPatched + s.hudLayout.settings, sizeof rows);
    return rows.data && rows.num > 0 && rows.num <= 65 && FindStepRow(rows, s.hudLayout, kHudCmds, kHudSteps) >= 0;
}

// --- the HUD's padding ------------------------------------------------------------------------------------------------

// UObject::ProcessEvent(UFunction*, void* Parms) (exe+0x10F3EA0): seven pushes, sub rsp,110h, the frame, the cookie,
// then this null -> out, (this->ObjectFlags & 0x60000000) -> out (being destroyed), Function->FunctionFlags & 0x400
// (native) tested.
constexpr char kProcessEvent[] =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 10 01 00 00 48 8D 6C 24 30 48 89 9D 38 01 00 00 48 8B 05 ?? ?? ?? ?? "
    "48 33 C5 48 89 85 D0 00 00 00 4D 8B E0 48 8B F2 4C 8B F9 48 85 C9 0F 84 ?? ?? ?? ?? 8B 41 08 A9 00 00 00 60 0F 85 "
    "?? ?? ?? ?? 45 33 F6 F7 82 B0 00 00 00 00 04 00 00";
// UE 5.3: UStruct::Children (UField*) at +0x48, UField::Next at +0x28; UFunction::NumParms (uint8) +0xB4, ParmsSize
// (uint16) +0xB6, ReturnValueOffset (uint16) +0xB8 (FunctionFlags at +0xB0, as ProcessEvent reads it)
constexpr size_t kStructChildren = 0x48, kFieldNextObj = 0x28, kFnNumParms = 0xB4, kFnParmsSize = 0xB6, kFnReturnOffset = 0xB8;
constexpr uint32_t kRfArchetype = 0x20;
constexpr int kHudMaxWidgets = 2;            // live HUD widgets followed
constexpr int kHudMaxParts = 24;             // their root's parts
constexpr uint32_t kHudApplyFrames = 30;     // padding checked twice a second at 60 fps
constexpr uint64_t kHudRescanMs = 10000;     // the object search refreshed every 10 s (a new HUD after a reload); by the clock:
                                             // a frame count was 3.6 s at 165 fps (measured: 0.12-0.15 ms per search frame)

using ProcessEventFn = void (*)(void* obj, void* fn, void* parms);
using FindCvarFn = void* (*)(void* manager, const wchar_t* name, bool trackFrequentCalls);
using RegisterIntFn = void* (*)(void* manager, const wchar_t* name, int32_t def, const wchar_t* help, uint32_t flags);
using AsIntFn = int32_t* (*)(void* cvar);
constexpr uint32_t kCmFind = 0xB8, kCmRegisterInt = 0x18, kCvAsInt = 0x58;

struct HudNames {
    NameId hudClass, vignetteClass, widgetTree, rootWidget, slots, content, padding, setPadding, viewportSize, viewportScale,
        wllDefault, owningPlayer;
};

struct Part {            // one part of the HUD's root (an overlay slot) and its own padding before ours
    uint8_t* slot = nullptr;
    int32_t index = -1;
    float original[4] = {};
};

struct HudState {
    ProcessEventFn processEvent = nullptr;
    bool namesReady = false;
    HudNames n;
    int32_t* value = nullptr;          // OPF.HudAspect's two copies ([0] game thread)
    bool registerFailed = false;
    uint8_t* huds[kHudMaxWidgets] = {};
    int32_t hudIndex[kHudMaxWidgets] = {};
    int hudCount = 0;
    uint8_t* wll = nullptr;            // Default__WidgetLayoutLibrary
    int32_t wllIndex = -1;
    uint8_t* fnSetPadding = nullptr;
    uint8_t* fnViewportSize = nullptr;
    uint8_t* fnViewportScale = nullptr;
    uint8_t* fnOwner = nullptr;
    uint32_t treeOff = 0, rootOff = 0, slotsOff = 0, contentOff = 0, paddingOff = 0;
    bool offsetsReady = false, offsetsFailed = false;
    uint32_t cursor = 0;
    uint64_t scanStartMs = 0;  // GetTickCount64 when the last whole pass ended
    uint8_t* found[kHudMaxWidgets] = {};
    int foundCount = 0;
    uint8_t* foundWll = nullptr;
    Part parts[kHudMaxParts];
    int partCount = 0;
    float lastPad = -1.f;
    long applied = 0;
} h;

void** VTab(void* obj) { return *static_cast<void***>(obj); }

bool MakeHudNames()
{
    HudNames& n = h.n;
    struct { NameId* id; const wchar_t* text; } all[] = {
        {&n.hudClass, L"WBP_ModernHud_PrimaryLayout_C"}, {&n.vignetteClass, L"WBP_Modern_Hud_Vignette_C"},
        {&n.widgetTree, L"WidgetTree"}, {&n.rootWidget, L"RootWidget"}, {&n.slots, L"Slots"}, {&n.content, L"Content"},
        {&n.padding, L"Padding"}, {&n.setPadding, L"SetPadding"}, {&n.viewportSize, L"GetViewportSize"},
        {&n.viewportScale, L"GetViewportScale"}, {&n.wllDefault, L"Default__WidgetLayoutLibrary"}, {&n.owningPlayer, L"GetOwningPlayer"},
    };
    for (auto& e : all) {
        *e.id = MakeName(e.text);
        if (!e.id->valid()) return false;
    }
    return true;
}

// The UFunction `name` on class `cls` (or a parent), with `parms` parameters, a parameter block of `size` (or `size2`)
// bytes and (retOff != 0xFFFF) its return value at `retOff`; null otherwise.
uint8_t* FindFunction(const uint8_t* cls, const NameId& name, uint8_t parms, uint16_t size, uint16_t size2, uint16_t retOff)
{
    for (int depth = 0; cls && depth < 16; ++depth, cls = Ptr(cls, kStructSuper))
        for (uint8_t* f = Ptr(cls, kStructChildren); f; f = Ptr(f, kFieldNextObj)) {
            if (!(ObjName(f) == name)) continue;
            if (At<uint8_t>(f, kFnNumParms) != parms) return nullptr;
            const uint16_t ps = At<uint16_t>(f, kFnParmsSize);
            if (ps != size && ps != size2) return nullptr;
            if (retOff != 0xFFFF && At<uint16_t>(f, kFnReturnOffset) != retOff) return nullptr;
            return f;
        }
    return nullptr;
}

// OPF.HudAspect registered (once) through the console manager; its value's address.
bool RegisterHudVar()
{
    if (h.value) return true;
    if (h.registerFailed) return false;
    void** const slot = HdrConsoleManager();
    void* const manager = slot ? *slot : nullptr;
    if (!manager) return false;  // not made yet: next time
    auto find = reinterpret_cast<FindCvarFn>(VTab(manager)[kCmFind / 8]);
    void* var = find(manager, kHudVar, false);
    if (!var) var = reinterpret_cast<RegisterIntFn>(VTab(manager)[kCmRegisterInt / 8])(manager, kHudVar, 0, kHudHelp, 0);
    int32_t* const data = var ? reinterpret_cast<AsIntFn>(VTab(var)[kCvAsInt / 8])(var) : nullptr;
    if (!data) {
        Log("[hudaspect] OPF.HudAspect could not be registered as an int variable: the HUD stays as the game places it");
        h.registerFailed = true;
        return false;
    }
    h.value = data;
    Log("[hudaspect] console variable OPF.HudAspect registered (now %d)", data[0]);
    return true;
}

bool ValidObj(uint8_t* obj, int32_t index)
{
    const Item* const item = ItemOf(index);
    return item && item->object == obj;
}

// The live HUD widgets and the layout library's default object, an incremental walk of GUObjectArray (16384 objects a
// frame).
void HudScan()
{
    const int32_t num = At<int32_t>(s.objectArray, kArrayNum);
    Item** const chunks = reinterpret_cast<Item**>(Ptr(s.objectArray, kArrayChunks));
    if (!chunks || num <= 0) return;
    const uint32_t end = h.cursor + kScanPerFrame < static_cast<uint32_t>(num) ? h.cursor + kScanPerFrame : static_cast<uint32_t>(num);
    for (uint32_t i = h.cursor; i < end; ++i) {
        Item* const chunk = chunks[i / kChunkItems];
        if (!chunk) continue;
        uint8_t* const obj = chunk[i % kChunkItems].object;
        if (!obj || !Ptr(obj, kObjClass)) continue;
        if (ObjName(obj) == h.n.wllDefault) h.foundWll = obj;
        else if (ClassName(obj) == h.n.hudClass && h.foundCount < kHudMaxWidgets &&
                 !(At<uint32_t>(obj, kObjFlags) & (kRfClassDefaultObject | kRfArchetype)))
            h.found[h.foundCount++] = obj;
    }
    h.cursor = end;
    if (h.cursor < static_cast<uint32_t>(num)) return;
    // a whole pass: take what it found
    h.cursor = 0;
    h.hudCount = h.foundCount;
    for (int i = 0; i < h.foundCount; ++i) {
        h.huds[i] = h.found[i];
        h.hudIndex[i] = At<int32_t>(h.found[i], kObjIndex);
    }
    if (h.foundWll) {
        h.wll = h.foundWll;
        h.wllIndex = At<int32_t>(h.foundWll, kObjIndex);
    }
    static int passes = 0;
    if (++passes <= 3 || (h.hudCount == 0 && passes <= 20))
        Log("[hudaspect] object search %d: %d HUD widget(s), layout library %s (%d objects)", passes, h.hudCount, h.wll ? "found" : "not found", num);
    h.foundCount = 0;
    h.foundWll = nullptr;
    h.scanStartMs = GetTickCount64();
}

// The offsets and functions, from the HUD's class, its root panel's class, a part's slot class and the library (once).
bool HudOffsets(uint8_t* hud)
{
    if (h.offsetsReady) return true;
    if (h.offsetsFailed) return false;
    const uint8_t* const pTree = FindProp(Ptr(hud, kObjClass), h.n.widgetTree, s.n.objectProp, 8, true);
    uint8_t* const tree = pTree ? Ptr(hud, Off(pTree)) : nullptr;
    if (!tree) return false;  // not built yet
    const uint8_t* const pRoot = FindProp(Ptr(tree, kObjClass), h.n.rootWidget, s.n.objectProp, 8, true);
    uint8_t* const root = pRoot ? Ptr(tree, Off(pRoot)) : nullptr;
    if (!root) return false;
    const uint8_t* const pSlots = FindProp(Ptr(root, kObjClass), h.n.slots, s.n.arrayProp, 0x10, true);
    TArr slots{};
    if (pSlots) memcpy(&slots, root + Off(pSlots), sizeof slots);
    uint8_t* const slot0 = slots.data && slots.num > 0 ? Ptr(slots.data, 0) : nullptr;
    const uint8_t* const pContent = slot0 ? FindProp(Ptr(slot0, kObjClass), h.n.content, s.n.objectProp, 8, true) : nullptr;
    const uint8_t* const pPad = slot0 ? FindProp(Ptr(slot0, kObjClass), h.n.padding, s.n.structProp, 16, true) : nullptr;
    h.fnSetPadding = slot0 ? FindFunction(Ptr(slot0, kObjClass), h.n.setPadding, 1, 16, 16, 0xFFFF) : nullptr;
    h.fnViewportSize = FindFunction(Ptr(h.wll, kObjClass), h.n.viewportSize, 2, 24, 24, 8);
    h.fnViewportScale = FindFunction(Ptr(h.wll, kObjClass), h.n.viewportScale, 2, 12, 16, 8);
    h.fnOwner = FindFunction(Ptr(hud, kObjClass), h.n.owningPlayer, 1, 8, 8, 0);
    if (!pSlots || !pContent || !pPad || !h.fnSetPadding || !h.fnViewportSize || !h.fnViewportScale || !h.fnOwner) {
        Log("[hudaspect] the HUD's parts, their padding or the layout functions are not as in build 19115871 (slots %d, content %d, "
            "padding %d, SetPadding %d, GetViewportSize %d, GetViewportScale %d, GetOwningPlayer %d): the HUD stays as the game places it",
            pSlots != nullptr, pContent != nullptr, pPad != nullptr, h.fnSetPadding != nullptr, h.fnViewportSize != nullptr,
            h.fnViewportScale != nullptr, h.fnOwner != nullptr);
        h.offsetsFailed = true;
        return false;
    }
    h.treeOff = Off(pTree);
    h.rootOff = Off(pRoot);
    h.slotsOff = Off(pSlots);
    h.contentOff = Off(pContent);
    h.paddingOff = Off(pPad);
    h.offsetsReady = true;
    return true;
}

// The record of a part (its padding before ours, kept the first time it is seen); null when the table is full.
Part* PartOf(uint8_t* slot)
{
    const int32_t index = At<int32_t>(slot, kObjIndex);
    for (int i = 0; i < h.partCount; ++i)
        if (h.parts[i].slot == slot && h.parts[i].index == index) return &h.parts[i];
    // a stale entry (its slot gone) is reused
    for (int i = 0; i < h.partCount; ++i)
        if (!ValidObj(h.parts[i].slot, h.parts[i].index)) {
            h.parts[i] = Part{slot, index, {}};
            memcpy(h.parts[i].original, slot + h.paddingOff, sizeof h.parts[i].original);
            return &h.parts[i];
        }
    if (h.partCount >= kHudMaxParts) return nullptr;
    Part& p = h.parts[h.partCount++];
    p = Part{slot, index, {}};
    memcpy(p.original, slot + h.paddingOff, sizeof p.original);
    return &p;
}

void HudTick()
{
    if (!h.processEvent || h.offsetsFailed) return;
    if (!h.namesReady) {
        if (s.frames % 30 != 0 || !MakeHudNames()) return;
        h.namesReady = true;
    }
    if (!RegisterHudVar()) return;
    // the objects: re-found when one is gone, and refreshed every ~10 s
    bool valid = h.hudCount > 0 && h.wll && ValidObj(h.wll, h.wllIndex);
    for (int i = 0; valid && i < h.hudCount; ++i) valid = ValidObj(h.huds[i], h.hudIndex[i]);
    if (!valid || GetTickCount64() - h.scanStartMs >= kHudRescanMs || h.cursor != 0) {
        if (!valid) h.hudCount = 0;
        HudScan();
        if (!valid) return;
    }
    if (s.frames % kHudApplyFrames != 0) return;
    const int32_t v = h.value[0];
    float aspect = 0.f;  // 0: Wide
    switch (v) {
    case 1: aspect = 4.f / 3.f; break;
    case 2: aspect = 16.f / 9.f; break;
    case 3: aspect = 21.f / 9.f; break;
    case 4: aspect = 32.f / 9.f; break;
    default: break;
    }
    for (int i = 0; i < h.hudCount; ++i) {
        uint8_t* const hud = h.huds[i];
        if (!HudOffsets(hud)) {
            if (h.offsetsFailed) return;
            continue;  // not built yet
        }
        uint8_t* const tree = Ptr(hud, h.treeOff);
        uint8_t* const root = tree ? Ptr(tree, h.rootOff) : nullptr;
        if (!root) continue;
        // the world context: the player owning the HUD (the widgets themselves give the library's 1x1 fallback, seen
        // in a probe game: the HUD's world is not found as the viewport's through them)
        struct { void* player; } owner{nullptr};
        h.processEvent(hud, h.fnOwner, &owner);
        if (!owner.player) continue;
        struct { void* ctx; double x, y; } size{owner.player, 0, 0};
        struct { void* ctx; float scale; float pad; } scale{owner.player, 0, 0};
        h.processEvent(h.wll, h.fnViewportSize, &size);
        h.processEvent(h.wll, h.fnViewportScale, &scale);
        const float pad = HudPadding(static_cast<float>(size.x), static_cast<float>(size.y), scale.scale, aspect);
        if (pad < 0) continue;  // no viewport (yet)
        // every part of the HUD's root except its vignette (the full-screen effects: low health and the like), which
        // stays full screen; each keeps its own padding, ours added left and right
        TArr slots;
        memcpy(&slots, root + h.slotsOff, sizeof slots);
        if (!slots.data || slots.num <= 0 || slots.num > kHudMaxParts) continue;
        int changed = 0;
        for (int32_t k = 0; k < slots.num; ++k) {
            uint8_t* const slot = Ptr(slots.data, static_cast<size_t>(k) * 8);
            if (!slot || Ptr(slot, kObjClass) != Ptr(Ptr(slots.data, 0), kObjClass)) continue;  // overlay slots only
            uint8_t* const content = Ptr(slot, h.contentOff);
            if (!content || ClassName(content) == h.n.vignetteClass) continue;
            Part* const part = PartOf(slot);
            if (!part) continue;
            const float want[4] = {part->original[0] + pad, part->original[1], part->original[2] + pad, part->original[3]};
            float cur[4];
            memcpy(cur, slot + h.paddingOff, sizeof cur);
            bool same = true;
            for (int c = 0; c < 4; ++c) same = same && std::fabs(cur[c] - want[c]) < 0.5f;
            if (same) continue;
            float margin[4] = {want[0], want[1], want[2], want[3]};  // FMargin: left, top, right, bottom
            h.processEvent(slot, h.fnSetPadding, margin);
            ++changed;
        }
        if (!changed) continue;
        ++h.applied;
        if (h.applied <= 10 || pad != h.lastPad)
            Log("[hudaspect] HUD area %s: %d HUD part(s) padded %.0f each side, the vignette left full screen (viewport %.0fx%.0f at "
                "scale %.3f)", v == 1 ? "4:3" : v == 2 ? "16:9" : v == 3 ? "21:9" : v == 4 ? "32:9" : "Wide", changed, pad, size.x, size.y,
                scale.scale);
        h.lastPad = pad;
    }
}

}  // namespace

// The padding on each side (Slate units) that leaves a centred box of `aspect` (width / height; 0 = Wide) in a viewport
// of `w` x `h` pixels at Slate scale `scale`; 0 when the box would be as wide as the screen or wider; -1 when the
// viewport is unknown.
float HudPadding(float w, float hgt, float scale, float aspect)
{
    if (!(w > 0) || !(hgt > 0) || !(scale > 0)) return -1.f;
    if (!(aspect > 0)) return 0.f;
    const float sw = w / scale, sh = hgt / scale;
    const float pad = (sw - sh * aspect) / 2.f;
    return pad > 0.f ? pad : 0.f;
}

// UObject::ProcessEvent by its prologue (one match), or null.
uint8_t* FindProcessEvent()
{
    const auto pe = FindAll(Game().text, ParsePattern(kProcessEvent));
    return pe.size() == 1 ? pe[0] : nullptr;
}

bool FindHdrMenu(HdrMenuInfoOut* out)
{
    if (!s.found) {
        const GameModule& game = Game();
        const auto execs = FindAll(game.text, ParsePattern(kStringToTextExec));
        uint8_t* impl = nullptr;
        for (uint8_t* e : execs) {  // every FString -> FText exec function found must lead to the same FromString
            uint8_t* const wrapper = RipTarget(e + kExecCall, 1, 5);
            if (!game.text.Contains(wrapper) || !MatchesAt(wrapper, ParsePattern(kWrapper))) return Fail("Conv_StringToText's wrapper differs");
            uint8_t* const fn = RipTarget(wrapper + kWrapperCall, 1, 5);
            if (impl && fn != impl) return Fail("two FString -> FText exec functions lead to different functions");
            impl = fn;
        }
        if (!impl) return Fail("Conv_StringToText's exec function not found");
        if (!game.text.Contains(impl) || !MatchesAt(impl, ParsePattern(kFromString))) return Fail("FText::FromString differs");
        const auto ctors = FindAll(game.text, ParsePattern(kFNameCtor));
        if (ctors.size() != 1) {
            Log("[hdrmenu] FName's TCHAR constructor: %zu matches", ctors.size());
            return Fail("FName's constructor not found");
        }
        const auto mallocs = FindAll(game.text, ParsePattern(kMalloc));
        const auto frees = FindAll(game.text, ParsePattern(kFree));
        if (mallocs.empty() || frees.empty()) return Fail("FMemory::Malloc / Free not found");
        uint8_t* const gm = RipTarget(mallocs[0] + kMallocG1, 3, 7);
        for (uint8_t* m : mallocs)
            if (RipTarget(m + kMallocG1, 3, 7) != gm || RipTarget(m + kMallocG2, 3, 7) != gm) return Fail("FMemory::Malloc's GMalloc differs");
        for (uint8_t* f : frees)
            if (RipTarget(f + kFreeG1, 3, 7) != gm || RipTarget(f + kFreeG2, 3, 7) != gm) return Fail("FMemory::Free's GMalloc differs");
        if (!game.data.Contains(gm)) return Fail("GMalloc is not in the game's data");
        uint8_t* const array = FindObjectArrayFromDtor();
        if (!array) return Fail("GUObjectArray not found");
        uint8_t* const setter = AltarCommandSetter();
        if (!setter) return Fail("Altar's command setter not found (FrameRateLimitAtStart's frame generation switch)");
        s.set = reinterpret_cast<SetFn>(setter);
        // AddOrUpdateSettingsToSave (by its registered name) and the settings save's writer
        uint8_t* const exec = FindNativeByName("AddOrUpdateSettingsToSave", kAddOrUpdateExec);
        if (!exec) return Fail("AddOrUpdateSettingsToSave's exec function not found");
        uint8_t* const addOrUpdate = RipTarget(exec + kAddOrUpdateCall, 1, 5);
        if (!game.text.Contains(addOrUpdate) || !MatchesAt(addOrUpdate, ParsePattern(kAddOrUpdate)) ||
            At<uint32_t>(addOrUpdate, kAddOrUpdateMap) != 0x1C8)
            return Fail("AddOrUpdateSettingsToSave differs (SavedSettings at +0x1C8 expected)");
        const auto writers = FindAll(game.text, ParsePattern(kSaveWriter));
        uint8_t* writer = nullptr;
        for (uint8_t* w : writers)
            if (MatchesAt(w + kSaveWriterSlotAt, ParsePattern(kSaveWriterSlot))) {
                if (writer) return Fail("two settings save writers");
                writer = w;
            }
        if (!writer) return Fail("the settings save's writer not found");
        uint8_t* const slot = RipTarget(writer + kSaveWriterSlotAt + kSaveWriterSlotLoad, 4, 8);
        if (RipTarget(writer + kSaveWriterSlotAt, 2, 6) != slot + 8 || !game.data.Contains(slot))
            return Fail("the settings save's slot name differs");
        // UGameplayStatics::SpawnObject (by its registered name): NewObject for the HDR table
        uint8_t* const spawnExec = FindNativeByName("SpawnObject", kSpawnObjectExec);
        uint8_t* const spawn = spawnExec ? RipTarget(spawnExec + kSpawnObjectCall, 1, 5) : nullptr;
        if (!spawn || !game.text.Contains(spawn) || !MentionsNewObject(spawn)) return Fail("UGameplayStatics::SpawnObject not found");
        s.addOrUpdate = reinterpret_cast<AddOrUpdateFn>(addOrUpdate);
        s.saveWriter = reinterpret_cast<SaveWriterFn>(writer);
        s.saveSlot = reinterpret_cast<const FStr*>(slot);
        s.spawn = reinterpret_cast<SpawnFn>(spawn);
        s.fromStringAt = impl;
        s.fnameAt = ctors[0];
        s.mallocAt = mallocs[0];
        s.freeAt = frees[0];
        s.gmalloc = gm;
        s.objectArray = array;
        s.fromString = reinterpret_cast<FromStringFn>(impl);
        s.fname = reinterpret_cast<FNameFn>(ctors[0]);
        s.malloc = reinterpret_cast<MallocFn>(mallocs[0]);
        s.found = true;
        Log("[hdrmenu] FText::FromString exe+0x%llX, FName(TCHAR*) exe+0x%llX, FMemory::Malloc exe+0x%llX (GMalloc exe+0x%llX), "
            "GUObjectArray exe+0x%llX, SpawnObject exe+0x%llX",
            static_cast<unsigned long long>(impl - game.base), static_cast<unsigned long long>(ctors[0] - game.base),
            static_cast<unsigned long long>(mallocs[0] - game.base), static_cast<unsigned long long>(gm - game.base),
            static_cast<unsigned long long>(array - game.base), static_cast<unsigned long long>(spawn - game.base));
    }
    if (out) {
        out->fromString = s.fromStringAt;
        out->fnameCtor = s.fnameAt;
        out->malloc = s.mallocAt;
        out->free = s.freeAt;
        out->gmalloc = s.gmalloc;
        out->objectArray = s.objectArray;
        out->setter = reinterpret_cast<uint8_t*>(s.set);
        out->addOrUpdate = reinterpret_cast<uint8_t*>(s.addOrUpdate);
        out->saveWriter = reinterpret_cast<uint8_t*>(s.saveWriter);
        out->saveSlot = reinterpret_cast<const uint8_t*>(s.saveSlot);
        out->spawnObject = reinterpret_cast<uint8_t*>(s.spawn);
    }
    return true;
}

void HdrMenuMonitorTick()
{
    if (!s.installed || s.saveRead.load(std::memory_order_relaxed)) return;
    // the monitor's first tick, about a second after the DLL loaded; the game loads its saved options and writes them
    // back about 20 s after the DLL loaded (probe games). Not in DllMain: shell32 under the loader lock.
    s.save = ReadSettingsSave();
    s.saveRead.store(true, std::memory_order_release);
    Log("[hdrmenu] saved options: Save_Settings.sav %s (%zu bytes)", s.save.empty() ? "not read" : "read", s.save.size());
}

namespace {

// NVIDIA's Ray Reconstruction library: nvngx_dlssd.dll in the game's DLSS plugin folder (three folders up from the exe:
// Engine\Plugins\Marketplace\nvidia\DLSS\DLSS\Binaries\ThirdParty\Win64). The game does not ship it; Ray Reconstruction was
// only tested with it there.
bool RayReconstructionLibrary(std::wstring* where)
{
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring root(exe, n);
        for (int up = 0; up < 4 && !root.empty(); ++up) root.resize(root.find_last_of(L'\\') == std::wstring::npos ? 0 : root.find_last_of(L'\\'));
        const std::wstring dll = root + L"\\Engine\\Plugins\\Marketplace\\nvidia\\DLSS\\DLSS\\Binaries\\ThirdParty\\Win64\\nvngx_dlssd.dll";
        const DWORD a = GetFileAttributesW(dll.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { *where = dll; return true; }
    }
    return false;
}

}  // namespace

namespace {

// The game registers its own int console variables through the console manager's vtable +0x18 (RegisterConsoleVariable,
// int), as OPF.HudAspect is registered here. Checked at one known site: r.VT.MaxUploadsPerFrame.Streaming's registration
// (lea rdx,[name]; xor r8d,r8d; mov [rsp+30h],rdi; mov dword [rsp+20h],60h; call [rax+18h]; ...), exactly once.
constexpr wchar_t kIntRegName[] = L"r.VT.MaxUploadsPerFrame.Streaming";
constexpr char kIntRegister[] =
    "48 8D 15 ?? ?? ?? ?? 45 33 C0 48 89 7C 24 30 C7 44 24 20 60 00 00 00 FF 50 18 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
    "48 89 0D ?? ?? ?? ??";

bool IntRegistrationAsExpected()
{
    const GameModule& game = Game();
    int n = 0;
    for (uint8_t* p : FindAll(game.text, ParsePattern(kIntRegister))) {
        const auto* const name = reinterpret_cast<const wchar_t*>(RipTarget(p, 3, 7));
        if (game.rdata.Contains(name) && wcscmp(name, kIntRegName) == 0) ++n;
    }
    return n == 1;
}

}  // namespace

bool InstallHdrMenu(bool hdrRows, bool rayReconstruction, bool hudAspect, bool beforeGameCode)
{
    if (!hdrRows && !rayReconstruction && !hudAspect) return false;
    if (!FindHdrMenu(nullptr)) return false;
    if (hudAspect) {
        uint8_t* const pe = FindProcessEvent();
        if (!pe) {
            Log("[hudaspect] UObject::ProcessEvent not found (once) by its prologue: HudAspectRow not installed");
            hudAspect = false;
        } else if (!IntRegistrationAsExpected()) {
            Log("[hudaspect] the console manager's int registration (r.VT.MaxUploadsPerFrame.Streaming's) not as in build 19115871: HudAspectRow not installed");
            hudAspect = false;
        } else {
            h.processEvent = reinterpret_cast<ProcessEventFn>(pe);
            Log("[hudaspect] installed: UObject::ProcessEvent exe+0x%llX; the row is made before Show FPS once the Graphics tables exist",
                static_cast<unsigned long long>(pe - Game().base));
        }
    }
    s.hudWanted = hudAspect;
    std::wstring where;
    if (rayReconstruction && !RayReconstructionLibrary(&where)) {
        Log("[rrrow] NVIDIA's Ray Reconstruction library not found (nvngx_dlssd.dll in the game's DLSS plugin folder, "
            "Engine\\Plugins\\Marketplace\\nvidia\\DLSS\\DLSS\\Binaries\\ThirdParty\\Win64): DlssRayReconstruction not installed");
        rayReconstruction = false;
    }
    if (!hdrRows && !rayReconstruction && !hudAspect) return false;
    s.hdrWanted = hdrRows;
    s.rrWanted = rayReconstruction;
    s.installed = true;
    if (hdrRows) Log("[hdrmenu] installed: the HDR section is made on the game thread once the Graphics tables exist");
    if (rayReconstruction) {
        Log("[rrrow] installed (%ls): \"DLSS - Ray Reconstruction\" is added to the Advanced section once the Graphics tables exist",
            where.c_str());
        ApplyRrLock(beforeGameCode);
    }
    return true;
}

bool RrLockInfo(uint8_t** call, uint8_t** original, uint8_t** stub, void** hook)
{
    if (!g_lock.call && !ApplyRrLock(true)) return false;
    *call = g_lock.call;
    *original = reinterpret_cast<uint8_t*>(g_lock.original);
    *stub = g_lock.stub;
    *hook = reinterpret_cast<void*>(&LockLookupWrapper);
    return true;
}

void RrLockTestSetup(void* original) { g_lock.original = reinterpret_cast<LookupFn>(original); }

void HdrMenuFrameEnd()
{
    if (!s.installed) return;
    ++s.frames;
    if (s.hudWanted) HudTick();
    if (s.failed) return;
    if (s.syncAt && s.frames >= s.syncAt) FinishSync();
    const bool hdrDone = !s.hdrWanted || s.patched, rrDone = !s.rrWanted || s.rrFailed || s.rrPatched;
    const bool hudDone = !s.hudWanted || s.hudFailed || s.hudPatched;
    if (hdrDone && rrDone && hudDone) {
        if (s.frames % 300 != 0) return;
        bool lost = false;
        if (s.hdrWanted && !StillPatched()) {
            Log("[hdrmenu] the Graphics category table was reloaded: looking for it again");
            s.patched = nullptr;
            s.patchedIndex = -1;
            lost = true;
        }
        if (s.rrWanted && !s.rrFailed && !RrStillPatched()) {
            Log("[rrrow] the upscaler table was reloaded: looking for it again");
            s.rrPatched = nullptr;
            s.rrPatchedIndex = -1;
            lost = true;
        }
        if (s.hudWanted && !s.hudFailed && !HudStillPatched()) {
            Log("[hudaspect] the General table was reloaded: looking for it again");
            s.hudPatched = nullptr;
            s.hudPatchedIndex = -1;
            lost = true;
        }
        if (!lost) return;
        s.table = nullptr;
        s.catTable = nullptr;
        s.upTable = nullptr;
        s.cursor = 0;
    }
    if (!s.namesReady) {
        if (s.frames % 4 != 0) return;  // the names exist once the settings assets are loaded
        if (!MakeNames()) return;
        s.namesReady = true;
        s.firstPass = true;
        Log("[hdrmenu] the Graphics tables' names exist (frame %u): looking for them", s.frames);
    }
    const int32_t num = At<int32_t>(s.objectArray, kArrayNum);
    Item** const chunks = reinterpret_cast<Item**>(Ptr(s.objectArray, kArrayChunks));
    if (!chunks || num <= 0) return;
    // the first pass in one frame, later passes spread over frames
    const uint32_t per = s.firstPass ? static_cast<uint32_t>(num) : kScanPerFrame;
    const uint32_t end = s.cursor + per < static_cast<uint32_t>(num) ? s.cursor + per : static_cast<uint32_t>(num);
    for (uint32_t i = s.cursor; i < end; ++i) {
        Item* const chunk = chunks[i / kChunkItems];
        if (!chunk) continue;
        uint8_t* const obj = chunk[i % kChunkItems].object;
        if (!obj || !Ptr(obj, kObjClass)) continue;
        const NameId name = ObjName(obj);
        if (name == s.n.tableName && ClassName(obj) == s.n.tableClass) s.table = obj;
        else if (name == s.n.upName && ClassName(obj) == s.n.tableClass) s.upTable = obj;
        else if (name == s.n.catName && ClassName(obj) == s.n.catClass) s.catTable = obj;
        else if (ClassName(obj) == s.n.gusClass && !(At<uint32_t>(obj, kObjFlags) & kRfClassDefaultObject)) s.gus = obj;
    }
    s.cursor = end;
    if (s.cursor < static_cast<uint32_t>(num)) return;
    s.cursor = 0;  // a whole pass
    s.firstPass = false;
    if (s.hdrWanted && !s.patched && s.table && s.catTable && s.gus) Patch(s.table, s.catTable, s.gus);
    if (s.rrWanted && !s.rrFailed && !s.rrPatched && s.upTable && s.catTable && s.gus) PatchRr(s.upTable, s.catTable, s.gus);
    if (s.hudWanted && !s.hudFailed && !s.hudPatched && s.table && s.catTable && s.gus) PatchHud(s.table, s.catTable, s.gus);
    s.table = nullptr;
    s.catTable = nullptr;
    s.upTable = nullptr;
}


bool HudAspectIntRegistrationFound() { return IntRegistrationAsExpected(); }

}  // namespace opf
