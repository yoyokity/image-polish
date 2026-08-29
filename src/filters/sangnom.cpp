#include "sangnom.h"

#include "pfor.h"

#include <algorithm>
#include <cstring>
#include <vector>

// Scalar port of the VapourSynth sangnom plugin (dubhater, com.mio.sangnom):
// the output is built from the kept (top) field rows; every other line is
// reconstructed by directional interpolation, picking among 9 candidate
// directions the one with minimum (blurred) difference between the source
// lines above and below. `aa` scales the threshold that decides whether the
// directional value is taken or the plain vertical average (aa=48 -> 63).
namespace {

constexpr int NBUF = 9;
enum {
    AD_M3P3 = 0, AD_M2P2 = 1, AD_M1P1 = 2, SG_FWD = 3,
    AD_0    = 4, SG_REV    = 5, AD_P1M1 = 6, AD_P2M2 = 7, AD_P3M3 = 8
};

// (4*p1 + 5*p2 - p3) >> 3, on the 16-bit unsigned representation, clamped to
// 255 (reference calculateSangNom: negative sums saturate to 255).
inline int calcSangNom(int p1, int p2, int p3)
{
    unsigned s = static_cast<unsigned>(p1 * 4 + p2 * 5 - p3) >> 3;
    return static_cast<int>(std::min(s, 255u));
}

inline int avgPix(int a, int b) { return (a + b + 1) >> 1; }

// Border replicate (reference loadPixel clamping to the row ends).
inline int rowPx(const u8 *row, int x, int w)
{
    if (x < 0)
        return row[0];
    if (x >= w)
        return row[w - 1];
    return row[x];
}

} // namespace

