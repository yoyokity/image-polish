#include "dehalo.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

inline int clamp255(int v) { return std::max(0, std::min(255, v)); }

// round half to even, matching std.Expr's float rounding
inline int rintE(double v) { return int(std::rint(v)); }

const double kPi = std::acos(-1.0);

// ---- morphology (std.Maximum / std.Minimum semantics: OOB neighbours are
// skipped, the centre pixel always participates) ---------------------------
// neighbour order: tl, t, tr, l, r, bl, b, br
const int DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
const int DY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

static void morpho(const u8 *src, int w, int h, const int mask[8], bool isMax, u8 *dst)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int v = src[y * w + x];
            for (int k = 0; k < 8; k++) {
                if (!mask[k])
                    continue;
                const int yy = y + DY[k], xx = x + DX[k];
                if (yy < 0 || yy >= h || xx < 0 || xx >= w)
                    continue;
                const int nv = src[yy * w + xx];
                v = isMax ? std::max(v, nv) : std::min(v, nv);
            }
            dst[y * w + x] = u8(v);
        }
    }
}

// mt_expand_multi / mt_inpand_multi: mode 0 rectangle, 1 ellipse, 2 losange
static void multiRec(const u8 *src, int w, int h, int sw, int sh, int mode, bool isMax, u8 *dst)
{
    if (sw <= 0 && sh <= 0) {
        std::memcpy(dst, src, std::size_t(w) * h);
        return;
    }
    int mask[8];
    if (sw > 0 && sh > 0) {
        const bool losange = (mode == 2) || (mode == 1 && (sw % 3) != 1);
        for (int k = 0; k < 8; k++)
            mask[k] = losange ? (k == 1 || k == 3 || k == 4 || k == 6) : 1;
    } else if (sw > 0) {
        // horizontal only: l, r
        for (int k = 0; k < 8; k++)
            mask[k] = (k == 3 || k == 4);
    } else {
        // vertical only: t, b
        for (int k = 0; k < 8; k++)
            mask[k] = (k == 1 || k == 6);
    }
    std::vector<u8> tmp(std::size_t(w) * h);
    morpho(src, w, h, mask, isMax, tmp.data());
    multiRec(tmp.data(), w, h, sw - 1, sh - 1, mode, isMax, dst);
}

// ---- std.Convolution (border = edge replication; the result is divided by
// the matrix sum, or not divided when the sum is zero) ----------------------
static void convolve(const u8 *src, int w, int h, const int m[9], u8 *dst)
{
    long scale = 0;
    for (int k = 0; k < 9; k++)
        scale += m[k];
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            long s = 0;
            for (int ky = -1; ky <= 1; ky++) {
                for (int kx = -1; kx <= 1; kx++) {
                    const int yy = std::max(0, std::min(h - 1, y + ky));
                    const int xx = std::max(0, std::min(w - 1, x + kx));
                    s += long(m[(ky + 1) * 3 + (kx + 1)]) * src[yy * w + xx];
                }
            }
            const double v = (scale != 0) ? double(s) / scale : double(s);
            dst[y * w + x] = u8(clamp255(rintE(v)));
        }
    }
}

static void box3(const u8 *src, int w, int h, u8 *dst)
{
    static const int m[9] = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    convolve(src, w, h, m, dst);
}

// ---- Prewitt edge strength: 4 convolutions, elementwise max --------------
static void prewitt(const u8 *src, int w, int h, u8 *dst)
{
    static const int m1[9] = { 1, 1, 0, 1, 0, -1, 0, -1, -1 };
    static const int m2[9] = { 1, 1, 1, 0, 0, 0, -1, -1, -1 };
    static const int m3[9] = { 1, 0, -1, 1, 0, -1, 1, 0, -1 };
    static const int m4[9] = { 0, -1, -1, 1, 0, -1, 1, 1, 0 };
    std::vector<u8> c1(std::size_t(w) * h), c2(std::size_t(w) * h),
                    c3(std::size_t(w) * h), c4(std::size_t(w) * h);
    convolve(src, w, h, m1, c1.data());
    convolve(src, w, h, m2, c2.data());
    convolve(src, w, h, m3, c3.data());
    convolve(src, w, h, m4, c4.data());
    for (int i = 0; i < w * h; i++)
        dst[i] = u8(std::max(std::max(int(c1[i]), int(c2[i])),
                             std::max(int(c3[i]), int(c4[i]))));
}

// ---- zimg-style separable resize (normalized) ----------------------------
// kernel: VS Bicubic with B = a, C = b, and Lanczos with the given taps
static double bicubW(double d, double b, double c)
{
    if (d <= 1.0)
        return ((12 - 9 * b - 6 * c) * d * d * d + (-18 + 12 * b + 6 * c) * d * d + (6 - 2 * b)) / 6.0;
    if (d <= 2.0)
        return ((-b - 6 * c) * d * d * d + (6 * b + 30 * c) * d * d + (-12 * b - 48 * c) * d + (8 * b + 24 * c)) / 6.0;
    return 0.0;
}

