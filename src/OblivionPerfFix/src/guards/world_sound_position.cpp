// WorldSoundPositionFix (GitHub issue #52, a game bug, also without the mod; worker gh52build 2026-11-20; default off until
// it has been tested in play). Evidence: findings tags gh52, gh52fix, gh52att, gh52build.
//
// What the player hears: an NPC lighting a torch anywhere in the loaded world (NPCs light their torches when the game creates
// them, often 100-230 m ahead) is heard at full volume in both ears; deer, daedra-spider and ogre footsteps and 11 dungeon traps
// fade with distance but always sound centred, never from their side.
//
// The cause is in the game's Wwise data (the per-event SoundBanks in the game's pak, read with our own parser; 136,707 banks):
// each listed sound container overrides its parent's positioning with "3D spatialization: none". The NPC variant of
// `char_fol_torch_equip` (container 0x0BCA8316, the switch Player = NPC, which is also the default) has attenuation switched
// off as well, although the designers attached a 20 m distance curve to it: flat and centred at any distance. The player's own
// variants (1st person 2D, 3rd person 3D with 2 m) and every other torch sound are set correctly. The footstep and trap
// containers keep their distance curves (21-60 m), only the panning is off.
//
// The fix: when Wwise loads a SoundBank (the game calls the Wwise SDK's AK::SoundEngine::LoadBankMemoryView / LoadBankMemoryCopy,
// which the game's exe exports by name: 8 overloads, each detoured at its entry), the bank's HIRC chunk is walked (object headers
// only) before Wwise reads it. For each listed container id the object's type, parent, positioning-override flags, its
// attenuation id and the 3D flags byte are checked against what the game ships; only then that one byte is changed: 3D
// spatialization "position + orientation" (the value of their correctly set parents), and for the torch's NPC container also
// attenuation on (its own 20 m curve). A bank whose bytes differ (another Wwise version, a re-authored sound) is loaded
// unchanged. Nothing on disk changes; nothing else in any bank is touched; every other sound plays as before.
#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {

constexpr uint32_t kBankVersion = 150;  // BKHD dwBankGeneratorVersion of the game's banks (Wwise 2023.1)
constexpr uint8_t kPropAttenuation = 0x55;
constexpr uint8_t kSpatialPosOrient = 0x02;  // uBits3d bits 0-1: 3D spatialization mode 2 = position + orientation
constexpr uint8_t kAttenuationOn = 0x08;     // uBits3d bit 3: attenuation enabled

// HIRC object types used here
constexpr uint8_t kSound = 2, kRanSeq = 5;

}  // namespace

// The listed containers (ids are Wwise short ids of the game's objects; old = the byte the game ships)
const WorldSoundTarget kWorldSoundTargets[] = {
    {0x0BCA8316, kRanSeq, 0x0E0B72D9, 0x15942315, 0x80, 0x80 | kSpatialPosOrient | kAttenuationOn, "torch lit by an NPC (20 m)"},
    {0x121578BC, kRanSeq, 0x210871A2, 0x3B8BC7B9, 0x88, 0x88 | kSpatialPosOrient, "deer footsteps (60 m)"},
    {0x0330BC32, kRanSeq, 0x07F032AF, 0x3B8BC7B9, 0x08, 0x08 | kSpatialPosOrient, "daedra spider footsteps (60 m)"},
    {0x39635892, kRanSeq, 0x3BBD798D, 0x03FE78AC, 0x08, 0x08 | kSpatialPosOrient, "ogre footsteps (21 m)"},
    {0x17C5C5E5, kSound, 0x3DC3B884, 0x2922A303, 0x08, 0x08 | kSpatialPosOrient, "trap: blunt object swish (30 m)"},
    {0x288241B8, kRanSeq, 0x037F1AC2, 0x2922A303, 0x88, 0x88 | kSpatialPosOrient, "trap: gears (30 m)"},
    {0x0B3518D2, kRanSeq, 0x037F1AC2, 0x37517387, 0x88, 0x88 | kSpatialPosOrient, "trap: hall spikes (20 m)"},
    {0x1EF4C97C, kSound, 0x1C57DBBD, 0x2922A303, 0x08, 0x08 | kSpatialPosOrient, "trap: iron scrape on stone (30 m)"},
    {0x0DD98FF7, kSound, 0x38976342, 0x2922A303, 0x08, 0x08 | kSpatialPosOrient, "trap: iron blade swish (30 m)"},
    {0x3BCE953C, kRanSeq, 0x037F1AC2, 0x37517387, 0x88, 0x88 | kSpatialPosOrient, "trap: mine (20 m)"},
    {0x2A329A40, kRanSeq, 0x037F1AC2, 0x2922A303, 0x08, 0x08 | kSpatialPosOrient, "trap: root worm (30 m)"},
    {0x3AC9B299, kSound, 0x270564DC, 0x1E00FFB8, 0x08, 0x08 | kSpatialPosOrient, "trap: ruins floor back (20 m)"},
    {0x38B40273, kRanSeq, 0x037F1AC2, 0x1E00FFB8, 0x88, 0x88 | kSpatialPosOrient, "trap: ruins floor forward (20 m)"},
    {0x37EAEAD1, kRanSeq, 0x037F1AC2, 0x2922A303, 0x88, 0x88 | kSpatialPosOrient, "trap: spikes channel (30 m)"},
    {0x25B900EA, kSound, 0x0DCD4672, 0x2922A303, 0x08, 0x08 | kSpatialPosOrient, "trap: stone switch (30 m)"},
};
const size_t kNumWorldSoundTargets = sizeof kWorldSoundTargets / sizeof kWorldSoundTargets[0];

