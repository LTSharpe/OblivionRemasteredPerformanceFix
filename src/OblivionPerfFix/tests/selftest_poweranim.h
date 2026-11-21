// PowerAttackAnimKeep self-test (worker humananim 2026-11-15), included by selftest.cpp: the discovery on the mapped exe (the
// animation release's tail, the loading purge's power-attack rule, the group getter, one destroy function), refusals on fake
// images, the hook's answer for every kind of entry, the install (8 bytes), then the game's own release function runs in
// the mapped image through the patch with a fake loader and map: a power-attack entry whose count reaches 0 stays in the
// map with the key on; the paths that end in the game's destroy are checked through the hook's answer only (the destroy
// frees with the game's allocator, which the mapped image does not have).
#pragma once

namespace pak {
struct Entry {          // the old engine's animation entry, as far as the game's code reads it here
    const char* name;   // +0x00
    void* unused;       // +0x08
    uint8_t* sequence;  // +0x10
    uint32_t count;     // +0x18
};
struct Map {
    void** vtable;
    Entry* entry;
    int lookups, removes;
};
bool Lookup(Map* m, const char*, Entry** out)
{
    ++m->lookups;
    *out = m->entry;
    return m->entry != nullptr;
}
void Remove(Map* m, const char*) { ++m->removes; }
}  // namespace pak

