// SaveThumbLeftoverFix self-test (worker shot130q 2026-11-09), included by selftest.cpp:
//  - the sites by pattern on the mapped exe: the save function's service binding (its AddRaw call), the callback it binds
//    (and its Remove call on the engine delegate), the engine branch binding the same callback with the same AddRaw (one
//    delegate type); on build 19115871 at the known addresses;
//  - the decision: remove only the remembered service binding's own handle, on its binding thread, when the bound entry
//    is a raw delegate (never handle 0, another handle, another thread, the switch off);
//  - the bound-entry search on a fake invocation list (unbound and empty slots skipped).
//  - the hooks' sequence with fake game functions and delegates: the engine Remove first, its result passed through; the
//    service binding removed once, the note cleared after; nothing noted or removed while off; another handle or an entry
//    with another ExecuteIfSafe left alone.
#pragma once

namespace thumbleft_test {
struct Call { void* delegate; uint64_t handle; };
inline Call tlCalls[8];
inline int tlCallCount = 0;
inline bool tlEngineResult = false;
inline void* tlEngine = nullptr;
inline int tlAddCalls = 0;
inline bool TlFakeRemove(void* delegate, uint64_t handle)
{
    if (tlCallCount < 8) tlCalls[tlCallCount] = {delegate, handle};
    ++tlCallCount;
    return delegate == tlEngine ? tlEngineResult : true;
}
inline uint64_t* TlFakeAddRaw(void*, uint64_t* out, void*, void*)
{
    ++tlAddCalls;
    *out = 77;
    return out;
}
}  // namespace thumbleft_test

