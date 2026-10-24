// PhysicsDestroyBatch self-test (worker physwait; public since 1.1.9), included by selftest.cpp: the call sites, the solver offsets the
// guard reads (cluster unions, geometry collections) and the proxy's deleting destructor with its DirtyIdx check, all found by
// pattern on the mapped exe; the wrapper run with fake proxies and a fake destroy loop in every mode (each proxy deleted
// exactly once before the wrapper returns; UpdateViews n times when off, once with the views part on, n times again when a
// geometry-collection or cluster-union proxy is pending; an unknown mode acts as off); and two or three solvers' steps in the
// wrapper at the same time (no proxy missed or deleted twice).
#pragma once

namespace dbt {

inline void DestroyBatchSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    Check(PhysicsDestroyBatchTestSites(game),
          "destroy batch: DestroyPendingProxies' call, its proxy delete, SOA UpdateViews call, GC/CU offsets and the proxy destructor's DirtyIdx check found");
    bool ok = true;
    for (int mode : {4, 1, 2, 3, 7}) {
        for (int n : {1, 300, 5000}) {
            const char* why = PhysicsDestroyBatchTestRun(mode, n, 0, 0);
            if (why) { Check(false, why); ok = false; }
        }
    }
    Check(ok, "destroy batch: wrapper with fakes in modes 4/1/2/3/7 (deletes once, views as expected)");
    const char* gc = PhysicsDestroyBatchTestRun(3, 300, 1, 0);
    const char* cu = PhysicsDestroyBatchTestRun(3, 300, 0, 2);
    Check(!gc && !cu, gc ? gc : cu ? cu : "destroy batch: views not deferred while a GC or cluster-union proxy is pending");
    const char* two = PhysicsDestroyBatchTestConcurrent(2, 200, 3000);
    Check(two == nullptr, two ? two : "destroy batch: two concurrent solvers, 200 rounds x 3000: 0 missed, 0 double deletes");
    const char* paths = PhysicsDestroyBatchTestPaths();
    Check(paths == nullptr, paths ? paths : "destroy batch: inline paths (DirtyIdx set, another vtable), no helper, sleeping helper, stall guard -> off");
    const char* three = PhysicsDestroyBatchTestConcurrent(3, 50, 2000);
    Check(three == nullptr, three ? three : "destroy batch: three concurrent solvers: 0 missed, 0 double deletes");
}

}  // namespace dbt