static void PowerAnimRun(const GameModule& game)
{
    PowerAttackAnimKeepDiscovery d{};
    Check(PowerAttackAnimKeepDiscover(game, &d), "PowerAttackAnimKeep: discovery on the mapped exe");
    Check(d.releases == 1 && d.purges == 1, "PowerAttackAnimKeep: one release tail, one purge rule");
    if (game.timeDateStamp == 0xF19077A4u)
        Check(d.site - game.base == 0x67EC8E3 && d.purge - game.base == 0x67E74E6 && d.getter - game.base == 0x67E2580 &&
                  d.destroy - game.base == 0x67E51F0,
              "PowerAttackAnimKeep: release re-read exe+0x67EC8E3, purge rule exe+0x67E74E6, getter exe+0x67E2580, destroy exe+0x67E51F0 (build 19115871)");
    {
        // Fakes: the discovery accepts only one release tail and one purge rule whose two getter calls reach the 8-byte
        // getter and whose destroy call reaches the function the release's destroy call reaches.
        std::vector<uint8_t> img(0x2000, 0xCC);
        const Pattern rel = ParsePattern(
            "48 8B 44 24 30 F0 FF 48 18 40 84 F6 74 2D 48 8B 44 24 30 8B 48 18 85 C9 75 21 48 8B 4F 08 48 8B D3 48 8B 01 FF 50 28 "
            "48 8B 4C 24 30 48 85 C9 74 0A BA 01 00 00 00 E8 ?? ?? ?? ??");
        const Pattern purge = ParsePattern(
            "8B 41 18 85 C0 75 3F 48 8B 5D 28 48 8B 4B 10 48 85 C9 74 18 E8 ?? ?? ?? ?? 83 F8 16 7C 0E 48 8B 4B 10 E8 ?? ?? ?? ?? "
            "83 F8 1B 7C 1A 48 8B 4E 08 48 8B 01 48 8B D7 FF 50 28 BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ??");
        const Pattern getter = ParsePattern("0F B7 41 10 0F B6 C0 C3");
        auto put = [&](size_t at, const Pattern& p) {
            for (size_t i = 0; i < p.size(); ++i) img[at + i] = p.care[i] ? p.bytes[i] : 0;
        };
        auto call = [&](size_t e8, size_t fn) {
            const int32_t r = static_cast<int32_t>(fn) - static_cast<int32_t>(e8 + 5);
            memcpy(&img[e8 + 1], &r, 4);
        };
        auto found = [&]() {
            GameModule fake{};
            fake.base = img.data();
            fake.text = {img.data(), img.data() + img.size()};
            PowerAttackAnimKeepDiscovery f{};
            return PowerAttackAnimKeepDiscover(fake, &f) ? f.site : nullptr;
        };
        const size_t R = 0x100, P = 0x400, G = 0x800, D = 0x900;
        put(R, rel), put(P, purge), put(G, getter);
        call(R + 54, D), call(P + 20, G), call(P + 34, G), call(P + 65, D);
        Check(found() == img.data() + R + 14, "PowerAttackAnimKeep fake: release tail, purge rule, getter, one destroy: found");
        call(P + 34, G + 0x40);
        Check(found() == nullptr, "PowerAttackAnimKeep fake: the purge's two group calls differ: refused");
        call(P + 34, G);
        img[G + 3] = 0x18;
        Check(found() == nullptr, "PowerAttackAnimKeep fake: the getter reads another field: refused");
        img[G + 3] = 0x10;
        call(P + 65, D + 0x40);
        Check(found() == nullptr, "PowerAttackAnimKeep fake: the purge destroys with another function: refused");
        call(P + 65, D);
        call(R + 54, 0x3000), call(P + 65, 0x3000);
        Check(found() == nullptr, "PowerAttackAnimKeep fake: the destroy function outside the code: refused");
        call(R + 54, D), call(P + 65, D);
        img[P + 27] = 0x15;
        Check(found() == nullptr, "PowerAttackAnimKeep fake: the purge keeps another group range: refused");
        img[P + 27] = 0x16;
        img[R + 21] = 0x1C;
        Check(found() == nullptr, "PowerAttackAnimKeep fake: the release reads the count elsewhere: refused");
        img[R + 21] = 0x18;
        Check(found() == img.data() + R + 14, "PowerAttackAnimKeep fake: found again once restored");
        put(0x1000, rel), call(0x1000 + 54, D);
        Check(found() == nullptr, "PowerAttackAnimKeep fake: two release tails: refused");
        for (size_t i = 0; i < rel.size(); ++i) img[0x1000 + i] = 0xCC;
        put(0x1400, purge), call(0x1400 + 20, G), call(0x1400 + 34, G), call(0x1400 + 65, D);
        Check(found() == nullptr, "PowerAttackAnimKeep fake: two purge rules: refused");
    }
    if (!d.site) return;

    // The hook's answer (what the game's "mov ecx, [entry+18h]" gets).
    alignas(8) uint8_t seq[0x20] = {};
    pak::Entry e{"characters\\_male\\selftestattackpower.kf", nullptr, seq, 0};
    const auto ask = [&](uint8_t group, uint32_t count) {
        seq[0x10] = group;
        seq[0x11] = 0x7F;  // the word's high byte is not part of the group
        e.count = count;
        return PowerAttackAnimKeepTestCount(reinterpret_cast<const uint8_t*>(&e));
    };
    SetPowerAttackAnimKeep(0);
    uint64_t k0 = PowerAttackAnimKeepKept();
    Check(ask(0x16, 0) == 0 && ask(0x1A, 0) == 0 && ask(0x03, 0) == 0 && ask(0x16, 3) == 3,
          "PowerAttackAnimKeep 0: the hook returns the entry's count, whatever the group");
    Check(PowerAttackAnimKeepKept() == k0, "PowerAttackAnimKeep 0: nothing counted");
    SetPowerAttackAnimKeep(1);
    Check(ask(0x16, 0) == 1 && ask(0x17, 0) == 1 && ask(0x18, 0) == 1 && ask(0x19, 0) == 1 && ask(0x1A, 0) == 1,
          "PowerAttackAnimKeep 1: a power-attack entry (groups 0x16-0x1A) at count 0 reads as in use");
    Check(PowerAttackAnimKeepKept() == k0 + 5, "PowerAttackAnimKeep 1: five entries counted");
    Check(ask(0x15, 0) == 0 && ask(0x1B, 0) == 0 && ask(0x00, 0) == 0 && ask(0xFF, 0) == 0,
          "PowerAttackAnimKeep 1: the groups next to the range and any other group at count 0 read 0 (removed and destroyed as before)");
    Check(ask(0x16, 1) == 1 && ask(0x16, 7) == 7 && ask(0x03, 2) == 2 && ask(0x16, 0xFFFFFFFFu) == 0xFFFFFFFFu,
          "PowerAttackAnimKeep 1: a count other than 0 is returned as it is");
    e.sequence = nullptr;
    e.count = 0;
    Check(PowerAttackAnimKeepTestCount(reinterpret_cast<const uint8_t*>(&e)) == 0,
          "PowerAttackAnimKeep 1: an entry without a sequence at count 0 reads 0");
    e.sequence = seq;
    Check(PowerAttackAnimKeepKept() == k0 + 5, "PowerAttackAnimKeep 1: only the five power-attack entries at count 0 were counted");

    // Install, then the game's own release function through the patch.
    uint8_t before[8];
    memcpy(before, d.site, 8);
    Check(PowerAttackAnimKeepTestInstall(), "PowerAttackAnimKeep: installs on the mapped exe");
    Check(d.site[0] == 0xE8 && d.site[5] == 0x8B && d.site[6] == 0xC8 && d.site[7] == 0x90 && memcmp(d.site + 8, "\x85\xC9\x75\x21", 4) == 0 &&
              before[0] == 0x48 && before[5] == 0x8B,
          "PowerAttackAnimKeep: the count's re-read is call stub; mov ecx, eax; nop, the test and branch after it untouched");
    uint8_t* const stub = d.site + 5 + *reinterpret_cast<int32_t*>(d.site + 1);
    Check(stub[0] == 0x48 && stub[1] == 0x89 && stub[2] == 0xC1 && stub[3] == 0xFF && stub[4] == 0x25,
          "PowerAttackAnimKeep: the stub is mov rcx, rax; jmp [hook]");
    PowerAttackAnimKeepDiscovery again{};
    Check(!PowerAttackAnimKeepDiscover(game, &again) && again.releases == 0, "PowerAttackAnimKeep: a patched release is not found again");
    uint8_t* const fn = d.site - 0x43;  // the release function's entry (exe+0x67EC8A0)
    const Pattern entry = ParsePattern("48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F9");
    if (Installed(game.text.Contains(fn) && MatchesAt(fn, entry), "PowerAttackAnimKeep: the release function's entry 0x43 bytes before the patch")) {
        using Release = void (*)(void* loader, const char* name, uint8_t remove);
        const auto release = reinterpret_cast<Release>(fn);
        void* vtable[6] = {nullptr, nullptr, reinterpret_cast<void*>(&pak::Lookup), nullptr, nullptr, reinterpret_cast<void*>(&pak::Remove)};
        pak::Map map{vtable, &e, 0, 0};
        void* loader[2] = {nullptr, &map};
        SetPowerAttackAnimKeep(1);
        k0 = PowerAttackAnimKeepKept();
        seq[0x10] = 0x17;
        e.count = 1;
        release(loader, e.name, 1);
        Check(map.lookups == 1 && e.count == 0 && map.removes == 0 && PowerAttackAnimKeepKept() == k0 + 1,
              "PowerAttackAnimKeep 1, the game's release: a power-attack entry's last holder lets go: count 0, still in the map, counted");
        e.count = 1;  // a later request counts it up (the game's lock inc); its release leaves it again
        release(loader, e.name, 1);
        Check(e.count == 0 && map.removes == 0 && PowerAttackAnimKeepKept() == k0 + 2,
              "PowerAttackAnimKeep 1, the game's release: used and released again: still in the map");
        seq[0x10] = 0x03;
        e.count = 2;
        release(loader, e.name, 1);
        Check(e.count == 1 && map.removes == 0 && PowerAttackAnimKeepKept() == k0 + 2,
              "PowerAttackAnimKeep 1, the game's release: another group with a holder left: counted down only");
        seq[0x10] = 0x17;
        e.count = 3;
        release(loader, e.name, 1);
        Check(e.count == 2 && map.removes == 0 && PowerAttackAnimKeepKept() == k0 + 2,
              "PowerAttackAnimKeep 1, the game's release: a power-attack entry with holders left: counted down only");
        e.count = 1;
        release(loader, e.name, 0);
        Check(e.count == 0 && map.removes == 0 && PowerAttackAnimKeepKept() == k0 + 2,
              "PowerAttackAnimKeep 1, the game's release without removal: counted down, the hook is not reached");
        SetPowerAttackAnimKeep(0);
        e.count = 2;
        release(loader, e.name, 1);
        Check(e.count == 1 && map.removes == 0, "PowerAttackAnimKeep 0, the game's release: counted down as before");
        map.entry = nullptr;
        release(loader, e.name, 1);
        Check(e.count == 1 && map.lookups == 7, "PowerAttackAnimKeep: a name that is not in the map: nothing touched");
    }
    SetPowerAttackAnimKeep(0);
}