namespace {

struct Reader {
    const uint8_t* p;
    size_t n, o = 0;
    bool ok = true;
    bool Need(size_t k)
    {
        if (!ok || o + k > n) ok = false;
        return ok;
    }
    uint8_t U8() { return Need(1) ? p[o++] : 0; }
    uint32_t U32()
    {
        uint32_t v = 0;
        if (Need(4)) { memcpy(&v, p + o, 4); o += 4; }
        return v;
    }
    void Skip(size_t k)
    {
        if (Need(k)) o += k;
    }
};

// The offset (within the object's body, after its id) of a node's uBits3d byte, or 0 when the object does not have the
// expected layout. Checks parent, the positioning-override flags and the attenuation property on the way.
size_t Bits3dOffset(const uint8_t* body, size_t len, const WorldSoundTarget& t)
{
    Reader r{body, len};
    if (t.type == kSound) {
        const uint32_t plugin = r.U32();
        r.U8();      // stream type
        r.U32();     // source id
        r.U32();     // in-memory media size
        r.U8();      // source bits
        if ((plugin & 0xF) == 2) r.Skip(r.U32());  // a source plugin's parameter block
    } else if (t.type != kRanSeq) {
        return 0;
    }
    r.U8();  // bIsOverrideParentFX
    const uint8_t nfx = r.U8();
    if (nfx) r.Skip(1 + 6u * nfx);
    r.U8();  // bIsOverrideParentMetadata
    const uint8_t nmeta = r.U8();
    r.Skip(6u * nmeta);
    r.U32();  // OverrideBusId
    if (r.U32() != t.parent) return 0;
    r.U8();  // priority bits
    const uint8_t nprops = r.U8();
    if (!r.Need(nprops + 4u * nprops)) return 0;
    uint32_t att = 0;
    bool haveAtt = false;
    for (uint8_t i = 0; i < nprops; ++i) {
        if (body[r.o + i] == kPropAttenuation) {
            memcpy(&att, body + r.o + nprops + 4u * i, 4);
            haveAtt = true;
        }
    }
    r.Skip(nprops + 4u * nprops);
    const uint8_t nranges = r.U8();
    r.Skip(nranges + 8u * nranges);
    const uint8_t posBits = r.U8();
    if (!r.ok || (posBits & 3) != 3 || ((posBits >> 5) & 3) != 0) return 0;  // override + listener routing, emitter-positioned
    if (!haveAtt || att != t.attenuation) return 0;
    return r.Need(1) ? r.o : 0;
}

std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_banks{0}, g_patched{0}, g_already{0}, g_mismatch{0}, g_readOnly{0}, g_otherVersion{0};
std::atomic<uint32_t> g_seen{0};  // bit per target: logged once
bool g_installed = false;

using LoadFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
constexpr int kNumLoads = 8;
LoadFn g_orig[kNumLoads] = {};
uint8_t* g_fn[kNumLoads] = {};
std::atomic<uint64_t> g_calls[kNumLoads];

// The 8 overloads (MSVC names as the exe exports them); in all of them the bank's bytes are rcx and its size edx
constexpr const char* kLoadNames[kNumLoads] = {
    "?LoadBankMemoryView@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIAEAI@Z",
    "?LoadBankMemoryView@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIAEAI1@Z",
    "?LoadBankMemoryView@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIP6AXI0W43@PEAX@Z2AEAI@Z",
    "?LoadBankMemoryView@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIP6AXI0W43@PEAX@Z2AEAI4@Z",
    "?LoadBankMemoryCopy@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIAEAI@Z",
    "?LoadBankMemoryCopy@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIAEAI1@Z",
    "?LoadBankMemoryCopy@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIP6AXI0W43@PEAX@Z2AEAI@Z",
    "?LoadBankMemoryCopy@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIP6AXI0W43@PEAX@Z2AEAI4@Z",
};

bool Writable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    const uint8_t* a = static_cast<const uint8_t*>(p);
    const uint8_t* const end = a + n;
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xFF;
        if ((mbi.Protect & PAGE_GUARD) || !(prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY ||
                                            prot == PAGE_EXECUTE_WRITECOPY))
            return false;
        a = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}