static double lanczW(double d, double taps)
{
    if (d == 0.0)
        return 1.0;
    if (d >= taps)
        return 0.0;
    const double x = kPi * d;
    return std::sin(x) / x * std::sin(x / taps) / (x / taps);
}

enum class Kern { Bicubic, Lanczos };

static int axis1D(const u8 *src, int N, int o, int n, Kern kern, double p1, double p2)
{
    const double pos = (o + 0.5) * (double(N) / n) - 0.5;
    const double support = (kern == Kern::Bicubic) ? 2.0 : p2;
    const int left = int(std::floor(pos - support));
    const int right = int(std::ceil(pos + support));
    double sum = 0.0, wsum = 0.0;
    for (int k = left; k <= right; k++) {
        const double w = (kern == Kern::Bicubic) ? bicubW(std::fabs(pos - k), p1, p2)
                                                 : lanczW(std::fabs(pos - k), p2);
        const int kk = std::max(0, std::min(N - 1, k));
        sum += w * src[kk];
        wsum += w;
    }
    if (wsum != 0.0)
        sum /= wsum;
    return clamp255(rintE(std::min(255.0, std::max(0.0, sum))));
}

static void resizeSep(const u8 *src, int w, int h, int nw, int nh, Kern kern, double p1, double p2, u8 *dst)
{
    std::vector<u8> tmp(std::size_t(nw) * h);
    for (int y = 0; y < h; y++)
        for (int o = 0; o < nw; o++)
            tmp[y * nw + o] = u8(axis1D(src + y * w, w, o, nw, kern, p1, p2));
    for (int x = 0; x < nw; x++) {
        std::vector<u8> col(h);
        for (int y = 0; y < h; y++)
            col[y] = tmp[y * nw + x];
        for (int o = 0; o < nh; o++)
            dst[o * nw + x] = u8(axis1D(col.data(), h, o, nh, kern, p1, p2));
    }
}

static void resizeBicubic(const u8 *src, int w, int h, int nw, int nh, double b, double c, u8 *dst)
{
    resizeSep(src, w, h, nw, nh, Kern::Bicubic, b, c, dst);
}

static void resizeLanczos(const u8 *src, int w, int h, int nw, int nh, u8 *dst)
{
    resizeSep(src, w, h, nw, nh, Kern::Lanczos, 0.0, 3.0, dst);
}

// ---- MaskedMerge: linear blend weighted by the mask ----------------------
static void maskedMerge(const u8 *a, const u8 *b, const u8 *mask, int n, u8 *dst)
{
    for (int i = 0; i < n; i++)
        dst[i] = u8(clamp255(rintE((double(a[i]) * (255 - mask[i]) +
                                    double(b[i]) * mask[i]) / 255.0)));
}

static int m4(double x)
{
    if (x < 16.0)
        return 16;
    return int(std::floor(x / 4.0 + 0.5)) * 4;
}

