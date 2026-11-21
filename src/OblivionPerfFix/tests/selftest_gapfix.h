// TopicSort / PluginPrefetch self-test (worker gapfix, 2026-11-16, S-058), included by selftest.cpp.
// TopicSort: on the mapped exe (Steam 19115871) the sort exe+0x684EC60, its _stricmp import slot exe+0x6D98228, the data
// handler pointer exe+0x9480018 (+0xF8: the topic list) and the default name exe+0x870F14F. Then the GAME'S OWN sort, run
// from the mapped exe (its _stricmp slot pointed at the C runtime's _stricmp), against ours on the same lists: random
// lists with case variants, ties, empty and missing names and nodes with no item, sorted / reversed / all-equal lists and a
// topic-shaped list of 4,000 names; every node's item must be the same. Then the patched entry: key 0 runs the game's
// sort through the trampoline, key 1 ours; the default list (argument null) through a fake data handler.
// PluginPrefetch: the game's literal in the mapped exe, the data folder from the exe path, the Plugins.txt parser.
#pragma once

#include <random>

namespace gapfix_test {
struct Item {
    uint8_t bytes[0x40];
    uint32_t& id() { return *reinterpret_cast<uint32_t*>(bytes + 0x10); }
    const char*& name() { return *reinterpret_cast<const char**>(bytes + 0x38); }
};
struct Node {
    uint8_t* item;
    Node* next;
};
using SortFn = void (*)(void*);

// two lists with the same items (nullptr = a node with no item)
struct Pair {
    std::vector<Node> a, b;
    void build(const std::vector<Item*>& items)
    {
        a.assign(items.size(), Node{});
        b.assign(items.size(), Node{});
        for (size_t i = 0; i < items.size(); ++i) {
            uint8_t* const it = items[i] ? items[i]->bytes : nullptr;
            a[i] = {it, i + 1 < items.size() ? &a[i + 1] : nullptr};
            b[i] = {it, i + 1 < items.size() ? &b[i + 1] : nullptr};
        }
    }
    bool same() const
    {
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].item != b[i].item) return false;
        return true;
    }
};

int Stricmp(const char* x, const char* y) { return _stricmp(x, y); }
}  // namespace gapfix_test

