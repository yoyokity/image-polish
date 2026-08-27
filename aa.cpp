/*
 * aa.cpp - EEDI2-based anti-aliasing, standalone C++ command line tool.
 *
 * Implements the well-known VapourSynth anti-aliasing chain:
 *
 *     aa = ShufflePlanes(clip,0,GRAY)
 *     aa = EEDI2(aa, field=1, mthresh=10, lthresh=20, vthresh=20, maxd=24, nt=50)
 *     aa = resample(aa, w, h, 0, -0.5).Transpose()
 *     aa = EEDI2(aa, field=1, mthresh=10, lthresh=20, vthresh=20, maxd=24, nt=50)
 *     aa = resample(aa, h, w, 0, -0.5).Transpose()
 *     aa  = ShufflePlanes([aa, clip], [0,1,2], YUV)   // luma replaced, chroma kept
 *     out = Repair(aa, clip, 2)
 *
 * Behavioral references (specification only; this is an original port):
 *   - EEDI2:   tritical / HolyWu, VapourSynth-EEDI2, EEDI2.cpp v0.9.2 (GPLv2)
 *   - Repair:  Laurent de Soras / Fredrik Mellbin, vs-removegrain repairvs.cpp
 *   - resample: Laurent de Soras, fmtconv (Scaler shift semantics + ContFirSpline36)
 * Image I/O via stb_image / stb_image_write (public domain).
 *
 * Usage:
 *   aa.exe -i input.png -o output.png [options]
 *   options: --mthresh N --lthresh N --vthresh N --maxd N --nt N --field N
 *            --repair N (0 disables Repair), -h / --help
 *
 * Build:
 *   g++ -O2 -std=c++17 -o aa.exe aa.cpp
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

typedef std::uint8_t u8;

// ---------------------------------------------------------------------------
// EEDI2 parameters and setup (defaults identical to the original script)
// ---------------------------------------------------------------------------
struct Eedi2Params {
    int field    = 1;      // 1: original lines land on even rows, new lines on odd
    int mthresh  = 10;
    int lthresh  = 20;
    int vthresh  = 20;
    int estr     = 2;
    int dstr     = 4;
    int maxd     = 24;
    int nt       = 50;
    int pp       = 1;

    int mthresh2 = 0;      // mthresh^2
    int vthresh9 = 0;      // vthresh * 81
    unsigned nt4 = 0, nt7 = 0, nt8 = 0, nt13 = 0, nt19 = 0;
    int8_t  limlut[33];
    int16_t limlut2[33];
};

static void eedi2_prepare(Eedi2Params &p)
{
    p.mthresh2 = p.mthresh * p.mthresh;
    p.vthresh9 = p.vthresh * 81;
    p.nt4  = unsigned(p.nt) * 4;
    p.nt7  = unsigned(p.nt) * 7;
    p.nt8  = unsigned(p.nt) * 8;
    p.nt13 = unsigned(p.nt) * 13;
    p.nt19 = unsigned(p.nt) * 19;

    static const int8_t limlut[33] = {
        6, 6, 7, 7, 8, 8, 9, 9, 9, 10,
        10, 11, 11, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
        12, -1, -1
    };
    std::memcpy(p.limlut, limlut, 33);
    for (int i = 0; i < 33; i++)
        p.limlut2[i] = limlut[i];
}

// ---------------------------------------------------------------------------
// EEDI2 core: doubles the height with edge-directed interpolation (gray8)
// ---------------------------------------------------------------------------
class Eedi2
{
public:
    Eedi2(const Eedi2Params &p) : d(p) {}

    // in: w*h gray8 -> out: w*2h gray8.
    // Original lines land on rows with parity (1-field), interpolated lines
    // on rows with parity field.
    void run(const u8 *src, int w, int h, u8 *out)
    {
        const int stride = w;
        const int H2 = h * 2;
        std::vector<u8> msk(std::size_t(w) * h), tmp(std::size_t(w) * h), dst(std::size_t(w) * h);

        // The 2x buffers keep a padding frame(2 rows above + 2 left bytes) so that
        // the stride-contiguous reads at x=0 / x=w-1 (which in the reference bleed
        // into the neighbouring row) and postProcess's first-row backward read can
        // be emulated without out-of-bounds access; pads are zeroed.
        const size_t padRow = 2u * stride, padCol = 2u;
        struct Buf2 {
            void alloc(int w, int H2, size_t padRow, size_t padCol)
            {
                base.assign(std::size_t(w) * H2 + padRow + padCol, 0);
                frame = base.data() + padRow + padCol;
            }
            std::vector<u8> base;
            u8 *frame = nullptr;
        } dst2, dst2M, tmp2, tmp2_2, msk2;
        dst2.alloc(w, H2, padRow, padCol);
        dst2M.alloc(w, H2, padRow, padCol);
        tmp2.alloc(w, H2, padRow, padCol);
        tmp2_2.alloc(w, H2, padRow, padCol);
        msk2.alloc(w, H2, padRow, padCol);

        buildEdgeMask(src, w, h, msk.data());
        erode(msk.data(), w, h, tmp.data());
        dilate(tmp.data(), w, h, msk.data());
        erode(msk.data(), w, h, tmp.data());
        removeSmallHorzGaps(tmp.data(), w, h, msk.data());

        calcDirections(src, msk.data(), w, h, tmp.data());
        filterDirMap(msk.data(), tmp.data(), w, h, dst.data());
        expandDirMap(msk.data(), dst.data(), w, h, tmp.data());
        filterMap(msk.data(), tmp.data(), w, h, dst.data());

        std::fill(dst2.base.begin(), dst2.base.end(), 0);
        std::fill(tmp2_2.base.begin(), tmp2_2.base.end(), 0);
        std::fill(msk2.base.begin(), msk2.base.end(), 0);
        upscaleBy2(src, w, h, dst2.frame);
        upscaleBy2(dst.data(), w, h, tmp2_2.frame);
        upscaleBy2(msk.data(), w, h, msk2.frame);

        markDirections2X(msk2.frame, tmp2_2.frame, w, H2, tmp2.frame);
        filterDirMap2X(msk2.frame, tmp2.frame, w, H2, dst2M.frame);
        expandDirMap2X(msk2.frame, dst2M.frame, w, H2, tmp2.frame);
        fillGaps2X(msk2.frame, tmp2.frame, w, H2, dst2M.frame);
        fillGaps2X(msk2.frame, dst2M.frame, w, H2, tmp2.frame);
        interpolateLattice(tmp2_2.frame, tmp2.frame, w, H2, dst2.frame);

        if (d.pp == 1 || d.pp == 3) {
            std::memcpy(tmp2_2.frame, tmp2.frame, sizeof(u8) * stride * H2);
            filterDirMap2X(msk2.frame, tmp2.frame, w, H2, dst2M.frame);
            expandDirMap2X(msk2.frame, dst2M.frame, w, H2, tmp2.frame);
            postProcess(tmp2.frame, tmp2_2.frame, w, H2, dst2.frame);
        }

        std::memcpy(out, dst2.frame, sizeof(u8) * stride * H2);
    }

private:
    static const u8 NEUTRAL = 128;
    static const u8 PEAK = 255;
    Eedi2Params d;

    // ---- 1. edge mask: mark pixels with enough local structure ------------
    void buildEdgeMask(const u8 *src, int w, int h, u8 *msk)
    {
        std::memset(msk, 0, sizeof(u8) * w * h);
        const int stride = w;
        for (int y = 1; y < h - 1; y++) {
            const u8 *pp = src + (y - 1) * stride;
            const u8 *pc = src + y * stride;
            const u8 *pn = src + (y + 1) * stride;
            u8 *m = msk + y * stride;
            for (int x = 1; x < w - 1; x++) {
                const bool flat3 = std::abs(int(pp[x]) - int(pc[x])) < 10 &&
                                   std::abs(int(pc[x]) - int(pn[x])) < 10 &&
                                   std::abs(int(pp[x]) - int(pn[x])) < 10;
                const bool flatL = std::abs(int(pp[x - 1]) - int(pc[x - 1])) < 10 &&
                                   std::abs(int(pc[x - 1]) - int(pn[x - 1])) < 10 &&
                                   std::abs(int(pp[x - 1]) - int(pn[x - 1])) < 10;
                const bool flatR = std::abs(int(pp[x + 1]) - int(pc[x + 1])) < 10 &&
                                   std::abs(int(pc[x + 1]) - int(pn[x + 1])) < 10 &&
                                   std::abs(int(pp[x + 1]) - int(pn[x + 1])) < 10;
                if (flat3 || (flatL && flatR))
                    continue;

                const int sum = pp[x - 1] + pp[x] + pp[x + 1] + pc[x - 1] + pc[x] + pc[x + 1] + pn[x - 1] + pn[x] + pn[x + 1];
                const int sumsq = pp[x - 1] * pp[x - 1] + pp[x] * pp[x] + pp[x + 1] * pp[x + 1] +
                                  pc[x - 1] * pc[x - 1] + pc[x] * pc[x] + pc[x + 1] * pc[x + 1] +
                                  pn[x - 1] * pn[x - 1] + pn[x] * pn[x] + pn[x + 1] * pn[x + 1];
                if (9 * sumsq - sum * sum < d.vthresh9)
                    continue;

                const int Ix = std::abs(int(pc[x + 1]) - int(pc[x - 1]));
                const int Iy = std::max({ std::abs(int(pp[x]) - int(pn[x])),
                                          std::abs(int(pp[x]) - int(pc[x])),
                                          std::abs(int(pc[x]) - int(pn[x])) });
                if (Ix * Ix + Iy * Iy >= d.mthresh2) {
                    m[x] = PEAK;
                    continue;
                }

                const int Ixx = std::abs(int(pc[x - 1]) - 2 * int(pc[x]) + int(pc[x + 1]));
                const int Iyy = std::abs(int(pp[x]) - 2 * int(pc[x]) + int(pn[x]));
                if (Ixx + Iyy >= d.lthresh)
                    m[x] = PEAK;
            }
        }
    }

    static void erode(const u8 *msk, int w, int h, u8 *dst)
    {
        std::memcpy(dst, msk, sizeof(u8) * w * h);
        for (int y = 1; y < h - 1; y++) {
            const u8 *pp = msk + (y - 1) * w;
            const u8 *pc = msk + y * w;
            const u8 *pn = msk + (y + 1) * w;
            u8 *o = dst + y * w;
            for (int x = 1; x < w - 1; x++) {
                if (pc[x] != PEAK)
                    continue;
                int count = 0;
                if (pp[x - 1] == PEAK) count++;
                if (pp[x] == PEAK) count++;
                if (pp[x + 1] == PEAK) count++;
                if (pc[x - 1] == PEAK) count++;
                if (pc[x + 1] == PEAK) count++;
                if (pn[x - 1] == PEAK) count++;
                if (pn[x] == PEAK) count++;
                if (pn[x + 1] == PEAK) count++;
                if (count < 2)   // estr = 2
                    o[x] = 0;
            }
        }
    }

    static void dilate(const u8 *msk, int w, int h, u8 *dst)
    {
        std::memcpy(dst, msk, sizeof(u8) * w * h);
        for (int y = 1; y < h - 1; y++) {
            const u8 *pp = msk + (y - 1) * w;
            const u8 *pc = msk + y * w;
            const u8 *pn = msk + (y + 1) * w;
            u8 *o = dst + y * w;
            for (int x = 1; x < w - 1; x++) {
                if (pc[x] != 0)
                    continue;
                int count = 0;
                if (pp[x - 1] == PEAK) count++;
                if (pp[x] == PEAK) count++;
                if (pp[x + 1] == PEAK) count++;
                if (pc[x - 1] == PEAK) count++;
                if (pc[x + 1] == PEAK) count++;
                if (pn[x - 1] == PEAK) count++;
                if (pn[x] == PEAK) count++;
                if (pn[x + 1] == PEAK) count++;
                if (count >= 4)  // dstr = 4
                    o[x] = PEAK;
            }
        }
    }

    static void removeSmallHorzGaps(const u8 *msk, int w, int h, u8 *dst)
    {
        std::memcpy(dst, msk, sizeof(u8) * w * h);
        for (int y = 1; y < h - 1; y++) {
            const u8 *pc = msk + y * w;
            u8 *o = dst + y * w;
            for (int x = 3; x < w - 3; x++) {
                if (pc[x]) {
                    if (pc[x - 3] || pc[x - 2] || pc[x - 1] || pc[x + 1] || pc[x + 2] || pc[x + 3])
                        continue;
                    o[x] = 0;
                } else {
                    if ((pc[x + 1] && (pc[x - 1] || pc[x - 2] || pc[x - 3])) ||
                        (pc[x + 2] && (pc[x - 1] || pc[x - 2])) ||
                        (pc[x + 3] && pc[x - 1]))
                        o[x] = PEAK;
                }
            }
        }
    }

    // ---- 2. direction map at original resolution --------------------------
    void calcDirections(const u8 *src, const u8 *msk, int w, int h, u8 *dst)
    {
        const int stride = w;
        std::fill_n(dst, std::size_t(stride) * h, PEAK);
        for (int y = 1; y < h - 1; y++) {
            const u8 *src2p = src + (y - 2) * stride;
            const u8 *srcpp = src + (y - 1) * stride;
            const u8 *srcp  = src + y * stride;
            const u8 *srcpn = src + (y + 1) * stride;
            const u8 *src2n = src + (y + 2) * stride;
            const u8 *mskpp = msk + (y - 1) * stride;
            const u8 *mskp  = msk + y * stride;
            const u8 *mskpn = msk + (y + 1) * stride;
            u8 *dstp = dst + y * stride;

            for (int x = 1; x < w - 1; x++) {
                if (mskp[x] != PEAK || (mskp[x - 1] != PEAK && mskp[x + 1] != PEAK))
                    continue;

                const int uStart = std::max(-x + 1, -d.maxd);
                const int uStop = std::min(w - 2 - x, d.maxd);
                const unsigned min0 = std::abs(int(srcp[x]) - int(srcpn[x])) + std::abs(int(srcp[x]) - int(srcpp[x]));
                unsigned minA = std::min(d.nt19, min0 * 9);
                unsigned minB = std::min(d.nt13, min0 * 6);
                unsigned minC = minA, minD = minB, minE = minB;
                int dirA = -5000, dirB = -5000, dirC = -5000, dirD = -5000, dirE = -5000;

                for (int u = uStart; u <= uStop; u++) {
                    if ((y == 1 || mskpp[x - 1 + u] == PEAK || mskpp[x + u] == PEAK || mskpp[x + 1 + u] == PEAK) &&
                        (y == h - 2 || mskpn[x - 1 - u] == PEAK || mskpn[x - u] == PEAK || mskpn[x + 1 - u] == PEAK)) {
                        const unsigned diffsn = std::abs(int(srcp[x - 1]) - int(srcpn[x - 1 - u])) +
                                               std::abs(int(srcp[x]) - int(srcpn[x - u])) +
                                               std::abs(int(srcp[x + 1]) - int(srcpn[x + 1 - u]));
                        const unsigned diffsp = std::abs(int(srcp[x - 1]) - int(srcpp[x - 1 + u])) +
                                               std::abs(int(srcp[x]) - int(srcpp[x + u])) +
                                               std::abs(int(srcp[x + 1]) - int(srcpp[x + 1 + u]));
                        const unsigned diffps = std::abs(int(srcpp[x - 1]) - int(srcp[x - 1 - u])) +
                                               std::abs(int(srcpp[x]) - int(srcp[x - u])) +
                                               std::abs(int(srcpp[x + 1]) - int(srcp[x + 1 - u]));
                        const unsigned diffns = std::abs(int(srcpn[x - 1]) - int(srcp[x - 1 + u])) +
                                               std::abs(int(srcpn[x]) - int(srcp[x + u])) +
                                               std::abs(int(srcpn[x + 1]) - int(srcp[x + 1 + u]));
                        const unsigned diff = diffsn + diffsp + diffps + diffns;
                        unsigned diffD = diffsp + diffns;
                        unsigned diffE = diffsn + diffps;

                        if (diff < minB) {
                            dirB = u;
                            minB = diff;
                        }

                        if (y > 1) {
                            const unsigned diff2pp = std::abs(int(src2p[x - 1]) - int(srcpp[x - 1 - u])) +
                                                    std::abs(int(src2p[x]) - int(srcpp[x - u])) +
                                                    std::abs(int(src2p[x + 1]) - int(srcpp[x + 1 - u]));
                            const unsigned diffp2p = std::abs(int(srcpp[x - 1]) - int(src2p[x - 1 + u])) +
                                                    std::abs(int(srcpp[x]) - int(src2p[x + u])) +
                                                    std::abs(int(srcpp[x + 1]) - int(src2p[x + 1 + u]));
                            const unsigned diffA = diff + diff2pp + diffp2p;
                            diffD += diffp2p;
                            diffE += diff2pp;

                            if (diffA < minA) {
                                dirA = u;
                                minA = diffA;
                            }
                        }

                        if (y < h - 2) {
                            const unsigned diff2nn = std::abs(int(src2n[x - 1]) - int(srcpn[x - 1 + u])) +
                                                    std::abs(int(src2n[x]) - int(srcpn[x + u])) +
                                                    std::abs(int(src2n[x + 1]) - int(srcpn[x + 1 + u]));
                            const unsigned diffn2n = std::abs(int(srcpn[x - 1]) - int(src2n[x - 1 - u])) +
                                                    std::abs(int(srcpn[x]) - int(src2n[x - u])) +
                                                    std::abs(int(srcpn[x + 1]) - int(src2n[x + 1 - u]));
                            const unsigned diffC = diff + diff2nn + diffn2n;
                            diffD += diff2nn;
                            diffE += diffn2n;

                            if (diffC < minC) {
                                dirC = u;
                                minC = diffC;
                            }
                        }

                        if (diffD < minD) {
                            dirD = u;
                            minD = diffD;
                        }

                        if (diffE < minE) {
                            dirE = u;
                            minE = diffE;
                        }
                    }
                }

                int order[5];
                unsigned k = 0;
                if (dirA != -5000) order[k++] = dirA;
                if (dirB != -5000) order[k++] = dirB;
                if (dirC != -5000) order[k++] = dirC;
                if (dirD != -5000) order[k++] = dirD;
                if (dirE != -5000) order[k++] = dirE;

                if (k > 1) {
                    std::sort(order, order + k);
                    const int mid = (k & 1) ? order[k / 2] : (order[(k - 1) / 2] + order[k / 2] + 1) / 2;
                    const int lim = std::max<int>(d.limlut[std::abs(mid)] / 4, 2);
                    int sum = 0;
                    unsigned count = 0;
                    for (unsigned i = 0; i < k; i++) {
                        if (std::abs(order[i] - mid) <= lim) {
                            sum += order[i];
                            count++;
                        }
                    }
                    dstp[x] = (count > 1) ? u8(NEUTRAL + (static_cast<int>(static_cast<float>(sum) / count) << 2)) : NEUTRAL;
                } else {
                    dstp[x] = NEUTRAL;
                }
            }
        }
    }

    // ---- 3. direction map cleaning (original resolution) -------------------
    void filterDirMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst)
    {
        std::memcpy(dst, dmsk, sizeof(u8) * w * h);
        for (int y = 1; y < h - 1; y++) {
            const u8 *mskp  = msk + y * w;
            const u8 *dmskpp = dmsk + (y - 1) * w;
            const u8 *dmskp  = dmsk + y * w;
            const u8 *dmskpn = dmsk + (y + 1) * w;
            u8 *dstp = dst + y * w;
            for (int x = 1; x < w - 1; x++) {
                if (mskp[x] != PEAK)
                    continue;

                int order[9];
                unsigned u = 0;
                if (dmskpp[x - 1] != PEAK) order[u++] = dmskpp[x - 1];
                if (dmskpp[x] != PEAK) order[u++] = dmskpp[x];
                if (dmskpp[x + 1] != PEAK) order[u++] = dmskpp[x + 1];
                if (dmskp[x - 1] != PEAK) order[u++] = dmskp[x - 1];
                if (dmskp[x] != PEAK) order[u++] = dmskp[x];
                if (dmskp[x + 1] != PEAK) order[u++] = dmskp[x + 1];
                if (dmskpn[x - 1] != PEAK) order[u++] = dmskpn[x - 1];
                if (dmskpn[x] != PEAK) order[u++] = dmskpn[x];
                if (dmskpn[x + 1] != PEAK) order[u++] = dmskpn[x + 1];

                if (u < 4) {
                    dstp[x] = PEAK;
                    continue;
                }

                std::sort(order, order + u);
                const int mid = (u & 1) ? order[u / 2] : (order[(u - 1) / 2] + order[u / 2] + 1) / 2;
                const int lim = maxLut2(mid);
                int sum = 0;
                unsigned count = 0;
                for (unsigned i = 0; i < u; i++) {
                    if (std::abs(order[i] - mid) <= lim) {
                        sum += order[i];
                        count++;
                    }
                }

                if (count < 4 || (count < 5 && dmskp[x] == PEAK)) {
                    dstp[x] = PEAK;
                    continue;
                }

                dstp[x] = u8(static_cast<int>(static_cast<float>(sum + mid) / (count + 1) + 0.5f));
            }
        }
    }

    void expandDirMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst)
    {
        std::memcpy(dst, dmsk, sizeof(u8) * w * h);
        for (int y = 1; y < h - 1; y++) {
            const u8 *mskp  = msk + y * w;
            const u8 *dmskpp = dmsk + (y - 1) * w;
            const u8 *dmskp  = dmsk + y * w;
            const u8 *dmskpn = dmsk + (y + 1) * w;
            u8 *dstp = dst + y * w;
            for (int x = 1; x < w - 1; x++) {
                if (dmskp[x] != PEAK || mskp[x] != PEAK)
                    continue;

                int order[9];
                unsigned u = 0;
                if (dmskpp[x - 1] != PEAK) order[u++] = dmskpp[x - 1];
                if (dmskpp[x] != PEAK) order[u++] = dmskpp[x];
                if (dmskpp[x + 1] != PEAK) order[u++] = dmskpp[x + 1];
                if (dmskp[x - 1] != PEAK) order[u++] = dmskp[x - 1];
                if (dmskp[x + 1] != PEAK) order[u++] = dmskp[x + 1];
                if (dmskpn[x - 1] != PEAK) order[u++] = dmskpn[x - 1];
                if (dmskpn[x] != PEAK) order[u++] = dmskpn[x];
                if (dmskpn[x + 1] != PEAK) order[u++] = dmskpn[x + 1];

                if (u < 5)
                    continue;

                std::sort(order, order + u);
                const int mid = (u & 1) ? order[u / 2] : (order[(u - 1) / 2] + order[u / 2] + 1) / 2;
                const int lim = maxLut2(mid);
                int sum = 0;
                unsigned count = 0;
                for (unsigned i = 0; i < u; i++) {
                    if (std::abs(order[i] - mid) <= lim) {
                        sum += order[i];
                        count++;
                    }
                }

                if (count < 5)
                    continue;

                dstp[x] = u8(static_cast<int>(static_cast<float>(sum + mid) / (count + 1) + 0.5f));
            }
        }
    }

    void filterMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst)
    {
        std::memcpy(dst, dmsk, sizeof(u8) * w * h);
        const int twleve = 12;
        for (int y = 1; y < h - 1; y++) {
            const u8 *mskp  = msk + y * w;
            const u8 *dmskpp = dmsk + (y - 1) * w;
            const u8 *dmskp  = dmsk + y * w;
            const u8 *dmskpn = dmsk + (y + 1) * w;
            u8 *dstp = dst + y * w;
            for (int x = 1; x < w - 1; x++) {
                if (dmskp[x] == PEAK || mskp[x] != PEAK)
                    continue;

                int dir = (int(dmskp[x]) - NEUTRAL) / 4;
                const int lim = std::max(std::abs(dir) * 2, twleve);
                bool ict = false, icb = false;

                if (dir < 0) {
                    for (int j = std::max(-x, dir); j <= 0; j++) {
                        if ((std::abs(int(dmskpp[x + j]) - int(dmskp[x])) > lim && dmskpp[x + j] != PEAK) ||
                            (dmskp[x + j] == PEAK && dmskpp[x + j] == PEAK) ||
                            (std::abs(int(dmskp[x + j]) - int(dmskp[x])) > lim && dmskp[x + j] != PEAK)) {
                            ict = true;
                            break;
                        }
                    }
                } else {
                    for (int j = 0; j <= std::min(w - x - 1, dir); j++) {
                        if ((std::abs(int(dmskpp[x + j]) - int(dmskp[x])) > lim && dmskpp[x + j] != PEAK) ||
                            (dmskp[x + j] == PEAK && dmskpp[x + j] == PEAK) ||
                            (std::abs(int(dmskp[x + j]) - int(dmskp[x])) > lim && dmskp[x + j] != PEAK)) {
                            ict = true;
                            break;
                        }
                    }
                }

                if (ict) {
                    if (dir < 0) {
                        for (int j = 0; j <= std::min(w - x - 1, std::abs(dir)); j++) {
                            if ((std::abs(int(dmskpn[x + j]) - int(dmskp[x])) > lim && dmskpn[x + j] != PEAK) ||
                                (dmskpn[x + j] == PEAK && dmskp[x + j] == PEAK) ||
                                (std::abs(int(dmskp[x + j]) - int(dmskp[x])) > lim && dmskp[x + j] != PEAK)) {
                                icb = true;
                                break;
                            }
                        }
                    } else {
                        for (int j = std::max(-x, -dir); j <= 0; j++) {
                            if ((std::abs(int(dmskpn[x + j]) - int(dmskp[x])) > lim && dmskpn[x + j] != PEAK) ||
                                (dmskpn[x + j] == PEAK && dmskp[x + j] == PEAK) ||
                                (std::abs(int(dmskp[x + j]) - int(dmskp[x])) > lim && dmskp[x + j] != PEAK)) {
                                icb = true;
                                break;
                            }
                        }
                    }

                    if (icb)
                        dstp[x] = PEAK;
                }
            }
        }
    }

    // ---- 4. 2x stage -------------------------------------------------------
    void upscaleBy2(const u8 *src, int w, int h, u8 *dst)
    {
        for (int y = 0; y < h; y++)
            std::memcpy(dst + (y * 2 + (1 - d.field)) * w, src + y * w, sizeof(u8) * w);
    }

    // dmsk: direction map on even rows (upscaled), out: odd-row direction map
    void markDirections2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
    {
        const int field = d.field;
        const int stride = w;
        std::fill_n(dst, std::size_t(stride) * H2, PEAK);

        const u8 *mskp  = msk + stride * (1 - field);
        const u8 *dmskp = dmsk + stride * (1 - field);
        u8 *dstp = dst + stride * (2 - field);
        const u8 *mskpn = mskp + stride * 2;
        const u8 *dmskpn = dmskp + stride * 2;

        for (int y = 2 - field; y < H2 - 1; y += 2) {
            for (int x = 1; x < w - 1; x++) {
                if (mskp[x] != PEAK && mskpn[x] != PEAK)
                    continue;

                int order[6];
                unsigned v = 0;
                if (dmskp[x - 1] != PEAK) order[v++] = dmskp[x - 1];
                if (dmskp[x] != PEAK) order[v++] = dmskp[x];
                if (dmskp[x + 1] != PEAK) order[v++] = dmskp[x + 1];
                if (dmskpn[x - 1] != PEAK) order[v++] = dmskpn[x - 1];
                if (dmskpn[x] != PEAK) order[v++] = dmskpn[x];
                if (dmskpn[x + 1] != PEAK) order[v++] = dmskpn[x + 1];

                if (v < 3)
                    continue;

                std::sort(order, order + v);
                const int mid = (v & 1) ? order[v / 2] : (order[(v - 1) / 2] + order[v / 2] + 1) / 2;
                const int lim = maxLut2(mid);

                unsigned u = 0;
                if (std::abs(int(dmskp[x - 1]) - int(dmskpn[x - 1])) <= lim || dmskp[x - 1] == PEAK || dmskpn[x - 1] == PEAK)
                    u++;
                if (std::abs(int(dmskp[x]) - int(dmskpn[x])) <= lim || dmskp[x] == PEAK || dmskpn[x] == PEAK)
                    u++;
                if (std::abs(int(dmskp[x + 1]) - int(dmskpn[x - 1])) <= lim || dmskp[x + 1] == PEAK || dmskpn[x + 1] == PEAK)
                    u++;
                if (u < 2)
                    continue;

                int sum = 0;
                unsigned count = 0;
                for (unsigned i = 0; i < v; i++) {
                    if (std::abs(order[i] - mid) <= lim) {
                        sum += order[i];
                        count++;
                    }
                }

                if (count < v - 2 || count < 2)
                    continue;

                dstp[x] = u8(static_cast<int>(static_cast<float>(sum + mid) / (count + 1) + 0.5f));
            }

            mskp += stride * 2;
            mskpn += stride * 2;
            dmskp += stride * 2;
            dmskpn += stride * 2;
            dstp += stride * 2;
        }
    }

    void filterDirMap2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
    {
        const int field = d.field;
        const int stride = w;
        std::memcpy(dst, dmsk, sizeof(u8) * stride * H2);

        const u8 *mskp  = msk + stride * (1 - field);
        const u8 *dmskp = dmsk + stride * (2 - field);
        u8 *dstp = dst + stride * (2 - field);
        const u8 *mskpn = mskp + stride * 2;
        const u8 *dmskpp = dmskp - stride * 2;
        const u8 *dmskpn = dmskp + stride * 2;

        for (int y = 2 - field; y < H2 - 1; y += 2) {
            for (int x = 1; x < w - 1; x++) {
                if (mskp[x] != PEAK && mskpn[x] != PEAK)
                    continue;

                int order[9];
                unsigned u = 0;

                if (y > 1) {
                    if (dmskpp[x - 1] != PEAK) order[u++] = dmskpp[x - 1];
                    if (dmskpp[x] != PEAK) order[u++] = dmskpp[x];
                    if (dmskpp[x + 1] != PEAK) order[u++] = dmskpp[x + 1];
                }

                if (dmskp[x - 1] != PEAK) order[u++] = dmskp[x - 1];
                if (dmskp[x] != PEAK) order[u++] = dmskp[x];
                if (dmskp[x + 1] != PEAK) order[u++] = dmskp[x + 1];

                if (y < H2 - 2) {
                    if (dmskpn[x - 1] != PEAK) order[u++] = dmskpn[x - 1];
                    if (dmskpn[x] != PEAK) order[u++] = dmskpn[x];
                    if (dmskpn[x + 1] != PEAK) order[u++] = dmskpn[x + 1];
                }

                if (u < 4) {
                    dstp[x] = PEAK;
                    continue;
                }

                std::sort(order, order + u);
                const int mid = (u & 1) ? order[u / 2] : (order[(u - 1) / 2] + order[u / 2] + 1) / 2;
                const int lim = maxLut2(mid);
                int sum = 0;
                unsigned count = 0;
                for (unsigned i = 0; i < u; i++) {
                    if (std::abs(order[i] - mid) <= lim) {
                        sum += order[i];
                        count++;
                    }
                }

                if (count < 4 || (count < 5 && dmskp[x] == PEAK)) {
                    dstp[x] = PEAK;
                    continue;
                }

                dstp[x] = u8(static_cast<int>(static_cast<float>(sum + mid) / (count + 1) + 0.5f));
            }

            mskp += stride * 2;
            mskpn += stride * 2;
            dmskpp += stride * 2;
            dmskp += stride * 2;
            dmskpn += stride * 2;
            dstp += stride * 2;
        }
    }

    void expandDirMap2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
    {
        const int field = d.field;
        const int stride = w;
        std::memcpy(dst, dmsk, sizeof(u8) * stride * H2);

        const u8 *mskp  = msk + stride * (1 - field);
        const u8 *dmskp = dmsk + stride * (2 - field);
        u8 *dstp = dst + stride * (2 - field);
        const u8 *mskpn = mskp + stride * 2;
        const u8 *dmskpp = dmskp - stride * 2;
        const u8 *dmskpn = dmskp + stride * 2;

        for (int y = 2 - field; y < H2 - 1; y += 2) {
            for (int x = 1; x < w - 1; x++) {
                if (dmskp[x] != PEAK || (mskp[x] != PEAK && mskpn[x] != PEAK))
                    continue;

                int order[9];
                unsigned u = 0;

                if (y > 1) {
                    if (dmskpp[x - 1] != PEAK) order[u++] = dmskpp[x - 1];
                    if (dmskpp[x] != PEAK) order[u++] = dmskpp[x];
                    if (dmskpp[x + 1] != PEAK) order[u++] = dmskpp[x + 1];
                }

                if (dmskp[x - 1] != PEAK) order[u++] = dmskp[x - 1];
                if (dmskp[x + 1] != PEAK) order[u++] = dmskp[x + 1];

                if (y < H2 - 2) {
                    if (dmskpn[x - 1] != PEAK) order[u++] = dmskpn[x - 1];
                    if (dmskpn[x] != PEAK) order[u++] = dmskpn[x];
                    if (dmskpn[x + 1] != PEAK) order[u++] = dmskpn[x + 1];
                }

                if (u < 5)
                    continue;

                std::sort(order, order + u);
                const int mid = (u & 1) ? order[u / 2] : (order[(u - 1) / 2] + order[u / 2] + 1) / 2;
                const int lim = maxLut2(mid);
                int sum = 0;
                unsigned count = 0;
                for (unsigned i = 0; i < u; i++) {
                    if (std::abs(order[i] - mid) <= lim) {
                        sum += order[i];
                        count++;
                    }
                }

                if (count < 5)
                    continue;

                dstp[x] = u8(static_cast<int>(static_cast<float>(sum + mid) / (count + 1) + 0.5f));
            }

            mskp += stride * 2;
            mskpn += stride * 2;
            dmskpp += stride * 2;
            dmskp += stride * 2;
            dmskpn += stride * 2;
            dstp += stride * 2;
        }
    }

    void fillGaps2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
    {
        const int field = d.field;
        const int stride = w;
        std::memcpy(dst, dmsk, sizeof(u8) * stride * H2);

        const u8 *mskp    = msk + stride * (1 - field);
        const u8 *dmskp   = dmsk + stride * (2 - field);
        u8 *dstp = dst + stride * (2 - field);
        const u8 *mskpp   = mskp - stride * 2;
        const u8 *mskpn   = mskp + stride * 2;
        const u8 *mskpnn  = mskpn + stride * 2;
        const u8 *dmskpp  = dmskp - stride * 2;
        const u8 *dmskpn  = dmskp + stride * 2;

        const int eight = 8;
        const int fiveHundred = 500;
        const int twenty = 20;

        for (int y = 2 - field; y < H2 - 1; y += 2) {
            for (int x = 1; x < w - 1; x++) {
                if (dmskp[x] != PEAK || (mskp[x] != PEAK && mskpn[x] != PEAK))
                    continue;

                unsigned u = x - 1, v = x + 1;
                int back = fiveHundred, forward = -fiveHundred;

                while (u) {
                    if (dmskp[u] != PEAK) {
                        back = dmskp[u];
                        break;
                    }
                    if (mskp[u] != PEAK && mskpn[u] != PEAK)
                        break;
                    u--;
                }

                while (v < unsigned(w)) {
                    if (dmskp[v] != PEAK) {
                        forward = dmskp[v];
                        break;
                    }
                    if (mskp[v] != PEAK && mskpn[v] != PEAK)
                        break;
                    v++;
                }

                bool tc = true, bc = true;
                int mint = fiveHundred, maxt = -twenty;
                int minb = fiveHundred, maxb = -twenty;

                for (unsigned j = u; j <= v; j++) {
                    if (tc) {
                        if (y <= 2 || dmskpp[j] == PEAK || (mskpp[j] != PEAK && mskp[j] != PEAK)) {
                            tc = false;
                            mint = maxt = twenty;
                        } else {
                            if (dmskpp[j] < mint) mint = dmskpp[j];
                            if (dmskpp[j] > maxt) maxt = dmskpp[j];
                        }
                    }

                    if (bc) {
                        if (y >= H2 - 3 || dmskpn[j] == PEAK || (mskpn[j] != PEAK && mskpnn[j] != PEAK)) {
                            bc = false;
                            minb = maxb = twenty;
                        } else {
                            if (dmskpn[j] < minb) minb = dmskpn[j];
                            if (dmskpn[j] > maxb) maxb = dmskpn[j];
                        }
                    }
                }

                if (maxt == -twenty)
                    maxt = mint = twenty;
                if (maxb == -twenty)
                    maxb = minb = twenty;

                const int thresh = std::max({ std::max(std::abs(forward - NEUTRAL), std::abs(back - NEUTRAL)) / 4,
                                              eight, std::abs(mint - maxt), std::abs(minb - maxb) });
                const unsigned lim = std::min<unsigned>(std::max(std::abs(forward - NEUTRAL), std::abs(back - NEUTRAL)) >> 2, 6);
                if (std::abs(forward - back) <= thresh && (v - u - 1 <= lim || tc || bc)) {
                    const float step = static_cast<float>(forward - back) / (v - u);
                    for (unsigned j = 0; j < v - u - 1; j++)
                        dstp[u + j + 1] = u8(back + static_cast<int>(j * step + 0.5f));
                }
            }

            mskpp += stride * 2;
            mskp += stride * 2;
            mskpn += stride * 2;
            mskpnn += stride * 2;
            dmskpp += stride * 2;
            dmskp += stride * 2;
            dmskpn += stride * 2;
            dstp += stride * 2;
        }
    }

    // The actual edge-directed interpolation: fills the odd rows of dst.
    void interpolateLattice(const u8 *omsk, u8 *dmsk, int w, int H2, u8 *dst)
    {
        const int field = d.field;
        const int stride = w;

        if (field)
            std::memcpy(dst + stride * (H2 - 1), dst + stride * (H2 - 2), sizeof(u8) * w);
        else
            std::memcpy(dst, dst + stride, sizeof(u8) * w);

        const u8 *omskp = omsk + stride * (1 - field);
        u8 *dmskp = dmsk + stride * (2 - field);
        u8 *dstp = dst + stride * (1 - field);
        const u8 *omskn = omskp + stride * 2;
        u8 *dstpn = dstp + stride;
        const u8 *dstpnn = dstp + stride * 2;

        for (int y = 2 - field; y < H2 - 1; y += 2) {
            for (int x = 0; x < w; x++) {
                int dir = dmskp[x];
                const int lim = maxLut2(dir);

                if (dir == PEAK || (std::abs(int(dmskp[x]) - int(dmskp[x - 1])) > lim && std::abs(int(dmskp[x]) - int(dmskp[x + 1])) > lim)) {
                    dstpn[x] = u8((dstp[x] + dstpnn[x] + 1) / 2);
                    if (dir != PEAK)
                        dmskp[x] = NEUTRAL;
                    continue;
                }

                if (lim < 9) {
                    const int s = dstp[x - 1] + dstp[x] + dstp[x + 1] +
                                  dstpnn[x - 1] + dstpnn[x] + dstpnn[x + 1];
                    const int sq = dstp[x - 1] * dstp[x - 1] + dstp[x] * dstp[x] + dstp[x + 1] * dstp[x + 1] +
                                   dstpnn[x - 1] * dstpnn[x - 1] + dstpnn[x] * dstpnn[x] + dstpnn[x + 1] * dstpnn[x + 1];
                    if (6 * sq - s * s < 576) {
                        dstpn[x] = u8((dstp[x] + dstpnn[x] + 1) / 2);
                        dmskp[x] = PEAK;
                        continue;
                    }
                }

                if (x > 1 && x < w - 2 &&
                    ((int(dstp[x]) < std::max(dstp[x - 2], dstp[x - 1]) - 3 && int(dstp[x]) < std::max(dstp[x + 2], dstp[x + 1]) - 3 &&
                      int(dstpnn[x]) < std::max(dstpnn[x - 2], dstpnn[x - 1]) - 3 && int(dstpnn[x]) < std::max(dstpnn[x + 2], dstpnn[x + 1]) - 3) ||
                     (int(dstp[x]) > std::min(dstp[x - 2], dstp[x - 1]) + 3 && int(dstp[x]) > std::min(dstp[x + 2], dstp[x + 1]) + 3 &&
                      int(dstpnn[x]) > std::min(dstpnn[x - 2], dstpnn[x - 1]) + 3 && int(dstpnn[x]) > std::min(dstpnn[x + 2], dstpnn[x + 1]) + 3))) {
                    dstpn[x] = u8((dstp[x] + dstpnn[x] + 1) / 2);
                    dmskp[x] = NEUTRAL;
                    continue;
                }

                dir = (dir - NEUTRAL + 2) >> 2;
                const int uStart = (dir - 2 < 0) ? std::max({ -x + 1, dir - 2, -w + 2 + x }) : std::min({ x - 1, dir - 2, w - 2 - x });
                const int uStop = (dir + 2 < 0) ? std::max({ -x + 1, dir + 2, -w + 2 + x }) : std::min({ x - 1, dir + 2, w - 2 - x });
                unsigned min = d.nt8;
                unsigned val = (dstp[x] + dstpnn[x] + 1) / 2;

                for (int u = uStart; u <= uStop; u++) {
                    const unsigned diff = std::abs(int(dstp[x - 1]) - int(dstpnn[x - u - 1])) +
                                          std::abs(int(dstp[x]) - int(dstpnn[x - u])) +
                                          std::abs(int(dstp[x + 1]) - int(dstpnn[x - u + 1])) +
                                          std::abs(int(dstpnn[x - 1]) - int(dstp[x + u - 1])) +
                                          std::abs(int(dstpnn[x]) - int(dstp[x + u])) +
                                          std::abs(int(dstpnn[x + 1]) - int(dstp[x + u + 1]));
                    if (diff < min &&
                        ((omskp[x - 1 + u] != PEAK && std::abs(int(omskp[x - 1 + u]) - int(dmskp[x])) <= lim) ||
                         (omskp[x + u] != PEAK && std::abs(int(omskp[x + u]) - int(dmskp[x])) <= lim) ||
                         (omskp[x + 1 + u] != PEAK && std::abs(int(omskp[x + 1 + u]) - int(dmskp[x])) <= lim)) &&
                        ((omskn[x - 1 - u] != PEAK && std::abs(int(omskn[x - 1 - u]) - int(dmskp[x])) <= lim) ||
                         (omskn[x - u] != PEAK && std::abs(int(omskn[x - u]) - int(dmskp[x])) <= lim) ||
                         (omskn[x + 1 - u] != PEAK && std::abs(int(omskn[x + 1 - u]) - int(dmskp[x])) <= lim))) {
                        const unsigned diff2 = std::abs(int(dstp[x + u / 2 - 1]) - int(dstpnn[x - u / 2 - 1])) +
                                               std::abs(int(dstp[x + u / 2]) - int(dstpnn[x - u / 2])) +
                                               std::abs(int(dstp[x + u / 2 + 1]) - int(dstpnn[x - u / 2 + 1]));
                        if (diff2 < d.nt4 &&
                            (((std::abs(int(omskp[x + u / 2]) - int(omskn[x - u / 2])) <= lim ||
                               std::abs(int(omskp[x + u / 2]) - int(omskn[x - (u + 1) / 2])) <= lim) &&
                              omskp[x + u / 2] != PEAK) ||
                             ((std::abs(int(omskp[x + (u + 1) / 2]) - int(omskn[x - u / 2])) <= lim ||
                               std::abs(int(omskp[x + (u + 1) / 2]) - int(omskn[x - (u + 1) / 2])) <= lim) &&
                              omskp[x + (u + 1) / 2] != PEAK))) {
                            if ((std::abs(int(dmskp[x]) - int(omskp[x + u / 2])) <= lim || std::abs(int(dmskp[x]) - int(omskp[x + (u + 1) / 2])) <= lim) &&
                                (std::abs(int(dmskp[x]) - int(omskn[x - u / 2])) <= lim || std::abs(int(dmskp[x]) - int(omskn[x - (u + 1) / 2])) <= lim)) {
                                val = (dstp[x + u / 2] + dstp[x + (u + 1) / 2] + dstpnn[x - u / 2] + dstpnn[x - (u + 1) / 2] + 2) / 4;
                                min = diff;
                                dir = u;
                            }
                        }
                    }
                }

                if (min != d.nt8) {
                    dstpn[x] = u8(val);
                    dmskp[x] = u8(NEUTRAL + (dir << 2));
                } else {
                    const int dt = 4;
                    const int uStart2 = std::max(-x + 1, -dt);
                    const int uStop2 = std::min(w - 2 - x, dt);
                    const unsigned minm = std::min(dstp[x], dstpnn[x]);
                    const unsigned maxm = std::max(dstp[x], dstpnn[x]);
                    min = d.nt7;

                    for (int u = uStart2; u <= uStop2; u++) {
                        const int p1 = int(dstp[x + u / 2]) + int(dstp[x + (u + 1) / 2]);
                        const int p2 = int(dstpnn[x - u / 2]) + int(dstpnn[x - (u + 1) / 2]);
                        const unsigned diff = std::abs(int(dstp[x - 1]) - int(dstpnn[x - u - 1])) +
                                              std::abs(int(dstp[x]) - int(dstpnn[x - u])) +
                                              std::abs(int(dstp[x + 1]) - int(dstpnn[x - u + 1])) +
                                              std::abs(int(dstpnn[x - 1]) - int(dstp[x + u - 1])) +
                                              std::abs(int(dstpnn[x]) - int(dstp[x + u])) +
                                              std::abs(int(dstpnn[x + 1]) - int(dstp[x + u + 1])) +
                                              std::abs(p1 - p2);
                        if (diff < min) {
                            const unsigned valt = (p1 + p2 + 2) / 4;
                            if (valt >= minm && valt <= maxm) {
                                val = valt;
                                min = diff;
                                dir = u;
                            }
                        }
                    }

                    dstpn[x] = u8(val);
                    dmskp[x] = (min == d.nt7) ? NEUTRAL : u8(NEUTRAL + (dir << 2));
                }
            }

            omskp += stride * 2;
            omskn += stride * 2;
            dmskp += stride * 2;
            dstp += stride * 2;
            dstpn += stride * 2;
            dstpnn += stride * 2;
        }
    }

    void postProcess(const u8 *nmsk, const u8 *omsk, int w, int H2, u8 *dst)
    {
        const int field = d.field;
        const int stride = w;

        const u8 *nmskp = nmsk + stride * (2 - field);
        const u8 *omskp = omsk + stride * (2 - field);
        u8 *dstp = dst + stride * (2 - field);
        const u8 *dstpp = dstp - stride;
        const u8 *dstpn = dstp + stride;

        for (int y = 2 - field; y < H2 - 1; y += 2) {
            for (int x = 0; x < w; x++) {
                const int lim = maxLut2(int(nmskp[x]));
                if (std::abs(int(nmskp[x]) - int(omskp[x])) > lim && omskp[x] != PEAK && omskp[x] != NEUTRAL)
                    dstp[x] = u8((dstpp[x] + dstpn[x] + 1) / 2);
            }

            nmskp += stride * 2;
            omskp += stride * 2;
            dstpp += stride * 2;
            dstp += stride * 2;
            dstpn += stride * 2;
        }
    }

    // ---- helpers ----------------------------------------------------------
    int maxLut2(int dirMapValue) const
    {
        return d.limlut2[std::abs(int(dirMapValue) - NEUTRAL) >> 2];
    }
};

// ---------------------------------------------------------------------------
// Resampling (spline36, shift semantics of fmtconv resample)
// ---------------------------------------------------------------------------
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
static void resampleV(const u8 *src, int w, int srcH, int dstH, double sy, u8 *dst)
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

static void transpose(const u8 *src, int w, int h, u8 *dst)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[x * h + y] = src[y * w + x];
}

// ---------------------------------------------------------------------------
// Repair mode 2: clamp each pixel to [2nd smallest, 2nd largest] of the 3x3
// neighbourhood of the reference clip (vs-removegrain repairvs.cpp semantics).
// ---------------------------------------------------------------------------
static void repair2(const u8 *a, const u8 *b, int w, int h, u8 *out)
{
    if (h < 3 || w < 3) {
        std::memcpy(out, a, sizeof(u8) * w * h);
        return;
    }
    std::memcpy(out, a, sizeof(u8) * w);                                  // row 0
    std::memcpy(out + (h - 1) * w, a + (h - 1) * w, sizeof(u8) * w);      // row h-1

    for (int y = 1; y < h - 1; y++) {
        const u8 *ap = a + y * w;
        const u8 *bpp = b + (y - 1) * w;
        const u8 *bpc = b + y * w;
        const u8 *bpn = b + (y + 1) * w;
        u8 *op = out + y * w;

        op[0] = ap[0];
        op[w - 1] = ap[w - 1];

        for (int x = 1; x < w - 1; x++) {
            int n[9] = {
                int(bpp[x - 1]), int(bpp[x]), int(bpp[x + 1]),
                int(bpc[x - 1]), int(bpc[x]), int(bpc[x + 1]),
                int(bpn[x - 1]), int(bpn[x]), int(bpn[x + 1])
            };
            std::sort(n, n + 9);
            const int lo = n[1];
            const int hi = n[7];
            op[x] = u8(std::max(lo, std::min(int(ap[x]), hi)));
        }
    }
}

// ---------------------------------------------------------------------------
// Color helpers (BT.601 full-range YUV444 as the "planes" of an image)
// ---------------------------------------------------------------------------
static inline u8 clamp8(int v) { return u8(std::max(0, std::min(255, v))); }

static void rgbToYuv(const u8 *rgb, int n, std::vector<u8> &y, std::vector<u8> &u, std::vector<u8> &v)
{
    y.resize(n); u.resize(n); v.resize(n);
    for (int i = 0; i < n; i++) {
        const double R = rgb[3 * i + 0], G = rgb[3 * i + 1], B = rgb[3 * i + 2];
        const double Y = 0.299 * R + 0.587 * G + 0.114 * B;
        // full-range BT.601 Cb/Cr, matched with the inverse below
        y[i] = clamp8(int(Y + 0.5));
        u[i] = clamp8(int((B - Y) / 1.772 + 128.0 + 0.5));
        v[i] = clamp8(int((R - Y) / 1.402 + 128.0 + 0.5));
    }
}

static void yuvToRgb(const std::vector<u8> &y, const std::vector<u8> &u, const std::vector<u8> &v, std::vector<u8> &rgb)
{
    const int n = int(y.size());
    rgb.resize(std::size_t(n) * 3);
    for (int i = 0; i < n; i++) {
        const double Y = y[i], U = int(u[i]) - 128, V = int(v[i]) - 128;
        int R = int(Y + 1.402 * V + 0.5);
        int G = int(Y - 0.344136 * U - 0.714136 * V + 0.5);
        int B = int(Y + 1.772 * U + 0.5);
        rgb[3 * i + 0] = clamp8(R);
        rgb[3 * i + 1] = clamp8(G);
        rgb[3 * i + 2] = clamp8(B);
    }
}

// ---------------------------------------------------------------------------
// The AA chain on a single luma plane (w*h gray8 -> w*h gray8)
// ---------------------------------------------------------------------------
static void aaChain(const u8 *gray, int w, int h, const Eedi2Params &p, std::vector<u8> &out)
{
    Eedi2 eedi2(p);

    // pass 1: vertical
    std::vector<u8> a1(std::size_t(w) * h * 2);
    eedi2.run(gray, w, h, a1.data());                       // w x 2h
    std::vector<u8> r1(std::size_t(w) * h);
    resampleV(a1.data(), w, h * 2, h, -0.5, r1.data());     // w x h

    // pass 2: horizontal (transpose -> double -> resample -> transpose back)
    std::vector<u8> t1(std::size_t(h) * w);
    transpose(r1.data(), w, h, t1.data());                  // h x w
    std::vector<u8> a2(std::size_t(h) * w * 2);
    eedi2.run(t1.data(), h, w, a2.data());                  // h x 2w
    std::vector<u8> r2(std::size_t(h) * w);
    resampleV(a2.data(), h, w * 2, w, -0.5, r2.data());     // h x w
    out.resize(std::size_t(w) * h);
    transpose(r2.data(), h, w, out.data());                 // w x h
}

// ---------------------------------------------------------------------------
// Image IO
// ---------------------------------------------------------------------------
struct Image {
    int w = 0, h = 0, ch = 0;          // ch: 1 gray, 3 RGB
    std::vector<u8> p;
};

static bool loadImage(const std::string &path, Image &img)
{
    int w, h, ch;
    u8 *data = stbi_load(path.c_str(), &w, &h, &ch, 0);
    if (!data)
        return false;
    img.w = w; img.h = h;
    if (ch == 1 || ch == 2) {
        img.ch = 1;
        img.p.assign(data, data + std::size_t(w) * h);
    } else {
        img.ch = 3;
        img.p.resize(std::size_t(w) * h * 3);
        for (int i = 0; i < w * h; i++) {
            img.p[3 * i + 0] = data[ch * i + 0];
            img.p[3 * i + 1] = data[ch * i + 1];
            img.p[3 * i + 2] = data[ch * i + 2];
        }
    }
    stbi_image_free(data);
    return true;
}

static bool saveImage(const std::string &path, const Image &img)
{
    const std::string ext = [&]() {
        const auto pos = path.find_last_of('.');
        std::string e = (pos == std::string::npos) ? "" : path.substr(pos + 1);
        std::transform(e.begin(), e.end(), e.begin(), [](char c) { return char(std::tolower((unsigned char)c)); });
        return e;
    }();
    const int stride = img.w * img.ch;
    if (ext == "png")
        return stbi_write_png(path.c_str(), img.w, img.h, img.ch, img.p.data(), stride) != 0;
    if (ext == "bmp")
        return stbi_write_bmp(path.c_str(), img.w, img.h, img.ch, img.p.data()) != 0;
    if (ext == "tga")
        return stbi_write_tga(path.c_str(), img.w, img.h, img.ch, img.p.data()) != 0;
    if (ext == "jpg" || ext == "jpeg")
        return stbi_write_jpg(path.c_str(), img.w, img.h, img.ch, img.p.data(), 95) != 0;
    if (ext == "pgm" || ext == "pnm" || ext == "ppm") {
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const char magic = (img.ch == 3) ? '6' : '5';
        std::fprintf(f, "P%c\n%d %d\n255\n", magic, img.w, img.h);
        const bool ok = std::fwrite(img.p.data(), 1, img.p.size(), f) == img.p.size();
        std::fclose(f);
        return ok;
    }
    // default: PNG
    return stbi_write_png(path.c_str(), img.w, img.h, img.ch, img.p.data(), stride) != 0;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
static void printHelp()
{
    std::printf(
        "aa.exe - EEDI2 anti-aliasing (port of the VapourSynth eedi2+fmtc+repair chain)\n"
        "\n"
        "Usage: aa.exe -i <input> -o <output> [options]\n"
        "\n"
        "  -i, --input  <file>   input image (PNG/BMP/PNM/TGA, gray or RGB)\n"
        "  -o, --output <file>   output image (format from extension: PNG/BMP/TGA/JPG)\n"
        "\n"
        "Options (defaults match the original script):\n"
        "  --mthresh N    motion threshold         (10)\n"
        "  --lthresh N    linear interpolation th. (20)\n"
        "  --vthresh N    variance threshold       (20)\n"
        "  --maxd N       max edge search distance (24)\n"
        "  --nt N         noise threshold          (50)\n"
        "  --field N      field parity, 0 or 1     (1)\n"
        "  --repair N     Repair mode: 2 (default), 0 disables repair\n"
        "  -h, --help     show this help\n"
        "\n"
        "Examples:\n"
        "  aa.exe -i clip.png -o clip_aa.png\n"
        "  aa.exe -i in.bmp -o out.bmp --maxd 32 --nt 30\n");
}

static int parseInt(const char *s)
{
    return std::atoi(s);
}

int main(int argc, char **argv)
{
    std::string input, output;
    std::string dumpEedi2, dumpRs;
    Eedi2Params p;
    int repairMode = 2;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-i" || a == "--input") input = next("-i");
        else if (a == "-o" || a == "--output") output = next("-o");
        else if (a == "--mthresh") p.mthresh = parseInt(next("--mthresh"));
        else if (a == "--lthresh") p.lthresh = parseInt(next("--lthresh"));
        else if (a == "--vthresh") p.vthresh = parseInt(next("--vthresh"));
        else if (a == "--maxd") p.maxd = parseInt(next("--maxd"));
        else if (a == "--nt") p.nt = parseInt(next("--nt"));
        else if (a == "--field") p.field = parseInt(next("--field"));
        else if (a == "--repair") repairMode = parseInt(next("--repair"));
        else if (a == "--dump-eedi2") dumpEedi2 = next("--dump-eedi2");
        else if (a == "--dump-rs") dumpRs = next("--dump-rs");
        else if (a == "-h" || a == "--help") { printHelp(); return 0; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            printHelp();
            return 1;
        }
    }

    if (input.empty() || output.empty()) {
        std::fprintf(stderr, "error: -i and -o are required\n");
        printHelp();
        return 1;
    }

    if (p.maxd < 1 || p.maxd > 29) {
        std::fprintf(stderr, "error: maxd must be in 1..29\n");
        return 1;
    }
    if (p.field != 0 && p.field != 1) {
        std::fprintf(stderr, "error: field must be 0 or 1\n");
        return 1;
    }

    Image img;
    if (!loadImage(input, img)) {
        std::fprintf(stderr, "error: cannot read image '%s'\n", input.c_str());
        return 1;
    }
    if (img.w < 8 || img.h < 8) {
        std::fprintf(stderr, "error: image too small (need w>=8, h>=8)\n");
        return 1;
    }

    eedi2_prepare(p);

    if (!dumpEedi2.empty() && !dumpRs.empty()) {
        // debug: dump the first EEDI2 pass (w x 2h raw gray8) and its
        // resampleV result (w x h raw gray8) for comparison with eedi2/fmtc
        std::vector<u8> y, u, v;
        if (img.ch == 3)
            rgbToYuv(img.p.data(), img.w * img.h, y, u, v);
        else
            y = img.p;
        Eedi2 eedi2(p);
        std::vector<u8> h2(std::size_t(img.w) * img.h * 2);
        eedi2.run(y.data(), img.w, img.h, h2.data());
        FILE *f = std::fopen(dumpEedi2.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpEedi2.c_str()); return 1; }
        std::fwrite(h2.data(), 1, h2.size(), f);
        std::fclose(f);
        std::vector<u8> r1(std::size_t(img.w) * img.h);
        resampleV(h2.data(), img.w, img.h * 2, img.h, -0.5, r1.data());
        f = std::fopen(dumpRs.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpRs.c_str()); return 1; }
        std::fwrite(r1.data(), 1, r1.size(), f);
        std::fclose(f);
        std::printf("ok: dumped eedi2 %d x %d -> %d x %d, rs %d x %d\n",
                    img.w, img.h, img.w, img.h * 2, img.w, img.h);
        return 0;
    }

    if (!dumpEedi2.empty()) {
        // debug: dump the first EEDI2 pass (w x 2h raw gray8) for comparison
        std::vector<u8> y, u, v;
        if (img.ch == 3)
            rgbToYuv(img.p.data(), img.w * img.h, y, u, v);
        else
            y = img.p;
        Eedi2 eedi2(p);
        std::vector<u8> h2(std::size_t(img.w) * img.h * 2);
        eedi2.run(y.data(), img.w, img.h, h2.data());
        FILE *f = std::fopen(dumpEedi2.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write '%s'\n", dumpEedi2.c_str()); return 1; }
        std::fwrite(h2.data(), 1, h2.size(), f);
        std::fclose(f);
        std::printf("ok: dumped %d x %d -> %d x %d\n", img.w, img.h, img.w, img.h * 2);
        return 0;
    }

    std::vector<u8> lumaAA;
    std::vector<u8> grayRef;
    if (img.ch == 1) {
        grayRef = img.p;
        aaChain(img.p.data(), img.w, img.h, p, lumaAA);
    } else {
        std::vector<u8> y, u, v;
        rgbToYuv(img.p.data(), img.w * img.h, y, u, v);
        grayRef = y;
        aaChain(y.data(), img.w, img.h, p, lumaAA);
        if (repairMode != 0)
            repair2(lumaAA.data(), grayRef.data(), img.w, img.h, lumaAA.data());
        std::vector<u8> rgb;
        yuvToRgb(lumaAA, u, v, rgb);
        img.p = std::move(rgb);
    }

    if (img.ch == 1 && repairMode != 0)
        repair2(lumaAA.data(), grayRef.data(), img.w, img.h, lumaAA.data());

    if (img.ch == 1)
        img.p = std::move(lumaAA);

    if (!saveImage(output, img)) {
        std::fprintf(stderr, "error: cannot write image '%s'\n", output.c_str());
        return 1;
    }

    std::printf("ok: %d x %d -> %d x %d (%s)\n", img.w, img.h, img.w, img.h, output.c_str());
    return 0;
}