static void ThumbLeftoverRun(const GameModule& game)
{
    {
        SaveThumbLeftoverSites s{};
        const char* why = SaveThumbLeftoverDiscover(game, &s);
        Check(why == nullptr, "SaveThumbLeftoverFix: the service binding, its AddRaw call, the callback and its Remove call found by pattern");
        if (why) printf("    missing: %s\n", why);
        if (!why) {
            Check(s.addRawCall[0] == 0xE8 && s.removeCall[0] == 0xE8 && s.removeCall == s.callback + 0x5C,
                  "SaveThumbLeftoverFix: both redirected sites are E8 calls");
            Check(s.rawExec != nullptr && (game.timeDateStamp != 0xF19077A4u || s.rawExec - game.base == 0x48B7790),
                  "SaveThumbLeftoverFix: the raw delegate's ExecuteIfSafe found once by its whole code (build 19115871: exe+0x48B7790)");
            if (game.timeDateStamp == 0xF19077A4u)
                Check(s.serviceBind - game.base == 0x48CBDFD && s.addRawCall - game.base == 0x48CBE20 && s.callback - game.base == 0x48AF8A0 &&
                          s.engineDelegate - game.base == 0x8E50A28,
                      "SaveThumbLeftoverFix: build 19115871's binding exe+0x48CBDFD (AddRaw call exe+0x48CBE20), callback exe+0x48AF8A0, "
                      "engine delegate exe+0x8E50A28");
        }
    }
    {
        const DWORD t = 7, other = 9;
        const bool ok = SaveThumbLeftoverShouldRemove(true, 55, 55, t, t, true);
        int refused = 0, cases = 0;
        auto no = [&](bool r) { ++cases; if (!r) ++refused; };
        no(SaveThumbLeftoverShouldRemove(false, 55, 55, t, t, true));   // switched off
        no(SaveThumbLeftoverShouldRemove(true, 0, 0, t, t, true));      // no handle (the callback skips its Remove then too)
        no(SaveThumbLeftoverShouldRemove(true, 55, 56, t, t, true));    // an engine binding's or an older save's handle
        no(SaveThumbLeftoverShouldRemove(true, 55, 55, t, other, true));  // another thread than the one that bound it
        no(SaveThumbLeftoverShouldRemove(true, 55, 55, t, t, false));   // not bound there / not a raw delegate
        Check(ok && refused == cases, "SaveThumbLeftoverFix: removes only the remembered service binding's own handle, on its thread, "
                                      "when bound there as a raw delegate");
    }
    {
        alignas(8) uint8_t a[0x28] = {}, b[0x28] = {}, c[0x28] = {};
        *reinterpret_cast<uint64_t*>(a + 0x10) = 11;
        *reinterpret_cast<uint64_t*>(b + 0x10) = 22;
        *reinterpret_cast<uint64_t*>(c + 0x10) = 33;
        struct Entry { void* inst; int32_t size, pad; };
        Entry list[4] = {{a, 0x28, 0}, {b, 0, 0}, {nullptr, 0x28, 0}, {c, 0x28, 0}};
        struct Delegate { Entry* data; int32_t num, max; } del{list, 4, 4};
        const auto* const d = reinterpret_cast<const uint8_t*>(&del);
        Check(SaveThumbLeftoverFind(d, 11) == a && SaveThumbLeftoverFind(d, 33) == c && !SaveThumbLeftoverFind(d, 22) &&
                  !SaveThumbLeftoverFind(d, 44) && !SaveThumbLeftoverFind(d, 0) && !SaveThumbLeftoverFind(nullptr, 11),
              "SaveThumbLeftoverFix: the bound entry with the handle found (unbound and empty slots skipped, handle 0 never)");
    }
    Check(SaveThumbLeftoverResolve(nullptr) == nullptr, "SaveThumbLeftoverFix: not installed, the resolve passes pointers through");
    // The hooks in sequence on fake delegates.
    {
        using thumbleft_test::tlCalls; using thumbleft_test::tlCallCount; using thumbleft_test::tlEngineResult;
        using thumbleft_test::tlEngine; using thumbleft_test::tlAddCalls; using thumbleft_test::TlFakeRemove; using thumbleft_test::TlFakeAddRaw;
        alignas(8) static uint8_t rawExec[16], otherExec[16];
        alignas(8) static void* vtable[16] = {};
        alignas(8) static void* vtableOther[16] = {};
        vtable[0x60 / 8] = rawExec;
        vtableOther[0x60 / 8] = otherExec;
        alignas(8) static uint8_t inst[0x28] = {};
        *reinterpret_cast<void***>(inst) = vtable;
        *reinterpret_cast<uint64_t*>(inst + 0x10) = 77;
        struct Entry { void* inst; int32_t size, pad; };
        static Entry list[1] = {{inst, 0x28, 0}};
        struct Delegate { Entry* data; int32_t num, max; };
        static Delegate svc{list, 1, 1}, engine{nullptr, 0, 0};
        tlEngine = &engine;
        uint64_t out = 0;
        auto reset = [&](bool on, const uint8_t* exec) {
            SaveThumbLeftoverTestSetup(reinterpret_cast<void*>(&TlFakeAddRaw), reinterpret_cast<void*>(&TlFakeRemove), exec, on);
            tlCallCount = 0; tlAddCalls = 0;
        };
        // On: noted at the AddRaw, removed from the service after the engine Remove, note cleared, result passed through.
        reset(true, rawExec);
        const bool addOk = SaveThumbLeftoverTestAddRaw(&svc, &out, nullptr, nullptr) == &out && out == 77 && tlAddCalls == 1 &&
                           SaveThumbLeftoverTestNoted() == 77;
        tlEngineResult = false;
        const bool r1 = SaveThumbLeftoverTestRemove(&engine, 77);
        const bool seq = !r1 && tlCallCount == 2 && tlCalls[0].delegate == &engine && tlCalls[0].handle == 77 &&
                         tlCalls[1].delegate == &svc && tlCalls[1].handle == 77 && SaveThumbLeftoverTestNoted() == 0;
        tlCallCount = 0;
        tlEngineResult = true;
        const bool r2 = SaveThumbLeftoverTestRemove(&engine, 77);  // note cleared: only the engine Remove now
        const bool once = r2 && tlCallCount == 1 && tlCalls[0].delegate == &engine;
        Check(addOk && seq && once, "SaveThumbLeftoverFix hooks: AddRaw noted; Remove = the engine's first (its result passed "
                                    "through), then the service's once; the note cleared after");
        // Off: nothing noted, only the engine Remove.
        reset(false, rawExec);
        SaveThumbLeftoverTestAddRaw(&svc, &out, nullptr, nullptr);
        const bool offNoNote = SaveThumbLeftoverTestNoted() == 0 && tlAddCalls == 1;
        SaveThumbLeftoverTestRemove(&engine, 77);
        const bool offOnlyEngine = tlCallCount == 1 && tlCalls[0].delegate == &engine;
        // Another handle: only the engine Remove, the note kept.
        reset(true, rawExec);
        SaveThumbLeftoverTestAddRaw(&svc, &out, nullptr, nullptr);
        SaveThumbLeftoverTestRemove(&engine, 78);
        const bool otherHandle = tlCallCount == 1 && SaveThumbLeftoverTestNoted() == 77;
        // The entry with another ExecuteIfSafe: left alone.
        reset(true, rawExec);
        *reinterpret_cast<void***>(inst) = vtableOther;
        SaveThumbLeftoverTestAddRaw(&svc, &out, nullptr, nullptr);
        SaveThumbLeftoverTestRemove(&engine, 77);
        const bool otherExecLeft = tlCallCount == 1;
        *reinterpret_cast<void***>(inst) = vtable;
        // No raw ExecuteIfSafe known (not found at start-up): left alone.
        reset(true, nullptr);
        SaveThumbLeftoverTestAddRaw(&svc, &out, nullptr, nullptr);
        SaveThumbLeftoverTestRemove(&engine, 77);
        const bool noExecLeft = tlCallCount == 1;
        Check(offNoNote && offOnlyEngine && otherHandle && otherExecLeft && noExecLeft,
              "SaveThumbLeftoverFix hooks: off, another handle, another or unknown ExecuteIfSafe -> only the engine's Remove");
        SaveThumbLeftoverTestSetup(nullptr, nullptr, nullptr, false);
    }
}
