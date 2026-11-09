// BodyRemovalSpread self-test, included by selftest.cpp: its sites match on the mapped exe (build
// 19115871: ISM OnDestroyPhysicsState's call to ClearAllInstanceBodies, TermBody and its callees, RemoveFromWorld's two
// unregister loops, FChaosSolversModule::DestroySolver's call, FPhysScene_Chaos' deleting destructor exe+0x351A8D0 and its
// vtable slot exe+0x775AF00; bodies +0x6E8, actor +0x158, owner +0x160), and the queue logic with fakes (body_spread.cpp's
// BodySpreadTestLogic: stock outside a level removal, below the minimum, with a notifying body or a foreign body, near
// the player or with no known position; queued inside: maps at once in stock order, the clear empties, nothing terminated
// inside the removal, the post-actor tick's quota, the age bound in frames (also with no post-actor tick), a dead
// component's batch first within the quota, a deleted scene's batch terminated before its delete, the fallback drop at
// the solver's destroy, the fast map order, the measure-only split, the bounds-box distance).
#pragma once

static void BodySpreadRun(const GameModule& game)
{
    if (game.timeDateStamp == 0xF19077A4u) {
        uint32_t arr = 0, handle = 0, weak = 0;
        const int ok = BodySpreadTestDiscover(game, &arr, &handle, &weak);
        Check(ok == 1 && arr == 0x6E8 && handle == 0x158 && weak == 0x160,
              "BodyRemovalSpread: every site (and the scene destructor's vtable slot) matches on the mapped exe");
        if (ok != 1 || arr != 0x6E8 || handle != 0x158 || weak != 0x160)
            printf("    found %d, bodies +0x%X, actor +0x%X, owner +0x%X\n", ok, arr, handle, weak);
    }
    const int fail = BodySpreadTestLogic();
    Check(fail == 0, "BodyRemovalSpread: queue / drain / bound / drop logic with fakes");
    if (fail) printf("    failed checks (bits): 0x%X\n", fail);
}