void OnBank(int which, const void* data, uint32_t size)
{
    g_calls[which].fetch_add(1, std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_relaxed) || !data || size < 16) return;
    g_banks.fetch_add(1, std::memory_order_relaxed);
    WorldSoundBankResult r{};
    bool readOnly = false;
    __try {
        // the bank's bytes are read first (the scan never writes); a listed object found -> written only if the pages are writable
        WorldSoundScanBank(static_cast<const uint8_t*>(data), size, &r, nullptr);
        if (r.toPatch && !Writable(data, size)) {
            g_readOnly.fetch_add(1, std::memory_order_relaxed);
            readOnly = true;
        } else if (r.toPatch) {
            WorldSoundScanBank(static_cast<const uint8_t*>(data), size, &r, static_cast<uint8_t*>(const_cast<void*>(data)));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (r.otherVersion) g_otherVersion.fetch_add(1, std::memory_order_relaxed);
    g_patched.fetch_add(r.patched, std::memory_order_relaxed);
    g_already.fetch_add(r.already, std::memory_order_relaxed);
    g_mismatch.fetch_add(r.mismatch, std::memory_order_relaxed);
    for (size_t i = 0; i < kNumWorldSoundTargets; ++i) {
        const uint32_t bit = 1u << i;
        if (!(r.touchedMask & bit) || (g_seen.fetch_or(bit) & bit)) continue;
        const char* const how = (r.mismatchMask & bit)  ? "NOT changed: its bytes differ from what the game ships"
                                 : readOnly                ? "NOT changed: the game loaded its sound data read-only"
                                 : (r.alreadyMask & bit)   ? "already 3D in the game's data, nothing to change"
                                                           : "changed to 3D";
        Log("[worldpos] %s: container %08X %s", kWorldSoundTargets[i].what, kWorldSoundTargets[i].id, how);
    }
}

#define OPF_WSP_HOOK(i)                                                                                                     \
    uint64_t LoadHook##i(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, \
                         uint64_t a9, uint64_t a10)                                                                         \
    {                                                                                                                       \
        OnBank(i, reinterpret_cast<const void*>(a1), static_cast<uint32_t>(a2));                                            \
        return g_orig[i](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);                                                          \
    }
OPF_WSP_HOOK(0)
OPF_WSP_HOOK(1)
OPF_WSP_HOOK(2)
OPF_WSP_HOOK(3)
OPF_WSP_HOOK(4)
OPF_WSP_HOOK(5)
OPF_WSP_HOOK(6)
OPF_WSP_HOOK(7)
#undef OPF_WSP_HOOK
constexpr LoadFn kHooks[kNumLoads] = {&LoadHook0, &LoadHook1, &LoadHook2, &LoadHook3, &LoadHook4, &LoadHook5, &LoadHook6, &LoadHook7};

// mov [rsp+disp8], r64 (REX.W 89 /r, ModRM mod 01 r/m 100, SIB rsp): 5 bytes, position-independent
bool PrologueOk(const uint8_t* fn)
{
    return (fn[0] == 0x48 || fn[0] == 0x4C) && fn[1] == 0x89 && (fn[2] & 0xC7) == 0x44 && fn[3] == 0x24;
}

