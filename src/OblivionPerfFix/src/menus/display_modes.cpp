// DisplayModeCache: the game stops for ~0.3-0.5 s whenever its graphics settings page is shown (the pause menu reopens on the
// last page used, so for a player who adjusts graphics this is every time the menu opens).
//
// The page builds its "Select Monitor" and "Display Resolution" lists from the engine's display metrics, and has a
// cached copy of them rebuilt from scratch four times each time it is shown (build 19115871: the getter exe+0x31AA020
// with its force-refresh flag set -> FDisplayMetrics::RebuildDisplayMetrics exe+0x1183FB0 -> GetMonitorsInfo
// exe+0x1172DB0). GetMonitorsInfo lists every display mode of every monitor twice with USER32 EnumDisplaySettingsW:
// once for the game's list of resolutions (each distinct width x height) and once for the engine's MaxResolution.
// Windows takes ~0.1 ms per EnumDisplaySettingsW call and a 4K monitor has hundreds of modes (683 on the dev PC), so
// each list costs 60-110 ms, and the page reads it eight times (more with several monitors). Nothing else is slow.
//
// The fix keeps each display device's mode list the first time GetMonitorsInfo reads it and answers its later reads
// from the kept copy, but only after checking the copy against Windows at the start of every read: the device's current
// mode, modes 0, N/4, N/2, 3N/4 and N-1 (read from Windows and compared byte for byte) and that mode N does not exist.
// Mode 0 and the end of the list always come from Windows itself, as in the stock loop (reading mode 0 is what makes
// Windows refresh its own list). Any difference, and the read goes to Windows as before and the copy is replaced.
// An answer from the copy is the exact bytes Windows wrote for that mode: EnumDisplaySettingsW writes the first dmSize
// bytes of the DEVMODEW (dmSize as it returns it) whatever the buffer held before and leaves the rest alone; this is
// checked once at the first use, and the fix stays off if Windows behaves differently.
//
// Only GetMonitorsInfo's four calls (recognised by their return address) are answered from the copy; every other
// EnumDisplaySettingsW call in the game goes to Windows as before. The hook is the game exe's import slot for
// EnumDisplaySettingsW (one pointer write, safe at any time); GetMonitorsInfo is found by its reference to the string
// "EDID" and its calls through that slot.
#include "common.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <memory>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace opf {
namespace {

using EnumFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);

constexpr size_t kMaxSites = 8;
constexpr DWORD kMaxModes = 1u << 15;
constexpr ptrdiff_t kWindow = 0x1000;  // GetMonitorsInfo's calls lie within this distance of its "EDID" reference
constexpr DWORD kCurrent = ENUM_CURRENT_SETTINGS;

// One device's mode list as Windows reported it; immutable once published.
struct ModeList {
    wchar_t device[CCHDEVICENAME + 1] = {};
    uint16_t written = 0;            // bytes Windows writes per mode (the dmSize it returns)
    std::vector<uint8_t> modes;      // count * written
    uint8_t current[sizeof(DEVMODEW)] = {};  // ENUM_CURRENT_SETTINGS when the list was read
    DWORD count() const { return written ? static_cast<DWORD>(modes.size() / written) : 0; }
    const uint8_t* mode(DWORD i) const { return modes.data() + static_cast<size_t>(i) * written; }
};

// One reading loop of GetMonitorsInfo on one thread: answered from a list, or recorded into a new one.
struct Session {
    std::shared_ptr<const ModeList> list;
    std::shared_ptr<ModeList> recording;
    DWORD next = 0;
    uint64_t started = 0;  // QPC ticks when a recording began
};
thread_local Session t_session;

std::atomic<EnumFn> g_real{nullptr};
void** g_slot = nullptr;
uint8_t* g_edidRef = nullptr;
uintptr_t g_returns[kMaxSites] = {};
size_t g_numReturns = 0;
bool g_anyCaller = false;               // self-test only
std::atomic<bool> g_enabled{true};
std::atomic<int> g_writeCheck{0};       // 0 = not yet, 1 = Windows writes as expected, -1 = it does not (fix off)
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::shared_ptr<const ModeList>> g_lists;  // one per device, under g_lock

