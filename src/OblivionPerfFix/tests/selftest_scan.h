// Pattern search (FindAll): the fast search gives exactly the results of the original search (FindAllReference).
//  - every FindAll call the self-test makes runs both (ScanVerify on): no call may differ;
//  - random patterns cut from the game's code, with random wildcards, over random sections (also tiny ones, ones at the
//    end of .text, ones shorter than the pattern) and over all of .text, .rdata and .data;
//  - the report at the end: how long the self-test's searches take with each (the start-up cost of the fixes' searches).
namespace scantest {

struct Rec {
    Section s;
    Pattern p;
    size_t hits;
    uint64_t ticks;
};
std::vector<Rec> g_recs;

void Observe(const Section& s, const Pattern& p, const std::vector<uint8_t*>& hits, uint64_t ticks)
{
    g_recs.push_back({s, p, hits.size(), ticks});
}

void Begin()
{
    SetScanMode(1);
    SetScanObserver(Observe);
}

struct Rng {
    uint64_t x = 0x9E3779B97F4A7C15ull;
    uint64_t Next()
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    }
    size_t Below(size_t n) { return n ? static_cast<size_t>(Next() % n) : 0; }
};

Pattern RandomPattern(Rng& r, const uint8_t* from, size_t len, unsigned wildPercent)
{
    Pattern p;
    for (size_t i = 0; i < len; ++i) {
        const bool care = i == 0 || r.Below(100) >= wildPercent;
        p.bytes.push_back(care ? from[i] : 0);
        p.care.push_back(care ? 1 : 0);
    }
    return p;
}

void RandomTests(const GameModule& game)
{
    SetScanObserver(nullptr);
    Rng r;
    const size_t textSize = static_cast<size_t>(game.text.end - game.text.begin);
    int bad = 0, total = 0;
    size_t hitsSeen = 0;
    auto compare = [&](const Section& s, const Pattern& p) {
        const std::vector<uint8_t*> fast = FindAll(s, p), ref = FindAllReference(s, p);
        ++total;
        hitsSeen += ref.size();
        if (fast != ref && ++bad <= 5)
            printf("      differs: %zu-byte pattern over %zu bytes: fast %zu, reference %zu\n", p.size(),
                   static_cast<size_t>(s.end - s.begin), fast.size(), ref.size());
    };
    // Small sections anywhere in .text (also at its very end), patterns cut from inside, near or outside them.
    for (int i = 0; i < 3000; ++i) {
        const size_t size = r.Below(4) == 0 ? r.Below(64) : 1 + r.Below(256 * 1024);
        const size_t off = r.Below(8) == 0 ? textSize - size : r.Below(textSize - size);
        const Section s{game.text.begin + off, game.text.begin + off + size};
        const size_t len = 1 + r.Below(r.Below(3) == 0 ? 6 : 40);
        const uint8_t* src = r.Below(3) == 0 ? game.text.begin + r.Below(textSize - len) : s.begin + r.Below(size > len ? size - len : 1);
        if (src + len > game.text.end) src = game.text.end - len;
        compare(s, RandomPattern(r, src, len, static_cast<unsigned>(r.Below(60))));
    }
    // Patterns made only of the commonest code bytes (many candidates per 16 starts).
    const uint8_t common[] = {0x48, 0x00, 0x8B, 0x0F, 0xCC, 0x24, 0x89};
    for (int i = 0; i < 300; ++i) {
        const size_t size = 1 + r.Below(512 * 1024);
        const size_t off = r.Below(textSize - size);
        Pattern p;
        const size_t len = 1 + r.Below(6);
        for (size_t k = 0; k < len; ++k) {
            const bool care = k == 0 || r.Below(3) != 0;
            p.bytes.push_back(care ? common[r.Below(sizeof common)] : 0);
            p.care.push_back(care ? 1 : 0);
        }
        compare(Section{game.text.begin + off, game.text.begin + off + size}, p);
    }
    // Whole sections, as the fixes search them.
    const Section whole[] = {game.text, game.rdata, game.data};
    for (int i = 0; i < 24; ++i) {
        const Section& s = whole[i % 3];
        const size_t size = static_cast<size_t>(s.end - s.begin);
        const size_t len = 4 + r.Below(24);
        compare(s, RandomPattern(r, s.begin + r.Below(size - len), len, 25));
    }
    // A first-byte wildcard and an empty pattern find nothing, as before.
    compare(game.text, ParsePattern("?? 8B 05"));
    compare(game.text, Pattern{});
    printf("      %d random searches, %zu matches in all\n", total, hitsSeen);
    Check(bad == 0, "pattern search: random patterns and sections give the reference search's matches, in order");
    SetScanObserver(Observe);
}

void Report()
{
    SetScanObserver(nullptr);
    const ScanStats st = GetScanStats();
    Check(st.mismatches == 0, "pattern search: every search of the self-test gave the reference search's matches");
    SetScanMode(0);  // time each search alone
    // Time the self-test's own searches (the fixes' Find functions) both ways, on the image as it is now.
    LARGE_INTEGER f, a, b, c;
    QueryPerformanceFrequency(&f);
    double fastMs = 0, refMs = 0, fastFull = 0, refFull = 0;
    int fullScans = 0, differ = 0;
    struct Slow {
        double ref, fast;
        size_t size, len, hits;
    };
    std::vector<Slow> slow;
    for (const Rec& rec : g_recs) {
        QueryPerformanceCounter(&a);
        const std::vector<uint8_t*> x = FindAll(rec.s, rec.p);
        QueryPerformanceCounter(&b);
        const std::vector<uint8_t*> y = FindAllReference(rec.s, rec.p);
        QueryPerformanceCounter(&c);
        if (x != y) ++differ;
        const double fm = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart, rm = (c.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart;
        fastMs += fm, refMs += rm;
        const size_t size = static_cast<size_t>(rec.s.end - rec.s.begin);
        if (size >= 16 * 1024 * 1024) ++fullScans, fastFull += fm, refFull += rm;
        slow.push_back({rm, fm, size, rec.p.size(), y.size()});
    }
    Check(differ == 0, "pattern search: the self-test's searches repeated on the patched image agree too");
    printf("      self-test searches: %zu (%d over 16+ MB): reference %.0f ms (%.0f in the big ones), fast %.0f ms (%.0f)\n",
           g_recs.size(), fullScans, refMs, refFull, fastMs, fastFull);
    std::sort(slow.begin(), slow.end(), [](const Slow& l, const Slow& r) { return l.ref > r.ref; });
    for (size_t i = 0; i < slow.size() && i < 8; ++i)
        printf("        reference %6.1f ms, fast %5.1f ms: %zu-byte pattern over %.1f MB, %zu match(es)\n", slow[i].ref,
               slow[i].fast, slow[i].len, slow[i].size / 1e6, slow[i].hits);
}

}  // namespace scantest
