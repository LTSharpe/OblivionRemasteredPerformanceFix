// RigImportTemplate self-test (public since 1.3.0, worker rig130i; design rig130h), included by selftest.cpp: on the mapped exe
// every pattern is found once, the call targets are cross-checked (the guarded call's target is Reset_Impl's DestroyElement, the
// call lies in ImportBones' RemoveElement, RemoveElement's and Reset_Impl's Notify agree) and the 21 member offsets read from the
// game's code are the expected ones; nothing is installed. Then the guard and the copied-hierarchy table with fakes (a fake
// DestroyElement records the element and its OwnedInstances at the call; a fake CopyHierarchy counts): on a marked hierarchy a
// batch owner gives up its block (OwnedInstances 0 at the call, counted), a member (0) and a single element (1) are left alone;
// on an unmarked hierarchy an owner is left alone and counted as stock; a null element passes through; the 8th kept block
// switches template copies off for the session (a live 1 stays off) while the guard keeps acting; Altar's first import on an
// empty hierarchy forgets its mark; a full table makes no copy; a late install is refused. RT-N1: only the block owners the
// copy made (recorded right after it from the hierarchy's Elements) are kept; another block owner on a marked hierarchy (a
// stale mark at a reused address) is stock and does not count toward the cap.
#pragma once

namespace rtm {
struct DestroyCall {
    uint8_t* hier;
    uint8_t* elem;
    int owned;
};
DestroyCall g_calls[64];
int g_nCalls = 0;
int g_copies = 0;
void FakeDestroy(uint8_t* hier, uint8_t** pElem)
{
    uint8_t* const e = pElem ? *pElem : nullptr;
    int owned = -1;
    if (e) memcpy(&owned, e + 0xBC, 4);
    g_calls[g_nCalls++ & 63] = {hier, e, owned};
}
uint8_t* g_copyElems[16];  // what the fake copy leaves in the hierarchy's Elements (+0x70, num +0x78)
int g_copyElemNum = 0;
void FakeCopy(uint8_t* hier, uint8_t*)
{
    ++g_copies;
    uint8_t* const arr = reinterpret_cast<uint8_t*>(g_copyElems);
    memcpy(hier + 0x70, &arr, 8);
    memcpy(hier + 0x78, &g_copyElemNum, 4);
}
int Owned(const uint8_t* e)
{
    int v;
    memcpy(&v, e + 0xBC, 4);
    return v;
}
void SetOwned(uint8_t* e, int v) { memcpy(e + 0xBC, &v, 4); }

inline void RigTmplSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    const char* missing = nullptr;
    const char* detail = nullptr;
    Check(RigImportTemplateDiscover(game, &missing, &detail),
          "rigtmpl: 17 patterns found once (Altar's CacheBones, ImportBones, CopyHierarchy, Reset_Impl, NewObject, RemoveElement and its "
          "DestroyElement call, ...), call targets cross-checked, 21 member offsets read from the game's code as expected");
    if (missing) Log("[selftest] rigtmpl: %s %s", missing, detail ? detail : "");

    alignas(16) static uint8_t block[4][0x2C0];   // a copied bone block: owner + 3 members
    alignas(16) static uint8_t single[0x2C0];     // an element allocated alone
    alignas(16) static uint8_t extra[10][0x2C0];  // more block owners (the cap)
    alignas(16) static uint8_t hierA[0x300], hierB[0x300], tmpl[0x300];
    memset(block, 0, sizeof block);
    memset(single, 0, sizeof single);
    memset(extra, 0, sizeof extra);
    SetOwned(block[0], 4);
    SetOwned(single, 1);
    for (auto& x : extra) SetOwned(x, 2);
    g_nCalls = 0;
    g_copies = 0;
    g_copyElemNum = 0;  // the fake copy's Elements: the block (owner + members), the single element, the extra owners
    for (auto& b : block) g_copyElems[g_copyElemNum++] = b;
    g_copyElems[g_copyElemNum++] = single;
    for (int i = 0; i < 10; ++i) g_copyElems[g_copyElemNum++] = extra[i];