static void GapFixRun(const GameModule& game)
{
    using namespace gapfix_test;
    // ---- PluginPrefetch ----
    Check(PluginPrefetchFindLiteral(game), "PluginPrefetch: the game's literal Dev/ObvData/Data/Plugins.txt found in the mapped exe");
    Check(PluginPrefetchDataDir(L"C:\\Games\\Oblivion Remastered\\OblivionRemastered\\Binaries\\Win64\\OblivionRemastered-Win64-Shipping.exe",
                                L"Dev/ObvData/Data/Plugins.txt") == L"C:\\Games\\Oblivion Remastered\\OblivionRemastered\\Content\\Dev\\ObvData\\Data\\",
          "PluginPrefetch: the data folder = <project above Binaries\\Win64>\\Content\\ + the game's literal's folder");
    Check(PluginPrefetchDataDir(L"C:\\x\\Game.exe", L"Dev/ObvData/Data/Plugins.txt").empty() &&
              PluginPrefetchDataDir(L"C:\\p\\Binaries\\Win64\\g.exe", L"Dev/ObvData/Data/Other.txt").empty(),
          "PluginPrefetch: an exe outside Binaries\\Win64 or another literal: no folder (not started)");
    {
        const char text[] = "\xEF\xBB\xBFOblivion.esm\r\nDLCOrrery.esp\n\n# comment.esp\n; also.esp\n  Knights.esp \t\n..\\up.esp\nsub/x.esp\n"
                            "C:x.esp\nreadme.txt\n*Star.esp\nAltar.ESL\nOblivion.esm.bak\n..esp";
        std::vector<std::wstring> names;
        PluginPrefetchParse(text, sizeof text - 1, &names);
        const bool ok = names.size() == 4 && names[0] == L"Oblivion.esm" && names[1] == L"DLCOrrery.esp" && names[2] == L"Knights.esp" &&
                        names[3] == L"Altar.ESL";
        Check(ok, "PluginPrefetch: Plugins.txt parse: BOM, CRLF, trims, comments, no folders / drives / wildcards / '..', only .esm/.esp/.esl");
        if (!ok) for (auto& n : names) printf("    name '%ls'\n", n.c_str());
        std::vector<std::wstring> none;
        PluginPrefetchParse("", 0, &none);
        Check(none.empty(), "PluginPrefetch: an empty Plugins.txt gives no names");
    }

    // ---- TopicSort ----
    if (game.timeDateStamp != 0xF19077A4u) return;  // the game's own sort runs from the mapped exe: only on the build we know
    uint32_t rva[5] = {};
    int32_t off = 0;
    const bool found = TopicSortTestDiscover(game, rva, &off);
    Check(found && rva[0] == 0x684EC60 && rva[1] == 0x6D98228 && rva[2] == 0x9480018 && rva[3] == 0x870F14F && off == 0xF8,
          "TopicSort: the topic list sort, its _stricmp import, the default list ([handler]+0xF8) and default name on the mapped exe");
    if (!found) return;
    if (rva[0] != 0x684EC60) printf("    found %X slot %X handler %X def %X off %X\n", rva[0], rva[1], rva[2], rva[3], off);
    Check(*reinterpret_cast<const char*>(game.base + rva[3]) == 0 && *reinterpret_cast<const char*>(game.base + rva[4]) == 0,
          "TopicSort: both default names (this item's and the next item's) are the empty string");

    // the game's sort from the mapped exe: its import slot pointed at _stricmp for the test, put back after
    void** const slot = reinterpret_cast<void**>(game.base + rva[1]);
    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        Check(false, "TopicSort: the mapped exe's import slot writable for the test");
        return;
    }
    void* const keepSlot = *slot;
    *slot = reinterpret_cast<void*>(&Stricmp);
    const SortFn gameSort = reinterpret_cast<SortFn>(game.base + rva[0]);

    std::mt19937 rng(0x5EED0458u);
    const char* pool[] = {"Rumors", "rumors", "RUMORS", "Rumor", "Rum", "", "Imperial City", "imperial city", "_under", "[bracket",
                          "zeta", "{brace", "Z", "a", "Aa", "aA", "Mages Guild", "Mages' Guild", "mages guild ", "Fighters Guild",
                          "Dark Brotherhood", "dark brotherhood", "Oblivion", "Kvatch", "kvatch", "Bruma", "~tilde", "123", "1-2", "\xE9t\xE9"};
    std::vector<Item> store(5000);
    for (size_t i = 0; i < store.size(); ++i) {
        memset(store[i].bytes, 0xCD, sizeof store[i].bytes);
        store[i].id() = 0x01000000u + static_cast<uint32_t>(i);
    }
    int fails = 0, trials = 0;
    Pair pr;
    auto runPair = [&](std::vector<Item*>& items) {
        pr.build(items);
        gameSort(pr.a.empty() ? nullptr : pr.a.data());
        TopicSortTestSort(pr.b.empty() ? nullptr : pr.b.data(), &Stricmp);
        ++trials;
        if (!pr.same()) ++fails;
    };
    std::vector<Item*> items;
    for (int t = 0; t < 600; ++t) {
        const size_t n = 1 + rng() % (t < 300 ? 12 : 160);
        items.clear();
        for (size_t i = 0; i < n; ++i) {
            Item* const it = &store[i];
            const unsigned r = rng() % 40;
            it->name() = r < 30 ? pool[r] : r < 33 ? nullptr : pool[rng() % 6];  // many ties and missing names
            items.push_back(it);
        }
        if (t % 7 == 3 && n > 2) items[1 + rng() % (n - 2)] = nullptr;  // a node with no item in the middle
        if (t % 11 == 5) items[0] = nullptr;                             // the head has no item
        runPair(items);
    }
    Check(fails == 0, "TopicSort: 600 random lists (ties, case variants, missing names, nodes with no item): the game's sort and ours give every node the same item");
    if (fails) printf("    %d of %d differ\n", fails, trials);
    // sorted, reversed, all equal, two items
    fails = 0;
    {
        static const char* const sorted[] = {"a", "B", "c", "D", "e", "F", "g"};
        items.clear();
        for (int i = 0; i < 7; ++i) store[i].name() = sorted[i], items.push_back(&store[i]);
        runPair(items);
        items.clear();
        for (int i = 0; i < 7; ++i) store[i].name() = sorted[6 - i], items.push_back(&store[i]);
        runPair(items);
        const bool reversedSorted = pr.a[0].item == store[6].bytes && pr.a[6].item == store[0].bytes;
        items.clear();
        for (int i = 0; i < 9; ++i) store[i].name() = (i % 2) ? "SAME" : "same", items.push_back(&store[i]);
        runPair(items);
        bool stable = true;
        for (int i = 0; i < 9; ++i) stable = stable && pr.b[i].item == store[i].bytes;
        items.clear();
        store[0].name() = "b";
        store[1].name() = "A";
        items = {&store[0], &store[1]};
        runPair(items);
        Check(fails == 0 && reversedSorted && stable && pr.b[0].item == store[1].bytes,
              "TopicSort: sorted, reversed, all-equal (order kept) and two-item lists: same as the game's sort");
    }
    // topic-shaped: 4,000 names of 1-3 words, a fifth of them repeated
    {
        static const char* const words[] = {"Rumors", "Imperial", "City", "Guild", "Mages", "Fighters", "Thieves", "Kvatch", "Bruma",
                                            "Anvil", "Chorrol", "Skingrad", "Leyawiin", "Cheydinhal", "Oblivion", "Gate", "Amulet",
                                            "of", "Kings", "the", "Dark", "Brotherhood", "Arena", "Shrine", "daedra", "Lord",
                                            "Training", "Spells", "Arcane", "University", "Martin", "Jauffre", "Baurus", "Septim"};
        static std::vector<std::string> names(4000);
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0 && rng() % 5 == 0) {
                names[i] = names[rng() % i];
                continue;
            }
            std::string s;
            const int w = 1 + rng() % 3;
            for (int k = 0; k < w; ++k) s += std::string(k ? " " : "") + words[rng() % (sizeof words / sizeof *words)];
            if (rng() % 4 == 0) for (char& c : s) c = static_cast<char>(tolower(static_cast<uint8_t>(c)));
            names[i] = s;
        }
        items.clear();
        for (size_t i = 0; i < names.size(); ++i) store[i].name() = names[i].c_str(), items.push_back(&store[i]);
        const int before = fails;
        LARGE_INTEGER f, t0, t1, t2;
        QueryPerformanceFrequency(&f);
        pr.build(items);
        QueryPerformanceCounter(&t0);
        gameSort(pr.a.data());
        QueryPerformanceCounter(&t1);
        TopicSortTestSort(pr.b.data(), &Stricmp);
        QueryPerformanceCounter(&t2);
        if (!pr.same()) ++fails;
        Check(fails == before, "TopicSort: a topic-shaped list of 4,000 names (a fifth repeated, case variants): same as the game's sort");
        printf("    4,000 names: the game's sort %.1f ms, ours %.2f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
               (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart);
    }

    // the patched entry (the mapped exe's code): key 0 = the game's sort through the trampoline, key 1 = ours
    if (TopicSortTestInstall(game, 0)) {
        const uint8_t* const e = game.base + rva[0];
        Check(e[0] == 0xE9, "TopicSort: the entry jumps to the hook");
        SortFn entry = reinterpret_cast<SortFn>(game.base + rva[0]);
        bool ok = true;
        for (int mode = 0; mode <= 2; ++mode) {
            if (mode == 2) break;
            TopicSortTestMode(mode);
            for (int t = 0; t < 40; ++t) {
                const size_t n = 2 + rng() % 60;
                items.clear();
                for (size_t i = 0; i < n; ++i) store[i].name() = pool[rng() % 30], items.push_back(&store[i]);
                if (t % 5 == 2) items[n / 2] = nullptr;
                pr.build(items);
                entry(pr.a.data());
                TopicSortTestSort(pr.b.data(), &Stricmp);
                ok = ok && pr.same();
            }
        }
        Check(ok, "TopicSort: through the patched entry, key 0 (the game's sort via the trampoline), key 1 (ours) give the sorted order");
        // the default list: argument null = the list at [handler] + the offset (a fake handler object for the test)
        {
            uint8_t** const hslot = reinterpret_cast<uint8_t**>(game.base + rva[2]);
            DWORD hOld = 0;
            if (VirtualProtect(hslot, sizeof(void*), PAGE_READWRITE, &hOld)) {
                uint8_t* const keepH = *hslot;
                bool okDef = true;
                TopicSortTestMode(1);
                for (int t = 0; t < 20; ++t) {
                    const size_t n = 2 + rng() % 40;
                    items.clear();
                    for (size_t i = 0; i < n; ++i) store[i].name() = pool[rng() % 30], items.push_back(&store[i]);
                    pr.build(items);
                    *hslot = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pr.a.data()) - static_cast<uintptr_t>(off));
                    entry(nullptr);
                    gameSort(pr.b.data());
                    okDef = okDef && pr.same();
                }
                *hslot = nullptr;
                entry(nullptr);  // no handler object: nothing to sort, must not crash
                *hslot = keepH;
                VirtualProtect(hslot, sizeof(void*), hOld, &hOld);
                Check(okDef, "TopicSort: key 1 with a null argument sorts the data handler's default list like the game's sort");
            } else {
                Check(false, "TopicSort: the mapped exe's handler pointer writable for the test");
            }
        }
        TopicSortTestMode(0);
    } else {
        Check(false, "TopicSort: the mapped exe's entry patched for the test");
    }
    *slot = keepSlot;
    VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
}
