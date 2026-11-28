// MenuFileCache self-test (worker menufile 2026-11-25), included by selftest.cpp: the discovery on the mapped exe (the file get's
// one call of the menu XML loader, the loader's entry and its two allocations through the game's operator new), the install
// (the call goes to a stub that jumps to the hook), then the hook with a fake stock loader and a fake operator new: mode 0
// always asks the loader; mode 1 keeps a menu path's text after two identical reads and then hands out fresh copies (a
// differing pair is not kept; the miss path returns the loader's own block; same bytes, NUL at [len], both
// blocks from operator new, never the kept bytes themselves); an in-memory text, a path outside Data\Menus, a failed or empty
// read are asked every time; lab mode 2 returns the loader's own result and counts differences; a file past the size limit
// is not kept, a smaller later one still is.
#pragma once

#include <cstdlib>
#include <set>
#include <string>

namespace mfc {
struct Buf {
    uint32_t len, pad;
    char* data;
};
int g_calls = 0;
std::string g_text = "<menu><rect name=\"a\"/></menu>";
bool g_fail = false;      // the loader returns nothing
size_t g_bigLen = 0;      // > 0: the loader returns that many bytes of 'x'
std::set<void*> g_newed;  // blocks from the fake operator new
void* FakeNew(size_t n)
{
    void* const p = malloc(n);
    g_newed.insert(p);
    return p;
}
void FakeFree(void* p)
{
    g_newed.erase(p);
    free(p);
}
void* g_lastLoaded = nullptr;  // the block the fake loader returned last
void* FakeLoader(const char*, const char* text)
{
    ++g_calls;
    if (g_fail) return nullptr;
    const std::string src = g_bigLen ? std::string(g_bigLen, 'x') : text ? std::string(text) : g_text;
    char* const d = static_cast<char*>(FakeNew(src.size() + 1));
    memcpy(d, src.data(), src.size());
    d[src.size()] = 0;
    Buf* const b = static_cast<Buf*>(FakeNew(sizeof(Buf)));
    b->len = static_cast<uint32_t>(src.size());
    b->pad = 0;
    b->data = d;
    g_lastLoaded = b;
    return b;
}
// the parse's frees
void Release(void* r)
{
    if (!r) return;
    Buf* const b = static_cast<Buf*>(r);
    FakeFree(b->data);
    FakeFree(b);
}
}  // namespace mfc

