// SaveRefIdIndex self-test, included by selftest.cpp: the sites on the mapped exe, then the helper (linear and indexed)
// against a reference linear search on fake formID tables: random ids with repeats (and 0), appends made by the fake
// append, appends made behind the helper's back, clears, and moves of the table's data (reallocation).
#pragma once
#include <random>

namespace srid {

struct Table {
    void* vtable = nullptr;
    uint32_t* data = nullptr;
    uint32_t cap = 0, count = 0;  // +0x10, +0x14
    uint32_t nonzero = 0, grow = 0;
};
static_assert(offsetof(Table, data) == 0x08 && offsetof(Table, count) == 0x14, "table layout");
struct SaveLoad {
    uint8_t pad[0xE8] = {};
    Table* table = nullptr;  // +0xE8
};
static_assert(offsetof(SaveLoad, table) == 0xE8, "save-load layout");

std::vector<std::vector<uint32_t>> g_storage;
uint64_t FakeAppend(Table* t, uint32_t at, const uint32_t* id)
{
    if (at >= t->cap) {
        g_storage.emplace_back(static_cast<size_t>(at) * 2 + 8);
        auto& v = g_storage.back();
        for (uint32_t i = 0; i < t->count; ++i) v[i] = t->data[i];
        t->data = v.data();
        t->cap = static_cast<uint32_t>(v.size());
    }
    t->data[at] = *id;
    if (at >= t->count) t->count = at + 1;
    return at;
}

uint32_t RefLinear(Table& t, uint32_t id)
{
    for (uint32_t i = 0; i < t.count; ++i)
        if (t.data[i] == id) return i;
    uint32_t c = id;
    return static_cast<uint32_t>(FakeAppend(&t, t.count, &c));
}

inline void SaveRefIdSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    Check(InstallSaveRefIdIndex(true), "saverefid: SaveGame, SaveFormIDs' and GetRefID's lookups found, installed");
    {
        const uint8_t* const b = game.base;
        Check(b[0x67198B0] == 0xFF && b[0x67198B1] == 0x25, "saverefid: SaveGame's entry (exe+0x67198B0) jumps to the hook");
        Check(b[0x671C2F2] == 0x48 && b[0x671C2F8] == 0xB8 && b[0x671C301] == 0xFF && b[0x671C302] == 0xD0 && b[0x671C303] == 0xEB &&
                  b[0x671C304] == 0x1B,
              "saverefid: SaveFormIDs' lookup block (exe+0x671C2F2) calls the helper and resumes at exe+0x671C320");
        Check(b[0x6703670] == 0x48 && b[0x6703681] == 0xEB && b[0x6703682] == 0x1B,
              "saverefid: GetRefID's lookup block (exe+0x6703670) calls the helper and resumes at exe+0x670369E");
    }
    std::mt19937 rng(11);
    bool same = true;
    int cases = 0;
    for (int mode : {0, 1}) {
        for (int run = 0; run < 6; ++run) {
            g_storage.clear();
            Table mine, ref;
            SaveLoad sl;
            sl.table = &mine;
            SaveRefIdTestSetup(reinterpret_cast<void*>(&FakeAppend), mode);
            SaveRefIdTestNewSave();
            const uint32_t range = run < 2 ? 50u : run < 4 ? 5000u : 200000u;
            for (int i = 0; i < 60000; ++i) {
                const uint32_t r = rng() % 1000;
                if (r == 0 && run % 2) {  // a clear (a new save) behind the helper's back
                    mine.count = 0;
                    ref.count = 0;
                    continue;
                }
                if (r == 1) {  // an append made elsewhere (another inline copy of the lookup)
                    const uint32_t id = rng() % range;
                    RefLinear(mine, id);
                    RefLinear(ref, id);
                    continue;
                }
                const uint32_t id = (r < 5) ? 0u : rng() % range;
                const uint32_t a = static_cast<uint32_t>(SaveRefIdTestCall(&sl, id, (i % 7) != 3));
                const uint32_t b = RefLinear(ref, id);
                same &= a == b && mine.count == ref.count;
                ++cases;
            }
            same &= mine.count == ref.count && std::equal(mine.data, mine.data + mine.count, ref.data);
        }
    }
    Check(same, "saverefid: the helper (linear, indexed, verify) returns the linear search's refID and builds the same table "
                "(random formIDs with repeats and 0, clears, outside appends, reallocations)");
    SaveRefIdTestSetup(reinterpret_cast<void*>(&FakeAppend), 0);
    Log("[saverefid] self-test: %d lookups compared", cases);
}

}  // namespace srid
