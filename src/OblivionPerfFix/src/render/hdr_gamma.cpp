// HdrGamma22 (TRADE-OFF, on by default; nothing changes in SDR): the HDR picture's shadows as an SDR monitor
// shows the game (gamma 2.2) instead of the lifted shadows of the game's HDR curve.
//
// Measured (docs/research.md, HdrGamma22): the game's HDR output reproduces its SDR picture decoded with the sRGB curve;
// an SDR monitor decodes with gamma 2.2, so next to SDR the HDR darks sit 1.7-2.7x higher relative to the mid-tones.
// The HDR curve is an ACES-style tone scale fully described by a 20-float block the game computes on the CPU when an
// HDR setting changes (exe+0x26C03D0) and every pass that uses it copies through one getter (exe+0x26C0790): min / mid
// / max points and two quadratic B-splines of 5 coefficients each in log10.
//
// The fix: when the block holds the game's values, the monitor thread fits new spline coefficients (and the split
// points min.x / mid.x, which the evaluation only uses as segment bounds) so the curve's output becomes g(stock output):
// below W the value re-decoded as gamma 2.2 instead of sRGB (per channel, as the tone scale works), above W unchanged,
// never below the curve's black floor. W = HdrGamma22White / 10 x the stock output at input 0.18 (the Mid-Tone; 9.5 =
// the measured SDR-white equivalent of the HDR curve). Least squares in log10 with the floor, the peak and the join at the
// mid point kept (value and slope); a grid search over the split points; rejected unless monotonic and within 10 % of
// the target from 0.1 % of W up (measured offline: 5-6 % worst, deep shadows; under 2.5 % from 0.5 % of W). The game
// thread writes the result into the block (it is read by the render passes; one frame may see a mixed block, as with the
// game's own writes) and puts the game's values back when switched off. The game's exposure shift is solved before our
// change and stays the game's.
//
// What changes: the HDR picture's darks and mid-tones (below W), on purpose: a trade-off (CLAUDE.md), default 0.
// Highlights, the peak, the black floor and the UI's own levels stay the game's. Frame generation sees the corrected
// scene (the correction is in the tone curve, before the UI and before DLSS / FSR frame generation). Live (ini).
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace opf {
namespace {

// the getter (exe+0x26C0790): movaps xmm0,[block] / mov rax,rcx / movaps xmm1,[block+10h] / movaps [rcx],xmm0 /
// movaps xmm0,[block+20h] / movaps [rcx+10h],xmm1 / movaps xmm1,[block+30h] / movaps [rcx+20h],xmm0 /
// movaps xmm0,[block+40h] / movaps [rcx+30h],xmm1 / movaps [rcx+40h],xmm0 / ret
constexpr char kGetter[] =
    "0F 28 05 ?? ?? ?? ?? 48 8B C1 0F 28 0D ?? ?? ?? ?? 0F 29 01 0F 28 05 ?? ?? ?? ?? 0F 29 49 10 0F 28 0D ?? ?? ?? ?? "
    "0F 29 41 20 0F 28 05 ?? ?? ?? ?? 0F 29 49 30 0F 29 41 40 C3";
constexpr size_t kGetterLoads[5] = {0, 10, 20, 31, 42};  // the five movaps loads (7 bytes, disp at +3)
// the tone-scale parameter build (exe+0x26CA6CC): mov dword [rbx+10h],4.8f (mid.y) / mov dword [rbx+14h],1.55f
// (mid.slope) / movups xmm0,[rbp-51h] / movss [rbx+18h],xmm1
constexpr char kBuilder[] = "C7 43 10 9A 99 99 40 C7 43 14 66 66 C6 3F 0F 10 45 ?? F3 0F 11 4B 18";

// the block (floats)
enum : int { kMinX = 0, kMinY = 1, kMaxX = 2, kMaxY = 3, kMidX = 4, kMidY = 5, kMidSlope = 6, kLow0 = 8, kHigh0 = 12, kLow4 = 16, kHigh4 = 17, kN = 20 };

struct Curve {
    double minx, miny, midx, maxx, maxy;
    double low[5], high[5];
};

Curve FromBlock(const float* b)
{
    Curve c{b[kMinX], b[kMinY], b[kMidX], b[kMaxX], b[kMaxY], {}, {}};
    for (int i = 0; i < 4; ++i) {
        c.low[i] = b[kLow0 + i];
        c.high[i] = b[kHigh0 + i];
    }
    c.low[4] = b[kLow4];
    c.high[4] = b[kHigh4];
    return c;
}

bool Valid(const float* b)
{
    return b[kMidY] == 4.8f && b[kMidSlope] == 1.55f && b[kMinX] > 0 && b[kMinX] < b[kMidX] && b[kMidX] < b[kMaxX] && b[kMinY] > 0 &&
           b[kMaxY] > b[kMidY] && std::isfinite(b[kMaxX]);
}

// the quadratic B-spline's weights for the three coefficients of a segment at t
void Basis(double t, double w[3])
{
    w[0] = 0.5 * t * t - t + 0.5;
    w[1] = -t * t + t + 0.5;
    w[2] = 0.5 * t * t;
}

// segment of log10 input lx: false below min / above max; side 0 = low, 1 = high
bool Segment(const Curve& c, double lx, int* side, int* j, double* t)
{
    const double lmin = std::log10(c.minx), lmid = std::log10(c.midx), lmax = std::log10(c.maxx);
    if (lx <= lmin || lx >= lmax) return false;
    const double kc = lx < lmid ? 3 * (lx - lmin) / (lmid - lmin) : 3 * (lx - lmid) / (lmax - lmid);
    *side = lx < lmid ? 0 : 1;
    *j = kc >= 2 ? 2 : static_cast<int>(kc);
    *t = kc - *j;
    return true;
}

double Eval(const Curve& c, double x)
{
    const double lx = std::log10(x > 1e-10 ? x : 1e-10);
    int side, j;
    double t, w[3];
    if (!Segment(c, lx, &side, &j, &t)) return lx <= std::log10(c.minx) ? c.miny : c.maxy;
    Basis(t, w);
    const double* k = side ? c.high : c.low;
    return std::pow(10.0, w[0] * k[j] + w[1] * k[j + 1] + w[2] * k[j + 2]);
}

double SrgbEncode(double y) { return y <= 0.0031308 ? 12.92 * y : 1.055 * std::pow(y, 1 / 2.4) - 0.055; }

double Target(double y, double white, double floor)
{
    if (y >= white) return y;
    const double v = white * std::pow(SrgbEncode(y / white), 2.2);
    return v > floor ? v : floor;
}

// Gaussian elimination with partial pivoting (n <= 16); false if singular
bool Solve(double a[16][17], int n, double* x)
{
    for (int col = 0; col < n; ++col) {
        int p = col;
        for (int r = col + 1; r < n; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[p][col])) p = r;
        if (std::fabs(a[p][col]) < 1e-14) return false;
        if (p != col)
            for (int k = 0; k <= n; ++k) std::swap(a[p][k], a[col][k]);
        for (int r = 0; r < n; ++r) {
            if (r == col || a[r][col] == 0) continue;
            const double f = a[r][col] / a[col][col];
            for (int k = col; k <= n; ++k) a[r][k] -= f * a[col][k];
        }
    }
    for (int i = 0; i < n; ++i) x[i] = a[i][n] / a[i][i];
    return true;
}

// least squares over `samples` log-spaced inputs of the new split s, unknowns low[0..4] + high[0..4]; s's coefficients set
bool Fit(const Curve& stock, double white, Curve& s, int samples)
{
    const double lmin = std::log10(s.minx), lmid = std::log10(s.midx), lmax = std::log10(s.maxx);
    double a[16][17] = {};
    for (int i = 0; i < samples; ++i) {
        const double lx = lmin + (lmax - lmin) * (i + 0.5) / samples;
        int side, j;
        double t, w[3];
        if (!Segment(s, lx, &side, &j, &t)) continue;
        Basis(t, w);
        const double y0 = Eval(stock, std::pow(10.0, lx));
        const double target = std::log10(Target(y0, white, stock.miny));
        const double weight = y0 < 0.02 ? 0.1 : 1.0;  // below 0.02 nits matters little (black)
        const int off = side * 5 + j;
        for (int p = 0; p < 3; ++p) {
            a[off + p][16] += 2 * weight * w[p] * target;
            for (int q = 0; q < 3; ++q) a[off + p][off + q] += 2 * weight * w[p] * w[q];
        }
    }
    // constraints (rows 10..15): floor value, flat at the floor, mid join value, mid join slope, peak value, flat at the peak
    const double dl = 3 / (lmid - lmin), dh = 3 / (lmax - lmid);
    const double cons[6][10] = {
        {0.5, 0.5, 0, 0, 0, 0, 0, 0, 0, 0},   {1, -1, 0, 0, 0, 0, 0, 0, 0, 0},     {0, 0, 0, 0.5, 0.5, -0.5, -0.5, 0, 0, 0},
        {0, 0, 0, -dl, dl, dh, -dh, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0.5}, {0, 0, 0, 0, 0, 0, 0, 0, 1, -1}};
    const double val[6] = {std::log10(stock.miny), 0, 0, 0, std::log10(stock.maxy), 0};
    for (int c = 0; c < 6; ++c) {
        for (int p = 0; p < 10; ++p) {
            a[p][10 + c] = cons[c][p];
            a[10 + c][p] = cons[c][p];
        }
        a[10 + c][16] = val[c];
    }
    double x[16];
    if (!Solve(a, 16, x)) return false;
    for (int i = 0; i < 5; ++i) {
        s.low[i] = x[i];
        s.high[i] = x[5 + i];
    }
    return true;
}

// worst |fit / target - 1| from 0.1 % of W up; +1 if the curve is not monotonic
double FitError(const Curve& stock, const Curve& s, double white)
{
    const double l0 = std::log10(stock.minx), l1 = std::log10(stock.maxx);
    double worst = 0, prev = 0;
    for (int i = 0; i < 300; ++i) {
        const double x = std::pow(10.0, l0 + (l1 - l0) * i / 299);
        const double y0 = Eval(stock, x), f = Eval(s, x);
        if (y0 >= 0.001 * white) worst = std::fmax(worst, std::fabs(f / Target(y0, white, stock.miny) - 1));
        if (f < prev * (1 - 1e-6)) worst += 1;
        prev = f;
    }
    return worst;
}

struct FitResult {
    bool ok = false;
    double white = 0, mid = 0, worst = 0;
    float block[kN] = {};
};

FitResult Compute(const float* stockBlock, double factor)
{
    FitResult r;
    const Curve stock = FromBlock(stockBlock);
    r.mid = Eval(stock, 0.18);
    r.white = factor * r.mid;
    if (!(r.white > stock.miny && r.white < stock.maxy)) return r;
    const double l0 = std::log10(stock.minx), l1 = std::log10(stock.maxx);
    double best = 1e30;
    Curve bestCurve = stock;
    for (int a = 0; a <= 40; ++a) {
        for (int b = 1; b <= 40; ++b) {
            const double lmn = l0 + (l1 - l0) * a / 60;
            const double lmd = lmn + (l1 - lmn) * b / 42;
            Curve s = stock;
            s.minx = std::pow(10.0, lmn);
            s.midx = std::pow(10.0, lmd);
            if (!Fit(stock, r.white, s, 200)) continue;
            const double e = FitError(stock, s, r.white);
            if (e < best) {
                best = e;
                bestCurve = s;
            }
        }
    }
    if (best >= 1e30 || !Fit(stock, r.white, bestCurve, 400)) return r;
    r.worst = FitError(stock, bestCurve, r.white);
    if (r.worst > 0.10) return r;
    memcpy(r.block, stockBlock, sizeof r.block);
    r.block[kMinX] = static_cast<float>(bestCurve.minx);
    r.block[kMidX] = static_cast<float>(bestCurve.midx);
    for (int i = 0; i < 4; ++i) {
        r.block[kLow0 + i] = static_cast<float>(bestCurve.low[i]);
        r.block[kHigh0 + i] = static_cast<float>(bestCurve.high[i]);
    }
    r.block[kLow4] = static_cast<float>(bestCurve.low[4]);
    r.block[kHigh4] = static_cast<float>(bestCurve.high[4]);
    r.ok = true;
    return r;
}

float* g_block = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<int> g_factorX10{95};
// game thread
float g_stock[kN] = {}, g_written[kN] = {};
bool g_haveWritten = false;
int g_writtenFactor = 0;
float g_rejectedStock[kN] = {};
int g_rejectedFactor = -1;
long g_applied = 0, g_restored = 0, g_rejected = 0;
double g_lastWorst = 0;
// shared with the monitor thread
std::mutex g_lock;
bool g_pending = false;
float g_reqStock[kN] = {};
int g_reqFactor = 0;
bool g_haveResult = false;
float g_resStock[kN] = {};
int g_resFactor = 0;
FitResult g_result;

bool Fail(const char* why)
{
    Log("[hdrgamma] %s: HdrGamma22 not installed", why);
    return false;
}

}  // namespace

