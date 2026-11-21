// PhysicsSubstepCap ([Fixes] PhysicsSubstepCap, default 0 = the game's physics steps; 1 = at most one physics step for a
// frame of up to 30 ms; worker physkey 2026-11-20, from workers pawnbase's and physstep's measurements, S-065).
//
// What it changes: the game runs the engine's physics with substepping on, steps of at most 20 ms (the physics settings'
// MaxSubstepDeltaTime 0.02 from the game's own config) and at most 6 of them (MaxSubsteps 6): a frame of more than 20 ms
// is simulated as two physics steps back to back (UE 5.3 FChaosScene::SetUpForFrame passes both to the solver, which runs
// ceil(frame time / step) steps; PhysicsSolverBase.cpp), and the game thread waits for both at the end of its physics
// tick group. In a crowd a frame just over 20 ms then pays a second step (1.7-1.8 ms on a worker thread, waited for by
// the game thread), which makes the next frame slow too: a chain of slow frames. With 1 the step is at most 30 ms and at
// most 4 of them (MaxSubstepDeltaTime 0.03, MaxSubsteps 4): one step up to 30 ms, two up to 60 ms; the longest stretch
// of time one frame simulates stays 120 ms (4 x 30 = the game's 6 x 20), so a very long frame (a loading hitch) is
// clamped exactly as in the game.
//
// What it does not change: frames up to 20 ms (at a 60 fps cap nearly every frame) take one step of the frame's time, as
// in the game. Walking and riding are not affected (characters and horses move with their own movement code); simulated
// bodies, i.e. loose objects, ragdolls and characters' physically simulated parts (hit reactions, physical animation), are
// stepped by these settings. What differs is how they are integrated in a frame of 20-30 ms (one step instead
// of two half steps), and in a frame of 40-60 ms (two steps instead of three): a coarser integration of the same motion,
// the engine's own path (without substepping the engine's default is one step of up to 1/30 s). Not bit for bit; the
// same as the two Engine.ini lines
//   [/Script/Engine.PhysicsSettings]
//   MaxSubstepDeltaTime=0.03
//   MaxSubsteps=4
// (the same values: "0.03" in an ini parses to the same float).
//
// How: both are properties of the engine's physics settings object (UPhysicsSettings), which the engine reads every frame
// (UWorld::SetupPhysicsTickFunctions -> SetUpForFrame, UE 5.3 PhysLevel.cpp:150; the same in UE 5.4-5.7: 5.7 only adds
// networked physics prediction, which the game does not use); there is no console variable for them. On the game thread
// at UWorld::AddToWorld (the hook StreamRegisterGranularity / StreamInBudget / NavQueryPriority install; it runs at every
// area load, before anyone plays in it) the default object Default__PhysicsSettings is found by its name in the engine's
// object list (GUObjectArray, found from ~UObjectBase as the guards find it; names through FName's own constructor in find
// mode, which creates nothing; both found by pattern while the game starts, before other tools may patch the constructor),
// and its three properties by name in the class's reflection data
// (bSubstepping a BoolProperty, MaxSubstepDeltaTime a FloatProperty, MaxSubsteps an IntProperty, each checked by type,
// size and offset). Only when the object holds exactly the game's values (substepping on, 0.02, 6) are the two written
// (MaxSubsteps first), on the game thread between two frames, so every later frame reads the pair as from the ini lines.
// Switching the key off (live) puts the game's values back if the object still holds ours. A changed key is applied at
// the next AddToWorld (the next area that loads in).
//
// The player's own lines win: values other than the game's in the object (any Engine.ini line that changes either value,
// or a game update) are left alone, logged once. The player's Engine.ini (Saved\Config\Windows or WinGDK in the game's own
// folder, GameUserFolder: local AppData + ..\..\Documents\My Games\Oblivion Remastered, as the game derives it)
// is also read once, on the monitor thread: a MaxSubstepDeltaTime or MaxSubsteps line in its
// [/Script/Engine.PhysicsSettings] section (even one with the game's own values) also leaves the object alone.
//
// Clean off: GUObjectArray, FName's constructor, the object, its class or a property not found, or a type, size or offset
// that differs: a log line and nothing is written.
#include "common.h"

#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>
#include <windows.h>