// ---- DeHalo_alpha (havsfunc defaults: rx=2, ry=2, darkstr=1, brightstr=1,
// lowsens=50, highsens=50, ss=1.5) ----------------------------------------
static void dehaloAlpha(const u8 *src, int w, int h, u8 *dst)
{
    const int ox = w, oy = h;
    const double rx = 2.0, ry = 2.0;
    const double ss = 1.5;

    const int dw = m4(ox / rx), dh = m4(oy / ry);

    // halos = Bicubic down (a=1/3,b=1/3) then Bicubic up (a=1,b=0)
    std::vector<u8> t1(std::size_t(dw) * dh), halos(std::size_t(ox) * oy);
    resizeBicubic(src, w, h, dw, dh, 1.0 / 3.0, 1.0 / 3.0, t1.data());
    resizeBicubic(t1.data(), dw, dh, ox, oy, 1.0, 0.0, halos.data());

    // are = max(src) - min(src); ugly = max(halos) - min(halos)
    const int full[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    std::vector<u8> mx(std::size_t(ox) * oy), mn(std::size_t(ox) * oy), are(std::size_t(ox) * oy);
    morpho(src, w, h, full, true, mx.data());
    morpho(src, w, h, full, false, mn.data());
    for (int i = 0; i < ox * oy; i++)
        are[i] = u8(clamp255(int(mx[i]) - int(mn[i])));
    morpho(halos.data(), ox, oy, full, true, mx.data());
    morpho(halos.data(), ox, oy, full, false, mn.data());
    std::vector<u8> ugly(std::size_t(ox) * oy);
    for (int i = 0; i < ox * oy; i++)
        ugly[i] = u8(clamp255(int(mx[i]) - int(mn[i])));

    // so = expr(ugly, are); whole expression in float, one final round
    std::vector<u8> so(std::size_t(ox) * oy);
    for (int i = 0; i < ox * oy; i++) {
        const double x = ugly[i], y = are[i];
        const double t = (y - x) / (y + (y == 0 ? 1.0 : 0.0));
        const double v = (t * 255.0 - 50.0) * ((y + 256.0) / 512.0 + 0.5);
        so[i] = u8(clamp255(rintE(v)));
    }

    // lets = MaskedMerge(halos, src, so)
    std::vector<u8> lets(std::size_t(ox) * oy);
    maskedMerge(halos.data(), src, so.data(), ox * oy, lets.data());

    // ss > 1: upsample to m4(ox*ss) x m4(oy*ss), min/max blend, downsample back
    const int upW = m4(ox * ss), upH = m4(oy * ss);
    std::vector<u8> up(std::size_t(upW) * upH), maxL(std::size_t(upW) * upH),
                    minL(std::size_t(upW) * upH), lower(std::size_t(upW) * upH),
                    upper(std::size_t(upW) * upH);
    resizeLanczos(src, w, h, upW, upH, up.data());
    morpho(lets.data(), ox, oy, full, true, mx.data());
    resizeBicubic(mx.data(), ox, oy, upW, upH, 1.0 / 3.0, 1.0 / 3.0, maxL.data());
    morpho(lets.data(), ox, oy, full, false, mn.data());
    resizeBicubic(mn.data(), ox, oy, upW, upH, 1.0 / 3.0, 1.0 / 3.0, minL.data());
    for (int i = 0; i < upW * upH; i++)
        lower[i] = u8(std::min(int(up[i]), int(maxL[i])));
    for (int i = 0; i < upW * upH; i++)
        upper[i] = u8(std::max(int(lower[i]), int(minL[i])));
    std::vector<u8> remove(std::size_t(ox) * oy);
    resizeLanczos(upper.data(), upW, upH, ox, oy, remove.data());

    // them = expr(src, remove, 'x y < x x y - 1 * - x x y - 1 * - ?')
    for (int i = 0; i < ox * oy; i++) {
        const int x = src[i], y = remove[i];
        dst[i] = u8(clamp255(x < y ? 2 * x - y : 2 * x - y));
    }
}

} // namespace

// FineDehalo (havsfunc defaults), luma only, 8-bit.
void fineDehalo(const u8 *src, int w, int h, u8 *dst)
{
    const int thmi = 80, thma = 128, thlimi = 50, thlima = 100;
    const int rxI = 2, ryI = 2;
    const int n = w * h;

    std::vector<u8> dehaloed(n), edges(n), strong(n), large(n), light(n),
                    shrink(n), sc1(n), sc2(n), shrMed(n), outside(n);

    dehaloAlpha(src, w, h, dehaloed.data());
    prewitt(src, w, h, edges.data());

    // strong = expr 'x thmi - (thma-thmi) / 255 *'
    for (int i = 0; i < n; i++)
        strong[i] = u8(clamp255(rintE((double(int(edges[i]) - thmi) / double(thma - thmi)) * 255.0)));
    // large = expand rectangle rx x ry
    multiRec(strong.data(), w, h, rxI, ryI, 0, true, large.data());

    // light = expr 'x thlimi - (thlima-thlimi) / 255 *'
    for (int i = 0; i < n; i++)
        light[i] = u8(clamp255(rintE((double(int(edges[i]) - thlimi) / double(thlima - thlimi)) * 255.0)));
    // shrink = expand ellipse, *4, inpand ellipse, 2x box3
    multiRec(light.data(), w, h, rxI, ryI, 1, true, shrink.data());
    for (int i = 0; i < n; i++)
        shrink[i] = u8(clamp255(int(shrink[i]) * 4));
    multiRec(shrink.data(), w, h, rxI, ryI, 1, false, sc1.data());
    box3(sc1.data(), w, h, sc2.data());
    box3(sc2.data(), w, h, shrink.data());

    // shr_med = max(strong, shrink)
    for (int i = 0; i < n; i++)
        shrMed[i] = u8(std::max(int(strong[i]), int(shrink[i])));
    // outside = (large - shr_med) * 2
    for (int i = 0; i < n; i++)
        outside[i] = u8(clamp255((int(large[i]) - int(shrMed[i])) * 2));
    box3(outside.data(), w, h, sc1.data());
    for (int i = 0; i < n; i++)
        outside[i] = u8(clamp255(int(sc1[i]) * 2));

    // dst = MaskedMerge(src, dehaloed, outside)
    maskedMerge(src, dehaloed.data(), outside.data(), n, dst);
}