bool FindHdrGamma(HdrGammaInfoOut* out)
{
    const GameModule& game = Game();
    if (out) *out = HdrGammaInfoOut{};
    const auto getters = FindAll(game.text, ParsePattern(kGetter));
    const auto builders = FindAll(game.text, ParsePattern(kBuilder));
    if (getters.size() != 1) return false;
    uint8_t* const block = RipTarget(getters[0] + kGetterLoads[0], 3, 7);
    for (int i = 1; i < 5; ++i)
        if (RipTarget(getters[0] + kGetterLoads[i], 3, 7) != block + 0x10 * i) return false;
    if (!game.data.Contains(block) || !game.data.Contains(block + 0x4F) || (reinterpret_cast<uintptr_t>(block) & 15)) return false;
    if (out) {
        out->getter = getters[0];
        out->block = block;
        out->builder = builders.size() == 1 ? builders[0] : nullptr;
    }
    return builders.size() == 1;
}

bool InstallHdrGamma()
{
    if (g_installed) return true;
    HdrGammaInfoOut i{};
    if (!FindHdrGamma(&i)) return Fail(i.getter ? "the tone-scale parameter build (4.8 / 1.55) not found" : "the HDR tone curve's getter not found");
    g_block = reinterpret_cast<float*>(i.block);
    g_installed = true;
    Log("[hdrgamma] HdrGamma22: the HDR tone curve's block at exe+0x%llX (getter exe+0x%llX, build exe+0x%llX); nothing written "
        "while off",
        static_cast<unsigned long long>(i.block - Game().base), static_cast<unsigned long long>(i.getter - Game().base),
        static_cast<unsigned long long>(i.builder - Game().base));
    return true;
}