    RigTmplTestBegin(reinterpret_cast<void*>(&FakeDestroy), reinterpret_cast<void*>(&FakeCopy), 65536);
    Check(RigTmplTestCopy(hierA, tmpl) && g_copies == 1 && RigTmplTestGet().marks == 1 && RigTmplTestGet().copies == 1,
          "rigtmpl: a template copy stores its mark first, then copies");
    // the guard on the marked hierarchy
    uint8_t* e = block[1];
    RigTmplTestDestroy(hierA, &e);
    RigTmplTestCounts c = RigTmplTestGet();
    Check(g_nCalls == 1 && g_calls[0].hier == hierA && g_calls[0].elem == block[1] && g_calls[0].owned == 0 && c.removesOnCopied == 1 &&
              c.blocksKept == 0,
          "rigtmpl: a member of a copied block is removed as stock (counted as a remove on a copied hierarchy, nothing kept)");
    e = single;
    RigTmplTestDestroy(hierA, &e);
    c = RigTmplTestGet();
    Check(g_nCalls == 2 && g_calls[1].owned == 1 && Owned(single) == 1 && c.removesOnCopied == 2 && c.blocksKept == 0,
          "rigtmpl: a single element (OwnedInstances 1) is destroyed as stock");
    e = block[0];
    RigTmplTestDestroy(hierA, &e);
    c = RigTmplTestGet();
    Check(g_nCalls == 3 && g_calls[2].elem == block[0] && g_calls[2].owned == 0 && c.removesOnCopied == 3 && c.blocksKept == 1 &&
              c.elemsKept == 4 && c.mode == 1,
          "rigtmpl: the block owner on a copied hierarchy gives up its block before DestroyElement (OwnedInstances 0 at the call, 1 block "
          "of 4 kept)");
    // an unmarked hierarchy: stock, counted only
    SetOwned(block[0], 4);
    e = block[0];
    RigTmplTestDestroy(hierB, &e);
    c = RigTmplTestGet();
    Check(g_nCalls == 4 && g_calls[3].hier == hierB && g_calls[3].owned == 4 && Owned(block[0]) == 4 && c.stockOwnerRemoves == 1 &&
              c.blocksKept == 1 && c.removesOnCopied == 3,
          "rigtmpl: a block owner on a hierarchy the game copied itself is left alone (counted as a stock batch-owner remove)");
    e = nullptr;
    RigTmplTestDestroy(hierA, &e);
    c = RigTmplTestGet();
    Check(g_nCalls == 5 && g_calls[4].elem == nullptr && c.removesOnCopied == 3 && c.stockOwnerRemoves == 1,
          "rigtmpl: a null element passes through untouched");
    // the cap: the 8th kept block switches template copies off; the guard keeps acting; a live 1 stays off
    for (int i = 0; i < 7; ++i) {
        e = extra[i];
        RigTmplTestDestroy(hierA, &e);
    }
    c = RigTmplTestGet();
    Check(c.blocksKept == 8 && c.mode == 0, "rigtmpl: 8 kept blocks switch template copies off for the session");
    e = extra[7];
    RigTmplTestDestroy(hierA, &e);
    SetRigImportTemplate(1);
    c = RigTmplTestGet();
    Check(c.blocksKept == 9 && Owned(extra[7]) == 0 && g_calls[(g_nCalls - 1) & 63].owned == 0 && c.mode == 0,
          "rigtmpl: after the cap the guard still keeps blocks, and RigImportTemplate=1 stays off");
    // Altar's first import on an empty hierarchy at a marked address: the mark goes
    RigTmplTestNewImport(hierA);
    e = extra[8];
    RigTmplTestDestroy(hierA, &e);
    c = RigTmplTestGet();
    Check(c.marks == 0 && Owned(extra[8]) == 2 && c.blocksKept == 9 && c.stockOwnerRemoves == 2,
          "rigtmpl: a new hierarchy at a marked address loses the mark (its owners are stock again)");
    RigTmplTestEnd();
    // RT-N1: a marked hierarchy keeps only the blocks its copy made
    alignas(16) static uint8_t foreign[0x2C0];  // a block owner the copy did not make
    memset(foreign, 0, sizeof foreign);
    SetOwned(foreign, 3);
    SetOwned(block[0], 4);
    RigTmplTestBegin(reinterpret_cast<void*>(&FakeDestroy), reinterpret_cast<void*>(&FakeCopy), 65536);
    g_nCalls = 0;
    RigTmplTestCopy(hierA, tmpl);
    e = foreign;
    RigTmplTestDestroy(hierA, &e);
    c = RigTmplTestGet();
    Check(g_nCalls == 1 && g_calls[0].owned == 3 && Owned(foreign) == 3 && c.blocksKept == 0 && c.stockOwnerRemoves == 1 &&
              c.removesOnCopied == 1,
          "rigtmpl: a block owner the copy did not make (a stale mark at a reused address) is left alone and not counted toward "
          "the cap (RT-N1)");
    e = block[0];
    RigTmplTestDestroy(hierA, &e);
    c = RigTmplTestGet();
    Check(g_nCalls == 2 && g_calls[1].owned == 0 && c.blocksKept == 1 && c.elemsKept == 4,
          "rigtmpl: the copy's own block owner on the same hierarchy is still kept");
    RigTmplTestEnd();
    // a full table: no copy without a mark
    RigTmplTestBegin(reinterpret_cast<void*>(&FakeDestroy), reinterpret_cast<void*>(&FakeCopy), 1);
    g_copies = 0;
    const bool first = RigTmplTestCopy(hierA, tmpl);
    const bool second = RigTmplTestCopy(hierB, tmpl);
    c = RigTmplTestGet();
    Check(first && !second && g_copies == 1 && c.copies == 1 && c.marksFull == 1 && c.marks == 1,
          "rigtmpl: with the copied-hierarchy table full no copy is made (the game's import runs)");
    RigTmplTestEnd();
    Check(!InstallRigImportTemplate(false), "rigtmpl: refuses to install after the game's code started");
}

}  // namespace rtm