static void MenuFileRun(const GameModule& game)
{
    MenuFileDiscovery d{};
    Check(MenuFileDiscover(game, &d), "MenuFileCache: discovery on the mapped exe");
    Check(d.sites == 1, "MenuFileCache: one call site of the menu file loader");
    if (game.timeDateStamp == 0xF19077A4u)
        Check(d.call - game.base == 0x66F9024 && d.loader - game.base == 0x66F8950 && d.opNew - game.base == 0x4702CB0,
              "MenuFileCache: call exe+0x66F9024, loader exe+0x66F8950, operator new exe+0x4702CB0 (build 19115871)");
    if (!d.call) return;
    Check(MenuFileTestInstall(), "MenuFileCache: installs on the mapped exe");
    {
        int32_t rel;
        memcpy(&rel, d.call + 1, 4);
        const uint8_t* const stub = d.call + 5 + rel;
        void* target = nullptr;
        memcpy(&target, stub + 6, 8);
        Check(d.call[0] == 0xE8 && stub[0] == 0xFF && stub[1] == 0x25 && target == MenuFileTestHook(),
              "MenuFileCache: the loader call goes to the stub, the stub to the hook");
    }
    MenuFileTestSetFns(reinterpret_cast<void*>(&mfc::FakeLoader), reinterpret_cast<void*>(&mfc::FakeNew));
    using Hook = void* (*)(const char* path, const char* text);
    const auto hook = reinterpret_cast<Hook>(MenuFileTestHook());
    const char* const path = "Data\\Menus\\Dialog\\dialog_menu.xml";
    MenuFileStats s0{}, s1{};

    SetMenuFileCache(0);
    MenuFileTestStats(&s0);
    mfc::g_calls = 0;
    mfc::Release(hook(path, nullptr));
    mfc::Release(hook(path, nullptr));
    MenuFileTestStats(&s1);
    Check(mfc::g_calls == 2 && s1.stored == s0.stored && s1.hits == s0.hits, "MenuFileCache 0: every open asks the loader, nothing kept");

    SetMenuFileCache(1);
    mfc::g_calls = 0;
    void* const first = hook(path, nullptr);
    Check(first && first == mfc::g_lastLoaded, "MenuFileCache 1: a miss returns the loader's own block unchanged");
    mfc::Release(first);
    MenuFileTestStats(&s0);
    Check(s0.stored == s1.stored, "MenuFileCache 1: one read is not kept yet (review S3-1)");
    mfc::Release(hook(path, nullptr));
    void* const r = hook(path, nullptr);
    MenuFileTestStats(&s0);
    Check(mfc::g_calls == 2 && s0.stored == s1.stored + 1 && s0.hits == s1.hits + 1,
          "MenuFileCache 1: a menu file is kept after two identical reads, then copied from memory");
    const auto* const b = static_cast<const mfc::Buf*>(r);
    Check(b && b->len == mfc::g_text.size() && memcmp(b->data, mfc::g_text.data(), b->len) == 0 && b->data[b->len] == 0,
          "MenuFileCache 1: the copy has the loader's bytes and length, NUL at [len]");
    Check(b && mfc::g_newed.count(const_cast<mfc::Buf*>(b)) && mfc::g_newed.count(b->data),
          "MenuFileCache 1: both blocks of the copy come from the game's operator new");
    void* const r2 = hook(path, nullptr);
    Check(r2 && r2 != r && static_cast<mfc::Buf*>(r2)->data != b->data, "MenuFileCache 1: each open gets its own fresh copy");
    static_cast<mfc::Buf*>(r2)->data[0] = '#';  // the parse may write into its buffer
    mfc::Release(r2);
    void* const r3 = hook(path, nullptr);
    Check(r3 && static_cast<mfc::Buf*>(r3)->data[0] == '<', "MenuFileCache 1: a copy written by its user does not change the kept bytes");
    mfc::Release(r3);
    mfc::Release(r);

    mfc::g_calls = 0;
    mfc::Release(hook(path, "<inline/>"));
    mfc::Release(hook(path, "<inline/>"));
    Check(mfc::g_calls == 2, "MenuFileCache 1: an in-memory text goes to the loader every time");
    const char* const other = "Data\\Textures\\menus\\x.xml";
    mfc::g_calls = 0;
    mfc::Release(hook(other, nullptr));
    mfc::Release(hook(other, nullptr));
    Check(mfc::g_calls == 2, "MenuFileCache 1: a path outside Data\\Menus is read every time");
    mfc::g_fail = true;
    mfc::g_calls = 0;
    const char* const missing = "Data\\Menus\\selftest_missing.xml";
    Check(hook(missing, nullptr) == nullptr && hook(missing, nullptr) == nullptr && mfc::g_calls == 2,
          "MenuFileCache 1: a failed read is asked again (nothing kept)");
    mfc::g_fail = false;
    const std::string keep = mfc::g_text;
    mfc::g_text.clear();
    mfc::g_calls = 0;
    const char* const empty = "Data\\Menus\\selftest_empty.xml";
    mfc::Release(hook(empty, nullptr));
    mfc::Release(hook(empty, nullptr));
    Check(mfc::g_calls == 2, "MenuFileCache 1: an empty result is asked again (nothing kept)");
    mfc::g_text = keep;
    {   // a short first read (truncated text) differs from the next: not kept; two later identical reads are
        const char* const shortp = "Data\\Menus\\selftest_short.xml";
        MenuFileTestStats(&s0);
        mfc::g_text = keep.substr(0, 7);
        mfc::g_calls = 0;
        mfc::Release(hook(shortp, nullptr));
        mfc::g_text = keep;
        mfc::Release(hook(shortp, nullptr));
        MenuFileTestStats(&s1);
        const bool notKept = s1.stored == s0.stored;
        mfc::Release(hook(shortp, nullptr));
        void* const k = hook(shortp, nullptr);
        MenuFileTestStats(&s1);
        Check(notKept && mfc::g_calls == 3 && s1.stored == s0.stored + 1 && k &&
                  static_cast<mfc::Buf*>(k)->len == keep.size() && memcmp(static_cast<mfc::Buf*>(k)->data, keep.data(), keep.size()) == 0,
              "MenuFileCache 1: a short read differing from the next is not kept; the full text is kept after two identical reads");
        mfc::Release(k);
    }

    SetMenuFileCache(0);
    mfc::g_calls = 0;
    mfc::Release(hook(path, nullptr));
    Check(mfc::g_calls == 1, "MenuFileCache live 0: a kept file is read from disk again");


    Check(MenuFileTestPath("Data\\Menus\\Dialog\\dialog_menu.xml") && MenuFileTestPath("data/menus/a.xml") &&
              MenuFileTestPath("MENUS\\Prefabs\\b.xml") && MenuFileTestPath(".\\Data\\Menus\\c.xml") &&
              !MenuFileTestPath("Data\\Menu\\a.xml") && !MenuFileTestPath("Data\\MenusX\\a.xml") &&
              !MenuFileTestPath("Data\\Sound\\Menus\\a.xml") && !MenuFileTestPath("Menus") && !MenuFileTestPath(""),
          "MenuFileCache: the cached family (Data\\Menus\\..., Menus\\...; any case, either slash) and nothing else");
    Check(MenuFileTestPath("..\\..\\OblivionRemastered\\Content\\Dev\\ObvData\\Data\\Menus\\Dialog\\dialog_menu.xml") &&
              MenuFileTestPath("C:\\Games\\ObvData\\data\\menus\\prefabs\\b.xml") && MenuFileTestPath("\\Data\\Menus\\a.xml") &&
              !MenuFileTestPath("C:\\Games\\MyData\\Menus\\a.xml") && !MenuFileTestPath("C:\\Games\\Data\\MenusX\\a.xml") &&
              !MenuFileTestPath("x\\Menus\\a.xml"),
          "MenuFileCache: a Data\\Menus pair after a folder prefix (game check c1) is kept; other folders are not");

    // last: the size limit (a file past it is not kept; smaller later files still are, review S3-7)
    SetMenuFileCache(1);
    mfc::g_bigLen = 9u << 20;
    mfc::g_calls = 0;
    const char* const big = "Data\\Menus\\selftest_big.xml";
    mfc::Release(hook(big, nullptr));
    mfc::Release(hook(big, nullptr));
    mfc::g_bigLen = 0;
    MenuFileTestStats(&s0);
    Check(mfc::g_calls == 2 && s0.full, "MenuFileCache 1: a file past the size limit is not kept (limit reached)");
    const char* const afterLimit = "Data\\Menus\\selftest_after.xml";
    mfc::g_calls = 0;
    mfc::Release(hook(afterLimit, nullptr));
    mfc::Release(hook(afterLimit, nullptr));
    mfc::Release(hook(afterLimit, nullptr));
    Check(mfc::g_calls == 2, "MenuFileCache 1: after an oversized file a smaller one is still kept (two reads, then memory)");
    Check(mfc::g_newed.empty(), "MenuFileCache: every block handed out was freed by its user (no leak in the test)");
    SetMenuFileCache(0);
}