void SetHdrGamma22(bool on, int whiteX10)
{
    g_on.store(on && g_installed);
    g_factorX10.store(whiteX10 < 20 ? 20 : whiteX10 > 400 ? 400 : whiteX10);
}

void HdrGammaFrameEnd()
{
    if (!g_installed) return;
    float b[kN];
    memcpy(b, g_block, sizeof b);
    if (!g_haveWritten || memcmp(b, g_written, sizeof b) != 0) {  // the block holds the game's values
        if (!Valid(b)) return;
        memcpy(g_stock, b, sizeof b);
        g_haveWritten = false;
    }
    const bool on = g_on.load();
    const int factor = g_factorX10.load();
    if (!on) {
        if (g_haveWritten) {
            memcpy(g_block, g_stock, sizeof g_stock);
            g_haveWritten = false;
            ++g_restored;
            Log("[hdrgamma] off: the game's HDR curve put back");
        }
        return;
    }
    if (g_haveWritten && g_writtenFactor == factor) return;
    if (g_rejectedFactor == factor && memcmp(g_rejectedStock, g_stock, sizeof g_stock) == 0) return;
    FitResult r;
    bool ready = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_haveResult && g_resFactor == factor && memcmp(g_resStock, g_stock, sizeof g_stock) == 0) {
            r = g_result;
            ready = true;
        } else if (!g_pending || g_reqFactor != factor || memcmp(g_reqStock, g_stock, sizeof g_stock) != 0) {
            memcpy(g_reqStock, g_stock, sizeof g_stock);
            g_reqFactor = factor;
            g_pending = true;
        }
    }
    if (!ready) return;
    if (!r.ok) {
        memcpy(g_rejectedStock, g_stock, sizeof g_stock);
        g_rejectedFactor = factor;
        ++g_rejected;
        Log("[hdrgamma] the fit for Mid-Tone %.1f (W %.0f nits) is %s: the game's HDR curve kept", r.mid, r.white,
            r.white > 0 ? "not within 10 % or not monotonic" : "out of range");
        return;
    }
    memcpy(g_block, r.block, sizeof r.block);
    memcpy(g_written, r.block, sizeof r.block);
    g_haveWritten = true;
    g_writtenFactor = factor;
    g_lastWorst = r.worst;
    ++g_applied;
    Log("[hdrgamma] applied: Mid-Tone %.1f nits, W %.0f nits (x%.1f), split %.4g / %.4g (game's %.4g / %.4g), worst fit error %.1f %%",
        r.mid, r.white, factor / 10.0, r.block[kMinX], r.block[kMidX], g_stock[kMinX], g_stock[kMidX], 100 * r.worst);
}

