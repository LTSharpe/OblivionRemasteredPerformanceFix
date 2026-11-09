// CreatePreload / CreatePreloadNear (lab bigcreate 2026-10-20; played by the maintainer since 2026-10-24; public since
// 1.3.0, worker port130j 2026-11-08 after review rev130e; [Fixes] CreatePreload, default 1; CreatePreloadNear, default 12
// frames). Installed at start-up when CreatePreload != 0 (version.dll route; needs NpcCreationPacing: it is asked from its
// create hook), then live 0/1 and 0..30 frames.
//
// What it fixes (findings "worker bigcreate", "fightcost"): the old engine's create-actor command (Execute -> Altar's create)
// first looks up the reference's UE-side form asset by form id (a lookup with two maps: form id -> loaded asset, else form
// id -> asset path string, which it then loads with FSoftObjectPath::TryLoad), then the actor class (a soft class path read
// from the form asset) through TryLoad -> StaticLoadObject -> LoadPackage: LoadPackageAsync at priority INT32_MAX plus
// FlushAsyncLoading, so the game thread waits for the async loader. When the form asset or class is not resident (first of
// its kind in the session, or collected since) that wait is 9-60 ms for a creature class (BP_Horse_Chestnut, BP_Rat,
// BP_Skeleton) and up to 160 ms next to a ruin while the loader streams cells: the big create units.
//
// The fix: a create that NpcCreationPacing may defer (far from the player, pacing active, not forced) and whose form asset
// or class package is not resident waits at the queue head while that package is requested with LoadPackageAsync (the
// FString overload, the sync load's own priority INT32_MAX, no delegate). The command is offered again in the next frames;
// once everything is resident it runs and its TryLoads find the objects loaded. A create waits at most until pacing's lag
// bound (PacingMaxLagFrames; it then runs and loads synchronously, as stock), and a package requested more than 3 s ago is
// no longer waited for (its own synchronous load decides). CreatePreloadNear: a *near* create may also wait, at most this
// many frames, while its spot is more than 75 degrees off the camera's forward direction (a fixed angle, not tied to the
// field of view: covers horizontal fields of view up to ~150 degrees); it runs at once when its spot comes into view. The
// camera is the frame-generation view (at most 250 ms old), so without frame generation no near create waits.
// Equivalent, not identical: the same packages are loaded by the same loader with the same contents, earlier and without
// blocking the game thread; the create runs a few frames later in the same queue order (pacing's shipped deferral).
//
// Which packages: stage 1, the form asset, read from the lookup's two maps as the lookup reads them (cached asset by form id;
// else its path string, resolved with the game's own FSoftObjectPath::SetPath / ResolveObject: not resident = request);
// stage 2, the actor class path in the form asset by Altar's create's rule: the soft path at +0x80 when the soft path at
// +0x58 is set, else the one the asset's virtual +0x2C8 (a getter) copies, whose offset is read from the getter's code
// (unknown getter = no prediction). A wrong prediction only changes timing within pacing's bounds.
//
// How: one entry detour on the form-asset lookup only notes its map holder (the subsystem's maps; a second holder switches
// the fix off for the session). Every function and layout is found by pattern on the exe: the lookup (both map readers,
// pinning every holder offset), FSoftObjectPath::SetPath (the lookup's own call), ResolveObject, ToString, FMemory::Free,
// LoadPackageAsync(FString); the create command's form id (+0x70, its Execute's reads, inside NpcCreationPacing's create
// Execute); Altar's create's class-rule reads (+0x58, +0x80, virtual +0x2C8). Any mismatch: not installed, one log line.
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace opf {
namespace {

// The form-asset lookup (exe+0x47B2910): prologue, then map 1 (loaded assets) at +6 and map 2 (path strings) at +0x75.
constexpr char kLookupPrologue[] = "40 57 48 83 EC 50";
constexpr char kLookupMap1[] =
    "8B 81 20 01 00 00 4C 8B C9 8B FA 44 8B DA 3B 81 4C 01 00 00 74 ?? 48 8D 91 50 01 00 00 44 8B D7 48 8B 42 08 8B 89 60 01 00 00 "
    "48 85 C0 48 0F 45 D0 FF C9 48 23 CF 44 8B 04 8A 41 83 F8 FF 74 ?? 49 8B 91 18 01 00 00";
constexpr size_t kLookupMap1At = 6;
constexpr char kLookupMap2[] =
    "41 8B 41 30 41 3B 41 5C 74 ?? 49 8B 41 68 49 8D 51 60 41 8B 49 70 48 85 C0 48 0F 45 D0 FF C9 49 23 CA 8B 04 8A 83 F8 FF 74 ?? "
    "49 8B 51 28";
constexpr size_t kLookupMap2At = 0x75;
constexpr size_t kLookupSetPathCall = 0x14B;  // E8: FSoftObjectPath::SetPath (exe+0x10F72F0)
constexpr char kSetPath[] = "48 89 5C 24 20 55 56 57 41 56 41 57 48 8B EC 48 83 EC 40 48 63 42 08";
constexpr char kResolve[] = "48 83 39 00 74 0D 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 33 C0 C3";  // exe+0x10F6700
constexpr char kToString[] = "40 55 53 56 57 48 8D AC 24 88 F8 FF FF 48 81 EC 78 08 00 00";        // exe+0x10F8710
// FMemory::Free (exe+0xE26500)
constexpr char kFree[] =
    "48 85 C9 74 2E 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B "
    "D3 FF 50 48 48 83 C4 20 5B C3";
// LoadPackageAsync(const FString&, FLoadPackageAsyncDelegate, int32 priority, ...) (exe+0xFEA060)
constexpr char kLoadAsync[] =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8B EC 48 81 EC 80 00 00 00 48 8B 01 45 33 F6 48 89 45 D0 48 8B DA 8B 41 08 48 8D "
    "55 C0 85 C0 0F 29 74 24 70 41 8B F9";
// The create command's Execute (exe+0x657C8D0) reads its form ids (+0x68 / +0x70 / +0x78) at +0x22.
constexpr char kCmdForms[] = "0F B6 47 20 F3 0F 10 47 30 F3 0F 10 4F 38 44 8B 4F 68 44 8B 47 70 8B 57 78 48 8B 4F 10";
constexpr size_t kCmdFormsSpan = 0x80;
constexpr uint32_t kCmdForm = 0x70;
// Altar's create (exe+0x47A35A0): ToString of the soft path at +0x58; the path at +0x80; the asset's virtual +0x2C8.
constexpr char kRule58[] = "49 8D 4F 58 48 8D 55 ?? E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 ?? E8";
constexpr char kRule80[] = "40 84 FF 0F 84 ?? ?? ?? ?? 41 0F 10 87 80 00 00 00 49 8D 97 90 00 00 00";
constexpr char kRule2C8[] = "49 8B 07 48 8D 54 24 ?? 49 8B CF FF 90 C8 02 00 00";
constexpr size_t kRuleSpan = 0x400;
constexpr uint32_t kPathSet = 0x58, kClassPath = 0x80, kClassGetter = 0x2C8;

using F8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using SetPathFn = void (*)(void* sp, const void* view);
using ResolveFn = void* (*)(const void* sp);
using ToStringFn = void* (*)(const void* sp, void* outStr);
using FreeFn = void (*)(void*);
using LoadAsyncFn = int32_t (*)(const void* name, void* delegate, int32_t priority, uint32_t flags, int32_t pie);

struct FStr {
    wchar_t* data;
    int32_t num, max;
};
struct FView {
    const wchar_t* data;
    int32_t len;
};
struct SoftPath {  // FSoftObjectPath: FTopLevelAssetPath (package FName, asset FName) + FString sub-path
    uint64_t pkg, asset;
    FStr sub;
};
struct Req {
    uint64_t pkg;
    uint64_t tick;
};
struct Head {  // a deferred command and the frames it has waited (rev130e C-S1: keyed by the command)
    const void* cmd;
    int waited;
    uint64_t frame;
};

constexpr int kMaxReq = 512, kMaxHeads = 8, kPathChars = 512;
// A head entry not offered for this many frames is stale (its command ran or went; a budget deferral by pacing in between
// keeps the count; a new command at a reused address may at worst run earlier).
constexpr uint64_t kHeadStale = 60;

struct State {
    uint8_t* base = nullptr;
    void* origLookup = nullptr;
    SetPathFn setPath = nullptr;
    ResolveFn resolve = nullptr;
    ToStringFn toString = nullptr;
    FreeFn free = nullptr;
    LoadAsyncFn loadAsync = nullptr;
    std::atomic<int> mode{0}, nearFrames{0};
    std::atomic<const uint8_t*> holder{nullptr};
    std::atomic<bool> holderChanged{false};
    // game thread
    Req req[kMaxReq];
    int nReq = 0;
    Head heads[kMaxHeads] = {};
    double nearAngle = -1;  // the last near create's angle off the camera's forward (deg; -1 = no camera)
    // stats (game thread writes, the stats line reads)
    std::atomic<uint64_t> waits{0}, waitFrames{0}, timeouts{0}, requests{0}, formWaits{0}, classWaits{0};
    std::atomic<uint64_t> nearAsks{0}, nearNoCamera{0}, nearWaits{0}, nearWaitFrames{0}, nearBound{0}, nearToView{0};
    std::atomic<uint64_t> headsFull{0};  // creates run at once because all kMaxHeads entries were in use
} g;

// ---- the lookup's maps (read as the lookup reads them; SEH-guarded) ----
const void* CachedAsset(const uint8_t* h, uint32_t form)
{
    if (!h) return nullptr;
    __try {
        if (*reinterpret_cast<const int32_t*>(h + 0x120) == *reinterpret_cast<const int32_t*>(h + 0x14C)) return nullptr;
        const uint8_t* buckets = *reinterpret_cast<const uint8_t* const*>(h + 0x158);
        if (!buckets) buckets = h + 0x150;
        const int32_t size = *reinterpret_cast<const int32_t*>(h + 0x160);
        if (size <= 0) return nullptr;
        int32_t i = *reinterpret_cast<const int32_t*>(buckets + static_cast<size_t>((static_cast<uint32_t>(size) - 1u) & form) * 4);
        const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(h + 0x118);
        for (int guard = 0; i != -1 && guard < 4096; ++guard) {
            const uint8_t* x = e + static_cast<size_t>(i) * 0x18;
            if (*reinterpret_cast<const uint32_t*>(x) == form) return *reinterpret_cast<const void* const*>(x + 8);
            i = *reinterpret_cast<const int32_t*>(x + 0x10);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}
// The asset path string of a form not cached yet, copied into buf (rev130e C-S2: nothing of the map is used after the
// guarded read); returns its length (0 = none).
int AssetPath(const uint8_t* h, uint32_t form, wchar_t* buf, int cap)
{
    if (!h) return 0;
    __try {
        if (*reinterpret_cast<const int32_t*>(h + 0x30) == *reinterpret_cast<const int32_t*>(h + 0x5C)) return 0;
        const uint8_t* buckets = *reinterpret_cast<const uint8_t* const*>(h + 0x68);
        if (!buckets) buckets = h + 0x60;
        const int32_t size = *reinterpret_cast<const int32_t*>(h + 0x70);
        if (size <= 0) return 0;
        int32_t i = *reinterpret_cast<const int32_t*>(buckets + static_cast<size_t>((static_cast<uint32_t>(size) - 1u) & form) * 4);
        const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(h + 0x28);
        for (int guard = 0; i != -1 && guard < 4096; ++guard) {
            const uint8_t* x = e + static_cast<size_t>(i) * 0x20;
            if (*reinterpret_cast<const uint32_t*>(x) == form) {
                const wchar_t* s = *reinterpret_cast<const wchar_t* const*>(x + 8);
                const int32_t num = *reinterpret_cast<const int32_t*>(x + 0x10);
                if (!s || num < 2 || num > cap) return 0;  // too long to copy: no prediction (rev130e nit)
                memcpy(buf, s, static_cast<size_t>(num - 1) * sizeof(wchar_t));
                buf[num - 1] = 0;
                return num - 1;
            }
            i = *reinterpret_cast<const int32_t*>(x + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// ---- requests ----
// A package is requested once; a create waits for it only while the request is younger than kGiveUpMs (a load that failed
// or takes longer runs the create as stock: its own synchronous load decides). After kRetryMs a package that is still (or
// again, after a garbage collection) not resident may be requested again.
constexpr uint64_t kGiveUpMs = 3000, kRetryMs = 20000;
// 0: do not wait for pkg; 1: wait (requested less than kGiveUpMs ago); 2: wait and request it now
int WaitFor(uint64_t pkg)
{
    const uint64_t now = GetTickCount64();
    Req* r = nullptr;
    for (int i = 0; i < g.nReq && !r; ++i)
        if (g.req[i].pkg == pkg) r = &g.req[i];
    if (r && now - r->tick < kGiveUpMs) return 1;
    if (r && now - r->tick < kRetryMs) return 0;  // given up on this one for now
    if (r) r->tick = now;
    else if (g.nReq < kMaxReq) g.req[g.nReq++] = {pkg, now};
    else {
        memmove(g.req, g.req + 1, sizeof(Req) * (kMaxReq - 1));
        g.req[kMaxReq - 1] = {pkg, now};
    }
    return 2;
}
// LoadPackageAsync(package name of "Package.Object" or "Package") at the sync load's priority, no delegate
void RequestPath(const wchar_t* s, int32_t len)
{
    wchar_t buf[kPathChars];
    int32_t n = 0;
    while (n < len && n < kPathChars - 2 && s[n] && s[n] != L'.' && s[n] != L':') {
        buf[n] = s[n];
        ++n;
    }
    if (n < 2) return;
    buf[n] = 0;
    FStr name{buf, n + 1, n + 1};
    uint64_t delegate[2] = {0, 0};  // unbound FLoadPackageAsyncDelegate (the callee destroys it: nothing to free)
    g.loadAsync(&name, delegate, 0x7FFFFFFF, 0, -1);
    g.requests.fetch_add(1, std::memory_order_relaxed);
}
// The soft path at sp as a string (the game's ToString), copied into buf; its length (0 = none).
int SoftPathString(const void* sp, wchar_t* buf, int cap)
{
    FStr out{};
    int n = 0;
    __try {
        g.toString(sp, &out);
        if (out.data && out.num > 1 && out.num <= cap) {
            n = out.num - 1;
            memcpy(buf, out.data, static_cast<size_t>(n) * sizeof(wchar_t));
            buf[n] = 0;
        }
        if (out.data) g.free(out.data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    return n;
}
// FSoftObjectPath from a string (the game's own SetPath), resolved without loading; the temporary is freed
const void* ResolveString(const wchar_t* s, int32_t len, uint64_t* pkg)
{
    SoftPath sp{};
    FView v{s, len};
    const void* r = nullptr;
    __try {
        g.setPath(&sp, &v);
        *pkg = sp.pkg;
        r = g.resolve(&sp);
        if (sp.sub.data) g.free(sp.sub.data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *pkg = 0;
        r = nullptr;
    }
    return r;
}
// true: the FSoftObjectPath at sp names something that is not resident (pkg = its package FName)
bool NotResident(const uint8_t* sp, uint64_t* pkg)
{
    __try {
        *pkg = *reinterpret_cast<const uint64_t*>(sp);
        if (!*pkg) return false;
        return g.resolve(sp) == nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The class path getter's offset, read from its code (kGetterPrologue, then movups xmm0, [rcx+disp] = the path's first
// 16 bytes); -1: not recognised (no prediction). A MatchesAt check only (the prologue is common).
constexpr char kGetterPrologue[] = "40 53 48 83 EC 20";
struct Getter {
    uintptr_t fn;
    int off;
};
Getter g_getters[64];
int g_nGetters = 0;
int GetterOffset(const uint8_t* fn)
{
    static const Pattern kPrologue = ParsePattern(kGetterPrologue);
    if (!MatchesAt(fn, kPrologue)) return -1;
    for (int i = 6; i < 40; ++i) {
        if (fn[i] == 0x0F && fn[i + 1] == 0x10 && fn[i + 2] == 0x81) {
            int32_t d;
            memcpy(&d, fn + i + 3, 4);
            return d > 0 && d < 0x2000 ? d : -1;
        }
        if (fn[i] == 0x0F && fn[i + 1] == 0x10 && fn[i + 2] == 0x41) return fn[i + 3];
    }
    return -1;
}
// Altar's create's rule: the soft path at +0x58 set (package or asset name) -> the class path at +0x80; else the getter's.
int ClassPathOffset(const void* asset)
{
    __try {
        const uint8_t* a = static_cast<const uint8_t*>(asset);
        if (*reinterpret_cast<const uint64_t*>(a + kPathSet) || *reinterpret_cast<const uint64_t*>(a + kPathSet + 8)) return kClassPath;
        const uintptr_t fn = reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(a))[kClassGetter / 8];
        for (int i = 0; i < g_nGetters; ++i)
            if (g_getters[i].fn == fn) return g_getters[i].off;
        const GameModule& game = Game();
        const int off = game.text.Contains(reinterpret_cast<const void*>(fn)) ? GetterOffset(reinterpret_cast<const uint8_t*>(fn)) : -1;
        if (g_nGetters < 64) g_getters[g_nGetters++] = {fn, off};
        return off;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Requests what this create will load synchronously; returns the number of packages it is waiting for (0 = run it).
int Predict(const uint8_t* holder, uint32_t form)
{
    const void* asset = CachedAsset(holder, form);
    if (!asset) {
        wchar_t path[kPathChars];
        const int len = AssetPath(holder, form, path, kPathChars);
        if (!len) return 0;  // unknown form: nothing to say
        uint64_t pkg = 0;
        asset = ResolveString(path, len, &pkg);
        if (!asset) {
            if (!pkg) return 0;
            const int w = WaitFor(pkg);
            if (w == 0) return 0;
            if (w == 2) {
                RequestPath(path, len);
                g.formWaits.fetch_add(1, std::memory_order_relaxed);
            }
            return 1;  // the class can only be read once the form asset is loaded
        }
    }
    const int off = ClassPathOffset(asset);
    if (off < 0) return 0;
    const uint8_t* sp = static_cast<const uint8_t*>(asset) + off;
    uint64_t pkg = 0;
    if (!NotResident(sp, &pkg)) return 0;  // no class path, unreadable or resident
    const int w = WaitFor(pkg);
    if (w == 0) return 0;
    if (w == 2) {
        wchar_t path[kPathChars];
        const int len = SoftPathString(sp, path, kPathChars);
        if (len) RequestPath(path, len);
        g.classWaits.fetch_add(1, std::memory_order_relaxed);
    }
    return 1;
}

uint64_t LookupHook(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
    const uint8_t* h = reinterpret_cast<const uint8_t*>(a1);
    const uint8_t* expect = nullptr;
    if (!g.holder.compare_exchange_strong(expect, h) && expect != h && !g.holderChanged.exchange(true))
        Log("[createpreload] the form-asset lookup has a second map holder: CreatePreload off for this session");
    return reinterpret_cast<F8>(g.origLookup)(a1, a2, a3, a4, a5, a6, a7, a8);
}

// The frames a command offered again has waited (0 = new); its entry is freed (it runs unless PutHead defers it again).
int TakeHead(const void* cmd, uint64_t frame)
{
    for (Head& h : g.heads)
        if (h.cmd == cmd) {
            const int waited = frame - h.frame > kHeadStale ? 0 : h.waited;  // not offered for a while: a new command at this address
            h.cmd = nullptr;
            return waited;
        }
    return 0;
}
// false: every entry is in use; the caller then runs the command (rev130m CP-N1: overwriting a busy entry restarted that
// command's count, so a near create could wait past CreatePreloadNear up to the lag bound).
bool PutHead(const void* cmd, int waited, uint64_t frame)
{
    Head* slot = nullptr;
    for (Head& h : g.heads)
        if (!h.cmd || frame - h.frame > kHeadStale) { slot = &h; break; }
    if (!slot) return false;
    *slot = {cmd, waited, frame};
    return true;
}

uint8_t* One(const GameModule& game, const char* pattern)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    return hits.size() == 1 ? hits[0] : nullptr;
}

struct Found {
    uint8_t *lookup = nullptr, *setPath = nullptr, *resolve = nullptr, *toString = nullptr, *free = nullptr, *loadAsync = nullptr;
};

bool Discover(const GameModule& game, const uint8_t* createExec, Found* f, const char** missing)
{
    const auto fail = [&](const char* what) { *missing = what; return false; };
    uint8_t* const m1 = One(game, kLookupMap1);
    if (!m1) return fail("the form-asset lookup");
    f->lookup = m1 - kLookupMap1At;
    if (!MatchesAt(f->lookup, ParsePattern(kLookupPrologue)) || !MatchesAt(f->lookup + kLookupMap2At, ParsePattern(kLookupMap2)) ||
        f->lookup[kLookupSetPathCall] != 0xE8)
        return fail("the form-asset lookup's maps");
    f->setPath = RipTarget(f->lookup + kLookupSetPathCall, 1, 5);
    if (!game.text.Contains(f->setPath) || !MatchesAt(f->setPath, ParsePattern(kSetPath))) return fail("FSoftObjectPath::SetPath");
    if (!(f->resolve = One(game, kResolve))) return fail("FSoftObjectPath::ResolveObject");
    if (!(f->toString = One(game, kToString))) return fail("FSoftObjectPath::ToString");
    if (!(f->free = One(game, kFree))) return fail("FMemory::Free");
    if (!(f->loadAsync = One(game, kLoadAsync))) return fail("LoadPackageAsync");
    const uint8_t* const forms = One(game, kCmdForms);
    if (!forms || (createExec && (forms < createExec || forms >= createExec + kCmdFormsSpan)))
        return fail("the create command's form id");
    const uint8_t *r58 = One(game, kRule58), *r80 = One(game, kRule80), *r2c8 = One(game, kRule2C8);
    if (!r58 || !r80 || !r2c8 || r80 < r58 || r2c8 < r80 || r2c8 - r58 > static_cast<ptrdiff_t>(kRuleSpan))
        return fail("Altar's create's class path rule");
    return true;
}

}  // namespace

bool CreatePreloadDiscover(const GameModule& game, const uint8_t* createExec, const char** missing)
{
    Found f;
    *missing = nullptr;
    return Discover(game, createExec, &f, missing);
}

bool InstallCreatePreload(bool beforeGameCode)
{
    if (g.base) return true;
    if (!beforeGameCode) {
        Log("[createpreload] CreatePreload: start-up only (version.dll route), not installed");
        return false;
    }
    NpcPacingInfoOut pi{};
    if (!NpcPacingInfo(&pi) || !pi.createExec) {
        Log("[createpreload] CreatePreload: needs NpcCreationPacing, not installed");
        return false;
    }
    const GameModule& game = Game();
    Found f;
    const char* missing = nullptr;
    if (!Discover(game, pi.createExec, &f, &missing)) {
        Log("[createpreload] CreatePreload: %s not recognised, not installed", missing);
        return false;
    }
    g.setPath = reinterpret_cast<SetPathFn>(f.setPath);
    g.resolve = reinterpret_cast<ResolveFn>(f.resolve);
    g.toString = reinterpret_cast<ToStringFn>(f.toString);
    g.free = reinterpret_cast<FreeFn>(f.free);
    g.loadAsync = reinterpret_cast<LoadAsyncFn>(f.loadAsync);
    // The lookup's entry detour: a trampoline (its 6-byte prologue, then a jump back) and the hook stub, in code memory near
    // the game, made read-execute before the patch.
    constexpr size_t kN = 6;
    uint8_t* const region = AllocNearGame(96);
    if (!region) {
        Log("[createpreload] CreatePreload: no memory near the game, not installed");
        return false;
    }
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = f.lookup + kN;
    void* const hook = reinterpret_cast<void*>(&LookupHook);
    memcpy(region, f.lookup, kN);
    memcpy(region + kN, jmpAbs, 6);
    memcpy(region + kN + 6, &resume, 8);
    memcpy(region + 64, jmpAbs, 6);
    memcpy(region + 64 + 6, &hook, 8);
    DWORD old;
    VirtualProtect(region, 96, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 96);
    const int64_t rel = (region + 64) - (f.lookup + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t code[kN] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(code + 1, &rel32, 4);
    g.origLookup = region;
    if (!WriteCode(f.lookup, std::span<const uint8_t>(code, kN))) {
        Log("[createpreload] CreatePreload: could not patch the form-asset lookup, not installed");
        return false;
    }
    g.base = game.base;
    Log("[createpreload] CreatePreload installed (form-asset lookup exe+0x%llX, LoadPackageAsync exe+0x%llX)",
        static_cast<unsigned long long>(f.lookup - game.base), static_cast<unsigned long long>(f.loadAsync - game.base));
    return true;
}

void SetCreatePreload(int mode) { g.mode.store(mode ? 1 : 0, std::memory_order_relaxed); }
void SetCreatePreloadNear(int frames) { g.nearFrames.store(frames < 0 ? 0 : (frames > 30 ? 30 : frames), std::memory_order_relaxed); }

// npc_pacing.cpp's CreateExecHook, game thread, for a near create pacing could otherwise defer: may it wait for its loads?
// Only with CreatePreload=1 and CreatePreloadNear > 0, and only while the spot is more than kNearAngleDeg off the camera's
// forward direction (the last frame-generation view, at most 250 ms old; none = no wait).
bool CreatePreloadNearMayWait(double x, double y, double z)
{
    constexpr double kNearAngleDeg = 75.0;
    g.nearAngle = -1;
    if (!g.base || g.mode.load(std::memory_order_relaxed) != 1 || g.nearFrames.load(std::memory_order_relaxed) <= 0) return false;
    g.nearAsks.fetch_add(1, std::memory_order_relaxed);
    float pos[3], fwd[3];
    if (!FrameGenLastCamera(pos, fwd, 250)) {
        g.nearNoCamera.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const double d[3] = {x - pos[0], y - pos[1], z - pos[2]};
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0) return false;
    double c = (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / len;
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    g.nearAngle = std::acos(c) * 57.29577951308232;
    return g.nearAngle > kNearAngleDeg;
}

// npc_pacing.cpp's CreateExecHook, game thread, before a main create runs. mayWait: pacing could defer it (far, active, not
// forced; or a near one out of view). Returns true: defer it this frame (its packages are loading).
bool CreatePreloadBefore(const void* cmd, bool mayWait, uint64_t lag, uint64_t maxLag, bool nearCreate)
{
    if (!g.base) return false;
    const uint64_t frame = NpcPacingFrame();
    const int waited = TakeHead(cmd, frame);  // runs now unless deferred again below
    const uint8_t* holder = g.holder.load(std::memory_order_relaxed);
    if (g.mode.load(std::memory_order_relaxed) == 0 || !holder || g.holderChanged.load(std::memory_order_relaxed)) return false;
    if (waited > 0 && !mayWait) g.nearToView.fetch_add(1, std::memory_order_relaxed);  // waited out of view, now in view (or forced)
    if (!mayWait) return false;
    uint32_t form = 0;
    __try {
        form = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(cmd) + kCmdForm);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (Predict(holder, form) == 0) return false;
    if (nearCreate && waited >= g.nearFrames.load(std::memory_order_relaxed)) {
        g.nearBound.fetch_add(1, std::memory_order_relaxed);
        return false;  // CreatePreloadNear's bound: run it now (its remaining loads are synchronous, as stock)
    }
    if (lag >= maxLag) {
        g.timeouts.fetch_add(1, std::memory_order_relaxed);
        return false;  // the lag bound: run it now (its remaining loads are synchronous, as stock)
    }
    if (!PutHead(cmd, waited + 1, frame)) {  // the same command comes back next frame
        g.headsFull.fetch_add(1, std::memory_order_relaxed);
        return false;  // no free entry to keep its count: run it now (as stock)
    }
    g.waitFrames.fetch_add(1, std::memory_order_relaxed);
    if (waited == 0) g.waits.fetch_add(1, std::memory_order_relaxed);
    if (nearCreate) {
        g.nearWaitFrames.fetch_add(1, std::memory_order_relaxed);
        if (waited == 0) g.nearWaits.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void LogCreatePreloadStats()
{
    if (!g.base) return;
    static uint64_t last[2] = {};
    const uint64_t w = g.waitFrames.load(), r = g.requests.load();
    if (w == last[0] && r == last[1]) return;
    last[0] = w;
    last[1] = r;
    Log("[createpreload] waits %llu (form asset %llu, class %llu requests; %llu frames; %llu ran at the lag bound), async "
        "requests %llu; CreatePreloadNear %d: near asked %llu (no camera %llu), waited %llu (%llu frames), at its bound %llu, "
        "ran on coming into view %llu; ran with every entry in use %llu",
        static_cast<unsigned long long>(g.waits.load()), static_cast<unsigned long long>(g.formWaits.load()),
        static_cast<unsigned long long>(g.classWaits.load()), static_cast<unsigned long long>(w),
        static_cast<unsigned long long>(g.timeouts.load()), static_cast<unsigned long long>(r), g.nearFrames.load(),
        static_cast<unsigned long long>(g.nearAsks.load()), static_cast<unsigned long long>(g.nearNoCamera.load()),
        static_cast<unsigned long long>(g.nearWaits.load()), static_cast<unsigned long long>(g.nearWaitFrames.load()),
        static_cast<unsigned long long>(g.nearBound.load()), static_cast<unsigned long long>(g.nearToView.load()),
        static_cast<unsigned long long>(g.headsFull.load()));
}

// self-test access to the readers (fake holder / getter code) and the deferred-command bookkeeping
const void* CreatePreloadTestCached(const void* holder, uint32_t form) { return CachedAsset(static_cast<const uint8_t*>(holder), form); }
int CreatePreloadTestPath(const void* holder, uint32_t form, wchar_t* buf, int cap)
{
    return AssetPath(static_cast<const uint8_t*>(holder), form, buf, cap);
}
int CreatePreloadTestGetter(const uint8_t* fn) { return GetterOffset(fn); }
int CreatePreloadTestHeads(const void* cmd, uint64_t frame, bool deferAgain)
{
    if (!cmd) {
        memset(g.heads, 0, sizeof g.heads);
        return 0;
    }
    const int waited = TakeHead(cmd, frame);
    if (deferAgain && !PutHead(cmd, waited + 1, frame)) return -1;  // every entry in use: not deferred
    return waited;
}

}  // namespace opf