std::atomic<uint64_t> g_reads{0}, g_fromCopy{0}, g_fromWindows{0}, g_modesAnswered{0};
std::atomic<uint64_t> g_changedCurrent{0}, g_changedMode0{0}, g_changedProbe{0}, g_changedEnd{0};
std::atomic<uint64_t> g_checkTicks{0}, g_recordTicks{0}, g_recorded{0};
uint64_t g_readsLogged = 0;

bool IsSite(uintptr_t ret)
{
    if (g_anyCaller) return true;
    for (size_t i = 0; i < g_numReturns; ++i)
        if (g_returns[i] == ret) return true;
    return false;
}

bool SameDevice(const wchar_t* a, LPCWSTR b) { return b && _wcsicmp(a, b) == 0; }

uint64_t Ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// Windows' reply for one mode into our own buffer (as the game's loops call it: zeroed, dmDriverExtra 0).
BOOL Read(EnumFn real, LPCWSTR device, DWORD mode, DEVMODEW& out)
{
    memset(&out, 0, sizeof out);
    out.dmSize = sizeof(DEVMODEW);
    return real(device, mode, &out);
}

// Once: does EnumDisplaySettingsW write exactly its returned dmSize bytes, whatever the buffer held?
bool WriteCheck(EnumFn real, LPCWSTR device)
{
    const int state = g_writeCheck.load();
    if (state) return state > 0;
    DEVMODEW a, b;
    memset(&a, 0x00, sizeof a);
    memset(&b, 0xFF, sizeof b);
    a.dmSize = b.dmSize = sizeof(DEVMODEW);
    a.dmDriverExtra = b.dmDriverExtra = 0;
    const BOOL ra = real(device, 0, &a), rb = real(device, 0, &b);
    bool ok = ra && rb && a.dmSize == b.dmSize && a.dmDriverExtra == 0 && b.dmDriverExtra == 0 &&
              a.dmSize >= offsetof(DEVMODEW, dmDisplayFrequency) + sizeof(DWORD) && a.dmSize <= sizeof(DEVMODEW) &&
              memcmp(&a, &b, a.dmSize) == 0;
    for (size_t i = ok ? a.dmSize : sizeof(DEVMODEW); i < sizeof(DEVMODEW); ++i)
        if (reinterpret_cast<uint8_t*>(&a)[i] != 0x00 || reinterpret_cast<uint8_t*>(&b)[i] != 0xFF) ok = false;
    if (!ra || !rb) return false;  // no mode 0 on this device: decide on another call
    g_writeCheck = ok ? 1 : -1;
    if (ok) Log("[display] EnumDisplaySettingsW writes %u bytes per mode: mode list copies enabled", static_cast<unsigned>(a.dmSize));
    else Log("[display] EnumDisplaySettingsW does not write its reply as expected: mode lists are always read from Windows");
    return ok;
}

std::shared_ptr<const ModeList> Find(LPCWSTR device)
{
    AcquireSRWLockShared(&g_lock);
    std::shared_ptr<const ModeList> out;
    for (const auto& l : g_lists)
        if (SameDevice(l->device, device)) { out = l; break; }
    ReleaseSRWLockShared(&g_lock);
    return out;
}

void Publish(const std::shared_ptr<const ModeList>& list)
{
    AcquireSRWLockExclusive(&g_lock);
    bool replaced = false;
    for (auto& l : g_lists)
        if (SameDevice(l->device, list->device)) { l = list; replaced = true; break; }
    if (!replaced && g_lists.size() < 32) g_lists.push_back(list);
    ReleaseSRWLockExclusive(&g_lock);
}

void Forget(LPCWSTR device)
{
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_lists.size(); ++i)
        if (SameDevice(g_lists[i]->device, device)) { g_lists.erase(g_lists.begin() + static_cast<ptrdiff_t>(i)); break; }
    ReleaseSRWLockExclusive(&g_lock);
}

