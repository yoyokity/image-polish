#include "resample.h"

#include <algorithm>
#include <cmath>
#include <vector>

static double spline36(double x)
{
    x = std::fabs(x);
    double v = 0.0;
    if (x < 1.0)
        v = ((13.0 / 11.0 * x - 453.0 / 209.0) * x - 3.0 / 209.0) * x + 1.0;
    else if (x < 2.0) {
        x -= 1.0;
        v = ((-6.0 / 11.0 * x + 270.0 / 209.0) * x - 156.0 / 209.0) * x;
    } else if (x < 3.0) {
        x -= 2.0;
        v = ((1.0 / 11.0 * x - 45.0 / 209.0) * x + 26.0 / 209.0) * x;
    }
    return v;
}

// Resample one axis with the exact fmtconv ResampleUtil convention:
//   srcPos(o) = (o + 0.5) * (srcLen/dstLen) - 0.5 + shift   (pixel centres aligned)
// kernel spline36 scaled by max(ratio,1), taps renormalized by the total weight
// (negative lobes included), result rounded to nearest.
static void resampleAxis(const u8 *src, int srcLen, u8 *dst, int dstLen, double shift)
{
    const double ratio = double(srcLen) / dstLen;
    const double zc = std::max(ratio, 1.0);          // kernel scale
    const double support = 3.0 * zc;

    for (int o = 0; o < dstLen; o++) {
        const double pos = (o + 0.5) * ratio - 0.5 + shift;
        const int k0 = int(std::floor(pos - support));
        const int k1 = int(std::ceil(pos + support));
        double sum = 0.0, wsum = 0.0;
        for (int k = k0; k <= k1; k++) {
            const double w = spline36((pos - k) / zc);
            const int kk = std::max(0, std::min(srcLen - 1, k));
            sum += w * src[kk];
            wsum += w;
        }
        if (wsum != 0.0)
            sum /= wsum;
        dst[o] = u8(std::max(0.0, std::min(255.0, sum + 0.5)));
    }
}

// vertical resize of a w*srcH image to w*dstH
void resampleV(const u8 *src, int w, int srcH, int dstH, double sy, u8 *dst)
{
    for (int x = 0; x < w; x++) {
        std::vector<u8> col(srcH);
        for (int y = 0; y < srcH; y++)
            col[y] = src[y * w + x];
        std::vector<u8> r(dstH);
        resampleAxis(col.data(), srcH, r.data(), dstH, sy);
        for (int y = 0; y < dstH; y++)
            dst[y * w + x] = r[y];
    }
}

void transpose(const u8 *src, int w, int h, u8 *dst)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[x * h + y] = src[y * w + x];
}

// resize a w*h image to nw*nh with the spline36 kernel, pixel centres
// aligned (shift = 0); implemented as two separable passes
void resample2D(const u8 *src, int w, int h, int nw, int nh, u8 *dst)
{
    // horizontal: transpose -> vertical resample (w -> nw) -> transpose back
    std::vector<u8> t1(std::size_t(h) * w);
    transpose(src, w, h, t1.data());                        // h x w
    std::vector<u8> t2(std::size_t(h) * nw);
    resampleV(t1.data(), h, w, nw, 0.0, t2.data());         // h x nw
    std::vector<u8> tmp(std::size_t(nw) * h);
    transpose(t2.data(), h, nw, tmp.data());                // nw x h
    // vertical
    resampleV(tmp.data(), nw, h, nh, 0.0, dst);             // nw x nh
}