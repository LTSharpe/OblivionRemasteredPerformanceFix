// FrameRateLimitAtStart self-test (worker fpscap), included by selftest.cpp: the frame generation switch, the Frame Rate
// Limit sink and the saved-settings lookup on the mapped exe, the install as on the OBSE64 route (one atomic write at the
// switch's entry), then the hook's decisions with fakes (the original switch, the settings getter, the map's hash / find
// functions and the command setter); since 1.2.2 also the caller (FSR's sink or not), T's Reflex flag, the GPU vendor and
// the limit's value.
#pragma once

namespace fpc {

struct FStr {
    const wchar_t* data;
    int32_t num, max;
};
struct Pair {
    FStr name, value;
};
inline int g_origCalls = 0, g_setCalls = 0, g_findResult = 0;
inline bool g_lastOn = false, g_lastForce = true;
inline wchar_t g_setName[64], g_setValue[16];
inline uint8_t g_settings[0x200];
inline uint8_t g_elements[0x28 * 2];
inline const wchar_t kName[] = L"Altar.GraphicsOptions.FrameRateLimit";
inline const FStr g_nameStr{kName, 37, 37};

inline uint8_t g_state = 0;  // T's state byte: the fake switch sets it as T does
inline void FakeOrig(void*, bool on) { ++g_origCalls; g_lastOn = on; g_state = on ? 1 : 0; }
inline void* FakeGetter() { return g_settings; }
inline uint32_t FakeHash(uint32_t len, const wchar_t*) { return len; }
inline int32_t FakeFind(void* map, uint32_t hash, const FStr* key)
{
    if (map != g_settings + 0x1C8 || hash != 36 || key != &g_nameStr) return -2;  // wrong arguments: an index that cannot be used
    return g_findResult;
}
inline bool FakeSet(const Pair* p, bool force)
{
    ++g_setCalls;
    g_lastForce = force;
    wcsncpy_s(g_setName, p->name.data, _TRUNCATE);
    wcsncpy_s(g_setValue, p->value.data, _TRUNCATE);
    return true;
}

inline void FpsCapSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    if (!Installed(FindFrameCapStart(), "fpscap: frame generation switch, Frame Rate Limit sink and saved-settings lookup found")) return;
    {
        // 1.2.2: GRHIVendorId through T's own vendor check (the Reflex flag's initialisation block that stores T's flag)
        const uint32_t* const v = FrameCapStartTestVendor();
        Check(v && (game.data.Contains(v) || game.rdata.Contains(v)) && (reinterpret_cast<uintptr_t>(v) & 3) == 0,
              "fpscap: the GPU vendor variable found through the Reflex flag's initialisation in T (cmp GRHIVendorId,0x10DE)");
    }
    Check(InstallFrameCapStart(false, 1) && FrameCapStartTestPatched(),
          "fpscap: installs with frame generation's state off, also on the OBSE64 route (5-byte jump in one atomic write, trampoline)");
    // the saved settings: element 1 holds "60"
    static const wchar_t sixty[] = L"60";
    const FStr value{sixty, 3, 3};
    memcpy(g_elements + 0x28 + 0x10, &value, sizeof value);
    uint8_t* const elements = g_elements;
    memcpy(g_settings + 0x1C8, &elements, sizeof elements);
    // the cvar's value as T leaves it on "on" (0) and T's Reflex flag (0: AMD / no Reflex)
    static int32_t cvar = 0;
    static int32_t* const cvarSlot = &cvar;
    static uint8_t reflex = 0;
    static uint32_t vendor = 0x1002;  // GRHIVendorId: AMD
    const auto run = [](int menu, int mode, bool on, bool reset, int caller = 1) {
        if (reset) g_state = 0;
        return FrameCapStartTestRun(reinterpret_cast<void*>(&FakeOrig), reinterpret_cast<void*>(&FakeGetter),
                                    reinterpret_cast<void*>(&FakeHash), reinterpret_cast<void*>(&FakeFind), reinterpret_cast<void*>(&FakeSet),
                                    &g_nameStr, &g_state, menu, mode, on, reset, caller, &reflex, &cvarSlot, &vendor);
    };
    g_findResult = 1;
    run(0, 1, false, true, 0);  // DLSS's sink: T(off) in the first frames
    Check(g_origCalls == 1 && !g_lastOn && g_setCalls == 0, "fpscap: the game's switch runs first; a switch to off sets nothing");
    run(0, 1, true, false);  // FSR's sink after the saved options: T(on)
    Check(g_setCalls == 1 && !g_lastForce && wcscmp(g_setName, kName) == 0 && wcscmp(g_setValue, L"60") == 0,
          "fpscap: first switch off -> on from FSR's sink, no Reflex, limit 0, no menu: the saved \"60\" goes through the setter "
          "(same name, force off)");
    run(0, 1, true, false);  // T(on) again: T does nothing (state already on), nor does the fix
    run(0, 1, false, false);
    run(0, 1, true, false);  // a later switch on (the player's): left to the game
    Check(g_origCalls == 5 && g_setCalls == 1, "fpscap: later switches are left to the game");
    run(1, 1, true, true);
    run(0, 1, false, false);
    run(0, 1, true, false);
    Check(g_setCalls == 1, "fpscap: a menu open at the first switch on: left to the game, also afterwards");
    // 1.2.2: DLSS frame generation and Reflex are left to the game
    run(0, 1, true, true, 0);  // the first switch on comes from a DLSS sink
    run(0, 1, false, false);
    run(0, 1, true, false);    // and a later one from FSR's sink
    Check(g_setCalls == 1, "fpscap: the first switch on from DLSS frame generation's sink: left to the game, also afterwards");
    reflex = 1;
    run(0, 1, true, true);
    Check(g_setCalls == 1, "fpscap: FSR's switch on with T's Reflex flag set (Reflex switched on): left to the game");
    reflex = 0;
    // 1.2.2 (rel122fix): NVIDIA GPUs are left to the game even when T did not switch Reflex on (hardware GPU scheduling off)
    vendor = 0x10DE;
    run(0, 1, true, true);
    Check(g_setCalls == 1, "fpscap: FSR's switch on with an NVIDIA GPU and T's Reflex flag 0: left to the game");
    vendor = 0;
    run(0, 1, true, true);
    Check(g_setCalls == 1, "fpscap: GPU vendor not known (0): left to the game");
    vendor = 0x8086;
    run(0, 1, true, true);
    Check(g_setCalls == 2 && wcscmp(g_setValue, L"60") == 0, "fpscap: an Intel GPU: the saved value applied as on AMD");
    vendor = 0x1002;
    cvar = 120;
    run(0, 1, true, true);
    Check(g_setCalls == 2, "fpscap: the limit still in effect after the switch (not 0): nothing set");
    cvar = 0;
    g_findResult = -1;
    run(0, 1, true, true);
    Check(g_setCalls == 2, "fpscap: no saved entry: nothing set");
    g_findResult = 0;  // element 0: empty value
    run(0, 1, true, true);
    Check(g_setCalls == 2, "fpscap: an empty saved value: nothing set");
    g_findResult = 1;
    g_settings[8 + 3] = 0x20;  // RF_BeginDestroyed in the settings object's flags
    run(0, 1, true, true);
    Check(g_setCalls == 2, "fpscap: a settings object being destroyed: nothing set");
    g_settings[8 + 3] = 0;
    run(0, 1, true, true);
    Check(g_setCalls == 3 && wcscmp(g_setValue, L"60") == 0, "fpscap: and set again after a reset");
    Check(g_origCalls == 20, "fpscap: the game's switch ran on every call");
}

}  // namespace fpc