namespace opf {
namespace {

// UE 5.3 layouts of this build (the same as StuckEffectLookFix and HdrMenuRows use; checked on dumps and by UE4SS):
// UObjectBase ObjectFlags +0x08, InternalIndex +0x0C, ClassPrivate +0x10, NamePrivate +0x18; UStruct SuperStruct +0x40,
// ChildProperties +0x50; FField ClassPrivate +0x08 (FFieldClass: its FName at +0), Next +0x18, NamePrivate +0x20;
// FProperty ArrayDim +0x30, ElementSize +0x34, Offset_Internal +0x44; FBoolProperty FieldSize +0x70, ByteOffset +0x71,
// FieldMask +0x73; GUObjectArray chunk table +0x10, NumElements +0x24; FUObjectItem 24 bytes {Object, Flags, ...}.
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18;
constexpr size_t kStructSuper = 0x40, kStructProps = 0x50;
constexpr size_t kFieldClass = 0x08, kFieldNext = 0x18, kFieldName = 0x20;
constexpr size_t kPropArrayDim = 0x30, kPropElementSize = 0x34, kPropOffset = 0x44;
constexpr size_t kBoolFieldSize = 0x70, kBoolByteOffset = 0x71, kBoolFieldMask = 0x73;
constexpr size_t kArrayChunks = 0x10, kArrayNum = 0x24, kItemSize = 24, kItemFlags = 8;
constexpr uint32_t kChunkItems = 64 * 1024;
constexpr int32_t kItemDead = (1 << 21) | (1 << 28) | (1 << 29);  // EInternalObjectFlags Garbage, Unreachable, PendingKill
constexpr int32_t kRfClassDefaultObject = 0x10;
constexpr uint32_t kMaxPropOffset = 0x4000;  // a sanity bound for the settings object's property offsets

// The game's values (its config; the engine's constructor has 1/60 and substepping off) and ours.
constexpr float kGameStep = 0.02f, kOurStep = 0.03f;
constexpr int32_t kGameSteps = 6, kOurSteps = 4;

constexpr int kMaxFindTries = 200;  // AddToWorld calls with the two names not there yet, before giving up (a walk that
                                    // finds no object while they exist gives up at once)

struct NameId {
    uint32_t index = 0, number = 0;
    bool operator==(const NameId& o) const { return index == o.index && number == o.number; }
    bool valid() const { return index != 0; }
};
using FNameFn = NameId* (*)(NameId* out, const wchar_t* name, int findType);

std::atomic<int> g_key{0};       // monitor thread -> game thread
// monitor thread -> game thread: 0 not looked up yet, 1 found (g_fname, g_array set), 2 not found
std::atomic<int> g_findState{0};
// monitor thread -> game thread: 0 not read yet, 1 the player's Engine.ini has no line for the two, 2 it has one
std::atomic<int> g_iniState{0};
FNameFn g_fname = nullptr;
uint8_t* g_array = nullptr;
const wchar_t* g_testIni = nullptr;  // self-test: the Engine.ini to read instead of the player's
bool g_startLooked = false;          // PhysicsSubstepCapFindAtStart ran (start-up, before the monitor thread)

// game thread only
uint8_t* g_obj = nullptr;                       // Default__PhysicsSettings
uint32_t g_offSub = 0, g_offStep = 0, g_offSteps = 0;
uint8_t g_subMask = 0;
bool g_gaveUp = false, g_ours = false, g_leftAloneLogged = false;
int g_findTries = 0;
int g_decidedKey = -1;
float g_origStep = 0;                           // what we replaced (the game's values, by the rule above)
int32_t g_origSteps = 0;
unsigned g_sets = 0;

template <class T> bool Rd(const void* p, T* v)
{
    if (!p) return false;
    __try {
        memcpy(v, p, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> bool Wr(void* p, const T& v)
{
    if (!p) return false;
    __try {
        memcpy(p, &v, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint8_t* Ptr(const uint8_t* p, size_t off)
{
    uint8_t* v = nullptr;
    return p && Rd(p + off, &v) ? v : nullptr;
}
NameId NameAt(const uint8_t* p)
{
    NameId n;
    if (p) Rd(p, &n);
    return n;
}
NameId Lookup(const wchar_t* text)
{
    NameId n;
    if (g_fname) g_fname(&n, text, 0);  // FNAME_Find: none until the name exists
    return n;
}

// The default object by its name, the class's name and the class-default-object flag (whole object list; game thread,
// where objects are destroyed, so every listed object is whole). Null if not there.
uint8_t* FindDefaultObject(const NameId& objName, const NameId& clsName)
{
    int32_t num = 0;
    uint8_t* const* chunks = nullptr;
    if (!Rd(g_array + kArrayNum, &num) || !Rd(g_array + kArrayChunks, &chunks) || !chunks || num <= 0) return nullptr;
    for (int32_t c = 0; c * static_cast<int64_t>(kChunkItems) < num; ++c) {
        uint8_t* chunk = nullptr;
        if (!Rd(chunks + c, &chunk) || !chunk) continue;
        const int32_t end = num - c * static_cast<int32_t>(kChunkItems) < static_cast<int32_t>(kChunkItems)
                                ? num - c * static_cast<int32_t>(kChunkItems) : static_cast<int32_t>(kChunkItems);
        for (int32_t i = 0; i < end; ++i) {
            const uint8_t* const item = chunk + kItemSize * i;
            uint8_t* obj = nullptr;
            int32_t itemFlags = 0;
            if (!Rd(item, &obj) || !obj || !Rd(item + kItemFlags, &itemFlags) || (itemFlags & kItemDead)) continue;
            if (!(NameAt(obj + kObjName) == objName)) continue;
            int32_t flags = 0, index = -1;
            if (!Rd(obj + kObjFlags, &flags) || !(flags & kRfClassDefaultObject)) continue;
            if (!Rd(obj + kObjIndex, &index) || index != c * static_cast<int32_t>(kChunkItems) + i) continue;
            const uint8_t* const cls = Ptr(obj, kObjClass);
            if (!cls || !(NameAt(cls + kObjName) == clsName)) continue;
            return obj;
        }
    }
    return nullptr;
}

// A property of the class (or a parent) by name and property type; its offset and element size checked.
const uint8_t* FindProp(const uint8_t* cls, const NameId& name, const NameId& type, int32_t size, uint32_t* offset)
{
    for (int depth = 0; cls && depth < 16; ++depth, cls = Ptr(cls, kStructSuper)) {
        const uint8_t* f = Ptr(cls, kStructProps);
        for (int n = 0; f && n < 512; ++n, f = Ptr(f, kFieldNext)) {
            if (!(NameAt(f + kFieldName) == name)) continue;
            const uint8_t* const fc = Ptr(f, kFieldClass);
            int32_t dim = 0, elem = 0, off = -1;
            if (!fc || !(NameAt(fc) == type) || !Rd(f + kPropArrayDim, &dim) || !Rd(f + kPropElementSize, &elem) ||
                !Rd(f + kPropOffset, &off) || dim != 1 || elem != size || off < 0x28 || static_cast<uint32_t>(off) > kMaxPropOffset ||
                off % size)
                return nullptr;
            *offset = static_cast<uint32_t>(off);
            return f;
        }
    }
    return nullptr;
}

// 1 found, 0 not yet (names or object not there), -1 never (a type or layout that differs)
int FindSettings()
{
    const NameId objName = Lookup(L"Default__PhysicsSettings"), clsName = Lookup(L"PhysicsSettings");
    if (!objName.valid() || !clsName.valid()) return 0;
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);
    uint8_t* const obj = FindDefaultObject(objName, clsName);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const double walkMs = freq.QuadPart ? (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart : 0.0;
    if (!obj) {  // the names exist, so the engine's native default objects do too (made at engine start): no retry
        Log("[physstep] Default__PhysicsSettings not found in the object list (walk %.2f ms): PhysicsSubstepCap off", walkMs);
        return -1;
    }
    const uint8_t* const cls = Ptr(obj, kObjClass);
    const NameId nSub = Lookup(L"bSubstepping"), nStep = Lookup(L"MaxSubstepDeltaTime"), nSteps = Lookup(L"MaxSubsteps");
    const NameId tBool = Lookup(L"BoolProperty"), tFloat = Lookup(L"FloatProperty"), tInt = Lookup(L"IntProperty");
    if (!nSub.valid() || !nStep.valid() || !nSteps.valid() || !tBool.valid() || !tFloat.valid() || !tInt.valid()) {
        Log("[physstep] the property names are not known to the engine: PhysicsSubstepCap off");
        return -1;
    }
    uint32_t offSub = 0, offStep = 0, offSteps = 0;
    const uint8_t* const pSub = FindProp(cls, nSub, tBool, 1, &offSub);
    const uint8_t* const pStep = FindProp(cls, nStep, tFloat, 4, &offStep);
    const uint8_t* const pSteps = FindProp(cls, nSteps, tInt, 4, &offSteps);
    uint8_t fieldSize = 0, byteOffset = 0xFF, fieldMask = 0;
    if (!pSub || !pStep || !pSteps || !Rd(pSub + kBoolFieldSize, &fieldSize) || !Rd(pSub + kBoolByteOffset, &byteOffset) ||
        !Rd(pSub + kBoolFieldMask, &fieldMask) || fieldSize != 1 || byteOffset != 0 || !fieldMask || offStep == offSteps ||
        offSub == offStep || offSub == offSteps) {
        Log("[physstep] PhysicsSettings' bSubstepping / MaxSubstepDeltaTime / MaxSubsteps not found as expected (bool %s, float %s, "
            "int %s): PhysicsSubstepCap off", pSub ? "ok" : "missing", pStep ? "ok" : "missing", pSteps ? "ok" : "missing");
        return -1;
    }
    g_obj = obj;
    g_offSub = offSub;
    g_offStep = offStep;
    g_offSteps = offSteps;
    g_subMask = fieldMask;
    Log("[physstep] Default__PhysicsSettings found (bSubstepping +0x%X, MaxSubstepDeltaTime +0x%X, MaxSubsteps +0x%X; object list "
        "walk %.2f ms)", offSub, offStep, offSteps, walkMs);
    return 1;
}

// The player's Engine.ini: a MaxSubstepDeltaTime or MaxSubsteps line in [/Script/Engine.PhysicsSettings].
bool IniLines(const char* text, size_t len)
{
    if (len >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB &&
        static_cast<uint8_t>(text[2]) == 0xBF) {  // a UTF-8 byte order mark
        text += 3;
        len -= 3;
    }
    bool inSection = false;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e] != '\r') ++e;
        size_t b = i;
        while (b < e && (text[b] == ' ' || text[b] == '\t')) ++b;
        size_t t = e;
        while (t > b && (text[t - 1] == ' ' || text[t - 1] == '\t')) --t;
        if (b < t && text[b] == '[') {
            constexpr char kSection[] = "[/Script/Engine.PhysicsSettings]";
            inSection = t - b == sizeof(kSection) - 1 && _strnicmp(text + b, kSection, sizeof(kSection) - 1) == 0;
        } else if (inSection && b < t && text[b] != ';' && text[b] != '#') {
            if (text[b] == '+' || text[b] == '-' || text[b] == '.' || text[b] == '!') ++b;
            size_t k = b;
            while (k < t && text[k] != '=' && text[k] != ' ' && text[k] != '\t') ++k;
            size_t q = k;
            while (q < t && (text[q] == ' ' || text[q] == '\t')) ++q;
            if (q < t && text[q] == '=') {
                const size_t n = k - b;
                if ((n == 19 && _strnicmp(text + b, "MaxSubstepDeltaTime", 19) == 0) ||
                    (n == 11 && _strnicmp(text + b, "MaxSubsteps", 11) == 0))
                    return true;
            }
        }
        i = e + 1;
    }
    return false;
}

// 1 no line (or no file), 2 a line
int ReadIni(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER size{};
    int result = 1;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024) {
        const DWORD n = static_cast<DWORD>(size.QuadPart);
        char* const buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, n + 2));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, n, &got, nullptr)) {
            size_t len = got;
            if (len >= 2 && static_cast<uint8_t>(buf[0]) == 0xFF && static_cast<uint8_t>(buf[1]) == 0xFE) {  // UTF-16 LE: keep the low bytes
                size_t o = 0;
                for (size_t j = 2; j + 1 < len; j += 2) buf[o++] = buf[j + 1] ? '?' : buf[j];
                len = o;
            }
            if (IniLines(buf, len)) result = 2;
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return result;
}

int ReadPlayerIni()
{
    if (g_testIni) return ReadIni(g_testIni);
    // the folder the game itself uses (local AppData\..\..\Documents\My Games\Oblivion Remastered, as HdrMenuRows reads its
    // saved options); %USERPROFILE%\Documents\... only if that cannot be had
    std::wstring base = GameUserFolder();
    if (base.empty()) {
        wchar_t home[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        if (!n || n >= MAX_PATH) return 1;
        base = std::wstring(home) + L"\\Documents\\My Games\\Oblivion Remastered\\";
    }
    for (const wchar_t* platform : {L"Windows", L"WinGDK"}) {
        const std::wstring path = base + L"Saved\\Config\\" + platform + L"\\Engine.ini";
        if (ReadIni(path.c_str()) == 2) {
            Log("[physstep] %ls sets MaxSubstepDeltaTime or MaxSubsteps in [/Script/Engine.PhysicsSettings]: PhysicsSubstepCap leaves "
                "them as that file has them", path.c_str());
            return 2;
        }
    }
    return 1;
}

bool ReadSettings(bool* sub, float* step, int32_t* steps)
{
    uint8_t b = 0;
    if (!Rd(g_obj + g_offSub, &b) || !Rd(g_obj + g_offStep, step) || !Rd(g_obj + g_offSteps, steps)) return false;
    *sub = (b & g_subMask) != 0;
    return true;
}

}  // namespace

// Game thread, at every UWorld::AddToWorld call (StreamGranBeforeAdd), between frames.
void PhysicsSubstepCapOnGameThread()
{
    const int key = g_key.load(std::memory_order_relaxed);
    if (key == g_decidedKey) return;
    if (key == 0 && !g_ours) { g_decidedKey = 0; return; }  // off and nothing of ours in place
    if (g_gaveUp) { g_decidedKey = key; return; }
    const int findState = g_findState.load(std::memory_order_acquire);
    const int iniState = g_iniState.load(std::memory_order_acquire);
    if (findState == 0 || iniState == 0) return;            // the monitor thread has not looked yet: next call
    if (findState != 1) { g_gaveUp = true; g_decidedKey = key; return; }
    if (!g_obj) {
        const int r = FindSettings();
        if (r < 0 || (r == 0 && ++g_findTries >= kMaxFindTries)) {
            if (r == 0) Log("[physstep] the names Default__PhysicsSettings / PhysicsSettings never appeared: PhysicsSubstepCap off");
            g_gaveUp = true;
            g_decidedKey = key;
            return;
        }
        if (r == 0) return;                                 // retried at the next call
    }
    bool sub = false;
    float step = 0;
    int32_t steps = 0;
    if (!ReadSettings(&sub, &step, &steps)) {
        Log("[physstep] the physics settings could not be read: PhysicsSubstepCap off");
        g_gaveUp = true;
        g_decidedKey = key;
        return;
    }
    if (key == 1) {
        if (g_ours) {
            if (step != kOurStep || steps != kOurSteps) {   // changed by someone else since: theirs
                g_ours = false;
                Log("[physstep] the physics settings were changed after the fix (MaxSubstepDeltaTime %.4f, MaxSubsteps %d): left alone",
                    step, steps);
            }
        } else if (iniState == 1 && sub && step == kGameStep && steps == kGameSteps) {
            if (Wr(g_obj + g_offSteps, kOurSteps) && Wr(g_obj + g_offStep, kOurStep)) {
                g_ours = true;
                g_origStep = step;
                g_origSteps = steps;
                ++g_sets;
                Log("[physstep] PhysicsSubstepCap=1: MaxSubstepDeltaTime %.2f -> %.2f, MaxSubsteps %d -> %d (one physics step for frames "
                    "up to 30 ms instead of 20; the longest stretch per frame stays %.0f ms; set #%u)", kGameStep, kOurStep, kGameSteps,
                    kOurSteps, kOurStep * kOurSteps * 1000.0f, g_sets);
            } else {
                Log("[physstep] the physics settings could not be written: PhysicsSubstepCap off");
                g_gaveUp = true;
            }
        } else if (!g_leftAloneLogged) {
            g_leftAloneLogged = true;
            if (iniState == 2)
                Log("[physstep] PhysicsSubstepCap=1: the player's Engine.ini sets the physics steps (now substepping %d, MaxSubstepDeltaTime "
                    "%.4f, MaxSubsteps %d): left alone", sub ? 1 : 0, step, steps);
            else
                Log("[physstep] PhysicsSubstepCap=1: the physics settings are not the game's (substepping %d, MaxSubstepDeltaTime %.4f, "
                    "MaxSubsteps %d; the game's: 1, 0.02, 6): set by Engine.ini or a game update, left alone", sub ? 1 : 0, step, steps);
        }
    } else if (g_ours) {                                     // key switched off live
        g_ours = false;
        if (step == kOurStep && steps == kOurSteps) {
            if (Wr(g_obj + g_offStep, g_origStep) && Wr(g_obj + g_offSteps, g_origSteps)) {
                ++g_sets;
                Log("[physstep] PhysicsSubstepCap=0: the game's MaxSubstepDeltaTime %.2f and MaxSubsteps %d put back (set #%u)", g_origStep,
                    g_origSteps, g_sets);
            }
        } else {
            Log("[physstep] PhysicsSubstepCap=0: the physics settings were changed by someone else: left alone");
        }
    }
    if (key == 0) g_leftAloneLogged = false;
    g_decidedKey = key;
}

// Start-up (with the other installs, before the game's code runs; whenever the shared AddToWorld hook is installed, so the
// key can be switched on live): FName's constructor and GUObjectArray by their patterns (13 ms measured). Later the constructor's first bytes may be patched by other tools that load into the game
// (seen in a bench game 3 s after start-up), so the search is not left to the monitor thread.
void PhysicsSubstepCapFindAtStart()
{
    g_startLooked = true;
    g_fname = reinterpret_cast<FNameFn>(FindFNameCtor());
    g_array = FindObjectArrayFromDtor();
    if (!g_fname || !g_array) {
        Log("[physstep] %s not found: PhysicsSubstepCap off", !g_fname ? "FName's constructor" : "GUObjectArray");
        g_fname = nullptr;
        g_array = nullptr;
        return;
    }
    const uint8_t* const base = Game().base;
    Log("[physstep] FName(const TCHAR*) exe+0x%llX, GUObjectArray exe+0x%llX", static_cast<unsigned long long>(
        reinterpret_cast<const uint8_t*>(g_fname) - base), static_cast<unsigned long long>(g_array - base));
}

// Monitor thread, once per second: the key (live); applied by the game thread at the next AddToWorld call. The Engine.ini
// read runs here, once, when the key first reads 1.
void SetPhysicsSubstepCap(int value)
{
    if (value != 1) value = 0;
    if (value == 1 && g_findState.load(std::memory_order_relaxed) == 0) {
        if (!g_startLooked)
            Log("[physstep] PhysicsSubstepCap=1: its look-ups are made only while the game starts, with the AddToWorld hook (all of "
                "StreamRegisterGranularity, StreamInBudget, NavQueryPriority and this key were 0 then): takes a restart");
        g_iniState.store(ReadPlayerIni(), std::memory_order_release);
        g_findState.store(g_fname && g_array ? 1 : 2, std::memory_order_release);
    }
    const int before = g_key.exchange(value, std::memory_order_relaxed);
    static bool logged = false;  // monitor thread only
    if (value == 1 && before != 1 && !logged) {
        logged = true;
        if (StreamGranHookActive())
            Log("[physstep] PhysicsSubstepCap=1: applied on the game thread at the next area load");
        else
            Log("[physstep] PhysicsSubstepCap=1: not active (needs the AddToWorld hook, installed at start-up on the version.dll route with "
                "this or a streaming key on: see the [streamgran] lines)");
    }
}

// Self-test: a fake object list, FName constructor and Engine.ini instead of the game's.
void PhysicsSubstepCapTestSetup(uint8_t* fakeArray, void* fakeFname, const wchar_t* fakeIni)
{
    g_array = fakeArray;
    g_fname = reinterpret_cast<FNameFn>(fakeFname);
    g_testIni = fakeIni;
    g_startLooked = true;
    g_findState.store(0);
    g_iniState.store(0);
    g_obj = nullptr;
    g_offSub = g_offStep = g_offSteps = 0;
    g_subMask = 0;
    g_gaveUp = g_ours = g_leftAloneLogged = false;
    g_findTries = 0;
    g_decidedKey = -1;
    g_origStep = 0;
    g_origSteps = 0;
    g_sets = 0;
    g_key.store(0);
}
unsigned PhysicsSubstepCapSets() { return g_sets; }
bool PhysicsSubstepCapIniLines(const char* text) { return IniLines(text, strlen(text)); }

}  // namespace opf