// Does the kept list still match Windows? Reads the current mode, then mode 0 into the caller's buffer (the call the
// game makes anyway), then a few modes spread over the list and the end. Returns 0 = matches, else the reason.
int CheckCopy(EnumFn real, LPCWSTR device, const ModeList& list, DEVMODEW* dm, BOOL* mode0)
{
    DEVMODEW probe;
    const bool currentOk = Read(real, device, kCurrent, probe) && memcmp(&probe, list.current, list.written) == 0;
    *mode0 = real(device, 0, dm);  // the caller's own call, as before
    if (!currentOk) return 1;
    if (!*mode0 || dm->dmSize != list.written || dm->dmDriverExtra != 0 || memcmp(dm, list.mode(0), list.written) != 0)
        return 2;
    const DWORD n = list.count();
    if (n == 0) return 2;
    const DWORD probes[4] = {n / 4, n / 2, (3 * n) / 4, n - 1};
    DWORD last = 0;
    for (const DWORD k : probes) {
        if (k == 0 || k == last) continue;
        last = k;
        if (!Read(real, device, k, probe) || probe.dmSize != list.written || memcmp(&probe, list.mode(k), list.written) != 0)
            return 3;
    }
    if (Read(real, device, n, probe)) return 4;
    return 0;
}

void StartRecording(Session& s, LPCWSTR device, const DEVMODEW* dm)
{
    if (dm->dmSize > sizeof(DEVMODEW) || dm->dmSize < offsetof(DEVMODEW, dmDisplayFrequency) + sizeof(DWORD) ||
        dm->dmDriverExtra != 0)
        return;
    auto list = std::make_shared<ModeList>();
    wcsncpy_s(list->device, device, CCHDEVICENAME);
    list->written = dm->dmSize;
    list->modes.assign(reinterpret_cast<const uint8_t*>(dm), reinterpret_cast<const uint8_t*>(dm) + dm->dmSize);
    s.recording = std::move(list);
    s.next = 1;
}

BOOL Begin(EnumFn real, LPCWSTR device, DEVMODEW* dm)
{
    Session& s = t_session;
    s = {};
    if (!g_enabled.load(std::memory_order_relaxed) || !device || !dm || dm->dmDriverExtra != 0 ||
        wcsnlen(device, CCHDEVICENAME + 1) > CCHDEVICENAME || !WriteCheck(real, device))
        return real(device, 0, dm);
    g_reads.fetch_add(1, std::memory_order_relaxed);
    const uint64_t t0 = Ticks();
    BOOL mode0 = FALSE;
    if (const auto list = Find(device)) {
        const int changed = CheckCopy(real, device, *list, dm, &mode0);
        g_checkTicks.fetch_add(Ticks() - t0, std::memory_order_relaxed);
        if (!changed) {
            s.list = list;
            s.next = 1;
            g_fromCopy.fetch_add(1, std::memory_order_relaxed);
            return mode0;  // Windows' own reply for mode 0, which equals the copy's
        }
        (changed == 1 ? g_changedCurrent : changed == 2 ? g_changedMode0 : changed == 3 ? g_changedProbe : g_changedEnd)
            .fetch_add(1, std::memory_order_relaxed);
        Forget(device);
    } else {
        mode0 = real(device, 0, dm);
    }
    g_fromWindows.fetch_add(1, std::memory_order_relaxed);
    if (mode0) StartRecording(s, device, dm);
    s.started = t0;
    if (s.recording) s.recording->modes.reserve(static_cast<size_t>(s.recording->written) * 1024);
    return mode0;
}

void Record(Session& s, LPCWSTR device, BOOL r, const DEVMODEW* dm)
{
    ModeList& l = *s.recording;
    if (r) {
        if (dm->dmSize != l.written || dm->dmDriverExtra != 0 || l.count() >= kMaxModes) { s = {}; return; }
        const auto* p = reinterpret_cast<const uint8_t*>(dm);
        l.modes.insert(l.modes.end(), p, p + l.written);
        ++s.next;
        return;
    }
    // The end of the list: keep it, with the current mode as Windows reports it now.
    DEVMODEW cur;
    if (Read(g_real.load(), device, kCurrent, cur) && cur.dmSize == l.written) {
        memcpy(l.current, &cur, l.written);
        Publish(std::const_pointer_cast<const ModeList>(s.recording));
        g_recorded.fetch_add(1, std::memory_order_relaxed);
        g_recordTicks.fetch_add(Ticks() - s.started, std::memory_order_relaxed);
    }
    s = {};
}

