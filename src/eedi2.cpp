#include "eedi2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

void eedi2_prepare(Eedi2Params &p)
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

Eedi2::Eedi2(const Eedi2Params &p) : d(p) {}

void Eedi2::run(const u8 *src, int w, int h, u8 *out)
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

// ---- 1. edge mask: mark pixels with enough local structure ------------
void Eedi2::buildEdgeMask(const u8 *src, int w, int h, u8 *msk)
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

void Eedi2::erode(const u8 *msk, int w, int h, u8 *dst)
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

void Eedi2::dilate(const u8 *msk, int w, int h, u8 *dst)
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

void Eedi2::removeSmallHorzGaps(const u8 *msk, int w, int h, u8 *dst)
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
void Eedi2::calcDirections(const u8 *src, const u8 *msk, int w, int h, u8 *dst)
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
void Eedi2::filterDirMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst)
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

void Eedi2::expandDirMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst)
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

void Eedi2::filterMap(const u8 *msk, const u8 *dmsk, int w, int h, u8 *dst)
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
void Eedi2::upscaleBy2(const u8 *src, int w, int h, u8 *dst)
{
    for (int y = 0; y < h; y++)
        std::memcpy(dst + (y * 2 + (1 - d.field)) * w, src + y * w, sizeof(u8) * w);
}

// dmsk: direction map on even rows (upscaled), out: odd-row direction map
void Eedi2::markDirections2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
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

void Eedi2::filterDirMap2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
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

void Eedi2::expandDirMap2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
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

void Eedi2::fillGaps2X(const u8 *msk, const u8 *dmsk, int w, int H2, u8 *dst)
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
void Eedi2::interpolateLattice(const u8 *omsk, u8 *dmsk, int w, int H2, u8 *dst)
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

void Eedi2::postProcess(const u8 *nmsk, const u8 *omsk, int w, int H2, u8 *dst)
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

int Eedi2::maxLut2(int dirMapValue) const
{
    return d.limlut2[std::abs(int(dirMapValue) - NEUTRAL) >> 2];
}