// An entry detour in two steps: Trampoline() writes the slot (the first 5 bytes, a jump back to fn + 5; at +32 a jump to the
// hook) while the slot is still writable; once every slot is built and made executable, Redirect() starts fn with a jmp to
// slot + 32 (one aligned qword written atomically when it fits: safe while the game runs).
bool Trampoline(uint8_t* fn, void* hook, uint8_t* slot)
{
    if (!PrologueOk(fn)) return false;
    const int64_t rel = (slot + 32) - (fn + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + 5;
    memcpy(slot, fn, 5);
    memcpy(slot + 5, jmpAbs, sizeof jmpAbs);
    memcpy(slot + 11, &resume, sizeof resume);
    memcpy(slot + 32, jmpAbs, sizeof jmpAbs);
    memcpy(slot + 38, &hook, sizeof hook);
    return true;
}
bool Redirect(uint8_t* fn, uint8_t* slot, bool beforeGameCode)
{
    uint8_t jmp[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel32 = static_cast<int32_t>((slot + 32) - (fn + 5));
    memcpy(jmp + 1, &rel32, sizeof rel32);
    DWORD old;
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
        return ok;
    }
    return beforeGameCode && WriteCode(fn, std::span<const uint8_t>(jmp, sizeof jmp));
}

}  // namespace

// Walks a bank's chunks; for every listed object checks its bytes and, with `out` (the same buffer, writable), changes the
// 3D flags byte. Never reads outside [data, data + size).
void WorldSoundScanBank(const uint8_t* data, size_t size, WorldSoundBankResult* res, uint8_t* out)
{
    WorldSoundBankResult r{};
    size_t o = 0;
    bool versionOk = false;
    while (o + 8 <= size) {
        uint32_t len = 0;
        memcpy(&len, data + o + 4, 4);
        const uint8_t* const c = data + o + 8;
        if (len > size - o - 8) break;
        if (!memcmp(data + o, "BKHD", 4)) {
            uint32_t v = 0;
            if (len >= 4) memcpy(&v, c, 4);
            versionOk = v == kBankVersion;
            if (!versionOk) { r.otherVersion = true; break; }
        } else if (!memcmp(data + o, "HIRC", 4) && versionOk && len >= 4) {
            uint32_t count = 0;
            memcpy(&count, c, 4);
            size_t p = 4;
            for (uint32_t k = 0; k < count && p + 9 <= len; ++k) {
                const uint8_t type = c[p];
                uint32_t osz = 0, id = 0;
                memcpy(&osz, c + p + 1, 4);
                if (osz < 4 || osz > len - p - 5) break;
                memcpy(&id, c + p + 5, 4);
                for (size_t i = 0; i < kNumWorldSoundTargets; ++i) {
                    const WorldSoundTarget& t = kWorldSoundTargets[i];
                    if (t.id != id) continue;
                    const uint32_t bit = 1u << i;
                    const uint8_t* const body = c + p + 9;
                    const size_t at = type == t.type ? Bits3dOffset(body, osz - 4, t) : 0;
                    const uint8_t now = at ? body[at] : 0;
                    if (at && now == t.newBits) { ++r.already; r.touchedMask |= bit; r.alreadyMask |= bit; }
                    else if (at && now == t.oldBits) {
                        ++r.toPatch;
                        r.touchedMask |= bit;
                        if (out) {
                            out[(body - data) + at] = t.newBits;
                            ++r.patched;
                        }
                    } else { ++r.mismatch; r.mismatchMask |= bit; r.touchedMask |= bit; }
                }
                p += 5 + osz;
            }
        }
        o += 8 + static_cast<size_t>(len);
    }
    if (out) r.toPatch = 0;
    *res = r;
}