void HdrGammaMonitorTick()
{
    if (!g_installed) return;
    float stock[kN];
    int factor;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_pending) return;
        memcpy(stock, g_reqStock, sizeof stock);
        factor = g_reqFactor;
    }
    const FitResult r = Compute(stock, factor / 10.0);
    std::lock_guard<std::mutex> l(g_lock);
    g_result = r;
    memcpy(g_resStock, stock, sizeof stock);
    g_resFactor = factor;
    g_haveResult = true;
    if (g_reqFactor == factor && memcmp(g_reqStock, stock, sizeof stock) == 0) g_pending = false;
}

bool HdrGammaSelfFit(const float* block, double factor, double* worst, float* out)
{
    if (!Valid(block)) return false;
    const FitResult r = Compute(block, factor);
    if (worst) *worst = r.worst;
    if (out && r.ok) memcpy(out, r.block, sizeof r.block);
    return r.ok;
}

double HdrGammaEval(const float* block, double x) { return Eval(FromBlock(block), x); }

void LogHdrGammaStats()
{
    if (g_installed && (g_applied || g_restored || g_rejected))
        Log("[hdrgamma] curve applied %ld time(s), put back %ld, fits rejected %ld; last worst fit error %.1f %%", g_applied, g_restored,
            g_rejected, 100 * g_lastWorst);
}

}  // namespace opf
