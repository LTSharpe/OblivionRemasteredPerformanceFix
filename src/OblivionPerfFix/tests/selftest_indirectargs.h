// IndirectArgBufferPool self-test (worker poolargs 2026-11-14), included by selftest.cpp: on the mapped exe the console variable
// d3d12.AllowPoolAllocateIndirectArgBuffers is found by its registration (the int-reference call, the int in .data) and its only
// use in the code (cmp dword [int],0 in the D3D12 buffer allocator), holding the stock 0 (exe+0x91F9E6C on Steam 19115871), and
// its auto-object's vtable slot (0 before registration); then the start-up rules on that int: already registered, key 0 and the
// OBSE64 route write nothing, a value already set is left alone, key 1 writes 1
// once, later ini changes are ignored, a player's Engine.ini value afterwards is kept. The int is put back to 0 at the end.
#pragma once

static void IndirectArgsRun()
{
    using namespace opf;
    const bool found = FindIndirectArgBufferPool();
    int32_t* const v = IndirectArgBufferPoolVar();
    Check(found && v && *v == 0, "indirectargs: d3d12.AllowPoolAllocateIndirectArgBuffers's int found (int-reference registration + its one test), the stock 0");
    if (!v) return;
    if (Game().timeDateStamp == 0xF19077A4u)
        Check(reinterpret_cast<uint8_t*>(v) - Game().base == 0x91F9E6C, "indirectargs: the int at exe+0x91F9E6C (Steam 19115871)");
    uint64_t* const vt = IndirectArgBufferPoolVtableSlot();
    Check(vt && *vt == 0, "indirectargs: the auto-object's vtable slot found after the registration call, 0 (not yet registered)");
    if (!vt) return;
    if (Game().timeDateStamp == 0xF19077A4u)
        Check(reinterpret_cast<uint8_t*>(vt) - Game().base == 0x91F9F30, "indirectargs: the auto-object at exe+0x91F9F30 (Steam 19115871)");
    IndirectArgBufferPoolTestReset();
    *vt = 0x1234;                       // the variable already registered (a late load)
    SetIndirectArgBufferPool(1, true);
    Check(*v == 0 && !IndirectArgBufferPoolApplied(), "indirectargs: already registered: nothing written");
    *vt = 0;
    IndirectArgBufferPoolTestReset();
    SetIndirectArgBufferPool(0, true);
    Check(*v == 0 && !IndirectArgBufferPoolApplied(), "indirectargs: key 0: nothing written");
    IndirectArgBufferPoolTestReset();
    SetIndirectArgBufferPool(1, false);
    Check(*v == 0 && !IndirectArgBufferPoolApplied(), "indirectargs: not before the game's code (OBSE64 route): nothing written");
    IndirectArgBufferPoolTestReset();
    *v = 2;
    SetIndirectArgBufferPool(1, true);
    Check(*v == 2 && !IndirectArgBufferPoolApplied(), "indirectargs: a value other than the stock 0 is left alone");
    *v = 0;
    IndirectArgBufferPoolTestReset();
    SetIndirectArgBufferPool(1, true);
    Check(*v == 1 && IndirectArgBufferPoolApplied(), "indirectargs: key 1 at start-up: 1 written");
    SetIndirectArgBufferPool(0, true);  // a second call (start-up is once)
    IndirectArgBufferPoolTick(0);       // the ini changed while running
    IndirectArgBufferPoolTick(1);
    Check(*v == 1, "indirectargs: start-up once; later ini changes are ignored (restart needed)");
    *v = 0;                             // the player's Engine.ini sets 0 after our write
    IndirectArgBufferPoolTick(1);
    IndirectArgBufferPoolTick(1);
    Check(*v == 0, "indirectargs: a later Engine.ini value is the player's and kept");
    *v = 0;
    IndirectArgBufferPoolTestReset();
}