bool InstallWorldSoundPosition(bool beforeGameCode)
{
    if (g_installed) return true;
    const HMODULE exe = reinterpret_cast<HMODULE>(Game().base);
    uint8_t* fns[kNumLoads] = {};
    for (int i = 0; i < kNumLoads; ++i) {
        fns[i] = reinterpret_cast<uint8_t*>(GetProcAddress(exe, kLoadNames[i]));
        if (!fns[i] || !Game().text.Contains(fns[i]) || !PrologueOk(fns[i])) {
            Log("[worldpos] the Wwise bank loader AK::SoundEngine::LoadBankMemory%s (overload %d) %s: WorldSoundPositionFix not available",
                i < 4 ? "View" : "Copy", i % 4, fns[i] ? "starts differently" : "is not exported");
            return false;
        }
    }
    uint8_t* const region = AllocNearGame(64 * kNumLoads);
    if (!region) { Log("[worldpos] no free memory near the game's code: WorldSoundPositionFix not available"); return false; }
    for (int i = 0; i < kNumLoads; ++i) {
        if (!Trampoline(fns[i], reinterpret_cast<void*>(kHooks[i]), region + 64 * i)) {
            Log("[worldpos] LoadBankMemory%s overload %d: no trampoline: WorldSoundPositionFix not available", i < 4 ? "View" : "Copy", i % 4);
            return false;
        }
        g_orig[i] = reinterpret_cast<LoadFn>(region + 64 * i);
        g_fn[i] = fns[i];
    }
    DWORD old;
    if (!VirtualProtect(region, 64 * kNumLoads, PAGE_EXECUTE_READ, &old)) {
        Log("[worldpos] the trampolines could not be made executable: WorldSoundPositionFix not available");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region, 64 * kNumLoads);
    int placed = 0;
    for (int i = 0; i < kNumLoads; ++i) {
        if (!Redirect(fns[i], region + 64 * i, beforeGameCode)) {
            Log("[worldpos] the detour of LoadBankMemory%s overload %d could not be placed", i < 4 ? "View" : "Copy", i % 4);
            break;
        }
        ++placed;
    }
    if (placed != kNumLoads) {
        // the placed ones pass every bank through unchanged while the key stays off
        Log("[worldpos] %d of %d bank loaders hooked: WorldSoundPositionFix not available", placed, kNumLoads);
        return false;
    }
    g_installed = true;
    Log("[worldpos] installed: the Wwise bank loaders (LoadBankMemoryView / Copy, 8 exports) hooked; %zu containers listed",
        kNumWorldSoundTargets);
    return true;
}

void SetWorldSoundPositionFix(bool on)
{
    if (on && !g_installed) return;
    if (g_on.exchange(on) != on) Log("[worldpos] WorldSoundPositionFix %s (banks loaded from now on)", on ? "on" : "off");
}


void LogWorldSoundPositionStats()
{
    if (!g_installed) return;
    // a line when a container was changed or refused (or another Wwise version seen), not for every bank the game loads
    static uint64_t last = 0;
    const uint64_t sum = g_patched + g_already + g_mismatch + g_readOnly + g_otherVersion;
    if (sum == last) return;
    last = sum;
    Log("[worldpos] stats: banks %llu, containers changed %llu (already %llu), bytes differ %llu, read-only banks %llu, other Wwise "
        "version %llu; loads View %llu/%llu/%llu/%llu Copy %llu/%llu/%llu/%llu",
        static_cast<unsigned long long>(g_banks.load()), static_cast<unsigned long long>(g_patched.load()),
        static_cast<unsigned long long>(g_already.load()), static_cast<unsigned long long>(g_mismatch.load()),
        static_cast<unsigned long long>(g_readOnly.load()), static_cast<unsigned long long>(g_otherVersion.load()),
        static_cast<unsigned long long>(g_calls[0].load()), static_cast<unsigned long long>(g_calls[1].load()),
        static_cast<unsigned long long>(g_calls[2].load()), static_cast<unsigned long long>(g_calls[3].load()),
        static_cast<unsigned long long>(g_calls[4].load()), static_cast<unsigned long long>(g_calls[5].load()),
        static_cast<unsigned long long>(g_calls[6].load()), static_cast<unsigned long long>(g_calls[7].load()));
}

bool WorldSoundPositionInfo(uint8_t* const** fns, int* n)
{
    *fns = g_fn;
    *n = kNumLoads;
    return g_installed;
}

void WorldSoundPositionTestCall(int which, void* data, uint32_t size, LoadFn_t fake)
{
    LoadFn const keep = g_orig[which];
    g_orig[which] = reinterpret_cast<LoadFn>(fake);
    const bool on = g_on.exchange(true);
    kHooks[which](reinterpret_cast<uint64_t>(data), size, 0, 0, 0, 0, 0, 0, 0, 0);
    g_on = on;
    g_orig[which] = keep;
}

}  // namespace opf