__declspec(noinline) BOOL WINAPI EnumDisplaySettingsHook(LPCWSTR device, DWORD mode, DEVMODEW* dm)
{
    const EnumFn real = g_real.load(std::memory_order_relaxed);
    if (!IsSite(reinterpret_cast<uintptr_t>(_ReturnAddress())) || mode >= 0x80000000u) return real(device, mode, dm);
    if (mode == 0) return Begin(real, device, dm);
    Session& s = t_session;
    const bool sequential = dm && dm->dmDriverExtra == 0 && mode == s.next;
    if (s.list && sequential && SameDevice(s.list->device, device)) {
        if (mode < s.list->count()) {
            memcpy(dm, s.list->mode(mode), s.list->written);
            ++s.next;
            g_modesAnswered.fetch_add(1, std::memory_order_relaxed);
            return TRUE;
        }
        s = {};
        return real(device, mode, dm);  // the end of the list, from Windows (checked to be the end)
    }
    if (s.recording && sequential && SameDevice(s.recording->device, device)) {
        const BOOL r = real(device, mode, dm);
        Record(s, device, r, dm);
        return r;
    }
    s = {};
    return real(device, mode, dm);
}

// GetMonitorsInfo: the one function referring to the string L"EDID", and its calls through the import slot.
bool FindSites(const GameModule& game, void** slot)
{
    static const wchar_t kEdid[] = L"EDID";
    Pattern edid;
    for (size_t i = 0; i < sizeof kEdid; ++i) {
        edid.bytes.push_back(reinterpret_cast<const uint8_t*>(kEdid)[i]);
        edid.care.push_back(1);
    }
    const auto strings = FindAll(game.rdata, edid);
    std::vector<uint8_t*> refs;
    for (uint8_t* at = game.text.begin; at + 7 <= game.text.end; ++at) {
        if ((at[0] != 0x48 && at[0] != 0x4C) || at[1] != 0x8D || (at[2] & 0xC7) != 0x05) continue;
        uint8_t* const target = RipTarget(at, 3, 7);
        for (uint8_t* s : strings)
            if (target == s) refs.push_back(at);
    }
    if (refs.size() != 1) {
        Log("[display] GetMonitorsInfo not found (%zu \"EDID\" strings, %zu references): fix not installed", strings.size(), refs.size());
        return false;
    }
    uint8_t* const lo = std::max(game.text.begin, refs[0] - kWindow);
    uint8_t* const hi = std::min(game.text.end - 8, refs[0] + kWindow);
    size_t n = 0;
    for (uint8_t* at = lo; at < hi; ++at) {
        if (at[0] != 0xFF || at[1] != 0x15 || RipTarget(at, 2, 6) != reinterpret_cast<uint8_t*>(slot)) continue;
        if (at[6] != 0x85 || at[7] != 0xC0 || n == kMaxSites) {  // each call is followed by test eax,eax
            Log("[display] unexpected EnumDisplaySettingsW call at exe+0x%llX: fix not installed",
                static_cast<unsigned long long>(at - game.base));
            return false;
        }
        g_returns[n++] = reinterpret_cast<uintptr_t>(at + 6);
    }
    if (n < 2) {
        Log("[display] GetMonitorsInfo (exe+0x%llX) has %zu EnumDisplaySettingsW calls, expected the mode loops: fix not installed",
            static_cast<unsigned long long>(refs[0] - game.base), n);
        return false;
    }
    g_numReturns = n;
    g_edidRef = refs[0];
    return true;
}

}  // namespace