void sangnom(const u8 *src, int w, int h, int aa, u8 *out)
{
    const int thresh = (21 * std::min(255, aa)) / 16;      // reference aaf (8-bit)
    const int bufStride = (w + 31) & ~31;                  // padded like the plugin
    const int bufH = (h + 1) >> 1;

    // Keep the top field and mirror the last line (order=1): the interpolated
    // rows below overwrite the odd lines; row h-1 has no line below it.
    std::memcpy(out, src, std::size_t(w) * h);
    std::memcpy(out + (h - 1) * w, out + (h - 2) * w, sizeof(u8) * w);

    // 9 zeroed difference buffers; rows 0 and bufH stay zero and only feed the
    // smoothing windows at the top/bottom edges (same padding as the plugin).
    const size_t bufSize = std::size_t(bufStride) * (bufH + 1);
    std::vector<u8> pool(NBUF * bufSize, 0);
    u8 *bufs[NBUF];
    for (int i = 0; i < NBUF; i++)
        bufs[i] = pool.data() + i * bufSize;

    // --- 1. directional difference buffers, one row per interpolated line ---
    parallelFor(0, h / 2 - 1, [&](int y) {
        const u8 *p = src + std::size_t(2 * y) * w;
        const u8 *n = src + std::size_t(2 * y + 2) * w;
        const int off = (y + 1) * bufStride;
        for (int x = 0; x < w; x++) {
            const int pm3 = rowPx(p, x - 3, w), pm2 = rowPx(p, x - 2, w);
            const int pm1 = rowPx(p, x - 1, w), pc = p[x];
            const int pp1 = rowPx(p, x + 1, w), pp2 = rowPx(p, x + 2, w);
            const int pp3 = rowPx(p, x + 3, w);
            const int nm3 = rowPx(n, x - 3, w), nm2 = rowPx(n, x - 2, w);
            const int nm1 = rowPx(n, x - 1, w), nc = n[x];
            const int np1 = rowPx(n, x + 1, w), np2 = rowPx(n, x + 2, w);
            const int np3 = rowPx(n, x + 3, w);

            bufs[AD_M3P3][off + x] = u8(std::abs(pm3 - np3));
            bufs[AD_M2P2][off + x] = u8(std::abs(pm2 - np2));
            bufs[AD_M1P1][off + x] = u8(std::abs(pm1 - np1));
            bufs[AD_0][off + x]    = u8(std::abs(pc - nc));
            bufs[AD_P1M1][off + x] = u8(std::abs(pp1 - nm1));
            bufs[AD_P2M2][off + x] = u8(std::abs(pp2 - nm2));
            bufs[AD_P3M3][off + x] = u8(std::abs(pp3 - nm3));
            bufs[SG_FWD][off + x]  = u8(std::abs(calcSangNom(pm1, pc, pp1)
                                                - calcSangNom(np1, nc, nm1)));
            bufs[SG_REV][off + x]  = u8(std::abs(calcSangNom(pp1, pc, pm1)
                                                - calcSangNom(nm1, nc, np1)));
        }
    });

    // --- 2. smooth each buffer -----------------------------------------------
    // Sequential recurrence in the reference: the 3-row vertical sum at row
    // y+1 re-reads the value written at row y, so each buffer must be swept
    // top-down. The 9 buffers are independent -> parallelize over buffers.
    parallelFor(0, NBUF, [&](int b) {
        u8 *bp = bufs[b];
        std::vector<int> line(bufStride);
        for (int y = 0; y < bufH - 1; y++) {
            const u8 *r0 = bp + std::size_t(y) * bufStride;
            const u8 *r1 = r0 + bufStride;
            const u8 *r2 = r1 + bufStride;
            for (int x = 0; x < bufStride; x++)
                line[x] = r0[x] + r1[x] + r2[x];
            u8 *dstRow = bp + std::size_t(y + 1) * bufStride;
            for (int x = 0; x < bufStride; x++) {
                int s = 0;
                for (int k = -3; k <= 3; k++) {
                    int xi = x + k;
                    if (xi < 0)
                        xi = 0;
                    else if (xi >= bufStride)
                        xi = bufStride - 1;
                    s += line[xi];
                }
                s /= 16;
                dstRow[x] = u8(std::min(s, 255));
            }
        }
    });

    // --- 3. pick the direction of minimum smoothed difference ---------------
    parallelFor(0, h / 2 - 1, [&](int y) {
        const u8 *p = src + std::size_t(2 * y) * w;
        const u8 *n = src + std::size_t(2 * y + 2) * w;
        u8 *o = out + std::size_t(2 * y + 1) * w;
        const int off = (y + 1) * bufStride;
        for (int x = 0; x < w; x++) {
            const int pm3 = rowPx(p, x - 3, w), pm2 = rowPx(p, x - 2, w);
            const int pm1 = rowPx(p, x - 1, w), pc = p[x];
            const int pp1 = rowPx(p, x + 1, w), pp2 = rowPx(p, x + 2, w);
            const int pp3 = rowPx(p, x + 3, w);
            const int nm3 = rowPx(n, x - 3, w), nm2 = rowPx(n, x - 2, w);
            const int nm1 = rowPx(n, x - 1, w), nc = n[x];
            const int np1 = rowPx(n, x + 1, w), np2 = rowPx(n, x + 2, w);
            const int np3 = rowPx(n, x + 3, w);

            const int fwd1 = calcSangNom(pm1, pc, pp1);
            const int fwd2 = calcSangNom(np1, nc, nm1);
            const int bwd1 = calcSangNom(pp1, pc, pm1);
            const int bwd2 = calcSangNom(nm1, nc, np1);

            const int buf[9] = {
                bufs[AD_M3P3][off + x], bufs[AD_M2P2][off + x],
                bufs[AD_M1P1][off + x], bufs[SG_FWD][off + x],
                bufs[AD_0][off + x],    bufs[SG_REV][off + x],
                bufs[AD_P1M1][off + x], bufs[AD_P2M2][off + x],
                bufs[AD_P3M3][off + x]
            };
            int minBuf = buf[0];
            for (int i = 1; i < 9; i++)
                minBuf = std::min(minBuf, buf[i]);

            // priority order matches the reference scalar path exactly
            if (buf[AD_0] == minBuf || minBuf > thresh)
                o[x] = u8(avgPix(pc, nc));
            else if (buf[SG_REV] == minBuf)
                o[x] = u8(avgPix(bwd1, bwd2));
            else if (buf[SG_FWD] == minBuf)
                o[x] = u8(avgPix(fwd1, fwd2));
            else if (buf[AD_P1M1] == minBuf)
                o[x] = u8(avgPix(pp1, nm1));
            else if (buf[AD_M1P1] == minBuf)
                o[x] = u8(avgPix(pm1, np1));
            else if (buf[AD_P2M2] == minBuf)
                o[x] = u8(avgPix(pp2, nm2));
            else if (buf[AD_M2P2] == minBuf)
                o[x] = u8(avgPix(pm2, np2));
            else if (buf[AD_P3M3] == minBuf)
                o[x] = u8(avgPix(pp3, nm3));
            else
                o[x] = u8(avgPix(pm3, np3));       // buf[AD_M3P3] == minBuf
        }
    });
}