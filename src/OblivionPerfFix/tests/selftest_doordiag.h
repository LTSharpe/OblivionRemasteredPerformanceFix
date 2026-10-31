// Diagnostic DoorDiag self-test, included by selftest.cpp: its sites on the mapped exe (locate only; the
// diagnostic is never installed in the test). Checks named "at exe+0x..." pin build 19115871.
#pragma once

namespace dd {
void Run(const GameModule& game)
{
    DoorDiagInfoOut i{};
    Check(FindDoorDiag(&i), "DoorDiag: pool, flush flag, free-all, find-free-element, element size and stream-all found");
    if (!i.pool) return;
    auto rva = [&](const void* p) { return static_cast<unsigned>(static_cast<const uint8_t*>(p) - game.base); };
    Check(rva(i.pool) == 0x8DD9680, "DoorDiag: render-target pool at exe+0x8DD9680");
    Check(rva(i.flushFlag) == 0x8DD1F8C, "DoorDiag: r.bFlushRenderTargetsOnWorldCleanup at exe+0x8DD1F8C");
    Check(rva(i.freeAll) == 0x26DD640, "DoorDiag: free-all at exe+0x26DD640");
    Check(rva(i.findFree) == 0x26DC760, "DoorDiag: find-free-element at exe+0x26DC760");
    Check(rva(i.elemSize) == 0x26D67E0, "DoorDiag: element size at exe+0x26D67E0");
    Check(rva(i.streamAll) == 0x30D6CB0, "DoorDiag: stream-all at exe+0x30D6CB0");
    Check(i.streamAllSlot && *i.streamAllSlot == i.streamAll, "DoorDiag: stream-all's vtable slot holds it");
}
}  // namespace dd