int ApplyDisplayModeCache(bool requireBound)
{
    if (g_slot) return 1;
    const GameModule& game = Game();
    void** const slot = FindImportSlot(game.base, "USER32.dll", "EnumDisplaySettingsW");
    if (!slot) {
        Log("[display] the game does not import EnumDisplaySettingsW: fix not installed");
        return 0;
    }
    if (!g_numReturns && !FindSites(game, slot)) return 0;
    void* const current = *slot;
    if (requireBound) {
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(current), &owner) || !owner) {
            return -1;  // the loader has not filled it yet
        }
    }
    if (current == reinterpret_cast<void*>(&EnumDisplaySettingsHook)) {
        g_slot = slot;
        return 1;
    }
    g_real = reinterpret_cast<EnumFn>(current);
    if (!WritePointer(slot, reinterpret_cast<void*>(&EnumDisplaySettingsHook))) {
        Log("[display] could not write the import slot: fix not installed");
        return 0;
    }
    g_slot = slot;
    Log("[display] GetMonitorsInfo (\"EDID\" at exe+0x%llX): %zu EnumDisplaySettingsW calls; import slot exe+0x%llX hooked",
        static_cast<unsigned long long>(g_edidRef - game.base), g_numReturns,
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(slot) - game.base));
    return 1;
}

void SetDisplayModeCacheEnabled(bool enabled)
{
    g_enabled = enabled;
    if (!enabled) {
        AcquireSRWLockExclusive(&g_lock);
        g_lists.clear();
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void LogDisplayModeCacheStats()
{
    const uint64_t reads = g_reads.load();
    if (!g_slot || reads == g_readsLogged) return;
    g_readsLogged = reads;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const uint64_t fromCopy = g_fromCopy.load(), checks = fromCopy + g_changedCurrent + g_changedMode0 + g_changedProbe + g_changedEnd;
    const uint64_t recorded = g_recorded.load();
    Log("[display] mode list reads: %llu (from the kept copy %llu, from Windows %llu; copy out of date: current mode %llu, "
        "mode 0 %llu, spread modes %llu, end %llu); modes answered from the copy %llu; check %.2f ms on average, "
        "a full read from Windows %.1f ms on average",
        static_cast<unsigned long long>(reads), static_cast<unsigned long long>(fromCopy),
        static_cast<unsigned long long>(g_fromWindows.load()), static_cast<unsigned long long>(g_changedCurrent.load()),
        static_cast<unsigned long long>(g_changedMode0.load()), static_cast<unsigned long long>(g_changedProbe.load()),
        static_cast<unsigned long long>(g_changedEnd.load()), static_cast<unsigned long long>(g_modesAnswered.load()),
        checks ? 1000.0 * static_cast<double>(g_checkTicks.load()) / static_cast<double>(f.QuadPart) / static_cast<double>(checks) : 0.0,
        recorded ? 1000.0 * static_cast<double>(g_recordTicks.load()) / static_cast<double>(f.QuadPart) / static_cast<double>(recorded) : 0.0);
}

bool DisplayModeCacheInfo(DisplayModeCacheInfoOut* out)
{
    if (!g_numReturns) return false;
    out->slot = g_slot;
    out->edidRef = g_edidRef;
    out->numSites = g_numReturns;
    for (size_t i = 0; i < g_numReturns && i < 8; ++i) out->returns[i] = reinterpret_cast<uint8_t*>(g_returns[i]);
    out->hook = reinterpret_cast<void*>(&EnumDisplaySettingsHook);
    out->reads = g_reads.load();
    out->fromCopy = g_fromCopy.load();
    out->fromWindows = g_fromWindows.load();
    out->modesAnswered = g_modesAnswered.load();
    return true;
}

void* DisplayModeCacheTestSetup(void* fakeReal, bool anyCaller)
{
    g_real = reinterpret_cast<EnumFn>(fakeReal);
    g_anyCaller = anyCaller;
    g_writeCheck = 0;
    SetDisplayModeCacheEnabled(false);
    SetDisplayModeCacheEnabled(true);
    t_session = {};
    return reinterpret_cast<void*>(&EnumDisplaySettingsHook);
}

}  // namespace opf
