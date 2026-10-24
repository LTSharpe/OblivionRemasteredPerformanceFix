// ObjectReplaceCrashGuard self-test (worker ingamebug), included by selftest.cpp: StaticAllocateObject's in-place destroy,
// ~UObjectBase and the old index's frame slot on the mapped exe, the patch, then the stub run through a thunk that sets up
// the game's state at that call (rcx = the object, the old index at [rsp+off]) with a fake GUObjectArray and fake objects
// whose destructor frees their slot like ~UObjectBase (only with a class pointer): a normal destroy is left alone, a
// destroy that kept the slot gets it released (mode 1) or only logged (mode 2 / 0), other slots and bad indices untouched.
#pragma once

namespace objrep {

struct FakeItem {
    void* object;
    int32_t flags, cluster, serial, pad;
};
FakeItem g_items[16];
FakeItem* g_chunks[1] = {g_items};
alignas(16) uint8_t g_array[0x100];
int g_dtorCalls = 0;
uint32_t g_dtorFlags = 0xFFFFFFFF;
void* g_baseVt[1] = {};

// ~UObject of a fake object: like ~UObjectBase, writes the base vtable and frees the slot only with a class pointer
void FakeDtor(uint8_t* obj, uint32_t flags)
{
    ++g_dtorCalls;
    g_dtorFlags = flags;
    void* vt = g_baseVt;
    memcpy(obj, &vt, 8);
    void* cls;
    memcpy(&cls, obj + 0x10, 8);
    int32_t index;
    memcpy(&index, obj + 0xC, 4);
    if (cls && index >= 0 && index < 16) g_items[index] = {};
}
void* g_vt[1] = {reinterpret_cast<void*>(&FakeDtor)};

struct FakeObject {
    alignas(16) uint8_t b[0x40];
    void Init(int32_t index, bool withClass)
    {
        memset(b, 0, sizeof b);
        void* vt = g_vt;
        memcpy(b, &vt, 8);
        memcpy(b + 0xC, &index, 4);
        void* cls = withClass ? static_cast<void*>(g_items) : nullptr;  // any non-null pointer
        memcpy(b + 0x10, &cls, 8);
    }
};

inline void ObjectReplaceSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    if (!Installed(ApplyObjectReplaceGuard(), "ObjectReplaceCrashGuard: StaticAllocateObject's in-place destroy found and patched")) return;
    ObjectReplaceGuardInfoOut info{};
    Check(ObjectReplaceGuardInfo(&info), "replace: guard info");
    Check(info.site == game.base + 0x111F8E5 && info.call == game.base + 0x111F902, "replace: destroy at exe+0x111F8E5, call at exe+0x111F902");
    Check(info.objectArray == game.base + 0x9112A60, "replace: GUObjectArray at exe+0x9112A60");
    Check(info.oldIndexOffset == 0x54, "replace: the old index at [rsp+0x54] (stored at exe+0x111F808, read for AddObject at exe+0x111F7BD)");
    Check(info.baseVtable == game.base + 0x6E7D0A8 && info.objInitialized == game.base + 0x9112B30 &&
              info.criticalError == game.base + 0x903C5E4,
          "replace: ~UObjectBase's vtable, GObjInitialized and GIsCriticalError");
    Check(ObjectReplaceGuardTestPatched(), "replace: mov rcx,rdi; mov rax,[rdi]; call [rax] -> mov rcx,rdi; call jump");
    Check(ApplyObjectReplaceGuard(), "replace: second installation finds the guard in place");

    // thunk (the game's state at the patched call: rcx = the object, the old index at [rsp+off]):
    // push rdi; sub rsp,60h; mov [rsp+off],edx; mov rax,jump; call rax; add rsp,60h; pop rdi; ret
    auto* thunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!thunk) return Check(false, "replace: thunk memory");
    const uint8_t t[] = {0x57, 0x48, 0x83, 0xEC, 0x60, 0x89, 0x54, 0x24, static_cast<uint8_t>(info.oldIndexOffset),
                         0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x60, 0x5F, 0xC3};
    memcpy(thunk, t, sizeof t);
    memcpy(thunk + 11, &info.stub, 8);
    DWORD old;
    VirtualProtect(thunk, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 64);
    const auto run = reinterpret_cast<void (*)(void* obj, int32_t index)>(thunk);

    uint8_t* const realArray = info.objectArray;
    memset(g_array, 0, sizeof g_array);
    FakeItem** chunks = g_chunks;
    memcpy(g_array + 0x10, &chunks, 8);
    const int32_t num = 16;
    memcpy(g_array + 0x24, &num, 4);
    ObjectReplaceGuardTestArray(g_array);
    SetObjectReplaceGuardMode(1);
    FakeObject o;
    auto put = [](int i, void* obj) { g_items[i] = {obj, 0, 0, 77, 0}; };

    ObjectReplaceGuardInfo(&info);
    const uint64_t k0 = info.kept, r0 = info.released, n0 = info.replaced;
    o.Init(5, true);
    put(5, o.b);
    run(o.b, 5);
    ObjectReplaceGuardInfo(&info);
    Check(g_dtorCalls == 1 && g_dtorFlags == 0, "replace: the guard makes the game's destructor call once (flags 0)");
    Check(g_items[5].object == nullptr && info.kept == k0 && info.released == r0 && info.replaced == n0 + 1,
          "replace: a destroy that frees its slot (the game's normal case) is left as it is");

    o.Init(6, false);  // the crash's state: no class pointer, ~UObjectBase keeps the slot
    put(6, o.b);
    g_items[6].flags = 0x40;
    run(o.b, 6);
    ObjectReplaceGuardInfo(&info);
    Check(info.kept == k0 + 1 && info.released == r0 + 1, "replace (1): a destroy that kept its slot is counted and released");
    Check(g_items[6].object == nullptr && g_items[6].flags == 0 && g_items[6].cluster == 0 && g_items[6].serial == 0,
          "replace (1): the slot is cleared as FreeUObjectIndex clears it");

    SetObjectReplaceGuardMode(0);  // switched off live: the game's way (it stops at AddObject)
    o.Init(7, false);
    put(7, o.b);
    run(o.b, 7);
    ObjectReplaceGuardInfo(&info);
    Check(g_items[7].object == o.b && g_items[7].serial == 77 && info.kept == k0 + 2 && info.released == r0 + 1,
          "replace (0): the kept slot is only counted, left for the game's own check");
    SetObjectReplaceGuardMode(1);

    FakeObject other;
    other.Init(8, true);
    o.Init(8, false);
    put(8, other.b);  // another object at that index: not the destroyed one's slot
    run(o.b, 8);
    ObjectReplaceGuardInfo(&info);
    Check(g_items[8].object == other.b && info.kept == k0 + 2, "replace: a slot holding another object is not touched");
    o.Init(40, false);
    run(o.b, 40);
    o.Init(-1, false);
    run(o.b, -1);
    ObjectReplaceGuardInfo(&info);
    Check(info.kept == k0 + 2, "replace: an index outside GUObjectArray is ignored");
    LogObjectReplaceGuardStats();
    ObjectReplaceGuardTestArray(realArray);
}

}  // namespace objrep
